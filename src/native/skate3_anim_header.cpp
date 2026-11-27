// ANIM_CODEC::VBRDecompressor::InitPerAnim and UnPackHeaderBits: native replacements
// (cvar sk83_native_anim_header).
//
// RETAIL PORT (from sk83_na_m src/native/sk83_anim_header.cpp): retail InitPerAnim = sub_82E88FA8
// (dev 0x831CDDD8), UnPackHeaderBits = sub_82E8A780 (dev 0x831CD7C0), read from retail's listing:
// same data flow and fused fmadds lowering, but LTCG changed the interfaces and dropped the
// asserts -- retail UnPackHeaderBits takes the work buffer in r3 (dev: this r3, W r4) and retail
// InitPerAnim has no alignment / tid / channel-count / per-channel checks (its channel loop is
// unrolled by two, UnPackHeaderBits' by four). So the native retail InitPerAnim never falls back.
// The description below is the dev listing; offsets and results are identical.
//
// They run once per animation record inside every Andale fetch (Vector_ExtractPackedNVBR),
// which sits on the living-world job's critical path. Profiled inside the native fetch they were
// still recompiled: UnPackHeaderBits 14.5%, InitPerAnim 5% self + its 2 KB XMemSet 4.8%.
//
// Transcribed from the recompiled listing (generated/sk83_na_m_recomp.116.cpp):
//
//   InitPerAnim(this, data, header, tid):
//     W  = this + 9264 + tid*64            DataPerDecompress (mWorkBuffers[tid])
//     hp = [W+0]                           per-header cache: +0 header, +4 header+16,
//                                          +8 channel table (16 B entries), +12, +16 byte flag
//     [W+20] = data
//     if [hp] != header: refill hp from the header (+13 count of u32s, +2 u16 frame count)
//     n = header[12]; for each channel i < n with e = table + i*16, c = e[12]:
//       starts[i] = i ? ends[i-1] : 0;  ends[i] = starts[i] + e.u16[0]*c;  sum += e.u16[2]*c
//       (u16 arrays at W+48 / W+56)
//     [W+40] = sum; [W+44] = u16 at W+(n+27)*2; [this+11696+tid*2052] = 0;
//     zero 2048 B at this+9648+tid*2052; [W+8] = UnPackHeaderBits(this, W) (also returned)
//
//   UnPackHeaderBits(this, W):
//     src = [W+20] (bytes); out = [W+32] (floats); table = [hp+4] (floats indexed by byte)
//     per channel i < hdr[12]: min = e.f32[1], range = float(e.f32[2] - min),
//       e[12]*e.u16[1] times: *out++ = float(fma(table[*src++], range, min))   (fmadds)
//     flags = [W+36]: hdr.u16[0] run lengths from src, alternating 0/1 starting with 0
//       (no runs: hdr[12] zero bytes at flags)
//     [W+16] = src - [W+20];  return ([W+44]*4 & 0x7FFFFFFC) + (src - [W+20])
//
// The guest's asserts (alignment, tid < 6, channel-count / per-channel checks against the
// decompressor) are kept by handing any call that would trip one to the recompiled body.
// sk83_native_anim_header_verify runs both from the same memory and compares every byte they
// write.
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma clang fp contract(off)

REXCVAR_DEFINE_BOOL(sk83_native_anim_header, true, "Skate 3",
                    "Native ANIM_CODEC VBRDecompressor InitPerAnim / UnPackHeaderBits (bit-exact).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_anim_header_verify, false, "Skate 3",
                    "DIAGNOSTIC: run the recompiled InitPerAnim too and compare every written byte.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

inline float LoadF32(uint8_t* base, uint32_t a) {
  const uint32_t u = REX_LOAD_U32(a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void StoreF32(uint8_t* base, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  REX_STORE_U32(a, u);
}

uint32_t UnPackHeaderBits(uint8_t* base, uint32_t dpd) {
  const uint32_t hp = REX_LOAD_U32(dpd + 0);
  uint32_t out = REX_LOAD_U32(dpd + 32);
  uint32_t src = REX_LOAD_U32(dpd + 20);
  const uint32_t table = REX_LOAD_U32(hp + 4);
  const uint32_t hdr = REX_LOAD_U32(hp + 0);
  const uint32_t nch = REX_LOAD_U8(hdr + 12);
  if (nch) {
    const uint32_t chans = REX_LOAD_U32(hp + 8);
    for (uint32_t i = 0; i < nch; ++i) {
      const uint32_t e = chans + ((i << 4) & 0xFF0u);
      const uint32_t cnt = REX_LOAD_U8(e + 12) * uint32_t(REX_LOAD_U16(e + 2));
      const double mn = double(LoadF32(base, e + 4));
      const double range = double(float(double(LoadF32(base, e + 8)) - mn));
      for (uint32_t k = 0; k < cnt; ++k) {
        const uint32_t b = REX_LOAD_U8(src);
        ++src;
        const double v = double(LoadF32(base, table + (b << 2)));
        StoreF32(base, out, float(std::fma(v, range, mn)));
        out += 4;
      }
    }
  }
  const uint32_t runs = REX_LOAD_U16(hdr + 0);
  uint32_t flags = REX_LOAD_U32(dpd + 36);
  if (runs) {
    uint8_t val = 0;
    for (uint32_t j = 0; j < runs; ++j) {
      const uint32_t c = REX_LOAD_U8(src);
      if (c) {
        std::memset(REX_RAW_ADDR(flags), val, c);
        flags += c;
      }
      val = val == 0 ? 1 : 0;
      ++src;
    }
  } else {
    const uint32_t c = REX_LOAD_U8(REX_LOAD_U32(hp + 0) + 12);
    if (c) std::memset(REX_RAW_ADDR(flags), 0, c);
  }
  const uint32_t start = REX_LOAD_U32(dpd + 20);
  REX_STORE_U32(dpd + 16, src - start);
  return ((REX_LOAD_U32(dpd + 44) << 2) & 0x7FFFFFFCu) + (src - start);
}

// false = a guest assert would fire (nothing written yet that the recompiled body would not
// write the same way): run the recompiled body instead.
bool InitPerAnim(uint8_t* base, uint32_t self, uint32_t data, uint32_t header, uint32_t tid_reg,
                 uint32_t* ret) {
  const uint32_t tid = tid_reg & 0xFF;  // retail: no alignment / tid / channel-count asserts
  const uint32_t nch = REX_LOAD_U8(header + 12);
  const uint32_t W = self + 9264 + tid * 64;
  const uint32_t hp = REX_LOAD_U32(W + 0);
  // the channel table the call will use, for the per-channel assert pre-check
  uint32_t chans;
  if (REX_LOAD_U32(hp + 0) != header) {
    const uint32_t p = header + 16 + (uint32_t(REX_LOAD_U8(header + 13)) << 2);
    const int32_t frames = int32_t(REX_LOAD_U16(header + 2)) + 7;
    const int32_t blocks = frames / 8;  // srawi + addze: rounds toward zero
    chans = (p + uint32_t(blocks) * 2u + 15u) & ~15u;
  } else {
    chans = REX_LOAD_U32(hp + 8);
  }

  REX_STORE_U32(W + 20, data);
  if (REX_LOAD_U32(hp + 0) != header) {
    REX_STORE_U32(hp + 0, header);
    REX_STORE_U32(hp + 4, header + 16);
    REX_STORE_U8(hp + 16, 1);
    const uint32_t p = header + 16 + (uint32_t(REX_LOAD_U8(header + 13)) << 2);
    REX_STORE_U32(hp + 12, p);
    REX_STORE_U32(hp + 8, chans);
  }
  uint32_t sum = 0;
  for (uint32_t i = 0; i < nch; ++i) {
    const uint32_t e = chans + ((i << 4) & 0xFF0u);
    const uint32_t c = REX_LOAD_U8(e + 12);
    const uint16_t start = i == 0 ? 0 : REX_LOAD_U16(W + (i + 27) * 2);
    REX_STORE_U16(W + (i + 24) * 2, start);
    REX_STORE_U16(W + (i + 28) * 2, uint16_t(uint32_t(REX_LOAD_U16(e + 0)) * c + start));
    sum += uint32_t(REX_LOAD_U16(e + 2)) * c;
  }
  REX_STORE_U32(W + 40, sum);
  REX_STORE_U32(W + 44, REX_LOAD_U16(W + (nch + 27) * 2));
  const uint32_t q = self + tid * 2052;
  REX_STORE_U32(q + 11696, 0);
  std::memset(REX_RAW_ADDR(q + 9648), 0, 2048);
  *ret = UnPackHeaderBits(base, W);
  REX_STORE_U32(W + 8, *ret);
  return true;
}

// ---- verify --------------------------------------------------------------------------------
struct Region {
  uint32_t addr, len;
};
std::atomic<uint64_t> g_v_calls{0}, g_v_bad{0}, g_v_fallback{0};

void Snap(uint8_t* base, const std::vector<Region>& rs, std::vector<uint8_t>& out) {
  out.clear();
  for (const Region& r : rs) out.insert(out.end(), REX_RAW_ADDR(r.addr), REX_RAW_ADDR(r.addr) + r.len);
}
void Restore(uint8_t* base, const std::vector<Region>& rs, const std::vector<uint8_t>& in) {
  size_t o = 0;
  for (const Region& r : rs) {
    std::memcpy(REX_RAW_ADDR(r.addr), in.data() + o, r.len);
    o += r.len;
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82E8A780) {  // retail UnPackHeaderBits(W in r3)
  if (!REXCVAR_GET(sk83_native_anim_header) || REXCVAR_GET(sk83_native_anim_header_verify)) {
    __imp__sub_82E8A780(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.r3.u64 = UnPackHeaderBits(base, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_82E88FA8) {
  if (!REXCVAR_GET(sk83_native_anim_header)) {
    __imp__sub_82E88FA8(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, data = ctx.r4.u32, header = ctx.r5.u32, tid = ctx.r6.u32;
  if (REXCVAR_GET(sk83_native_anim_header_verify)) {
    // Regions either version writes: W, the header cache, the quant slot, the float output
    // (sum floats) and the flag bytes (bounded by the header's run total / channel count).
    const uint32_t t = tid & 0xFF, W = self + 9264 + t * 64, hp = REX_LOAD_U32(W + 0);
    thread_local std::vector<uint8_t> before, guest;
    // Run the guest first on a snapshot to learn the output extents.
    std::vector<Region> rs = {{W, 64}, {hp, 17}, {self + 9648 + t * 2052, 2052}};  // exactly the written bytes: neighbours may belong to other threads
    Snap(base, rs, before);
    // exact extents of the variable outputs, from the header the call will use
    const uint32_t nch = REX_LOAD_U8(header + 12);
    uint32_t chans;
    if (REX_LOAD_U32(hp + 0) != header) {
      const uint32_t p = header + 16 + (uint32_t(REX_LOAD_U8(header + 13)) << 2);
      chans = (p + uint32_t((int32_t(REX_LOAD_U16(header + 2)) + 7) / 8) * 2u + 15u) & ~15u;
    } else {
      chans = REX_LOAD_U32(hp + 8);
    }
    uint32_t nfloats = 0;
    for (uint32_t i = 0; i < nch; ++i) {
      const uint32_t e = chans + ((i << 4) & 0xFF0u);
      nfloats += REX_LOAD_U8(e + 12) * uint32_t(REX_LOAD_U16(e + 2));
    }
    const uint32_t runs = REX_LOAD_U16(header + 0);
    uint32_t nflags = runs ? 0 : nch;
    for (uint32_t j = 0; j < runs; ++j) nflags += REX_LOAD_U8(data + nfloats + j);
    if (nfloats) rs.push_back({REX_LOAD_U32(W + 32), nfloats * 4});
    if (nflags) rs.push_back({REX_LOAD_U32(W + 36), nflags});
    Snap(base, rs, before);
    PPCContext g = ctx;
    __imp__sub_82E88FA8(g, base);
    Snap(base, rs, guest);
    Restore(base, rs, before);
    uint32_t r = 0;
    ctx.fpscr.disableFlushModeUnconditional();
    if (!InitPerAnim(base, self, data, header, tid, &r)) {
      Restore(base, rs, guest);
      ctx.r3.u64 = g.r3.u64;
      g_v_fallback++;
      return;
    }
    std::vector<uint8_t> mine;
    Snap(base, rs, mine);
    const uint64_t c = g_v_calls.fetch_add(1) + 1;
    if (mine != guest || r != g.r3.u32) {
      if (g_v_bad.fetch_add(1) < 8) {
        size_t o = 0, first = SIZE_MAX;
        for (size_t i = 0; i < mine.size(); ++i)
          if (mine[i] != guest[i]) { first = i; break; }
        for (const Region& rr : rs) {
          if (first != SIZE_MAX && first < o + rr.len) {
            REXLOG_WARN("anim header verify: diff at region {:08X}+{} (native {:02X} guest {:02X}); ret {:08X} vs {:08X}",
                        rr.addr, first - o, mine[first], guest[first], r, g.r3.u32);
            break;
          }
          o += rr.len;
        }
        if (first == SIZE_MAX) REXLOG_WARN("anim header verify: return {:08X} vs guest {:08X}", r, g.r3.u32);
      }
      Restore(base, rs, guest);  // keep the game on the guest's result
      ctx.r3.u64 = g.r3.u64;
    }
    ctx.r3.u64 = g.r3.u64;
    if ((c & 0xFFFF) == 0)
      REXLOG_INFO("anim header verify: {} calls, {} differ, {} fell back", c, g_v_bad.load(), g_v_fallback.load());
    return;
  }
  uint32_t r = 0;
  ctx.fpscr.disableFlushModeUnconditional();
  if (InitPerAnim(base, self, data, header, tid, &r)) {
    ctx.r3.u64 = r;
    return;
  }
  __imp__sub_82E88FA8(ctx, base);
}
