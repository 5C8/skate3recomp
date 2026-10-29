// Water families, hand-ported from the game's own pixel shaders and
// verified per-pixel against the ucode: flowingwater[alpha] (fam 30),
// the ocean surface (fam 31) and the baked ocean horizon sheet
// (fam 32). Callers pass the shared world-family locals (squared
// diffuse, fog terms, scene exposure).
// flowingwateralpha (fam 30): the canal/waterfall water shader,
// hand-ported from flowingwateralpha_defaultPS and verified per-pixel
// against the ucode (dual time-scrolled ripple-normal taps -> TBN ->
// lightmap-lit body + phong sun spec + cube reflection, then the
// standard fog/exposure/tonemap chain; alpha = the spec term's green
// channel, floored; reflection strength drives opacity over the bed).
float4 ShadeFlowingWater(VSOut i, float3 dlin, float fog_a,
                         float3 fog_rgb, float expo) {
  // overlay.w: & 15 = the item's own material set in fw_set (0 = the frame
  // rows wat_*), & 16 = flowingwater_defaultPS (water.flowing, alpha 1).
  const uint fsel = uint(overlay.w + 0.5);
  const uint fset = fsel & 15u;
  float4 fp1 = wat_p1, fp2 = wat_p2, fp3 = wat_p3;
  float4 fgate = wat_gate, fspec = wat_spec, fnrm = wat_nrm;
  if (fset > 0u) {
    const uint fb = (fset - 1u) * 7u;
    fp1 = fw_set[fb + 1];
    fp2 = fw_set[fb + 2];
    fp3 = fw_set[fb + 3];
    fgate = fw_set[fb + 4];
    fspec = fw_set[fb + 5];
    fnrm = fw_set[fb + 6];
  }
  float t = fp3.x;
  float3 n1 = macro.Sample(smp, i.uv * fp2.xy + fp1.xy * t).rgb;
  float3 n2 = macro.Sample(smp, i.uv * fp2.zw + fp1.zw * t).rgb;
  float3 nsum = 2.0 * n1 + 2.0 * n2 - 2.0;
  // ucode 27: mul r9.xyz, r6.wzyy, c15.wzxx -- the tangent-normal scales
  // are c15.w/z/x (wat_nrm.xyz), NOT c11.x/z/w as this had. c11.w is 290,
  // a specular exponent, which pinned the normal to near-flat.
  float3 vn = normalize(float3(nsum.x * fnrm.x, nsum.y * fnrm.y,
                               nsum.z * fnrm.z));
  float3 wn0 = dot(i.nrm, i.nrm) > 0.01
                   ? normalize(i.nrm)
                   : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  // Stored frame: tanb = the mesh's usage-6 TANGENT here (see the water
  // pack in DecodeMesh), sentinel sign = the unwrap sign.x;
  // B = sign.x * cross(N, T), exactly the game's VS.
  float3 wt, wb;
  if (i.tanb.w > 0.1) {
    wt = normalize(i.tanb.xyz);
    wb = cross(wn0, wt) * (i.tanb.w > 0.6 ? 1.0 : -1.0);
  } else {
    ScreenTangentFrame(wn0, i.rpos, i.uv, wt, wb);
  }
  float3 wn = normalize(vn.x * wt + vn.y * wb + vn.z * wn0);
  float3 vd = -normalize(i.rpos);
  float s = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, wn, i.pos.xy);
  // Lightmap at the unwrap + 0.01 * vNormal.xz (the ripple refraction
  // wobble), console fetch semantics, min-clamped like every world
  // family. White-fallback pages skip the clamp (see the env fetch).
  float2 wlm_uv = i.uv2 + 0.01 * vn.xz;
  float3 wlm = lightmap.SampleLevel(smp_clamp, wlm_uv, 0.0).rgb;
  float3 lml = tint.r > 0.0 ? min(wlm * wlm, s + sh_color.rgb)
                            : wlm * wlm;
  // GetTangentLight on the WORLD normal: fixed axis (0.58, 0.62, 0.39)
  // with xy signs following the sun direction, x 1/normalize(axis).z.
  float kd = (wn.x * 0.58 * sign(sh_sun.x) +
              wn.y * 0.62 * sign(sh_sun.y) + wn.z * 0.39) * 2.39562;
  float3 lin = lml * kd * dlin;
  // Spec/reflection masks (t4): x = phong mask, z = reflection mask,
  // thresholded by m_params[3].y. Zero until resolved: matte water.
  float2 wmask = float2(0.0, 0.0);
  if (overlay.x > 0.0) {
    float4 wm4 = decal_art.Sample(smp, i.uv);
    wmask = saturate(float2(wm4.x, wm4.z) - fp3.y);
  }
  // Phong vs the real sun, power m_params[3].z, tint (2.1, 1.8, 1.5),
  // scaled by sat(clamped lightmap green - 0.1).
  float3 wrl = 2.0 * wn * dot(wn, sh_sun.xyz) - sh_sun.xyz;
  float ks = pow(max(saturate(dot(vd, wrl)), 1e-6), fp3.z);
  // Tint from c12 and the lightmap floor from c10.w (ucode 64/80/81/82),
  // rather than the baked float3(2.1, 1.8, 1.5) and 0.1 -- those are this
  // one material's numbers.
  float3 wspec =
      ks * wmask.x * fspec.rgb * saturate(lml.g - fspec.w);
  // Cube reflection, same shape as fams 5/6: reflect(E, wN) with xy
  // negated, luminosity lerped toward 1 by 0.3 * sat(4*mask - 2.6),
  // x mask x 1.5. misc.y = the cube LOD bias (640p-gradient parity).
  float3 wrv = vd - 2.0 * wn * dot(vd, wn);
  float3 wcube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-wrv.x, -wrv.y, wrv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  // Reflection strength (flowingwateralpha_defaultPS 81-86). The gate is
  // scaled by (1 - lml) BEFORE the add, via the subsc co-issued at 81:
  //   81: mul r7.x, r2.wwww, c12.xxxx + subsc r0._y__, c253.z, r1.x
  //   82: mul r0, r7, r0.xxxy          -> r0.w = gate * (1 - lml)
  //   83: add r1.x, r0.wwww, r1.xxxx   -> + lml
  //   84: mul r1.xyz, r1.xxxx, r3.xyzz -> cube * that
  //   85: mul r1.xyz, r1.xzyy, r4.xxxx -> * the mask term
  //   86: mad r0.xy_w, r1.xyzz, c13.wwww, r0.xyzz
  // gate*(1 - lml) + lml IS lerp(lml, 1, gate): the lightmap modulates the
  // reflection but the term stays capped at 1. Reading the `add` as a plain
  // sum (and missing the co-issued subsc) overshot it by gate*lml.
  float wrlum = fgate.x * saturate(fgate.y * (wmask.y - fgate.z));
  // Strength from c13.w (ucode 86), not a baked 1.5.
  float3 wrefl = wcube * lerp(lml.g, 1.0, wrlum) * wmask.y * fgate.w;
  float3 wspec_term = wspec + wrefl;
  lin += wspec_term;
  // ucode 87/88: the multiplier is c15.y (0.2 here), NOT c11.y (0.62) --
  // the port was running this surface about 3x bright.
  float3 wxe = (lin * (fog_a * fnrm.w) + fog_rgb) * expo;
  // flowingwater_defaultPS 95: max oC0._001 -> alpha 1 (opaque);
  // flowingwateralpha_defaultPS 97: max oC0.w, r0.w, c18.w (floored).
  return ToneOut(wxe, (fsel & 16u) != 0u ? 1.0 : max(wspec_term.g, fp3.w), false);

}

// Exact ocean surface (fam 31, ocean_defaultPS; verified per-pixel
// against the ucode): PCA-compressed dual-component normal map, lerped
// toward flat by the overlay-scaled tonedown, cube reflection x squared
// lightmap x fresnel, ward anisotropic sun spec. OPAQUE (the game's PS
// outputs alpha 0 and draws without blending).
float4 ShadeOcean(VSOut i, float fog_a, float3 fog_rgb, float expo) {
  float2 suv = i.uv * oc_p1.w;
  float4 c0v = macro.Sample(smp, suv) * 2.0 - 1.0;       // t3: comp 0
  float4 c1v = detail_map.Sample(smp, suv) * 2.0 - 1.0;  // t8: comp 1
  // PCA normal in the shader's Z-UP basis; mean/weights are stored in
  // 0..1 texture space and folded 2x-1 after the sum.
  float3 pca = (float3(dot(c0v, oc_w0) + dot(c1v, oc_w1),
                       dot(c0v, oc_w2) + dot(c1v, oc_w3),
                       dot(c0v, oc_w4) + dot(c1v, oc_w5)) +
                oc_mean.xyz) * 2.0 - 1.0;
  // tonedown = 2 * m_params[1].z * sat(dot(c1v, wB1) + overlay.x);
  // the ucode folds the second PCA blue dot into the overlay term.
  float ovx = overlay.z > 0.0
                  ? spec2_map.Sample(smp, i.uv * overlay.w).x
                  : 1.0;
  float otd = (2.0 * oc_p1.z) * saturate(dot(c1v, oc_w5) + ovx);
  // ucode 34-40: rsq of an ABSOLUTE length, then max(|n|, eps) and a second
  // rsq. A plain normalize() of a near-zero lerp is NaN (a black pixel).
  float3 ntl = lerp(float3(0.0, 0.0, 1.0), pca, otd);
  float3 nt = ntl * rsqrt(max(dot(ntl, ntl), 1e-12));
  nt = max(abs(nt), 0.001);
  nt *= rsqrt(max(dot(nt, nt), 1e-12));
  float3 N = float3(nt.x, nt.z, nt.y);  // shader Z-up -> world Y-up
  float3 E = -normalize(i.rpos);
  // Cube reflection: reflect(E, N) with xy negated, x the SQUARED
  // single-tap lightmap (no chromaticity, no CSM clamp in this PS).
  float3 orv = E - 2.0 * N * dot(E, N);
  float3 ocube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-orv.x, -orv.y, orv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  float2 olm_uv = i.uv2 + 0.01 * nt.xy;
  float3 olm = lightmap.SampleLevel(smp_clamp, olm_uv, 0.0).rgb;
  float3 orefl = ocube * (olm * olm);
  float NV = abs(dot(N, E));
  // Fresnel exactly as ucode 56/65/71/74 build it: |1 - |N.E|| multiplied
  // out to the 5th power. pow(1 - NV, 5) is exp(5 * log(x)), and with N and
  // E both unit length NV can round to 1.0000001 wherever a ripple faces the
  // camera -- a negative base, NaN, and a BLACK pixel. Those clustered in
  // groups across the ocean at steep view angles.
  float omv = abs(1.0 - NV);
  float omv2 = omv * omv;
  float F = 0.25 + 0.75 * (omv2 * omv2 * omv);
  // Ward anisotropic sun spec (spreads oc_p2.xy, roughness oc_p2.z).
  float3 oudv = float3(-N.z, 0.0, N.x);
  float3 oud = oudv * rsqrt(max(dot(oudv, oudv), 1e-12));
  float3 ovd = cross(N, oud);
  float dNE = dot(N, E);
  float dNL = dot(N, sh_sun.xyz);
  float oax = oc_p2.z / oc_p2.x;
  float oay = oc_p2.z / oc_p2.y;
  float3 Hs = sh_sun.xyz + E;
  float3 H = Hs * rsqrt(max(dot(Hs, Hs), 1e-12));
  float eu = dot(H, oud) / oax;
  float ev = dot(H, ovd) / oay;
  float oden = sqrt(abs(dNL * dNE)) * 12.566371 * oax * oay;
  float ward = exp(-2.0 * (eu * eu + ev * ev) / max(1.0 + dot(H, N), 1e-6)) /
               max(oden, 1e-6);
  float3 ocol = orefl * F + ward * oc_p0.rgb;
  float3 oxe = (ocol * (fog_a * oc_p1.y) + fog_rgb) * expo;
  return ToneOut(oxe, 1.0, false);

}

// Baked ocean horizon sheet (fam 32, oceanreflection_defaultPS;
// verified per-pixel): squared sea texture x multiplier, fogged and
// tonemapped; alpha ramps with world height between the orf.z/orf.y
// bounds (the bright haze band fading in toward the horizon).
float4 ShadeOceanReflection(VSOut i, float3 dlin, float fog_a,
                            float3 fog_rgb, float expo) {
  float3 rcol = dlin * orf.x;
  float ra = saturate((i.rpos.y + cam_pos.y - orf.z) /
                      max(orf.y - orf.z, 1e-4));
  float3 rxe = (rcol * fog_a + fog_rgb) * expo;
  return ToneOut(rxe, ra, false);

}

// water.default (fam 33, water_defaultPS) -- ported from THIS build's own
// ucode (data/render/shaders/final/water_defaultPS.fpo). Not a variant of
// flowingwateralpha: it decodes PCA dual-component normals like the ocean,
// from TWO maps sampled at TWO time-scrolled uv sets:
//
//   mul r4, c24.zwxy, c25.xxxx      uv scale  = c24 * c25.x
//   mul r3, c23, c26.xxxx           scroll    = c23 * time(c26.x)
//   mad r6, r4.zwxy, r0.xyxy, r3    uv1 = uv*scale.xy + scroll.xy
//                                   uv2 = uv*scale.zw + scroll.zw
//   tfetch tf6/tf7 at uv1 and uv2   four taps
//   mad r7, r*.wzyx, 2, -1          unpack, swizzled wzyx
//   dp4 ... c9..c14                 PCA weight rows -> normal components
//   mul r4, r4, c22.zwzw            per-tap strengths
//   cube + tfetchCube tf8           environment reflection
//   mad r1.xyz, r1.xyz, c20.wwww    reflection strength
//
// The guest also refracts the framebuffer (tf3, four offset taps) and reads
// a depth term (tf0); neither is bound in our transparent pass, so the body
// colour comes from the lightmap-lit diffuse as in the other water families.
float4 ShadeWaterDefault(VSOut i, float3 dlin, float fog_a, float3 fog_rgb,
                         float expo) {
  // misc.z selects THIS surface's captured constant set. The rows are per
  // MATERIAL, not per frame: a level can hold several water surfaces with
  // different PCA bases, and decoding one through another's basis tips the
  // normal far off vertical and scatters the reflection (the speckle that
  // tracked the view angle -- it changed which draw was captured first).
  // overlay.w packs the material set index (0..7) and, +8, the alpha
  // variant. misc.zw carry the fog ramp for water, so they cannot hold it.
  int wsel = int(overlay.w + 0.5);
  bool w_alpha = wsel >= 8;
  int wb = clamp(wsel & 7, 0, 7) * 14;
  float4 wmean  = wd_set[wb + 0];
  float4 ww0    = wd_set[wb + 1];
  float4 ww1    = wd_set[wb + 2];
  float4 ww2    = wd_set[wb + 3];
  float4 ww3    = wd_set[wb + 4];
  float4 ww4    = wd_set[wb + 5];
  float4 ww5    = wd_set[wb + 6];
  float4 wstr   = wd_set[wb + 7];
  float4 wspeed = wd_set[wb + 8];
  float4 wscale = wd_set[wb + 9];
  float4 wmisc  = wd_set[wb + 10];
  float4 wtime  = wd_set[wb + 11];
  float4 wrefl  = wd_set[wb + 12];
  float4 wfres  = wd_set[wb + 13];
  float t = wtime.x;
  float4 sc = wscale * wmisc.x;
  float2 uv1 = i.uv * sc.xy + wspeed.xy * t;
  float2 uv2 = i.uv * sc.zw + wspeed.zw * t;
  // Normal decode, transcribed from the ucode. Every dp4 reduces to
  // dot(2*sample - 1, cN): the taps carry .wzyx from the unpack mad and the
  // constants carry .yxzw / .wzxy, and the two reversals cancel.
  //
  // Per uv set, with P6/P7 the unpacked taps of the two maps:
  //   sx = dot(P6,c9)  + dot(P7,c10) + c8.x        (lines 28-33, 41)
  //   sz = dot(P6,c13) + dot(P7,c14) + c8.z        (lines 30-35, 41)
  //   sy = dot(P6,c11) + dot(P7,c12) + c8.y - 1    (lines 22-27, 38-46)
  //   N  = normalize(k*(2sx-1), k*(2sz-1), 2k*sy + 1)
  // with k = c22.w for the scrolling set and c22.z for the static one.
  //
  // Adding c8 as a uniform "PCA mean" to all three components (as this did)
  // swamps the dot products and leaves the normal constant -- the flat white
  // surface the debug view showed.
  float4 p6a = macro.Sample(smp, uv1) * 2.0 - 1.0;
  float4 p7a = overlay.x > 0.0 ? detail_map.Sample(smp, uv1) * 2.0 - 1.0
                               : float4(0.0, 0.0, 0.0, 0.0);
  float4 p6b = macro.Sample(smp, uv2) * 2.0 - 1.0;
  float4 p7b = overlay.x > 0.0 ? detail_map.Sample(smp, uv2) * 2.0 - 1.0
                               : float4(0.0, 0.0, 0.0, 0.0);
  float3 nt;
  float3 nt_mask;
  {
    float kx = wstr.w;  // c22.w, the scrolling set
    float sx = dot(p6a, ww0) + dot(p7a, ww1) + wmean.x;
    float sz = dot(p6a, ww4) + dot(p7a, ww5) + wmean.z;
    float sy = dot(p6a, ww2) + dot(p7a, ww3) + wmean.y - 1.0;
    float3 n1 = float3(kx * (2.0 * sx - 1.0), kx * (2.0 * sz - 1.0),
                       2.0 * kx * sy + 1.0);
    float kz = wstr.z;  // c22.z, the static set
    float bx = dot(p6b, ww0) + dot(p7b, ww1) + wmean.x;
    float bz = dot(p6b, ww4) + dot(p7b, ww5) + wmean.z;
    float by = dot(p6b, ww2) + dot(p7b, ww3) + wmean.y - 1.0;
    float3 n2 = float3(kz * (2.0 * bx - 1.0), kz * (2.0 * bz - 1.0),
                       2.0 * kz * by + 1.0);
    // The two sets are NOT averaged. c24 = (0.02, 0.02, 1, 1) makes set 1
    // a hugely magnified slow swell (uv * 0.02, scrolling with c23) and set
    // 2 the fine 1:1 detail (uv * 1.0, static). The ucode uses set 2 as the
    // SHADING normal (lines 70-76 build it and dp3 it against the view) and
    // set 1 only to offset the mask fetch (line 55). Averaging them let the
    // magnified swell dominate -- the oversized ripple.
    // Verified against the ucode AND by experiment: the STATIC tap set
    // (c23.zw = 0) drives the shading normal and the scrolling set offsets
    // the mask fetch. Swapping them breaks the surface completely.
    // NaN-safe: normalize() is v * rsqrt(dot(v,v)), which is inf/NaN for a
    // zero-length vector. n.z here is 2*k*s + 1, so a pixel where k*s is
    // near -0.5 with small x/y produces exactly that -- and a NaN reaches
    // the target as a BLACK pixel. Scattered black pixels through moving
    // water, thickening into a network wherever the ripple stalls, is the
    // signature. ShadeOcean guards the same construction for this reason.
    nt = n2 * rsqrt(max(dot(n2, n2), 1e-12));
    nt_mask = n1 * rsqrt(max(dot(n1, n1), 1e-12));
  }
  nt = nt * rsqrt(max(dot(nt, nt), 1e-12));
  // No tangent transform: the guest PS dots this normal straight against
  // c7.zyxx - r1.zyxx (camera minus world position), a WORLD-space view
  // vector, and water_defaultVS emits no tangent frame at all -- only world
  // position and fog. So the decode is already world-space for this flat
  // plane, in the shader's Z-up basis. Swap to the engine's Y-up exactly as
  // the verified ocean port does. Running it through the mesh TBN (as this
  // did) tilts the ripples and changes how they read.
  float3 wn_u = float3(nt.x, nt.z, nt.y);
  float3 wn = wn_u * rsqrt(max(dot(wn_u, wn_u), 1e-12));
  float3 E = -normalize(i.rpos);
  float s = SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, wn, i.pos.xy);
  // Lightmap body, wobbled by the ripple like the other water families.
  float2 lm_uv = i.uv2 + 0.01 * nt.xy;
  float3 lm = lightmap.SampleLevel(smp_clamp, lm_uv, 0.0).rgb;
  float3 lml = tint.r > 0.0 ? min(lm * lm, s + sh_color.rgb) : lm * lm;
  // Environment cube (overlay.y = a real cube resolved), strength c20.w.
  float3 rv = E - 2.0 * wn * dot(E, wn);
  float3 cube = overlay.y > 0.0
                    ? env_cube.SampleBias(smp, float3(-rv.x, -rv.y, rv.z),
                                          misc.y).rgb
                    : fog_rgb;
  float NV = saturate(dot(wn, E));
  // Schlick with water's real F0. c21.z is NOT a fresnel bias -- it reads 4.0
  // and the ucode uses it as a gain inside a mulsc_sat on a mask, so feeding
  // it here pinned F at 1.0 and the reflection alone (x c20.w = 1.5) blew the
  // surface out to white.
  float F = 0.02 + 0.98 * pow(1.0 - NV, 5.0);
  // NOT dlin: water.default carries no usable diffuse channel (same as
  // ocean.default), so the white fallback texture blew the surface out to a
  // near-white plane. In the guest shader the base colour is the REFRACTED
  // framebuffer (tf3, four offset taps), which is not bound in our
  // transparent pass -- the environment cube is the closest stand-in, since
  // what this surface refracts and what it reflects are both mostly sky.
  // The water's colour is AUTHORED: this material's diffuse is a solid
  // water blue (confirmed by rendering it unlit). So the body is the
  // diffuse lit by the lightmap, exactly like the other world families --
  // not something synthesised from the sky, which is what washed it out.
  //
  // The environment cube is HDR (sky well above 1.0), so the reflection
  // uses its COLOUR at an explicit strength; scaling it directly saturated
  // the whole surface. The rippled normals make F vary across the surface,
  // which is what produces the streaky highlights over the blue.
  float3 cubec = cube / max(dot(cube, float3(0.2126, 0.7152, 0.0722)), 1e-3);
  // Body = the authored water diffuse lit by the LIGHTMAP.
  //
  // tf3 is the lightmap, not a refraction or a colour map: the ucode averages
  // its four half-texel taps and then SQUARES the result
  // (mul r0._yzw, r0.xxyz, r0.xxyz), which is this engine's lightmap
  // convention -- and rendering the bound texture confirmed a lightmap atlas.
  // We already sample that map as `lightmap` (lml = lm * lm above), so no
  // extra binding is needed; the earlier attempts to treat sampler 3 as a
  // framebuffer refraction, and then as a colour map, were both wrong.
  float3 body = dlin * lml;
  // Reflection / specular gates, transcribed from the ucode now that the
  // game's tf5 mask is bound (overlay.z > 1.5 = it resolved):
  //   tfetch2D r3.x_z_, r0.yx, tf5        mask fetched at the SWAPPED uv,
  //                                       offset by the ripple (c253.y=0.02)
  //   r7.w = sat(mask.x - c25.y)          -> gates the sun specular
  //   r3.x = sat(mask.z - c25.y)          -> feeds the reflection gate
  //   r0.x = r3.x - c21.w                 (0.65)
  //   r4.w = c21.y * sat(c21.z * r0.x)    (0.3 * sat(4 * r0.x))
  //   out += cube * r4.w * c20.w          (1.5)
  // The reflection therefore only fires where mask.z clears ~0.76, which is
  // what makes the reference's foam patchy instead of a broad sheen.
  float spec_gate = 1.0;
  float refl_gate = 1.0;
  float mask_z = 1.0;  // r3.x: the mask term, a SEPARATE factor below
  if (overlay.z > 1.5) {
    // The ucode's two swaps CANCEL: line 55 writes the offset through
    // r0.yxxx (so x gets the old y) and line 56/57 then fetch at r0.yx.
    // Net: uv + n.xy * 0.02, unswapped. Sampling at i.uv.yx (as this did)
    // transposes the mask along the diagonal, which changes how the ripple
    // motion reads.
    float2 muv = i.uv + nt_mask.xy * 0.02;
    float4 m = normal_map.Sample(smp, muv);
    mask_z = saturate(m.z - wmisc.y);
    spec_gate = saturate(m.x - wmisc.y);
    refl_gate = saturate(wfres.z * (mask_z - wfres.w));
  } else {
    // No mask this draw: keep the previous fresnel stand-in.
    refl_gate = saturate(wfres.z * F + 0.5);
  }
  // Reflection strength, straight off the ucode (water_defaultPS 118-121):
  //   r4.w = c21.y * sat(c21.z * (r3.x - c21.w))      the fresnel gate
  //   116: ... + subsc r3.__z_, c255.z, r1.x       -> r3.z = 1 - lml
  //   117: mul r4, r4, r3.yyyz                     -> gate * (1 - lml)
  //   118: add r1.x, r4.wwww, r1.xxxx              -> + lml
  //   119: mul r1.xyz, r1.xxxx, r6.xyzz               cube * that
  //   120: mul r1.xyz, r1.xzyy, r3.xxxx               * mask_z
  //   121: mad r1.xyz, r1.xyzz, c20.wwww, r4.xyzz     * c20.w, + sun
  // The LIGHTMAP therefore modulates the reflection, not just the body --
  // which is how the baked shading reads on the surface at all, since the
  // reflection is what dominates on water. Omitting it also made the
  // reflection collapse to nothing whenever the gate closed.
  // gate*(1 - lml) + lml IS lerp(lml, 1, gate) -- see 116/117 above, where
  // the co-issued `subsc r3.__z_, c255.z, r1.x` supplies the (1 - lml).
  // A plain sum here overshoots by gate*lml.
  float refl_w = lerp(lml.g, 1.0, refl_gate * max(wfres.y, 0.0)) * mask_z *
                 max(wrefl.w, 0.0);
  float3 refl = cube * refl_w;
  // Sun specular, power c25.z, gated by the shadow term.
  float3 hv = normalize(sh_sun.xyz + E);
  float spec = pow(saturate(dot(wn, hv)), max(wmisc.z, 1.0));
  float3 sun = spec * s * spec_gate * max(wfres.y, 0.0) *
               float3(2.1, 1.8, 1.5);
  float3 lin = body + refl + sun;
  // Never let the surface exceed the authored body by more than a little:
  // this water is a painted blue, not a mirror.

  // Alpha depends on WHICH of the two shaders this port is standing in for
  // (overlay.w = 1 marks the alpha variant):
  //   water_defaultPS      ends `max oC0._001, r0, r0`  -> w = 1, opaque.
  //     The pool floor does not read through it; c21.w (0.65) is not the
  //     alpha it looked like, and using it let a third of the bright
  //     concrete bleed through and washed the blue toward grey.
  //   wateralpha_defaultPS ends `max oC0.___w, r0.wwww, c25.wwww`.
  //     r0.w is the same fresnel/gate term this port calls
  //     refl_gate * wfres.y (its `add r1.x, r0.wwww, r1.xxxx` mirrors
  //     water_default's `add r1.x, r4.wwww, r1.xxxx`), and c25 is wd_misc,
  //     so the floor is wmisc.w.
  float a = w_alpha
                ? saturate(max(refl_gate * max(wfres.y, 0.0), wmisc.w))
                : 1.0;
  float3 xe = (lin * fog_a + fog_rgb) * expo;
  return ToneOut(xe, a, false);
}
