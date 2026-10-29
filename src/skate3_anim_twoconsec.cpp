// DecompressTwoConsecFrames: native replacement.
//
// RETAIL PORT (from sk83_na_m, same source): the codec is the same EA library
// code in both builds, minus the dev build's asserts and VMX save/restore.
//   VBRDecompressor::DecompressTwoConsecFrames  dev 0x831CE528 -> retail sub_82E89B80
//   VBRChannelTypeBlockInfo::PutData            dev 0x831CCE48 -> retail sub_82E88BA0
// Matched by: the decompressor cluster (InitPerAnim / DecompressFrame /
// TwoConsec at 82E88FA8 / 82E89A60 / 82E89B80, the only retail users of the
// work-buffer offset 9264), identical work-buffer field offsets (+12..+44),
// QuantTableInfo stride 2052, the same r3..r9 argument use (work = self +
// 9264 + (r9 & 0xFF) * 64), and PutData's identical body (4 calls from
// TwoConsec in both builds).
//
// VERIFIED: 48,000 in-block calls, 0 divergences, comparing the full ordered
// PutData sequence (frame, joint, putLoc, size, payload) against the guest's.
//
// WHY. Codec-chain self time (nested codec calls excluded) put this at 83.22%
// of the chain, ~5.7% of process CPU, 4,159,537 calls at 6,395 cyc each
// (5,365 after native PutData landed). The work is small; the cost is
// emulation -- __savevmx_114/__restvmx_114 save and restore 14 vector
// registers on EVERY call, every VMX op round-trips through PPCContext, and
// branches materialise condition registers.
//
// INTERFACE (decoded from captured calls, not from the listing):
//   r3=this r4=compressedData r5=frame0 r6=frame1(=frame0+1)
//   r7=putLoc0 r8=putLoc1 r9=threadId(0..5)
// r7/r8 matched the putLoc of every recorded PutData, alternating
// frame0->r7, frame1->r8.
//
// LAYOUT (confirmed against live memory: this=41400320 work=41402790 tid=1,
// difference 0x2470 = 9264 + 1*64):
//   VBRDecompressor: mChannelBlocks +0, mIdctCoefTables +48,
//     mInitialQuantizeTables +304, mQuantizeTables +8496,
//     mWorkBuffers +9264 (64B each), mQuantTableInfo +9648 (2052B each)
//   DataPerDecompress: mhdr +0, mUncompressedBlkDataScratch +4,
//     mCompressedData +24, mNormalizedData +28, mConstData +32,
//     mIsConstantChannel +36, mFrameOffset +44,
//     mChannelBlockStarts +48, mChannelBlockEnds +56
//   The header is *(*(work+0)) -- a DOUBLE indirection. work+0 stayed constant
//   across captures while *(work+0) varied per animation.
//
// DECODE: input = scratch + start*32, 128-byte chunks of 4 outputs x 8 floats,
//   out = sum_{k<8} data[k]*coef[k] + 0.5, then *chRange + chMin,
//   coef[i] = (initial[chIdx][i]*scale^2 + scale/32768) * idctCoef[frame&7][i]
//
// FOUR THINGS THAT HAD TO BE EXACTLY RIGHT, each found by measurement:
//   1. FP CONTRACTION OFF. The guest's vmaddcfp is lowered as unfused
//      add(mul(a,b),c) -- two roundings. Clang contracts to FMA (one) under
//      -O3 -march=x86-64-v3. A case with a single non-zero input term still
//      diverged, which ruled out summation order and pointed here.
//   2. vmsum4fp128, not scalar math. The SDK lowers it to one DPPS, whose
//      internal summation no scalar tree reproduces. Using the SDK's own
//      function is bit-exact AND cheaper (one instruction per 4-element dot),
//      and inherits its guest-FTZ / overflow-to-QNaN handling.
//   3. COMPUTE the quant table, do not read the stored one. The harness showed
//      the guest's coefficients uniformly scaled by ~0.9408 versus the stored
//      table, constant across entries with different input mixes. Since
//      bias = scale/32768 is negligible beside initial*scale^2, tables for
//      different scales are proportional -- the stored one was built for a
//      PREVIOUS animation and the flag entry's index is not the slot used.
//   4. The 1/32768 bias, A/B tested in-game against 0.5 (which gave ~-112
//      where the answer is ~-0.083). Confirms the AnimTest note.
//
// BLOCK-CROSSING CALLS DELEGATE. When frame0 & 7 == 7 the two frames live in
// different frame blocks, so the guest unpacks block A, distributes frame0,
// then unpacks block B over the same scratch and distributes frame1. A single
// pass over one scratch buffer cannot reproduce that, and it is ~12% of calls,
// so those go to __imp__ unchanged.
//
// STATE PARITY: the decode writes its results into mNormalizedData exactly
// where the guest would, because ~44% of calls have no live channel and their
// distribution reads whatever the previous call left there.

#include "generated/skate3_init.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// See note 1 above -- this must stay unfused to match the guest.
#pragma clang fp contract(off)

namespace {

constexpr uint32_t kWorkBuffers = 9264;
constexpr uint32_t kWorkStride = 64;
constexpr uint32_t kIdctCoefTables = 48;
constexpr uint32_t kInitialQuant = 304;

constexpr uint32_t kScratch = 4;
constexpr uint32_t kCompressedData = 24;
constexpr uint32_t kNormalizedData = 28;
constexpr uint32_t kConstData = 32;
constexpr uint32_t kIsConstantChannel = 36;
constexpr uint32_t kFrameOffset = 44;

inline float AsF(uint32_t b) {
  PPCRegister t{};
  t.u32 = b;
  return t.f32;
}
inline uint32_t AsU(float f) {
  PPCRegister t{};
  t.f32 = f;
  return t.u32;
}

// lvx128: big-endian 16-byte load reverses the vector, so memory element i
// lands in lane 3-i.
inline simde__m128 GuestLoadF4(uint8_t* base, uint32_t ea) {
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
      simde_mm_loadu_si128((const simde__m128i*)REX_RAW_ADDR(ea)),
      simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
}

// Coefficient vector in the guest's lane order (lane i = c[3-i]).
inline simde__m128 CoefVec(const float* c) {
  return simde_mm_set_ps(c[0], c[1], c[2], c[3]);
}

// PutData, inlined. Verified in-game at 79,691,776 calls, 0 mismatches.
//   putLoc != 0 : dest = mStride*joint + mOffset + putLoc
//   putLoc == 0 : dest = mStride*joint + *(mData + frame*4) + mOffset
//   then copy mDataSize floats.
// NOTE for the AnimTest reversing project: in the putLoc == 0 path the guest
// indexes an ARRAY OF POINTERS by frame (rlwinm r8,r29,2,0,29 ; lwzx r9,r8,r9).
// AnimTest's PutData ignores `frame` and treats mData as a direct base, which
// is wrong for that path. The game never takes it -- every captured in-game
// call had putLoc != 0 -- but AnimTest's own offline extraction can.
inline void PutData(uint8_t* base, uint32_t blk, uint32_t frame, uint32_t joint,
                    uint32_t put_loc, const uint32_t* words) {
  const uint32_t off = REX_LOAD_U16(blk + 0);
  const uint32_t stride = REX_LOAD_U16(blk + 2);
  const uint32_t size = REX_LOAD_U8(blk + 8);
  uint32_t dest;
  if (put_loc != 0) {
    dest = stride * joint + off + put_loc;
  } else {
    const uint32_t data = REX_LOAD_U32(blk + 4);
    if (data == 0) {
      return;
    }
    dest = stride * joint + REX_LOAD_U32(data + frame * 4u) + off;
  }
  for (uint32_t i = 0; i < size; ++i) {
    REX_STORE_U32(dest + i * 4u, words[i]);
  }
}


constexpr uint32_t kCompressedHeaderData = 20;
constexpr uint32_t kNumBitsOffset = 16;
constexpr uint32_t kMaxScratchInts = 8192;

// Bitstream unpack -- the piece that was missing when the first native
// replacement produced wrong animation. Ported from AnimTest's
// Vector_UnPackFrameBlockAndDecompressOneFrame, whose own notes cite the same
// instructions this function opens with (addi r11,r31,1 ; lvsl ; vperm).
//
// Two independent streams:
//   A (selector): 32-bit LE shift register from pCmp[1..4], refilled 24 bits
//     at a time from selRefillPtr[1..3] after each 8-nibble bone group when
//     <= 8 bits remain. One bit per non-zero nibble.
//   B (magnitude): 64-bit lazy-refill buffer starting at
//     pCmp + 1 + ceil(pCmp[0]/8). Consumed only when the selector bit is 1:
//     one sign bit then N magnitude bits, N = the nibble value.
//
// Output is the raw float bit pattern of (float)(uint32_t)mag, with the IEEE
// sign bit SET when the stream's sign bit is 0 (vcuxwfp + vor in the guest).
//
// `out` is a host buffer so the unpack can be verified against the guest's
// scratch without disturbing it.
void Unpack(uint8_t* base, uint32_t compressed_stream, uint32_t work,
            uint32_t bone_count, uint32_t dest_ea, uint32_t out_ints) {
  const uint32_t sel_header_bits = REX_LOAD_U8(compressed_stream + 0);
  uint32_t sel_buf = uint32_t(REX_LOAD_U8(compressed_stream + 1)) |
                     (uint32_t(REX_LOAD_U8(compressed_stream + 2)) << 8) |
                     (uint32_t(REX_LOAD_U8(compressed_stream + 3)) << 16) |
                     (uint32_t(REX_LOAD_U8(compressed_stream + 4)) << 24);
  uint32_t sel_count = 32;
  // +4, NOT AnimTest's +5. The init consumes pCmp[1..4], so the next unread
  // byte is pCmp[5]; AnimTest's pointer makes the first refill read pCmp[6..8]
  // and skip one byte, which desyncs the selector stream a few dozen
  // coefficients in. A/B tested in-game: offset 4 was clean on 4000/4000
  // calls, offset 5 failed 2305 of them. (This is a real bug in AnimTest's
  // Vector_UnPackFrameBlockAndDecompressOneFrame.)
  uint32_t sel_refill = compressed_stream + 4;

  uint32_t mag_ptr = compressed_stream + 1 + ((sel_header_bits + 7u) >> 3);
  uint64_t mag_buf = 0;
  uint32_t mag_count = 0;

  uint32_t ctrl_ptr = REX_LOAD_U32(work + kCompressedHeaderData) +
                      REX_LOAD_U32(work + kNumBitsOffset);

  for (uint32_t bone = 0; bone < bone_count; ++bone) {
    // Ctrl word is big-endian on PPC.
    // REX_LOAD_U32 byte-swaps big-endian guest memory to host order, which is
    // exactly what assembling the four bytes by hand produced -- same value,
    // one load instead of four plus shifts.
    const uint32_t ctrl = REX_LOAD_U32(ctrl_ptr);
    ctrl_ptr += 4;

    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t idx = bone * 8u + k;
      if (idx >= out_ints) {
        return;
      }
      const uint32_t nb = (ctrl >> (k * 4u)) & 0xFu;
      if (nb == 0) {
        REX_STORE_U32(dest_ea + idx * 4u, 0);
        continue;
      }
      const uint32_t sel = sel_buf & 1u;
      sel_buf >>= 1;
      sel_count -= 1;
      if (sel == 0) {
        REX_STORE_U32(dest_ea + idx * 4u, 0);
        continue;
      }
      while (mag_count <= 56) {
        mag_buf |= uint64_t(REX_LOAD_U8(mag_ptr)) << mag_count;
        ++mag_ptr;
        mag_count += 8;
      }
      const uint32_t sign_stream = uint32_t(mag_buf & 1u);
      mag_buf >>= 1;
      mag_count -= 1;
      const uint32_t mag = uint32_t(mag_buf & ((1u << nb) - 1u));
      mag_buf >>= nb;
      mag_count -= nb;

      uint32_t fbits = AsU(float(mag));
      if (sign_stream == 0) {
        fbits |= 0x80000000u;  // stream sign 0 => negative
      }
      REX_STORE_U32(dest_ea + idx * 4u, fbits);
    }
    if (sel_count <= 8) {
      const uint32_t chunk = uint32_t(REX_LOAD_U8(sel_refill + 1)) |
                             (uint32_t(REX_LOAD_U8(sel_refill + 2)) << 8) |
                             (uint32_t(REX_LOAD_U8(sel_refill + 3)) << 16);
      sel_refill += 3;
      sel_buf = (sel_buf | (chunk << sel_count));
      sel_count += 24;
    }
  }
}

bool NativeTwoConsec(uint8_t* base, uint32_t self, uint32_t compressed_in,
                     uint32_t frame0, uint32_t frame1, uint32_t put0,
                     uint32_t put1, uint32_t tid,
                     uint32_t* out_extent = nullptr) {
  if (tid >= 6 || self == 0) {
    return false;
  }
  const uint32_t work = self + kWorkBuffers + tid * kWorkStride;
  const uint32_t hdr_outer = REX_LOAD_U32(work + 0);
  if (hdr_outer == 0) {
    return false;
  }
  const uint32_t hdr = REX_LOAD_U32(hdr_outer);
  if (hdr == 0) {
    return false;
  }

  const uint32_t num_channels = REX_LOAD_U8(hdr + 12);
  const uint32_t palette_size = REX_LOAD_U8(hdr + 13);
  const uint32_t num_frames = REX_LOAD_U16(hdr + 2);
  const uint32_t channel_info =
      (hdr + 16u + 4u * palette_size + 2u * ((num_frames + 7u) / 8u) + 0xFu) &
      ~0xFu;

  const float dct_min = AsF(REX_LOAD_U32(hdr + 4));
  const float dct_max = AsF(REX_LOAD_U32(hdr + 8));
  const float scale = std::fmax(std::fabs(dct_min), std::fabs(dct_max));
  const float scale_sq = scale * scale;
  const float quant_bias = scale * (1.0f / 32768.0f);

  const uint32_t scratch = REX_LOAD_U32(work + kScratch);
  // Use the ARGUMENT, not the work-buffer field. The guest sets
  // mCompressedData = r4 near the top of the call (stw r30,24(r27), with
  // add r31,r11,r30 making the bitstream r4 + numChannels), so reading the
  // field before writing it yields the PREVIOUS call's pointer. That is
  // invisible to a harness running after the guest -- the field is already
  // updated by then -- and is why the decode verified 48,000 times yet
  // produced wrong channel indices as a replacement.
  const uint32_t compressed = compressed_in;
  const uint32_t normalized = REX_LOAD_U32(work + kNormalizedData);
  const uint32_t frame_offset = REX_LOAD_U32(work + kFrameOffset);
  if (normalized == 0 || scratch == 0) {
    return false;
  }

  // Mirror the guest's work-buffer updates (stw r30,24(r27) / stw r21,12(r27)).
  REX_STORE_U32(work + kCompressedData, compressed_in);
  REX_STORE_U32(work + 12, 0);  // mUsedTableOffset

  // Unpack the bitstream into the guest's scratch. This is the step whose
  // absence made the first native attempt produce wrong animation: the
  // harness had been feeding it the guest's already-unpacked scratch.
  if (frame_offset != 0 && frame_offset * 8u <= kMaxScratchInts) {
    Unpack(base, compressed_in + num_channels, work, frame_offset, scratch,
           kMaxScratchInts);
  }

  const uint32_t aligned = (frame_offset + 3u) & ~3u;
  const uint32_t out0 = normalized + aligned * (frame0 & 7u) * 4u;
  const uint32_t out1 = normalized + aligned * (frame1 & 7u) * 4u;

  const uint32_t idct0 =
      self + kIdctCoefTables + (((frame0 & 7u) * 32u) & 0x1FE0u);
  const uint32_t idct1 =
      self + kIdctCoefTables + (((frame1 & 7u) * 32u) & 0x1FE0u);

  // ---- decode -----------------------------------------------------------
  for (uint32_t ch = 0; ch < num_channels && ch < 4; ++ch) {
    const uint32_t start = REX_LOAD_U16(work + 48 + ch * 2u);
    const uint32_t end = REX_LOAD_U16(work + 56 + ch * 2u);
    if (start == end || end > frame_offset) {
      continue;
    }

    const uint32_t entry = channel_info + ch * 16u;
    const float ch_min = AsF(REX_LOAD_U32(entry + 4));
    const float ch_max = AsF(REX_LOAD_U32(entry + 8));
    const float ch_range = ch_max - ch_min;

    const uint32_t ch_index = compressed != 0 ? REX_LOAD_U8(compressed + ch) : 0;
    const uint32_t initial = self + kInitialQuant + ch_index * 32u;

    float coef0[8];
    float coef1[8];
    for (int i = 0; i < 8; ++i) {
      const float q =
          AsF(REX_LOAD_U32(initial + uint32_t(i) * 4u)) * scale_sq + quant_bias;
      coef0[i] = q * AsF(REX_LOAD_U32(idct0 + uint32_t(i) * 4u));
      coef1[i] = q * AsF(REX_LOAD_U32(idct1 + uint32_t(i) * 4u));
    }
    const simde__m128 cv0_lo = CoefVec(coef0 + 0);
    const simde__m128 cv0_hi = CoefVec(coef0 + 4);
    const simde__m128 cv1_lo = CoefVec(coef1 + 0);
    const simde__m128 cv1_hi = CoefVec(coef1 + 4);

    const uint32_t total = end - start + 3u;
    const uint32_t chunks = (total & ~3u) >> 2;
    uint32_t in = scratch + start * 32u;
    uint32_t oi = start;
    for (uint32_t c = 0; c < chunks; ++c) {
      for (uint32_t slot = 0; slot < 4; ++slot, ++oi) {
        const uint32_t row = in + slot * 32u;
        const simde__m128 dv0 = GuestLoadF4(base, row + 0);
        const simde__m128 dv1 = GuestLoadF4(base, row + 16);
        const float s0 = simde_mm_cvtss_f32(simde_mm_add_ps(
            rex::ppc::simde_mm_vmsum4fp128_ps(dv0, cv0_lo),
            rex::ppc::simde_mm_vmsum4fp128_ps(dv1, cv0_hi)));
        const float s1 = simde_mm_cvtss_f32(simde_mm_add_ps(
            rex::ppc::simde_mm_vmsum4fp128_ps(dv0, cv1_lo),
            rex::ppc::simde_mm_vmsum4fp128_ps(dv1, cv1_hi)));
        REX_STORE_U32(out0 + oi * 4u, AsU((s0 + 0.5f) * ch_range + ch_min));
        REX_STORE_U32(out1 + oi * 4u, AsU((s1 + 0.5f) * ch_range + ch_min));
      }
      in += 128u;
    }
  }

  // ---- distribute -------------------------------------------------------
  uint32_t fallback = REX_LOAD_U32(work + kConstData);
  uint32_t is_const_p = REX_LOAD_U32(work + kIsConstantChannel);
  uint32_t prim = 0;
  // mNumConstantChannels bounds how many constant entries exist. The walk
  // consumes one isConst byte per joint and advances `fallback` per constant
  // joint; without this bound it runs past the array on animations with many
  // joints and few decoded entries (observed at joints 55/58/64 with
  // frameOffset 7-32: unpack and decode both correct, the walk off by one).
  const uint32_t num_const_avail = REX_LOAD_U32(work + 40);
  uint32_t const_used = 0;

  for (uint32_t ch = 0; ch < num_channels; ++ch) {
    const uint32_t entry = channel_info + ch * 16u;
    const uint32_t num_ent = REX_LOAD_U16(entry + 0);
    const uint32_t num_const = REX_LOAD_U16(entry + 2);
    const uint32_t size = REX_LOAD_U8(entry + 12);
    const uint32_t total = num_ent + num_const;
    if (total == 0) {
      continue;
    }
    const uint32_t blk = self + ch * 12u;  // mChannelBlocks[ch]
    // Hoisted out of the joint loop: these are per-channel constants that the
    // previous version re-read on every PutData call (three loads x two calls
    // x every joint).
    const uint32_t blk_off = REX_LOAD_U16(blk + 0);
    const uint32_t blk_stride = REX_LOAD_U16(blk + 2);
    const uint32_t blk_data = REX_LOAD_U32(blk + 4);
    const uint32_t blk_size = REX_LOAD_U8(blk + 8);
    const uint32_t copy_bytes = blk_size * 4u;
    if (copy_bytes > 64u) {
      return false;  // unexpected width; let the guest handle it
    }
    if (out_extent != nullptr) {
      const uint32_t end_byte =
          blk_stride * (total - 1u) + blk_off + copy_bytes;
      if (end_byte > *out_extent) {
        *out_extent = end_byte;
      }
    }

    for (uint32_t j = 0; j < total; ++j) {
      bool is_const = false;
      if (is_const_p != 0) {
        is_const = REX_LOAD_U8(is_const_p) != 0;
        ++is_const_p;
      }
      // A non-constant joint reads decoded entries [prim, prim+size); bail if
      // that would index past what we decoded (rare shape, guest handles it).
      if (!is_const && prim + size > frame_offset) {
        return false;
      }

      const uint32_t src0 = is_const ? fallback : out0 + prim * 4u;
      const uint32_t src1 = is_const ? fallback : out1 + prim * 4u;

      uint32_t d0;
      uint32_t d1;
      if (put0 != 0) {
        d0 = blk_stride * j + blk_off + put0;
      } else {
        if (blk_data == 0) return false;
        d0 = blk_stride * j + REX_LOAD_U32(blk_data + frame0 * 4u) + blk_off;
      }
      if (put1 != 0) {
        d1 = blk_stride * j + blk_off + put1;
      } else {
        if (blk_data == 0) return false;
        d1 = blk_stride * j + REX_LOAD_U32(blk_data + frame1 * 4u) + blk_off;
      }

      // Raw byte copy. The previous version did REX_LOAD_U32 (byte-swap) into
      // a staging array then REX_STORE_U32 (byte-swap back) -- the two swaps
      // cancel, so copying the bytes straight across is identical and drops
      // the staging array and both swaps per word.
      std::memcpy(REX_RAW_ADDR(d0), REX_RAW_ADDR(src0), copy_bytes);
      std::memcpy(REX_RAW_ADDR(d1), REX_RAW_ADDR(src1), copy_bytes);

      if (is_const) {
        if (++const_used > num_const_avail) {
          return false;
        }
        fallback += size * 4u;
      } else {
        prim += size;
      }
    }
  }
  return true;
}

}  // namespace

extern "C" REX_FUNC(
    sub_82E88BA0) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t off = REX_LOAD_U16(self + 0);
  const uint32_t stride = REX_LOAD_U16(self + 2);
  const uint32_t size = REX_LOAD_U8(self + 8);
  const uint32_t put_loc = ctx.r7.u32;
  uint32_t dest;
  if (put_loc != 0) {
    dest = stride * ctx.r5.u32 + off + put_loc;
  } else {
    const uint32_t data = REX_LOAD_U32(self + 4);
    if (data == 0) {
      __imp__sub_82E88BA0(
          ctx, base);
      return;
    }
    dest = stride * ctx.r5.u32 + REX_LOAD_U32(data + ctx.r4.u32 * 4u) + off;
  }
  for (uint32_t i = 0; i < size; ++i) {
    REX_STORE_U32(dest + i * 4u, REX_LOAD_U32(ctx.r6.u32 + i * 4u));
  }
}

extern "C" REX_FUNC(
    sub_82E89B80) {
  const uint32_t frame0 = ctx.r5.u32;
  // Block-crossing (frame0 & 7 == 7, ~12% of calls) needs two unpack passes
  // over one scratch buffer; NativeTwoConsec also returns false for shapes it
  // will not attempt (a joint indexing past the decoded range, a channel wider
  // than the staging buffer, or a walk running past mNumConstantChannels).
  // Everything it declines goes to the guest unchanged.
  if ((frame0 & 7u) == 7u ||
      !NativeTwoConsec(base, ctx.r3.u32, ctx.r4.u32, frame0, ctx.r6.u32,
                       ctx.r7.u32, ctx.r8.u32, ctx.r9.u32 & 0xFF)) {
    __imp__sub_82E89B80(
        ctx, base);
  }
}
