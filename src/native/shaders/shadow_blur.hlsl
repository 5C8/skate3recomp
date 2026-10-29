// Shadow blur: the game's shadow_h/vblur passes, exact semantics from the
// disassembled Xenos ucode. One fullscreen-triangle draw
// per tile per direction; taps stay inside the tile (clamped). Coverage is
// Gaussian-blurred; depth keeps the exact center value where covered and
// dilates (binary-coverage-weighted neighbour average) into the penumbra
// otherwise, so depth compares stay valid across the blurred edge.
// b0 floats: c0 = (dir.x, dir.y, ntaps, src_is_raw), c1 = (w0, w1, w2, 0),
// c2 = (tile_x0, tile_x1, tile_y1, 0). ntaps 0 = format-convert only
// (cascade 2 is never blurred).
cbuffer C : register(b0) {
  float4 c0;
  float4 c1;
  float4 c2;
};
Texture2D<float2> src : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps_main(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  int ntaps = int(c0.z);
  float w[3] = {c1.x, c1.y, c1.z};
  float2 center = src.Load(int3(p, 0));
  float ccen = c0.w > 0.0 ? 1.0 - center.y : center.y;
  float cov = 0.0;
  float dsum = 0.0;
  float dcnt = 0.0;
  [unroll] for (int k = -2; k <= 2; ++k) {
    if (abs(k) > ntaps) continue;
    int2 q = p + int2(c0.xy) * k;
    q.x = clamp(q.x, int(c2.x), int(c2.y));
    q.y = clamp(q.y, 0, int(c2.z));
    float2 t = src.Load(int3(q, 0));
    float c = c0.w > 0.0 ? 1.0 - t.y : t.y;
    cov += w[abs(k)] * c;
    if (k != 0) {
      float hard = c > 0.001 ? 1.0 : 0.0;
      dcnt += hard;
      dsum += hard * t.x;
    }
  }
  float depth = ccen > 0.001 ? center.x : (dcnt > 0.0 ? dsum / dcnt : center.x);
  return float2(depth, saturate(cov));
}

// The skate.Park world-shadow map's (tf2, "DMO Exponential Atlas")
// log-space blur: the game's shadow_{h,v}blur_exp_{5,3}PS, run after each
// tile's caster pass (5 taps on the inner tile, 3 on the outer). Per the
// ucode, with k = the blur bank's c1.z (= the receivers' c10.z):
//   out = (1/k) * ln( sum_i w_i * exp(k * d_i) )          (hblur 8-20)
// i.e. the exponential-shadow-map prefilter: the receivers' later
// exp(k * (stored - ray)) then sees the filtered exp(k * d). Evaluated
// relative to the centre depth (identical algebra) because exp(88 * d)
// sits at the edge of float range. Taps clamp inside the tile.
// b0 floats: c0 = (dir.x, dir.y, ntaps, k), c1 = (w0, w1, w2, 0),
// c2 = (tile_x0, tile_x1, tile_y1, 0).
float2 ps_exp(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  int ntaps = int(c0.z);
  float k = c0.w;
  float w[3] = {c1.x, c1.y, c1.z};
  float dc = src.Load(int3(p, 0)).x;
  float sum = 0.0;
  [unroll] for (int t = -2; t <= 2; ++t) {
    if (abs(t) > ntaps) continue;
    int2 q = p + int2(c0.xy) * t;
    q.x = clamp(q.x, int(c2.x), int(c2.y));
    q.y = clamp(q.y, 0, int(c2.z));
    float d = src.Load(int3(q, 0)).x;
    sum += w[abs(t)] * exp(k * (d - dc));
  }
  float v = dc + log(max(sum, 1e-30)) / k;
  return float2(v, v);
}
