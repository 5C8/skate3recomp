// In-world neon spline shader (waypoint arrows / marker beams). The guest
// B-spline VS is evaluated on the CPU at publish time into WORLD-space
// vertices; the VS here projects them with the scene's (smoothed) view_proj
// rows so the signs stay glued to the world under camera re-timing. The two
// PS variants transcribe the game's spline ucode: "default" = additive
// glow, "darken" = straight-alpha backdrop dimming. The gradient texture uses the wrap/aniso
// sampler like the original i_diffuse (U runs 0..N along the band).
cbuffer C : register(b0) {
  float4 vp0;  // scene view_proj rows (row-vector: clip = x*vp0+y*vp1+z*vp2+w*vp3)
  float4 vp1;
  float4 vp2;
  float4 vp3;
  float4 intensity;  // x/y = i_intensity as staged (default/darken gain).
                     // zw = showcase blackout gate, staged CPU-side over the
                     // guest row's unused lanes: z = split x (output pixels),
                     // w = left|right visibility bits (3 = fully visible).
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float fade : TEXCOORD1;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float fade : TEXCOORD1) {
  VSOut o;
  o.pos = p.x * vp0 + p.y * vp1 + p.z * vp2 + p.w * vp3;
  o.uv = uv;
  o.fade = fade;
  return o;
}
// Exact transcription of the retail Skate 3 spline pixel shaders
// (tools/sk83_port/disasm_retail; EA added in-shader sqrt gamma
// compensation): default: oC0.rgb = sqrt(rgb^2 * i.x * fade / a);
// darken: oC0.rgb = sqrt(rgb^2 * i.y), oC0.a = sqrt(a * i.y * fade).
// (sk83_na_m's dev compile drops the sqrt for plain products.)
// HDR=1: the scene target holds the tone chain's pre-tonemap input (see
// scene.hlsl ToneOut), so the gamma-authored spline colors encode through
// the chain's exact inverse before blending; the host tonemap pass then
// restores them (and the neon glow feeds bloom).
float3 SplineOut(float3 c) {
#ifdef HDR
  float3 tm = c * c * (2.0 / (1.41 * 1.41));
  float3 lo = 1.0 - sqrt(saturate(1.0 - tm));
  float3 hi = 4.0 * tm - 3.0;
  return lerp(lo, hi, step(1.0, tm));  // per-channel select (fxc + dxc)
#else
  return c;
#endif
}
// Showcase blackout gate: the spline pass draws outside the scene shaders,
// so ps_main's blackout stage (scene.hlsl, mask bit 1024) cannot cover it;
// without this the neon lines leak over the black recording bookends.
// Compiled in only for the SHOWCASE=1 variant swapped in during a run; the
// default build folds to visible and the gate dead-strips.
#ifndef SHOWCASE
#define SHOWCASE 0
#endif
bool SplineVisible(float px_x) {
#if SHOWCASE
  int bits = (int)(intensity.w + 0.5);
  return ((px_x < intensity.z ? 1 : 2) & bits) != 0;
#else
  return true;
#endif
}
float4 ps_default(VSOut i) : SV_Target {
  if (!SplineVisible(i.pos.x)) {
    clip(-1.0);
  }
  float4 c = tex.Sample(smp, i.uv);
  float3 sq = c.rgb * c.rgb * (intensity.x * i.fade / max(c.a, 1.0 / 255.0));
  return float4(SplineOut(sqrt(abs(sq))), 1.0);
}
float4 ps_darken(VSOut i) : SV_Target {
  if (!SplineVisible(i.pos.x)) {
    clip(-1.0);
  }
  float4 c = tex.Sample(smp, i.uv);
  float3 rgb = sqrt(abs(c.rgb * c.rgb * intensity.y));
  float a = sqrt(abs(c.a * intensity.y * i.fade));
  return float4(SplineOut(rgb), a);
}

// SimpleDraw debug primitives (Sk8::Render::SimpleDraw, simpledraw_*
// shaders; e.g. the cv_ShowRenderSkeleton bone lines). Vertices are WORLD
// space (the guest VS's world rows c4..c7 applied at publish); this VS
// projects them with the scene's view_proj like the splines. A line vertex
// carries both endpoints and a quad corner (corner.x: 0 = at a, 1 = at b;
// corner.y: side -1 / +1) and is pushed out perpendicular to the projected
// segment by intensity.x pixels (half width), intensity.yz = 2 / viewport
// size; corner.y = 0 marks a plain triangle vertex. The guest PS
// (SimpleDrawPS) outputs the interpolated colour as is.
struct DebugVSOut {
  float4 pos : SV_Position;
  float4 color : COLOR0;
};
DebugVSOut vs_debug(float4 a : POSITION, float4 b : TEXCOORD0, float4 color : TEXCOORD1,
                    float2 corner : TEXCOORD2) {
  DebugVSOut o;
  float4 ca = a.x * vp0 + a.y * vp1 + a.z * vp2 + a.w * vp3;
  float4 cb = b.x * vp0 + b.y * vp1 + b.z * vp2 + b.w * vp3;
  float4 c = corner.x < 0.5 ? ca : cb;
  if (abs(corner.y) > 0.5) {
    float2 sa = ca.xy / max(abs(ca.w), 1e-4) / intensity.yz;
    float2 sb = cb.xy / max(abs(cb.w), 1e-4) / intensity.yz;
    float2 d = sb - sa;
    float len = length(d);
    float2 n = len > 1e-4 ? float2(-d.y, d.x) / len : float2(1.0, 0.0);
    c.xy += n * corner.y * intensity.x * intensity.yz * c.w;
  }
  o.pos = c;
  o.color = color;
  return o;
}
float4 ps_debug(DebugVSOut i) : SV_Target {
  return float4(SplineOut(saturate(i.color.rgb)), saturate(i.color.a));
}
