// DoFrustumCulling: native replacement for the guest's world visibility pass.
//
// The guest function is 2.86% of all process CPU. Its recompiled form compiles
// to ~1578 x86 instructions for ~225 instructions of real work: it emulates a
// machine with 128 vector registers and explicit condition-register semantics,
// so every VMX result round-trips through the PPCContext register file and
// every `vcmp.` materialises a CR field through mfocrf. Worse, capture showed
// the pass runs ~3400 times a second with only ONE TO FOUR objects per call,
// so fixed overhead dominates: __savevmx_88/__restvmx_88 alone save and
// restore 40 vector registers per call, and the six frustum planes are
// re-split and re-abs'd every time.
//
// This is a from-scratch implementation. It does not emulate the machine: no
// register file, no condition registers, ~15 live vectors instead of 104.
//
// WHAT IT COMPUTES, recovered from the recompiled listing. An earlier
// version of this file was validated against 48 recorded calls and still had
// the distance/screen-size cull missing entirely -- every one of those samples
// was close-range, so the clause never fired. Correctness here rests on the
// in-game differential harness across millions of calls, not on that sample
// set.
//
//   centre = (min + max) * 0.5            extent = max - centre
//   per plane: t = extent.|n| + (centre.n + d)      [accumulated z, y, x]
//   visible   = min(t over the 6 planes) >= 0
//   occluded  = for some occluder, all 8 AABB corners lie behind all 5 of
//               its planes
//
//   then a distance / screen-size cull, applied ONLY when there is an aux
//   array, something is still visible, and aux W is non-zero (the guest
//   branches around it otherwise, which matters because LOD-disabled cameras
//   have f0 == 0 and the reciprocal chain would go to NaN):
//     depth  = the box's n-vertex against plane 4 (min or max per axis by the
//              sign of that axis normal), + d, clamped to >= 1
//     size   = aux . |[cam+5136]|          (zero when the aux array is null)
//     screen = size * refined(f0 / depth^2)
//     visible &= (depth < [cam+5268]) || (screen >= [cam+5264])
//
//   and the stored LOD value:
//     w = minW + [cam+5252]   num = w * [cam+5248]
//     dist = 1 / (1 / sqrt(dx^2 + (dy^2 + dz^2)))  from the point at cam+5120
//     den = dist - w,  r = 1/den refined by one Newton-Raphson step
//     value = (den >= w) ? num*r : [cam+5248],  then zeroed if < [cam+5256]
//   out lane  = visible ? value : 0
//   counter  += 1 per lane that frustum culling passed but occlusion removed
//
// BIT-EXACTNESS. The emulation lowers vrefp and vrsqrtefp to exact IEEE
// divides (not the PPC estimate tables) and vmaddfp to an unfused
// add(mul(a,b),c), so the same operations in the same order reproduce it
// exactly. The Newton-Raphson step refines an already-exact reciprocal --
// wasted work the guest does anyway -- and is kept, because removing it would
// perturb the result.
//
// CAVEAT, stated plainly: 96 recorded calls across two sessions all had
// occluder count == 0, so the occluder loop below is transcribed faithfully
// from the listing but has NOT been verified against live data. Everything
// else has.

#include "generated/skate3_init.h"

#include <cstdint>
#include <vector>

namespace {


using F = simde__m128;
using I = simde__m128i;

// lvx128 / stvx128: the guest is big-endian, so a 16-byte access reverses the
// whole vector. A float4 (x,y,z,w) in memory lands in lanes [w,z,y,x].
inline I GuestLoad(uint8_t* base, uint32_t ea) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128((const simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
      simde_mm_load_si128((const simde__m128i*)VectorMaskL));
}

inline void GuestStore(uint8_t* base, uint32_t ea, I v) {
  simde_mm_store_si128(
      (simde__m128i*)REX_RAW_ADDR(ea & ~0xFu),
      simde_mm_shuffle_epi8(v, simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
}

inline float GuestLoadF(uint8_t* base, uint32_t ea) {
  PPCRegister t{};
  t.u32 = REX_LOAD_U32(ea);
  return t.f32;
}

// vspltw element N: with the reversed load, guest element N is lane 3 - N.
template <int N>
inline F Splat(I v) {
  constexpr int kImm = (N == 0) ? 0xFF : (N == 1) ? 0xAA : (N == 2) ? 0x55 : 0x00;
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v, kImm));
}

inline F Mul(F a, F b) { return simde_mm_mul_ps(a, b); }
inline F Add(F a, F b) { return simde_mm_add_ps(a, b); }
inline F Sub(F a, F b) { return simde_mm_sub_ps(a, b); }
// vmaddfp is NOT fused in the emulation: two roundings, and we match that.
inline F Madd(F a, F b, F c) { return simde_mm_add_ps(simde_mm_mul_ps(a, b), c); }
inline F Abs(F a) {
  return simde_mm_andnot_ps(
      simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))), a);
}
inline F Recip(F a) { return simde_mm_div_ps(simde_mm_set1_ps(1.0f), a); }

// The guest's two-stage 4x4 transpose. Rows are passed in REVERSE object
// order because the guest lands object i in lane 3-i: its stvx128 reverses
// all 16 bytes on the way out, so lane 3 is the first dword in memory. (An
// earlier version had this mirrored and still "verified" against recorded
// data, because the offline check compared model lane i with memory dword i
// -- two mirror errors that cancelled.)
inline void Transpose(I r0, I r1, I r2, I r3, F& x, F& y, F& z, F& w) {
  const I a = simde_mm_unpacklo_epi32(r0, r2);
  const I b = simde_mm_unpacklo_epi32(r1, r3);
  const I c = simde_mm_unpackhi_epi32(r0, r2);
  const I d = simde_mm_unpackhi_epi32(r1, r3);
  z = simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(a, b));
  w = simde_mm_castsi128_ps(simde_mm_unpacklo_epi32(a, b));
  x = simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(c, d));
  y = simde_mm_castsi128_ps(simde_mm_unpacklo_epi32(c, d));
}

// One occluder plane, already byte-swapped and splatted. The guest re-reads
// this from guest memory for every group of 4 objects even though it cannot
// change between groups; we read it once per call instead.
struct OccPlane {
  F ox, oy, oz, ax, ay, az;
};

}  // namespace

extern "C" REX_FUNC(sub_82816A40) {
  // The guest runs its vector work with VMX flush-to-zero established; match
  // it so denormal inputs round identically.
  ctx.fpscr.enableFlushModeUnconditional();

  const uint32_t work = ctx.r3.u32;
  const uint32_t cam = ctx.r4.u32;
  const uint32_t occl_base = ctx.r5.u32;
  const uint32_t occl_count = ctx.r6.u32;
  const uint32_t counter_addr = ctx.r7.u32;

  const uint32_t aabb_base = REX_LOAD_U32(work + 0);
  const uint32_t out_base = REX_LOAD_U32(work + 8);
  const int32_t count = int32_t(REX_LOAD_U32(work + 12));
  if (count <= 0) {
    return;  // the guest's `ble` straight to the epilogue
  }
  const uint32_t groups = uint32_t((count - 1) >> 2) + 1u;

  // ---- per-call setup ---------------------------------------------------
  F nrm_x[6], nrm_y[6], nrm_z[6], plane_d[6], abs_x[6], abs_y[6], abs_z[6];
  for (int i = 0; i < 6; ++i) {
    const I p = GuestLoad(base, cam + 5152u + uint32_t(i) * 16u);
    nrm_x[i] = Splat<0>(p);
    nrm_y[i] = Splat<1>(p);
    nrm_z[i] = Splat<2>(p);
    plane_d[i] = Splat<3>(p);
    abs_x[i] = Abs(nrm_x[i]);
    abs_y[i] = Abs(nrm_y[i]);
    abs_z[i] = Abs(nrm_z[i]);
  }
  const I view = GuestLoad(base, cam + 5120u);
  const F view_x = Splat<0>(view);
  const F view_y = Splat<1>(view);
  const F view_z = Splat<2>(view);

  const F lod_ref = simde_mm_set1_ps(GuestLoadF(base, cam + 5248u));
  const F lod_bias = simde_mm_set1_ps(GuestLoadF(base, cam + 5252u));
  const F one = simde_mm_set1_ps(1.0f);
  const F half = simde_mm_set1_ps(0.5f);
  const F zero = simde_mm_setzero_ps();
  const I lane_one = simde_mm_set1_epi32(1);

  // Plane 4 is the one the depth term uses; precompute its per-axis sign
  // masks once instead of per group.
  const F nz4_pos = simde_mm_cmpge_ps(nrm_z[4], zero);
  const F ny4_pos = simde_mm_cmpge_ps(nrm_y[4], zero);
  const F nx4_pos = simde_mm_cmpge_ps(nrm_x[4], zero);

  const I axis = GuestLoad(base, cam + 5136u);
  const F axis_abs_x = Abs(Splat<0>(axis));
  const F axis_abs_y = Abs(Splat<1>(axis));
  const F axis_abs_z = Abs(Splat<2>(axis));

  const F lod_floor = simde_mm_set1_ps(GuestLoadF(base, cam + 5256u));
  const F cull_size = simde_mm_set1_ps(GuestLoadF(base, cam + 5264u));
  const F cull_depth = simde_mm_set1_ps(GuestLoadF(base, cam + 5268u));

  // The guest refines 1/f0 once per call and reuses it for every group.
  const F f0 = simde_mm_set1_ps(GuestLoadF(base, cam + 5272u));
  F inv_f0 = Recip(f0);
  inv_f0 = Madd(inv_f0, Sub(one, Mul(inv_f0, f0)), inv_f0);

  const uint32_t aux_base = REX_LOAD_U32(work + 4);

  // Occluder planes do not vary between groups, so load them once. The guest
  // reloaded 30 vectors per occluder per group; this is the single biggest
  // difference between what the guest does and what the work requires.
  const bool use_occ = occl_count != 0 && occl_base != 0;
  static thread_local std::vector<OccPlane> occ;
  if (use_occ) {
    occ.resize(size_t(occl_count) * 5u);
    uint32_t p = occl_base;
    for (size_t i = 0; i < occ.size(); ++i, p += 96u) {
      occ[i].ox = simde_mm_castsi128_ps(GuestLoad(base, p + 0));
      occ[i].oy = simde_mm_castsi128_ps(GuestLoad(base, p + 16));
      occ[i].oz = simde_mm_castsi128_ps(GuestLoad(base, p + 32));
      occ[i].ax = simde_mm_castsi128_ps(GuestLoad(base, p + 48));
      occ[i].ay = simde_mm_castsi128_ps(GuestLoad(base, p + 64));
      occ[i].az = simde_mm_castsi128_ps(GuestLoad(base, p + 80));
    }
  }

  // The visible-removed counter is a running sum over groups. Accumulating it
  // in a register removes a read-modify-write to the same guest address from
  // every iteration (a loop-carried store-to-load dependency); the total is
  // identical because integer adds associate.
  I counter_acc = simde_mm_setzero_si128();

  uint32_t aabb_p = aabb_base;
  uint32_t aux_p = aux_base;
  uint32_t out_p = out_base;

  for (uint32_t g = 0; g < groups; ++g, aabb_p += 128u, aux_p += 64u,
       out_p += 16u) {
    // Four objects, each a min float4 followed by a max float4.
    F min_x, min_y, min_z, min_w, max_x, max_y, max_z, max_w;
    Transpose(GuestLoad(base, aabb_p + 96), GuestLoad(base, aabb_p + 64),
              GuestLoad(base, aabb_p + 32), GuestLoad(base, aabb_p + 0),
              min_x, min_y, min_z, min_w);
    Transpose(GuestLoad(base, aabb_p + 112), GuestLoad(base, aabb_p + 80),
              GuestLoad(base, aabb_p + 48), GuestLoad(base, aabb_p + 16),
              max_x, max_y, max_z, max_w);

    // The aux array is optional; when absent its contribution is zero, which
    // makes every far object fail the screen-size test.
    F aux_x = zero, aux_y = zero, aux_z = zero, aux_w = zero;
    if (aux_base != 0) {
      Transpose(GuestLoad(base, aux_p + 48), GuestLoad(base, aux_p + 32),
                GuestLoad(base, aux_p + 16), GuestLoad(base, aux_p + 0),
                aux_x, aux_y, aux_z, aux_w);
    }

    const F ctr_x = Mul(Add(min_x, max_x), half);
    const F ctr_y = Mul(Add(min_y, max_y), half);
    const F ctr_z = Mul(Add(min_z, max_z), half);
    const F ext_x = Sub(max_x, ctr_x);
    const F ext_y = Sub(max_y, ctr_y);
    const F ext_z = Sub(max_z, ctr_z);

    // ---- frustum: visible where every plane's signed distance clears -----
    F worst = zero;
    for (int i = 0; i < 6; ++i) {
      F dist = Madd(ctr_z, nrm_z[i], plane_d[i]);
      dist = Madd(ctr_y, nrm_y[i], dist);
      dist = Madd(ctr_x, nrm_x[i], dist);
      F radius = Mul(ext_z, abs_z[i]);
      radius = Madd(ext_y, abs_y[i], radius);
      radius = Madd(ext_x, abs_x[i], radius);
      const F t = Add(radius, dist);
      worst = (i == 0) ? t : simde_mm_min_ps(worst, t);
    }
    F visible = simde_mm_cmpge_ps(worst, zero);
    const F after_frustum = visible;

    // ---- occlusion: an occluder hides the box when all 8 corners lie ------
    // behind all 5 of its planes. UNVERIFIED against live data: every
    // recorded call had occl_count == 0.
    if (use_occ && simde_mm_movemask_ps(visible) != 0) {
      for (uint32_t o = 0; o < occl_count; ++o) {
        // `hidden` only ever loses bits: it starts all-ones and is ANDed with
        // each corner test, then used as andnot(hidden, visible). Once it is
        // zero the remaining planes cannot change the outcome, so stopping
        // early is exactly equivalent -- and it is the common case, because
        // most boxes are ruled out by the first occluder plane. This is where
        // the time goes: ~216M (group, occluder) pairs per run, 40 corner
        // tests each if run to completion.
        F hidden = simde_mm_castsi128_ps(simde_mm_set1_epi32(-1));
        for (int plane = 0; plane < 5; ++plane) {
          const OccPlane& pl = occ[size_t(o) * 5u + size_t(plane)];
          // Each corner is one of two values per axis, so the per-axis terms
          // take two values, not eight. Same expressions, same rounding --
          // Mul(Sub(c,o),a) is still formed exactly as before, just once per
          // distinct c instead of once per corner.
          const F zt[2] = {Mul(Sub(max_z, pl.oz), pl.az),
                           Mul(Sub(min_z, pl.oz), pl.az)};
          const F yd[2] = {Sub(max_y, pl.oy), Sub(min_y, pl.oy)};
          const F xd[2] = {Sub(max_x, pl.ox), Sub(min_x, pl.ox)};
          // Fold z and y once for the four (z,y) combinations.
          const F zy[2][2] = {
              {Madd(yd[0], pl.ay, zt[0]), Madd(yd[1], pl.ay, zt[0])},
              {Madd(yd[0], pl.ay, zt[1]), Madd(yd[1], pl.ay, zt[1])}};
          for (int corner = 0; corner < 8; ++corner) {
            const F acc =
                Madd(xd[(corner >> 2) & 1], pl.ax, zy[corner & 1][(corner >> 1) & 1]);
            hidden = simde_mm_and_ps(hidden, simde_mm_cmpge_ps(zero, acc));
            // Check halfway through as well; a movemask+branch is cheap next
            // to four more corners.
            if (corner == 3 && simde_mm_movemask_ps(hidden) == 0) {
              break;
            }
          }
          if (simde_mm_movemask_ps(hidden) == 0) {
            break;  // nothing can be occluded by this occluder
          }
        }
        // andnot with an all-zero `hidden` leaves `visible` untouched, so the
        // common path skips the update entirely.
        if (simde_mm_movemask_ps(hidden) != 0) {
          visible = simde_mm_andnot_ps(hidden, visible);
          if (simde_mm_movemask_ps(visible) == 0) {
            break;  // the guest's early-out once nothing survives
          }
        }
      }
    }

    // ---- distance / screen-size cull -------------------------------------
    // Plane 4's n-vertex: per axis take min or max, whichever projects least
    // along that axis normal. That is the closest point of the box to the
    // plane, so `depth` is the box's nearest view depth.
    const F sel_z = simde_mm_or_ps(
        simde_mm_and_ps(nz4_pos, min_z), simde_mm_andnot_ps(nz4_pos, max_z));
    const F sel_y = simde_mm_or_ps(
        simde_mm_and_ps(ny4_pos, min_y), simde_mm_andnot_ps(ny4_pos, max_y));
    const F sel_x = simde_mm_or_ps(
        simde_mm_and_ps(nx4_pos, min_x), simde_mm_andnot_ps(nx4_pos, max_x));
    F depth = Mul(sel_z, nrm_z[4]);
    depth = Madd(sel_y, nrm_y[4], depth);
    depth = Madd(sel_x, nrm_x[4], depth);
    depth = simde_mm_max_ps(Add(depth, plane_d[4]), one);

    // Apparent size: the aux extent projected on |[cam+5136]|, over depth^2
    // scaled by f0. Both reciprocals get the guest's Newton-Raphson step.
    F size = Mul(aux_z, axis_abs_z);
    size = Madd(aux_y, axis_abs_y, size);
    size = Madd(aux_x, axis_abs_x, size);
    const F d_over_f0 = Mul(depth, inv_f0);
    const F denom = Mul(depth, d_over_f0);
    F inv_denom = Recip(denom);
    inv_denom = Madd(inv_denom, Sub(one, Mul(inv_denom, denom)), inv_denom);
    const F screen = Mul(size, inv_denom);

    // The guest guards this whole stage behind three tests: there must be an
    // aux array, something must still be visible, and the aux W component
    // must be non-zero. Skipping it matters -- cameras with LOD disabled have
    // f0 == 0, so the reciprocal chain would produce NaN and cull everything.
    if (aux_base != 0 && simde_mm_movemask_ps(visible) != 0 &&
        simde_mm_movemask_ps(simde_mm_cmpeq_ps(aux_w, zero)) != 0xF) {
      const F is_far = simde_mm_cmpge_ps(depth, cull_depth);
      const F big_enough = simde_mm_cmpge_ps(screen, cull_size);
      // Far objects must still be big enough on screen; near ones pass.
      const F keep = simde_mm_or_ps(simde_mm_andnot_ps(is_far, visible),
                                    simde_mm_and_ps(is_far, big_enough));
      visible = simde_mm_and_ps(visible, keep);
    }

    // ---- distance / LOD --------------------------------------------------
    const F dx = Sub(ctr_x, view_x);
    const F dy = Sub(ctr_y, view_y);
    const F dz = Sub(ctr_z, view_z);
    F d2 = Mul(dz, dz);
    d2 = Madd(dy, dy, d2);
    d2 = Madd(dx, dx, d2);
    const F dist = Recip(Recip(simde_mm_sqrt_ps(d2)));
    const F w = Add(min_w, lod_bias);
    const F num = Mul(w, lod_ref);
    const F den = Sub(dist, w);
    F r = Recip(den);
    r = Madd(r, Sub(one, Mul(r, den)), r);
    const F scaled = Mul(num, r);
    const F use_scaled = simde_mm_cmpge_ps(den, w);
    const F lod = simde_mm_or_ps(simde_mm_and_ps(use_scaled, scaled),
                                 simde_mm_andnot_ps(use_scaled, lod_ref));

    const F lod_kept = simde_mm_and_ps(simde_mm_cmpge_ps(lod, lod_floor), lod);
    GuestStore(base, out_p,
               simde_mm_castps_si128(simde_mm_and_ps(visible, lod_kept)));

    // Counter: one per lane that frustum culling passed but occlusion took.
    const I removed = simde_mm_and_si128(
        simde_mm_castps_si128(simde_mm_xor_ps(after_frustum, visible)), lane_one);
    counter_acc = simde_mm_add_epi32(counter_acc, removed);
  }

  GuestStore(base, counter_addr,
             simde_mm_add_epi32(GuestLoad(base, counter_addr), counter_acc));

}

