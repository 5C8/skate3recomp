// Authoritative character bone-palette serve (see the header).

#include "native/skate3_native_palette.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "skate3_native_scene.h"

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_palette_serve, true, "Skate 3",
                    "Serve character bone palettes from the game's own packed "
                    "m_matrices snapshots (taken at pack-function exit: the exact "
                    "bytes the game's VS constant upload consumes) instead of the "
                    "draw-time bank captures. Per-instance and per-piece, one sim "
                    "tick, coherent by construction. Items with no mapped snapshot "
                    "keep the capture pipeline's palette unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_palette_ident_primary, true, "Skate 3",
                    "Entity-exact palette picks: for items the ctx -> entity "
                    "identity store maps, a snapshot stamped with the SAME owning "
                    "entity is identity-proven; it is exempt from the "
                    "anchor/proximity caps (which exist only to reject other "
                    "characters' packs) and wins over any unstamped candidate the "
                    "content heuristics matched. Unmapped items and unstamped "
                    "candidates keep the content matching unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_palette_log_interval, 0, "Skate 3",
                     "Frames between palette-serve stats log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_palette {
namespace {

using skate3::native_scene::DrawItem;
using skate3::native_scene::FrameScene;
using skate3::native_scene::GuestTryCopy;

// cModelInstance field offsets (sizeof 0x28).
constexpr uint32_t kInstModel = 0x00;
constexpr uint32_t kInstArrCtx = 0x04;     // m_arrMeshContext begin
constexpr uint32_t kInstArrCtxEnd = 0x08;  // VectorBase end (validated, else 50-slot cap)
constexpr uint32_t kInstMatrices = 0x14;   // packed palette, 48-byte 3x4 rows
// sizeof(MeshContext) is 76 in this build, not retail's 80:
// cModelInstance::SetParamPointer (rex_rw_math_vpu_VVector4_SetParamPointer_cModelInstance_Render_Sk8_QAAXABV_PooledStringTemplate_0BI_0A_Util_0PBVVector4_vpu_math_rw_W4ParamLevel_012_I_Z_826F18E0) steps m_arrMeshContext by
// 76 per mesh, and the player skater's per-piece instances carry
// end = begin + 0x4C. At 80 every such window failed the span check and
// fell back to the 50-slot cap, so a piece's window swallowed the ctxs of
// the pieces allocated after it: the skateboard's 1-bone pack became a
// candidate for the 1-bone trucker hat, and the proximity pick locked the
// hat onto the board's root joint (the hat floating above the idle
// skater's head until skating broke the lock).
constexpr uint32_t kCtxStride = 0x50;
constexpr uint32_t kCtxSlotsFallback = 50;

bool LoadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw;
  if (!GuestTryCopy(&raw, base + addr, 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

// Plausible guest heap/image pointer (the guest address space tops out under
// 0x84A10000).
bool PlausiblePtr(uint32_t p) { return p >= 0x10000 && p < 0x84A10000 && (p & 3) == 0; }

std::mutex g_mu;  // guards every per-frame collection below

// Pack-exit palette snapshot: header + m_matrices rows copied into the
// per-frame arena the moment the game finished packing them; comparing at
// frame end is too late (m_matrices is repacked with the NEXT animation step
// by then), so the copy happens the moment the pack function returns: those
// bytes are exactly what the VS constant upload reads. An instance can pack
// more than once per frame (multi-pass poses); every snapshot is kept.
struct PackSnap {
  uint32_t inst = 0;
  // Owning presentation entity, stamped from the bracketed pack-owner
  // (every pack call sits inside an entity method whose r3 is the owner;
  // LivingWorld slices carry the writer's entity directly). 0 = the pack
  // fired outside any bracketed owner (unknown).
  uint32_t entity = 0;
  uint32_t arr_begin = 0;  // m_arrMeshContext block (ctx containment key)
  uint32_t arr_bytes = 0;
  uint32_t row_ofs = 0;  // offset into g_pack_rows (floats)
  uint32_t rows = 0;     // 48-byte 3x4 rows copied
  // Sk8::UpdateBoneTransforms part-record snapshot (the player skater's
  // pieces): rows serve only after the per-session layout vote confirms the
  // row interpretation (see ServeAuthoritativePalettes).
  bool ubt = false;
  // arr_bytes came from the instance's own validated m_arrMeshContext end
  // pointer (not the fixed-slot fallback): containment is then exact.
  bool window_exact = false;
};
std::vector<PackSnap> g_packs;
std::vector<float> g_pack_rows;  // host-order floats, 12 per row (arena)
// Previous frame's snapshots: the draws of frame N may consume the palette
// packed during frame N-1 (pack at frame end, upload next frame).
std::vector<PackSnap> g_packs_prev;
std::vector<float> g_pack_rows_prev;

uint64_t g_frames = 0;  // OnFrameBuilt calls (serve-history clock)

// Self-validation probe health: if the cModelInstance layout fails its
// probes the module turns itself off for the session (logged once); image
// drift must degrade, not misread.
std::atomic<bool> g_pal_ok{true};

// ---- serve stats (window; exchanged to 0 at each stats line) ---------------

std::atomic<uint64_t> c_pal_unreadable{0};
std::atomic<uint64_t> c_pal_served{0};
std::atomic<uint64_t> c_pal_serve_miss{0};
std::atomic<uint64_t> c_pal_hyst{0};
// Identity filter (ctx -> entity store): candidates stamped with a
// DIFFERENT owning entity than the item's are never served (the
// wrong-sibling/wrong-character serve class: garments tracking the wrong
// skater).
std::atomic<uint64_t> c_pal_serve_ident_skip{0};
// Content gate: candidates whose served range contains a degenerate
// (zeroed/garbage) bone row are never served; a partially-written
// m_matrices snapshotted around instance teardown/transition serves rows
// the game would never upload (weighted verts collapse to the origin).
std::atomic<uint64_t> c_pal_serve_bad_rows{0};
// Entity-exact primary picks: serves where the winning candidate carried the
// item's own entity stamp / serves of a mapped item that had to fall back to
// content matching / picks where the entity-exact candidate and the
// content-matched candidate held DIFFERENT rows; each divergence is a serve
// the proximity heuristics would have gotten wrong.
std::atomic<uint64_t> c_pal_ident_exact{0};
std::atomic<uint64_t> c_pal_ident_fallback{0};
std::atomic<uint64_t> c_pal_ident_prim_div{0};
std::atomic<uint64_t> c_ubt_snap_ok{0};
std::atomic<uint64_t> c_ubt_rej_ptrs{0};
std::atomic<uint64_t> c_ubt_arr_fallback{0};

// The entity whose pack-producing method is on this thread's call stack
// (the producing entity methods are bracketed by the hook layer).
thread_local uint32_t tl_pack_owner = 0;

// Sk8::UpdateBoneTransforms m_matrices layout vote: fresh bank captures
// score the UBT rows; >=75% near/exact over 24 votes locks the
// interpretation, otherwise UBT serving stays off.
std::atomic<int> g_ubt_vote48{0};
std::atomic<int> g_ubt_vote44{0};
std::atomic<int> g_ubt_vote_other{0};
// 0 = voting, 1 = packed 3x4, 2 = 4x4 Matrix44 (transpose on serve),
// -1 = refused (neither interpretation matched fresh bank captures).
std::atomic<int> g_ubt_layout{0};

// Highest blend index + 1 actually referenced by a mesh's vertices, cached
// per mesh (VB content is stable; the serve needs it to pick the right
// per-piece pack among an instance's multi-pass snapshots). g_mu-guarded.
std::unordered_map<uint32_t, uint32_t> g_needed_rows_cache;


// Self-validation probe for the cModelInstance layout: the first packed
// palette row must look like a column-vector affine (sane rotation norms,
// bounded translation). 16 samples; >4 failures turns the module off.
void ProbeInstance(const float* row) {
  static std::atomic<uint32_t> s_seen{0};
  static std::atomic<uint32_t> s_fail{0};
  const uint32_t n = s_seen.fetch_add(1, std::memory_order_relaxed);
  if (n >= 16) {
    return;
  }
  bool ok = true;
  // Packed 4x3 column-vector rows: [ r.x r.y r.z t ] x3; column norms of
  // the 3x3 block land in a scale band, translations stay world-plausible.
  for (int c = 0; c < 3 && ok; ++c) {
    const float norm = std::sqrt(row[c] * row[c] + row[4 + c] * row[4 + c] +
                                 row[8 + c] * row[8 + c]);
    ok = norm > 1e-3f && norm < 1e3f && std::isfinite(norm);
  }
  for (int r = 0; r < 3 && ok; ++r) {
    ok = std::isfinite(row[r * 4 + 3]) && std::fabs(row[r * 4 + 3]) < 1e6f;
  }
  if (!ok) {
    s_fail.fetch_add(1, std::memory_order_relaxed);
  }
  if (n == 15) {
    const uint32_t fails = s_fail.load(std::memory_order_relaxed);
    if (fails > 4) {
      g_pal_ok.store(false, std::memory_order_relaxed);
      REXLOG_WARN(
          "native-palette: cModelInstance layout FAILED validation ({}/16 bad "
          "first rows); palette serve disabled (image drift?)",
          fails);
    }
  }
}

// Copy one instance's header + m_matrices rows. ubt = the record came from
// an UpdateBoneTransforms parts array (stride 0x28, same header layout),
// snapshotted at the hook because the parts arena AND its m_matrices buffers
// zero by frame end.
void SnapshotPack(uint8_t* base, uint32_t inst, bool ubt = false) {
  uint32_t head[0x28 / 4];
  if (!GuestTryCopy(head, base + inst, sizeof(head))) {
    return;
  }
  const uint32_t model = __builtin_bswap32(head[kInstModel / 4]);
  const uint32_t arr = __builtin_bswap32(head[kInstArrCtx / 4]);
  const uint32_t arr_end = __builtin_bswap32(head[kInstArrCtxEnd / 4]);
  const uint32_t matrices = __builtin_bswap32(head[kInstMatrices / 4]);
  const uint32_t count = __builtin_bswap32(head[0x18 / 4]);
  if (!PlausiblePtr(model) || !PlausiblePtr(arr) || !PlausiblePtr(matrices)) {
    if (ubt) {
      c_ubt_rej_ptrs.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  PackSnap snap;
  snap.inst = inst;
  snap.entity = tl_pack_owner;
  snap.arr_begin = arr;
  snap.ubt = ubt;
  if (arr_end > arr && (arr_end - arr) % kCtxStride == 0 &&
      (arr_end - arr) / kCtxStride <= 256) {
    snap.arr_bytes = arr_end - arr;
    snap.window_exact = true;
  } else if (ubt) {
    // Live UBT arena records don't always carry a populated end pointer.
    // Structurally each part owns exactly ONE MeshContext, so a one-slot
    // window is the safe assumption; it can only ever match the piece's
    // own ctx.
    snap.arr_bytes = kCtxStride;
    c_ubt_arr_fallback.fetch_add(1, std::memory_order_relaxed);
  } else {
    snap.arr_bytes = kCtxSlotsFallback * kCtxStride;
  }
  // +0x18 tracked as the pack count: it equals the highest blend index the
  // mesh references, so it is trusted within bounds.
  snap.rows = (count >= 1 && count <= 96) ? count : 84;
  // UBT snapshots copy 16 floats/row: rw::math::vpu::Matrix44 is a 64-byte
  // 4x4, and UpdateBoneTransforms (unlike PackAndMultiply) may leave
  // m_matrices in that shape; the serve's layout vote evaluates BOTH the
  // packed-3x4 (12-float rows over the same contiguous span) and the
  // 4x4-transposed interpretation from this one copy.
  const uint32_t fpr = ubt ? 16u : 12u;
  uint32_t raw[96 * 16];
  if (!GuestTryCopy(raw, base + matrices, size_t(snap.rows) * fpr * 4)) {
    c_pal_unreadable.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_packs.size() >= 512 || g_pack_rows.size() > 512 * 1024) {
    return;  // growth cap (scene path off / frame never built)
  }
  snap.row_ofs = uint32_t(g_pack_rows.size());
  g_pack_rows.resize(g_pack_rows.size() + size_t(snap.rows) * fpr);
  float* dst = g_pack_rows.data() + snap.row_ofs;
  for (uint32_t i = 0; i < snap.rows * fpr; ++i) {
    dst[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  g_packs.push_back(snap);
  if (ubt) {
    c_ubt_snap_ok.fetch_add(1, std::memory_order_relaxed);
  }
  // Layout self-probe on the first pack-hook snapshot only: UBT rows carry
  // their own vote-based validation (a failed probe here would kill the
  // whole module for the session on an unproven layout).
  static std::atomic<bool> s_probed{false};
  if (!ubt && !s_probed.exchange(true)) {
    ProbeInstance(dst);
  }
}

// LOD pedestrians. The LivingWorld pack writer fuses UpdateBoneTransforms +
// PackAndMultiply for cLivingWorldPresEntity-derived classes (their vtable
// slot +16 is an Update thunk, so they never reach the pack/UBT hooks) and
// writes the CONCATENATION of per-mesh packed palettes into m_matrices;
// mesh k's rows start at sum(boneCount(0..k-1)). Each mesh slice becomes
// its own PackSnap keyed to exactly its MeshContext, so the containment
// matching works unchanged.
void SnapshotLwEntity(uint8_t* base, uint32_t entity) {
  uint32_t idx = 0, count = 0, arr = 0;
  if (!LoadU32(base, entity + 16, &idx) || idx > 1024 ||
      !LoadU32(base, entity + (idx + 5) * 4, &count) ||
      !LoadU32(base, entity + (idx + 8) * 4, &arr) || count == 0 || count > 8 ||
      !PlausiblePtr(arr)) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = arr + i * 0x28;
    uint32_t head[0x28 / 4];
    if (!GuestTryCopy(head, base + rec, sizeof(head))) {
      continue;
    }
    const uint32_t model = __builtin_bswap32(head[kInstModel / 4]);
    const uint32_t actx = __builtin_bswap32(head[kInstArrCtx / 4]);
    const uint32_t matrices = __builtin_bswap32(head[kInstMatrices / 4]);
    if (!PlausiblePtr(model) || !PlausiblePtr(actx) || !PlausiblePtr(matrices)) {
      continue;
    }
    // Model walk mirrors the writer's own: mesh table at +0x24 (ptr array,
    // stride 4), mesh count u16 at +0x32, bone-count gate u16 at +0x34
    // (<= 80, same gate the writer applies). Every bone count must land in
    // 1..96; a wrong table shape fails here and the instance is skipped.
    uint32_t count_w = 0, table = 0, gate_w = 0;
    if (!LoadU32(base, model + 0x30, &count_w) ||
        !LoadU32(base, model + 0x24, &table) ||
        !LoadU32(base, model + 0x34, &gate_w)) {
      continue;
    }
    const uint32_t nmesh = count_w & 0xFFFF;
    if (nmesh == 0 || nmesh > 16 || (gate_w >> 16) > 80) {
      continue;
    }
    uint32_t bones[16];
    uint32_t total = 0;
    bool ok = true;
    for (uint32_t k = 0; k < nmesh && ok; ++k) {
      uint32_t mesh = 0, bc = 0;
      ok = LoadU32(base, table + k * 4, &mesh) && PlausiblePtr(mesh) &&
           LoadU32(base, mesh + 0x48, &bc) && bc >= 1 && bc <= 96;
      if (ok) {
        bones[k] = bc;
        total += bc;
      }
    }
    if (!ok || total == 0 || total > 96) {
      continue;
    }
    uint32_t raw[96 * 12];
    if (!GuestTryCopy(raw, base + matrices, size_t(total) * 48)) {
      continue;
    }
    // Byteswap OUTSIDE g_mu: this runs on the sim thread, and holding the
    // lock through a multi-KB swap loop stalls the guest render thread's
    // per-frame palette consumers.
    float host[96 * 12];
    for (uint32_t f = 0; f < total * 12; ++f) {
      host[f] = std::bit_cast<float>(__builtin_bswap32(raw[f]));
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_packs.size() + nmesh > 512 || g_pack_rows.size() > 512 * 1024) {
      return;
    }
    const uint32_t ofs = uint32_t(g_pack_rows.size());
    g_pack_rows.resize(g_pack_rows.size() + size_t(total) * 12);
    std::memcpy(g_pack_rows.data() + ofs, host, size_t(total) * 12 * sizeof(float));
    uint32_t row = 0;
    for (uint32_t k = 0; k < nmesh; ++k) {
      PackSnap snap;
      snap.inst = rec;
      snap.entity = entity;
      snap.arr_begin = actx + k * kCtxStride;  // exactly this mesh's ctx
      snap.arr_bytes = kCtxStride;
      snap.row_ofs = ofs + row * 12;
      snap.rows = bones[k];
      g_packs.push_back(snap);
      row += bones[k];
    }
  }
}

// The pack of the cModelInstance whose m_arrMeshContext window holds ctx
// (see ServeAuthoritativePalettesImpl). g_mu held by the caller.
const PackSnap* FindExactPack(uint32_t ctx, const float** out_rows) {
  skate3::native_entity::CtxInfo ident;
  const bool ident_mapped = skate3::native_entity::LookupCtx(ctx, &ident);
  const PackSnap* pick = nullptr;
  for (int set = 0; set < 2 && pick == nullptr; ++set) {
    const auto& packs = set == 0 ? g_packs : g_packs_prev;
    const auto& rows = set == 0 ? g_pack_rows : g_pack_rows_prev;
    for (const PackSnap& snap : packs) {
      if (snap.ubt || !snap.window_exact) {
        continue;
      }
      if (!(ctx >= snap.arr_begin && ctx < snap.arr_begin + snap.arr_bytes &&
            (ctx - snap.arr_begin) % kCtxStride == 0)) {
        continue;
      }
      if (ident_mapped && ident.entity != 0 && snap.entity != 0 &&
          snap.entity != ident.entity) {
        c_pal_serve_ident_skip.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      pick = &snap;  // keep overwriting: the frame's last pack wins
      *out_rows = rows.data() + snap.row_ofs;
    }
  }
  return pick;
}

uint32_t ServeAuthoritativePalettesImpl(uint8_t* base,
                                        skate3::native_scene::FrameScene& scene) {
  (void)base;
  if (!REXCVAR_GET(skate3_native_render_scene_palette_serve) ||
      !g_pal_ok.load(std::memory_order_relaxed)) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_packs.empty() && g_packs_prev.empty()) {
    return 0;
  }
  // The game's own link, exactly: a MeshContext renders with the palette of
  // the cModelInstance whose m_arrMeshContext window holds it (BindConstants
  // binds per instance record; PackAndMultiplyMatricesForUpload packs that
  // instance's m_matrices in place right before the upload). Serve that
  // pack: the frame's LAST pack of the instance (the main pass packs last),
  // this frame's set first, the previous frame's when the instance did not
  // pack this frame. A pack stamped with a different owning entity than the
  // ctx's known entity is not this piece's. No content / proximity / count
  // matching: a piece without an exact containing pack keeps its draw-time
  // capture (the bank the game's own draw consumed).
  uint32_t served = 0;
  for (DrawItem& item : scene.items) {
    if (!item.skinned || item.ctx == 0 || item.dbg_src == 10) {
      continue;
    }
    skate3::native_entity::CtxInfo ident;
    const bool ident_mapped = skate3::native_entity::LookupCtx(item.ctx, &ident);
    const float* pick_rows = nullptr;
    const PackSnap* pick = FindExactPack(item.ctx, &pick_rows);
    if (pick == nullptr) {
      c_pal_serve_miss.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    constexpr size_t kPaletteFloats = 84 * 12;
    if (item.bones.size() != kPaletteFloats) {
      item.bones.assign(kPaletteFloats, 0.0f);
    }
    const uint32_t n = std::min<uint32_t>(pick->rows, 84u);
    std::memcpy(item.bones.data(), pick_rows, size_t(n) * 12 * sizeof(float));
    item.bone_rows = uint16_t(n);
    // Rows beyond the game's packed count are never indexed by the mesh.
    std::fill(item.bones.begin() + size_t(n) * 12, item.bones.end(), 0.0f);
    item.caster_bank = false;
    item.pending = false;
    item.dbg_src = 11;
    if (ident_mapped) {
      c_pal_ident_exact.fetch_add(1, std::memory_order_relaxed);
    }
    ++served;
  }
  c_pal_served.fetch_add(served, std::memory_order_relaxed);
  return served;
}

void MaybeLog() {
  const int32_t interval =
      REXCVAR_GET(skate3_native_render_scene_palette_log_interval);
  if (interval <= 0 || g_frames % uint64_t(interval) != 0) {
    return;
  }
  const auto x = [](std::atomic<uint64_t>& a) {
    return a.exchange(0, std::memory_order_relaxed);
  };
  REXLOG_INFO(
      "native-palette: frame {} srv[hit={} miss={} hyst={} ident_skip={} "
      "bad_rows={} eprim={}/{}/{} unread={}] ubt[48={} 44={} o={} lay={} "
      "snap={} arr={} rej={}]",
      g_frames, x(c_pal_served), x(c_pal_serve_miss), x(c_pal_hyst),
      x(c_pal_serve_ident_skip), x(c_pal_serve_bad_rows), x(c_pal_ident_exact),
      x(c_pal_ident_fallback), x(c_pal_ident_prim_div), x(c_pal_unreadable),
      g_ubt_vote48.load(std::memory_order_relaxed),
      g_ubt_vote44.load(std::memory_order_relaxed),
      g_ubt_vote_other.load(std::memory_order_relaxed),
      g_ubt_layout.load(std::memory_order_relaxed), x(c_ubt_snap_ok),
      x(c_ubt_arr_fallback), x(c_ubt_rej_ptrs));
}

bool CaptureActive() {
  return REXCVAR_GET(skate3_native_render_scene_palette_serve) &&
         g_pal_ok.load(std::memory_order_relaxed);
}

}  // namespace

uint32_t ServeAuthoritativePalettes(uint8_t* base,
                                    skate3::native_scene::FrameScene& scene) {
  return ServeAuthoritativePalettesImpl(base, scene);
}

bool ServeCtxPalette(uint32_t ctx, std::vector<float>* bones) {
  if (ctx == 0 || !CaptureActive()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const float* rows = nullptr;
  const PackSnap* pick = FindExactPack(ctx, &rows);
  if (pick == nullptr) {
    return false;
  }
  const uint32_t n = std::min<uint32_t>(pick->rows, 84u);
  bones->assign(size_t(84) * 12, 0.0f);
  std::memcpy(bones->data(), rows, size_t(n) * 12 * sizeof(float));
  return true;
}

void PurgeRebuiltMesh(uint32_t mesh, uint32_t ctx) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_needed_rows_cache.erase(mesh);
  (void)ctx;
}

uint32_t ExchangePackOwner(uint32_t entity) {
  const uint32_t prev = tl_pack_owner;
  tl_pack_owner = entity;
  return prev;
}

void OnPackPalette(uint8_t* base, uint32_t instance) {
  if (instance == 0 || !CaptureActive()) {
    return;
  }
  SnapshotPack(base, instance);
}

void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count) {
  if (parts == 0 || count == 0 || count > 64 || !CaptureActive()) {
    return;
  }
  // Snapshot every part record NOW: the parts array is per-frame arena
  // scratch (all fields read zero by frame end), so this hook exit is the
  // only moment the player skater's per-piece palettes are readable. Each
  // part becomes a per-piece PackSnap (its ctx window is exactly one
  // MeshContext), which maps the player through the same containment
  // matching as every other instance.
  // Not served: the player's arena part records carry no persistent
  // MeshContext, so there is no exact piece link; those pieces keep their
  // draw-time capture (the bank the game's own draw consumed).
  (void)base;
  (void)parts;
  (void)count;
}

void OnLwPack(uint8_t* base, uint32_t entity) {
  if (entity == 0 || !CaptureActive()) {
    return;
  }
  SnapshotLwEntity(base, entity);
}

void OnFrameBuilt() {
  ++g_frames;
  {
    // Rotate this frame's snapshots into the previous-frame slot: the serve
    // at frame N reads g_packs_prev from frame N-1.
    std::lock_guard<std::mutex> lock(g_mu);
    g_packs_prev = std::move(g_packs);
    g_pack_rows_prev = std::move(g_pack_rows);
    g_packs.clear();
    g_pack_rows.clear();
  }
  MaybeLog();
}

}  // namespace skate3::native_palette
