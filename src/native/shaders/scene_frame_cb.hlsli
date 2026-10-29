// Per-frame shadow / lighting / material constant slice (b1). Shared by
// the scene pass (scene.hlsl) and the HDR post chain (hdr.hlsl) - the
// volumetric march samples sun visibility through the same receiver
// rows. The CPU staging in skate3_native_scene_gpu.cpp mirrors this
// layout; row offsets there and here must agree.
cbuffer S : register(b1) {
  float4 sh_x;      // light-space X row (xyz) + translation (w)   [PS c0]
  float4 sh_y;      // light-space Y row                           [PS c3]
  float4 sh_z;      // depth row (height ramp)                     [PS c4]
  float4 sh_c1;     // cascade 1 scale.xy + offset.zw              [PS c1]
  float4 sh_c2;     // cascade 2 scale.xy + offset.zw              [PS c2]
  float4 sh_color;  // shadow color rgb [PS c8] + its luma in w
  float4 sh_misc;   // x = depth bias [PS c5.x], y = enable,
                    // zw = atlas dimensions (3*tile, tile)
  // Exact world-shading frame rows (consumed by the env-family branch):
  float4 sh_sun;    // xyz = sun direction [PS c6], w = scene exposure [c10.x]
  float4 sh_env;    // x = material multiplier [PS c11.y], yz = tree lightmap
                    // scale/floor [tree PS c0.xy], w = tree tint mult [c4.y]
  float4 sh_fogp;   // xyz = global fog ramp scale/bias/exp [VS c5],
                    // w = proxyworld scale [proxy PS c3.y]
  float4 sh_fogc;   // fog color rgb + transmittance scale in w [VS c6]
  // dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows).
  float4 dyn_sun;   // xyz = sun direction (PS c9), w = scene exposure (c13.x)
  float4 dyn_amb;   // xyz = flat ambient (c15.rgb), w = bounce scale (c15.w)
  float4 dyn_misc;  // x = material multiplier (c14.y),
                    // y = static world-shadow floor (c8.w)
  // Character CSM receive biases (game char PS bank, finest cascade first)
  // + enable in w: the exact 9-tap character shadow sampling
  // (cacstamp_skin_nisPS port; see SampleCsmShadow). sh_misc.zw carry the
  // atlas dimensions for the point-snapped taps.
  float4 sh_char;
  // World-shading v2: x = stored-tangent polarity (the sign relating
  // cross(binormal, normal) x handedness to the game's tangent; +-1,
  // cvar-tunable against the emulated reference), yzw spare.
  float4 sh_v2;
  // dynamicobject static world-shadow transform (the game's PS c5/c6/c7):
  // u = dot(wp4, dyn_wsx) * 0.5 + 0.5, v = dot(wp4, dyn_wsy) * -0.5 + 0.5,
  // ray depth = dot(wp4, dyn_wsz). Compared against the natively
  // re-rendered 512x512 baked-shade map at t4 (misc.z flag 8; see the
  // dynobj branch).
  float4 dyn_wsx;
  float4 dyn_wsy;
  float4 dyn_wsz;
  // flowingwateralpha m_params rows (captured per frame from the water PS
  // bank; consumed by the exact water branch, cam_pos.w = -30):
  float4 wat_p0;  // m_params[0]: tangent-normal scale xzw, material mult y
  float4 wat_p1;  // m_params[1]: animated-UV scroll speeds (tap1 xy, tap2 zw)
  float4 wat_p2;  // m_params[2]: tap UV scales (tap1 xy, tap2 zw)
  float4 wat_p3;  // x = g_fAnimationTime, y = mask threshold,
                  // z = spec power, w = alpha floor
  // ocean_defaultPS rows (exact ocean branch, cam_pos.w = -31): the PCA
  // mean (pre-swizzled to R,G,B channel order) + the six weight rows
  // (component-0/1 pairs for R, G, B) + m_params[0..2].
  float4 oc_mean;
  float4 oc_w0;   // R, component 0
  float4 oc_w1;   // R, component 1
  float4 oc_w2;   // G, component 0
  float4 oc_w3;   // G, component 1
  float4 oc_w4;   // B, component 0
  float4 oc_w5;   // B, component 1
  float4 oc_p0;   // m_params[0]: ward spec color
  float4 oc_p1;   // m_params[1]: y = multiplier, z = tonedown scale,
                  // w = PCA-tap uv scale
  float4 oc_p2;   // m_params[2]: ward spread u/v (xy), roughness (z)
  // oceanreflection row (fam 32): x = PS c3.x multiplier, y/z = VS
  // c7.y/c7.z, the world-height alpha fade bounds.
  float4 orf;
  // Contact-hardening soft shadows (PCSS over the same cascade atlas; see
  // SampleCsmShadowSoft): x = enable, y = tan of the sun's angular radius
  // (penumbra width per meter of caster distance), z = max penumbra
  // half-width in meters, w = static-caster receive bias in depth units
  // (slope-scaled in the sampler; zero when static geometry is not in the
  // atlas).
  float4 sh_pcss;
  // x = blocker-search radius in meters, y = meters per depth-map unit
  // (1 / |sh_z.xyz|), z = light-space units per meter (|sh_x.xyz|, base
  // cascade), w = minimum filter radius in physical atlas texels.
  float4 sh_pcss2;
  // Native static sun-shadow map transform (see SampleStaticSun): the
  // world -> FAR-tile rows. uc = dot(nsm_x.xyz, wp) + nsm_x.w in [-1,1]
  // (same for y), depth = dot(nsm_z.xyz, wp) + nsm_z.w in [0,1]. The near
  // tile shares the origin, so its coordinate is exactly uc * nsm_p2.z.
  float4 nsm_x;
  float4 nsm_y;
  float4 nsm_z;
  // x = static shadow strength (0 = feature off), y = far-tile uc units
  // per meter (1 / far radius), z = meters per depth unit, w = minimum
  // filter radius in tile uc units.
  float4 nsm_p;
  // x = base receiver bias (depth units), y = slope-scaled bias term
  // (depth units), z = reserved (cascade ratios are the shader-side
  // kRatioInner/kRatioMid constants), w = tile size in pixels (tap
  // clamping).
  float4 nsm_p2;
  // water.default (water_defaultPS, fam 33), captured per frame. Read off
  // this build's own ucode; see FrameScene::waterdef_rows for the row map.
  float4 wd_mean;   // c8   PCA mean / tap bias
  float4 wd_w0;     // c9   PCA weight rows ...
  float4 wd_w1;     // c10
  float4 wd_w2;     // c11
  float4 wd_w3;     // c12
  float4 wd_w4;     // c13
  float4 wd_w5;     // c14
  float4 wd_str;    // c22  per-tap normal strengths (.z/.w)
  float4 wd_speed;  // c23  scroll speeds (tap1 xy, tap2 zw)
  float4 wd_scale;  // c24  uv scales     (tap1 xy, tap2 zw)
  float4 wd_misc;   // c25  x = uv-scale mult, y = threshold, z = spec power
  float4 wd_time;   // c26  x = animation time
  float4 wd_refl;   // c20  w = reflection strength
  float4 wd_fres;   // c21  y/z/w fresnel + spec scales
  // xy = 1/render-target size, for projecting the refraction fetch; zw free.
  float4 wd_screen;
  // flowingwateralpha reflection gate, c14.yzw: the ucode builds it as
  // c14.y * sat(c14.z * (mask - c14.w)) (lines 71/76/78). These were baked
  // into the shader as 0.3 / 4.0 / 2.6, which happen to be this material's
  // values but do not follow any other one.
  float4 wat_gate;  // xyz = c14.y/z/w, w = c13.w refl strength
  // flowingwateralpha sun-spec: rgb = the c12 tint (ucode 64/81), w = the
  // c10.w lightmap floor it is scaled by (ucode 80/82). Both were baked in
  // as float3(2.1, 1.8, 1.5) and 0.1.
  float4 wat_spec;
  // flowingwateralpha tangent-normal scales, c15.w/z/x (ucode 27). The port
  // took these from c11.x/z/w, and c11.w is 290 -- a specular exponent, not
  // a normal scale.
  float4 wat_nrm;  // xyz = c15.w/z/x scales, w = c15.y multiplier
  // dynamicobject CSM receive biases (PS c8.xyz: finest, mid, outer
  // cascade) + enable in w (dynamicobject_defaultPS 38/41/44).
  float4 dyn_bias;   // 236
  // Deck (skateboard) contact shadow, the console's postfx_deck_AO decal
  // rebuilt around the board's world transform (see skate3_deck_shadow.h).
  // deck_x / deck_z map world space straight to the decal's uv (the scale
  // folds in the guest's c4.xy = 1.6/0.9 half-extents), deck_y measures
  // metres off the board's plane for the falloff.
  float4 deck_x;     // 240  world -> decal u  (xyz rows + w offset)
  float4 deck_y;     // 244  world -> height above the board's plane
  float4 deck_z;     // 248  world -> decal v
  float4 deck_p;     // 252  x = strength (0 = no board placed), y = 1/fade_m
  // Per-material water.default sets: 8 x 14 rows at float 256, selected by
  // misc.z. The level can show several water surfaces at once and their PCA
  // bases differ, so one shared set decoded some of them through another
  // material's basis.
  float4 wd_set[112];
  // environmentpark.* frame rows at float 704 (wd_set ends at 704), captured
  // from the park pixel shaders' own banks; see FrameScene::park_rows for the
  // per-variant register layouts they came from, and ShadePark for the two
  // shading models. Kept apart from the dyn_* rows on purpose: those are
  // refreshed only by dynamicobject_defaultPS draws and go stale in a
  // park-only view, which made the pieces flicker.
  float4 park_sun;   // 704  xyz = sun direction, w = tone exposure
  float4 park_fa;    // 708  xyz = shadow floor A (cascade term, model 0),
                     //      w = material multiplier
  float4 park_fb;    // 712  xyz = shadow floor B (model 0) or the additive
                     //      flat ambient (model 1), w = output alpha
  float4 park_kd;    // 716  xyz = kd tint, w = bounce scale (model 1)
  float4 park_lt;    // 720  GetTangentLight axes + spec power
  float4 park_bias;  // 724  xyz = per-cascade receive bias (0 = fall back to
                     //      the dynobj default), w = shading model
  // Model-0 light cookie projection (environmentpark{decal,diffuse}_defaultVS
  // c5 / c6): uv = (dot(park_lmu.xyz, wp) + park_lmu.w, dot(park_lmv.xyz, wp)
  // + park_lmv.w); the texture rides the lightmap slot (tint.r > 0 = bound).
  float4 park_lmu;   // 728
  float4 park_lmv;   // 732
  // Park world-shadow receive (environmentpark{diffuse,decal}_defaultPS tf2;
  // see ParkWorldShadow): the PS's own c5 / c7 / c8 / c6 and c10.
  float4 park_wsu;   // 736  c5: inner tile u row
  float4 park_wsv;   // 740  c7: inner tile v row
  float4 park_wsd;   // 744  c8: height ray row
  float4 park_wst;   // 748  c6: outer tile = inner * xy + zw
  float4 park_wsb;   // 752  c10.x/y biases (inner/outer), z exp scale,
                     //      w = 1 when the native map is built
  // The world's `_skatepark` pixel-shader permutations (see ParkWorldShade):
  float4 park_dfa;   // 756  xyz = cascade floor c11, w = cascade bias c9.x
  float4 park_dfb;   // 760  xyz = park-map floor c12, w = 1 when captured
  // The environmentpark.alphatest variant's own rows (same canonical layout
  // as park_sun..park_bias above, which carry the diffuse / decal set):
  // ShadePark reads the set of the piece's own variant.
  float4 park1_sun;  // 764
  float4 park1_fa;   // 768
  float4 park1_fb;   // 772
  float4 park1_kd;   // 776
  float4 park1_lt;   // 780
  float4 park1_bias; // 784
  // Per-material flowing-water rows at float 788: 6 sets x 7 rows (wat_p0,
  // wat_p1, wat_p2, wat_p3, wat_gate, wat_spec, wat_nrm, same layout),
  // selected by overlay.w on the exact water branch (fam 30). Each surface's
  // own material rows; the wat_* rows above are one material's.
  float4 fw_set[42];
  // One-pass VR stereo (float 956): left-eye clip -> right-eye clip,
  // clip_R = clip_L * st_d (row-vector; inverse(VP_left) * VP_right). Read
  // by vs_main for draws flagged world._44 = 2 (see there).
  row_major float4x4 st_d;
  float4 fp_head; // 972: local first-person head centre and exclusion radius
};
