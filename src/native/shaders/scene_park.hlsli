// environmentpark.* -- the skate-park editor pieces (ramps, rails, funboxes),
// cam_pos.w = -(20 + variant): -24 diffuse, -25 alphatest, -26 decal.
//
// These are dynamic objects that happen to move only in the park editor, and
// the game shades them as such: all three park pixel shaders do the DUAL
// shadow fetch (cascades + the baked world shade), exactly like
// dynamicobject_defaultPS and unlike every static family, which does one
// fetch and has no world term. Routed through the legacy static fallback they
// neither cast nor received shadows; routed through the DYNOBJ rows they
// flickered, because those are refreshed only by dynamicobject_defaultPS
// draws and go stale in a park-only view. Hence their own rows (park_*, from
// the park banks) and their own branch.
//
// Two shading models, selected by park_bias.w, both read off this build's
// ucode (ps_environmentpark{diffuse,alphatest,decal}_defaultPS):
//
//   model 0 (diffuse / decal) -- the shade is a pair of floor-lerps min'd
//     together, NOT a key/ambient sum:
//       A = lerp(c11, 1, cascade_shade)          (48/49)
//       B = lerp(c12, 1, world_shade * N.L_gate) (64/65)
//       shade = min(A, B)                        (67)
//     then x kd x albedo^2 x kd_tint x c18.y, fog, tone curve, sqrt out.
//   model 1 (alphatest) -- register for register the dynamicobject shape
//     (it matches alphatestdynamicobject_defaultPS): key x min(cascade,
//     world) + bounce x c18.w + flat ambient c18.xyz (92/94).
//
// kd is the same GetTangentLight fold as the dynobj branch: the fixed SIGNED
// tangent-space light dotted with the mapped normal, scaled by |Lt| / Lt.z
// (the ucode's rcp(rsq(dot(Lt,Lt)) * Lt.z); 0.93429 / 0.39 = 2.39562 for the
// stock (0.58, 0.62, 0.39) axes). With no normal map bound the fold is |Lt|.
//
// Not ported: the specular lobe the alphatest and decal variants carry (the
// diffuse variant has none). Its tint / exponent / fixed world light sit past
// the captured row window; the park pieces are matte ramp surfaces and read
// correct without it.
float4 ShadePark(VSOut i, float4 albedo) {
  // Each variant reads its OWN bank's rows: the alphatest set (park1_*)
  // or the diffuse / decal set (park_*). A single shared set took the
  // model of whichever park draw came first in the frame.
  const bool at = cam_pos.w < -24.5 && cam_pos.w > -25.5;
  if (at) {
    clip(albedo.a - 0.1176);  // environmentpark.alphatest: ALPHAREF 30
  }
  const float4 Psun = at ? park1_sun : park_sun;
  const float4 Pfa = at ? park1_fa : park_fa;
  const float4 Pfb = at ? park1_fb : park_fb;
  const float4 Pkd = at ? park1_kd : park_kd;
  const float4 Plt = at ? park1_lt : park_lt;
  const float4 Pbias = at ? park1_bias : park_bias;
  const bool additive = Pbias.w > 0.5;
  // Decal variant (ucode 21-24, 32): albedo^2 = lerp(diffuse^2, decal^2,
  // decal.a) x kd tint; the decal at the half4 texcoord's zw (uv3), clamped.
  float3 alb2 = albedo.rgb * albedo.rgb;
  if (overlay.w > 5.5 && overlay.w < 6.5) {
    const float4 dk = decal_art.Sample(smp_clamp, i.uv3);
    alb2 = lerp(alb2, dk.rgb * dk.rgb, dk.a);
  }
  float3 dlin = alb2 * Pkd.rgb;
  // environmentpark.alphatest's stored frame (see DecodeMesh): T in tanb
  // (tangent-stored sentinel) and B raw in uv3.xy + uv2.x (uv2.y = 16). The
  // game uses the pair raw, with N = cross(T, B) UNnormalized
  // (environmentparkalphatest_defaultVS 16-28): these frames are far from
  // orthogonal, so neither rebuilding T from B nor normalizing N first
  // reproduces its per-leaf lighting, and N is never swapped for the
  // camera-facing face normal (which made the leaves' shading change with
  // the viewpoint and distance).
  const bool stored_tb = at && i.uv2.y > 8.0 && i.tanb.w > 0.1 && i.tanb.w < 0.33;
  float3 sT = float3(1.0, 0.0, 0.0), sB = float3(0.0, 1.0, 0.0);
  if (stored_tb) {
    sT = i.tanb.xyz;
    sB = mul(float3(i.uv3.x, i.uv3.y, i.uv2.x), (float3x3)world);
  }
  float3 n = (stored_tb || dot(i.nrm, i.nrm) > 0.01)
                 ? normalize(i.nrm)
                 : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  uint v2f = (uint)(misc.z + 0.5);
  // Tangent axes for the kd sign dots and the TBN mapping: the stored
  // per-vertex frame when the mesh carries one, a screen-space UV-gradient
  // frame otherwise (identical to the dynobj branch).
  float3 kt = float3(1.0, 0.0, 0.0), kb = float3(0.0, 1.0, 0.0);
  if (v2f != 0u) {
    float3 tt_s, bb_s;
    ScreenTangentFrame(n, i.rpos, i.uv, tt_s, bb_s);
    KdAxes(i.tanb, n, tt_s, bb_s, kt, kb);
    if (stored_tb) {
      kt = sT;  // alphatest: the stored pair itself (see above)
      kb = sB;
    }
  }
  float3 Lt = float3(Plt.x * sign(dot(kt, Psun.xyz)),
                     Plt.y * sign(dot(kb, Psun.xyz)), Plt.z);
  float ktn = length(Plt.xyz) / max(abs(Plt.z), 1e-6);
  float3 wn = n;
  float kd = length(Plt.xyz);  // flat-normal fold
  if ((v2f & 1u) != 0u) {
    float3 nmv = normal_map.Sample(smp, i.uv).rgb;
    float2 dxy = (v2f & 2u) != 0u
                     ? detail_map.Sample(smp, i.uv * misc.w).rg
                     : float2(0.5, 0.5);
    float3 vndn =
        normalize(float3(nmv.xy * 2.0 + dxy * 2.0 - 2.0, nmv.z * 2.0 - 1.0));
    // The game maps with its raw interpolated N; the alphatest frame keeps
    // that (i.nrm is the unnormalized cross(T, B)).
    wn = normalize(vndn.x * kt + vndn.y * kb + vndn.z * (stored_tb ? i.nrm : n));
    kd = dot(vndn, Lt) * ktn;
  }
  float ndl = dot(wn, Psun.xyz);
  float gate = ndl >= 0.0 ? 1.0 : 0.0;
  // Cascade receive bias (model 1, the dynamicobject-shaped alphatest bank's
  // per-cascade c8.xyz): the bank's own, falling back to the dynobj
  // defaults when it captured as zero. (The fallback was introduced when
  // the park pieces were wrongly cast into the cascades -- those ~440
  // "park shadow draws" are the DMO atlas passes -- and a zero bias then
  // self-shadowed their flat tops. Model 0 now uses its c9.x as captured.)
  const float4 pcb = Pbias.x > 0.0 ? float4(Pbias.xyz, 1.0)
                                       : float4(0.007, 0.015, 0.015, 1.0);
  float csm = SampleCsmShadowVehicle(i.rpos + cam_pos.xyz, i.pos.xy, pcb);
  float3 lighting;
  if (additive) {
    // environmentparkalphatest_defaultPS world term (ucode 55-76): the
    // game's world-shade map (tf1, the native re-render bound at t10 for
    // these draws), four point taps with an sge compare -- exactly the
    // dynamicobject model, rows c5 / c6 / c7. Lit until the map is built.
    float world = dyn_misc.z > 0.5 ? SampleWorldShadeT10(i.rpos + cam_pos.xyz) : 1.0;
    float shadow = min(csm, world) * gate;
    float bounce = saturate(dot(wn, float3(-Psun.x, Psun.y,
                                           -Psun.z)));
    lighting = saturate(ndl) * shadow + bounce * Pkd.w + Pfb.rgb;
  } else {
    // Model 0 (ucode decal 43-50 / 84-90): the cascade term is coverage
    // aware (SampleCsmParkCover). The world term is the game's skate.Park
    // world-shadow map (tf2), natively rebuilt from the game's own two
    // passes and compared exponentially exactly like the ucode (see
    // ParkWorldShadow). It used to reuse the cascade sample, which only
    // covers the few metres around the skater and has none of the pass's
    // reach or casters: park floors under a structure went uniformly dark.
    // Cascade bias = the bank's own c9.x (0 in capture 1790871823): the
    // cascades hold only the characters here, so nothing self-shadows.
    csm = SampleCsmParkCover(i.rpos + cam_pos.xyz, i.pos.xy, Pbias.x);
    float world = ParkWorldShadow(i.rpos + cam_pos.xyz);
    float3 one = float3(1.0, 1.0, 1.0);
    lighting = min(lerp(Pfa.rgb, one, csm),
                   lerp(Pfb.rgb, one, world * gate));
    // The light cookie (ucode decal 63/90, diffuse 46/67): the park's baked
    // indoor light and window sun shafts, projected from the world position.
    if (tint.r > 0.0) {
      const float3 wp = i.rpos + cam_pos.xyz;
      const float2 luv = float2(dot(park_lmu.xyz, wp) + park_lmu.w,
                                dot(park_lmv.xyz, wp) + park_lmv.w);
      lighting = min(lighting, lightmap.SampleLevel(smp_clamp, luv, 0.0).rgb);
    }
  }
  // The editor's preview object (cam_pos.w = -(20 + variant + 0.25); see
  // DrawItem::foreground): environmentparkdecal_foregroundPS lights it with
  // the kd fold alone -- body = kd * ktn * albedo^2 * tint (ucode 29-49):
  // no N.L, no cascade / park-map terms, no floor lerps, no light cookie.
  if (frac(-cam_pos.w) > 0.1) {
    lighting = float3(1.0, 1.0, 1.0);
  }
  float3 lin = lighting * kd * dlin;
  // Fog -> exposure -> tonemap -> sqrt, then the 1.41 uber scene multiplier.
  float fdist = length(i.rpos);
  float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
  if (sh_fogp.z != 1.0) {
    f1 = pow(max(f1, 1e-6), sh_fogp.z);
  }
  float3 fog_rgb = sh_fogc.rgb * f1;
  float fog_a = (1.0 + sh_fogc.a * f1) * Pfa.w;  // x material multiplier
  float3 xe = (lin * fog_a + fog_rgb) * Psun.w;
  float4 outc = ToneOut(xe, Pfb.w, false);
  // The selected piece's overlap highlight (mat_tint.w = 2, rgb = c24; see
  // DrawItem::highlight): environmentparkdecal_highlightPS adds c24 AFTER
  // the sqrt encode (ucode 110), i.e. to the pixel shader's own output --
  // what the classic scene target holds. Under HDR the target holds the
  // tone chain's input instead: rebuild the game's output value, add, and
  // re-encode through PassGamma (its exact inverse, x1.41 composite folded).
  if (mat_tint.w > 1.5) {
#ifdef HDR
    const float3 t1 = saturate(1.0 - xe);
    const float3 tm = max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
    const float3 o = saturate(sqrt(abs(tm * 0.5)) + mat_tint.rgb);
    outc.rgb = PassGamma(o * 1.41);
#else
    outc.rgb = saturate(outc.rgb + mat_tint.rgb);
#endif
  }
  return outc;
}
