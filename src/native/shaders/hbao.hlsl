// Horizon-based ambient occlusion, interleaved-rendering variant.
//
// This is a clean-room implementation of the technique NVIDIA's HBAO+
// documents (Bavoil & Sainz 2008, "Image-Space Horizon-Based Ambient
// Occlusion"; the interleaved-rendering refinement from Bavoil & Jansen
// 2013, "Particle Shadows & Cache-Efficient Post-Processing"; the simplified
// per-sample falloff from McGuire et al. 2012, "Scalable Ambient
// Obscurance"). No NVIDIA source was read or copied to write this file --
// only the public technique descriptions. See ssao.hlsl for the existing
// GTAO pass (horizon-ANGLE integral, per-pixel noise, optional half-res);
// this is the other family (per-SAMPLE falloff accumulation, per-TILE
// jitter, always full-res), offered as a sibling technique selected by
// skate3_native_render_scene_ao_technique.
//
// Pass chain (all fullscreen triangles, shares ssao.hlsl's binding layout
// and its ps_blur / ps_composite / ps_debug -- those operate on the
// finished R8_UNORM multiplier plane and don't care which technique wrote
// it):
//   ps_hbao_interleave   full-res linear depth (ssao.hlsl's ps_linearize
//                        output) -> a 4x4 tiled rearrangement, one
//                        quarter-res "tile" per interleave cell, so a cell's
//                        samples land on contiguous texels (the cache-
//                        locality point of interleaved rendering: a naive
//                        per-pixel-jittered full-res march scatters its
//                        samples across the whole depth texture's cache
//                        footprint instead).
//   ps_hbao              one interleave cell's worth of pixels (1/16 of the
//                        frame) per invocation x16 passes, each with its own
//                        jitter; direction-march + simple falloff AO at full
//                        depth precision, written into the matching
//                        tile of an R8_UNORM plane shaped like the
//                        interleave target.
//   ps_hbao_reinterleave tiled AO plane -> full-res AO plane (the inverse of
//                        ps_hbao_interleave's rearrangement).
//
// Reuses ssao.hlsl's cbuffer C layout exactly (same b0 rows), its
// ps_linearize output (g_r.ao_lin_depth) and its ps_luma output (the sun-lit
// protection mask) as inputs, and its ps_blur / ps_composite / ps_debug as
// the tail of the chain -- only the AO estimate itself (ps_hbao) and the
// interleave/reinterleave pair are new.
// size.xy is always the FULL-RES output size here (matching ssao.hlsl's
// convention for ps_linearize/ps_luma, whose outputs this pass consumes
// directly) -- never the tiled-plane size, which every pass below derives
// locally as size.xy / HBAO_CELL instead of taking it from the constant
// buffer, so one b0 layout serves all three passes unambiguously.
cbuffer C : register(b0) {
  float4 size;  // xy = full-res size in px, zw = 1 / size
  float4 proj;  // x = |m00|, y = |m11|, z = m22, w = m32
  float4 p0;    // x = radius (view units), y = intensity,
                // z = fade start (view Z), w = 1 / fade range
  float4 p1;    // xy = interleave cell (cx, cy) for ps_hbao, z = sun-lit
                // luma protection scale, w unused
  float4 p2;    // xy = cell block size in px (floor(full / HBAO_CELL); the
                // tiled plane is p2.xy * HBAO_CELL, which can be up to
                // HBAO_CELL-1 px SMALLER than full res when the output
                // dimensions are not multiples of HBAO_CELL -- so this is
                // passed, never re-derived from size.xy), zw = 1 / p2.xy
};
Texture2D<float> tex0 : register(t0);   // interleave: full-res linear depth
                                         // hbao: the tiled linear depth
                                         // reinterleave: the tiled AO plane
Texture2D<float> tex1 : register(t1);   // hbao: the sun-lit luma mask
                                         // (full res; ssao.hlsl's ps_luma)
SamplerState smp_point : register(s0);
SamplerState smp_linear : register(s1);

static const float kPi = 3.14159265f;
#define HBAO_CELL 4  // 4x4 = 16 interleaved passes, one jittered direction
                     // set apiece: matches HBAO+'s documented interleave
                     // factor.
#define HBAO_DIRECTIONS 4
#define HBAO_STEPS 4

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

float3 ViewPos(float2 uv, float z) {
  float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
  return float3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}

// Rearranges the full-res plane into a tiled layout where interleave cell
// (cx,cy) in [0,HBAO_CELL) occupies the sub-rectangle
// [cx*cellW, (cx+1)*cellW) x [cy*cellH, (cy+1)*cellH) of the output, each
// sub-rectangle holding every HBAO_CELL'th full-res pixel starting at that
// cell's offset. A pixel's own cell is interleave-sampled contiguously, the
// point of the layout: ps_hbao's samples for one cell land in a tight
// texture-cache footprint instead of being scattered across the full-res
// plane by a per-pixel jitter.
// The tiled layout, used identically by all three passes below:
//   tiled pixel (tx, ty) <-> full-res pixel (within * HBAO_CELL + cell)
// where cell = tiled_px / cell_size and within = tiled_px % cell_size.
// The tiled plane is the same total size as the full-res one; it is a 4x4
// arrangement of cell_size blocks, block (cx, cy) holding every 4th full-res
// pixel starting at offset (cx, cy). SV_Position is in render-target space,
// so a cell-offset viewport still yields the absolute tiled pixel here --
// which is what keeps ps_hbao's addressing identical to the other two.
uint2 TiledToFull(uint2 tiled_px, uint2 cell_size) {
  uint2 cell = tiled_px / cell_size;
  uint2 within = tiled_px % cell_size;
  return within * HBAO_CELL + cell;
}

float ps_hbao_interleave(VSOut i) : SV_Target {
  uint2 full_size = uint2(size.x, size.y);
  uint2 cell_size = uint2(p2.x, p2.y);
  uint2 src_px = min(TiledToFull(uint2(i.pos.xy), cell_size), full_size - 1);
  return tex0.Load(int3(int2(src_px), 0));
}

float ps_hbao_reinterleave(VSOut i) : SV_Target {
  // Inverse of TiledToFull: full-res (fx, fy) lives in block (fx, fy) % 4 at
  // within-block position (fx, fy) / 4. The tiled plane covers only
  // cell_size * HBAO_CELL px, so full-res pixels in the right/bottom
  // remainder strip (when the output is not a multiple of HBAO_CELL) have no
  // tiled counterpart; clamp them to the last covered block texel rather
  // than sampling out of bounds. At most 3 px wide, and the depth-aware blur
  // that follows smooths the seam.
  uint2 cell_size = uint2(p2.x, p2.y);
  uint2 out_px = uint2(i.pos.xy);  // full-res pixel
  uint2 cell = out_px % HBAO_CELL;
  uint2 within = min(out_px / HBAO_CELL, cell_size - 1);
  uint2 src_px = cell * cell_size + within;
  return tex0.Load(int3(int2(src_px), 0));
}

// One interleave cell's AO estimate: HBAO_DIRECTIONS screen-space march
// directions, HBAO_STEPS samples each, a simple distance-weighted falloff
// per sample (no horizon-angle integral -- the "simpler AO approximation"
// HBAO+'s README credits to Scalable Ambient Obscurance, chosen there to
// avoid over-occlusion artifacts the full horizon-angle form can show).
// Operates on the tiled plane, so its texture fetches land in one
// contiguous cell-sized region per invocation: the interleave pass already
// did the scatter, once, for the whole frame.
float ps_hbao(VSOut i) : SV_Target {
  // This draw's viewport covers ONE cell block of the tiled plane, but
  // SV_Position is absolute in render-target space, so tile_px is the
  // tiled-plane pixel and TiledToFull maps it exactly as the interleave pass
  // did -- no dependence on which cell the viewport selected, so the two
  // addressings cannot drift apart. p1.xy carries the same cell only for the
  // jitter below (cheaper than re-deriving it, and identical by
  // construction).
  uint2 full_size = uint2(size.x, size.y);
  uint2 cell_size = uint2(p2.x, p2.y);
  uint2 tile_px = uint2(i.pos.xy);
  uint2 cell = tile_px / cell_size;
  uint2 full_px = min(TiledToFull(tile_px, cell_size), full_size - 1);
  float2 full_uv = (float2(full_px) + 0.5f) * size.zw;
  // tex0 is the tiled plane, whose UV space spans cell_size * HBAO_CELL px
  // (p2.zw / HBAO_CELL per texel) -- NOT size.zw, which would be wrong
  // whenever the output is not a multiple of HBAO_CELL.
  float2 tile_texel = p2.zw / float(HBAO_CELL);
  float2 tile_uv = (float2(tile_px) + 0.5f) * tile_texel;

  float zc = tex0.SampleLevel(smp_point, tile_uv, 0);
  float fade = saturate((zc - p0.z) * p0.w);
  float luma = tex1.SampleLevel(smp_point, full_uv, 0);
  float protect = saturate(luma * p1.z);
  protect *= protect;
  if (fade >= 1.0f || protect >= 0.999f || luma < 0.015f) {
    return 1.0f;
  }

  float3 P = ViewPos(full_uv, zc);
  float3 V = -normalize(P);

  // Every tex0 fetch must stay inside THIS cell's block: neighbouring blocks
  // hold a different interleave phase (unrelated pixels), so a step across a
  // block edge reads geometry from somewhere else entirely. Clamp to the
  // block's texel bounds, which also gives the usual edge-clamp behaviour at
  // the screen border.
  uint2 blk_lo_px = cell * cell_size;
  uint2 blk_hi_px = blk_lo_px + cell_size - 1;
  float2 blk_lo = (float2(blk_lo_px) + 0.5f) * tile_texel;
  float2 blk_hi = (float2(blk_hi_px) + 0.5f) * tile_texel;

  // Depth-derived normal (no G-buffer input, same reconstruction approach as
  // ssao.hlsl). One tiled-plane texel step is HBAO_CELL full-res pixels, so
  // the view-space tangents below advance by that much.
  float2 dx_uv = float2(tile_texel.x, 0.0f);
  float2 dy_uv = float2(0.0f, tile_texel.y);
  float zl = tex0.SampleLevel(smp_point, clamp(tile_uv - dx_uv, blk_lo, blk_hi), 0);
  float zr = tex0.SampleLevel(smp_point, clamp(tile_uv + dx_uv, blk_lo, blk_hi), 0);
  float zu = tex0.SampleLevel(smp_point, clamp(tile_uv - dy_uv, blk_lo, blk_hi), 0);
  float zd = tex0.SampleLevel(smp_point, clamp(tile_uv + dy_uv, blk_lo, blk_hi), 0);
  float du = float(HBAO_CELL) * size.z, dv = float(HBAO_CELL) * size.w;
  float3 th = (abs(zr - zc) < abs(zl - zc))
                  ? (ViewPos(full_uv + float2(du, 0), zr) - P)
                  : (P - ViewPos(full_uv - float2(du, 0), zl));
  float3 tv = (abs(zd - zc) < abs(zu - zc))
                  ? (ViewPos(full_uv + float2(0, dv), zd) - P)
                  : (P - ViewPos(full_uv - float2(0, dv), zu));
  float3 N = normalize(cross(tv, th));
  N = (dot(N, V) < 0.0f) ? -N : N;

  // Per-cell (not per-pixel) direction jitter: the interleave-rendering
  // point is that every pixel drawn in one ps_hbao invocation uses the SAME
  // rotation, so they march in lockstep and keep hitting the same relative
  // texel offsets -- a predictable, cache-friendly access pattern instead
  // of a per-pixel-random one. The 16 cells between them still cover 16
  // distinct rotations, so the full reinterleaved image has the same
  // directional coverage a per-pixel-noise approach would.
  float cell_id = cell.y * float(HBAO_CELL) + cell.x;
  float jitter_angle = frac(cell_id * 0.6180339887f) * 2.0f * kPi;
  float jitter_off = frac(cell_id * 0.7548776662f);

  // World-space radius projected to screen, expressed in TILE texels (the
  // units the march below steps in). One tile texel spans HBAO_CELL full-res
  // pixels, hence the /HBAO_CELL; the clamp is applied AFTER that conversion
  // and in tile units, because a bound expressed in full-res pixels collapses
  // here: a 2 px floor would be half a tile texel, so every step would land
  // back on the center texel and the whole march would integrate to zero AO
  // (an AO term of exactly 1.0 everywhere -- i.e. no visible effect at all).
  // The upper bound likewise keeps the per-cell footprint cache-local.
  float radius_px = clamp(p0.x * proj.y / zc * size.y * 0.5f /
                              float(HBAO_CELL),
                          2.0f, 32.0f);
  // The falloff radius must match the distance actually marched, not the
  // requested world radius: radius_px is clamped in tile texels above, so on
  // near surfaces the march reaches well beyond p0.x and on far ones well
  // short of it. Converting the CLAMPED pixel radius back to view units keeps
  // the two in step -- otherwise the falloff either zeroes every sample
  // (no AO) or saturates on all of them (a uniformly black frame), which is
  // exactly the pair of failures this pass showed.
  float px_to_view = zc / max(proj.y * size.y * 0.5f, 1e-6f);
  float world_r = max(radius_px * float(HBAO_CELL) * px_to_view, 1e-4f);
  float inv_r2 = 1.0f / (world_r * world_r);
  float n_bias = 0.05f;  // NdotV bias: suppresses self-occlusion on flats.

  float ao = 0.0f;
  [unroll] for (int d = 0; d < HBAO_DIRECTIONS; ++d) {
    float phi = jitter_angle + (float(d) / float(HBAO_DIRECTIONS)) * 2.0f * kPi;
    float2 sdir = float2(cos(phi), sin(phi));
    [loop] for (int s = 0; s < HBAO_STEPS; ++s) {
      // t is a step distance in TILE texels. One tile texel spans HBAO_CELL
      // full-res pixels, so the same step is t*sdir*size.zw in tile UV and
      // HBAO_CELL times that in full-res UV. The tile fetch is clamped into
      // this cell's block (see blk_lo/blk_hi); the full-res UV used for the
      // view-space position is NOT clamped, so a sample that clamps simply
      // re-reads the edge depth -- matching the usual border behaviour.
      float t = radius_px * (float(s) + jitter_off + 1.0f) / float(HBAO_STEPS);
      float2 tile_step = sdir * t * tile_texel;
      float2 full_step = sdir * t * float(HBAO_CELL) * size.zw;
      float2 suv = clamp(tile_uv + tile_step, blk_lo, blk_hi);
      float zs = tex0.SampleLevel(smp_point, suv, 0);
      float3 Ps = ViewPos(full_uv + full_step, zs);
      float3 Vs = Ps - P;
      float vlen2 = dot(Vs, Vs);
      float vlen = sqrt(vlen2);
      float ndotv = dot(N, Vs) / max(vlen, 1e-5f);
      // Scalable Ambient Obscurance falloff: linear-in-distance weight
      // instead of GTAO's horizon-angle integral -- cheaper (no acos/cos
      // per sample) and, per HBAO+'s own notes, avoids over-occlusion
      // because distant near-grazing samples contribute less rather than
      // saturating a horizon angle.
      float falloff = saturate(1.0f - vlen2 * inv_r2);
      ao += saturate(ndotv - n_bias) * falloff;
    }
  }
  ao = saturate(ao / float(HBAO_DIRECTIONS * HBAO_STEPS));

  // Same encoding split ssao.hlsl's ps_gtao uses: the classic target is
  // gamma-2-encoded so the exponent halves to express a linear-space
  // multiply there; the HDR target is pre-tonemap linear and takes it whole.
  float vis = saturate(1.0f - ao * 2.0f);
#ifdef HDR
  float a = pow(vis, p0.y);
#else
  float a = pow(vis, p0.y * 0.5f);
#endif
  a = lerp(a, 1.0f, protect);
  return lerp(a, 1.0f, fade);
}
