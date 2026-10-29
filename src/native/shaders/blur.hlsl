// Popup background blur: EXACT port of the game's dedicated pass chain
// (captured from a live frame with the Rewards popup up):
//   blur_hBlurPS:  11 gaussian taps along +X over the finished frame,
//                  offsets k * 0.0003125 * PS c0.x (c0.x = 8 -> 0.0025/tap,
//                  +/-1.25% of the buffer), literal weights below (sum 1);
//   blur_vBlurPS:  the same kernel along +Y;
//   postfx_basictex: plain fullscreen REPLACE of the frame with the result.
// The game runs it at its fixed 1152x640 internal resolution and the
// console's bilinear upscale of that buffer is what reads as the "frosted
// glass" lattice, so the passes here render into 1152x640 intermediates and
// stretch back, reproducing both the kernel and the lattice.
cbuffer C : register(b0) {
  float4 dir;   // xy = blur axis, z = kernel scale (the game's PS c0.x, 8)
  float4 tint;  // rgb = the game's PS c1 of the blur pass: the menu fade
                // modulate. Both blur ucodes end in `mul oC0, r0, c1`, so it
                // applies PER PASS (squared overall). Gameplay popups stage
                // (1,1,1), why the original port had no multiply, the
                // pause menu stages ~(0.35,0.33,0.32) = the darkened
                // backdrop (measured in a live pause-menu capture).
};
Texture2D<float4> src : register(t0);
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  // Literal kernel from the blur_hBlurPS ucode (c251..c255).
  const float w[6] = {0.2005654, 0.1769984, 0.1216491, 0.0651141,
                      0.0271436, 0.0088122};
  float2 step = dir.xy * (0.0003125 * dir.z);
  float3 c = src.SampleLevel(smp_clamp, i.uv, 0).rgb * w[0];
  [unroll] for (int k = 1; k < 6; ++k) {
    c += src.SampleLevel(smp_clamp, i.uv + step * k, 0).rgb * w[k];
    c += src.SampleLevel(smp_clamp, i.uv - step * k, 0).rgb * w[k];
  }
  return float4(c * tint.rgb, 1.0);
}
// Prefiltered downsample of the (higher-res) native output into the game's
// 1152x640 blur space (dir.xy = source texel size). A 4x4 grid of bilinear
// taps covers reduction ratios up to ~4x (4K -> 1152 is 3.33x); narrower
// footprints undersampled the scale and the aliased detail crawled as the
// scene animated: the whole blurred backdrop shimmered, worst around
// high-contrast edges. This pass reads the second constant register as the
// source UV rect (xy = scale, zw = offset): the photo grab center-crops the
// 16:9 band out of a wide output; (1,1,0,0) = full frame.
float4 ps_down(VSOut i) : SV_Target {
  float4 rect = tint;
  float2 uv = i.uv * rect.xy + rect.zw;
  float3 c = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = float2(float(x) - 1.5, float(y) - 1.5) * dir.xy;
      c += src.SampleLevel(smp_clamp, uv + off, 0).rgb;
    }
  }
  return float4(c / 16.0, 1.0);
}
// PhotoEffects portrait pass output (native resolve job): the photo scene
// rendered into a 1152:640 region of the scene plane, downsampled into the
// game's 1152x640 target like ps_down (tint = source UV rect) and decoded
// back to the game's raw pixel-shader output: the portrait pass writes its
// draws straight into an 8-bit target with no post, while the scene plane
// holds that output pre-scaled for the fast_uber composite (classic: / 1.41,
// see PassGamma) or tone-encoded (HDR, dir.z = 1: PassGamma's inverse).
float3 PhotoDecode(float3 v) {
  if (dir.z > 0.5) {
    float3 tm = lerp(1.0 - (1.0 - v) * (1.0 - v), (v + 3.0) * 0.25, step(1.0, v));
    return saturate(1.41 * sqrt(max(tm, 0.0) * 0.5));
  }
  return saturate(v * 1.41);
}
float4 ps_photo_out(VSOut i) : SV_Target {
  float4 rect = tint;
  float2 uv = i.uv * rect.xy + rect.zw;
  float3 c = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = float2(float(x) - 1.5, float(y) - 1.5) * dir.xy;
      c += PhotoDecode(src.SampleLevel(smp_clamp, uv + off, 0).rgb);
    }
  }
  return float4(c / 16.0, 1.0);
}
// postfx_basictex: oC0 = tex (verified: `max oC0, r0, r0`).
float4 ps_blit(VSOut i) : SV_Target {
  return float4(src.SampleLevel(smp_clamp, i.uv, 0).rgb, 1.0);
}
// OpenVR's Linear contract uses the exact DXGI sRGB inverse transfer.
// Load preserves pixel positions and the full side-by-side eye boundary.
float4 ps_vr_linear(VSOut i) : SV_Target {
  float3 c = src.Load(int3(uint2(i.pos.xy), 0)).rgb;
  float3 linear_rgb = lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4),
                       step(0.04045, c));
  return float4(linear_rgb, 1.0);
}
// Guest display gamma ramp, the emulated presenter's apply_gamma_table pass
// transcribed from its compiled ps_5_1 (rexglue d3d12 bytecode):
//   ftou r0.xy, v0.xy ; ld r0.xyz, r0, T1        source texel at the pixel
//   mad r0.xyz, r0, 255, 0.5 ; ftou r0.xyz       per-channel 8-bit index
//   ld r0.x, r0.x, T0.zxyw  -> red   = ramp[r].z
//   ld r0.y, r0.y, T0.xyzw  -> green = ramp[g].y
//   ld r0.z, r0.z, T0.yzxw  -> blue  = ramp[b].x ; o0.w = 1
// T0 holds the raw DC_LUT_30_COLOR entries (blue bits 0-9, green 10-19,
// red 20-29) as R10G10B10A2, so .z is the red field and .x the blue field.
// The native path returns before the presenter runs this pass, so the
// native frame applies it itself (see ApplyGuestGammaRamp).
Texture2D<float4> gamma_ramp : register(t1);
// blur_hBlurMaskedPS's mask test as a final-composite discard (the masked
// popup blur, FrameScene::ui_blur_masked): pixels the foreground preview
// object's draws marked (t1 = the R8 foreground mark at the output size and
// the scene's sample count -- the game's stencil byte, see fg_mark) keep the
// sharp, untinted frame already in the target: the ucode's `(p0) oC0 =
// centre tap` with no c1 multiply.
#ifdef MASK_MSAA
// Shares t1 with this file's Texture2D t1 (gamma_ramp), which this entry
// never references (the same arrangement as photo_fx's PFX_MSAA mark).
Texture2DMS<float> fg_mark_ms : register(t1);
#endif
float4 ps_blit_masked(VSOut i) : SV_Target {
#ifdef MASK_MSAA
  // The pixel's mark COVERAGE over all samples: the colour beneath is the
  // MSAA resolve, so an edge pixel whose first sample alone hits the object
  // is mostly the sharp background -- keeping it drew a bright halo around
  // thin pieces (rails). The console has no MSAA: each pixel is either the
  // object or the blur; majority coverage is the closest per-pixel match.
  uint mw, mh, ns;
  fg_mark_ms.GetDimensions(mw, mh, ns);
  float m = 0.0;
  for (uint s = 0; s < ns; ++s) {
    m += fg_mark_ms.Load(int2(i.pos.xy), int(s));
  }
  m /= float(max(ns, 1u));
#else
  const float m = gamma_ramp.Load(int3(int2(i.pos.xy), 0)).r;
#endif
  clip(0.5 - m);
  return float4(src.SampleLevel(smp_clamp, i.uv, 0).rgb, 1.0);
}
float4 ps_gamma(VSOut i) : SV_Target {
  uint3 idx = uint3(src.Load(int3(uint2(i.pos.xy), 0)).rgb * 255.0 + 0.5);
  return float4(gamma_ramp.Load(int3(idx.r, 0, 0)).z,
                gamma_ramp.Load(int3(idx.g, 0, 0)).y,
                gamma_ramp.Load(int3(idx.b, 0, 0)).x, 1.0);
}
// Host settings-menu backdrop: clean separable GAUSSIAN, deliberately NOT the
// game's popup chain above (that one reproduces the console's 1152x640
// lattice on purpose). Runs on a
// half-res intermediate; weights are computed and normalised in-shader so the
// sigma is a free runtime constant.
//   dir.xy = blur axis in UV units (one texel along the axis)
//   dir.z  = sigma in (half-res) pixels
//   dir.w  = tap radius, ceil(3*sigma), capped by the caller
float4 ps_menu_gauss(VSOut i) : SV_Target {
  const float sigma = max(dir.z, 0.001);
  const int radius = int(dir.w);
  float3 acc = src.SampleLevel(smp_clamp, i.uv, 0).rgb;
  float wsum = 1.0;
  for (int k = 1; k <= radius; ++k) {
    float w = exp(-0.5 * float(k) * float(k) / (sigma * sigma));
    acc += src.SampleLevel(smp_clamp, i.uv + dir.xy * k, 0).rgb * w;
    acc += src.SampleLevel(smp_clamp, i.uv - dir.xy * k, 0).rgb * w;
    wsum += 2.0 * w;
  }
  return float4(acc / wsum, 1.0);
}
// ---- Auto-exposure luminance measurement (the game's PostProcessor chain) --
// The game measures its PRE-uber 1152x640 scene each frame and the CPU
// (PostProcessor sub_82978630) averages the result, center-weighted, out of
// a 32x16 8888 target (luminance in ALPHA, the byte it reads):
//   1463 bloom_dof_tap4_minusthresholdPS  1152x640 -> 288x160
//        a = sat(dot(sat(avg(tap^2)), (0.3, 0.4, 0.3)))   (literal c255)
//        four bilinear taps at +-1 source texel (PS c0..c3)
//   1464/1465 bloom_dof_tap4_minusthreshold_alphalumPS
//        288x160 -> 72x40 -> 32x16: sat(0.25 * sum of 4 taps), same offsets
// The native renderer never executes those draws, so the measurement target
// stayed zero and exposure sat on its clamp. These passes rebuild it from
// the native output; the result is written into the guest target before
// the evaluator locks it (see skate3_native_render.cpp).
// ps_lum_first: dir.xy = one OUTPUT pixel in UV, tint = the centered 16:9
// source rect (xy scale, zw offset). Each 288x160 cell integrates its 4x4
// game-texel footprint with a 4x4 grid of bilinear taps (2x2 output pixels
// each at the ~3x native/1152 ratio); it runs on the scene target before
// the postfx composite, i.e. on the game's pre-uber scene values.
float4 ps_lum_first(VSOut i) : SV_Target {
  float4 rect = tint;
  float2 uv = i.uv * rect.xy + rect.zw;
  // Cell size in source UV: (1/288, 1/160) of the band.
  float2 cell = float2(1.0 / 288.0, 1.0 / 160.0) * rect.xy;
  float3 acc = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = (float2(float(x), float(y)) - 1.5) * 0.25 * cell;
      float3 c = src.SampleLevel(smp_clamp, uv + off, 0).rgb;
      acc += c * c;
    }
  }
  float3 lin = saturate(acc / 16.0);
  float lum = saturate(dot(lin, float3(0.3, 0.4, 0.3)));
  return float4(0.0, 0.0, 0.0, lum);
}
// ps_lum_down: dir.xy = one SOURCE texel in UV (the game's PS c0..c3 are
// exactly +-1/srcW, +-1/srcH); sat(0.25 * sum) of four bilinear taps.
float4 ps_lum_down(VSOut i) : SV_Target {
  float a = src.SampleLevel(smp_clamp, i.uv + float2(-dir.x, -dir.y), 0).a +
            src.SampleLevel(smp_clamp, i.uv + float2(dir.x, -dir.y), 0).a +
            src.SampleLevel(smp_clamp, i.uv + float2(-dir.x, dir.y), 0).a +
            src.SampleLevel(smp_clamp, i.uv + float2(dir.x, dir.y), 0).a;
  return float4(0.0, 0.0, 0.0, saturate(0.25 * a));
}
