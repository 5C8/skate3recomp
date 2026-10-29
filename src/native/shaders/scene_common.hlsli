// Shared shading helpers: screen-space / stored tangent frames and the
// output encode (the exact scene tone chain and its HDR pre-tonemap
// passthrough).
// Screen-space UV-gradient tangent frame (cotangent reconstruction from the
// position/uv derivatives; signs/lengths calibrated against the meshes' REAL
// stored TBN; see the env-family branch notes). The RAW screen frame is
// exact for the GetTangentLight kd SIGN terms under ANY UV orientation:
// bowl/ramp walls map their texture rotated relative to world-up, and
// mirrored islands flip dP/dU exactly like the stored tangent does.
void ScreenTangentFrame(float3 wn, float3 rpos, float2 uv,
                        out float3 tt, out float3 bb) {
  float3 dp1 = ddx(rpos), dp2 = ddy(rpos);
  float2 du1 = ddx(uv), du2 = ddy(uv);
  float3 dp2p = cross(dp2, wn), dp1p = cross(wn, dp1);
  tt = dp2p * du1.x + dp1p * du2.x;
  bb = dp2p * du1.y + dp1p * du2.y;
  tt *= -rsqrt(max(dot(tt, tt), 1e-12));
  bb *= rsqrt(max(dot(bb, bb), 1e-12));
}
// kd/normal-mapping axes: prefer the mesh's STORED frame (binormal +
// handedness in tanb, T = cross(B, N) x handedness x calibration polarity)
// - authoring decides per UV island whether the frame follows a mirror,
// which no derivative frame can know (per-island brightness steps on the
// wooden ramp panels / faint bowl striping). The screen frame is the
// fallback for meshes without one.
void KdAxes(float4 tanb, float3 wn, float3 tt_s, float3 bb_s,
            out float3 kt, out float3 kb) {
  kt = tt_s;
  kb = bb_s;
  if (tanb.w > 0.1 && tanb.w < 0.33) {
    // Tangent-stored layouts (sentinel bytes 40/70; decalenvironment_
    // tileable and other usage-6-only env meshes): the game's VS builds
    // B = sign.x * cross(N, T) from the stored T. Without this the kd sign
    // terms fell back to the screen frame, whose sign(T.sun) flips per
    // pixel wherever T is near-perpendicular to the sun's horizontal
    // component: the two-level dither on the plaza slabs (kd 1.045 vs
    // 0.889, measured 1.22 in the capture).
    kt = normalize(tanb.xyz);
    kb = cross(wn, kt) * (tanb.w > 0.22 ? 1.0 : -1.0);
  } else if (tanb.w > 0.1) {
    kb = normalize(tanb.xyz);
    kt = cross(kb, wn) * ((tanb.w > 0.6 ? 1.0 : -1.0) * sh_v2.x);
  }
}
// ---- Output encode --------------------------------------------------------
// Every exact branch ends in the same chain: x = the post-fog,
// post-exposure linear value ("xe"), then
//   tonemap = max(x*0.25 + 0.75, 1) - sat(1 - x)^2
//   out     = sat(sqrt(0.5 * tonemap))
// which is what the game's material shaders write: its 1152x640 scene
// target. The x1.41 scene multiplier belongs to the game's final postfx
// composite (postfx_fast_uberPS / postfx_visualfxPS: scene * 1.41 +
// bloom), which the native renderer runs as its own pass afterwards (see
// ApplyGamePostfx); folding it in here clipped every value the bloom
// threshold reads.
// Under HDR=1 the branch returns xe itself into the float scene
// intermediate and the host post pass (hdr.hlsl ps_tonemap) applies the
// chain exactly once, which is what lets real bloom inject energy
// pre-tonemap. `reduced` = environmentdiffuse's cheap curve 1 - sat(1-x)^2;
// it equals the full curve evaluated at min(x, 1) (the two only differ past
// the x = 1 saturation point), so the HDR encode is a clamp.
float4 ToneOut(float3 xe, float a, bool reduced) {
#ifdef HDR
  float3 x = max(xe, 0.0);
  return float4(reduced ? min(x, 1.0) : x, a);
#else
  float3 t1 = saturate(1.0 - xe);
  float3 tm = reduced ? 1.0 - t1 * t1 : max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
  // sqrt of the ABSOLUTE value, as every one of these shaders ends:
  //   127: max r0.xyz_, r0.xyzz, c255.zzzz    clamp to >= 0
  //   128: mad r0.xyz_, -r1.xyzz, r1.xyzz, r0.xyzz   SUBTRACT r1*r1
  //   130: sqrt oC0.x___, r_abs[0].x          sqrt(|x|)
  // Line 128 comes AFTER the clamp, so the value genuinely goes negative,
  // and the hardware's r_abs modifier folds it back positive. Clamping to
  // zero here instead emitted BLACK along every zero crossing -- thin dark
  // contour lines tracing the ripples on water, where the crossing locus
  // moves with the view.
  return float4(saturate(sqrt(abs(tm * 0.5))), a);
#endif
}
// Values authored in the FINAL gamma space (the legacy empirical branches,
// water, debug visualizations): the scene target sits before the postfx
// composite's x1.41, so they enter it divided by 1.41 and come out of the
// composite unchanged. Under HDR they encode as the tone chain's exact
// inverse, so the host tonemap restores them.
float3 PassGamma(float3 c) {
#ifdef HDR
  float3 tm = c * c * (2.0 / (1.41 * 1.41));
  float3 lo = 1.0 - sqrt(saturate(1.0 - tm));
  float3 hi = 4.0 * tm - 3.0;
  return lerp(lo, hi, step(1.0, tm));  // per-channel select (fxc + dxc)
#else
  return c * (1.0 / 1.41);
#endif
}
