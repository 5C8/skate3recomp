// Character families: the game's own lighting in LINEAR space (diffuse is
// gamma -> square it), then the exact tone chain from the disassembly and
// the postfx uber's 1.41 scene multiplier (which the empirical world
// shading already folds into its constants; without it characters sit
// ~30% darker than their surroundings, measured on an F11 A/B pair).
// Fam 1/2 (defaultcharacter / CAC) lighting inputs shared by the main pass
// and the CAS skin unwrap pass (cac_face / cacstamp_skin _unwrapPS carry
// the same rows, shifted, with identical values): the normal-mapped normal,
// the receive shadow (CSM gated on N.L >= 0, min the static map), and
// amb_rim = c19.w * sat(SH) + rim (the terms that do not scale with the
// key shadow). See ShadeCharacter for the per-term provenance.
struct CharLightTerms {
  float3 vn;
  float vndl_u;
  float csm;
  float3 amb_rim;
  float fb;
  float rfres;
  float3 rd;
};
CharLightTerms CharLight(VSOut i, float3 cn, float3 vd) {
  CharLightTerms t;
  float3 vn = cn;
  if (overlay.z > 0.5) {
    float2 nm = macro.SampleBias(smp, i.uv, misc.y).ag * 2.0 - 1.0;
    float3 tt, bb;
    ScreenTangentFrame(cn, i.rpos, i.uv, tt, bb);
    vn = normalize(nm.x * tt + nm.y * bb +
                   cn * sqrt(saturate(1.0 - dot(nm, nm))));
  }
  t.vn = vn;
  t.vndl_u = dot(vn, ch_light.xyz);
  t.csm = min(SampleCsmShadowChar(i.rpos + cam_pos.xyz, i.pos.xy),
              CharStaticShade(i.rpos + cam_pos.xyz, cn, i.pos.xy));
  float3 irr = saturate(
      ch_sh[0].rgb + vn.x * ch_sh[1].rgb + vn.y * ch_sh[2].rgb +
      vn.z * ch_sh[3].rgb + (vn.x * vn.z) * ch_sh[4].rgb +
      (vn.z * vn.y) * ch_sh[5].rgb + (vn.y * vn.x) * ch_sh[6].rgb +
      (vn.z * vn.z) * ch_sh[7].rgb +
      (vn.x * vn.x - vn.y * vn.y) * ch_sh[8].rgb);
  t.amb_rim = irr * ch_amb.w;
  t.fb = 1.0 - saturate(dot(vn, vd));
  t.rfres = pow(max(t.fb, 1e-6), ch_misc.w);
  t.rd = normalize(ch_light.xyz * float3(-1.0, 0.2, -1.0) - vd);
  if (ch_ks.w > 0.0) {
    t.amb_rim += saturate(t.rfres * saturate(dot(vn, t.rd))) * ch_rim.rgb;
  }
  return t;
}

// Hall of Meat x-ray (character fam 8): defaulthom_defaultPS, transcribed
// from its ucode (literal pool as every tone-tail PS: c255 = (0.75, 0.5, 1,
// 0.25)). Constants as captured off the game's draw: mat_tint = c2 (tint rgb,
// fresnel power), overlay.x = c7.x, overlay.y = c7.y, overlay.z = c1.y.
//    3/6: T = tf3(uv)^2
//  4-9:   ndv = sat(V . N), V = normalize(c0 - pos); F = |1 - ndv|^c2.w
//  10-11: base = max(T, c7.y) * c2.rgb
//  12-16: col = base + T * (1 - F) * c7.x * (T - base)
//  13-17: col * (fog.w * c1.y) + fog.rgb       (fog = the VS c5/c6 ramp)
//  18-26: tone tail WITHOUT an exposure multiply, alpha = tone(2 * fog.w)
float4 ShadeHom(VSOut i, float4 albedo) {
  float3 t = albedo.rgb * albedo.rgb;
  float3 n = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                      : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float ndv = saturate(dot(-normalize(i.rpos), n));
  float fr = pow(max(abs(1.0 - ndv), 1e-6), mat_tint.w);
  float3 base = max(t, overlay.y) * mat_tint.rgb;
  float3 col = base + t * (1.0 - fr) * overlay.x * (t - base);
  float f1 = saturate(length(i.rpos) * sh_fogp.x + sh_fogp.y);
  if (sh_fogp.z != 1.0) {
    f1 = pow(max(f1, 1e-6), sh_fogp.z);
  }
  float fog_t = 1.0 + sh_fogc.a * f1;
  float3 xe = col * (fog_t * overlay.z) + sh_fogc.rgb * f1;
  // Alpha runs the same tail on 2 * fog.w (line 13's adds r0.w, r2.ww).
  float xa = 2.0 * fog_t;
  float ta = saturate(1.0 - xa);
  float a = saturate(sqrt(abs((max(xa * 0.25 + 0.75, 1.0) - ta * ta) * 0.5)));
  return ToneOut(xe, a, false);
}
float4 ShadeCharacter(VSOut i, float4 albedo) {
  float fam = cam_pos.w;
  if (fam > 7.5 && fam < 8.5) {
    return ShadeHom(i, albedo);
  }
  // cacstamp_shift_defaultPS samples the board graphic at tf5 using the
  // c190/c191 UV transform emitted by the shift vertex shader. The game
  // composites the raw art RGB over its squared, modulated diffuse.
  float4 shift_art = float4(0.0, 0.0, 0.0, 0.0);
  if (fam > 1.5 && fam < 2.5 && overlay.w > 6.5 && overlay.w < 7.5) {
    const float3 uvw = float3(i.uv2, 1.0);
    const float2 cg = float2(dot(uvw, mat_tint.xyz),
                             dot(uvw, float3(mat_tint.w, misc.z, misc.w)));
    if (all(cg >= 0.0) && all(cg <= 1.0)) {
      shift_art = decal_art2.Sample(smp_clamp, cg);
    }
  }
  // Printed garments use a separate cacstamp_cloth tf5 stamp. misc.x selects
  // it while overlay.w remains available for the independent tf6 dirt layer.
  float4 cloth_stamp = float4(0.0, 0.0, 0.0, 0.0);
  if (fam > 1.5 && fam < 2.5 && misc.x > 0.5 && misc.x < 1.5) {
    const float3 uvw = float3(i.uv2, 1.0);
    const float2 cg = float2(dot(uvw, mat_tint.xyz),
                             dot(uvw, float3(mat_tint.w, misc.z, misc.w)));
    if (all(cg >= 0.0) && all(cg <= 1.0)) {
      cloth_stamp = decal_art2.Sample(smp_clamp, cg);
    }
  }
  float3 dlin = albedo.rgb * albedo.rgb;
  float3 cn = dot(i.nrm, i.nrm) > 0.01
                  ? normalize(i.nrm)
                  : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float ndl = saturate(dot(cn, ch_light.xyz));
  float3 vd = -normalize(i.rpos);
  float3 lin;
  float out_a = 1.0;
  if (fam > 5.5 && fam < 7.5) {
    // vehicle_defaultPS (fam 6) and vehicle_glass_defaultPS (fam 7, the
    // windows), transcribed from their ucode. The glass program is the body
    // program with three differences (glass ucode lines 68-111): sel = the
    // glass tint c18.rgb (no diffuse / paint recolor), no gloss factor on
    // the reflection, and its SH rows sit at c21..c29 (packed into the same
    // ch_sh slots by the capture). Body lines, literals c249..c255:
    //   35-42: sel = dg^2 >= 0.001225 ? D^2 : c21 * dr^2 + c22 * db^2
    //   10/16-22: DXN normal (tf4.wy * 2 - 1, z = sqrt(1 - xy^2))
    //   26-34: R = V - 2(V.N)N, cube(-R.x, -R.y, R.z) squared
    //   45-68/75-92: shadow = min(static tf1 (c5/c6/c7), cascade tf0
    //                (c0..c3, c8 biases) * (N.L >= 0)), bilinear 4-tap
    //   73-74/93-96: spec = shadow * sat(V . -reflect(L))^c16.w * c16.rgb
    //   59/97-99: (spec + cube) * (1 - sat(V.N))^c15.w * da^2
    //   70-102: irr = sat(c23 + x c24 + y c25 + z c26 + xz c27 + zy c28
    //                     + yx c29 + (3z^2 - 1) c30 + (x^2 - y^2) c31)
    //   103-104: col = c19.w * sel * irr + refl + c15.rgb * max(N.L,0)
    //                  * shadow * sel
    //   105: col * (c14.y * fog.w) + fog.rgb ; 106-113: tone curve (c13.z)
    // Normal (lines 10/16-25): tf4.wy * 2 - 1, z = sqrt(1 - xy^2),
    // N = normalize(x * T + y * B + z * cross(T, B)) from the VS's rotated
    // usage-6/7 pair; B rides tanb (mesh decode), the stored normal equals
    // the pair's cross, so T = cross(B, N). The texture views apply the
    // console fetch swizzle, so .ag here IS the ucode's .wy. Meshes without
    // a stored pair keep the screen-space cotangent frame.
    float3 sel = fam > 6.5 ? ch_tintA.rgb
                 : dlin.g >= 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    float3 vn = cn;
    if (overlay.z > 0.5 && i.tanb.w > 0.5 && dot(i.tanb.xyz, i.tanb.xyz) > 1e-6) {
      float2 nm = macro.Sample(smp, i.uv).ag * 2.0 - 1.0;
      const float3 vbn = i.tanb.xyz;
      const float3 vtn = cross(vbn, cn);
      vn = normalize(nm.x * vtn + nm.y * vbn + cn * sqrt(saturate(1.0 - dot(nm, nm))));
    } else if (overlay.z > 0.5) {
      float2 nm = macro.Sample(smp, i.uv).ag * 2.0 - 1.0;
      float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
      float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
      float3 dp2p = cross(dp2, cn), dp1p = cross(cn, dp1);
      float3 tt = dp2p * du1.x + dp1p * du2.x;
      float3 bb = dp2p * du1.y + dp1p * du2.y;
      float im = rsqrt(max(max(dot(tt, tt), dot(bb, bb)), 1e-12));
      vn = normalize(nm.x * tt * im + nm.y * bb * im +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    const float3 wp = i.rpos + cam_pos.xyz;
    const float nl = dot(vn, ch_light.xyz);
    // Cascade compare against depth - c8.{x,y,z} per cascade (lines 49/52/
    // 55): a positive extra_bias selects those captured per-cascade rows
    // (sh_char, see CaptureCharLighting). With only the world bias (0) the
    // car compared against its OWN caster depth and sat in self-shadow,
    // hiding the player's shadow on it.
    // The game's own one-texel cascade filter + c8 biases (see
    // SampleCsmShadowVehicle), gated on N.L >= 0 (line 91).
    const float shadow_csm = SampleCsmShadowVehicle(wp, i.pos.xy, ch_rs);
    const float shadow =
        min(CharStaticShade(wp, cn, i.pos.xy), shadow_csm * (nl >= 0.0 ? 1.0 : 0.0));
    const float3 rfl = ch_light.xyz - 2.0 * nl * vn;
    const float3 spec_v = shadow *
                          pow(max(saturate(dot(vd, -rfl)), 1e-6), max(ch_ks.w, 0.5)) *
                          ch_ks.rgb;
    float3 cube = float3(0.0, 0.0, 0.0);
    if (overlay.y > 0.5) {
      const float3 rv = vd - 2.0 * dot(vd, vn) * vn;
      cube = env_cube.Sample(smp, float3(-rv.x, -rv.y, rv.z)).rgb;
      cube *= cube;
    }
    const float fres = pow(max(1.0 - saturate(dot(vd, vn)), 1e-6), max(ch_light.w, 1.0));
    const float gloss = fam > 6.5 ? 1.0 : albedo.a * albedo.a;
    const float3 irr = saturate(
        ch_sh[0].rgb + vn.x * ch_sh[1].rgb + vn.y * ch_sh[2].rgb +
        vn.z * ch_sh[3].rgb + (vn.x * vn.z) * ch_sh[4].rgb +
        (vn.z * vn.y) * ch_sh[5].rgb + (vn.y * vn.x) * ch_sh[6].rgb +
        (vn.z * vn.z) * ch_sh[7].rgb +
        (vn.x * vn.x - vn.y * vn.y) * ch_sh[8].rgb);
    const float3 col = ch_amb.w * sel * irr + (spec_v + cube) * fres * gloss +
                       ch_key.rgb * max(nl, 0.0) * shadow * sel;
    // Global distance fog, same VS c5/c6 ramp as the world families.
    float f1 = saturate(length(i.rpos) * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    lin = col * (ch_tintB.w * (1.0 + sh_fogc.a * f1)) + sh_fogc.rgb * f1;
    out_a = ch_misc.x;
  } else if (fam > 3.5 && fam < 4.5) {
    // cac_hair_defaultPS (+ its _ropa twin, identical ucode), transcribed
    // from this build's .fpo (literals c252 = (0.25, -1, 1, 0), c253 =
    // (0.75, 1.75, 0.5, 256.5)):
    //   10/15/28-33: N' = normalize(nm.x T + nm.y B + nz N), tf4.wy normal
    //   16-21/24-26: H = normalize(V + L)          (V = c4 - pos, L = c3)
    //   32/36/38-39: kk = sqrt(1 - (H.T)^2)        (Kajiya-Kay, tangent T)
    //   34-35: wrap = sat(0.75 N'.L + 0.25), ramp = sat(1.75 N'.L + 0.25)
    //   11-14/22-31: shadow = static world-shadow, bilinear 4 taps (tf1)
    //   37/42: diffuse = (wrap * shadow + c13.w / 4) * c15.rgb
    //   40-46: spec = kk^c10.w * c16.rgb * ramp * D^2 * shadow
    //   47: col = diffuse * D^2 + spec ; 48 fog ; 49-57 tone (c7.z)
    //   57: alpha = tf5(uv2) * c14.x
    // There is NO ambient/SH and NO cascade term. The old approximation
    // swapped the Kajiya-Kay highlight (near 1 across most strands) for a
    // fresnel rim (near 0 on camera-facing hair), which left lit hair at
    // roughly a third of the game's brightness. The tangent is the
    // U-aligned screen frame (the skinned layout carries no stored frame);
    // its sign is irrelevant to (H.T)^2. The world-shadow term follows the
    // renderer-wide substitution of the static sun map for the game's
    // world-shadow map (hair never renders into it: base bias only).
    float3 tt, bb;
    ScreenTangentFrame(cn, i.rpos, i.uv, tt, bb);
    float3 vn = cn;
    if (overlay.z > 0.5) {
      float2 nm = macro.Sample(smp, i.uv).ag * 2.0 - 1.0;
      vn = normalize(nm.x * tt + nm.y * bb +
                     cn * sqrt(saturate(1.0 - dot(nm, nm))));
    }
    const float3 hv = normalize(vd + ch_light.xyz);
    // Kajiya-Kay axis = the mesh's skinned usage-6 tangent (VS o2 -> PS r2),
    // carried in tanb (w = 1 when present); the screen U axis only as the
    // fallback for meshes without one.
    const float3 kk_t = (i.tanb.w > 0.5 && dot(i.tanb.xyz, i.tanb.xyz) > 1e-6)
                            ? normalize(i.tanb.xyz)
                            : tt;
    const float th = dot(hv, kk_t);
    const float kk = sqrt(saturate(1.0 - th * th));
    const float nl = dot(vn, ch_light.xyz);
    const float wrap = saturate(nl * 0.75 + 0.25);
    const float ramp = saturate(nl * 1.75 + 0.25);
    const float sh = CharStaticShade(i.rpos + cam_pos.xyz, cn, i.pos.xy);
    const float3 diff = (wrap * sh + ch_amb.w) * ch_key.rgb;
    const float3 spec = pow(max(kk, 1e-6), ch_light.w) * ch_tintB.rgb * ramp * sh;
    lin = dlin * (diff + spec);
    out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
  } else if (fam > 3.5) {
    // Hair (defaulthair): key on a wrapped N.L ramp + flat
    // ambient, fresnel rim tint on a steeper ramp; strand coverage from
    // the mesh's "alpha" channel at the raw second texcoord (bound at t4)
    // - alpha-blended in the sorted sub-pass (hair drawn opaque is the
    // blocky-helmet look).
    // World shadow: defaulthair_defaultPS samples the world-shade map like
    // every receiver (four tf1 taps at +-0.5 texel, sge against the c3 ray,
    // frac-bilinear) and multiplies it into the key diffuse (sat(N.L) * c10
    // * shadow) and the c12 highlight; the c15 fresnel rim stays unshadowed
    // (sk83_na_m's build: the same program one register lower). Without it
    // the hair stayed sunlit under world shadow that darkened the body.
    const float sh = CharStaticShade(i.rpos + cam_pos.xyz, cn, i.pos.xy);
    float fres = pow(1.0 - saturate(dot(cn, vd)), max(ch_light.w, 1.0));
    float3 hl = ch_key.rgb * (saturate(ndl * 0.75 + 0.25) * sh + ch_amb.w) +
                ch_tintB.rgb * fres * saturate(ndl * 1.75 + 0.25);
    lin = dlin * hl;
    out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
  } else if (fam > 2.5 && fam < 3.5 && ch_misc.z > 1.5) {
    // Far pedestrians: defaultlivingworld_defaultPS, transcribed from its
    // ucode (literals c253 = (0.25, 0.75), c254 = (0.5, 256.5, 0.001225, 1),
    // c255 = (0.5, 256, -0.5, -256)):
    //   13-18: sel = g^2 >= 0.001225 ? D^2 : c14 * r^2 + c15 * b^2
    //   5-11/15-16/19-23: static world shadow, 4 point taps at +-0.5 texel
    //          of the 512 map (u = c0.pos * 0.5 + 0.5, v = c1.pos * -0.5
    //          + 0.5, depth c2.pos), bilinear by frac
    //   24-25: light = c6.y * c11.w * saturate(shadow + c12.w)
    //   26-27: col = light * fog.w * sel + fog.rgb ; 28-35 tone (c5.z)
    // No N.L, no ambient, no dynamic shadow: the old path lit them with a
    // white key * N.L plus a flat ambient of 1 (up to 2x too bright).
    float3 sel = dlin.g >= 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    float world = 1.0;
    if (overlay.w > 7.5) {
      float4 wp4 = float4(i.rpos + cam_pos.xyz, 1.0);
      float2 wuv = float2(dot(wp4, dyn_wsx) * 0.5 + 0.5,
                          dot(wp4, dyn_wsy) * -0.5 + 0.5);
      float rdw = dot(wp4, dyn_wsz);
      float2 st = wuv * 512.0 - 0.5;
      float2 fw = frac(st);
      int2 b0 = int2(floor(st));
      float t00 = decal_art.Load(int3(clamp(b0, int2(0, 0), int2(511, 511)), 0)).x >= rdw ? 1.0 : 0.0;
      float t10 = decal_art.Load(int3(clamp(b0 + int2(1, 0), int2(0, 0), int2(511, 511)), 0)).x >= rdw ? 1.0 : 0.0;
      float t01 = decal_art.Load(int3(clamp(b0 + int2(0, 1), int2(0, 0), int2(511, 511)), 0)).x >= rdw ? 1.0 : 0.0;
      float t11 = decal_art.Load(int3(clamp(b0 + int2(1, 1), int2(0, 0), int2(511, 511)), 0)).x >= rdw ? 1.0 : 0.0;
      world = lerp(lerp(t00, t10, fw.x), lerp(t01, t11, fw.x), fw.y);
    }
    const float light = ch_amb.w * saturate(world + ch_amb.x);
    float f1 = saturate(length(i.rpos) * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    lin = sel * (light * (1.0 + sh_fogc.a * f1)) + sh_fogc.rgb * f1;
    out_a = ch_misc.x;
  } else if (fam > 2.5 && fam < 3.5 && ch_misc.z > 0.5) {
    // Near pedestrians, exact: livingworld_stamp_defaultPS, transcribed from
    // its ucode (literals c249 = (0.25, 3), c250 = (0.2, -1, 0.001225,
    // 0.75), c252 = (256.5, 0.5, 0, 1), c255 = (-1, -0.99, 0.5, -0.5); rows
    // in CaptureCharLighting). It is the cacstamp lighting (fam 2 above)
    // with the stamp recolor and a full tangent-space normal map:
    //   12/15/18-23: sel = g^2 >= 0.001225 ? D^2 : c23 r^2 + c24 b^2
    //   11/16-17/27-33: N' = normalize((2x-1) T + (2y-1) B + (2z-1) N),
    //          tf4.xyz; T/B = the VS's skinned usage-6/7 pair (o2/o3),
    //          N = cross(T, B) (o1)
    //   34/97: key = sat(N'.L) * c15 ; shadow = min(static tf1, cascade
    //          tf0 * (N'.L >= 0))
    //   46-88/102: amb = c19.w * sat(SH c25..c33 at c21.y-scaled N')
    //   62/76/92/105: rim = sat(sat(N'.rd) * fb^c14.z) * c20, rd =
    //          normalize(L * (-1, 0.2, -1) - V), fb = 1 - sat(N'.V)
    //   64-65/99-107: spec = sat(V.R)^c16.w * shadow * c16 * fb^c15.w
    //          + sat(V.-reflect(rd, N'))^c17.w * c17 * fb^c14.z
    //   108: spec = sat(spec * (2y - 1))   (the map's own y as the mask)
    //   109: col = (key * shadow + amb + rim) * sel + spec
    //   95-96/103/110-111: col = lerp(col, sel * c19.w * sat(shadow +
    //          c20.w), c34.x)
    //   110b-121: col * c14.y (* fog) -> tone chain (c13.z)
    float3 sel = dlin.g >= 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    float3 vn = cn;
    float spec_mask = 0.0;
    if (overlay.z > 0.5) {
      const float3 nm = macro.SampleBias(smp, i.uv, misc.y).rgb * 2.0 - 1.0;
      float3 tt, bb;
      if (i.tanb.w > 0.5 && dot(i.tanb.xyz, i.tanb.xyz) > 1e-6) {
        // The skinned stored tangent (mesh decode -> uv3/uv2 -> vs_main);
        // N is cross(T, B), so B = cross(N, T).
        tt = normalize(i.tanb.xyz);
        bb = cross(cn, tt);
      } else {
        ScreenTangentFrame(cn, i.rpos, i.uv, tt, bb);
      }
      vn = normalize(nm.x * tt + nm.y * bb + nm.z * cn);
      spec_mask = nm.y;
    }
    const float vndl_u = dot(vn, ch_light.xyz);
    const float vndl = saturate(vndl_u);
    const float3 wp = i.rpos + cam_pos.xyz;
    // ch_amb.z: the retail compile's static world-shade floor (c8.w;
    // `max r0.z, r2.z, c8.w` before the min with the cascade). 0 = none.
    float shadow = min(SampleCsmShadowChar(wp, i.pos.xy) * (vndl_u >= 0.0 ? 1.0 : 0.0),
                       max(CharStaticShade(wp, cn, i.pos.xy), ch_amb.z));
    float3 irr = saturate(
        ch_sh[0].rgb + vn.x * ch_sh[1].rgb + vn.y * ch_sh[2].rgb +
        vn.z * ch_sh[3].rgb + (vn.x * vn.z) * ch_sh[4].rgb +
        (vn.z * vn.y) * ch_sh[5].rgb + (vn.y * vn.x) * ch_sh[6].rgb +
        (vn.z * vn.z) * ch_sh[7].rgb +
        (vn.x * vn.x - vn.y * vn.y) * ch_sh[8].rgb);
    const float fb = 1.0 - saturate(dot(vn, vd));
    const float rfres = pow(max(fb, 1e-6), ch_misc.w);
    const float3 rd = normalize(ch_light.xyz * float3(-1.0, 0.2, -1.0) - vd);
    float3 lit = ch_key.rgb * vndl * shadow + irr * ch_amb.w;
    float3 spec = float3(0.0, 0.0, 0.0);
    if (ch_ks.w > 0.0) {
      lit += saturate(rfres * saturate(dot(vn, rd))) * ch_rim.rgb;
      const float kfres = pow(max(fb, 1e-6), ch_rim.w);
      const float3 kr = ch_light.xyz - 2.0 * vndl_u * vn;
      const float ks = pow(saturate(dot(vd, -kr)), ch_ks.w) * shadow;
      const float3 rr = rd - 2.0 * dot(vn, rd) * vn;
      const float rs = pow(saturate(dot(vd, -rr)), ch_rs.w) * rfres;
      spec = saturate((ks * ch_ks.rgb * kfres + rs * ch_rs.rgb) * spec_mask);
    }
    lin = lit * sel + spec;
    // c34.x: blend toward the flat shadow-only term (ch_amb.x = c20.w,
    // ch_amb.y = c34.x).
    const float3 flat_lit = sel * ch_amb.w * saturate(shadow + ch_amb.x);
    lin = lerp(lin, flat_lit, ch_amb.y);
    lin *= ch_tintB.w;  // material multiplier c14.y
    out_a = ch_misc.x;
  } else if (fam > 2.5) {
    // livingworld pedestrians: the diffuse is a stamp-mask atlas; red
    // regions recolor with tintA, blue with tintB (judged in linear
    // space; real-color regions have green above the threshold).
    // The game's character PSes multiply the key light by the CSM shadow
    // (tap >= ray = lit); characters are casters themselves, so an extra
    // receiver bias suppresses self-shadow acne while the body-onto-board
    // / body-onto-NPC shading survives (the sun-axis depth gap there is
    // tens of cm). Without this the held skateboard, a big flat surface
    // that is almost always inside the skater's own shadow, renders
    // fully sunlit (near-white) against the emulated dark deck.
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, cn, i.pos.xy),
                    CharStaticShade(i.rpos + cam_pos.xyz, cn, i.pos.xy));
    float3 sel = dlin.g > 0.001225
                     ? dlin
                     : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
    lin = sel * (ch_key.rgb * ndl * csm + ch_amb.rgb);
    // livingworld_stamp_defaultPS ends `max oC0.w, c21.x`: the entity's
    // spawn/distance fade. Only visible when the item is routed to the
    // blended sub-pass (alpha < 1); the opaque pass ignores it.
    out_a = ch_misc.x;
  } else {
    // defaultcharacter / CAC pieces: key light + SH irradiance ambient,
    // key gated by the CSM shadow (see the livingworld comment above).
    // The material's DXT5nm normal map (x in ALPHA, y in GREEN; the PSes
    // read tf4.wy) rides the macro slot; overlay.z > 0 = resolved. The
    // skinned vertex layout has no free bytes for the authored tangent
    // frame (blend weights/indices own them), so use the screen-space
    // cotangent frame. ScreenTangentFrame's env calibration (negated U
    // axis, raw V axis) holds for characters too: projecting the skinned
    // meshes through the frame's real view_proj and taking the D3D y-down
    // screen-basis derivatives on the CAMERA-FACING triangles dots the
    // frame against the skinned usage-6/7 tangent frame at -0.95 (tt.T) /
    // +0.95 (bb.B); an edge-basis check without the projection lands on
    // the back-face orientation and reads inverted. Map x rides T, map y
    // rides B (the VS skins the usage-6 tangent into the interpolator the
    // PS pairs with tf4.w, the usage-7 binormal into the tf4.y one).
    // Normal map / shadow / ambient + rim: CharLight (shared with the CAS
    // skin unwrap pass). The DXT5nm map (x in ALPHA, y in GREEN; the PSes
    // read tf4.wy) rides the macro slot, overlay.z > 0 = resolved; the
    // skinned layout has no free bytes for the authored tangent frame, so
    // the screen-space cotangent frame stands in (env calibration: negated
    // U axis, raw V axis; verified against the skinned usage-6/7 frame).
    // misc.y = LOD bias to the console's 640p-gradient mip.
    const CharLightTerms lt = CharLight(i, cn, vd);
    const float3 vn = lt.vn;
    const float vndl_u = lt.vndl_u;
    const float vndl = saturate(vndl_u);
    float csm = lt.csm;
    if (ch_tintA.w > 0.0) {
      dlin *= ch_tintA.rgb;
    }
    if (shift_art.a > 0.0) {
      dlin = lerp(dlin, shift_art.rgb, shift_art.a);
    }
    if (cloth_stamp.a > 0.0) {
      dlin = lerp(dlin, cloth_stamp.rgb, cloth_stamp.a);
    }
    // CAS skin custom graphic (cacstamp_skin_defaultPS lines 10/18-24):
    // the squared, tinted diffuse times the material's "decal" texture
    // (tf6, bound at t1, overlay.x = 1) at the VS-placed second texcoord
    // (vs_cacstamp_skin_defaultVS 40/41: dot((u,v,1), c190.xyz) and
    // dot((u,v,1), c191.xyz); rows in mat_tint.xyz / mat_tint.w + misc.zw).
    // tf6 alpha blends its RGB against neutral white before multiplying the
    // diffuse: the ucode computes 1 + (tf6.rgb - 1) * tf6.a. Ignoring alpha
    // turns transparent pixels in custom team-logo tattoos into dark ink.
    // tf6 clamps to a WHITE border, so only the placed rectangle carries
    // the graphic.
    // Face injury layer (cac_face_{default,nis}PS lines 12/20-23): the
    // bruise / scrape colour used RAW (unsquared) against the squared,
    // tinted skin, lerped by the map's mask x c22.z (overlay.x = 2, the map
    // at t1, strength in misc.z).
    if (overlay.x > 1.5) {
      const float4 inj = lightmap.Sample(smp, i.uv);
      dlin = lerp(dlin, inj.rgb, inj.a * misc.z);
    } else if (overlay.x > 0.5) {
      const float3 uvw = float3(i.uv2, 1.0);
      const float2 cg = float2(dot(uvw, mat_tint.xyz),
                               dot(uvw, float3(mat_tint.w, misc.z, misc.w)));
      if (all(cg >= 0.0) && all(cg <= 1.0)) {
        const float4 tattoo = lightmap.Sample(smp_clamp, cg);
        dlin *= lerp(float3(1.0, 1.0, 1.0), tattoo.rgb, tattoo.a);
      }
    }
    // CAC skin body injury layer (cacstamp_skin_{default,nis}PS 13/18-28):
    // after the custom-graphic multiply, lerp to the injury map's raw rgb by
    // its mask x c22.z (map at t5, strength ch_tintB.y; 0 = none).
    if (ch_tintB.y > 0.0) {
      const float4 inj = normal_map.Sample(smp, i.uv);
      dlin = lerp(dlin, inj.rgb, inj.a * ch_tintB.y);
    }
    // CAC cloth dirt layer (overlay.w == 5: decal2 bound at t4), exact from
    // cac_cloth[_ropa]_defaultPS lines 19-28: the dirt rgb is used RAW
    // (unsquared) against the squared, tinted base:
    //   albedo = base + (lerp(base, dirt.rgb, dirt.a) - base) * c22.y
    // (c22.y in ch_tintB.x). Without it the stained tee rendered clean.
    if (overlay.w > 4.5 && overlay.w < 5.5) {
      float4 dt = decal_art.Sample(smp, i.uv);
      dlin = lerp(dlin, lerp(dlin, dt.rgb, dt.a), ch_tintB.x);
    }
    // CAS subsurface scattering (cac_face_nisPS / cacstamp_skin_nisPS, the
    // create-a-skater compiles): overlay.y = the scatter amount c21.x when
    // this piece's blurred texture-space lighting is bound at t8 (see
    // ps_skin_unwrap / ps_skin_blur) with its "blurmask" at t9.
    //   a   = mask.x * c21.x                          (nis 62/68)
    //   s'  = shadow + a * (blur.a - shadow)           (94-98)
    //   K   = c15 * sat(N.L) * s' + c19.w SH + rim     (104)
    //   lit = K + a * (blur.rgb - K * (1 - c255.rgb))  (105-112)
    // c255 = (0.22044, 0.437, 0.635): red keeps the least of the direct
    // light and takes the most of the blurred, so shade fills in red.
    float sss_a = 0.0;
    float4 sss_blur = float4(0.0, 0.0, 0.0, 0.0);
    if (overlay.y > 0.0) {
      sss_blur = detail_map.Sample(smp_clamp, i.uv);
      sss_a = spec2_map.Sample(smp, i.uv).r * overlay.y;
      csm = csm + sss_a * (sss_blur.a - csm);
    }
    float3 lit = ch_key.rgb * vndl * csm + lt.amb_rim;
    if (overlay.y > 0.0) {
      lit += sss_a * (sss_blur.rgb - lit * (1.0 - float3(0.22044, 0.437, 0.635)));
    }
    float3 spec = float3(0.0, 0.0, 0.0);
    // Key/rim phong specular, exact from the character PSes (ch_ks.w == 0 =
    // rows not captured -> the terms vanish). The key spec reflects the sun
    // about the mapped normal, gated on N.L >= 0 and the (scatter-blended)
    // shadow; the rim terms share the game's fixed rim direction built from
    // the sun and view. Spec mask = the diffuse alpha SQUARED (linear-space
    // decode, like the vehicle gloss); skin/face carry a dedicated mask map
    // in the free decal slot instead (overlay.w = 3; 2 = map present but not
    // yet decoded -> mask 0, because the DXT1 skin diffuse's opaque alpha
    // would read as a full-white mask).
    if (ch_ks.w > 0.0) {
      float kfres = pow(max(lt.fb, 1e-6), ch_rim.w);
      float3 kr = ch_light.xyz - 2.0 * vndl_u * vn;
      float ks = pow(saturate(dot(vd, -kr)), ch_ks.w) * csm *
                 (vndl_u >= 0.0 ? 1.0 : 0.0);
      float3 rr = lt.rd - 2.0 * dot(vn, lt.rd) * vn;
      float rs = pow(saturate(dot(vd, -rr)), ch_rs.w) * lt.rfres;
      // overlay.w: 3 = spec mask at t4, 2 = mask pending (off), 5 = the
      // cloth dirt layer rides t4 instead (cloth masks by diffuse alpha).
      float smask = (overlay.w > 2.5 && overlay.w < 3.5)
                        ? decal_art.SampleBias(smp, i.uv, misc.y).r
                        : ((overlay.w > 1.5 && overlay.w < 2.5) ? 0.0 : albedo.a);
      smask *= smask;
      spec = saturate((ks * ch_ks.rgb * kfres + rs * ch_rs.rgb) * smask);
    }
    lin = dlin * lit + spec;
    // The PS multiplies the lit color by the material multiplier
    // m_params[0].y before the tone chain (1.2 on the gameplay banks;
    // without it the jeans sit ~14% darker than the emulated reference).
    // Captured into the otherwise-unused tintB.w on fams 1/2; 0 = an
    // older capture without it.
    if (ch_tintB.w > 0.25) {
      lin *= ch_tintB.w;
    }
    // defaultcharacter/cacstamp PSes end `max oC0.w, c13.x / c22.x`:
    // the entity's spawn fade (see the livingworld comment above).
    out_a = ch_misc.x;
    // character.alpha accessory (sunglass lens): coverage from the mesh's
    // "alpha" channel at the raw second texcoord, like hair (cac_alphaPS:
    // oC0.w = tf5(uv2).r * c22.x), routed to the blended sub-pass.
    if (ch_misc.z > 0.5) {
      out_a *= saturate(decal_art.Sample(smp, i.uv2).r);
    }
  }
  // Exact tone chain: sqrt(0.5 * (max(x*E/4 + 0.75, 1) - sat(1 - x*E)^2)).
  float E = max(ch_key.w, 0.01);
  return ToneOut(lin * E, out_a, false);

}

// CAS skin texture-space lighting (cac_face_unwrapPS == cacstamp_skin_
// unwrapPS, ucode lines 8-87): the piece rasterized in its TEXCOORD0 layout
// (vs_skin_unwrap) writes rgb = c12 * sat(N.L) * shadow + c16.w * sat(SH) +
// rim and a = shadow (min(world, CSM * (N.L >= 0))) into the 128x128
// k_8_8_8_8 target the jitter blur reads. Same rows as the main pass (see
// CharLight).
float4 ps_skin_unwrap(VSOut i) : SV_Target {
  float3 cn = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                       : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  const float3 vd = -normalize(i.rpos);
  const CharLightTerms lt = CharLight(i, cn, vd);
  // Shadow = min(world, CSM * (N.L >= 0)) (unwrap 31/84-85), also the key's.
  const float sh = lt.csm * (lt.vndl_u >= 0.0 ? 1.0 : 0.0);
  const float3 k = ch_key.rgb * saturate(lt.vndl_u) * sh + lt.amb_rim;
  return saturate(float4(k, sh));
}

// postfx_jitterBlurPS (ucode lines 3-32): twelve bilinear clamped taps of
// the unwrap target (t8) at uv + c0.xy * literal offsets (c238..c243, two
// taps per row), each weighted per channel by its literal row (c244..c255;
// the weights sum to (0.778, 0.563, 0.365, 1): the diffusion profile, red
// widest). The game's c0.xy (0.75 in every capture) rides overlay.xy.
struct SkinBlurOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
SkinBlurOut vs_skin_blur(uint id : SV_VertexID) {
  SkinBlurOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = t;
  return o;
}
float4 ps_skin_blur(SkinBlurOut i) : SV_Target {
  static const float4 L[6] = {
      float4(0.03247, -0.03137, -0.03174, -0.03211),   // c238
      float4(0.00198, 0.03874, 0.04665, -0.00851),     // c239
      float4(0.01031, -0.00003, -0.00092, -0.00142),   // c240
      float4(0.00071, -0.00152, -0.00172, -0.00187),   // c241
      float4(0.00029, 0.00139, 0.01277, 0.01342),      // c242
      float4(0.01986, -0.00499, -0.02536, -0.04920)};  // c243
  static const float4 W[12] = {
      float4(0.03094, 0.00287, 0.00065, 0.08333),   // c244
      float4(0.05132, 0.00603, 0.00142, 0.08333),   // c245
      float4(0.04809, 0.00304, 0.00040, 0.08333),   // c246
      float4(0.02569, 0.04273, 0.03300, 0.08333),   // c247
      float4(0.06484, 0.08639, 0.06227, 0.08333),   // c248
      float4(0.07636, 0.06449, 0.03910, 0.08333),   // c249
      float4(0.11652, 0.10322, 0.06491, 0.08333),   // c250
      float4(0.13180, 0.15169, 0.10368, 0.08333),   // c251
      float4(0.04859, 0.06474, 0.04613, 0.08333),   // c252
      float4(0.04885, 0.00541, 0.00122, 0.08333),   // c253
      float4(0.06143, 0.00915, 0.00251, 0.08333),   // c254
      float4(0.07358, 0.02324, 0.00970, 0.08333)};  // c255
  const float2 s = overlay.xy;
  const float2 uv = i.uv;
  // Each row R feeds two taps: "yx" = (R.s1, R.s0) and "zw" = (R.s2, R.s3)
  // of the row's swizzle s (u offset, v offset), read off the ucode.
  float4 acc = 0.0;
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[0].z, L[0].w), 0) * W[10];  // c238.wzyx yx -> c254
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[0].y, L[0].x), 0) * W[1];   //            zw -> c245
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[1].w, L[1].y), 0) * W[9];   // c239.ywzx yx -> c253
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[1].z, L[1].x), 0) * W[2];   //            zw -> c246
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[2].x, L[2].w), 0) * W[8];   // c240.wxyz yx -> c252
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[2].y, L[2].z), 0) * W[3];   //            zw -> c247
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[3].w, L[3].z), 0) * W[7];   // c241.zwyx yx -> c251
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[3].y, L[3].x), 0) * W[4];   //            zw -> c248
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[4].y, L[4].w), 0) * W[6];   // c242.wyzx yx -> c250
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[4].z, L[4].x), 0) * W[5];   //            zw -> c249
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[5].x, L[5].z), 0) * W[11];  // c243.zxyw yx -> c255
  acc += detail_map.SampleLevel(smp_clamp, uv + s * float2(L[5].y, L[5].w), 0) * W[0];   //            zw -> c244
  return acc;
}
