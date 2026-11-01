#include "skate3_native_render.h"

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
#include "skate3_native_scene.h"
#include "skate3_guest_aspect.h"
#include "skate3_vr.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")

#include <dbghelp.h>
#include <xmmintrin.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(skate3_native_render, true, "Skate 3",
                    "Enable the Skate 3 data-driven native renderer hook layer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_native_render_log_interval, 0, "Skate 3",
                     "Frames between native-render hook liveness log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull_guest);
REXCVAR_DEFINE_DOUBLE(skate3_guest_fps_cap, 0.0, "Skate 3",
                      "Pace the guest render loop to this frame rate (0 = uncapped). The "
                      "guest produces frames at irregular 2-9 ms intervals; the display "
                      "(especially with G-Sync/VRR, which follows present times directly) "
                      "turns that variance into visible irregular judder that no content "
                      "smoothing can fix. An even cap a few fps below the display refresh "
                      "(e.g. 140 on a 144 Hz panel) is the standard VRR recipe: every "
                      "frame arrives on a steady beat. Precise pacing: coarse sleep to "
                      "~1.5 ms before the target, then spin.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_guest_fps_cap_auto, false, "Skate 3",
                    "Derive the guest frame cap from the display the window is on: "
                    "cap a safety margin below the refresh rate (4 fps or 5%, "
                    "whichever is larger; see rex::ui::Window::AutoFrameCapHz), "
                    "the VRR recipe above, without hand-tuning per monitor. Above "
                    "the display refresh the extra frames cannot be shown anyway; "
                    "refreshes beat-sample the frame stream and steady motion "
                    "judders (measured: a mathematically perfect synthetic pan "
                    "judders at 330 fps on a 144 Hz panel and is smooth capped "
                    "below it). The margin must also absorb swap-to-present "
                    "jitter: presents run with tearing allowed, so a present "
                    "landing inside the panel's minimum refresh period tears even "
                    "under VRR. Overrides skate3_guest_fps_cap while the display "
                    "refresh is known. "
                    "OFF by default here: the cap is a smoothness aid, not a "
                    "performance setting, and leaving it on makes the game look "
                    "like it tops out a few fps under the panel's refresh (56 on "
                    "a 60 Hz display, 137 on 144 Hz). Turn it on if you want the "
                    "VRR pacing behaviour described above.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);


namespace skate3::native_render {
namespace {

// One per-mesh submission. kind 0 = RenderMesh (dynamic entities: a = the
// MeshContext, b = VertexProgramState, c = dynitem index+1). kind 1 =
// SceneRenderView sort-list entry (a = MeshContext, b = list offset from the
// view, c = view). kind 2 = world-path capture (skinned / model-space prop:
// a = MeshContext, b = submitting view, c = dynitem index+1). kind 3 =
// quad-list DrawVertices capture (a = synthetic key, c = dynitem index+1).
using RenderMeshRecord = skate3::native_scene::SubmitRecord;


std::mutex g_mutex;
std::vector<RenderMeshRecord> g_current_frame;
uint64_t g_frame_index = 0;
std::atomic<bool> g_announced{false};


bool Enabled() {
  return REXCVAR_GET(skate3_native_render);
}





void OnRenderMesh(uint8_t* base, uint32_t mesh_context, uint32_t vertex_program_state,
                  bool drew_inside) {
  const uint32_t dyn = skate3::native_scene::CaptureDynamicState(
      base, mesh_context, /*world_path=*/false, drew_inside);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({0, mesh_context, vertex_program_state, dyn});
}


// SceneRenderView draw-list renderer sub_827FAF50(view, sort_vec, first, count):
// sort_vec points at an eastl vector whose [0] is the entry array; entries are
// 8 bytes {u32 sort_key, MeshContext*}.
//
// Skinned entries and rigid MODEL-SPACE props (vending machines and other
// movables that never reach RenderMesh) are captured HERE, before the
// dispatcher draws the list. The captures are transform-pending; the
// post-draw fixup attaches the palette / world matrix at whichever draw
// eventually consumes the mesh's buffers.
void OnSceneDrawList(uint8_t* base, uint32_t view, uint32_t sort_vec, uint32_t first,
                     uint32_t count) {
  if (count == 0 || count > 100000) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return;
  }
  // b = which of the view's sort lists this came from (sort_vec - view), so
  // the scene builder can select the primary opaque list (+20176).
  const uint32_t list_offset = sort_vec - view;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + (first + i) * 8;
    const uint32_t mesh_context = REX_LOAD_U32(entry + 4);
    if (mesh_context == 0) {
      continue;
    }
    g_current_frame.push_back({1, mesh_context, list_offset, view});
    const uint32_t dyn =
        skate3::native_scene::CaptureDynamicState(base, mesh_context, /*world_path=*/true);
    if (dyn != 0) {
      // b = the submitting view: shadow-cascade views submit their own
      // contexts for the same NPCs; rendering those creates ghost
      // duplicates (torso-less: their deferred skin passes never run).
      g_current_frame.push_back({2, mesh_context, view, dyn});
    }
  }
}

// ---- Guest-side occlusion dispatch filter ---------------------------------
// The native renderer suppresses the guest's emulated draws, so for world
// statics the sorted-list dispatch below (material setup, command-packet
// building) produces nothing anyone consumes - yet it is the guest render
// thread's dominant per-item cost. For MeshContexts the render-side
// occlusion cull proved hidden last frame (published by RenderScene), the
// hook compacts the entry segment before invoking the guest dispatcher and
// restores it afterwards. Capture above has already recorded every entry,
// so scene.items, the shadow caster caches, and all state capture stay
// complete; the only guest work skipped is packet-building for draws that
// were both suppressed and occlusion-culled anyway. Guest render thread
// only.
struct OcclDispatchFilter {
  uint64_t stamp = ~0ull;          // g_frame_index of the ctx snapshot
  std::vector<uint32_t> ctxs;      // sorted culled-ctx snapshot
  std::vector<uint8_t> saved;      // original segment bytes for restore
  uint32_t saved_addr = 0;
  uint32_t saved_bytes = 0;
  bool active = false;             // a filtered dispatch is in flight
};
OcclDispatchFilter g_occl_filter;

// Compacts entries[first..first+count) in place, dropping culled ctxs, and
// returns the kept count. Returns `count` unchanged (nothing saved) when
// filtering is off, stale, empty, or nothing matched.
uint32_t FilterSceneDrawList(uint8_t* base, uint32_t sort_vec, uint32_t first,
                             uint32_t count) {
  OcclDispatchFilter& f = g_occl_filter;
  if (f.active || count == 0 || count > 100000 ||
      !REXCVAR_GET(skate3_native_render_scene_occlusion_cull_guest)) {
    return count;
  }
  if (f.stamp != g_frame_index) {
    f.stamp = g_frame_index;
    skate3::native_scene::CopyOcclusionCulledCtxs(f.ctxs);
  }
  if (f.ctxs.empty()) {
    return count;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return count;
  }
  const uint32_t seg = entries + first * 8;
  f.saved.assign(base + seg, base + seg + size_t(count) * 8);
  uint32_t kept = 0;
  uint32_t skipped = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* src = f.saved.data() + size_t(i) * 8;
    uint32_t ctx_be;
    std::memcpy(&ctx_be, src + 4, 4);
    const uint32_t mesh_context = __builtin_bswap32(ctx_be);
    if (mesh_context != 0 &&
        std::binary_search(f.ctxs.begin(), f.ctxs.end(), mesh_context)) {
      ++skipped;
      continue;
    }
    if (kept != i) {
      std::memcpy(base + seg + size_t(kept) * 8, src, 8);
    }
    ++kept;
  }
  if (skipped == 0) {
    f.saved.clear();
    return count;
  }
  f.active = true;
  f.saved_addr = seg;
  f.saved_bytes = count * 8;
  skate3::native_scene::AddGuestOcclSkipped(skipped);
  return kept;
}

void RestoreSceneDrawList(uint8_t* base) {
  OcclDispatchFilter& f = g_occl_filter;
  if (!f.active) {
    return;
  }
  std::memcpy(base + f.saved_addr, f.saved.data(), f.saved_bytes);
  f.active = false;
}

// Non-indexed cloth patch draws (see native_scene::CaptureClothDraw). The
// synthetic "context" key is the dynamic buffer object, stable per garment.
void OnClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  uint32_t key = 0;
  const uint32_t dyn = skate3::native_scene::CaptureClothDraw(base, r4, r5, r6, r7, &key);
  if (dyn == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({3, key, 0, dyn});
}

// Precise guest frame pacing (see skate3_guest_fps_cap): called on the guest
// render thread at the swap boundary. Absolute-schedule pacing (target +=
// interval) so sleep jitter never accumulates; resyncs when the guest falls
// more than one interval behind (loads, hitches).
void PaceGuestFrame() {
  double cap = REXCVAR_GET(skate3_guest_fps_cap);
  if (REXCVAR_GET(skate3_guest_fps_cap_auto)) {
    // Refresh-derived cap (see the cvar). Falls through to the explicit cap
    // while the platform hasn't reported a refresh rate.
    const double auto_cap = double(rex::ui::Window::AutoFrameCapHz(
        rex::ui::Window::CachedDisplayRefreshHz()));
    if (auto_cap > 0.0) {
      cap = auto_cap;
    }
  }
  // VR: the headset compositor paces every frame (WaitGetPoses blocks until
  // the headset wants the next one) and inter-frames do not run there, so a
  // desktop cap only throttles the headset below its refresh (a 60 cap left
  // a 90 Hz headset reprojecting every other frame).
  if (skate3::vr::Active()) {
    cap = 0.0;
  }
  static std::chrono::steady_clock::time_point s_next{};
  if (cap < 1.0) {
    s_next = {};
    return;
  }
  const auto interval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / cap));
  const auto now = std::chrono::steady_clock::now();
  if (s_next.time_since_epoch().count() == 0 || now > s_next + interval) {
    s_next = now + interval;
    return;
  }
  // Coarse sleep to ~1.5 ms before the target, then spin for precision.
  while (true) {
    const auto remaining = s_next - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      break;
    }
    if (remaining > std::chrono::milliseconds(2)) {
      std::this_thread::sleep_for(remaining - std::chrono::milliseconds(2));
    } else if (remaining > std::chrono::microseconds(50)) {
      std::this_thread::yield();
    }
  }
  s_next += interval;
}

// Guest-thread hook profiler (perf_log only; the flag is sampled once per
// guest frame). Buckets: 0 = our OnDrawDone, 1 = the game's own D3D draw
// call (recompiled D3D + the SDK's command-stream build), 2 = our
// OnVsConstantUpload, 3 = the game's SetPending_AluConstants. Guest render
// thread only.
enum HookBucket { kHookDrawOurs = 0, kHookDrawGame, kHookVsOurs, kHookVsGame, kHookCount };
bool g_hook_prof = false;
double g_hook_ns[kHookCount] = {};
uint64_t g_hook_n[kHookCount] = {};
struct HookTimer {
  int b;
  std::chrono::steady_clock::time_point t0;
  explicit HookTimer(int bucket) : b(g_hook_prof ? bucket : -1) {
    if (b >= 0) t0 = std::chrono::steady_clock::now();
  }
  ~HookTimer() {
    if (b >= 0) {
      g_hook_ns[b] +=
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
      ++g_hook_n[b];
    }
  }
};

void OnFrameEnd(uint8_t* base) {
  const auto pace0 = std::chrono::steady_clock::now();
  PaceGuestFrame();
  const double pace_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - pace0).count();
  // EMULATED-mode guest frame breakdown (emulated gameplay once regressed
  // from 140 to 66 fps while native stayed at cap; the native-scene perf
  // line only prints while the native renderer is active, so emulated
  // stretches had no perf visibility at all). Logs every
  // ~600 guest frames while the native scene is OFF: whole-frame time (the
  // fps), time spent inside BuildFrameScene, and the per-frame draw/upload
  // hook traffic. If frame_avg is ~15 ms but build/hooks are small, the
  // cost is in the emulated GPU/CP pipeline, not our guest-thread code.
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_bd_prev{};
  static uint64_t s_bd_frames = 0;
  static double s_bd_frame_ns = 0, s_bd_frame_max = 0;
  static double s_bd_build_ns = 0, s_bd_build_max = 0;
  static double s_bd_pace_ns = 0, s_bd_capend_ns = 0;
  static uint64_t s_bd_draws0 = 0;
  // Guest-frame breakdown in BOTH modes (perf_log): with the native scene on
  // it shows how much of each game frame our guest-thread work takes
  // (BuildFrameScene, capture bookkeeping, the pacer) versus the game itself
  // plus any wait on the GPU-command thread.
  const bool native_on = skate3::native_scene::Enabled();
  const bool emu_profile = REXCVAR_GET(skate3_native_render_scene_perf_log);
  g_hook_prof = emu_profile;
  skate3::native_scene::EnableDrawSectProfiling(emu_profile);
  if (emu_profile) {
    s_bd_pace_ns += pace_ns;
  }
  const auto bd_now = Clock::now();
  if (emu_profile && s_bd_prev.time_since_epoch().count() != 0) {
    const double dt =
        std::chrono::duration<double, std::nano>(bd_now - s_bd_prev).count();
    s_bd_frame_ns += dt;
    s_bd_frame_max = std::max(s_bd_frame_max, dt);
  }
  s_bd_prev = bd_now;

  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame_index;
  const size_t mesh_count = g_current_frame.size();

  const auto bd_build0 = Clock::now();
  skate3::native_scene::BuildFrameScene(base, g_current_frame.data(),
                                        g_current_frame.size());
  if (emu_profile) {
    const double bns = std::chrono::duration<double, std::nano>(Clock::now() -
                                                                bd_build0)
                           .count();
    s_bd_build_ns += bns;
    s_bd_build_max = std::max(s_bd_build_max, bns);
    if (++s_bd_frames >= 600) {
      const uint64_t draws = skate3::native_scene::DrawSequence();
      // Quantum probe: a measured 1 ms sleep + the kernel's current timer
      // resolution. frame avg ~15 ms with build ~0 and the GPU at idle
      // wattage is the signature of every pacing sleep rounding up to the
      // 15.625 ms default Windows quantum; sleep1ms ~15.6 here proves it,
      // ~1-2 ms refutes it.
      const auto sp0 = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const double sleep_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - sp0).count();
      double timer_res_ms = -1.0;
#if defined(_WIN32)
      {
        using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
        static const auto query = reinterpret_cast<NtQueryTimerResolutionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
        if (query != nullptr) {
          ULONG min_res = 0, max_res = 0, cur_res = 0;
          if (query(&min_res, &max_res, &cur_res) == 0) {
            timer_res_ms = double(cur_res) / 10000.0;  // 100 ns units
          }
        }
      }
#endif
      const double fr = s_bd_frame_ns / s_bd_frames / 1e6;
      const double bd = s_bd_build_ns / s_bd_frames / 1e6;
      const double pc = s_bd_pace_ns / s_bd_frames / 1e6;
      const double ce = s_bd_capend_ns / s_bd_frames / 1e6;
      REXLOG_INFO(
          "native-render {} guest-frame breakdown: frame avg={:.2f}/max={:.2f}ms "
          "(= {:.0f} fps) | build avg={:.3f}/max={:.3f}ms pace={:.3f}ms capend={:.3f}ms "
          "-> game+wait={:.2f}ms | draws/frame={} sleep1ms={:.2f}ms timer_res={:.2f}ms "
          "(600-frame window)",
          native_on ? "NATIVE" : "EMULATED", fr, s_bd_frame_max / 1e6,
          fr > 0.0 ? 1000.0 / fr : 0.0, bd, s_bd_build_max / 1e6, pc, ce,
          fr - bd - pc - ce, (draws - s_bd_draws0) / s_bd_frames, sleep_ms, timer_res_ms);
      const auto per_frame = [&](int b) { return g_hook_ns[b] / s_bd_frames / 1e6; };
      const auto per_call = [&](int b) {
        return g_hook_n[b] != 0 ? g_hook_ns[b] / double(g_hook_n[b]) / 1e3 : 0.0;
      };
      REXLOG_INFO(
          "native-render guest-thread hooks per frame: draw[ours={:.3f}ms ({:.2f}us x {}) "
          "game_d3d={:.3f}ms ({:.2f}us)] vsconst[ours={:.3f}ms ({:.2f}us x {}) "
          "game_d3d={:.3f}ms] | OnDrawDone sections:{} | BuildFrameScene sections:{}",
          per_frame(kHookDrawOurs), per_call(kHookDrawOurs), g_hook_n[kHookDrawOurs] / s_bd_frames,
          per_frame(kHookDrawGame), per_call(kHookDrawGame), per_frame(kHookVsOurs),
          per_call(kHookVsOurs), g_hook_n[kHookVsOurs] / s_bd_frames, per_frame(kHookVsGame),
          skate3::native_scene::DrawSectionShares(emu_profile),
          skate3::native_scene::BuildSectionShares());
      for (int b = 0; b < kHookCount; ++b) {
        g_hook_ns[b] = 0.0;
        g_hook_n[b] = 0;
      }
      s_bd_frames = 0;
      s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
      s_bd_pace_ns = s_bd_capend_ns = 0;
      s_bd_draws0 = draws;
    }
  } else {
    s_bd_frames = 0;
    s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
    s_bd_pace_ns = s_bd_capend_ns = 0;
    s_bd_draws0 = skate3::native_scene::DrawSequence();
  }

  const int32_t log_interval = REXCVAR_GET(skate3_native_render_log_interval);
  if (log_interval > 0 && g_frame_index % static_cast<uint64_t>(log_interval) == 0) {
    REXLOG_INFO("native-render frame={} meshes={} snapshot_done={}", g_frame_index,
                mesh_count, skate3::native_scene::SnapshotWritten());
  }

  const auto ce0 = Clock::now();
  skate3::native_scene::OnCaptureFrameEnd(base, g_frame_index,
                                           g_current_frame);
  if (emu_profile) {
    s_bd_capend_ns += std::chrono::duration<double, std::nano>(Clock::now() - ce0).count();
  }

  g_current_frame.clear();
}

}  // namespace

void Install() {
  if (!Enabled()) {
    return;
  }
  if (!g_announced.exchange(true)) {
    REXLOG_INFO(
        "native-render hook layer enabled");
  }
  skate3::native_scene::Install();
}

}  // namespace skate3::native_render

// "RenderMesh" per-visible-mesh submission for dynamic entities (characters,
// props). Actual convention (verified via recompiled code + snapshot):
// r3 = MeshContext*, r4 = renderengine::VertexProgramState*.
//
// The dynamic state snapshot must be taken AFTER the original call: the
// mesh's VS constants (instance world matrix, bone palette) are only flushed
// through D3D::SetPending_AluConstants from inside DrawIndexedVertices
// (rex_D3DDevice_DrawIndexedVertices_82B7AD68), i.e. during the RenderMesh body. Capturing on entry reads
// the PREVIOUS draw's constants and renders every dynamic entity with the
// previous entity's transform/palette.
// Deferred (multi-pass) meshes draw nothing inside the call, detected via
// the draw sequence counter so their transforms are left for the post-draw
// fixup instead of being read from a stale constant bank.
extern "C" REX_FUNC(sub_82795AD8) {
  const bool enabled = skate3::native_render::Enabled();
  // Retail's GraphicsMan<XBox2GraphicsMan>::RenderMesh (sub_82795AD8) takes
  // the MeshContext directly in r3 ("mr r30,r3; lwz r11,0(r30)") and the
  // VertexProgramState in r4. (The dev build's DebugRenderMesh is a member
  // function with both shifted one register later; reading r4 here walks the
  // VertexProgramState as a MeshContext and faults in BuildItemFromMesh.)
  const uint32_t mesh_context = ctx.r3.u32;
  const uint32_t vertex_program_state = ctx.r4.u32;
  const uint64_t draws_before = skate3::native_scene::DrawSequence();
  __imp__sub_82795AD8(ctx, base);
  if (enabled) {
    const bool drew_inside = skate3::native_scene::DrawSequence() != draws_before;
    skate3::native_render::OnRenderMesh(base, mesh_context, vertex_program_state,
                                        drew_inside);
  }
}

// SceneRenderView z-prepass, sub_827FAB50(r3 = SceneRenderView*):
// SceneRenderView::Render (82 7FB158) calls it right before dispatching the
// primary opaque list (+20160) and draws that list depth-only
// (ZPrePass_defaultVS, no pixel shader). It only fills the emulated depth
// buffer; the caller keeps nothing from it (only r28/r30/r31 live across
// the call). Skipped for the main camera view while the native renderer
// draws the world (see SkipGuestZPrepass).
extern "C" REX_FUNC(sub_827FAB50) {
  if (skate3::native_render::Enabled() &&
      skate3::native_scene::SkipGuestZPrepass(ctx.r3.u32)) {
    return;
  }
  __imp__sub_827FAB50(ctx, base);
}

// SceneRenderView sorted draw-list renderer (world geometry):
// sub_827FAF50(r3 = SceneRenderView*, r4 = eastl vector of 8-byte
// {sort_key, MeshContext*} entries, r5 = first, r6 = count). Called from
// SceneRenderView::Render (82 7FB158) for each of the view's key lists.
extern "C" REX_FUNC(sub_827FAF50) {
  struct ListScope {
    uint32_t prev = skate3::native_scene::g_dispatch_list_offset;
    explicit ListScope(uint32_t off) { skate3::native_scene::g_dispatch_list_offset = off; }
    ~ListScope() { skate3::native_scene::g_dispatch_list_offset = prev; }
  } list_scope(ctx.r4.u32 - ctx.r3.u32);
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnSceneDrawList(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                           ctx.r6.u32);
    // Guest-side occlusion dispatch filter: capture above saw every entry;
    // the dispatcher only gets the ones the occlusion cull did not prove
    // hidden. The segment is restored after the call (the list object
    // outlives this dispatch).
    const uint32_t kept = skate3::native_render::FilterSceneDrawList(
        base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (kept != ctx.r6.u32) {
      ctx.r6.u64 = kept;
      {
    __imp__sub_827FAF50(ctx, base);
  }
      skate3::native_render::RestoreSceneDrawList(base);
      return;
    }
  }
  {
    __imp__sub_827FAF50(ctx, base);
  }
}


// The world-shade pass's render function (WorldShadowView vtable slot 10;
// dev 0x8297B5B0): r3 + 20200 / + 20204 is the MeshContext* list it draws,
// including geometry the camera view culled. Retail inlines RenderShadowMesh
// (SetVertexShader / SetIndices / SetStreamSource / DrawIndexedVertices per
// list entry); same prologue, pixel-shader reset and list walk as the dev one.
extern "C" REX_FUNC(sub_827FD170) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnWorldShadowRender(base, ctx.r3.u32);
  }
  __imp__sub_827FD170(ctx, base);
}


// Guest D3D Swap: frame boundary.
extern "C" REX_FUNC(sub_82B82E08) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnFrameEnd(base);
  }
  __imp__sub_82B82E08(ctx, base);
}

// cProcessArenaAsset::RegisterTexture(cAssetList*, cAssetID,
// renderengine::Texture*, rw::Resource&): r4 = 64-bit asset guid,
// r5 = texture object.
extern "C" REX_FUNC(rex_cProcessArenaAsset_RegisterTexture_CAXPAVcAssetList_VcAssetID_assman_PAVTexture_renderengine_AAVResource_rw_Z_82C9A618) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnRegisterTexture(ctx.r4.u64, ctx.r5.u32);
    }
  }
  __imp__rex_cProcessArenaAsset_RegisterTexture_CAXPAVcAssetList_VcAssetID_assman_PAVTexture_renderengine_AAVResource_rw_Z_82C9A618(ctx, base);
}

// ---- palette snapshot hooks (native/skate3_native_palette.h). Each fires
// POST-call, when m_matrices holds the settled packed upload palette.

// cModelInstance::PackAndMultiplyMatricesForUpload(Matrix44& localToWorld):
// post-call, m_matrices (+0x14) holds exactly the packed 4x3 column-major
// upload palette the VS consumes. r3 = the cModelInstance.
// ---- palette snapshot hooks (native/skate3_native_palette.h). Each fires
// POST-call, when m_matrices holds the settled packed upload palette.

// cModelInstance::PackAndMultiplyMatricesForUpload(Matrix44& localToWorld):
// post-call, m_matrices (+0x14) holds exactly the packed 4x3 column-major
// upload palette the VS consumes. r3 = the cModelInstance.
extern "C" REX_FUNC(rex_Sk8_Render_cModelInstance_PackAndMultiplyMatricesForUpload_QAAXABVMatrix44_vpu_math_rw_Z_827E5B30) {
  const uint32_t instance = ctx.r3.u32;
  __imp__rex_Sk8_Render_cModelInstance_PackAndMultiplyMatricesForUpload_QAAXABVMatrix44_vpu_math_rw_Z_827E5B30(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnPackPalette(base, instance);
  }
}

// Sk8::UpdateBoneTransforms(cModelInstance* parts, uint count, Matrix44*
// srcPalette, uint** remaps, Matrix44& out): r3 is an ARRAY of
// cModelInstance part records (stride 0x28): the player skater's per-piece
// palette source (its parts never reach the pack function above).
extern "C" REX_FUNC(sub_827A52C8) {
  const uint32_t parts = ctx.r3.u32;
  const uint32_t count = ctx.r4.u32;
  __imp__sub_827A52C8(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnBoneTransforms(base, parts, count);
  }
}

// Sk8::cLivingWorldPresEntity::Update: post-call, this+528 holds the
// entity's evaluated spawn/distance fade opacity (x = alpha), this+16 the
// current LOD index. Feeds the LW entity store (per-instance ctx ->
// alpha/identity, the serving path).
extern "C" REX_FUNC(rex_Sk8_cLivingWorldPresEntity_Update_UAAXXZ_827C1188) {
  const uint32_t entity = ctx.r3.u32;
  __imp__rex_Sk8_cLivingWorldPresEntity_Update_UAAXXZ_827C1188(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_lw::OnLwEntityTick(base, entity);
  }
}

// Sk8::SkaterPresEntity::StartJobs, bracketed as a pack owner:
// UpdateBoneTransforms calls inside stamp their snapshots with this entity.
extern "C" REX_FUNC(rex_Sk8_SkaterPresEntity_StartJobs_QAAXXZ_827825B0) {
  const uint32_t prev_owner = skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__rex_Sk8_SkaterPresEntity_StartJobs_QAAXXZ_827825B0(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::SkaterPresEntity::DoubleBuffer(garment index): runs once per
// garment per COMPLETED cloth sim tick (the job's output memcpy + buffer
// flip). The identity store's deformed-VB freshness signal: garment-table
// fields persist after the sim stops, so this is the only per-tick proof
// the garment is really CPU-simulated right now.
extern "C" REX_FUNC(rex_Sk8_SkaterPresEntity_DoubleBuffer_AAAXH_Z_82783038) {
  const uint32_t skater = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  __imp__rex_Sk8_SkaterPresEntity_DoubleBuffer_AAAXH_Z_82783038(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnRopaDoubleBuffer(base, skater, index);
  }
}

// Sk8::SkaterPresEntity::EndJobs: calls PackAndMultiplyMatricesForUpload
// per instance with the tick's final locomotion (the body pose of record).
// Bracketed as a pack owner so those snapshots carry the entity.
extern "C" REX_FUNC(rex_Sk8_SkaterPresEntity_EndJobs_QAAXXZ_82782818) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__rex_Sk8_SkaterPresEntity_EndJobs_QAAXXZ_82782818(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Skater-family virtual UBT+Pack driver (vtable slot +16): the remaining
// palette-write path (UpdateBoneTransforms + PackAndMultiply outside the
// StartJobs/EndJobs pair). Bracketed as a pack owner.
// Virtual UBT+Pack driver: the remaining palette-write path
// (UpdateBoneTransforms + PackAndMultiply outside the StartJobs/EndJobs
// pair). Bracketed as a pack owner.
//
// sk83_na_m: retail sub_82785778 -> 0x8290A7F0. Identified by opcode-stream
// equivalence, not by name: retail's three UpdateBoneTransforms callers
// (82781DE0 / 827825B0 / 82785778) correspond 1:1 to sk83_na_m's three
// (8290EDD8 / 829125C0 / 8290A7F0), and 8290A7F0's body matches 82785778's
// instruction for instruction. The recovered symbol here reads
// "cLivingWorldSkinnedPresEntity::Update", which is another stale Skate 2
// import name -- the address is the ground truth.
extern "C" REX_FUNC(sub_82785778) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_82785778(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::RenderPresentation::AddEntityToRenderViews / RmvEntityFrmRenderViews
// - r4 is the PresentationEntity being (de)registered: the identity store's
// scene-membership/lifetime signal (native/skate3_native_entity.h).
extern "C" REX_FUNC(rex_Sk8_RenderPresentation_AddEntityToRenderViews_IAAXPAVPresentationEntity_2_W4eRenderViewType_Render_2_Z_827A6C50) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewAdd(entity);
  }
  __imp__rex_Sk8_RenderPresentation_AddEntityToRenderViews_IAAXPAVPresentationEntity_2_W4eRenderViewType_Render_2_Z_827A6C50(ctx, base);
}

extern "C" REX_FUNC(rex_Sk8_RenderPresentation_RmvEntityFrmRenderViews_IAAXPAVPresentationEntity_2_W4eRenderViewType_Render_2_Z_827A6CE8) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewRemove(entity);
  }
  __imp__rex_Sk8_RenderPresentation_RmvEntityFrmRenderViews_IAAXPAVPresentationEntity_2_W4eRenderViewType_Render_2_Z_827A6CE8(ctx, base);
}

// Sk8::PresentationEntity::BindConstants (base override): post-call, every
// MeshContext of the entity's current LOD has just received its constant
// param POINTERS (world hash 0x7DDF552B -> &entity+416, etc.; the game
// binds by pointer and dereferences at draw). Every subclass bind calls
// this base, so the exit sees every renderable presentation entity with
// its full ctx set: the ctx -> entity identity write point.
extern "C" REX_FUNC(rex_Sk8_PresentationEntity_BindConstants_UAAXXZ_827A6658) {
  const uint32_t entity = ctx.r3.u32;
  __imp__rex_Sk8_PresentationEntity_BindConstants_UAAXXZ_827A6658(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindConstants(base, entity);
  }
}

// Derived BindConstants overrides: class tags for the identity store. The
// most-derived override returns last (each calls its parent first), so the
// last tag to land is the entity's concrete class.
extern "C" REX_FUNC(rex_Sk8_SkaterPresEntity_BindConstants_UAAXXZ_82783D68) {  // Sk8::SkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__rex_Sk8_SkaterPresEntity_BindConstants_UAAXXZ_82783D68(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkater);
  }
}

extern "C" REX_FUNC(rex_Sk8_ColorizedSkaterPresEntity_BindConstants_UAAXXZ_82785260) {  // ColorizedSkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__rex_Sk8_ColorizedSkaterPresEntity_BindConstants_UAAXXZ_82785260(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kColorized);
  }
}

#if 1  // retail hook sub_82793F70 (live before the sk83 port; sk83 had no dev address for it)
extern "C" REX_FUNC(sub_82793F70) {  // CACPresEntity (untransposed world)
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82793F70(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kCac);
  }
}
#endif

#if 1  // retail hook sub_82785528 (live before the sk83 port; sk83 had no dev address for it)
extern "C" REX_FUNC(sub_82785528) {  // unnamed skater-layout class
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82785528(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkaterAux);
  }
}
#endif

extern "C" REX_FUNC(rex_Sk8_cLivingWorldPresEntity_BindConstants_UAAXXZ_827C1720) {  // cLivingWorldPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__rex_Sk8_cLivingWorldPresEntity_BindConstants_UAAXXZ_827C1720(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kLivingWorld);
  }
}

// pegasus::tRModelData::Fixup(void* model, rw::core::arena::ArenaIterator*)
// - the rw-arena LOAD-time pointer resolve, fired once per model while its
// arena streams in. (The `Unfix(void*, SizeAndAlignment*)` atoms are the
// SAVE/size path; hooking tROptiMeshData::Unfix never fired during loads.)
// Post-call the model's mesh table is live: queue its meshes for the
// prewarm decode workers. This is the EARLY prewarm source; it fires
// throughout the load's disk-streaming phase, hours of decode headroom
// before the final-seconds AddRenderInstance activation burst. The atoms
// dispatch indirectly through the recomp function table, which resolves to
// this override at link time like any other reference.
extern "C" REX_FUNC(rex_pegasus_tRModelData_Fixup_SAIPAXPAVArenaIterator_arena_core_rw_Z_82963510) {
  const uint32_t model = ctx.r3.u32;
  __imp__rex_pegasus_tRModelData_Fixup_SAIPAXPAVArenaIterator_arena_core_rw_Z_82963510(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnModelFixup(base, model);
  }
}

// Sk8::Challenge::PhotoReplayController::Update(float): runs once per guest
// frame while a photo-mission's photo editor is up (pick-a-photo +
// depth-of-field / saturation / brightness / contrast controls). Heartbeat
// for the native scene's photo-editor yield: the editor's effects are the
// game's own postfx chain, which only the emulated path executes. Reached
// via the recomp function table (virtual dispatch), so the override fires
// like any direct call.
extern "C" REX_FUNC(rex_Sk8_Challenge_PhotoReplayController_Update_UAAXM_Z_825623F0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoReplayUpdate();
  }
  __imp__rex_Sk8_Challenge_PhotoReplayController_Update_UAAXM_Z_825623F0(ctx, base);
}

// Sk8::FE::FrontEndState_Replay2::TakePhoto(): fires once when the player
// takes a photo (replay editor / photo mission Select). The game then
// renders the shot into the 1152x640 PostFX screenshot target, resolves it,
// and ScreenshotBackEnd::GrabScreenshot CPU-reads the resolved guest memory
// to JPEG-encode it, which is all zeros unless resolve readback is forced
// (the invisible-final-photo bug: an F11 capture showed the grab texture
// 0x04911000 memory fully zero). Arms the photo-grab readback window.
extern "C" REX_FUNC(sub_826147C8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_826147C8(ctx, base);
}

// ScreenshotBackEnd grab execution (rex_Sk8_BE_FEBackEnd_Init_UAAXXZ_8267E8F0): runs once the game's
// frame for a requested screenshot is ready and immediately CPU-reads the
// screenshot target (rex_Sk8_BE_ScreenshotBackEnd_GrabScreenshot_QAA_N_N_Z_826797D0). The native frame is written into the
// targets synchronously here, so the read always gets the current frame
// (racing the render thread's armed writes returned the previous photo).
// Retail: sub_824FD370, the only caller of GrabScreenshot (sub_824FD550);
// same atomic take of this+4, then GrabScreenshot(this-12, byte this+608).
extern "C" REX_FUNC(sub_824FD370) {
  if (skate3::native_render::Enabled()) {
    const int result = skate3::native_scene::SyncWriteNewestGrab(base);
    REXLOG_INFO("native-render: screenshot execution - native frame {} (code {})",
                result == 1 ? "written to the targets" : "NOT written", result);
  }
  __imp__sub_824FD370(ctx, base);
}


// CACPresEntity::GenerateSnapShotJPEG (0x8298CF40, r4 = the texture it
// reads; runs three times per create-a-skater exit).
#if 0  // sk83_na_m diagnostic markers on dev-build functions (not mapped to retail)
// DIAGNOSTIC photo-effects markers (which game function issues each draw of
// the create-a-skater portrait pass; see DumpRecentPasses).
#define SK83_PHOTO_MARK(fn, label)                                                     extern "C" REX_FUNC(fn) {                                                               const bool mark = skate3::native_render::Enabled();                                  if (mark) {                                                                             skate3::native_scene::RecordPassMarker(label " enter", ctx.r3.u32, ctx.r4.u32,                                               uint32_t(ctx.lr));                           }                                                                                     __imp__##fn(ctx, base);                                                               if (mark) {                                                                             skate3::native_scene::RecordPassMarker(label " exit", ctx.r3.u32, 0, 0);            }                                                                                   }
SK83_PHOTO_MARK(rex_Sk8_CACPresEntity_SetupSnapshotCamera_AAAXXZ_829AB768,
                "PhotoEffects::GeneratePhoto(829AB768)")
#endif
// CACPresEntity::SetupSnapshotCamera (0x8290CB30, script-called before the
// profile pictures): builds the photo camera's projection from the game's
// 16/9 aspect double, which skate3_guest_aspect widens to the output for the
// Hor+ gameplay camera. The picture is an image of the game's fixed
// 1152x640 raster, so its camera must be the console's: hold 16/9 here.
extern "C" REX_FUNC(rex_Sk8_Audio_MusicSystem_GetNumSongsInPlayList_IAAIVaud_playlists_Gen_Attrib_Z_8278F788) {
  skate3::native_scene::PhotoTraceBegin("snapshot camera 8278F788");
  skate3::native_scene::PhotoTraceCall(base, "SnapshotCamera(8278F788)", ctx.r3.u32, ctx.r4.u32,
                                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, 0, 0);
  Skate3GuestAspectHold16x9(true);
  __imp__rex_Sk8_Audio_MusicSystem_GetNumSongsInPlayList_IAAIVaud_playlists_Gen_Attrib_Z_8278F788(ctx, base);
  Skate3GuestAspectHold16x9(false);
}

// ---- Snapshot trace hooks (retail addresses; see PhotoTraceBegin) ----
// The retail PhotoEffects pipeline: 827E1F58 looks up a preset (827E46C8,
// Attrib collection) and runs 827E2020, which draws the screenshot target
// (827E3F60), lets every layer object fill its 60-byte record (vtable +4) and
// draws it by type: 1 827E2E30, 2 827E2F68, 3 827E3E20, 4/6/7 827E41C0
// (texture layer), 5 827E3070; then resolves and JPEG-encodes (827E2C48).
extern "C" REX_FUNC(sub_827E1F58) {
  skate3::native_scene::PhotoTraceBegin("PhotoEffects 827E1F58");
  skate3::native_scene::PhotoTraceCall(base, "Generate(827E1F58)", ctx.r3.u32, ctx.r4.u32,
                                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r4.u32, 32, 0);
  __imp__sub_827E1F58(ctx, base);
  skate3::native_scene::PhotoTraceCall(base, "Generate(827E1F58) ret", ctx.r3.u32, 0, 0, 0, 0,
                                       0, 0, 0);
}
extern "C" REX_FUNC(sub_827E46C8) {
  skate3::native_scene::PhotoTraceCall(base, "PresetLookup(827E46C8)", ctx.r3.u32, ctx.r4.u32,
                                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r4.u32, 16, 0);
  __imp__sub_827E46C8(ctx, base);
  skate3::native_scene::PhotoTraceCall(base, "PresetLookup(827E46C8) ret", ctx.r3.u32, 0, 0,
                                       0, 0, 0, 0, 0);
}
#define SKATE3_PHOTO_LAYER_HOOK(fn, label, desc_reg, tex_expr)                              \
  extern "C" REX_FUNC(fn) {                                                                  \
    skate3::native_scene::PhotoTraceCall(base, label, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,    \
                                         ctx.r6.u32, ctx.r7.u32, ctx.desc_reg.u32, 60,       \
                                         tex_expr);                                          \
    __imp__##fn(ctx, base);                                                                  \
  }
SKATE3_PHOTO_LAYER_HOOK(sub_827E2E30, "Layer1(827E2E30)", r4, 0)
SKATE3_PHOTO_LAYER_HOOK(sub_827E2F68, "Layer2(827E2F68)", r3, 0)
SKATE3_PHOTO_LAYER_HOOK(sub_827E3E20, "Layer3(827E3E20)", r3, 0)
SKATE3_PHOTO_LAYER_HOOK(sub_827E41C0, "Layer4/6/7 texture(827E41C0)", r4, ctx.r3.u32)
SKATE3_PHOTO_LAYER_HOOK(sub_827E3070, "Layer5(827E3070)", r3, 0)
#undef SKATE3_PHOTO_LAYER_HOOK
// Snapshot trace: save the photo texture (k_8_8_8_8 1152x640) as a PPM in
// logs/, tagged with the renderer mode, so the native and emulated results of
// the same pass can be compared pixel for pixel.
static void PhotoTraceDumpTexture(uint8_t* base, uint32_t texture, const char* stage) {
  if (!skate3::native_scene::PhotoTraceActive() || texture < 0x10000) {
    return;
  }
  static int s_dump = 0;
  const std::string path =
      fmt::format("logs/photo_trace_{:02}_{}_{}.ppm", s_dump++,
                  skate3::native_scene::Enabled() ? "native" : "emulated", stage);
  skate3::native_scene::DumpGuestTextureForDiag(base, texture, path.c_str());
  REXLOG_INFO("native-scene: PHOTO-TRACE saved {} (texture {:08X})", path, texture);
}

extern "C" REX_FUNC(rex_Sk8_CACPresEntity_GenerateSnapShotJPEG_QAA_AVcAssetID_assman_XZ_827E2C48) {
  PhotoTraceDumpTexture(base, ctx.r4.u32, "encoded");
  uint32_t src_tex = 0;
  if (ctx.r3.u32 >= 0x10000) {
    src_tex = REX_LOAD_U32(ctx.r3.u32 + 0);
  }
  skate3::native_scene::PhotoTraceCall(base, "EncodeJPEG(827E2C48)", ctx.r3.u32, ctx.r4.u32,
                                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r3.u32, 48,
                                       src_tex);
  __imp__rex_Sk8_CACPresEntity_GenerateSnapShotJPEG_QAA_AVcAssetID_assman_XZ_827E2C48(ctx, base);
}

// PhotoEffects::Apply (0x829AB318): the portrait pass scope (see PhotoJob).
extern "C" REX_FUNC(sub_827E2020) {
  skate3::native_scene::PhotoTraceBegin("PhotoEffects::Apply 827E2020");
  // r4 = layer object array, r5 = layer count, r6 = output size (w, h).
  skate3::native_scene::PhotoTraceCall(base, "Apply(827E2020) layers", ctx.r3.u32, ctx.r4.u32,
                                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r4.u32,
                                       std::min<uint32_t>(ctx.r5.u32, 32u) * 4u, 0);
  skate3::native_scene::PhotoTraceCall(base, "Apply(827E2020) size", ctx.r3.u32, 0, 0, 0, 0,
                                       ctx.r6.u32, 8, 0);
  if (skate3::native_scene::PhotoTraceActive() && ctx.r4.u32 >= 0x10000) {
    // Each layer object: vtable + its 60-byte record (+4), before the fill.
    for (uint32_t i = 0; i < std::min<uint32_t>(ctx.r5.u32, 32u); ++i) {
      const uint32_t layer = REX_LOAD_U32(ctx.r4.u32 + i * 4);
      if (layer >= 0x10000) {
        skate3::native_scene::PhotoTraceCall(base, "Apply layer obj", layer,
                                             REX_LOAD_U32(layer + 0), i, 0, 0, layer + 4, 60, 0);
      }
    }
  }
  const bool native = skate3::native_render::Enabled();
  if (native) {
    skate3::native_scene::RecordPassMarker("PhotoEffects::Apply(829AB318) enter", ctx.r3.u32,
                                           ctx.r4.u32, uint32_t(ctx.lr));
    skate3::native_scene::OnPhotoApply(true);
  }
  __imp__sub_827E2020(ctx, base);
  if (native) {
    skate3::native_scene::OnPhotoApply(false);
    skate3::native_scene::RecordPassMarker("PhotoEffects::Apply(829AB318) exit", ctx.r3.u32, 0,
                                           0);
  }
}
extern "C" REX_FUNC(sub_827E3F60) {
  skate3::native_scene::PhotoTraceCall(
      base, "DrawTextureQuad(827E3F60)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
      ctx.r7.u32, ctx.r3.u32, 48, ctx.r3.u32 >= 0x10000 ? REX_LOAD_U32(ctx.r3.u32 + 0) : 0);
  PhotoTraceDumpTexture(base, ctx.r3.u32, "before");
  const bool mark = skate3::native_render::Enabled();
  if (mark) {
    skate3::native_scene::RecordPassMarker("PhotoEffects::DrawTextureQuad(8299EBE0) enter",
                                           ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  }
  __imp__sub_827E3F60(ctx, base);
  if (mark) {
    skate3::native_scene::RecordPassMarker("PhotoEffects::DrawTextureQuad(8299EBE0) exit",
                                           ctx.r3.u32, 0, 0);
    skate3::native_scene::DiagArmCameraLog();  // the skater redraw follows
  }
}
#if 0  // sk83_na_m diagnostic markers (dev-build addresses)
SK83_PHOTO_MARK(sub_8299E6F8, "PhotoEffects::SetOrthoCamera(8299E6F8)")
SK83_PHOTO_MARK(sub_829AC138, "PhotoEffects::829AC138")
SK83_PHOTO_MARK(sub_829AC210, "PhotoEffects::829AC210")
SK83_PHOTO_MARK(rex_Sk8_FE_FrontEndState_OnlineHubMenu_SetupSendInviteMenuItem_AAAXXZ_829750B0, "829750B0")
SK83_PHOTO_MARK(rex_f0VIPresentation_Sk8_QAA_XZ_8291BA80, "VIPresentation(8291BA80)")
#undef SK83_PHOTO_MARK
#endif

#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(rex_Sk8_CACPresEntity_GenerateSnapShotJPEG_QAA_AVcAssetID_assman_XZ_8298CF40) {
#ifndef SK83_RELEASE_BUILD
  if (skate3::native_render::Enabled()) {
    // DIAGNOSTIC: PhotoEffects::ExtractTextureImageData about to read the
    // photo target (pass-ring marker + crop log).
    skate3::native_scene::RecordPassMarker("PhotoEffects::ExtractTextureImageData(8298CF40)",
                                           ctx.r4.u32, ctx.r3.u32, uint32_t(ctx.lr));
    const uint32_t fx = ctx.r3.u32;
    REXLOG_INFO("native-render: photo extract from texture {:08X}: crop x={} w={} h={} -> "
                "{}x{}",
                ctx.r4.u32, REX_LOAD_U32(fx + 28), REX_LOAD_U32(fx + 12),
                REX_LOAD_U32(fx + 16), REX_LOAD_U32(fx + 20), REX_LOAD_U32(fx + 24));
  }
#endif
  __imp__rex_Sk8_CACPresEntity_GenerateSnapShotJPEG_QAA_AVcAssetID_assman_XZ_8298CF40(ctx, base);
}
#endif  // TODO(retail port)


// Lua-side snapshot request (rex_cMsgPlayerHasCollidedWithLWVehicle_PostMessage_LuaBinding_824E36E0: posts message -87029522 with the
// hash of its string argument; the create-a-skater exit snapshot is issued
// from the frontend scripts, not through cMsgPresCASSnapshot::Deliver).
#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(rex_cMsgPlayerHasCollidedWithLWVehicle_PostMessage_LuaBinding_824E36E0) {
  if (skate3::native_render::Enabled()) {
    REXLOG_INFO("native-render: Lua snapshot request (L=0x{:08X})", ctx.r3.u32);
    skate3::native_scene::OnTakePhoto();
  }
  __imp__rex_cMsgPlayerHasCollidedWithLWVehicle_PostMessage_LuaBinding_824E36E0(ctx, base);
}
#endif  // TODO(retail port)


// Create-a-skater profile snapshot (kbEnableCACSnapshot, "snapshot at CAC
// shutdown"): the sim posts cMsgPresCASSnapshot (message id -87029522,
// rex_cMsgPlayerHasCollidedWithLWVehicle_Deliver_SAXPBUStaticData_Hermes_N_Z_824FED58) to the presentation thread, which renders the skater and
// the game CPU-reads the resolved image for the profile picture;
// cMsgSimCASSnapshotComplete (sub_825022F8) reports back. The emulated
// resolve never reaches CPU memory, so the picture was never taken in
// either renderer. The request arms the same native grab as the photo
// flow (our frame is written into the game's screenshot targets).
#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(rex_cMsgPlayerHasCollidedWithLWVehicle_Deliver_SAXPBUStaticData_Hermes_N_Z_824FED58) {
  if (skate3::native_render::Enabled()) {
    REXLOG_INFO("native-render: CAS profile snapshot requested (msg 0x{:08X})", ctx.r3.u32);
    skate3::native_scene::OnTakePhoto();
  }
  __imp__rex_cMsgPlayerHasCollidedWithLWVehicle_Deliver_SAXPBUStaticData_Hermes_N_Z_824FED58(ctx, base);
}
#endif  // TODO(retail port)

#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(sub_825022F8) {
  if (skate3::native_render::Enabled()) {
    REXLOG_INFO("native-render: CAS profile snapshot complete (msg 0x{:08X})", ctx.r3.u32);
  }
  __imp__sub_825022F8(ctx, base);
}
#endif  // TODO(retail port)


// Sk8::BE::ScreenshotBackEnd::GrabScreenshot(bool), the actual grab: the
// game CPU-reads the resolved screenshot target from guest memory and
// JPEG-encodes it. Fires in EVERY grab flow; the photo-mission Select
// confirm does NOT go through FrontEndState_Replay2::TakePhoto (a logged
// full photo-mission run never opened the card compose window), so this is
// the canonical shutter heartbeat. Right after the grab
// the FE composes the framed display card (white border / caption / logo)
// over the JPEG texture in a ONE-SHOT RTT pass; the card compose window
// (UpdatePhotoGrabWindow) keys off this timestamp so that pass executes and
// its resolve lands in CPU guest memory for the native 2D decoder.
extern "C" REX_FUNC(sub_824FD550) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_824FD550(ctx, base);
  // Post-call: the grab has CPU-read the shot; the card compose that
  // follows reads already-copied memory. Event-closes the shutter burst.
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabDone();
  }
}

// Sk8::BE::ScreenshotBackEnd grab REQUEST (sets the grab params + arms the
// request flag; called by the FE photo flows 1-2 frames BEFORE the game
// renders the shot + card-composite frame sequence whose resolves
// GrabScreenshot/OnScreenShot then CPU-read). This is the only PREDICTIVE
// shutter signal; the GrabScreenshot hook above fires AFTER those frames
// already rendered (suppressed). Arms the shutter burst: suppression lifted
// + full small-resolve readbacks for ~1.5 s so the card-build's inputs are
// real (a logged control run showed the composite CPU copies land exactly
// in this window; every post-hoc window/bound missed them).
#if 1  // retail hook sub_824FD4B0 (live before the sk83 port; sk83 had no dev address for it)
// Sk8::BE::ScreenshotBackEnd grab REQUEST (sets the grab params + arms the
// request flag; called by the FE photo flows 1-2 frames BEFORE the game
// renders the shot + card-composite frame sequence whose resolves
// GrabScreenshot/OnScreenShot then CPU-read). This is the only PREDICTIVE
// shutter signal; the GrabScreenshot hook above fires AFTER those frames
// already rendered (suppressed). Arms the shutter burst: suppression lifted
// + full small-resolve readbacks for ~1.5 s so the card-build's inputs are
// real (a logged control run showed the composite CPU copies land exactly
// in this window; every post-hoc window/bound missed them).
extern "C" REX_FUNC(sub_824FD4B0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabRequest();
  }
  __imp__sub_824FD4B0(ctx, base);
}
#endif

namespace skate3::native_scene {
// Fault-guarded guest read (defined in skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
}  // namespace skate3::native_scene

REXCVAR_DEFINE_BOOL(
    skate3_autoexposure_pin, true, "Skate 3",
    "Pin the game's auto-exposure at its per-zone maximum. The game "
    "adapts world exposure from a GPU luminance measurement it reads "
    "back from memory; with resolve readbacks disabled (the native "
    "renderer's standard configuration) that measurement reads empty and "
    "exposure settles at the maximum anyway - but windows that "
    "temporarily enable readbacks (the photo shutter) can leak one real "
    "measurement of a bright frame, after which the frozen measurement "
    "drags exposure to the minimum clamp permanently (the stuck world "
    "darkening after photo missions). Pinning removes the dependence on "
    "measurement availability. Turn OFF only when running with full "
    "resolve readbacks enabled, where live per-frame measurements make "
    "the game's own adaptation behave as on console.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// World auto-exposure evaluator (per frame): reads the GPU luminance
// measurement (sum / area of a resolved measurement surface), adapts the
// scene exposure toward the target luminance and clamps it to the
// per-zone [min, max] from the tuning block at [this+2468]+120..132
// (target, min, max, rate). Without resolve readbacks the measurement
// reads empty and exposure settles at the maximum; a readback window
// that briefly lets one bright measurement through (the photo shutter
// burst) leaves a frozen too-bright measurement that drags exposure to
// the minimum clamp permanently. The hook re-pins the adapted exposure
// (the evaluator's internal state and both published outputs, honoring
// the same ownership gates the game checks) to the per-zone maximum.
#if 0  // TODO(sk83_na_m): retail sub_827F0D00 -- genuinely unresolved
// World auto-exposure evaluator (per frame): reads the GPU luminance
// measurement (sum / area of a resolved measurement surface), adapts the
// scene exposure toward the target luminance and clamps it to the
// per-zone [min, max] from the tuning block at [this+2468]+120..132
// (target, min, max, rate). Without resolve readbacks the measurement
// reads empty and exposure settles at the maximum; a readback window
// that briefly lets one bright measurement through (the photo shutter
// burst) leaves a frozen too-bright measurement that drags exposure to
// the minimum clamp permanently. The hook re-pins the adapted exposure
// (the evaluator's internal state and both published outputs, honoring
// the same ownership gates the game checks) to the per-zone maximum.
extern "C" REX_FUNC(sub_827F0D00) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_827F0D00(ctx, base);
  if (!REXCVAR_GET(skate3_autoexposure_pin) || self < 0x10000) {
    return;
  }
  uint32_t aux = 0, inst_a = 0, inst_b = 0, max_bits = 0;
  if (!skate3::native_scene::GuestTryLoadU32(base, self + 2468, &aux) ||
      aux < 0x10000 ||
      !skate3::native_scene::GuestTryLoadU32(base, aux + 128, &max_bits) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2484, &inst_a) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2500, &inst_b)) {
    return;
  }
  float max_expo;
  std::memcpy(&max_expo, &max_bits, sizeof(max_expo));
  if (!(max_expo > 0.0f) || max_expo > 16.0f) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  uint32_t prev_bits = REX_LOAD_U32(self + 3240);
  float prev;
  std::memcpy(&prev, &prev_bits, sizeof(prev));
  static std::atomic<int64_t> s_last_log_ns{-1};
  if (prev > 0.0f && prev < max_expo * 0.98f) {
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const int64_t last = s_last_log_ns.load(std::memory_order_relaxed);
    if (last < 0 || now_ns - last > 60'000'000'000) {
      s_last_log_ns.store(now_ns, std::memory_order_relaxed);
      REXLOG_INFO(
          "native-render: auto-exposure re-pinned at zone max {:.3f} "
          "(adaptation had moved it to {:.3f}; luminance measurement "
          "unavailable without resolve readbacks)",
          max_expo, prev);
    }
  }
  REX_STORE_U32(self + 3240, max_bits);
  REX_STORE_U32(self + 3392, max_bits);
  const uint32_t own_a = REX_LOAD_U8(self + 3397);
  const uint32_t own_b = REX_LOAD_U8(self + 3396);
  if (own_a == 0 && inst_a >= 0x10000) {
    REX_STORE_U32(inst_a + 48, max_bits);
  }
  if (own_b == 0 && inst_b >= 0x10000) {
    REX_STORE_U32(inst_b + 56, max_bits);
  }
}
#endif

// sk83's auto-exposure evaluator: PostProcessor sub_82978630
// (PostProcessor.cpp). It locks the ring's current 32x16 luminance target,
// [[this + 4*([this+460] + 117)] + 664], CPU-averages its alpha bytes
// (center-weighted, rex_Fesl_SymmVoIPGroupImpl_GagAllMembers_UAAXXZ_82978458), and adapts exposure toward the zone
// target within the zone clamps; kExposureControl (debug menu) != Auto
// overrides the result with value * 0.1. The target is rendered by the
// game's postfx draws 1463-1465, which never execute natively, so the
// evaluator averaged black and exposure sat on its clamp. Before the game
// reads it, land the native renderer's rebuilt measurement (blur.hlsl
// ps_lum_*, LatestExposureMeasurement) in that target, in the texture's own
// layout (tiled big-endian ARGB, alpha = byte 0). Only a FRESH measurement
// is written: after a runtime switch to the emulated renderer the native
// plane stops updating and the game's own pipeline owns the target again.
// Retail: sub_827F0D00 (same 460/117/664 target lookup and lock, then the
// same alpha-byte averager rex_Fesl_SymmVoIPGroupImpl_GagAllMembers_UAAXXZ_827F0B78 = dev 0x82978458).
extern "C" REX_FUNC(sub_827F0D00) {
  const uint32_t self = ctx.r3.u32;
  uint8_t lum[32 * 16];
  int64_t lum_ns = -1;
  if (skate3::native_render::Enabled() && self >= 0x10000 &&
      skate3::native_scene::LatestExposureMeasurement(lum, &lum_ns)) {
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    uint32_t idx = 0, wrapper = 0, tex = 0;
    uint32_t fw[6] = {};
    bool ok = now_ns - lum_ns < 250'000'000 &&
              skate3::native_scene::GuestTryLoadU32(base, self + 460, &idx) &&
              idx < 4 &&
              skate3::native_scene::GuestTryLoadU32(base, self + 4 * (idx + 117),
                                                    &wrapper) &&
              wrapper >= 0x10000 &&
              skate3::native_scene::GuestTryLoadU32(base, wrapper + 664, &tex) &&
              tex >= 0x10000;
    for (int k = 0; k < 6 && ok; ++k) {
      ok = skate3::native_scene::GuestTryLoadU32(base, tex + 0x1C + uint32_t(k) * 4,
                                                 &fw[k]);
    }
    // Fetch constant: k_8_8_8_8 (6), 32x16, the layout the evaluator and
    // this writer assume; anything else is left to the game.
    const uint32_t fmt = fw[1] & 0x3F;
    const uint32_t tw = (fw[2] & 0x1FFF) + 1;
    const uint32_t th = ((fw[2] >> 13) & 0x1FFF) + 1;
    const uint32_t addr = fw[1] & 0x1FFFF000;
    const bool tiled = (fw[0] >> 31) != 0;
    const uint32_t pitch = ((fw[0] >> 22) & 0x1FF) * 32;
    ok = ok && fmt == 6 && tw == 32 && th == 16 && addr != 0 && pitch >= 32;
    if (ok) {
      uint32_t offs[32 * 16];
      uint32_t span = 0;
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          const uint32_t off =
              tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                          int32_t(x), int32_t(y), pitch, 2))
                    : (y * pitch + x) * 4;
          offs[y * 32 + x] = off;
          span = std::max(span, off + 4);
        }
      }
      // Read-modify-write the texel span so tile padding keeps its bytes.
      std::vector<uint8_t> staging(span);
      uint8_t* guest = base + (0xA0000000u | addr);
      if (span <= 0x4000 &&
          skate3::native_scene::GuestTryCopy(staging.data(), guest, span)) {
        for (uint32_t t = 0; t < 32 * 16; ++t) {
          uint8_t* px = staging.data() + offs[t];
          px[0] = lum[t];  // A: the byte the evaluator averages
          px[1] = 0;
          px[2] = 0;
          px[3] = 0;
        }
        skate3::native_scene::GuestTryCopy(guest, staging.data(), span);
        static bool s_logged = false;
        if (!s_logged) {
          s_logged = true;
          REXLOG_INFO(
              "native-render: auto-exposure measurement live (guest target "
              "0x{:08X}, {} 32x16, pitch {})",
              addr, tiled ? "tiled" : "linear", pitch);
        }
      }
    }
  }
  __imp__sub_827F0D00(ctx, base);
}


// rw::movie::MovieDecoder::Decode(int, VideoRenderable**,
// SubtitleRenderable*): fires per decoded FMV frame while any movie plays
// (boot intro logos and all other rw::movie playback). Heartbeat for the
// native scene's FMV yield: the video frame is CPU-decoded into a texture
// and reaches the screen through the game's postfx chain + swap without a
// capturable 2D draw, so only the emulated path can show it.
extern "C" REX_FUNC(sub_82A92DC8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieDecode();
  }
  __imp__sub_82A92DC8(ctx, base);
}

// VideoRenderer_RwTexture::Render(VideoRenderable*, int): fills the three
// YUV plane textures (members this+12/+124/+68) with the decoded FMV frame
// via Texture::Lock/FillTextureData/Unlock. Post-call, the planes hold the
// finished frame: publish their fetch words for the native FMV blit.
extern "C" REX_FUNC(rex_VideoRenderer_RwTexture_Render_AAAXPAVVideoRenderable_movie_rw_H_Z_8263C498) {
  const uint32_t self = ctx.r3.u32;
  __imp__rex_VideoRenderer_RwTexture_Render_AAAXPAVVideoRenderable_movie_rw_H_Z_8263C498(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieFrame(base, self);
  }
}

// Sk8::WorldPresentation::AddRenderInstance(pegasus::tInstance*): the world
// registry add, fired per placed instance while a map loads (r4 = tInstance).
// The prewarm's primary mesh source: the instance's tRModelData mesh table
// is walked (validated offsets) and every optimesh queued for the
// loading-screen decode.
extern "C" REX_FUNC(sub_82791290) {
  const uint32_t instance = ctx.r4.u32;
  __imp__sub_82791290(ctx, base);
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnAddRenderInstance(base, instance);
    }
  }
}

// D3D::SetPending_AluConstants(device, u64 dirty_group_mask, bank, ptr):
// bank 0x4000 = vertex constants. Called from inside the Draw* functions;
// ptr is the device's positional constant shadow bank.
extern "C" REX_FUNC(rex_D3D_SetPending_AluConstants_YAXPAVCDevice_1_KKPAX_Z_82B83FE0) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_render::HookTimer ht(skate3::native_render::kHookVsOurs);
      skate3::native_scene::OnVsConstantUpload(base, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                             ctx.r3.u32);
    }
  }
  {
    skate3::native_render::HookTimer ht(skate3::native_render::kHookVsGame);
    __imp__rex_D3D_SetPending_AluConstants_YAXPAVCDevice_1_KKPAX_Z_82B83FE0(ctx, base);
  }
}

// D3DDevice_SetIndices(device, ib) / D3DDevice_SetStreamSource(device,
// stream, vb, offset, stride): track the currently bound guest buffers so
// draws can be matched back to captured skinned items.
extern "C" REX_FUNC(rex_D3DDevice_SetIndices_82B79190) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnSetIndices(ctx.r4.u32);
    }
  }
  {
    __imp__rex_D3DDevice_SetIndices_82B79190(ctx, base);
  }
}

// D3DDevice_ClearF(r3 device, r4 flags, r5 rects, r6 D3DVECTOR4* color,
// f1 z, r8 stencil); D3DDevice_Clear converts its D3DCOLOR and forwards
// here. Captures the game's scene clear color for the native frame.
extern "C" REX_FUNC(rex_D3DDevice_ClearF_82B7D708) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnClearF(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
  }
  __imp__rex_D3DDevice_ClearF_82B7D708(ctx, base);
}

// D3DDevice_Resolve(r3 device, r4 flags, r5 rects, r6 dest texture, ...):
// the world-shade pass's depth resolve marks the frame (see OnResolve).
extern "C" REX_FUNC(rex_D3DDevice_Resolve_82B7DE28) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnResolve(base, ctx.r4.u32, ctx.r6.u32);
  }
  __imp__rex_D3DDevice_Resolve_82B7DE28(ctx, base);
}

// Sk8::Morph_VMX(BlendJobParams_t&): the vectorized CPU blend-shape kernel
// the create-a-skater morph jobs run (CACPresEntity::MorphParts ->
// FireMorphJob -> MorphJob_vec -> Morph_VMX). It rewrites the character VB
// in place; bump the morph epoch after it so the renderer re-decodes the
// skinned meshes (face sliders applied live, matching the emulated frame).
extern "C" REX_FUNC(sub_82794218) {
  __imp__sub_82794218(ctx, base);
  skate3::native_scene::g_morph_epoch.fetch_add(1, std::memory_order_relaxed);
}

// D3DDevice_SetStreamSource. Retail's takes (device, stream, vb, stride):
// no offset argument -- r6 is the stride ("mr r26,r6 ... rlwinm r9,r26,30"),
// where the dev build has (device, stream, vb, offset, stride) and adds r6
// to the fetch base.
extern "C" REX_FUNC(rex_D3DDevice_SetStreamSource_82B78FF0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, /*offset=*/0,
                                            ctx.r6.u32);
  }
  {
    __imp__rex_D3DDevice_SetStreamSource_82B78FF0(ctx, base);
  }
}

// D3DDevice_DrawIndexedVertices: post-call, the draw's VS constants are now
// in the shadow bank; refresh any pending skinned item bound to these
// buffers (deferred multi-pass meshes only draw here).
// Retail's DrawIndexedVertices(prim, base, start, count) has no device
// argument (r3..r6; see DrawVertices below); shifted into the dev layout.
extern "C" REX_FUNC(rex_D3DDevice_DrawIndexedVertices_82B7AD68) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r3.u32;
  const uint32_t r5 = ctx.r4.u32;
  const uint32_t r6 = ctx.r5.u32;
  const uint32_t r7 = ctx.r6.u32;
  {
    skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawGame);
    __imp__rex_D3DDevice_DrawIndexedVertices_82B7AD68(ctx, base);
  }
  if (enabled) {
    {
      skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawOurs);
      skate3::native_scene::OnDrawDone(base, 0, r4, r5, r6, r7);
    }
  }
}

// D3DDevice_DrawVertices, non-indexed draw path: cloth-simulated garments
// (captured live as world-space quad items) and character shadow proxies.
// Retail's DrawVertices(prim, start, count) is not a device member: it loads
// the device from the global (lwz r31,17516) and takes prim/start/count in
// r3/r4/r5 ("mr r25,r3; mr r23,r4; mr r24,r5"); the dev build passes the
// device in r3 and everything one register later. The native capture code
// speaks the dev layout, so shift here (prim -> "r4", start -> "r5", count ->
// "r6"); reading r4..r6 directly rejected every lionREC particle draw.
extern "C" REX_FUNC(rex_D3DDevice_DrawVertices_82B7A970) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r3.u32;
  const uint32_t r5 = ctx.r4.u32;
  const uint32_t r6 = ctx.r5.u32;
  const uint32_t r7 = 0;
  {
    skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawGame);
    __imp__rex_D3DDevice_DrawVertices_82B7A970(ctx, base);
  }
  if (enabled) {
    {
      skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawOurs);
      skate3::native_scene::OnDrawDone(base, 1, r4, r5, r6, r7);
    }
    skate3::native_render::OnClothDraw(base, r4, r5, r6, r7);
  }
}

// ---- 2D / APT (Flash-converted HUD) reconnaissance hooks -----------------
// Every HUD/menu 2D element is a Flash SWF converted to EA APT, rendered by
// Sk8::FE::AptRenderingIntegration through the same guest D3D draw functions
// hooked above. These brackets tag draws issued inside the 2D pass so the
// recorder can capture and the future native 2D pass can replay them.

// Sk8::FE::FrontEndManager::Render2D(unsigned int): the game's whole 2D
// pass (FE movies + HUD).
// ---- 2D / APT (Flash-converted HUD) reconnaissance hooks -----------------
// Every HUD/menu 2D element is a Flash SWF converted to EA APT, rendered by
// Sk8::FE::AptRenderingIntegration through the same guest D3D draw functions
// hooked above. These brackets tag draws issued inside the 2D pass so the
// recorder can capture and the future native 2D pass can replay them.

// Sk8::FE::FrontEndManager::Render2D(unsigned int): the game's whole 2D
// pass (FE movies + HUD).
extern "C" REX_FUNC(sub_825D9168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(0, true);
  __imp__sub_825D9168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(0, false);
}

// Sk8::FE::AptMovieIntegration::Render(unsigned int): one APT movie.
extern "C" REX_FUNC(sub_825D67D8) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(1, true);
  __imp__sub_825D67D8(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(1, false);
}

// Sk8::FE::AptRenderingIntegration::DrawRenderingUnit(void*, AptRenderInfo
// const*): one APT display-list element (texture quad / vector shape).
extern "C" REX_FUNC(sub_825D4490) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(2, true);
  __imp__sub_825D4490(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(2, false);
}

// Sk8::FE::AptRenderingIntegration::UpdateRenderToTexture(unsigned int):
// in gameplay this renders the whole HUD into a screen-sized overlay
// texture at true screen coordinates (the game composites it later through
// the suppressed emulated pass). Diagnostic bracket only.
extern "C" REX_FUNC(sub_825D4E50) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(3, true);
  __imp__sub_825D4E50(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(3, false);
}

// Sk8::Render::cFont::DrawstringLocal<char> / <unsigned short>: the glyph
// text emitter (trick names, scores). Text can flush OUTSIDE the APT
// brackets, so it gets its own bit.
extern "C" REX_FUNC(sub_82808388) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808388(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

// RenderMan::FinalQuadFade (0x82997488): the full-screen colour fade quad
// (SimpleDraw QuadListColoured), drawn whenever the fade colour's alpha in
// the render globals ([0x83083C38]+0x7C10/+0x7C20, selector byte +0x7C30)
// is above 0. The challenge-start fade-in stops one ramp step short and
// leaves alpha 1/30 behind (F11 probe: 0 -> 1 -> 0.033, then 0.033 for
// seconds), so the game darkens every later frame by 3.3% until the next
// fade. A live fade rewrites the alpha every update; that leftover never
// changes. Skip the quad only while a PARTIAL alpha has held bit-exact for
// 250 ms: every real ramp step still draws, the deliberate full-black hold
// between fade-out and fade-in (exactly 1.0) still draws, and the leftover
// disappears a quarter second after the fade ends. (Keying this on the
// TransitionPresentation effect pointer instead also dropped real fades:
// the ramp can be written while that pointer reads 0.)
extern "C" REX_FUNC(sub_827FEFD0) {
  static uint32_t s_alpha_bits = 0;
  static std::chrono::steady_clock::time_point s_changed{};
  const uint32_t globals = REX_LOAD_U32(0x83083C38);
  if (globals != 0) {
    // Retail layout: [globals]+31376+848/+864 colour/alpha pair, selector
    // byte +880 (sk83_na_m's dev build has the block 464 bytes lower).
    const bool sel = REX_LOAD_U8(globals + 32256) != 0;
    const uint32_t alpha_bits = REX_LOAD_U32(globals + (sel ? 32224u : 32240u) + 12);
    const auto now = std::chrono::steady_clock::now();
    if (alpha_bits != s_alpha_bits) {
      s_alpha_bits = alpha_bits;
      s_changed = now;
    }
    float alpha;
    std::memcpy(&alpha, &alpha_bits, sizeof(alpha));
    if (alpha > 0.0f && alpha < 1.0f &&
        now - s_changed > std::chrono::milliseconds(250)) {
      return;
    }
  }
  __imp__sub_827FEFD0(ctx, base);
}

extern "C" REX_FUNC(sub_82808708) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808708(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

// Sk8::Render::SimpleDraw::DrawParameters::Draw: the game's immediate-mode
// quad/tri utility (chase arrows, in-world guide markers/beams, debug
// draws). Bottoms out in BeginVertices like the APT path.
extern "C" REX_FUNC(sub_82804168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(5, true);
  __imp__sub_82804168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(5, false);
}

// D3DDevice_SetPixelShader / SetVertexShader (r4 = guest shader object):
// recorded per draw to group the 2D stream by shader variant.
// sk83_na_m: retail rex_D3DDevice_SetVertexShader_82B7F408 -> 0x82D9DFD8, registered under the stable
// SDK name rex_D3DDevice_SetVertexShader_82B7F408.
extern "C" REX_FUNC(rex_D3DDevice_SetVertexShader_82B7F408) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnSetShader(true, ctx.r4.u32);
    }
  }
  {
    __imp__rex_D3DDevice_SetVertexShader_82B7F408(ctx, base);
  }
}

// sk83_na_m: retail rex_D3DDevice_SetPixelShader_82B7F150 -> 0x82D9DE18, registered under the stable
// SDK name rex_D3DDevice_SetPixelShader_82B7F150.
extern "C" REX_FUNC(rex_D3DDevice_SetPixelShader_82B7F150) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnSetShader(false, ctx.r4.u32);
    }
  }
  {
    __imp__rex_D3DDevice_SetPixelShader_82B7F150(ctx, base);
  }
}

// D3D::SetPending_RenderStates(device, u64 dirty mask, bank, ptr): the
// render-state shadow bank (blend/depth state for 2D draws lives here).
extern "C" REX_FUNC(rex_D3D_SetPending_RenderStates_YAXPAVCDevice_1_KKPAX_Z_82B83C48) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnRenderStateUpload(ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
    }
  }
  {
    __imp__rex_D3D_SetPending_RenderStates_YAXPAVCDevice_1_KKPAX_Z_82B83C48(ctx, base);
  }
}

// D3DDevice_SetViewport(device, D3DVIEWPORT*) / SetScissorRect(device,
// RECT*): recorded per draw (render-to-texture APT passes and mask rects).
// D3DDevice_SetViewport(device, D3DVIEWPORT*) / SetScissorRect(device,
// RECT*): recorded per draw (render-to-texture APT passes and mask rects).
extern "C" REX_FUNC(rex_D3D_CRingAllocList_MakeSpace_AAAPAKXZ_82B74310) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnSetViewport(base, ctx.r4.u32);
    }
  }
  {
    __imp__rex_D3D_CRingAllocList_MakeSpace_AAAPAKXZ_82B74310(ctx, base);
  }
}

extern "C" REX_FUNC(rex_D3D_SetViewport_YAXPAUD3DDevice_MMMMMMK_Z_82B769C0) {
  if (skate3::native_render::Enabled()) {
    {
      skate3::native_scene::OnSetScissor(base, ctx.r4.u32);
    }
  }
  {
    __imp__rex_D3D_SetViewport_YAXPAUD3DDevice_MMMMMMK_Z_82B769C0(ctx, base);
  }
}

// D3DDevice_BeginIndexedVertices: indexed inline path (the SimpleDraw debug
// renderer, see native_scene::SimpleDrawCap). Retail's XDK signature has no
// base-vertex argument: r4 = primitive type, r5 = vertex count, r6 = index
// count, r7 = index format, r8 = vertex stride, r9 = void** index data out,
// r10 = void** vertex data out ("mullw r11,r5,r8"; on success "stw r26,0(r9)"
// = index ring, "stw r28,0(r10)" = vertex ring). The dev build shifts these
// one register later and passes the vertex out-pointer on the stack. The
// game writes the data after this returns.
extern "C" REX_FUNC(rex_D3DDevice_BeginIndexedVertices_82B7A458) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;  // prim
  const uint32_t r6 = ctx.r5.u32;  // vertex count
  const uint32_t r7 = ctx.r6.u32;  // index count
  const uint32_t r9 = ctx.r8.u32;  // stride
  const uint32_t pp_index = ctx.r9.u32;
  const uint32_t pp_vertex = ctx.r10.u32;
  {
    skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawGame);
    __imp__rex_D3DDevice_BeginIndexedVertices_82B7A458(ctx, base);
  }
  if (enabled) {
    const uint32_t iaddr = pp_index >= 0x10000 ? REX_LOAD_U32(pp_index) : 0;
    const uint32_t vaddr = pp_vertex >= 0x10000 ? REX_LOAD_U32(pp_vertex) : 0;
    skate3::native_scene::OnInlineIndexedDraw(base, r4, r6, r7, r9, iaddr, vaddr);
  }
}


// D3DDevice_BeginVertices: inline (write-through-ring) vertex path; the
// CPU writes computed vertices directly. Post-call r3 = guest write pointer.
extern "C" REX_FUNC(rex_D3DDevice_BeginVertices_82B79FC0) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  {
    skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawGame);
    __imp__rex_D3DDevice_BeginVertices_82B79FC0(ctx, base);
  }
  if (enabled) {
    {
      skate3::native_render::HookTimer ht(skate3::native_render::kHookDrawOurs);
      skate3::native_scene::OnDrawDone(base, 2, r4, r5, r6, ctx.r3.u32 != 0 ? ctx.r3.u32 : r7);
    }
  }
}

// LivingWorld batch pack writer (unnamed; called per entity per sim tick
// from the tail of cLivingWorldPresEntityManager::Update), the LOD-
// pedestrian skinning-palette source: fused UpdateBoneTransforms+Pack
// writing the concatenated per-mesh 4x3 palettes into
// cModelInstance.m_matrices. r3 = the cLivingWorldPresEntity-derived
// entity; post-call m_matrices holds the packed rows.
// LivingWorld batch pack writer (unnamed; called per entity per sim tick
// from the tail of cLivingWorldPresEntityManager::Update), the LOD-
// pedestrian skinning-palette source: fused UpdateBoneTransforms+Pack
// writing the concatenated per-mesh 4x3 palettes into
// cModelInstance.m_matrices. r3 = the cLivingWorldPresEntity-derived
// entity; post-call m_matrices holds the packed rows.
// mapped to this bare-name address with no decorated-symbol confirmation at all;
// your own document flagged it "?" uncertain
extern "C" REX_FUNC(sub_827C1D38) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1D38(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnLwPack(base, entity);
  }
}



// audio PlugIn::SetAttribute: bounded append (see the overflow guard).
// (dev-build-only hook removed for retail: rex_rw_audio_core_PlugIn_SetAttribute_QA...)



// D3DDevice_SetTexture(this, sampler, texture): r4 = sampler index,
// r5 = guest texture. Recorded so the water branch can read the game's own
// resolved framebuffer copy (tf3) instead of substituting a colour for it.
extern "C" REX_FUNC(rex_D3DDevice_SetTexture_82B82478) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetTexture(ctx.r4.u32, ctx.r5.u32);
  }
  __imp__rex_D3DDevice_SetTexture_82B82478(ctx, base);
}
