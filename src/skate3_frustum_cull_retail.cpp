// DoFrustumCulling (retail sub_82816A40): native replacement for the guest's
// per-object visibility pass, run by Sk8::VectorizedCullingJob on the job
// threads and by the guest render thread while it waits on that job.
//
// In dense views the render thread spends ~13% of its frame waiting for the
// culling job; most of the job is this function. The recompiled form runs
// every VMX op through the PPCContext register file (the function keeps ~100
// vector registers live and saves/restores 40 of them per call), which is
// most of its cost. This is a from-scratch version of the same arithmetic,
// derived from retail's own listing. (sk83_na_m has its own native version,
// src/skate3_frustum_cull.cpp; retail's differs: no LOD output, u16 masks,
// count at +16, and the distance cull only on the occluder path.)
//
// WHAT IT COMPUTES. r3 = work {+0 AABB array, +4 aux array or 0, +12 output,
// +16 u16 count}, r4 = camera, r5/r6 = occluders (5 planes x 96 bytes each) /
// count, r7 = 16-byte counter vector. Per group of four objects (min, max
// float4 each):
//   centre = (min + max) * 0.5            extent = max - centre
//   per frustum plane (cam+5152 + 16k, k < 6):
//     t = fma(ex,|nx|, fma(ey,|ny|, ez*|nz|)) + fma(cx,nx, fma(cy,ny, fma(cz,nz, d*1)))
//   visible = min(t) >= 0
//   if nothing is visible, or there are no occluders: done (the distance
//   cull below is never reached without occluders - retail's branch order).
//   occluders: hidden when all 8 corners satisfy 0 >= fma(dx,ax, fma(dy,ay,
//     dz*az)) for all 5 planes; visible &= ~hidden; stop once nothing is left.
//   distance cull (aux array present, something visible, aux.w not all 0):
//     depth  = max(fma(sx,n4x, fma(sy,n4y, sz*n4z)) + d4, 1), s = the box's
//              n-vertex against plane 4
//     size   = fma(ax,|cx|, fma(ay,|cy|, az*|cz|))  with c = [cam+5136]
//     screen = size * refine(1 / (depth * (depth * refine(1/f0))))
//     visible &= far ? screen >= [cam+5264] : true   (far = depth >= [cam+5268])
//   counter += 1 per lane visible after the frustum but not at the end.
//   output: the four lane masks as big-endian u16 (0xFFFF / 0).
//
// BIT-EXACTNESS. The recompiler emits vmaddfp/vnmsubfp as simde fmadd/fnmadd
// (single rounding with x86-64-v3), vrefp as an exact 1/x divide and
// vminfp/vmaxfp as min_ps/max_ps; the same intrinsics with the same operand
// order are used here (min_ps's NaN behaviour depends on order). The guest's
// own constant vectors (0, 0.5, 1, all-ones) are read from the same .rdata
// addresses. skate3_frustum_cull_verify runs this version, then the guest's,
// on every call and compares the outputs byte for byte.

#include "generated/skate3_init.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(skate3_frustum_cull_native, true, "Skate 3",
                    "Run the game's per-object visibility test "
                    "(DoFrustumCulling) natively instead of the recompiled "
                    "guest code. Same results; much cheaper in dense views.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_frustum_cull_verify, false, "Skate 3",
                    "DIAGNOSTIC: run both the native and the guest "
                    "DoFrustumCulling on every call and log any difference "
                    "in their outputs (the guest result is kept).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#pragma clang fp contract(off)

namespace skate3::frustum_cull {
namespace {

using F = simde__m128;
using I = simde__m128i;

// The guest's constant vectors (lvx128 from .rdata in the prologue).
constexpr uint32_t kConstZero = 0x8231A910;  // v63 / v109
constexpr uint32_t kConstOnes = 0x82312170;  // v111
constexpr uint32_t kConstHalf = 0x8232F650;  // v54
constexpr uint32_t kConstOne = 0x8231BA60;   // v17

// One guest vector in ELEMENT order: lane i = guest element i (each 32-bit
// element byte-swapped, the 16-byte block aligned down as lvx128 does).
inline I LoadE(uint8_t* base, uint32_t ea) {
  const I swap = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  return simde_mm_shuffle_epi8(
      simde_mm_loadu_si128((const simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)), swap);
}
inline F LoadEF(uint8_t* base, uint32_t ea) { return simde_mm_castsi128_ps(LoadE(base, ea)); }
inline void StoreE(uint8_t* base, uint32_t ea, I v) {
  const I swap = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  simde_mm_storeu_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu), simde_mm_shuffle_epi8(v, swap));
}

template <int N>
inline F Splat(F v) {
  return simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(N, N, N, N));
}

inline F Mul(F a, F b) { return simde_mm_mul_ps(a, b); }
inline F Add(F a, F b) { return simde_mm_add_ps(a, b); }
inline F Sub(F a, F b) { return simde_mm_sub_ps(a, b); }
inline F Fma(F a, F b, F c) { return simde_mm_fmadd_ps(a, b, c); }     // vmaddfp
inline F Fnma(F a, F b, F c) { return simde_mm_fnmadd_ps(a, b, c); }   // vnmsubfp
inline F Recip(F a) { return simde_mm_div_ps(simde_mm_set1_ps(1.0f), a); }  // vrefp
inline F Sel(F a, F b, F m) {  // vsel: m ? b : a, bitwise
  return simde_mm_or_ps(simde_mm_andnot_ps(m, a), simde_mm_and_ps(m, b));
}
// vcmpequw. against vspltisw 0 + the CR6 "all equal" bit: every bit zero.
inline bool AllZero(F m) {
  return simde_mm_testz_si128(simde_mm_castps_si128(m), simde_mm_castps_si128(m)) != 0;
}

// Rows r0..r3 (element order) -> columns: x = (r0.x, r1.x, r2.x, r3.x), ...
inline void Transpose(F r0, F r1, F r2, F r3, F& x, F& y, F& z, F& w) {
  const F t0 = simde_mm_unpacklo_ps(r0, r1);
  const F t1 = simde_mm_unpacklo_ps(r2, r3);
  const F t2 = simde_mm_unpackhi_ps(r0, r1);
  const F t3 = simde_mm_unpackhi_ps(r2, r3);
  x = simde_mm_movelh_ps(t0, t1);
  y = simde_mm_movehl_ps(t1, t0);
  z = simde_mm_movelh_ps(t2, t3);
  w = simde_mm_movehl_ps(t3, t2);
}

struct OccPlane {
  F ox, oy, oz, ax, ay, az;
};

// Verify coverage: this call reached the occluder stage / the distance cull.
thread_local bool t_hit_occ = false;
thread_local bool t_hit_dist = false;

// Returns false WITHOUT having written anything when the call reaches the
// distance / screen-size cull and `allow_dist` is off: that stage never ran
// in any verified session (every live call had no aux array or aux.w == 0),
// so it is left to the guest rather than trusted unverified. Outputs and the
// counter are committed only at the end, so bailing out is side-effect free.
bool RunNative(PPCContext& ctx, uint8_t* base, bool allow_dist) {
  ctx.fpscr.enableFlushMode();

  const uint32_t work = ctx.r3.u32;
  const uint32_t cam = ctx.r4.u32;
  const uint32_t occl_base = ctx.r5.u32;
  const uint32_t occl_count = ctx.r6.u32;
  const uint32_t counter_addr = ctx.r7.u32;

  const uint32_t count = REX_LOAD_U16(work + 16);
  if (count == 0) {
    return true;  // the guest's `ble` straight to the epilogue (lhz: never negative)
  }
  const uint32_t groups = ((count - 1) >> 2) + 1;
  const uint32_t aabb_base = REX_LOAD_U32(work + 0);
  const uint32_t aux_base = REX_LOAD_U32(work + 4);
  const uint32_t out_base = REX_LOAD_U32(work + 12);

  const F zero = LoadEF(base, kConstZero);
  const F ones = LoadEF(base, kConstOnes);
  const F half = LoadEF(base, kConstHalf);
  const F one = LoadEF(base, kConstOne);
  const F sign = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)));  // vslw(-1,-1)

  F nx[6], ny[6], nz[6], dw[6], ax[6], ay[6], az[6];
  for (int k = 0; k < 6; ++k) {
    const F p = LoadEF(base, cam + 5152u + uint32_t(k) * 16u);
    nx[k] = Splat<0>(p);
    ny[k] = Splat<1>(p);
    nz[k] = Splat<2>(p);
    dw[k] = Mul(one, Splat<3>(p));  // vmulfp v118.., v17, plane.w
    ax[k] = simde_mm_andnot_ps(sign, nx[k]);
    ay[k] = simde_mm_andnot_ps(sign, ny[k]);
    az[k] = simde_mm_andnot_ps(sign, nz[k]);
  }
  // Plane 4's raw d (v110) feeds the depth term unscaled.
  const F d4 = Splat<3>(LoadEF(base, cam + 5152u + 4u * 16u));
  const F caxis = LoadEF(base, cam + 5136u);
  const F cax = simde_mm_andnot_ps(sign, Splat<0>(caxis));
  const F cay = simde_mm_andnot_ps(sign, Splat<1>(caxis));
  const F caz = simde_mm_andnot_ps(sign, Splat<2>(caxis));

  // f0 / cull size / cull depth: lfs + stfs into a stack vector, so exact.
  PPCRegister t{};
  t.u32 = REX_LOAD_U32(cam + 5272u);
  const F f0 = simde_mm_set1_ps(t.f32);
  t.u32 = REX_LOAD_U32(cam + 5264u);
  const F cull_size = simde_mm_set1_ps(t.f32);
  t.u32 = REX_LOAD_U32(cam + 5268u);
  const F cull_depth = simde_mm_set1_ps(t.f32);
  F inv_f0 = Recip(f0);
  inv_f0 = Fma(inv_f0, Fnma(inv_f0, f0, one), inv_f0);  // v102 = v0*v11 + v0

  // Plane 4's per-axis signs for the n-vertex (vcmpgefp n, 0).
  const F n4z_pos = simde_mm_cmpge_ps(nz[4], zero);
  const F n4y_pos = simde_mm_cmpge_ps(ny[4], zero);
  const F n4x_pos = simde_mm_cmpge_ps(nx[4], zero);

  const bool have_occ = occl_count != 0;
  static thread_local std::vector<OccPlane> occ;
  if (have_occ) {
    occ.resize(size_t(occl_count) * 5u);
    uint32_t p = occl_base;
    for (size_t i = 0; i < occ.size(); ++i, p += 96u) {
      occ[i] = {LoadEF(base, p + 0), LoadEF(base, p + 16), LoadEF(base, p + 32),
                LoadEF(base, p + 48), LoadEF(base, p + 64), LoadEF(base, p + 80)};
    }
  }

  I counter_acc = simde_mm_setzero_si128();
  bool counter_touched = false;
  uint32_t aabb_p = aabb_base;
  uint32_t aux_p = aux_base;
  static thread_local std::vector<uint16_t> out_buf;
  out_buf.resize(size_t(groups) * 4u);
  for (uint32_t g = 0; g < groups; ++g, aabb_p += 128u) {
    F min_x, min_y, min_z, min_w, max_x, max_y, max_z, max_w;
    Transpose(LoadEF(base, aabb_p + 0), LoadEF(base, aabb_p + 32), LoadEF(base, aabb_p + 64),
              LoadEF(base, aabb_p + 96), min_x, min_y, min_z, min_w);
    Transpose(LoadEF(base, aabb_p + 16), LoadEF(base, aabb_p + 48), LoadEF(base, aabb_p + 80),
              LoadEF(base, aabb_p + 112), max_x, max_y, max_z, max_w);
    (void)min_w;
    (void)max_w;
    F aux_x = zero, aux_y = zero, aux_z = zero, aux_w = zero;
    if (aux_base != 0) {
      Transpose(LoadEF(base, aux_p + 0), LoadEF(base, aux_p + 16), LoadEF(base, aux_p + 32),
                LoadEF(base, aux_p + 48), aux_x, aux_y, aux_z, aux_w);
      aux_p += 64u;
    }

    const F cz = Mul(Add(min_z, max_z), half);
    const F cy = Mul(Add(min_y, max_y), half);
    const F cx = Mul(Add(min_x, max_x), half);
    const F ez = Sub(max_z, cz);
    const F ey = Sub(max_y, cy);
    const F ex = Sub(max_x, cx);

    F tk[6];
    for (int k = 0; k < 6; ++k) {
      const F dist = Fma(cx, nx[k], Fma(cy, ny[k], Fma(cz, nz[k], dw[k])));
      const F rad = Fma(ex, ax[k], Fma(ey, ay[k], Mul(ez, az[k])));
      tk[k] = Add(rad, dist);
    }
    const F m01 = simde_mm_min_ps(tk[0], tk[1]);
    const F m23 = simde_mm_min_ps(tk[2], tk[3]);
    const F m45 = simde_mm_min_ps(tk[4], tk[5]);
    const F worst = simde_mm_min_ps(simde_mm_min_ps(m01, m23), m45);
    F visible = simde_mm_cmpge_ps(worst, zero);

    if (!AllZero(visible) && have_occ) {
      const F after_frustum = visible;
      t_hit_occ = true;
      for (uint32_t o = 0; o < occl_count; ++o) {
        F hidden = ones;
        for (int pl = 0; pl < 5; ++pl) {
          const OccPlane& q = occ[size_t(o) * 5u + size_t(pl)];
          const F zt[2] = {Mul(Sub(max_z, q.oz), q.az), Mul(Sub(min_z, q.oz), q.az)};
          const F yd[2] = {Sub(max_y, q.oy), Sub(min_y, q.oy)};
          const F xd[2] = {Sub(max_x, q.ox), Sub(min_x, q.ox)};
          for (int c = 0; c < 8; ++c) {
            const F acc = Fma(xd[(c >> 2) & 1], q.ax, Fma(yd[(c >> 1) & 1], q.ay, zt[c & 1]));
            hidden = simde_mm_and_ps(hidden, simde_mm_cmpge_ps(zero, acc));
          }
        }
        visible = simde_mm_and_ps(visible, simde_mm_xor_ps(hidden, ones));
        if (AllZero(visible)) {
          break;
        }
      }
      // (aux.w all-zero is an integer test against vspltisw 0: -0.0f counts
      // as non-zero, exactly as in the guest.)
      if (aux_base != 0 && !AllZero(visible) && !AllZero(aux_w)) {
        t_hit_dist = true;
        if (!allow_dist) {
          return false;
        }
        const F sz = Sel(max_z, min_z, n4z_pos);
        const F sy = Sel(max_y, min_y, n4y_pos);
        const F sx = Sel(max_x, min_x, n4x_pos);
        const F size = Fma(aux_x, cax, Fma(aux_y, cay, Mul(aux_z, caz)));
        const F depth0 = Fma(sx, nx[4], Fma(sy, ny[4], Mul(sz, nz[4])));
        const F depth = simde_mm_max_ps(Add(depth0, d4), one);
        const F d_f0 = Mul(depth, inv_f0);
        const F is_far = simde_mm_cmpge_ps(depth, cull_depth);
        const F denom = Mul(depth, d_f0);
        F r = Recip(denom);
        r = Fma(r, Fnma(r, denom, one), r);
        const F screen = Mul(size, r);
        const F big = simde_mm_cmpge_ps(screen, cull_size);
        const F keep = Sel(visible, big, is_far);
        visible = Sel(zero, visible, keep);
      }
      counter_acc = simde_mm_add_epi32(
          counter_acc, simde_mm_and_si128(simde_mm_castps_si128(simde_mm_xor_ps(after_frustum, visible)),
                                          simde_mm_set1_epi32(1)));
      counter_touched = true;
    }

    alignas(16) uint32_t lanes[4];
    simde_mm_store_si128((simde__m128i*)lanes, simde_mm_castps_si128(visible));
    for (int i = 0; i < 4; ++i) {
      out_buf[size_t(g) * 4u + size_t(i)] = uint16_t(lanes[i]);
    }
  }
  for (size_t i = 0; i < out_buf.size(); ++i) {
    REX_STORE_U16(out_base + uint32_t(i) * 2u, out_buf[i]);
  }
  if (counter_touched) {
    StoreE(base, counter_addr, simde_mm_add_epi32(LoadE(base, counter_addr), counter_acc));
  }
  return true;
}

std::atomic<uint64_t> g_verify_calls{0};
std::atomic<uint64_t> g_verify_bad{0};
std::atomic<uint64_t> g_verify_occ{0};
std::atomic<uint64_t> g_verify_dist{0};

void LogConstantsOnce(uint8_t* base) {
  static std::atomic<bool> s_logged{false};
  if (s_logged.exchange(true)) {
    return;
  }
  const auto f = [&](uint32_t ea) {
    alignas(16) float v[4];
    simde_mm_store_ps(v, LoadEF(base, ea));
    return fmt::format("({}, {}, {}, {})", v[0], v[1], v[2], v[3]);
  };
  REXLOG_INFO("frustum-cull: native DoFrustumCulling; guest constants zero={} ones={} half={} one={}",
              f(kConstZero), f(kConstOnes), f(kConstHalf), f(kConstOne));
}

}  // namespace

// Called from the sub_82816A40 hook (skate3_vr_guest_cull.cpp) after the VR
// plane override. Returns false when the guest body should run instead.
bool Run(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(skate3_frustum_cull_native)) {
    return false;
  }
  LogConstantsOnce(base);
  if (!REXCVAR_GET(skate3_frustum_cull_verify)) {
    return RunNative(ctx, base, /*allow_dist=*/false);
  }
  // Verify: native first (into the real buffers), keep its bytes, restore
  // the originals, run the guest, compare. The guest's result is the one
  // that stays.
  const uint32_t work = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U16(work + 16);
  const uint32_t groups = count == 0 ? 0 : ((count - 1) >> 2) + 1;
  const uint32_t out = REX_LOAD_U32(work + 12);
  const uint32_t ctr = ctx.r7.u32 & ~0xFu;
  thread_local std::vector<uint8_t> orig_out, native_out;
  uint8_t orig_ctr[16], native_ctr[16];
  orig_out.assign(base + out, base + out + size_t(groups) * 8);
  std::memcpy(orig_ctr, base + ctr, 16);
  PPCContext saved = ctx;
  t_hit_occ = t_hit_dist = false;
  RunNative(ctx, base, /*allow_dist=*/true);
  if (t_hit_occ) g_verify_occ.fetch_add(1, std::memory_order_relaxed);
  if (t_hit_dist) g_verify_dist.fetch_add(1, std::memory_order_relaxed);
  native_out.assign(base + out, base + out + size_t(groups) * 8);
  std::memcpy(native_ctr, base + ctr, 16);
  std::memcpy(base + out, orig_out.data(), orig_out.size());
  std::memcpy(base + ctr, orig_ctr, 16);
  ctx = saved;
  __imp__sub_82816A40(ctx, base);
  const bool same_out = std::memcmp(base + out, native_out.data(), native_out.size()) == 0;
  const bool same_ctr = std::memcmp(base + ctr, native_ctr, 16) == 0;
  const uint64_t n = g_verify_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!same_out || !same_ctr) {
    const uint64_t bad = g_verify_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 20) {
      size_t first = 0;
      while (first < native_out.size() && base[out + first] == native_out[first]) ++first;
      REXLOG_WARN(
          "frustum-cull VERIFY MISMATCH #{}: count={} groups={} occl={} aux={:08X} out_ok={} "
          "ctr_ok={} first_diff_byte={} guest={:02X} native={:02X}",
          bad, count, groups, saved.r6.u32, REX_LOAD_U32(work + 4), same_out, same_ctr, first,
          first < native_out.size() ? base[out + first] : 0,
          first < native_out.size() ? native_out[first] : 0);
    }
  }
  if ((n & 0xFFFFF) == 0) {
    REXLOG_INFO("frustum-cull verify: {} calls ({} reached occluders, {} the distance cull), "
                "{} mismatches",
                n, g_verify_occ.load(std::memory_order_relaxed),
                g_verify_dist.load(std::memory_order_relaxed),
                g_verify_bad.load(std::memory_order_relaxed));
  }
  return true;
}

}  // namespace skate3::frustum_cull
