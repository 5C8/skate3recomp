#include "scene_alpha.hlsli"

// Face-normal shading uses the camera-relative world position: interpolating
// absolute world coordinates (hundreds of meters) destroys ddx/ddy precision
// and produces per-pixel noise.
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 mvp;
  float4 tint;
  float4 cam_pos;
  // Per-material color multiplier (w > 0 enables): CAS hair is a grayscale
  // texture times the character's hair color.
  float4 mat_tint;
  // x = macroOverlayUVScale, y = macroOverlayOpacity, z > 0 = macro overlay
  // texture bound at t3, w > 0 = environment.decal item with its `decal`
  // art bound at t4 (composited in-shader over the diffuse, exactly like
  // the game's decalenvironment PS: lerp by the art's alpha; opaque).
  // Macro and decal art are INDEPENDENT: decal ground/wall sections carry
  // the same macrooverlay as their non-decal neighbors, sharing one slot
  // dropped the macro grime there and rendered alternating plaza sections
  // ~1.4x too bright (the large-scale ground checkerboard).
  float4 overlay;
  // x > 0 = environment.transparent item (alpha-blended sub-pass): shading
  // follows the game's transparentenvironment.fx; the opaque pass's
  // alpha-test turned the soft mist gradients into solid white cloud blobs.
  // yzw = the global distance-fog RAMP (scale/bias/exponent from main-pass
  // VS c5), and mat_tint doubles as the linear-space fog COLOR (rgb +
  // transmittance scale in w, VS c6) for transparent items; the root
  // signature is capped at 64 DWORDs, so fog rides in slots transparent
  // items never use otherwise. Fog is currently applied to transparent
  // items only (the km-distant mist sheets; everything else we render is
  // near enough for fog to be negligible).
  // Fam 14 (scrollincandescent, opaque, so every fog slot is free):
  // xy = the period-wrapped UV scroll offset, z = the material multiplier.
  float4 misc;
};
// Per-frame dynamic-shadow (CSM) receiver constants, captured from the
// game's world-material PIXEL banks.
#include "scene_frame_cb.hlsli"
// Character-family lighting (defaultcharacter.fx and friends): canonical
// per-draw rows captured from the guest PIXEL constant bank at palette
// capture (CaptureCharLighting has the per-family register maps; the math
// below was validated offline by executing the game's own pixel shaders).
// Enabled per draw via cam_pos.w = family (0 = not a character / capture
// failed -> the legacy empirical shading below).
cbuffer CH : register(b2) {
  float4 ch_light;  // xyz = sun direction, w = hair fresnel power
  float4 ch_key;    // rgb = key (sun) color, w = exposure
  float4 ch_amb;    // rgb = flat ambient, w = SH ambient multiplier / hair ambient
  float4 ch_sh[9];  // SH irradiance rows, pre-scaled (see capture);
                    // vehicles keep spec color + power in row 0 instead
  float4 ch_tintA;  // CAC diffuse tint / livingworld red-mask tint (w = apply)
  float4 ch_tintB;  // livingworld blue-mask tint / hair fresnel tint (w = strand-alpha scale)
  float4 ch_misc;   // x = alpha out, y = family, z = lens-alpha flag,
                    // w = rim fresnel power
  float4 ch_ks;     // rgb = key spec color, w = power (0 = spec rows absent)
  float4 ch_rs;     // rgb = rim spec color, w = power
  float4 ch_rim;    // rgb = rim light color, w = key-spec fresnel power
};
Texture2D<float4> diffuse : register(t0);
Texture2D<float4> lightmap : register(t1);
Texture2D<float4> macro : register(t3);
Texture2D<float4> decal_art : register(t4);
// Paired second descriptor of the t4 table: the reflective families'
// (fam 5/6) normal map. Only valid, and only sampled, when overlay.w == 4.
Texture2D<float4> normal_map : register(t5);
Texture2D<float2> shadow_atlas : register(t7);
TextureCube<float4> env_cube : register(t6);
// World-shading v2 material maps (the t8/t9 pair table): the detail normal
// map (t8, sampled at uv * misc.w) and the spec/ecc masks (t9). Sampled
// only when the misc.z bind flags say so (env fams 1-4 and the
// dynamicobject families; see those branches).
Texture2D<float4> detail_map : register(t8);
Texture2D<float4> spec2_map : register(t9);
// Native static sun-shadow map (third entry of the t8/t9 table): a single
// camera-centered ortho depth map of the STATIC world along the material
// sun, rendered natively each frame. Sampled by SampleStaticSun as an
// additional receive term on every lit branch.
Texture2D<float2> static_sun : register(t10);
// The game's deck-shadow decal: a static 64x128 5_6_5 blob, WHITE at the
// border and dark in the centre, so it modulates (postfx_deck_AO's combine).
Texture2D<float4> deck_decal : register(t11);
// decal2environment's second decal art (the game's tf5; fifth entry of the
// t8 table). Only sampled when misc.y >= 256 (layer bound, fam 3).
Texture2D<float4> decal_art2 : register(t12);
// Raw bone palette: 3 float4 rows per bone, column-vector affine [R | t],
// applied with explicit dots (StructuredBuffer<float4x4> default packing is
// column-major and would silently transpose the matrices).
StructuredBuffer<float4> bones : register(t2);
SamplerState smp : register(s0);
// s1 = bilinear CLAMP (shared with the 2D pass). Decal art whose game
// sampler state clamps both axes to the edge samples through it (see
// SampleDecalArt): the art UV runs far outside [0,1] across big ground
// sheets and wrap tiled the graffiti across the whole plaza.
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float3 rpos : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float3 nrm : TEXCOORD3;
  float2 uv3 : TEXCOORD4;
  // Stored tangent frame (world-shading v2): xyz = the mesh's authored
  // binormal (world-rotated; zero when the mesh carries none), w = the
  // tangent handedness sign. Rides the static meshes' otherwise-unused
  // blend-weight bytes (see DecodeMesh).
  float4 tanb : TEXCOORD5;
  // decal2environment's SECOND art layer UV (its extra half2 vertex
  // element, VS line 7 vfetch r2.__yx -> o5.zw -> PS tf5 at r5.wz), carried
  // as two halves in the blend-index word of static decal2 meshes (see
  // DecodeMesh). Meaningless on every other item.
  float2 uv4 : TEXCOORD6;
  // One-pass VR stereo: keeps each eye's copy inside its half of the
  // side-by-side target (1 = unclipped everywhere else).
  float clip : SV_ClipDistance0;
};
VSOut vs_main(float3 p : POSITION, float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1,
              float4 bw : BLENDWEIGHT0, uint4 bi : BLENDINDICES0,
              float3 nrm : NORMAL0, float2 uv3 : TEXCOORD2,
              uint inst : SV_InstanceID) {
  VSOut o;
  float4 mp = float4(p, 1.0);
  float3 n = nrm;
  // tint.g selects the skinning rule; the bone palette (row-vector matrices)
  // maps model space to world space, mvp is then just view*proj.
  //   1.0  = character VS families (defaultcharacter / cac / cacstamp /
  //          livingworld / hair): THREE stored weights and an implied
  //          fourth, straight from every character ucode:
  //            dp3 r0.w, weights.xyz, c254.w        (sum)
  //            add r1.y, -r0.w, c254.y              (w3 = 1 - sum)
  //          weights x/y/z pair with index x/y/z, the implied weight with
  //          index w; no renormalisation. The fourth weight byte is never
  //          read by the game. Reading it (and dividing by the byte sum)
  //          mis-skinned any vertex whose byte disagreed, and a vertex
  //          bound wholly to its fourth bone (x/y/z bytes 0) fell out of
  //          skinning to raw model coordinates: the NPC shirt/hair spikes.
  //   0.5  = other skinned items (vehicles etc.): the explicit normalised
  //          four-weight blend.
  //   0.25 = ropa garments rendered rigid: no skinning (zero weights).
  float wsum = dot(bw, float4(1, 1, 1, 1));
  // Car bodies (family 6) carry their stored binormal in uv3 (see the mesh
  // decode); it rotates with the same bone rows as the normal, like the
  // vehicle VS's tangent pair.
  // cac_hair (family 4) carries its stored usage-6 TANGENT the same way
  // (the Kajiya-Kay strand axis, see the mesh decode and ShadeCharacter).
  const bool veh_b = ((cam_pos.w > 5.5 && cam_pos.w < 6.5) ||
                      (cam_pos.w > 3.5 && cam_pos.w < 4.5)) &&
                     uv3.x > 14.0;
  // Near livingworld pedestrians (family 3) carry their stored usage-6
  // TANGENT raw: x/y in uv3, z in uv2.x, uv2.y = 16 as the presence marker
  // (raw values interpolate exactly like the stamp VS's o2; see the mesh
  // decode). Skinned with the normal's bone rows, like the VS.
  const bool lw_t = cam_pos.w > 2.5 && cam_pos.w < 3.5 && uv2.y > 15.0;
  float3 vb = float3(0.0, 0.0, 0.0);
  if (lw_t) {
    vb = float3(uv3.x, uv3.y, uv2.x);
  } else if (veh_b) {
    const bool neg_z = uv3.x > 24.0;
    vb.x = uv3.x - (neg_z ? 32.0 : 16.0);
    vb.y = uv3.y;
    vb.z = sqrt(saturate(1.0 - vb.x * vb.x - vb.y * vb.y)) * (neg_z ? -1.0 : 1.0);
  }
  const bool skin_implied = tint.g > 0.75;
  const bool skin_explicit = tint.g > 0.375 && !skin_implied && wsum > 0.001;
  if (skin_implied || skin_explicit) {
    float4 w = skin_implied ? float4(bw.xyz, 1.0 - (bw.x + bw.y + bw.z)) : bw / wsum;
    float3 skinned = float3(0, 0, 0);
    float3 sn = float3(0, 0, 0);
    float3 sbv = float3(0, 0, 0);
    // Guest blend indices are plain bone numbers (verified live: byte
    // streams like 02 00 03 01); bone k = palette rows 3k..3k+2.
    [unroll] for (int k = 0; k < 4; ++k) {
      uint row = bi[k] * 3u;
      skinned += w[k] * float3(dot(mp, bones[row]), dot(mp, bones[row + 1]),
                               dot(mp, bones[row + 2]));
      sn += w[k] * float3(dot(n, bones[row].xyz), dot(n, bones[row + 1].xyz),
                          dot(n, bones[row + 2].xyz));
      sbv += w[k] * float3(dot(vb, bones[row].xyz), dot(vb, bones[row + 1].xyz),
                           dot(vb, bones[row + 2].xyz));
    }
    mp = float4(skinned, 1.0);
    n = sn;
    vb = sbv;
  } else {
    n = mul(n, (float3x3)world);
    vb = mul(vb, (float3x3)world);
  }
  o.pos = mul(mp, mvp);
  o.rpos = mul(mp, world).xyz - cam_pos.xyz;
  o.uv = uv;
  o.uv2 = uv2;
  o.nrm = n;
  o.uv3 = uv3;
  if (cam_pos.w < -16.5 && cam_pos.w > -17.5) {
    // trafficlight_*_defaultVS 11-24: lamp = floor(z * 4) (decoded into
    // blend-index byte 0), state = c7[lamp] (mat_tint; other lamps 0);
    //   21: sgt   r0.x, state, 1
    //   24: cndeq o0.xy, r0.x, e3.xy, e2.xy   (cndeq: r0.x == 0 ? a : b)
    // so state > 1 takes the 2-component element's uv (slot uv3), else
    // the 3-component element's xy (slot uv).
    const uint lamp = bi.x;
    const float state = lamp == 0u   ? mat_tint.x
                        : lamp == 1u ? mat_tint.y
                        : lamp == 2u ? mat_tint.z
                        : lamp == 3u ? mat_tint.w
                                     : 0.0;
    o.uv = state > 1.0 ? uv3 : uv;
  }
  // Stored binormal + handedness from the blend-weight bytes (statics
  // only; skinned items use those bytes for actual weights). w passes
  // RAW: ~0 = no stored frame, ~0.39 = negative handedness, ~0.78 =
  // positive (the DecodeMesh sentinel bytes 0/100/200).
  float3 sb = bw.xyz * 2.0 - 1.0;
  o.tanb = (veh_b || lw_t) ? float4(vb, 1.0)
                 : (tint.g > 0.0 ? float4(0.0, 0.0, 0.0, 0.0)
                                 : float4(mul(sb, (float3x3)world), bw.w));
  // decal2 layer-2 UV: two IEEE halves packed little-endian in the
  // blend-index bytes (x|y<<8 = u, z|w<<8 = v); see VSOut::uv4.
  o.uv4 = float2(f16tof32(bi.x | (bi.y << 8)), f16tof32(bi.z | (bi.w << 8)));
  // One-pass VR stereo (world._44 = 2, never read otherwise: positions use
  // world's xyz columns, normals its 3x3): the draw is instanced twice into
  // a side-by-side target, instance 0 = left eye, 1 = right. mvp is the
  // left eye's; st_d carries left clip -> right clip. Each eye's clip x is
  // squashed into its half and the clip distance cuts at the eye's inner
  // edge (the outer edge is the viewport's).
  o.clip = 1.0;
  if (world._44 > 1.5) {
    float4 c = inst == 1u ? mul(o.pos, st_d) : o.pos;
    o.clip = inst == 1u ? c.w + c.x : c.w - c.x;
    c.x = c.x * 0.5 + (inst == 1u ? 0.5 : -0.5) * c.w;
    o.pos = c;
  }
  return o;
}
// CAS skin unwrap (vs_cac_face_unwrapVS == vs_cacstamp_skin_unwrapVS lines
// 11-13): the piece's world-space attributes as vs_main computes them, but
// rasterized at its TEXCOORD0: oPos = (2u - 1, 1 - 2v, z, 1).
VSOut vs_skin_unwrap(float3 p : POSITION, float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1,
                     float4 bw : BLENDWEIGHT0, uint4 bi : BLENDINDICES0,
                     float3 nrm : NORMAL0, float2 uv3 : TEXCOORD2) {
  VSOut o = vs_main(p, uv, uv2, bw, bi, nrm, uv3, 0u);
  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
  o.clip = 1.0;
  return o;
}
// ---- Graphics build-up showcase -------------------------------------------
// sh_v2.yzw carry the showcase split state: y/z = the LAYER MASK shown
// left/right of the vertical split at w (in output pixels), encoded as
// 256 + mask so an all-zero row (showcase off) is unambiguous. Mask bits
// (the contract shared with hdr.hlsl, ssr.hlsl and the CPU sequencer,
// see kShowcaseLayers in skate3_native_scene.h):
//    1 albedo textures    2 baked lighting     4 materials & surface detail
//    8 dynamic shadows   16 ambient occlusion 32 reflections (SSR)
//   64 volumetrics      128 bloom
//  256 decals & grime   512 dynamic entities
// 1024 blackout: the run's opening/closing bookends (ps_main renders every
// draw black; doubles as the recording cut marker), not an orderable layer.
// The material bits are progressive looks (materials subsumes lighting
// subsumes albedo); the rest are independent, so the sequencer can reveal
// them in any order or grouping. 256/512 are SUBTRACTIVE layers: their
// content belongs to the normal frame and stays hidden until the bit
// reveals (the sequencer folds their bits into every step when the layer
// is not part of the run). Returns -1 when the showcase is off.
// The showcase code compiles in only for the SHOWCASE=1 shader variants,
// which the pipeline family swaps in for the duration of a run. The default
// build folds the mask to the off value, so every showcase branch
// dead-strips and the program matches the pre-showcase compile exactly.
#ifndef SHOWCASE
#define SHOWCASE 0
#endif
int ShowcaseMask(float px_x) {
#if SHOWCASE
  float v = px_x < sh_v2.w ? sh_v2.y : sh_v2.z;
  return v < 255.5 ? -1 : (int)(v + 0.5) - 256;
#else
  return -1;
#endif
}
// True when the given layer bit is revealed on this pixel's side of the
// split (or the showcase is off entirely).
bool ShowcaseLayerOn(float px_x, int bit) {
  int m = ShowcaseMask(px_x);
  return m < 0 || (m & bit) != 0;
}
#include "scene_shadows.hlsli"
#include "scene_common.hlsli"
#include "scene_char.hlsli"
#include "scene_dynobj.hlsli"
#include "scene_park.hlsli"
#include "scene_water.hlsli"
// Decal art fetch with the GAME's per-draw addressing. Every decal family
// (decalenvironment[_simple][_tileable], decal2environment) fetches its art
// at tf4, and the plain and _tileable variants are the SAME microcode (the
// dumped VS/PS differ only in their trailing hash): whether the art tiles
// is purely the sampler state renderengine binds to tf4.
// Xbox2SetGpuTextureFetchRegister (82C727F8) writes SamplerState bytes
// 8/9/15 into fetch dword 0 clamp_x (bits 10-12) / clamp_y (13-15) and
// dword 5 border_color (bits 0-1); the CPU records those bits off the
// item's main-pass draw and packs them into overlay.w (see the decal_addr
// comment in skate3_native_scene_gpu):
//   overlay.w = 1 + (code + 256 * fade) / 65536,
//   code = cx | cy << 3 | border << 6, fade = macro fade floor * 255
// Xenos ClampMode: 0 repeat, 1 mirrored repeat, 2 clamp-to-edge,
// 3 mirror-once-to-edge, 4 clamp-halfway, 5 mirror-once-halfway,
// 6 clamp-to-border, 7 mirror-once-to-border. Border colors (Xenia's
// d3d12_texture_cache values): 0 = (0,0,0,0), 1 = (1,1,1,1),
// 2 = ACBYCR black (0.5,0,0.5,0), 3 = ACBCRY black (0,0.5,0.5,0). Every
// black variant has ALPHA 0: a clamp-to-border decal is clear outside its
// placement (an opaque black here painted whole decal surfaces black).
uint DecalArtWord() {
  return uint(clamp(round((overlay.w - 1.0) * 65536.0), 0.0, 65535.0));
}
uint DecalArtCode() {
  return DecalArtWord() & 255u;
}
// decal[2]environment VS 17 / PS 23/27/60: the macro overlay fades out under
// the art, ov' = lerp(1, ov, max(1 - art.a, c10.x)); this is that VS c10.x.
float DecalMacroFadeFloor() {
  return float(DecalArtWord() >> 8) / 255.0;
}
// One axis: the remapped coordinate, and the fraction of the bilinear
// footprint that lies inside the texture (1 except near a BORDER edge).
// h = half a texel of the FINEST level: clamping a non-repeating axis any
// further in crops the art (an isotropic coarse-LOD half texel clamped a
// distant decal to its middle part -- the "zoomed-in" arrow), and the
// anisotropic hardware filter samples finer levels than such an estimate.
float DecalAddrAxis(float t, uint mode, float dim0, out float cover) {
  cover = 1.0;
  if (mode == 0u) {
    return t;
  }
  const float h = min(0.5 / dim0, 0.5);
  if (mode == 1u) {
    // Mirrored repeat: the hardware filters across each seam onto the same
    // edge texel, i.e. a clamp at the seam.
    float m = frac(t * 0.5) * 2.0;
    return clamp(m > 1.0 ? 2.0 - m : m, h, 1.0 - h);
  }
  if ((mode & 1u) != 0u) {
    t = abs(t);  // mirror once
  }
  if (mode >= 6u) {
    // Clamp to border: the edge texel fades to the border color across the
    // last half texel (bilinear between edge and border).
    cover = saturate(min(t, 1.0 - t) * dim0 + 0.5);
  }
  return clamp(t, h, 1.0 - h);
}
float4 SampleDecalTex(Texture2D<float4> tex, float2 uv, uint code) {
  const uint mu = code & 7u;
  const uint mv = (code >> 3) & 7u;
  const uint border = (code >> 6) & 3u;
  float w, hgt;
  tex.GetDimensions(w, hgt);
  float cu, cv;
  const float2 st = float2(DecalAddrAxis(uv.x, mu, w, cu),
                           DecalAddrAxis(uv.y, mv, hgt, cv));
  const float4 bc = border == 1u   ? float4(1.0, 1.0, 1.0, 1.0)
                    : border == 2u ? float4(0.5, 0.0, 0.5, 0.0)
                    : border == 3u ? float4(0.0, 0.5, 0.5, 0.0)
                                   : float4(0.0, 0.0, 0.0, 0.0);
  if (mu >= 2u && mv >= 2u) {
    // Neither axis repeats (clamp to edge / halfway / border, mirror once):
    // the hardware clamp sampler, which clamps every level at its own edge
    // texel and so never crops. Mirror-once folds the coordinate first;
    // border modes fade by the coverage term.
    const float2 cuv = float2((mu & 1u) != 0u ? abs(uv.x) : uv.x,
                              (mv & 1u) != 0u ? abs(uv.y) : uv.y);
    return lerp(bc, tex.Sample(smp_clamp, cuv), cu * cv);
  }
  // A repeating axis (possibly mixed with a clamped one): remap per axis
  // and fetch through the wrap sampler with the ORIGINAL gradients (the
  // remap is piecewise constant or a reflection; its own derivatives would
  // drop the clamped regions to mip 0 and break the mip choice at mirror
  // seams).
  const float4 texel = tex.SampleGrad(smp, st, ddx(uv), ddy(uv));
  return lerp(bc, texel, cu * cv);
}
float4 SampleDecalArt(float2 uv) {
  return SampleDecalTex(decal_art, uv, DecalArtCode());
}
// decal2environment layer 2 (tf5): misc.y = 256 + its addressing code
// (same encoding as DecalArtCode) when bound, 0 otherwise.
bool DecalLayer2Bound() {
  return misc.y > 255.5;
}
float4 SampleDecalArt2(float2 uv) {
  return SampleDecalTex(decal_art2, uv, uint(misc.y + 0.5) & 255u);
}
// The full material shading for every family (see ps_main below; the
// showcase wrapper substitutes the early build-up stages' looks over this
// result's rgb while keeping its alpha).
// TEMP debug (SHADERCOLOR): solid color per shading path (legend in chat).
float3 ShaderDebugColor(VSOut i) {
  float w = cam_pos.w;
  if (w > 0.5) {
    if (w < 1.5) return float3(1.0, 0.0, 0.0);                       // 1 defaultcharacter: red
    if (w < 2.5) return float3(1.0, 0.5, 0.0);                       // 2 CAC/cacstamp: orange
    if (w < 3.5) return ch_misc.z > 1.5 ? float3(0.5, 1.0, 0.0)      // 3 far ped: lime
                                        : float3(1.0, 1.0, 0.0);     // 3 near ped: yellow
    if (w < 4.5) return float3(1.0, 0.6, 0.8);                       // 4 CAC hair: pink
    if (w < 5.5) return float3(1.0, 0.0, 1.0);                       // 5 NPC hair: magenta
    if (w < 6.5) return float3(0.0, 0.0, 1.0);                       // 6 vehicle body: blue
    return float3(0.0, 1.0, 1.0);                                    // 7 vehicle glass: cyan
  }
  if (w < -0.5) {
    float f = -w;
    if (f > 39.5) return float3(0.0, 0.0, 0.0);                      // 40 sky: black
    if (f > 29.5) return float3(0.0, 0.8, 1.0);                      // 30-33 water: sky blue
    if (f > 20.5) return f < 21.5 ? float3(1.0, 0.4, 0.4)            // 21 dynobj: salmon
                                  : float3(0.6, 0.0, 0.0);           // 22 dynobj alphatest: dark red
    const float3 kEnv[16] = {
        float3(0.0, 0.6, 0.0),  // 1 baseenvironment: dark green
        float3(0.6, 0.6, 0.6),  // 2 defaultenvironment: gray
        float3(0.6, 0.3, 0.0),  // 3 decal: brown
        float3(0.8, 0.5, 0.2),  // 4 decal tileable: tan
        float3(0.3, 0.3, 1.0),  // 5 reflective: light blue
        float3(0.0, 0.0, 0.5),  // 6 reflective simple: navy
        float3(0.0, 0.5, 0.5),  // 7 alphatest: teal
        float3(0.5, 0.0, 0.5),  // 8 environment diffuse: purple
        float3(0.2, 1.0, 0.2),  // 9 tree: bright green
        float3(0.6, 1.0, 0.6),  // 10 tree animate: mint
        float3(0.4, 0.2, 0.1),  // 11 proxyworld: dark brown
        float3(1.0, 1.0, 1.0),  // 12 incandescent: white
        float3(0.7, 0.9, 1.0),  // 13 reflective trans: pale blue
        float3(1.0, 0.9, 0.6),  // 14 scroll incandescent: cream
        float3(0.5, 0.5, 0.0),  // 15 advertisement: olive
        float3(0.8, 0.0, 0.3)}; // 16 transparentenvironment: crimson
    return kEnv[clamp(int(f + 0.5) - 1, 0, 15)];
  }
  // 0 = legacy fallback shading: black/white diagonal stripes.
  return frac((i.pos.x + i.pos.y) / 16.0) < 0.5 ? float3(0.0, 0.0, 0.0)
                                                 : float3(1.0, 1.0, 1.0);
}
float4 ShadePixel(VSOut i) {
  if (tint.a > 0.0) {
    return float4(PassGamma(tint.rgb), tint.a);
  }
  float4 albedo = diffuse.Sample(smp, i.uv);
  // Particles: lionREC_RenderPS, oC0 = tf0(uv) * vertex color (mul oC0,
  // r1, r0), no lighting / fog / tone curve. The color rides tanb (unorm
  // bytes mapped *2-1 by the VS, identity world).
  if (cam_pos.w < -49.5 && cam_pos.w > -50.5) {
    const float4 vc = float4(i.tanb.xyz * 0.5 + 0.5, i.tanb.w);
    const float4 pc = albedo * vc;
    return float4(PassGamma(pc.rgb), pc.a);
  }
  // Alpha-tested foliage/fences; opaque formats sample alpha = 1. Character
  // diffuse packs GLOSS in alpha; never clip characters. tint.g > 0 marks
  // them: set for bones-bound skinned items AND for ropa cloth garments
  // rendered rigid (sim-active player tees, clipping their gloss alpha
  // discarded every pixel: the invisible-shirt bug; their decode writes
  // zero blend weights, so the VS skinning branch stays off).
  // tint.b < 0: the game's own draw has alpha test OFF (RB_COLORCONTROL,
  // captured per island), so nothing is discarded here.
  // Explicit cutout families (alphatest / tree / animated.tree) still test
  // their diffuse alpha when overlay.w marks a bound specular/normal map.
  if (SceneDiffuseAlphaTestEnabled(tint.g, tint.b, overlay.w, cam_pos.w)) {
    // environment.transparent alpha-tests its SQUARED alpha at ref 16/255
    // (transparentenvironment.xml: ALPHAREF 16, PS outputs a = diffuse.a^2).
    // Exact env families (cam_pos.w < 0) use the game's world ALPHAREF 30.
    // dynamicobject items (cam_pos.w <= -21) are excluded here and clip in
    // their own branch (only the .alphatest variant tests, at ALPHAREF 30).
    // Fam 13 (reflective_trans glass, cam_pos.w = -13) never alpha-tests;
    // it alpha-BLENDS in the sorted sub-pass. Fam 14 (scrollincandescent)
    // never alpha-tests either (its technique sets no test state, and its
    // PS ignores the diffuse alpha entirely). Fam 15 (advertisement_default
    // / _skateparkPS) writes oC0.w = 0 whatever the art's alpha (ucode
    // 38 / 62: max oC0._000), so the game's alpha test, enabled on e.g. the
    // direction-sign arrow panels, compares a constant and never cuts the
    // art's alpha mask out: clipping by the texture alpha dropped the panel
    // around the arrow (and grew the arrow at distance as the filtered
    // mask spread).
    if (cam_pos.w > -12.5 || cam_pos.w < -15.5) {
      float aref = cam_pos.w < -0.5 ? 0.1176 : 0.35;
      clip(misc.x > 0.0 ? albedo.a * albedo.a - 0.0627 : albedo.a - aref);
    }
  }
  // Exact sky dome (cam_pos.w = -40; sky_defaultPS transcribed from the
  // Skate 3 ucode in a live capture). The emulated frame's big sun
  // glow is computed IN the dome shader: a 1D radial gradient (the sky
  // material's `specular` channel, bound at t4 here) indexed by the sine of
  // the angle between the dome direction and the sun, its rgb^2 amplified
  // by 1/sat(a + 0.01), the alpha falloff makes the core HDR-bright, then
  // added to the squared panorama and run through the standard exposure/
  // tonemap/sqrt chain (no fog on the sky) and the postfx uber 1.41.
  // mat_tint.xyz = sun dir, mat_tint.w = the level sky elevation;
  // overlay.x = sun angular scale, overlay.y = pre-tone multiplier,
  // misc.y = scene exposure.
  if (cam_pos.w < -39.5 && cam_pos.w > -40.5) {
    // The dome mesh is camera-relative: world = mesh + (cam.x, sky_h,
    // cam.z), so the shader's dome-local direction (sky.fx In.vPos) is
    // rpos + (0, cam.y - sky_h, 0).
    float3 dome = normalize(i.rpos + float3(0.0, cam_pos.y - mat_tint.w, 0.0));
    float3 lin = albedo.rgb * albedo.rgb;
    float dotPL = saturate(dot(dome, mat_tint.xyz));
    float sinA = sqrt(saturate(1.0 - dotPL * dotPL));
    float4 sun = decal_art.Sample(smp_clamp, float2(sinA / overlay.x, 0.5 / 16.0));
    lin += sun.rgb * sun.rgb / saturate(sun.a + 0.01);
    float3 xe = lin * overlay.y * misc.y;
    return ToneOut(xe, 1.0, false);
  }
  // dynamicobject.fx props: scene_dynobj.hlsli. Variants -21/-22 only;
  // the exact water families (-30..) and the sky (-40) pass through.
  // environmentpark.* (-24/-25/-26) shares the marker range but has its own
  // rows and its own two shading models; see scene_park.hlsli.
  if (cam_pos.w < -23.5 && cam_pos.w > -26.5) {
    return ShadePark(i, albedo);
  }
  if (cam_pos.w < -20.5 && cam_pos.w > -23.5) {
    return ShadeDynObject(i, albedo);
  }
  // Character families (defaultcharacter / CAC / livingworld / hair /
  // vehicles): scene_char.hlsli.
  if (cam_pos.w > 0.5) {
    return ShadeCharacter(i, albedo);
  }
  // Exact world-material families (cam_pos.w = -family): hand-ported from
  // the game's own pixel shaders and verified per-pixel against them with
  // an offline ucode interpreter. All texture
  // colors linearize IN-SHADER as x^2 (the fetch signs are unsigned on every
  // world texture). Families: 1 baseenvironment, 2 defaultenvironment,
  // 3/4 decalenvironment(_tileable), 5/6 reflective(_simple), 7 alphatest,
  // 8 environmentdiffuse, 9/10 tree(animate), 11 proxyworld,
  // 12 incandescent. v1 runs with NEUTRAL normal/detail maps (kd is the
  // exact flat-map constant 0.39 * 2.39562); spec/reflection masks bind at
  // t4 (overlay.w == 3) on families without decal art.
  if (cam_pos.w < -0.5) {
    float fam = -cam_pos.w;
    float3 dlin = albedo.rgb * albedo.rgb;
#if DECAL_PROBE == 5
    // TEMP probe 5: solid colour per env family, so it is obvious which
    // shader branch owns a given surface (the curb kept being attributed to
    // the wrong one). Legend:
    //   1 environment.default ........ RED
    //   2 environmentsimple.default .. ORANGE
    //   3 environment.decal .......... YELLOW
    //   4 environment.decal_tileable . GREEN
    //   5 environment.reflective ..... CYAN
    //   6 reflective_simple .......... BLUE
    //   7 environmentsimple.alphatest. MAGENTA
    //   8 environmentsimple.diffuse .. PURPLE
    //   9 tree.default ............... DARK GREEN
    //  10 animated.tree ............... LIME
    //  11 proxyworld .................. BROWN
    //  12 incandescent ................ WHITE
    //  13 reflective_trans ............ PINK
    //  14 backlituvscroll ............. GREY
    //  15 advertisement ............... OLIVE
    //  16 environment.transparent ..... TEAL
    //  17+ ............................ DARK GREY
    // A _skatepark permutation shows as the family + 0.25, so the test
    // windows below are +-0.5 like every other family test in this file.
    {
      float3 fc = float3(0.0, 0.0, 0.0);
      if (fam < 1.5)       fc = float3(1.0, 0.0, 0.0);
      else if (fam < 2.5)  fc = float3(1.0, 0.5, 0.0);
      else if (fam < 3.5)  fc = float3(1.0, 1.0, 0.0);
      else if (fam < 4.5)  fc = float3(0.0, 1.0, 0.0);
      else if (fam < 5.5)  fc = float3(0.0, 1.0, 1.0);
      else if (fam < 6.5)  fc = float3(0.0, 0.0, 1.0);
      else if (fam < 7.5)  fc = float3(1.0, 0.0, 1.0);
      else if (fam < 8.5)  fc = float3(0.5, 0.0, 1.0);
      else if (fam < 9.5)  fc = float3(0.0, 0.4, 0.0);
      else if (fam < 10.5) fc = float3(0.6, 1.0, 0.0);
      else if (fam < 11.5) fc = float3(0.5, 0.3, 0.1);
      else if (fam < 12.5) fc = float3(1.0, 1.0, 1.0);
      else if (fam < 13.5) fc = float3(1.0, 0.4, 0.7);
      else if (fam < 14.5) fc = float3(0.5, 0.5, 0.5);
      else if (fam < 15.5) fc = float3(0.5, 0.5, 0.0);
      else if (fam < 16.5) fc = float3(0.0, 0.5, 0.5);
      else                 fc = float3(0.2, 0.2, 0.2);
      return float4(fc, 1.0);
    }
#endif
    // Global distance fog (VS c5/c6, captured per frame): every world PS
    // ends with col * fog.a + fog.rgb before exposure/tonemap.
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 fog_rgb = sh_fogc.rgb * f1;
    float fog_a = 1.0 + sh_fogc.a * f1;
    float expo = sh_sun.w;
    float3 lin;
    float out_a = 1.0;
    bool reduced_tone = false;
    // Water families (30 flowingwateralpha / 31 ocean /
    // 32 oceanreflection / 33 water.default): ports in scene_water.hlsli.
    if (fam > 29.5 && fam < 30.5) {
      return ShadeFlowingWater(i, dlin, fog_a, fog_rgb, expo);
    }
    if (fam > 30.5 && fam < 31.5) {
      return ShadeOcean(i, fog_a, fog_rgb, expo);
    }
    if (fam > 31.5 && fam < 32.5) {
      return ShadeOceanReflection(i, dlin, fog_a, fog_rgb, expo);
    }
    // water.default (33): this build's canal/pool water binds
    // water_defaultPS, not flowingwateralpha.
    if (fam > 32.5 && fam < 33.5) {
      return ShadeWaterDefault(i, dlin, fog_a, fog_rgb, expo);
    }
    if (fam > 15.5 && fam < 16.5) {
      // transparentenvironment_defaultPS, transcribed from its ucode:
      //   9-14: diffuse rgb squared, alpha raw     55: out alpha = a^2
      //   23-26/34-35/38-39: lightmap = (4-tap avg)^2
      //   27/30-33: shadow = sat(infront + 1 - coverage); 35/37: max(s, 0.2)
      //   40: lml = min(lm, max(s, 0.2))            41: body = D^2 * lml
      //   29: power = ecc * c10.w + c11.w          32/36: R = L - 2N(N.L)
      //   37: sat(E . -R) ^ power (38/40/41)       28/42: x c11.rgb * 3
      //   43: x lml.g                              44: x spec mask
      //   45: body * a + spec                      46: fog, x c13.y
      //   47-52: full tone curve (c9.x exposure)
      // No kd term. ALPHAREF 16 on a^2 (transparentenvironment.xml).
      clip(albedo.a * albedo.a - 0.0627);
      float s_t = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, i.nrm, i.pos.xy);
      float3 lm_t = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
      // Retail (35-38): min(lm^2, s + (0.05, 0.09, 0.13)), the tinted
      // shadow floor; sk83_na_m's dev compile clamps max(s, 0.2) instead.
      float3 lml_t = min(lm_t * lm_t, s_t + float3(0.05, 0.09, 0.13));
      if (ParkWorldReceiver(fam)) {
        // transparentenvironment_skateparkPS 52/64-66: the park floor-lerps
        // replace the max(s, 0.2) clamp (see ParkWorldShade).
        lml_t = min(lm_t * lm_t, ParkWorldShade(i.rpos + cam_pos.xyz, i.pos.xy));
      }
      float3 body_t = dlin * lml_t * albedo.a;
      float3 wn_t = dot(i.nrm, i.nrm) > 0.01
                        ? normalize(i.nrm)
                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
      float3 E_t = -normalize(i.rpos);
      float3 L_t = mat_tint.xyz;
      float3 R_t = L_t - 2.0 * wn_t * dot(wn_t, L_t);
      float2 m_t = overlay.w > 2.5 ? decal_art.Sample(smp, i.uv).xy : float2(0.0, 0.0);
      float pw_t = m_t.y * misc.y + misc.z;
      float ks_t = pow(max(saturate(dot(E_t, -R_t)), 1e-6), max(pw_t, 1e-3));
      float3 spec_t = overlay.xyz * 3.0 * ks_t * lml_t.g * m_t.x;
      float3 xe_t = ((body_t + spec_t) * (fog_a * mat_tint.w) + fog_rgb) * misc.x;
      // oC0.w = a^2 (line 55). Alpha-tested opaque draws (misc.w = 0) keep
      // alpha 1; draws the game blends (misc.w = 1) write the real a^2.
      return ToneOut(xe_t, misc.w > 0.5 ? albedo.a * albedo.a : 1.0, false);
    }
    if (fam > 14.5 && fam < 15.5) {
      // advertisement_defaultPS, transcribed from its ucode:
      //   19-22: 4 half-texel lightmap taps, 29: x 0.25, 30: squared
      //   23-28: shadow = sat(infront + 1 - coverage); 29-30: max(shadow, 0.2)
      //   31: min(lm^2, max(shadow, 0.2))      32: max(.., c6.z)
      //   7-10: diffuse squared                33: x diffuse
      //   34: x c10.y                          35-37: sat(1 - x*c9.x) curve
      // No fog, no kd, no spec. The shared env chain (macro, kd, fog) on top
      // of the legacy fallback read the billboards far too bright.
      float s_ad = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, i.nrm, i.pos.xy);
      float3 lm_ad = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
      // Retail (24-32): max(min(lm^2, s + (0.05, 0.09, 0.13)), c6.z); the
      // dev compile clamps max(s, 0.2) instead.
      float3 lml_ad = max(min(lm_ad * lm_ad, s_ad + float3(0.05, 0.09, 0.13)), overlay.x);
      if (ParkWorldReceiver(fam)) {
        // advertisement_skateparkPS 46/53-56: min(lm^2, park floor-lerps),
        // then the same lightmap floor.
        lml_ad = max(min(lm_ad * lm_ad, ParkWorldShade(i.rpos + cam_pos.xyz, i.pos.xy)),
                     overlay.x);
      }
      // Retail 33-36: x c10.y, then the VS fog (mad r0, r0, fog.a, fog.rgb)
      // before the exposure curve; the dev compile is unfogged.
      float3 xe_ad = (dlin * lml_ad * overlay.y * fog_a + fog_rgb) * overlay.z;
      return ToneOut(xe_ad, 1.0, true);
    }
    if (fam > 16.5 && fam < 17.5) {
      // trafficlight_one/two_defaultPS (identical ucode): the lamp cell the
      // VS selected (lit or unlit, see vs_main), NOT linearized: D * c3.y
      // (overlay.x) -> the shared fog/exposure/tone tail, oC0.w = D.a. No
      // lightmap, kd, shadow or material multiplier on the fog term.
      lin = albedo.rgb * overlay.x;
      out_a = albedo.a;
    } else if (fam > 8.5 && fam < 12.5) {
      // tree/treeanimate: D^2 * max(lm^2, floor) * scale [* tint mult];
      // proxyworld/incandescent: D^2 * scale. No shadow receive, no kd, no
      // material multiplier on the fog term.
      if (fam < 10.5) {
        // Console lightmap semantics: bilinear, clamped, mip 0 (see the
        // main env fetch below).
        float3 lmg = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
        lin = dlin * max(lmg * lmg, sh_env.z) * sh_env.y;
        if (fam < 9.5) {
          lin *= sh_env.w;
        }
        out_a = albedo.a;
      } else {
        lin = dlin * (fam < 11.5 ? sh_fogp.w : 1.0);
      }
    } else if (fam > 13.5 && fam < 14.5) {
      // scrollincandescent (the stadium LED chyron band): emissive scrolled
      // diffuse. The game's VS adds g_fAnimationTime * (uSpeed, vSpeed) to
      // the texcoord (misc.xy carries that offset, period-wrapped CPU-side);
      // its PS is one diffuse fetch -> D^2 * m_params[0].y (misc.z) ->
      // the shared fog/exposure/tone tail. No lightmap term, no kd, no
      // shadow receive, no material multiplier on the fog term
      // (ucode-exact: scrollincandescent_defaultPS).
      float3 sd = diffuse.Sample(smp, i.uv + misc.xy).rgb;
      lin = sd * sd * misc.z;
    } else {
      // Environment families: macro overlay (0.5-neutral, fades under decal
      // art), linear decal composite, lightmap squared and min-clamped
      // against (CSM s + shadow color), kd, phong spec vs the shader's
      // fixed literal light, cube reflection on 5/6.
      float3 ov = float3(1.0, 1.0, 1.0);
      // Decals-&-grime build-up layer (bit 256): macro weathering and decal
      // art both render neutral until revealed.
      bool sc_decals = ShowcaseLayerOn(i.pos.x, 256);
      // Fam 13 (transparentenvironmentreflective) carries no macro term.
      if (overlay.z > 0.0 && fam < 12.5 && sc_decals) {
        float3 mo = macro.Sample(smp, i.uv * overlay.x).rgb;
        ov = saturate((mo - 0.5) * overlay.y + 0.5);
      }
#if DECAL_PROBE >= 1 && DECAL_PROBE <= 4
      // TEMP probe (skate3_native_render_scene_decal_probe): the MACRO
      // weathering path, which is what environment.default (fam 1) uses.
      // Modes 5-8 must NOT return here: 5 is handled at the top of the world
      // branch and 6-8 belong to the decal block below, which this block
      // would otherwise shadow (every fam < 12.5 surface returned the macro
      // sample before reaching them).
      //   1 = the macro sample at the SHIPPING uv (uv * overlay.x)
      //   2 = the resulting overlay multiplier ov
      //   3 = PATH MARKER: proves this block runs, and shows the inputs --
      //       red   = overlay.z (the macro gate; 0 means the block is
      //               skipped and ov stays 1 = no weathering at all)
      //       green = overlay.x (macroOverlayUVScale, expect 0.3)
      //       blue  = overlay.y (macroOverlayOpacity)
      //   4 = the macro sampled at the RAW uv (no scale): if 1 is flat
      //       white but this shows concrete, the UV SCALE is the bug.
      if (fam < 12.5) {
#if DECAL_PROBE == 4
        return float4(macro.Sample(smp, i.uv).rgb, 1.0);
#elif DECAL_PROBE == 3
        return float4(overlay.z, overlay.x, overlay.y, 1.0);
#elif DECAL_PROBE == 2
        return float4(ov, 1.0);
#else
        return float4(macro.Sample(smp, i.uv * overlay.x).rgb, 1.0);
#endif
      }
#endif
      if (fam > 2.5 && fam < 4.5 && overlay.w > 0.5 && sc_decals) {
        // overlay.w == 0 = art unresolved (white fallback alpha 1 would
        // whitewash the whole surface).
        //
        // Art UV = uv3, TEXCOORD0.zw, for every decal family: the VS routes
        // it to o5.xy and the PS samples tf4 at r5.xy. The spec fetch
        //     tfetch2D r5.__yx, r0.xy, tf9
        // (ps_decalenvironment_defaultPS 11) writes only r5.zw (ecc / mask,
        // read at 74/84), so it never feeds the art coordinates.
        // Addressing = the game's own tf4 sampler state for this draw (see
        // SampleDecalArt), not the family name: decal vs decal_tileable is
        // nothing but a sampler-state difference on identical microcode.
        float4 dk = SampleDecalArt(i.uv3);
#if DECAL_PROBE >= 6
        // TEMP probe: the DECAL path for families 3/4.
        //   6 = decal alpha (the blend weight)
        //   7 = decal rgb
        //   8 = uv3 as red/green (are the decal UVs sane?)
#if DECAL_PROBE == 11
        // Alpha of the decal sampled at a FIXED, known-in-range uv. If this
        // is black too, the bound texture carries no alpha at all (decode /
        // format); if it is non-black while mode 6 is black, the alpha is
        // fine and the uv3 sample lands somewhere transparent.
        return float4(decal_art.SampleLevel(smp_clamp, float2(0.5, 0.5), 0).aaa,
                      1.0);
#elif DECAL_PROBE == 10
        // Mip 0 explicitly: a high mip of an atlas page can average to
        // alpha 0 even where mip 0 is opaque.
        return float4(decal_art.SampleLevel(smp_clamp, i.uv3, 0).aaa, 1.0);
#elif DECAL_PROBE == 9
        // uv3 RANGE check: green where uv3 is inside [0,1] on both axes,
        // red outside. Clamped sampling off the edge is the classic cause
        // of a flat, zero-alpha decal.
        {
          bool inside = i.uv3.x >= 0.0 && i.uv3.x <= 1.0 &&
                        i.uv3.y >= 0.0 && i.uv3.y <= 1.0;
          return float4(inside ? 0.0 : 1.0, inside ? 1.0 : 0.0, 0.0, 1.0);
        }
#elif DECAL_PROBE == 8
        return float4(frac(i.uv3.x), frac(i.uv3.y), 0.0, 1.0);
#elif DECAL_PROBE == 7
        return float4(dk.rgb, 1.0);
#else
        return float4(dk.a, dk.a, dk.a, 1.0);
#endif
#endif
        dlin = lerp(dlin, dk.rgb * dk.rgb, dk.a);
        float art_a = dk.a;
        if (DecalLayer2Bound()) {
          // decal2environment_defaultPS 17/20/67-68: the second layer (tf5,
          // its own UV and sampler state) composites over the first,
          // lerp(lerp(D^2, A1^2, a1), A2^2, a2); 23/39/42: the macro fade
          // keys on max(a1, a2).
          float4 dk2 = SampleDecalArt2(i.uv4);
          dlin = lerp(dlin, dk2.rgb * dk2.rgb, dk2.a);
          art_a = max(art_a, dk2.a);
        }
        // The macro weathering overlay applies over the composited art,
        // faded under it down to a per-material floor (ucode-exact,
        // decalenvironment_defaultPS 23/27/60):
        //   ov' = lerp(1, ov, max(1 - art.a, VS c10.x))
        // The old pure `lerp(1, ov, 1-dk.a)` fade (floor 0) rendered the
        // PCU Library ramp stencils 1/ov too bright; the no-fade model that
        // replaced it is this formula's floor-1 case, which that capture's
        // material evidently carries. The floor is captured per draw (255 =
        // 1.0 = no fade until it is).
        ov = lerp(float3(1.0, 1.0, 1.0), ov, max(1.0 - art_a, DecalMacroFadeFloor()));
      }
      float3 dcol = dlin * ov;
      // CSM shadow term s = sat(infront + 1 - coverage) from the native
      // atlas (SampleCsmShadow bias/cascade conventions; the soft variant
      // adds the contact-hardening filter when PCSS is enabled), min'd
      // with the native static sun-shadow term.
      float s = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, i.nrm, i.pos.xy);
      // Lightmap fetch = the console's semantics: BILINEAR, CLAMPED, mip 0.
      // The composed atlas pages are single-level on console and the fetch
      // constants carry clamp_x/clamp_y = 2. Sampling them with the aniso-8
      // WRAP sampler over our generated mip chain averaged NEIGHBORING
      // atlas cells together at grazing angles (deep aniso LOD), and at the
      // page border the wrap pulled in the opposite edge of the page: the
      // reflective_trans canopy slope (cells at v ~ 0.996, white-cliff
      // cells wrapping in from v ~ 0) glowed ~2x bright while the right
      // awning went dark, with the decode, UVs and constants all verified
      // exact (diagnosed via the mode-7 raw-lightmap isolation view).
      float3 lmg = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
      // F12 isolation mode 7: visualize the RAW lightmap sample; mode 8:
      // visualize the lightmap unwrap coordinate (frac(uv2*16) in rg).
      // Debug taps live on fams 5/6/13 only; on fams 1-4 misc.z carries
      // the v2 material bind flags instead (family-gated below).
      if (fam > 4.5 && abs(misc.z - 7.0) < 0.5) {
        return float4(PassGamma(lmg), 1.0);
      }
      if (fam > 4.5 && abs(misc.z - 8.0) < 0.5) {
        return float4(PassGamma(float3(frac(i.uv2 * 16.0), 0.0)), 1.0);
      }
      // Mode 9: lightmap-resolve status; RED = a real lightmap is bound
      // (tint.r set by the C++ resolve), BLUE = white fallback in t1.
      if (fam > 4.5 && abs(misc.z - 9.0) < 0.5) {
        return float4(PassGamma(float3(tint.r, 0.0, 1.0 - tint.r)), 1.0);
      }
      // tint.r == 0 = the real lightmap has not resolved yet (first-sight
      // decode in flight; t1 = the white fallback). Min-clamping the
      // fallback against the CSM term rendered in-shadow surfaces as BLACK
      // patches for the decode window (the ramp-stencil "black square
      // flash"); serve unshadowed brightness until the real page lands.
      float3 lml = tint.r > 0.0 ? min(lmg * lmg, s + sh_color.rgb) : lmg * lmg;
      // skate.Park loaded: the `_skatepark` permutations shade with
      // min(lm^2, lerp(c11, 1, cascade), lerp(c12, 1, park world map))
      // instead (baseenvironment_skateparkPS 72/87/88/90; ParkWorldShade).
      if (tint.r > 0.0 && ParkWorldReceiver(fam)) {
        lml = min(lmg * lmg, ParkWorldShade(i.rpos + cam_pos.xyz, i.pos.xy));
      }
      // GetTangentLight (world-shading v2). vnd is the tangent-space mapped
      // normal from the material's base normal map (t5) + detail map (t8 at
      // uv * misc.w): xy = 2*base + 2*detail - 2, z = 2*base.z - 1.
      // baseenvironment/decal normalize vnd for the kd dot; default-
      // environment (plain normal map, no detail pair) uses it raw. Fams
      // 1-4 carry bind flags in misc.z (1 = base normal at t5, 2 = detail
      // at t8, 4 = spec2ch at t9); fams 5/6 derive vnd from the reflective
      // path's nt below. Without a normal map the exact flat-map fold
      // 0.39 * 2.39562 = 0.93429 applies (the v1 constant).
      uint v2f = fam < 4.5 ? (uint)(misc.z + 0.5) : 0u;
      float kd = 0.93429;
      float3 wn = dot(i.nrm, i.nrm) > 0.01
                      ? normalize(i.nrm)
                      : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
      float3 vnd_raw = float3(0.0, 0.0, 1.0);  // wn mapping (max-z clamped)
      float3 vnd = float3(0.0, 0.0, 1.0);      // kd dot (family-normalized)
      bool have_vnd = false;
      if ((v2f & 1u) != 0u) {
        float3 nmv = normal_map.Sample(smp, i.uv).rgb;
        float2 dxy = (v2f & 2u) != 0u
                         ? detail_map.Sample(smp, i.uv * misc.w).rg
                         : float2(0.5, 0.5);
        vnd_raw = float3(nmv.xy * 2.0 + dxy * 2.0 - 2.0, nmv.z * 2.0 - 1.0);
        vnd = (fam < 1.5 || fam > 2.5) ? normalize(vnd_raw) : vnd_raw;
        have_vnd = true;
      }
      // Tangent frames shared by kd and the reflective normal mapping.
      // Two variants of the same construction:
      //  - tt_s/bb_s = the RAW screen-space UV-gradient frame
      //    (ScreenTangentFrame: exact for the kd sign terms under any UV
      //    orientation).
      //  - tt/bb = analytic world-up axes carrying the screen frame's
      //    signs; degree-accurate where the mapping IS world-up aligned
      //    (building facades, calibrated for the glass reflections).
      //    Transferring signs onto misaligned axes is METASTABLE when the
      //    UV grid sits ~90 deg off world-up: the transfer dots cross zero
      //    along a curved wall and kd banded light/dark around the bowl
      //    transition (seen in a matched F11 capture pair), which is why
      //    kd and the fam 1-4 normal mapping use the screen frame instead.
      float3 tt = float3(1.0, 0.0, 0.0), bb = float3(0.0, 1.0, 0.0);
      float3 tt_s = tt, bb_s = bb;
      if (have_vnd || (overlay.w > 3.5 && abs(misc.z - 3.0) > 0.5)) {
        ScreenTangentFrame(wn, i.rpos, i.uv, tt, bb);
        tt_s = tt;
        bb_s = bb;
        float3 bb2 = float3(0.0, 1.0, 0.0) - wn * wn.y;
        float lb2 = length(bb2);
        if (lb2 > 0.05) {
          bb2 /= lb2;
          float3 tt2 = cross(bb2, wn);
          tt = tt2 * (dot(tt2, tt) >= 0.0 ? 1.0 : -1.0);
          bb = bb2 * (dot(bb2, bb) >= 0.0 ? 1.0 : -1.0);
        }
      }
      // kd axes: stored per-vertex frame with the screen fallback (KdAxes).
      float3 kt, kb;
      KdAxes(i.tanb, wn, tt_s, bb_s, kt, kb);
      if (have_vnd) {
        // The mapped world normal feeds the spec mirror below; matte spec
        // is broad-lobed, so exact axis precision matters less than the
        // per-island signs (the analytic substitution exists for
        // mirror-sharp glass).
        wn = normalize(vnd_raw.x * kt + vnd_raw.y * kb +
                       wn * max(vnd_raw.z, 0.05));
        kd = (vnd.x * 0.58 * sign(dot(kt, sh_sun.xyz)) +
              vnd.y * 0.62 * sign(dot(kb, sh_sun.xyz)) +
              vnd.z * 0.39) * 2.39562;
      }
      // misc.z = 3 (F12 reflection isolation): force the flat normal.
      // Only the reflective families (5/6/13) ever carry overlay.w == 4;
      // fams 1-4 signal their normal map through the misc.z flags above.
      if (overlay.w > 3.5 && abs(misc.z - 3.0) > 0.5) {
          // Per-pixel normal map (t5, paired descriptor): the real PS
          // (baseenvironmentreflective_defaultPS) reflects off the
          // normal-mapped normal: tangent normal = 2*(n + detail - 1) on
          // xy, 2*n.z - 1 on z (the material's detail map is a constant
          // neutral 16x16, folded here as 0.5). With the FLAT vertex normal
          // every panel of a glass facade reflects the same tiny cube
          // region: one giant magnified smear of the plaza cube's
          // lamp-heads/trees (the "streetlight head" artifact, ucode-traced
          // on a captured pixel; flat N lands on the
          // face-0 tree/lamp texels, the mapped N tilts onto sky). The
          // per-panel tilts break the reflection up exactly like the
          // emulated frame. Screen-space cotangent frame (same shape as
          // the vehicle DXN branch; the world VS carries no tangent
          // attributes we decode).
          // F12 mode 6: the slider drives the normal-map LOD bias live;
          // the console fetches the nm at its 640p gradient LOD (blurrier,
          // weaker bump tilts), so the matching bias is the remaining
          // reflection-rotation candidate. Default stays SHARP (bias 0):
          // an unconditional misc.y bias visibly degraded the glass
          // reflections; tune with the
          // slider first, promote the found value to a default after.
          float nm_bias = misc.z > 5.5 ? misc.w : 0.0;
          float3 nmv = normal_map.SampleBias(smp, i.uv, nm_bias).rgb;
          // Exact composition: xy = 2*normal + 2*detail - 2, z = 2*n.z - 1.
          // The detail map is a CONSTANT 16x16 (0.514, 0.506), NOT the
          // formula's 0.5 neutral, so its fold is a constant tangent tilt.
          // The tilt rides the two F12 trim sliders (packed in misc.x;
          // defaults = the exact fold) so the residual reflection rotation
          // can be dialed live against the emulated frame.
          float trim_yi = floor(misc.x / 1000.0);
          float2 trim = float2(misc.x - trim_yi * 1000.0 - 500.0, trim_yi - 500.0) *
                        0.001;
          float3 nt = float3(nmv.xy * 2.0 - 1.0 + trim, nmv.z * 2.0 - 1.0);
          // (Frame construction hoisted above; the calibration notes ride
          // with it; tt/bb here are the shared axes.)
          wn = normalize(nt.x * tt + nt.y * bb + wn * max(nt.z, 0.05));
          // v2: the reflective families share the kd term; fam 5
          // (reflective) is a base-family material and normalizes vnd for
          // the dot like baseenvironment; fam 6 (reflective_simple) uses
          // it raw like the other _simple family. kt/kb = the shared kd
          // axes computed above (stored frame when the mesh carries one),
          // built from the pre-mapping geometric normal.
          float3 v56 = fam < 5.5 ? normalize(nt) : nt;
          kd = (v56.x * 0.58 * sign(dot(kt, sh_sun.xyz)) +
                v56.y * 0.62 * sign(dot(kb, sh_sun.xyz)) +
                v56.z * 0.39) * 2.39562;
      }
      // Fam 13 has no kd term at all; its body is D^2 * lml * ALPHA,
      // premultiplied once in the shader on top of the a^2 blend factor
      // (verified 0.0-error vs the ucode).
      lin = fam > 12.5 ? lml * dcol * albedo.a : lml * kd * dcol;
      if (overlay.w > 2.5) {
        // spec/ecc/refmask at t4: phong vs the FIXED literal light
        // (-0.14, 0.5, 0.9), power 10 + 290*ecc, tint (2.1, 1.8, 1.5),
        // scaled by the clamped lightmap green and the spec mask.
        float4 masks = decal_art.Sample(smp, i.uv);
        float3 vd = -normalize(i.rpos);
        float3 Ls = float3(-0.14, 0.5, 0.9);
        float3 refl = Ls - 2.0 * wn * dot(wn, Ls);
        float bp = saturate(dot(vd, -refl));
        float ks = pow(max(bp, 1e-6), 10.0 + 290.0 * masks.y);
        // misc.z = 5 (F12 reflection isolation): body only, no spec/cube.
        if (abs(misc.z - 5.0) > 0.5) {
          lin += ks * float3(2.1, 1.8, 1.5) * lml.g * masks.x;
        }
        if (((fam > 4.5 && fam < 6.5) || fam > 12.5) &&
            abs(misc.z - 5.0) > 0.5 && abs(misc.z - 1.0) > 0.5) {
          // Cube reflection: reflect(E, wN) with xy negated (the source's
          // ref_vec.xy *= -1), luminosity lerped toward 1 by
          // 0.3 * sat(4*refmask - 2.6), x refmask x reflection_scale 1.5.
          float3 rv = vd - 2.0 * wn * dot(vd, wn);
          // misc.y = cube LOD bias log2(render_height / 640): the guest
          // computes the cube fetch's gradient LOD at its own 1152x640
          // render; at 4K our per-pixel gradients are ~3.4x smaller, so
          // without the bias baked cube detail (the plaza streetlight
          // heads) survives through mips the console's fetch blurs away.
          // F12 isolation (misc.z): mode 2 samples the ABSOLUTE level in
          // misc.w; other modes add misc.w as extra bias; mode 4 shows the
          // raw cube sample in place of the shaded result.
          float3 dir = float3(-rv.x, -rv.y, rv.z);
          // Mode 6 gives the slider to the NM fetch; the cube keeps just
          // the automatic bias there.
          float cube_extra = misc.z > 5.5 ? 0.0 : misc.w;
          float3 cube = abs(misc.z - 2.0) < 0.5
                            ? env_cube.SampleLevel(smp, dir, misc.w).rgb
                            : env_cube.SampleBias(smp, dir, misc.y + cube_extra).rgb;
          float rl = 0.3 * saturate(4.0 * masks.z - 2.6);
          float lum = lml.g + rl * (1.0 - lml.g);
          lin += cube * lum * masks.z * 1.5;
          if (abs(misc.z - 4.0) < 0.5) {
            lin = cube * cube;
          }
        }
      }
      // v2 decal-family spec (spec/ecc at t9): the same phong-vs-fixed-
      // light term as the base families; the decal art occupies t4 on
      // fams 3/4, so their masks ride the second pair table.
      if ((v2f & 4u) != 0u) {
        float2 m2 = spec2_map.Sample(smp, i.uv).rg;
        float3 vd2 = -normalize(i.rpos);
        float3 Ls2 = float3(-0.14, 0.5, 0.9);
        float3 refl2 = Ls2 - 2.0 * wn * dot(wn, Ls2);
        float ks2 =
            pow(max(saturate(dot(vd2, -refl2)), 1e-6), 10.0 + 290.0 * m2.y);
        lin += ks2 * float3(2.1, 1.8, 1.5) * lml.g * m2.x;
      }
      if (fam > 12.5) {
        // Fam 13 blend factor: the PS outputs a^2 (straight-alpha blend on
        // top of the in-shader premultiplied body; wisps thin as ~a^3,
        // same convention as transparentenvironment).
        out_a = albedo.a * albedo.a;
      } else if (fam > 6.5) {
        out_a = albedo.a;
        reduced_tone = fam > 7.5;  // environmentdiffuse's cheap tonemap
      }
      fog_a *= sh_env.x;  // material multiplier (PS c11.y)
    }
    // Fog -> exposure -> tonemap -> sqrt, then the postfx uber's measured
    // 1.41 scene multiplier (same as the character branch).
    float3 xe = (lin * fog_a + fog_rgb) * expo;
    return ToneOut(xe, out_a, reduced_tone);
  }
  // environment.decal surfaces: the paint/graffiti art (t4) is composited
  // over the base diffuse by ITS alpha, opaque output; these meshes ARE
  // the wall/ground there. The art maps with uv3, the second half-pair of
  // the packed half4 first texcoord (validated offline: sampling with the
  // tiling uv0 repeats it: "Stereo Stereo Stereo"; the fmt-26 second
  // element is the lightmap unwrap, not the decal's). Composited BEFORE the
  // macro overlay: the weathering applies over the paint too
  // (ApplyOverlay(cOverlay, ApplyDecal(...)); the old order left the paint
  // unweathered, the too-white ramp stencils).
  // Decals-&-grime build-up layer (bit 256): art and macro weathering both
  // render neutral until revealed.
  bool sc_decals = ShowcaseLayerOn(i.pos.x, 256);
  if (overlay.w > 0.0 && sc_decals) {
    // Addressing = the game's per-draw tf4 sampler state packed into
    // overlay.w (see SampleDecalArt): tileable rock/cliff art wraps, single
    // placements clamp, exactly as the game bound them.
    float4 dk = SampleDecalArt(i.uv3);
    albedo.rgb = lerp(albedo.rgb, dk.rgb, dk.a);
#if DECAL_PROBE
    // TEMP probe (compile-time; the root signature is full so this cannot
    // ride a constant). Visualizes what the decal sampler actually returns
    // on decal surfaces whose art the game samples with a REPEAT axis:
    //   DECAL_PROBE=1 decal alpha as greyscale (the blend weight)
    //   DECAL_PROBE=2 decal rgb straight (is the right art bound?)
    //   DECAL_PROBE=3 uv3 as red/green (are the decal UVs sane?)
    if ((DecalArtCode() & 7u) == 0u || ((DecalArtCode() >> 3) & 7u) == 0u) {
#if DECAL_PROBE == 3
      return float4(frac(i.uv3.x), frac(i.uv3.y), 0.0, 1.0);
#elif DECAL_PROBE == 2
      return float4(dk.rgb, 1.0);
#else
      return float4(dk.a, dk.a, dk.a, 1.0);
#endif
    }
#endif
  }
  // Macro overlay: large-scale grime/cracks multiplied over the diffuse at
  // uv * macroOverlayUVScale, faded by macroOverlayOpacity: the ground and
  // wall weathering. WHITE is the neutral (materials without weathering
  // bind a 16x16 "default_white"). The game multiplies it ONCE in its
  // linear (squared) color space, so the gamma-space equivalent is
  // sqrt(m); a direct multiply doubles the darkening (harsh black
  // patchwork vs the emulated subtle weathering).
  if (overlay.z > 0.0 && misc.x < 1.5 &&  // water reuses overlay.z (ripple map flag)
      sc_decals) {
    float4 m = macro.Sample(smp, i.uv * overlay.x);
    albedo.rgb *= lerp(float3(1.0, 1.0, 1.0), sqrt(m.rgb), overlay.y * m.a);
  }
  // tint.r > 0 marks items with a lightmap bound (2x baked lighting);
  // otherwise fall back to derivative face shading. The lighting term stays
  // separate from the albedo so the CSM receive below can min-clamp IT, the
  // way the game's GetShadowedLightMap clamps the lightmap lighting.
  float3 light;
  if (tint.b > 0.0) {
    light = float3(1.0, 1.0, 1.0);  // unlit (sky dome)
  } else if (tint.r > 0.0) {
    // Console lightmap semantics: bilinear, clamped, mip 0; the composed
    // atlas pages are single-level; mip/wrap sampling bled neighbor cells
    // and page edges (see the exact env fetch).
    light = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb * 2.0;
  } else {
    // Smooth per-vertex normal when the mesh has one; face normal from
    // position derivatives otherwise.
    float3 n = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    light = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.35 + 0.75;
  }
  // Dynamic CSM shadow receive (world geometry + rigid props; characters
  // need the game's separate PCF/bias variant; skipping them avoids
  // self-shadow acne, and the ground shadow is 95% of the visible effect).
  // Exact receiver math from the baseenvironment PS disassembly: finest
  // cascade whose |ls| < 0.99 wins; shadow = saturate(infront + 1 -
  // coverage), then the game min-clamps the LINEAR lighting term:
  //   light_linear = min(light_linear, s + c8.rgb)
  // Full shadow clamps to the dim bluish c8 ambient, the penumbra saturates
  // wherever the clamp exceeds the lit level (which is what keeps the edge
  // crisp), and surfaces already darker than the clamp, baked shade under
  // bridges/trees, show NO dynamic shadow at all. Our light term is
  // gamma-space (light^2 ~ the game's linear term: the lightmap x2 folds
  // its x4 linear multiplier), so the clamp maps to min(light, sqrt(s+c8))
  // per channel. A fixed-denominator curve here read pitch-black and
  // double-darkened baked shade.
  if (sh_misc.y > 0.0 && tint.g == 0.0 && tint.b <= 0.0 && misc.x == 0.0) {
    // Uncovered positions return 1 and the min-clamp is a no-op there.
    // Static world geometry receives the DYNAMIC cascades only: in the game
    // every static family (baseenvironment / defaultenvironment / baseterrain /
    // building / decalenvironment / advertisement / flowingwater) does exactly
    // ONE 4-tap shadow fetch (tf3), while the dynamic receivers
    // (dynamicobject, defaultcharacter, livingworld_stamp, vehicle) do TWO
    // (tf0 + tf1: cascades + the baked-shade world map). The world's static
    // shade is already in its lightmaps, so sampling the static sun map here
    // painted shadows the game never draws and double-darkened baked shade.

    float s = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, i.nrm, i.pos.xy);
    light = min(light, sqrt(s + sh_color.rgb));
  }
  float3 lit = albedo.rgb * light;
  if (mat_tint.w > 0.0 && misc.x == 0.0) {
    lit *= mat_tint.rgb;
  }
  if (misc.x > 1.5) {
    // water.* (flowingwater.fx approximation): the real shader is
    // near-black diffuse + dual time-scrolled ripple-normal taps + an
    // environment-cube reflection + sun specular. We have no cube map
    // bound, so the reflection term is the frame fog color (the best
    // single approximation of the surroundings' haze tone) scaled by a
    // fresnel curve; the ripple normal perturbs both the fresnel and a sun
    // sparkle along the captured CSM light axis. The lightmap (x2) keeps
    // the baked bridge/wall shading on the surface. Calibrated against the
    // canal capture (emulated mid-canal mean ~(24,28,32)/255).
    float t = overlay.x;
    float2 rip;
    if (overlay.z > 0.0) {
      // Dual scrolled taps of the material's ripple normal map (macro slot).
      float2 wuv = i.uv * 6.0;
      float3 n1 = macro.Sample(smp, wuv + t * float2(0.11, 0.06)).rgb;
      float3 n2 = macro.Sample(smp, wuv * 1.71 - t * float2(0.07, 0.13)).rgb;
      rip = (n1.xy + n2.xy) - 1.0;
    } else {
      // Normal map unresolved: procedural ripples from world position.
      // Wavelengths ~0.4-1m (emulated ripples are decimeter-scale); low
      // frequencies formed meter-wide chevron interference bands that read
      // as giant arrows on the surface.
      float3 wp = i.rpos + cam_pos.xyz;
      rip = 0.35 * float2(sin(wp.x * 9.7 + wp.z * 6.1 + t * 2.3) +
                              0.6 * sin(wp.x * 17.3 - wp.z * 11.9 + t * 3.4),
                          cos(wp.x * 7.1 - wp.z * 13.7 + t * 2.7) +
                              0.6 * cos(wp.x * 12.9 + wp.z * 18.3 + t * 3.1));
    }
    float3 wn = normalize(float3(rip.x * 0.4, 2.0, rip.y * 0.4));
    float3 vd = -normalize(i.rpos);
    float fres = pow(1.0 - saturate(dot(vd, wn)), 3.0);
    // The flowing-water lightmap unwrap decodes unreliably (bands across
    // atlas gutters), so the water term deliberately ignores it: near-black
    // body + ripple-perturbed cube reflection + sun sparkle.
    // Deep body: the water "diffuse" is a faint STRIPE MASK (max 24/255,
    // WaterFallFoamAlpha, a lookup for the real shader, not a color). The
    // game consumes it in linear space where 0.09^2 vanishes; squaring here
    // likewise kills the visible blue/black banding. overlay.w > 0 = no
    // diffuse channel at all (ocean.default); body is zero there, NOT the
    // white fallback (ocean.fx: diffTerm = (0,0,0), color is all reflection).
    float3 col = overlay.w > 0.5 ? 0.0 : albedo.rgb * albedo.rgb * 0.6;
    // Reflection tint: the environment cube when resolved (t6); otherwise a
    // haze derived from the frame fog color, lifted toward neutral so dark
    // dusk fog doesn't collapse the water to black (fit: emulated canal
    // mean ~(24,28,32)/255 with fog color (0.02,0.07,0.13)).
    float3 renv = overlay.y > 0.0
                      ? env_cube.Sample(smp, reflect(-vd, wn)).rgb
                      : mat_tint.rgb * 0.5 + 0.06;
    col += renv * (0.55 + 0.45 * fres);
    if (sh_misc.y > 0.0) {
      float3 h = normalize(vd + normalize(-sh_z.xyz));
      col += pow(saturate(dot(wn, h)), 90.0) * 0.35;            // sun sparkle
    }
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    col = sqrt(max(col * col * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    // Opaque: the game's murk hides the canal bed entirely (and our bed
    // shading is untrustworthy under water: striped lightmap unwraps).
    return float4(PassGamma(col), 1.0);
  }
  if (misc.x > 0.0) {
    // transparentenvironment.fx (Skate 2 source; disassembled Skate 3 PS
    // matches): outcolor.rgb = diffTerm * diffuse.rgb * diffuse.a; the rgb
    // is premultiplied by alpha ONCE IN THE SHADER on top of the a^2 blend
    // factor, so wisps thin out as ~a^3. That cubic falloff is most of the
    // emulated "sparse clouds" look. Fog is applied in the game's linear
    // (squared) color space: fade = saturate(dist * ramp.x + ramp.y)^ramp.z
    // toward the fog color, transmittance = 1 + fade * fogcolor.w.
    lit *= albedo.a;
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    lit = sqrt(max(lit * lit * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    return float4(PassGamma(lit), albedo.a * albedo.a);
  }
  return float4(PassGamma(lit), 1.0);
}
float4 ps_main(VSOut i) : SV_Target {
  // Remove only the local avatar's head around the first-person anchor.
  // Body below the neck, other skaters, and shadow passes stay visible.
  if (world._34 > 0.5 && fp_head.w > 0.0) {
    float3 d = i.rpos + cam_pos.xyz - fp_head.xyz;
    clip(dot(d,d) - fp_head.w*fp_head.w);
  }
  int mask = ShowcaseMask(i.pos.x);
  // Dynamic-entities build-up layer (bit 512): entity draws (tint.a < 0,
  // staged CPU-side from DrawItem::dyn_entity) drop out entirely until the
  // bit reveals on this pixel's side of the split; no color and no depth,
  // so the world behind them renders as if they were never published.
  if (mask >= 0 && (mask & 512) == 0 && tint.a < -0.5) {
    clip(-1.0);
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  float4 c = ShadePixel(i);
  // Deck (skateboard) contact shadow: the console applies it as a deferred
  // decal over the resolved scene, so modulating the shaded pixel -- after
  // each family's own tone curve -- is the faithful place for it. The
  // guest's shader adds the depth buffer's STENCIL byte into the mask,
  // which whites the decal out over dynamic geometry; the families it
  // marks are exactly the ones excluded here (characters, dynamicobject
  // props -- the board itself -- particles, sky and the flat/2D path).
  // The skate.Park pieces (environmentpark.*, cam_pos.w -24..-26) are NOT
  // marked: neither their z-prepass nor their main-pass draws enable
  // stencil (RB_DEPTHCONTROL 0x700736, capture 1790871823), so the decal
  // darkens them like the world.
  // The preview object (foreground, -(20 + variant + 0.25)) is excluded: its
  // draws DO enable stencil (RB_DEPTHCONTROL 0x708777 / 0x708737, capture
  // 1790890720), which masks the decal there.
  const bool park_piece = cam_pos.w < -23.5 && cam_pos.w > -26.5 && frac(-cam_pos.w) < 0.1;
  if (tint.a <= 0.0 && ((cam_pos.w < 0.5 && cam_pos.w > -20.5) || park_piece)) {
    c.rgb *= DeckShadow(i.rpos + cam_pos.xyz);
  }
  // Blackout stage (bit 1024, the showcase's opening/closing bookends and
  // the recording cut marker): every draw renders black, keeping its own
  // alpha so coverage, alpha test and blend routing stay intact.
  if (mask >= 0 && (mask & 1024) != 0) {
    return float4(0.0, 0.0, 0.0, c.a);
  }
  if (mask < 0 || (mask & 4) != 0) {
    return c;  // showcase off, or the full material layer is revealed
  }
  // Pre-material build-up looks replace the shaded rgb but keep the
  // branch's own alpha, so alpha tests, coverage channels, fades and blend
  // routing all behave exactly as in the full render. The gamma-space
  // looks encode through PassGamma like the legacy branches; the exact-
  // chain lighting look goes through ToneOut. The sky dome (exact family
  // -40 / the legacy unlit marker) is authored content; it arrives
  // complete with the lighting layer and never receives cast shadows.
  bool sky = (cam_pos.w < -39.5 && cam_pos.w > -40.5) || tint.b > 0.0;
  if (sky && (mask & 2) != 0) {
    return c;
  }
  float3 n = dot(i.nrm, i.nrm) > 0.01
                 ? normalize(i.nrm)
                 : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float3 ldir = dot(sh_sun.xyz, sh_sun.xyz) > 0.1
                    ? sh_sun.xyz
                    : normalize(float3(0.4, 0.8, 0.3));
  // The dynamic-shadow layer applies to every pre-material look (the
  // helpers self-gate on the mask, so uncovered frames sample lit).
  float s = 1.0;
  if (!sky && (mask & 8) != 0) {
    s = SampleCsmShadowSoft(i.rpos + cam_pos.xyz,
                                tint.g > 0.0 ? 0.012 : 0.0, i.nrm, i.pos.xy);
  }
  // Base color: the diffuse texture once the albedo layer is revealed,
  // clay grey before it.
  float3 base = (mask & 1) != 0 ? diffuse.Sample(smp, i.uv).rgb
                                : float3(0.62, 0.62, 0.62);
  if ((mask & 2) != 0 && tint.r > 0.0 && sh_sun.w > 0.01) {
    // Baked lighting on a lightmapped surface: the REAL diffuse chain,
    // lightmap^2 x the flat-map kd x fog/exposure/material multiplier,
    // the exact-family formula minus surface detail, so the later step
    // to full materials only adds normal/detail maps, specular,
    // reflections and weathering instead of also re-grading the frame's
    // tone. The shadow layer joins with the game's own lightmap min-clamp.
    float3 lm = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
    float3 lml = lm * lm;
    if ((mask & 8) != 0) {
      lml = min(lml, s + sh_color.rgb);
    }
    float3 lin = lml * 0.93429 * base * base;
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 xe = (lin * ((1.0 + sh_fogc.a * f1) * sh_env.x) +
                 sh_fogc.rgb * f1) * sh_sun.w;
    return ToneOut(xe, c.a, false);
  }
  float3 col;
  if ((mask & 2) != 0) {
    // Lighting without exact frame rows: the legacy-branch convention
    // (lightmap x2 in gamma space), or a neutral wrapped lambert for
    // surfaces without a lightmap (characters, props).
    float3 lite;
    if (tint.r > 0.0) {
      lite = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb * 2.0;
    } else {
      float wrap = 0.35 + 0.65 * saturate(dot(n, ldir));
      lite = float3(wrap, wrap, wrap);
    }
    col = base * lite;
  } else if ((mask & 1) != 0) {
    col = base;  // raw diffuse, unlit
  } else {
    // Clay: grey with a simple sun-lambert so the geometry reads (a fully
    // flat grey collapses everything into silhouettes).
    col = base * (0.61 + 0.68 * saturate(dot(n, ldir)));
  }
  // Shadow layer on the gamma-space looks: a soft darkening multiply (the
  // exact-chain look above uses the game's own clamp instead).
  if (!sky && (mask & 8) != 0) {
    col *= s * 0.65 + 0.35;
  }
  return float4(PassGamma(saturate(col)), c.a);
}
// ---- SSR reflection G-buffer (consumed by ssr.hlsl ps_march) --------------
// Rendered after the main pass from the frame's reflective items only (env
// families 5/6/13 + water), half res, single sample, NO depth buffer:
// RG = octahedral WORLD-space normal, B = linear view depth (the clip w),
// A = reflectivity weight, negated for surfaces that never write scene
// depth (water / blended fam-13 glass), which selects the march's in-front
// visibility rule instead of the exact depth match. The caller re-stages
// each item's main-pass root constants and t3/t4/t5 textures, so the normal
// composition below is the material branch's own math.
float2 OctEncode(float3 n) {
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  if (n.z < 0.0) {
    n.xy = float2((1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0),
                  (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0));
  }
  return n.xy;
}
// Game stencil port (RenderStencilMark): the draw's z-pass stencil REPLACE
// value (ref / 255, staged in tint.r, which vs_main never reads) into the
// R8 mark target. The marked draws run with alpha test off, so the game's
// stencil covers the full rasterized coverage: no texture reads here.
float4 ps_stencil_mark(VSOut i) : SV_Target {
  return float4(tint.r, 0.0, 0.0, 0.0);
}
// The same mark for alpha-tested draws (the foreground preview prop's leaf
// cards): the game's mask is the stencil its draw writes, and the alpha
// test kills those pixels before any stencil write -- ALPHAREF 30, the
// alphatest families' own (diffuse at t0).
float4 ps_stencil_mark_clip(VSOut i) : SV_Target {
  clip(diffuse.Sample(smp, i.uv).a - 0.1176);
  return float4(tint.r, 0.0, 0.0, 0.0);
}

float4 ps_refl_gbuf(VSOut i) : SV_Target {
  float3 wn;
  float w;
  float trans;
  // Env families carry cam_pos.w = -fam; water rides the legacy branch with
  // cam_pos.w = 0 (misc.x is the packed normal-trim on fams 5/6 here, so it
  // cannot discriminate).
  if (cam_pos.w < -30.5 && cam_pos.w > -32.5) {
    // Exact ocean families: no SSR contribution (the ocean keeps its own
    // cube/baked reflections): zero weight, in-front visibility.
    return float4(OctEncode(float3(0.0, 1.0, 0.0)), i.pos.w, -0.0);
  }
  if (cam_pos.w < -29.5 && cam_pos.w > -30.5) {
    // Exact water (fam 30): the branch's own ripple-perturbed normal;
    // weight from the thresholded reflection mask (t4.z), boosted like the
    // glass arm below. Never writes scene depth -> in-front visibility.
    // The item's own material set when it has one (see ShadeFlowingWater).
    const uint fset = uint(overlay.w + 0.5) & 15u;
    float4 fp0 = wat_p0, fp1 = wat_p1, fp2 = wat_p2, fp3 = wat_p3;
    if (fset > 0u) {
      const uint fb = (fset - 1u) * 7u;
      fp0 = fw_set[fb + 0];
      fp1 = fw_set[fb + 1];
      fp2 = fw_set[fb + 2];
      fp3 = fw_set[fb + 3];
    }
    float t = fp3.x;
    float3 n1 = macro.Sample(smp, i.uv * fp2.xy + fp1.xy * t).rgb;
    float3 n2 = macro.Sample(smp, i.uv * fp2.zw + fp1.zw * t).rgb;
    float3 nsum = 2.0 * n1 + 2.0 * n2 - 2.0;
    float3 vn = normalize(float3(nsum.x * fp0.x, nsum.y * fp0.z,
                                 nsum.z * fp0.w));
    float3 wn0 = dot(i.nrm, i.nrm) > 0.01
                     ? normalize(i.nrm)
                     : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float3 wt, wb;
    if (i.tanb.w > 0.1) {
      wt = normalize(i.tanb.xyz);
      wb = cross(wn0, wt) * (i.tanb.w > 0.6 ? 1.0 : -1.0);
    } else {
      ScreenTangentFrame(wn0, i.rpos, i.uv, wt, wb);
    }
    float3 wwn = normalize(vn.x * wt + vn.y * wb + vn.z * wn0);
    float wmask = overlay.x > 0.0
                      ? saturate(decal_art.Sample(smp, i.uv).z - fp3.y)
                      : 0.0;
    return float4(OctEncode(wwn), i.pos.w, -saturate(wmask * 2.5));
  }
  if (cam_pos.w < -0.5) {
    // Fams 5/6/13: the reflective branch's own normal + reflection mask.
    // No alpha test; masked reflective draws skip the clip in ps_main too
    // (overlay.w >= 3).
    float fam = -cam_pos.w;
    float4 masks = decal_art.Sample(smp, i.uv);
    // Blend weight, not the material's energy term: the raw refmask
    // (~0.2-0.5 on glass) is authored to scale the cube ADDEND; using it
    // directly as the SSR lerp weight leaves the cube fallback dominating
    // and the reflections imperceptible. Boost so real glass approaches
    // full replacement while zero-mask texels stay out.
    w = saturate(masks.z * 2.5);
    trans = fam > 12.5 ? 1.0 : 0.0;
    wn = dot(i.nrm, i.nrm) > 0.01
             ? normalize(i.nrm)
             : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    if (overlay.w > 3.5) {
      // Normal-mapped per-panel tilts (fams 5/6): the same composition as
      // the material branch (t5 map + the constant detail-fold trim packed
      // in misc.x; analytic world-up axes carrying the screen-frame signs).
      float3 tt, bb;
      ScreenTangentFrame(wn, i.rpos, i.uv, tt, bb);
      float3 bb2 = float3(0.0, 1.0, 0.0) - wn * wn.y;
      float lb2 = length(bb2);
      if (lb2 > 0.05) {
        bb2 /= lb2;
        float3 tt2 = cross(bb2, wn);
        tt = tt2 * (dot(tt2, tt) >= 0.0 ? 1.0 : -1.0);
        bb = bb2 * (dot(bb2, bb) >= 0.0 ? 1.0 : -1.0);
      }
      float trim_yi = floor(misc.x / 1000.0);
      float2 trim = float2(misc.x - trim_yi * 1000.0 - 500.0,
                           trim_yi - 500.0) * 0.001;
      float3 nmv = normal_map.Sample(smp, i.uv).rgb;
      float3 nt = float3(nmv.xy * 2.0 - 1.0 + trim, nmv.z * 2.0 - 1.0);
      wn = normalize(nt.x * tt + nt.y * bb + wn * max(nt.z, 0.05));
    }
  } else {
    // Water: the water branch's ripple-perturbed up normal; reflectivity =
    // the branch's own fresnel-scaled reflection weight (0.55 + 0.45*fres).
    float t = overlay.x;
    float2 rip;
    if (overlay.z > 0.0) {
      float2 wuv = i.uv * 6.0;
      float3 n1 = macro.Sample(smp, wuv + t * float2(0.11, 0.06)).rgb;
      float3 n2 = macro.Sample(smp, wuv * 1.71 - t * float2(0.07, 0.13)).rgb;
      rip = (n1.xy + n2.xy) - 1.0;
    } else {
      float3 wp = i.rpos + cam_pos.xyz;
      rip = 0.35 * float2(sin(wp.x * 9.7 + wp.z * 6.1 + t * 2.3) +
                              0.6 * sin(wp.x * 17.3 - wp.z * 11.9 + t * 3.4),
                          cos(wp.x * 7.1 - wp.z * 13.7 + t * 2.7) +
                              0.6 * cos(wp.x * 12.9 + wp.z * 18.3 + t * 3.1));
    }
    wn = normalize(float3(rip.x * 0.4, 2.0, rip.y * 0.4));
    float3 vd = -normalize(i.rpos);
    float fres = pow(1.0 - saturate(dot(vd, wn)), 3.0);
    w = 0.55 + 0.45 * fres;
    trans = 1.0;
  }
  // B = the view depth in meters. SV_Position.w here delivers clip-space w
  // directly, which equals view Z under the row-vector projection (m23 = 1)
  // - verified empirically: marches from ViewPos(uv, pos.w) produce
  // geometrically coherent reflections, and inverting this value collapses
  // every downstream depth test (all rays die at the visibility gate).
  return float4(OctEncode(wn), i.pos.w, w * (trans > 0.5 ? -1.0 : 1.0));
}
// Shadow caster pass: vs_main runs with mvp = (world *) lightVP built from
// the captured receiver rows, so SV_Position.z IS the light-space depth
// (the height-ramp row; viewport z range 0..1, depth clip on so casters
// outside the height window are clipped, as the game's clip state does). MIN blend on
// both channels against a (1, 1) clear: R accumulates the min depth, G
// drops to 0 where any caster drew ("uncoverage"; the blur pass converts
// to the game's coverage convention).
float2 ps_shadow_caster(VSOut i) : SV_Target {
  return float2(i.pos.z, 0.0);
}
// Alpha-tested caster variant (foliage cards, alphatest fences/grates in
// the static sun-shadow map; dynamicobject.alphatest props in the CSM
// atlas): sampling the item's diffuse keeps the cutout silhouette instead
// of casting the full card quad. ALPHAREF 30/255 matches the world
// families' own opaque alpha test.
float2 ps_shadow_caster_clip(VSOut i) : SV_Target {
  // +2 LOD bias: at the static sun map's ~2 cm near-cascade texels the
  // raw leaf cutout rasters as binary salt-and-pepper dapple; two mip
  // levels of alpha pre-filtering merge it into the soft blobs the baked
  // lighting shows. The low ALPHAREF keeps mip-averaged coverage from
  // eroding the canopy.
  clip(diffuse.SampleBias(smp, i.uv, 2.0).a - 0.1176);
  return float2(i.pos.z, 0.0);
}
// CSM-atlas variant for alphatestdynamicobject casters, exact from
// alphatestdynamicobject_shadowPS: tfetch2D tf3.a with the pass's own
// gradients (NO LOD bias), kill when a < c255.x (literal 0.12). The +2 bias
// above averaged fine cutouts (the goal net's mesh) into alpha above the
// ref beyond a few meters, casting a solid sheet where the game keeps the
// net pattern.
float2 ps_shadow_caster_clip_exact(VSOut i) : SV_Target {
  clip(diffuse.Sample(smp, i.uv).a - 0.12);
  return float2(i.pos.z, 0.0);
}
