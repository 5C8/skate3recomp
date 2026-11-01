// Native scene renderer, render-thread half: consumes the FrameScene
// published by skate3_native_scene.cpp (game-thread capture/build) and draws
// it through the RHI (D3D12 / Vulkan) into the guest output texture. Shared
// cross-thread state lives in skate3_native_scene_state.h.

#include "skate3_apt_spread.h"
#include "skate3_native_debug_dialog.h"
#include "skate3_native_font_atlas.h"
#include "skate3_native_scene.h"
#include "skate3_vr.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include <map>
#include <array>
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
// Offline-compiled SPIR-V for the native shaders (compiled from the HLSL
// sources with DXC): the Vulkan RHI backend consumes these blobs; the D3D12
// backend runtime-compiles the embedded HLSL as before.
#include "native/shaders/spirv/skate3_native_shaders_spirv.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/runtime.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/presenter.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#include "skate3_native_scene_state.h"
#include "skate3_deck_shadow.h"
#include "skate3_native_scene_gpu_internal.h"

// Native inter-frames: see InterFrameState.
// Generated frames are opt-in in Release builds (they are not real game
// frames; the settings menu explains the trade), on by default elsewhere.
#ifdef SK83_RELEASE_BUILD
#define SKATE3_INTERFRAMES_DEFAULT 0
#else
#define SKATE3_INTERFRAMES_DEFAULT 2
#endif
REXCVAR_DEFINE_INT32(skate3_native_render_scene_interframes, SKATE3_INTERFRAMES_DEFAULT, "Skate 3",
                     "At most this many generated native frames between two guest frames "
                     "(camera and moving objects posed for their display time); fewer or "
                     "none while the guest rate already reaches the target rate "
                     "(see _interframes_target_hz). 0 = off")
    .range(0, 3);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_interframes_target_hz, 0.0, "Skate 3",
                      "Presented-rate target of frame interpolation: generated frames fill "
                      "each guest frame up to this rate. 0 = the refresh rate of the monitor "
                      "showing the game");
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_present_pacing, true, "Skate 3",
                    "Pose every present at its own present time on the scenes' content "
                    "clock (steady motion steps under uneven guest/present timing); also "
                    "active with frame interpolation off");
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_pace_record, 0.0, "Skate 3",
                      "Diagnostic: record every present (time, scene, camera, player) for "
                      "this many seconds into logs/pace_<time>.csv (frame pacing / stutter "
                      "analysis); 0 = off");
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_interframes_debug_alpha, -1.0, "Skate 3",
                      "Debug: pose EVERY frame at this inter-frame alpha (0 = previous guest "
                      "frame, 1 = newest) so inter-frame artifacts show in every frame and in "
                      "screenshots; < 0 = off");
REXCVAR_DEFINE_BOOL(skate3_native_render_async, true, "Skate 3",
                    "Render native frames (and the generated inter-frames) on a dedicated "
                    "render thread instead of the GPU command thread, so the game does not "
                    "wait on native rendering (off = render on the GPU command thread)");
REXCVAR_DEFINE_BOOL(skate3_vr_linear_submit, true, "VR",
                    "Submit VR colors as linear RGBA16F to avoid the washed-out "
                    "R10/Gamma handoff on Steam Link. Disable only to compare the legacy path.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Cvars defined in skate3_native_scene.cpp (and SDK cvars re-declared there).
REXCVAR_DECLARE(bool, async_shader_compilation);
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
REXCVAR_DECLARE(bool, readback_resolve_half_pixel_offset);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(bool, skate3_native_render_scene_backface_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(bool, skate3_native_render_scene_boot_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_cas_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_char_shadow_exact);
REXCVAR_DECLARE(bool, skate3_native_render_scene_decals);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynobj_v2);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr_packed);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lightmaps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lm_dump);
REXCVAR_DECLARE(bool, skate3_native_render_scene_loading_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_macro);
REXCVAR_DECLARE(bool, skate3_native_render_scene_menu_rtt_passes);
REXCVAR_DECLARE(bool, skate3_native_render_scene_menu_unsuppress);
REXCVAR_DECLARE(bool, skate3_native_render_scene_mesh_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_pause_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_perf_interval);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_skip_guest_zprepass);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_display_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_grab_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_readback);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_refl_bias_auto);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ring);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_blend);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_inline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_selection_outline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_caster_parity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_pcss);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_static_casters);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_blocker_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_max_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_min_texel);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_sun_deg);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_bias);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_bias_vk);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_radius);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_static_size);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_static_strength);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadows);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sort_opaque);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_simpledraw);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_decal_probe);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ao_technique);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_debug);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao_full_res);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssr);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sun_override);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_mips);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_transparents);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_v2);
REXCVAR_DECLARE(double, skate3_menu_blur_sigma);
REXCVAR_DECLARE(double, skate3_native_render_scene_2d_sharp);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_knee);
REXCVAR_DECLARE(double, skate3_native_render_scene_bloom_threshold);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_x);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_y);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_lod);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_luma_protect);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssao_radius);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_hold);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_wipe);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_showcase_order);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_intensity);
REXCVAR_DECLARE(double, skate3_native_render_scene_ssr_thickness);
REXCVAR_DECLARE(double, skate3_native_render_scene_world_v2_tan_sign);
REXCVAR_DECLARE(int32_t, native_render_force_resolve_readback_max_length);
REXCVAR_DECLARE(int32_t, native_render_suppress_mode);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_2d_async_px);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_detail_hold);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_hdr_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_menu_rtt_scope);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_decode_budget);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_photo_native_accum);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_photo_native_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_prewarm_budget_ms);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_refl_mode);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_settle_max_frames);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_tile);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ssr_steps);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_tex_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_warmup_budget_ms);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_trace_mesh);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_trace_2d);
REXCVAR_DECLARE(std::string, skate3_native_render_snapshot_dir);

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)

namespace skate3::native_scene {
namespace {

// Retire a guest texture's GPU resources AND its view. Destruction is
// deferred inside the RHI against the CURRENT submission; the `submission`
// parameter is kept for call-site compatibility and ignored.
void RetireGuestTexture(const GuestTexture& t, uint64_t submission) {
  (void)submission;
  g_r.device->DestroyDeferred(t.texture);
  g_r.device->DestroyDeferred(t.upload);
  g_r.device->DestroyDeferred(t.upload_b);
  g_r.device->DestroyDeferred(t.srv);
}

// ---- Content store helpers --------------------------------------------------
// The store is words-keyed; the guest
// streamer's object retargeting (mip flap A<->B, detail demote, object
// reuse) is just different keys, so both states of any transition stay
// resident and no rebind can ever serve another binding's art.
std::atomic<uint64_t> g_store_evicted{0};
constexpr size_t kTexStoreCap = 12288;

uint32_t SwapU32(uint32_t v);  // defined with the decode helpers below

// (kTexStoreCap sizing: dense areas with the extended draw distance hold a
// ~3000-4000 entry live working set, and a map switch stacks two working
// sets; a 6144 cap kept the eviction latch cycling every ~20 s in dense
// areas, re-decoding content the player was about to face again (visible
// pop-in / late resolves). The count cap is a wide entry-bloat backstop
// now: the byte budget (skate3_native_render_scene_tex_store_mb) is the
// real VRAM bound and engages first on texture-heavy content.)

// Seqlock-stable read of a texture object's six fetch words, guest -> host
// order. The streamer rewrites the words word-by-word on its own thread; a
// mixed snapshot decodes a coherent image of the WRONG memory (a shared
// mip-pool page reads as a collage of neighbor art), and the result is
// stable, valid-looking content no fingerprint can reject after the fact;
// the read itself must be self-consistent. Two consecutive identical
// snapshots (4 attempts) or the caller keeps its previous route/skips.
bool ReadStableTexWords(uint8_t* base, uint32_t tex_ptr, uint32_t out[6]) {
  uint32_t raw[6];
  uint32_t raw2[6];
  bool stable = false;
  for (int attempt = 0; attempt < 4 && !stable; ++attempt) {
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + tex_ptr + 7 * 4, sizeof(raw2))) {
      return false;
    }
    stable = std::memcmp(raw, raw2, sizeof(raw)) == 0;
  }
  if (!stable) {
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    out[i] = SwapU32(raw[i]);
  }
  return true;
}

// Frames-per-second estimate for the eviction age guards. The LRU clocks
// are frame-stamped, so wall-clock idle thresholds must convert through the
// live frame rate: a fixed frame count that means 30 seconds at 60 fps
// passes in 3 seconds on a 600 fps loading screen, which made a whole
// map's freshly prewarmed working set eviction-eligible at takeover.
double g_evict_fps_estimate = 240.0;

void UpdateEvictFpsEstimate(uint64_t frame_number) {
  static double s_last_t = 0.0;
  static uint64_t s_last_f = 0;
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  if (s_last_t != 0.0 && now > s_last_t + 0.5) {
    const double rate = double(frame_number - s_last_f) / (now - s_last_t);
    if (rate > 1.0 && rate < 2000.0) {
      g_evict_fps_estimate = g_evict_fps_estimate * 0.7 + rate * 0.3;
    }
  }
  if (s_last_t == 0.0 || now > s_last_t + 0.5) {
    s_last_t = now;
    s_last_f = frame_number;
  }
}

// Store LRU eviction, run once per frame: superseded words states (old mip
// levels, pre-demote detail sets, one-shot UI art) hold SRV slots + GPU
// memory until nothing has routed to them for a while. Amortized: once over
// the cap, a bounded batch of oldest entries goes per frame until the store
// is back under the low-water mark (the previous evict-half-in-one-frame
// sweep retired ~1500 textures in a single frame, a measured recurring
// hitch). The idle guard is wall-clock scaled: entries used within the
// last kTexEvictMinIdleSeconds are never candidates, so a dense area whose
// working set exceeds the cap parks over it instead of thrash-evicting
// live content into a re-decode loop.
constexpr size_t kTexEvictPerFrame = 64;
constexpr double kTexEvictMinIdleSeconds = 45.0;

// Byte-budget layer over the count caps: the caps above bound ENTRY counts,
// but per-entry sizes differ per map, and after a map switch the union of
// the old and new maps' working sets sits under the idle guards for minutes
// at full size. Each store therefore also tracks an estimated GPU byte
// total (rescanned periodically, decremented per eviction); crossing the
// byte budget latches the same amortized LRU, and while the total is over
// the budget's low-water mark the idle guard shortens so the superseded
// map's content drains promptly instead of stacking VRAM across switches.
// The pressure guard stays multi-second so a live working set (stamped
// every frame it serves) is never evicted into a re-decode loop.
constexpr uint64_t kStoreByteScanIntervalFrames = 120;
constexpr double kEvictPressureIdleSeconds = 5.0;
uint64_t g_tex_store_bytes = 0;
uint64_t g_mesh_store_bytes = 0;

// Estimated bytes of one surface in the host formats the store decodes to.
uint64_t FormatSurfaceBytes(nrhi::Format format, uint32_t w, uint32_t h) {
  switch (format) {
    case nrhi::Format::kBC1_UNORM:
    case nrhi::Format::kBC4_UNORM:
      return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case nrhi::Format::kBC2_UNORM:
    case nrhi::Format::kBC3_UNORM:
    case nrhi::Format::kBC5_UNORM:
      return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case nrhi::Format::kR8_UNORM:
      return uint64_t(w) * h;
    case nrhi::Format::kR8G8_UNORM:
    case nrhi::Format::kB5G6R5_UNORM:
      return uint64_t(w) * h * 2;
    default:  // kR8G8B8A8_UNORM and anything unanticipated
      return uint64_t(w) * h * 4;
  }
}

// Committed-texture footprint estimate (base level + the mip chain's
// asymptotic 1/3; cube entries carry 6 faces). Accuracy at the few-percent
// level is enough: this feeds the eviction budget, not an allocator.
uint32_t GuestTextureGpuBytes(const GuestTexture& t, uint32_t faces = 1) {
  if (t.texture == nullptr) {
    return 0;
  }
  uint64_t bytes = FormatSurfaceBytes(t.texture->format(), t.texture->width(),
                                      t.texture->height());
  if (t.srv_mips != 1) {
    bytes += bytes / 3;
  }
  bytes *= faces;
  return bytes > UINT32_MAX ? UINT32_MAX : uint32_t(bytes);
}

void EvictTexStore(uint64_t frame_number, uint64_t submission) {
  static bool s_evicting = false;
  static uint64_t s_next_scan_frame = 0;
  static uint64_t s_evicted_run = 0;
  static uint64_t s_next_bytes_frame = 0;
  if (frame_number >= s_next_bytes_frame) {
    s_next_bytes_frame = frame_number + kStoreByteScanIntervalFrames;
    uint64_t total = 0;
    for (auto& [k, t] : g_r.tex_store) {
      if (t.gpu_bytes == 0) {
        t.gpu_bytes = GuestTextureGpuBytes(t);
      }
      total += t.gpu_bytes;
      // In-place-update entries retain their staging ping-pong buffers,
      // which land in device-local memory on resizable-BAR systems.
      if (t.upload != nullptr) {
        total += t.upload->size();
      }
      if (t.upload_b != nullptr) {
        total += t.upload_b->size();
      }
    }
    g_tex_store_bytes = total;
  }
  const uint64_t byte_cap =
      uint64_t(std::max(256, REXCVAR_GET(skate3_native_render_scene_tex_store_mb)))
      << 20;
  const uint64_t byte_low = byte_cap - byte_cap / 8;
  const size_t low_water = kTexStoreCap - kTexStoreCap / 8;
  const bool was_evicting = s_evicting;
  if (g_r.tex_store.size() > kTexStoreCap || g_tex_store_bytes > byte_cap) {
    s_evicting = true;
  } else if (g_r.tex_store.size() <= low_water && g_tex_store_bytes <= byte_low) {
    s_evicting = false;
  }
  const bool byte_pressure = g_tex_store_bytes > byte_low;
  const uint64_t min_idle_frames = std::max<uint64_t>(
      4, uint64_t(g_evict_fps_estimate * (byte_pressure ? kEvictPressureIdleSeconds
                                                        : kTexEvictMinIdleSeconds)));
  if (was_evicting != s_evicting) {
    if (s_evicting) {
      s_evicted_run = 0;
      REXLOG_INFO(
          "native-scene: tex store LRU start (size={} cap={} mb={} cap_mb={} "
          "min_idle={}f @ {:.0f}fps)",
          g_r.tex_store.size(), kTexStoreCap, g_tex_store_bytes >> 20,
          byte_cap >> 20, min_idle_frames, g_evict_fps_estimate);
    } else {
      REXLOG_INFO("native-scene: tex store LRU done (size={} mb={} evicted={})",
                  g_r.tex_store.size(), g_tex_store_bytes >> 20, s_evicted_run);
    }
  }
  if (!s_evicting || frame_number < s_next_scan_frame) {
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> ages;  // (last-used frame, key)
  ages.reserve(g_r.tex_store.size());
  for (const auto& [k, t] : g_r.tex_store) {
    if (t.last_used_frame + min_idle_frames < frame_number) {
      ages.emplace_back(t.last_used_frame, k);
    }
  }
  size_t excess = g_r.tex_store.size() > low_water
                      ? g_r.tex_store.size() - low_water
                      : 0;
  if (byte_pressure) {
    // Keep draining while over the byte budget even with the count under
    // its cap: convert the byte excess into entries via the average size.
    const uint64_t avg =
        std::max<uint64_t>(1, g_tex_store_bytes /
                                  std::max<size_t>(1, g_r.tex_store.size()));
    excess = std::max(excess, size_t((g_tex_store_bytes - byte_low) / avg) + 1);
  }
  const size_t n = std::min({ages.size(), excess, kTexEvictPerFrame});
  if (n == 0) {
    // Everything over the cap is recent (a dense area's live working set):
    // park and re-scan later instead of burning a full-store scan per frame.
    s_next_scan_frame = frame_number + 120;
    return;
  }
  std::nth_element(ages.begin(), ages.begin() + (n - 1), ages.end());
  for (size_t i = 0; i < n; ++i) {
    const auto it = g_r.tex_store.find(ages[i].second);
    if (it != g_r.tex_store.end()) {
      g_tex_store_bytes -= std::min<uint64_t>(g_tex_store_bytes,
                                              it->second.gpu_bytes);
      RetireGuestTexture(it->second, submission);
      g_r.tex_store.erase(it);
    }
  }
  s_evicted_run += n;
  g_store_evicted.fetch_add(n, std::memory_order_relaxed);
}

// Mesh cache LRU, same amortized shape: without it every streamed arena's
// meshes accumulated for the whole session (observed: 61k live buffers
// after a few minutes of map changes). The wall-clock idle guard keeps the
// current map's prewarmed set resident (prewarm decodes thousands of
// meshes the player has not seen yet; evicting them defeats the prewarm
// and turns panning into a re-decode churn loop); an evicted mesh
// re-decodes on the workers like any first sight.
// (Cap sizing: dense maps prewarm ~21k meshes at ~13 KB average, ~280 MB
// total - a 6144 cap forced the whole prewarm through a churn funnel for
// trivial VRAM. The count cap is an entry backstop; the byte budget
// (skate3_native_render_scene_mesh_store_mb) is the VRAM bound.)
constexpr size_t kMeshStoreCap = 24576;
constexpr size_t kMeshEvictPerFrame = 64;
constexpr double kMeshEvictMinIdleSeconds = 90.0;

void EvictMeshStore(uint64_t frame_number) {
  static bool s_evicting = false;
  static uint64_t s_next_scan_frame = 0;
  static uint64_t s_evicted_run = 0;
  static uint64_t s_next_bytes_frame = 0;
  if (frame_number >= s_next_bytes_frame) {
    s_next_bytes_frame = frame_number + kStoreByteScanIntervalFrames;
    uint64_t total = 0;
    for (const auto& [k, m] : g_r.meshes) {
      total += uint64_t(m.vb_view.size_bytes) + m.ib_view.size_bytes;
    }
    g_mesh_store_bytes = total;
  }
  const uint64_t byte_cap =
      uint64_t(std::max(256, REXCVAR_GET(skate3_native_render_scene_mesh_store_mb)))
      << 20;
  const uint64_t byte_low = byte_cap - byte_cap / 8;
  const size_t low_water = kMeshStoreCap - kMeshStoreCap / 8;
  const bool was_evicting = s_evicting;
  if (g_r.meshes.size() > kMeshStoreCap || g_mesh_store_bytes > byte_cap) {
    s_evicting = true;
  } else if (g_r.meshes.size() <= low_water && g_mesh_store_bytes <= byte_low) {
    s_evicting = false;
  }
  const bool byte_pressure = g_mesh_store_bytes > byte_low;
  const uint64_t min_idle_frames =
      byte_pressure
          ? std::max<uint64_t>(
                240, uint64_t(g_evict_fps_estimate * kEvictPressureIdleSeconds))
          : std::max<uint64_t>(
                1800, uint64_t(g_evict_fps_estimate * kMeshEvictMinIdleSeconds));
  if (was_evicting != s_evicting) {
    if (s_evicting) {
      s_evicted_run = 0;
      REXLOG_INFO(
          "native-scene: mesh store LRU start (size={} cap={} mb={} cap_mb={} "
          "min_idle={}f @ {:.0f}fps)",
          g_r.meshes.size(), kMeshStoreCap, g_mesh_store_bytes >> 20,
          byte_cap >> 20, min_idle_frames, g_evict_fps_estimate);
    } else {
      REXLOG_INFO("native-scene: mesh store LRU done (size={} mb={} evicted={})",
                  g_r.meshes.size(), g_mesh_store_bytes >> 20, s_evicted_run);
    }
  }
  if (!s_evicting || frame_number < s_next_scan_frame) {
    return;
  }
  std::vector<std::pair<uint64_t, uint32_t>> ages;  // (last-used frame, key)
  ages.reserve(g_r.meshes.size());
  for (const auto& [k, m] : g_r.meshes) {
    if (m.last_used_frame + min_idle_frames < frame_number) {
      ages.emplace_back(m.last_used_frame, k);
    }
  }
  size_t excess =
      g_r.meshes.size() > low_water ? g_r.meshes.size() - low_water : 0;
  if (byte_pressure) {
    const uint64_t avg = std::max<uint64_t>(
        1, g_mesh_store_bytes / std::max<size_t>(1, g_r.meshes.size()));
    excess = std::max(excess, size_t((g_mesh_store_bytes - byte_low) / avg) + 1);
  }
  const size_t n = std::min({ages.size(), excess, kMeshEvictPerFrame});
  if (n == 0) {
    // Everything over the cap is in recent use (a dense map's population):
    // park and re-scan later instead of burning a full-store scan per frame.
    s_next_scan_frame = frame_number + 120;
    return;
  }
  std::nth_element(ages.begin(), ages.begin() + (n - 1), ages.end());
  for (size_t i = 0; i < n; ++i) {
    const auto it = g_r.meshes.find(ages[i].second);
    if (it != g_r.meshes.end()) {
      g_mesh_store_bytes -= std::min<uint64_t>(
          g_mesh_store_bytes,
          uint64_t(it->second.vb_view.size_bytes) + it->second.ib_view.size_bytes);
      g_r.device->DestroyDeferred(it->second.vb);
      g_r.device->DestroyDeferred(it->second.ib);
      g_r.ropa_shapes.erase(it->first);
      g_r.meshes.erase(it);
    }
  }
  s_evicted_run += n;
}

// Environment-cube cache LRU: a handful of cubes serve any one area, but
// entries are keyed by guest object address and accumulated across every
// map/park visited in the session. Amortized one eviction per frame; the
// idle guard keeps anything recently sampled resident.
constexpr size_t kCubeStoreCap = 48;

void EvictCubeStore(uint64_t frame_number, uint64_t submission) {
  if (g_r.cube_textures.size() <= kCubeStoreCap) {
    return;
  }
  const uint64_t min_idle_frames = std::max<uint64_t>(
      4, uint64_t(g_evict_fps_estimate * kTexEvictMinIdleSeconds));
  auto oldest = g_r.cube_textures.end();
  for (auto it = g_r.cube_textures.begin(); it != g_r.cube_textures.end(); ++it) {
    if (it->second.last_used_frame + min_idle_frames < frame_number &&
        (oldest == g_r.cube_textures.end() ||
         it->second.last_used_frame < oldest->second.last_used_frame)) {
      oldest = it;
    }
  }
  if (oldest != g_r.cube_textures.end()) {
    RetireGuestTexture(oldest->second, submission);
    g_r.cube_textures.erase(oldest);
    g_store_evicted.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- Staged texture decode (worker-thread half) ---------------------------
// The texture decoders normally finish by recording GPU copies into the
// frame's command stream, pushing a barrier and creating the SRV, all
// render-thread-only. When `g_tex_stage_out` is set (decode worker), they
// stop after filling the upload resource and export what the render-thread
// commit needs instead. RHI resource creation and upload mapping are
// thread-safe, so everything up to that point is safe off-thread.
struct StagedMipCopy {
  uint32_t offset, pitch, w, h;  // upload footprint per mip
};
struct StagedTexCommit {
  nrhi::Format copy_format = nrhi::Format::kUnknown;
  nrhi::Format srv_format = nrhi::Format::kUnknown;
  nrhi::Swizzle swizzle[4] = {nrhi::Swizzle::kX, nrhi::Swizzle::kY,
                              nrhi::Swizzle::kZ, nrhi::Swizzle::kW};
  uint32_t mip_count = 0;
  // Cube map (environment cubes): mips[] holds face-major (face * levels +
  // mip) subresource copies, the old D3D12 subresource numbering, which the
  // commit decomposes back into (face, mip), and the SRV is a kCube view
  // with cube_mip_levels levels. The cube MIP CHAIN
  // is load-bearing: the game's reflective glass perturbs its reflection
  // vector with a per-pixel normal map, so the hardware cube fetch runs at
  // a DEEP gradient-derived LOD; a mip-0-only cube shows the plaza cube's
  // baked streetlight heads as a sharp magnified smear the real console
  // output blurs away.
  bool cube = false;
  uint32_t cube_mip_levels = 1;
  // 6 faces x up to 16 levels: levels = 1 + countr_zero(width), so a 4096
  // cube needs 6 * 13 = 78 entries. The old 64 fit only a 512 cube and
  // anything larger overflowed the array.
  static constexpr uint32_t kMaxStagedMips = 6 * 16;
  StagedMipCopy mips[kMaxStagedMips] = {};
};
thread_local StagedTexCommit* g_tex_stage_out = nullptr;

// Render-thread half: record the staged upload's copies + barrier, create
// the SRV, mark the texture live. On view-creation failure the texture stays
// invalid (renders white; the backend-managed views make this near-impossible).
void CommitStagedGuestTexture(const NativeGuestOutputRenderContext& context,
                              GuestTexture& gt, const StagedTexCommit& sc) {
  const uint32_t staged =
      std::min<uint32_t>(sc.mip_count, StagedTexCommit::kMaxStagedMips);
  for (uint32_t m = 0; m < staged; ++m) {
    const StagedMipCopy& p = sc.mips[m];
    // Cube commits stage face-major (face * levels + mip) entries (the old
    // D3D12 subresource numbering): decompose into (mip, face).
    const uint32_t mip = sc.cube ? m % sc.cube_mip_levels : m;
    const uint32_t face = sc.cube ? m / sc.cube_mip_levels : 0;
    context.cmd->CopyBufferToTexture(gt.texture, mip, face, gt.upload, p.offset,
                                     p.pitch, p.w, p.h, 1);
  }
  context.cmd->Barrier(gt.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  // The copies are recorded; the staging buffer has no further use (the
  // in-place update path recreates its own on demand), and holding it for
  // the cache entry's lifetime doubled the store's memory. Destruction is
  // deferred until the recorded submission completes.
  g_r.device->DestroyDeferred(gt.upload);
  gt.upload = nullptr;
  nrhi::TextureViewDesc vd;
  vd.format = sc.srv_format;
  for (uint32_t c = 0; c < 4; ++c) {
    vd.swizzle[c] = sc.swizzle[c];
  }
  if (sc.cube) {
    vd.dimension = nrhi::ViewDimension::kCube;
    vd.mip_levels = sc.cube_mip_levels;
  } else {
    vd.dimension = nrhi::ViewDimension::k2D;
    vd.mip_levels = sc.mip_count;
    gt.srv_format = sc.srv_format;
    gt.srv_mips = sc.mip_count;
  }
  gt.srv = g_r.device->CreateTextureView(gt.texture, vd);
  if (gt.srv == nullptr) {
    gt.valid = false;
    return;
  }
  gt.valid = true;
}

// Worker-pool result plumbing (see the prewarm queue globals above; these
// live here because they need the resource/item types).
struct StagedTexResult {
  uint32_t key = 0;        // guest texture object address (object-keyed cache)
  uint64_t words_key = 0;  // != 0: content-store result (g_r.tex_store)
  bool cube = false;       // environment cube (g_r.cube_textures)
  GuestTexture gt;
  StagedTexCommit commit;
  bool valid = false;
  // The commit-time stability verify rejected this decode (payload moved
  // between the worker's read and the commit): retry fast, not on the
  // failed-decode ladder.
  bool verify_failed = false;
  // 2D/HUD overlay origin: skip the stability verify (see PrewarmEntry::ui).
  bool ui = false;
};
struct PrewarmResult {
  DrawItem item;
  MeshBuffers buffers;
  bool mesh_valid = false;
  std::vector<StagedTexResult> textures;
  // Draw-path miss result (visible right now): the commit takes it this
  // frame regardless of the gameplay per-frame cap.
  bool miss = false;
};
std::mutex g_prewarm_out_mutex;
std::vector<PrewarmResult> g_prewarm_out;
// Failed builds (buffer objects not initialized yet) land here; the render
// thread re-injects them each frame so retries are frame-paced instead of
// hot-spinning the workers.
std::vector<PrewarmEntry> g_prewarm_retry;  // under g_prewarm_out_mutex

// Shader sources live in src/native/shaders/*.hlsl; the build embeds them
// into this generated header (cmake/EmbedShaders.cmake) so the exe stays
// self-contained. Same kFooSource char arrays as before. (The offline
// SPIR-V table for the same shaders is included at the top of the file;
// it declares ::skate3::native_spirv and must not nest inside this
// namespace.)
#include "skate3_native_shaders.h"

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  if (exp == 0) {
    if (mant == 0) return std::bit_cast<float>(sign);
    // subnormal
    while (!(mant & 0x400)) {
      mant <<= 1;
      --exp;
    }
    ++exp;
    mant &= 0x3FF;
  } else if (exp == 31) {
    return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
  }
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (mant << 13));
}

nrhi::Buffer* CreateUploadBuffer(
    nrhi::Device* device, size_t size,
    nrhi::BufferBindClass bind_class = nrhi::BufferBindClass::kFull) {
  nrhi::BufferDesc desc;
  desc.size = size;
  desc.heap = nrhi::HeapKind::kUpload;
  desc.bind_class = bind_class;
  return device->CreateBuffer(desc);  // nullptr on failure, like the old helper
}

// ---- Dynamic-mesh buffer reuse pool ---------------------------------------
// Cloth/ropa garments and streaming heals replace their VB/IB every commit;
// creating and destroying upload buffers per frame churns the allocator and
// contends with the decode workers. Replaced buffers park here keyed by
// exact byte size and are reused once the GPU is past the submission that
// last referenced them (a stable garment then alternates between two
// allocations). Thread-safe: decode workers and the render thread both
// acquire and retire.
struct PooledMeshBuffer {
  nrhi::Buffer* buffer = nullptr;
  uint64_t retire_submission = 0;
};
std::mutex g_mesh_pool_mutex;
std::vector<PooledMeshBuffer> g_mesh_pool;
constexpr size_t kMeshPoolCap = 96;

void PoolMeshBuffer(nrhi::Device* device, nrhi::Buffer* buffer) {
  if (buffer == nullptr) return;
  nrhi::Buffer* overflow = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
    if (g_mesh_pool.size() >= kMeshPoolCap) {
      overflow = g_mesh_pool.front().buffer;
      g_mesh_pool.erase(g_mesh_pool.begin());
    }
    g_mesh_pool.push_back({buffer, device->CurrentSubmission()});
  }
  device->DestroyDeferred(overflow);
}

nrhi::Buffer* AcquireMeshUploadBuffer(nrhi::Device* device, size_t size) {
  {
    const uint64_t completed = device->CompletedSubmission();
    std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
    for (auto it = g_mesh_pool.begin(); it != g_mesh_pool.end(); ++it) {
      if (it->buffer->size() == size && it->retire_submission < completed) {
        nrhi::Buffer* buffer = it->buffer;
        *it = g_mesh_pool.back();
        g_mesh_pool.pop_back();
        return buffer;
      }
    }
  }
  return CreateUploadBuffer(device, size, nrhi::BufferBindClass::kVertexIndex);
}

uint16_t SwapU16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
uint32_t SwapU32(uint32_t v) {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

// Upload sub-allocation alignment for per-mip footprints (the old
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; row pitches use
// nrhi::kRowPitchAlignment = the old D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
constexpr uint32_t kUploadPlacementAlignment = 512;

// Decode guest vertices into {float3 position, float2 uv, float2 uv2,
// unorm4 blend weights, u8x4 blend indices, float3 normal, float2 decal_uv}
// (56-byte stride). decal_uv = the 3rd/4th halves of a half4 first texcoord
// (the packed second UV pair environment.decal art is mapped with), equal to
// the first pair when the element has only two halves.
// vb_payload/ib_payload: optional pre-copied guest payload snapshots (the
// dynamic cloth decode jobs snapshot on the guest thread and convert on a
// worker, see DynDecodeJob); when null the payloads are read live from
// guest memory with the guarded copy.
// Buffers + extents that define a piece's geometry layout (the same
// fields the scene builder's rebuild detection keys on).
uint64_t MeshLayoutKey(const DrawItem& item) {
  // Cloth-simmed (ROPA) garments rotate their CPU-written vertices through
  // several vertex-buffer objects (the hair was seen cycling more than two),
  // so the VB object is no piece identity for them: a real garment swap
  // brings its own index buffer and counts, which the key keeps.
  uint64_t k = 1469598103934665603ull;
  // The decal-UV source is part of the layout: two meshes can share buffers
  // and stride yet decode slot 12/13 from different elements (a dedicated
  // usage-3 texcoord vs the half4 TEXCOORD0 .zw fallback). Without it a
  // resident decode made under the old rule is never rebuilt. decal2 too:
  // it moves the extra element from slot 12/13 to the layer-2 word; and the
  // trafficlight layout (lit/unlit uv + lamp index).
  for (uint64_t v : {item.ropa ? 0ull : uint64_t(item.vb_obj), uint64_t(item.ib_obj),
                     uint64_t(item.vb_bytes), uint64_t(item.ib_count), uint64_t(item.stride),
                     uint64_t(item.uv3_fmt), uint64_t(item.uv3_offset),
                     uint64_t(item.decal2), uint64_t(item.env_family == 17)}) {
    k = (k ^ v) * 1099511628211ull;
  }
  return k;
}

// Is `item` a DIFFERENT piece than the resident decode (layout key
// `resident`)? Cloth-simmed (ROPA) garments ping-pong their vertices
// between two buffer objects frame to frame, so their key alternates
// between two values; treating each flip as a garment swap erased the
// garment's shape-generation history EVERY frame (ring=1: the play-clock
// blend never had its generations, and the hair/shirt drew the newest raw
// shape ahead of the smoothed body). Same rule as the scene side's
// character rebuild detector: a key matching either of the last two seen
// for the mesh is the same piece; only a third key is a swap (the
// create-a-skater garment swap at a reused mesh address). Render thread.
bool ForeignLayout(uint32_t mesh, uint64_t resident, uint64_t key) {
  struct Pair {
    uint64_t k0 = 0;
    uint64_t k1 = 0;
  };
  static std::unordered_map<uint32_t, Pair> s_hist;
  if (s_hist.size() > 16384) {
    s_hist.clear();
  }
  Pair& h = s_hist[mesh];
  if (h.k0 == 0) {
    h.k0 = resident;
  }
  if (key == h.k0 || key == h.k1 || key == resident) {
    if (key != h.k0) {
      h.k1 = h.k0;
      h.k0 = key;
    }
    return false;
  }
  h.k1 = 0;
  h.k0 = key;
  return true;
}

bool DecodeMesh(nrhi::Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out, const uint8_t* vb_payload = nullptr,
                const uint8_t* ib_payload = nullptr) {
  out.layout = MeshLayoutKey(item);
  out.bones_used = {};
  out.bone_centroids.clear();
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  // This runs on the render thread; the guest payloads were valid on the
  // game thread this frame but streaming can decommit them in between.
  // Copy them out with the lock-free guarded copy (never VirtualQuery here:
  // the VAD lock stalls behind the guest streaming threads while panning).
  static thread_local std::vector<uint8_t> vb_scratch;
  static thread_local std::vector<uint8_t> ib_scratch;
  if (vb_payload == nullptr) {
    vb_scratch.resize(item.vb_bytes);
    if (!GuestTryCopy(vb_scratch.data(), base + item.vb_addr, item.vb_bytes)) {
      return false;
    }
    vb_payload = vb_scratch.data();
  }
  if (!item.cloth_quads && ib_payload == nullptr) {
    ib_scratch.resize(size_t(item.ib_count) * 2);
    if (!GuestTryCopy(ib_scratch.data(), base + item.ib_addr, size_t(item.ib_count) * 2)) {
      return false;
    }
    ib_payload = ib_scratch.data();
  }
  nrhi::Buffer* vb = AcquireMeshUploadBuffer(device, size_t(num_verts) * 56);
  nrhi::Buffer* ib = AcquireMeshUploadBuffer(device, size_t(item.ib_count) * 2);
  if (!vb || !ib) {
    PoolMeshBuffer(device, vb);
    PoolMeshBuffer(device, ib);
    return false;
  }

  // two_sided_sheet detection eligibility (see MeshBuffers): small static
  // triangle-list meshes only. Strips alternate winding per triangle and
  // skinned/cloth meshes deform, so both stay on the uncull(ed) PSO.
  bool detect_sheet =
      !item.skinned && !item.cloth_quads && item.ib_count >= 6 && item.ib_count <= 8192;
  for (const DrawEntry& de : item.draws) {
    if (de.prim != 4) {
      detect_sheet = false;
    }
  }
  std::vector<float> sheet_pos;
  if (detect_sheet) {
    sheet_pos.reserve(size_t(num_verts) * 3);
  }

  const uint8_t* src_vb = vb_payload;
  float* dst = nullptr;
  uint32_t garbage = 0;
  int min_bi = 255;
  int max_bi = -1;
  dst = static_cast<float*>(device->Map(vb));
  if (dst == nullptr) {
    device->DestroyDeferred(vb);
    device->DestroyDeferred(ib);
    return false;
  }
  // Decal-art UV source, decided ONCE for the mesh. These layouts carry two
  // candidates and exactly one of them is a real UV set; the other is a
  // per-vertex CONSTANT (measured in capture 1790952904: of the fam 3/4
  // meshes, 4 have a constant TEXCOORD0.zw with a varying usage-3 element,
  // 4 are the other way round, and the rest carry no usage-3 element at
  // all). Sampling the constant one pins the whole surface to a single
  // texel of the art -- on the curb strips that texel is in the fully
  // transparent bottom row, so the weathering vanished entirely. Probe a
  // few vertices and take whichever actually varies.
  bool decal_uv_from_tc3 = false;
  if (item.uv3_fmt != 0 && num_verts > 1) {
    const uint32_t probe = num_verts < 32u ? num_verts : 32u;
    const auto varies = [&](uint32_t off, uint32_t fmt) {
      float u0 = 0.0f, w0 = 0.0f;
      for (uint32_t v = 0; v < probe; ++v) {
        float u = 0.0f, w = 0.0f;
        const uint8_t* q = src_vb + size_t(v) * item.stride + off;
        if (fmt == 31 || fmt == 32) {
          u = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          w = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
        } else {
          return false;  // only the half formats appear on these layouts
        }
        if (v == 0) {
          u0 = u;
          w0 = w;
        } else if (std::fabs(u - u0) > 1e-4f || std::fabs(w - w0) > 1e-4f) {
          return true;
        }
      }
      return false;
    };
    const bool zw_varies =
        item.uv_fmt == 32 && varies(item.uv_offset + 4, 31);
    const bool tc3_varies = varies(item.uv3_offset, item.uv3_fmt);
    // decal2environment meshes (the stride-36 layouts that carry this extra
    // element) follow their VS exactly: tf4's art ALWAYS reads
    // TEXCOORD0.zw (VS 19-23: o5.xy = r1.zw), and the extra element is the
    // SECOND layer's UV (VS 7/23: o5.zw, PS tf5 at r5.wz), packed below. A
    // constant zw is then genuinely a single art texel in the game too;
    // substituting the extra element sampled tf4's art at layer 2's tiling
    // UVs under tf4's (clamping) sampler: one stretched texel row.
    decal_uv_from_tc3 = !item.decal2 && tc3_varies && !zw_varies;
  }
  // decal2 layer-2 UV (VSOut::uv4): the extra element's two raw halves,
  // packed into the otherwise unused blend-index word of these static meshes.
  const bool decal2_uv4 = item.decal2 && !item.skinned && item.uv3_offset != 0 &&
                          (item.uv3_fmt == 31 || item.uv3_fmt == 32);
  // trafficlight (fam 17), per trafficlight_*_defaultVS: a 3-component
  // texcoord carries one cell's uv (xy) and the lamp index (floor(z * 4),
  // VS 11-12 with literal c255.x = 4), a 2-component one the other cell's
  // uv; o0.xy = lamp state > 1 ? 2-component uv : 3-component xy (VS
  // 21/24, cndeq picks its first source when the test is 0). 3-component
  // xy -> slot 3/4, 2-component -> 12/13, lamp index -> blend-index byte 0
  // (statics never skin). The 3-component element is TEXCOORD0 in the VS
  // fetch order; take whichever element actually has a z.
  const bool traffic = item.env_family == 17 && !item.skinned;
  const auto has_z = [](uint32_t f) { return f == 26 || f == 32 || f == 38 || f == 57; };
  uint32_t lit_fmt = item.uv_fmt, lit_off = item.uv_offset;
  uint32_t unlit_fmt = item.uv2_fmt, unlit_off = item.uv2_offset;
  if (traffic && !has_z(lit_fmt) && has_z(unlit_fmt)) {
    std::swap(lit_fmt, unlit_fmt);
    std::swap(lit_off, unlit_off);
  }
  for (uint32_t v = 0; v < num_verts; ++v) {
    const uint8_t* p = src_vb + size_t(v) * item.stride + item.pos_offset;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    switch (item.pos_fmt) {
      case 57: {  // k_32_32_32_FLOAT
        x = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p)));
        y = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 4)));
        z = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 8)));
        break;
      }
      case 32: {  // k_16_16_16_16_FLOAT
        x = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p)));
        y = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 2)));
        z = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 4)));
        break;
      }
      case 26: {  // k_16_16_16_16 snorm character dequant
        const auto s16 = [&](int off) {
          return int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(p + off)));
        };
        constexpr float kScale = 2.0f / 32767.0f;
        x = s16(0) * kScale;
        y = s16(2) * kScale + 0.8f;
        z = s16(4) * kScale;
        break;
      }
      default:
        device->Unmap(vb);
        device->DestroyDeferred(vb);
        device->DestroyDeferred(ib);
        return false;
    }
    if (!(x == x && y == y && z == z) ||
        x < item.bbox_min[0] - 2.f || x > item.bbox_max[0] + 2.f ||
        y < item.bbox_min[1] - 2.f || y > item.bbox_max[1] + 2.f ||
        z < item.bbox_min[2] - 2.f || z > item.bbox_max[2] + 2.f) {
      ++garbage;
    }
    const auto decode_uv = [&](uint32_t fmt, uint32_t offset, float& u, float& w) {
      u = 0.0f;
      w = 0.0f;
      if (fmt == 0) return;
      const uint8_t* q = src_vb + size_t(v) * item.stride + offset;
      switch (fmt) {
        case 31:  // k_16_16_FLOAT
        case 32:  // k_16_16_16_16_FLOAT (use xy)
          u = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          w = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          break;
        case 37:  // k_32_32_FLOAT (hair strand-alpha UV; xenos enum 37)
        case 38:  // k_32_32_32_32_FLOAT (use xy)
        case 57:  // k_32_32_32_FLOAT (use xy)
          u = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q)));
          w = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q + 4)));
          break;
        case 25: {  // k_16_16 (snorm; UVs span the full s16 range for [0,1])
          const int16_t su = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          const int16_t sv = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          u = su / 32767.0f;
          w = sv / 32767.0f;
          break;
        }
        case 26: {  // k_16_16_16_16 (snorm; use xy: unwrap UV sets)
          const int16_t su = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          const int16_t sv = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          u = su / 32767.0f;
          w = sv / 32767.0f;
          break;
        }
        default:
          break;
      }
    };
    dst[v * 14 + 0] = x;
    dst[v * 14 + 1] = y;
    dst[v * 14 + 2] = z;
    if (detect_sheet) {
      sheet_pos.push_back(x);
      sheet_pos.push_back(y);
      sheet_pos.push_back(z);
    }
    decode_uv(item.uv_fmt, item.uv_offset, dst[v * 14 + 3], dst[v * 14 + 4]);
    decode_uv(item.uv2_fmt, item.uv2_offset, dst[v * 14 + 5], dst[v * 14 + 6]);
    // The second texcoord is the lightmap/decal UNWRAP, stored as an
    // absolute value with tangent handedness in the sign bits, in BOTH the
    // s16-snorm (fmt 26, decal meshes) and half-float (fmt 31, world tiles)
    // encodings (decalenvironment VS: o0.zw = |uv|; baseenvironment VS:
    // maxs r2.zw = |r4.xy|). Raw signed values sample mirrored atlas cells.
    // Exception: hair meshes, their second texcoord is the strand-alpha
    // UV, passed RAW by the hair VS (o0.zw = uv, float2), and the ocean,
    // whose VS passes the lightmap UV raw (Out.UV.zw = In.LM, no abs),
    // and CAS skin, whose VS feeds the raw texcoord (w = 1) to the
    // custom-graphic rows (see DrawItem::char_skin).
    // cacstamp_* VSes apply c190/c191 directly to the raw second UV (the
    // retail ucode has no abs/mirror operation before those dot products).
    // Keep it signed for skin, deck-shift, and stamped-cloth graphics; the
    // generic character/environment lightmap path still uses |uv2|.
    if (item.char_family < 4 && item.water_ocean == 0 && !item.char_skin &&
        !item.char_shift && !item.char_cloth_stamp) {
      dst[v * 14 + 5] = std::fabs(dst[v * 14 + 5]);
      dst[v * 14 + 6] = std::fabs(dst[v * 14 + 6]);
    }
    // Blend weights (unorm bytes) and indices (raw bytes). Guest u8x4
    // attributes are stored big-endian per 32-bit word; swap so component k
    // in the shader matches guest component k.
    uint32_t bw = 0;
    uint32_t bi = 0;
    if (item.skinned) {
      bw = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bw_offset));
      bi = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bi_offset));
      for (int k = 0; k < 4; ++k) {
        const uint8_t weight = uint8_t(bw >> (k * 8));
        const uint8_t index = uint8_t(bi >> (k * 8));
        if (weight != 0) {
          if (index < min_bi) min_bi = index;
          if (index > max_bi) max_bi = index;
          out.bones_used[index >> 6] |= 1ull << (index & 63);
          if (out.bone_centroids.size() <= index) {
            out.bone_centroids.resize(size_t(index) + 1, {0.0f, 0.0f, 0.0f, 0.0f});
          }
          const float wf = float(weight) * (1.0f / 255.0f);
          std::array<float, 4>& cen = out.bone_centroids[index];
          cen[0] += dst[v * 14 + 0] * wf;
          cen[1] += dst[v * 14 + 1] * wf;
          cen[2] += dst[v * 14 + 2] * wf;
          cen[3] += wf;
        }
      }
    }
    if (decal2_uv4) {
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv3_offset;
      bi = uint32_t(SwapU16(*reinterpret_cast<const uint16_t*>(q))) |
           (uint32_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2))) << 16);
    }
    if (traffic && has_z(lit_fmt)) {
      const uint8_t* q = src_vb + size_t(v) * item.stride + lit_off;
      float lz = 0.0f;
      if (lit_fmt == 32) {
        lz = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      } else if (lit_fmt == 26) {
        lz = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4))) / 32767.0f;
      } else {
        lz = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q + 8)));
      }
      const float lamp = std::floor(lz * 4.0f);
      bi = (std::isfinite(lamp) && lamp >= 0.0f && lamp < 255.0f) ? uint32_t(lamp) : 255u;
    }
    std::memcpy(&dst[v * 14 + 7], &bw, 4);
    std::memcpy(&dst[v * 14 + 8], &bi, 4);
    if (item.cloth_quads) {
      // lionREC_RenderVS: vfetch_mini r1.zyxw, Offset=3 (byte 12),
      // FMT_8_8_8_8. Component x of that format is the LOW byte of the
      // big-endian word, so the word 0xAARRGGBB fetches as (B, G, R, A) and
      // the .zyxw swizzle rebuilds (R, G, B, A) -- the D3DCOLOR the particle
      // system writes. Captured proof (landing dust): the colour words ramp
      // the FIRST byte (7, 20, 34, 46, ... 255) with the other three pinned
      // at 255, i.e. white puffs with a fading alpha. Reading x as the high
      // byte instead made that ramp the BLUE channel of an always-opaque
      // white particle: dust that never faded and read yellow-green.
      // Packed unorm into the blend-weight bytes (particles never skin); the
      // VS passes them to the PS through tanb (identity world).
      const uint32_t cw = SwapU32(*reinterpret_cast<const uint32_t*>(
          src_vb + size_t(v) * item.stride + 12));
      const uint32_t a = (cw >> 24) & 0xFFu, r = (cw >> 16) & 0xFFu,
                     g = (cw >> 8) & 0xFFu, b = cw & 0xFFu;
      const uint32_t packed = r | (g << 8) | (b << 16) | (a << 24);
      std::memcpy(&dst[v * 14 + 7], &packed, 4);
    }
    // Decal-art (tf4) UV: the second half pair of the half4 first texcoord,
    // as every decal VS reads it, else uv0. decal_uv_from_tc3 (above) is a
    // non-decal2 fallback only; decal2 meshes route the extra element to
    // their second layer instead (VSOut::uv4).
    if (decal_uv_from_tc3) {
      decode_uv(item.uv3_fmt, item.uv3_offset, dst[v * 14 + 12],
                dst[v * 14 + 13]);
    } else if (item.uv_fmt == 32) {
      decode_uv(31, item.uv_offset + 4, dst[v * 14 + 12], dst[v * 14 + 13]);
    } else {
      dst[v * 14 + 12] = dst[v * 14 + 3];
      dst[v * 14 + 13] = dst[v * 14 + 4];
    }
    // treeanimate_defaultVS fetches a full TEXCOORD1 and uses its zw as the
    // per-vertex wind seed (r3.zw in the captured ucode). These animated.tree
    // declarations use signed 16-bit four-component UV1 at item.uv2_offset;
    // preserve zw in the otherwise-unused uv3 slot for vs_main.
    // vertexanimate_defaultVS (animated.flag, fam 19) fetches the same
    // four-component TEXCOORD1 and seeds its wave from zw too (its r0.xw).
    // The VS reads it by element SHAPE (vf0: position, a 2-component
    // diffuse UV, the 4-component lightmap-UV + seed element), not by usage
    // index, so take the 4-component element from whichever texcoord slot
    // holds it; reading zw past a 2-component element fed neighbouring
    // bytes in as seeds and flung most of the cloth away.
    if (item.env_family == 10 || item.env_family == 19) {
      const auto four = [](uint32_t f) { return f == 26 || f == 32 || f == 38; };
      const auto seeds_from = [&](uint32_t fmt, uint32_t off) {
        const uint32_t pair = fmt == 26 ? 25u : fmt == 32 ? 31u : 37u;
        decode_uv(pair, off + (fmt == 38 ? 8u : 4u), dst[v * 14 + 12], dst[v * 14 + 13]);
      };
      if (four(item.uv2_fmt)) {
        seeds_from(item.uv2_fmt, item.uv2_offset);
      } else if (item.env_family == 19 && four(item.uv_fmt)) {
        // Usage indices swapped: TEXCOORD0 is the 4-component element (the
        // lightmap UV + seeds), the 2-component one is the diffuse UV.
        seeds_from(item.uv_fmt, item.uv_offset);
        decode_uv(item.uv_fmt, item.uv_offset, dst[v * 14 + 5], dst[v * 14 + 6]);
        decode_uv(item.uv2_fmt, item.uv2_offset, dst[v * 14 + 3], dst[v * 14 + 4]);
      } else if (item.env_family == 19) {
        // No seed element: neutral seeds (0.5, 0.5) = zero amplitude.
        dst[v * 14 + 12] = 0.5f;
        dst[v * 14 + 13] = 0.5f;
      }
      if (item.env_family == 19 && v + 1 == num_verts) {
        // DIAGNOSTIC (animated.flag): the element layout and seed range.
        static std::mutex s_flag_mutex;
        static std::unordered_set<uint32_t> s_flag_logged;
        std::lock_guard<std::mutex> lock(s_flag_mutex);
        if (s_flag_logged.size() < 16 && s_flag_logged.insert(item.mesh).second) {
          float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
          for (uint32_t k = 0; k < num_verts; ++k) {
            for (int c = 0; c < 2; ++c) {
              lo[c] = std::min(lo[c], dst[k * 14 + 12 + c]);
              hi[c] = std::max(hi[c], dst[k * 14 + 12 + c]);
            }
          }
          REXLOG_INFO("native-scene: flag mesh={:08X} stride={} pos_fmt={} uv=({},{}) uv2=({},{}) "
                      "uv3=({},{}) verts={} seed_u=[{:.3g},{:.3g}] seed_v=[{:.3g},{:.3g}]",
                      item.mesh, item.stride, item.pos_fmt, item.uv_fmt, item.uv_offset,
                      item.uv2_fmt, item.uv2_offset, item.uv3_fmt, item.uv3_offset, num_verts,
                      lo[0], hi[0], lo[1], hi[1]);
        }
      }
    }
    if (traffic) {
      decode_uv(lit_fmt, lit_off, dst[v * 14 + 3], dst[v * 14 + 4]);
      decode_uv(unlit_fmt, unlit_off, dst[v * 14 + 12], dst[v * 14 + 13]);
    }
    // Vertex normal: k_10_11_11 packed (x 11 bits, y 11 bits, z 10 bits,
    // LSB to MSB, signed). Zero when absent -> derivative face-normal
    // fallback in the shader.
    const auto unpack_10_11_11 = [&](uint32_t offset, float out[3]) {
      const uint32_t word = SwapU32(*reinterpret_cast<const uint32_t*>(
          src_vb + size_t(v) * item.stride + offset));
      const int32_t ix = int32_t(word << 21) >> 21;
      const int32_t iy = int32_t((word >> 11) << 21) >> 21;
      const int32_t iz = int32_t((word >> 22) << 22) >> 22;
      out[0] = float(ix) / 1023.0f;
      out[1] = float(iy) / 1023.0f;
      out[2] = float(iz) / 511.0f;
    };
    // Stored-tangent-frame pack (world-shading / dynobj v2): snorm
    // component to the unorm byte of the blend-weight word.
    const auto pk = [](float f) {
      return uint32_t(std::lround((f * 0.5f + 0.5f) * 255.0f)) & 0xFFu;
    };
    float n3[3] = {0.0f, 0.0f, 0.0f};
    if (item.water_flowing && !item.skinned && item.uv2_fmt == 26 &&
        item.tan_fmt == 16) {
      // Water meshes (flowingwater VS layout): the fmt-26 element is
      // lm_norm: xy = |unwrap| with sign bits, zw = normal.xy (snorm),
      // normal.z = sign.y * sqrt(1 - xy^2), and the usage-6 k_10_11_11
      // element is the TANGENT (no usage-3 normal, no usage-7 binormal).
      // Pack the tangent into the free blend-weight bytes with the unwrap
      // sign.x in the sentinel byte; the shader rebuilds the frame as
      // B = sign.x * cross(N, T), exactly the game's VS
      // (vBinormal = signs.x * cross(vNormal, vTangent)).
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv2_offset;
      const int16_t sx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
      const int16_t sy = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
      const int16_t nx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      const int16_t ny = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 6)));
      n3[0] = nx / 32767.0f;
      n3[1] = ny / 32767.0f;
      // The game clamps xy_len at 0.999 (water normals ride |xy| ~ 1, so
      // z floors at 0.0316 rather than 0, verified against the VS ucode).
      const float xylen =
          std::min(n3[0] * n3[0] + n3[1] * n3[1], 0.999f);
      n3[2] = (sy > 0 ? 1.0f : -1.0f) * std::sqrt(1.0f - xylen);
      float t3[3];
      unpack_10_11_11(item.tangent_offset, t3);
      const uint32_t packed = pk(t3[0]) | (pk(t3[1]) << 8) |
                              (pk(t3[2]) << 16) |
                              ((sx > 0 ? 200u : 100u) << 24);
      std::memcpy(&dst[v * 14 + 7], &packed, 4);
    } else if (item.env_family != 0 &&
        (item.env_family <= 6 || item.env_family == 13) &&
        item.uv2_fmt == 26) {
      // Exact world families: the REAL vertex normal is packed in the
      // lightmap-unwrap element (fmt 26 s16x4): zw = normal.xy (snorm), and
      // the unwrap xy SIGN bits carry the handedness; sign.y flips
      // normal.z (baseenvironment VS: vNormal.z = signs.y * sqrt(1 - xy^2);
      // signs = saturate(65535 * lm.xy) * 2 - 1). The k_10_11_11 element on
      // these meshes is the BINORMAL, not the normal; using it as the
      // normal breaks the sun/spec terms of the exact shading.
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv2_offset;
      const int16_t sx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
      const int16_t sy = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
      const int16_t nx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      const int16_t ny = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 6)));
      n3[0] = nx / 32767.0f;
      n3[1] = ny / 32767.0f;
      const float d = 1.0f - n3[0] * n3[0] - n3[1] * n3[1];
      n3[2] = (sy > 0 ? 1.0f : -1.0f) * std::sqrt(d > 0.0f ? d : 0.0f);
      // World-shading v2: the STORED tangent frame. These static meshes
      // never skin, so the blend-weight bytes are free: pack the mesh's
      // k_10_11_11 binormal (unorm-mapped) plus the unwrap sign.x tangent
      // handedness into them. The shader reconstructs T = cross(B, N) x
      // handedness; authoring decides per UV island whether the frame
      // follows a mirror, which derivative frames cannot know (the wooden
      // ramp panels banded light/dark per island without this).
      if (!item.skinned && item.normal_fmt == 16) {
        float b3[3];
        unpack_10_11_11(item.normal_offset, b3);
        // w byte is a three-state presence sentinel: 0 = no stored frame
        // (meshes that skip this pack leave the word zero), 100 = negative
        // handedness, 200 = positive.
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) |
                                ((sx > 0 ? 200u : 100u) << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      } else if (!item.skinned && item.tan_fmt == 16) {
        // Tangent-only layout (decalenvironment_tileable: pos, uv, lm+
        // normal s16x4, usage-6 k_10_11_11 and nothing else). Its VS passes
        // the stored T straight through and builds B = sign.x * cross(N, T)
        // (vs_decalenvironment_tileable_defaultVS lines 24-33). Pack T with
        // the tangent-stored sentinels 40/70 (KdAxes); with no stored frame
        // the kd sign terms fell back to the per-pixel screen frame and
        // dithered the slabs.
        float t3[3];
        unpack_10_11_11(item.tangent_offset, t3);
        const uint32_t packed = pk(t3[0]) | (pk(t3[1]) << 8) |
                                (pk(t3[2]) << 16) |
                                ((sx > 0 ? 70u : 40u) << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    } else if (item.normal_fmt == 16) {
      // (Vehicle meshes: the game's VS derives cross(tangent, binormal)
      // instead, but the stored usage-3 normal MATCHES it on every vertex
      // with a non-degenerate tangent frame and stays sane on the ~13%
      // interior verts where t is parallel to b and the cross vanishes,
      // measured in capture, so the stored normal is used.)
      unpack_10_11_11(item.normal_offset, n3);
      // vehicle_defaultPS builds its normal from the VS's rotated usage-6/7
      // pair: N = nx * T + ny * B + nz * cross(T, B). Car bodies never use
      // the decal-art uv3 slot, so the stored binormal rides it: x offset
      // by +16 (z >= 0) or +32 (z < 0) as the presence marker, y raw, z
      // rebuilt from the unit length (see scene.hlsl vs_main).
      if (item.char_family == 4 && item.tb_fmt == 16) {
        // cac_hair: the PS's Kajiya-Kay highlight runs along the VS-skinned
        // usage-6 TANGENT (cac_hair_defaultVS o2; the strand direction on
        // hair cards). Hair never samples uv3, so the tangent rides it with
        // the vehicle-binormal encoding (x +16 / +32 by z sign, y raw, z
        // rebuilt); the VS skins it with the normal's bone rows. The
        // screen-space U axis the port used instead ran ACROSS the strands:
        // sqrt(1 - (H.T)^2) pinned near 1 and the highlight lit every
        // strand, the hair ~1.3x (gamma) brighter than the emulated frame.
        float t3[3];
        unpack_10_11_11(item.tangent_offset, t3);
        const float tl = std::sqrt(t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2]);
        if (tl > 1e-4f) {
          dst[v * 14 + 12] = t3[0] / tl + (t3[2] < 0.0f ? 32.0f : 16.0f);
          dst[v * 14 + 13] = t3[1] / tl;
        }
      }
      if ((item.char_family == 6 || item.char_family == 7) && item.tb_fmt == 16) {
        float b3[3];
        unpack_10_11_11(item.binormal_offset, b3);
        const float bl = std::sqrt(b3[0] * b3[0] + b3[1] * b3[1] + b3[2] * b3[2]);
        if (bl > 1e-4f) {
          dst[v * 14 + 12] = b3[0] / bl + (b3[2] < 0.0f ? 32.0f : 16.0f);
          dst[v * 14 + 13] = b3[1] / bl;
        }
      }
      // Dynamicobject v2: rigid props store a full usage-6/7 tangent +
      // binormal pair alongside the usage-3 normal. Pack the binormal plus
      // the handedness relating cross(B, N) to the stored tangent into the
      // unused blend-weight bytes (same sentinel encoding as the env
      // families above); the authored frame carries the per-UV-island
      // mirror decisions the screen-space fallback cannot know.
      if (!item.skinned && item.dynobj != 0 && item.tb_fmt == 16) {
        float t3[3], b3[3];
        unpack_10_11_11(item.tangent_offset, t3);
        unpack_10_11_11(item.binormal_offset, b3);
        const float h = (b3[1] * n3[2] - b3[2] * n3[1]) * t3[0] +
                        (b3[2] * n3[0] - b3[0] * n3[2]) * t3[1] +
                        (b3[0] * n3[1] - b3[1] * n3[0]) * t3[2];
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) |
                                ((h >= 0.0f ? 200u : 100u) << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    } else if (item.tb_fmt == 16) {
      // NPC character meshes carry no normal element; the game's VS
      // derives it as cross(tangent, binormal) (usage 6 x usage 7; verified
      // against the emulated VS outputs). Without this the strong
      // character N.L shading exposes the face-normal fallback as visible
      // low-poly facets on every pedestrian.
      float t3[3], b3[3];
      unpack_10_11_11(item.tangent_offset, t3);
      unpack_10_11_11(item.binormal_offset, b3);
      n3[0] = t3[1] * b3[2] - t3[2] * b3[1];
      n3[1] = t3[2] * b3[0] - t3[0] * b3[2];
      n3[2] = t3[0] * b3[1] - t3[1] * b3[0];
      // livingworld_stamp_defaultPS maps its tangent-space normal onto the
      // VS's skinned usage-6/7 pair (x -> T, y -> B, z -> cross(T, B)).
      // Skinned layouts have no free blend bytes, so the stored tangent
      // rides the uv3 slot (x/y) and uv2.x (z), with 16 in uv2.y as the
      // presence marker; vs_main skins it with the normal's bone rows. The
      // stamp VS reads only TEXCOORD0, and the fam-3 branches never sample
      // uv2/uv3. Raw values interpolate like the game's o2 (no sign
      // encoding seams).
      if (item.skinned && item.char_family == 3) {
        const float tl = std::sqrt(t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2]);
        if (tl > 1e-4f) {
          dst[v * 14 + 12] = t3[0] / tl;
          dst[v * 14 + 13] = t3[1] / tl;
          dst[v * 14 + 5] = t3[2] / tl;
          dst[v * 14 + 6] = 16.0f;
        }
      }
      // Dynamicobject v2, tangent+binormal-only layout variant: the derived
      // normal makes cross(B, N) == T x |B|^2 up to orthogonality error, so
      // the handedness is positive by construction.
      if (!item.skinned && item.dynobj == 5) {
        // environmentpark.alphatest (tree canopies, grates): the game's
        // frame is the STORED pair, used raw -- kd signs from dot(T, L) /
        // dot(B, L) and wn = normalize(nt.x T + nt.y B + nt.z cross(T, B))
        // (environmentparkalphatest_defaultVS 16-28, _defaultPS 16-43).
        // These frames are far from orthogonal (|cross(T, B)| < 0.5 on 12%
        // of the canopy's vertices, capture 1790887078), so rebuilding T
        // as cross(B, N) turned it and flipped sign(T.L) leaf by leaf: the
        // per-leaf light/dark speckle. Pack T in the blend-weight bytes
        // (tangent-stored sentinel 70) and B RAW: x/y in the free uv3 slot,
        // z in the free uv2 slot with 16 in uv2.y as the presence marker
        // (these meshes carry one texcoord; ShadePark never reads uv2). The
        // vehicle encoding (x +16 / +32 by the sign of z) interpolates
        // through garbage wherever a triangle's corners disagree on that
        // sign -- frequent on leaf cards -- and its decode flipped B on one
        // side of a straight line across the card: half-card shading
        // breaks. Raw values interpolate exactly like the game's VS output.
        const uint32_t packed = pk(t3[0]) | (pk(t3[1]) << 8) |
                                (pk(t3[2]) << 16) | (70u << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
        dst[v * 14 + 12] = b3[0];
        dst[v * 14 + 13] = b3[1];
        dst[v * 14 + 5] = b3[2];
        dst[v * 14 + 6] = 16.0f;
      } else if (!item.skinned && item.dynobj != 0) {
        const uint32_t packed = pk(b3[0]) | (pk(b3[1]) << 8) |
                                (pk(b3[2]) << 16) | (200u << 24);
        std::memcpy(&dst[v * 14 + 7], &packed, 4);
      }
    }
    dst[v * 14 + 9] = n3[0];
    dst[v * 14 + 10] = n3[1];
    dst[v * 14 + 11] = n3[2];
  }
  // Dynobj v2 layout telemetry (first few meshes): which tangent-frame
  // source the props carry decides between the packed authored frame and
  // the shader's screen-space fallback.
  if (item.dynobj != 0) {
    static std::atomic<int> dynobj_layout_logged{0};
    if (dynobj_layout_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_INFO(
          "native-scene: dynobj mesh {:08X} layout normal_fmt={} tb_fmt={} "
          "skinned={}",
          item.mesh, item.normal_fmt, item.tb_fmt, item.skinned);
    }
  }
  // Stretch-veto probe (see g_skin_probe): cache ~32 decoded sample verts
  // so the guest thread can cheaply skin what the GPU will actually draw.
  if (item.skinned) {
    constexpr uint32_t kProbe = 32;
    const uint32_t pn = std::min(kProbe, num_verts);
    SkinProbe probe;
    probe.fp = item.fingerprint;
    probe.s.reserve(pn);
    for (uint32_t s = 0; s < pn; ++s) {
      const uint32_t v = pn > 1 ? (s * (num_verts - 1) / (pn - 1)) : 0;
      SkinProbeSample ps;
      ps.p[0] = dst[v * 14 + 0];
      ps.p[1] = dst[v * 14 + 1];
      ps.p[2] = dst[v * 14 + 2];
      std::memcpy(&ps.bw, &dst[v * 14 + 7], 4);
      std::memcpy(&ps.bi, &dst[v * 14 + 8], 4);
      probe.s.push_back(ps);
    }
    std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
    if (g_skin_probe.size() > 4096) {
      g_skin_probe.clear();
    }
    g_skin_probe[item.mesh] = std::move(probe);
  }
  // ROPA shape blending: retain the decoded vertex array (14 floats per
  // vertex, the scene VS layout) so consecutive generations can be lerped
  // onto the motion-smoothing play clock at draw time; the stepped shape
  // against the interpolated body was the tee jelly (worse at LOWER fps:
  // the excursion scales with the guest period; emulated pairs pose N with
  // shape N and shows none of it).
  if (item.ropa) {
    out.ropa_verts.assign(dst, dst + size_t(num_verts) * 14);
  }
  device->Unmap(vb);
  // Blend indices outside the captured palette read garbage rows and mangle
  // the vertex. Indices are plain bone numbers, bone k = palette rows 3k.
  // (Dyn decode jobs carry no palette: the item's bones ride the scene item,
  // not the decode; skip the check there.)
  if (item.skinned && max_bi >= 0 && !item.bones.empty()) {
    const uint32_t palette_bones = uint32_t(item.bones.size() / 12);
    if (uint32_t(max_bi) >= palette_bones) {
      REXLOG_WARN(
          "native-scene: skinned mesh {:08X} blend index range {}..{} exceeds "
          "captured palette of {} bones",
          item.mesh, min_bi, max_bi, palette_bones);
    }
  }
  if (garbage != 0) {
    REXLOG_WARN(
        "native-scene: mesh {:08X} decoded {} of {} verts outside bbox "
        "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) fmt {} stride {} vb {:08X}",
        item.mesh, garbage, num_verts, item.bbox_min[0], item.bbox_min[1], item.bbox_min[2],
        item.bbox_max[0], item.bbox_max[1], item.bbox_max[2], item.pos_fmt, item.stride,
        item.vb_addr);
  }

  uint16_t* dst_ib = nullptr;
  dst_ib = static_cast<uint16_t*>(device->Map(ib));
  if (dst_ib == nullptr) {
    device->DestroyDeferred(vb);
    device->DestroyDeferred(ib);
    return false;
  }
  if (item.cloth_quads) {
    // Quad-list topology with no guest index buffer (live vertex range
    // already exact from the draw args): two triangles per quad.
    const uint32_t quads = item.ib_count / 6;
    for (uint32_t q = 0; q < quads; ++q) {
      const uint16_t v = uint16_t(q * 4);
      uint16_t* o = dst_ib + q * 6;
      o[0] = v;
      o[1] = uint16_t(v + 1);
      o[2] = uint16_t(v + 2);
      o[3] = v;
      o[4] = uint16_t(v + 2);
      o[5] = uint16_t(v + 3);
    }
  } else {
    const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(ib_payload);
    for (uint32_t i = 0; i < item.ib_count; ++i) {
      dst_ib[i] = SwapU16(src_ib[i]);
    }
  }
  // Front/back sheet pattern: opposite-winding twin triangles a few mm-cm
  // apart ALONG THE NORMAL. The two sides are often triangulated along
  // OPPOSITE quad diagonals (downtown lamppost posters: sheets 4mm apart,
  // twin centroids 0.26m apart in-plane), so twins are matched by
  // plane-to-plane distance with an in-plane tolerance scaled to triangle
  // size; raw centroid distance misses them. >=60% twinned marks the mesh
  // double-sided (banners are 100%); O(T^2) but only for <=8192-index
  // static tri-list meshes and only once per decode.
  bool two_sided = false;
  if (detect_sheet) {
    struct SheetTri {
      float c[3];
      float n[3];
      float edge;  // longest edge length (in-plane tolerance scale)
    };
    std::vector<SheetTri> tris;
    tris.reserve(item.ib_count / 3);
    for (const DrawEntry& de : item.draws) {
      if (uint64_t(de.start_index) + de.index_count > item.ib_count) {
        continue;
      }
      for (uint32_t i = 0; i + 2 < de.index_count; i += 3) {
        const uint32_t a = uint32_t(dst_ib[de.start_index + i]) + de.base_vertex;
        const uint32_t b = uint32_t(dst_ib[de.start_index + i + 1]) + de.base_vertex;
        const uint32_t c = uint32_t(dst_ib[de.start_index + i + 2]) + de.base_vertex;
        if (a >= num_verts || b >= num_verts || c >= num_verts) {
          continue;
        }
        const float* pa = &sheet_pos[size_t(a) * 3];
        const float* pb = &sheet_pos[size_t(b) * 3];
        const float* pc = &sheet_pos[size_t(c) * 3];
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-8f) {
          continue;
        }
        SheetTri t;
        for (int k = 0; k < 3; ++k) {
          t.c[k] = (pa[k] + pb[k] + pc[k]) / 3.0f;
          t.n[k] = n[k] / len;
        }
        const float e3[3] = {pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2]};
        const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        const float l3 = e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2];
        t.edge = std::sqrt(std::max(l1, std::max(l2, l3)));
        tris.push_back(t);
      }
    }
    if (tris.size() >= 2) {
      std::vector<char> used(tris.size(), 0);
      size_t twins = 0;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (used[i]) {
          continue;
        }
        for (size_t j = i + 1; j < tris.size(); ++j) {
          if (used[j]) {
            continue;
          }
          const float dx = tris[i].c[0] - tris[j].c[0];
          const float dy = tris[i].c[1] - tris[j].c[1];
          const float dz = tris[i].c[2] - tris[j].c[2];
          const float d2 = dx * dx + dy * dy + dz * dz;
          // Plane separation along i's normal must be small (back-to-back
          // sheets); in-plane offset up to the triangle scale (opposite
          // diagonal splits put twin centroids half a quad apart).
          const float along = std::fabs(dx * tris[i].n[0] + dy * tris[i].n[1] +
                                        dz * tris[i].n[2]);
          const float lat_limit = 0.5f * (tris[i].edge + tris[j].edge);
          if (along > 0.05f || d2 - along * along > lat_limit * lat_limit) {
            continue;
          }
          const float dot = tris[i].n[0] * tris[j].n[0] + tris[i].n[1] * tris[j].n[1] +
                            tris[i].n[2] * tris[j].n[2];
          if (dot < -0.9f) {
            used[i] = 1;
            used[j] = 1;
            twins += 2;
            break;
          }
        }
      }
      // Fully twinned sheets (banners ~100%), OR a meaningful twin patch
      // inside a larger single-sided mesh (harbor sailboats: the twinned
      // SAIL rides a hull/mast mesh at 4-8% twins; the coincident copies
      // sit below far-field depth precision and z-fight into per-frame
      // lit/dark shimmer at range). The twins themselves are the evidence
      // the mesh was authored for culling-on; coincident opposite-normal
      // copies would z-fight on console too, so cull the whole mesh.
      // EXCEPT alpha-tested foliage (tree fams 9/10, envsimple.alphatest 7,
      // alphatest dynobj, transparent, and unclassified fam 0): a z-fight
      // between twinned leaf cards is invisible through the alpha test, so
      // twins do NOT prove culling-on there; culling stripped every
      // single-sided leaf/branch card seen from its back (the missing-
      // foliage regression). Those keep the strict fully-twinned rule.
      const bool partial_rule_ok =
          !item.transparent && item.dynobj != 2 && item.dynobj != 5 &&
          item.env_family != 0 &&
          item.env_family != 7 && item.env_family != 9 &&
          item.env_family != 10 && item.env_family != 13;
      two_sided = twins * 10 >= tris.size() * 6 ||
                  (partial_rule_ok && twins >= 12 && twins * 33 >= tris.size());
      if (two_sided) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
          REXLOG_INFO("native-scene: two-sided sheet mesh {:08X} ({} indices, {}/{} twins) -> cull-back",
                      item.mesh, item.ib_count, twins, tris.size());
        }
      }
    }
  }
  device->Unmap(ib);

  out.vb = vb;
  out.ib = ib;
  out.vb_view = {vb, 0, num_verts * 56u, 56u};
  out.ib_view = {ib, 0, item.ib_count * 2u};
  out.two_sided_sheet = two_sided;
  return true;
}

// Decode a guest texture from its 6 fetch-constant words (host-endian),
// the v1-verified path: CPU untile block by block through the 0xA0000000
// physical mirror, endian swap, and create its SRV view.
// The 3D path reads the words from renderengine::Texture objects; the 2D
// path passes the device fetch-shadow words directly.
// BC1/DXT1 block decode (both color modes) into 16 RGBA8 texels.
void DecodeBc1Block(const uint8_t* b, uint8_t px[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t col[4][4];
  // Endpoint expansion by BIT REPLICATION, what GPU hardware does. The
  // previous integer-division expansion (v*255/31) reads up to 1/255 dark;
  // on the reflective glass that error, folded through the constant detail
  // texture, tilted every reflection ~0.8 deg (found empirically via the
  // F12 trim sliders: the hand-matched values equaled the
  // replication expansion exactly).
  const auto expand = [](uint16_t c, uint8_t* o) {
    const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    o[0] = uint8_t((r5 << 3) | (r5 >> 2));
    o[1] = uint8_t((g6 << 2) | (g6 >> 4));
    o[2] = uint8_t((b5 << 3) | (b5 >> 2));
    o[3] = 255;
  };
  expand(c0, col[0]);
  expand(c1, col[1]);
  if (c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((2 * col[0][k] + col[1][k]) / 3);
      col[3][k] = uint8_t((col[0][k] + 2 * col[1][k]) / 3);
    }
    col[2][3] = 255;
    col[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((col[0][k] + col[1][k]) / 2);
      col[3][k] = 0;
    }
    col[2][3] = 255;
    col[3][3] = 0;
  }
  uint32_t bits =
      uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(px[i], col[bits & 3u], 4);
    bits >>= 2;
  }
}

// Runtime-composed textures (lightmap atlas pages above all) ship with NO
// mip chain; sampling their razor-contrast mip 0 under minification
// shimmers, grazing-angle banner posters, distant thin geometry, because
// the pixel footprint spans many texels the aniso sampler cannot average at
// mip 0 alone. The game masked this with its 720p softness and the 4-tap
// lightmap filter; at native 4K we need real prefiltering. Small no-mip
// DXT1/8888 textures are decoded to RGBA8 and uploaded with a CPU
// box-filtered mip chain instead. Returns false to fall back to the plain
// single-mip path.
bool UploadGeneratedMips(const NativeGuestOutputRenderContext& context, uint8_t* base,
                         const rex::graphics::TextureInfo& info, uint32_t fetch_swizzle,
                         GuestTexture& out) {
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const bool bc1 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_DXT1;

  // Guest mip 0 copy: same packed-base + tiled macro-row padding rules as
  // the plain path.
  uint32_t ox = 0, oy = 0;
  const uint32_t addr = info.GetMipLocation(0, &ox, &oy, true);
  if (addr == 0) {
    return false;
  }
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t min_size = info.memory.base_size;
  uint32_t size = min_size;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  if (info.is_tiled) {
    const uint32_t padded_rows = ((rows + oy) + 31u) & ~31u;
    size = std::max(size, padded_rows * pitch_blocks * bytes_per_block);
  }
  static thread_local std::vector<uint8_t> gen_scratch;
  gen_scratch.resize(size);
  if (!GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), size)) {
    if (min_size >= size ||
        !GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), min_size)) {
      return false;
    }
    size = min_size;
  }

  // Untile into linear block rows, endian-swap per row. Run-copy untiling:
  // same contiguity rule as the plain path (see EnsureGuestTextureFromWords):
  // aligned x-runs of clamp(16 >> bpb_log2, 1, 8) blocks are contiguous.
  std::vector<uint8_t> linear(size_t(cols) * rows * bytes_per_block);
  const uint32_t run_blocks = std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
  for (uint32_t by = 0; by < rows; ++by) {
    uint8_t* out_row = linear.data() + size_t(by) * cols * bytes_per_block;
    if (!info.is_tiled) {
      const uint32_t row_off = ((by + oy) * pitch_blocks + ox) * bytes_per_block;
      const uint32_t row_bytes = cols * bytes_per_block;
      if (row_off + row_bytes <= size) {
        std::memcpy(out_row, gen_scratch.data() + row_off, row_bytes);
      } else {
        for (uint32_t bx = 0; bx < cols; ++bx) {
          const uint32_t off = row_off + bx * bytes_per_block;
          if (off + bytes_per_block > size) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
            continue;
          }
          std::memcpy(out_row + size_t(bx) * bytes_per_block, gen_scratch.data() + off,
                      bytes_per_block);
        }
      }
    } else {
      uint32_t bx = 0;
      while (bx < cols) {
        const uint32_t x = bx + ox;
        const uint32_t run = std::min(cols - bx, run_blocks - (x & (run_blocks - 1)));
        const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(by + oy), pitch_blocks, bytes_per_block_log2));
        const uint32_t bytes = run * bytes_per_block;
        if (off + bytes <= size) {
          std::memcpy(out_row + size_t(bx) * bytes_per_block, gen_scratch.data() + off,
                      bytes);
        } else {
          for (uint32_t i = 0; i < run; ++i) {
            const uint32_t boff = off + i * bytes_per_block;
            if (boff + bytes_per_block > size) {
              std::memset(out_row + size_t(bx + i) * bytes_per_block, 0, bytes_per_block);
            } else {
              std::memcpy(out_row + size_t(bx + i) * bytes_per_block,
                          gen_scratch.data() + boff, bytes_per_block);
            }
          }
        }
        bx += run;
      }
    }
    SwapGuestEndian(out_row, cols * bytes_per_block, info.endianness);
  }

  // Decode to RGBA8 mip 0.
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  if (bc1) {
    for (uint32_t by = 0; by < rows; ++by) {
      for (uint32_t bx = 0; bx < cols; ++bx) {
        uint8_t px[16][4];
        DecodeBc1Block(linear.data() + (size_t(by) * cols + bx) * bytes_per_block, px);
        for (uint32_t t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t & 3u);
          const uint32_t y = by * 4 + (t >> 2);
          if (x < width && y < height) {
            std::memcpy(&rgba[(size_t(y) * width + x) * 4], px[t], 4);
          }
        }
      }
    }
  } else {  // k_8_8_8_8: rows are already RGBA8 after the endian swap
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(&rgba[size_t(y) * width * 4], linear.data() + size_t(y) * cols * 4,
                  size_t(width) * 4);
    }
  }

  // Diagnostic dump: what the RUNTIME decoded, for an offline byte diff
  // against gsnap_tex_decode of the same fetch words (the reflective_trans
  // glass panels' wrong-brightness hunt: their composed lightmap pages take
  // exactly this generated-mips path).
  if (REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/gen_%08X_%ux%u_t%u.rgba",
                  info.memory.base_address, width, height,
                  info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(rgba.data(), 1, rgba.size(), f);
      std::fclose(f);
    }
  }

  // Box-filtered chain down to 1x1.
  std::vector<std::vector<uint8_t>> mips;
  mips.emplace_back(std::move(rgba));
  uint32_t mw = width, mh = height;
  while (mw > 1 || mh > 1) {
    const uint32_t nw = std::max(mw >> 1, 1u);
    const uint32_t nh = std::max(mh >> 1, 1u);
    const std::vector<uint8_t>& srcm = mips.back();
    std::vector<uint8_t> dstm(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      const uint32_t y0 = std::min(y * 2, mh - 1);
      const uint32_t y1 = std::min(y * 2 + 1, mh - 1);
      for (uint32_t x = 0; x < nw; ++x) {
        const uint32_t x0 = std::min(x * 2, mw - 1);
        const uint32_t x1 = std::min(x * 2 + 1, mw - 1);
        for (int k = 0; k < 4; ++k) {
          const uint32_t sum = srcm[(size_t(y0) * mw + x0) * 4 + k] +
                               srcm[(size_t(y0) * mw + x1) * 4 + k] +
                               srcm[(size_t(y1) * mw + x0) * 4 + k] +
                               srcm[(size_t(y1) * mw + x1) * 4 + k];
          dstm[(size_t(y) * nw + x) * 4 + k] = uint8_t((sum + 2) / 4);
        }
      }
    }
    mips.emplace_back(std::move(dstm));
    mw = nw;
    mh = nh;
  }
  const uint32_t mip_count = uint32_t(mips.size());

  // Upload plan + resource (RGBA8).
  struct Plan {
    uint32_t offset, pitch, w, h;
  };
  std::vector<Plan> plans(mip_count);
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    Plan& p = plans[m];
    p.w = std::max(width >> m, 1u);
    p.h = std::max(height >> m, 1u);
    p.pitch = (p.w * 4u + (nrhi::kRowPitchAlignment - 1u)) &
              ~(nrhi::kRowPitchAlignment - 1u);
    p.offset = (upload_size + (kUploadPlacementAlignment - 1u)) &
               ~(kUploadPlacementAlignment - 1u);
    upload_size = p.offset + p.pitch * p.h;
  }
  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = width;
  desc.height = height;
  desc.mip_levels = mip_count;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  if (mapping == nullptr) {
    device->DestroyDeferred(out.upload);
    out.upload = {};
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  for (uint32_t m = 0; m < mip_count; ++m) {
    const Plan& p = plans[m];
    for (uint32_t y = 0; y < p.h; ++y) {
      std::memcpy(mapping + p.offset + size_t(y) * p.pitch, &mips[m][size_t(y) * p.w * 4],
                  size_t(p.w) * 4);
    }
  }
  device->Unmap(out.upload);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = nrhi::Format::kR8G8B8A8_UNORM;
    sc.srv_format = nrhi::Format::kR8G8B8A8_UNORM;
    ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, sc.swizzle);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, plans[m].w, plans[m].h};
    }
  } else {
    for (uint32_t m = 0; m < mip_count; ++m) {
      const Plan& p = plans[m];
      context.cmd->CopyBufferToTexture(out.texture, m, 0, out.upload, p.offset,
                                       p.pitch, p.w, p.h, 1);
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes); the in-place path recreates its own on demand.
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;

    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::k2D;
    srv.format = nrhi::Format::kR8G8B8A8_UNORM;
    ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, srv.swizzle);
    srv.mip_levels = mip_count;
    out.srv_format = srv.format;
    out.srv_mips = mip_count;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = size;
  BuildPayloadProbes(info, addr, ox, oy, pitch_blocks, size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// Phase attribution for the SLOW-inline-decode log: time inside RHI
// resource creation / the generated-mips path / the guest scratch copies,
// reset at each EnsureGuestTextureFromWords entry (per thread: workers and
// the render thread decode concurrently).
thread_local uint64_t g_tex_dec_create_ns = 0;
thread_local uint64_t g_tex_dec_gen_ns = 0;
thread_local uint64_t g_tex_dec_copy_ns = 0;

bool EnsureGuestTextureFromWords(const NativeGuestOutputRenderContext& context,
                                 uint8_t* base, const uint32_t words[6],
                                 GuestTexture& out) {
  g_tex_dec_create_ns = 0;
  g_tex_dec_gen_ns = 0;
  g_tex_dec_copy_ns = 0;
  std::memcpy(out.fetch_words, words, 6 * sizeof(uint32_t));

  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  // Pre-validate the raw format BEFORE Prepare: TextureInfo::Prepare calls
  // TextureExtent::Calculate, which divides by the format table's
  // block_width/block_height/bytes_per_block; a garbage fetch constant
  // (freed texture object read mid-teardown during a map change) carries a
  // format whose table entry has zeros and crashed a prewarm worker with an
  // integer divide-by-zero (dump skate3.exe.pre-icon.24004, second load
  // into Daly Estates).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
    return false;
  }

  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;

  // Upload the guest MIP CHAIN, not just mip 0; sampling mip 0 at distance
  // is the source of the grass "TV static" and flickering floor/window
  // lines. Power-of-two sizes only (everything the game ships) so BC block
  // alignment holds on every level.
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  uint32_t mip_count = 1;
  if (pow2 && info.memory.mip_address != 0 &&
      REXCVAR_GET(skate3_native_render_scene_tex_mips)) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_count < avail && (width >> mip_count) >= 4 && (height >> mip_count) >= 4) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_count, &ox, &oy, true) == 0) {
        break;
      }
      ++mip_count;
    }
  }
  // No guest chain at all (runtime-composed lightmap pages): generate one,
  // see UploadGeneratedMips. Small DXT1/8888 textures only; falls back to
  // the plain single-mip path on any failure.
  if (mip_count == 1 && pow2 && REXCVAR_GET(skate3_native_render_scene_tex_mips) &&
      width >= 8 && height >= 8 && width <= 512 && height <= 512) {
    const auto base_fmt = rex::graphics::GetBaseFormat(info.format);
    if (base_fmt == xenos::TextureFormat::k_DXT1 ||
        base_fmt == xenos::TextureFormat::k_8_8_8_8) {
      GuestTexture gen = out;  // keeps fetch_words already copied
      const auto gen_t0 = PerfClock::now();
      const bool gen_ok = UploadGeneratedMips(context, base, info, fetch.swizzle, gen);
      g_tex_dec_gen_ns += PerfNsSince(gen_t0);
      if (gen_ok) {
        out = gen;
        return true;
      }
    }
  }

  // Copy the whole guest mip chain out up front with the lock-free guarded
  // copy (never VirtualQuery on the render thread: the VAD lock stalls
  // behind the guest streaming threads exactly while panning streams
  // textures in), truncating the chain at the first unreadable level.
  struct MipSrc {
    uint32_t addr, scratch_off, size, min_size, pitch_blocks, ox, oy;
  };
  MipSrc srcs[16] = {};
  static thread_local std::vector<uint8_t> tex_scratch;
  uint32_t scratch_total = 0;
  const rex::graphics::texture_util::TextureGuestLayout guest_layout =
      rex::graphics::texture_util::GetGuestTextureLayout(
          xenos::DataDimension::k2DOrStacked, info.pitch >> 5, width, height, 1,
          info.is_tiled, info.format, info.has_packed_mips, true,
          std::max(mip_count, 1u) - 1u);
  for (uint32_t m = 0; m < mip_count; ++m) {
    uint32_t ox = 0, oy = 0;
    // Mip 0 through GetMipLocation too: textures <= 16 texels on a side
    // store their BASE level packed inside a 32x32 tile at a block offset.
    // Reading at (0,0) decoded garbage; the 16x16 "default_white" macro
    // overlay came out as pink/black blocks and multiplied giant soft black
    // blobs over every wall whose material uses it (validated: with the
    // packed offset it decodes pure white). Non-packed textures return the
    // plain base address with zero offsets.
    const uint32_t mip_addr = info.GetMipLocation(m, &ox, &oy, true);
    const auto ext = info.GetMipExtent(m, true);
    MipSrc& s = srcs[m];
    s.addr = mip_addr;
    s.pitch_blocks = m == 0 ? info.extent.block_pitch_h : ext.block_pitch_h;
    s.size = m == 0 ? info.memory.base_size : ext.all_blocks() * bytes_per_block;
    // Mip levels: address, row pitch and packed-tail offset from the
    // emulator's own loader layout (texture_util::GetGuestTextureLayout /
    // GetPackedMipOffset). TextureInfo::GetMipLocation's legacy walk differs
    // in two ways: it does not start each level on the 4 KB subresource
    // boundary (a 64x64 5_6_5 level 1 is 2 KB of data in a 4 KB slot, so
    // level 2 was read from inside level 1: the distant ad arrow showed a
    // zoomed-in crop), and its tail table is one slot off when the BASE is
    // already packed (shortest side <= 16).
    if (m > 0 && mip_addr != 0) {
      const uint32_t L = std::min(m, guest_layout.packed_level);
      s.addr = info.memory.mip_address + guest_layout.mip_offsets_bytes[L];
      s.pitch_blocks = guest_layout.mips[L].row_pitch_bytes / bytes_per_block;
      ox = 0;
      oy = 0;
      if (m >= guest_layout.packed_level) {
        uint32_t pz = 0;
        rex::graphics::texture_util::GetPackedMipOffset(width, height, 1, info.format, m, ox,
                                                        oy, pz);
      }
    }
    s.min_size = s.size;
    if (!info.is_tiled && m > 0 && mip_addr != 0) {
      // Linear level: exactly the bytes its rows read (last row ends at
      // ((rows - 1 + oy) * pitch + ox + cols) blocks). NOT the layout's
      // whole-tail extent: copying past what this level reads can run off
      // the allocation, and a fallback copy marks the texture incomplete,
      // which re-decodes it every frame.
      const uint32_t mw = std::max(width >> m, 1u);
      const uint32_t mh = std::max(height >> m, 1u);
      const uint32_t cols = (mw + block_w - 1) / block_w;
      const uint32_t rows = (mh + block_h - 1) / block_h;
      const uint32_t need =
          ((rows - 1 + oy) * s.pitch_blocks + ox + cols) * bytes_per_block;
      s.size = need;
      s.min_size = need;
    }
    if (info.is_tiled) {
      // Tiled addressing swizzles across 32x32-BLOCK macro tiles AND, for
      // narrow block formats, interleaves across 64x64/128x128-block
      // portions whose byte extent EXCEEDS the linear size; 16bpp reaches
      // 0xC00 bytes from a 32x32 tile origin vs the naive 32*32*2 = 0x800.
      // The previous padded-macro-ROW estimate (added for the 32x32 DXT5
      // HUD compass icons, whose last block sat at 5952 vs base_size 1024)
      // missed that interleave: every 16bpp mip's swizzled offsets for the
      // BOTTOM half of its rows landed past the estimate, the range guard
      // zeroed them, and every PCU Library banner rendered with its lower
      // half black at mip-1 viewing distance (MIP DIAG: guard_zeroed
      // exactly total/2 on 64x64 fmt-4 mips, total/1 on packed oy=16 mips).
      // Size the copy with the SDK's swizzle-aware upper bound instead; if
      // that over-reaches the committed allocation the copy loop below
      // falls back to the reported size and marks the decode incomplete.
      const uint32_t mw = std::max(width >> m, 1u);
      const uint32_t mh = std::max(height >> m, 1u);
      const uint32_t right = (mw + block_w - 1) / block_w + ox;
      const uint32_t bottom = (mh + block_h - 1) / block_h + oy;
      s.size = std::max(
          s.size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                      right, bottom, s.pitch_blocks, bytes_per_block_log2));
    }
    s.ox = ox;
    s.oy = oy;
    s.scratch_off = scratch_total;
    scratch_total += s.size;
  }
  const auto copy_t0 = PerfClock::now();
  tex_scratch.resize(scratch_total);
  uint32_t mips_copied = 0;
  bool copy_truncated = false;
  for (uint32_t m = 0; m < mip_count; ++m) {
    MipSrc& s = srcs[m];
    if (!GuestTryCopy(tex_scratch.data() + s.scratch_off,
                      base + (0xA0000000u | s.addr), s.size)) {
      if (s.min_size >= s.size ||
          !GuestTryCopy(tex_scratch.data() + s.scratch_off,
                        base + (0xA0000000u | s.addr), s.min_size)) {
        break;
      }
      // Tiled fallback: the padded macro rows hold real blocks past
      // min_size; every one of them uploads as ZERO below (the PCU
      // Library half-black banner mips). The decode is marked incomplete
      // so the draw path keeps retrying until the pool commits.
      s.size = s.min_size;
      copy_truncated = true;
    }
    ++mips_copied;
  }
  g_tex_dec_copy_ns += PerfNsSince(copy_t0);
  if (mips_copied == 0) {
    return false;
  }
  mip_count = mips_copied;
  out.incomplete = copy_truncated;
  if (copy_truncated) {
    static std::atomic<uint32_t> s_trunc_logs{0};
    if (s_trunc_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO(
          "native-scene: texture decode INCOMPLETE (tiled mip copy fell back "
          "to min_size: zeroed tail blocks) {}x{} mips={} w1={:08X}; will "
          "re-decode until complete",
          width, height, mip_count, words[1]);
    }
  }

  // Per-mip upload footprints (D3D12 alignment rules).
  struct MipPlan {
    uint32_t offset, pitch, cols, rows;
  };
  MipPlan plans[16] = {};
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    MipPlan& p = plans[m];
    p.cols = (mw + block_w - 1) / block_w;
    p.rows = (mh + block_h - 1) / block_h;
    p.pitch = (p.cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
              ~(nrhi::kRowPitchAlignment - 1u);
    p.offset = (upload_size + (kUploadPlacementAlignment - 1u)) &
               ~(kUploadPlacementAlignment - 1u);
    upload_size = p.offset + p.pitch * p.rows;
  }

  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::k2D;
  desc.width = host_width;
  desc.height = host_height;
  desc.mip_levels = mip_count;
  desc.format = host.resource_format;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  const auto create_t0 = PerfClock::now();
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
  g_tex_dec_create_ns += PerfNsSince(create_t0);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  if (mapping == nullptr) {
    device->DestroyDeferred(out.upload);
    out.upload = {};
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const MipPlan& p = plans[m];
    const uint32_t ox = srcs[m].ox;
    const uint32_t oy = srcs[m].oy;
    const uint32_t src_pitch_blocks = srcs[m].pitch_blocks;
    const uint32_t src_size = srcs[m].size;
    const uint8_t* guest = tex_scratch.data() + srcs[m].scratch_off;
    const uint32_t row_bytes = p.cols * bytes_per_block;
    uint32_t guard_zeroed = 0;  // blocks zeroed by the range guard (diag)
    // Run-copy untiling. The per-BLOCK GetTiledOffset2D loop made a single
    // 1280x720 8888 decode cost 3-13 ms (921k address computations + 4-byte
    // memcpys); animating fullscreen menu art and FMV planes pay that on
    // EVERY content change, which is where the menu frame spikes lived.
    // From the tiling formula (util.cpp GetTiledOffset2D): within a row, an
    // x-run aligned to run_blocks = clamp(16 >> bpb_log2, 1, 8) maps to
    // CONTIGUOUS byte offsets; the micro term stays inside its 16-byte
    // nibble group ((y & 0xE) contributes only multiples of 16 at every
    // bpb), and runs <= 8 aligned blocks never cross the (x>>3)/(x>>5) mix
    // boundaries. Linear mips copy whole rows.
    //
    // ROWS STAGE THROUGH A CACHED BUFFER, never the upload mapping directly:
    // upload heaps are WRITE-COMBINED memory, and SwapGuestEndian/565-swap
    // READ the row back; uncached WC reads cost ~33 ns/byte, which was the
    // long-unattributed 35-135 ms decode class (512x512 = 1 MB = 34 ms,
    // 1024x1024+chain = 135 ms; linear fit across a slow-decode log).
    // The mapping gets exactly one streaming memcpy per row.
    const uint32_t run_blocks =
        std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
    static thread_local std::vector<uint8_t> row_buf;
    row_buf.resize(row_bytes);
    for (uint32_t by = 0; by < p.rows; ++by) {
      uint8_t* out_row = row_buf.data();
      if (!info.is_tiled) {
        const uint32_t row_off =
            ((by + oy) * src_pitch_blocks + ox) * bytes_per_block;
        if (row_off + row_bytes <= src_size) {
          std::memcpy(out_row, guest + row_off, row_bytes);
        } else {
          for (uint32_t bx = 0; bx < p.cols; ++bx) {
            const uint32_t off = row_off + bx * bytes_per_block;
            if (off + bytes_per_block > src_size) {
              std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
              ++guard_zeroed;
              continue;
            }
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off,
                        bytes_per_block);
          }
        }
      } else {
        uint32_t bx = 0;
        while (bx < p.cols) {
          const uint32_t x = bx + ox;
          const uint32_t run =
              std::min(p.cols - bx, run_blocks - (x & (run_blocks - 1)));
          const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(x), int32_t(by + oy), src_pitch_blocks, bytes_per_block_log2));
          const uint32_t bytes = run * bytes_per_block;
          if (off + bytes <= src_size) {
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off, bytes);
          } else {
            // Padded macro rows past the copied size: per-block guard-zero
            // (offsets within the run are contiguous, same proof as above).
            for (uint32_t i = 0; i < run; ++i) {
              const uint32_t boff = off + i * bytes_per_block;
              if (boff + bytes_per_block > src_size) {
                std::memset(out_row + size_t(bx + i) * bytes_per_block, 0,
                            bytes_per_block);
                ++guard_zeroed;
              } else {
                std::memcpy(out_row + size_t(bx + i) * bytes_per_block, guest + boff,
                            bytes_per_block);
              }
            }
          }
          bx += run;
        }
      }
      SwapGuestEndian(out_row, row_bytes, info.endianness);
      if (swap_rb_565) {
        for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
          uint16_t value;
          std::memcpy(&value, out_row + i, sizeof(value));
          value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                           ((value & 0x1Fu) << 11));
          std::memcpy(out_row + i, &value, sizeof(value));
        }
      }
      std::memcpy(mapping + p.offset + size_t(by) * p.pitch, out_row, row_bytes);
    }
    // Half-black-mip diagnostic (PCU Library banners): discriminate "the
    // guest pool genuinely holds zeros for this mip" from "our addressing
    // zeroed/misread it". Samples 32 uploaded blocks spread over the mip;
    // guard_zeroed separates range-guard zeroing from zero CONTENT.
    if (m > 0) {
      uint32_t zero_samples = 0;
      const uint32_t total_blocks = p.rows * p.cols;
      for (uint32_t s = 0; s < 32; ++s) {
        const uint32_t bi = uint32_t(uint64_t(total_blocks - 1) * s / 31u);
        const uint32_t by = bi / p.cols;
        const uint32_t bx = bi % p.cols;
        uint64_t q = 0;
        std::memcpy(&q, mapping + p.offset + size_t(by) * p.pitch +
                            size_t(bx) * bytes_per_block,
                    std::min<uint32_t>(8, bytes_per_block));
        zero_samples += q == 0 ? 1 : 0;
      }
      if (total_blocks >= 32 && (guard_zeroed * 4 >= total_blocks ||
                                 zero_samples >= 12)) {
        static std::atomic<uint32_t> s_mip_diag{0};
        if (s_mip_diag.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: MIP DIAG {}x{} mip {}/{} zero_samples={}/32 "
              "guard_zeroed={}/{} ox={} oy={} pitch_b={} size={} min={} "
              "tiled={} fmt={} w0={:08X} w1={:08X} w2={:08X} mip_addr={:08X}",
              width, height, m, mip_count, zero_samples, guard_zeroed,
              total_blocks, ox, oy, src_pitch_blocks, src_size,
              srcs[m].min_size, info.is_tiled ? 1 : 0, uint32_t(info.format),
              words[0], words[1], words[2], srcs[m].addr);
        }
      }
    }
  }
  // Diagnostic dump (skate3_native_render_scene_lm_dump): no-chain textures
  // through the PLAIN path (the >512px composed lightmap pages the
  // generated-mips gate excludes): mip 0 as linear block rows, raw guest
  // block format post-endian-swap, for offline decode + byte-diff against
  // gsnap_tex_decode of the same fetch words.
  if (mip_count == 1 && REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/plain_%08X_%ux%u_f%u_t%u.blk",
                  info.memory.base_address, width, height,
                  uint32_t(info.format), info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      const MipPlan& p0 = plans[0];
      for (uint32_t by = 0; by < p0.rows; ++by) {
        std::fwrite(mapping + p0.offset + size_t(by) * p0.pitch, 1,
                    size_t(p0.cols) * bytes_per_block, f);
      }
      std::fclose(f);
    }
  }
  device->Unmap(out.upload);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, sc.swizzle);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, std::max(host_width >> m, 1u),
                    std::max(host_height >> m, 1u)};
    }
  } else {
    // Record the upload copies into the deferred command list.
    for (uint32_t m = 0; m < mip_count; ++m) {
      const MipPlan& p = plans[m];
      context.cmd->CopyBufferToTexture(out.texture, m, 0, out.upload, p.offset,
                                       p.pitch, std::max(host_width >> m, 1u),
                                       std::max(host_height >> m, 1u), 1);
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes); the in-place path recreates its own on demand.
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;

    // SRV view with the composed swizzle.
    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::k2D;
    srv.format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, srv.swizzle);
    srv.mip_levels = mip_count;
    out.srv_format = srv.format;
    out.srv_mips = mip_count;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
  }
  // Payload sample for content revalidation (see GuestTexture).
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = srcs[0].size;
  BuildPayloadProbes(info, srcs[0].addr, srcs[0].ox, srcs[0].oy,
                     srcs[0].pitch_blocks, srcs[0].size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// (The object-keyed EnsureGuestTexture wrapper is gone: everything decodes
// from an explicit stable words snapshot, ReadStableTexWords +
// EnsureGuestTextureFromWords, and lands in the words-keyed store.)

// In-place re-decode for HOT single-mip 2D content (video planes, animating
// fullscreen menu/loading art): identical fetch words = identical layout, so
// the committed texture, upload footprint and SRV slot are all reusable;
// the retire+recreate path paid two GPU resource creations per
// content change (~90/s during FMV playback, plus every animation step of
// fullscreen frontend art). Non-pow2 single-mip entries only: pow2 entries
// may own a real or generated mip chain (the full path handles those).
// The TEAR-GUARD also lives here: the CPU writer (VP6 video decode, APT
// re-raster) can be mid-write when the liveness probe fires; decoding a
// half-written fullscreen payload shows a frame of mixed old/new content:
// a subtle whole-screen brightness flicker on fades and video. The guest
// copy retries until the probe fingerprint reads stable across it.
bool UpdateGuestTexture2DInPlace(const NativeGuestOutputRenderContext& context,
                                 uint8_t* base, GuestTexture& t) {
  if (!t.valid || t.texture == nullptr || t.srv_mips != 1 || t.incomplete) {
    return false;
  }
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = t.fetch_words[0];
  fetch.dword_1 = t.fetch_words[1];
  fetch.dword_2 = t.fetch_words[2];
  fetch.dword_3 = t.fetch_words[3];
  fetch.dword_4 = t.fetch_words[4];
  fetch.dword_5 = t.fetch_words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
    return false;
  }
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  if ((width & (width - 1)) == 0 && (height & (height - 1)) == 0) {
    return false;  // pow2: may own a mip chain, full path
  }
  if (host.srv_format != t.srv_format) {
    return false;  // the existing SRV recipe must stay exact
  }
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;

  // Mip-0 source extent, same rules as the full decode.
  uint32_t ox = 0, oy = 0;
  const uint32_t addr = info.GetMipLocation(0, &ox, &oy, true);
  if (addr == 0) {
    return false;
  }
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  const uint32_t min_size = info.memory.base_size;
  uint32_t size = min_size;
  if (info.is_tiled) {
    size = std::max(size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                              cols + ox, rows + oy, pitch_blocks, bytes_per_block_log2));
  }
  static thread_local std::vector<uint8_t> inplace_scratch;
  inplace_scratch.resize(size);

  // Tear-guarded guest copy: retry while the writer races us.
  uint64_t fp = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint64_t fp_before = SampleProbeFingerprint(base, t);
    if (!GuestTryCopy(inplace_scratch.data(), base + (0xA0000000u | addr), size)) {
      if (min_size >= size ||
          !GuestTryCopy(inplace_scratch.data(), base + (0xA0000000u | addr), min_size)) {
        return false;  // payload unreadable: let the full path sort it out
      }
      size = min_size;
    }
    fp = SampleProbeFingerprint(base, t);
    if (fp != 0 && fp == fp_before) {
      break;
    }
  }

  // Upload footprint (single mip at offset 0).
  const uint32_t pitch =
      (cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
      ~(nrhi::kRowPitchAlignment - 1u);
  const uint32_t upload_size = pitch * rows;
  nrhi::Device* device = context.device;
  nrhi::Buffer*& up = t.upload_flip ? t.upload_b : t.upload;
  if (up != nullptr && up->size() < upload_size) {
    // Never expected (same words = same footprint): recreate defensively.
    device->DestroyDeferred(up);
    up = nullptr;
  }
  if (up == nullptr) {
    up = CreateUploadBuffer(device, upload_size, nrhi::BufferBindClass::kCopySrc);
    if (up == nullptr) {
      return false;
    }
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(up));
  if (mapping == nullptr) {
    return false;
  }
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  const uint8_t* guest = inplace_scratch.data();
  const uint32_t row_bytes = cols * bytes_per_block;
  const uint32_t run_blocks = std::clamp(16u >> bytes_per_block_log2, 1u, 8u);
  // Rows stage through a CACHED buffer; the endian/565 swaps read the row
  // back, and reading the write-combined upload mapping costs ~33 ns/byte
  // (the 35-135 ms decode class; see the plain path).
  static thread_local std::vector<uint8_t> row_buf;
  row_buf.resize(row_bytes);
  for (uint32_t by = 0; by < rows; ++by) {
    uint8_t* out_row = row_buf.data();
    if (!info.is_tiled) {
      const uint32_t row_off = ((by + oy) * pitch_blocks + ox) * bytes_per_block;
      if (row_off + row_bytes <= size) {
        std::memcpy(out_row, guest + row_off, row_bytes);
      } else {
        for (uint32_t bx = 0; bx < cols; ++bx) {
          const uint32_t off = row_off + bx * bytes_per_block;
          if (off + bytes_per_block > size) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
          } else {
            std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off,
                        bytes_per_block);
          }
        }
      }
    } else {
      uint32_t bx = 0;
      while (bx < cols) {
        const uint32_t x = bx + ox;
        const uint32_t run = std::min(cols - bx, run_blocks - (x & (run_blocks - 1)));
        const uint32_t off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(by + oy), pitch_blocks, bytes_per_block_log2));
        const uint32_t bytes = run * bytes_per_block;
        if (off + bytes <= size) {
          std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + off, bytes);
        } else {
          for (uint32_t i = 0; i < run; ++i) {
            const uint32_t boff = off + i * bytes_per_block;
            if (boff + bytes_per_block > size) {
              std::memset(out_row + size_t(bx + i) * bytes_per_block, 0, bytes_per_block);
            } else {
              std::memcpy(out_row + size_t(bx + i) * bytes_per_block, guest + boff,
                          bytes_per_block);
            }
          }
        }
        bx += run;
      }
    }
    SwapGuestEndian(out_row, row_bytes, info.endianness);
    if (swap_rb_565) {
      for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
        uint16_t value;
        std::memcpy(&value, out_row + i, sizeof(value));
        value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                         ((value & 0x1Fu) << 11));
        std::memcpy(out_row + i, &value, sizeof(value));
      }
    }
    std::memcpy(mapping + size_t(by) * pitch, out_row, row_bytes);
  }
  device->Unmap(up);

  // Barriers must land in the deferred list BEFORE the copy: flush both
  // sides explicitly (earlier draws this frame sample the OLD content,
  // later ones the new: same semantics as a retire+recreate mid-frame).
  context.cmd->Barrier(t.texture, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopyDest);
  context.cmd->FlushBarriers();
  context.cmd->CopyBufferToTexture(t.texture, 0, 0, up, 0, pitch, cols * block_w,
                                   rows * block_h, 1);
  context.cmd->Barrier(t.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  context.cmd->FlushBarriers();

  t.upload_flip = !t.upload_flip;
  t.payload_fp = fp;
  t.near_black = SampleProbeNearBlack(base, t);
  t.recheck_frame = 0;
  return true;
}

// Environment CUBE map for the water / reflective-glass reflection term
// (t6). Six faces untiled independently per level (Xenos cubes are 2D-tiled
// per face slice), WITH the guest mip chain; gradient-derived LOD on the
// normal-mapped reflection vector is what blurs baked cube detail
// (streetlight heads) into the soft reflections of the real console output.
bool EnsureGuestCubeTexture(const NativeGuestOutputRenderContext& context, uint8_t* base,
                            uint32_t tex_ptr, GuestTexture& out) {
  uint32_t words[6] = {};
  {
    uint32_t raw[6];
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw))) {
      return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
      words[i] = SwapU32(raw[i]);
    }
  }
  std::memcpy(out.fetch_words, words, sizeof(words));
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  // Same divide-by-zero pre-guard as EnsureGuestTextureFromWords (garbage
  // format -> zero block dims inside Prepare's extent math).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  if (info.dimension != xenos::DataDimension::kCube || info.memory.base_address == 0 ||
      info.width >= 1024 || info.height >= 1024) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;
  // A FULL mip chain is load-bearing for the reflective families. The real
  // baseenvironmentreflective PS perturbs its reflection vector with a
  // per-pixel normal map, so adjacent pixels reflect degrees apart; the
  // hardware cube fetch runs at a VERY deep gradient-derived LOD and the
  // whole reflection resolves to a near-face-average wash (the emulated
  // frame's uniform blue glass; offline: a CUBE_LOD=8 box-downsample probe
  // reproduces the emulated facade's uniformity where mip 0 shows a sharp
  // baked tree/streetlight blob). A truncated chain
  // clamps the LOD shallow and leaves that blob visible. DXT1 cubes (every
  // env cube observed) decode mip 0 to RGBA8 and generate the complete
  // chain down to 1x1 below; other formats fall back to copying the guest
  // chain (down to 32px: smaller levels live packed inside a shared 32x32
  // tile, see GetPackedTileOffset).
  const xenos::TextureFormat cube_base_fmt = rex::graphics::GetBaseFormat(info.format);
  const bool cube_swap_rb_565 =
      cube_base_fmt == xenos::TextureFormat::k_5_6_5;
  // 5_6_5 cubes take the CPU-chain path too. Their guest mip chain was
  // copied only down to 32 px, and those small levels are where the black
  // texels lived: Industrial's ocean samples one (128x128) at a deep
  // gradient LOD under the camera, where rippled normals change reflection
  // direction per pixel, and got clusters of black pixels that shrank as
  // the camera closed in (sharper LOD). Mip 0 of the same cube is clean.
  const bool rgba_chain =
      (cube_base_fmt == xenos::TextureFormat::k_DXT1 ||
       cube_base_fmt == xenos::TextureFormat::k_8_8_8_8 ||
       cube_base_fmt == xenos::TextureFormat::k_5_6_5) &&
      width >= 8 && (width & (width - 1)) == 0 && width == height;
  uint32_t mip_levels = 1;
  if (!rgba_chain && info.memory.mip_address != 0 && (width & (width - 1)) == 0 &&
      (height & (height - 1)) == 0) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_levels < avail && (width >> mip_levels) >= 32 &&
           (height >> mip_levels) >= 32) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_levels, &ox, &oy, true) == 0 || ox != 0 || oy != 0) {
        break;
      }
      ++mip_levels;
    }
  }
  // Per-level guest layout: each level stores the six face slices
  // consecutively (extent depth = 6; GetMipLocation walks whole levels).
  struct CubeLevel {
    uint32_t addr, pitch_blocks, slice_bytes, cols, rows, scratch_off;
    uint32_t up_pitch, up_face_bytes, w, h;
  };
  CubeLevel lv[6] = {};
  uint32_t scratch_total = 0;
  for (uint32_t m = 0; m < mip_levels; ++m) {
    CubeLevel& L = lv[m];
    const auto ext = m == 0 ? info.extent : info.GetMipExtent(m, true);
    uint32_t ox = 0, oy = 0;
    L.addr = m == 0 ? info.memory.base_address : info.GetMipLocation(m, &ox, &oy, true);
    L.pitch_blocks = ext.block_pitch_h;
    L.slice_bytes = ext.block_pitch_h * ext.block_pitch_v * bytes_per_block;
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    L.cols = (mw + block_w - 1) / block_w;
    L.rows = (mh + block_h - 1) / block_h;
    L.w = L.cols * block_w;
    L.h = L.rows * block_h;
    L.up_pitch = (L.cols * bytes_per_block + (nrhi::kRowPitchAlignment - 1u)) &
                 ~(nrhi::kRowPitchAlignment - 1u);
    L.up_face_bytes =
        (L.up_pitch * L.rows + (kUploadPlacementAlignment - 1u)) &
        ~(kUploadPlacementAlignment - 1u);
    L.scratch_off = scratch_total;
    scratch_total += L.slice_bytes * 6;
  }
  if (scratch_total == 0 || scratch_total > 24u * 1024u * 1024u) {
    return false;
  }
  static thread_local std::vector<uint8_t> cube_scratch;
  cube_scratch.resize(scratch_total);
  for (uint32_t m = 0; m < mip_levels; ++m) {
    if (!GuestTryCopy(cube_scratch.data() + lv[m].scratch_off,
                      base + (0xA0000000u | lv[m].addr), lv[m].slice_bytes * 6)) {
      if (m == 0) {
        return false;
      }
      mip_levels = m;  // truncate the chain at the first unreadable level
      break;
    }
  }

  if (rgba_chain) {
    // Decode mip 0 -> RGBA8 per face (DXT1 block decode, or plain 8888
    // texels: the water canal cubes), box-filter the full chain to 1x1,
    // upload as an RGBA cube. The GUEST cube mip chain is never trusted:
    // its packed sub-32px levels and per-face slice layout are exactly
    // where garbage sneaks in, and the reflection fetch runs at a deep
    // gradient LOD where a bad level dominates. CPU cost is one-time per
    // cube (runs on the decode workers).
    nrhi::Device* device = context.device;
    const uint32_t levels = 1u + uint32_t(std::countr_zero(width));
    struct Level {
      uint32_t w, pitch, face_bytes, upload_off;  // upload_off within a face
    };
    Level lvs[16] = {};
    uint32_t face_upload = 0;
    for (uint32_t m = 0; m < levels; ++m) {
      Level& L = lvs[m];
      L.w = std::max(width >> m, 1u);
      L.pitch = (L.w * 4u + (nrhi::kRowPitchAlignment - 1u)) &
                ~(nrhi::kRowPitchAlignment - 1u);
      L.face_bytes = (L.pitch * L.w + (kUploadPlacementAlignment - 1u)) &
                     ~(kUploadPlacementAlignment - 1u);
      L.upload_off = face_upload;
      face_upload += L.face_bytes;
    }
    nrhi::TextureDesc desc;
    desc.kind = nrhi::TextureKind::kCube;
    desc.width = width;
    desc.height = height;
    desc.mip_levels = levels;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    out.texture = device->CreateTexture(desc);
    if (out.texture == nullptr) {
      return false;
    }
    out.upload = CreateUploadBuffer(device, face_upload * 6, nrhi::BufferBindClass::kCopySrc);
    if (!out.upload) {
      device->DestroyDeferred(out.texture);
      out.texture = nullptr;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
    if (mapping == nullptr) {
      device->DestroyDeferred(out.upload);
      out.upload = {};
      device->DestroyDeferred(out.texture);
      out.texture = nullptr;
      return false;
    }
    static thread_local std::vector<uint8_t> rgba;   // level 0 of one face
    static thread_local std::vector<uint8_t> down;   // downsample scratch
    static thread_local std::vector<uint8_t> bc_row; // one untiled block row
    const CubeLevel& L0 = lv[0];
    bc_row.resize(size_t(L0.cols) * bytes_per_block);
    for (uint32_t face = 0; face < 6; ++face) {
      // Per-face: the downsample loop below SWAPS rgba/down, so their sizes
      // end the chain tiny; the next face's full-size decode writes must
      // not index a shrunken buffer.
      rgba.resize(size_t(width) * height * 4);
      down.resize(size_t(width / 2) * (height / 2) * 4);
      const uint8_t* guest =
          cube_scratch.data() + L0.scratch_off + size_t(face) * L0.slice_bytes;
      for (uint32_t by = 0; by < L0.rows; ++by) {
        for (uint32_t bx = 0; bx < L0.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L0.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L0.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L0.slice_bytes) {
            std::memset(&bc_row[size_t(bx) * bytes_per_block], 0, bytes_per_block);
            continue;
          }
          std::memcpy(&bc_row[size_t(bx) * bytes_per_block], guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(bc_row.data(), uint32_t(bc_row.size()), info.endianness);
        if (cube_base_fmt == xenos::TextureFormat::k_DXT1) {
          for (uint32_t bx = 0; bx < L0.cols; ++bx) {
            uint8_t px[16][4];
            DecodeBc1Block(&bc_row[size_t(bx) * bytes_per_block], px);
            for (uint32_t r = 0; r < 4; ++r) {
              std::memcpy(&rgba[(size_t(by * 4 + r) * width + bx * 4) * 4],
                          px[r * 4], 16);
            }
          }
        } else if (cube_base_fmt == xenos::TextureFormat::k_5_6_5) {
          // Same components the B5G6R5 upload delivered after its CPU
          // red/blue transpose (see swap_rb_565): X = bits 0-4, Y = bits
          // 5-10, Z = bits 11-15, expanded by bit replication; W = 1 (the
          // host RGBB swizzle and the RGBA identity differ only in alpha,
          // which the fetch swizzle drives).
          for (uint32_t bx = 0; bx < width; ++bx) {
            uint16_t v;
            std::memcpy(&v, &bc_row[size_t(bx) * 2], sizeof(v));
            const uint32_t x5 = v & 31u, y6 = (v >> 5) & 63u, z5 = (v >> 11) & 31u;
            uint8_t* o = &rgba[(size_t(by) * width + bx) * 4];
            o[0] = uint8_t((x5 << 3) | (x5 >> 2));
            o[1] = uint8_t((y6 << 2) | (y6 >> 4));
            o[2] = uint8_t((z5 << 3) | (z5 >> 2));
            o[3] = 255;
          }
        } else {
          // k_8_8_8_8: post-endian-swap bytes are already component order
          // X,Y,Z,W (the SRV swizzle applies the fetch's channel remap).
          std::memcpy(&rgba[size_t(by) * width * 4], bc_row.data(),
                      size_t(width) * 4);
        }
      }
      // Diagnostic dump (skate3_native_render_scene_lm_dump): the decoded
      // face exactly as uploaded, for offline byte-diff against the
      // reference decode of the same fetch words.
      if (REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
        char path[260];
        std::snprintf(path, sizeof(path),
                      "native_texture_dumps/cube_%08X_f%u_%ux%u.rgba",
                      info.memory.base_address, face, width, height);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(rgba.data(), 1, size_t(width) * height * 4, f);
          std::fclose(f);
        }
      }
      // Upload level 0, then box-filter down the chain in place.
      const uint8_t* src = rgba.data();
      uint32_t w = width;
      for (uint32_t m = 0; m < levels; ++m) {
        uint8_t* up = mapping + size_t(face) * face_upload + lvs[m].upload_off;
        for (uint32_t y = 0; y < w; ++y) {
          std::memcpy(up + size_t(y) * lvs[m].pitch, src + size_t(y) * w * 4,
                      size_t(w) * 4);
        }
        if (m + 1 >= levels) {
          break;
        }
        const uint32_t hw = w / 2;
        for (uint32_t y = 0; y < hw; ++y) {
          for (uint32_t x = 0; x < hw; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
              const uint32_t s =
                  uint32_t(src[((y * 2) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2) * w + x * 2 + 1) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2 + 1) * 4 + c]);
              down[(size_t(y) * hw + x) * 4 + c] = uint8_t((s + 2) / 4);
            }
          }
        }
        rgba.swap(down);
        src = rgba.data();
        w = hw;
      }
    }
    device->Unmap(out.upload);
    REXLOG_DEBUG("native-scene: cube {:08X} {}x{} fmt={} -> RGBA full chain ({} levels)",
                 tex_ptr, width, height, uint32_t(info.format), levels);
    nrhi::Swizzle swizzle_mapping[4];
    ComposeSrvSwizzle(fetch.swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
                      swizzle_mapping);
    if (g_tex_stage_out != nullptr) {
      StagedTexCommit& sc = *g_tex_stage_out;
      sc.copy_format = nrhi::Format::kR8G8B8A8_UNORM;
      sc.srv_format = nrhi::Format::kR8G8B8A8_UNORM;
      for (uint32_t c = 0; c < 4; ++c) {
        sc.swizzle[c] = swizzle_mapping[c];
      }
      sc.cube = true;
      sc.cube_mip_levels = levels;
      sc.mip_count = std::min<uint32_t>(6u * levels,
                                        StagedTexCommit::kMaxStagedMips);
      for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t m = 0; m < levels; ++m) {
          sc.mips[face * levels + m] = {face * face_upload + lvs[m].upload_off,
                                        lvs[m].pitch, lvs[m].w, lvs[m].w};
        }
      }
      out.payload_addr = 0xA0000000u | info.memory.base_address;
      out.payload_size = lv[0].slice_bytes * 6;
      out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
      out.recheck_frame = 0;
      out.valid = false;  // live only after commit
      return true;
    }
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < levels; ++m) {
        context.cmd->CopyBufferToTexture(
            out.texture, m, face, out.upload,
            size_t(face) * face_upload + lvs[m].upload_off, lvs[m].pitch,
            lvs[m].w, lvs[m].w, 1);
      }
    }
    context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    // Copies recorded: release the staging buffer (deferred until the
    // submission completes).
    device->DestroyDeferred(out.upload);
    out.upload = nullptr;
    nrhi::TextureViewDesc srv;
    srv.dimension = nrhi::ViewDimension::kCube;
    srv.format = nrhi::Format::kR8G8B8A8_UNORM;
    for (uint32_t c = 0; c < 4; ++c) {
      srv.swizzle[c] = swizzle_mapping[c];
    }
    srv.mip_levels = levels;
    out.srv = device->CreateTextureView(out.texture, srv);
    if (out.srv == nullptr) {
      return false;
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = true;
    return true;
  }

  uint32_t face_upload = 0;  // one face's full mip chain in the upload buffer
  for (uint32_t m = 0; m < mip_levels; ++m) {
    face_upload += lv[m].up_face_bytes;
  }
  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.kind = nrhi::TextureKind::kCube;
  desc.width = host_width;
  desc.height = host_height;
  desc.mip_levels = mip_levels;
  desc.format = host.resource_format;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, face_upload * 6, nrhi::BufferBindClass::kCopySrc);
  if (!out.upload) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = static_cast<uint8_t*>(device->Map(out.upload));
  if (mapping == nullptr) {
    device->DestroyDeferred(out.upload);
    out.upload = {};
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return false;
  }
  const auto upload_offset = [&](uint32_t face, uint32_t m) {
    uint32_t off = face * face_upload;
    for (uint32_t k = 0; k < m; ++k) {
      off += lv[k].up_face_bytes;
    }
    return off;
  };
  // (The "Xenos cube T runs bottom-up" flip that briefly lived here was
  // WRONG; it matched two probe pixels by coincidence and turned every
  // facade pavement-white in game. The stored face orientation is correct
  // as-is; the emulated look comes from LOD depth, not orientation.)
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      const CubeLevel& L = lv[m];
      const uint8_t* guest =
          cube_scratch.data() + L.scratch_off + size_t(face) * L.slice_bytes;
      uint8_t* up = mapping + upload_offset(face, m);
      const uint32_t row_bytes = L.cols * bytes_per_block;
      for (uint32_t by = 0; by < L.rows; ++by) {
        uint8_t* out_row = up + size_t(by) * L.up_pitch;
        for (uint32_t bx = 0; bx < L.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L.slice_bytes) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
            continue;
          }
          std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(out_row, row_bytes, info.endianness);
        if (cube_swap_rb_565) {
          // Same transpose the 2D paths apply (see swap_rb_565), and the
          // contract the host-format table states outright for k_5_6_5
          // ("Red/blue swapped CPU-side while uploading", mapped to
          // kB5G6R5_UNORM). Missing here, every 5_6_5 environment cube
          // reflected with red and blue transposed -- Industrial's ocean
          // samples one, and a blue sky (0.24, 0.38, 0.60) came back as
          // (0.60, 0.38, 0.24), i.e. brown. The DXT1 and 8888 cubes that the
          // rgba_chain path handles were unaffected, which is why this
          // survived so long.
          for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
            uint16_t value;
            std::memcpy(&value, out_row + i, sizeof(value));
            value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                             ((value & 0x1Fu) << 11));
            std::memcpy(out_row + i, &value, sizeof(value));
          }
        }
      }
    }
  }
  device->Unmap(out.upload);

  REXLOG_DEBUG("native-scene: cube {:08X} {}x{} fmt={} mips={} (of {} avail)", tex_ptr,
               width, height, uint32_t(info.format), mip_levels, info.mip_levels());
  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe, face-major (face * levels +
    // mip) entries matching the old D3D12 subresource numbering.
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, sc.swizzle);
    sc.cube = true;
    sc.cube_mip_levels = mip_levels;
    sc.mip_count = std::min<uint32_t>(6u * mip_levels,
                                      StagedTexCommit::kMaxStagedMips);
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < mip_levels; ++m) {
        sc.mips[face * mip_levels + m] = {upload_offset(face, m), lv[m].up_pitch,
                                          lv[m].w, lv[m].h};
      }
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = false;  // live only after commit
    return true;
  }

  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      context.cmd->CopyBufferToTexture(out.texture, m, face, out.upload,
                                       upload_offset(face, m), lv[m].up_pitch,
                                       lv[m].w, lv[m].h, 1);
    }
  }
  context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  // Copies recorded: release the staging buffer (deferred until the
  // submission completes).
  device->DestroyDeferred(out.upload);
  out.upload = nullptr;
  nrhi::TextureViewDesc srv;
  srv.dimension = nrhi::ViewDimension::kCube;
  srv.format = host.srv_format;
  ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle, srv.swizzle);
  srv.mip_levels = mip_levels;
  out.srv = device->CreateTextureView(out.texture, srv);
  if (out.srv == nullptr) {
    return false;
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = lv[0].slice_bytes * 6;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
  return true;
}

// Scene vertex layout (DecodeMesh's 56-byte output): shared by the scene
// PSO family and the shadow-caster PSO. `location` = the element's
// declaration order in the scene VS input struct (the Vulkan input
// location; see nrhi::InputElementDesc).

}  // namespace

// ---- EnsurePipeline helpers ------------------------------------------------
// Split from the former ~1,000-line EnsurePipeline: one resource
// group per function, bodies unchanged. Every helper is idempotent (guarded
// by its own g_r state) and returns false only on a failure that must abort
// the native path (g_r.failed set where the original did).

// Looks up the offline-compiled SPIR-V for (file, entry, variant); variant is
// the canonical macro signature ("", "SAMPLES=4", "PFX_MSAA=1"). Returns a
// fully-populated ShaderDesc; SPIR-V absent only if the table and the compile
// matrix ever drift (Vulkan pipeline creation then fails loudly, D3D12 is
// unaffected).
nrhi::ShaderDesc MakeShaderDesc(nrhi::ShaderStage stage, const char* file,
                                const char* hlsl_source, const char* entry,
                                const nrhi::ShaderMacro* macros,
                                const char* variant) {
  nrhi::ShaderDesc sd;
  sd.stage = stage;
  sd.name = file;
  sd.hlsl_source = hlsl_source;
  sd.entry_point = entry;
  sd.macros = macros;
  for (size_t i = 0; i < skate3::native_spirv::kNativeSpirvBlobCount; ++i) {
    const auto& b = skate3::native_spirv::kNativeSpirvBlobs[i];
    if (std::strcmp(b.file, file) == 0 && std::strcmp(b.entry, entry) == 0 &&
        std::strcmp(b.variant, variant) == 0) {
      sd.spirv = b.data;
      sd.spirv_size_bytes = b.size_bytes;
      break;
    }
  }
  return sd;
}

bool EnsureRootSignature(const NativeGuestOutputRenderContext& context) {
  if (g_r.layout) {
    return true;
  }
  {
    nrhi::BindingLayoutDesc ld;
    // NOTE the 64-DWORD root-signature budget (a D3D12-backend constraint:
    // the layout maps 1:1 onto a D3D12 root signature there): 52 constants +
    // 6 descriptor tables (1 each) + 1 root SRV (2) + 2 root CBVs (2 each)
    // = 64, FULL. Going past 64 makes the D3D12 layout creation fail
    // (renderer falls back to emulated). Any further addition must pack into
    // existing rows/tables.
    ld.param_count = 10;
    ld.params[0] = {nrhi::BindingParamKind::kConstants, /*b*/ 0, 52,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 1,
                    nrhi::Visibility::kPixel};
    ld.params[2] = {nrhi::BindingParamKind::kTextureTable, 1, 1,
                    nrhi::Visibility::kPixel};
    ld.params[3] = {nrhi::BindingParamKind::kBufferSrv, /*t*/ 2, 1,
                    nrhi::Visibility::kVertex};
    // Macro overlay (t3).
    ld.params[4] = {nrhi::BindingParamKind::kTextureTable, 3, 1,
                    nrhi::Visibility::kPixel};
    // Decal art / spec masks (t4). Second entry of the table = the fam 5/6
    // normal map (t5), bound via cmd->SetTexturePair. Draws without a pair
    // leave t5 at the backend fallback; the shader only samples t5 when
    // overlay.w == 4 (pair bound).
    ld.params[5] = {nrhi::BindingParamKind::kTextureTable, 4, 2,
                    nrhi::Visibility::kPixel};
    // Dynamic-shadow additions: per-frame receiver constants (b1).
    // Vertex-visible too: the one-pass VR stereo matrix (st_d) rides here.
    ld.params[6] = {nrhi::BindingParamKind::kConstantBuffer, /*b*/ 1, 1,
                    nrhi::Visibility::kAll};
    // Environment cube (t6) + blurred shadow atlas (t7) as ONE two-entry
    // table (bound together via SetTexturePair); merging them freed the
    // root-signature DWORD the v2 material table below needs.
    ld.params[7] = {nrhi::BindingParamKind::kTextureTable, 6, 2,
                    nrhi::Visibility::kPixel};
    // World-shading v2 material maps: the detail normal map (t8) + the
    // decal families' spec/ecc masks (t9), plus the native static
    // sun-shadow map (t10) as the table's third entry; extending an
    // existing table costs no root-signature DWORDs. Bound together via
    // SetTextures (a pair-only bind would drop t10 to the backend
    // fallback).
    // ... and the deck-shadow decal (t11) as the fourth.
    // ... and decal2environment's second decal art layer (t12) as the fifth.
    ld.params[8] = {nrhi::BindingParamKind::kTextureTable, 8, 5,
                    nrhi::Visibility::kPixel};
    // Character lighting block (b2): the canonical per-draw rows captured
    // from the guest PS bank (CaptureCharLighting), sliced out of the bone
    // upload ring per character draw.
    ld.params[9] = {nrhi::BindingParamKind::kConstantBuffer, 2, 1,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 2;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kAnisotropic,
                             nrhi::AddressMode::kWrap, 8};
    // s1: bilinear CLAMP for the 2D overlay. The HUD fetch constants carry
    // clamp_x/clamp_y = 2 (clamp-to-edge), and the clock face is built from
    // MIRRORED quadrant tiles whose outer-edge UVs run past 1.0; wrap
    // sampling pulls the opposite edge of the art in as 1px seam lines
    // (bright rim row across the middle, dark center column at the edges).
    ld.static_samplers[1] = {1, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.allow_input_layout = true;
    g_r.layout = context.device->CreateBindingLayout(ld);
    if (g_r.layout == nullptr) {
      REXLOG_ERROR("native-scene: root signature creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// The main scene PSO and its culling/blend/depth variants (cull-back
// sheets, transparent, entity fade, hair 2-pass, no-depth, outline mask),
// built at the g_r.msaa sample count EnsurePipeline selected.
bool EnsureScenePsoFamily(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  // Scene color format: the float HDR intermediate when the HDR post chain
  // is active (the tonemap pass then writes the gamma guest output),
  // otherwise the guest output itself.
  const nrhi::Format scene_fmt = g_r.hdr_active
                                     ? g_r.hdr_scene_format
                                     : context.guest_output->format();
  // g_r.msaa is selected by EnsurePipeline (which triggers this rebuild on
  // change).

  // The family rebuilds on HDR/MSAA toggles / output-format changes: retire the
  // previous pipelines (in-flight frames keep them alive via the deferred
  // destruction queue).
  for (nrhi::Pipeline** p :
       {&g_r.pso, &g_r.pso_cullback, &g_r.pso_cullfront, &g_r.pso_fg_inner, &g_r.pso_fg_outer,
        &g_r.pso_wireframe, &g_r.pso_wire_depth, &g_r.pso_xray_a,
        &g_r.pso_xray_b,
        &g_r.pso_transparent, &g_r.pso_fade, &g_r.pso_fade_cull[0],
        &g_r.pso_fade_cull[1],
        &g_r.pso_hair_a, &g_r.pso_hair_b, &g_r.pso_nodepth,
        &g_r.pso_outline_mask, &g_r.pso_stencil_mark}) {
    if (*p != nullptr) {
      device->DestroyDeferred(*p);
      *p = nullptr;
    }
  }

  // HDR=1 switches the pixel shader's output encode to pre-tonemap linear
  // (see scene.hlsl ToneOut/PassGamma); SHOWCASE=1 compiles the build-up
  // split/mask gates in (swapped in only while a run is live). The variant
  // string is the canonical ";"-joined macro signature the offline SPIR-V
  // matrix uses.
  nrhi::ShaderMacro ps_defs[4];
  uint32_t ps_def_count = 0;
  if (g_r.hdr_active) {
    ps_defs[ps_def_count++] = {"HDR", "1"};
  }
  if (g_r.showcase_shaders) {
    ps_defs[ps_def_count++] = {"SHOWCASE", "1"};
  }
  // Multi-digit safe: probe modes run past 9, and a 2-char buffer turned
  // 10 into ':' (a non-numeric macro value), which failed the runtime
  // compile and dropped the renderer to the emulated path.
  char probe_val[8] = {'0', 0};
  char probe_variant[24] = {0};
  if (g_r.decal_probe != 0) {
    std::snprintf(probe_val, sizeof(probe_val), "%d", g_r.decal_probe);
    std::snprintf(probe_variant, sizeof(probe_variant), "DECAL_PROBE=%d",
                  g_r.decal_probe);
    ps_defs[ps_def_count++] = {"DECAL_PROBE", probe_val};
  }
  ps_defs[ps_def_count] = {nullptr, nullptr};
  char ps_variant[64];
  std::snprintf(ps_variant, sizeof(ps_variant), "%s%s%s%s%s",
                g_r.hdr_active ? "HDR=1" : "",
                g_r.hdr_active && g_r.showcase_shaders ? ";" : "",
                g_r.showcase_shaders ? "SHOWCASE=1" : "",
                (g_r.hdr_active || g_r.showcase_shaders) && g_r.decal_probe ? ";" : "",
                probe_variant);
  nrhi::Shader* vs = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource,
                     "vs_main", nullptr, ""));
  nrhi::Shader* ps = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                     "ps_main", ps_def_count != 0 ? ps_defs : nullptr,
                     ps_variant));
  if (vs == nullptr || ps == nullptr) {
    REXLOG_ERROR("native-scene: shader compile failed");
    g_r.failed = true;
    return false;
  }
  nrhi::GraphicsPipelineDesc pso;
  pso.layout = g_r.layout;
  pso.vs = vs;
  pso.ps = ps;
  pso.input_elements = kSceneInputLayout;
  pso.input_element_count = 7;
  pso.vertex_stride = 56;
  pso.cull = nrhi::CullMode::kNone;
  pso.depth_clip = true;
  pso.depth.test_enable = true;
  pso.depth.write_enable = true;
  pso.depth.func = nrhi::CompareFunc::kLess;
  pso.rtv_format = scene_fmt;
  pso.dsv_format = nrhi::Format::kD32_FLOAT;
  pso.sample_count = g_r.msaa;
  g_r.pso = device->CreateGraphicsPipeline(pso);
  // Culling variant for double-sided sheet props (banners/flags,
  // MeshBuffers::two_sided_sheet). The sheet whose winding-derived world
  // normal faces the camera is the one the game keeps (its lightmap cell
  // reproduces the emulated banner exactly: albedo x lm x 2 lands within
  // 3-8% of the F11 emulated reference on capture 1783387480); under our
  // pipeline those triangles are D3D12 BACK faces, so cull FRONT.
  // (CULL_BACK was tried first and kept the wrong sheet; banners rendered
  // the sun-side lightmap cell ~2.4x brighter than emulated.)
  pso.cull = nrhi::CullMode::kFront;
  g_r.pso_cullback = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kBack;
  g_r.pso_cullfront = device->CreateGraphicsPipeline(pso);
  // Wireframe (see RendererState::pso_wireframe): the guest's global line
  // raster mode. Uncull(ed) so both facings' edges draw, matching the
  // game's dual-mode line output.
  pso.cull = nrhi::CullMode::kNone;
  pso.fill = nrhi::FillMode::kWireframe;
  g_r.pso_wireframe = device->CreateGraphicsPipeline(pso);
  // The matching filled depth-only prepass (see pso_wire_depth): no colour,
  // depth writes on, solid fill.
  pso.fill = nrhi::FillMode::kSolid;
  pso.blend.write_mask = 0;
  g_r.pso_wire_depth = device->CreateGraphicsPipeline(pso);
  pso.blend.write_mask = 0xF;
  pso.cull = nrhi::CullMode::kFront;
  // The editor's preview object (DrawItem::foreground), the game's two
  // passes (capture 1790890720, draws 1063-1066): first PA_SU_SC_MODE_CNTL
  // 0x18006 with RB_DEPTHCONTROL zfunc ALWAYS + z-write (the inner faces,
  // drawn over whatever the object is sunk into), then the usual 0x18005
  // with LEQUAL (the outer faces). 0x18005 is our CULL_FRONT (see the
  // banner calibration above), so 0x18006 is our CULL_BACK.
  pso.cull = nrhi::CullMode::kBack;
  pso.depth.func = nrhi::CompareFunc::kAlways;
  g_r.pso_fg_inner = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kFront;
  pso.depth.func = nrhi::CompareFunc::kLessEqual;
  g_r.pso_fg_outer = device->CreateGraphicsPipeline(pso);
  // The editor's selected piece (DrawItem::xray; capture 1790895957 draws
  // 1483-1486): both passes uncull(ed) (0x18004); first RB_DEPTHCONTROL
  // 0x708773 (zfunc ALWAYS, NO z-write: the x-ray), then 0x708737 (LEQUAL,
  // z-write).
  //
  // The ALWAYS pass writes ALPHA ONLY (RB_COLOR_MASK 0x8, capture
  // 1790955633 draws 1301/1302; the depth-tested pass that follows is the
  // full 0xF). It exists to mark the piece for postfx_edgedetectstencil,
  // which draws the cyan selection outline THROUGH occluders -- it is not
  // meant to paint the piece itself. Writing RGB here rendered the
  // selection as an opaque slab floating over the lamp posts and ramps in
  // front of it, instead of a correctly occluded object with a show-through
  // outline.
  pso.cull = nrhi::CullMode::kNone;
  pso.depth.func = nrhi::CompareFunc::kAlways;
  pso.depth.write_enable = false;
  pso.blend.write_mask = 0x8;
  g_r.pso_xray_a = device->CreateGraphicsPipeline(pso);
  pso.blend.write_mask = 0xF;
  pso.depth.func = nrhi::CompareFunc::kLessEqual;
  pso.depth.write_enable = true;
  g_r.pso_xray_b = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kFront;
  pso.depth.func = nrhi::CompareFunc::kLess;
  // CAS skin subsurface-scattering passes (see RendererState::pso_skin_*):
  // the game's unwrap draws are uncull(ed) (PA_SU_SC_MODE_CNTL 0x18004), no
  // depth, into a k_8_8_8_8 target; the blur is a fullscreen quad.
  for (nrhi::Pipeline** pp : {&g_r.pso_skin_unwrap, &g_r.pso_skin_blur}) {
    if (*pp != nullptr) {
      device->DestroyDeferred(*pp);
      *pp = nullptr;
    }
  }
  {
    nrhi::Shader* uvs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource, "vs_skin_unwrap", nullptr, ""));
    nrhi::Shader* ups = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource, "ps_skin_unwrap", nullptr, ""));
    nrhi::Shader* bvs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource, "vs_skin_blur", nullptr, ""));
    nrhi::Shader* bps = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource, "ps_skin_blur", nullptr, ""));
    if (uvs != nullptr && ups != nullptr && bvs != nullptr && bps != nullptr) {
      nrhi::GraphicsPipelineDesc sp = pso;
      sp.vs = uvs;
      sp.ps = ups;
      sp.cull = nrhi::CullMode::kNone;
      sp.blend = {};
      sp.depth.test_enable = false;
      sp.depth.write_enable = false;
      sp.dsv_format = nrhi::Format::kUnknown;
      sp.rtv_format = nrhi::Format::kR8G8B8A8_UNORM;
      sp.sample_count = 1;
      g_r.pso_skin_unwrap = device->CreateGraphicsPipeline(sp);
      sp.vs = bvs;
      sp.ps = bps;
      sp.input_elements = nullptr;
      sp.input_element_count = 0;
      g_r.pso_skin_blur = device->CreateGraphicsPipeline(sp);
    }
    for (nrhi::Shader* sh : {uvs, ups, bvs, bps}) {
      if (sh != nullptr) device->DestroyDeferred(sh);
    }
    if (g_r.pso_skin_unwrap == nullptr || g_r.pso_skin_blur == nullptr) {
      REXLOG_WARN("native-scene: skin subsurface pipelines unavailable");
    }
  }
  if (g_r.pso_cullback == nullptr) {
    REXLOG_WARN("native-scene: cull-back PSO creation failed");
    // sheets fall back to the uncull(ed) PSO
  }
  pso.cull = nrhi::CullMode::kNone;
  // Transparent variant: straight alpha blend, depth-tested, no z-write.
  pso.depth.write_enable = false;
  pso.blend.enable = true;
  pso.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.op = nrhi::BlendOp::kAdd;
  pso.blend.src_alpha = nrhi::BlendFactor::kOne;
  pso.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.op_alpha = nrhi::BlendOp::kAdd;
  g_r.pso_transparent = device->CreateGraphicsPipeline(pso);
  // Additive variant for draws the game blends ONE/ONE (see GameBlendMode).
  if (g_r.pso_transparent_add != nullptr) {
    device->DestroyDeferred(g_r.pso_transparent_add);
    g_r.pso_transparent_add = nullptr;
  }
  pso.blend.src = nrhi::BlendFactor::kOne;
  pso.blend.dst = nrhi::BlendFactor::kOne;
  pso.blend.src_alpha = nrhi::BlendFactor::kZero;
  pso.blend.dst_alpha = nrhi::BlendFactor::kOne;
  g_r.pso_transparent_add = device->CreateGraphicsPipeline(pso);
  // Particle blend variants (each lionREC_Render system keeps the game's
  // own RB_BLENDCONTROL): additive by alpha (SRC_ALPHA / ONE) and
  // premultiplied (ONE / INV_SRC_ALPHA). ONE / ONE is pso_transparent_add.
  for (nrhi::Pipeline*& p : g_r.pso_particle) {
    if (p != nullptr) {
      device->DestroyDeferred(p);
      p = nullptr;
    }
  }
  pso.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.dst = nrhi::BlendFactor::kOne;
  pso.blend.src_alpha = nrhi::BlendFactor::kZero;
  pso.blend.dst_alpha = nrhi::BlendFactor::kOne;
  g_r.pso_particle[0] = device->CreateGraphicsPipeline(pso);
  pso.blend.src = nrhi::BlendFactor::kOne;
  pso.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.src_alpha = nrhi::BlendFactor::kOne;
  pso.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  g_r.pso_particle[1] = device->CreateGraphicsPipeline(pso);
  pso.blend.src = nrhi::BlendFactor::kSrcAlpha;
  pso.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
  pso.blend.src_alpha = nrhi::BlendFactor::kOne;
  pso.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
  // Entity-fade variant: z-write ON (see RendererState::pso_fade). Drawn
  // at the head of the blended sub-pass so glass/hair still composite
  // over the faded body.
  pso.depth.write_enable = true;
  g_r.pso_fade = device->CreateGraphicsPipeline(pso);
  // Culled variants of the z-write blend for environment.transparent draws
  // that carry a game cull state (see GameCullMode).
  pso.cull = nrhi::CullMode::kFront;
  g_r.pso_fade_cull[0] = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kBack;
  g_r.pso_fade_cull[1] = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kNone;
  // (nullptr = fade items fall back to the z-write-off blend)
  pso.depth.write_enable = false;
  // Hair passes: the game draws hair twice with the SAME shader; cull
  // BACK then cull FRONT (cac_hair.xml passes 0/1) so far-side strands
  // never composite over near-side ones (one uncull(ed) blended pass
  // interleaves them per triangle order = crunchy noise). Same blend /
  // z-write-off state as the transparent variant.
  pso.cull = nrhi::CullMode::kBack;
  g_r.pso_hair_a = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kFront;
  g_r.pso_hair_b = device->CreateGraphicsPipeline(pso);
  pso.cull = nrhi::CullMode::kNone;
  pso.blend = {};
  pso.depth.write_enable = true;
  pso.depth.test_enable = false;
  pso.dsv_format = nrhi::Format::kUnknown;
  g_r.pso_nodepth = device->CreateGraphicsPipeline(pso);
  // Selection-outline mask: the same scene VS/PS (the tint.a > 0 solid
  // path renders flat 1.0) into the small single-sample R8 target. No
  // depth: the mask is the full silhouette. The guest marking pass is
  // depth-tested, so a partially occluded selection outlines slightly
  // differently, acceptable for the editor UI.
  // The pixel shader is ps_stencil_mark (tint.r raw): through ps_main's
  // solid path the mask took PassGamma(1) = 1/1.41 = 0.709 on the SDR
  // variant, and the edge composite's band 6.4 m (1 - m) then saturated
  // over the WHOLE selection (a white, blooming piece instead of a contour).
  nrhi::Shader* mask_ps = device->CreateShader(
      MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                     "ps_stencil_mark", nullptr, ""));
  pso.rtv_format = nrhi::Format::kR8_UNORM;
  pso.sample_count = 1;
  if (mask_ps != nullptr) {
    pso.ps = mask_ps;
    g_r.pso_outline_mask = device->CreateGraphicsPipeline(pso);
    pso.ps = ps;
  }
  if (g_r.pso_outline_mask == nullptr) {
    REXLOG_WARN("native-scene: outline mask PSO creation failed");
    // outline pass disables itself
  }
  // Game stencil mark (RenderStencilMark): the SAME vs_main object as the
  // main pass, so a re-drawn item reproduces its scene depth bit for bit
  // and a LESS_EQUAL test (no write) against the scene depth stands in for
  // the game's stencil REPLACE-on-z-pass. Uncull(ed): the marked draws'
  // coverage includes the hair back/front passes.
  {
    nrhi::Shader* mps = mask_ps;
    if (mps != nullptr) {
      nrhi::GraphicsPipelineDesc mp = pso;
      mp.ps = mps;
      mp.blend = {};
      mp.cull = nrhi::CullMode::kNone;
      mp.depth.test_enable = true;
      mp.depth.write_enable = false;
      mp.depth.func = nrhi::CompareFunc::kLessEqual;
      mp.rtv_format = nrhi::Format::kR8_UNORM;
      mp.dsv_format = nrhi::Format::kD32_FLOAT;
      mp.sample_count = g_r.msaa;
      g_r.pso_stencil_mark = device->CreateGraphicsPipeline(mp);
      device->DestroyDeferred(mps);
      // Its alpha-tested variant (the foreground mark; see fg_mark).
      if (g_r.pso_stencil_mark_clip != nullptr) {
        device->DestroyDeferred(g_r.pso_stencil_mark_clip);
        g_r.pso_stencil_mark_clip = nullptr;
      }
      nrhi::Shader* mcp = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                         "ps_stencil_mark_clip", nullptr, ""));
      if (mcp != nullptr) {
        mp.ps = mcp;
        g_r.pso_stencil_mark_clip = device->CreateGraphicsPipeline(mp);
        device->DestroyDeferred(mcp);
      }
    }
    if (g_r.pso_stencil_mark == nullptr) {
      REXLOG_WARN("native-scene: stencil mark PSO creation failed (postfx "
                  "chain runs without the character stencil)");
    }
  }
  pso.rtv_format = scene_fmt;
  pso.sample_count = g_r.msaa;
  device->DestroyDeferred(vs);
  device->DestroyDeferred(ps);
  if (g_r.pso == nullptr || g_r.pso_nodepth == nullptr ||
      g_r.pso_transparent == nullptr) {
    REXLOG_ERROR("native-scene: PSO creation failed");
    g_r.failed = true;
    return false;
  }
  return true;
}

// Fullscreen MSAA resolve PSO (no-op below MSAA 2x).
bool EnsureResolvePso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.resolve_pso != nullptr) {
    device->DestroyDeferred(g_r.resolve_pso);
    g_r.resolve_pso = nullptr;
  }
  if (g_r.msaa > 1) {
    char samples[8];
    std::snprintf(samples, sizeof(samples), "%u", g_r.msaa);
    const nrhi::ShaderMacro macros[] = {{"SAMPLES", samples},
                                        {nullptr, nullptr}};
    char variant[16];
    std::snprintf(variant, sizeof(variant), "SAMPLES=%u", g_r.msaa);
    nrhi::Shader* rvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "resolve.hlsl",
                       kResolveShaderSource, "vs_main", macros, variant));
    nrhi::Shader* rps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "resolve.hlsl",
                       kResolveShaderSource, "ps_main", macros, variant));
    if (rvs == nullptr || rps == nullptr) {
      REXLOG_ERROR("native-scene: resolve shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc rp;
    rp.layout = g_r.layout;
    rp.vs = rvs;
    rp.ps = rps;
    rp.cull = nrhi::CullMode::kNone;
    // Under HDR the resolve averages the float MSAA scene into the 1x float
    // plane (hdr_resolved); classic resolves straight into the guest output.
    rp.rtv_format = g_r.hdr_active ? g_r.hdr_scene_format
                                   : context.guest_output->format();
    rp.sample_count = 1;
    g_r.resolve_pso = device->CreateGraphicsPipeline(rp);
    device->DestroyDeferred(rvs);
    device->DestroyDeferred(rps);
    if (g_r.resolve_pso == nullptr) {
      REXLOG_ERROR("native-scene: resolve PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// Popup background blur pipelines; PSO-create failure only disables blur.
bool EnsureBlurPsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Popup background blur pipelines (blur_hBlur/vBlur port + basictex
    // replace): fullscreen-triangle passes, no vertex buffer.
    nrhi::Shader* bvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "blur.hlsl",
                       kBlurShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* bps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_main", nullptr, ""));
    nrhi::Shader* bblit = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_blit", nullptr, ""));
    nrhi::Shader* bdown = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_down", nullptr, ""));
    nrhi::Shader* bgauss = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_menu_gauss", nullptr, ""));
    nrhi::Shader* bphoto = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl",
                       kBlurShaderSource, "ps_photo_out", nullptr, ""));
    if (bvs == nullptr || bps == nullptr || bblit == nullptr ||
        bdown == nullptr || bgauss == nullptr) {
      REXLOG_ERROR("native-scene: blur shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc bp;
    bp.layout = g_r.layout;
    bp.vs = bvs;
    bp.ps = bps;
    bp.cull = nrhi::CullMode::kNone;
    bp.rtv_format = context.guest_output->format();
    bp.sample_count = 1;
    if (g_r.pso_blur) device->DestroyDeferred(g_r.pso_blur);
    if (g_r.pso_blur_blit) device->DestroyDeferred(g_r.pso_blur_blit);
    if (g_r.pso_blur_down) device->DestroyDeferred(g_r.pso_blur_down);
    if (g_r.pso_menu_gauss) device->DestroyDeferred(g_r.pso_menu_gauss);
    g_r.pso_blur = device->CreateGraphicsPipeline(bp);
    bp.ps = bblit;
    g_r.pso_blur_blit = device->CreateGraphicsPipeline(bp);
    bp.ps = bdown;
    g_r.pso_blur_down = device->CreateGraphicsPipeline(bp);
    bp.ps = bgauss;
    g_r.pso_menu_gauss = device->CreateGraphicsPipeline(bp);
    // The masked popup blur's final composite (see FrameScene::ui_blur_masked).
    for (nrhi::Pipeline** pp : {&g_r.pso_blur_blit_masked, &g_r.pso_blur_blit_masked_ms}) {
      if (*pp) device->DestroyDeferred(*pp);
      *pp = nullptr;
    }
    {
      // 1x and MSAA variants: the mark lives at the scene's sample count.
      nrhi::Shader* bmask = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource,
                         "ps_blit_masked", nullptr, ""));
      if (bmask != nullptr) {
        bp.ps = bmask;
        g_r.pso_blur_blit_masked = device->CreateGraphicsPipeline(bp);
        device->DestroyDeferred(bmask);
      }
      nrhi::ShaderMacro ms_defs[2] = {{"MASK_MSAA", "1"}, {nullptr, nullptr}};
      nrhi::Shader* bmask_ms = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource,
                         "ps_blit_masked", ms_defs, "MASK_MSAA=1"));
      if (bmask_ms != nullptr) {
        bp.ps = bmask_ms;
        g_r.pso_blur_blit_masked_ms = device->CreateGraphicsPipeline(bp);
        device->DestroyDeferred(bmask_ms);
      }
    }
    if (g_r.pso_photo_out) device->DestroyDeferred(g_r.pso_photo_out);
    g_r.pso_photo_out = nullptr;
    if (bphoto != nullptr) {
      // Premultiplied over the photo's clear + backdrop layers (see ps_photo_out).
      nrhi::GraphicsPipelineDesc pp = bp;
      pp.ps = bphoto;
      pp.blend.enable = true;
      pp.blend.src = nrhi::BlendFactor::kOne;
      pp.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
      pp.blend.op = nrhi::BlendOp::kAdd;
      pp.blend.src_alpha = nrhi::BlendFactor::kOne;
      pp.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
      pp.blend.op_alpha = nrhi::BlendOp::kAdd;
      g_r.pso_photo_out = device->CreateGraphicsPipeline(pp);
      device->DestroyDeferred(bphoto);
    }
    device->DestroyDeferred(bvs);
    device->DestroyDeferred(bps);
    device->DestroyDeferred(bblit);
    device->DestroyDeferred(bdown);
    device->DestroyDeferred(bgauss);
    if (g_r.pso_blur == nullptr || g_r.pso_blur_blit == nullptr ||
        g_r.pso_blur_down == nullptr || g_r.pso_menu_gauss == nullptr) {
      REXLOG_ERROR("native-scene: blur PSO creation failed");
      g_r.pso_blur = nullptr;
      g_r.pso_blur_blit = nullptr;  // blur unavailable; everything else runs
      g_r.pso_blur_down = nullptr;
      g_r.pso_menu_gauss = nullptr;
    }
  }
  return true;
}

// Selection-outline edge composite PSO; create failure disables outline.
bool EnsureOutlineEdgePso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Selection-outline edge composite (postfx_edgedetectstencil port):
    // fullscreen triangle over the resolved output, additive blend.
    nrhi::Shader* ovs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "outline.hlsl",
                       kOutlineShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* ops = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "outline.hlsl",
                       kOutlineShaderSource, "ps_main", nullptr, ""));
    if (ovs == nullptr || ops == nullptr) {
      REXLOG_ERROR("native-scene: outline shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc op;
    op.layout = g_r.layout;
    op.vs = ovs;
    op.ps = ops;
    op.blend.enable = true;
    op.blend.src = nrhi::BlendFactor::kOne;
    op.blend.dst = nrhi::BlendFactor::kOne;
    op.blend.op = nrhi::BlendOp::kAdd;
    op.blend.src_alpha = nrhi::BlendFactor::kZero;
    op.blend.dst_alpha = nrhi::BlendFactor::kOne;
    op.blend.op_alpha = nrhi::BlendOp::kAdd;
    op.cull = nrhi::CullMode::kNone;
    op.rtv_format = context.guest_output->format();
    op.sample_count = 1;
    if (g_r.pso_outline_edge) device->DestroyDeferred(g_r.pso_outline_edge);
    g_r.pso_outline_edge = device->CreateGraphicsPipeline(op);
    device->DestroyDeferred(ovs);
    device->DestroyDeferred(ops);
    if (g_r.pso_outline_edge == nullptr) {
      REXLOG_WARN("native-scene: outline edge PSO creation failed");
      // outline unavailable; everything else runs
    }
  }
  return true;
}

// Does the guest draw THIS item as wireframe? The debug menu's Render ->
// Settings -> Wireframe switches PA_SU_SC_MODE_CNTL to poly_mode =
// kDualMode(1) with polymode_front/back_ptype = kLines(1).
//
// It is NOT a global state. Capture 1790959265 (wireframe on) splits 943
// line draws from 559 filled ones: the main-pass material shaders go to
// lines, while every *_shadowPS pass, the z-prepass/world-shadow draws, the
// deck AO and the whole postfx/font/SimpleDraw tail stay FILLED. Forcing the
// frame wholesale broke exactly those: no z-prepass meant the world drew
// see-through, the shadow passes stopped casting (the board lost its
// shadow), and hair rendered as a solid cap instead of lines.
// The game's render-state registers for an item's draw: its own main-pass
// island state when captured (DrawItem::game_rs), else the ib-keyed
// FrameScene::draw_rs entry (last draw with that index buffer, which can be
// a neighbouring island's when meshes share buffers). False = unrecorded.
bool ItemDrawRs(const FrameScene& scene, const DrawItem& item, DrawRegState& out) {
  if (item.game_rs_seen) {
    out.depth = item.game_rs[0];
    out.blend = item.game_rs[1];
    out.color_ctl = item.game_rs[2];
    out.color_mask = item.game_rs[3];
    out.stencil = item.game_rs[4];
    out.mode_cntl = item.game_rs[5];
    return true;
  }
  const DrawRegState* it = scene.draw_rs.find(item.ib_obj);
  if (it == nullptr) {
    return false;
  }
  out = *it;
  return true;
}

bool GameWireframeItem(const FrameScene& scene, const DrawItem& item) {
  DrawRegState st;
  if (!ItemDrawRs(scene, item, st) || st.mode_cntl == 0) {
    return false;
  }
  const uint32_t poly_mode = (st.mode_cntl >> 3) & 3u;
  const uint32_t front_ptype = (st.mode_cntl >> 5) & 7u;
  return poly_mode == 1u && front_ptype == 1u;
}

// The game's own face culling for an item's draw (PA_SU_SC_MODE_CNTL:
// cull_front bit 0, cull_back bit 1, face (front = CW) bit 2), as our D3D12
// cull mode: 1 = kFront, 2 = kBack, 0 = unrecorded / uncull(ed) / both
// facings culled (left to the default path). Calibration: 0x18005
// (cull_front, face CW) keeps the faces our pipeline sees as D3D12 BACK
// faces = kFront, 0x18006 = kBack (see the banner / editor preview notes in
// the scene PSO setup); a clear face bit swaps which winding is "front".
// The register applies to the transformed triangles on console exactly as
// here, so mirrored instances need no special case.
int GameCullMode(const FrameScene& scene, const DrawItem& item) {
  if (!REXCVAR_GET(skate3_native_render_scene_backface_cull)) {
    return 0;
  }
  DrawRegState st;
  if (!ItemDrawRs(scene, item, st) || st.mode_cntl == 0) {
    return 0;
  }
  const bool cull_front = (st.mode_cntl & 1u) != 0;
  const bool cull_back = (st.mode_cntl & 2u) != 0;
  if (cull_front == cull_back) {
    return 0;
  }
  const bool face_cw = (st.mode_cntl & 4u) != 0;
  return cull_front == face_cw ? 1 : 2;
}

// The game's blend mode for an item's perspective draw this frame, from its
// recorded register state (FrameScene::draw_rs): 0 = not recorded, 1 = blend
// off (RB_BLENDCONTROL0 src ONE / dst ZERO), 2 = straight alpha (SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA), 3 = additive (dst ONE), 4 = color writes off
// (RB_COLOR_MASK rt0 = 0), 5 = some other blend.
int GameBlendMode(const FrameScene& scene, const DrawItem& item) {
  DrawRegState st;
  if (!ItemDrawRs(scene, item, st)) {
    return 0;
  }
  if ((st.color_mask & 0xFu) == 0) {
    return 4;
  }
  const uint32_t cs = st.blend & 0x1Fu;
  const uint32_t co = (st.blend >> 5) & 0x7u;
  const uint32_t cd = (st.blend >> 8) & 0x1Fu;
  if (st.blend == 0) {
    return 0;  // array not populated: unknown
  }
  if (cs == 1 && cd == 0 && co == 0) return 1;
  if (cs == 6 && cd == 7 && co == 0) return 2;
  if (cd == 1 && co == 0) return 3;
  return 5;
}

// RB_DEPTHCONTROL.z_write_enable (bit 2) of the item's recorded draw.
bool GameDrawZWrite(const FrameScene& scene, const DrawItem& item) {
  DrawRegState st;
  return ItemDrawRs(scene, item, st) && ((st.depth >> 2) & 1u) != 0;
}

// The stencil byte the item's recorded draw leaves on the pixels it covers
// (RB_DEPTHCONTROL: stencil_enable bit 0, front z-pass op bits 14-16;
// RB_STENCILREFMASK 0x210D: ref bits 0-7, write mask bits 16-23), or -1 if
// the draw does not write stencil. Characters and pedestrians REPLACE with
// 0xFF (func ALWAYS, alpha test off); world geometry leaves the cleared 0.
// Only KEEP / ZERO / REPLACE are resolvable without the prior contents.
int GameStencilWrite(const FrameScene& scene, const DrawItem& item) {
  DrawRegState st;
  if (!ItemDrawRs(scene, item, st) || (st.depth & 1u) == 0) {
    return -1;
  }
  const uint32_t zpass = (st.depth >> 14) & 7u;
  const uint32_t ref = st.stencil & 0xFFu;
  const uint32_t wmask = (st.stencil >> 16) & 0xFFu;
  if (zpass == 1) {
    return 0;
  }
  if (zpass == 2) {
    return int(ref & wmask);
  }
  return -1;
}

// The captured character alpha is what the game FEEDS the shader, which is not
// always what it SHOWS. A remote skater in the pass-through (skate-through)
// state gets alpha 0.5 in its alpha row, but the game draws those pieces with
// blend ONE/ZERO (captured rs_blend 0x00010001), so the value only reaches the
// target's alpha channel and the skater stays solid on console -- only its
// genuinely blended pieces (lens, hair coverage) show anything. Follow the
// draw rather than the constant, the same way the transenv routing does, and
// let a fade through only where the game itself blends. An unrecorded draw
// (mode 0) keeps the constant, so items whose registers we never saw behave as
// before.
float CharFadeAlphaDrawn(const FrameScene& scene, const DrawItem& item) {
  const float a = CharFadeAlpha(item);
  if (a >= 0.999f) {
    return a;
  }
  return GameBlendMode(scene, item) == 1 ? 1.0f : a;
}

// 2D pipeline matching the game's RB_BLENDCONTROL0 for a draw (Xenos
// layout: color src 0-4, op 5-7, dst 8-12; alpha src 16-20, op 21-23, dst
// 24-28; xenos::BlendFactor 0 zero, 1 one, 6 src alpha, 7 one-minus-src
// alpha; BlendOp 0 add, 2 min). 0x07060706 (straight alpha) is pso_2d
// itself; ONE/ZERO is blending off. Values the RHI cannot express keep
// pso_2d (logged once each).
nrhi::Pipeline* Get2dBlendPso(const NativeGuestOutputRenderContext& context,
                              uint32_t blend_control) {
  if (blend_control == 0x07060706u || g_r.pso_2d == nullptr) {
    return g_r.pso_2d;
  }
  const nrhi::Format fmt = context.guest_output->format();
  for (uint32_t i = 0; i < g_r.pso_2d_blend_count; ++i) {
    const auto& e = g_r.pso_2d_blend[i];
    if (e.blend_control == blend_control && e.format == fmt) {
      return e.pso != nullptr ? e.pso : g_r.pso_2d;
    }
  }
  if (g_r.pso_2d_blend_count >= std::size(g_r.pso_2d_blend)) {
    // Cache full: never build uncached pipelines per draw (that would
    // compile + leak one every frame); keep straight alpha.
    static bool s_full_logged = false;
    if (!s_full_logged) {
      s_full_logged = true;
      REXLOG_WARN("native-scene: 2D blend pipeline cache full; {:08X} keeps straight alpha",
                  blend_control);
    }
    return g_r.pso_2d;
  }
  const auto factor = [](uint32_t f, bool* ok) {
    switch (f) {
      case 0: return nrhi::BlendFactor::kZero;
      case 1: return nrhi::BlendFactor::kOne;
      case 6: return nrhi::BlendFactor::kSrcAlpha;
      case 7: return nrhi::BlendFactor::kInvSrcAlpha;
      default: *ok = false; return nrhi::BlendFactor::kOne;
    }
  };
  const auto op = [](uint32_t o, bool* ok) {
    switch (o) {
      case 0: return nrhi::BlendOp::kAdd;
      case 2: return nrhi::BlendOp::kMin;
      default: *ok = false; return nrhi::BlendOp::kAdd;
    }
  };
  bool ok = true;
  const uint32_t cs = blend_control & 0x1Fu, co = (blend_control >> 5) & 0x7u,
                 cd = (blend_control >> 8) & 0x1Fu, as = (blend_control >> 16) & 0x1Fu,
                 ao = (blend_control >> 21) & 0x7u, ad = (blend_control >> 24) & 0x1Fu;
  nrhi::GraphicsPipelineDesc up;
  up.blend.src = factor(cs, &ok);
  up.blend.op = op(co, &ok);
  up.blend.dst = factor(cd, &ok);
  up.blend.src_alpha = factor(as, &ok);
  up.blend.op_alpha = op(ao, &ok);
  up.blend.dst_alpha = factor(ad, &ok);
  up.blend.enable = !(cs == 1 && cd == 0 && co == 0 && as == 1 && ad == 0 && ao == 0);
  nrhi::Pipeline* pso = nullptr;
  if (ok) {
    nrhi::Device* device = context.device;
    nrhi::Shader* vs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "overlay2d.hlsl", kShader2dSource, "vs_main", nullptr, ""));
    nrhi::Shader* ps = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "overlay2d.hlsl", kShader2dSource, "ps_main", nullptr, ""));
    if (vs != nullptr && ps != nullptr) {
      static constexpr nrhi::InputElementDesc input2d[3] = {
          {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
          {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
          {"COLOR", 0, 2, nrhi::Format::kR8G8B8A8_UNORM, 24}};
      up.layout = g_r.layout;
      up.vs = vs;
      up.ps = ps;
      up.cull = nrhi::CullMode::kNone;
      up.input_elements = input2d;
      up.input_element_count = 3;
      up.vertex_stride = 28;
      up.rtv_format = fmt;
      up.sample_count = 1;
      pso = device->CreateGraphicsPipeline(up);
    }
    if (vs) device->DestroyDeferred(vs);
    if (ps) device->DestroyDeferred(ps);
  }
  REXLOG_INFO("native-scene: 2D blend state {:08X} -> {}", blend_control,
              !ok ? "unsupported factors, straight alpha kept"
                  : (pso == nullptr ? "pipeline creation FAILED, straight alpha kept"
                                    : (up.blend.enable ? "blend pipeline" : "opaque pipeline")));
  if (g_r.pso_2d_blend_count < std::size(g_r.pso_2d_blend)) {
    g_r.pso_2d_blend[g_r.pso_2d_blend_count++] = {blend_control, fmt, pso};
  }
  return pso != nullptr ? pso : g_r.pso_2d;
}

// 2D overlay pipeline.
bool Ensure2dPso(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // 2D overlay pipeline: standard alpha blend, no depth, drawn into the
    // resolved guest output (sample count 1).
    nrhi::Shader* uvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "overlay2d.hlsl",
                       kShader2dSource, "vs_main", nullptr, ""));
    nrhi::Shader* ups = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "overlay2d.hlsl",
                       kShader2dSource, "ps_main", nullptr, ""));
    if (uvs == nullptr || ups == nullptr) {
      REXLOG_ERROR("native-scene: 2D shader compile failed");
      g_r.failed = true;
      return false;
    }
    static constexpr nrhi::InputElementDesc input2d[3] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
        {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
        {"COLOR", 0, 2, nrhi::Format::kR8G8B8A8_UNORM, 24}};
    nrhi::GraphicsPipelineDesc up;
    up.layout = g_r.layout;
    up.vs = uvs;
    up.ps = ups;
    up.blend.enable = true;
    // Straight (non-premultiplied) alpha, verified against the decoded
    // HUD clock art: the glass-sheen texture is white RGB at low alpha
    // (premultiplied blending blows it out into an opaque white blob).
    up.blend.src = nrhi::BlendFactor::kSrcAlpha;
    up.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    up.blend.op = nrhi::BlendOp::kAdd;
    up.blend.src_alpha = nrhi::BlendFactor::kOne;
    up.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    up.blend.op_alpha = nrhi::BlendOp::kAdd;
    up.cull = nrhi::CullMode::kNone;
    up.input_elements = input2d;
    up.input_element_count = 3;
    up.vertex_stride = 28;
    up.rtv_format = context.guest_output->format();
    up.sample_count = 1;
    g_r.pso_2d = device->CreateGraphicsPipeline(up);
    // FMV variant: identical pipeline, ps_yuv2d combine (see the movie-quad
    // substitution in the 2D replay). Failure only loses native FMV; the
    // emulated yield fallback covers it.
    nrhi::Shader* uyuv = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "overlay2d.hlsl",
                       kShader2dSource, "ps_yuv2d", nullptr, ""));
    if (uyuv != nullptr) {
      up.ps = uyuv;
      if (g_r.pso_yuv2d) device->DestroyDeferred(g_r.pso_yuv2d);
      g_r.pso_yuv2d = device->CreateGraphicsPipeline(up);
      if (g_r.pso_yuv2d == nullptr) {
        REXLOG_WARN("native-scene: FMV 2D PSO creation failed - movies yield");
      }
      device->DestroyDeferred(uyuv);
    } else {
      REXLOG_WARN("native-scene: ps_yuv2d compile failed - movies yield");
    }
    device->DestroyDeferred(uvs);
    device->DestroyDeferred(ups);
    if (g_r.pso_2d == nullptr) {
      REXLOG_ERROR("native-scene: 2D PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// In-world spline pipelines (darken + additive default).
bool EnsureSplinePsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // In-world spline pipelines: drawn inside the scene pass (MSAA sample
    // count, depth test LESS_EQUAL, no z-write) with the game's own blend
    // states (spline.xml): darken = straight alpha, default = additive.
    // Under HDR the pixel shaders encode through the tone chain's inverse
    // (SplineOut) so the host tonemap restores the authored look.
    for (nrhi::Pipeline** p : {&g_r.pso_spline_darken, &g_r.pso_spline_default,
                               &g_r.pso_sdraw}) {
      if (*p != nullptr) {
        device->DestroyDeferred(*p);
        *p = nullptr;
      }
    }
    // SHOWCASE=1 compiles the blackout gate in (see spline.hlsl
    // SplineVisible); like the scene family, the variant follows the
    // showcase swap so normal sessions carry no gate at all.
    nrhi::ShaderMacro sp_defs[3];
    uint32_t sp_def_count = 0;
    if (g_r.hdr_active) {
      sp_defs[sp_def_count++] = {"HDR", "1"};
    }
    if (g_r.showcase_shaders) {
      sp_defs[sp_def_count++] = {"SHOWCASE", "1"};
    }
    sp_defs[sp_def_count] = {nullptr, nullptr};
    char sp_variant[24];
    std::snprintf(sp_variant, sizeof(sp_variant), "%s%s%s",
                  g_r.hdr_active ? "HDR=1" : "",
                  g_r.hdr_active && g_r.showcase_shaders ? ";" : "",
                  g_r.showcase_shaders ? "SHOWCASE=1" : "");
    nrhi::Shader* svs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "spline.hlsl",
                       kShaderSplineSource, "vs_main", nullptr, ""));
    nrhi::Shader* spd = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "spline.hlsl",
                       kShaderSplineSource, "ps_default",
                       sp_def_count != 0 ? sp_defs : nullptr, sp_variant));
    nrhi::Shader* spk = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "spline.hlsl",
                       kShaderSplineSource, "ps_darken",
                       sp_def_count != 0 ? sp_defs : nullptr, sp_variant));
    if (svs == nullptr || spd == nullptr || spk == nullptr) {
      REXLOG_ERROR("native-scene: spline shader compile failed");
      g_r.failed = true;
      return false;
    }
    static constexpr nrhi::InputElementDesc input_spline[3] = {
        {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
        {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
        {"TEXCOORD", 1, 2, nrhi::Format::kR32_FLOAT, 24}};
    nrhi::GraphicsPipelineDesc sp;
    sp.layout = g_r.layout;
    sp.vs = svs;
    sp.cull = nrhi::CullMode::kNone;
    sp.depth_clip = true;
    sp.depth.test_enable = true;
    sp.depth.write_enable = false;
    sp.depth.func = nrhi::CompareFunc::kLessEqual;
    sp.input_elements = input_spline;
    sp.input_element_count = 3;
    sp.vertex_stride = 28;
    sp.rtv_format = g_r.hdr_active ? g_r.hdr_scene_format
                                   : context.guest_output->format();
    sp.dsv_format = nrhi::Format::kD32_FLOAT;
    sp.sample_count = g_r.msaa;
    sp.blend.enable = true;
    sp.blend.op = nrhi::BlendOp::kAdd;
    sp.blend.op_alpha = nrhi::BlendOp::kAdd;
    // darken pass: straight alpha.
    sp.ps = spk;
    sp.blend.src = nrhi::BlendFactor::kSrcAlpha;
    sp.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
    sp.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
    sp.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
    g_r.pso_spline_darken = device->CreateGraphicsPipeline(sp);
    // default pass: additive glow.
    sp.ps = spd;
    sp.blend.src = nrhi::BlendFactor::kOne;
    sp.blend.dst = nrhi::BlendFactor::kOne;
    sp.blend.src_alpha = nrhi::BlendFactor::kOne;
    sp.blend.dst_alpha = nrhi::BlendFactor::kOne;
    g_r.pso_spline_default = device->CreateGraphicsPipeline(sp);
    device->DestroyDeferred(svs);
    device->DestroyDeferred(spd);
    device->DestroyDeferred(spk);
    // SimpleDraw debug primitives: world-space triangle lists (lines arrive
    // pre-expanded into quads), straight alpha, no depth test.
    nrhi::Shader* dvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "spline.hlsl",
                       kShaderSplineSource, "vs_debug", nullptr, ""));
    nrhi::Shader* dps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "spline.hlsl",
                       kShaderSplineSource, "ps_debug",
                       sp_def_count != 0 ? sp_defs : nullptr, sp_variant));
    if (dvs != nullptr && dps != nullptr) {
      static constexpr nrhi::InputElementDesc input_sdraw[4] = {
          {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
          {"TEXCOORD", 0, 1, nrhi::Format::kR32G32B32A32_FLOAT, 16},
          {"TEXCOORD", 1, 2, nrhi::Format::kR32G32B32A32_FLOAT, 32},
          {"TEXCOORD", 2, 3, nrhi::Format::kR32G32_FLOAT, 48}};
      nrhi::GraphicsPipelineDesc dd = sp;
      dd.vs = dvs;
      dd.ps = dps;
      dd.input_elements = input_sdraw;
      dd.input_element_count = 4;
      dd.vertex_stride = sizeof(SimpleDrawVert);
      dd.blend.src = nrhi::BlendFactor::kSrcAlpha;
      dd.blend.dst = nrhi::BlendFactor::kInvSrcAlpha;
      dd.blend.src_alpha = nrhi::BlendFactor::kSrcAlpha;
      dd.blend.dst_alpha = nrhi::BlendFactor::kInvSrcAlpha;
      dd.depth.test_enable = false;
      g_r.pso_sdraw = device->CreateGraphicsPipeline(dd);
    }
    if (dvs != nullptr) device->DestroyDeferred(dvs);
    if (dps != nullptr) device->DestroyDeferred(dps);
    if (g_r.pso_sdraw == nullptr) {
      // Debug primitives only: never fail the renderer over them.
      REXLOG_WARN("native-scene: SimpleDraw PSO creation failed");
    }
    if (g_r.pso_spline_darken == nullptr || g_r.pso_spline_default == nullptr) {
      REXLOG_ERROR("native-scene: spline PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// Dynamic-shadow caster + per-tile blur/convert PSOs.
bool EnsureShadowPsos(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  {
    // Dynamic-shadow pipelines: casters (scene VS with a light-space
    // "view-proj", MIN-blend depth/uncoverage accumulation, no depth
    // buffer, depth clip ON so casters outside the height window are
    // clipped like the game's PA_CL_CLIP_CNTL) + the per-tile blur/convert pass.
    nrhi::Shader* cvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "scene.hlsl", kShaderSource,
                       "vs_main", nullptr, ""));
    nrhi::Shader* cps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                       "ps_shadow_caster", nullptr, ""));
    nrhi::Shader* cpc = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                       "ps_shadow_caster_clip", nullptr, ""));
    nrhi::Shader* cpx = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                       "ps_shadow_caster_clip_exact", nullptr, ""));
    nrhi::Shader* bvs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "shadow_blur.hlsl",
                       kShadowBlurSource, "vs_main", nullptr, ""));
    nrhi::Shader* bps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "shadow_blur.hlsl",
                       kShadowBlurSource, "ps_main", nullptr, ""));
    if (cvs == nullptr || cps == nullptr || cpc == nullptr || bvs == nullptr ||
        bps == nullptr) {
      REXLOG_ERROR("native-scene: shadow shader compile failed");
      g_r.failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc cp;
    cp.layout = g_r.layout;
    cp.vs = cvs;
    cp.ps = cps;
    cp.blend.enable = true;
    cp.blend.src = nrhi::BlendFactor::kOne;
    cp.blend.dst = nrhi::BlendFactor::kOne;
    cp.blend.op = nrhi::BlendOp::kMin;
    cp.blend.src_alpha = nrhi::BlendFactor::kOne;
    cp.blend.dst_alpha = nrhi::BlendFactor::kOne;
    cp.blend.op_alpha = nrhi::BlendOp::kMin;
    cp.blend.write_mask = 0x3;  // RG only
    // The game's shadow-caster passes cull the FRONT faces (every caster
    // draw in the F11 records: PA_SU_SC_MODE_CNTL cull_back + face_cw, the
    // opposite of its main pass's cull_front), so the atlas holds the far
    // side of each caster and lit front surfaces never shadow themselves.
    // Casting both sides self-shadowed the faces (a hard nose shadow and a
    // key-light-less dark cheek on the CAS skater). Same Xenos -> D3D12
    // mapping as the main pass (its CULL_FRONT materials use kFront).
    cp.cull = nrhi::CullMode::kBack;
    // Depth CLIP like the game: every caster draw runs PA_CL_CLIP_CNTL =
    // 0x80000 (clipping on, D3D z range, near/far z-clip enabled), so a
    // caster outside a cascade's height window (the stylized depth row
    // spans only ~6 m around the player) is CLIPPED, not clamped. Clamping
    // stored the out-of-window caster at depth 1.0 with full coverage and
    // shadowed every receiver below the window above it too (a street car
    // painting its shadow onto the bridge deck overhead).
    cp.depth_clip = true;
    cp.input_elements = kSceneInputLayout;
    cp.input_element_count = 7;
    cp.vertex_stride = 56;
    cp.rtv_format = nrhi::Format::kR16G16_UNORM;
    cp.sample_count = 1;
    g_r.pso_shadow_caster = device->CreateGraphicsPipeline(cp);
    cp.ps = cpc;
    g_r.pso_shadow_caster_clip = device->CreateGraphicsPipeline(cp);
    if (cpx != nullptr) {
      cp.ps = cpx;
      g_r.pso_shadow_caster_clip_exact = device->CreateGraphicsPipeline(cp);
      device->DestroyDeferred(cpx);
    }
    // Static maps: uncull(ed) (see RendererState::pso_static_caster). The
    // native static sun map keeps its own +-2r depth window unclipped.
    cp.cull = nrhi::CullMode::kNone;
    cp.depth_clip = false;
    cp.ps = cpc;
    g_r.pso_static_caster_clip = device->CreateGraphicsPipeline(cp);
    cp.ps = cps;
    g_r.pso_static_caster = device->CreateGraphicsPipeline(cp);
    // The game's world-shade map is a 24-bit depth target; its depth row
    // c7 moves ~1.6e-4 per metre of height, so 16-bit UNORM (1.5e-5 per
    // step) quantized heights to ~9.4 cm and the ground under a prop read
    // above the prop's lower surfaces. Float storage keeps the compare
    // sub-millimetre like the console's.
    cp.rtv_format = nrhi::Format::kR32G32_FLOAT;
    // The game's world-shade pass draws with the same clip state
    // (PA_CL_CLIP_CNTL 0x80000) as its caster passes.
    cp.depth_clip = true;
    g_r.pso_ws_caster = device->CreateGraphicsPipeline(cp);
    cp.cull = nrhi::CullMode::kFront;
    g_r.pso_ws_caster_game = device->CreateGraphicsPipeline(cp);
    // The skate.Park world-shadow passes draw with 0x18006 (cull_back),
    // the cascade caster passes' state.
    cp.cull = nrhi::CullMode::kBack;
    g_r.pso_park_ws_caster = device->CreateGraphicsPipeline(cp);
    {
      // The alphatest pieces' pass (environmentparkalphatest_shadowPS): one
      // tf3 alpha fetch at texcoord 0, kill when a < 0.12 (literal c255.x),
      // no LOD bias -- exactly ps_shadow_caster_clip_exact.
      nrhi::Shader* pcx = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "scene.hlsl", kShaderSource,
                         "ps_shadow_caster_clip_exact", nullptr, ""));
      if (pcx != nullptr) {
        cp.ps = pcx;
        // Double-sided: the game draws this pass with PA_SU_SC_MODE_CNTL
        // 0x18004 (no culling; capture 1790885990), where the opaque park
        // passes use 0x18006 (cull_back). Back-face culling here dropped
        // every leaf card facing away from the light: thinned tree shadows.
        cp.cull = nrhi::CullMode::kNone;
        g_r.pso_park_ws_caster_clip = device->CreateGraphicsPipeline(cp);
        device->DestroyDeferred(pcx);
      }
    }
    nrhi::GraphicsPipelineDesc bp;
    bp.layout = g_r.layout;
    bp.vs = bvs;
    bp.ps = bps;
    bp.cull = nrhi::CullMode::kNone;
    bp.depth_clip = true;
    bp.rtv_format = nrhi::Format::kR16G16_UNORM;
    bp.sample_count = 1;
    g_r.pso_shadow_blur = device->CreateGraphicsPipeline(bp);
    {
      nrhi::Shader* eps = device->CreateShader(
          MakeShaderDesc(nrhi::ShaderStage::kPixel, "shadow_blur.hlsl",
                         kShadowBlurSource, "ps_exp", nullptr, ""));
      if (eps != nullptr) {
        bp.ps = eps;
        bp.rtv_format = nrhi::Format::kR32G32_FLOAT;
        g_r.pso_park_ws_blur = device->CreateGraphicsPipeline(bp);
        device->DestroyDeferred(eps);
      }
      if (g_r.pso_park_ws_blur == nullptr) {
        // Not fatal: the park map is then used unfiltered.
        REXLOG_ERROR("native-scene: park world-shadow blur PSO creation failed");
      }
    }
    device->DestroyDeferred(cvs);
    device->DestroyDeferred(cps);
    device->DestroyDeferred(cpc);
    device->DestroyDeferred(bvs);
    device->DestroyDeferred(bps);
    if (g_r.pso_shadow_caster == nullptr ||
        g_r.pso_shadow_caster_clip == nullptr ||
        g_r.pso_shadow_blur == nullptr) {
      REXLOG_ERROR("native-scene: shadow PSO creation failed");
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// The bone / ROPA / 2D-vertex upload rings (persistently mapped). The old
// RTV/DSV/SRV descriptor heaps and their increment-size bookkeeping are gone
// - render targets bind by texture and sampled views are backend-managed
// objects. (Name kept for the call sites.)
bool EnsureHeapsAndRings(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;

  if (!g_r.bone_ring) {
    g_r.bone_ring = CreateUploadBuffer(
        device, size_t(RendererState::kBoneRegionSize) * RendererState::kBoneRegions);
    g_r.bone_ring_cpu =
        g_r.bone_ring ? static_cast<uint8_t*>(device->Map(g_r.bone_ring))
                      : nullptr;
    if (g_r.bone_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.ropa_ring) {
    g_r.ropa_ring = CreateUploadBuffer(
        device, size_t(RendererState::kRopaRegionSize) * RendererState::kBoneRegions);
    g_r.ropa_ring_cpu =
        g_r.ropa_ring ? static_cast<uint8_t*>(device->Map(g_r.ropa_ring))
                      : nullptr;
    if (g_r.ropa_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.ui_ring) {
    g_r.ui_ring = CreateUploadBuffer(
        device, size_t(RendererState::kUiRegionSize) * RendererState::kUiRegions);
    g_r.ui_ring_cpu = g_r.ui_ring
                          ? static_cast<uint8_t*>(device->Map(g_r.ui_ring))
                          : nullptr;
    if (g_r.ui_ring_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// ---- Photo-editor postfx chain (photo_fx.hlsl) -----------------------------
// Root signature + eight PSOs + the fixed-size intermediates. Lazy: built on
// the first photo-editor frame (a one-time ~100 ms compile the frozen-scene
// editor absorbs invisibly). Output-sized targets are (re)built per frame by
// the render block on size change.
bool EnsurePhotoFxPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.pfx_ready) {
    return true;
  }
  if (g_r.pfx_failed) {
    return false;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: photo postfx pipeline setup failed ({})", what);
    g_r.pfx_failed = true;
    return false;
  };
  // Binding layout: CBV b0 + ONE eight-texture table t0..t7 + samplers. (The
  // old eight single-SRV tables would need nine Vulkan descriptor sets,
  // over the 8-set minimum on Intel/MoltenVK, so the passes gather their
  // t0..t7 views and bind them with one cmd->SetTextures(1, views, 8).)
  if (g_r.pfx_layout == nullptr) {
    nrhi::BindingLayoutDesc ld;
    ld.param_count = 2;
    ld.params[0] = {nrhi::BindingParamKind::kConstantBuffer, /*b*/ 0, 1,
                    nrhi::Visibility::kAll};
    ld.params[1] = {nrhi::BindingParamKind::kTextureTable, /*t*/ 0, 8,
                    nrhi::Visibility::kPixel};
    ld.static_sampler_count = 3;
    ld.static_samplers[0] = {/*s*/ 0, nrhi::Filter::kLinear,
                             nrhi::AddressMode::kClamp, 1};
    ld.static_samplers[1] = {1, nrhi::Filter::kPoint, nrhi::AddressMode::kClamp,
                             1};  // packed depth
    ld.static_samplers[2] = {2, nrhi::Filter::kLinear, nrhi::AddressMode::kWrap,
                             1};  // grain
    ld.allow_input_layout = false;
    g_r.pfx_layout = device->CreateBindingLayout(ld);
    if (g_r.pfx_layout == nullptr) {
      return fail("root signature");
    }
  }
  // Shaders + PSOs. Pass order matches RendererState::pfx_pso.
  struct Entry {
    const char* vs;
    const char* ps;
    nrhi::Format rtv;
  };
  const Entry entries[9] = {
      {"vs_raw", "ps_depthpack", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_visualfx", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_down", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_mb", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_offset", "ps_dof", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_raw", "ps_uber", nrhi::Format::kR8G8B8A8_UNORM},
      {"vs_scaled", "ps_fisheye", context.guest_output->format()},
      {"vs_raw", "ps_blit", nrhi::Format::kR8G8B8A8_UNORM},
      // Debug visualizer (photo_native_debug cvar): CoC / packed-depth view
      // drawn over the output instead of the fisheye result.
      {"vs_raw", "ps_pfx_debug", context.guest_output->format()},
  };
  const nrhi::ShaderMacro msaa_defines[] = {{"PFX_MSAA", "1"},
                                            {nullptr, nullptr}};
  for (int i = 0; i < 9; ++i) {
    const bool msaa_pass = (i == 0 && g_r.msaa > 1);
    const nrhi::ShaderMacro* defs = msaa_pass ? msaa_defines : nullptr;
    const char* variant = msaa_pass ? "PFX_MSAA=1" : "";
    nrhi::Shader* vs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i].vs, defs, variant));
    nrhi::Shader* ps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i].ps, defs, variant));
    if (vs == nullptr || ps == nullptr) {
      REXLOG_ERROR("native-scene: photo postfx shader compile failed ({})",
                   entries[i].ps);
      g_r.pfx_failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc pso;
    pso.layout = g_r.pfx_layout;
    pso.vs = vs;
    pso.ps = ps;
    pso.cull = nrhi::CullMode::kNone;
    pso.depth_clip = true;
    pso.rtv_format = entries[i].rtv;
    pso.sample_count = 1;
    g_r.pfx_pso[i] = device->CreateGraphicsPipeline(pso);
    device->DestroyDeferred(vs);
    device->DestroyDeferred(ps);
    if (g_r.pfx_pso[i] == nullptr) {
      return fail(entries[i].ps);
    }
  }
  // Fixed-size intermediates + the identity grade LUT + the CB ring.
  {
    nrhi::TextureDesc desc;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    struct Fixed {
      nrhi::Texture** res;
      uint32_t w, h;
    };
    const Fixed fixed[1] = {
        {&g_r.pfx_quarter, RendererState::kPfxQuarterW, RendererState::kPfxQuarterH},
    };
    for (const Fixed& f : fixed) {
      if (*f.res != nullptr) {
        continue;
      }
      desc.width = f.w;
      desc.height = f.h;
      *f.res = device->CreateTexture(desc);
      if (*f.res == nullptr) {
        return fail("intermediate target");
      }
    }
    if (g_r.pfx_lut == nullptr) {
      // 32^3 identity grade LUT (the editor captures run with the LUT blend
      // weight at 0; identity keeps any treatment that enables it neutral
      // instead of garbage). Coordinate mapping mirrors the uber literals:
      // u = g*0.96875 + 0.015625, v = r*(-0.96875) + 0.984375 (flipped),
      // w = b*0.96875 + 0.015625, so voxel (ix,iy,iz) stores
      // r = (0.984375 - v)/0.96875, g = (u - 0.015625)/0.96875, b likewise.
      nrhi::TextureDesc lut;
      lut.kind = nrhi::TextureKind::k3D;
      lut.width = 32;
      lut.height = 32;
      lut.depth = 32;
      lut.format = nrhi::Format::kR8G8B8A8_UNORM;
      lut.initial_state = nrhi::ResourceState::kCopyDest;
      g_r.pfx_lut = device->CreateTexture(lut);
      if (g_r.pfx_lut == nullptr) {
        return fail("LUT");
      }
      // Upload: 32 rows of 32 texels x 32 slices, 256-byte row pitch.
      const uint32_t row_pitch = 256;
      const uint32_t slice_pitch = row_pitch * 32;
      g_r.pfx_lut_upload =
          CreateUploadBuffer(device, size_t(slice_pitch) * 32, nrhi::BufferBindClass::kCopySrc);
      if (g_r.pfx_lut_upload == nullptr) {
        return fail("LUT upload");
      }
      uint8_t* p = static_cast<uint8_t*>(device->Map(g_r.pfx_lut_upload));
      if (p == nullptr) {
        return fail("LUT map");
      }
      for (uint32_t iz = 0; iz < 32; ++iz) {
        for (uint32_t iy = 0; iy < 32; ++iy) {
          uint8_t* row = p + size_t(iz) * slice_pitch + size_t(iy) * row_pitch;
          const float v = (float(iy) + 0.5f) / 32.0f;
          const float w = (float(iz) + 0.5f) / 32.0f;
          const float r = (0.984375f - v) / 0.96875f;
          const float b = (w - 0.015625f) / 0.96875f;
          for (uint32_t ix = 0; ix < 32; ++ix) {
            const float u = (float(ix) + 0.5f) / 32.0f;
            const float g = (u - 0.015625f) / 0.96875f;
            row[ix * 4 + 0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 3] = 255;
          }
        }
      }
      device->Unmap(g_r.pfx_lut_upload);
    }
    if (g_r.pfx_cb == nullptr) {
      // 32 slots x 4 KB per frame region x 4 frames in flight: the photo
      // chain uses 0..9 (depth pack + accum feed + 6 passes + accum mode 2
      // + the debug view), the lens pass 10, the game postfx 16..25.
      g_r.pfx_cb = CreateUploadBuffer(device, 32u * 4096u * 4u);
      g_r.pfx_cb_ptr = g_r.pfx_cb
                           ? static_cast<uint8_t*>(device->Map(g_r.pfx_cb))
                           : nullptr;
      if (g_r.pfx_cb_ptr == nullptr) {
        return fail("constant ring");
      }
    }
  }
  // Fixed-target views (4 = quarter, 6 = LUT; 0/1/2/3/5/7, the
  // output-sized full/half/depth targets and the native depth resource, are
  // (re)created by the render block on size change).
  if (!g_r.pfx_srv_allocated) {
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.pfx_srv[4] = device->CreateTextureView(g_r.pfx_quarter, vd);
    // LUT SRV (Texture3D).
    nrhi::TextureViewDesc lv;
    lv.dimension = nrhi::ViewDimension::k3D;
    lv.mip_levels = 1;
    g_r.pfx_srv[6] = device->CreateTextureView(g_r.pfx_lut, lv);
    if (g_r.pfx_srv[4] == nullptr || g_r.pfx_srv[6] == nullptr) {
      return fail("fixed-target views");
    }
    g_r.pfx_srv_allocated = true;
  }
  g_r.pfx_ready = true;
  REXLOG_INFO("native-scene: photo postfx pipeline ready (9 passes, msaa={})",
              g_r.msaa);
  return true;
}

// Fisheye-family final pass PSOs on the photo postfx binding layout (call
// after EnsurePhotoFxPipeline succeeded): the three variant ports, drawn
// from lens_tex (the frame copy) to the output at the output format
// (rebuilt when it changes).
// The CAS subsurface-scattering targets (see RendererState::sss_unwrap):
// 128x128 RGBA8, the game's texture-space target size and format.
// The world-shade map is usable by receivers this frame (see dyn_misc.z).
bool WorldShadeReady(const FrameScene& scene) {
  (void)scene;
  return g_r.world_shadow_srv != nullptr && g_r.world_shadow_in_srv &&
         g_r.world_shadow_primed;
}

bool EnsureSkinSssTargets(nrhi::Device* device) {
  if (g_r.sss_unwrap_view != nullptr && g_r.sss_blur_view != nullptr) {
    return true;
  }
  static bool s_failed = false;
  if (s_failed) {
    return false;
  }
  for (int k = 0; k < 2; ++k) {
    nrhi::TextureDesc desc;
    desc.width = 128;
    desc.height = 128;
    desc.mip_levels = 1;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    nrhi::Texture* t = device->CreateTexture(desc);
    nrhi::TextureView* v = nullptr;
    if (t != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      v = device->CreateTextureView(t, vd);
    }
    if (v == nullptr) {
      s_failed = true;
      REXLOG_WARN("native-scene: skin subsurface targets unavailable");
      return false;
    }
    (k == 0 ? g_r.sss_unwrap : g_r.sss_blur) = t;
    (k == 0 ? g_r.sss_unwrap_view : g_r.sss_blur_view) = v;
  }
  g_r.sss_unwrap_srv = false;
  g_r.sss_blur_srv = false;
  return true;
}

bool EnsureLensFxPipeline(const NativeGuestOutputRenderContext& context) {
  static nrhi::Format s_format = nrhi::Format::kR8G8B8A8_UNORM;
  const nrhi::Format fmt = context.guest_output->format();
  if (g_r.lens_pso[2] != nullptr && s_format == fmt) {
    return true;
  }
  if (g_r.lens_failed) {
    return false;
  }
  nrhi::Device* device = context.device;
  for (nrhi::Pipeline*& p : g_r.lens_pso) {
    if (p != nullptr) {
      device->DestroyDeferred(p);
      p = nullptr;
    }
  }
  const char* const entries[3][2] = {{"vs_scaled", "ps_fisheye_noise"},
                                     {"vs_scaled", "ps_fisheye_blend"},
                                     {"vs_scaled", "ps_fisheye"}};
  for (int i = 0; i < 3; ++i) {
    nrhi::Shader* vs = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kVertex, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i][0], nullptr, ""));
    nrhi::Shader* ps = device->CreateShader(
        MakeShaderDesc(nrhi::ShaderStage::kPixel, "photo_fx.hlsl",
                       kPhotoFxShaderSource, entries[i][1], nullptr, ""));
    if (vs == nullptr || ps == nullptr) {
      REXLOG_ERROR("native-scene: lens postfx shader compile failed ({})", entries[i][1]);
      g_r.lens_failed = true;
      return false;
    }
    nrhi::GraphicsPipelineDesc pso;
    pso.layout = g_r.pfx_layout;
    pso.vs = vs;
    pso.ps = ps;
    pso.cull = nrhi::CullMode::kNone;
    pso.depth_clip = true;
    pso.rtv_format = fmt;
    pso.sample_count = 1;
    g_r.lens_pso[i] = device->CreateGraphicsPipeline(pso);
    device->DestroyDeferred(vs);
    device->DestroyDeferred(ps);
    if (g_r.lens_pso[i] == nullptr) {
      REXLOG_ERROR("native-scene: lens postfx pipeline setup failed ({})", entries[i][1]);
      g_r.lens_failed = true;
      return false;
    }
  }
  s_format = fmt;
  REXLOG_INFO("native-scene: lens postfx pipelines ready (noise, fisheye, opaque)");
  return true;
}

// ---- The game's bloom chain + final postfx composite ----------------------
// The game renders its scene into a 1152x640 target and finishes it with
// its postfx: the bloom chain (bloom_dof_tap4_minusthreshold -> tap4 ->
// tap9 blurs -> tap9_combine3 -> bloom_ghost, at 288x160 / 72x40) and the
// composite postfx_fast_uber (front end) / postfx_visualfx (gameplay):
// scene * 1.41 + bloom. The native material shaders write that scene
// target (see ToneOut); this pass runs the game's own passes on it, with
// the constants the game staged for them this frame. The bloom runs only
// when the game drew its chain (so the build's debug-menu bloom switch and
// any scene that runs without bloom behave as on the console); the
// composite always runs, as it always does in the game.
namespace {
bool EnsureGamePostfxPipeline(const NativeGuestOutputRenderContext& context) {
  const nrhi::Format fmt = context.guest_output->format();
  if (g_r.gpfx_pso[6] != nullptr && g_r.gpfx_format == fmt) {
    return true;
  }
  if (g_r.gpfx_failed) {
    return false;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: game postfx pipeline setup failed ({})", what);
    g_r.gpfx_failed = true;
    return false;
  };
  for (nrhi::Pipeline*& p : g_r.gpfx_pso) {
    if (p != nullptr) {
      device->DestroyDeferred(p);
      p = nullptr;
    }
  }
  const char* const ps_names[7] = {
      "ps_bloom_scene",             "ps_bloom_dof_tap4_minusthreshold",
      "ps_bloom_dof_tap4",          "ps_bloom_dof_tap9",
      "ps_bloom_dof_tap9_combine3", "ps_bloom_dof_bloom_ghost",
      "ps_postfx_fast_uber"};
  nrhi::Shader* vs = device->CreateShader(MakeShaderDesc(
      nrhi::ShaderStage::kVertex, "photo_fx.hlsl", kPhotoFxShaderSource, "vs_raw", nullptr, ""));
  if (vs == nullptr) {
    return fail("vs_raw");
  }
  for (int i = 0; i < 7; ++i) {
    nrhi::Shader* ps = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "photo_fx.hlsl", kPhotoFxShaderSource, ps_names[i], nullptr,
        ""));
    if (ps == nullptr) {
      device->DestroyDeferred(vs);
      return fail(ps_names[i]);
    }
    nrhi::GraphicsPipelineDesc pso;
    pso.layout = g_r.pfx_layout;
    pso.vs = vs;
    pso.ps = ps;
    pso.cull = nrhi::CullMode::kNone;
    pso.depth_clip = true;
    pso.rtv_format = i == 6 ? fmt : nrhi::Format::kR16G16B16A16_FLOAT;
    pso.sample_count = 1;
    g_r.gpfx_pso[i] = device->CreateGraphicsPipeline(pso);
    device->DestroyDeferred(ps);
    if (g_r.gpfx_pso[i] == nullptr) {
      device->DestroyDeferred(vs);
      return fail(ps_names[i]);
    }
  }
  device->DestroyDeferred(vs);
  // The game-sized chain targets (fixed; the output-sized copy is (re)built
  // by ApplyGamePostfx on size change).
  const auto make = [&](uint32_t w, uint32_t h, nrhi::Texture** tex,
                        nrhi::TextureView** srv) -> bool {
    if (*tex != nullptr) {
      return true;
    }
    nrhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.mip_levels = 1;
    desc.format = nrhi::Format::kR16G16B16A16_FLOAT;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    *tex = device->CreateTexture(desc);
    if (*tex == nullptr) {
      return false;
    }
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    *srv = device->CreateTextureView(*tex, vd);
    return *srv != nullptr;
  };
  if (!make(1152, 640, &g_r.gpfx_scene, &g_r.gpfx_scene_srv) ||
      !make(288, 160, &g_r.gpfx_a[0], &g_r.gpfx_a_srv[0]) ||
      !make(288, 160, &g_r.gpfx_a[1], &g_r.gpfx_a_srv[1]) ||
      !make(72, 40, &g_r.gpfx_b[0], &g_r.gpfx_b_srv[0]) ||
      !make(72, 40, &g_r.gpfx_b[1], &g_r.gpfx_b_srv[1])) {
    return fail("targets");
  }
  g_r.gpfx_format = fmt;
  REXLOG_INFO("native-scene: game postfx pipelines ready (bloom_dof chain + fast_uber)");
  return true;
}
}  // namespace

// composite = false: the frame runs the game's cinematic chain instead of
// postfx_fast_uber (FrameScene::PhotoFx::gameplay): run the bloom chain
// only and hand its result (left in PIXEL_SHADER_RESOURCE; the caller
// returns it to RENDER_TARGET) to the chain's visualfx accumulation input.
void ApplyGamePostfx(const NativeGuestOutputRenderContext& context, nrhi::Cmd* cmd,
                     const FrameScene& scene, const nrhi::Viewport& viewport,
                     const nrhi::Rect& scissor, uint64_t frame_number,
                     bool composite = true, nrhi::Texture** bloom_tex_out = nullptr,
                     nrhi::TextureView** bloom_srv_out = nullptr) {
  if (g_r.output_srv_slot == nullptr || g_r.pso_blur_blit == nullptr ||
      !EnsurePhotoFxPipeline(context) || !EnsureGamePostfxPipeline(context)) {
    return;
  }
  nrhi::Device* device = context.device;
  const uint32_t ow = context.guest_output_width;
  const uint32_t oh = context.guest_output_height;
  if (g_r.gpfx_src == nullptr || g_r.gpfx_src_w != ow || g_r.gpfx_src_h != oh ||
      g_r.gpfx_src->format() != context.guest_output->format()) {
    if (g_r.gpfx_src_srv != nullptr) {
      device->DestroyDeferred(g_r.gpfx_src_srv);
      g_r.gpfx_src_srv = nullptr;
    }
    if (g_r.gpfx_src != nullptr) {
      device->DestroyDeferred(g_r.gpfx_src);
      g_r.gpfx_src = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = ow;
    desc.height = oh;
    desc.mip_levels = 1;
    desc.format = context.guest_output->format();
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.gpfx_src = device->CreateTexture(desc);
    if (g_r.gpfx_src != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.gpfx_src_srv = device->CreateTextureView(g_r.gpfx_src, vd);
    }
    g_r.gpfx_src_w = g_r.gpfx_src_srv != nullptr ? ow : 0;
    g_r.gpfx_src_h = g_r.gpfx_src_srv != nullptr ? oh : 0;
    if (g_r.gpfx_src_srv == nullptr) {
      return;
    }
  }
  const auto to_srv = [&](nrhi::Texture* t) {
    cmd->Barrier(t, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
  };
  const auto to_rt = [&](nrhi::Texture* t) {
    cmd->Barrier(t, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
  };
  // 1) Scene target -> gpfx_src through the main layout's SINGLE-view table
  //    (output_srv_slot is a new view each frame; see the lens pass).
  to_srv(context.guest_output);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.gpfx_src, nullptr);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->SetPipeline(g_r.pso_blur_blit);
  cmd->SetTexture(1, g_r.output_srv_slot);
  cmd->Draw(3, 0);
  to_srv(g_r.gpfx_src);
  to_rt(context.guest_output);
  cmd->FlushBarriers();

  nrhi::TextureView* const W = g_r.white.srv;
  const uint32_t slot_base = uint32_t(frame_number % 4) * 32 + 16;
  uint32_t slot_next = 0;
  cmd->SetBindingLayout(g_r.pfx_layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // One pass: the game's rows c0..c10 (or just c0.x = flag), destination
  // size, target, PSO, t0/t1.
  const auto pass = [&](int pso, const float (*rows)[4], nrhi::Texture* target,
                        uint32_t w, uint32_t h, nrhi::TextureView* in0,
                        nrhi::TextureView* in1, float flag) {
    const uint32_t slot = slot_base + (slot_next++);
    uint8_t* dst = g_r.pfx_cb_ptr + size_t(slot) * 4096;
    std::memset(dst, 0, 4096);
    float* c = reinterpret_cast<float*>(dst);
    if (rows != nullptr) {
      std::memcpy(c, rows, 11 * 4 * sizeof(float));
    } else {
      c[0] = flag;
    }
    c[248 * 4 + 0] = float(w);
    c[248 * 4 + 1] = float(h);
    c[248 * 4 + 2] = 0.5f / float(w);
    c[248 * 4 + 3] = 0.5f / float(h);
    const nrhi::Viewport vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    const nrhi::Rect sc{0, 0, int32_t(w), int32_t(h)};
    cmd->SetRenderTargets(target, nullptr);
    cmd->SetViewport(vp);
    cmd->SetScissor(sc);
    cmd->SetPipeline(g_r.gpfx_pso[pso]);
    cmd->SetConstantBuffer(0, g_r.pfx_cb, uint64_t(slot) * 4096);
    nrhi::TextureView* views[8] = {in0, in1, W, W, g_r.pfx_srv[6], W, W, W};
    cmd->SetTextures(1, views, 8);
    cmd->Draw(3, 0);
  };
  using GB = FrameScene::GameBloom;
  const bool bloom = scene.game_bloom.valid;
  // Ping-pong state: which target of each level holds the current result,
  // and which targets sit in SRV state (all start in RENDER_TARGET).
  bool in_srv[4] = {};  // a0, a1, b0, b1
  nrhi::Texture* const tex4[4] = {g_r.gpfx_a[0], g_r.gpfx_a[1], g_r.gpfx_b[0], g_r.gpfx_b[1]};
  nrhi::TextureView* const srv4[4] = {g_r.gpfx_a_srv[0], g_r.gpfx_a_srv[1],
                                      g_r.gpfx_b_srv[0], g_r.gpfx_b_srv[1]};
  const auto as_rt = [&](int k) {
    if (in_srv[k]) {
      to_rt(tex4[k]);
      in_srv[k] = false;
    }
  };
  const auto as_srv = [&](int k) {
    if (!in_srv[k]) {
      to_srv(tex4[k]);
      in_srv[k] = true;
    }
  };
  int a_cur = 0;  // index into tex4 (0/1 = level A, 2/3 = level B)
  int b_cur = 2;
  if (bloom) {
    const auto& r = scene.game_bloom.rows;
    const uint32_t passes = scene.game_bloom.passes;
    const auto ran = [&](int p) { return ((passes >> p) & 1u) != 0; };
    // 2) The game's 1152x640 scene.
    pass(0, nullptr, g_r.gpfx_scene, 1152, 640, g_r.gpfx_src_srv, W, 0.0f);
    to_srv(g_r.gpfx_scene);
    cmd->FlushBarriers();
    // 3) bloom_dof_tap4_minusthreshold: scene -> A (288x160).
    pass(1, r[GB::kThreshold], tex4[0], 288, 160, g_r.gpfx_scene_srv, W, 0.0f);
    as_srv(0);
    cmd->FlushBarriers();
    a_cur = 0;
    // 4) bloom_dof_tap4: A -> B (72x40).
    pass(2, r[GB::kDownB], tex4[2], 72, 40, srv4[0], W, 0.0f);
    as_srv(2);
    cmd->FlushBarriers();
    b_cur = 2;
    // One pass into the other target of a level; a pass the game skipped
    // this frame leaves the level's current result as it is.
    const auto level_pass = [&](int pso, int p, int& cur, uint32_t w, uint32_t h,
                                nrhi::TextureView* in1) {
      const int dst = cur ^ 1;
      as_rt(dst);
      cmd->FlushBarriers();
      pass(pso, r[p], tex4[dst], w, h, srv4[cur], in1, 0.0f);
      as_srv(dst);
      cmd->FlushBarriers();
      cur = dst;
    };
    // 5/6) bloom_dof_tap9 blurs (horizontal on A, horizontal + vertical on B).
    if (ran(GB::kBlurAH)) level_pass(3, GB::kBlurAH, a_cur, 288, 160, W);
    if (ran(GB::kBlurBH)) level_pass(3, GB::kBlurBH, b_cur, 72, 40, W);
    if (ran(GB::kBlurBV)) level_pass(3, GB::kBlurBV, b_cur, 72, 40, W);
    // 7) bloom_dof_tap9_combine3: A (+ B as the next level) -> A.
    level_pass(4, GB::kCombine, a_cur, 288, 160, srv4[b_cur]);
    // 8) bloom_dof_bloom_ghost (the lens flare): A -> A = the bloom.
    if (ran(GB::kGhost)) level_pass(5, GB::kGhost, a_cur, 288, 160, W);
  }
  // 9) postfx_fast_uber: scene * 1.41 + bloom -> the output.
  if (composite) {
    pass(6, nullptr, context.guest_output, context.guest_output_width,
         context.guest_output_height, g_r.gpfx_src_srv, bloom ? srv4[a_cur] : W,
         bloom ? 1.0f : 0.0f);
  }
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  // Restore the steady states (all RENDER_TARGET), except a bloom result
  // handed to the cinematic chain.
  to_rt(g_r.gpfx_src);
  if (bloom) {
    to_rt(g_r.gpfx_scene);
  }
  const bool hand_off = !composite && bloom && bloom_tex_out != nullptr &&
                        bloom_srv_out != nullptr;
  for (int k = 0; k < 4; ++k) {
    if (hand_off && k == a_cur) {
      continue;
    }
    as_rt(k);
  }
  if (hand_off) {
    as_srv(a_cur);
    *bloom_tex_out = tex4[a_cur];
    *bloom_srv_out = srv4[a_cur];
  }
  cmd->FlushBarriers();
  if (!composite) {
    return;
  }
  static bool s_logged[2] = {};
  if (!s_logged[bloom ? 1 : 0]) {
    s_logged[bloom ? 1 : 0] = true;
    REXLOG_INFO("native-scene: game postfx LIVE (fast_uber composite, bloom {})",
                bloom ? "on" : "off");
  }
}

// Shadow atlas targets + the always-bound b1 receiver constant buffer.
bool EnsureShadowResources(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  // Cascade tile size: explicit, or auto = the game's 512 tiles at the
  // RENDER resolution scale. guest_output is the scaled frontbuffer (720p x
  // the draw resolution scale, independent of window/monitor size), so
  // deriving from its height follows the Resolution Scale setting, including
  // any device-limit clamping the texture cache applied, and gives the same
  // effective shadow raster the emulated GPU renders at that scale.
  const int32_t tile_cfg = REXCVAR_GET(skate3_native_render_scene_shadow_tile);
  uint32_t want_tile =
      tile_cfg > 0
          ? uint32_t(tile_cfg)
          : std::min(512u * std::max(1u, (context.guest_output_height + 719u) /
                                             720u),
                     4096u);
  // Settle gate. Dragging a window fires a size update (and a swapchain
  // resize) every few milliseconds, and the derived tile size flips
  // 512 <-> 1024 as the output crosses the 720-line boundary. Each flip
  // retires the atlas chain and re-renders every caster plus the blur
  // passes; stacking dozens of those on top of the driver's own resize work
  // hangs a slow adapter -- captured on a GTX 960M as
  // "D3D12 device removed: 0x887A0006 DEVICE_HUNG (TDR)" 150 ms after the
  // last of ~40 atlas rebuilds in one drag, with VRAM at 765MB of a 3445MB
  // budget (so: command length, not memory). Take a new size only once the
  // output has held it for a few frames; until then keep rendering with the
  // atlas we already have, which costs nothing but a slightly stale tile
  // resolution for a fraction of a second. The first creation is never
  // delayed.
  if (g_r.shadow_raw != nullptr && want_tile != g_r.shadow_tile) {
    static uint32_t s_pending_tile = 0;
    static uint32_t s_pending_frames = 0;
    if (want_tile != s_pending_tile) {
      s_pending_tile = want_tile;
      s_pending_frames = 0;
    }
    if (++s_pending_frames < 12) {
      want_tile = g_r.shadow_tile;
    }
  }
  if (g_r.shadow_raw != nullptr && g_r.shadow_tile != want_tile) {
    // Hot tile-size change: retire the atlas chain; recreated below. The
    // new atlas starts in RENDER_TARGET state and is re-rendered by this
    // frame's shadow pass.
    nrhi::Texture** targets[3] = {&g_r.shadow_raw, &g_r.shadow_mid,
                                  &g_r.shadow_final};
    nrhi::TextureView** views[3] = {&g_r.shadow_srv_raw, &g_r.shadow_srv_mid,
                                    &g_r.shadow_srv_final};
    for (int t = 0; t < 3; ++t) {
      if (*views[t] != nullptr) {
        device->DestroyDeferred(*views[t]);
        *views[t] = nullptr;
      }
      if (*targets[t] != nullptr) {
        device->DestroyDeferred(*targets[t]);
        *targets[t] = nullptr;
      }
    }
  }
  if (!g_r.shadow_raw && REXCVAR_GET(skate3_native_render_scene_shadows)) {
    // Dynamic-shadow atlas chain: raw casters -> hblur intermediate ->
    // blurred final (the texture the scene pass samples). Three fixed-size
    // R16G16_UNORM targets (the game's atlas is fmt 25 = 16_16 fixed point;
    // half-float ulp at the typical ~0.85 depth is ~6 mm of world height,
    // too coarse for board/feet-height casters 1-2 cm off the ground),
    // 3 tiles of tile x tile each.
    g_r.shadow_tile = want_tile;
    nrhi::TextureDesc desc;
    desc.width = g_r.shadow_tile * 3;
    desc.height = g_r.shadow_tile;
    desc.format = nrhi::Format::kR16G16_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far
    desc.clear_color[1] = 1.0f;  // "uncoverage": empty
    nrhi::Texture** targets[3] = {&g_r.shadow_raw, &g_r.shadow_mid,
                                  &g_r.shadow_final};
    nrhi::TextureView** views[3] = {&g_r.shadow_srv_raw, &g_r.shadow_srv_mid,
                                    &g_r.shadow_srv_final};
    for (int t = 0; t < 3; ++t) {
      // The raw and final tiles are the shadow-dump readback sources, so
      // those two also need copy-source usage; the intermediate is never
      // copied and stays render-target only.
      desc.usage = nrhi::kTextureUsageRenderTarget |
                   (t != 1 ? nrhi::kTextureUsageCopySource
                           : nrhi::kTextureUsageNone);
      *targets[t] = device->CreateTexture(desc);
      if (*targets[t] == nullptr) {
        REXLOG_ERROR("native-scene: shadow atlas creation failed");
        g_r.failed = true;
        return false;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      *views[t] = device->CreateTextureView(*targets[t], vd);
      if (*views[t] == nullptr) {
        REXLOG_ERROR("native-scene: shadow atlas creation failed");
        g_r.failed = true;
        return false;
      }
    }
    g_r.shadow_in_srv_state = false;
    REXLOG_INFO("native-scene: shadow atlas created ({}x{} tiles{})",
                g_r.shadow_tile, g_r.shadow_tile,
                tile_cfg > 0 ? "" : ", auto");
  }
  if (!g_r.world_shadow && g_r.shadow_raw != nullptr) {
    // dynamicobject static world-shadow map (see RendererState). Fixed at
    // the game's own 512x512: a coarse region-scale baked-shade map whose
    // PCF footprint the props' shading was tuned against.
    nrhi::TextureDesc desc;
    desc.width = RendererState::kWorldShadowSize;
    desc.height = RendererState::kWorldShadowSize;
    desc.format = nrhi::Format::kR32G32_FLOAT;  // see RendererState::pso_ws_caster
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far = lit
    desc.clear_color[1] = 1.0f;
    g_r.world_shadow = device->CreateTexture(desc);
    if (g_r.world_shadow != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.world_shadow_srv = device->CreateTextureView(g_r.world_shadow, vd);
    }
    if (g_r.world_shadow_srv == nullptr) {
      REXLOG_ERROR("native-scene: world-shadow map creation failed");
      g_r.failed = true;
      return false;
    }
    g_r.world_shadow_in_srv = false;
    g_r.world_shadow_primed = false;
  }
  // The park map's tile follows the render resolution scale like the
  // cascade atlas's auto tile: the emulated GPU rasterizes the game's
  // 1024x512 target at 512 x scale per tile, which both sharpens the
  // shadows and -- through the halved derivatives -- makes the alphatest
  // casters' cutout fetch one mip finer (at 512 the coarser mip averaged
  // the tree leaves below the 0.12 kill threshold: thinned foliage).
  {
    const uint32_t want_park_tile = std::min(
        512u * std::max(1u, (context.guest_output_height + 719u) / 720u), 2048u);
    if (g_r.park_ws != nullptr && want_park_tile != g_r.park_ws_tile) {
      for (nrhi::TextureView** v : {&g_r.park_ws_srv, &g_r.park_ws_tmp_srv}) {
        if (*v != nullptr) {
          device->DestroyDeferred(*v);
          *v = nullptr;
        }
      }
      for (nrhi::Texture** t : {&g_r.park_ws, &g_r.park_ws_tmp}) {
        if (*t != nullptr) {
          device->DestroyDeferred(*t);
          *t = nullptr;
        }
      }
      g_r.park_ws_built_valid = false;
    }
    if (g_r.park_ws == nullptr) {
      g_r.park_ws_tile = want_park_tile;
    }
  }
  if (!g_r.park_ws && g_r.shadow_raw != nullptr) {
    // The game's skate.Park world-shadow map (see RendererState::park_ws):
    // its own 1024x512, two tiles.
    nrhi::TextureDesc desc;
    desc.width = g_r.park_ws_tile * 2;
    desc.height = g_r.park_ws_tile;
    desc.format = nrhi::Format::kR32G32_FLOAT;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far = lit
    desc.clear_color[1] = 1.0f;
    g_r.park_ws = device->CreateTexture(desc);
    if (g_r.park_ws != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.park_ws_srv = device->CreateTextureView(g_r.park_ws, vd);
    }
    g_r.park_ws_tmp = device->CreateTexture(desc);
    if (g_r.park_ws_tmp != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.park_ws_tmp_srv = device->CreateTextureView(g_r.park_ws_tmp, vd);
    }
    if (g_r.park_ws_srv == nullptr) {
      // Not fatal: park receivers fall back to a lit world term.
      REXLOG_ERROR("native-scene: park world-shadow map creation failed");
    }
    g_r.park_ws_in_srv = false;
    g_r.park_ws_primed = false;
  }
  uint32_t want_static_size = uint32_t(std::clamp(
      REXCVAR_GET(skate3_native_render_scene_shadow_static_size), 1024, 8192));
  if (device->backend() == nrhi::Backend::kD3D12) {
    // The 3-tile cascade row must fit D3D12's 16384 2D-texture width cap:
    // per-tile sizes above 5461 cannot allocate there (the 8192 setting
    // asks for 24576x8192), while desktop Vulkan allows 32768-wide.
    while (want_static_size * 3 > 16384) {
      want_static_size /= 2;
    }
  }
  if (g_r.static_sun != nullptr && g_r.static_sun_requested != want_static_size) {
    // Hot size change: retire the map; recreated below. The recreated map
    // is uninitialized, so force the cross-frame cache to re-render it
    // (RenderStaticSunMap rebuilds whenever nsm_built_radius <= 0).
    if (g_r.static_sun_srv != nullptr) {
      device->DestroyDeferred(g_r.static_sun_srv);
      g_r.static_sun_srv = nullptr;
    }
    device->DestroyDeferred(g_r.static_sun);
    g_r.static_sun = nullptr;
    g_r.nsm_built_radius = 0.0f;
  }
  if (!g_r.static_sun && g_r.shadow_raw != nullptr &&
      REXCVAR_GET(skate3_native_render_scene_shadow_static_casters)) {
    // Native static sun-shadow map (see RendererState). THREE cascade
    // tiles side by side: inner (r/6, centimeter contact detail with
    // useful reach), mid (r/2) and far (full radius, large-caster
    // coverage); size is per tile.
    g_r.static_sun_requested = want_static_size;
    g_r.static_sun_size = want_static_size;
    nrhi::TextureDesc desc;
    desc.width = g_r.static_sun_size * 3;
    desc.height = g_r.static_sun_size;
    desc.format = nrhi::Format::kR16G16_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.clear_color[0] = 1.0f;  // depth: far = lit
    desc.clear_color[1] = 1.0f;
    g_r.static_sun = device->CreateTexture(desc);
    // Allocation-failure fallback (VRAM pressure, driver limits): a
    // coarser static sun map beats marking the whole renderer failed and
    // dropping the session to emulated output.
    while (g_r.static_sun == nullptr && g_r.static_sun_size > 1024) {
      g_r.static_sun_size /= 2;
      desc.width = g_r.static_sun_size * 3;
      desc.height = g_r.static_sun_size;
      REXLOG_WARN(
          "native-scene: static sun-shadow map allocation failed, retrying "
          "at {}x{}",
          g_r.static_sun_size * 3, g_r.static_sun_size);
      g_r.static_sun = device->CreateTexture(desc);
    }
    if (g_r.static_sun != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.static_sun_srv = device->CreateTextureView(g_r.static_sun, vd);
    }
    if (g_r.static_sun_srv == nullptr) {
      REXLOG_ERROR("native-scene: static sun-shadow map creation failed");
      g_r.failed = true;
      return false;
    }
    g_r.static_sun_in_srv = false;
    REXLOG_INFO(
        "native-scene: static sun-shadow map created ({}x{}, 3 cascades)",
        g_r.static_sun_size * 3, g_r.static_sun_size);
  }
  if (!g_r.shadow_cb) {
    // Always created (even with shadows off): the scene PS declares b1 and
    // a root CBV must be bound; a zeroed block disables the shadow branch.
    g_r.shadow_cb = CreateUploadBuffer(
        device, size_t(RendererState::kShadowCbSlice) * RendererState::kShadowCbRegions);
    g_r.shadow_cb_cpu =
        g_r.shadow_cb ? static_cast<uint8_t*>(device->Map(g_r.shadow_cb))
                      : nullptr;
    if (g_r.shadow_cb_cpu == nullptr) {
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// ---- Auto-exposure measurement --------------------------------------------
// The game adapts exposure from a 32x16 luminance target its postfx chain
// renders (draws 1463-1465) and PostProcessor::sub_82978630 CPU-averages the
// next frames. Natively those draws never execute: the target stayed zero,
// the evaluator read black and exposure sat on its clamp (hence the manual
// debug-menu exposure). This rebuilds the measurement from the finished
// native frame (blur.hlsl ps_lum_first / ps_lum_down) and publishes the
// alpha plane; the evaluator hook writes it into the guest target.
std::mutex g_lum_mutex;
uint8_t g_lum_latest[32 * 16] = {};
int64_t g_lum_latest_ns = -1;

bool LatestExposureMeasurement(uint8_t out[32 * 16], int64_t* out_ns) {
  std::lock_guard<std::mutex> lock(g_lum_mutex);
  if (g_lum_latest_ns < 0) {
    return false;
  }
  std::memcpy(out, g_lum_latest, sizeof(g_lum_latest));
  *out_ns = g_lum_latest_ns;
  return true;
}

// Records the 3-pass chain over the finished output (guest_output in
// RENDER_TARGET at entry and exit) and queues a never-waited readback of
// the 32x16 result; publishes the newest completed slot. Failures disable
// only this feature.
void ApplyExposureMeasure(const NativeGuestOutputRenderContext& context,
                          nrhi::Cmd* cmd) {
  if (g_r.lum_failed || g_r.output_srv_slot == nullptr) {
    return;
  }
  nrhi::Device* device = context.device;
  // Consume: publish the newest fence-completed slot, retire completed ones.
  {
    const uint64_t done = device->CompletedSubmission();
    int newest = -1;
    for (int i = 0; i < 2; ++i) {
      if (g_r.lum_pending[i] && g_r.lum_submission[i] < done &&
          (newest < 0 || g_r.lum_submission[i] > g_r.lum_submission[newest])) {
        newest = i;
      }
    }
    if (newest >= 0) {
      device->InvalidateForRead(g_r.lum_readback[newest], 0,
                                uint64_t(RendererState::kLumRowPitch) *
                                    RendererState::kLumH[2]);
      std::lock_guard<std::mutex> lock(g_lum_mutex);
      for (uint32_t y = 0; y < RendererState::kLumH[2]; ++y) {
        const uint8_t* row = g_r.lum_readback_ptr[newest] +
                             size_t(y) * RendererState::kLumRowPitch;
        for (uint32_t x = 0; x < RendererState::kLumW[2]; ++x) {
          g_lum_latest[y * RendererState::kLumW[2] + x] = row[x * 4 + 3];  // A
        }
      }
      g_lum_latest_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    }
    for (int i = 0; i < 2; ++i) {
      if (g_r.lum_pending[i] && g_r.lum_submission[i] < done) {
        g_r.lum_pending[i] = false;
      }
    }
  }
  if (g_r.pso_lum_first == nullptr) {
    nrhi::Shader* vs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "blur.hlsl", kBlurShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* ps0 = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource, "ps_lum_first", nullptr, ""));
    nrhi::Shader* ps1 = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource, "ps_lum_down", nullptr, ""));
    if (vs != nullptr && ps0 != nullptr && ps1 != nullptr) {
      nrhi::GraphicsPipelineDesc gp;
      gp.layout = g_r.layout;
      gp.vs = vs;
      gp.ps = ps0;
      gp.cull = nrhi::CullMode::kNone;
      gp.rtv_format = nrhi::Format::kR8G8B8A8_UNORM;  // the game's 8888 targets
      gp.sample_count = 1;
      g_r.pso_lum_first = device->CreateGraphicsPipeline(gp);
      gp.ps = ps1;
      g_r.pso_lum_down = device->CreateGraphicsPipeline(gp);
    }
    if (vs) device->DestroyDeferred(vs);
    if (ps0) device->DestroyDeferred(ps0);
    if (ps1) device->DestroyDeferred(ps1);
    if (g_r.pso_lum_first == nullptr || g_r.pso_lum_down == nullptr) {
      g_r.lum_failed = true;
      REXLOG_WARN("native-scene: exposure-measurement PSO creation failed - "
                  "auto exposure keeps the game's unmeasured behavior");
      return;
    }
  }
  if (g_r.lum_tex[0] == nullptr) {
    for (int i = 0; i < 3; ++i) {
      nrhi::TextureDesc td;
      td.width = RendererState::kLumW[i];
      td.height = RendererState::kLumH[i];
      td.mip_levels = 1;
      td.format = nrhi::Format::kR8G8B8A8_UNORM;
      // The last level is read back every frame: copy-source usage is
      // required for the Vulkan backend's TRANSFER_SRC (see the occlusion
      // grid).
      td.usage = nrhi::kTextureUsageRenderTarget |
                 (i == 2 ? nrhi::kTextureUsageCopySource : 0u);
      td.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.lum_tex[i] = device->CreateTexture(td);
      if (g_r.lum_tex[i] != nullptr) {
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        g_r.lum_srv[i] = device->CreateTextureView(g_r.lum_tex[i], vd);
      }
      if (g_r.lum_tex[i] == nullptr || g_r.lum_srv[i] == nullptr) {
        g_r.lum_failed = true;
      }
    }
    nrhi::BufferDesc bd;
    bd.size = uint64_t(RendererState::kLumRowPitch) * RendererState::kLumH[2];
    bd.heap = nrhi::HeapKind::kReadback;
    for (int i = 0; i < 2 && !g_r.lum_failed; ++i) {
      g_r.lum_readback[i] = device->CreateBuffer(bd);
      if (g_r.lum_readback[i] == nullptr ||
          (g_r.lum_readback_ptr[i] =
               static_cast<uint8_t*>(device->Map(g_r.lum_readback[i]))) == nullptr) {
        g_r.lum_failed = true;
      }
    }
    if (g_r.lum_failed) {
      REXLOG_WARN("native-scene: exposure-measurement targets unavailable - "
                  "auto exposure keeps the game's unmeasured behavior");
      return;
    }
  }
  // Produce into the free slot; skip the frame (never wait) when both are
  // still in flight.
  const int w = g_r.lum_write_index;
  if (g_r.lum_pending[w]) {
    return;
  }
  const float out_aspect =
      float(context.guest_output_width) / float(context.guest_output_height);
  const float wide = out_aspect > (16.0f / 9.0f) * 1.01f ? (16.0f / 9.0f) / out_aspect : 1.0f;
  const float tall = out_aspect < (16.0f / 9.0f) * 0.99f ? out_aspect / (16.0f / 9.0f) : 1.0f;
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  for (int pass = 0; pass < 3; ++pass) {
    const uint32_t tw = RendererState::kLumW[pass];
    const uint32_t th = RendererState::kLumH[pass];
    cmd->SetRenderTargets(g_r.lum_tex[pass], nullptr);
    cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(tw), float(th), 0.0f, 1.0f});
    cmd->SetScissor(nrhi::Rect{0, 0, int32_t(tw), int32_t(th)});
    if (pass == 0) {
      const float c[8] = {1.0f / float(context.guest_output_width),
                          1.0f / float(context.guest_output_height),
                          0.0f,
                          0.0f,
                          wide,
                          tall,
                          (1.0f - wide) * 0.5f,
                          (1.0f - tall) * 0.5f};
      cmd->SetPipeline(g_r.pso_lum_first);
      cmd->SetRootConstants(0, 8, c, 0);
      cmd->SetTexture(1, g_r.output_srv_slot);
    } else {
      const float c[8] = {1.0f / float(RendererState::kLumW[pass - 1]),
                          1.0f / float(RendererState::kLumH[pass - 1]),
                          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
      cmd->SetPipeline(g_r.pso_lum_down);
      cmd->SetRootConstants(0, 8, c, 0);
      cmd->SetTexture(1, g_r.lum_srv[pass - 1]);
    }
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.lum_tex[pass], nrhi::ResourceState::kRenderTarget,
                 pass == 2 ? nrhi::ResourceState::kCopySource
                           : nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
  }
  cmd->CopyTextureToBuffer(g_r.lum_readback[w], 0, RendererState::kLumRowPitch,
                           g_r.lum_tex[2], 0, RendererState::kLumW[2],
                           RendererState::kLumH[2]);
  // Restore steady states (everything idles in RENDER_TARGET).
  cmd->Barrier(g_r.lum_tex[0], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.lum_tex[1], nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(g_r.lum_tex[2], nrhi::ResourceState::kCopySource,
               nrhi::ResourceState::kRenderTarget);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  g_r.lum_submission[w] = device->CurrentSubmission();
  g_r.lum_pending[w] = true;
  g_r.lum_write_index = 1 - w;
}

// Guest display gamma ramp over the finished native frame. The emulated
// presenter applies the game's 256-entry DC_LUT_30_COLOR table to every
// frame (apply_gamma_table, D3D::InitializePresentationParameters sets it up
// for 8_8_8_8 output) but TryRenderNativeGuestOutput returns before that
// pass, so native frames showed the raw framebuffer: every tone differed
// slightly from the game (first noticed on the flat debug-menu bars). Runs
// last, over scene + 2D + blur, exactly where the presenter would.
void ApplyGuestGammaRamp(const NativeGuestOutputRenderContext& context, nrhi::Cmd* cmd) {
  if (g_r.gamma_failed || g_r.output_srv_slot == nullptr || g_r.pso_blur_blit == nullptr) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  auto* gs = runtime != nullptr
                 ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system())
                 : nullptr;
  auto* cp = gs != nullptr ? gs->command_processor() : nullptr;
  if (cp == nullptr) {
    return;
  }
  nrhi::Device* device = context.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: guest gamma ramp pass unavailable ({})", what);
    g_r.gamma_failed = true;
  };
  if (g_r.pso_gamma == nullptr) {
    nrhi::Shader* vs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "blur.hlsl", kBlurShaderSource, "vs_main", nullptr, ""));
    nrhi::Shader* ps = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource, "ps_gamma", nullptr, ""));
    if (vs != nullptr && ps != nullptr) {
      nrhi::GraphicsPipelineDesc gp;
      gp.layout = g_r.layout;
      gp.vs = vs;
      gp.ps = ps;
      gp.cull = nrhi::CullMode::kNone;
      gp.rtv_format = context.guest_output->format();
      gp.sample_count = 1;
      g_r.pso_gamma = device->CreateGraphicsPipeline(gp);
    }
    if (vs) device->DestroyDeferred(vs);
    if (ps) device->DestroyDeferred(ps);
    if (g_r.pso_gamma == nullptr) {
      fail("pso");
      return;
    }
  }
  if (g_r.gamma_lut_tex == nullptr) {
    nrhi::TextureDesc desc;
    desc.width = 256;
    desc.height = 1;
    desc.format = nrhi::Format::kR10G10B10A2_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    g_r.gamma_lut_tex = device->CreateTexture(desc);
    g_r.gamma_lut_upload = g_r.gamma_lut_tex != nullptr
                               ? CreateUploadBuffer(device, 256 * 4,
                                                    nrhi::BufferBindClass::kCopySrc)
                               : nullptr;
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.gamma_lut_srv =
        g_r.gamma_lut_tex != nullptr ? device->CreateTextureView(g_r.gamma_lut_tex, vd) : nullptr;
    if (g_r.gamma_lut_tex == nullptr || g_r.gamma_lut_upload == nullptr ||
        g_r.gamma_lut_srv == nullptr) {
      fail("ramp texture");
      return;
    }
    g_r.gamma_lut_copy_dest = true;
  }
  const uint32_t w = context.guest_output_width;
  const uint32_t h = context.guest_output_height;
  const nrhi::Format fmt = context.guest_output->format();
  if (g_r.gamma_tex == nullptr || g_r.gamma_w != w || g_r.gamma_h != h ||
      g_r.gamma_fmt != fmt) {
    if (g_r.gamma_srv) device->DestroyDeferred(g_r.gamma_srv);
    if (g_r.gamma_tex) device->DestroyDeferred(g_r.gamma_tex);
    g_r.gamma_srv = nullptr;
    nrhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = fmt;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.gamma_tex = device->CreateTexture(desc);
    if (g_r.gamma_tex != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.gamma_srv = device->CreateTextureView(g_r.gamma_tex, vd);
    }
    if (g_r.gamma_tex == nullptr || g_r.gamma_srv == nullptr) {
      fail("intermediate");
      return;
    }
    g_r.gamma_w = w;
    g_r.gamma_h = h;
    g_r.gamma_fmt = fmt;
  }
  // Re-upload whenever the game's table changes (brightness setting).
  // CommandProcessor::gamma_ramp_256_entry_table() is protected; a
  // never-instantiated derived helper hands out the base-member pointer
  // (legal access, no SDK change).
  struct GammaRampAccess : rex::graphics::CommandProcessor {
    static auto Getter() { return &GammaRampAccess::gamma_ramp_256_entry_table; }
  };
  const rex::graphics::reg::DC_LUT_30_COLOR* ramp = (cp->*GammaRampAccess::Getter())();
  uint32_t values[256];
  for (int i = 0; i < 256; ++i) {
    values[i] = ramp[i].value;
  }
  if (!g_r.gamma_lut_valid ||
      std::memcmp(values, g_r.gamma_lut_cache, sizeof(values)) != 0) {
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(g_r.gamma_lut_upload));
    if (mapping == nullptr) {
      return;
    }
    std::memcpy(mapping, values, sizeof(values));
    device->Unmap(g_r.gamma_lut_upload);
    if (!g_r.gamma_lut_copy_dest) {
      cmd->Barrier(g_r.gamma_lut_tex, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kCopyDest);
      cmd->FlushBarriers();
    }
    cmd->CopyBufferToTexture(g_r.gamma_lut_tex, 0, 0, g_r.gamma_lut_upload, 0, 256 * 4, 256,
                             1, 1);
    cmd->Barrier(g_r.gamma_lut_tex, nrhi::ResourceState::kCopyDest,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    g_r.gamma_lut_copy_dest = false;
    std::memcpy(g_r.gamma_lut_cache, values, sizeof(values));
    g_r.gamma_lut_valid = true;
    static int s_logs = 0;
    if (s_logs < 4) {
      ++s_logs;
      const auto red = [&](int i) { return (values[i] >> 20) & 0x3FFu; };
      const auto green = [&](int i) { return (values[i] >> 10) & 0x3FFu; };
      const auto blue = [&](int i) { return values[i] & 0x3FFu; };
      REXLOG_INFO(
          "native-scene: guest gamma ramp uploaded (10-bit r/g/b): [0]={}/{}/{} "
          "[16]={}/{}/{} [64]={}/{}/{} [128]={}/{}/{} [192]={}/{}/{} [255]={}/{}/{}",
          red(0), green(0), blue(0), red(16), green(16), blue(16), red(64), green(64),
          blue(64), red(128), green(128), blue(128), red(192), green(192), blue(192),
          red(255), green(255), blue(255));
    }
  }
  const nrhi::Viewport vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const nrhi::Rect sc{0, 0, int32_t(w), int32_t(h)};
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  // output -> gamma_tex through the table.
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(g_r.gamma_tex, nullptr);
  cmd->SetViewport(vp);
  cmd->SetScissor(sc);
  cmd->SetPipeline(g_r.pso_gamma);
  cmd->SetTexture(1, g_r.output_srv_slot);
  cmd->SetTexture(2, g_r.gamma_lut_srv);
  cmd->Draw(3, 0);
  // gamma_tex -> output (1:1 blit).
  cmd->Barrier(g_r.gamma_tex, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->Barrier(context.guest_output, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
  cmd->SetRenderTargets(context.guest_output, nullptr);
  cmd->SetPipeline(g_r.pso_blur_blit);
  cmd->SetTexture(1, g_r.gamma_srv);
  cmd->Draw(3, 0);
  cmd->Barrier(g_r.gamma_tex, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
  cmd->FlushBarriers();
}

// Blur intermediates + the output-sized selection-outline mask. Failures
// only disable their feature, never abort the native path.
bool EnsureBlurOutlineTargets(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.blur_tex[0] == nullptr && g_r.pso_blur != nullptr) {
    // Popup background blur intermediates at the game's fixed 1152x640
    // internal resolution (the bilinear stretch back to the output is what
    // produces the authentic frosted-glass lattice).
    nrhi::TextureDesc desc;
    desc.width = RendererState::kBlurWidth;
    desc.height = RendererState::kBlurHeight;
    desc.format = context.guest_output->format();
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    bool ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      // blur_tex[0] is also the photo-grab readback source (copied into the
      // grab readback buffers), so it alone needs copy-source usage.
      desc.usage = nrhi::kTextureUsageRenderTarget |
                   (t == 0 ? nrhi::kTextureUsageCopySource
                           : nrhi::kTextureUsageNone);
      g_r.blur_tex[t] = device->CreateTexture(desc);
      if (g_r.blur_tex[t] == nullptr) {
        ok = false;
        break;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.blur_srv[t] = device->CreateTextureView(g_r.blur_tex[t], vd);
      if (g_r.blur_srv[t] == nullptr) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      REXLOG_ERROR("native-scene: blur intermediate creation failed; blur disabled");
      for (int t = 0; t < 2; ++t) {
        if (g_r.blur_srv[t]) device->DestroyDeferred(g_r.blur_srv[t]);
        g_r.blur_srv[t] = nullptr;
        if (g_r.blur_tex[t]) device->DestroyDeferred(g_r.blur_tex[t]);
        g_r.blur_tex[t] = nullptr;
      }
      g_r.pso_blur = nullptr;  // leaked PSO acceptable on this cold path
    }
  }

  // Settings-menu gaussian intermediates: half the output resolution
  // (sigma halves with it; the final bilinear stretch back is invisible
  // under a real blur), recreated whenever the output size changes.
  const uint32_t menu_w = std::max(1u, context.guest_output_width / 2);
  const uint32_t menu_h = std::max(1u, context.guest_output_height / 2);
  if ((g_r.menu_blur_tex[0] == nullptr || g_r.menu_blur_w != menu_w ||
       g_r.menu_blur_h != menu_h) &&
      g_r.pso_menu_gauss != nullptr) {
    for (int t = 0; t < 2; ++t) {
      if (g_r.menu_blur_srv[t]) device->DestroyDeferred(g_r.menu_blur_srv[t]);
      g_r.menu_blur_srv[t] = nullptr;
      if (g_r.menu_blur_tex[t]) device->DestroyDeferred(g_r.menu_blur_tex[t]);
      g_r.menu_blur_tex[t] = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = menu_w;
    desc.height = menu_h;
    desc.format = context.guest_output->format();
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    bool ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      g_r.menu_blur_tex[t] = device->CreateTexture(desc);
      if (g_r.menu_blur_tex[t] == nullptr) {
        ok = false;
        break;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.menu_blur_srv[t] = device->CreateTextureView(g_r.menu_blur_tex[t], vd);
      if (g_r.menu_blur_srv[t] == nullptr) {
        ok = false;
        break;
      }
    }
    if (ok) {
      g_r.menu_blur_w = menu_w;
      g_r.menu_blur_h = menu_h;
    } else {
      REXLOG_ERROR(
          "native-scene: menu blur intermediate creation failed; menu blur "
          "disabled");
      for (int t = 0; t < 2; ++t) {
        if (g_r.menu_blur_srv[t]) device->DestroyDeferred(g_r.menu_blur_srv[t]);
        g_r.menu_blur_srv[t] = nullptr;
        if (g_r.menu_blur_tex[t]) device->DestroyDeferred(g_r.menu_blur_tex[t]);
        g_r.menu_blur_tex[t] = nullptr;
      }
      g_r.pso_menu_gauss = nullptr;  // leaked PSO acceptable on this cold path
    }
  }

  if ((g_r.outline_mask == nullptr ||
       g_r.outline_mask_width != context.guest_output_width ||
       g_r.outline_mask_height != context.guest_output_height) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr) {
    // Selection-outline mask: single-sample R8 target at output resolution
    // (a 1152x640 mask left the contour centerline visibly stairstepped;
    // the mask's own rasterization aliasing survives any amount of
    // downstream filtering).
    if (g_r.outline_mask) {
      device->DestroyDeferred(g_r.outline_mask);
      g_r.outline_mask = nullptr;
    }
    nrhi::TextureDesc desc;
    desc.width = context.guest_output_width;
    desc.height = context.guest_output_height;
    desc.format = nrhi::Format::kR8_UNORM;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.outline_mask = device->CreateTexture(desc);
    if (g_r.outline_mask == nullptr) {
      REXLOG_ERROR("native-scene: outline mask creation failed; outline disabled");
      g_r.pso_outline_edge = nullptr;
    } else {
      g_r.outline_mask_width = context.guest_output_width;
      g_r.outline_mask_height = context.guest_output_height;
      // Re-point the mask view at the recreated texture.
      if (g_r.outline_mask_srv) {
        device->DestroyDeferred(g_r.outline_mask_srv);
        g_r.outline_mask_srv = nullptr;
      }
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_r.outline_mask_srv = device->CreateTextureView(g_r.outline_mask, vd);
      g_r.outline_mask_srv_allocated = g_r.outline_mask_srv != nullptr;
    }
  }
  return true;
}

// Replacement bitmap-font atlas for one of the game's fonts, created on
// first use. The image keeps the guest atlas's normalised glyph layout at a
// higher texel density, so the game's own text quads index it unchanged.
const GuestTexture* EnsureFontAtlas(
    const NativeGuestOutputRenderContext& context, int index) {
  if (index < 0 || index >= int(RendererState::kMaxFontAtlases)) {
    return nullptr;
  }
  GuestTexture& out = g_r.font_tex[index];
  if (out.valid) {
    return &out;
  }
  if (g_r.font_tex_tried[index]) {
    return nullptr;
  }
  g_r.font_tex_tried[index] = true;

  std::vector<uint8_t> alpha;
  uint32_t w = 0;
  uint32_t h = 0;
  if (!skate3::native_scene::DecodeFontAtlas(index, alpha, w, h) || w == 0 ||
      h == 0) {
    return nullptr;
  }
  // Mip chain, stopping at the GUEST atlas's resolution. The replacement is
  // 4-8x the guest atlas, so on-screen glyphs MINIFY heavily -- with mip 0
  // alone the text crawls and sparkles as the element moves, since the 2D
  // sampler (s1) is trilinear with no LOD clamp. But a FULL chain is just as
  // wrong in the other direction: heavily minified text then lands on a mip
  // finer than the game's own atlas never was, and reads blurrier than the
  // original renderer. Clamping the chain at the guest size makes the
  // replacement strictly >= the original at every LOD. RGB stays 255
  // throughout (these atlases are white + coverage), so only alpha is
  // averaged.
  struct Mip {
    uint32_t w, h;
    std::vector<uint8_t> a;
  };
  const uint32_t mip_limit = skate3::native_scene::FontAtlasMipCount(index);
  std::vector<Mip> mips;
  mips.push_back({w, h, std::move(alpha)});
  while (mips.size() < mip_limit && (mips.back().w > 1 || mips.back().h > 1)) {
    const Mip& src = mips.back();
    Mip dst;
    dst.w = std::max(1u, src.w / 2);
    dst.h = std::max(1u, src.h / 2);
    dst.a.resize(size_t(dst.w) * dst.h);
    for (uint32_t y = 0; y < dst.h; ++y) {
      const uint8_t* r0 =
          src.a.data() + size_t(std::min(y * 2, src.h - 1)) * src.w;
      const uint8_t* r1 =
          src.a.data() + size_t(std::min(y * 2 + 1, src.h - 1)) * src.w;
      uint8_t* dr = dst.a.data() + size_t(y) * dst.w;
      for (uint32_t x = 0; x < dst.w; ++x) {
        const uint32_t x0 = std::min(x * 2, src.w - 1);
        const uint32_t x1 = std::min(x * 2 + 1, src.w - 1);
        dr[x] = uint8_t((uint32_t(r0[x0]) + r0[x1] + r1[x0] + r1[x1] + 2u) / 4u);
      }
    }
    mips.push_back(std::move(dst));
  }

  nrhi::Device* device = context.device;
  nrhi::TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.mip_levels = uint32_t(mips.size());
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.initial_state = nrhi::ResourceState::kCopyDest;
  out.texture = device->CreateTexture(desc);
  if (out.texture == nullptr) {
    return nullptr;
  }
  nrhi::TextureViewDesc vd;
  vd.mip_levels = uint32_t(mips.size());
  out.srv = device->CreateTextureView(out.texture, vd);
  if (out.srv == nullptr) {
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return nullptr;
  }
  // One upload buffer for the whole chain; each mip's rows are pitch-aligned
  // and its start offset kept aligned too.
  std::vector<uint32_t> pitch(mips.size());
  std::vector<uint64_t> offset(mips.size());
  uint64_t total = 0;
  for (size_t m = 0; m < mips.size(); ++m) {
    pitch[m] = (mips[m].w * 4 + nrhi::kRowPitchAlignment - 1) &
               ~(nrhi::kRowPitchAlignment - 1);
    offset[m] = total;
    total += uint64_t(pitch[m]) * mips[m].h;
    total = (total + 511) & ~uint64_t(511);
  }
  nrhi::Buffer* upload =
      CreateUploadBuffer(device, total, nrhi::BufferBindClass::kCopySrc);
  uint8_t* mapping =
      upload != nullptr ? static_cast<uint8_t*>(device->Map(upload)) : nullptr;
  if (mapping == nullptr) {
    if (upload != nullptr) {
      device->DestroyDeferred(upload);
    }
    device->DestroyDeferred(out.srv);
    out.srv = nullptr;
    device->DestroyDeferred(out.texture);
    out.texture = nullptr;
    return nullptr;
  }
  for (size_t m = 0; m < mips.size(); ++m) {
    for (uint32_t y = 0; y < mips[m].h; ++y) {
      // RGB is the constant white these atlases carry; only coverage was
      // stored, so expand straight into the (write-combined) upload.
      const uint8_t* srow = mips[m].a.data() + size_t(y) * mips[m].w;
      uint8_t* drow = mapping + offset[m] + size_t(y) * pitch[m];
      for (uint32_t x = 0; x < mips[m].w; ++x) {
        drow[x * 4 + 0] = 255;
        drow[x * 4 + 1] = 255;
        drow[x * 4 + 2] = 255;
        drow[x * 4 + 3] = srow[x];
      }
    }
    context.cmd->CopyBufferToTexture(out.texture, uint32_t(m), 0, upload,
                                     offset[m], pitch[m], mips[m].w, mips[m].h,
                                     1);
  }
  device->Unmap(upload);
  context.cmd->Barrier(out.texture, nrhi::ResourceState::kCopyDest,
                       nrhi::ResourceState::kPixelShaderResource);
  device->DestroyDeferred(upload);
  out.valid = true;
  REXLOG_INFO("native-scene: font atlas '{}' ready at {}x{} ({} mips)",
              skate3::native_scene::FontAtlasName(index), w, h,
              uint32_t(mips.size()));
  return &out;
}

// 1x1 white diffuse fallback + 1x1x6 mid-gray environment-cube fallback.
bool EnsureFallbackTextures(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (!g_r.white.valid) {
    // 1x1 white fallback for items without a resolved diffuse texture.
    nrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    g_r.white.texture = device->CreateTexture(desc);
    if (g_r.white.texture == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white.upload = CreateUploadBuffer(device, 256, nrhi::BufferBindClass::kCopySrc);
    if (!g_r.white.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(g_r.white.upload));
    if (mapping == nullptr) {
      g_r.failed = true;
      return false;
    }
    std::memset(mapping, 0xFF, 4);
    device->Unmap(g_r.white.upload);
    context.cmd->CopyBufferToTexture(g_r.white.texture, 0, 0, g_r.white.upload,
                                     0, 256, 1, 1, 1);
    context.cmd->Barrier(g_r.white.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    g_r.device->DestroyDeferred(g_r.white.upload);
    g_r.white.upload = nullptr;
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.white.srv = device->CreateTextureView(g_r.white.texture, vd);
    if (g_r.white.srv == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white.valid = true;
  }
  if (!g_r.white_cube.valid) {
    // 1x1x6 mid-gray fallback cube for the water reflection slot (t6): a
    // TextureCube SRV must always be bound where the shader declares one.
    nrhi::TextureDesc desc;
    desc.kind = nrhi::TextureKind::kCube;
    desc.width = 1;
    desc.height = 1;
    desc.format = nrhi::Format::kR8G8B8A8_UNORM;
    desc.initial_state = nrhi::ResourceState::kCopyDest;
    g_r.white_cube.texture = device->CreateTexture(desc);
    if (g_r.white_cube.texture == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white_cube.upload = CreateUploadBuffer(device, 512 * 6, nrhi::BufferBindClass::kCopySrc);
    if (!g_r.white_cube.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = static_cast<uint8_t*>(device->Map(g_r.white_cube.upload));
    if (mapping == nullptr) {
      g_r.failed = true;
      return false;
    }
    for (uint32_t f = 0; f < 6; ++f) {
      std::memset(mapping + f * 512, 0x80, 4);
    }
    device->Unmap(g_r.white_cube.upload);
    for (uint32_t f = 0; f < 6; ++f) {
      context.cmd->CopyBufferToTexture(g_r.white_cube.texture, 0, f,
                                       g_r.white_cube.upload, f * 512, 256, 1,
                                       1, 1);
    }
    context.cmd->Barrier(g_r.white_cube.texture, nrhi::ResourceState::kCopyDest,
                         nrhi::ResourceState::kPixelShaderResource);
    g_r.device->DestroyDeferred(g_r.white_cube.upload);
    g_r.white_cube.upload = nullptr;
    nrhi::TextureViewDesc vd;
    vd.dimension = nrhi::ViewDimension::kCube;
    vd.mip_levels = 1;
    g_r.white_cube.srv = device->CreateTextureView(g_r.white_cube.texture, vd);
    if (g_r.white_cube.srv == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.white_cube.valid = true;
  }
  return true;
}

// Depth buffer + MSAA color target, rebuilt on output-size change.
bool EnsureOutputSizedTargets(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  const nrhi::Format want_scene_fmt =
      g_r.hdr_active ? g_r.hdr_scene_format : nrhi::Format::kUnknown;
  if (!g_r.depth || g_r.depth_width != width || g_r.depth_height != height ||
      g_r.targets_hdr != g_r.hdr_active ||
      g_r.targets_scene_fmt != want_scene_fmt ||
      g_r.targets_msaa != g_r.msaa) {
    if (g_r.depth) {
      // The AO/SSR/volumetric scene-depth SRVs alias this texture and
      // re-point on pointer identity; heap reuse can hand the NEW depth
      // texture the OLD one's address, so that comparison must never be
      // the only invalidation. Retire the views with the buffer they view:
      // a stale view kept sampling the retired depth image (still in
      // attachment layout), a GPU fault and device hang on Vulkan.
      nrhi::TextureView** depth_views[3] = {
          &g_r.ao_depth_srv, &g_r.ssr_depth_srv, &g_r.vol_depth_srv};
      nrhi::Texture** depth_views_of[3] = {
          &g_r.ao_depth_srv_of, &g_r.ssr_depth_srv_of, &g_r.vol_depth_srv_of};
      for (int v = 0; v < 3; ++v) {
        if (*depth_views[v] != nullptr) {
          g_r.device->DestroyDeferred(*depth_views[v]);
          *depth_views[v] = nullptr;
        }
        *depth_views_of[v] = nullptr;
      }
      g_r.device->DestroyDeferred(g_r.depth);
      g_r.depth = nullptr;
    }
    // kD32_FLOAT here is the backend's R32_TYPELESS + D32 DSV + R32_FLOAT
    // SRV arrangement, not a fully-typed depth format: the photo-editor
    // postfx depth pack samples this buffer through an R32_FLOAT SRV, and
    // casting an SRV over a fully-typed depth resource is undefined in
    // D3D12; in practice those reads returned the 1.0 clear
    // value for the whole frame, which saturated the ported DoF CoC
    // everywhere (a uniform smear + silhouette halos + scaffold ghosting;
    // sim-verified as the cleared-depth failure mode).
    nrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nrhi::Format::kD32_FLOAT;
    desc.sample_count = g_r.msaa;
    desc.usage = nrhi::kTextureUsageDepthStencil;
    desc.initial_state = nrhi::ResourceState::kDepthWrite;
    desc.clear_depth = 1.0f;
    g_r.depth = g_r.device->CreateTexture(desc);
    if (g_r.depth == nullptr) {
      g_r.failed = true;
      return false;
    }
    g_r.depth_width = width;
    g_r.depth_height = height;

    if (g_r.msaa > 1) {
      // MSAA color target + its Texture2DMS view for the fullscreen resolve
      // pass. Lives in RENDER_TARGET state between frames. Float under HDR
      // (the scene writes pre-tonemap linear).
      if (g_r.msaa_color) {
        g_r.device->DestroyDeferred(g_r.msaa_color);
        g_r.msaa_color = nullptr;
      }
      nrhi::TextureDesc cdesc = desc;
      cdesc.format = g_r.hdr_active ? g_r.hdr_scene_format
                                    : context.guest_output->format();
      cdesc.usage = nrhi::kTextureUsageRenderTarget;
      cdesc.initial_state = nrhi::ResourceState::kRenderTarget;
      cdesc.clear_color[0] = 0.25f;
      cdesc.clear_color[1] = 0.35f;
      cdesc.clear_color[2] = 0.55f;
      cdesc.clear_color[3] = 1.0f;
      g_r.msaa_color = g_r.device->CreateTexture(cdesc);
      if (g_r.msaa_color == nullptr) {
        if (g_r.hdr_active) {
          // Float MSAA target unavailable: drop to the classic path instead
          // of killing the native renderer (rebuilt next frame).
          REXLOG_WARN(
              "native-scene: HDR MSAA color target creation failed, "
              "falling back to the classic tonemap path");
          g_r.hdr_failed = true;
          g_r.depth_width = 0;
          return false;
        }
        g_r.failed = true;
        return false;
      }
      // Re-point the resolve pass's view at the recreated texture.
      if (g_r.msaa_srv_slot) {
        g_r.device->DestroyDeferred(g_r.msaa_srv_slot);
        g_r.msaa_srv_slot = nullptr;
      }
      nrhi::TextureViewDesc vd;
      vd.dimension = nrhi::ViewDimension::k2DMS;
      g_r.msaa_srv_slot = device->CreateTextureView(g_r.msaa_color, vd);
      g_r.msaa_srv_allocated = g_r.msaa_srv_slot != nullptr;
    } else {
      // MSAA switched off: retire the multisample color target and its
      // resolve view (the scene renders directly into the 1x target).
      if (g_r.msaa_srv_slot) {
        g_r.device->DestroyDeferred(g_r.msaa_srv_slot);
        g_r.msaa_srv_slot = nullptr;
        g_r.msaa_srv_allocated = false;
      }
      if (g_r.msaa_color) {
        g_r.device->DestroyDeferred(g_r.msaa_color);
        g_r.msaa_color = nullptr;
      }
    }

    // 1x float scene plane for the HDR post chain: the MSAA resolve
    // destination, or the scene target itself when MSAA is off. Idles in
    // RENDER_TARGET state; the bloom/tonemap passes sample it through
    // hdr_srv.
    if (g_r.hdr_srv != nullptr) {
      g_r.device->DestroyDeferred(g_r.hdr_srv);
      g_r.hdr_srv = nullptr;
    }
    if (g_r.hdr_resolved != nullptr) {
      g_r.device->DestroyDeferred(g_r.hdr_resolved);
      g_r.hdr_resolved = nullptr;
    }
    if (g_r.hdr_active) {
      nrhi::TextureDesc hdesc;
      hdesc.width = width;
      hdesc.height = height;
      hdesc.mip_levels = 1;
      hdesc.format = g_r.hdr_scene_format;
      hdesc.usage = nrhi::kTextureUsageRenderTarget;
      hdesc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.hdr_resolved = g_r.device->CreateTexture(hdesc);
      if (g_r.hdr_resolved != nullptr) {
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        g_r.hdr_srv = g_r.device->CreateTextureView(g_r.hdr_resolved, vd);
      }
      if (g_r.hdr_resolved == nullptr || g_r.hdr_srv == nullptr) {
        REXLOG_WARN(
            "native-scene: HDR scene plane creation failed, falling back "
            "to the classic tonemap path");
        g_r.hdr_failed = true;
        g_r.depth_width = 0;
        return false;
      }
    }
    g_r.targets_hdr = g_r.hdr_active;
    g_r.targets_scene_fmt = want_scene_fmt;
    g_r.targets_msaa = g_r.msaa;
  }
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  nrhi::Device* device = context.device;
  g_r.device = device;

  if (!EnsureRootSignature(context)) {
    return false;
  }

  // HDR path decision, latched for the whole pipeline family (scene/spline
  // PSO formats + shader variants, resolve target, AO composite target,
  // output-sized targets). Hot cvar toggles rebuild everything below;
  // hdr_failed pins the classic path after a float-target failure.
  const bool hdr_want =
      REXCVAR_GET(skate3_native_render_scene_hdr) && !g_r.hdr_failed;
  const nrhi::Format hdr_fmt_want =
      REXCVAR_GET(skate3_native_render_scene_hdr_packed)
          ? nrhi::Format::kR11G11B10_FLOAT
          : nrhi::Format::kR16G16B16A16_FLOAT;
  // MSAA level: the requested count, reduced to what the scene color format
  // supports (1 disables and renders directly into the 1x target). The RHI
  // walks the requested count down in powers of two, exactly like the old
  // CheckFeatureSupport loop. Hot: a change rebuilds the pipeline family
  // (the AO/SSR/volumetric passes and the MSAA targets latch on g_r.msaa
  // and follow).
  const nrhi::Format scene_fmt_want =
      hdr_want ? hdr_fmt_want : context.guest_output->format();
  // VR renders two headset-resolution eyes per frame: its own MSAA cap.
  const int32_t msaa_req =
      skate3::vr::Active()
          ? std::min(REXCVAR_GET(skate3_native_render_scene_msaa), skate3::vr::MaxMsaa())
          : REXCVAR_GET(skate3_native_render_scene_msaa);
  uint32_t msaa_want =
      msaa_req >= 8 ? 8u : msaa_req >= 4 ? 4u : msaa_req >= 2 ? 2u : 1u;
  msaa_want = device->GetSupportedSampleCount(scene_fmt_want, msaa_want);
  // TEMP decal probe: a hot cvar, rebuilt like the showcase variants.
  g_r.decal_probe_want = REXCVAR_GET(skate3_native_render_scene_decal_probe);
  if (!g_r.pso || g_r.rtv_format != context.guest_output->format() ||
      g_r.hdr_active != hdr_want ||
      (hdr_want && g_r.hdr_scene_format != hdr_fmt_want) ||
      g_r.msaa != msaa_want ||
      g_r.showcase_shaders != g_r.showcase_shaders_want ||
      g_r.decal_probe != g_r.decal_probe_want) {
    if (g_r.msaa != msaa_want && g_r.pfx_ready) {
      // The photo-postfx depth-pack pass is compiled against the depth
      // buffer's sample count (PFX_MSAA variant); retire the chain's PSOs
      // so the next photo-editor frame rebuilds them.
      for (nrhi::Pipeline*& p : g_r.pfx_pso) {
        if (p != nullptr) {
          device->DestroyDeferred(p);
          p = nullptr;
        }
      }
      g_r.pfx_ready = false;
    }
    g_r.hdr_active = hdr_want;
    g_r.hdr_scene_format = hdr_fmt_want;
    g_r.msaa = msaa_want;
    g_r.showcase_shaders = g_r.showcase_shaders_want;
    g_r.decal_probe = g_r.decal_probe_want;
    if (!EnsureScenePsoFamily(context) || !EnsureResolvePso(context) ||
        !EnsureBlurPsos(context) || !EnsureOutlineEdgePso(context) ||
        !Ensure2dPso(context) || !EnsureSplinePsos(context) ||
        !EnsureShadowPsos(context)) {
      if (g_r.showcase_shaders) {
        // A showcase-variant build failure must not pin the sticky failure
        // latch: drop the swap request so the F5 retry rebuilds the
        // standard shaders.
        g_r.showcase_shaders_want = false;
        g_r.showcase_shaders = false;
        REXCVAR_SET(skate3_native_render_scene_showcase, false);
      }
      return false;
    }
    REXLOG_INFO("native-scene: pipelines created (MSAA x{}, {}{})", g_r.msaa,
                g_r.hdr_active ? "HDR" : "classic",
                g_r.showcase_shaders ? ", showcase variants" : "");
    g_r.rtv_format = context.guest_output->format();
  }

  if (!EnsureHeapsAndRings(context) || !EnsureShadowResources(context)) {
    return false;
  }
  EnsureBlurOutlineTargets(context);
  if (!EnsureFallbackTextures(context) || !EnsureOutputSizedTargets(context)) {
    return false;
  }
  if (g_r.hdr_active && !EnsureHdrPipeline(context)) {
    // hdr_failed is set: the next frame rebuilds the classic path. Abort
    // this frame (the scene PSOs already target the float format).
    return false;
  }

  {
    // Sampled view of the output (gamma / blur source / photo passes),
    // rebuilt EVERY frame from this frame's guest_output. The RHI destroys
    // guest-output wrappers when the presenter recreates the image (window
    // resize / fullscreen switch reuses the resource pointer at a new size)
    // and evicts older ones once its mailbox map grows past 6; a new wrapper
    // can then land at the freed wrapper's address, so a pointer comparison
    // kept a view of a released resource and the next sample faulted inside
    // D3D12Core. DestroyDeferred retires the old view after the GPU is done.
    if (g_r.output_srv_slot) {
      device->DestroyDeferred(g_r.output_srv_slot);
      g_r.output_srv_slot = nullptr;
    }
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    g_r.output_srv_slot = device->CreateTextureView(context.guest_output, vd);
    g_r.output_srv_allocated = g_r.output_srv_slot != nullptr;
    g_r.rtv_resource = context.guest_output;
  }
  return true;
}

namespace {

// ---- Warm decode (loading-screen prewarm + gameplay warmup) --------------
// Decodes, within `deadline`, every GPU resource `item`'s draw would
// resolve: the mesh VB/IB (with fingerprint revalidation) and each texture
// slot its material binds. Mirrors the draw loop's miss paths, including
// negative-caching of failed texture decodes and retiring replaced
// resources, so the caches end up exactly as drawing would leave them and
// the takeover frame has nothing left to pay. Work past the deadline counts
// as deferred. Cached textures get ONE content revalidation per warmup
// (recheck_frame gate; the frame counter is frozen while yielded): a
// texture pre-decoded while its payload was still streaming in is healed
// here, under budget, instead of by an unbudgeted re-decode burst on the
// takeover frame.
struct WarmCounters {
  uint32_t decodes = 0;
  uint32_t deferred = 0;
};

void WarmItemResources(const NativeGuestOutputRenderContext& context, uint8_t* base,
                       uint64_t frame_number, const DrawItem& item,
                       std::chrono::steady_clock::time_point deadline,
                       WarmCounters& wc) {
  const auto within = [&] { return std::chrono::steady_clock::now() < deadline; };

  bool need_mesh = false;
  auto mit = g_r.meshes.find(item.mesh);
  if (mit == g_r.meshes.end()) {
    need_mesh = true;
  } else if (mit->second.fingerprint != item.fingerprint && !item.ropa &&
             REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
    // Streaming filled/replaced the payload after an earlier (pre)decode.
    // Ropa meshes are excluded: the cloth sim rewrites their payload every
    // frame BY DESIGN (fingerprint never converges), so revalidating them
    // here just destroys+redecodes the base mesh once per settle frame -
    // the churn that kept the settle pass extending itself (observed: one
    // mesh re-fingerprinting 4x in 50 ms). The draw path serves ropa
    // content from the ropa ring / shape generations regardless.
    if (within()) {
      if (g_warm_mesh_log_budget.load(std::memory_order_relaxed) > 0 &&
          g_warm_mesh_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_DEBUG("native-scene: settle mesh REVALIDATE mesh={:08X} fp {:016X}->{:016X}",
                     item.mesh, mit->second.fingerprint, item.fingerprint);
      }
      PoolMeshBuffer(g_r.device, mit->second.vb);
      PoolMeshBuffer(g_r.device, mit->second.ib);
      g_r.meshes.erase(mit);
      need_mesh = true;
    } else {
      ++wc.deferred;
    }
  }
  if (need_mesh) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      MeshBuffers buffers;
      if (DecodeMesh(g_r.device, base, item, buffers)) {
        buffers.fingerprint = item.fingerprint;
        buffers.last_used_frame = frame_number;
        g_r.meshes.emplace(item.mesh, buffers);
      }
      // Failures retry through the draw path (logged + counted there).
    }
  }

  const auto warm_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;  // unreadable / mid-rewrite / demoted: the draw path routes it
    }
    const uint64_t key = FetchWordsKey(words);
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      if (!it->second.valid || frame_number < it->second.recheck_frame ||
          !REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        return;  // negative caches retry via the draw path's schedule
      }
      if (!within()) {
        ++wc.deferred;
        return;
      }
      it->second.recheck_frame = frame_number + 16;
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      if (!it->second.incomplete && (fp == 0 || fp == it->second.payload_fp)) {
        return;
      }
      if (g_warm_tex_log_budget.load(std::memory_order_relaxed) > 0 &&
          g_warm_tex_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_DEBUG("native-scene: settle tex RETIRE ptr={:08X} key={:016X} incomplete={} "
                     "fp {:016X}->{:016X}",
                     tex_ptr, key, it->second.incomplete, it->second.payload_fp, fp);
      }
      RetireGuestTexture(it->second, context.device->CurrentSubmission());
      g_r.tex_store.erase(it);
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    if (!gt.valid) {
      // Negative-cache exactly like the draw path so a permanently
      // unreadable payload cannot hold warmup open.
      std::memcpy(gt.fetch_words, words, sizeof(gt.fetch_words));
      gt.retry_after_frame = frame_number + 120;
    }
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(key, gt);
  };
  // Draw-time fetch-word bindings (streamed artwork / decal ad overrides)
  // share the same store.
  const auto warm_fetch_words = [&](const uint32_t words[6]) {
    if (words[1] == 0) {
      return;
    }
    const uint64_t fkey = FetchWordsKey(words);
    if (g_r.tex_store.contains(fkey)) {
      return;
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(fkey, gt);
  };

  warm_fetch_words(item.diffuse_fetch);
  if (item.ad_seen) {
    warm_fetch_words(item.ad_fetch);
    warm_fetch_words(item.ad_lm_fetch);
  }
  warm_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    warm_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    warm_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6 || item.dynobj != 0 ||
      (item.env_family >= 1 && item.env_family <= 6)) {
    // v2: fams 1-4 and the dynobj families sample their base normal map
    // per-pixel too.
    warm_texture(item.water_normal);
  }
  if (item.water_ocean == 1) {
    warm_texture(item.water_normal2);  // second PCA component (t8)
  }
  if ((item.env_family >= 1 && item.env_family <= 4 && item.env_family != 2) ||
      item.dynobj != 0) {
    // v2 detail normal map (t8; fam 2 carries no base+detail pair).
    warm_texture(item.detail_tex);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    warm_texture(item.decal_art);
    warm_fetch_words(item.decal_fetch);
  }
  if (item.char_skin || item.char_shift || item.char_cloth_stamp) {
    warm_fetch_words(item.char_cg_fetch);
  }
  if (item.sss) {
    warm_fetch_words(item.sss_mask);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    warm_texture(item.hair_alpha_tex);
    warm_texture(item.dirt_tex);
  }
  if ((item.env_family != 0 && item.env_family != 10) || item.unlit ||
      item.dynobj != 0) {
    // unlit = sky: spec_tex is the 1D sun gradient. Decal fams (3/4) and
    // the dynobj families consume their spec masks too (v2, t9).
    warm_texture(item.spec_tex);
  }
  // Environment cube (negative-cached like the draw path).
  if ((item.water || item.char_family >= 6 ||
       (item.env_family >= 5 && item.env_family <= 6) ||
       item.env_family == 13) &&
      item.water_env != 0 && !g_r.cube_textures.contains(item.water_env)) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      GuestTexture c{};
      if (!EnsureGuestCubeTexture(context, base, item.water_env, c)) {
        if (c.upload) g_r.device->DestroyDeferred(c.upload);
        if (c.texture) g_r.device->DestroyDeferred(c.texture);
        c = GuestTexture{};
        c.valid = false;
      }
      c.last_used_frame = frame_number;
      g_r.cube_textures.emplace(item.water_env, c);
    }
  }
}

// ---- Steady-state miss routing (render thread -> decode workers) ----------
// Draw-path cache misses for STATIC content (world meshes, material
// textures) enqueue here instead of decoding inline on the render thread:
// the item skips / renders white / keeps its previous decode for the 1-3
// frames the workers need, instead of stalling the frame for the decode
// (measured ~10 ms avg, ~70 ms max per texture, the panning lag spikes).
// Dynamic payloads (skinned, cloth, ropa) still decode inline: their buffers
// change every frame, so an async result would always be stale.
void EnqueueMeshMiss(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_mesh.insert(mesh).second) {
    PrewarmEntry e{mesh, 8};
    e.miss = true;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}


// Words-keyed texture miss (streamed-artwork posters / event ads): the art
// exists only as draw-time fetch words. Decoded unbudgeted inline these were
// a traversal hitch (a poster decode costs the same ~10 ms as any texture);
// while a decode is in flight the item falls back to its channel diffuse
// (the placeholder poster), not white.
void EnqueueWordsMiss(uint64_t key, const uint32_t words[6], bool ui = false) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_words.insert(key).second) {
    PrewarmEntry e{0, 0, 0, key};
    std::memcpy(e.words, words, sizeof(e.words));
    e.miss = true;
    e.ui = ui;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// Environment-cube miss: one cube decode measured up to ~100 ms inline;
// the gray fallback cube shows for the 1-3 frames the workers need instead.
void EnqueueCubeMiss(uint32_t tex) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_tex.insert(tex).second) {
    PrewarmEntry e{0, 0, tex};
    e.cube = true;
    e.miss = true;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// ---- Prewarm decode workers -----------------------------------------------
// Process one registered mesh on a worker: build the item (same walk and
// payload fingerprint as the capture, so cache entries are identical),
// decode the mesh into upload-heap buffers, and stage every material
// texture up to a filled upload resource. Results go to g_prewarm_out for
// the render thread's commit.
void ProcessPrewarmEntry(uint8_t* base, const PrewarmEntry& e) {
  if (e.mesh == 0 && e.tex == 0 && e.wkey != 0) {
    // Words-keyed texture miss (posters/ads, see EnqueueWordsMiss): stage
    // the decode from the captured fetch words.
    StagedTexResult tr;
    tr.words_key = e.wkey;
    tr.ui = e.ui;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, e.words, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;
    res.mesh_valid = false;
    res.miss = e.miss;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  if (e.mesh == 0 && e.tex != 0) {
    // Environment-cube miss (see EnqueueCubeMiss, the only object-keyed
    // texture path left): stage the decode up to a filled upload resource;
    // the commit records the GPU copies + SRV.
    StagedTexResult tr;
    tr.key = e.tex;
    tr.cube = true;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestCubeTexture(stage_ctx, base, e.tex, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;  // texture-only result (DrawItem::mesh has no default)
    res.mesh_valid = false;
    res.miss = e.miss;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  uint8_t record[0x60];
  DrawItem item{};
  if (!GuestTryCopy(record, base + e.mesh, sizeof(record)) ||
      !BuildItemFromMesh(base, e.mesh, item)) {
    // Buffer objects can finish initializing shortly after registration;
    // retries are re-injected frame-paced by the render thread.
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
      if (e.retries > 0) {
        g_prewarm_retry.push_back({e.mesh, uint16_t(e.retries - 1)});
      } else {
        g_prewarm_dropped.fetch_add(1, std::memory_order_relaxed);
        dropped = true;
      }
    }
    if (dropped) {
      // A dropped draw-path miss must leave the in-flight set so a later
      // frame can retry it (the payload may finish streaming in).
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      g_miss_inflight_mesh.erase(e.mesh);
    }
    return;
  }
  // One representative draw entry so DecodeMesh's two-sided-sheet detection
  // keys off the real primitive type (empty draw lists would pass it
  // unconditionally). Guarded: island params live outside the validated
  // mesh record.
  const uint32_t num_islands = REX_LOAD_U32(e.mesh + 0x38);
  const uint32_t island_params = REX_LOAD_U32(e.mesh + 0x44);
  uint32_t prim0 = 0;
  if (num_islands != 0 && num_islands < 4096 &&
      GuestTryLoadU32(base, island_params, &prim0)) {
    item.draws.push_back(DrawEntry{prim0, 0, 0, item.ib_count});
  }

  PrewarmResult res;
  res.miss = e.miss;
  res.mesh_valid = DecodeMesh(g_r.device, base, item, res.buffers);
  if (res.mesh_valid) {
    res.buffers.fingerprint = item.fingerprint;
  }

  // Stage the material textures (cube maps stay on the render thread:
  // rare, and their decode has its own path). Dedupe through the shared
  // per-load set: workers cannot read the render thread's g_r caches, so a
  // texture cached from a previous map decodes once more per load; the
  // commit discards the duplicate.
  const auto stage_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    // Stable words snapshot: the decode and its store key both come from
    // this snapshot, so a mid-rewrite object can never stage a mixed state.
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;
    }
    const uint64_t wkey = FetchWordsKey(words);
    {
      // Words-aware dedupe: the same content staged once per load; a
      // rebound object (new words) re-stages under its new key.
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      const auto [it, fresh] = g_prewarm_tex_seen.try_emplace(tex_ptr, wkey);
      if (!fresh) {
        if (it->second == wkey) {
          return;
        }
        it->second = wkey;
      }
    }
    StagedTexResult tr;
    tr.words_key = wkey;
    // Staged mode uses only context.device (copies/barrier/SRV are
    // exported for the commit), so a device-only context suffices.
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, words, tr.gt);
    g_tex_stage_out = nullptr;
    if (!tr.valid) {
      std::memcpy(tr.gt.fetch_words, words, sizeof(tr.gt.fetch_words));
    }
    res.textures.push_back(std::move(tr));
  };
  stage_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    stage_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    stage_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6 || item.dynobj != 0 ||
      (item.env_family >= 1 && item.env_family <= 6)) {
    // v2: fams 1-4 and the dynobj families sample their base normal map
    // per-pixel too.
    stage_texture(item.water_normal);
  }
  if (item.water_ocean == 1) {
    stage_texture(item.water_normal2);  // second PCA component (t8)
  }
  if ((item.env_family >= 1 && item.env_family <= 4 && item.env_family != 2) ||
      item.dynobj != 0) {
    // v2 detail normal map (t8; fam 2 carries no base+detail pair).
    stage_texture(item.detail_tex);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    stage_texture(item.decal_art);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    stage_texture(item.hair_alpha_tex);
    stage_texture(item.dirt_tex);
  }
  if ((item.env_family != 0 && item.env_family != 10) || item.unlit ||
      item.dynobj != 0) {
    // unlit = sky: spec_tex is the 1D sun gradient. Decal fams (3/4) and
    // the dynobj families consume their spec masks too (v2, t9).
    stage_texture(item.spec_tex);
  }

  res.item = std::move(item);
  std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
  g_prewarm_out.push_back(std::move(res));
}

void PrewarmWorkerLoop() {
#if defined(_WIN32)
  // Below-normal priority: the workers flood in exactly when the guest's
  // single-threaded world ACTIVATION runs (registration is the final load
  // phase), and at normal priority they stretch the game's own black
  // window at the loading->gameplay boundary. At below-normal they only
  // soak idle cores and the guest always wins the contention.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    PrewarmEntry e{};
    DynDecodeJob dyn;
    bool have_dyn = false;
    {
      std::unique_lock<std::mutex> lock(g_prewarm_mutex);
      g_prewarm_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
        return !g_prewarm_queue.empty() || !g_miss_queue.empty() ||
               !g_dyn_jobs.empty();
      });
      if (!g_dyn_jobs.empty()) {
        // Dynamic cloth first: these are per-frame payloads whose result
        // should land at the very next commit.
        dyn = std::move(g_dyn_jobs.front());
        g_dyn_jobs.erase(g_dyn_jobs.begin());
        have_dyn = true;
      } else if (!g_miss_queue.empty()) {
        // Draw-path misses next: this content is visible RIGHT NOW (white /
        // skipped geometry). On the old shared LIFO queue a streaming
        // registration burst kept cutting the line ahead of the visible
        // miss; medium-distance pop-in lasted the whole backlog.
        e = g_miss_queue.front();
        g_miss_queue.erase(g_miss_queue.begin());
      } else if (!g_prewarm_queue.empty()) {
        e = g_prewarm_queue.back();
        g_prewarm_queue.pop_back();
      } else {
        continue;
      }
    }
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (base == nullptr || g_r.device == nullptr) {
      if (!have_dyn) {
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_retry.push_back(e);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    // Decode workers only ever read guest memory on the renderer's behalf:
    // keep them permanently armed for raw-load read-fault recovery (POSIX;
    // no-op on Windows).
    ArmGuestReadRecoveryForThread(base);
    if (have_dyn) {
      PrewarmResult res;
      res.mesh_valid =
          DecodeMesh(g_r.device, base, dyn.item, res.buffers, dyn.vb.data(),
                     dyn.ib.empty() ? nullptr : dyn.ib.data());
      if (res.mesh_valid) {
        res.buffers.fingerprint = dyn.item.fingerprint;
        res.buffers.dyn_seq = dyn.seq;
        res.item = std::move(dyn.item);
        res.miss = true;  // per-frame cloth: never behind the commit cap
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_out.push_back(std::move(res));
      }
      continue;
    }
    ProcessPrewarmEntry(base, e);
  }
}

// Lazily start the decode workers (process-lifetime, parked on the queue's
// condition variable when idle). Only started once the pipeline exists;
// the workers create GPU resources through g_r.device (thread-safe).
void EnsurePrewarmWorkers() {
  if (g_r.device == nullptr ||
      g_prewarm_workers_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const unsigned hw = std::max(4u, std::thread::hardware_concurrency());
  // Below-normal priority makes extra workers near-free (they only soak
  // idle cores; the guest always wins the contention) while the pool size
  // governs how fast a gameplay streaming burst decodes; 4 workers left
  // medium-distance pop-in on big sectors visibly behind the emulated
  // renderer.
  const unsigned n = std::clamp(hw / 3u, 2u, 8u);
  for (unsigned i = 0; i < n; ++i) {
    std::thread(PrewarmWorkerLoop).detach();
  }
  REXLOG_INFO("native-scene: {} prewarm decode workers started", n);
}

// Render-thread commit of the workers' results: record GPU copies +
// barriers, create SRVs, insert into the caches, re-inject retries.
// Microseconds per item, safe to run every frame (loading, settle and
// gameplay alike).
void PrewarmCommit(const NativeGuestOutputRenderContext& context,
                   uint64_t frame_number, bool loading = false) {
  // Keep resources referenced by the paired overlay snapshot alive/unchanged.
  if (skate3::vr::CurrentEye() == 1) return;
  {
    std::scoped_lock lock(g_prewarm_out_mutex, g_prewarm_mutex);
    if (!g_prewarm_retry.empty()) {
      g_prewarm_queue.insert(g_prewarm_queue.end(), g_prewarm_retry.begin(),
                             g_prewarm_retry.end());
      g_prewarm_retry.clear();
      g_prewarm_cv.notify_all();
    }
  }
  std::vector<PrewarmResult> done;
  {
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    // Soft cap per frame: during the activation burst the workers can
    // complete hundreds of items between two refreshes, and committing
    // them all at once records thousands of copies + SRVs into a single
    // frame (measured ~96 us per item; a 256 batch is ~25 ms). Behind a
    // loading screen that is fine; in gameplay the cap stays small so a
    // streaming burst spreads over a few frames instead of hitching one.
    const size_t kMaxCommitPerFrame = loading ? 256 : 32;
    if (g_prewarm_out.size() <= kMaxCommitPerFrame) {
      done.swap(g_prewarm_out);
    } else {
      // Oldest first: draining from the END starved early results under a
      // sustained streaming burst (they sat behind an ever-refilling tail,
      // so exactly the content that had waited longest stayed popped-out).
      // Draw-path MISS results (content visible right now: white/skipped)
      // bypass the cap entirely: under a big sector streaming burst they
      // otherwise queued behind hundreds of speculative prewarm results
      // for several frames of visible pop-in. Only a handful arrive per
      // frame, so the bypass cannot recreate the commit hitch the cap
      // exists to prevent.
      std::vector<PrewarmResult> rest;
      rest.reserve(g_prewarm_out.size());
      size_t taken = 0;
      for (PrewarmResult& r : g_prewarm_out) {
        const bool is_miss = r.miss;
        if (is_miss || taken < kMaxCommitPerFrame) {
          if (!is_miss) {
            ++taken;
          }
          done.push_back(std::move(r));
        } else {
          rest.push_back(std::move(r));
        }
      }
      g_prewarm_out.swap(rest);
    }
  }
  if (done.empty()) {
    return;
  }
  const auto commit_t0 = PerfClock::now();
  bool committed_tex = false;
  // For the payload-stability verify below (SEH-guarded sampled reads).
  uint8_t* verify_base = g_guest_base.load(std::memory_order_relaxed);
  for (PrewarmResult& r : done) {
    if (r.mesh_valid) {
      auto mit = g_r.meshes.find(r.item.mesh);
      const bool superseded =
          mit != g_r.meshes.end() && mit->second.dyn_seq > r.buffers.dyn_seq;
      // ROPA shape-generation ring: retain this decode's vertex array
      // (keyed by dyn_seq) for the draw-time blend onto the play clock.
      // Runs even when the GPU buffers get dropped as identical below;
      // the SEQ still advances and the interp ring may reference it.
      // SUPERSEDED decodes are retained too: the decode workers finish out
      // of order, and the play clock runs BEHIND now, so the kernel asks
      // for exactly the older generations a newer commit overtook.
      // Dropping them starved the blend (blend=19 vs blendmiss=7005 over a
      // session): the garment fell back to the newest raw shape, off the
      // body's play clock - the loose hair / shirt with smoothing on. The
      // ring is kept in seq order (the stale gate reads its newest time).
      if (r.item.ropa && r.buffers.dyn_seq != 0 && !r.buffers.ropa_verts.empty()) {
        auto& ring = g_r.ropa_shapes[r.item.mesh];
        const double now_s =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        auto pos = ring.end();
        while (pos != ring.begin() && std::prev(pos)->seq > r.buffers.dyn_seq) {
          --pos;
        }
        // A late (out-of-order) generation inherits its newer neighbour's
        // commit time at most, so the ring's times stay monotonic too.
        const double t = pos != ring.end() ? std::min(now_s, pos->t) : now_s;
        ring.insert(pos, {r.buffers.dyn_seq, t, std::move(r.buffers.ropa_verts)});
        // The 8-tap boxcar kernel reaches filter_w/2 (~25 ms) past the
        // play clock (itself ~2 guest periods behind), plus decode-latency
        // slack. Generations arrive per rendered frame while the cloth sim
        // runs (the pose/shape pairing depends on that cadence), so the
        // retention must scale with the highest supported render rate:
        // 48 slots keep the kernel's ~55 ms reach covered up to ~800 fps.
        // Time bound as well: only the kernel's reach (~55 ms behind the
        // newest pose) plus decode-latency slack is ever read, and with
        // every pedestrian garment retained (see the backstop below) a
        // fixed 48 full vertex copies per garment would cost hundreds of MB.
        while (ring.size() > 48 ||
               (ring.size() > 2 && ring.back().t - ring.front().t > 0.15)) {
          ring.pop_front();
        }
        // Growth backstop. A blanket clear at 64 garments wiped EVERY
        // garment's history whenever the city's pedestrians (each with
        // cloth-simmed hair/clothes) pushed the count past it, which is
        // most gameplay commits: rings never held more than one
        // generation, the play-clock blend missed ~99.7% of frames
        // (gen_absent, ring=1) and the player's hair/shirt drew the newest
        // raw shape ahead of the smoothed body. Prune only garments that
        // stopped producing shapes (> 1 s, streamed out / sim asleep; the
        // mesh eviction and layout-change paths erase their own entries);
        // clear outright only past a far larger bound.
        if (g_r.ropa_shapes.size() > 128) {
          std::erase_if(g_r.ropa_shapes, [now_s](const auto& e) {
            return e.second.empty() || now_s - e.second.back().t > 1.0;
          });
          if (g_r.ropa_shapes.size() > 1024) {
            g_r.ropa_shapes.clear();
          }
        }
      }
      if (mit != g_r.meshes.end() &&
          (mit->second.fingerprint == r.buffers.fingerprint || superseded)) {
        // Identical content already cached (lost the race against the draw
        // path / an earlier result), or a NEWER dynamic decode already
        // landed (multi-worker reordering must not step the cloth
        // backwards): the staged buffers were never referenced by any
        // submission.
        PoolMeshBuffer(g_r.device, r.buffers.vb);
        PoolMeshBuffer(g_r.device, r.buffers.ib);
        mit->second.last_used_frame = frame_number;
      } else {
        if (mit != g_r.meshes.end()) {
          // Stale decode (miss-driven revalidation heal): swap it out. The
          // old buffers may be referenced by the in-flight submission.
          PoolMeshBuffer(g_r.device, mit->second.vb);
          PoolMeshBuffer(g_r.device, mit->second.ib);
          g_r.meshes.erase(mit);
        }
        r.buffers.last_used_frame = frame_number;
        g_r.meshes.emplace(r.item.mesh, r.buffers);
      }
    }
    for (StagedTexResult& t : r.textures) {
      // Payload-stability verify: a decode read while its payload was still
      // STREAMING IN is a garbage interleave (the mip-churn "goes black,
      // then reloads" flash; fresh mip words repoint mid-upload, and the
      // fingerprint sampled right after the worker's read can look stable).
      // The commit runs 1-3 frames later: re-sample here and FAIL unstable
      // results, so the cache keeps the previous good decode and the retry
      // clock re-runs the heal once the payload settles. Cubes are exempt
      // (static assets; a failed cube negative-caches permanently).
      // UI-origin results skip the verify (see PrewarmEntry::ui): animating
      // APT art legitimately rewrites its payload every guest frame, so the
      // re-sample below would reject every mid-animation commit and freeze
      // the element; the 2D resolve's content probe is the heal path there.
      if (t.valid && !t.cube && !t.ui && verify_base != nullptr &&
          t.gt.payload_addr != 0 &&
          SampleProbeFingerprint(verify_base, t.gt) != t.gt.payload_fp) {
        if (t.gt.texture) {
          g_r.device->DestroyDeferred(t.gt.texture);
          t.gt.texture = nullptr;
        }
        if (t.gt.upload) {
          g_r.device->DestroyDeferred(t.gt.upload);
          t.gt.upload = nullptr;
        }
        t.valid = false;
        t.verify_failed = true;
        g_heal_verify_fail.fetch_add(1, std::memory_order_relaxed);
      }
      if (t.cube) {
        // Environment cube: lands in the cube cache. Cubes are static
        // assets: an existing valid entry wins; a failed decode
        // negative-caches like the old inline path did.
        auto cit = g_r.cube_textures.find(t.key);
        if (cit != g_r.cube_textures.end() && cit->second.valid) {
          if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
          if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
          continue;
        }
        if (cit != g_r.cube_textures.end()) {
          RetireGuestTexture(cit->second, context.device->CurrentSubmission());
          g_r.cube_textures.erase(cit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
        }
        if (t.gt.last_used_frame == 0) {
          t.gt.last_used_frame = frame_number;
        }
        g_r.cube_textures.emplace(t.key, t.gt);
        continue;
      }
      if (t.words_key != 0) {
        // Store commit: the result files under its decode-time words key
        // unconditionally; a rebound object simply routes elsewhere, so a
        // worker result can never land on the wrong identity. The only
        // remaining valid->valid swap class is an in-place content change
        // at the same words (event-ad rotation, mip-pool fills, composed
        // lightmap pages); the payload verify above covers exactly that.
        const bool tr_key =
            !g_trace_keys.empty() && g_trace_keys.count(t.words_key) != 0;
        auto wit = g_r.tex_store.find(t.words_key);
        if (tr_key) {
          REXLOG_INFO(
              "tex-trace: f{} COMMIT key={:016X} valid={} vfail={} "
              "fp={:016X} inc={} nb={} cached={}",
              frame_number, t.words_key, t.valid ? 1 : 0,
              t.verify_failed ? 1 : 0, t.gt.payload_fp,
              t.gt.incomplete ? 1 : 0, t.gt.near_black ? 1 : 0,
              wit != g_r.tex_store.end()
                  ? (wit->second.valid ? "valid" : "invalid")
                  : "none");
        }
        if (wit != g_r.tex_store.end()) {
          // Same-content dedup, except a complete re-decode always
          // displaces an incomplete cached entry (truncated tiled-mip copy:
          // the zeroed blocks live in mips the fingerprint never samples).
          const bool same_content = t.valid && wit->second.valid &&
                                    t.gt.payload_fp == wit->second.payload_fp &&
                                    !(wit->second.incomplete && !t.gt.incomplete);
          if (same_content || (!t.valid && wit->second.valid)) {
            // Keep the cached decode ("keep the old decode when the payload
            // became unreadable": mips stream out at range). A failed heal
            // of a still-serving entry needs no retry stamp: the payload
            // poll re-detects on its own cadence and the miss-inflight set
            // already dedupes.
            if (same_content && t.gt.near_black && wit->second.near_black &&
                wit->second.nb_redecodes < 255) {
              // A forced near-black re-decode came back identical: one more
              // confirmation toward "genuinely uniform content".
              ++wit->second.nb_redecodes;
            }
            if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
            if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
            continue;
          }
          if (t.valid && wit->second.valid) {
            // In-place content swap: the only commit class a player can
            // SEE; rolling-capped log so a flicker sighting names its
            // texture.
            static std::atomic<uint32_t> s_swap_logs{0};
            static std::atomic<int64_t> s_swap_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t swin = s_swap_win.load(std::memory_order_relaxed);
            if (now_s - swin >= 5 &&
                s_swap_win.compare_exchange_strong(swin, now_s)) {
              s_swap_logs.store(0, std::memory_order_relaxed);
            }
            if (s_swap_logs.fetch_add(1) < 8) {
              REXLOG_DEBUG(
                  "native-scene: texture heal commit key={:016X} fp {:016X} "
                  "-> {:016X}",
                  t.words_key, wit->second.payload_fp, t.gt.payload_fp);
            }
          }
          if (!t.valid) {
            t.gt.fail_count = wit->second.fail_count;  // keep the backoff arc
          }
          t.gt.last_used_frame = wit->second.last_used_frame;
          RetireGuestTexture(wit->second, context.device->CurrentSubmission());
          g_r.tex_store.erase(wit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
          // Content landed this frame; the video-start cold/hot classifier
          // (GuestTexture::last_change_frame) keys off commit times.
          t.gt.last_change_frame = frame_number;
        } else {
          t.gt.fail_count = BumpFail(t.gt.fail_count);
          t.gt.retry_after_frame = frame_number + RetryBackoff(t.gt.fail_count);
          // Failed decodes render white: log each once (capped) so white
          // meshes stay attributable to a specific texture.
          static std::unordered_set<uint64_t> logged_failed;
          if (logged_failed.size() < 64 &&
              logged_failed.insert(t.words_key).second) {
            REXLOG_INFO(
                "native-scene: texture decode FAILED key={:016X} "
                "fetch=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                t.words_key, t.gt.fetch_words[0], t.gt.fetch_words[1],
                t.gt.fetch_words[2], t.gt.fetch_words[3], t.gt.fetch_words[4],
                t.gt.fetch_words[5]);
          }
        }
        // Fresh commits must enter the store with a live LRU stamp: an
        // unstamped (0) clock sorts as the oldest entry, and with the store
        // over cap the eviction latch would retire brand-new decodes before
        // their first draw ever stamps them - a commit/evict/re-decode loop
        // that starved dense areas of all new content.
        if (t.gt.last_used_frame == 0) {
          t.gt.last_used_frame = frame_number;
        }
        g_r.tex_store.emplace(t.words_key, t.gt);
        continue;
      }
      // No words key and not a cube: an empty/failed stage slot; release
      // whatever it carries (nothing routes to it).
      if (t.gt.texture) g_r.device->DestroyDeferred(t.gt.texture);
      if (t.gt.upload) g_r.device->DestroyDeferred(t.gt.upload);
    }
    g_prewarm_done.fetch_add(1, std::memory_order_relaxed);
  }
  // Release the miss-in-flight keys so later revalidation cycles can enqueue
  // these again (erasing keys that were never in the sets is harmless).
  {
    std::lock_guard<std::mutex> lock(g_prewarm_mutex);
    for (const PrewarmResult& r : done) {
      if (r.item.mesh != 0) {
        g_miss_inflight_mesh.erase(r.item.mesh);
      }
      for (const StagedTexResult& t : r.textures) {
        if (t.words_key != 0) {
          g_miss_inflight_words.erase(t.words_key);
        } else {
          g_miss_inflight_tex.erase(t.key);
        }
      }
    }
  }
  if (committed_tex) {
    context.cmd->FlushBarriers();
  }
  g_pw_commit.Add(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - commit_t0)
          .count()));
}

// Render-thread mirror of the presence-context check (set in YieldForMenus):
// menu screens shorten the 2D texture liveness recheck cadence so in-place
// UI-texture rewrites (portrait resolves) heal fast.
std::atomic<bool> g_in_menus_frame{false};

// Set by YieldForMenus, consumed by RenderScene in the same call (render
// thread only): this frame is a NATIVE loading-screen frame; no world scene
// exists (or it is the previous map's stale one), so RenderScene renders a
// black backdrop + the captured 2D loading UI instead of the world.
bool g_loading_native_frame = false;
// Render thread only: the previous rendered frame was a native loading
// frame. Used to HOLD the loading visuals through the post-load takeover
// gate window; the emulated output was suppressed all through the native
// loading screen, so yielding there would flash the stale pre-load frame.
bool g_loading_hold = false;

// Menus / pause / loading yield gate (see the comment at the call site):
// returns true when RenderScene must yield this frame to the emulated
// output. Handles the cache clears on entry and the takeover re-arm +
// loading-screen pipeline build / prewarm commit while in a load, whether
// the loading pixels themselves render emulated (yield) or natively
// (g_loading_native_frame).
bool YieldForMenus(const NativeGuestOutputRenderContext& context) {
  static bool s_in_loading = false;
  static bool s_seen_gameplay = false;
  static bool s_pause_native = false;
  const bool in_menus = rex::kernel::guest_presence::GameplayContextValue() == 0;
  const bool difficulty_init = in_menus && DifficultyScreenActive();
  // Render-thread mirror for the 2D texture resolver: menu screens shorten
  // the content-liveness recheck cadence (see resolve_2d_texture) so
  // in-place rewrites of UI textures (the one-shot skater-portrait resolves)
  // heal within a couple of frames instead of up to 16.
  g_in_menus_frame.store(in_menus, std::memory_order_relaxed);
  if (!in_menus) {
    s_seen_gameplay = true;
  }
  // In-game pause menu: the presence context reads 0, but the world keeps
  // resubmitting perspective scenes behind the menu (loading screens and the
  // boot frontend stop publishing): stay native there so the pause backdrop
  // renders natively and the caches survive the pause. The 2D pause UI rides
  // the same captured-APT overlay replay as the gameplay HUD. If publishes
  // go stale (a load was picked from the pause menu, or the game stops
  // redrawing the world), this degrades to the yield path below within
  // ~300 ms, cache clears and all.
  // boot_native lifts the first-gameplay prerequisite from both native menu
  // modes: a boot-frontend 3D backdrop renders like a pause backdrop, and
  // everything else (videos, menus, the first load) renders as 2D-over-black.
  const bool boot_native = REXCVAR_GET(skate3_native_render_scene_boot_native);
  bool pause_native = false;
  if (in_menus && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_pause_native)) {
    const int64_t last_ns = g_last_publish_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    pause_native = last_ns >= 0 && now_ns - last_ns < 300'000'000;
  }
  if (pause_native != s_pause_native) {
    s_pause_native = pause_native;
    if (pause_native) {
      REXLOG_INFO(
          "native-scene: pause menu over live world - staying NATIVE "
          "(2d stats at entry: draws_2d={} other={} dropped={})",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    } else {
      REXLOG_INFO(
          "native-scene: leaving native pause ({}; 2d stats at exit: "
          "draws_2d={} other={} dropped={})",
          in_menus ? "scene publishes went stale - loading/frontend"
                   : "gameplay resumed",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    }
  }
  // Menu-context un-suppression: the game produces some content as one-shot
  // off-screen renders; the team-menu skater portrait boxes are a
  // render-to-texture pass issued once when the screen opens (and re-issued
  // after an edit). With emulated draws suppressed those passes never
  // execute, the resolve never writes the portrait texture, and the boxes
  // stay empty forever (the F11 emulated pair-shot showed the same empty box
  // - the texture is persistent guest state that was simply never filled).
  // While a menu context is up, clear the SDK suppress cvar so the emulated
  // pipeline keeps every RTT/composite current; the extra GPU cost is
  // menu-only. The saved value is restored on the first gameplay frame, so a
  // user toggle of the underlying cvar in the debug dialog survives (it is
  // re-read at each menu entry).
  {
    static bool s_unsup_forced = false;
    static bool s_unsup_saved = false;
    // Screen 9 can need initialization passes outside the pitch-exempt
    // portrait band. Let those execute while continuing to present the
    // native UI. Restore the configured suppression when the screen closes.
    const bool want = difficulty_init ||
        (in_menus && REXCVAR_GET(skate3_native_render_scene_menu_unsuppress));
    if (want && !s_unsup_forced) {
      s_unsup_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, false);
        REXLOG_INFO(
            "native-scene: {} - emulated initialization draws enabled "
            "(native presentation retained)",
            difficulty_init ? "difficulty screen" : "menu context");
      }
      s_unsup_forced = true;
    } else if (!want && s_unsup_forced) {
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
        REXLOG_INFO(
            "native-scene: menu initialization window closed - emulated "
            "draw suppression restored");
      }
      s_unsup_forced = false;
    }
  }
  // Menu-context suppression FILTER relaxation (the near-native sibling of
  // the full lift above): mode 0 keeps the framebuffer passes suppressed,
  // the screen stays natively composed, but lets the sub-framebuffer RTT
  // passes execute, which is where the one-shot skater-portrait renders
  // live (their pitch sits inside the mode-2 suppressed band; with mode 2
  // active in menus the boxes stayed empty).
  {
    static bool s_mode_forced = false;
    static int32_t s_mode_saved = 0;
    // menu_rtt_scope 1: only hold mode 0 inside the portrait window
    // (screen-transition grace / not-known-steady screens / CAS); outside
    // it, menus keep the gameplay-proven mode-2 suppression instead of
    // executing the game's postfx chain emulated at scale every frame
    // (the pause-menu/title-screen GPU contention).
    const bool want = difficulty_init ||
        (in_menus && REXCVAR_GET(skate3_native_render_scene_menu_rtt_passes) &&
        (REXCVAR_GET(skate3_native_render_scene_menu_rtt_scope) == 0 ||
         PortraitRttWindowActive()));
    if (want && !s_mode_forced) {
      s_mode_saved = REXCVAR_GET(native_render_suppress_mode);
      // Mode 3, not 0: the portrait-class RTTs (census 560-1200 during the
      // window) execute, but the 1152-wide main scene + postfx band stays
      // suppressed; mode 0 ran the game's whole pipeline at scaled
      // resolution for the window's duration, dropping the pause menu to
      // ~60 fps whenever a screen push (or its 3 s transition grace)
      // opened the window.
      if (s_mode_saved != 3) {
        REXCVAR_SET(native_render_suppress_mode, 3);
        REXLOG_INFO(
            "native-scene: portrait window - suppress mode {} -> 3 (portrait "
            "RTT passes execute, scene/postfx band stays suppressed; "
            "restored when the window closes)",
            s_mode_saved);
      }
      s_mode_forced = true;
    } else if (!want && s_mode_forced) {
      if (s_mode_saved != 3) {
        REXCVAR_SET(native_render_suppress_mode, s_mode_saved);
        REXLOG_INFO(
            "native-scene: portrait window closed - suppress mode {} restored",
            s_mode_saved);
      }
      s_mode_forced = false;
    }
    // Same menu window: shader compilation goes SYNCHRONOUS. With
    // async_shader_compilation on, the d3d12 command processor SKIPS any
    // draw whose pipeline is still compiling (command_processor.cpp
    // ConfigurePipeline tail): fine mid-gameplay, but the skater-portrait
    // boxes are ONE-SHOT renders: pieces skipped during a first-run compile
    // are baked into the portrait forever (the armless/torso-less
    // skaters; later runs are fine because the
    // shader/pipeline disk storage is warm). Menus tolerate the one-time
    // compile stalls invisibly.
    static bool s_async_forced = false;
    static bool s_async_saved = false;
    if (want && !s_async_forced) {
      s_async_saved = REXCVAR_GET(async_shader_compilation);
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, false);
        REXLOG_INFO(
            "native-scene: menu context - shader compilation synchronous "
            "(one-shot portrait renders can't skip still-compiling pieces)");
      }
      s_async_forced = true;
    } else if (!want && s_async_forced) {
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, true);
      }
      s_async_forced = false;
    }
  }
  const bool in_loading = in_menus && !pause_native;
  // Loading screens themselves render natively too (black + the captured 2D
  // loading UI) when enabled, everything after the first gameplay. The
  // housekeeping below runs for the loading STATE either way; only the
  // yield decision changes.
  const bool loading_native =
      in_loading && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_loading_native);
  g_loading_native_frame = loading_native;
  if (in_loading != s_in_loading) {
    s_in_loading = in_loading;
    if (in_loading) {
      REXLOG_INFO(
          "native-scene: menus/loading - {} (presence context)",
          loading_native ? "rendering the loading screen NATIVELY"
                         : "yielding to emulated output");
      // Arena addresses are reused across map loads: let the next load's
      // registrations re-queue meshes (and re-stage textures) at reused
      // addresses, and drop the cached item cores built from them.
      ClearItemCache();
      // Off-screen retention holds guest-address-keyed copies too.
      g_retained_clear.store(true, std::memory_order_relaxed);
      // The static-caster cache holds world-positioned records from the
      // OUTGOING map; different maps share coordinate ranges, so stale
      // records whose meshes survive the transition would keep casting
      // (an entire arriving area sat under the previous map's building
      // shadows). Invalidate the sun map with it.
      g_r.static_casters.clear();
      g_r.nsm_dirty = true;
      g_r.static_sun_valid = false;
      g_r.nsm_built_radius = 0.0f;
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      g_prewarm_seen.clear();
      g_prewarm_tex_seen.clear();
    } else {
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(g_prewarm_mutex);
        queued = g_prewarm_queue.size();
      }
      REXLOG_INFO(
          "native-scene: gameplay (prewarm: {} meshes decoded, {} dropped, {} still "
          "queued)",
          g_prewarm_done.load(std::memory_order_relaxed),
          g_prewarm_dropped.load(std::memory_order_relaxed), queued);
    }
  }
  if (in_loading) {
    // Re-arm the takeover gate for the next stretch of gameplay (map
    // load, unpause).
    g_warmup_armed.store(true, std::memory_order_relaxed);
    {
      // Freshness gate: only scenes published AFTER this point qualify;
      // g_scene still holds the previous map's last scene all through the
      // loading screen (BuildFrameScene stops publishing without a
      // perspective view).
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      g_warmup_fresh_generation = (g_scene ? g_scene->generation : 0) + 1;
    }
    // Build the pipelines / render targets behind the loading screen so
    // the one-time PSO compilation (~200 ms) never lands on a gameplay
    // frame.
    if (!g_r.failed && g_r.pso == nullptr) {
      EnsurePipeline(context);
    }
    if (loading_native) {
      // RenderScene renders this frame (black + 2D loading UI) and runs
      // the prewarm commit itself with the loading budget.
      return false;
    }
    // THE loading-screen heavy lifting runs on the prewarm decode WORKER
    // POOL (a serial render-thread drain both tanked the loading spinner
    // to ~13 fps and still left 1500 of a map's ~2600 meshes undecoded at
    // takeover; map-change loads register most of the world in their
    // final seconds and drop the guest to 10-25 fps while doing it).
    // Here the render thread only commits finished results: record the
    // GPU copies, create SRVs, insert into the caches.
    if (REXCVAR_GET(skate3_native_render_scene_prewarm_budget_ms) > 0 &&
        !g_r.failed && g_r.pso != nullptr) {
      EnsurePrewarmWorkers();
      PrewarmCommit(context, g_frames_rendered.load(std::memory_order_relaxed),
                    /*loading=*/true);
    }
    return true;
  }
  return false;
}

  // Photo-mission photo editor (the "Pick a photo" screen with the depth of
  // field / saturation / brightness / contrast controls; FE screen class
  // PhotoSelect, challenge/photoselect.swf): the editor's effects ARE the
  // game's postfx chain, which native rendering suppresses, so natively
  // the photo showed the raw scene and the controls did nothing. Yield to
  // the emulated output while the editor is up: the emulated frame there is
  // complete and exact, and the scene is frozen so emulated-path
  // performance is fine. Unlike the menus branch above this touches no
  // caches and no takeover gates; native rendering resumes on the next
  // frame after the editor closes.
  //
  // Detection is a per-frame poll of the game's FE state (stateless, so it
  // can't get stuck): FrontEndManager singleton ptr global 0x830CFE14
  // (TU3; from SingletonHolder<FrontEndManager>::Instance = 824AD2F0),
  // whose NIS FE push-state stack (eastl::vector of 20-byte records at
  // +0x210) holds a {1, 11} record exactly while the photographer NIS has
  // the editor pushed. Surveyed across 50 gsnaps spanning gameplay, menus
  // and other missions: every non-editor record reads {x, -1}; only the
  // photo-editor capture shows a non-(-1) second field. The
  // PhotoReplayController heartbeat (rex_Sk8_Challenge_PhotoReplayController_Update_UAAXM_Z_825623F0 hook) is kept as a
  // secondary signal for photo flows that bypass the photographer NIS.
// FMV playback (intro logos, any full-motion video): the frame is
// CPU-decoded into a texture (VideoRenderer_RwTexture Lock/Fill/Unlock) and
// reaches the screen through the game's postfx chain + swap; no capturable
// 2D draw exists (captured FMV frames show ~15 draws, all postfx
// passes + fade fills), so the native path has nothing to replay. Yield
// while the MovieDecoder::Decode heartbeat is fresh; the emulated frame is
// complete and correct there (photo-editor class). Touches no caches or
// takeover gates; native rendering resumes on the next frame after the
// movie ends.
bool YieldForMovie() {
  if (!REXCVAR_GET(skate3_native_render_scene_fmv_yield)) {
    return false;
  }
  const int64_t last_ns = g_movie_decode_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const bool active = now_ns - last_ns < 500'000'000;
  // LAST-RESORT gating: yielding INSTANTLY at video start presented the
  // emulated framebuffer, which under draw suppression still holds a stale
  // frame: the flash of the PREVIOUS video at every video boundary
  // (measured: yield, planes published +16 ms, substitution
  // native +38 ms, a ~5-frame stale-frame window). The substitution path
  // gets 400 ms from the first heartbeat to engage, and once it has drawn
  // recently the yield stays off entirely (covers the video-END race the
  // same way). Genuine native-FMV failure (fmv_native off, pso missing,
  // plane decode failure) still reaches the emulated yield after the
  // grace window.
  static int64_t s_fresh_since = -1;
  if (!active) {
    s_fresh_since = -1;
  } else if (s_fresh_since < 0) {
    s_fresh_since = now_ns;
  }
  const int64_t native_ns = g_movie_native_last_ns.load(std::memory_order_relaxed);
  // Yield only for a video that is ON SCREEN and NOT being served:
  // (a) quad_recent - the 2D replay recently carried a video quad (detection
  //     runs regardless of movie_sub). A skipped/force-completed video
  //     (intro skip) leaves the decoder heartbeat alive ~0.5 s with no quad
  //     anywhere; yielding then flashed the stale emulated buffer.
  // (b) staleness measured decoder-vs-native, not wall-clock: at a manual
  //     skip the quad vanishes instantly but the decoder tail keeps beating
  //     up to ~500 ms - a genuinely unsubstituted video keeps decoding long
  //     past the last native draw and crosses the 1 s bar within ~1.4 s of
  //     starting; a skip tail (<= ~600 ms) never can.
  const int64_t quad_ns = g_movie_quad_last_ns.load(std::memory_order_relaxed);
  const bool quad_recent = quad_ns >= 0 && now_ns - quad_ns < 500'000'000;
  const bool yield = active && now_ns - s_fresh_since >= 400'000'000 && quad_recent &&
                     (native_ns < 0 || last_ns - native_ns > 1'000'000'000);
  static bool s_active = false;
  if (yield != s_active) {
    s_active = yield;
    if (yield) {
      REXLOG_INFO(
          "native-scene: FMV playing - yielding to emulated output "
          "(MovieDecoder heartbeat; substitution did not engage)");
    } else {
      REXLOG_INFO("native-scene: FMV ended - native output resumes");
    }
  }
  return yield;
}

}  // namespace
// The photo-editor detection, shared by the yield and the photo-grab
// readback window: nullptr when inactive, else the name of the signal.
const char* PhotoEditorSignal(uint8_t* base) {
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0, f1 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) &&
            GuestTryLoadU32(base, beg + i * 20 + 4, &f1) && f0 == 1 && f1 == 11) {
          signal = "FE PhotoSelect push-state";
        }
      }
    }
  }
  if (signal == nullptr) {
    const int64_t last_ns = g_photo_replay_last_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    if (last_ns >= 0 && now_ns - last_ns < 250'000'000) {
      signal = "PhotoReplayController heartbeat";
    }
  }
  return signal;
}

namespace {

// ---- Native photo grab (photo_grab_native) ---------------------------------
// The grab content is produced NATIVELY while emulated-draw suppression stays
// ON: RenderScene downsamples the finished native frame into blur_tex[0]
// (1152x640: the blur space IS the game's screenshot raster), copies it to a
// persistently-mapped READBACK buffer (double-buffered, never a GPU drain),
// and the next armed frame CPU-tiles the completed one big-endian into the
// guest screenshot target that ScreenshotBackEnd::GrabScreenshot reads.
// Layout contract (traced through rex_cMsgNISLoadingOverlay_Deliver_SAXPBUStaticData_Hermes_N_Z_824FD550 -> the rex_renderengine_TextureHelper_Extract_QAA_NPAXIII_Z_82A7DC20 surface
// download): tiled Xenos 2D, 32bpp big-endian ARGB (guest byte order
// A,R,G,B); the game XGUntileTextureLevel's it into a linear buffer and
// hands the raw bytes to JPEG::WriteJPEG with no channel swap anywhere. The
// 160x90 gallery thumb is CPU-resized from this same image INSIDE
// GrabScreenshot (cImageOperateMan::ResizeJPEG); there is no second buffer
// to fill. Ground truth fetch words (from capture): 1152x640
// pitch 1152 k_8_8_8_8 tiled at 0x04911000 (the same surface the SDK's
// Import-Skater special case hardcodes). The write transform is
// byte-for-byte PROVEN offline against that capture's real readback
// content: tiling + [A,R,G,B] guest order reproduced all 0x2D0000 bytes
// with zero mismatches (alpha included: the emulated resolve also wrote
// 255).

// Photo window armed AND fulfilled natively this window (RenderScene's grab
// block produces/consumes while this is set; the watchdog in
// UpdatePhotoGrabWindow clears it and flips to the emulated fallback if
// native frames stop landing).
std::atomic<bool> g_photo_grab_native_armed{false};
// Last successful guest write (PerfClock ns): the watchdog liveness signal.
std::atomic<int64_t> g_grab_last_write_ns{-1};
// The photo display-card quad was in the 2D stream this instant (PerfClock
// ns, -1 = never). Stamped by Publish2dDraws (guest thread, runs every
// guest frame regardless of yields) whenever the card-signature quad, the
// 504x640 LINEAR JPEG-decode texture, unique to the photo display card,
// is present. Drives YieldForPhotoDisplay: the display screen renders
// EMULATED, because the game composes the framed card (white border /
// caption / logo) in a ONE-SHOT pitch-1280 pass whose timing proved
// unpredictable (observed anywhere from settle+0.8s to settle+4.0s; every
// readback-window attempt missed it and cost seconds of concurrent-pipeline
// lag). Yielded-emulated, the compose and the display chain just work
// GPU-side with no readbacks, and with the native renderer not running
// concurrently the frozen scene renders at the emulated pipeline's own
// healthy rate. (g_photo_card_seen_ns itself is defined next to
// g_photo_flow_frame; Publish2dDraws lives outside this anonymous
// namespace.)
// The display yield is active / when it last ended (watchdog interplay:
// grab writes legitimately stop while yielded, and get fresh grace after).
std::atomic<bool> g_photo_display_yielding{false};
std::atomic<int64_t> g_photo_display_yield_end_ns{-1};

// The game's screenshot targets, read from THIS build's own objects (the
// ScreenshotBackEnd grab, rex_Sk8_BE_ScreenshotBackEnd_GrabScreenshot_QAA_N_N_Z_826797D0): RenderMan = *(0x83083C5C); the
// full-size target texture = *(*(RenderMan + 1276) + 664), the downsampled
// one = *(*(RenderMan + 1568) + 664) (GrabScreenshot(downsample) picks one;
// both are filled). Each D3D texture object carries its GPU fetch constant at
// +0x1C (after Common/ReferenceCount/Fence/ReadFence/Identifier/BaseFlush/
// MipFlush): address, size, pitch and tiling come from the game.
// (The previous chain, *(0x83083C5C) + 380/384, was retail Skate 3's and
// failed validation on every grab of this build: no photo reached the
// magazine card and no profile picture was taken.)
struct GrabTarget {
  uint32_t addr = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  bool tiled = false;
};

int ResolveGrabTargets(uint8_t* base, GrabTarget out[2]) {
  // Retail Skate 3: GrabScreenshot reads its main-target dims from the postfx
  // render manager (mgr = *(0x83083C5C), width/height at mgr+380/384) and the
  // full-size target is the 1152x640 tiled k_8_8_8_8 surface at 0x04911000
  // (stable across every observed session). Validate the dims chain before
  // returning it: a write into guest memory must never be a guess. (sk83_na_m
  // resolves its targets through RenderMan+1276/+1568 instead; that layout is
  // the dev build's.)
  uint32_t mgr = 0, w = 0, h = 0;
  if (!GuestTryLoadU32(base, 0x83083C5C, &mgr) || mgr == 0 ||
      !GuestTryLoadU32(base, mgr + 380, &w) || !GuestTryLoadU32(base, mgr + 384, &h) ||
      w != RendererState::kBlurWidth || h != RendererState::kBlurHeight) {
    return 0;
  }
  GrabTarget t;
  t.addr = 0x04911000u;
  t.width = w;
  t.height = h;
  t.pitch = w;
  t.tiled = true;
  out[0] = t;
  return 1;
}

// CPU-tile one mapped readback image (R10G10B10A2 rows at kGrabRowPitch,
// kBlurWidth x kBlurHeight) into a guest screenshot target, box-filtered
// to the target's size, as big-endian ARGB (the byte order proven against
// the emulated resolve of the full-size target), through the 0xA0000000
// cached-physical view the CPU reads it from.
bool WriteGrabToGuest(uint8_t* base, const uint8_t* src, const GrabTarget& t) {
  constexpr uint32_t kW = RendererState::kBlurWidth;
  constexpr uint32_t kH = RendererState::kBlurHeight;
  static thread_local std::vector<uint8_t> staging;
  const uint32_t rows = t.tiled ? ((t.height + 31u) & ~31u) : t.height;
  staging.assign(size_t(t.pitch) * rows * 4, 0);
  const uint32_t fx = kW / t.width;
  const uint32_t fy = kH / t.height;
  for (uint32_t y = 0; y < t.height; ++y) {
    for (uint32_t x = 0; x < t.width; ++x) {
      uint32_t acc[3] = {};
      uint32_t cnt = 0;
      const uint32_t sx0 = x * kW / t.width;
      const uint32_t sy0 = y * kH / t.height;
      for (uint32_t dy = 0; dy < std::max(fy, 1u); ++dy) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(
            src + size_t(sy0 + dy) * RendererState::kGrabRowPitch);
        for (uint32_t dx = 0; dx < std::max(fx, 1u); ++dx) {
          const uint32_t v = row[sx0 + dx];
          acc[0] += (v >> 2) & 0xFFu;   // R: top 8 of bits 0..9
          acc[1] += (v >> 12) & 0xFFu;  // G: top 8 of bits 10..19
          acc[2] += (v >> 22) & 0xFFu;  // B: top 8 of bits 20..29
          ++cnt;
        }
      }
      const uint32_t r = acc[0] / cnt, g = acc[1] / cnt, b = acc[2] / cnt;
      const uint32_t off =
          t.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                        int32_t(x), int32_t(y), t.pitch, 2))
                  : (y * t.pitch + x) * 4u;
      if (size_t(off) + 4 > staging.size()) {
        continue;
      }
      // Little-endian byte order A,R,G,B = the guest's big-endian ARGB.
      const uint32_t px = 0xFFu | (r << 8) | (g << 16) | (b << 24);
      std::memcpy(staging.data() + off, &px, 4);
    }
  }
  return GuestTryCopy(base + (0xA0000000u | t.addr), staging.data(), staging.size());
}

// Guards the grab readback pair between the render thread (production /
// armed consume) and the guest thread (SyncWriteNewestGrab).
std::mutex g_grab_mutex;
// CPU copy of the newest COMPLETED native frame readback (R10G10B10A2 rows at
// kGrabRowPitch), refreshed by the render thread whenever a readback retires
// or is consumed. The game's screenshot read (SyncWriteNewestGrab) writes
// from this copy without ever waiting on a GPU fence: at the moment of the
// read the render thread's newest copies are routinely still in flight (the
// armed flow's first frames stall on synchronous shader compilation).
std::vector<uint8_t> g_grab_cache;


// The photo-grab window (see skate3_native_render_scene_photo_readback /
// photo_grab_native): while the photo editor is up OR a TakePhoto fired
// within the last few seconds, the game CPU-reads the resolved screenshot
// target from guest memory to JPEG-encode the photo. Two ways to fulfill it:
//  - NATIVE (photo_grab_native, default): RenderScene writes the target
//    itself from the native output; emulated-draw suppression stays ON, so
//    the emulated pipeline never renders the scene+postfx concurrently (that
//    concurrent 3x3-scale render was the editor's 40-50 fps, proven by the
//    kFast run: zero readback drains, same fps).
//  - EMULATED fallback: (a) arm the SDK's forced small-resolve CPU readback
//    so the game's own resolve lands in guest memory, and (b) lift
//    emulated-draw suppression so the passes that render the target execute.
// Runs every frame from RenderScene regardless of the yield decisions. A
// watchdog flips native -> emulated mid-window if no native grab write has
// landed recently (mode toggle, unexpected yield, readback failure).
void UpdatePhotoGrabWindow(uint8_t* base) {
  // The grab target is 1152x640x4 = 0x2D0000 bytes (the same surface the
  // SDK's Import-Skater special case reads); the thumb path is smaller.
  // Framebuffer-sized resolves (0x384000 at 1280x720) stay excluded.
  constexpr int32_t kForceReadbackMaxLength = 0x2D0000;
  static bool s_armed = false;
  static bool s_emulated_armed = false;
  static bool s_burst_armed = false;
  static int64_t s_arm_ns = 0;
  static int64_t s_last_run_ns = 0;
  static int32_t s_readback_saved = 0;
  static bool s_suppress_saved = false;
  static bool s_halfpx_saved = false;
  static int32_t s_burst_max_saved = 0;
  static bool s_burst_suppress_saved = false;
  static bool s_burst_halfpx_saved = false;
  static int64_t s_burst_open_ns = 0;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const auto disarm_burst = [&] {
    s_burst_armed = false;
    if (s_burst_max_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  s_burst_max_saved);
    }
    if (s_burst_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, true);
    }
    if (!s_burst_halfpx_saved) {
      REXCVAR_SET(readback_resolve_half_pixel_offset, false);
    }
  };
  // This function only runs while the native renderer is producing frames
  // (RenderScene). A gap means the native path was toggled off or yielding
  // - grab writes legitimately paused, so give the watchdog a fresh grace
  // period on resume instead of instantly tripping the fallback
  // (an emulated/native comparison toggle on the display
  // screen fired the watchdog 2 ms after switching back).
  if (s_armed && s_last_run_ns != 0 && now_ns - s_last_run_ns > 300'000'000) {
    s_arm_ns = now_ns;
  }
  s_last_run_ns = now_ns;
  bool want = false;
  if (REXCVAR_GET(skate3_native_render_scene_photo_readback)) {
    want = PhotoEditorSignal(base) != nullptr;
    if (!want) {
      const int64_t last_ns = g_take_photo_last_ns.load(std::memory_order_relaxed);
      if (last_ns >= 0) {
        want = now_ns - last_ns < 3'000'000'000;
      }
    }
  }
  // Same window arms the photo postfx constant capture (CapturePfxConstants
  // runs on the guest thread at the draw-done hook).
  g_photo_flow_frame.store(want, std::memory_order_relaxed);
  const auto arm_emulated = [&] {
    s_readback_saved = REXCVAR_GET(native_render_force_resolve_readback_max_length);
    if (s_readback_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  kForceReadbackMaxLength);
    }
    s_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, false);
    }
    // Sample host-pixel CENTERS in the scaled->1x readback extraction while
    // the window is armed (photo quality: undoes the D3D9 half-pixel shift
    // that becomes a scale/2-pixel offset at scaled resolutions).
    s_halfpx_saved = REXCVAR_GET(readback_resolve_half_pixel_offset);
    if (!s_halfpx_saved) {
      REXCVAR_SET(readback_resolve_half_pixel_offset, true);
    }
    s_emulated_armed = true;
  };
  // Shader compilation goes SYNCHRONOUS for the whole photo flow; the
  // menus lesson applied here: with async_shader_compilation on, the d3d12
  // command processor SKIPS any draw whose pipeline is still compiling, and
  // the photo display screen's framed-card compose is a ONE-SHOT pass.
  // Its shaders have been suppressed-cold in every native session (and the
  // pipeline caches were invalidated by SDK rebuilds), so the compose's
  // draws were skipped at the exact moment they finally ran, which is why
  // the card stayed un-framed EVEN IN EMULATED rendering.
  // One-time compile stalls during a photo UI moment are invisible.
  static bool s_async_forced = false;
  static bool s_async_saved = false;
  if (want && !s_async_forced) {
    s_async_saved = REXCVAR_GET(async_shader_compilation);
    if (s_async_saved) {
      REXCVAR_SET(async_shader_compilation, false);
      REXLOG_INFO(
          "native-scene: photo flow - shader compilation synchronous (the "
          "one-shot card compose can't skip still-compiling draws)");
    }
    s_async_forced = true;
  } else if (!want && s_async_forced) {
    if (s_async_saved) {
      REXCVAR_SET(async_shader_compilation, true);
    }
    s_async_forced = false;
  }
  if (want && !s_armed) {
    s_armed = true;
    s_arm_ns = now_ns;
    g_grab_last_write_ns.store(-1, std::memory_order_relaxed);
    g_r.grab_writes = 0;
    g_r.grab_cpu_us = 0;
    // Drop any readback still pending from a previous window; its content
    // is stale (pre-window) and must never land ahead of fresh frames.
    g_r.grab_pending[0] = false;
    g_r.grab_pending[1] = false;
    if (REXCVAR_GET(skate3_native_render_scene_photo_grab_native) &&
        !g_r.grab_failed) {
      g_photo_grab_native_armed.store(true, std::memory_order_relaxed);
      REXLOG_INFO(
          "native-scene: photo flow - NATIVE grab armed (suppression stays "
          "on; native output CPU-tiled into the guest screenshot target; "
          "display card yields to emulated; shutter burst on grab request)");
    } else {
      arm_emulated();
      REXLOG_INFO(
          "native-scene: photo flow - forcing small-resolve CPU readback "
          "(the photo grab reads the resolved screenshot target from guest "
          "memory){}",
          s_suppress_saved ? " + emulated draw suppression OFF" : "");
    }
  } else if (!want && s_armed) {
    s_armed = false;
    if (s_burst_armed) {
      disarm_burst();
    }
    if (g_photo_grab_native_armed.exchange(false, std::memory_order_relaxed)) {
      REXLOG_INFO(
          "native-scene: photo flow ended - native grab wrote {} frames "
          "(avg tile+write {} us)",
          g_r.grab_writes,
          g_r.grab_writes ? g_r.grab_cpu_us / g_r.grab_writes : 0);
    }
    if (s_emulated_armed) {
      s_emulated_armed = false;
      if (s_readback_saved <= 0) {
        REXCVAR_SET(native_render_force_resolve_readback_max_length,
                    s_readback_saved);
      }
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (!s_halfpx_saved) {
        REXCVAR_SET(readback_resolve_half_pixel_offset, false);
      }
      REXLOG_INFO(
          "native-scene: photo flow ended - resolve readback{} restored",
          s_suppress_saved ? " + suppression" : "");
    }
  }
  // Watchdog: native mode armed but no grab write has landed within 700 ms
  // of the window opening / the last write / the display yield ending;
  // native frames stopped while the native output should be live
  // (persistent yield, readback failure). Flip to the emulated fallback so
  // the photo still works; one-way for the rest of this window. Skipped
  // while the display-card yield is active: grab writes legitimately stop
  // there (the photo was already taken and saved).
  if (s_armed && !s_emulated_armed &&
      !g_photo_display_yielding.load(std::memory_order_relaxed) &&
      g_photo_grab_native_armed.load(std::memory_order_relaxed)) {
    const int64_t last_write = g_grab_last_write_ns.load(std::memory_order_relaxed);
    const int64_t yield_end =
        g_photo_display_yield_end_ns.load(std::memory_order_relaxed);
    const int64_t basis = std::max(std::max(last_write, s_arm_ns), yield_end);
    if (now_ns - basis > 700'000'000) {
      g_photo_grab_native_armed.store(false, std::memory_order_relaxed);
      if (s_burst_armed) {
        disarm_burst();
      }
      arm_emulated();
      REXLOG_WARN(
          "native-scene: photo flow - native grab produced no frames for "
          "700 ms; falling back to the forced-readback window{}",
          s_suppress_saved ? " + suppression lifted" : "");
    }
  }
  // SHUTTER BURST: the game builds the framed card as a dedicated frame
  // sequence at the grab - request (hooked: OnPhotoGrabRequest) -> render
  // the shot + card-composite passes -> resolve -> GrabScreenshot/
  // OnScreenShot CPU-read the resolves and CPU-compose the card texture.
  // Under the native path those frames are suppressed and nothing lands in
  // CPU memory, so the compose blends zeros = the un-framed card (proven:
  // the control run's composite CPU copies all land inside this window;
  // every post-hoc timing window missed it because the CONSUMER runs
  // immediately at the shutter). For ~1.5 s from the request: suppression
  // fully lifted + kFull readbacks up to 0x2D0000 (the full composite
  // class; the handful of drains sits inside the shutter freeze the game
  // already has). The emulated 0x04911000 resolve may overwrite the
  // natively-written photo during the burst, same frozen scene, emulated
  // 3x3 quality, the pre-native-grab standard, acceptable.
  if (s_armed && !s_emulated_armed &&
      g_photo_grab_native_armed.load(std::memory_order_relaxed)) {
    const int64_t req = g_photo_grab_request_ns.load(std::memory_order_relaxed);
    // EVENT-DRIVEN close: GrabScreenshot COMPLETING (post-call stamp,
    // OnPhotoGrabDone) means every remaining card-build step, the
    // OnScreenShot compose included, is CPU-side work on memory the burst
    // already copied. The burst therefore spans exactly request ->
    // grab-done: the 1-3 shot frames, the physical minimum, all inside the
    // game's own shutter freeze, instead of a timer running into the
    // display fly-in (the user-visible lag spike). A 150 ms grace after
    // grab-done covers the aux-target grab some flows issue on the
    // following frame; the 1.5 s cap remains only as the fallback for a
    // request that never grabs.
    const int64_t done_ns = g_photo_grab_done_ns.load(std::memory_order_relaxed);
    const bool grab_done = done_ns > req && now_ns - done_ns > 150'000'000;
    const bool burst_want =
        req >= 0 && now_ns - req < 1'500'000'000 && !grab_done;
    if (burst_want && !s_burst_armed) {
      s_burst_armed = true;
      s_burst_max_saved =
          REXCVAR_GET(native_render_force_resolve_readback_max_length);
      if (s_burst_max_saved <= 0) {
        REXCVAR_SET(native_render_force_resolve_readback_max_length, 0x2D0000);
      }
      s_burst_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
      if (s_burst_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, false);
      }
      s_burst_halfpx_saved = REXCVAR_GET(readback_resolve_half_pixel_offset);
      if (!s_burst_halfpx_saved) {
        REXCVAR_SET(readback_resolve_half_pixel_offset, true);
      }
      s_burst_open_ns = now_ns;
      REXLOG_INFO(
          "native-scene: photo grab REQUEST - shutter burst OPEN "
          "(suppression lifted + <=0x2D0000 kFull readbacks; the shot + "
          "card-composite frames render and land in guest memory; closes "
          "when the grab completes)");
    } else if (!burst_want && s_burst_armed) {
      disarm_burst();
      REXLOG_INFO(
          "native-scene: shutter burst closed after {} ms ({}) - "
          "suppression + readback restored",
          (now_ns - s_burst_open_ns) / 1'000'000,
          grab_done ? "grab completed" : "1.5 s cap");
    }
  }
}

// Photo display-card screen: yield to the emulated output while the framed
// card is up (card-signature quad in the 2D stream, stamped by
// Publish2dDraws). The game composes the framed card in a ONE-SHOT
// pitch-1280 pass at an unpredictable moment after display-open
// (observed settle+0.8s to settle+4.0s); chasing it with
// suppression-lift + forced-readback windows never worked and cost
// seconds of concurrent-pipeline lag. Yielded-emulated, the compose and the
// display chain run GPU-side with no readbacks, exactly as on console, and
// with the native renderer idle the frozen scene renders at the emulated
// pipeline's own rate. A short PRE-ROLL lifts suppression while the native
// output still presents, so the emulated framebuffer has repainted (the
// card fly-in, live) before the swap, no stale-frame flash (the FMV-yield
// lesson).
bool YieldForPhotoDisplay() {
  constexpr int64_t kCardGoneNs = 250'000'000;
  constexpr int64_t kPreRollNs = 250'000'000;
  static bool s_preroll = false;
  static bool s_suppress_saved = false;
  static int64_t s_preroll_ns = 0;
  // Default OFF since the shutter burst was added: the framed card
  // is CPU-composed into the card texture at the grab, and the native 2D
  // replay samples that same texture; the display screen renders fully
  // native (card + blurred backdrop + buttons). The yield remains as a
  // safety hatch for display-screen regressions.
  if (!REXCVAR_GET(skate3_native_render_scene_photo_display_yield)) {
    if (s_preroll) {
      s_preroll = false;
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (g_photo_display_yielding.exchange(false, std::memory_order_relaxed)) {
        g_photo_display_yield_end_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                PerfClock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);
      }
    }
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const int64_t seen = g_photo_card_seen_ns.load(std::memory_order_relaxed);
  const bool card_up = g_photo_flow_frame.load(std::memory_order_relaxed) &&
                       seen >= 0 && now_ns - seen < kCardGoneNs;
  if (!card_up) {
    if (s_preroll) {
      s_preroll = false;
      if (s_suppress_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
      }
      if (g_photo_display_yielding.exchange(false, std::memory_order_relaxed)) {
        g_photo_display_yield_end_ns.store(now_ns, std::memory_order_relaxed);
        REXLOG_INFO(
            "native-scene: photo display card gone - native output resumes "
            "(suppression restored)");
      }
    }
    return false;
  }
  if (!s_preroll) {
    s_preroll = true;
    s_preroll_ns = now_ns;
    s_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, false);
    }
    REXLOG_INFO(
        "native-scene: photo display card up - emulated pre-roll "
        "(suppression lifted; yielding to the emulated output in {} ms so "
        "the game's own card compose + display chain render there)",
        kPreRollNs / 1'000'000);
  }
  if (now_ns - s_preroll_ns < kPreRollNs) {
    return false;
  }
  if (!g_photo_display_yielding.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("native-scene: photo display - yielded to the emulated output");
  }
  return true;
}

bool YieldForPhotoEditor(uint8_t* base) {
  // The native photo-fx chain (photo_fx.hlsl, photo_native cvar) takes
  // precedence: the editor stays native and RenderScene applies the game's
  // postfx as exact ported passes with live-captured constants.
  const bool native_fx = REXCVAR_GET(skate3_native_render_scene_photo_native);
  if (!native_fx && !REXCVAR_GET(skate3_native_render_scene_photo_yield)) {
    return false;
  }
  static bool s_in_photo_editor = false;
  const char* signal = PhotoEditorSignal(base);
  const bool photo_active = signal != nullptr;
  if (photo_active != s_in_photo_editor) {
    s_in_photo_editor = photo_active;
    if (photo_active) {
      REXLOG_INFO(
          "native-scene: photo editor - {} ({})",
          native_fx ? "staying NATIVE (ported postfx chain)"
                    : "yielding to emulated output (the game's postfx applies "
                      "the photo effects)",
          signal);
    } else {
      REXLOG_INFO("native-scene: photo editor closed");
    }
  }
  if (native_fx) {
    return false;
  }
  return photo_active && REXCVAR_GET(skate3_native_render_scene_photo_yield);
}

// Create-a-skater editor (the 'Edit Skater' screen: skater + garage wall,
// Skin/Clothing/Body Mods panels): a special FE renderer the native scene
// does not model; the skater draws with editor-only CAC shader variants
// (cacstamp_skin_nisPS / cac_cloth_nisPS / cac_face_nisPS...) whose constant
// layouts differ from gameplay (the cacstamp map shifted +1 row: light c10,
// key c16, SH c25..c33 scale c22.y, tint c24, alpha c23.x, measured
// in capture), so every char-lighting capture is rejected and
// the skater rendered legacy-shaded (grey tank, pale skin, black jeans). The
// editor also runs per-frame texture-space composite passes (cac*_unwrapPS
// paint the edited garment/skin art into textures) and its own DOF postfx;
// live-edit previews are only correct with the full emulated chain. Yield
// while it is up (photo-editor class: scene is a small frozen room, emulated
// performance is fine; no cache or takeover-gate side effects; native
// resumes the frame after the editor closes, and the un-suppressed yield
// window also lets the game re-render the team-box skater portrait RTT that
// follows an accepted edit).
//
// Detection: stateless per-frame poll of the FrontEndManager push-state
// stack (same struct as YieldForPhotoEditor above) for a record with screen
// id 15, surveyed across the 40 gsnaps on hand (gameplay, pause root 56,
// team screen 63, photo editor {1,11}, FMV): id 15 appears exactly in the
// CAS editor capture and nowhere else.
bool YieldForCasEditor(uint8_t* base) {
  if (!REXCVAR_GET(skate3_native_render_scene_cas_yield)) {
    return false;
  }
  static bool s_in_cas_editor = false;
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
          signal = "FE push-state id 15";
        }
      }
    }
  }
  if (signal == nullptr) {
    // Shader heartbeat: the editor's own "_nis" pixel shaders were set
    // within the last 0.5 s (see IsCasEditorPs): covers editor entry
    // points that use a different FE screen id (the startup new-game flow
    // stayed NATIVE on the FE detection alone).
    const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
    if (last_ns >= 0) {
      const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
      if (now_ns - last_ns < 500'000'000) {
        signal = "CAS _nis shader heartbeat";
      }
    }
  }
  const bool cas_active = signal != nullptr;
  if (cas_active != s_in_cas_editor) {
    s_in_cas_editor = cas_active;
    if (cas_active) {
      REXLOG_INFO(
          "native-scene: create-a-skater editor - yielding to emulated "
          "output ({}; editor CAC shading + live composite passes render "
          "exactly there)",
          signal);
    } else {
      REXLOG_INFO(
          "native-scene: create-a-skater editor closed - native output "
          "resumes");
    }
  }
  return cas_active;
}

// Services the debug-dialog texture/mesh cache flushes. (The old
// retired-buffer drain / SRV-slot recycling is gone: deferred destruction
// is backend-internal now: Device::DestroyDeferred.)
void ReleaseRetiredAndFlushCaches(const NativeGuestOutputRenderContext& context) {
  if (skate3::vr::CurrentEye() == 1) return;  // defer debug flush until the next pair
  // Debug-dialog cache flushes: retire every cached decode (freed once the
  // GPU is done with the current submission) so hot-toggled decode settings
  // rebuild the world with the new rules this frame.
  if (g_flush_textures.exchange(false, std::memory_order_relaxed)) {
    const uint64_t submission = context.device->CurrentSubmission();
    for (auto& [key, t] : g_r.tex_store) {
      RetireGuestTexture(t, submission);
    }
    g_r.tex_store.clear();
    g_r.tex_routes.clear();
    g_r.words_sticky.clear();
    g_r.tex_sticky.clear();
    g_r.tex_pending_first.clear();
    REXLOG_INFO("native-scene: texture cache flushed (debug dialog)");
  }
  if (g_flush_meshes.exchange(false, std::memory_order_relaxed)) {
    for (auto& [key, m] : g_r.meshes) {
      if (m.vb) g_r.device->DestroyDeferred(m.vb);
      if (m.ib) g_r.device->DestroyDeferred(m.ib);
    }
    g_r.meshes.clear();
    {
      std::lock_guard<std::mutex> lock(g_mesh_pool_mutex);
      for (const PooledMeshBuffer& p : g_mesh_pool) {
        g_r.device->DestroyDeferred(p.buffer);
      }
      g_mesh_pool.clear();
    }
    ClearItemCache();  // decode-affecting toggles should re-walk items too
    REXLOG_INFO("native-scene: mesh cache flushed (debug dialog)");
  }
}

  // ---- Dynamic-shadow atlas pass ----
  // Renders the frame's dynamic casters (skinned characters + rigid
  // non-identity-world props: exactly the game's caster list) into the
  // three cascade tiles with the captured light rows, then applies the
  // game's coverage blur + depth dilation. Runs before the main pass so the
  // scene shader can sample the finished atlas.
// Read-only resolved-texture lookup (route -> store entry) for the caster
// passes' alpha-test binding: the main pass owns decode scheduling and
// live guest reads; an unresolved diffuse just casts opaque for the frames
// a first-sight decode takes.
static nrhi::TextureView* LookupResolvedTexture(uint32_t tex_ptr) {
  if (tex_ptr == 0) {
    return nullptr;
  }
  const RendererState::TexRoute* rit = g_r.tex_routes.find(tex_ptr);
  if (rit == nullptr) {
    return nullptr;
  }
  auto sit = g_r.tex_store.find(rit->key);
  if (sit == g_r.tex_store.end() || !sit->second.valid) {
    return nullptr;
  }
  return sit->second.srv;
}

// Native static sun-shadow map: one camera-centered ortho depth pass over
// the STATIC world items along the material sun (the captured c6 row:
// the direction the world materials and their baked shadows are lit
// from). The game's own cascade transforms are a stylized near-vertical
// projection fit to a ~12 m height window (measured ~43 degrees off the
// material sun): adequate for the dynamic casters they were built for,
// but tall static geometry projected through them shadows its own
// plan-view footprint. This map gives static shade a true sun axis, a
// full-range depth window (no clamp blobs) and its own resolution;
// receivers min it with the CSM term (SampleStaticSun), bottoming out at
// the configured strength. Re-rendered every frame; the box follows the
// camera, with the origin snapped to the texel grid so the static raster
// never swims sub-texel (foliage shimmer).
static void RenderStaticSunMap(const NativeGuestOutputRenderContext& context,
                               const FrameScene& scene, uint32_t bone_region,
                               int32_t debug_mode, uint64_t frame_number) {
  g_r.static_sun_valid = false;
  if (g_r.static_sun == nullptr || g_r.pso_static_caster == nullptr ||
      g_r.pso_static_caster_clip == nullptr || debug_mode != 0 ||
      !scene.shadow_valid ||
      !REXCVAR_GET(skate3_native_render_scene_shadows) ||
      !REXCVAR_GET(skate3_native_render_scene_shadow_static_casters)) {
    return;
  }
  // Sun axis = the material sun (c6). It is the only sun-like direction
  // in the captured data: the game's cascade transforms AND its baked
  // static-shade (world-shadow) projection are both the same stylized
  // near-vertical axis, measured ~43 degrees off c6, projecting statics
  // along either shadows their plan-view footprint. Shadows cast along c6
  // are geometrically consistent with the materials' lighting; where the
  // baked lightmaps were authored with yet another sun the two can
  // disagree, which the strength floor and the receivers' min-clamp keep
  // plausible rather than doubled.
  float sun[3] = {scene.shadow_rows[24], scene.shadow_rows[25],
                  scene.shadow_rows[26]};
  const float slen =
      std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  bool sun_usable = slen >= 0.5f;
  if (sun_usable) {
    for (float& v : sun) {
      v /= slen;
    }
    sun_usable = sun[1] >= 0.08f;  // at/below the horizon = no sun term
  }
  // Single-frame capture outliers (a foreign light bank riding the row
  // capture, or a mid-transition frame) must not flap the term or the
  // built axis: dropping the whole map for one frame flashed every static
  // shadow lighter, and an unratelimited axis-delta rebuild rebuilt the map
  // along the outlier axis and back. Serve the cached map through short
  // outlier runs; a persistent change (genuine dusk, time-of-day scripts)
  // passes after a few frames.
  static uint32_t s_unusable_run = 0;
  if (!sun_usable) {
    if (g_r.nsm_built_radius > 0.0f && ++s_unusable_run <= 30) {
      g_r.static_sun_valid = true;  // hold the cached map + stored rows
      static std::atomic<uint32_t> s_hold_logs{0};
      const uint32_t n = s_hold_logs.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 255u) == 0) {
        REXLOG_INFO(
            "native-scene: static sun capture unusable (len={:.2f} y={:.2f}) - "
            "serving cached map (run={} n={})",
            slen, slen >= 0.5f ? sun[1] : 0.0f, s_unusable_run, n + 1);
      }
    }
    return;
  }
  s_unusable_run = 0;
  if (g_r.nsm_built_radius > 0.0f) {
    const float held_dot = sun[0] * g_r.nsm_sun[0] + sun[1] * g_r.nsm_sun[1] +
                           sun[2] * g_r.nsm_sun[2];
    if (held_dot < 0.999995f) {
      // The captured axis disagrees with the built one: require it to
      // persist two consecutive build frames before it can drive a
      // rebuild; until then keep working along the held axis.
      static float s_cand[3] = {};
      static uint64_t s_cand_frame = 0;
      const float cand_dot =
          sun[0] * s_cand[0] + sun[1] * s_cand[1] + sun[2] * s_cand[2];
      const bool confirmed = s_cand_frame + 1 == frame_number && cand_dot > 0.9999f;
      std::memcpy(s_cand, sun, sizeof(s_cand));
      s_cand_frame = frame_number;
      if (!confirmed) {
        static std::atomic<uint32_t> s_jump_logs{0};
        const uint32_t n = s_jump_logs.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: static sun axis jump held (dot={:.6f} n={})",
              held_dot, n + 1);
        }
        std::memcpy(sun, g_r.nsm_sun, sizeof(sun));
      }
    }
  }
  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};
  const float xll = std::sqrt(xl[0] * xl[0] + xl[2] * xl[2]);
  if (xll > 1e-5f) {
    xl[0] /= xll;
    xl[2] /= xll;
  } else {
    xl[0] = 1.0f;
    xl[2] = 0.0f;
  }
  const float yl[3] = {zl[1] * xl[2] - zl[2] * xl[1],
                       zl[2] * xl[0] - zl[0] * xl[2],
                       zl[0] * xl[1] - zl[1] * xl[0]};
  const float radius = std::clamp(
      float(REXCVAR_GET(skate3_native_render_scene_shadow_static_radius)),
      20.0f, 500.0f);
  // Depth window +-2r along the axis: covers cliffs/towers far above the
  // box at ~16-bit millimeter precision.
  const float depth_half = radius * 2.0f;
  const float texel_m = 2.0f * radius / float(g_r.static_sun_size);
  const float* cam = scene.cam_pos;
  // Upsert this frame's visible statics into the persistent caster cache
  // (same static filter as the world-shadow map pass). scene.items is the
  // game's view-culled list; drawing the map from it directly popped
  // shadows with the camera; the cache keeps once-seen geometry casting
  // from any direction. A new or content-changed record marks the map
  // dirty for a rebuild.
  for (const DrawItem& item : scene.items) {
    // proxyworld (env 11) is the game's far-LOD stand-in for regions not
    // loaded in detail; once the detailed region streams in the game stops
    // drawing it, but a cached record kept it casting over the real
    // geometry (its GPU mesh stays resident through the LRU touch below):
    // the skater shadowed with nothing overhead. Live-only geometry never
    // enters the persistent cache.
    if (item.transparent || item.unlit || item.cloth_quads || item.water ||
        item.water_ocean != 0 || item.skinned || item.ropa ||
        item.char_family != 0 || item.dynobj != 0 || item.env_family == 11) {
      continue;
    }
    // Key: mesh + quantized world translation (placed instances of one
    // mesh are distinct casters; a re-streamed identical placement maps
    // back onto its record).
    const auto q = [](float v) {
      return uint64_t(int64_t(std::llround(double(v) * 8.0)));
    };
    uint64_t key = uint64_t(item.mesh) * 0x9E3779B97F4A7C15ull;
    key ^= q(item.world[12]) * 0xC2B2AE3D27D4EB4Full;
    key ^= (q(item.world[13]) + 0x165667B19E3779F9ull) * 0x27D4EB2F165667C5ull;
    key ^= (q(item.world[14]) + 0x9E3779B97F4A7C15ull) * 0x85EBCA77C2B2AE63ull;
    RendererState::StaticCaster& rec = g_r.static_casters[key];
    if (rec.mesh == 0 || rec.fingerprint != item.fingerprint) {
      rec.mesh = item.mesh;
      rec.fingerprint = item.fingerprint;
      std::memcpy(rec.world, item.world, sizeof(rec.world));
      std::memcpy(rec.bbox_min, item.bbox_min, sizeof(rec.bbox_min));
      std::memcpy(rec.bbox_max, item.bbox_max, sizeof(rec.bbox_max));
      rec.diffuse_tex = item.diffuse_tex;
      rec.clip = item.env_family == 7 || item.env_family == 9 ||
                 item.env_family == 10;
      rec.draws.assign(item.draws.begin(), item.draws.end());
      g_r.nsm_dirty = true;
    }
    rec.last_seen_frame = frame_number;
  }
  // Scene-switch purge: the cache keeps casters the game does not draw
  // because they are OUTSIDE its view, but a caster lying fully inside the
  // view is one the game would draw if it still existed. Leaving gameplay
  // for create-a-skater stops the level's statics from rendering while the
  // editor set sits in the same world space; the cached level kept casting
  // onto the editor skater (shadowed until the 30000-frame staleness
  // timeout, ~100 s at the native frame rate). Ordinary occlusion hides a
  // minority of the in-view casters; when the game draws under a quarter
  // of them for 30 consecutive builds the scene has switched, and the
  // in-view casters it no longer draws are dropped.
  {
    const float* vp = scene.view_proj;
    std::vector<uint64_t> in_view_absent;
    uint32_t in_view = 0;
    uint32_t in_view_drawn = 0;
    for (const auto& [key, rec] : g_r.static_casters) {
      bool inside = true;
      for (int c = 0; c < 8 && inside; ++c) {
        const float lp[3] = {(c & 1) ? rec.bbox_max[0] : rec.bbox_min[0],
                             (c & 2) ? rec.bbox_max[1] : rec.bbox_min[1],
                             (c & 4) ? rec.bbox_max[2] : rec.bbox_min[2]};
        float wp[3];
        for (int a = 0; a < 3; ++a) {
          wp[a] = lp[0] * rec.world[0 * 4 + a] + lp[1] * rec.world[1 * 4 + a] +
                  lp[2] * rec.world[2 * 4 + a] + rec.world[12 + a];
        }
        float clip[4];
        for (int a = 0; a < 4; ++a) {
          clip[a] = wp[0] * vp[0 * 4 + a] + wp[1] * vp[1 * 4 + a] +
                    wp[2] * vp[2 * 4 + a] + vp[12 + a];
        }
        inside = clip[3] > 0.0f && std::fabs(clip[0]) <= 0.95f * clip[3] &&
                 std::fabs(clip[1]) <= 0.95f * clip[3] && clip[2] > 0.0f &&
                 clip[2] < clip[3];
      }
      if (!inside) {
        continue;
      }
      ++in_view;
      if (rec.last_seen_frame == frame_number) {
        ++in_view_drawn;
      } else {
        in_view_absent.push_back(key);
      }
    }
    if (in_view >= 8 && in_view_drawn * 4 < in_view) {
      ++g_r.static_casters_absent_run;
    } else {
      g_r.static_casters_absent_run = 0;
    }
    if (g_r.static_casters_absent_run >= 30 && !in_view_absent.empty()) {
      for (const uint64_t key : in_view_absent) {
        g_r.static_casters.erase(key);
      }
      g_r.nsm_dirty = true;
      g_r.nsm_last_build_frame = 0;  // rebuild now, not after the dirty throttle
      g_r.static_casters_absent_run = 0;
      REXLOG_INFO("native-scene: static caster cache dropped {} casters the game "
                  "no longer draws in view (scene switch)",
                  in_view_absent.size());
    }
  }
  // Cache maintenance: distance eviction (region streamed away) plus a
  // long staleness timeout, which bounds ghost shadows from content the
  // game actually removed (deleted park-editor objects) at the cost of a
  // late pop for casters never re-viewed.
  if (frame_number >= g_r.static_casters_sweep_frame) {
    g_r.static_casters_sweep_frame = frame_number + 600;
    const float evict_r = radius * 6.0f;
    for (auto it = g_r.static_casters.begin();
         it != g_r.static_casters.end();) {
      const RendererState::StaticCaster& rec = it->second;
      const float ex = rec.world[12] - cam[0];
      const float ey = rec.world[13] - cam[1];
      const float ez = rec.world[14] - cam[2];
      const bool far_away = ex * ex + ey * ey + ez * ez > evict_r * evict_r;
      const bool stale = frame_number > rec.last_seen_frame + 30000;
      if (far_away || stale) {
        it = g_r.static_casters.erase(it);
        g_r.nsm_dirty = true;
      } else {
        ++it;
      }
    }
  }
  // Cross-frame cache: the map re-renders only when something it depends
  // on changed. Between rebuilds the stored rows and the map contents are
  // served as-is; statics and the sun are near-constant, so this removes
  // almost the entire steady-state caster cost (and freezes the raster,
  // so foliage dapple cannot shimmer frame to frame).
  // Dirty (content changed) rebuilds are RATE-LIMITED: dense areas stream
  // new casters near-continuously, and an unthrottled dirty flag degraded
  // to a full map redraw every frame. A <=30-frame shadow latency for
  // newly streamed geometry is invisible; drift/sun rebuilds stay
  // immediate.
  bool rebuild = g_r.nsm_built_radius <= 0.0f ||
                 std::fabs(radius - g_r.nsm_built_radius) > 0.5f ||
                 frame_number >= g_r.nsm_rebuild_frame ||
                 (g_r.nsm_dirty &&
                  frame_number >= g_r.nsm_last_build_frame + 30);
  if (!rebuild) {
    // Camera drift from the built center, measured in the map plane: the
    // INNER tile (radius/6) must keep covering the player's
    // surroundings, so the threshold rides its extent.
    const float dcx = cam[0] - g_r.nsm_center[0];
    const float dcy = cam[1] - g_r.nsm_center[1];
    const float dcz = cam[2] - g_r.nsm_center[2];
    const float du = xl[0] * dcx + xl[1] * dcy + xl[2] * dcz;
    const float dv = yl[0] * dcx + yl[1] * dcy + yl[2] * dcz;
    rebuild = std::max(std::fabs(du), std::fabs(dv)) > radius * 0.0667f;
  }
  if (!rebuild) {
    rebuild = sun[0] * g_r.nsm_sun[0] + sun[1] * g_r.nsm_sun[1] +
                  sun[2] * g_r.nsm_sun[2] <
              0.999995f;
  }
  if (!rebuild) {
    g_r.static_sun_valid = true;  // serve the cached map + stored rows
    return;
  }
  g_r.nsm_dirty = false;
  g_r.nsm_rebuild_frame = frame_number + 600;
  g_r.nsm_last_build_frame = frame_number;
  g_r.nsm_built_radius = radius;
  std::memcpy(g_r.nsm_sun, sun, sizeof(g_r.nsm_sun));
  float o[3] = {cam[0], cam[1], cam[2]};
  const float cx = xl[0] * o[0] + xl[1] * o[1] + xl[2] * o[2];
  const float cy = yl[0] * o[0] + yl[1] * o[1] + yl[2] * o[2];
  const float dx = cx - std::round(cx / texel_m) * texel_m;
  const float dy = cy - std::round(cy / texel_m) * texel_m;
  for (int a = 0; a < 3; ++a) {
    o[a] -= xl[a] * dx + yl[a] * dy;
  }
  std::memcpy(g_r.nsm_center, o, sizeof(g_r.nsm_center));
  // world -> map rows, shared by this caster pass and the receivers
  // (b1 nsm_x/y/z): uc = dot(row.xyz, wp) + row.w.
  float rows[12];
  for (int a = 0; a < 3; ++a) {
    rows[0 + a] = xl[a] / radius;
    rows[4 + a] = yl[a] / radius;
    rows[8 + a] = zl[a] / (2.0f * depth_half);
  }
  rows[3] = -(rows[0] * o[0] + rows[1] * o[1] + rows[2] * o[2]);
  rows[7] = -(rows[4] * o[0] + rows[5] * o[1] + rows[6] * o[2]);
  rows[11] = 0.5f - (rows[8] * o[0] + rows[9] * o[1] + rows[10] * o[2]);
  std::memcpy(g_r.nsm_rows, rows, sizeof(rows));
  g_r.nsm_depth_range = 2.0f * depth_half;
  g_r.nsm_radius = radius;
  // View-proj columns from the rows (same construction as the CSM
  // lightvp; clip.y lands in the sampler's v = -y/2 + 0.5 convention via
  // the raster y-flip).
  float vp[16] = {};
  for (int r = 0; r < 3; ++r) {
    vp[r * 4 + 0] = rows[0 + r];
    vp[r * 4 + 1] = rows[4 + r];
    vp[r * 4 + 2] = rows[8 + r];
  }
  vp[12] = rows[3];
  vp[13] = rows[7];
  vp[14] = rows[11];
  vp[15] = 1.0f;
  // Finer-tile view-projections: same origin, radius/2 and radius/6,
  // exact rescales of the far transform's X/Y columns (the shader selects
  // tiles by the same ratios; keep in sync with its
  // kRatioInner/kRatioMid). Snapping used the FAR texel grid, whose
  // multiples are also finer-texel multiples (integer 1:2:6 chain), so
  // every tile stays swim-free.
  constexpr float kRatioMid = 2.0f;
  constexpr float kRatioInner = 6.0f;
  float vp_mid[16];
  float vp_inner[16];
  std::memcpy(vp_mid, vp, sizeof(vp));
  std::memcpy(vp_inner, vp, sizeof(vp));
  for (int r = 0; r < 4; ++r) {
    vp_mid[r * 4 + 0] *= kRatioMid;
    vp_mid[r * 4 + 1] *= kRatioMid;
    vp_inner[r * 4 + 0] *= kRatioInner;
    vp_inner[r * 4 + 1] *= kRatioInner;
  }
  nrhi::Cmd* cmd = context.cmd;
  if (g_r.static_sun_in_srv) {
    cmd->Barrier(g_r.static_sun, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    g_r.static_sun_in_srv = false;
  }
  const float clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  cmd->ClearRenderTarget(g_r.static_sun, clear);
  cmd->SetRenderTargets(g_r.static_sun, nullptr);
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
  const float size = float(g_r.static_sun_size);
  // Per tile (0 = inner r/6, 1 = mid r/2, 2 = far), two phases each:
  // opaque statics, then the alpha-tested families (trees, alphatest
  // fences/grates) with the clip PSO and their diffuse.
  for (int tile = 0; tile < 3; ++tile) {
    const float* tvp = tile == 0 ? vp_inner : (tile == 1 ? vp_mid : vp);
    // Cull threshold in FAR uc units (finer tiles cover 1/ratio of the
    // far extent).
    const float cull = tile == 0   ? 1.05f / kRatioInner
                       : tile == 1 ? 1.05f / kRatioMid
                                   : 1.05f;
    cmd->SetViewport(
        nrhi::Viewport{size * tile, 0.0f, size, size, 0.0f, 1.0f});
    cmd->SetScissor(nrhi::Rect{int32_t(g_r.static_sun_size) * tile, 0,
                               int32_t(g_r.static_sun_size) * (tile + 1),
                               int32_t(g_r.static_sun_size)});
    for (int phase = 0; phase < 2; ++phase) {
      cmd->SetPipeline(phase == 0 ? g_r.pso_static_caster
                                  : g_r.pso_static_caster_clip);
      for (const auto& [key, rec] : g_r.static_casters) {
        if (rec.clip != (phase == 1)) {
          continue;
        }
        auto mit = g_r.meshes.find(rec.mesh);
        // Fingerprint mismatch = the arena address was reused for
        // different content; drawing it would raster a foreign mesh into
        // the map. The record refreshes the next time the item is
        // actually seen.
        if (mit == g_r.meshes.end() ||
            mit->second.fingerprint != rec.fingerprint) {
          continue;
        }
        // NSM casters cover a wide radius the main pass never draws; touch
        // the LRU clock so the cascades keep their meshes resident.
        mit->second.last_used_frame = frame_number;
        // Cull by the mesh bbox footprint in (far) map space.
        float umin = std::numeric_limits<float>::max(), umax = -umin;
        float vmin = umin, vmax = -umin;
        for (int corner = 0; corner < 8; ++corner) {
          const float px = (corner & 1) ? rec.bbox_max[0] : rec.bbox_min[0];
          const float py = (corner & 2) ? rec.bbox_max[1] : rec.bbox_min[1];
          const float pz = (corner & 4) ? rec.bbox_max[2] : rec.bbox_min[2];
          float w[3];
          for (int a = 0; a < 3; ++a) {
            w[a] = px * rec.world[0 + a] + py * rec.world[4 + a] +
                   pz * rec.world[8 + a] + rec.world[12 + a];
          }
          const float u = rows[0] * w[0] + rows[1] * w[1] + rows[2] * w[2] +
                          rows[3];
          const float v = rows[4] * w[0] + rows[5] * w[1] + rows[6] * w[2] +
                          rows[7];
          umin = std::min(umin, u);
          umax = std::max(umax, u);
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
        }
        if (umax < -cull || umin > cull || vmax < -cull || vmin > cull) {
          continue;
        }
        float constants[52] = {};
        std::memcpy(constants, rec.world, sizeof(rec.world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += rec.world[r * 4 + k] * tvp[k * 4 + col];
            }
            mvp[r * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        if (phase == 1) {
          nrhi::TextureView* srv = LookupResolvedTexture(rec.diffuse_tex);
          cmd->SetTexture(1, srv != nullptr ? srv : g_r.white.srv);
        }
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const DrawEntry& draw : rec.draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(
                nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index,
                           draw.base_vertex);
        }
      }
    }
  }
  cmd->Barrier(g_r.static_sun, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kPixelShaderResource);
  cmd->FlushBarriers();
  g_r.static_sun_in_srv = true;
  g_r.static_sun_valid = true;
}

// F11 capture aid: read back the world-shade map (512x512 RG16) and the
// static sun map (3 tiles, RG16) once per recording session, with the rows
// each was rendered with, next to the shadow atlas dump
// (static_maps_<time>_ws512_ss<w>x<h>.bin + .txt). Diagnoses receiver
// darkening on dynamic objects offline (ws plane first, ss plane after).
void DumpStaticMaps(const NativeGuestOutputRenderContext& context,
                    const FrameScene& scene) {
  nrhi::Cmd* cmd = context.cmd;
  const bool rec = g_recording.load(std::memory_order_relaxed);
  if (!rec && g_r.maps_dump_state == 2) {
    g_r.maps_dump_state = 0;
  }
  const uint64_t ws_bytes = uint64_t(RendererState::kWorldShadowSize) *
                            RendererState::kWorldShadowSize * 8;  // RG32F
  if (g_r.maps_dump_state == 1 &&
      context.device->CompletedSubmission() > g_r.maps_dump_submission) {
    const uint64_t ss_bytes = uint64_t(g_r.maps_dump_ss_w) * g_r.maps_dump_ss_h * 4;
    context.device->InvalidateForRead(g_r.maps_dump_buf, 0, ws_bytes + ss_bytes);
    std::string dir = REXCVAR_GET(skate3_native_render_snapshot_dir);
    if (dir.empty()) {
      dir = "native_render_snapshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string stem = (std::filesystem::path(dir) /
                              ("static_maps_" + std::to_string(uint64_t(std::time(nullptr))) +
                               "_ws512rg32f_ss" + std::to_string(g_r.maps_dump_ss_w) + "x" +
                               std::to_string(g_r.maps_dump_ss_h)))
                                 .string();
    std::ofstream out(stem + ".bin", std::ios::binary);
    out.write(reinterpret_cast<const char*>(g_r.maps_dump_ptr),
              std::streamsize(ws_bytes + ss_bytes));
    std::ofstream txt(stem + ".txt");
    txt << "ws_rows";
    for (float v : g_r.maps_dump_ws_rows) txt << ' ' << v;
    txt << '\n' << "nsm_rows";
    for (float v : g_r.maps_dump_nsm_rows) txt << ' ' << v;
    txt << '\n';
    REXLOG_INFO("native-scene: static maps dumped -> {}.bin", stem);
    g_r.maps_dump_state = 2;
  }
  if (!rec || g_r.maps_dump_state != 0 || g_r.world_shadow == nullptr ||
      !g_r.world_shadow_primed) {
    return;
  }
  const uint32_t ss_w = g_r.static_sun != nullptr ? g_r.static_sun_size * 3 : 0;
  const uint32_t ss_h = g_r.static_sun != nullptr ? g_r.static_sun_size : 0;
  const uint64_t ss_bytes = uint64_t(ss_w) * ss_h * 4;
  if (g_r.maps_dump_buf != nullptr &&
      (g_r.maps_dump_ss_w != ss_w || g_r.maps_dump_ss_h != ss_h)) {
    context.device->DestroyDeferred(g_r.maps_dump_buf);
    g_r.maps_dump_buf = nullptr;
    g_r.maps_dump_ptr = nullptr;
  }
  if (g_r.maps_dump_buf == nullptr) {
    nrhi::BufferDesc desc;
    desc.size = ws_bytes + ss_bytes;
    desc.heap = nrhi::HeapKind::kReadback;
    g_r.maps_dump_buf = context.device->CreateBuffer(desc);
    g_r.maps_dump_ptr = g_r.maps_dump_buf != nullptr
                            ? static_cast<uint8_t*>(context.device->Map(g_r.maps_dump_buf))
                            : nullptr;
    g_r.maps_dump_ss_w = ss_w;
    g_r.maps_dump_ss_h = ss_h;
  }
  if (g_r.maps_dump_ptr == nullptr) {
    g_r.maps_dump_state = 2;  // allocation failed; don't retry this session
    return;
  }
  const auto copy = [&](nrhi::Texture* t, bool in_srv, uint64_t offset, uint32_t w,
                        uint32_t h, uint32_t bpp) {
    const nrhi::ResourceState st = in_srv ? nrhi::ResourceState::kPixelShaderResource
                                          : nrhi::ResourceState::kRenderTarget;
    cmd->Barrier(t, st, nrhi::ResourceState::kCopySource);
    cmd->FlushBarriers();
    cmd->CopyTextureToBuffer(g_r.maps_dump_buf, offset, w * bpp, t, 0, w, h);
    cmd->Barrier(t, nrhi::ResourceState::kCopySource, st);
    cmd->FlushBarriers();
  };
  copy(g_r.world_shadow, g_r.world_shadow_in_srv, 0, RendererState::kWorldShadowSize,
       RendererState::kWorldShadowSize, 8);
  if (ss_bytes != 0) {
    copy(g_r.static_sun, g_r.static_sun_in_srv, ws_bytes, ss_w, ss_h, 4);
  }
  std::memcpy(g_r.maps_dump_ws_rows, g_r.world_shadow_rows, sizeof(g_r.maps_dump_ws_rows));
  std::memcpy(g_r.maps_dump_nsm_rows, g_r.nsm_rows, sizeof(g_r.maps_dump_nsm_rows));
  g_r.maps_dump_submission = context.device->CurrentSubmission();
  g_r.maps_dump_state = 1;
}

bool RenderShadowAtlas(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, uint32_t bone_region,
                       int32_t debug_mode, uint32_t* out_draws) {
  nrhi::Cmd* cmd = context.cmd;
  const float* sh = scene.shadow_rows;
  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  // Atlas-readback bookkeeping: write completed dumps to disk in frame
  // order; re-arm once the recording session that requested them ends.
  if (!g_recording.load(std::memory_order_relaxed) &&
      g_r.shadow_dump_written == g_r.shadow_dump_enqueued) {
    g_r.shadow_dump_done = false;
    g_r.shadow_dump_enqueued = 0;
    g_r.shadow_dump_written = 0;
  }
  while (g_r.shadow_dump_written < g_r.shadow_dump_enqueued &&
         context.device->CompletedSubmission() >
             g_r.shadow_dump_submission[g_r.shadow_dump_written]) {
    const uint32_t k = g_r.shadow_dump_written;
    const uint32_t tile_px = g_r.shadow_tile;
    const uint32_t pitch = tile_px * 3u * 4u;
    const uint64_t plane = uint64_t(pitch) * tile_px;
    context.device->InvalidateForRead(g_r.shadow_dump_buf[k], 0, plane * 2);
    std::string dir = REXCVAR_GET(skate3_native_render_snapshot_dir);
    if (dir.empty()) {
      dir = "native_render_snapshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path =
        std::filesystem::path(dir) /
        ("shadow_atlas_" + std::to_string(uint64_t(std::time(nullptr))) + "_f" +
         std::to_string(k) + "_" + std::to_string(tile_px * 3) + "x" +
         std::to_string(tile_px) + "_rg16.bin");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(g_r.shadow_dump_ptr[k]),
              std::streamsize(plane * 2));
    REXLOG_INFO(
        "native-scene: shadow atlas frame {} dumped (raw+blurred planes) -> {}",
        k, path.string());
    ++g_r.shadow_dump_written;
  }
  if (REXCVAR_GET(skate3_native_render_scene_shadows) && scene.shadow_valid &&
      g_r.shadow_raw != nullptr && g_r.pso_shadow_caster != nullptr &&
      g_r.pso_shadow_blur != nullptr && debug_mode == 0) {
    struct Caster {
      const DrawItem* item;
      uint32_t bone_offset;
      bool bones;
      // Alpha-tested caster (dynamicobject.alphatest): draws with the clip
      // PSO and the item's diffuse bound so the cutout silhouette casts,
      // not the full card quad.
      bool clip;
      nrhi::TextureView* clip_srv;
    };
    std::vector<Caster> casters;
    bool any_clip = false;
    for (const DrawItem& item : scene.items) {
      if (item.transparent || item.unlit || item.cloth_quads) {
        continue;
      }
      const bool skinned = item.skinned && !item.bones.empty();
      // The game's CSM casts only DYNAMIC content: characters/vehicles
      // (skinned), Ropa cloth, and dynamicobject props (the truck's caster
      // draws, F10-verified). Static world scenery has its shadows BAKED
      // into the lightmaps, and that includes world-PLACED instances
      // (streetlights, trees, rails), which carry real world transforms.
      // The old identity-matrix test let every placed prop into the caster
      // pass, painting phantom streetlight-head/tree silhouettes onto the
      // plaza glass towers (a "reflection-like" soft dark blob high on the
      // facade, geometrically impossible as a real shadow: sun at 47 deg,
      // lamp 8 m tall, blob at 96 m; the emulated frame has no such
      // shadow). Moreover the cascade transforms are a stylized
      // near-vertical projection serving a ~12 m height window; tall
      // static geometry projected through them shadows its own plan-view
      // footprint. Live static shade comes from the separate sun-aligned
      // static shadow map instead (RenderStaticSunMap).
      // Rigid park-editor geometry DOES cast: an F11 capture had 436 park
      // shadow-pass draws in one frame (environmentparkdecal_shadowPS x410,
      // diffuse x14, alphatest x12). Those items are classified as park
      // variants of DrawItem::dynobj (4/5/6), so the dynobj test below lets
      // them through; no broader rule is needed, and admitting every
      // ortho-submitted ctx here would let the phantom-silhouette cases
      // back in.
      if (!skinned && item.dynobj == 0 && !item.ropa && item.char_family == 0) {
        continue;
      }
      // CORRECTION to the note above: those environmentpark*_shadowPS draws
      // are NOT cascade passes. They are the skate.Park world-shadow passes
      // (ortho rows = the park receivers' tf2 rows, 1024x512 k_16_16 target
      // = the "DMO Exponential Atlas"; see ParkWsDraw), rebuilt natively into
      // park_ws. The game's cascade passes in a park hold only characters
      // (capture 1790871823: cac_* / defaultcharacter _shadowPS, 1-3 draws
      // each). Casting the park pieces here too added a second copy of
      // their shadows through the camera-following cascades (shadows that
      // partly slid with the camera on the park pieces).
      if (item.dynobj >= 4 && item.dynobj <= 6) {
        continue;
      }
      // Entities the game is holding invisible (spawn settle / distance
      // fade, see CharFadeAlpha) must not paint a shadow either; the
      // emulated frame shows neither the NPC nor a blob under it.
      if (item.char_family != 0 && CharFadeAlphaDrawn(scene, item) < 0.05f &&
          REXCVAR_GET(skate3_native_render_scene_entity_fade)) {
        continue;
      }
      // Per-piece caster parity: the game's own shadow passes skip some
      // character pieces (the CAS trucker hat casts NOTHING; natively
      // casting it painted a hard low-sun brim band across the editor face
      // that the emulated frame never shows). Cast only what the game
      // submitted through an ortho bank this frame (see
      // DrawItem::shadow_caster / g_frame_ortho_ctx).
      if (item.char_family != 0 && !item.shadow_caster &&
          REXCVAR_GET(skate3_native_render_scene_shadow_caster_parity)) {
        continue;
      }
      // NO inline decode here (this block used to re-decode every cloth
      // garment every frame, ~2.9 ms, the 160 fps cap during real play).
      // The dyn decode jobs / worker miss queue keep the cache fresh, one
      // frame behind the sim; a first-sight caster shadows 1-2 frames late.
      if (!g_r.meshes.contains(item.mesh)) {
        continue;
      }
      Caster c{&item, 0, false, false, nullptr};
      if (item.dynobj == 2 || item.dynobj == 5) {
        c.clip = true;
        c.clip_srv = LookupResolvedTexture(item.diffuse_tex);
        any_clip = true;
      }
      if (skinned) {
        const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
        const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
        if (offset + bytes > RendererState::kBoneRegionSize) {
          continue;
        }
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        c.bone_offset = offset;
        c.bones = true;
      }
      casters.push_back(c);
    }
    // Renders one caster set into one atlas chain (raw -> hblur -> vblur),
    // leaving all three in PIXEL_SHADER_RESOURCE; returns the draw count.
    const auto render_set = [&](const std::vector<Caster>& list, nrhi::Texture* raw_t,
                                nrhi::Texture* mid_t, nrhi::Texture* fin_t,
                                nrhi::TextureView* srv_raw, nrhi::TextureView* srv_mid,
                                bool& in_srv) -> uint32_t {
      uint32_t set_draws = 0;
      if (in_srv) {
        for (nrhi::Texture* res : {raw_t, mid_t, fin_t}) {
          cmd->Barrier(res, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kRenderTarget);
        }
        cmd->FlushBarriers();
        in_srv = false;
      }
      const uint32_t tile = g_r.shadow_tile;
      const float shadow_clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(raw_t, shadow_clear);
      cmd->SetRenderTargets(raw_t, nullptr);
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPipeline(g_r.pso_shadow_caster);
      // Unused by the caster shaders, but never leave root CBVs unset.
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      for (int ci = 0; ci < 3; ++ci) {
        // Cascade scale/offset (cascade 0 = identity; PS c1/c2 for 1/2).
        float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
        if (ci == 1) {
          sx = sh[4]; sy = sh[5]; ox = sh[6]; oy = sh[7];
        } else if (ci == 2) {
          sx = sh[8]; sy = sh[9]; ox = sh[10]; oy = sh[11];
        }
        // Light view-proj, row-vector convention: clip.x = ls_i.x,
        // clip.y = ls_i.y, clip.z = the height-ramp depth, clip.w = 1,
        // columns built from the receiver rows c0/c3/c4.
        float lightvp[16];
        for (int r = 0; r < 3; ++r) {
          lightvp[r * 4 + 0] = sh[0 + r] * sx;
          lightvp[r * 4 + 1] = sh[12 + r] * sy;
          lightvp[r * 4 + 2] = sh[16 + r];
          lightvp[r * 4 + 3] = 0.0f;
        }
        lightvp[12] = sh[3] * sx + ox;
        lightvp[13] = sh[15] * sy + oy;
        lightvp[14] = sh[19];
        lightvp[15] = 1.0f;
        cmd->SetViewport(nrhi::Viewport{float(tile) * ci, 0.0f, float(tile),
                                        float(tile), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{int32_t(tile) * ci, 0,
                                   int32_t(tile) * (ci + 1), int32_t(tile)});
        // Two phases per cascade: opaque casters, then the alpha-tested
        // set with the clip PSO and per-item diffuse.
        for (int phase = 0; phase < (any_clip ? 2 : 1); ++phase) {
          cmd->SetPipeline(phase == 0 ? g_r.pso_shadow_caster
                           : g_r.pso_shadow_caster_clip_exact != nullptr
                               ? g_r.pso_shadow_caster_clip_exact
                               : g_r.pso_shadow_caster_clip);
          for (const Caster& c : list) {
            if (c.clip != (phase == 1)) {
              continue;
            }
            // Per-cascade parity: only the tiles the game itself drew this
            // piece into (DrawItem::caster_cascades; 0 = unknown -> all).
            if (c.item->caster_cascades != 0 &&
                (c.item->caster_cascades & (1u << ci)) == 0) {
              continue;
            }
            auto mit = g_r.meshes.find(c.item->mesh);
            if (mit == g_r.meshes.end()) {
              continue;  // undecodable this frame; casts once the workers land
            }
            // A stale fingerprint is fine here: cloth decodes ride one frame
            // behind the sim by design (dyn decode jobs).
            float constants[52] = {};
            std::memcpy(constants, c.item->world, sizeof(c.item->world));
            float* mvp = constants + 16;
            for (int r = 0; r < 4; ++r) {
              for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                  sum += c.item->world[r * 4 + k] * lightvp[k * 4 + col];
                }
                mvp[r * 4 + col] = sum;
              }
            }
            // tint.g = skinning rule, same as the scene pass.
            constants[33] = c.bones ? (c.item->char_family >= 1 &&
                                               c.item->char_family <= 5
                                           ? 1.0f
                                           : 0.5f)
                                    : 0.0f;
            cmd->SetRootConstants(0, 52, constants, 0);
            cmd->SetBufferSrv(3, g_r.bone_ring,
                              bone_region + (c.bones ? c.bone_offset : 0));
            if (phase == 1) {
              cmd->SetTexture(1, c.clip_srv != nullptr ? c.clip_srv
                                                       : g_r.white.srv);
            }
            cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                                 mit->second.vb_view.offset,
                                 mit->second.vb_view.size_bytes,
                                 mit->second.vb_view.stride);
            cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                                mit->second.ib_view.offset,
                                mit->second.ib_view.size_bytes);
            for (const DrawEntry& draw : c.item->draws) {
              if (draw.prim == 4) {
                cmd->SetPrimitiveTopology(
                    nrhi::PrimitiveTopology::kTriangleList);
              } else if (draw.prim == 6) {
                cmd->SetPrimitiveTopology(
                    nrhi::PrimitiveTopology::kTriangleStrip);
              } else {
                continue;
              }
              cmd->DrawIndexed(draw.index_count, draw.start_index,
                               draw.base_vertex);
              ++set_draws;
            }
          }
        }
      }
      // Blur/convert chain: raw -> (hblur) -> mid -> (vblur) -> final. The
      // game's kernels: 5-tap Gaussian coverage cascade 0, 3-tap cascade 1,
      // format-convert only for cascade 2 (weights (1,0,0), 0 taps), plus
      // depth dilation into the penumbra. One fullscreen-triangle draw per
      // tile per direction, taps clamped inside the tile.
      cmd->Barrier(raw_t, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      cmd->SetPipeline(g_r.pso_shadow_blur);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const float kernels[3][3] = {{0.292082f, 0.233881f, 0.120078f},
                                   {0.667243f, 0.166379f, 0.0f},
                                   {1.0f, 0.0f, 0.0f}};
      const float ntaps[3] = {2.0f, 1.0f, 0.0f};
      const auto blur_pass = [&](int ci, bool horizontal,
                                 nrhi::TextureView* src, nrhi::Texture* dst,
                                 bool src_raw) {
        cmd->SetRenderTargets(dst, nullptr);
        cmd->SetViewport(nrhi::Viewport{float(tile) * ci, 0.0f, float(tile),
                                        float(tile), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{int32_t(tile) * ci, 0,
                                   int32_t(tile) * (ci + 1), int32_t(tile)});
        // Blur tap offsets step RASTER texels, matching the emulated GPU:
        // the game's blur PS taps via tfetch integer offsets (OffsetX/Y
        // -2..+2), which a resolution-scaled
        // raster applies in physical texels, so the penumbra NARROWS as the
        // tile grows (the emulated baseline is sharper than original
        // hardware; 512 reproduces the softer original-360 look). Scaling
        // the step by tile/512 to hold a console-width penumbra was tried
        // and made receivers visibly blurrier than the emulated reference.
        const float bc[12] = {horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f,
                              ntaps[ci], src_raw ? 1.0f : 0.0f,
                              kernels[ci][0], kernels[ci][1], kernels[ci][2], 0.0f,
                              float(tile) * ci, float(tile) * (ci + 1) - 1.0f,
                              float(tile) - 1.0f, 0.0f};
        cmd->SetRootConstants(0, 12, bc, 0);
        cmd->SetTexture(1, src);
        cmd->Draw(3, 0);
      };
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, true, srv_raw, mid_t, true);
      }
      cmd->Barrier(mid_t, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, false, srv_mid, fin_t, false);
      }
      cmd->Barrier(fin_t, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      in_srv = true;
      return set_draws;
    };
    if (!casters.empty()) {
      const uint32_t main_draws =
          render_set(casters, g_r.shadow_raw, g_r.shadow_mid, g_r.shadow_final,
                     g_r.shadow_srv_raw, g_r.shadow_srv_mid, g_r.shadow_in_srv_state);
      shadow_draws += main_draws;
      shadow_ready = main_draws > 0;
      const uint32_t tile = g_r.shadow_tile;  // atlas readback below
      // Atlas readback on kShadowDumpFrames consecutive frames per
      // recording session (see the completion handler at function entry):
      // per frame, raw plane at offset 0, blurred plane after it. Each
      // frame gets its own readback buffer so no frame waits on a copy.
      if (g_recording.load(std::memory_order_relaxed) &&
          !g_r.shadow_dump_done &&
          g_r.shadow_dump_enqueued < RendererState::kShadowDumpFrames) {
        const uint32_t k = g_r.shadow_dump_enqueued;
        const uint32_t pitch = tile * 3u * 4u;
        const uint64_t plane = uint64_t(pitch) * tile;
        if (g_r.shadow_dump_buf[k] == nullptr) {
          nrhi::BufferDesc desc;
          desc.size = plane * 2;
          desc.heap = nrhi::HeapKind::kReadback;
          g_r.shadow_dump_buf[k] = context.device->CreateBuffer(desc);
          g_r.shadow_dump_ptr[k] =
              g_r.shadow_dump_buf[k] != nullptr
                  ? static_cast<uint8_t*>(
                        context.device->Map(g_r.shadow_dump_buf[k]))
                  : nullptr;
        }
        if (g_r.shadow_dump_ptr[k] != nullptr) {
          cmd->Barrier(g_r.shadow_raw, nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopySource);
          cmd->Barrier(g_r.shadow_final,
                       nrhi::ResourceState::kPixelShaderResource,
                       nrhi::ResourceState::kCopySource);
          cmd->FlushBarriers();
          cmd->CopyTextureToBuffer(g_r.shadow_dump_buf[k], 0, pitch,
                                   g_r.shadow_raw, 0, tile * 3, tile);
          cmd->CopyTextureToBuffer(g_r.shadow_dump_buf[k], plane, pitch,
                                   g_r.shadow_final, 0, tile * 3, tile);
          cmd->Barrier(g_r.shadow_raw, nrhi::ResourceState::kCopySource,
                       nrhi::ResourceState::kPixelShaderResource);
          cmd->Barrier(g_r.shadow_final, nrhi::ResourceState::kCopySource,
                       nrhi::ResourceState::kPixelShaderResource);
          cmd->FlushBarriers();
          g_r.shadow_dump_submission[k] = context.device->CurrentSubmission();
          ++g_r.shadow_dump_enqueued;
          if (g_r.shadow_dump_enqueued == RendererState::kShadowDumpFrames) {
            g_r.shadow_dump_done = true;
          }
        } else {
          g_r.shadow_dump_done = true;  // allocation failed; don't retry
        }
      }
    }
  }
  // ---- the game's world-shade map (tf1) ----
  // The game renders it every frame it runs its world-shade pass: depth
  // cleared to 1.0, its casters drawn with the pass's own projection rows,
  // then D3DDevice_Resolve'd into the 512x512 k_24_8 texture every
  // character / vehicle / dynamicobject PS samples as tf1 (verified live:
  // one depth resolve per frame in gameplay AND in create-a-skater, where
  // the pass draws no casters and the resolved map is empty). The guest
  // never executes it in native mode, so the map is rebuilt here on exactly
  // the frames the game resolved (FrameScene::ws_resolved): cleared, then
  // the pass's casters (none = empty). Between resolves the texture keeps
  // its contents, like the game's.
  if (g_r.world_shadow != nullptr && g_r.pso_ws_caster != nullptr &&
      debug_mode == 0 && scene.ws_resolved &&
      REXCVAR_GET(skate3_native_render_scene_shadows) &&
      REXCVAR_GET(skate3_native_render_scene_dynobj_v2)) {
    {
      if (g_r.world_shadow_in_srv) {
        cmd->Barrier(g_r.world_shadow, nrhi::ResourceState::kPixelShaderResource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        g_r.world_shadow_in_srv = false;
      }
      const float ws_clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(g_r.world_shadow, ws_clear);
      std::memcpy(g_r.world_shadow_rows, scene.ws_pass_rows, sizeof(g_r.world_shadow_rows));
      if (!g_r.world_shadow_primed) {
        REXLOG_INFO("native-scene: world-shade map built from the game's pass");
      }
      g_r.world_shadow_primed = true;
      // View-proj columns = the pass's own projection rows (its VS c0/c1/
      // c2): clip.x/y/z = dot(wp4, row), clip.w = 1.
      const float* pr = scene.ws_pass_rows;
      float wsvp[16];
      for (int r = 0; r < 3; ++r) {
        wsvp[r * 4 + 0] = pr[r];
        wsvp[r * 4 + 1] = pr[4 + r];
        wsvp[r * 4 + 2] = pr[8 + r];
        wsvp[r * 4 + 3] = 0.0f;
      }
      wsvp[12] = pr[3];
      wsvp[13] = pr[7];
      wsvp[14] = pr[11];
      wsvp[15] = 1.0f;
      cmd->SetRenderTargets(g_r.world_shadow, nullptr);
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPipeline(g_r.pso_ws_caster_game != nullptr ? g_r.pso_ws_caster_game
                                                          : g_r.pso_ws_caster);
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      const float ws_size = float(RendererState::kWorldShadowSize);
      cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, ws_size, ws_size, 0.0f, 1.0f});
      cmd->SetScissor(nrhi::Rect{0, 0, int32_t(RendererState::kWorldShadowSize),
                                 int32_t(RendererState::kWorldShadowSize)});
      const auto draw_game_caster = [&](const DrawItem& item) {
        // Exactly the game's world-shade casters (DrawItem::ws_caster).
        if (!item.ws_caster) {
          return;
        }
        auto gm = g_r.meshes.find(item.mesh);
        if (gm == g_r.meshes.end()) {
          EnqueueMeshMiss(item.mesh);  // off-view casters decode here first
          return;
        }
        float gc[52] = {};
        std::memcpy(gc, item.world, sizeof(item.world));
        for (int r = 0; r < 4; ++r) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += item.world[r * 4 + k] * wsvp[k * 4 + col];
            }
            gc[16 + r * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, gc, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        cmd->SetVertexBuffer(gm->second.vb_view.buffer, gm->second.vb_view.offset,
                             gm->second.vb_view.size_bytes, gm->second.vb_view.stride);
        cmd->SetIndexBuffer(gm->second.ib_view.buffer, gm->second.ib_view.offset,
                            gm->second.ib_view.size_bytes);
        for (const DrawEntry& draw : item.draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index, draw.base_vertex);
        }
      };
      for (const DrawItem& item : scene.items) {
        draw_game_caster(item);
      }
      for (const DrawItem& item : scene.ws_items) {
        draw_game_caster(item);
      }
      cmd->Barrier(g_r.world_shadow, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      g_r.world_shadow_in_srv = true;
    }
  }
  // ---- the game's skate.Park world-shadow map (tf2) ----
  // Two ortho passes, one per 512x512 tile of a 1024x512 target (inner at
  // x 0..511, outer at 512..1023), drawing the park pieces cull_back with
  // their own world matrices, depth cleared to 1.0, then resolved into the
  // texture environmentpark{diffuse,decal}_defaultPS read as tf2. Rebuilt
  // on the frames the game rebuilt it (FrameScene::park_ws_build), from
  // exactly the instances its passes drew.
  if (g_r.park_ws != nullptr && g_r.park_ws_srv != nullptr && g_r.pso_park_ws_caster != nullptr &&
      debug_mode == 0 && scene.park_ws_build && scene.park_ws_recv_valid &&
      REXCVAR_GET(skate3_native_render_scene_shadows)) {
    if (g_r.park_ws_in_srv) {
      cmd->Barrier(g_r.park_ws, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      g_r.park_ws_in_srv = false;
    }
    const float clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.park_ws, clear);
    if (!g_r.park_ws_primed) {
      REXLOG_INFO("native-scene: park world-shadow map built from the game's passes");
    }
    g_r.park_ws_primed = true;
    {
      // The receiver rows this build implies (see park_ws_built_recv):
      // inner = tile 0's VS c0 / c1 / c2 (= PS c5 / c7 / c8), outer scale /
      // offset c6 = tile 1's rows relative to tile 0's. A tile the game drew
      // nothing into this frame keeps the latched receiver values.
      float* br = g_r.park_ws_built_recv;
      std::memcpy(br, scene.park_ws_recv, sizeof(g_r.park_ws_built_recv));
      const float* p0 = scene.park_ws_pass_rows[0];
      const float* p1 = scene.park_ws_pass_rows[1];
      if (scene.park_ws_tile_have[0]) {
        std::memcpy(br + 0, p0 + 0, 4 * sizeof(float));   // c5 = u
        std::memcpy(br + 8, p0 + 4, 4 * sizeof(float));   // c7 = v
        std::memcpy(br + 12, p0 + 8, 4 * sizeof(float));  // c8 = ray
        if (scene.park_ws_tile_have[1]) {
          for (int r = 0; r < 2; ++r) {
            const float* a = p0 + 4 * r;
            const float* b = p1 + 4 * r;
            int j = 0;
            for (int k = 1; k < 3; ++k) {
              if (std::fabs(a[k]) > std::fabs(a[j])) {
                j = k;
              }
            }
            if (std::fabs(a[j]) > 1e-8f) {
              const float sc = b[j] / a[j];
              br[4 + r] = sc;                   // c6.x / c6.y
              br[6 + r] = b[3] - sc * a[3];     // c6.z / c6.w
            }
          }
        }
      }
      g_r.park_ws_built_valid = true;
    }
    cmd->SetRenderTargets(g_r.park_ws, nullptr);
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetPipeline(g_r.pso_park_ws_caster);
    cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    const float tile = float(g_r.park_ws_tile);
    for (int t = 0; t < 2; ++t) {
      if (!scene.park_ws_tile_have[t]) {
        continue;  // the game drew nothing into this tile: cleared = lit
      }
      // View-proj = the pass's own rows (VS c0 / c1 / c2), clip.w = 1.
      const float* pr = scene.park_ws_pass_rows[t];
      float vp[16];
      for (int r = 0; r < 3; ++r) {
        vp[r * 4 + 0] = pr[r];
        vp[r * 4 + 1] = pr[4 + r];
        vp[r * 4 + 2] = pr[8 + r];
        vp[r * 4 + 3] = 0.0f;
      }
      vp[12] = pr[3];
      vp[13] = pr[7];
      vp[14] = pr[11];
      vp[15] = 1.0f;
      cmd->SetViewport(nrhi::Viewport{tile * float(t), 0.0f, tile, tile, 0.0f, 1.0f});
      cmd->SetScissor(nrhi::Rect{int32_t(g_r.park_ws_tile) * t, 0,
                                 int32_t(g_r.park_ws_tile) * (t + 1),
                                 int32_t(g_r.park_ws_tile)});
      // Opaque pieces, then the alpha-tested ones (their own clip PSO and
      // diffuse at t0).
      for (int phase = 0; phase < 2; ++phase) {
      if (phase == 1) {
        if (g_r.pso_park_ws_caster_clip == nullptr) {
          break;
        }
        cmd->SetPipeline(g_r.pso_park_ws_caster_clip);
      }
      for (const FrameScene::ParkWsCaster& c : scene.park_ws_casters) {
        if ((c.tiles & (1u << t)) == 0) {
          continue;
        }
        const bool clip = c.clip_tex != 0 && g_r.pso_park_ws_caster_clip != nullptr;
        if (clip != (phase == 1)) {
          continue;
        }
        if (clip) {
          nrhi::TextureView* srv = LookupResolvedTexture(c.clip_tex);
          cmd->SetTexture(1, srv != nullptr ? srv : g_r.white.srv);
        }
        auto gm = g_r.meshes.find(c.mesh);
        if (gm == g_r.meshes.end()) {
          EnqueueMeshMiss(c.mesh);
          continue;
        }
        float gc[52] = {};
        std::memcpy(gc, c.world, sizeof(c.world));
        for (int r = 0; r < 4; ++r) {
          for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += c.world[r * 4 + k] * vp[k * 4 + col];
            }
            gc[16 + r * 4 + col] = sum;
          }
        }
        cmd->SetRootConstants(0, 52, gc, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
        cmd->SetVertexBuffer(gm->second.vb_view.buffer, gm->second.vb_view.offset,
                             gm->second.vb_view.size_bytes, gm->second.vb_view.stride);
        cmd->SetIndexBuffer(gm->second.ib_view.buffer, gm->second.ib_view.offset,
                            gm->second.ib_view.size_bytes);
        for (const DrawEntry& draw : c.draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexed(draw.index_count, draw.start_index, draw.base_vertex);
        }
      }
      }
      cmd->SetPipeline(g_r.pso_park_ws_caster);
    }
    // The game's log-space blur of each tile (capture 1790871823: the inner
    // pass's draws, then shadow_hblur_exp_5 + vblur_exp_5 through a 512x512
    // k_16_16 scratch, then the outer pass's, then hblur_exp_3 + vblur_exp_3
    // with the scissor on x 512..1023). Kernels = the blur banks' c0
    // (5 taps: 0.40262 centre, 0.2442, 0.05449; 3 taps: 0.91922, 0.04039),
    // k = their c1.z, which is the receivers' c10.z (88). Without it the
    // receivers' exponential compare ran on the raw depth: hard, aliased
    // edges where the game's are soft.
    if (g_r.pso_park_ws_blur != nullptr && g_r.park_ws_tmp != nullptr &&
        g_r.park_ws_tmp_srv != nullptr && scene.park_ws_recv[18] > 0.0f) {
      const float k = scene.park_ws_recv[18];
      const float kw[2][3] = {{0.40262f, 0.2442f, 0.05449f}, {0.91922f, 0.04039f, 0.0f}};
      const float kn[2] = {2.0f, 1.0f};
      const int32_t ts = int32_t(g_r.park_ws_tile);
      cmd->SetPipeline(g_r.pso_park_ws_blur);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const auto pass = [&](bool horizontal, nrhi::TextureView* src, nrhi::Texture* dst) {
        cmd->SetRenderTargets(dst, nullptr);
        for (int t = 0; t < 2; ++t) {
          if (!scene.park_ws_tile_have[t]) {
            continue;
          }
          cmd->SetViewport(nrhi::Viewport{tile * float(t), 0.0f, tile, tile, 0.0f, 1.0f});
          cmd->SetScissor(nrhi::Rect{ts * t, 0, ts * (t + 1), ts});
          const float bc[12] = {horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f, kn[t], k,
                                kw[t][0], kw[t][1], kw[t][2], 0.0f,
                                float(ts * t), float(ts * (t + 1) - 1), float(ts - 1), 0.0f};
          cmd->SetRootConstants(0, 12, bc, 0);
          cmd->SetTexture(1, src);
          cmd->Draw(3, 0);
        }
      };
      cmd->Barrier(g_r.park_ws, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      pass(true, g_r.park_ws_srv, g_r.park_ws_tmp);
      cmd->Barrier(g_r.park_ws_tmp, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->Barrier(g_r.park_ws, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      pass(false, g_r.park_ws_tmp_srv, g_r.park_ws);
      cmd->Barrier(g_r.park_ws_tmp, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    }
    cmd->Barrier(g_r.park_ws, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    g_r.park_ws_in_srv = true;
  }
  DumpStaticMaps(context, scene);
  *out_draws = shadow_draws;
  return shadow_ready;
}

  // Selection-outline mask (see kOutlineShaderSource): re-render the frame's
  // selected items into the small R8 target while the scene pass state is
  // still bound. The edge composite runs after the resolve, on the
  // single-sample output.
// Game stencil mark target: R8, output-sized, at the scene's sample count
// (it shares the scene depth buffer as its DSV). Idles in RENDER_TARGET;
// stencil_mark_ready = rendered this frame and handed off in
// PIXEL_SHADER_RESOURCE state for the postfx depth pack.
bool EnsureStencilMarkTarget(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.stencil_mark != nullptr &&
      g_r.stencil_mark_width == context.guest_output_width &&
      g_r.stencil_mark_height == context.guest_output_height &&
      g_r.stencil_mark_samples == g_r.msaa) {
    return true;
  }
  if (g_r.stencil_mark_srv != nullptr) {
    device->DestroyDeferred(g_r.stencil_mark_srv);
    g_r.stencil_mark_srv = nullptr;
  }
  if (g_r.stencil_mark != nullptr) {
    device->DestroyDeferred(g_r.stencil_mark);
    g_r.stencil_mark = nullptr;
  }
  g_r.stencil_mark_ready = false;
  g_r.stencil_mark_width = 0;
  nrhi::TextureDesc desc;
  desc.width = context.guest_output_width;
  desc.height = context.guest_output_height;
  desc.format = nrhi::Format::kR8_UNORM;
  desc.sample_count = g_r.msaa;
  desc.usage = nrhi::kTextureUsageRenderTarget;
  desc.initial_state = nrhi::ResourceState::kRenderTarget;
  g_r.stencil_mark = device->CreateTexture(desc);
  if (g_r.stencil_mark == nullptr) {
    return false;
  }
  nrhi::TextureViewDesc vd;
  if (g_r.msaa > 1) {
    vd.dimension = nrhi::ViewDimension::k2DMS;
  } else {
    vd.mip_levels = 1;
  }
  g_r.stencil_mark_srv = device->CreateTextureView(g_r.stencil_mark, vd);
  if (g_r.stencil_mark_srv == nullptr) {
    device->DestroyDeferred(g_r.stencil_mark);
    g_r.stencil_mark = nullptr;
    return false;
  }
  g_r.stencil_mark_width = context.guest_output_width;
  g_r.stencil_mark_height = context.guest_output_height;
  g_r.stencil_mark_samples = g_r.msaa;
  return true;
}

// The foreground preview object's mark target (see RendererState::fg_mark):
// the same shape as the stencil mark (R8, output-sized, the scene's sample
// count, scene depth as its DSV). Idles in RENDER_TARGET; fg_mark_ready =
// rendered this frame and in PIXEL_SHADER_RESOURCE for the masked blit.
bool EnsureFgMarkTarget(const NativeGuestOutputRenderContext& context) {
  nrhi::Device* device = context.device;
  if (g_r.fg_mark != nullptr && g_r.fg_mark_width == context.guest_output_width &&
      g_r.fg_mark_height == context.guest_output_height && g_r.fg_mark_samples == g_r.msaa) {
    return true;
  }
  if (g_r.fg_mark_srv != nullptr) {
    device->DestroyDeferred(g_r.fg_mark_srv);
    g_r.fg_mark_srv = nullptr;
  }
  if (g_r.fg_mark != nullptr) {
    device->DestroyDeferred(g_r.fg_mark);
    g_r.fg_mark = nullptr;
  }
  g_r.fg_mark_ready = false;
  g_r.fg_mark_width = 0;
  nrhi::TextureDesc desc;
  desc.width = context.guest_output_width;
  desc.height = context.guest_output_height;
  desc.format = nrhi::Format::kR8_UNORM;
  desc.sample_count = g_r.msaa;
  desc.usage = nrhi::kTextureUsageRenderTarget;
  desc.initial_state = nrhi::ResourceState::kRenderTarget;
  g_r.fg_mark = device->CreateTexture(desc);
  if (g_r.fg_mark == nullptr) {
    return false;
  }
  nrhi::TextureViewDesc vd;
  if (g_r.msaa > 1) {
    vd.dimension = nrhi::ViewDimension::k2DMS;
  } else {
    vd.mip_levels = 1;
  }
  g_r.fg_mark_srv = device->CreateTextureView(g_r.fg_mark, vd);
  if (g_r.fg_mark_srv == nullptr) {
    device->DestroyDeferred(g_r.fg_mark);
    g_r.fg_mark = nullptr;
    return false;
  }
  g_r.fg_mark_width = context.guest_output_width;
  g_r.fg_mark_height = context.guest_output_height;
  g_r.fg_mark_samples = g_r.msaa;
  return true;
}

bool RenderOutlineMask(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, const nrhi::Viewport& viewport,
                       const nrhi::Rect& scissor, bool msaa_on,
                       nrhi::Texture* scene_color, nrhi::Texture* scene_depth,
                       bool use_depth) {
  nrhi::Cmd* cmd = context.cmd;
  bool outline_ready = false;
  if (REXCVAR_GET(skate3_native_render_scene_selection_outline) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr &&
      g_r.outline_mask != nullptr) {
    std::vector<const DrawItem*> sel;
    for (const DrawItem& item : scene.items) {
      // Skinned items are excluded: the mask VS runs the rigid path (world
      // matrix), which renders a skinned mesh at bind pose at the origin.
      if (item.selected && !item.skinned) {
        sel.push_back(&item);
      }
    }
    if (!sel.empty()) {
      const float mask_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(g_r.outline_mask, mask_clear);
      cmd->SetRenderTargets(g_r.outline_mask, nullptr);
      cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f,
                                      float(g_r.outline_mask_width),
                                      float(g_r.outline_mask_height), 0.0f,
                                      1.0f});
      cmd->SetScissor(nrhi::Rect{0, 0, int32_t(g_r.outline_mask_width),
                                 int32_t(g_r.outline_mask_height)});
      cmd->SetPipeline(g_r.pso_outline_mask);
      for (const DrawItem* item : sel) {
        auto mit = g_r.meshes.find(item->mesh);
        if (mit == g_r.meshes.end() || mit->second.fingerprint != item->fingerprint) {
          continue;  // decoded by the main pass this frame; masks from the next
        }
        float constants[52] = {};
        std::memcpy(constants, item->world, sizeof(item->world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
            }
            mvp[r * 4 + c] = sum;
          }
        }
        // tint = (1, 0, 0, 1): the scene PS's solid-color early-out
        // (tint.a > 0) writes 1.0 into the R8 mask; tint.g = 0 keeps the VS
        // skinning branch off.
        constants[32] = 1.0f;
        constants[35] = 1.0f;
        // One-pass VR stereo: both eyes, as the main pass draws them.
        const bool stereo = skate3::vr::CurrentEye() == skate3::vr::kStereoEye;
        if (stereo) {
          constants[15] = 2.0f;
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                             mit->second.vb_view.offset,
                             mit->second.vb_view.size_bytes,
                             mit->second.vb_view.stride);
        cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                            mit->second.ib_view.offset,
                            mit->second.ib_view.size_bytes);
        for (const DrawEntry& draw : item->draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexedInstanced(draw.index_count, stereo ? 2u : 1u, draw.start_index,
                                    draw.base_vertex);
          outline_ready = true;
        }
      }
      // Restore the pass state the resolve/2D paths rely on (fullscreen
      // viewport; the non-MSAA path keeps rendering into the scene target).
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      if (!msaa_on) {
        cmd->SetRenderTargets(scene_color, use_depth ? scene_depth : nullptr);
      }
      if (outline_ready) {
        cmd->Barrier(g_r.outline_mask, nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kPixelShaderResource);
      }
    }
  }
  return outline_ready;
}

// Selection-outline composite: additive stencil-edge-detect over the
// resolved output (before the popup blur, like the game's postfx order).
void RenderOutlineComposite(const NativeGuestOutputRenderContext& context,
                            const FrameScene& scene, nrhi::Texture* output,
                            const nrhi::Viewport& viewport,
                            const nrhi::Rect& scissor) {
  nrhi::Cmd* cmd = context.cmd;
  cmd->FlushBarriers();
  cmd->SetRenderTargets(output, nullptr);
  cmd->SetViewport(viewport);
  cmd->SetScissor(scissor);
  cmd->SetPipeline(g_r.pso_outline_edge);
  cmd->SetRootConstants(0, 4, scene.outline_color, 0);
  cmd->SetTexture(1, g_r.outline_mask_srv);
  cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
  cmd->Draw(3, 0);
  // Back to the mask's steady state for the next frame.
  cmd->Barrier(g_r.outline_mask, nrhi::ResourceState::kPixelShaderResource,
               nrhi::ResourceState::kRenderTarget);
}

// Windowed perf + telemetry log lines (verbatim from the former tail of
// RenderScene). Window length in frames = the perf-interval cvar.
void LogFrameStats(const FrameScene& scene, uint64_t frames, uint32_t drawn,
                   uint32_t drawn_2d, uint32_t drawn_spline, bool shadow_ready,
                   uint32_t shadow_draws) {
  // Raw guest-load read-fault recoveries (POSIX only; always 0 on Windows).
  // Each one is a streaming race that revoked a captured range mid-walk and
  // would otherwise be a fatal SIGSEGV; a slowly growing count during heavy
  // world streaming is expected and benign.
  {
    static uint64_t s_recov_reported = 0;
    static uint64_t s_recov_frame = 0;
    const uint64_t recov = GuestReadRecoveryCount();
    if (recov != s_recov_reported &&
        (s_recov_frame == 0 || frames - s_recov_frame >= 600)) {
      REXLOG_INFO("native-scene: guest read faults recovered total={} (+{})",
                  recov, recov - s_recov_reported);
      s_recov_reported = recov;
      s_recov_frame = frames;
    }
  }
  const uint64_t interval = uint64_t(
      std::max(60, REXCVAR_GET(skate3_native_render_scene_perf_interval)));
  if (frames % interval == 0 && REXCVAR_GET(skate3_native_render_scene_perf_log)) {
    // CPU-side perf snapshot for this window. guest_fps is derived
    // from the guest frame interval; capture/build run on the guest render
    // thread (they extend guest frame time directly), render/items/shadow on
    // the command processor thread, decode inline on the render thread
    // (count = decodes this window; max = the worst single decode).
    const double guest_dt_ms = g_pw_guest_dt.AvgMs();
    REXLOG_INFO(
        "native-scene perf: guest_fps={:.0f} guest_dt_max={:.1f}ms "
        "capture={:.2f}/{:.2f}ms build={:.2f}/{:.2f}ms "
        "bld[2d={:.2f}/{:.2f} spl={:.2f}/{:.2f} pal={:.2f}/{:.2f}]ms "
        "dt[{}|{}|{}|{}|{}] ptail={:.2f}/{:.2f}ms | render={:.2f}/{:.2f}ms "
        "items={:.2f}/{:.2f}ms shadow={:.2f}/{:.2f}ms "
        "pre={:.2f}/{:.2f}ms settle[{:.2f}/{:.2f}ms n={} dec={} def={}] "
        "tail={:.2f}/{:.2f}ms twod={:.2f}/{:.2f}ms "
        "decode[mesh n={} avg={:.2f} max={:.2f}ms tex n={} avg={:.2f} max={:.2f}ms] "
        "commit={:.2f}/{:.2f}ms itemcache[hit={} build={}] cam[chg={} rep={} maxstreak={}]",
        guest_dt_ms > 0.0 ? 1000.0 / guest_dt_ms : 0.0, g_pw_guest_dt.MaxMs(),
        g_pw_capture.AvgMs(), g_pw_capture.MaxMs(), g_pw_build.AvgMs(),
        g_pw_build.MaxMs(), g_pw_b2d.AvgMs(), g_pw_b2d.MaxMs(), g_pw_bspl.AvgMs(),
        g_pw_bspl.MaxMs(), g_pw_bpal.AvgMs(), g_pw_bpal.MaxMs(),
        g_dt_hist[0].exchange(0, std::memory_order_relaxed),
        g_dt_hist[1].exchange(0, std::memory_order_relaxed),
        g_dt_hist[2].exchange(0, std::memory_order_relaxed),
        g_dt_hist[3].exchange(0, std::memory_order_relaxed),
        g_dt_hist[4].exchange(0, std::memory_order_relaxed),
        g_pw_pal_tail.AvgMs(), g_pw_pal_tail.MaxMs(),
        g_pw_render.AvgMs(), g_pw_render.MaxMs(),
        g_pw_items.AvgMs(), g_pw_items.MaxMs(), g_pw_shadow.AvgMs(),
        g_pw_shadow.MaxMs(), g_pw_pre.AvgMs(), g_pw_pre.MaxMs(),
        g_pw_settle.AvgMs(), g_pw_settle.MaxMs(),
        g_pw_settle.count.load(std::memory_order_relaxed),
        g_warm_decodes.exchange(0, std::memory_order_relaxed),
        g_warm_deferred.exchange(0, std::memory_order_relaxed),
        g_pw_tail.AvgMs(), g_pw_tail.MaxMs(), g_pw_2d.AvgMs(), g_pw_2d.MaxMs(),
        g_pw_mesh_decode.count.load(std::memory_order_relaxed),
        g_pw_mesh_decode.AvgMs(), g_pw_mesh_decode.MaxMs(),
        g_pw_tex_decode.count.load(std::memory_order_relaxed), g_pw_tex_decode.AvgMs(),
        g_pw_tex_decode.MaxMs(), g_pw_commit.AvgMs(), g_pw_commit.MaxMs(),
        g_item_cache_hits.exchange(0, std::memory_order_relaxed),
        g_item_cache_builds.exchange(0, std::memory_order_relaxed),
        g_cam_changes.exchange(0, std::memory_order_relaxed),
        g_cam_repeats.exchange(0, std::memory_order_relaxed),
        g_cam_max_streak.exchange(0, std::memory_order_relaxed));
    // Deep per-item attribution (see the perf-items cvar): visibility-class
    // draw costs, completed-draw stage split, build-walk decomposition, and
    // the off-screen retention pass. Averages are per item (the windows Add
    // once per item), so av= is the per-item unit cost in microseconds.
    if (REXCVAR_GET(skate3_native_render_scene_perf_items)) {
      const auto avg_us = [](const PerfWindow& w) { return w.AvgMs() * 1000.0; };
      REXLOG_INFO(
          "native-scene perf-items: draw[vis n={} av={:.2f}us mx={:.2f}ms | "
          "occ n={} av={:.2f}us mx={:.2f}ms | out n={} av={:.2f}us "
          "mx={:.2f}ms ret_out={}] idx_k[vis={} occ={} out={}] "
          "grid[valid={} age={}] "
          "stages_us[mesh={:.2f} tex={:.2f} const={:.2f} submit={:.2f} n={}] "
          "build_us[core n={} av={:.2f} fp n={} av={:.2f} walk n={} av={:.2f} "
          "fetch av={:.2f}] wloop[{:.2f}/{:.2f}ms recs={}] "
          "retain[{:.2f}/{:.2f}ms app={} live={}]",
          g_pw_di_in.count.load(std::memory_order_relaxed), avg_us(g_pw_di_in),
          g_pw_di_in.MaxMs(),
          g_pw_di_occ.count.load(std::memory_order_relaxed),
          avg_us(g_pw_di_occ), g_pw_di_occ.MaxMs(),
          g_pw_di_out.count.load(std::memory_order_relaxed),
          avg_us(g_pw_di_out), g_pw_di_out.MaxMs(),
          g_vis_out_retained.exchange(0, std::memory_order_relaxed),
          g_vis_in_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_vis_occ_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_vis_out_indices.exchange(0, std::memory_order_relaxed) / 1000,
          g_r.occl_grid_valid ? 1 : 0,
          g_r.occl_grid_valid ? frames - g_r.occl_grid_frame : 0,
          avg_us(g_pw_di_mesh), avg_us(g_pw_di_tex), avg_us(g_pw_di_const),
          avg_us(g_pw_di_submit),
          g_pw_di_submit.count.load(std::memory_order_relaxed),
          g_pw_bi_core.count.load(std::memory_order_relaxed),
          avg_us(g_pw_bi_core),
          g_pw_bi_fp.count.load(std::memory_order_relaxed), avg_us(g_pw_bi_fp),
          g_pw_bi_walk.count.load(std::memory_order_relaxed),
          avg_us(g_pw_bi_walk), avg_us(g_pw_bi_fetch), g_pw_bi_wloop.AvgMs(),
          g_pw_bi_wloop.MaxMs(),
          g_bi_records.exchange(0, std::memory_order_relaxed),
          g_pw_bi_retain.AvgMs(), g_pw_bi_retain.MaxMs(),
          g_retained_appended.exchange(0, std::memory_order_relaxed),
          g_retained_live.load(std::memory_order_relaxed));
    }
    for (PerfWindow* w : {&g_pw_guest_dt, &g_pw_capture, &g_pw_build, &g_pw_pal_tail,
                          &g_pw_b2d, &g_pw_bspl, &g_pw_bpal,
                          &g_pw_render, &g_pw_items, &g_pw_shadow, &g_pw_pre,
                          &g_pw_settle, &g_pw_tail, &g_pw_2d, &g_pw_mesh_decode,
                          &g_pw_tex_decode, &g_pw_commit,
                          &g_pw_di_in, &g_pw_di_occ, &g_pw_di_out,
                          &g_pw_di_mesh, &g_pw_di_tex, &g_pw_di_const,
                          &g_pw_di_submit, &g_pw_bi_core, &g_pw_bi_fp,
                          &g_pw_bi_walk, &g_pw_bi_fetch, &g_pw_bi_wloop,
                          &g_pw_bi_retain}) {
      w->Reset();
    }
    // Refill the settle offender-log + slow-frame budgets for the next window.
    g_warm_mesh_log_budget.store(4, std::memory_order_relaxed);
    g_warm_tex_log_budget.store(4, std::memory_order_relaxed);
    g_slow_frame_log_budget.store(3, std::memory_order_relaxed);
  }
  if (frames % interval == 0 && REXCVAR_GET(skate3_native_render_scene_perf_log)) {
    uint32_t lw_ctxs = 0, lw_ents = 0;
    skate3::native_lw::QueryLwStats(&lw_ctxs, &lw_ents);
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} draws_2d={} drawn_2d={} "
        "splines[{}/{}] "
        "2d[other={} dropped={} askip={} astale={} textures={}] cached_meshes={} mesh_mb={} textures={} tex_mb={} "
        "vs_uploads={} palettes={} palette_base_plus1={} ropa[rigid={} stale={} rescued={} relax={} caster={} incoh={} stretch={} blend={} blendmiss={}] dyn_gap={} skinned={} skinned_skipped={} foreign_bank={} "
        "rigid[pending={} dropped={} worldprops={}] "
        "rej[dyn={} range={} chain={} geom={} draws={} bbox={}] "
        "rr[decode_fail={} no_bones={} mesh_deferred={} tex_deferred={}] "
        "store[n={} routes={} evict={}] "
        "heal[vfail={} demote={}] serve[sticky={} skipnew={} adstale={} adnone={}] "
        "shadow[valid={} ready={} draws={}] char[attempt={} valid={} drawn={} "
        "reused={}] dynobj[valid={} drawn={}] water[valid={} drawn={}] "
        "lw[ctxs={} ents={} stamp={} fade0={} resc={} fill={} ghost={} pal={} rows={}] "
        "refl[pair={} flat={} gate={:#x}] flips={} alt[bones={} shadow={}] "
        "rows_inst={} pal_srv={} occl_culled={} guest_skip={} build_skip={}",
        frames, scene.items.size(), drawn, g_draws_2d.load(), drawn_2d,
        drawn_spline, g_draws_spline.load(),
        g_draws_2d_other.load(), g_draws_2d_dropped.load(),
        g_2d_async_skip.load(), g_2d_async_stale.load(), g_r.tex_store.size(),
        g_r.meshes.size(), g_mesh_store_bytes >> 20, g_r.tex_store.size(),
        g_tex_store_bytes >> 20,
        g_vs_uploads.load(), g_palette_snapshots.load(), g_palette_base_plus1.load(),
        g_ropa_rigid.load(), g_ropa_stale.load(), g_ropa_rescued.load(),
        g_ropa_relaxed.load(), g_ropa_caster.load(), g_pub_incoherent.load(),
        g_stretch_veto.load(),
        g_ropa_blend_drawn.load(), g_ropa_blend_miss.load(), g_dyn_gap.load(),
        g_skinned_items.load(),
        g_skinned_skipped.load(), g_capture_foreign_bank.load(),
        g_rigid_pending.load(), g_rigid_dropped.load(),
        g_world_props.load(),
        g_rej_no_dynstate.load(), g_rej_dyn_range.load(),
        g_rej_chain.load(), g_rej_geom.load(), g_rej_draws.load(), g_rej_bbox.load(),
        g_rr_decode_fail.load(), g_rr_no_bones.load(), g_rr_mesh_deferred.load(),
        g_rr_tex_deferred.load(), g_r.tex_store.size(), g_r.tex_routes.size(),
        g_store_evicted.load(), g_heal_verify_fail.load(),
        g_demote_hold.load(),
        g_tex_sticky_served.load(), g_skip_new.load(), g_ad_stale_served.load(),
        g_ad_placeholder.load(), scene.shadow_valid,
        shadow_ready, shadow_draws,
        g_char_attempts.load(), g_char_valid.load(), g_char_drawn.load(),
        g_char_rows_reused.load(), scene.dynobj_valid,
        g_dynobj_drawn.load(), scene.water_valid, g_water_drawn.load(),
        lw_ctxs, lw_ents, g_lw_stamped.load(),
        g_lw_fade0.load(), g_lw_ctx_rescued.load(), g_lw_gap_filled.load(),
        g_ghost_suppressed.load(),
        g_lw_pal_sub.load(), g_lw_rows_served.load(),
        g_refl_pair.load(), g_refl_flat.load(), g_refl_gate.load(),
        g_hair_route_flips.load(), g_hair_bone_alternations.load(),
        g_shadow_alternations.load(), g_char_rows_inst_served.load(),
        g_pal_served_total.load(), g_occl_culled.load(),
        g_occl_guest_skipped.load(), g_occl_build_skipped.load());
  }
}

// ---- Graphics build-up showcase -------------------------------------------
// Sequencer for the layer-by-layer showcase (cvar
// skate3_native_render_scene_showcase / the bind_skate3_showcase hotkey):
// one wipe strips the frame to clay geometry, then each following wipe
// reveals the next visual layer, a vertical split sweeping left-to-right
// with BOTH sides rendered live (per-pixel stage selection in the shaders,
// no frozen frames). The split state rides the per-frame b1 rows
// (scene.hlsl sh_v2.yzw) and g_r.showcase_rows (the SSR composite reads it
// from root constants); every consumer treats an all-zero row as "showcase
// off". Runs on the render thread once per frame at b1 staging. The layer
// mask encoding matches the shader contract in scene.hlsl ShowcaseMask;
// the reveal order and grouping come from the user-editable
// skate3_native_render_scene_showcase_order cvar (kShowcaseLayers tokens).
static void TickShowcase(uint32_t output_width, bool hdr_on) {
  // One reveal step: the b1 row encoding (256 + the cumulative layer mask;
  // 0 = the full render) and its log label.
  struct Step {
    float value;
    std::string label;
  };
  struct State {
    bool active = false;
    std::vector<Step> steps;
    int logged = -1;
    double wipe_s = 3.0;
    double hold_s = 2.5;
    PerfClock::time_point t0;
  };
  static State s;
  float* rows = g_r.showcase_rows;
  rows[0] = rows[1] = rows[2] = 0.0f;
  const bool want = REXCVAR_GET(skate3_native_render_scene_showcase);
  if (!s.active) {
    if (!want) {
      // No run pending: release the showcase shader swap (the next
      // EnsurePipeline rebuild returns the family to the standard
      // variants with the showcase code compiled out).
      g_r.showcase_shaders_want = false;
      return;
    }
    // The run's shaders carry the split/mask gates only in their
    // SHOWCASE=1 variants: request the swap and hold the start until the
    // next EnsurePipeline rebuild has them live (rows stay zero, so the
    // waiting frames render normally).
    g_r.showcase_shaders_want = true;
    if (!g_r.showcase_shaders) {
      return;
    }
    // Layer availability: the material looks need nothing; the shadow and
    // post layers join only when their feature is enabled (the post gates
    // live in the HDR tonemap, so they need the HDR chain). The subtractive
    // layers need their content published/composited at all.
    const auto layer_available = [&](uint32_t bit) -> bool {
      switch (bit) {
        case 8u:
          return REXCVAR_GET(skate3_native_render_scene_shadows);
        case 16u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssao);
        case 32u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_ssr);
        case 64u:
          return hdr_on && (REXCVAR_GET(skate3_native_render_scene_shafts) ||
                            REXCVAR_GET(skate3_native_render_scene_haze));
        case 128u:
          return hdr_on && REXCVAR_GET(skate3_native_render_scene_bloom);
        case 256u:
          return REXCVAR_GET(skate3_native_render_scene_decals);
        case 512u:
          return REXCVAR_GET(skate3_native_render_scene_dynamic_items);
        default:
          return true;
      }
    };
    uint32_t avail_mask = 0;
    for (const auto& layer : kShowcaseLayers) {
      if (layer_available(layer.bit)) {
        avail_mask |= layer.bit;
      }
    }
    // Build the reveal steps from the user-ordered layer list (syntax in
    // the skate3_native_render_scene_showcase_order cvar description):
    // comma-separated steps, '+' joins layers into one wipe, '-' prefix =
    // disabled. Unknown, unavailable and duplicate tokens drop out; steps
    // left empty are skipped. The run opens ON black (mask bit 1024; a
    // snap, not a wipe - see the step-0 timing below), wipes clay in from
    // it, and closes by wiping back out to black. The >= hold_s stretches
    // of pure black double as machine-findable cut markers for screen
    // recordings (local/scripts/cut_showcase_clips.py pairs them with
    // ffmpeg blackdetect).
    s.steps.clear();
    s.steps.push_back({float(256u + 1024u), "black (start marker)"});
    s.steps.push_back({256.0f, "clay geometry"});
    uint32_t cum = 0;
    const std::string order =
        REXCVAR_GET(skate3_native_render_scene_showcase_order);
    size_t pos = 0;
    while (pos <= order.size()) {
      size_t end = order.find(',', pos);
      if (end == std::string::npos) {
        end = order.size();
      }
      const std::string group = order.substr(pos, end - pos);
      pos = end + 1;
      uint32_t add = 0;
      std::string label;
      size_t g = 0;
      while (g <= group.size()) {
        size_t ge = group.find('+', g);
        if (ge == std::string::npos) {
          ge = group.size();
        }
        std::string tok = group.substr(g, ge - g);
        g = ge + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
          tok.erase(tok.begin());
        }
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
          tok.pop_back();
        }
        for (char& ch : tok) {
          if (ch >= 'A' && ch <= 'Z') {
            ch = char(ch + 32);
          }
        }
        if (tok.empty() || tok[0] == '-') {
          continue;  // disabled layer (kept in the string for its position)
        }
        for (const auto& layer : kShowcaseLayers) {
          if (tok == layer.token) {
            if ((avail_mask & layer.bit) != 0 && (cum & layer.bit) == 0 &&
                (add & layer.bit) == 0) {
              add |= layer.bit;
              if (!label.empty()) {
                label += " & ";
              }
              label += layer.label;
            }
            break;
          }
        }
      }
      if (add != 0) {
        cum |= add;
        s.steps.push_back({float(256u + cum), std::move(label)});
      }
    }
    // Subtractive layers (decals/grime, dynamic entities) left out of the
    // order or '-'disabled are not build-up beats: fold their bits into
    // every step, clay included, so their content renders from the first
    // frame exactly as in the non-showcase frame. An UNAVAILABLE layer
    // needs no fold; its feature cvar already removes the content itself.
    const uint32_t always = kShowcaseSubtractiveMask & avail_mask & ~cum;
    if (always != 0u) {
      for (Step& st : s.steps) {
        st.value = float(256u + ((uint32_t(st.value) - 256u) | always));
      }
    }
    // A final full-render reveal when the list leaves layers unrevealed
    // (the materials look subsumes the albedo/lighting bits, so those two
    // stop counting once materials is in; the subtractive bits are either
    // revealed steps or folded in above, so they never force the step).
    const bool covered =
        (cum & 4u) != 0 && ((avail_mask & ~cum) & 0xF8u) == 0u;
    if (!covered) {
      s.steps.push_back({0.0f, "full render"});
    }
    // Closing blackout bookend (the end-of-run cut marker).
    s.steps.push_back({float(256u + 1024u), "black (end marker)"});
    // Durations snapshot at start so a live cvar edit cannot jump the
    // elapsed-time -> step mapping mid-run.
    s.wipe_s = std::max(0.2, REXCVAR_GET(skate3_native_render_scene_showcase_wipe));
    s.hold_s = std::max(0.0, REXCVAR_GET(skate3_native_render_scene_showcase_hold));
    s.t0 = PerfClock::now();
    s.logged = -1;
    s.active = true;
    REXLOG_INFO(
        "native-scene: showcase started ({} steps, wipe {:.1f}s, hold "
        "{:.1f}s, hdr={}, order=\"{}\")",
        s.steps.size(), s.wipe_s, s.hold_s, hdr_on ? 1 : 0, order);
  }
  if (!want) {
    s.active = false;
    g_r.showcase_shaders_want = false;
    REXLOG_INFO("native-scene: showcase cancelled");
    return;
  }
  const double elapsed =
      std::chrono::duration<double>(PerfClock::now() - s.t0).count();
  const double per_step = s.wipe_s + s.hold_s;
  // Step 0 (the blackout start bookend) SNAPS to black instead of wiping
  // from the live frame and lasts hold_s only: the recording cut lands on
  // a hard black edge, and the build-up proper starts with clay wiping in
  // from black.
  int idx;
  double phase;
  if (elapsed < s.hold_s) {
    idx = 0;
    phase = s.wipe_s;  // past the wipe window: hold, no split
  } else {
    const double e = elapsed - s.hold_s;
    idx = 1 + int(e / per_step);
    phase = e - double(idx - 1) * per_step;
  }
  if (idx >= int(s.steps.size())) {
    s.active = false;
    g_r.showcase_shaders_want = false;
    REXCVAR_SET(skate3_native_render_scene_showcase, false);
    REXLOG_INFO("native-scene: showcase finished");
    return;
  }
  const float cur = s.steps[idx].value;
  if (idx != s.logged) {
    REXLOG_INFO("native-scene: showcase step {}/{} - {}", idx + 1,
                s.steps.size(), s.steps[idx].label);
    s.logged = idx;
  }
  const float prev = idx == 0 ? 0.0f : s.steps[idx - 1].value;
  if (phase < s.wipe_s) {
    float f = float(phase / s.wipe_s);
    f = f * f * (3.0f - 2.0f * f);  // ease the sweep in and out
    rows[0] = cur;
    rows[1] = prev;
    // Overshoot slightly so the divider line exits the screen edge cleanly.
    rows[2] = f * float(output_width) * 1.02f;
  } else {
    rows[0] = rows[1] = cur;
    rows[2] = 0.0f;
  }
}

// True when the item's world-space bbox is provably hidden behind already-
// rendered geometry: its NEAREST corner view-depth is farther than the
// FARTHEST scene depth (tile MAX) in every occlusion-grid tile it covers.
// The corners are projected with the view_proj the grid's depth was
// rendered with (1-2 frames old; see the reduce in ApplySsaoPass), keeping
// bounds and depth consistent under camera motion. Anything not provable -
// invalid grid, a near-plane crosser, bounds leaving that frame's frustum -
// classifies as visible; the consumers (telemetry and the occlusion cull)
// must never overstate occlusion. `depth_margin` is the extra distance (m)
// the item must sit behind every tile's far depth: the telemetry uses a
// small margin (0.25, enough that an item's own last-frame depth counts as
// visible), the cull a larger one for disocclusion safety at speed.
bool ItemOccludedByGrid(const DrawItem& it, float depth_margin) {
  if (!g_r.occl_grid_valid ||
      g_r.occl_grid.size() <
          size_t(RendererState::kOcclGridW) * RendererState::kOcclGridH) {
    return false;
  }
  const float* vp = g_r.occl_grid_vp;
  float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
  float min_z = 3.4e38f;
  for (int c = 0; c < 8; ++c) {
    const float l[3] = {c & 1 ? it.bbox_max[0] : it.bbox_min[0],
                        c & 2 ? it.bbox_max[1] : it.bbox_min[1],
                        c & 4 ? it.bbox_max[2] : it.bbox_min[2]};
    const float* w = it.world;
    float p[3];
    for (int k = 0; k < 3; ++k) {
      p[k] = l[0] * w[0 * 4 + k] + l[1] * w[1 * 4 + k] + l[2] * w[2 * 4 + k] +
             w[3 * 4 + k];
    }
    float clip[4];
    for (int k = 0; k < 4; ++k) {
      clip[k] = p[0] * vp[0 * 4 + k] + p[1] * vp[1 * 4 + k] +
                p[2] * vp[2 * 4 + k] + vp[3 * 4 + k];
    }
    if (clip[3] < 0.25f || clip[2] < 0.0f || clip[0] < -clip[3] ||
        clip[0] > clip[3] || clip[1] < -clip[3] || clip[1] > clip[3]) {
      return false;
    }
    const float u = 0.5f + 0.5f * clip[0] / clip[3];
    const float v = 0.5f - 0.5f * clip[1] / clip[3];
    u0 = std::min(u0, u);
    v0 = std::min(v0, v);
    u1 = std::max(u1, u);
    v1 = std::max(v1, v);
    min_z = std::min(min_z, clip[3]);  // row-vector proj: w == view Z
  }
  const int gw = int(RendererState::kOcclGridW);
  const int gh = int(RendererState::kOcclGridH);
  const int tx0 = std::clamp(int(u0 * float(gw)), 0, gw - 1);
  const int tx1 = std::clamp(int(u1 * float(gw)), 0, gw - 1);
  const int ty0 = std::clamp(int(v0 * float(gh)), 0, gh - 1);
  const int ty1 = std::clamp(int(v1 * float(gh)), 0, gh - 1);
  const float need = min_z - depth_margin;
  for (int ty = ty0; ty <= ty1; ++ty) {
    const float* row = &g_r.occl_grid[size_t(ty) * gw];
    for (int tx = tx0; tx <= tx1; ++tx) {
      if (row[tx] >= need) {
        return false;
      }
    }
  }
  return true;
}

// Culled-ctx publication for the guest-side dispatch filter: the classify
// loop collects the MeshContexts of statics it occlusion-culled and swaps
// them in here once per rendered frame (empty when the cull is inactive, so
// the guest filter clears within a frame). The timestamp lets the consumer
// expire the set when the native render idles.
std::mutex g_occl_pub_mutex;
std::vector<uint32_t> g_occl_pub_ctxs;
std::atomic<int64_t> g_occl_pub_ms{0};

}  // namespace

int WriteNativeFrameToTexture(uint8_t* base, uint32_t texture) {
  uint32_t w[3] = {};
  if (texture < 0x10000 || !GuestTryLoadU32(base, texture + 0x1C, &w[0]) ||
      !GuestTryLoadU32(base, texture + 0x20, &w[1]) ||
      !GuestTryLoadU32(base, texture + 0x24, &w[2])) {
    return -1;
  }
  GrabTarget t;
  t.addr = w[1] & 0x1FFFF000u;
  t.width = (w[2] & 0x1FFFu) + 1;
  t.height = ((w[2] >> 13) & 0x1FFFu) + 1;
  t.tiled = (w[0] >> 31) != 0;
  t.pitch = ((w[0] >> 22) & 0x1FFu) * 32u;
  if (t.pitch < t.width) {
    t.pitch = (t.width + 31u) & ~31u;
  }
  if ((w[1] & 0x3Fu) != 6u || t.addr == 0 || t.width > RendererState::kBlurWidth ||
      t.height > RendererState::kBlurHeight || t.width < 16 || t.height < 16) {
    return -2;
  }
  std::lock_guard<std::mutex> lock(g_grab_mutex);
  if (g_grab_cache.empty()) {
    return -3;
  }
  return WriteGrabToGuest(base, g_grab_cache.data(), t) ? 1 : -4;
}

void DumpGuestTextureForDiag(uint8_t* base, uint32_t texture, const char* path) {
  uint32_t w[3] = {};
  if (texture < 0x10000 || !GuestTryLoadU32(base, texture + 0x1C, &w[0]) ||
      !GuestTryLoadU32(base, texture + 0x20, &w[1]) ||
      !GuestTryLoadU32(base, texture + 0x24, &w[2]) || (w[1] & 0x3Fu) != 6u) {
    return;
  }
  const uint32_t addr = w[1] & 0x1FFFF000u;
  const uint32_t width = (w[2] & 0x1FFFu) + 1;
  const uint32_t height = ((w[2] >> 13) & 0x1FFFu) + 1;
  const bool tiled = (w[0] >> 31) != 0;
  uint32_t pitch = ((w[0] >> 22) & 0x1FFu) * 32u;
  if (pitch < width) {
    pitch = (width + 31u) & ~31u;
  }
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    return;
  }
  std::fprintf(f, "P6 %u %u 255 ", width, height);
  std::vector<uint8_t> row(size_t(width) * 3);
  const uint8_t* mem = base + (0xA0000000u | addr);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t off =
          tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y),
                                                                          pitch, 2))
                : (y * pitch + x) * 4u;
      // Big-endian ARGB (see WriteGrabToGuest).
      row[x * 3 + 0] = mem[off + 1];
      row[x * 3 + 1] = mem[off + 2];
      row[x * 3 + 2] = mem[off + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  REXLOG_INFO("native-scene: DIAG photo target {:08X} ({}x{}) dumped to {}", addr, width,
              height, path);
}

int SyncWriteNewestGrab(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_grab_mutex);
  if (!g_grab_cache.empty()) {
    GrabTarget targets[2];
    const int target_count = ResolveGrabTargets(base, targets);
    bool wrote = target_count > 0;
    for (int ti = 0; ti < target_count; ++ti) {
      wrote = WriteGrabToGuest(base, g_grab_cache.data(), targets[ti]) && wrote;
    }
    return wrote ? 1 : -4;
  }
  if (g_r.device == nullptr || g_r.grab_failed || g_r.grab_readback[0] == nullptr) {
    return -1;  // readback pair never created (no frame produced yet)
  }
  // Prefer the newest copy whose GPU work has COMPLETED. The very newest
  // one usually sits in the render thread's still-open submission, which is
  // only submitted at the end of a frame that cannot finish while the game
  // thread waits in here (every wait on it timed out: code -3).
  const uint64_t completed = g_r.device->CompletedSubmission();
  int newest = -1;
  for (int i = 0; i < 2; ++i) {
    if (g_r.grab_submission[i] != 0 && g_r.grab_submission[i] < completed &&
        (newest < 0 || g_r.grab_submission[i] > g_r.grab_submission[newest])) {
      newest = i;
    }
  }
  if (newest < 0) {
    for (int i = 0; i < 2; ++i) {
      if (g_r.grab_submission[i] != 0 &&
          (newest < 0 || g_r.grab_submission[i] > g_r.grab_submission[newest])) {
        newest = i;
      }
    }
  }
  if (newest < 0) {
    return -2;  // nothing produced
  }
  // Only reached waiting when no copy has completed yet.
  const auto t0 = PerfClock::now();
  while (g_r.device->CompletedSubmission() <= g_r.grab_submission[newest]) {
    if (PerfClock::now() - t0 > std::chrono::milliseconds(200)) {
      return -3;  // GPU copy did not complete in time
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  g_r.device->InvalidateForRead(
      g_r.grab_readback[newest], 0,
      uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight);
  GrabTarget targets[2];
  const int target_count = ResolveGrabTargets(base, targets);
  bool wrote = target_count > 0;
  for (int ti = 0; ti < target_count; ++ti) {
    wrote = WriteGrabToGuest(base, g_r.grab_readback_ptr[newest], targets[ti]) && wrote;
  }
  return wrote ? 1 : -4;  // -4: targets unresolvable / write failed
}

bool CopyOcclusionCulledCtxs(std::vector<uint32_t>& out) {
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (now_ms - g_occl_pub_ms.load(std::memory_order_relaxed) > 250) {
    out.clear();
    return false;
  }
  std::lock_guard<std::mutex> lock(g_occl_pub_mutex);
  out = g_occl_pub_ctxs;
  return !out.empty();
}

void AddGuestOcclSkipped(uint32_t n) {
  g_occl_guest_skipped.fetch_add(n, std::memory_order_relaxed);
}

bool SkipGuestZPrepass(uint32_t view) {
  if (!REXCVAR_GET(skate3_native_render_scene_skip_guest_zprepass) || !SceneEnabled() ||
      view == 0 || view != g_main_scene_view.load(std::memory_order_relaxed)) {
    return false;
  }
  // g_occl_pub_ms is stamped by every rendered (non-portrait) native frame:
  // stale = the native renderer is not drawing the world (loading, boot
  // frontend), so the emulated frame may be the one on screen.
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ms - g_occl_pub_ms.load(std::memory_order_relaxed) <= 250;
}

namespace {

// PhotoEffects portrait job being rendered (command processor thread; see
// PhotoRender): RenderScene then draws this scene offscreen only.
bool g_photo_mode = false;
std::shared_ptr<const FrameScene> g_photo_scene;
std::shared_ptr<const PhotoLayers> g_photo_layers;  // the job's 2D composition

// One-pass VR stereo lens mask (RenderScene main-pass setup): a depth-only
// variant of the 2D pipeline drawing each eye's hidden-area mesh at depth 0,
// and its vertex ring.
nrhi::Pipeline* g_vr_mask_pso = nullptr;
// Curved third-person HUD (RenderScene 2D tail): the HUD layer drawn flat
// into this 16:9 texture exactly as on a 16:9 screen, then wrapped onto each
// eye's curved mesh (skate3::vr::HudCurveMesh). RGBA8: real coverage alpha
// (the guest output's 2-bit alpha cannot carry the HUD's soft edges).
nrhi::Texture* g_vr_hud_tex = nullptr;
nrhi::TextureView* g_vr_hud_srv = nullptr;
constexpr uint32_t kVrHudWidth = 2560;
constexpr uint32_t kVrHudHeight = 1440;

bool EnsureVrHudTexture(const NativeGuestOutputRenderContext& context) {
  if (g_vr_hud_tex != nullptr) {
    return g_vr_hud_srv != nullptr;
  }
  nrhi::TextureDesc desc;
  desc.width = kVrHudWidth;
  desc.height = kVrHudHeight;
  desc.format = nrhi::Format::kR8G8B8A8_UNORM;
  desc.usage = nrhi::kTextureUsageRenderTarget;
  desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
  g_vr_hud_tex = context.device->CreateTexture(desc);
  if (g_vr_hud_tex == nullptr) {
    return false;
  }
  nrhi::TextureViewDesc vd;
  vd.mip_levels = 1;
  g_vr_hud_srv = context.device->CreateTextureView(g_vr_hud_tex, vd);
  return g_vr_hud_srv != nullptr;
}
uint32_t g_vr_mask_key = 0;
nrhi::Buffer* g_vr_mask_vb = nullptr;
uint8_t* g_vr_mask_vb_cpu = nullptr;
uint32_t g_vr_mask_slot = 0;

// VR: the left eye's scene and 2D stream, rendered again by the right eye
// of the same swap. The guest publishes asynchronously; taking the newest
// scene per pass gave the two eyes different game frames (the skater and
// camera a sim step apart between the eyes).
std::shared_ptr<const FrameScene> g_vr_eye_scene;
std::vector<Draw2d> g_vr_eye_2d;
std::vector<SplineDraw> g_vr_eye_splines;
// A swap's readiness/content decision must be identical for both eyes.
// Copies are non-owning; renderer deferred destruction keeps views alive.
struct VrOverlayTexture { GuestTexture texture; int kind = 0; }; // 0 pending, 1 white, 2 ready
std::map<std::array<uint32_t,8>, VrOverlayTexture> g_vr_overlay_textures;

// ---- Native inter-frames (rex::graphics::NativeInterFrameHooks) -----------
// The guest render thread bounds the guest frame rate (the game's own render
// code plus the capture/build hooks run there), while the command processor
// thread and the GPU have headroom. Between two guest swaps the command
// processor presents up to N extra frames.
//
// Every present while inter-frames are on (swap frames too) shows the last
// two swapped scenes INTERPOLATED at the present time: the camera and every
// dynamic item that exists in both (bone palettes, rigid worlds) at
// prev + (cur - prev) * alpha, alpha = time since cur's swap over the last
// guest frame interval, clamped to [0, 1]. A swap frame therefore shows the
// previous scene's poses (alpha 0) and the inter-frames walk them to cur:
// one guest frame of extra latency, but every rendered pose lies between two
// real ones. (Extrapolating past cur instead amplified the frame-to-frame
// noise of the captured palettes into visible skater flicker.)
//
// Fast-spinning bones (wheels: up to ~100 degrees per guest frame at speed)
// cannot be lerped: their translation carries the axle-pivot compensation of
// a rotation about the model origin. They ride rigidly on the nearest
// non-spinning bone (truck/deck/body) with cur's spin angle; wheels are
// rotationally symmetric, so snapping the angle is invisible.
// Command processor thread only (the hooks and RenderScene).

namespace {

double InterFrameNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A posed slow rigid piece, as a column-vector 3x4 affine (the bone palette
// convention): skinned spinning bones can ride it (retail's board wheels are
// skinned over the character bank while the deck and trucks are rigid).
struct InterFrameRigidAnchor {
  float cur[12];
  float posed[12];
  float center[3];
};

struct InterFrameState {
  std::shared_ptr<const FrameScene> cur;   // scene of the last swap render
  std::shared_ptr<const FrameScene> prev;  // the scene swapped before it
  uint64_t cur_generation = 0;
  double cur_t = 0.0;  // first present of cur
  double dt = 0.0;     // cur_t - first present of prev
  // Content-clock pacing (skate3_native_render_scene_present_pacing): every
  // present shows content time (present time - lat) on the scenes' own
  // playback clock (FrameScene::play_time), interpolated between prev and
  // cur. lat tracks the age of a scene at its first present (+ one content
  // interval while inter-frames run, + a small margin without), smoothed so
  // the content clock advances at real-time rate: motion steps then match
  // present intervals however unevenly the guest frames and the presents
  // arrive (the arrival-time alpha above held and jumped whenever a guest
  // frame came early or late).
  double lat = 0.0;
  bool lat_valid = false;
  double content_dt = 0.0;  // EMA of the scenes' content intervals
  // Render-thread cost of one present (RenderScene, EMA seconds): inter-
  // frames are spaced at least this far apart and never started when they
  // would still be rendering as the next guest frame arrives. Requesting
  // them on the plain dt/(n+1) grid while each took longer than a slot
  // queued them back to back: the presents bunched, and real frames were
  // replaced before they were ever shown.
  double render_cost = 0.002;
  // Display-time pacing: what a present shows is decided when its render
  // starts but seen when it ends, and swap presents (scene pairing, uploads,
  // the shadow atlas) cost more than inter-frames; posing both at their
  // render START showed swap frames' content older than the inter-frames'
  // around them, a judder at the guest rate. Costs per kind (EMA seconds),
  // the swap present's end, and a smoothed guest interval for scheduling.
  double real_cost = 0.002;
  double inter_cost = 0.0015;
  // Mean absolute deviation of each (EMA seconds): planned display times
  // use cost + 1.5 x deviation, so ~90% of renders finish before their
  // planned time and are held to it (NativeInterFrameHooks::present_at).
  double real_mad = 0.0004;
  double inter_mad = 0.0003;
  // Planned display time of the present being rendered (steady_clock s,
  // 0 = none: hand over at once).
  double present_at = 0.0;
  double cur_end = 0.0;
  uint64_t cur_end_gen = 0;
  double dt_ema = 0.0;
  int requested = 0;   // inter-frames asked for since cur
  bool valid = false;  // prev -> cur motion usable (no cut, sane interval)
  bool camera = false;
  struct Match {
    uint32_t cur;
    uint32_t prev;
    bool bones;
    bool world;
  };
  std::vector<Match> matches;
  uint32_t moving = 0;       // moving items in cur (match-rate stats)
  std::vector<float> saved;  // restore scratch (InterFrameAdvance)
  std::vector<int32_t> anchor;  // per-bone scratch: -1 lerp, else anchor bone
  std::vector<float> rot_cos;   // per-bone scratch: prev -> cur rotation cosine
  std::vector<float> deltas;    // per-bone scratch: prev -> cur translation
  std::vector<std::pair<uint32_t, size_t>> fast_world;  // InterFrameAdvance scratch
  std::vector<InterFrameRigidAnchor> rigid_anchors;
  std::vector<size_t> bones_at;  // per match: restore offset in `saved` (npos: none)
  std::vector<size_t> world_at;
  std::unordered_map<uint64_t, std::array<uint64_t, 4>> pools;
  std::vector<uint64_t> match_pool;
  // Scene generation (and match count) pools/match_pool were built for:
  // every present of one guest frame poses the same captured palettes (the
  // advance restores them), so they are built once per published scene.
  uint64_t pools_gen = ~0ull;
  size_t pools_matches = 0;
  std::vector<std::pair<uint32_t, size_t>> slow_world;
};
InterFrameState g_inter;

// Conservative render cost of a present of this kind (seconds).
double InterFramePlannedCost(bool inter) {
  return inter ? g_inter.inter_cost + 1.5 * g_inter.inter_mad
               : g_inter.real_cost + 1.5 * g_inter.real_mad;
}

// NativeInterFrameHooks::present_at.
double InterFramePresentAt(void* /*user_data*/) { return g_inter.present_at; }

// The presented-rate target (Hz): the override cvar, else the refresh rate of
// the monitor the game window is on, re-read every 2 s (window moves, mode
// changes).
BOOL CALLBACK InterFrameFindOwnWindow(HWND wnd, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(wnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(wnd) && GetWindow(wnd, GW_OWNER) == nullptr) {
    *reinterpret_cast<HWND*>(out) = wnd;
    return FALSE;
  }
  return TRUE;
}
double InterFrameTargetHz() {
  const double forced = REXCVAR_GET(skate3_native_render_scene_interframes_target_hz);
  if (forced > 0.0) {
    return forced;
  }
  static double s_hz = 60.0;
  static double s_next = 0.0;
  const double now = InterFrameNow();
  if (now >= s_next) {
    s_next = now + 2.0;
    HWND wnd = nullptr;
    EnumWindows(&InterFrameFindOwnWindow, reinterpret_cast<LPARAM>(&wnd));
    HMONITOR mon = wnd != nullptr ? MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY)
                                  : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1) {
      const double hz = double(dm.dmDisplayFrequency);
      if (hz != s_hz) {
        REXLOG_INFO("native-scene: frame interpolation target {} Hz (monitor refresh)", hz);
      }
      s_hz = hz;
    }
  }
  return s_hz;
}

// Generated frames per guest frame right now: fill each guest frame up to
// the target rate (floor(target x guest interval) presents, one of them the
// real frame), at most the cvar's count. A guest rate at or above the target
// gets none: frames beyond the display's rate are never fully shown, and on
// the one render thread they only crowd the real frames (measured uncapped
// at ~215 fps: worse motion steps than no interpolation). The count moves
// only once the rate is ~4% past a boundary, so a rate hovering at one does
// not flip it every few frames.
int InterFrameActiveCount() {
  const int n_max = REXCVAR_GET(skate3_native_render_scene_interframes);
  static int s_n = 0;
  if (n_max <= 0) {
    s_n = 0;
    return 0;
  }
  const double dt = g_inter.dt_ema > 0.0 ? g_inter.dt_ema : g_inter.dt;
  if (!(dt > 0.0)) {
    return 0;
  }
  const double x = InterFrameTargetHz() * dt;  // presents per guest frame at the target
  const int up = int(std::floor(x * 0.96)) - 1;
  const int down = int(std::floor(x * 1.04)) - 1;
  int n = s_n;
  if (n < up) n = up;
  if (n > down) n = down;
  n = std::clamp(n, 0, n_max);
  if (n != s_n) {
    static uint32_t s_logs = 0;
    if (s_logs < 200) {
      ++s_logs;
      REXLOG_INFO("native-scene: frame interpolation {} generated frame(s) per real frame "
                  "(real {:.1f} fps, target {:.0f} Hz)",
                  n, 1.0 / dt, InterFrameTargetHz());
    }
    s_n = n;
  }
  return n;
}

bool InterFrameMoving(const DrawItem& item) {
  static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  return (item.skinned && !item.bones.empty()) ||
         (item.ctx != 0 && std::memcmp(item.world, kIdent, sizeof(kIdent)) != 0);
}

// Identity of a moving piece across scenes. NOT the vertex buffer: cloth
// (ROPA) garments ping-pong their vertices between two buffer objects every
// frame, so a vb-keyed garment never matched and stood still while its body
// moved.
uint64_t InterFrameKey(const DrawItem& item, uint32_t occurrence) {
  uint64_t h = 1469598103934665603ull;
  for (const uint64_t v : {uint64_t(item.mesh), uint64_t(item.ctx), uint64_t(item.ib_obj),
                           uint64_t(occurrence)}) {
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

void InterFrameWorldCenter(const DrawItem& d, const float* w, float out[3]);

// A swap render presented `scene`: on a new generation, pair its moving
// items with the previous scene's.
// Where a skinned piece's bone b actually puts its vertices: the bone matrix
// (column-vector 3x4) applied to the bind-space centroid of the vertices it
// drives (DecodedMesh::bone_centroids). The bare translation column is the
// image of the model ORIGIN, which for a spinning bone (a car wheel) sweeps a
// circle of the pivot's radius; it is used only when no centroid is known.
void InterFrameBonePos(const DrawItem& d, size_t b, float out[3]) {
  const float* m = &d.bones[b * 12];
  float c[3] = {0.0f, 0.0f, 0.0f};
  const auto mit = g_r.meshes.find(d.mesh);
  if (mit != g_r.meshes.end() && b < mit->second.bone_centroids.size() &&
      mit->second.bone_centroids[b][3] > 1e-3f) {
    const std::array<float, 4>& s = mit->second.bone_centroids[b];
    c[0] = s[0] / s[3];
    c[1] = s[1] / s[3];
    c[2] = s[2] / s[3];
  }
  for (int r = 0; r < 3; ++r) {
    out[r] = m[r * 4 + 0] * c[0] + m[r * 4 + 1] * c[1] + m[r * 4 + 2] * c[2] + m[r * 4 + 3];
  }
}

void InterFrameNoteSwap(const std::shared_ptr<const FrameScene>& scene) {
  if (g_inter.cur && scene->generation == g_inter.cur_generation) {
    return;  // the same scene presented again (the guest published nothing new)
  }
  const double now = InterFrameNow();
  g_inter.prev = std::move(g_inter.cur);
  g_inter.cur = scene;
  g_inter.cur_generation = scene->generation;
  g_inter.dt = now - g_inter.cur_t;
  g_inter.cur_t = now;
  g_inter.requested = 0;
  if (g_inter.dt > 0.0005 && g_inter.dt < 0.1) {
    g_inter.dt_ema = g_inter.dt_ema > 0.0 ? g_inter.dt_ema + (g_inter.dt - g_inter.dt_ema) * 0.1
                                          : g_inter.dt;
  }
  if (g_inter.prev && scene->play_time > 0.0 && g_inter.prev->play_time > 0.0 &&
      scene->play_time > g_inter.prev->play_time) {
    const double content_dt = scene->play_time - g_inter.prev->play_time;
    g_inter.content_dt = g_inter.content_dt > 0.0
                             ? g_inter.content_dt + (content_dt - g_inter.content_dt) * 0.05
                             : content_dt;
    // Inter-frames interpolate prev -> cur across one content interval (only
    // two scenes are kept, so the content clock cannot sit further back than
    // prev); a late guest frame then extrapolates slightly past cur instead
    // of holding (see InterFrameAdvance). Without inter-frames only
    // render-start jitter needs covering.
    const double margin = InterFrameActiveCount() > 0
                              ? g_inter.content_dt
                              : 0.001;
    // Age at this swap present's predicted DISPLAY time (render end).
    const double target = (now + InterFramePlannedCost(false) - scene->play_time) + margin;
    if (!g_inter.lat_valid || std::fabs(target - g_inter.lat) > 0.05) {
      g_inter.lat = target;  // first sample / pause / clock reset: snap
      g_inter.lat_valid = true;
    } else {
      g_inter.lat += (target - g_inter.lat) * 0.03;
    }
  } else if (scene->play_time <= 0.0) {
    g_inter.lat_valid = false;
  }
  g_inter.valid = false;
  g_inter.camera = false;
  g_inter.matches.clear();
  g_inter.moving = 0;
  if (!g_inter.prev || g_inter.prev->items.empty() || g_inter.dt <= 0.0005 ||
      g_inter.dt > 0.1) {
    return;
  }
  const FrameScene& c = *g_inter.cur;
  const FrameScene& p = *g_inter.prev;
  // Camera cut: a jump no guest frame of real motion makes.
  const float dx = c.cam_pos[0] - p.cam_pos[0];
  const float dy = c.cam_pos[1] - p.cam_pos[1];
  const float dz = c.cam_pos[2] - p.cam_pos[2];
  // Rotation: the normalized basis rows of view_proj's 3x3 (the projection's
  // per-axis scale cancels in the normalization). The projection itself is
  // interpolated too: Skate 3 widens the FOV with speed, so it changes on
  // most frames while pushing (requiring it unchanged disabled the camera
  // half the time, and a stepping camera under interpolated objects reads
  // as the skater twitching).
  float rot_cos = 1.0f;
  for (int r = 0; r < 3; ++r) {
    const float* a = &c.view_proj[r * 4];
    const float* b = &p.view_proj[r * 4];
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la > 1e-6f && lb > 1e-6f) {
      rot_cos = std::min(rot_cos, (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb));
    }
  }
  g_inter.camera = dx * dx + dy * dy + dz * dz < 1.0f && rot_cos > 0.866f;  // 30 degrees
  // Identity groups: pieces with the same mesh, context and index buffer. A
  // group can hold several pieces (retail draws a board's two trucks and four
  // wheels as rigid instances of one mesh each), and their draw ORDER changes
  // from frame to frame, so pieces inside a group pair by nearest position:
  // pairing by draw order blended the front truck with the back one.
  std::unordered_map<uint64_t, std::vector<uint32_t>> prev_groups;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cur_groups;
  prev_groups.reserve(p.items.size());
  for (uint32_t i = 0; i < uint32_t(p.items.size()); ++i) {
    if (InterFrameMoving(p.items[i])) {
      prev_groups[InterFrameKey(p.items[i], 0)].push_back(i);
    }
  }
  for (uint32_t i = 0; i < uint32_t(c.items.size()); ++i) {
    if (InterFrameMoving(c.items[i])) {
      ++g_inter.moving;
      cur_groups[InterFrameKey(c.items[i], 0)].push_back(i);
    }
  }
  std::vector<uint8_t> prev_claimed(p.items.size(), 0);
  std::vector<uint8_t> cur_paired(c.items.size(), 0);
  const auto root_of = [](const DrawItem& d, float out[3]) {
    if (!d.bones.empty() && d.bones.size() >= 12) {
      InterFrameBonePos(d, 0, out);
    } else {
      InterFrameWorldCenter(d, d.world, out);
    }
  };
  const auto dist2 = [&](uint32_t ci, uint32_t pj) {
    float a3[3], b3[3];
    root_of(c.items[ci], a3);
    root_of(p.items[pj], b3);
    return (a3[0] - b3[0]) * (a3[0] - b3[0]) + (a3[1] - b3[1]) * (a3[1] - b3[1]) +
           (a3[2] - b3[2]) * (a3[2] - b3[2]);
  };
  std::vector<std::pair<uint32_t, uint32_t>> pairs;  // cur, prev
  struct Cand {
    float d2;
    uint32_t ci;
    uint32_t pj;
  };
  std::vector<Cand> cands;
  for (const auto& [key, members] : cur_groups) {
    const auto pg = prev_groups.find(key);
    if (pg == prev_groups.end()) {
      continue;
    }
    if (members.size() == 1 && pg->second.size() == 1) {
      cur_paired[members[0]] = 1;
      prev_claimed[pg->second[0]] = 1;
      pairs.emplace_back(members[0], pg->second[0]);
      continue;
    }
    // Several instances: closest pairs first (globally within the group).
    cands.clear();
    for (const uint32_t ci : members) {
      for (const uint32_t pj : pg->second) {
        const float d2 = dist2(ci, pj);
        if (d2 < 1.0f) {
          cands.push_back({d2, ci, pj});
        }
      }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& x, const Cand& y) { return x.d2 < y.d2; });
    for (const Cand& cd : cands) {
      if (!cur_paired[cd.ci] && !prev_claimed[cd.pj]) {
        cur_paired[cd.ci] = 1;
        prev_claimed[cd.pj] = 1;
        pairs.emplace_back(cd.ci, cd.pj);
      }
    }
  }
  // Pieces whose context moved (dynamic entities draw from per-frame arenas)
  // miss their group: pair them with the nearest unclaimed previous piece of
  // the same mesh and index buffer within 1 m.
  std::vector<uint32_t> unkeyed;
  for (uint32_t i = 0; i < uint32_t(c.items.size()); ++i) {
    if (!cur_paired[i] && InterFrameMoving(c.items[i])) {
      unkeyed.push_back(i);
    }
  }
  if (!unkeyed.empty()) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_mesh;
    for (uint32_t j = 0; j < uint32_t(p.items.size()); ++j) {
      if (!prev_claimed[j] && InterFrameMoving(p.items[j])) {
        by_mesh[(uint64_t(p.items[j].mesh) << 32) | p.items[j].ib_obj].push_back(j);
      }
    }
    for (const uint32_t i : unkeyed) {
      const DrawItem& it = c.items[i];
      const auto bit = by_mesh.find((uint64_t(it.mesh) << 32) | it.ib_obj);
      if (bit == by_mesh.end()) {
        continue;
      }
      float ci[3];
      root_of(it, ci);
      int32_t best = -1;
      float best_d2 = 1.0f;
      for (const uint32_t j : bit->second) {
        if (prev_claimed[j]) {
          continue;
        }
        float pj[3];
        root_of(p.items[j], pj);
        const float d2 = (ci[0] - pj[0]) * (ci[0] - pj[0]) + (ci[1] - pj[1]) * (ci[1] - pj[1]) +
                         (ci[2] - pj[2]) * (ci[2] - pj[2]);
        if (d2 < best_d2) {
          best_d2 = d2;
          best = int32_t(j);
        }
      }
      if (best >= 0) {
        prev_claimed[uint32_t(best)] = 1;
        pairs.emplace_back(i, uint32_t(best));
      }
    }
  }
  for (const auto& [i, j] : pairs) {
    const DrawItem& it = c.items[i];
    const DrawItem& pv = p.items[j];
    InterFrameState::Match m{i, j, false, false};
    if (!it.bones.empty() && it.bones.size() == pv.bones.size() && it.bones.size() >= 12) {
      // How far each referenced bone moved its VERTICES this guest frame:
      // the bone matrix applied to the bind-space centroid of the vertices it
      // drives (DecodedMesh::bone_centroids). The raw translation column is
      // not a position: for a spinning bone (a car wheel, whose pivot is not
      // the model origin) it sweeps a circle of the pivot's radius every
      // revolution, which read as a >1 m "teleport" / outlier on most frames
      // and left every traffic vehicle un-interpolated (stepping at the
      // guest rate while the camera and skater moved smoothly: obvious when
      // skitching). Bones without a centroid fall back to the column.
      const auto mit = g_r.meshes.find(it.mesh);
      const std::vector<std::array<float, 4>>* cens =
          mit != g_r.meshes.end() ? &mit->second.bone_centroids : nullptr;
      const auto bone_move = [&](size_t b) -> float {
        const float* cb = &it.bones[b * 12];
        const float* pb = &pv.bones[b * 12];
        float c[3] = {0.0f, 0.0f, 0.0f};
        if (cens != nullptr && b < cens->size() && (*cens)[b][3] > 1e-3f) {
          const std::array<float, 4>& s = (*cens)[b];
          c[0] = s[0] / s[3];
          c[1] = s[1] / s[3];
          c[2] = s[2] / s[3];
        }
        float d2 = 0.0f;
        for (int r = 0; r < 3; ++r) {
          const float e = (cb[r * 4 + 0] - pb[r * 4 + 0]) * c[0] +
                          (cb[r * 4 + 1] - pb[r * 4 + 1]) * c[1] +
                          (cb[r * 4 + 2] - pb[r * 4 + 2]) * c[2] + (cb[r * 4 + 3] - pb[r * 4 + 3]);
          d2 += e * e;
        }
        return std::sqrt(d2);
      };
      std::vector<float>& deltas = g_inter.deltas;
      deltas.clear();
      if (mit != g_r.meshes.end()) {
        const std::array<uint64_t, 4>& used = mit->second.bones_used;
        for (size_t b = 0; b * 12 + 12 <= it.bones.size() && b < 256; ++b) {
          if (((used[b >> 6] >> (b & 63)) & 1u) != 0) {
            deltas.push_back(bone_move(b));
          }
        }
      }
      if (deltas.empty()) {
        deltas.push_back(bone_move(0));
      }
      std::vector<float> sorted = deltas;
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
      const float median = sorted[sorted.size() / 2];
      // Teleport / re-pose guard: the piece as a whole jumped >= 1 m.
      m.bones = median < 1.0f;
      // Palette discontinuity (a different pose/bank in prev, e.g. an NPC's
      // LOD swap): several referenced bones move by a very different amount
      // than the piece as a whole within one guest frame. Blending two
      // unrelated poses is garbage; render cur's pose.
      if (m.bones && deltas.size() >= 3) {
        int outliers = 0;
        for (const float dd : deltas) {
          outliers += std::fabs(dd - median) > 0.5f ? 1 : 0;
        }
        if (outliers >= 3) {
          m.bones = false;
        }
      }
    }
    {
      const float wx = it.world[12] - pv.world[12];
      const float wy = it.world[13] - pv.world[13];
      const float wz = it.world[14] - pv.world[14];
      m.world = wx * wx + wy * wy + wz * wz < 1.0f &&
                std::memcmp(it.world, pv.world, sizeof(it.world)) != 0;
    }
    if (m.bones || m.world) {
      g_inter.matches.push_back(m);
    }
  }
  g_inter.valid = true;
  // Coverage over 600 scenes: an unmatched moving piece renders at cur while
  // its neighbours interpolate (the visible symptom: it twitches).
  static uint64_t s_scenes = 0, s_matched = 0, s_moving = 0, s_cuts = 0;
  ++s_scenes;
  s_matched += pairs.size();
  s_moving += g_inter.moving;
  s_cuts += g_inter.camera ? 0 : 1;
  if (s_scenes == 600) {
    REXLOG_INFO("native-scene: inter-frames matched {}/{} moving items over 600 scenes "
                "(last dt={:.2f} ms, camera cuts {})",
                s_matched, s_moving, g_inter.dt * 1e3, s_cuts);
    s_scenes = s_matched = s_moving = s_cuts = 0;
  }
}

// NativeInterFrameHooks::want: due when the next of the N evenly spaced
// slots inside the last guest interval has come.
// Inter-frame slot k of the current guest interval: its present is due to
// END at cur_end + dt * (k + 1) / (n_eff + 1) (evenly spaced between the
// swap presents' ends, which is when frames are seen), so its render starts
// one inter-frame cost earlier. n_eff = what fits: each present costs
// inter_cost of the one render thread, and slots that would still be
// rendering when the next swap is due are dropped. False when no slot is
// pending (or the swap present itself is still rendering).
bool InterFrameNextSlot(int n, double* start, double* deadline) {
  if (g_inter.cur_end_gen != g_inter.cur_generation) {
    return false;
  }
  const double dt = g_inter.dt_ema > 0.0 ? g_inter.dt_ema : g_inter.dt;
  const double cost = std::max(InterFramePlannedCost(true), 0.0002);
  const int n_eff = std::min(n, int(dt / (cost * 1.15)) - 1);
  if (n_eff <= 0 || g_inter.requested >= n_eff) {
    return false;
  }
  const double end = g_inter.cur_end + dt * double(g_inter.requested + 1) / double(n_eff + 1);
  *start = end - cost;
  // Too late to finish before the next swap present is due: skip it.
  *deadline = g_inter.cur_end + dt * 0.97 - cost;
  return true;
}

bool InterFrameWant(void* /*user_data*/) {
  const int n = InterFrameActiveCount();
  if (n <= 0 || !g_inter.valid || g_inter.requested >= n || g_photo_mode ||
      skate3::vr::Active() || !SceneEnabled()) {
    return false;
  }
  double start = 0.0, deadline = 0.0;
  if (!InterFrameNextSlot(n, &start, &deadline)) {
    return false;
  }
  const double now = InterFrameNow();
  if (now < start) {
    return false;
  }
  if (now > deadline) {
    g_inter.requested = n;  // the rest of this interval cannot fit
    return false;
  }
  // Counted on request, not on success: a render that yields must not make
  // the idle command processor retry it every spin.
  ++g_inter.requested;
  return true;
}

// NativeInterFrameHooks::seconds_until_want: when InterFrameWant will next
// say yes (same conditions), so the native render thread can sleep until
// then; -1 when nothing more is due before the next guest swap.
double InterFrameSecondsUntilWant(void* /*user_data*/) {
  const int n = InterFrameActiveCount();
  if (n <= 0 || !g_inter.valid || g_inter.requested >= n || g_photo_mode ||
      skate3::vr::Active() || !SceneEnabled()) {
    return -1.0;
  }
  double start = 0.0, deadline = 0.0;
  if (!InterFrameNextSlot(n, &start, &deadline)) {
    return -1.0;
  }
  const double now = InterFrameNow();
  if (now > deadline) {
    return -1.0;
  }
  return start - now;
}

// NativeInterFrameHooks::allow_async (command processor thread, once per
// guest swap): render on the native render thread unless VR (its frame
// pacing and submit hook live on the command processor) or a photo flow is
// running.
bool AllowAsyncNativeRender(void* /*user_data*/) {
  return REXCVAR_GET(skate3_native_render_async) && !skate3::vr::Active() && !g_photo_mode;
}

// Basis vector b of a bone palette row block (3x4, v' = M * v): column b.
inline float InterFrameColLen(const float* m, int b) {
  return std::sqrt(m[b] * m[b] + m[b + 4] * m[b + 4] + m[b + 8] * m[b + 8]);
}

// x = p + (c - p) * a over a 3x4 affine, basis lengths lerped too (the
// componentwise lerp of two rotations shortens the axes in between).
void InterFrameLerpAffine(float* x, const float* p, const float* c, float a) {
  for (int k = 0; k < 12; ++k) {
    x[k] = p[k] + (c[k] - p[k]) * a;
  }
  for (int b = 0; b < 3; ++b) {
    const float want = InterFrameColLen(p, b) + (InterFrameColLen(c, b) - InterFrameColLen(p, b)) * a;
    const float have = InterFrameColLen(x, b);
    if (have > 1e-6f) {
      const float r = want / have;
      x[b] *= r;
      x[b + 4] *= r;
      x[b + 8] *= r;
    }
  }
}

// Smallest cosine between matching unit basis columns of two 3x4 affines
// (1 = same orientation).
float InterFrameRotCos(const float* p, const float* c) {
  float worst = 1.0f;
  for (int b = 0; b < 3; ++b) {
    const float lp = InterFrameColLen(p, b);
    const float lc = InterFrameColLen(c, b);
    if (lp < 1e-6f || lc < 1e-6f) {
      continue;
    }
    const float d = (p[b] * c[b] + p[b + 4] * c[b + 4] + p[b + 8] * c[b + 8]) / (lp * lc);
    worst = std::min(worst, d);
  }
  return worst;
}

// out = a * b for 3x4 affines (v' = M * v).
void InterFrameMulAffine(float* out, const float* a, const float* b) {
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 4; ++k) {
      float v = a[r * 4 + 0] * b[0 * 4 + k] + a[r * 4 + 1] * b[1 * 4 + k] +
                a[r * 4 + 2] * b[2 * 4 + k];
      if (k == 3) {
        v += a[r * 4 + 3];
      }
      out[r * 4 + k] = v;
    }
  }
}

// out = inverse(m) for a 3x4 affine; false when singular.
bool InterFrameInvertAffine(float* out, const float* m) {
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
              i = m[10];
  const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-12f) {
    return false;
  }
  const float inv = 1.0f / det;
  const float r[9] = {A * inv, -(b * i - c * h) * inv, (b * f - c * e) * inv,
                      B * inv, (a * i - c * g) * inv, -(a * f - c * d) * inv,
                      C * inv, -(a * h - b * g) * inv, (a * e - b * d) * inv};
  for (int row = 0; row < 3; ++row) {
    out[row * 4 + 0] = r[row * 3 + 0];
    out[row * 4 + 1] = r[row * 3 + 1];
    out[row * 4 + 2] = r[row * 3 + 2];
    out[row * 4 + 3] = -(r[row * 3 + 0] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
  }
  return true;
}

// A palette at alpha: slow bones lerp; fast-spinning bones ride their
// nearest slow bone with cur's relative transform (see the block comment).
void InterFrameLerpPalette(float* x, const float* p, const float* c, size_t floats, float a,
                           const std::array<uint64_t, 4>* used,
                           const std::array<uint64_t, 4>* pool,
                           const std::vector<InterFrameRigidAnchor>* rigid,
                           const std::vector<std::array<float, 4>>* centroids) {
  const size_t nb = floats / 12;
  // Nearest slow rigid piece within 2 m of a bone (-1: none).
  const auto nearest_rigid = [rigid](const float* bone) -> int32_t {
    if (rigid == nullptr) {
      return -1;
    }
    int32_t best = -1;
    float best_d2 = 4.0f;
    for (size_t k = 0; k < rigid->size(); ++k) {
      const float* rc = (*rigid)[k].center;
      const float dx = rc[0] - bone[3], dy = rc[1] - bone[7], dz = rc[2] - bone[11];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = int32_t(k);
      }
    }
    return best;
  };
  const auto in_mask = [](const std::array<uint64_t, 4>* m, size_t b) {
    return m == nullptr || (b < 256 && ((*m)[b >> 6] >> (b & 63)) & 1u);
  };
  // Spinning classification: bones THIS mesh's vertices reference (other
  // rows only lerp). Anchors: bones ANY mesh drawing from this palette
  // references (`pool`): retail draws the board's wheels as their own mesh
  // over the character bank, and their truck/deck bones belong to the deck
  // mesh. Rows no mesh references (the bank's churning staged rows) are
  // never anchors (see MeshBuffers::bones_used).
  const auto referenced = [&](size_t b) { return in_mask(used, b); };
  const auto anchorable = [&](size_t b) { return in_mask(pool != nullptr ? pool : used, b); };
  constexpr float kFastCos = 0.94f;  // ~20 degrees between two guest frames
  std::vector<int32_t>& anchor = g_inter.anchor;
  anchor.assign(nb, -1);
  bool any_fast = false;
  g_inter.rot_cos.resize(nb);
  float* const rot_cos = g_inter.rot_cos.data();
  for (size_t b = 0; b < nb; ++b) {
    rot_cos[b] = InterFrameRotCos(&p[b * 12], &c[b * 12]);
    if (rot_cos[b] < kFastCos && referenced(b)) {
      anchor[b] = -2;  // fast: anchor resolved below
      any_fast = true;
    }
  }
  // Spinning bones with a known vertex centroid (wheels): the centre follows
  // its interpolated path, the orientation is cur's (a wheel is rotationally
  // symmetric). No anchor needed, which is what retail's board needs: its
  // wheels are a mesh of their own (4 bones, all spinning) while the deck
  // and trucks are separate meshes; sk83's board is ONE 7-bone mesh, where
  // the anchors below always found the deck.
  for (size_t b = 0; any_fast && centroids != nullptr && b < nb; ++b) {
    if (anchor[b] != -2 || b >= centroids->size() || (*centroids)[b][3] <= 1e-3f) {
      continue;
    }
    const std::array<float, 4>& s = (*centroids)[b];
    const float cen[3] = {s[0] / s[3], s[1] / s[3], s[2] / s[3]};
    const float* cb = &c[b * 12];
    const float* pb = &p[b * 12];
    float* xb = &x[b * 12];
    for (int r = 0; r < 3; ++r) {
      const float rot_c = cb[r * 4 + 0] * cen[0] + cb[r * 4 + 1] * cen[1] + cb[r * 4 + 2] * cen[2];
      const float rot_p = pb[r * 4 + 0] * cen[0] + pb[r * 4 + 1] * cen[1] + pb[r * 4 + 2] * cen[2];
      const float center_c = rot_c + cb[r * 4 + 3];
      const float center_p = rot_p + pb[r * 4 + 3];
      const float center_x = center_p + (center_c - center_p) * a;
      xb[r * 4 + 0] = cb[r * 4 + 0];
      xb[r * 4 + 1] = cb[r * 4 + 1];
      xb[r * 4 + 2] = cb[r * 4 + 2];
      xb[r * 4 + 3] = center_x - rot_c;
    }
    anchor[b] = -3;  // resolved
  }
  // A spinning bone with no slow bone within 2 m (a board mid-flip: the deck
  // spins too) promotes the least-rotating spinning bone near it to an
  // interpolated anchor, so the wheels ride the flipping deck instead of
  // all holding cur's pose.
  for (size_t b = 0; any_fast && b < nb; ++b) {
    if (anchor[b] != -2) {
      continue;
    }
    const float* cb = &c[b * 12];
    bool has_slow = false;
    int32_t least = int32_t(b);
    for (size_t o = 0; o < nb && !has_slow; ++o) {
      const float* co = &c[o * 12];
      const float ddx = co[3] - cb[3], ddy = co[7] - cb[7], ddz = co[11] - cb[11];
      if (ddx * ddx + ddy * ddy + ddz * ddz >= 4.0f || !anchorable(o)) {
        continue;
      }
      if (anchor[o] == -1) {
        has_slow = true;
      } else if (rot_cos[o] > rot_cos[size_t(least)]) {
        least = int32_t(o);
      }
    }
    if (!has_slow && nearest_rigid(cb) < 0) {
      anchor[size_t(least)] = -1;
    }
  }
  for (size_t b = 0; b < nb; ++b) {
    if (anchor[b] == -1) {
      InterFrameLerpAffine(&x[b * 12], &p[b * 12], &c[b * 12], a);
    }
  }
  if (!any_fast) {
    return;
  }
  for (size_t b = 0; b < nb; ++b) {
    if (anchor[b] != -2) {
      continue;
    }
    const float* cb = &c[b * 12];
    int32_t best = -1;
    float best_d2 = 4.0f;  // within 2 m: the wheel's own truck/body
    for (size_t o = 0; o < nb; ++o) {
      if (anchor[o] != -1 || !anchorable(o)) {
        continue;
      }
      const float* co = &c[o * 12];
      const float ddx = co[3] - cb[3], ddy = co[7] - cb[7], ddz = co[11] - cb[11];
      const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = int32_t(o);
      }
    }
    float inv_anchor_c[12];
    if (best < 0) {
      // No slow bone: ride the nearest slow rigid piece (truck/deck).
      const int32_t r = nearest_rigid(cb);
      if (r >= 0 && InterFrameInvertAffine(inv_anchor_c, (*rigid)[size_t(r)].cur)) {
        float rel_r[12];
        InterFrameMulAffine(rel_r, inv_anchor_c, cb);
        InterFrameMulAffine(&x[b * 12], (*rigid)[size_t(r)].posed, rel_r);
      } else {
        // Nothing to ride on: cur's pose (a held wheel beats a misplaced one).
        std::memcpy(&x[b * 12], cb, 12 * sizeof(float));
      }
      continue;
    }
    if (!InterFrameInvertAffine(inv_anchor_c, &c[best * 12])) {
      std::memcpy(&x[b * 12], cb, 12 * sizeof(float));
      continue;
    }
    float rel[12];
    InterFrameMulAffine(rel, inv_anchor_c, cb);
    InterFrameMulAffine(&x[b * 12], &x[best * 12], rel);
  }
}

// World-space centre of a rigid piece: its mesh-local bbox centre through
// the row-vector world. NOT the world's translation: a piece whose mesh is
// modelled around the vehicle's origin (retail's board wheels) rotates about
// that origin, so its translation swings with the spin angle.
void InterFrameWorldCenter(const DrawItem& d, const float* w, float out[3]) {
  const float cx = 0.5f * (d.bbox_min[0] + d.bbox_max[0]);
  const float cy = 0.5f * (d.bbox_min[1] + d.bbox_max[1]);
  const float cz = 0.5f * (d.bbox_min[2] + d.bbox_max[2]);
  for (int k = 0; k < 3; ++k) {
    out[k] = cx * w[k] + cy * w[4 + k] + cz * w[8 + k] + w[12 + k];
  }
}

// out = a * b for row-vector 4x4 affines (last column 0,0,0,1).
void InterFrameMulWorld(float* out, const float* a, const float* b) {
  float r[16];
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 4; ++k) {
      r[i * 4 + k] = a[i * 4 + 0] * b[0 * 4 + k] + a[i * 4 + 1] * b[1 * 4 + k] +
                     a[i * 4 + 2] * b[2 * 4 + k] + a[i * 4 + 3] * b[3 * 4 + k];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

// out = inverse(m) for a row-vector 4x4 affine; false when singular.
bool InterFrameInvertWorld(float* out, const float* m) {
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
              i = m[10];
  const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-12f) {
    return false;
  }
  const float s = 1.0f / det;
  const float r[9] = {A * s, -(b * i - c * h) * s, (b * f - c * e) * s,
                      B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
                      C * s, -(a * h - b * g) * s, (a * e - b * d) * s};
  for (int row = 0; row < 3; ++row) {
    out[row * 4 + 0] = r[row * 3 + 0];
    out[row * 4 + 1] = r[row * 3 + 1];
    out[row * 4 + 2] = r[row * 3 + 2];
    out[row * 4 + 3] = 0.0f;
  }
  for (int k = 0; k < 3; ++k) {
    out[12 + k] = -(m[12] * r[0 * 3 + k] + m[13] * r[1 * 3 + k] + m[14] * r[2 * 3 + k]);
  }
  out[15] = 1.0f;
  return true;
}

// Row-vector 4x4 world (basis rows 0..2, translation row 3) at alpha.
// Returns true for a fast-spinning piece, left at cur for the caller to
// attach to a slow anchor (InterFrameAdvance).
bool InterFrameLerpWorld(float* x, const float* p, const float* c, float a) {
  for (int k = 0; k < 16; ++k) {
    x[k] = p[k] + (c[k] - p[k]) * a;
  }
  // A fast-spinning rigid piece (retail's board wheels are rigid instances):
  // lerping its rotation is meaningless past ~20 degrees per guest frame;
  // keep cur's orientation (wheels are rotationally symmetric) and move only
  // the position.
  float rot_cos = 1.0f;
  for (int r = 0; r < 3; ++r) {
    const float* pr = &p[r * 4];
    const float* cr = &c[r * 4];
    const float lp = std::sqrt(pr[0] * pr[0] + pr[1] * pr[1] + pr[2] * pr[2]);
    const float lc = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    if (lp > 1e-6f && lc > 1e-6f) {
      rot_cos = std::min(rot_cos, (pr[0] * cr[0] + pr[1] * cr[1] + pr[2] * cr[2]) / (lp * lc));
    }
  }
  if (rot_cos < 0.94f) {
    std::memcpy(x, c, 16 * sizeof(float));
    return true;
  }
  for (int r = 0; r < 3; ++r) {
    const float* pr = &p[r * 4];
    const float* cr = &c[r * 4];
    float* xr = &x[r * 4];
    const float lp = std::sqrt(pr[0] * pr[0] + pr[1] * pr[1] + pr[2] * pr[2]);
    const float lc = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    const float lx = std::sqrt(xr[0] * xr[0] + xr[1] * xr[1] + xr[2] * xr[2]);
    if (lx > 1e-6f) {
      const float s = (lp + (lc - lp) * a) / lx;
      xr[0] *= s;
      xr[1] *= s;
      xr[2] *= s;
    }
  }
  return false;
}

}  // namespace

// Spinning rigid pieces (wheels) ride a slow piece of the same entity (same
// context, else the nearest within 2 m: the deck/truck/body) with cur's
// relative transform: W = W_cur * inverse(A_cur) * A_posed.
void InterFrameAttachFastWorld(FrameScene* scene_, const std::vector<float>& saved,
                               const std::vector<std::pair<uint32_t, size_t>>& fast_world,
                               const std::vector<std::pair<uint32_t, size_t>>& slow_world) {
  for (const auto& [fm, fat] : fast_world) {
    DrawItem& w = scene_->items[g_inter.matches[fm].cur];
    float wc[3];
    InterFrameWorldCenter(w, &saved[fat], wc);
    int32_t best = -1;
    float best_score = 4.0f;  // 2 m
    for (size_t k = 0; k < slow_world.size(); ++k) {
      const DrawItem& an = scene_->items[g_inter.matches[slow_world[k].first].cur];
      float ac[3];
      InterFrameWorldCenter(an, &saved[slow_world[k].second], ac);
      float d2 = (ac[0] - wc[0]) * (ac[0] - wc[0]) + (ac[1] - wc[1]) * (ac[1] - wc[1]) +
                 (ac[2] - wc[2]) * (ac[2] - wc[2]);
      if (d2 >= 4.0f) {
        continue;
      }
      if (w.ctx != 0 && an.ctx == w.ctx) {
        d2 -= 4.0f;  // same entity wins over any other piece in range
      }
      if (d2 < best_score) {
        best_score = d2;
        best = int32_t(k);
      }
    }
    if (best < 0) {
      continue;  // nothing to ride on: cur's pose (already in place)
    }
    const auto& [am, aat] = slow_world[size_t(best)];
    const DrawItem& an = scene_->items[g_inter.matches[am].cur];
    float inv_ac[16];
    if (!InterFrameInvertWorld(inv_ac, &saved[aat])) {
      continue;
    }
    float rel[16];
    InterFrameMulWorld(rel, &saved[fat], inv_ac);
    InterFrameMulWorld(w.world, rel, an.world);
  }
}

// Poses the scene at the present time for one RenderScene pass and restores
// it after (published scenes are allocated non-const for exactly this, like
// VrEyeCamera). Swap frames and inter-frames alike while inter-frames are on.
class InterFrameAdvance {
 public:
  InterFrameAdvance(const FrameScene& scene, bool active) {
    if (!active || !g_inter.valid || &scene != g_inter.cur.get() || !g_inter.prev) {
      return;
    }
    const FrameScene& p = *g_inter.prev;
    scene_ = const_cast<FrameScene*>(&scene);
    float a = float(std::clamp((InterFrameNow() - g_inter.cur_t) / g_inter.dt, 0.0, 1.0));
    if (REXCVAR_GET(skate3_native_render_scene_present_pacing) && g_inter.lat_valid &&
        scene.play_time > p.play_time && p.play_time > 0.0) {
      // Up to a quarter interval of extrapolation while inter-frames run: a
      // late guest frame keeps the motion going instead of a held pose and
      // a jump when it lands.
      const double a_max =
          InterFrameActiveCount() > 0 ? 1.25 : 1.0;
      // Content for when this present will be SEEN: render start + the
      // expected render time of its kind.
      const double shown =
          InterFrameNow() + InterFramePlannedCost(rex::graphics::IsNativeInterFrame());
      g_inter.present_at = shown;
      a = float(std::clamp((shown - g_inter.lat - p.play_time) /
                               (scene.play_time - p.play_time),
                           0.0, a_max));
    }
    content_time_ = p.play_time + (scene.play_time - p.play_time) * double(a);
    if (p.player_pose_valid && scene.player_pose_valid) {
      for (int k = 0; k < 3; ++k) {
        posed_head_[k] = p.player_head[k] + (scene.player_head[k] - p.player_head[k]) * a;
      }
      posed_head_valid_ = true;
    }
    const float forced = REXCVAR_GET(skate3_native_render_scene_interframes_debug_alpha);
    if (forced >= 0.0f) {
      a = std::min(forced, 1.0f);
    }
    std::vector<float>& saved = g_inter.saved;
    saved.clear();
    // (match index, saved offset) of the rigid pieces, by spin class.
    std::vector<std::pair<uint32_t, size_t>>& fast_world = g_inter.fast_world;
    std::vector<std::pair<uint32_t, size_t>>& slow_world = g_inter.slow_world;
    fast_world.clear();
    slow_world.clear();
    std::memcpy(saved_view_proj_, scene_->view_proj, sizeof(saved_view_proj_));
    std::memcpy(saved_cam_pos_, scene_->cam_pos, sizeof(saved_cam_pos_));
    std::memcpy(saved_proj_, scene_->proj, sizeof(saved_proj_));
    if (g_inter.camera) {
      for (int i = 0; i < 16; ++i) {
        scene_->view_proj[i] = p.view_proj[i] + (saved_view_proj_[i] - p.view_proj[i]) * a;
        scene_->proj[i] = p.proj[i] + (saved_proj_[i] - p.proj[i]) * a;
      }
      for (int i = 0; i < 3; ++i) {
        scene_->cam_pos[i] = p.cam_pos[i] + (saved_cam_pos_[i] - p.cam_pos[i]) * a;
      }
    }
    // The board's contact shadow (the deck decal, built from deck_world at
    // render time) must follow the posed board, not cur's: left at cur it
    // led the interpolated board by up to a guest frame (16 ms under a 60
    // fps cap, plainly visible at speed). Mid-flip (fast spin) the deck
    // keeps cur's orientation like any spinning piece but its position
    // still interpolates; a teleport (respawn) is not interpolated.
    for (uint32_t index = 0; index < skate3::deck_shadow::kMaxSkaters; ++index) {
      const auto& previous = p.decks[index];
      auto& deck = scene_->decks[index];
      if (!previous.valid || !deck.valid || previous.owner != deck.owner) continue;
      const float jx = deck.world[12] - previous.world[12];
      const float jy = deck.world[13] - previous.world[13];
      const float jz = deck.world[14] - previous.world[14];
      if (jx * jx + jy * jy + jz * jz < 9.0f) {
        std::memcpy(saved_deck_[index], deck.world, sizeof(deck.world));
        if (InterFrameLerpWorld(deck.world, previous.world, saved_deck_[index], a)) {
          for (int k = 12; k < 15; ++k) {
            deck.world[k] = previous.world[k] + (saved_deck_[index][k] - previous.world[k]) * a;
          }
        }
        deck_posed_[index] = true;
      }
    }
    // Palette pools: skinned items whose palettes are identical draw from one
    // bank; their referenced-bone masks are pooled as anchor candidates.
    std::unordered_map<uint64_t, std::array<uint64_t, 4>>& pools = g_inter.pools;
    std::vector<uint64_t>& match_pool = g_inter.match_pool;
    const bool pools_fresh = g_inter.pools_gen == scene_->generation &&
                             g_inter.pools_matches == g_inter.matches.size();
    if (!pools_fresh) {
      g_inter.pools_gen = scene_->generation;
      g_inter.pools_matches = g_inter.matches.size();
      pools.clear();
      match_pool.assign(g_inter.matches.size(), 0);
    }
    for (size_t mi = 0; mi < g_inter.matches.size() && !pools_fresh; ++mi) {
      const InterFrameState::Match& m = g_inter.matches[mi];
      if (!m.bones) {
        continue;
      }
      const DrawItem& it = scene_->items[m.cur];
      const auto mit = g_r.meshes.find(it.mesh);
      if (mit == g_r.meshes.end()) {
        continue;
      }
      uint64_t h = 1469598103934665603ull ^ it.bones.size();
      const uint32_t* words = reinterpret_cast<const uint32_t*>(it.bones.data());
      for (size_t k = 0; k < it.bones.size(); ++k) {
        h = (h ^ words[k]) * 1099511628211ull;
      }
      match_pool[mi] = h | 1;
      std::array<uint64_t, 4>& pm = pools[h | 1];
      for (int k = 0; k < 4; ++k) {
        pm[size_t(k)] |= mit->second.bones_used[size_t(k)];
      }
    }
    std::vector<size_t>& bones_at = g_inter.bones_at;
    std::vector<size_t>& world_at = g_inter.world_at;
    bones_at.assign(g_inter.matches.size(), size_t(-1));
    world_at.assign(g_inter.matches.size(), size_t(-1));
    // Pass 1: rigid pieces (skinned spinning bones may ride them).
    for (size_t mi = 0; mi < g_inter.matches.size(); ++mi) {
      const InterFrameState::Match& m = g_inter.matches[mi];
      if (!m.world) {
        continue;
      }
      DrawItem& it = scene_->items[m.cur];
      const DrawItem& pv = p.items[m.prev];
      const size_t at = saved.size();
      world_at[mi] = at;
      saved.insert(saved.end(), it.world, it.world + 16);
      if (InterFrameLerpWorld(it.world, pv.world, &saved[at], a)) {
        fast_world.push_back({uint32_t(mi), at});
      } else {
        slow_world.push_back({uint32_t(mi), at});
      }
    }
    InterFrameAttachFastWorld(scene_, saved, fast_world, slow_world);
    // Slow rigid pieces as anchors for skinned bones (column-vector 3x4).
    std::vector<InterFrameRigidAnchor>& rigid = g_inter.rigid_anchors;
    rigid.clear();
    for (const auto& [mi, at] : slow_world) {
      const DrawItem& it = scene_->items[g_inter.matches[mi].cur];
      InterFrameRigidAnchor ra;
      const float* wc = &saved[at];
      for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
          ra.cur[r * 4 + k] = wc[k * 4 + r];
          ra.posed[r * 4 + k] = it.world[k * 4 + r];
        }
        ra.cur[r * 4 + 3] = wc[12 + r];
        ra.posed[r * 4 + 3] = it.world[12 + r];
      }
      InterFrameWorldCenter(it, wc, ra.center);
      rigid.push_back(ra);
    }
    // Pass 2: skinned palettes.
    for (const InterFrameState::Match& m : g_inter.matches) {
      DrawItem& it = scene_->items[m.cur];
      const DrawItem& pv = p.items[m.prev];
      if (m.bones) {
        const size_t at = saved.size();
        bones_at[size_t(&m - g_inter.matches.data())] = at;
        saved.insert(saved.end(), it.bones.begin(), it.bones.end());
        const auto mit = g_r.meshes.find(it.mesh);
        const bool have_mask =
            mit != g_r.meshes.end() &&
            (mit->second.bones_used[0] | mit->second.bones_used[1] |
             mit->second.bones_used[2] | mit->second.bones_used[3]) != 0;
        const uint64_t pool_key = match_pool[size_t(&m - g_inter.matches.data())];
        const auto pit = pool_key != 0 ? pools.find(pool_key) : pools.end();
        const bool have_pool =
            pit != pools.end() &&
            (pit->second[0] | pit->second[1] | pit->second[2] | pit->second[3]) != 0;
        InterFrameLerpPalette(it.bones.data(), pv.bones.data(), &saved[at], it.bones.size(), a,
                              have_mask ? &mit->second.bones_used : nullptr,
                              have_pool ? &pit->second : nullptr, &rigid,
                              mit != g_r.meshes.end() ? &mit->second.bone_centroids : nullptr);
      }
    }
    // Smoothness probe: the biggest palette (the skater's body) root bone,
    // per posed present. A reversal = consecutive present-to-present steps
    // pointing against each other while the piece moves (> 1 cm per
    // present): jitter/flicker shows up as many of them.
    const DrawItem* body = nullptr;
    for (const InterFrameState::Match& m : g_inter.matches) {
      const DrawItem& it = scene_->items[m.cur];
      if (m.bones && (body == nullptr || it.bones.size() > body->bones.size())) {
        body = &it;
      }
    }
    if (body != nullptr) {
      static float s_last[3] = {}, s_step[3] = {};
      static uint32_t s_n = 0, s_moving = 0, s_rev = 0;
      const float pos[3] = {body->bones[3], body->bones[7], body->bones[11]};
      const float step[3] = {pos[0] - s_last[0], pos[1] - s_last[1], pos[2] - s_last[2]};
      const float len2 = step[0] * step[0] + step[1] * step[1] + step[2] * step[2];
      if (len2 > 1e-4f && len2 < 1.0f) {
        ++s_moving;
        if (step[0] * s_step[0] + step[1] * s_step[1] + step[2] * s_step[2] < 0.0f) {
          ++s_rev;
        }
      }
      std::memcpy(s_last, pos, sizeof(pos));
      std::memcpy(s_step, step, sizeof(step));
      if (++s_n == 1200) {
        REXLOG_INFO("native-scene: inter-frame smoothness: {} reversals in {} moving presents "
                    "(of 1200)", s_rev, s_moving);
        s_n = s_moving = s_rev = 0;
      }
    }
  }
  // Content (playback-clock) time this present shows; `fallback` (the
  // scene's own) when the scene is not posed.
  double ContentTime(double fallback) const {
    return scene_ != nullptr && content_time_ > 0.0 ? content_time_ : fallback;
  }
  // The player's head as posed for this present (nullptr: not posed).
  const float* PosedHead() const {
    return scene_ != nullptr && posed_head_valid_ ? posed_head_ : nullptr;
  }
  ~InterFrameAdvance() {
    if (scene_ == nullptr) {
      return;
    }
    std::memcpy(scene_->view_proj, saved_view_proj_, sizeof(saved_view_proj_));
    std::memcpy(scene_->cam_pos, saved_cam_pos_, sizeof(saved_cam_pos_));
    std::memcpy(scene_->proj, saved_proj_, sizeof(saved_proj_));
    for (uint32_t index = 0; index < skate3::deck_shadow::kMaxSkaters; ++index) {
      if (deck_posed_[index]) std::memcpy(scene_->decks[index].world, saved_deck_[index], sizeof(saved_deck_[index]));
    }
    for (size_t mi = 0; mi < g_inter.matches.size(); ++mi) {
      DrawItem& it = scene_->items[g_inter.matches[mi].cur];
      if (g_inter.bones_at[mi] != size_t(-1)) {
        std::memcpy(it.bones.data(), &g_inter.saved[g_inter.bones_at[mi]],
                    it.bones.size() * sizeof(float));
      }
      if (g_inter.world_at[mi] != size_t(-1)) {
        std::memcpy(it.world, &g_inter.saved[g_inter.world_at[mi]], sizeof(it.world));
      }
    }
  }
  InterFrameAdvance(const InterFrameAdvance&) = delete;
  InterFrameAdvance& operator=(const InterFrameAdvance&) = delete;

 private:
  FrameScene* scene_ = nullptr;
  float saved_deck_[skate3::deck_shadow::kMaxSkaters][16] = {};
  bool deck_posed_[skate3::deck_shadow::kMaxSkaters] = {};
  double content_time_ = 0.0;
  float posed_head_[3] = {};
  bool posed_head_valid_ = false;
  float saved_view_proj_[16];
  float saved_cam_pos_[3];
  float saved_proj_[16];
};

// VR eye pass (RenderSceneVr): RenderScene draws the published scene with
// the current eye's camera. A published scene can be rendered again by a
// later swap, so the game camera is restored when the pass ends. Published
// scenes are allocated non-const (PublishFrameScene) for exactly this.
class VrEyeCamera {
 public:
  VrEyeCamera(const FrameScene& scene, bool skip) {
    skate3::vr::EyeCamera cam;
    // One-pass stereo: the scene camera is the LEFT eye; the right eye rides
    // st_d (left clip -> right clip) in the scene VS.
    const int eye = skate3::vr::CurrentEye();
    skate3::vr::SetFirstPersonAnchor(!skip && !scene.items.empty() && scene.player_pose_valid
                                        ? scene.player_head : nullptr, scene.player_forward,
                                    scene.player_chest, scene.player_face);
    if (!skip && !scene.items.empty()) {
      skate3::vr::SetGameFov(scene.guest_cam_proj11);  // headset zoom follows the game's
    }
    if (skip || scene.items.empty() ||
        !(eye == skate3::vr::kStereoEye
              ? skate3::vr::ComputeStereoCamera(scene.view_proj, scene.proj, &cam)
              : skate3::vr::ComputeEyeCamera(eye, scene.view_proj, scene.proj, &cam))) {
      return;
    }
    scene_ = const_cast<FrameScene*>(&scene);
    std::memcpy(saved_view_proj_, scene_->view_proj, sizeof(saved_view_proj_));
    std::memcpy(saved_proj_, scene_->proj, sizeof(saved_proj_));
    std::memcpy(saved_cam_pos_, scene_->cam_pos, sizeof(saved_cam_pos_));
    std::memcpy(scene_->view_proj, cam.view_proj, sizeof(cam.view_proj));
    std::memcpy(scene_->proj, cam.proj, sizeof(cam.proj));
    std::memcpy(scene_->cam_pos, cam.cam_pos, sizeof(cam.cam_pos));
  }
  ~VrEyeCamera() {
    if (scene_ == nullptr) {
      return;
    }
    std::memcpy(scene_->view_proj, saved_view_proj_, sizeof(saved_view_proj_));
    std::memcpy(scene_->proj, saved_proj_, sizeof(saved_proj_));
    std::memcpy(scene_->cam_pos, saved_cam_pos_, sizeof(saved_cam_pos_));
  }
  VrEyeCamera(const VrEyeCamera&) = delete;
  VrEyeCamera& operator=(const VrEyeCamera&) = delete;

 private:
  FrameScene* scene_ = nullptr;
  float saved_view_proj_[16];
  float saved_proj_[16];
  float saved_cam_pos_[3];
};

// DIAGNOSTIC present-pacing recorder (skate3_native_render_scene_pace_record):
// one row per present with its render start and end times, the scene shown
// (generation, content time, inter-frame flag) and what it shows (camera,
// the player's head and the board as posed). Recorded once per arming; the
// CSV is written when the window closes. Render thread only.
struct PaceRow {
  int64_t t_ns;
  int64_t end_ns;
  int64_t target_ns;  // planned display time (0: none)
  uint64_t gen;
  double play;
  uint8_t inter;
  float cam[3];
  float vp[16];
  float head[3];
  uint8_t head_valid;
  float deck[3];  // the board as posed (its contact shadow's source)
  float veh[3];   // tracked traffic vehicle's root bone as posed
  uint32_t veh_id;  // its ctx (0 = none in range)
};
std::vector<PaceRow> g_pace_rows;
int64_t g_pace_end_ns = 0;
double g_pace_armed_for = 0.0;

int64_t PaceNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void PaceRecordPresent(const FrameScene& scene, bool inter_frame, double content_time,
                       const float* posed_head) {
  const double secs = REXCVAR_GET(skate3_native_render_scene_pace_record);
  if (secs <= 0.0) {
    g_pace_armed_for = 0.0;
    return;
  }
  const int64_t now = PaceNowNs();
  if (g_pace_armed_for != secs) {
    // (Re-)armed: a new window.
    g_pace_armed_for = secs;
    g_pace_rows.clear();
    g_pace_rows.reserve(size_t(secs * 1200.0));
    g_pace_end_ns = now + int64_t(secs * 1e9);
  }
  if (g_pace_end_ns == 0) {
    return;  // this arming's window already written
  }
  PaceRow r{};
  r.t_ns = now;
  r.target_ns = g_inter.present_at > 0.0 ? int64_t(g_inter.present_at * 1e9) : 0;
  r.gen = scene.generation;
  r.play = content_time;
  r.inter = inter_frame ? 1 : 0;
  std::memcpy(r.cam, scene.cam_pos, sizeof(r.cam));
  std::memcpy(r.vp, scene.view_proj, sizeof(r.vp));
  std::memcpy(r.head, posed_head != nullptr ? posed_head : scene.player_head, sizeof(r.head));
  r.head_valid = scene.player_pose_valid ? 1 : 0;
  std::memcpy(r.deck, scene.decks[0].world + 12, sizeof(r.deck));
  // One traffic vehicle (char family 6) tracked by CONTINUITY: the same mesh
  // nearest its last recorded position (within 6 m: a real stale-pose jump
  // still shows), else a new track on the nearest within 40 m of the camera.
  // (MeshContext addresses are not identity: dynamic entities draw from
  // per-frame arenas, so a ctx can be another car the next frame.)
  {
    static uint32_t s_track = 0;
    static uint32_t s_mesh = 0;
    static float s_pos[3] = {};
    const DrawItem* pick = nullptr;
    if (s_track != 0) {
      float best = 6.0f * 6.0f;
      for (const DrawItem& it : scene.items) {
        if (it.char_family != 6 || it.bones.size() < 12 || it.mesh != s_mesh) {
          continue;
        }
        float vp[3];
        InterFrameBonePos(it, 0, vp);
        const float dx = vp[0] - s_pos[0];
        const float dy = vp[1] - s_pos[1];
        const float dz = vp[2] - s_pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) {
          best = d2;
          pick = &it;
        }
      }
    }
    if (pick == nullptr) {
      float best = 40.0f * 40.0f;
      for (const DrawItem& it : scene.items) {
        if (it.char_family != 6 || it.bones.size() < 12) {
          continue;
        }
        float vp[3];
        InterFrameBonePos(it, 0, vp);
        const float dx = vp[0] - scene.cam_pos[0];
        const float dy = vp[1] - scene.cam_pos[1];
        const float dz = vp[2] - scene.cam_pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) {
          best = d2;
          pick = &it;
        }
      }
      if (pick != nullptr) {
        ++s_track;
        s_mesh = pick->mesh;
      } else {
        s_track = 0;
      }
    }
    r.veh_id = pick != nullptr ? s_track : 0;
    if (pick != nullptr) {
      InterFrameBonePos(*pick, 0, r.veh);
      std::memcpy(s_pos, r.veh, sizeof(s_pos));
    }
  }
  g_pace_rows.push_back(r);
}

// The present's render ended (RenderScene returning): stamp the last row and
// write the CSV once the window has closed.
void PaceRecordEnd() {
  if (g_pace_end_ns == 0 || g_pace_rows.empty() || g_pace_rows.back().end_ns != 0) {
    return;
  }
  const int64_t now = PaceNowNs();
  g_pace_rows.back().end_ns = now;
  if (now < g_pace_end_ns) {
    return;
  }
  g_pace_end_ns = 0;
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  const std::string path = "logs/pace_" + std::to_string(stamp) + ".csv";
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "t_ns,end_ns,target_ns,gen,play,inter,cx,cy,cz");
    for (int i = 0; i < 16; ++i) std::fprintf(f, ",vp%d", i);
    std::fprintf(f, ",hx,hy,hz,hv,dx,dy,dz,vx,vy,vz,vid\n");
    for (const PaceRow& w : g_pace_rows) {
      std::fprintf(f, "%lld,%lld,%lld,%llu,%.9f,%u,%.6f,%.6f,%.6f", (long long)w.t_ns,
                   (long long)w.end_ns, (long long)w.target_ns, (unsigned long long)w.gen,
                   w.play, w.inter, w.cam[0], w.cam[1], w.cam[2]);
      for (int i = 0; i < 16; ++i) std::fprintf(f, ",%.7g", w.vp[i]);
      std::fprintf(f, ",%.6f,%.6f,%.6f,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u\n", w.head[0],
                   w.head[1], w.head[2], w.head_valid, w.deck[0], w.deck[1], w.deck[2],
                   w.veh[0], w.veh[1], w.veh[2], w.veh_id);
    }
    std::fclose(f);
    REXLOG_INFO("native-scene: pace recorder wrote {} presents to {}", g_pace_rows.size(), path);
  }
  g_pace_rows.clear();
  g_pace_rows.shrink_to_fit();
}

// Times one RenderScene into the per-kind costs (see InterFrameState) and
// stamps the swap present's end.
struct RenderCostTimer {
  double t0 = InterFrameNow();
  bool inter = rex::graphics::IsNativeInterFrame();
  RenderCostTimer() { g_inter.present_at = 0.0; }
  ~RenderCostTimer() {
    const double end = InterFrameNow();
    const double d = end - t0;
    if (d > 0.0 && d < 0.05) {
      double& c = inter ? g_inter.inter_cost : g_inter.real_cost;
      double& m = inter ? g_inter.inter_mad : g_inter.real_mad;
      m += (std::fabs(d - c) - m) * 0.05;
      c += (d - c) * 0.05;
      g_inter.render_cost += (d - g_inter.render_cost) * 0.05;
    }
    if (!inter && g_inter.cur && g_inter.cur_end_gen != g_inter.cur_generation) {
      g_inter.cur_end = end;
      g_inter.cur_end_gen = g_inter.cur_generation;
    }
    PaceRecordEnd();
  }
};

bool RenderScene(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  const RenderCostTimer render_cost_timer;
  {
    // Free the scenes the guest replaced since the last frame (see
    // g_scene_retire); swapped out first so the frees run unlocked.
    std::vector<std::shared_ptr<const FrameScene>> retired;
    {
      std::lock_guard<std::mutex> lock(g_scene_retire_mutex);
      retired.swap(g_scene_retire);
    }
  }
  UpdateBrowserScreenState();
  const bool photo_mode = g_photo_mode;
  const bool inter_frame = rex::graphics::IsNativeInterFrame();
  if (inter_frame && (photo_mode || !g_inter.cur)) {
    return false;
  }
  if (!SceneEnabled() ||
      (context.backend != NativeGuestOutputBackend::kD3D12 &&
       context.backend != NativeGuestOutputBackend::kVulkan)) {
    return false;
  }
  {
    // Debug stress driver (skate3_native_render_scene_maxq_cycle): cycles
    // the max-quality toggle to exercise the hot rebuild path.
    static uint64_t s_maxq_cycle_frame = 0;
    skate3::MaxQualityAutoCycle(++s_maxq_cycle_frame);
  }
  // While the game reports menus / loading (presence context 0x8001 == 0),
  // yield to the emulated output, EXCEPT the in-game pause menu (world
  // still publishing perspective scenes), which stays native when
  // skate3_native_render_scene_pause_native is on: the pause UI rides the
  // same captured-APT 2D replay as the gameplay HUD, and the backdrop is the
  // ordinary native world. Loading screens and the boot frontend still
  // render emulated (complete and correct there).
  // The photo-grab readback window runs before any yield decision so it
  // updates every frame in every mode (the grab must work whether the photo
  // flow renders yielded-emulated or native).
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr) {
    // The render thread reads guest memory for the rest of the frame (mesh
    // decode, texture fingerprints, dynamic-item interpolation) and runs no
    // guest code of its own: keep it permanently armed for raw-load
    // read-fault recovery (POSIX; no-op on Windows).
    ArmGuestReadRecoveryForThread(base);
    if (!photo_mode) {
      UpdatePhotoGrabWindow(base);
    }
  }
  if (!photo_mode && YieldForMenus(context)) {
    return false;
  }
  if (base == nullptr) {
    return false;
  }
  if (!photo_mode && YieldForPhotoDisplay()) {
    return false;
  }
  if (!photo_mode && YieldForPhotoEditor(base)) {
    return false;
  }
  if (!photo_mode && YieldForCasEditor(base)) {
    return false;
  }

  const auto render_t0 = PerfClock::now();
  const auto perf_ns_since = [](PerfClock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
            .count());
  };

  bool loading_native = !photo_mode && g_loading_native_frame;
  // Render the takeover-gate window (warmup armed, no fresh substantial
  // scene yet) as a native loading frame instead of yielding, in two cases:
  // (a) g_loading_hold: the frames right after a native loading screen
  //     (the presence context flips to gameplay before the first fresh
  //     scene, and the emulated output was suppressed all through the load,
  //     so yielding would flash its stale pre-load content);
  // (b) boot_native: the whole startup flow (intro videos, boot frontend)
  //     runs with the gate armed and no scene published, and should render
  //     natively as 2D-over-black rather than fall back to emulated.
  if (!photo_mode && !loading_native &&
      (g_loading_hold || REXCVAR_GET(skate3_native_render_scene_boot_native)) &&
      REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms) > 0 &&
      g_warmup_armed.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    // A fresh CAS-editor scene qualifies at ANY size: the startup-flow
    // editor is skater-only (~10 items) but is a
    // complete, deliberate scene; holding it rendered the 3D view black
    // behind the live editor menu. It renders WITHOUT disarming the gate
    // (see the takeover gate below); real gameplay still waits to settle.
    const bool fresh =
        g_scene && g_scene->generation >= g_warmup_fresh_generation;
    const bool ready = fresh && !g_scene->items.empty() &&
                       (!g_guest_loading.load(std::memory_order_acquire) ||
                        CasEditorActive(base));
    loading_native = !ready;
  }
  if (!photo_mode) {
    g_loading_hold = loading_native;
  }
  std::shared_ptr<const FrameScene> scene_ptr;
  if (photo_mode) {
    scene_ptr = g_photo_scene;
    if (!scene_ptr) {
      return false;
    }
  } else if (loading_native) {
    // Native loading screen: there is no current world scene (g_scene holds
    // the PREVIOUS map's stale one); render an empty scene, i.e. a black
    // backdrop, and let the 2D overlay tail replay the game's live loading
    // UI (Publish2dDraws publishes every guest frame, loads included).
    // Value-initialized: zero items, no shadow/blur/outline, generation 0.
    static const std::shared_ptr<const FrameScene> s_loading_scene =
        std::make_shared<const FrameScene>();
    scene_ptr = s_loading_scene;
  } else if (skate3::vr::CurrentEye() == 1 && g_vr_eye_scene) {
    scene_ptr = g_vr_eye_scene;
  } else if (inter_frame) {
    scene_ptr = g_inter.cur;  // the last swapped scene, advanced below
  } else {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    if (!g_scene || g_scene->items.empty()) {
      return false;
    }
    scene_ptr = g_scene;
  }
  if (skate3::vr::CurrentEye() == 0) {
    g_vr_eye_scene = scene_ptr;
  }
  const FrameScene& scene = *scene_ptr;
  if (inter_frame && (loading_native || &scene != g_inter.cur.get())) {
    return false;
  }
  if (!inter_frame && !photo_mode && !loading_native && skate3::vr::CurrentEye() < 0) {
    InterFrameNoteSwap(scene_ptr);
  }
  // Every present is posed while inter-frames are on (see InterFrameState);
  // with them off, the newest scene renders as captured.
  const InterFrameAdvance inter_frame_advance(
      scene, inter_frame || (!photo_mode && skate3::vr::CurrentEye() < 0 &&
                             (REXCVAR_GET(skate3_native_render_scene_interframes) > 0 ||
                              REXCVAR_GET(skate3_native_render_scene_present_pacing) ||
                              REXCVAR_GET(skate3_native_render_scene_interframes_debug_alpha) >=
                                  0.0)));
  const VrEyeCamera vr_eye_camera(scene, photo_mode || skate3::vr::CurrentEye() < 0);
  if (!photo_mode && skate3::vr::CurrentEye() < 0) {
    PaceRecordPresent(scene, inter_frame, inter_frame_advance.ContentTime(scene.play_time),
                      inter_frame_advance.PosedHead());
  }
  // One-pass VR stereo (RenderSceneVr): both eyes in this pass, side by side
  // in the guest output. Scene geometry draws are instanced x2 (scene VS,
  // world._44 = 2); world-space overlays and the HUD draw once per eye into
  // that eye's half (vr_eye_viewport).
  // Every VR frame is side by side, including scene-less ones (the boot
  // menus, loading screens): their 2D still has to land in each eye.
  const bool vr_stereo = !photo_mode && skate3::vr::CurrentEye() == skate3::vr::kStereoEye;
  // The right eye's view_proj for world-space overlays (the scene camera
  // until a stereo camera exists).
  const float* const vr_right_eye_vp =
      skate3::vr::EyeViewProj(1) != nullptr ? skate3::vr::EyeViewProj(1) : scene.view_proj;
  const auto vr_eye_viewport = [&](nrhi::Cmd* c, int e) {
    const uint32_t half = context.guest_output_width / 2;
    c->SetViewport(nrhi::Viewport{float(half * uint32_t(e)), 0.0f, float(half),
                                  float(context.guest_output_height), 0.0f, 1.0f});
    c->SetScissor(nrhi::Rect{int32_t(half * uint32_t(e)), 0, int32_t(half * uint32_t(e + 1)),
                             int32_t(context.guest_output_height)});
  };
  // F5 native -> emulated hand-off (see g_scene_handoff_until_ms): keep
  // presenting natively only while the published camera still carries the
  // WIDE projection; once it is back to 16:9 (aspect = |m11 / m00|) the
  // emulated output takes over with a correct image.
  if (!photo_mode && !REXCVAR_GET(skate3_native_render_scene)) {
    // The pre-widening game aspect: the published proj is always widened to
    // the window, so reading it held every switch to the 500 ms timeout.
    const bool camera_16x9 = !(scene.guest_cam_aspect > 0.0f) ||
                             scene.guest_cam_aspect < (16.0f / 9.0f) * 1.02f;
    if (!SceneHandoffActive() || camera_16x9) {
      g_scene_handoff_until_ms.store(0, std::memory_order_relaxed);
      return false;
    }
  }
  // Rebuilt character pieces (create-a-skater outfit change reuses mesh
  // addresses): drop their sticky texture sites (keyed mesh << 3 | slot) so
  // a pending decode never serves the PREVIOUS garment's art at the new
  // piece. Once per published scene (renders can repeat a generation).
  {
    static uint64_t s_rebuilt_gen = ~0ull;
    if (!scene.rebuilt_meshes.empty() && scene.generation != s_rebuilt_gen) {
      s_rebuilt_gen = scene.generation;
      for (uint32_t mesh : scene.rebuilt_meshes) {
        for (uint64_t slot = 0; slot < 8; ++slot) {
          g_r.tex_sticky.erase((uint64_t(mesh) << 3) | slot);
        }
      }
    }
  }

  if (!EnsurePipeline(context)) {
    return false;
  }
  if (!photo_mode) {
    // The geometry a portrait job renders at (see PhotoRender).
    g_r.last_out_w = context.guest_output_width;
    g_r.last_out_h = context.guest_output_height;
    g_r.last_display_w = context.display_width;
    g_r.last_display_h = context.display_height;
    g_r.last_out_fmt = context.guest_output->format();
  }
  // Foreign-layout pre-pass (see MeshBuffers::layout): before ANY pass
  // draws (the shadow casters run first and read g_r.meshes directly),
  // replace a character piece's cached decode whose layout no longer
  // matches the piece (create-a-skater garment swap at a reused mesh
  // address) with an inline decode of the new geometry. A failed decode
  // leaves the piece undrawn this frame, never drawn with the old shape.
  if (base != nullptr) {
    for (const DrawItem& item : scene.items) {
      if (!(item.skinned || item.ropa || item.cloth_quads) || item.retained) {
        continue;
      }
      auto mit = g_r.meshes.find(item.mesh);
      if (mit == g_r.meshes.end() || mit->second.layout == 0 ||
          !ForeignLayout(item.mesh, mit->second.layout, MeshLayoutKey(item))) {
        continue;
      }
      if (item.ropa) {
        static std::atomic<uint32_t> s_swaps{0};
        const uint32_t n = s_swaps.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: ropa garment swap mesh={:08X} - shape history reset (n={}) "
              "vb_obj={:08X} ib_obj={:08X} vb_bytes={} ib_count={} stride={}",
              item.mesh, n + 1, item.vb_obj, item.ib_obj, item.vb_bytes, item.ib_count,
              item.stride);
        }
      }
      PoolMeshBuffer(g_r.device, mit->second.vb);
      PoolMeshBuffer(g_r.device, mit->second.ib);
      g_r.meshes.erase(mit);
      g_r.ropa_shapes.erase(item.mesh);
      MeshBuffers fresh;
      if (DecodeMesh(g_r.device, base, item, fresh)) {
        fresh.fingerprint = item.fingerprint;
        g_r.meshes.emplace(item.mesh, fresh);
      }
    }
  }
  // Flush any barriers pushed by lazy resource creation (white texture).
  context.cmd->FlushBarriers();

  // FMV routing: prefer the native path; video quads in the 2D replay are
  // SUBSTITUTED with the ps_yuv2d combine, matched by their own captured
  // slot-0 fetch (== that video's Y plane; through ps_main a movie quad
  // renders as an opaque black cover, intro, or slow greyscale luma,
  // camera-page previews). Order-faithful: backdrop fills land under the
  // video like the emulated frame; multiple simultaneous videos each match
  // their own plane set. Only when this path is unavailable does a live
  // movie heartbeat yield to the emulated output.
  MoviePlanes movies[kMaxMovies];
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    std::memcpy(movies, g_movies, sizeof(movies));
  }
  const int64_t movie_now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count();
  bool movie_fresh = false;
  for (const MoviePlanes& m : movies) {
    movie_fresh |= m.ns >= 0 && movie_now_ns - m.ns < 500'000'000;
  }
  // Movie SESSION start (publish-freshness OFF->ON edge = a video began):
  // the triple substitution only serves plane content DECODED DURING THIS
  // SESSION (GuestTexture::last_change_frame >= this stamp). The APT plane
  // copies keep their addresses across videos, so at video N+1's start
  // both the store AND guest memory can still hold video N's last frame;
  // serving either flashed the previous video for a few frames at every
  // boundary. Until the new video's first frame lands (content change ->
  // inline re-decode), the quad holds black, which is what a starting
  // video looks like.
  static uint64_t s_movie_session_frame = 0;
  {
    static bool s_movie_fresh_prev = false;
    if (movie_fresh && !s_movie_fresh_prev) {
      s_movie_session_frame = g_frames_rendered.load(std::memory_order_relaxed);
    }
    s_movie_fresh_prev = movie_fresh;
  }
  const bool movie_sub = movie_fresh &&
                         REXCVAR_GET(skate3_native_render_scene_fmv_native) &&
                         g_r.pso_yuv2d != nullptr;
  if (!photo_mode && !movie_sub && YieldForMovie()) {
    return false;
  }

  nrhi::Cmd* cmd = context.cmd;

  ReleaseRetiredAndFlushCaches(context);

  // Reset this frame's bone ring region (shared by the shadow casters and
  // the main pass: the shadow pass allocates first, the main pass appends).
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint32_t bone_region =
      uint32_t(frame_number % RendererState::kBoneRegions) *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;
  g_r.ropa_ring_offset = 0;

  {
    const std::string tm(REXCVAR_GET(skate3_native_render_scene_trace_mesh));
    const uint32_t parsed =
        tm.empty() ? 0u : uint32_t(std::strtoul(tm.c_str(), nullptr, 16));
    if (parsed != g_trace_mesh_addr) {
      g_trace_mesh_addr = parsed;
      g_trace_keys.clear();
      g_trace_sig.clear();
      REXLOG_INFO("tex-trace: mesh={:08X} {}", parsed,
                  parsed ? "TRACING" : "off");
    }
    const std::string t2(REXCVAR_GET(skate3_native_render_scene_trace_2d));
    const uint32_t parsed2 =
        t2.empty() ? 0u : uint32_t(std::strtoul(t2.c_str(), nullptr, 16));
    if (parsed2 != g_trace_2d_w1) {
      g_trace_2d_w1 = parsed2;
      REXLOG_INFO("2d-trace: w1={:08X} {}", parsed2,
                  parsed2 ? "TRACING" : "off");
    }
  }

  // ---- Loading -> gameplay takeover (seamless boot / map-change loads) ----
  // The loading-screen prewarm (menus branch above) already decoded the
  // registered world behind the load; the FIRST substantial post-load scene
  // renders natively right away. Gates kept: staleness (g_scene holds the
  // PREVIOUS map's scene through the whole load; rendering it shows
  // old-map garbage) and min items (the capture holds a near-empty scene
  // for a few frames while the game fades in; taking over there shows a
  // black/void world; the brief yield shows the game's own fade instead).
  // The frames after takeover run a budgeted settle pass for whatever
  // prewarm missed (dynamic entities, late textures), and the draw path's
  // miss budgets are clamped while settling so leftovers render white/skip
  // for a frame instead of freezing the takeover frame.
  const int32_t warmup_ms = REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms);
  bool settling = false;
  // The takeover gates judge REAL scenes only; a native loading frame
  // renders its empty scene deliberately (the gates re-run as usual once
  // the presence context flips back to gameplay).
  if (!photo_mode && warmup_ms > 0 && !loading_native &&
      REXCVAR_GET(skate3_native_render_scene_debug) == 0) {
    if (g_warmup_armed.load(std::memory_order_relaxed)) {
      // The game's own loading bracket decides readiness (g_guest_loading);
      // the only scene property that still matters is that one has actually
      // been published for THIS world.
      const bool small_or_stale = scene.generation < g_warmup_fresh_generation ||
                                  scene.items.empty() ||
                                  g_guest_loading.load(std::memory_order_acquire);
      // A fresh CAS-editor scene renders despite being under min_items (the
      // startup-flow editor is skater-only, ~10 items), but does NOT count
      // as the gameplay takeover; the gate stays armed for the real load
      // that follows the editor.
      const bool editor_scene = scene.generation >= g_warmup_fresh_generation &&
                                !scene.items.empty() && CasEditorActive(base);
      if (small_or_stale && !editor_scene) {
        // Stale or fade-in scene: yield (brief, a few frames). Keep
        // committing worker results meanwhile; every pre-takeover frame
        // counts on map changes.
        PrewarmCommit(context, frame_number);
        return false;
      }
      if (!small_or_stale) {
        g_warmup_armed.store(false, std::memory_order_relaxed);
        g_settle_until_frame = frame_number + 120;
        g_takeover_frame = frame_number;
        size_t queued = 0;
        {
          std::lock_guard<std::mutex> lock(g_prewarm_mutex);
          queued = g_prewarm_queue.size();
        }
        REXLOG_INFO(
            "native-scene: taking over natively ({} items; prewarm {} done / {} dropped "
            "/ {} queued)",
            scene.items.size(), g_prewarm_done.load(std::memory_order_relaxed),
            g_prewarm_dropped.load(std::memory_order_relaxed), queued);
      }
    }
    settling = frame_number < g_settle_until_frame;
    if (settling) {
      const auto settle_t0 = PerfClock::now();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
      WarmCounters wc;
      for (const DrawItem& item : scene.items) {
        WarmItemResources(context, base, frame_number, item, deadline, wc);
      }
      if (wc.deferred != 0) {
        // Still behind: keep the settle pass (and the draw-path budget
        // clamp) alive until a frame clears with budget to spare - but never
        // past the hard cap: revalidation-churning content (streamed
        // payloads, post-map-change stale items) re-defers every frame and
        // would otherwise hold the full budget open for the whole session.
        const int32_t max_frames =
            REXCVAR_GET(skate3_native_render_scene_settle_max_frames);
        const uint64_t extended = frame_number + 8;
        if (max_frames <= 0 || extended <= g_takeover_frame + uint64_t(max_frames)) {
          g_settle_until_frame = std::max<uint64_t>(g_settle_until_frame, extended);
        } else if (g_settle_until_frame <= frame_number + 1) {
          REXLOG_INFO(
              "native-scene: settle pass CAPPED {} frames after takeover with {} "
              "deferred (revalidation churn?) - draw-path budgets resume",
              frame_number - g_takeover_frame, wc.deferred);
        }
      }
      if (wc.decodes > 0) {
        cmd->FlushBarriers();
      }
      g_warm_decodes.fetch_add(wc.decodes, std::memory_order_relaxed);
      g_warm_deferred.fetch_add(wc.deferred, std::memory_order_relaxed);
      g_pw_settle.Add(perf_ns_since(settle_t0));
    }
  }

  // GPU-time attribution: every pass below marks its start; the backend's
  // timestamp-bucket profiler (d3d12/vulkan_gpu_timestamp_buckets) reports
  // per-pass spans. The first mark starts here so the frame's store commits
  // and evictions are attributed separately from the render passes.
  cmd->ProfileRegion(nrhi::ProfileStage::kCommit);

  if (!g_r.announced) {
    g_r.announced = true;
    REXLOG_INFO("native-scene: rendering natively ({} items, {}x{})", scene.items.size(),
                context.guest_output_width, context.guest_output_height);
  }

  // Commit finished worker decodes every frame (streamed arenas decode on
  // the workers before their first draw instead of hitching the first frame
  // that sees them). Near-no-op when nothing completed. The workers also
  // serve the draw path's steady-state misses (EnqueueMeshMiss/
  // EnqueueWordsMiss), so make sure they exist even on a session that never
  // showed a loading screen with the pipeline up.
  // (Per-frame cache bookkeeping belongs to real frames, not portrait jobs.)
  if (!photo_mode && skate3::vr::CurrentEye() != 1) {
  EnsurePrewarmWorkers();
  // Native loading frames take the loading-screen commit budget; the heavy
  // decode lifting behind the load is unchanged from the yielded path.
  PrewarmCommit(context, frame_number, /*loading=*/loading_native);
  // Content-store LRU: superseded words states (old mip levels, pre-demote
  // detail sets, one-shot UI art) age out once nothing routes to them; the
  // mesh cache ages out streamed-out arenas the same way.
  UpdateEvictFpsEstimate(frame_number);
  // Publish which guest textures the previous frame sampled, so the command
  // processor stops executing per-frame emulated passes whose resolved output
  // nothing reads (rex::graphics::IsUnconsumedPerFramePass). Swap frames
  // only: inter-frames sample the same textures, and walking the whole
  // texture store per present was ~3% of the command processor thread.
  for (const auto& [k, t] : g_r.tex_store) {
    if (inter_frame) {
      break;
    }
    if (t.last_used_frame + 1 >= frame_number) {
      rex::graphics::MarkNativeSampledGuestAddress(t.fetch_words[1] & 0xFFFFF000u);
    }
  }
  for (const auto& [k, t] : g_r.cube_textures) {
    if (t.last_used_frame + 1 >= frame_number) {
      rex::graphics::MarkNativeSampledGuestAddress(t.fetch_words[1] & 0xFFFFF000u);
    }
  }
  rex::graphics::AdvanceNativeSampleFrame();
  EvictTexStore(frame_number, context.device->CurrentSubmission());
  EvictMeshStore(frame_number);
  EvictCubeStore(frame_number, context.device->CurrentSubmission());
  }

  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  const auto shadow_t0 = PerfClock::now();
  const float* sh = scene.shadow_rows;
  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  cmd->ProfileRegion(nrhi::ProfileStage::kShadow);
  // (Inter-frames render their own atlas: reusing the swap frame's made
  // moving shadows lag their interpolated casters and snap forward on every
  // guest frame -- visible shadow flicker.)
  if (photo_mode || skate3::vr::CurrentEye() == 1) {
    // VR right eye: the atlas and the static sun map are camera independent
    // (the game's own cascade rows), so the left eye's pass is reused.
    // The portrait pass's own caster pass renders the same skater in the
    // same pose under the same light as the frame just rendered: sample
    // that frame's atlas (re-rendering it from the photo subset would also
    // disturb the atlas/static caches the next frame reuses).
    shadow_ready = g_r.last_shadow_ready;
  } else {
    shadow_ready =
        RenderShadowAtlas(context, scene, bone_region, debug_mode, &shadow_draws);
    g_r.last_shadow_ready = shadow_ready;
    cmd->ProfileRegion(nrhi::ProfileStage::kStaticSun);
    RenderStaticSunMap(context, scene, bone_region, debug_mode, frame_number);
  }
  g_pw_shadow.Add(perf_ns_since(shadow_t0));
  cmd->ProfileRegion(nrhi::ProfileStage::kMain);
  // Scene-level flicker probe: the shadow cascade rows (or validity/readiness)
  // returning EXACTLY to the previous-but-one frame's state; hair samples the
  // atlas, so an alternating cascade capture darkens/lightens it per frame.
  {
    uint64_t sh_h = 1469598103934665603ull;
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(scene.shadow_rows);
    for (size_t bi = 0; bi < sizeof(scene.shadow_rows); ++bi) {
      sh_h = (sh_h ^ sb[bi]) * 1099511628211ull;
    }
    sh_h ^= (scene.shadow_valid ? 2u : 0u) | (shadow_ready ? 4u : 0u);
    static uint64_t s_sh_h1 = 0, s_sh_h2 = 0;  // render thread
    if (s_sh_h2 != 0 && sh_h != s_sh_h1 && sh_h == s_sh_h2) {
      g_shadow_alternations.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_sh_logged{0};
      const uint32_t sl = s_sh_logged.fetch_add(1, std::memory_order_relaxed);
      if (sl < 24 || (sl & 1023u) == 0) {
        REXLOG_DEBUG(
            "native-scene: SHADOW ALTERNATION valid={} ready={} draws={} "
            "row0=({:.3f},{:.3f},{:.3f},{:.3f}) (n={})",
            scene.shadow_valid ? 1 : 0, shadow_ready ? 1 : 0, shadow_draws,
            sh[0], sh[1], sh[2], sh[3], sl);
      }
    }
    s_sh_h2 = s_sh_h1;
    s_sh_h1 = sh_h;
  }

  // The scene draws into the MSAA target when enabled (resolved into the 1x
  // scene plane at the end of the pass), or straight into the 1x plane. The
  // 1x plane is the float HDR intermediate when the HDR post chain is live
  // (ps_tonemap then writes the guest output), else the guest output itself.
  const bool hdr_on = g_r.hdr_active && g_r.hdr_resolved != nullptr &&
                      g_r.hdr_srv != nullptr && g_r.pso_tonemap != nullptr;
  const bool msaa_on = g_r.msaa > 1 && g_r.msaa_color != nullptr && g_r.resolve_pso != nullptr;
  nrhi::Texture* scene_color =
      msaa_on ? g_r.msaa_color
              : (hdr_on ? g_r.hdr_resolved : context.guest_output);
  if (!msaa_on && !hdr_on) {
    cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
  }

  const bool use_depth = debug_mode != 4;
  // Loading frames clear to black (the game's loading UI composes over
  // black); real scenes keep the sky-ish debug clear that shows through
  // undecoded holes.
  float clear_color[4] = {loading_native ? 0.0f : 0.25f,
                          loading_native ? 0.0f : 0.35f,
                          loading_native ? 0.0f : 0.55f, 1.0f};
  // Scenes the game renders over its own clear (trick guide, create-a-skater,
  // replay editor: no sky) show the game's clear color, not the debug one.
  if (!loading_native && scene.clear_valid) {
    clear_color[0] = scene.clear_color[0];
    clear_color[1] = scene.clear_color[1];
    clear_color[2] = scene.clear_color[2];
  }
  // Portrait job: the skater renders over transparent black, so the plane's
  // alpha is its coverage (scene shaders write alpha 1; hair blends it). The
  // photo tail composites it between the pass's own 2D layers over the
  // game's raw clear colour, in the 1152x640 target. The packed HDR format
  // has no alpha: there the skater stays opaque over the clear (no backdrop
  // layer behind it; the layers drawn after it still apply).
  const bool photo_coverage =
      photo_mode && !(hdr_on && g_r.hdr_scene_format == nrhi::Format::kR11G11B10_FLOAT);
  if (photo_coverage) {
    clear_color[0] = clear_color[1] = clear_color[2] = clear_color[3] = 0.0f;
  } else if (hdr_on) {
    // The clear is authored in the final gamma space; the HDR plane holds
    // pre-tonemap values, so encode through the tone chain's inverse (the
    // same transform as scene.hlsl PassGamma) so undecoded holes keep the
    // same debug color after the tonemap pass.
    for (int k = 0; k < 3; ++k) {
      const float tm =
          clear_color[k] * clear_color[k] * (2.0f / (1.41f * 1.41f));
      clear_color[k] = tm > 1.0f
                           ? tm * 4.0f - 3.0f
                           : 1.0f - std::sqrt(std::max(1.0f - tm, 0.0f));
    }
  }
  // One-pass VR stereo: hand the shadow passes to the GPU before recording
  // the main pass (everything below binds its own state).
  if (vr_stereo && skate3::vr::FlushEyes() && context.device->FlushSubmission()) {
    skate3::vr::NoteMidFrameSubmit();
  }
  cmd->ClearRenderTarget(scene_color, clear_color);
  if (use_depth) {
    cmd->ClearDepth(g_r.depth, 1.0f);
    cmd->SetRenderTargets(scene_color, g_r.depth);
  } else {
    cmd->SetRenderTargets(scene_color, nullptr);
  }

  // Portrait job: the game's 1152x640 target, scaled to fit the output
  // plane (the photo camera's projection is authored for that aspect).
  uint32_t pass_w = context.guest_output_width;
  uint32_t pass_h = context.guest_output_height;
  if (photo_mode) {
    const float s = std::min(float(pass_w) / float(RendererState::kBlurWidth),
                             float(pass_h) / float(RendererState::kBlurHeight));
    pass_w = std::max(1u, uint32_t(float(RendererState::kBlurWidth) * s));
    pass_h = std::max(1u, uint32_t(float(RendererState::kBlurHeight) * s));
  }
  const nrhi::Viewport viewport{0.0f, 0.0f, float(pass_w), float(pass_h), 0.0f, 1.0f};
  cmd->SetViewport(viewport);
  const nrhi::Rect scissor{0, 0, int32_t(pass_w), int32_t(pass_h)};
  cmd->SetScissor(scissor);
  // The frame build extends the 16:9 camera to this output aspect.
  if (!photo_mode && context.guest_output_height != 0) {
    g_native_output_aspect.store(
        float(context.guest_output_width) / float(context.guest_output_height),
        std::memory_order_relaxed);
  }
  // One-pass VR stereo: the pixels the headset lenses never show get black
  // and near-plane depth before anything draws, so every scene draw there
  // fails the depth test and nothing is shaded (OpenVR's hidden-area mesh).
  if (vr_stereo && use_depth) {
    const std::vector<float>* masks[2] = {&skate3::vr::HiddenAreaTriangles(0),
                                          &skate3::vr::HiddenAreaTriangles(1)};
    const uint32_t samples = msaa_on ? g_r.msaa : 1u;
    const uint32_t key = (samples << 8) | uint32_t(scene_color->format());
    if (!masks[0]->empty() && (g_vr_mask_pso == nullptr || g_vr_mask_key != key)) {
      if (g_vr_mask_pso != nullptr) {
        context.device->DestroyDeferred(g_vr_mask_pso);
        g_vr_mask_pso = nullptr;
      }
      nrhi::Shader* vs = context.device->CreateShader(MakeShaderDesc(
          nrhi::ShaderStage::kVertex, "overlay2d.hlsl", kShader2dSource, "vs_main", nullptr, ""));
      nrhi::Shader* ps = context.device->CreateShader(MakeShaderDesc(
          nrhi::ShaderStage::kPixel, "overlay2d.hlsl", kShader2dSource, "ps_main", nullptr, ""));
      if (vs != nullptr && ps != nullptr) {
        static constexpr nrhi::InputElementDesc input2d[3] = {
            {"POSITION", 0, 0, nrhi::Format::kR32G32B32A32_FLOAT, 0},
            {"TEXCOORD", 0, 1, nrhi::Format::kR32G32_FLOAT, 16},
            {"COLOR", 0, 2, nrhi::Format::kR8G8B8A8_UNORM, 24}};
        nrhi::GraphicsPipelineDesc pd;
        pd.layout = g_r.layout;
        pd.vs = vs;
        pd.ps = ps;
        pd.input_elements = input2d;
        pd.input_element_count = 3;
        pd.vertex_stride = 28;
        pd.depth.test_enable = true;
        pd.depth.write_enable = true;
        pd.depth.func = nrhi::CompareFunc::kAlways;
        pd.cull = nrhi::CullMode::kNone;
        pd.rtv_format = scene_color->format();
        pd.dsv_format = nrhi::Format::kD32_FLOAT;
        pd.sample_count = samples;
        g_vr_mask_pso = context.device->CreateGraphicsPipeline(pd);
        g_vr_mask_key = key;
      }
      if (vs) context.device->DestroyDeferred(vs);
      if (ps) context.device->DestroyDeferred(ps);
    }
    constexpr uint32_t kMaskSlotBytes = 128 * 1024;
    constexpr uint32_t kMaskSlots = 8;
    if (g_vr_mask_vb == nullptr) {
      g_vr_mask_vb = CreateUploadBuffer(context.device, size_t(kMaskSlotBytes) * kMaskSlots,
                                        nrhi::BufferBindClass::kVertexIndex);
      g_vr_mask_vb_cpu =
          g_vr_mask_vb ? static_cast<uint8_t*>(context.device->Map(g_vr_mask_vb)) : nullptr;
    }
    if (g_vr_mask_pso != nullptr && g_vr_mask_vb_cpu != nullptr) {
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPipeline(g_vr_mask_pso);
      // overlay2d: identity clip rows / world rows, color multiplier
      // opaque black (the hidden pixels stay black instead of the clear
      // color, which would also skew the exposure measurement).
      float mc[52] = {};
      mc[0] = mc[5] = mc[10] = mc[15] = 1.0f;
      mc[16] = mc[21] = mc[26] = mc[31] = 1.0f;
      mc[35] = 1.0f;
      cmd->SetRootConstants(0, 52, mc, 0);
      cmd->SetTexture(1, g_r.white.srv);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const uint32_t slot_base = (g_vr_mask_slot++ % kMaskSlots) * kMaskSlotBytes;
      uint32_t used = 0;
      for (int e = 0; e < 2; ++e) {
        const std::vector<float>& m = *masks[e];
        const uint32_t verts = uint32_t(m.size() / 2);
        const uint32_t bytes = verts * 28u;
        if (verts == 0 || used + bytes > kMaskSlotBytes) {
          continue;
        }
        uint8_t* dst = g_vr_mask_vb_cpu + slot_base + used;
        for (uint32_t v = 0; v < verts; ++v) {
          const float vert[6] = {m[v * 2], m[v * 2 + 1], 0.0f, 1.0f, 0.0f, 0.0f};
          const uint32_t white = 0xFFFFFFFFu;
          std::memcpy(dst + v * 28u, vert, sizeof(vert));
          std::memcpy(dst + v * 28u + 24u, &white, 4);
        }
        cmd->SetVertexBuffer(g_vr_mask_vb, slot_base + used, bytes, 28);
        vr_eye_viewport(cmd, e);
        cmd->Draw(verts, 0);
        used += bytes;
      }
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
    }
  }
  cmd->SetBindingLayout(g_r.layout);
  cmd->SetPipeline(use_depth ? g_r.pso : g_r.pso_nodepth);
  // Per-item PSO tracking for the opaque pass: two_sided_sheet meshes swap
  // to the backface-culling variant (see MeshBuffers), everything else uses
  // the pass PSO. Only meaningful while use_depth; the transparent sub-pass
  // sets its own PSO and is not switched per item.
  nrhi::Pipeline* scene_pso_bound = use_depth ? g_r.pso : g_r.pso_nodepth;

  // Per-frame dynamic-shadow bindings: receiver constants at b1 (a small
  // per-frame CBV ring: the 52-float root-constant block is full) and the
  // blurred atlas at t5 (white when no atlas was rendered this frame; the
  // shader is also gated by sh_misc.y).
  // Per-material flowing-water sets packed into b1 below (fw_set): the
  // material rows of each, compared without the animation time (row 16).
  constexpr uint32_t kFlowSets = 6;
  const float* flow_set_rows[kFlowSets] = {};
  uint32_t flow_set_count = 0;
  const auto flow_set_of = [&](const DrawItem& it) -> int {
    if (!it.water_rows_seen) {
      return -1;
    }
    for (uint32_t k = 0; k < flow_set_count; ++k) {
      const float* a = flow_set_rows[k];
      if (std::memcmp(a, it.water_rows, 16 * sizeof(float)) == 0 &&
          std::memcmp(a + 17, it.water_rows + 17, 15 * sizeof(float)) == 0) {
        return int(k);
      }
    }
    return -1;
  };
  uint32_t main_cb_offset = 0;  // this frame's b1 slice (restore_main_pass)
  if (g_r.shadow_cb != nullptr) {
    const uint32_t cb_offset =
        uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
    main_cb_offset = cb_offset;
    float* cb = reinterpret_cast<float*>(g_r.shadow_cb_cpu + cb_offset);
    std::memset(cb, 0, RendererState::kShadowCbSlice);
    // st_d (scene_frame_cb.hlsli, float 956): one-pass VR stereo's left ->
    // right clip transform; only read by draws flagged world._44 = 2.
    if (vr_stereo) {
      std::memcpy(cb + 956, skate3::vr::StereoMatrix(), 16 * sizeof(float));
    }
    if (skate3::vr::FirstPersonActive()) {
      std::memcpy(cb+972, skate3::vr::FirstPersonHead(), 3*sizeof(float));
      cb[975]=0.32f;  // local player's head only; body and shadow remain
    }
    // Portrait jobs take no sun shadows: the cascades (and the static sun
    // map) are the editor view's, placed for the on-screen scene, while the
    // photo stage stands elsewhere; sampled there they laid unrelated hard
    // shadows across the skater. The game's own pass reads the same atlas
    // and its stage falls outside it, so its picture comes out unshadowed.
    if (shadow_ready && !photo_mode) {
      std::memcpy(cb + 0, sh + 0, 4 * sizeof(float));    // sh_x   = PS c0
      std::memcpy(cb + 4, sh + 12, 4 * sizeof(float));   // sh_y   = PS c3
      std::memcpy(cb + 8, sh + 16, 4 * sizeof(float));   // sh_z   = PS c4
      std::memcpy(cb + 12, sh + 4, 4 * sizeof(float));   // sh_c1  = PS c1
      std::memcpy(cb + 16, sh + 8, 4 * sizeof(float));   // sh_c2  = PS c2
      // sh_color: retail compiles the world shadow tint as a literal in
      // every world PS (min(lm^2, s + (0.05, 0.09, 0.13)): baseenvironment /
      // defaultenvironment / baseterrain / decal / alphatest / reflective /
      // environmentdiffuse c255.yzw). PS c8 (g_envattributes[0]), which the
      // dev port read here, is not referenced by any of them, and whatever
      // the game leaves there (gated only to <= 0.4) lifted the shadows.
      cb[20] = 0.05f;
      cb[21] = 0.09f;
      cb[22] = 0.13f;
      cb[23] = 0.299f * cb[20] + 0.587f * cb[21] + 0.114f * cb[22];
      cb[24] = sh[20];  // depth bias (PS c5.x)
      cb[25] = 1.0f;    // enable
      // sh_misc.zw = atlas dimensions (3*tile x tile) for the char path's
      // point-snapped taps.
      cb[26] = float(g_r.shadow_tile * 3);
      cb[27] = float(g_r.shadow_tile);
      // sh_char = the game's per-cascade character receive biases + enable
      // (see FrameScene::char_shadow_bias; exact 9-tap char sampling).
      if (scene.char_shadow_bias[0] > 0.0f &&
          REXCVAR_GET(skate3_native_render_scene_char_shadow_exact)) {
        cb[56] = scene.char_shadow_bias[0];
        cb[57] = scene.char_shadow_bias[1];
        cb[58] = scene.char_shadow_bias[2];
        cb[59] = 1.0f;
      }
    }
    // PCSS soft-shadow rows (sh_pcss / sh_pcss2, cb[136..143]). Filled
    // whenever the light rows are valid; the static sun-shadow sampler
    // shares the angular knobs even on frames with no dynamic casters.
    if (scene.shadow_valid) {
      cb[136] =
          REXCVAR_GET(skate3_native_render_scene_shadow_pcss) ? 1.0f : 0.0f;
      cb[137] = std::tan(
          0.5f * 0.01745329f *
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_sun_deg)));
      cb[138] =
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_max_m));
      cb[139] = 0.0f;  // atlas slope bias unused (dynamic casters only)
      cb[140] =
          float(REXCVAR_GET(skate3_native_render_scene_shadow_pcss_blocker_m));
      const float zlen =
          std::sqrt(sh[16] * sh[16] + sh[17] * sh[17] + sh[18] * sh[18]);
      const float xlen =
          std::sqrt(sh[0] * sh[0] + sh[1] * sh[1] + sh[2] * sh[2]);
      cb[141] = zlen > 1e-8f ? 1.0f / zlen : 0.0f;
      cb[142] = xlen;
      cb[143] = float(
          REXCVAR_GET(skate3_native_render_scene_shadow_pcss_min_texel));
    }
    // Native static sun-shadow rows (nsm_x/y/z + params, cb[144..163]);
    // nsm_p.x = 0 disables the term in every branch (the scene receive, the
    // character branch and the volumetric shafts all read this slice).
    // Gated on FRESH world rows, not just shadow_valid: interior venues
    // (park-editor warehouses) never produce a sane environment bank, so the
    // sun direction here would be a stale outdoor capture. The map then
    // shades the whole interior as roof-shadowed, and with no game shadow
    // pass this frame sh_color is zero, so the exact env families clamp
    // min(lm^2, s + sh_color) to pure black - the missing walls/floor.
    // The game itself never sun-shadows static world geometry indoors;
    // standing the map down restores its exact lm^2 shading.
    if (g_r.static_sun_valid && scene.shadow_fresh && !photo_mode) {
      std::memcpy(cb + 144, g_r.nsm_rows, sizeof(g_r.nsm_rows));
      cb[156] = std::clamp(
          float(REXCVAR_GET(skate3_native_render_scene_shadow_static_strength)),
          0.0f, 1.0f);
      cb[157] = 1.0f / g_r.nsm_radius;  // far-tile uc units per meter
      cb[158] = g_r.nsm_depth_range;    // meters per depth unit
      // Filter floor: 2 physical texels (uc spans [-1,1] over one tile);
      // the map has no dilation blur, so a sub-texel kernel exposes raw
      // stair-step aliasing at full strength; two texels of
      // rotated-poisson filtering dissolve the steps into a smooth edge.
      cb[159] = 2.0f * 2.0f *
                float(REXCVAR_GET(
                    skate3_native_render_scene_shadow_pcss_min_texel)) /
                float(std::max(1u, g_r.static_sun_size));
      float bias_m = float(
          REXCVAR_GET(skate3_native_render_scene_shadow_static_bias));
      if (context.device->backend() == nrhi::Backend::kVulkan) {
        // The two backends compile the caster/receiver depth math through
        // different shader toolchains; their rounding differences leave
        // borderline texels flipping at map rebuilds without this margin.
        bias_m += float(
            REXCVAR_GET(skate3_native_render_scene_shadow_static_bias_vk));
      }
      cb[160] = bias_m / g_r.nsm_depth_range;  // base receiver bias
      cb[161] = bias_m / g_r.nsm_depth_range;  // slope-scaled term
      cb[162] = 0.0f;  // reserved (cascade ratios are shader constants)
      cb[163] = float(std::max(1u, g_r.static_sun_size));  // tile px
    }
    // Exact world-shading frame rows (valid whenever the env-family PS bank
    // was captured this frame, independent of the shadow ATLAS being
    // rendered; draw_item only selects the exact branch when
    // scene.shadow_valid).
    if (scene.shadow_valid) {
      cb[28] = sh[24];  // sun direction (PS c6), for the normal-map kd (v2)
      cb[29] = sh[25];
      cb[30] = sh[26];
      cb[31] = sh[40];  // scene exposure (PS c10.x)
      cb[32] = sh[45];  // material multiplier (PS c11.y)
      cb[33] = scene.family_rows[0];  // tree lightmap scale (tree PS c0.x)
      cb[34] = scene.family_rows[1];  // tree lightmap floor (tree PS c0.y)
      cb[35] = scene.family_rows[2];  // tree tint multiplier (tree PS c4.y)
      cb[36] = scene.fog_ramp[0];     // global fog: sat(d*x+y)^z
      cb[37] = scene.fog_ramp[1];
      cb[38] = scene.fog_ramp[2];
      cb[39] = scene.family_rows[3];  // proxyworld scale (proxy PS c3.y)
      std::memcpy(cb + 40, scene.fog_color, 4 * sizeof(float));
    }
    // World-shading v2 row (sh_v2, cb[60..63]): x = stored-tangent
    // polarity, yzw = the build-up showcase split state (stage left/right
    // of the split + its position in output pixels; zeros = showcase off).
    cb[60] =
        float(REXCVAR_GET(skate3_native_render_scene_world_v2_tan_sign));
    TickShowcase(context.guest_output_width, hdr_on);
    cb[61] = g_r.showcase_rows[0];
    cb[62] = g_r.showcase_rows[1];
    cb[63] = g_r.showcase_rows[2];
    // environmentpark.* frame rows (park_* at cb[704..727]; see
    // FrameScene::park_rows for the row map and the per-variant bank
    // layouts they were read from).
    if (scene.park_valid[0]) {
      std::memcpy(cb + 704, scene.park_rows[0], sizeof(scene.park_rows[0]));
    }
    // The alphatest variant's own set (park1_* at cb[764..787]).
    if (scene.park_valid[1]) {
      std::memcpy(cb + 764, scene.park_rows[1], sizeof(scene.park_rows[1]));
    }
    // Park light-cookie projection (park_lmu / park_lmv at cb[728..735]).
    if (scene.park_lm_valid) {
      std::memcpy(cb + 728, scene.park_lm, sizeof(scene.park_lm));
    }
    // Park world-shadow receive (park_wsu / park_wsv / park_wsd / park_wst /
    // park_wsb at cb[736..755]): the receivers' own PS c5, c7, c8, c6 and
    // c10, sampled from the native park_ws map at t10. park_wsb.w = 1 when
    // the map is built (0 = the world term stays lit).
    if (scene.park_ws_recv_valid) {
      // The rows the map was built with (see park_ws_built_recv), so the
      // lookups always match the map's own projection.
      const float* rc = g_r.park_ws_built_valid ? g_r.park_ws_built_recv
                                                : scene.park_ws_recv;  // c5, c6, c7, c8, c10
      std::memcpy(cb + 736, rc + 0, 4 * sizeof(float));   // c5: u row
      std::memcpy(cb + 740, rc + 8, 4 * sizeof(float));   // c7: v row
      std::memcpy(cb + 744, rc + 12, 4 * sizeof(float));  // c8: ray row
      std::memcpy(cb + 748, rc + 4, 4 * sizeof(float));   // c6: outer scale / offset
      cb[752] = rc[16];  // c10.x inner bias
      cb[753] = rc[17];  // c10.y outer bias
      cb[754] = rc[18];  // c10.z exponential-compare scale
      cb[755] = (g_r.park_ws_primed && g_r.park_ws_in_srv) ? 1.0f : 0.0f;
    }
    // The world's skate.Park receivers (park_dfa / park_dfb at cb[756..763];
    // see DrawItem::dmo_recv): their cascade floor c11 + cascade bias c9.x,
    // the park-map floor c12 + enable.
    if (scene.park_dmo_valid) {
      const float* dr = scene.park_dmo_rows;
      cb[756] = dr[0];
      cb[757] = dr[1];
      cb[758] = dr[2];
      cb[759] = dr[8];
      cb[760] = dr[4];
      cb[761] = dr[5];
      cb[762] = dr[6];
      cb[763] = 1.0f;
    }
    // dynamicobject.fx frame rows (dyn_sun/dyn_amb/dyn_misc at cb[44..55]).
    if (scene.dynobj_valid) {
      cb[44] = scene.dynobj_rows[0];  // sun dir (PS c9)
      cb[45] = scene.dynobj_rows[1];
      cb[46] = scene.dynobj_rows[2];
      cb[47] = scene.dynobj_rows[3];  // scene exposure (c13.x)
      cb[48] = scene.dynobj_rows[4];  // flat ambient rgb (c15.xyz)
      cb[49] = scene.dynobj_rows[5];
      cb[50] = scene.dynobj_rows[6];
      cb[51] = scene.dynobj_rows[7];  // bounce scale (c15.w)
      cb[52] = scene.dynobj_rows[8];  // material multiplier (c14.y)
      cb[53] = scene.dynobj_rows[9];  // static world-shadow floor (c8.w)
      // dyn_bias (cb[236..239]): the per-cascade CSM receive biases c8.xyz +
      // enable, consumed by the dynobj branch's exact one-texel filter.
      if (scene.dynobj_rows[10] > 0.0f) {
        cb[236] = scene.dynobj_rows[10];
        cb[237] = scene.dynobj_rows[11];
        cb[238] = scene.dynobj_rows[12];
        cb[239] = 1.0f;
      }
    }
    // The world-shade map's flag (dyn_misc.z) and receiver rows
    // (dyn_wsx..z): properties of the map, not of the dynamicobject bank.
    // Staged inside the dynobj block they were left unset in sessions that
    // never draw a dynamicobject prop (a skate.Park), and every tf1
    // receiver there -- the park alphatest pieces, the characters -- lost
    // its world term. Character draws bind the map at t10 in place of the
    // static sun map when it is set (see CharStaticShade / char_t10_view).
    cb[54] = WorldShadeReady(scene) ? 1.0f : 0.0f;
    // Static world-shadow transform rows (dyn_wsx/dyn_wsy/dyn_wsz,
    // cb[64..75]): consumed only when a draw carries the misc.z world-
    // shadow bind flag (the map primed and bound at t4).
    if (scene.dynobj_ws_valid) {
      std::memcpy(cb + 64, scene.dynobj_ws, sizeof(scene.dynobj_ws));
    }
    // While the native map exists, sample it with the rows it was BUILT
    // with (the pass's VS c0..c2). In the game every receiver's PS
    // c5..c7 equal that frame's pass rows (capture 1790887078), but our
    // receiver rows are latched from the latest receiver draw while the
    // map is rebuilt only on the frames the game resolved it: one frame
    // apart while the camera moves (the map recentres on the camera,
    // which orbits the skater), texels near a shadow edge swapped and
    // the receivers' shading flickered with camera motion. Not under the
    // lighting-lab sun override, which re-bases the receiver rows.
    if (g_r.world_shadow_primed &&
        !REXCVAR_GET(skate3_native_render_scene_sun_override)) {
      std::memcpy(cb + 64, g_r.world_shadow_rows, sizeof(g_r.world_shadow_rows));
    }
    // One decal for every live skater, as in the guest's postfx deck loop.
    // The texture is shared; each placement follows its own playback history.
    uint32_t deck_words[6] = {};
    if (base && skate3::deck_shadow::GetDecalFetch(deck_words) &&
        (!g_r.deck_decal_valid || std::memcmp(g_r.deck_decal_words, deck_words, sizeof(deck_words)) != 0)) {
      if (EnsureGuestTextureFromWords(context, base, deck_words, g_r.deck_decal) && g_r.deck_decal.srv) {
        std::memcpy(g_r.deck_decal_words, deck_words, sizeof(deck_words));
        g_r.deck_decal_valid = true;
      }
    }
    if (scene.tree_anim_valid) {
      std::memcpy(cb + 1088, scene.tree_anim_rows, sizeof(scene.tree_anim_rows));
    }
    if (scene.flag_anim_valid) {
      std::memcpy(cb + 1104, scene.flag_anim_rows, sizeof(scene.flag_anim_rows));
    }
    static_assert(RendererState::kShadowCbSlice >= 1124 * sizeof(float));
    static_assert(RendererState::kShadowCbSlice >= (976 + skate3::deck_shadow::kMaxSkaters * 16) * sizeof(float));
    for (uint32_t index = 0; index < skate3::deck_shadow::kMaxSkaters; ++index) {
      const auto& deck = scene.decks[index];
      float* rows = cb + 976 + index * 16;
      rows[12] = 0.0f;
      if (base && deck.valid && g_r.deck_decal_valid) {
        // Re-timed onto the render playback clock by
        // InterpolateDynamicItems: the raw guest matrix runs ahead of the
        // board as rendered, which slid the decal forward at speed.
        const float* const right = deck.world + 0;  // local X (width)
        const float* const up = deck.world + 4;     // local Y
        const float* const fwd = deck.world + 8;    // local Z (length)
        const float* const pos = deck.world + 12;
        // 1 / (2 * half-extent) == the guest's own c4.xy (1.6, 0.9).
        const float su = 0.5f / skate3::deck_shadow::kHalfWidth;
        const float sv = 0.5f / skate3::deck_shadow::kHalfLength;
        rows[0] = right[0] * su;
        rows[1] = right[1] * su;
        rows[2] = right[2] * su;
        rows[3] = 0.5f - (right[0] * pos[0] + right[1] * pos[1] +
                          right[2] * pos[2]) * su;
        rows[4] = up[0];  // unscaled, as the guest's c1 row is
        rows[5] = up[1];
        rows[6] = up[2];
        rows[7] = -(up[0] * pos[0] + up[1] * pos[1] + up[2] * pos[2]);
        rows[8] = fwd[0] * sv;
        rows[9] = fwd[1] * sv;
        rows[10] = fwd[2] * sv;
        rows[11] = 0.5f - (fwd[0] * pos[0] + fwd[1] * pos[1] +
                          fwd[2] * pos[2]) * sv;
        // c4.z: the guest's fade-to-white rate per unit off the board's
        // plane (full white at 1/5 = 0.2 units), verbatim.
        rows[13] = 5.0f;
        rows[14] = 0.0f;
        rows[15] = 0.0f;
        rows[12] = 1.0f;
      }
    }
    // flowingwateralpha m_params rows (wat_p0..wat_p3 at cb[76..91]):
    // m_params[0..2] verbatim, then (time, mask threshold, spec power,
    // alpha floor). Consumed by the exact water branch (cam_pos.w = -30),
    // which is only selected per draw when scene.water_valid.
    if (scene.water_valid) {
      std::memcpy(cb + 76, scene.water_rows, 12 * sizeof(float));   // c11..c13
      cb[88] = scene.water_rows[16];  // g_fAnimationTime (c15.x)
      // The mask threshold is c18.y (ucode 17/25 subtract it from BOTH tf5
      // channels), not c14.y. All three of wat_p3.yzw were reading c14
      // where the ucode reads c18 -- one consistent wrong-row error.
      cb[89] = scene.water_rows[19];  // mask threshold (c18.y)
      // Spec power and alpha floor come from c18, not c14 (ucode 75-77 and
      // 97); taking both from c14 gave every material this one's numbers.
      cb[90] = scene.water_rows[17];  // spec power (c18.z)
      cb[91] = scene.water_rows[18];  // alpha floor (c18.w)
      // wat_gate at cb[224]: the reflection gate's own c14.yzw.
      cb[224] = scene.water_rows[13];
      cb[225] = scene.water_rows[14];
      cb[226] = scene.water_rows[15];
      // wat_gate.w = c13.w, the reflection strength (ucode 86). Baked as 1.5.
      cb[227] = scene.water_rows[28];
      // Spec tint = c12 * 3. The ucode scales c12.yyyz by the literal
      // c254.w (ucode 64) and the captured c12 is (0.7, 0.6, 0.5) -- exactly
      // a third of the (2.1, 1.8, 1.5) that was baked in and validated by
      // eye, so that literal is 3.0. Kept explicit rather than folded away,
      // since it now scales with the material instead of being frozen.
      cb[228] = scene.water_rows[21] * 3.0f;  // c12.x spec tint
      cb[229] = scene.water_rows[22] * 3.0f;  // c12.y
      cb[230] = scene.water_rows[23] * 3.0f;  // c12.z
      cb[231] = scene.water_rows[20];  // c10.w spec lightmap floor
      cb[232] = scene.water_rows[27];  // c15.w normal scale x
      cb[233] = scene.water_rows[26];  // c15.z normal scale y
      cb[234] = scene.water_rows[24];  // c15.x normal scale z
      // wat_nrm.w = c15.y, the output multiplier (ucode 87:
      // `mulsc r0.__z_, c15.y, r5.w`). The port used c11.y (0.62) where the
      // game uses c15.y (0.2) -- about 3x too bright over the whole surface.
      cb[235] = scene.water_rows[25];  // c15.y
    }
    // Per-material flowing-water sets (fw_set at float 788, 6 x 7 rows:
    // wat_p0, p1, p2, p3, gate, spec, nrm in the frame layout above): the
    // distinct material rows of this frame's flowing-water items, each
    // captured off its own draw (DrawItem::water_rows). The frame-global
    // rows above are ONE material's; a waterfall drawn with another
    // surface's scroll / scale / spec rows looked nothing like the game.
    // The animation time stays the frame's.
    flow_set_count = 0;
    for (const DrawItem& it : scene.items) {
      if (!it.water_flowing || !it.water_rows_seen || flow_set_count >= kFlowSets) {
        continue;
      }
      if (flow_set_of(it) >= 0) {
        continue;
      }
      flow_set_rows[flow_set_count] = it.water_rows;
      float* d = cb + 788 + flow_set_count * 28;
      const float* r = it.water_rows;
      std::memcpy(d, r, 12 * sizeof(float));  // p0, p1, p2
      d[12] = scene.water_valid ? scene.water_rows[16] : r[16];
      d[13] = r[19];
      d[14] = r[17];
      d[15] = r[18];
      d[16] = r[13];
      d[17] = r[14];
      d[18] = r[15];
      d[19] = r[28];
      d[20] = r[21] * 3.0f;
      d[21] = r[22] * 3.0f;
      d[22] = r[23] * 3.0f;
      d[23] = r[20];
      d[24] = r[27];
      d[25] = r[26];
      d[26] = r[24];
      d[27] = r[25];
      ++flow_set_count;
    }
    // ocean_defaultPS rows (oc_mean/oc_w0..5/oc_p0..2 at cb[92..131]) and
    // the oceanreflection fade row (orf at cb[132..135]); consumed by the
    // exact ocean branches (cam_pos.w = -31 / -32).
    if (scene.ocean_valid) {
      std::memcpy(cb + 92, scene.ocean_rows, sizeof(scene.ocean_rows));
    }
    if (scene.oceanrefl_valid) {
      std::memcpy(cb + 132, scene.oceanrefl_rows,
                  sizeof(scene.oceanrefl_rows));
    }
    // water.default rows (wd_* at cb[164..219]); consumed by the exact
    // water.default branch (cam_pos.w = -33).
    if (scene.waterdef_valid) {
      std::memcpy(g_r.waterdef_rows_last, scene.waterdef_rows,
                  sizeof(g_r.waterdef_rows_last));
      g_r.waterdef_rows_have = true;
    }
    // Unconditional, from the retained copy, into EVERY ring region -- not
    // just this frame's slice. A draw that selected fam 33 was observed
    // reading a slice whose wd_ block was zero while wd_screen (written in
    // the same fill) was present, i.e. it was reading a slice written by a
    // different invocation. Writing all regions removes the mismatch as a
    // variable; it is 56 floats x 8 regions per frame.
    // Per-material sets: set 0 at float 164, set 1 at float 256. A draw
    // picks between them with misc.z, matching item.water_normal against the
    // captured key.
    if (scene.waterdef_set_have[0]) {
      std::memcpy(cb + 164, scene.waterdef_set_rows[0], 56 * sizeof(float));
    } else if (g_r.waterdef_rows_have) {
      std::memcpy(cb + 164, g_r.waterdef_rows_last, 56 * sizeof(float));
    }
    for (uint32_t k = 0; k < 8; ++k) {
      if (scene.waterdef_set_have[k]) {
        std::memcpy(cb + 256 + k * 56, scene.waterdef_set_rows[k],
                    56 * sizeof(float));
      }
    }
    // wd_screen at cb[220]: 1 / render-target size, so the water branch can
    // project the refraction fetch from SV_Position.
    cb[220] = context.guest_output_width > 0
                  ? 1.0f / float(context.guest_output_width)
                  : 0.0f;
    cb[221] = context.guest_output_height > 0
                  ? 1.0f / float(context.guest_output_height)
                  : 0.0f;
    cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
    // b2 (character lighting) default: point at the ring base so the root
    // CBV is never left unset; character draws re-point it per item.
    cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    // t6/t7 ride one table: default = white cube + this frame's atlas
    // (draw_item re-pairs with the item's real cube when one resolves).
    cmd->SetTexturePair(7, g_r.white_cube.srv,
                        shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
    // v2 material table default (t8/t9 white) + the static sun-shadow map
    // at its third entry (t10); every lit branch samples it, so the table
    // must be bound for all draws, not only the v2/ocean re-binds.
    nrhi::TextureView* t8_default[4] = {
        g_r.white.srv, g_r.white.srv,
        g_r.static_sun_valid ? g_r.static_sun_srv : g_r.white.srv,
        // t11 deck decal; white = a no-op modulate, exactly like the
        // guest texture's own white border.
        g_r.deck_decal_valid ? g_r.deck_decal.srv : g_r.white.srv};
    cmd->SetTextures(8, t8_default, 4);
  }
  nrhi::TextureView* const nsm_view =
      g_r.static_sun_valid ? g_r.static_sun_srv : g_r.white.srv;
  // Character draws read the game's world-shade map at t10 instead (see
  // CharStaticShade / dyn_misc.z).
  nrhi::TextureView* const char_t10_view =
      WorldShadeReady(scene) ? g_r.world_shadow_srv : nsm_view;
  // What t10 currently holds for the draws below: the static sun map (every
  // non-character branch reads it) or the world-shade map (characters).
  nrhi::TextureView* t10_bound = nsm_view;
  nrhi::TextureView* const deck_view =
      g_r.deck_decal_valid ? g_r.deck_decal.srv : g_r.white.srv;
  // After a mid-pass FlushSubmission (one-pass VR stereo, below): the fresh
  // command list carries no bindings, so re-establish exactly what the pass
  // set up once above; draw_item binds everything else per item.
  const auto restore_main_pass = [&]() {
    cmd->SetRenderTargets(scene_color, use_depth ? g_r.depth : nullptr);
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetPipeline(scene_pso_bound);
    if (g_r.shadow_cb != nullptr) {
      cmd->SetConstantBuffer(6, g_r.shadow_cb, main_cb_offset);
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      cmd->SetBufferSrv(3, g_r.bone_ring, bone_region);
      cmd->SetTexturePair(7, g_r.white_cube.srv,
                          shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
      nrhi::TextureView* t8[4] = {g_r.white.srv, g_r.white.srv, t10_bound, deck_view};
      cmd->SetTextures(8, t8, 4);
    }
  };

  uint32_t drawn = 0;
  uint32_t item_index = 0;
  // F7 scene-composition ring (RequestSceneRingDump): one compact signature
  // per scene item per frame, ~900 frames deep. A 1-2 frame artifact (the
  // dam-bank blue flash) is uncapturable by F10/F11; the ring lets a
  // keypress seconds later name exactly which item appeared / vanished /
  // changed textures on the artifact frame. Entries are recorded at
  // classification below; draw_item stamps the issued-draw count, so an
  // item that early-returned (deferred mesh, skip-new, fade skip) shows
  // drawn=0 that frame.
  SceneRingFrame* ring_frame = nullptr;
  std::unordered_map<const DrawItem*, uint32_t> ring_map;
  if (!photo_mode && REXCVAR_GET(skate3_native_render_scene_ring) && debug_mode == 0) {
    g_scene_ring.emplace_back();
    while (g_scene_ring.size() > kSceneRingFrames) {
      g_scene_ring.pop_front();
    }
    ring_frame = &g_scene_ring.back();
    ring_frame->frame = frame_number;
    std::memcpy(ring_frame->cam, scene.cam_pos, sizeof(ring_frame->cam));
    ring_frame->fog[0] = scene.fog_ramp[0];
    ring_frame->fog[1] = scene.fog_ramp[1];
    ring_frame->fog[2] = scene.fog_ramp[2];
    ring_frame->fog[3] = scene.fog_color[0];
    ring_frame->fog[4] = scene.fog_color[1];
    ring_frame->fog[5] = scene.fog_color[2];
    std::memcpy(ring_frame->family_rows, scene.family_rows,
                sizeof(ring_frame->family_rows));
    ring_frame->sky_height = scene.sky_height;
    ring_frame->shadow_valid = scene.shadow_valid;
    ring_frame->shadow_ready = shadow_ready;
    ring_frame->shadow_draws = uint16_t(std::min<uint32_t>(shadow_draws, 0xFFFFu));
    ring_frame->static_sun_valid = g_r.static_sun_valid;
    std::memcpy(ring_frame->shadow_rows, scene.shadow_rows,
                sizeof(ring_frame->shadow_rows));
    ring_frame->items.reserve(scene.items.size());
    ring_map.reserve(scene.items.size());
  }
  // Inline decode budget for DYNAMIC payloads only (skinned/cloth/ropa
  // buffers that change every frame). Static meshes and all textures route
  // to the decode workers on miss; the render thread never pays their
  // decode cost.
  int32_t mesh_budget = REXCVAR_GET(skate3_native_render_scene_mesh_decode_budget);
  if (settling) {
    // Post-takeover settle window: cap inline dynamic decodes too, so the
    // takeover frame never absorbs an unbounded decode burst.
    if (mesh_budget == 0 || mesh_budget > 16) mesh_budget = 16;
  }
  uint32_t mesh_decodes = 0;
  // Item drawing body. environment.decal items draw in the same opaque pass:
  // they ARE the wall/ground sections they cover, with the art composited
  // in-shader (alpha-blending them as separate overlay geometry punched
  // holes in the world).
  // Ripple scroll clock for the water branch (seconds; wraps every hour to
  // keep float precision on the scrolled UVs).
  static const std::chrono::steady_clock::time_point water_t0 =
      std::chrono::steady_clock::now();
  const float water_time = float(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() -
      std::floor(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() /
          3600.0) *
          3600.0);

  // Words-keyed texture lookup with payload revalidation, shared by the
  // streamed-artwork diffuse override and the 2D pass. The event-ad system
  // rotates artwork IN PLACE; it streams the next poster into the same
  // guest buffer, so the fetch words (this cache's key) never change; without
  // the fingerprint recheck every frame keeps the first ad ever decoded at
  // that address and the wall posters diverge from the emulated frame.
  const auto find_words_texture = [&](uint64_t key) {
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid &&
        !inter_frame && frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      // 2-frame cadence in menu/editor contexts, like the item-draw poll
      // (in-place CAS composite rewrites during edits).
      it->second.recheck_frame =
          frame_number +
          (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      const bool fp_new = fp != 0 && fp != it->second.payload_fp;
      if (it->second.recheck_count < 3) {
        ++it->second.recheck_count;
      }
      if (it->second.incomplete || fp_new) {
        // In-place content rotation (event ads stream the next poster into
        // the same buffer; the words, and so the key, never change): heal
        // on the workers; keep serving the current decode meanwhile.
        EnqueueWordsMiss(key, it->second.fetch_words);
      }
    }
    return it;
  };
  // Resolve six raw fetch-constant words (a draw-time streamed-artwork
  // binding with no guest texture object) through the words-keyed cache.
  // `site` identifies the consuming slot (mesh << 1 | slot) for the sticky
  // fallback: streaming rebinds NEW mip words as you approach the art (the
  // cache key changes wholesale), and dropping to the placeholder / channel
  // diffuse for the worker round trip was the visible poster/decal flash;
  // the site's previous art keeps serving until the new decode lands.
  // Returns null only when nothing ever decoded for this site.
  const auto resolve_fetch_words = [&](const uint32_t words[6], uint64_t site,
                                       bool retained) -> const GuestTexture* {
    const uint64_t fkey = FetchWordsKey(words);
    auto fit = find_words_texture(fkey);
    if (fit == g_r.tex_store.end()) {
      if (!retained) {
        EnqueueWordsMiss(fkey, words);
      }
    } else if (fit->second.valid) {
      g_r.words_sticky[site] = fkey;
      return &fit->second;
    } else if (!retained && frame_number >= fit->second.retry_after_frame) {
      // Failed words decode (payload was mid-stream at first sight): keep
      // retrying; without this the entry negative-cached until the words
      // changed again.
      EnqueueWordsMiss(fkey, words);
      fit->second.retry_after_frame = frame_number + 30;
    }
    const auto pit = g_r.words_sticky.find(site);
    if (pit != g_r.words_sticky.end() && pit->second != fkey) {
      const auto old = g_r.tex_store.find(pit->second);
      if (old != g_r.tex_store.end() && old->second.valid) {
        old->second.last_used_frame = frame_number;
        g_ad_stale_served.fetch_add(1, std::memory_order_relaxed);
        return &old->second;
      }
    }
    // Nothing decoded for this site yet: the caller falls back to the
    // channel diffuse (the baked placeholder poster). If the "decal flash
    // and replace" report survives the dense-probe fix, a climbing none=
    // with a capped log here names this path as the flasher.
    g_ad_placeholder.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_ad_logs{0};
    if (!retained && s_ad_logs.fetch_add(1) < 24) {
      REXLOG_INFO(
          "native-scene: ad/decal words site={:X} fkey={:016X} -> placeholder "
          "(no decode yet) words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
          site, fkey, words[0], words[1], words[2], words[3], words[4],
          words[5]);
    }
    return nullptr;
  };

  // ---- SSR reflective-item capture (see ApplySsrPass) ----
  // draw_item records each reflective item's fully-staged draw state
  // (constants + resolved texture views) so the reflection G-buffer pass
  // after the scene pass re-renders exactly what the main pass drew.
  struct SsrGbufItem {
    float constants[52];
    nrhi::TextureView* t3;  // macro slot (water ripple normal map)
    nrhi::TextureView* t4;  // spec/reflection masks
    nrhi::TextureView* t5;  // fam 5/6 normal map (white when unpaired)
    uint32_t mesh;
    uint64_t fingerprint;
    const DrawItem* item;
  };
  std::vector<SsrGbufItem> ssr_items;
  // Game stencil port (RenderStencilMark): items whose recorded draw
  // REPLACEs a non-zero stencil byte, with the exact root constants and
  // bone palette they were drawn with (bit-identical depth on re-draw).
  struct StencilMarkItem {
    float constants[52];
    uint32_t bone_offset;  // bone ring SRV offset (0 = unskinned binding)
    VbBinding vb;          // the main draw's exact streams (ROPA: ring)
    IbBinding ib;
    const DrawItem* item;
  };
  std::vector<StencilMarkItem> stencil_items;
  // The foreground preview object's draws (see RendererState::fg_mark), with
  // the diffuse an alpha-tested piece's mark must clip by (nullptr = solid).
  struct FgMarkItem {
    StencilMarkItem sm;
    nrhi::TextureView* clip_srv;
  };
  std::vector<FgMarkItem> fg_mark_items;
  const bool stencil_mark_on = !photo_mode && scene.photo_fx.valid &&
                               use_depth && g_r.pso_stencil_mark != nullptr;
  const bool ssr_on =
      hdr_on && use_depth && debug_mode == 0 && !loading_native &&
      !g_r.ssr_failed && REXCVAR_GET(skate3_native_render_scene_ssr) && !vr_stereo &&
      // The published projection must be the live perspective matrix (same
      // gate as the SSAO pass).
      scene.proj[11] == 1.0f && scene.proj[0] != 0.0f &&
      scene.proj[5] != 0.0f && scene.proj[14] != 0.0f;

  // Deep per-item profiling (perf-items log line): stage timestamps inside
  // draw_item plus an in/out-of-frustum split of each item's total draw
  // cost. Sampled once per RenderScene call; the per-item clock reads only
  // happen while the cvar is on.
  const bool prof_items = REXCVAR_GET(skate3_native_render_scene_perf_items);
  // Set around draw_item calls that must use one specific opaque pipeline
  // (the foreground object's two passes; see DrawItem::foreground).
  nrhi::Pipeline* forced_pso = nullptr;
  // True while the sorted blended sub-pass draws: that pass owns the
  // pipeline (blend_bound) and draw_item must never bind the opaque/cull
  // choice under it. Character glass (char_family 7) and character.alpha
  // pieces are routed there without item.transparent, took the opaque
  // branch, and left an opaque pipeline bound that the next items (the
  // particles sorted after a car window) inherited: solid, unblended
  // sprite squares, depending on the sort order.
  bool blend_pass_active = false;

  const auto draw_item = [&](const DrawItem& item) {
    // Stage split points (profiling only): t0..t1 mesh serve, t1..t2 texture
    // serve, t2..t3 constants assembly, t3..end binds + draw recording.
    // Early returns skip the stage windows; the caller's in/out totals still
    // capture them.
    PerfClock::time_point di_t0{}, di_t1{}, di_t2{}, di_t3{};
    if (prof_items) {
      di_t0 = PerfClock::now();
    }
    // NO per-frame inline decodes here. Static content (world geometry,
    // props) loads/heals on the decode workers via the miss queue; a
    // texture decode averages ~10 ms and panning surfaces dozens of new
    // payloads in one frame; inline decode WAS the panning lag spike.
    // Dynamic payloads (skinned, CPU-rewritten every frame) are kept
    // fresh by the dyn decode jobs (guest-thread snapshot -> worker), one
    // frame behind the sim; only their FIRST sight decodes inline. The
    // cloth-quads particle path (gated off by default) still re-decodes
    // inline on change; it has no job route.
    // EXCEPTION: ROPA garments decode INLINE (skate3_native_render_scene_
    // ropa_inline): the worker route put the GPU-resident cloth shape 1-2
    // frames behind the body, visible as jelly/clip-through-torso during
    // direction changes (the shape only pauses when motion is steady). A
    // garment decode is sub-millisecond (a few hundred verts; it was
    // lumped in with the expensive texture decodes in the perf overhaul),
    // and the game's ping-pong double buffer makes the LIVE read tear-safe
    // (the sim writes the other half).
    const bool ropa_inline =
        item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline);
    const bool dynamic_payload = item.skinned || item.cloth_quads || item.ropa;
    auto it = g_r.meshes.find(item.mesh);
    if (item.retained &&
        (it == g_r.meshes.end() || it->second.fingerprint != item.fingerprint)) {
      // Retained off-screen items (edge-of-view guard band) outlive their
      // guest-side lifetime guarantees; the arena may have streamed out or
      // been reused since capture. Draw only the exact cached decode; no
      // guest reads, no heals, no miss enqueues from here.
      return;
    }
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint &&
        REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
      if (item.cloth_quads || ropa_inline) {
        PoolMeshBuffer(g_r.device, it->second.vb);
        PoolMeshBuffer(g_r.device, it->second.ib);
        g_r.meshes.erase(it);
        it = g_r.meshes.end();
      } else if (!dynamic_payload) {
        // Streaming heal: keep drawing the old decode this frame; the
        // workers decode the new payload and the commit swaps it in.
        EnqueueMeshMiss(item.mesh);
      }
    }
    // Foreign decode: the cached geometry belongs to the previous occupant
    // of this mesh address (create-a-skater garment swap), or a worker
    // result decoded before the swap landed after it. Drawing it pairs the
    // old vertices with the new piece's index ranges and bones (the
    // one-frame malformed garment). Drop it and decode the new piece NOW
    // (a garment decode is sub-millisecond), never a frame late.
    bool layout_changed = false;
    // Portrait jobs render once and the import-skater screen recycles the
    // same mesh objects for every skater it photographs: a cached decode of
    // another payload (fingerprint) is dropped and the piece decoded now,
    // never deferred to the workers (a deferred piece is simply missing).
    if (g_photo_mode && it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint) {
      PoolMeshBuffer(g_r.device, it->second.vb);
      PoolMeshBuffer(g_r.device, it->second.ib);
      g_r.meshes.erase(it);
      it = g_r.meshes.end();
      g_r.ropa_shapes.erase(item.mesh);
      layout_changed = true;
    }
    if (it != g_r.meshes.end() && dynamic_payload && it->second.layout != 0 &&
        ForeignLayout(item.mesh, it->second.layout, MeshLayoutKey(item))) {
      PoolMeshBuffer(g_r.device, it->second.vb);
      PoolMeshBuffer(g_r.device, it->second.ib);
      g_r.meshes.erase(it);
      it = g_r.meshes.end();
      g_r.ropa_shapes.erase(item.mesh);
      layout_changed = true;
    }
    if (it == g_r.meshes.end()) {
      if (!item.cloth_quads && !ropa_inline && !layout_changed && !g_photo_mode) {
        // ALL mesh misses decode on the workers, including first-sight
        // skinned entities (a streamed-in NPC appears 1-2 frames late
        // instead of hitching the frame; the dyn decode jobs usually land
        // the same content even sooner from the guest-thread snapshot).
        EnqueueMeshMiss(item.mesh);
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (!layout_changed && !g_photo_mode && mesh_budget > 0 &&
          mesh_decodes >= uint32_t(mesh_budget)) {
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;  // decodes on a later frame
      }
      ++mesh_decodes;
      const auto decode_t0 = PerfClock::now();
      MeshBuffers buffers;
      const bool decode_ok = DecodeMesh(g_r.device, base, item, buffers);
      g_pw_mesh_decode.Add(perf_ns_since(decode_t0));
      if (!decode_ok) {
        g_rr_decode_fail.fetch_add(1, std::memory_order_relaxed);
        static std::unordered_set<uint32_t> logged;
        if (logged.size() < 32 && logged.insert(item.mesh).second) {
          REXLOG_WARN("native-scene: DecodeMesh FAILED mesh={:08X} vb={:08X} fmt={} stride={}",
                      item.mesh, item.vb_addr, item.pos_fmt, item.stride);
        }
        return;
      }
      buffers.fingerprint = item.fingerprint;
      it = g_r.meshes.emplace(item.mesh, buffers).first;
    }
    it->second.last_used_frame = frame_number;
    const MeshBuffers& buffers = it->second;
    if (prof_items) {
      di_t1 = PerfClock::now();
    }

    // Double-sided sheet props draw with backface culling; without it the
    // front/back copies z-fight into lightmap flicker at range (banners/
    // flags). Opaque depth pass only; a mirrored instance (negative world
    // determinant) would flip winding, so those stay uncull(ed).
    // (hair items with a validated lighting capture draw in the blended
    // sub-pass under their own cull PSOs; never reset those here)
    const bool hair_pass = item.char_family >= 4 && item.char_family <= 5 &&
                           item.char_rows[14 * 4 + 1] > 0.0f;
    // reflective_trans glass draws in the blended sub-pass whenever its
    // exact branch is live (same gate as the sub-pass routing below); the
    // opaque cull-PSO reset must not fire there.
    const bool refl_trans_pass =
        item.env_family == 13 && debug_mode == 0 && scene.shadow_valid;
    // Character items mid-fade (spawn settle / distance) draw in the blended
    // sub-pass too (same gate as the routing below), same exemption.
    const bool char_fade_pass =
        (item.char_family == 1 || item.char_family == 2 ||
         item.char_family == 3 || item.char_family == 6) &&
        debug_mode == 0 && CharFadeAlphaDrawn(scene, item) < 0.999f &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    // Fam 16 draws in the opaque pass (see transenv_opaque in the routing), so
    // it takes the per-item opaque pipeline choice here too.
    const int transenv_mode =
        item.env_family == 16 ? GameBlendMode(scene, item) : 0;
    const bool transenv_pass = item.env_family == 16 && item.transparent &&
                               debug_mode == 0 && scene.shadow_valid &&
                               scene.ts_valid &&
                               (transenv_mode == 0 || transenv_mode == 1);
    // The Hall of Meat x-ray draws in its own final pass, which binds the
    // pipeline and its own depth buffer itself (see hom_items).
    const bool hom_pass = item.hom && item.hom_seen && debug_mode == 0;
    bool scene_pso_path = false;
    if (use_depth && (!item.transparent || transenv_pass) && !item.water &&
        !hair_pass && !refl_trans_pass && !char_fade_pass && !hom_pass &&
        !blend_pass_active && g_r.pso_cullback != nullptr) {
      const float* w = item.world;
      const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                         w[1] * (w[4] * w[10] - w[6] * w[8]) +
                         w[2] * (w[4] * w[9] - w[5] * w[8]);
      // Game-parity backface culling: every world environment material's
      // XML sets CULLMODE=FRONT (== our CULL_FRONT; the banner work
      // calibrated game-kept faces as our D3D12 BACK faces). CULL_NONE
      // showed interior/away faces the game never renders, e.g. the
      // building wall's inside face stacking behind the translucent canopy
      // glass. Trees/alphatest (fams 7/9/10: leaf/fence cards read from
      // both sides) and mirrored instances (flipped winding) stay
      // uncull(ed), matching the two-sided-sheet rules.
      const bool cull_family =
          REXCVAR_GET(skate3_native_render_scene_backface_cull) &&
          item.env_family != 0 && item.env_family != 7 &&
          item.env_family != 9 && item.env_family != 10 &&
          item.env_family != 13 &&
          // fam 16 cut-out cards (wheels, fences) are thin two-sided sheets
          item.env_family != 16 &&
          // fam 19 flag cloth: one sheet seen from both sides
          item.env_family != 19;
      // Guest wireframe (debug Render -> Settings -> Wireframe) overrides
      // the cull variants: with line rasterization there are no filled faces
      // to hide. It is a PER-DRAW state, not a global one -- the game keeps
      // its shadow/z-prepass/postfx draws filled while the main-pass
      // material draws go to lines. forced_pso still wins: the editor's
      // x-ray/preview passes and the wireframe depth prepass all carry their
      // own depth and colour-mask rules.
      // environment.transparent (fam 16) is excluded from the material-
      // wide rule (its wheel/fence cut-out cards are two-sided), but some
      // of its meshes are authored as back-to-back sheet pairs that rely on
      // the draw's cull state (the flower bushes: lit and shaded copies of
      // every card, which uncull(ed) z-fight into flicker). Follow the
      // game's recorded per-draw cull register for this family.
      // Fam 19 (animated.flag) follows the recorded register the same way.
      const int game_cull_mode =
          ((item.env_family == 16 || item.env_family == 19) && g_r.pso_cullfront != nullptr)
              ? GameCullMode(scene, item)
              : 0;
      if (item.env_family == 16 || item.env_family == 19) {
        // First sightings only: which cull state the game records for
        // environment.transparent draws (0x18004 = none, 0x18005/6 = cull).
        static std::atomic<uint32_t> s_te_cull_log{0};
        if (s_te_cull_log.load(std::memory_order_relaxed) < 24 &&
            s_te_cull_log.fetch_add(1, std::memory_order_relaxed) < 24) {
          DrawRegState rs_log;
          const bool have = ItemDrawRs(scene, item, rs_log);
          REXLOG_INFO(
              "native-scene: transenv cull mesh={:08X} rs={} own={} "
              "mode_cntl={:08X} -> {}",
              item.mesh, have ? 1 : 0, item.game_rs_seen ? 1 : 0,
              have ? rs_log.mode_cntl : 0u, game_cull_mode);
        }
      }
      if (item.env_family == 19) {
        // DIAGNOSTIC (animated.flag cull): the game's recorded state per mesh.
        static std::unordered_set<uint32_t> s_flag_cull_logged;
        if (s_flag_cull_logged.size() < 32 && s_flag_cull_logged.insert(item.mesh).second) {
          DrawRegState rs_log;
          const bool have = ItemDrawRs(scene, item, rs_log);
          REXLOG_INFO(
              "native-scene: flag cull mesh={:08X} rs={} own={} mode_cntl={:08X} "
              "det3={:.3g} sheet={} -> game_cull={}",
              item.mesh, have ? 1 : 0, item.game_rs_seen ? 1 : 0, have ? rs_log.mode_cntl : 0u,
              det3, buffers.two_sided_sheet ? 1 : 0, game_cull_mode);
        }
      }
      // Fam 19 (animated.flag): a RECORDED cull-off register (captured
      // mode_cntl 0x18004 on every flag draw) is an explicit "draw both
      // sides"; GameCullMode's 0 also means "unrecorded", which fell through
      // to the two-sided-sheet rule below and culled the cloth's far side
      // (the flags drew as slivers next to the pole).
      bool game_no_cull = false;
      if (item.env_family == 19 && game_cull_mode == 0) {
        DrawRegState rs_nc;
        game_no_cull = ItemDrawRs(scene, item, rs_nc) && rs_nc.mode_cntl != 0 &&
                       (rs_nc.mode_cntl & 3u) == 0;
      }
      nrhi::Pipeline* const game_cull =
          game_cull_mode == 1   ? g_r.pso_cullback
          : game_cull_mode == 2 ? g_r.pso_cullfront
          : game_no_cull        ? g_r.pso
                                : nullptr;
      nrhi::Pipeline* want =
          forced_pso != nullptr ? forced_pso
          : (g_r.pso_wireframe != nullptr && GameWireframeItem(scene, item))
              ? g_r.pso_wireframe
          : game_cull != nullptr ? game_cull
          : ((buffers.two_sided_sheet || cull_family) && det3 >= 0.0f)
              ? g_r.pso_cullback
              : g_r.pso;
      if (want != scene_pso_bound) {
        cmd->SetPipeline(want);
        scene_pso_bound = want;
      }
      scene_pso_path = true;
    }

    // Resolve guest textures (white fallback) through the words-keyed
    // content store: object -> stable live words -> store entry
    // (console identity semantics; a
    // retargeted or reused object is just a different key, never a stale
    // serve). The object addresses were readable on the game thread this
    // frame; NO VirtualQuery here; it takes the process VAD lock, which
    // the guest streaming threads hammer, and ~2 calls per item stalled
    // the whole renderer to 3 fps.
    // Set by resolve_texture_raw when it returns the white fallback while a
    // decode is in flight (first-sight miss or a still-failing heal); the
    // sticky wrapper below then serves the item's last-good texture instead.
    bool tex_pending = false;
    // Inline character-texture decode gate (menus/editor only, <= 6 per
    // frame; everything else keeps the async worker path).
    const auto char_tex_inline_ok = [&]() -> bool {
      // Portrait jobs render once: every miss decodes now (an async miss
      // means the piece is skipped; the import-skater screen's skaters are
      // all first-sight art, so most of their garments never drew).
      if (g_photo_mode) {
        return !item.retained;
      }
      if (item.char_family == 0 || item.retained ||
          !g_in_menus_frame.load(std::memory_order_relaxed)) {
        return false;
      }
      static uint64_t s_inline_frame = ~0ull;
      static uint32_t s_inline_count = 0;
      if (s_inline_frame != frame_number) {
        s_inline_frame = frame_number;
        s_inline_count = 0;
      }
      if (s_inline_count >= 6) {
        return false;
      }
      ++s_inline_count;
      return true;
    };
    const auto resolve_texture_raw = [&](uint32_t tex_ptr) -> const GuestTexture* {
      if (tex_ptr == 0) {
        return &g_r.white;
      }
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      RendererState::TexRoute* rit = g_r.tex_routes.find(tex_ptr);
      if (!item.retained && !inter_frame) {
        // Route refresh: seqlock-stable read of the live fetch words (a
        // mid-rewrite mixed snapshot must never become a key; it would
        // decode a coherent image of the WRONG memory, the pool-page
        // collage class). An unstable read keeps the previous route for a
        // frame. Retained items skip all live reads: the object may be
        // gone; their route is frozen at retention. Inter-frames re-render
        // the scene of the last guest swap, whose routes the real present
        // just refreshed; the guest is already mid-way through its next
        // frame, so a live read there is a cache miss per texture slot per
        // present for state that belongs to the next scene.
        uint32_t live[6];
        if (ReadStableTexWords(base, tex_ptr, live)) {
          const bool demoted = (live[1] & 0xFFFFF000u) == 0u;
          if (rit == nullptr) {
            rit = g_r.tex_routes.try_emplace(tex_ptr).first;
            std::memcpy(rit->words, live, sizeof(live));
            rit->key = FetchWordsKey(live);
            rit->demoted = demoted;
          } else if (demoted) {
            // Mip-0 demoted (base address cleared; the old pool range is
            // already reused): hold the pre-demote route; its decode
            // carries the full chain, strictly better, and suspend its
            // payload polls (the probes would read the reused pool and
            // heal in foreign bytes). A re-promote publishes fresh words
            // and re-routes.
            if (!rit->demoted) {
              rit->demoted = true;
              g_demote_hold.fetch_add(1, std::memory_order_relaxed);
              if (trm) {
                REXLOG_INFO("tex-trace: f{} obj={:08X} DEMOTED (route held, "
                            "polls suspended)",
                            frame_number, tex_ptr);
              }
            }
          } else if (std::memcmp(live, rit->words, sizeof(live)) != 0) {
            if (trm) {
              REXLOG_INFO(
                  "tex-trace: f{} obj={:08X} REROUTE old=[{:08X} {:08X} "
                  "{:08X} {:08X} {:08X} {:08X}] new=[{:08X} {:08X} {:08X} "
                  "{:08X} {:08X} {:08X}]",
                  frame_number, tex_ptr, rit->words[0],
                  rit->words[1], rit->words[2],
                  rit->words[3], rit->words[4],
                  rit->words[5], live[0], live[1], live[2], live[3],
                  live[4], live[5]);
            }
            std::memcpy(rit->words, live, sizeof(live));
            rit->key = FetchWordsKey(live);
            rit->demoted = false;
          } else {
            rit->demoted = false;
          }
        } else if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} words UNSTABLE (mid-rewrite)",
                      frame_number, tex_ptr);
        }
      }
      if (rit == nullptr) {
        // Unreadable/unstable object with no prior route: nothing safe to
        // serve or decode yet; next frame's read settles it.
        tex_pending = true;
        return &g_r.white;
      }
      const RendererState::TexRoute& route = *rit;
      auto sit = g_r.tex_store.find(route.key);
      if (sit == g_r.tex_store.end()) {
        if (item.retained || route.demoted) {
          // Retained: no enqueues, no live reads. Demote-held with nothing
          // cached: the pre-demote pool may already be reused; a decode
          // would commit foreign bytes under a good key. White/sticky until
          // a re-promote publishes live words.
          tex_pending = true;
          return &g_r.white;
        }
        // Decode on the workers; white/sticky for the 1-3 frames that takes
        // (inline decode measured ~10 ms avg / ~70 ms max, the panning
        // lag spikes).
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} MISS key={:016X} enqueued",
                      frame_number, tex_ptr, route.key);
        }
        // Menus/editor: character textures decode INLINE (bounded per
        // frame). Create-a-skater swaps every piece's art on an outfit
        // change; the worker round trip showed stale/white art for a moment
        // on each piece, desynchronized. Gameplay keeps the async path.
        if (char_tex_inline_ok()) {
          GuestTexture gt;
          if (EnsureGuestTextureFromWords(context, base, route.words, gt) && gt.valid) {
            gt.last_used_frame = frame_number;
            gt.last_change_frame = frame_number;
            return &g_r.tex_store.emplace(route.key, gt).first->second;
          }
          if (gt.texture) g_r.device->DestroyDeferred(gt.texture);
          if (gt.upload) g_r.device->DestroyDeferred(gt.upload);
        }
        EnqueueWordsMiss(route.key, route.words);
        g_rr_tex_deferred.fetch_add(1, std::memory_order_relaxed);
        tex_pending = true;
        return &g_r.white;
      }
      GuestTexture& e = sit->second;
      e.last_used_frame = frame_number;
      if (!e.valid) {
        // Failed decode: retry on its backoff clock (the payload usually
        // lands within a few frames of first sight; the commit stamps the
        // real backoff).
        if (!item.retained && !route.demoted &&
            frame_number >= e.retry_after_frame) {
          e.retry_after_frame = frame_number + 120;
          EnqueueWordsMiss(route.key, route.words);
        }
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} INVALID entry key={:016X} "
                      "(retry at f{})",
                      frame_number, tex_ptr, route.key, e.retry_after_frame);
        }
        tex_pending = true;
        return &g_r.white;
      }
      // Payload revalidation, the one irreducible heuristic: the game
      // streams content IN PLACE at addresses the words already point to
      // (event-ad rotation, mip-pool fills, composed lightmap pages) with
      // no CPU-visible event. Escalating cadence (2/4/8 then 16): a decode
      // taken while the payload was still streaming reads back garbage;
      // fresh entries re-verify fast, then settle to the cheap steady
      // cadence. An incomplete decode (truncated tiled-mip copy) and a
      // near_black verdict re-decode regardless of the mip-0 fingerprint;
      // the commit's same-content dedup absorbs the no-ops. Suspended while
      // the route is demote-held (the probes would read the reused pool).
      if (!item.retained && !route.demoted &&
          !inter_frame && frame_number >= e.recheck_frame &&
          REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        // Menu/editor contexts poll STEADY entries on a 2-frame cadence
        // (mirrors the 2D resolver): the CAS editor recomposes the skater's
        // skin/garment textures IN PLACE progressively over ~a second on
        // every edit, and the 16-frame steady cadence made each composite
        // step land ~100+ ms late, desynchronized across the pieces, the
        // "broken for 1-2 s after changing skin color" state. Gameplay
        // keeps the cheap steady cadence.
        e.recheck_frame =
            frame_number +
            (e.recheck_count < 3
                 ? (2ull << e.recheck_count)
                 : (g_in_menus_frame.load(std::memory_order_relaxed) ? 2ull
                                                                     : 16ull));
        const uint64_t fp = SampleProbeFingerprint(base, e);
        const bool fp_new = fp != 0 && fp != e.payload_fp;
        if (trm) {
          REXLOG_INFO(
              "tex-trace: f{} obj={:08X} poll key={:016X} fp={:016X} "
              "cached={:016X} new={} inc={} nb={} cnt={}",
              frame_number, tex_ptr, route.key, fp, e.payload_fp,
              fp_new ? 1 : 0, e.incomplete ? 1 : 0, e.near_black ? 1 : 0,
              e.recheck_count);
        }
        if (e.recheck_count < 3) {
          ++e.recheck_count;
        }
        if (fp_new || e.incomplete || (e.near_black && e.nb_redecodes < 3)) {
          // Re-decode churn diagnostic: repeated in-place payload changes
          // on one key = streaming oscillation, visible as texture flicker
          // on the affected meshes.
          static std::atomic<uint32_t> s_redecode_logs{0};
          if (s_redecode_logs.fetch_add(1) < 256) {
            REXLOG_INFO(
                "native-scene: texture re-decode key={:016X} reason={}{}{} "
                "words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                route.key, fp_new ? "fp" : "", e.incomplete ? "inc" : "",
                e.near_black ? "nb" : "", e.fetch_words[0], e.fetch_words[1],
                e.fetch_words[2], e.fetch_words[3], e.fetch_words[4],
                e.fetch_words[5]);
          }
          if (trm) {
            REXLOG_INFO("tex-trace: f{} obj={:08X} ENQUEUE heal key={:016X} "
                        "reason={}{}{}",
                        frame_number, tex_ptr, route.key, fp_new ? "fp" : "",
                        e.incomplete ? "inc" : "", e.near_black ? "nb" : "");
          }
          // Menus/editor: re-decode character art inline (see the miss path)
          // so an in-place recompose (skin/garment colour edit) lands this
          // frame instead of after the worker round trip. Mirrors the
          // worker commit: retire the old entry, keep its LRU stamp.
          if (fp_new && char_tex_inline_ok()) {
            GuestTexture gt;
            if (EnsureGuestTextureFromWords(context, base, e.fetch_words, gt) && gt.valid) {
              gt.last_used_frame = e.last_used_frame;
              gt.last_change_frame = frame_number;
              RetireGuestTexture(e, context.device->CurrentSubmission());
              e = gt;
              return &e;
            }
            if (gt.texture) g_r.device->DestroyDeferred(gt.texture);
            if (gt.upload) g_r.device->DestroyDeferred(gt.upload);
          }
          EnqueueWordsMiss(route.key, e.fetch_words);
        }
      }
      return &e;
    };
    // Sticky serving: streaming rotates content onto NEW texture objects (a
    // mip promote is usually a fresh object: 287 first-sight objects vs 73
    // in-place rebinds in one 45 s traversal), so a plain
    // cache miss white-flashed content that was on screen with the previous
    // mip one frame earlier. While the new object's decode is in flight,
    // serve the last texture this item slot successfully resolved, the
    // same visual as the console's own mip transition. slot: 0 diffuse,
    // 1 lightmap, 2 macro, 3 normal/ripple, 4 decal art, 5 hair, 6 spec.
    const auto resolve_texture = [&](uint32_t tex_ptr,
                                     uint32_t slot) -> const GuestTexture* {
      tex_pending = false;
      const GuestTexture* t = resolve_texture_raw(tex_ptr);
      // Near-uniform-black decodes on the WHITE-NEUTRAL slots (1 lightmap,
      // 2 macro) serve the white fallback until a heal lands real content.
      // Lightmap: a real-but-black page binds with tint.r > 0 and the CSM
      // min-clamp renders the surface BLACK (the door 59810af's shader gate
      // cannot see). Macro: since 59810af the weathering multiplies OVER
      // the composited decal art, so a mid-stream dark macro decode turns
      // the paint into the black-square flash; white is the macro's
      // authored neutral (materials without weathering bind default_white).
      // Slots with legitimately dark content (diffuse, decal art, spec)
      // are untouched. Applied in the wrapper so retained items get the
      // same protection.
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      // Only while UNCONFIRMED: a forced re-decode that comes back identical
      // counts toward "genuinely uniform content" (nb_redecodes, stamped at
      // commit). Materials legitimately author FLAT pages here: Industrial's
      // shipping containers tint a grey, blue-lettered diffuse through a
      // single-colour macro overlay (orange/teal/blue). Serving white for
      // those forever rendered every container grey with blue lettering,
      // while the console multiplies the tint in and the lettering reads dark.
      if ((slot == 1 || slot == 2) && t != &g_r.white && t->valid &&
          t->near_black && t->nb_redecodes < 3) {
        if (trm) {
          REXLOG_INFO("tex-trace: f{} slot={} obj={:08X} NEAR-BLACK -> white",
                      frame_number, slot, tex_ptr);
        }
        static std::atomic<uint32_t> s_nb_logs{0};
        if (s_nb_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: near-black decode obj={:08X} slot={} served as "
              "white fallback (mid-compose/stream content)",
              tex_ptr, slot);
        }
        return &g_r.white;
      }
      if (item.retained || tex_ptr == 0) {
        return t;
      }
      const uint64_t skey = (uint64_t(item.mesh) << 3) | slot;
      // The mesh this site serves (see TexStickySite::owner): its buffer
      // objects, payload addresses / sizes and material diffuse object.
      uint64_t owner = 1469598103934665603ull;
      for (const uint64_t v : {uint64_t(item.ib_obj), uint64_t(item.vb_obj),
                               uint64_t(item.ib_addr), uint64_t(item.vb_addr),
                               uint64_t(item.ib_count), uint64_t(item.vb_bytes),
                               uint64_t(item.diffuse_tex)}) {
        owner = (owner ^ v) * 1099511628211ull;
      }
      owner |= 1;
      if (t != &g_r.white) {
        RendererState::TexStickySite& site = g_r.tex_sticky[skey];
        if (site.owner != owner) {
          site = {};  // a different mesh now lives at this address
          site.owner = owner;
        }
        // Material-detail downgrade hold: the game's streaming demotes a
        // nearby mesh to its UN (undetailed) material for a fraction of a
        // second and back, the visible flash to completely different/
        // blurry art. Under words identity a demote is just a smaller-area
        // key; the site's last-adopted decode is still resident in the
        // store, so keep serving it while the downgrade is young. A
        // persistent downgrade (a real demote as the player leaves) adopts
        // after the hold window.
        const int32_t hold = REXCVAR_GET(skate3_native_render_scene_detail_hold);
        const uint64_t cur_key = FetchWordsKey(t->fetch_words);
        const uint64_t cur_area = FetchWordsArea(t->fetch_words);
        if (hold > 0 && site.area > cur_area && site.words_key != 0) {
          const auto hit = g_r.tex_store.find(site.words_key);
          const GuestTexture* held =
              hit != g_r.tex_store.end() && hit->second.valid ? &hit->second
                                                              : nullptr;
          if (held != nullptr) {
            // The held entry may no longer be polled by any live route;
            // re-probe its payload on its own recheck clock so a reused
            // pool page abandons the hold instead of serving foreign
            // bytes.
            hit->second.last_used_frame = frame_number;
            if (!inter_frame && frame_number >= hit->second.recheck_frame) {
              hit->second.recheck_frame = frame_number + 16;
              const uint64_t fp = SampleProbeFingerprint(base, hit->second);
              if (fp == 0 || fp != hit->second.payload_fp) {
                held = nullptr;
              }
            }
          }
          if (held != nullptr) {
            if (site.downgrade_since == 0) {
              site.downgrade_since = frame_number;
              static std::atomic<uint32_t> s_hold_logs{0};
              if (s_hold_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
                REXLOG_INFO(
                    "native-scene: detail-downgrade HOLD mesh={:08X} slot={} "
                    "held={:016X} bound={:016X} area {} -> {}",
                    item.mesh, slot, site.words_key, cur_key, site.area,
                    cur_area);
              }
            }
            if (frame_number - site.downgrade_since < uint64_t(hold)) {
              if (trm && ((frame_number - site.downgrade_since) & 63u) == 0) {
                REXLOG_INFO(
                    "tex-trace: f{} slot={} DETAIL-HOLD serving fp={:016X} "
                    "(bound area {} < {})",
                    frame_number, slot, held->payload_fp, cur_area,
                    site.area);
              }
              return held;
            }
          }
        }
        if (trm && site.words_key != 0 && site.words_key != cur_key) {
          REXLOG_INFO(
              "tex-trace: f{} slot={} ADOPT key {:016X}(area {}) -> "
              "{:016X}(area {})",
              frame_number, slot, site.words_key, site.area, cur_key,
              cur_area);
        }
        site.words_key = cur_key;
        site.area = cur_area;
        site.downgrade_since = 0;
        if (!g_r.tex_pending_first.empty()) {
          g_r.tex_pending_first.erase(tex_ptr);
        }
        return t;
      }
      if (tex_pending) {
        // The current binding's decode is in flight: serve the site's
        // previous art from the store, the console's own mip-transition
        // look instead of a white flash.
        const RendererState::TexStickySite* sit = g_r.tex_sticky.find(skey);
        if (sit != nullptr && sit->words_key != 0 && sit->owner == owner) {
          const auto old = g_r.tex_store.find(sit->words_key);
          if (old != g_r.tex_store.end() && old->second.valid) {
            old->second.last_used_frame = frame_number;
            g_tex_sticky_served.fetch_add(1, std::memory_order_relaxed);
            if (trm) {
              REXLOG_INFO("tex-trace: f{} slot={} STICKY serving key={:016X} "
                          "(pending {:08X})",
                          frame_number, slot, sit->words_key, tex_ptr);
            }
            return &old->second;
          }
        }
      }
      if (trm && t == &g_r.white) {
        REXLOG_INFO("tex-trace: f{} slot={} serving WHITE (req {:08X})",
                    frame_number, slot, tex_ptr);
      }
      return t;
    };
    // Streamed-artwork diffuse override (see DrawItem::diffuse_fetch): the
    // real art exists only as draw-time fetch words; resolve those through
    // the words-keyed cache (shared with the 2D pass; the art has no guest
    // object to key on).
    // Particles: the sticky site follows the sprite's own texture base, not
    // item.mesh (a particle ring offset, reused by whichever system lands
    // there, which served one system another system's sprite).
    const uint64_t diffuse_site =
        item.cloth_quads ? ((uint64_t(item.diffuse_fetch[1] & 0xFFFFF000u) << 1) | 0x800000000ull)
                         : uint64_t(item.mesh) << 1;
    // Particles, as lionREC_Render draws them: the sprite is whatever this
    // draw bound at tf0 (its fetch words), sampled that same frame. A sprite
    // seen for the first time decodes INLINE (small art, sub-ms) instead of
    // waiting on the workers, which drew it white (alpha 1) for a frame or
    // two.
    if (item.cloth_quads && item.diffuse_fetch[1] != 0 &&
        (item.diffuse_fetch[0] & 0x3u) == 2 && !item.retained) {
      const uint64_t pkey = FetchWordsKey(item.diffuse_fetch);
      if (g_r.tex_store.find(pkey) == g_r.tex_store.end()) {
        GuestTexture gt;
        EnsureGuestTextureFromWords(context, base, item.diffuse_fetch, gt);
        cmd->FlushBarriers();
        gt.last_used_frame = frame_number;
        gt.last_change_frame = frame_number;
        g_r.tex_store.emplace(pkey, gt);
      }
    }
    const GuestTexture* diffuse =
        item.diffuse_fetch[1] != 0
            ? resolve_fetch_words(item.diffuse_fetch, diffuse_site, item.retained)
            : nullptr;
    if (item.cloth_quads) {
      // One line per distinct particle sprite + blend (capped): format,
      // swizzle and size of what lionREC_RenderPS samples at tf0.
      static std::unordered_set<uint64_t> s_seen;
      const uint64_t pk = FetchWordsKey(item.diffuse_fetch) ^ (uint64_t(item.game_rs[1]) << 7);
      if (s_seen.size() < 64 && s_seen.insert(pk).second) {
        const uint32_t* f = item.diffuse_fetch;
        REXLOG_INFO(
            "native-scene: particle sprite fmt={} {}x{} swz={:03X} endian={} blend={:08X} "
            "tex={} words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
            f[1] & 0x3Fu, (f[2] & 0x1FFFu) + 1, ((f[2] >> 13) & 0x1FFFu) + 1, f[3] & 0xFFFu,
            (f[1] >> 6) & 3u, item.game_rs[1],
            diffuse == nullptr ? "none" : diffuse == &g_r.white ? "white"
                                 : diffuse->valid ? "ok" : "invalid",
            f[0], f[1], f[2], f[3], f[4], f[5]);
      }
    }
    // advertisement.default: the art the game bound at tf4 on this item's
    // own draw (DrawItem::ad_fetch), with that draw's base/mip addresses.
    if (diffuse == nullptr && item.env_family == 15 && item.ad_seen &&
        item.ad_fetch[1] != 0) {
      const GuestTexture* adt = resolve_fetch_words(
          item.ad_fetch, (uint64_t(item.mesh) << 1) | 0x400000000ull, item.retained);
      if (adt != nullptr && adt != &g_r.white && adt->valid) {
        diffuse = adt;
      }
    }
    if (diffuse == nullptr) {
      diffuse = resolve_texture(item.diffuse_tex, 0);
      if (diffuse == &g_r.white && tex_pending && item.diffuse_tex != 0 &&
          !item.retained) {
        // Brand-new content, first decode in flight, and no previous mip
        // cached to serve: draw NOTHING for a short window instead of a
        // white flash: the emulated pop-in semantics (the game shows
        // streamed content only once its data is ready). Bounded so a
        // permanently failing texture still falls back to visible white.
        const auto [pit, fresh] =
            g_r.tex_pending_first.try_emplace(item.diffuse_tex, frame_number);
        if (frame_number - pit->second < 20) {
          g_skip_new.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    }
    // flowingwateralpha's body colour is the game's tf4, sampled at the base
    // uv and squared (ucode 13/27/80). The material has no diffuse channel,
    // so dlin was white x lighting and the surface lost its colour entirely.
    // Feeding it through the diffuse slot lets the existing dlin carry it.
    // Each flowing-water item's own tf4 when captured (DrawItem::water_tf4):
    // the frame-global one is a single material's colour map.
    const uint32_t water_tf4 = item.water_rows_seen && item.water_tf4 != 0
                                   ? item.water_tf4
                                   : scene.water_tf4;
    if (item.water_flowing && water_tf4 != 0) {
      const GuestTexture* wc = resolve_texture(water_tf4, 0);
      if (wc != nullptr && wc != &g_r.white) {
        diffuse = wc;
      }
    }
    const GuestTexture* lightmap =
        item.lightmap_tex != 0 && REXCVAR_GET(skate3_native_render_scene_lightmaps)
            ? resolve_texture(item.lightmap_tex, 1)
            : nullptr;
    // advertisement.default: the lightmap the game bound at tf3 on this
    // item's own draw (DrawItem::ad_lm_fetch); the material need not carry
    // a lightmap channel, and the white fallback lit the board flat.
    if (item.env_family == 15 && item.ad_seen && item.ad_lm_fetch[1] != 0 &&
        REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
      const GuestTexture* alm = resolve_fetch_words(
          item.ad_lm_fetch, (uint64_t(item.mesh) << 1) | 0x800000000ull, item.retained);
      if (alm != nullptr && alm != &g_r.white && alm->valid) {
        lightmap = alm;
      }
    }
    if (lightmap == &g_r.white) {
      lightmap = nullptr;
    }
    // environmentpark diffuse / decal pieces (model 0): their tf1 is the
    // park's light cookie (see FrameScene::park_lm), not a per-mesh
    // lightmap; it rides the same slot and tint.r flag.
    if (lightmap == nullptr && (item.dynobj == 4 || item.dynobj == 6) &&
        scene.park_lm_valid && debug_mode == 0) {
      const GuestTexture* plm =
          resolve_fetch_words(scene.park_lm_fetch, 0x7061726B00000000ull, item.retained);
      if (plm != nullptr && plm->valid) {
        lightmap = plm;
      }
    }
    // CAS skin custom graphic (see DrawItem::char_skin): the skin draw's
    // own tf6 fetch words (the chosen tattoo) at the placed uv2. Characters
    // never carry a lightmap, so it rides the t1 slot; overlay.x = 1 tells
    // the PS. Needs this frame's c190/c191 placement rows.
    const GuestTexture* skin_decal = nullptr;
    // (The material's "decal" channel names the same texture in the F11
    // capture; it serves while the words decode is in flight.)
    if (item.char_skin && item.char_cg_valid && item.char_rows[14 * 4 + 1] > 0.0f &&
        debug_mode == 0) {
      if (item.char_cg_fetch[1] != 0) {
        skin_decal = resolve_fetch_words(item.char_cg_fetch,
                                         (uint64_t(item.mesh) << 1) | 1, item.retained);
      }
      if ((skin_decal == nullptr || !skin_decal->valid) && item.decal_art != 0) {
        skin_decal = resolve_texture(item.decal_art, 1);
        if (skin_decal == &g_r.white) {
          skin_decal = nullptr;
        }
      }
      if (skin_decal != nullptr && !skin_decal->valid) {
        skin_decal = nullptr;
      }
    }
    const GuestTexture* shift_graphic_tex = nullptr;
    if (item.char_shift && item.char_cg_valid && item.char_rows[14 * 4 + 1] > 0.0f &&
        debug_mode == 0 && REXCVAR_GET(skate3_native_render_scene_decals)) {
      shift_graphic_tex = resolve_fetch_words(
          item.char_cg_fetch, (uint64_t(item.mesh) << 1) ^ (5ull << 60), item.retained);
      if (shift_graphic_tex != nullptr &&
          (!shift_graphic_tex->valid || shift_graphic_tex->srv == nullptr)) {
        shift_graphic_tex = nullptr;
      }
    }
    const GuestTexture* cloth_stamp_tex = nullptr;
    if (item.char_cloth_stamp && item.char_cg_valid &&
        item.char_rows[14 * 4 + 1] > 0.0f && debug_mode == 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      cloth_stamp_tex = resolve_fetch_words(
          item.char_cg_fetch,
          (uint64_t(item.mesh) << 1) ^ (5ull << 60) ^ (1ull << 59), item.retained);
      if (cloth_stamp_tex != nullptr &&
          (!cloth_stamp_tex->valid || cloth_stamp_tex->srv == nullptr)) {
        cloth_stamp_tex = nullptr;
      }
    }
    // Temporary, state-change-only trace for the CAC custom-art path. This
    // distinguishes a missing shader capture, missing draw-item association,
    // or failed texture decode without logging every character draw.
    if (item.char_skin || item.char_shift || item.char_cloth_stamp) {
      const uint32_t kind = item.char_skin ? 1u : item.char_shift ? 2u : 3u;
      const GuestTexture* art = item.char_skin ? skin_decal
                                : item.char_shift ? shift_graphic_tex
                                                  : cloth_stamp_tex;
      const bool rows_ok = item.char_rows[14 * 4 + 1] > 0.0f;
      uint64_t signature = FetchWordsKey(item.char_cg_fetch);
      signature ^= uint64_t(item.char_cg_valid) << 1;
      signature ^= uint64_t(item.retained) << 2;
      signature ^= uint64_t(rows_ok) << 3;
      signature ^= uint64_t(art != nullptr && art->valid && art->srv != nullptr) << 4;
      const uint64_t probe_key = (uint64_t(item.mesh) << 2) | kind;
      static std::unordered_map<uint64_t, uint64_t> s_art_probe;
      if (s_art_probe.size() > 2048) s_art_probe.clear();
      const auto prior = s_art_probe.find(probe_key);
      if (prior == s_art_probe.end() || prior->second != signature) {
        s_art_probe[probe_key] = signature;
        REXLOG_INFO(
            "native-scene: stamp route kind={} mesh={:08X} ib={:08X} vb={:08X} "
            "cg={} retained={} rows={} fetch=[{:08X} {:08X} {:08X}] "
            "uv=({:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f}) tex={} srv={}",
            kind, item.mesh, item.ib_obj, item.vb_obj, item.char_cg_valid,
            item.retained, rows_ok, item.char_cg_fetch[0], item.char_cg_fetch[1],
            item.char_cg_fetch[2], item.char_cg[0], item.char_cg[1],
            item.char_cg[2], item.char_cg[3], item.char_cg[4], item.char_cg[5],
            art != nullptr && art->valid,
            art != nullptr && art->srv != nullptr);
      }
    }
    // Face injury layer (see DrawItem::face_inj_*): the skater's bruise /
    // scrape map (the face PS's tf6), bound in the t1 slot the face never
    // uses (it carries no custom graphic); overlay.x = 2 tells the branch.
    const GuestTexture* face_inj = nullptr;
    if (item.char_face && item.face_inj_valid && skin_decal == nullptr &&
        debug_mode == 0) {
      face_inj = resolve_fetch_words(item.face_inj_fetch,
                                     (uint64_t(item.mesh) << 1) ^ (3ull << 61),
                                     item.retained);
      if (face_inj != nullptr && !face_inj->valid) {
        face_inj = nullptr;
      }
    }
    // CAC skin body injury map (tf7, see DrawItem::char_layer_*): bound in
    // the free t5 pair slot, strength in ch_tintB.y (0 = none).
    const GuestTexture* skin_inj = nullptr;
    if (item.char_skin && item.char_layer_valid && debug_mode == 0) {
      skin_inj = resolve_fetch_words(
          item.char_layer_fetch,
          (uint64_t(item.mesh) << 1) ^ (uint64_t(item.char_layer_slot) << 60), item.retained);
      if (skin_inj != nullptr && !skin_inj->valid) {
        skin_inj = nullptr;
      }
    }
    // CAS subsurface scattering (see DrawItem::sss): the piece's blurmask
    // (draw-time fetch words) and the game's blur scale must both be in
    // hand, plus the two passes' targets; the passes run right before this
    // piece's own draw (see below), the main PS then mixes (overlay.y).
    const GuestTexture* sss_mask_tex = nullptr;
    float sss_c0[2] = {0.0f, 0.0f};
    if (item.sss && item.char_rows[14 * 4 + 1] > 0.0f && debug_mode == 0 &&
        g_r.pso_skin_unwrap != nullptr && g_r.pso_skin_blur != nullptr) {
      sss_c0[0] = scene.skin_blur_c0[0];
      sss_c0[1] = scene.skin_blur_c0[1];
      if (sss_c0[0] > 0.0f && sss_c0[1] > 0.0f && EnsureSkinSssTargets(context.device)) {
        sss_mask_tex = resolve_fetch_words(
            item.sss_mask, (uint64_t(item.mesh) << 1) ^ (1ull << 63), item.retained);
        if (sss_mask_tex != nullptr && !sss_mask_tex->valid) {
          sss_mask_tex = nullptr;
        }
      }
    }
    // constants = world + mvp (world * view_proj, row-vector) + tint + cam
    // + material tint + overlay params + misc. tint.a > 0 selects debug
    // solid colors; tint.r > 0 marks a bound lightmap. For transparent
    // items misc.yzw carries the fog ramp and mat_tint the fog color.
    if (prof_items) {
      di_t2 = PerfClock::now();
    }
    float constants[52] = {};
    std::memcpy(constants, item.world, sizeof(item.world));
    if (item.unlit) {
      // sky.*: the dome mesh is CAMERA-RELATIVE (sky.fx defaultVS adds
      // g_vViewPos to every vertex). Anchoring it at the world origin put
      // the baked skyline panorama ~700 m off, visibly rotated/parallaxed
      // against the emulated frame. The game's sky viewpos tracks the camera
      // in x/z but pins Y at the level's fixed sky elevation (captured per
      // frame from the sky draw's VS bank; 165.0 in every capture); using
      // cam.y rendered the skyline ~160 m too LOW.
      constants[12] += scene.cam_pos[0];
      constants[13] += scene.sky_height;
      constants[14] += scene.cam_pos[2];
    }
    float* mvp = constants + 16;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
        }
        mvp[r * 4 + c] = sum;
      }
    }
    // Bone palette upload for skinned items; tint.g flags skinning.
    bool bones_bound = false;
    uint32_t bone_srv_offset = 0;
    if (item.skinned && !item.bones.empty()) {
      const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      if (offset + bytes <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        cmd->SetBufferSrv(3, g_r.bone_ring, bone_region + offset);
        bones_bound = true;
        bone_srv_offset = bone_region + offset;
      }
    }
    if (!bones_bound) {
      if (item.skinned && !item.bones.empty()) {
        // Ring exhausted: this item renders bind-pose at identity,
        // effectively invisible. Must never happen silently.
        g_rr_no_bones.fetch_add(1, std::memory_order_relaxed);
      }
      cmd->SetBufferSrv(3, g_r.bone_ring, 0);
    }

    if (debug_mode >= 2) {
      // Stable per-object colors: hash the mesh address, not the (sort-order
      // dependent) item index.
      const uint32_t hash = (item.mesh >> 4) * 2654435761u;
      constants[32] = float((hash >> 0) & 0xFF) / 255.0f;
      // tint.g is also the VS skinning flag (>= 0.375 skins): items with a
      // bound palette keep their real skinning value so skinned characters
      // stay posed (a hash green below the threshold drew them bind-pose at
      // identity, off-screen); the rest keep a hash green under the threshold.
      constants[33] = bones_bound ? (item.char_family >= 1 && item.char_family <= 5
                                         ? 1.0f
                                         : 0.5f)
                                  : float((hash >> 8) & 0xFF) / 255.0f * 0.37f;
      constants[34] = float((hash >> 16) & 0xFF) / 255.0f;
      constants[35] = 1.0f;
    } else {
      constants[32] = lightmap != nullptr ? 1.0f : 0.0f;
      // tint.g doubles as the "character: alpha = gloss, no alpha-test"
      // marker; ropa garments rendered rigid have no bones but must not be
      // alpha-clipped (their VS skinning branch is inert: zero weights).
      // Skinning rule (see scene.hlsl vs_main): 1 = character VS families
      // (implied fourth weight), 0.5 = explicit weights (vehicles etc.),
      // 0.25 = rigid ropa garment (no skinning).
      constants[33] = bones_bound ? (item.char_family >= 1 && item.char_family <= 5
                                         ? 1.0f
                                         : 0.5f)
                                  : (item.ropa ? 0.25f : 0.0f);
      constants[34] = item.unlit ? 1.0f : 0.0f;
      // tint.b = -1: the game's own main-pass draw of this item does NOT
      // alpha-test (RB_COLORCONTROL alpha_test_enable, bit 3, off), so the
      // shader's world alpha test must not either. Opaque materials whose
      // diffuse alpha carries something else (a mask, gloss) were
      // discarded wholesale -- the sign board that rendered only its text.
      if (!item.unlit && item.char_family == 0 && item.game_rs_seen &&
          ((item.game_rs[2] >> 3) & 1u) == 0) {
        constants[34] = -1.0f;
      }
      // tint.a < 0 marks dynamic-entity items for the showcase dyn layer
      // (ps_main clips them until the bit reveals); the solid-color
      // early-out only ever reads tint.a > 0. Staged only while a showcase
      // frame is live (nonzero split rows) so normal rendering never
      // carries the marker at all.
      constants[35] = (item.dyn_entity && (g_r.showcase_rows[0] != 0.0f ||
                                           g_r.showcase_rows[1] != 0.0f))
                          ? -1.0f
                          : 0.0f;
    }
    constants[36] = scene.cam_pos[0];
    constants[37] = scene.cam_pos[1];
    constants[38] = scene.cam_pos[2];
    // cam_pos.w = character shading family: > 0 switches the PS to the
    // captured character-lighting branch (rows uploaded to b2 below);
    // char_rows[14*4+1] stays 0 when the capture failed validation, which
    // keeps the item on the legacy empirical shading.
    constants[39] = 0.0f;
    if (debug_mode == 0 && item.char_family != 0 &&
        item.char_rows[14 * 4 + 1] > 0.0f) {
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      // 18 float4 rows = 288 bytes -> a 512-byte slot keeps the next
      // allocation 256-aligned (CBV offset requirement).
      if (offset + 512u <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.char_rows,
                    sizeof(item.char_rows));
        // ch_tintB.y (row 13): the skin injury strength when its map is
        // bound at t5 (see skin_inj); unused by fam 1/2 otherwise.
        {
          float* rows = reinterpret_cast<float*>(g_r.bone_ring_cpu + bone_region + offset);
          if (item.char_family == 1 || item.char_family == 2) {
            rows[13 * 4 + 1] = skin_inj != nullptr ? item.char_layer_amount : 0.0f;
          }
        }
        g_r.bone_ring_offset = offset + 512u;
        cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region + offset);
        constants[39] = item.char_rows[14 * 4 + 1];
        g_char_drawn.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // cam_pos.w = -family selects the exact world-material branch. Gated on
    // the frame rows being captured (scene.shadow_valid carries the scene
    // exposure / material multiplier at b1); without them the tone chain
    // would multiply by zero and render black.
    if (debug_mode == 0 && item.env_family != 0 && scene.shadow_valid &&
        !item.water &&
        // environment.transparent runs its exact branch (fam 16) in the
        // sorted alpha pass once its rows are captured; every other
        // transparent item keeps the legacy transparent shading.
        // Fam 18 (transparentincandescent neon) needs no captured rows
        // beyond the frame's own (exposure / fog), like fam 12.
        (!item.transparent || (item.env_family == 16 && scene.ts_valid) ||
         item.env_family == 18) &&
        // Fam 14 needs the frame's scroll rows (time + multiplier); the
        // tone chain would multiply by zero without them; the legacy
        // shading covers the capture-less frames.
        (item.env_family != 14 || scene.scroll_valid) &&
        // Fam 19 (animated.flag) needs its PS c0.x lightmap scale.
        (item.env_family != 19 || scene.flag_anim_valid) &&
        // Fam 15 (advertisement) needs its captured floor/multiplier/
        // exposure rows; legacy shading covers capture-less frames.
        (item.env_family != 15 || scene.ad_valid || item.ad_seen)) {
      constants[39] = -float(item.env_family);
      // A `_skatepark` permutation (see DrawItem::dmo_recv) marks itself as
      // -(family + 0.25): the root block has no free slot left, and every
      // family test, here and in the shader, is a +-0.5 window.
      if (item.dmo_recv && scene.park_dmo_valid) {
        constants[39] -= 0.25f;
      }
    }
    // cam_pos.w = -(20 + variant) selects the exact dynamicobject branch
    // (rigid props). Gated on the frame's dynamicobject lighting rows having
    // been captured (dyn_* at b1); without them the tone chain multiplies by
    // zero and renders black; fall back to the legacy shading otherwise.
    // The park variants (4/5/6) take the same marker range but read
    // park_rows instead -- see ShadePark; they must not be gated on the
    // dynamicobject rows, which park-only views never refresh.
    if (debug_mode == 0 && item.dynobj != 0 &&
        (item.dynobj >= 4 ? scene.park_valid[item.dynobj == 5 ? 1 : 0]
                          : scene.dynobj_valid)) {
      constants[39] = -float(20 + item.dynobj);
      // The preview object's _foregroundPS variant: -(20 + variant + 0.25)
      // (ShadePark: no shadow / cookie terms; every test on this marker is
      // a +-0.5 window).
      if (item.foreground) {
        constants[39] -= 0.25f;
      }
      g_dynobj_drawn.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(constants + 40, item.tint, 4 * sizeof(float));
    // The selected piece's overlap highlight (DrawItem::highlight): mat_tint
    // carries c24.rgb, w = 2 flags it (ShadePark never reads mat_tint).
    if (item.highlight && item.dynobj >= 4) {
      constants[40] = item.highlight_rgb[0];
      constants[41] = item.highlight_rgb[1];
      constants[42] = item.highlight_rgb[2];
      constants[43] = 2.0f;
    }
    // t3 = macro grime/crack overlay, t4 = decal art for environment.decal
    // surfaces (in-shader composite). Independent slots: decal ground/wall
    // sections carry the same macrooverlay as their non-decal neighbors, and
    // binding only the art dropped the macro multiply there; alternating
    // plaza sections rendered ~1.4x too bright (the ground checkerboard).
    const GuestTexture* macro_tex =
        item.macro_tex != 0 && REXCVAR_GET(skate3_native_render_scene_macro)
            ? resolve_texture(item.macro_tex, 2)
            : &g_r.white;
    if ((item.water || item.char_family >= 6 ||
         (item.char_family >= 1 && item.char_family <= 4)) &&
        item.water_normal != 0) {
      // Water rides its ripple normal map in the macro slot (water never
      // carries a macro overlay; overlay.z stays 0 below so the macro
      // composite path never runs). Vehicles do the same with their DXN
      // panel normal map; without it the hinged panels' vertex normals
      // face away from the sun and shade as a dark ambient-blue patch that
      // stops at the door seams (the exact PS with a FLAT map reproduces
      // that artifact; with the real map it matches the emulated car).
      // Character fams 1/2 (defaultcharacter / CAC skin, face, cloth,
      // lenses) do the same with their DXT5nm `normal` channel, the
      // garment crease / skin pore detail the emulated render shows;
      // overlay.z > 0 tells the char branch the map resolved. Fam 4
      // (cac_hair) perturbs its N.L with the same tf4.wy map. Fam 3's
      // exact stamp branch (livingworld_stamp_defaultPS) reads its full
      // tangent-space map at tf4.xyz.
      macro_tex = resolve_texture(item.water_normal, 3);
    }
    // Water / vehicle environment cube (t6, root param 8): decoded once per
    // guest object into the cube cache; the gray fallback cube otherwise.
    // Vehicle materials carry an `environment` channel that resolves through
    // the same chan+0x1C path as the ocean's. Cube decodes run on the
    // workers (a SINGLE inline cube decode measured up to ~100 ms, the
    // largest remaining traversal hitch when a vehicle / reflective area
    // streamed in); flat-gray reflections for the 1-3 frames in flight are
    // invisible.
    const GuestTexture* cube_tex = &g_r.white_cube;
    // The ocean samples the cube the GAME binds at tf7 for its draw, not the
    // material's environment channel (see g_ocean_tf7).
    const bool ocean_bound_cube =
        item.water && item.water_ocean == 1 && scene.ocean_tf7 != 0;
    const uint32_t env_cube_ptr =
        ocean_bound_cube ? scene.ocean_tf7 : item.water_env;
    if ((item.water || item.char_family >= 6 ||
         (item.env_family >= 5 && item.env_family <= 6) ||
         item.env_family == 13) &&
        env_cube_ptr != 0) {
      auto cit = g_r.cube_textures.find(env_cube_ptr);
      if (cit == g_r.cube_textures.end()) {
        if (!item.retained || ocean_bound_cube) {
          EnqueueCubeMiss(env_cube_ptr);
        }
      } else if (cit->second.valid) {
        cube_tex = &cit->second;
        cit->second.last_used_frame = frame_number;
      }
    }
    const GuestTexture* decal_tex = item.decal && item.decal_art != 0 &&
                                            REXCVAR_GET(skate3_native_render_scene_decals)
                                        ? resolve_texture(item.decal_art, 4)
                                        : &g_r.white;
    // Draw-time decal art (see DrawItem::decal_fetch): whatever the game
    // bound at tf4 on the item's main-pass draw when it is not the
    // `decal` channel's own object (ApplyDecalDrawAddressing; e.g. ad
    // frames covered by an environment.decal section get the current
    // event-ad art bound over the channel).
    if (item.decal && item.decal_fetch[1] != 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* ad = resolve_fetch_words(
          item.decal_fetch, (uint64_t(item.mesh) << 1) | 1, item.retained);
      if (ad != nullptr) {
        decal_tex = ad;
      }
    }
    // decal2environment's second art layer: the texture the game bound at
    // tf5 on the main-pass draw (DrawItem::decal2_fetch), bound at t12.
    // Not drawn until observed and decoded (never a guessed channel).
    const GuestTexture* decal2_tex = nullptr;
    if (item.decal2 && item.decal2_fetch[1] != 0 && debug_mode == 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* d2 = resolve_fetch_words(
          item.decal2_fetch, (uint64_t(item.mesh) << 1) | 0x200000000ull, item.retained);
      if (d2 != nullptr && d2 != &g_r.white && d2->valid && d2->srv != nullptr) {
        decal2_tex = d2;
      }
    }
    // environmentparkdecal pieces: the material's decal art (the game's tf3,
    // sampled clamped at the half4 texcoord's zw = uv3) blends over the
    // wood: albedo^2 = lerp(diffuse^2, decal^2, decal.a) (ucode 21-24).
    // Only a DECODED texture: the white fallback (alpha 1) would bleach the
    // piece; overlay.w = 6 tells ShadePark it is bound.
    bool park_decal = false;
    if (item.dynobj == 6 && item.decal_art != 0 && debug_mode == 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* pd = resolve_texture(item.decal_art, 4);
      if (pd != &g_r.white && pd->valid) {
        decal_tex = pd;
        park_decal = true;
      }
    }
    if (((item.char_family >= 4 && item.char_family <= 5) || item.char_alpha) &&
        item.hair_alpha_tex != 0) {
      // Hair strand coverage rides the (otherwise unused) decal slot; the
      // PS hair branch samples it at the raw second texcoord. The white
      // fallback keeps failed decodes opaque rather than invisible.
      // character.alpha lenses use the same plumbing (ch_misc.z signals the
      // fam-1/2 branch to take alpha from this texture).
      decal_tex = resolve_texture(item.hair_alpha_tex, 5);
    }
    // F7 ring: stamp the SERVED content fingerprints (see SceneRingItem);
    // pointer identity cannot see an in-place content swap.
    if (ring_frame != nullptr) {
      const auto rit = ring_map.find(&item);
      if (rit != ring_map.end()) {
        SceneRingItem& ri = ring_frame->items[rit->second];
        const auto fp_of = [](const GuestTexture* t) -> uint64_t {
          return t != nullptr && t->valid ? t->payload_fp : 0;
        };
        ri.fp_diffuse = fp_of(diffuse);
        ri.fp_lightmap = fp_of(lightmap);
        ri.fp_macro = fp_of(macro_tex);
        ri.fp_decal = fp_of(decal_tex);
      }
    }
    if (g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr) {
      // Traced-mesh per-frame summary: SERVED state (post-resolve), logged
      // when anything changes. The mesh's texture objects also register for
      // the worker-commit trace.
      const auto fp_of = [](const GuestTexture* t) -> uint64_t {
        return t != nullptr && t->valid ? t->payload_fp : 0;
      };
      for (const GuestTexture* t :
           {diffuse, lightmap, macro_tex, decal_tex}) {
        if (t != nullptr && t != &g_r.white) {
          g_trace_keys.insert(FetchWordsKey(t->fetch_words));
        }
      }
      uint64_t sig = uint64_t(item.diffuse_tex) ^
                     (uint64_t(item.decal_art) << 16) ^
                     (uint64_t(item.lightmap_tex) << 32) ^
                     (uint64_t(item.macro_tex) << 48);
      sig ^= fp_of(diffuse) * 3 ^ fp_of(decal_tex) * 5 ^ fp_of(lightmap) * 7 ^
             fp_of(macro_tex) * 11;
      sig ^= item.retained ? 1 : 0;
      uint64_t& last = g_trace_sig[item.ctx];
      if (sig != last) {
        last = sig;
        REXLOG_INFO(
            "tex-trace: f{} SERVED ctx={:08X} retained={} "
            "dif={:08X}/{:016X} dec={:08X}/{:016X} lm={:08X}/{:016X} "
            "mac={:08X}/{:016X}",
            frame_number, item.ctx, item.retained ? 1 : 0, item.diffuse_tex,
            fp_of(diffuse), item.decal_art, fp_of(decal_tex),
            item.lightmap_tex, fp_of(lightmap), item.macro_tex,
            fp_of(macro_tex));
      }
    }
    // Exact env families without decal art bind the material's spec/ecc/
    // refmask map (or the animated.tree noise tint) in the free decal slot;
    // overlay.w == 3 tells the shader the masks are live.
    bool spec_bound = false;
    if (((item.env_family != 0 && !item.decal && item.env_family != 10) ||
         item.water_flowing) &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex, 6);
      if (spec != &g_r.white) {
        decal_tex = spec;
        spec_bound = true;
      }
    }
    // Sky dome: the material's `specular` channel is the 1D radial sun
    // gradient (512x16), bound in the free decal slot for the exact sky
    // branch. Until it decodes the dome falls back to the plain fullbright
    // panorama (sunless for the 1-3 frames in flight).
    bool sky_sun_bound = false;
    if (item.unlit && item.spec_tex != 0) {
      const GuestTexture* sun = resolve_texture(item.spec_tex, 6);
      if (sun != &g_r.white) {
        decal_tex = sun;
        sky_sun_bound = true;
      }
    }
    // Character skin/face spec-mask map (the cacstamp `specular` channel):
    // rides the free decal slot like the env families. Written to
    // overlay.w below: 3 = mask bound, 2 = channel present but not yet
    // decoded (the PS masks the spec OFF; the DXT1 skin diffuse's opaque
    // alpha would read as a full-white mask), 0 = no channel (the PS masks
    // by diffuse alpha^2, cloth/jeans). char_alpha lenses keep the decal
    // slot for their coverage texture.
    float char_spec = 0.0f;
    if ((item.char_family == 1 || item.char_family == 2) && !item.char_alpha &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex, 6);
      if (spec != &g_r.white && spec->valid && spec->srv_mips != 0) {
        decal_tex = spec;
        char_spec = 3.0f;
      } else {
        char_spec = 2.0f;
      }
    }
    // CAC cloth dirt layer (decal2 -> the cloth PSes' tf5): cloth carries no
    // spec-mask channel, so it takes the free decal slot; overlay.w = 5
    // tells the fam-2 branch to blend it (lerp by dirt.a x c22.y). Not yet
    // decoded = no dirt this frame (clean cloth, never garbage).
    // The game binds the skater's runtime dirt / wear map (tf5, see
    // DrawItem::char_layer_*) at the cloth draw; the material's decal2
    // channel is only the fallback until that capture lands.
    if (item.char_family == 2 && !item.char_alpha && char_spec == 0.0f &&
        !item.char_skin) {
      const GuestTexture* dirt = nullptr;
      if (item.char_layer_valid) {
        dirt = resolve_fetch_words(
            item.char_layer_fetch,
            (uint64_t(item.mesh) << 1) ^ (uint64_t(item.char_layer_slot) << 60),
            item.retained);
        if (dirt != nullptr && !dirt->valid) {
          dirt = nullptr;
        }
      }
      if (dirt == nullptr && item.dirt_tex != 0) {
        dirt = resolve_texture(item.dirt_tex, 4);
        if (dirt == &g_r.white || !dirt->valid) {
          dirt = nullptr;
        }
      }
      if (dirt != nullptr) {
        decal_tex = dirt;
        char_spec = 5.0f;
      }
    }
    const bool is_decal =
        item.char_family < 4 && item.decal && decal_tex != &g_r.white;
    // Fam 5/6 masks+normal pair (t4/t5): the reflective PS perturbs its
    // reflection/spec with the material's normal map (shader overlay.w == 4
    // branch, the fix for the giant flat-normal cube smear on glass
    // facades). Both views bind together at the draw via SetTexturePair;
    // pair descriptors are backend-cached, so prewarm/revalidation texture
    // swaps can never leave a stale descriptor. Until the normal map
    // decodes, overlay.w stays 3 (flat-normal reflection, the old behavior).
    const GuestTexture* pair_normal = nullptr;
    bool normal_paired = false;
    if (item.env_family >= 5 && item.env_family <= 6) {
      uint32_t gate = 0;
      if (!spec_bound) gate |= 1;
      if (item.water_normal == 0) gate |= 2;
      if (spec_bound && (!decal_tex->valid || decal_tex->srv_mips == 0)) gate |= 4;
      if (gate == 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm == &g_r.white) {
          gate |= 8;
        } else {
          if (!nrm->valid) gate |= 16;
          if (nrm->srv_mips == 0) gate |= 32;
        }
        if (gate == 0) {
          pair_normal = nrm;
          normal_paired = true;
        }
      }
      if (normal_paired) {
        g_refl_pair.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_refl_flat.fetch_add(1, std::memory_order_relaxed);
        g_refl_gate.store(gate, std::memory_order_relaxed);
      }
    }
    // World-shading v2 (fams 1-4): the material's real per-pixel maps. The
    // base normal map rides the free t5 pair slot (t4 holds the spec masks
    // or the decal art there), the detail normal map + the decal families'
    // spec/ecc masks ride the t8/t9 pair. misc.z carries the bind flags
    // (1 = normal, 2 = detail, 4 = spec2), misc.w the detailNormalUVScale
    // channel constant; both slots are spare on opaque fams 1-4 (fog only
    // uses them on transparent/water items, F12 only on fams 5/6/13).
    const GuestTexture* v2_detail = nullptr;
    const GuestTexture* v2_spec2 = nullptr;
    uint32_t v2_flags = 0;
    if (item.env_family >= 1 && item.env_family <= 4 && !item.transparent &&
        !item.water && debug_mode == 0 && scene.shadow_valid &&
        REXCVAR_GET(skate3_native_render_scene_world_v2)) {
      if (item.water_normal != 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm != &g_r.white && nrm->valid && nrm->srv_mips != 0) {
          pair_normal = nrm;
          normal_paired = true;  // t5 pair bind; overlay.w stays 0..3 here
          v2_flags |= 1;
        }
      }
      // Fam 2 (environmentsimple) carries a plain normal map with no
      // base+detail pair; its vnd is the raw 2t-1 (shader-side).
      if ((v2_flags & 1u) != 0 && item.env_family != 2 &&
          item.detail_tex != 0 && item.detail_scale > 0.0f) {
        const GuestTexture* det = resolve_texture(item.detail_tex, 7);
        if (det != &g_r.white && det->valid && det->srv_mips != 0) {
          v2_detail = det;
          v2_flags |= 2;
        }
      }
      if (item.decal && item.spec_tex != 0) {
        const GuestTexture* sp = resolve_texture(item.spec_tex, 6);
        if (sp != &g_r.white && sp->valid && sp->srv_mips != 0) {
          v2_spec2 = sp;
          v2_flags |= 4;
        }
      }
    }
    // Dynamicobject v2: the same misc.z/w convention and t5/t8/t9 slots as
    // fams 1-4; dynobj draws are never transparent/water and carry
    // env_family 0, so both misc slots are free, and t4 stays on its white
    // fallback (the spec-mask t4 bind is env-family-only), so the t5 pair
    // costs nothing. The spec/ecc masks go to t9 rather than t4: it keeps
    // t4's semantics untouched and matches the decal families' 2-channel
    // mask convention. Gated exactly like the PS branch selection (dynobj
    // frame rows captured) plus the dynobj_v2 cvar for A/B against the v1
    // flat response.
    // Park pieces (4/5/6) gate on their own rows, the same test that selects
    // ShadePark: a park-only session never refreshes the dynamicobject rows,
    // and gating on those left the pieces without their normal maps.
    if (item.dynobj != 0 && debug_mode == 0 &&
        (item.dynobj >= 4 ? scene.park_valid[item.dynobj == 5 ? 1 : 0]
                          : scene.dynobj_valid) &&
        REXCVAR_GET(skate3_native_render_scene_dynobj_v2)) {
      if (item.water_normal != 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm != &g_r.white && nrm->valid && nrm->srv_mips != 0) {
          pair_normal = nrm;
          normal_paired = true;
          v2_flags |= 1;
        }
      }
      if ((v2_flags & 1u) != 0 && item.detail_tex != 0 &&
          item.detail_scale > 0.0f) {
        const GuestTexture* det = resolve_texture(item.detail_tex, 7);
        if (det != &g_r.white && det->valid && det->srv_mips != 0) {
          v2_detail = det;
          v2_flags |= 2;
        }
      }
      if (item.spec_tex != 0) {
        const GuestTexture* sp = resolve_texture(item.spec_tex, 6);
        if (sp != &g_r.white && sp->valid && sp->srv_mips != 0) {
          v2_spec2 = sp;
          v2_flags |= 4;
        }
      }
      // Static world-shadow map at t4 (flag 8): the native re-render of the
      // game's baked-shade map (see RenderShadowAtlas). t4 is otherwise the
      // white fallback on dynobj draws. Without it the PS world term is 1
      // and props inside baked building shade light at full key.
      // (Not the park pieces: ShadePark has no t4 world term, and t4 carries
      // their decal art.)
      if (item.dynobj < 4 && g_r.world_shadow_srv != nullptr &&
          g_r.world_shadow_in_srv && scene.dynobj_ws_valid) {
        v2_flags |= 8;
      }
    }
    // Exact ocean (fam 31): the second PCA normal component rides the free
    // t8 pair slot and the 16x16 macro overlay the t9 slot (water items use
    // neither). v2_flags stays 0; the ocean branch signals its binds
    // through the overlay row.
    bool ocean_n2 = false;
    bool ocean_ov = false;
    if (item.water_ocean == 1 && debug_mode == 0 && scene.shadow_valid &&
        scene.ocean_valid) {
      if (item.water_normal2 != 0) {
        const GuestTexture* n2 = resolve_texture(item.water_normal2, 7);
        if (n2 != &g_r.white && n2->valid && n2->srv_mips != 0) {
          v2_detail = n2;
          ocean_n2 = true;
        }
      }
      if (item.macro_tex != 0) {
        const GuestTexture* mo = resolve_texture(item.macro_tex, 6);
        if (mo != &g_r.white && mo->valid && mo->srv_mips != 0) {
          v2_spec2 = mo;
          ocean_ov = true;
        }
      }
    }
    constants[44] = item.macro_scale;
    constants[45] = item.macro_opacity;
    constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
    // overlay.w: [1, 2) = decal art bound (the fraction packs its
    // addressing, below), 3 = spec masks bound (exact env families). The
    // 4-encode (t5 normal pair driving the reflective trim path) is fams
    // 5/6 only; fams 1-4 signal their normal map through the misc.z flags
    // instead.
    // Decal addressing is the GAME's per-draw tf4 sampler state
    // (DrawItem::decal_addr, recorded off the item's main-pass draw), per
    // axis: decalenvironment and decalenvironment_tileable (and their
    // _simple variants) are byte-identical microcode, so which art tiles
    // is decided ONLY by the sampler state renderengine binds -- capture
    // 1790952904 has the same 512x128 curb strip WRAP on four meshes and
    // CLAMP_LAST on six. Two earlier guesses got this wrong: forcing WRAP
    // on every decal_tileable item (tiled art its material clamps), and
    // reading clamp_x off the decoded texture's stored fetch words, which
    // are whichever bind variant decoded first (FetchWordsKey strips the
    // clamp bits) and only the U axis. Until the draw is observed, fall
    // back to the family default: tileable wraps, single placements clamp
    // to the edge. Packed as 1 + (code + 256 * fade) / 65536 (code =
    // clamp_x | clamp_y << 3 | border_color << 6; fade = the macro-overlay
    // fade floor VS c10.x in 1/255 steps, 255 = no fade while uncaptured),
    // exact in fp32 and < 2; DecalArtCode / DecalMacroFadeFloor decode it.
    uint32_t decal_code;
    if ((item.decal_addr & 0x8000u) != 0) {
      decal_code = item.decal_addr & 0xFFu;
    } else {
      decal_code = item.decal_tileable ? 0u : (2u | (2u << 3));
    }
    const uint32_t decal_fade_q =
        item.decal_fade < 0.0f
            ? 255u
            : uint32_t(std::lround(std::clamp(item.decal_fade, 0.0f, 1.0f) * 255.0f));
    constants[47] =
        is_decal ? 1.0f + float(decal_code + 256u * decal_fade_q) / 65536.0f
                 : (spec_bound ? ((normal_paired && item.env_family >= 5)
                                      ? 4.0f
                                      : 3.0f)
                               : 0.0f);
    if (char_spec != 0.0f) {
      // Character fam 1/2 spec-mask state (see the bind above); chars
      // never take the is_decal/spec_bound paths, so the slot is theirs.
      constants[47] = char_spec;
    }
    if (park_decal) {
      constants[47] = 6.0f;
    }
    // Far livingworld pedestrians (defaultlivingworld_defaultPS, capture
    // marks ch_misc.z = 2): the PS lights them only by the static world
    // shadow map (tf1, 4-tap). Bind the native re-render at t4 (free on
    // fam 3) and tell the branch through overlay.w = 8.
    const bool lw_ws = debug_mode == 0 && item.char_family == 3 &&
                       item.char_rows[14 * 4 + 1] > 0.0f &&
                       item.char_rows[14 * 4 + 2] > 1.5f &&
                       g_r.world_shadow_srv != nullptr && g_r.world_shadow_in_srv &&
                       scene.dynobj_ws_valid;
    if (lw_ws) {
      constants[47] = 8.0f;
    }
    if (item.char_family == 3) {
      // misc.y = the normal-map LOD bias (same rationale as fams 1/2 below).
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
    }
    if (item.char_family == 1 || item.char_family == 2) {
      // overlay.x = the skin decal layer bound at t1 (1, fam 2 skin) or the
      // face injury map (2, strength c22.z in misc.z).
      constants[44] = skin_decal != nullptr ? 1.0f : face_inj != nullptr ? 2.0f : 0.0f;
      // overlay.y = the subsurface scatter amount c21.x (0 = off).
      constants[45] = sss_mask_tex != nullptr && scene_pso_path ? item.sss_amount : 0.0f;
      // misc.y = normal/spec-map LOD bias to the console's 640p-gradient
      // mip (the fam 5/6 cube-bias rationale): at 4K the shader's UV
      // gradients pick ~1.75 mips finer than the game's own render, and
      // mip-0 sampling keeps fine wrinkle noise the console filters away
      // - the authored garment folds read weaker than the emulated
      // reference without this.
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
    }
    // Exact flowingwateralpha branch (cam_pos.w = -30): the canal/waterfall
    // shader hand-ported from the game's own PS and verified per-pixel
    // against the ucode. Requires the frame m_params rows (b1), the shared
    // world frame rows (exposure/sun/fog at b1, scene.shadow_valid) and the
    // ripple normal map resolved in the macro slot; the legacy water
    // shading covers every gap (and every non-flowing water material).
    const bool water_exact = item.water && item.water_flowing &&
                             debug_mode == 0 && scene.shadow_valid &&
                             (scene.water_valid || flow_set_of(item) >= 0) &&
                             macro_tex != &g_r.white;
    // Exact ocean surface (fam 31): both PCA component maps must resolve
    // (t3 = comp 0 via the water_normal macro-slot bind, t8 = comp 1); the
    // legacy water shading covers every gap. The horizon sheet (fam 32)
    // needs only its baked texture (t0) and the captured fade row.
    const bool ocean_exact = item.water && item.water_ocean == 1 &&
                             debug_mode == 0 && scene.shadow_valid &&
                             scene.ocean_valid && macro_tex != &g_r.white &&
                             ocean_n2;
    const bool oceanrefl_exact = item.water && item.water_ocean == 2 &&
                                 debug_mode == 0 && scene.shadow_valid &&
                                 scene.oceanrefl_valid;
    // Exact water.default (fam 33): every water material that is not
    // flowing* and not ocean* -- this build's canal/pool water, which binds
    // water_defaultPS. Needs the captured rows and the first PCA component
    // in the macro slot; the legacy approximation still covers the gaps.
    const bool waterdef_exact_pre = item.water && !item.water_flowing &&
                                    item.water_ocean == 0 && debug_mode == 0 &&
                                    scene.waterdef_valid;
    const bool waterdef_exact = item.water && !item.water_flowing &&
                                item.water_ocean == 0 && debug_mode == 0 &&
                                scene.shadow_valid && scene.waterdef_valid &&
                                macro_tex != &g_r.white;
    // The game's tf7 for water.default: the second normal tap set, captured
    // per draw and published per frame (FrameScene::waterdef_tf7).
    const GuestTexture* waterdef_n2 = nullptr;
    if (waterdef_exact_pre) {
      if (scene.waterdef_tf7 != 0) {
        const GuestTexture* t = resolve_texture(scene.waterdef_tf7, 8);
        if (t != nullptr && t != &g_r.white) {
          waterdef_n2 = t;
          g_r.waterdef_tf7_last = scene.waterdef_tf7;
        }
      }
      // Whenever the primary fails -- a different address that has not
      // decoded, OR the same one evicted mid-flight.
      if (waterdef_n2 == nullptr && g_r.waterdef_tf7_last != 0) {
        // This frame's tf7 has not decoded yet: hold the last one that did
        // rather than drop to zero, which would blank half the normal.
        const GuestTexture* t = resolve_texture(g_r.waterdef_tf7_last, 8);
        if (t != nullptr && t != &g_r.white) {
          waterdef_n2 = t;
        }
      }
    }
    if (waterdef_exact) {
      // overlay.y = a real environment cube at t6.
      constants[39] = -33.0f;
      // overlay.x = the game's tf7 (second normal tap set) resolved. This
      // used ocean_n2, an OCEAN flag that is always false for water.alpha,
      // so the shader zeroed every tf7 tap -- half the normal decode. The
      // ucode fetches tf6 and tf7 unconditionally (lines 14-17).
      constants[44] = waterdef_n2 != nullptr ? 1.0f : 0.0f;
      constants[47] = 0.0f;  // overlay.w: set below once resolved
      // misc.z: which captured material set this surface belongs to. Keyed
      // on the material's own normal map, so each water surface is decoded
      // through ITS OWN PCA basis instead of whichever draw happened to be
      // captured first that frame.
      // Packed into overlay.w, NOT misc.z: water items fall through to the
      // `item.transparent || item.water` branch below, which overwrites
      // misc.zw with the fog ramp. overlay.w is water-only.
      //   0..7  = material set index
      //   8..15 = the same, +8, marking the ALPHA variant
      uint32_t wset = 0;
      for (uint32_t k = 0; k < 8; ++k) {
        if (scene.waterdef_set_have[k] &&
            scene.waterdef_set_key[k] == item.water_normal) {
          wset = k;
          break;
        }
      }
      constants[46] = item.water_mask != 0 ? 2.0f : 1.0f;   // overlay.z
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      // overlay.z = 2 selects the REAL mask gates in ShadeWaterDefault over
      // the broad fresnel stand-in. The ucode multiplies the reflection by
      // mask_z (line 120) as a separate factor, so pinning this to 1 made
      // the reflection a full-strength sheet everywhere instead of the
      // reference's patchy foam. An earlier attempt to enable it wiped the
      // ripples out, but that was because the shader was also missing the
      // `+ lml` term the ucode adds at line 118: with a closed gate there
      // was nothing left. That term is in now, so the gate shapes the
      // reflection instead of erasing it.
      // overlay.w: material set index, +8 for the ALPHA variant
      // (wateralpha_defaultPS writes a real alpha; water_defaultPS forces
      // w = 1).
      constants[47] = float(wset) + (item.water_alpha ? 8.0f : 0.0f);
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (water_exact) {
      // overlay.x = spec/reflection masks bound at t4, overlay.y = real
      // environment cube at t6, overlay.z = ripple resolved (gate above).
      constants[39] = -30.0f;
      constants[44] = spec_bound ? 1.0f : 0.0f;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = 1.0f;
      // overlay.w: + (set + 1) = this material's own fw_set rows (0 = the
      // frame rows); + 16 = water.flowing's flowingwater_defaultPS, which
      // outputs alpha 1 (flowingwateralpha floors its own alpha instead).
      constants[47] = float(flow_set_of(item) + 1) + (item.water_opaque ? 16.0f : 0.0f);
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (ocean_exact) {
      // overlay.x = second PCA component at t8 (gate above guarantees it),
      // overlay.y = real environment cube at t6, overlay.z = macro overlay
      // at t9, overlay.w = macroOverlayUVScale.
      constants[39] = -31.0f;
      constants[44] = 1.0f;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = ocean_ov ? 1.0f : 0.0f;
      constants[47] = item.macro_scale;
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (oceanrefl_exact) {
      constants[39] = -32.0f;
      constants[44] = 0.0f;
      constants[45] = 0.0f;
      constants[46] = 0.0f;
      constants[47] = 0.0f;
      g_water_drawn.fetch_add(1, std::memory_order_relaxed);
    } else if (item.water) {
      // overlay.x = ripple scroll time, overlay.y = real environment cube
      // bound at t6, overlay.z = ripple normal map resolved (in the macro
      // slot; the shader synthesizes procedural ripples otherwise).
      // overlay.w = 1 marks water with NO diffuse channel (ocean.default):
      // the body term must be zero (ocean.fx diffTerm = 0); the white
      // fallback diffuse otherwise renders the whole sea as a bright plain.
      constants[44] = water_time;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
      constants[47] = item.diffuse_tex == 0 ? 1.0f : 0.0f;
    } else if (item.char_family >= 6) {
      // Vehicles reuse the water convention: overlay.y > 0 = a real
      // environment cube bound at t6 (the PS vehicle branch's reflection
      // term). Vehicles never carry macro/decal channels, so the macro
      // defaults staged above are inert, but overlay.y must not inherit
      // macro_opacity's 1.0 default when no cube resolved.
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
    }
    // misc.x: alpha-blended sub-pass item (1 = transparentenvironment
    // shading, 2 = water branch); fog rides in misc.yzw (ramp) and the
    // mat_tint row (color), unused by these items otherwise
    // (root-signature DWORD budget).
    constants[48] = item.water ? 2.0f : (item.transparent ? 1.0f : 0.0f);
    if (water_exact || ocean_exact || oceanrefl_exact) {
      // misc.y = cube LOD bias (same 640p-gradient rationale as fams 5/6);
      // fog comes from the shared b1 rows; misc.zw stay free. misc.x keeps
      // the water marker for the SSR G-buffer restage.
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
      constants[50] = 0.0f;
      constants[51] = 0.0f;
    } else if (item.transparent || item.water) {
      constants[49] = scene.fog_ramp[0];
      constants[50] = scene.fog_ramp[1];
      constants[51] = scene.fog_ramp[2];
      std::memcpy(constants + 40, scene.fog_color, 4 * sizeof(float));
    } else if (item.env_family == 14 && constants[39] < -13.5f) {
      // Fam 14 (scrollincandescent, the LED chyron): misc.xy = the frame's
      // UV scroll offset (g_fAnimationTime x the material's channel speeds,
      // wrapped to one texture period here; the shader adds a small
      // bounded offset instead of losing uv precision to a large raw
      // time x speed product), misc.z = the material multiplier
      // m_params[0].y.
      const float ou = scene.scroll_rows[0] * item.scroll_u;
      const float ov = scene.scroll_rows[0] * item.scroll_v;
      constants[48] = ou - std::floor(ou);
      constants[49] = ov - std::floor(ov);
      constants[50] = scene.scroll_rows[1];
    } else if (v2_flags != 0) {
      // misc.z = the v2 material bind flags, misc.w = detailNormalUVScale
      // (opaque fams 1-4 and the dynobj families; see the v2 blocks above).
      constants[50] = float(v2_flags);
      constants[51] = item.detail_scale;
    } else if ((item.env_family >= 5 && item.env_family <= 6) ||
               item.env_family == 13) {
      // misc.y = cube LOD bias for the reflective families: the guest's
      // cube fetch computes its gradient LOD at the game's own 1152x640
      // render; at 4K our reflection-vector gradients are ~3.4x smaller
      // per pixel, so mips alone leave baked cube detail (the streetlight
      // heads) visible that the console blurs away.
      // misc.z/misc.w = the F12 reflection-isolation controls (see the
      // refl_mode cvar; both spare on opaque items; the fog packing only
      // uses these slots on transparent/water).
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
      constants[50] = float(REXCVAR_GET(skate3_native_render_scene_refl_mode));
      constants[51] = float(REXCVAR_GET(skate3_native_render_scene_refl_lod));
      // misc.x (spare on opaque fam 5/6): both constant normal-tilt trims,
      // fixed-point packed (each mapped to 0..999 around 500; float-exact).
      // Only when the EXACT branch will run (same gate as cam_pos.w = -fam)
      // - the legacy fallback reads misc.x as the transparent/water flag.
      // Fam 13 has no normal map (its PS reflects off the vertex normal),
      // so its misc.x stays 0.
      if (debug_mode == 0 && scene.shadow_valid && item.env_family != 13) {
        double bx = REXCVAR_GET(skate3_native_render_scene_refl_bias_x);
        double by = REXCVAR_GET(skate3_native_render_scene_refl_bias_y);
        if (REXCVAR_GET(skate3_native_render_scene_refl_bias_auto) &&
            item.detail_tex != 0) {
          // Derive the fold from the material's own detail texture: its
          // first BC1 block decoded with hardware bit replication, texels
          // averaged, folded as 2*d - 1. Cached per texture object; the
          // packed-tile quirk of <=16px textures is benign here (neighbor
          // packed mips of a constant texture hold the same constant).
          // Implausible results (non-DXT1 / unreadable / |fold| > 0.1)
          // keep the cvar values.
          static std::unordered_map<uint32_t, std::pair<float, float>> fold_cache;
          auto fit = fold_cache.find(item.detail_tex);
          if (fit == fold_cache.end()) {
            std::pair<float, float> fold{float(bx), float(by)};
            uint32_t raw[6];
            if (GuestTryCopy(raw, base + item.detail_tex + 7 * 4, sizeof(raw))) {
              const uint32_t w0 = SwapU32(raw[0]);
              const uint32_t w1 = SwapU32(raw[1]);
              uint8_t block[8];
              if ((w0 & 3) == 2 && (w1 & 0x3F) == 0x12 &&
                  GuestTryCopy(block,
                               base + (0xA0000000u | ((w1 >> 12) << 12)), 8)) {
                uint8_t le[8];
                for (int k = 0; k < 8; k += 2) {  // k_8in16 guest endianness
                  le[k] = block[k + 1];
                  le[k + 1] = block[k];
                }
                uint8_t px[16][4];
                DecodeBc1Block(le, px);
                float ax = 0.0f, ay = 0.0f;
                for (int k = 0; k < 16; ++k) {
                  ax += px[k][0];
                  ay += px[k][1];
                }
                const float fx = (ax / 16.0f) * (2.0f / 255.0f) - 1.0f;
                const float fy = (ay / 16.0f) * (2.0f / 255.0f) - 1.0f;
                if (std::fabs(fx) < 0.1f && std::fabs(fy) < 0.1f) {
                  fold = {fx, fy};
                }
                REXLOG_INFO(
                    "native-scene: detail fold tex={:08X} = ({:+.6f}, {:+.6f})",
                    item.detail_tex, fx, fy);
              }
            }
            fit = fold_cache.emplace(item.detail_tex, fold).first;
          }
          bx = fit->second.first;
          by = fit->second.second;
        }
        const auto pack_trim = [](double v) {
          int i = int(std::lround(v * 1000.0)) + 500;
          return std::clamp(i, 0, 999);
        };
        constants[48] = float(pack_trim(bx) + 1000 * pack_trim(by));
      }
    }
    // CAS skin custom-graphic placement (see DrawItem::char_skin): c190.xyz
    // in mat_tint.xyz, c191.xyz in mat_tint.w + misc.zw (all unused by the
    // character branch, which returns before the legacy mat_tint tint).
    if (face_inj != nullptr) {
      constants[50] = item.face_inj_amount;
    }
    if (skin_decal != nullptr) {
      constants[40] = item.char_cg[0];
      constants[41] = item.char_cg[1];
      constants[42] = item.char_cg[2];
      constants[43] = item.char_cg[3];
      constants[50] = item.char_cg[4];
      constants[51] = item.char_cg[5];
    }
    if (shift_graphic_tex != nullptr || cloth_stamp_tex != nullptr) {
      constants[40] = item.char_cg[0];
      constants[41] = item.char_cg[1];
      constants[42] = item.char_cg[2];
      constants[43] = item.char_cg[3];
      constants[50] = item.char_cg[4];
      constants[51] = item.char_cg[5];
      if (shift_graphic_tex != nullptr) {
        constants[47] = 7.0f;  // overlay.w selects the CAC shift graphic.
      } else {
        constants[48] = 1.0f;  // misc.x selects the cacstamp_cloth graphic.
      }
    }
    // cam_pos.w = -40 selects the exact sky branch: the game computes the
    // sun glow inside the dome shader (see the PS sky branch). Needs the
    // frame's captured sky sun rows AND the 1D gradient bound above; falls
    // back to the legacy fullbright dome otherwise. The sky item never uses
    // mat_tint/overlay/misc, so those root constants carry the sun rows.
    if (debug_mode == 0 && item.unlit && sky_sun_bound && scene.sky_sun_valid) {
      constants[39] = -40.0f;
      constants[40] = scene.sky_sun[0];  // mat_tint.xyz = sun direction
      constants[41] = scene.sky_sun[1];
      constants[42] = scene.sky_sun[2];
      constants[43] = scene.sky_height;  // mat_tint.w = dome viewpos Y
      constants[44] = scene.sky_sun[3];  // overlay.x = sun angular scale
      constants[45] = scene.sky_sun[4];  // overlay.y = pre-tone multiplier
      constants[46] = 0.0f;
      constants[47] = 0.0f;
      constants[49] = scene.sky_sun[5];  // misc.y = scene exposure
    }
    // SSR: record reflective items, env fams 5/6/13 on their exact branch
    // with live spec masks (the reflection mask rides t4.z), plus water,
    // for the reflection G-buffer pass after the scene pass.
    if (ssr_on && !item.skinned &&
        (item.water ||
         ((item.env_family == 5 || item.env_family == 6 ||
           item.env_family == 13) &&
          constants[39] < -4.5f && constants[47] > 2.5f))) {
      SsrGbufItem si;
      std::memcpy(si.constants, constants, sizeof(constants));
      si.t3 = macro_tex->srv;
      si.t4 = decal_tex->srv;
      si.t5 = normal_paired ? pair_normal->srv : g_r.white.srv;
      si.mesh = item.mesh;
      si.fingerprint = item.fingerprint;
      si.item = &item;
      ssr_items.push_back(si);
    }
    // Fam 16 (transparentenvironment): mat_tint = c12.xyz light + c13.y
    // material multiplier, overlay.xyz = c11.xyz spec tint (overlay.w keeps
    // the spec-mask-bound flag), misc = c9.x exposure, c10.w / c11.w spec
    // power scale/bias. Written last, over the fog/transparent packing.
    // Fam 17 (trafficlight): mat_tint = the game's lamp states (VS c7.xyzw,
    // the VS picks each vertex's lit/unlit cell from them), overlay.x = the
    // emissive scale (PS c3.y). Neither row is read otherwise on this exact
    // branch.
    if (constants[39] < -16.5f && constants[39] > -17.5f) {
      std::memcpy(constants + 40, item.traffic_c7, 4 * sizeof(float));
      constants[44] = item.traffic_c3y;
    }
    // Fams 12 / 18 (base / transparent incandescent): overlay.x = the draw's
    // PS c3.y (material multiplier), overlay.y = its PS c2.x (the exposure
    // in its tone curve); 0 = not captured (shader keeps 1 / the frame's
    // exposure). overlay.zw are not read on this branch except overlay.w by
    // the alpha-test gate, which stays as staged.
    if ((constants[39] < -11.5f && constants[39] > -12.5f) ||
        (constants[39] < -17.5f && constants[39] > -18.5f)) {
      constants[44] = item.incand_mult;
      constants[45] = item.incand_expo;
    }
    if (constants[39] < -15.5f && constants[39] > -16.5f) {
      constants[40] = scene.ts_rows[0];
      constants[41] = scene.ts_rows[1];
      constants[42] = scene.ts_rows[2];
      constants[43] = scene.ts_rows[3];
      constants[44] = scene.ts_rows[4];
      constants[45] = scene.ts_rows[5];
      constants[46] = scene.ts_rows[6];
      constants[48] = scene.ts_rows[7];
      constants[49] = scene.ts_rows[8];
      constants[50] = scene.ts_rows[9];
      // misc.w = 1: the game blends this draw, so the PS writes its a^2
      // alpha (0 = the alpha-tested opaque route, alpha 1).
      {
        const int m = GameBlendMode(scene, item);
        constants[51] = (m == 2 || m == 3 || m == 5) ? 1.0f : 0.0f;
      }
    }
    // Fam 15 (advertisement): overlay.xyz = lightmap floor (c6.z), material
    // multiplier (c10.y), exposure (c9.x). Written last: the shared env
    // constant setup above fills these slots with macro-overlay values.
    if (constants[39] < -14.5f && constants[39] > -15.5f) {
      // The item's own rows when captured (DrawItem::ad_rows); the frame
      // rows are one advertisement draw's.
      const float* ar = item.ad_seen ? item.ad_rows : scene.ad_rows;
      constants[44] = ar[0];
      constants[45] = ar[1];
      constants[46] = ar[2];
    }
    // Particles (lionREC_Render quad lists): cam_pos.w = -50 selects the
    // exact particle branch in the scene PS.
    if (item.cloth_quads && debug_mode == 0) {
      constants[39] = -50.0f;
    }
    if (prof_items) {
      di_t3 = PerfClock::now();
    }
    // misc.y = decal2 second layer (see SampleDecalArt2): 256 + its tf5
    // addressing code when bound, else 0. misc.y is unread on the opaque
    // fam 3 exact branch otherwise.
    if (decal2_tex != nullptr && is_decal && item.env_family == 3 &&
        constants[48] < 0.5f) {
      constants[49] = 256.0f + float(item.decal2_addr & 0xFFu);
    }
    // Hall of Meat (character fam 8, ShadeHom): mat_tint = PS c2 (tint rgb
    // + fresnel power), overlay = (c7.x, c7.y, c1.y, 0). The skinning
    // inputs (tint.g, palette) stay the character ones.
    if (item.hom && item.hom_seen && debug_mode == 0) {
      constants[39] = 8.0f;
      std::memcpy(constants + 40, item.hom_c2, 4 * sizeof(float));
      constants[44] = item.hom_c7[0];
      constants[45] = item.hom_c7[1];
      constants[46] = item.hom_c1y;
      constants[47] = 0.0f;
    }
    // One-pass VR stereo: world._44 = 2 makes vs_main draw instance 1 as
    // the right eye (copied into the stencil / foreground mark re-draws
    // below with the rest of the constants).
    if (vr_stereo) {
      constants[15] = 2.0f;
    }
    if (skate3::vr::FirstPersonActive() && scene.player_entity != 0) {
      skate3::native_entity::CtxInfo owner;
      if (skate3::native_entity::LookupCtx(item.ctx, &owner) && owner.entity == scene.player_entity) {
        // Homogeneous world-column lane: xyz transforms never read this.
        // Used only in the main scene PS, so shadow casters stay intact.
        constants[11] = 1.0f;
      }
    }
    const uint32_t draw_instances = vr_stereo ? 2u : 1u;
    cmd->SetRootConstants(0, 52, constants, 0);

    cmd->SetTexture(1, diffuse->srv);
    cmd->SetTexture(2, (skin_decal != nullptr ? skin_decal
                        : face_inj != nullptr ? face_inj
                        : lightmap != nullptr ? lightmap
                                              : &g_r.white)
                           ->srv);
    cmd->SetTexture(4, macro_tex->srv);
    // t4 override: the dynobj world-shadow map rides the free first entry
    // of the t4/t5 pair table (flag 8: dynobj draws never bind decal art
    // or spec masks there).
    nrhi::TextureView* t4_view =
        ((v2_flags & 8u) != 0 || lw_ws) ? g_r.world_shadow_srv : decal_tex->srv;
    // water.default rides the game's own tf5 mask in the paired t5 slot
    // (water binds no decal/normal pair of its own). Its .x gates the sun
    // specular and its .z the cube reflection -- the patchy foam.
    const GuestTexture* water_mask_view = nullptr;
    if (waterdef_exact && item.water_mask != 0) {
      water_mask_view = resolve_texture(item.water_mask, 5);
      if (water_mask_view == &g_r.white) {
        water_mask_view = nullptr;
      }
    }
    if (water_mask_view != nullptr) {
      cmd->SetTexturePair(5, t4_view, water_mask_view->srv);
    } else if (skin_inj != nullptr) {
      cmd->SetTexturePair(5, t4_view, skin_inj->srv);
    } else if (normal_paired || t4_view != decal_tex->srv) {
      cmd->SetTexturePair(5, t4_view,
                          normal_paired ? pair_normal->srv : g_r.white.srv);
    } else {
      cmd->SetTexture(5, decal_tex->srv);
    }
    // t6 (cube) shares its table with t7 (shadow atlas); re-pair both.
    cmd->SetTexturePair(7, cube_tex->srv,
                        shadow_ready ? g_r.shadow_srv_final : g_r.white.srv);
    // World-shading v2 material pair (t8 detail + t9 decal spec); the exact
    // ocean rides the same pair (t8 = second PCA component, t9 = overlay).
    // water.default refracts the game's own resolved framebuffer copy,
    // which it binds at sampler 3; ride it in the t9 slot (water carries no
    // v2 spec2 map) so no new root-signature entry is needed.
    const GuestTexture* refract_view = nullptr;
    if (waterdef_exact && item.refract_tex != 0) {
      refract_view = resolve_texture(item.refract_tex, 9);
      if (refract_view == &g_r.white) {
        refract_view = nullptr;
      }
    }
    nrhi::TextureView* t10_want =
        (constants[39] > 0.5f && char_t10_view != nsm_view) ? char_t10_view : nsm_view;
    // Park pieces read the game's own world terms there (see ShadePark):
    // diffuse / decal (dynobj 4 / 6) its skate.Park world-shadow map (tf2),
    // alphatest (dynobj 5) the world-shade map like dynamicobject (tf1).
    // The world's `_skatepark` receivers read the same map (their world-
    // family branches sample no static sun map at t10).
    const bool dmo_world = constants[39] < -0.5f && constants[39] > -19.5f &&
                           std::fabs(constants[39] - std::round(constants[39])) > 0.1f;
    if ((item.dynobj == 4 || item.dynobj == 6 || dmo_world) && g_r.park_ws_srv != nullptr &&
        g_r.park_ws_primed && g_r.park_ws_in_srv) {
      t10_want = g_r.park_ws_srv;
    } else if (item.dynobj == 5 && WorldShadeReady(scene)) {
      t10_want = g_r.world_shadow_srv;
    }
    if (v2_flags != 0 || ocean_n2 || ocean_ov || refract_view != nullptr ||
        waterdef_n2 != nullptr || decal2_tex != nullptr || shift_graphic_tex != nullptr ||
        cloth_stamp_tex != nullptr) {
      // Full-table bind: re-pairing only t8/t9 would reset the t10 static
      // sun-shadow map to the backend fallback. The fifth entry (t12) is the
      // decal2 second art layer or the CAC shift graphic; each branch uses
      // its own per-draw selector.
      nrhi::TextureView* t8_views[5] = {
          (waterdef_n2 != nullptr
               ? waterdef_n2
               : (v2_detail != nullptr ? v2_detail : &g_r.white))
              ->srv,
          (refract_view != nullptr
               ? refract_view
               : (v2_spec2 != nullptr ? v2_spec2 : &g_r.white))
              ->srv,
          t10_want, deck_view,
          cloth_stamp_tex != nullptr
              ? cloth_stamp_tex->srv
              : (shift_graphic_tex != nullptr
                     ? shift_graphic_tex->srv
                     : (decal2_tex != nullptr ? decal2_tex->srv : g_r.white.srv))};
      cmd->SetTextures(8, t8_views, 5);
      t10_bound = t10_want;
    }
    // t10 per draw kind (see char_t10_view): the character families read
    // the world-shade map there, everything else the static sun map.
    if (t10_want != t10_bound) {
      nrhi::TextureView* t8v[4] = {g_r.white.srv, g_r.white.srv, t10_want, deck_view};
      cmd->SetTextures(8, t8v, 4);
      t10_bound = t10_want;
    }
    // ROPA shape blend (see RendererState::ropa_shapes): combine the shape
    // generations with the kernel weights InterpolateDynamicItems computed
    // (the SAME 8-tap boxcar / pair-lerp the body bones and garment world
    // took this frame) into the per-frame ropa upload ring and draw from
    // it; the stepped cloth shape against the interpolated body was the
    // tee jelly/clip-through, and a filter-MISMATCHED blend (plain lerp vs
    // boxcar body) kept a period-scaled residue of it. Positions/normals
    // (floats 0..6) blend; packed attributes (blend words, uvs: floats
    // 7..13) copy from the newest generation present. Generations missing
    // from the ring (evicted / decode in flight) renormalize over what IS
    // present when at least half the kernel's weight survives.
    VbBinding item_vbv = buffers.vb_view;
    if (item.ropa && item.shape_count > 0 &&
        REXCVAR_GET(skate3_native_render_scene_ropa_blend)) {
      const std::vector<float>* gv[DrawItem::kShapeGens] = {};
      float gw[DrawItem::kShapeGens] = {};
      int ng = 0;
      float total = 0.0f;
      uint64_t newest_seq = 0;
      const std::vector<float>* newest = nullptr;
      // Generations must match the RESIDENT decode's extent exactly; the
      // index buffer references that many vertices. After a re-stream/
      // outfit swap the ring briefly holds stale-size generations; binding
      // one against the current IB reads past the bound VB and collapses
      // triangles (a momentary partial-invisible blink). Stale sizes drop
      // out here; if too much kernel weight is stale, the raw resident VB
      // draws instead (always self-consistent).
      const size_t want_floats =
          size_t(buffers.vb_view.size_bytes) / sizeof(float);
      const auto rit = g_r.ropa_shapes.find(item.mesh);
      if (rit != g_r.ropa_shapes.end() && !rit->second.empty()) {
        // Sim-sleep gate: generations grossly older than the ring's newest
        // were recorded before a cloth-sim sleep (continuous streams span
        // ~114 ms end to end). Blending one against the fresh drape draws
        // the garment where/how the character USED to stand, the
        // detached/perpendicular class. Dropped generations shed their
        // kernel weight; if too much weight is stale the raw resident VB
        // draws instead (self-consistent current shape).
        const double ring_newest_t = rit->second.back().t;
        for (int k = 0; k < item.shape_count; ++k) {
          for (const RendererState::RopaGen& g : rit->second) {
            if (g.seq != item.shape_seq[k]) {
              continue;
            }
            if (g.verts.size() != want_floats) {
              break;  // stale-size generation (re-stream/outfit swap)
            }
            if (ring_newest_t - g.t > 0.35) {
              static std::atomic<uint32_t> s_stale{0};
              const uint32_t ln =
                  s_stale.fetch_add(1, std::memory_order_relaxed);
              if (ln < 16 || (ln & 1023u) == 0) {
                REXLOG_DEBUG(
                    "native-scene: ropa shape STALE gen dropped mesh={:08X} "
                    "age={:.2f}s seq_gap={} (n={})",
                    item.mesh, ring_newest_t - g.t,
                    rit->second.back().seq - g.seq, ln);
              }
              break;
            }
            gv[ng] = &g.verts;
            gw[ng] = item.shape_w[k];
            total += item.shape_w[k];
            ++ng;
            if (g.seq >= newest_seq) {
              newest_seq = g.seq;
              newest = &g.verts;
            }
            break;
          }
        }
      }
      const uint32_t region =
          uint32_t(frame_number % RendererState::kBoneRegions) *
          RendererState::kRopaRegionSize;
      const uint32_t bytes = uint32_t(want_floats * sizeof(float));
      if (ng > 0 && total >= 0.5f && newest != nullptr &&
          buffers.vb_view.stride == 56 &&
          g_r.ropa_ring_offset + bytes <= RendererState::kRopaRegionSize) {
        float* dst = reinterpret_cast<float*>(g_r.ropa_ring_cpu + region +
                                              g_r.ropa_ring_offset);
        const float inv = 1.0f / total;
        for (size_t v = 0; v + 14 <= want_floats; v += 14) {
          float blend7[7] = {};
          for (int k = 0; k < ng; ++k) {
            const float* src = gv[k]->data() + v;
            const float wk = gw[k] * inv;
            for (int f = 0; f < 7; ++f) {
              blend7[f] += src[f] * wk;
            }
          }
          // One store per float: dst is write-combined upload memory.
          std::memcpy(dst + v, blend7, sizeof(blend7));
          std::memcpy(dst + v + 7, newest->data() + v + 7, 7 * sizeof(float));
        }
        item_vbv.buffer = g_r.ropa_ring;
        item_vbv.offset = region + g_r.ropa_ring_offset;
        item_vbv.size_bytes = bytes;
        item_vbv.stride = buffers.vb_view.stride;
        g_r.ropa_ring_offset += (bytes + 255u) & ~255u;
        g_ropa_blend_drawn.fetch_add(1, std::memory_order_relaxed);
      } else {
        // Fallback to the raw resident shape. Rate-limited detail log: an
        // ALTERNATION of these with blended frames renders the garment
        // hopping between the play-clock drape and the zero-lag drape.
        g_ropa_blend_miss.fetch_add(1, std::memory_order_relaxed);
        // Miss reasons (DIAGNOSTIC): 0 no ring for the mesh, 1 kernel
        // generations absent from the ring, 2 surviving weight < 0.5,
        // 3 layout stride, 4 upload ring full.
        static uint64_t s_reason[5] = {};
        const bool ring_full =
            g_r.ropa_ring_offset + bytes > RendererState::kRopaRegionSize;
        ++s_reason[rit == g_r.ropa_shapes.end() || rit->second.empty() ? 0
                   : ng < item.shape_count                               ? 1
                   : total < 0.5f                                        ? 2
                   : buffers.vb_view.stride != 56                        ? 3
                   : ring_full                                           ? 4
                                                                         : 2];
        static int64_t s_reason_log_ms = 0;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        if (now_ms - s_reason_log_ms > 5000) {
          s_reason_log_ms = now_ms;
          REXLOG_INFO(
              "native-scene: ropa blend drawn={} miss={} [no_ring={} gen_absent={} "
              "low_weight={} stride={} ring_full={}] last: mesh={:08X} sc={} ng={} "
              "total={:.2f} ring={}",
              g_ropa_blend_drawn.load(), g_ropa_blend_miss.load(), s_reason[0], s_reason[1],
              s_reason[2], s_reason[3], s_reason[4], item.mesh, item.shape_count, ng, total,
              rit != g_r.ropa_shapes.end() ? rit->second.size() : size_t(0));
        }
        static std::atomic<uint32_t> s_blend_miss_log{0};
        const uint32_t ln =
            s_blend_miss_log.fetch_add(1, std::memory_order_relaxed);
        if (ln < 64 || (ln & 63u) == 0) {
          REXLOG_DEBUG(
              "native-scene: ropa blend MISS mesh={:08X} sc={} ng={} "
              "total={:.2f} stride={} ring={} ring_off={} (n={})",
              item.mesh, item.shape_count, ng, total, buffers.vb_view.stride,
              rit != g_r.ropa_shapes.end() ? rit->second.size() : size_t(0),
              g_r.ropa_ring_offset, ln);
        }
      }
    }
    // CAS subsurface scattering (see DrawItem::sss), the game's own order
    // per piece: unwrap (texture-space lighting, 128x128) -> jitter blur ->
    // this main draw sampling the blur at t8 and the blurmask at t9. Every
    // binding the unwrap needs (constants, bones, char rows, normal map,
    // shadow maps) is this draw's own, staged above.
    if (sss_mask_tex != nullptr && scene_pso_path) {
      const auto to = [&](nrhi::Texture* t, bool& srv, bool want_srv) {
        if (srv != want_srv) {
          cmd->Barrier(t, want_srv ? nrhi::ResourceState::kRenderTarget
                                   : nrhi::ResourceState::kPixelShaderResource,
                       want_srv ? nrhi::ResourceState::kPixelShaderResource
                                : nrhi::ResourceState::kRenderTarget);
          srv = want_srv;
        }
      };
      const nrhi::Viewport svp{0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 1.0f};
      const nrhi::Rect ssc{0, 0, 128, 128};
      // 1) unwrap
      to(g_r.sss_unwrap, g_r.sss_unwrap_srv, false);
      cmd->FlushBarriers();
      const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      cmd->ClearRenderTarget(g_r.sss_unwrap, zero4);
      cmd->SetRenderTargets(g_r.sss_unwrap, nullptr);
      cmd->SetViewport(svp);
      cmd->SetScissor(ssc);
      cmd->SetPipeline(g_r.pso_skin_unwrap);
      {
        nrhi::TextureView* t8v[4] = {g_r.white.srv, g_r.white.srv, char_t10_view, deck_view};
        cmd->SetTextures(8, t8v, 4);
      }
      cmd->SetVertexBuffer(item_vbv.buffer, item_vbv.offset, item_vbv.size_bytes,
                           item_vbv.stride);
      cmd->SetIndexBuffer(buffers.ib_view.buffer, buffers.ib_view.offset,
                          buffers.ib_view.size_bytes);
      for (const DrawEntry& draw : item.draws) {
        if (draw.prim == 4) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        } else if (draw.prim == 6) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
        } else {
          continue;
        }
        cmd->DrawIndexed(draw.index_count, draw.start_index, draw.base_vertex);
      }
      to(g_r.sss_unwrap, g_r.sss_unwrap_srv, true);
      // 2) jitter blur (c0.xy in overlay.xy for this pass only)
      to(g_r.sss_blur, g_r.sss_blur_srv, false);
      cmd->FlushBarriers();
      cmd->SetRenderTargets(g_r.sss_blur, nullptr);
      cmd->SetPipeline(g_r.pso_skin_blur);
      {
        float bc[52];
        std::memcpy(bc, constants, sizeof(bc));
        bc[44] = sss_c0[0];
        bc[45] = sss_c0[1];
        cmd->SetRootConstants(0, 52, bc, 0);
        nrhi::TextureView* t8v[4] = {g_r.sss_unwrap_view, g_r.white.srv, char_t10_view, deck_view};
        cmd->SetTextures(8, t8v, 4);
      }
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      cmd->Draw(3, 0);
      to(g_r.sss_blur, g_r.sss_blur_srv, true);
      cmd->FlushBarriers();
      // 3) restore this draw's scene state, bind the blur + mask
      cmd->SetRenderTargets(scene_color, use_depth ? g_r.depth : nullptr);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(scene_pso_bound);
      cmd->SetRootConstants(0, 52, constants, 0);
      {
        nrhi::TextureView* t8v[4] = {g_r.sss_blur_view, sss_mask_tex->srv, char_t10_view, deck_view};
        cmd->SetTextures(8, t8v, 4);
      }
    }
    if (stencil_mark_on) {
      const int mark = GameStencilWrite(scene, item);
      if (mark > 0) {
        StencilMarkItem sm;
        std::memcpy(sm.constants, constants, sizeof(sm.constants));
        sm.constants[32] = float(mark) / 255.0f;  // tint.r: vs_main never reads it
        sm.bone_offset = bone_srv_offset;
        sm.vb = item_vbv;
        sm.ib = buffers.ib_view;
        sm.item = &item;
        stencil_items.push_back(sm);
      }
    }
    // Recorded once (on the outer pass) with its exact constants/streams.
    if (item.foreground && use_depth && forced_pso == g_r.pso_fg_outer) {
      FgMarkItem fm;
      std::memcpy(fm.sm.constants, constants, sizeof(fm.sm.constants));
      fm.sm.constants[32] = 1.0f;  // tint.r = the mark value (vs_main never reads it)
      fm.sm.bone_offset = bone_srv_offset;
      fm.sm.vb = item_vbv;
      fm.sm.ib = buffers.ib_view;
      fm.sm.item = &item;
      fm.clip_srv = (item.dynobj == 2 || item.dynobj == 5) ? diffuse->srv : nullptr;
      fg_mark_items.push_back(fm);
    }
    cmd->SetVertexBuffer(item_vbv.buffer, item_vbv.offset, item_vbv.size_bytes,
                         item_vbv.stride);
    cmd->SetIndexBuffer(buffers.ib_view.buffer, buffers.ib_view.offset,
                        buffers.ib_view.size_bytes);
    for (const DrawEntry& draw : item.draws) {
      if (draw.prim == 4) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      } else if (draw.prim == 6) {
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
      } else {
        continue;
      }
      cmd->DrawIndexedInstanced(draw.index_count, draw_instances, draw.start_index,
                                draw.base_vertex);
      ++drawn;
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ++ring_frame->items[rit->second].drawn;
        }
      }
    }
    // Stage attribution for draws that reached submission (early returns
    // above skip this; their time still lands in the caller's in/out
    // windows).
    if (prof_items && di_t3 != PerfClock::time_point{}) {
      const auto di_t4 = PerfClock::now();
      const auto ns = [](PerfClock::time_point a, PerfClock::time_point b) {
        return uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
      };
      g_pw_di_mesh.Add(ns(di_t0, di_t1));
      g_pw_di_tex.Add(ns(di_t1, di_t2));
      g_pw_di_const.Add(ns(di_t2, di_t3));
      g_pw_di_submit.Add(ns(di_t3, di_t4));
    }
  };
  // Profiling-mode consumption of the occlusion-grid readback ring (filled
  // by the perf-items reduce in ApplySsaoPass on previous frames): newest
  // completed slot wins, every completed slot retires. A grid that stops
  // refreshing (AO off, loading flows) goes invalid instead of classifying
  // against ancient depth.
  const bool occl_cull_wanted =
      REXCVAR_GET(skate3_native_render_scene_occlusion_cull) &&
      !skate3::vr::Active();  // the grid holds one eye's depth
  if ((prof_items || occl_cull_wanted) && !g_r.occl_failed &&
      g_r.occl_readback_ptr[1] != nullptr) {
    const uint64_t occl_done = context.device->CompletedSubmission();
    int newest = -1;
    for (int i = 0; i < 2; ++i) {
      if (g_r.occl_pending[i] && g_r.occl_submission[i] < occl_done &&
          (newest < 0 ||
           g_r.occl_submission[i] > g_r.occl_submission[newest])) {
        newest = i;
      }
    }
    if (newest >= 0) {
      context.device->InvalidateForRead(
          g_r.occl_readback[newest], 0,
          uint64_t(RendererState::kOcclRowPitch) * RendererState::kOcclGridH);
      g_r.occl_grid.resize(size_t(RendererState::kOcclGridW) *
                           RendererState::kOcclGridH);
      for (uint32_t row = 0; row < RendererState::kOcclGridH; ++row) {
        std::memcpy(&g_r.occl_grid[size_t(row) * RendererState::kOcclGridW],
                    g_r.occl_readback_ptr[newest] +
                        size_t(row) * RendererState::kOcclRowPitch,
                    RendererState::kOcclGridW * sizeof(float));
      }
      std::memcpy(g_r.occl_grid_vp, g_r.occl_vp[newest],
                  sizeof(g_r.occl_grid_vp));
      std::memcpy(g_r.occl_grid_cam, g_r.occl_cam[newest],
                  sizeof(g_r.occl_grid_cam));
      g_r.occl_grid_frame = frame_number;
      g_r.occl_grid_valid = true;
      for (int i = 0; i < 2; ++i) {
        if (g_r.occl_pending[i] && g_r.occl_submission[i] < occl_done) {
          g_r.occl_pending[i] = false;
        }
      }
    } else if (g_r.occl_grid_valid &&
               frame_number - g_r.occl_grid_frame > 240) {
      g_r.occl_grid_valid = false;
    }
  }
  // Per-draw wrapper for the profiling mode: attribute each item's total
  // draw_item cost to its visibility class. Out-of-frustum items and
  // occlusion-proven-hidden items cost CPU here but can never contribute
  // pixels; a large share in either class is a visibility-culling gap, not
  // a scene-density cost.
  // VR eye pass: each eye draws only what its own frustum can see (the game
  // submits for both eyes plus a margin). Statics with trustworthy world
  // bounds only: skinned / cloth poses live in their palettes, and unlit
  // (sky.*) meshes are camera-relative.
  const bool vr_eye_cull = skate3::vr::CurrentEye() >= 0;
  const float* const vr_right_vp = vr_stereo ? skate3::vr::EyeViewProj(1) : nullptr;
  const auto timed_draw = [&](const DrawItem& item) {
    if (vr_eye_cull && !item.skinned && !item.ropa && !item.cloth_quads &&
        item.bones.empty() && !item.unlit &&
        ItemOutsideFrustum(item, scene.view_proj, 1.02f) &&
        (vr_right_vp == nullptr || ItemOutsideFrustum(item, vr_right_vp, 1.02f))) {
      return;
    }
    if (!prof_items) {
      draw_item(item);
      return;
    }
    // Statics only (same gate as the retention pass): a skinned/ropa item's
    // pose lives in its bone palette, its identity world + bind-pose bbox
    // say nothing about where it is; those always classify as visible.
    const bool classifiable = !item.skinned && !item.ropa &&
                              !item.cloth_quads && item.bones.empty();
    const bool off =
        classifiable && ItemOutsideFrustum(item, scene.view_proj, 1.0f);
    const bool occluded =
        classifiable && !off && ItemOccludedByGrid(item, 0.25f);
    uint64_t indices = 0;
    for (const DrawEntry& e : item.draws) {
      indices += e.index_count;
    }
    const auto t0 = PerfClock::now();
    draw_item(item);
    const uint64_t total_ns = perf_ns_since(t0);
    if (off) {
      g_pw_di_out.Add(total_ns);
      g_vis_out_indices.fetch_add(indices, std::memory_order_relaxed);
      if (item.retained) {
        g_vis_out_retained.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (occluded) {
      g_pw_di_occ.Add(total_ns);
      g_vis_occ_indices.fetch_add(indices, std::memory_order_relaxed);
    } else {
      g_pw_di_in.Add(total_ns);
      g_vis_in_indices.fetch_add(indices, std::memory_order_relaxed);
    }
  };

  // Opaque items first; environment.transparent items are deferred to an
  // alpha-blended sub-pass (depth test on, z-write off) drawn back-to-front;
  // interleaved opaque rendering alpha-tested the mist sheets into solid
  // white cloud blobs.
  const auto items_t0 = PerfClock::now();
  const auto view_dist2 = [&](const DrawItem& item) {
    float c[3], w[3];
    for (int a = 0; a < 3; ++a) {
      c[a] = (item.bbox_min[a] + item.bbox_max[a]) * 0.5f;
    }
    for (int a = 0; a < 3; ++a) {
      w[a] = c[0] * item.world[0 * 4 + a] + c[1] * item.world[1 * 4 + a] +
             c[2] * item.world[2 * 4 + a] + item.world[3 * 4 + a];
    }
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
      const float d = w[a] - scene.cam_pos[a];
      d2 += d * d;
    }
    return d2;
  };
  std::vector<const DrawItem*> transparent_items;
  std::vector<const DrawItem*> hom_items;
  // Mid-fade entity items (see CharFadeAlpha / pso_fade): blended but with
  // z-write ON, drawn at the HEAD of the blended sub-pass; they behave like
  // main-pass objects whose glass/hair still composites over them, and depth
  // writes stop their own overlapping pieces (skin under clothes, far-side
  // doors/wheels through the body shell) from double-blending into an x-ray.
  const auto char_fade_zwrite = [&](const DrawItem& it) {
    // environment.transparent draws the game BLENDS with z-write ON (the
    // parked-vehicle wheel cards: blend + ZWRITE 1 + alpha test): blend,
    // write depth and draw at the head of the pass like the game, so the
    // overlapping tyre/hub/far-side cards occlude each other.
    if (debug_mode == 0 && it.env_family == 16) {
      const int m = GameBlendMode(scene, it);
      return (m == 2 || m == 5) && GameDrawZWrite(scene, it);
    }
    return debug_mode == 0 && it.char_rows[14 * 4 + 1] > 0.0f &&
           (it.char_family == 1 || it.char_family == 2 ||
            it.char_family == 3 || it.char_family == 6) &&
           REXCVAR_GET(skate3_native_render_scene_entity_fade) &&
           CharFadeAlphaDrawn(scene, it) < 0.999f;
  };
  // Occlusion cull: statics provably hidden behind already-rendered
  // geometry skip the color pass entirely (every scene.items consumer that
  // must still see them - shadow casters, the world-shadow map, outline,
  // warm-settle - iterates the list itself and is unaffected). Engaged only
  // while the grid's camera matches this frame's within 1 m: the stored-
  // frustum check inside ItemOccludedByGrid already keeps rotation safe
  // (bounds outside the grid's view classify visible), and past 1 m of
  // translation the 1-2 frame-old depth could hide content parallax has
  // revealed. At gameplay speeds the grid refreshes well inside that
  // budget, so the gate only stands the cull down across teleports and
  // camera cuts.
  bool occl_cull_active = !photo_mode && occl_cull_wanted && debug_mode == 0 &&
                          g_r.occl_grid_valid;
  if (occl_cull_active) {
    float cam_d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
      const float d = scene.cam_pos[a] - g_r.occl_grid_cam[a];
      cam_d2 += d * d;
    }
    occl_cull_active = cam_d2 < 1.0f;
  }
  std::vector<uint32_t> culled_ctxs;
  if (occl_cull_active) {
    culled_ctxs.reserve(scene.items.size() / 2);
  }
  // Opaque items draw front-to-back (bbox-center distance): early-z rejects
  // occluded pixels before the heavy material PS runs. The game's sort-list
  // order is by render state, not depth; depth-write LESS_EQUAL makes the
  // final image order-independent, so reordering is safe.
  std::vector<std::pair<float, const DrawItem*>> opaque_items;
  std::vector<const DrawItem*> fg_items;
  std::vector<const DrawItem*> xray_items;
  opaque_items.reserve(scene.items.size());
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    if (ring_frame != nullptr) {
      SceneRingItem ri{};
      ri.ctx = item.ctx;
      ri.mesh = item.mesh;
      ri.diffuse = item.diffuse_tex;
      ri.lightmap = item.lightmap_tex;
      ri.vb_obj = item.vb_obj;
      ri.spec = item.spec_tex;
      ri.macro = item.macro_tex;
      ri.decal_art = item.decal_art;
      ri.wnormal = item.water_normal;
      uint32_t idx_total = 0;
      for (const DrawEntry& e : item.draws) {
        idx_total += e.index_count;
      }
      ri.indices = idx_total;
      ri.env_family = item.env_family;
      ri.char_family = item.char_family;
      ri.flags = uint8_t((item.transparent ? 1u : 0u) | (item.water ? 2u : 0u) |
                         (item.skinned ? 4u : 0u) | (item.retained ? 8u : 0u) |
                         (item.pending ? 16u : 0u) |
                         (item.caster_bank ? 32u : 0u) |
                         (item.decal ? 64u : 0u));
      ring_map.emplace(&item, uint32_t(ring_frame->items.size()));
      ring_frame->items.push_back(ri);
    }
    // Hair with a validated lighting capture joins the sorted alpha
    // sub-pass (strand coverage blend, depth test on / z-write off, the
    // game's own hair render state); without the capture it stays on the
    // legacy opaque path. Vehicle glass (fam 7) blends there too,
    // reflection-only windows at the captured alpha; vehicle bodies (fam 6)
    // stay opaque.
    const bool char_capture_ok = item.char_rows[14 * 4 + 1] > 0.0f;
    const bool hair_blend = item.char_family >= 4 && item.char_family <= 5 &&
                            char_capture_ok;
    const bool glass_blend = item.char_family == 7 && char_capture_ok;
    // character.alpha accessory pieces (sunglass lenses): translucent, alpha
    // from the coverage texture at uv2 (ch_misc.z set by the capture); the
    // game draws them after every opaque character piece (see
    // DrawItem::char_alpha). Gated on the SAME capture validation as hair:
    // without validated rows ch_misc.z is 0 and the item stays opaque.
    const bool cac_alpha_blend = item.char_alpha && char_capture_ok &&
                                 item.char_rows[14 * 4 + 2] > 0.0f;
    // Flicker probes: a hair/character-alpha piece whose pass route flips
    // between consecutive frames, or whose bone palette returns bit-exact
    // to the previous-but-one frame's value (A,B,A,B: genuine motion
    // never round-trips in two rendered frames), is the visible hair
    // flicker.
    if (item.char_family >= 4 || item.char_alpha) {
      struct RouteProbe {
        uint8_t route = 0;
        uint64_t bones_h1 = 0;  // last frame's palette hash
        uint64_t bones_h2 = 0;  // the frame before
      };
      static std::unordered_map<uint64_t, RouteProbe> s_probe;  // render thread
      const uint64_t rk = (uint64_t(item.mesh) << 32) | item.ctx;
      const uint8_t route = uint8_t((hair_blend || cac_alpha_blend) ? 2 : 1);
      uint64_t bones_h = 1469598103934665603ull;
      if (!item.bones.empty()) {
        const uint8_t* bb = reinterpret_cast<const uint8_t*>(item.bones.data());
        const size_t bn = item.bones.size() * sizeof(float);
        for (size_t bi = 0; bi < bn; ++bi) {
          bones_h = (bones_h ^ bb[bi]) * 1099511628211ull;
        }
      }
      auto [rit2, fresh2] = s_probe.try_emplace(rk);
      RouteProbe& pr = rit2->second;
      if (!fresh2 && pr.route != route) {
        g_hair_route_flips.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_flip_logged{0};
        const uint32_t fl = s_flip_logged.fetch_add(1, std::memory_order_relaxed);
        if (fl < 24 || (fl & 511u) == 0) {
          REXLOG_DEBUG(
              "native-scene: hair ROUTE FLIP mesh={:08X} ctx={:08X} fam={} "
              "alpha={} -> route {} rows14=({:.3f},{:.3f},{:.3f}) (n={})",
              item.mesh, item.ctx, item.char_family, item.char_alpha ? 1 : 0,
              route, item.char_rows[14 * 4 + 0], item.char_rows[14 * 4 + 1],
              item.char_rows[14 * 4 + 2], fl);
        }
      }
      if (!fresh2 && !item.bones.empty() && bones_h != pr.bones_h1 &&
          bones_h == pr.bones_h2) {
        g_hair_bone_alternations.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_bone_alt_logged{0};
        const uint32_t bl = s_bone_alt_logged.fetch_add(1, std::memory_order_relaxed);
        if (bl < 24 || (bl & 1023u) == 0) {
          REXLOG_DEBUG(
              "native-scene: hair BONES ALTERNATION mesh={:08X} ctx={:08X} "
              "fam={} bones={} src={} caster_bank={} (n={})",
              item.mesh, item.ctx, item.char_family, item.bones.size() / 12,
              item.dbg_src, item.caster_bank ? 1 : 0, bl);
        }
      }
      pr.route = route;
      pr.bones_h2 = pr.bones_h1;
      pr.bones_h1 = bones_h;
      if (s_probe.size() > 4096) {
        s_probe.clear();
      }
    }
    // Per-entity spawn/distance fade (CharFadeAlpha): the game submits
    // LivingWorld entity draws at alpha 0 through the whole spawn settle
    // (NPCs drop ~1 m to the ground before fading in) and ramps alpha with
    // distance; skip invisible items entirely, blend mid-fade ones.
    // LW-mapped items (lw_alpha >= 0) do not need a validated lighting
    // capture to honor the fade: the entity alpha is authoritative even
    // when the capture chain failed; a spawn-settling NPC is invisible
    // regardless of whether its rows validated this frame.
    const bool entity_fade = debug_mode == 0 && item.char_family != 0 &&
                             (char_capture_ok || item.lw_alpha >= 0.0f) &&
                             REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const float fade_a = entity_fade ? CharFadeAlphaDrawn(scene, item) : 1.0f;
    // One-frame fade-blink guard: a mesh that rendered ~opaque last frame
    // cannot legitimately sit at alpha 0 this frame; the game's spawn/
    // distance fades ramp over ~0.5 s. An opaque->0 step means a garbage/
    // foreign constant row served the alpha this capture (the clone-shared
    // char-rows suspect; observed as part of the tee flickering invisible
    // for a moment). Repair: draw the item OPAQUE this frame (skip the fade
    // skip + the mid-fade blend routing) and log it. The last-alpha map
    // updates from the RAW value, so a persisting alpha 0 (real despawn /
    // spawn settle) only gets one repaired frame and then skips normally.
    // LW-mapped items bypass the blink repair entirely (read AND write):
    // the entity alpha cannot blink; the repair exists for garbage/foreign
    // CAPTURED rows, and its mesh key is clone-shared, which force-drew
    // spawn-settling clones OPAQUE every frame their twin was visible (the
    // "NPC drops out of the sky with no fade" sighting).
    bool fade_blink = false;
    if (item.char_family != 0 && debug_mode == 0 && item.lw_alpha < 0.0f) {
      static std::unordered_map<uint32_t, uint8_t> s_fade_opaque;  // render thread
      uint8_t& was_opaque = s_fade_opaque[item.mesh];
      if (entity_fade && fade_a <= 0.004f && was_opaque) {
        fade_blink = true;
        static std::atomic<uint64_t> s_blinks{0};
        const uint64_t n = s_blinks.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_DEBUG(
              "native-scene: fade BLINK repaired mesh={:08X} fam={} ropa={} "
              "src={} rows13=({:.3f},{:.3f},{:.3f},{:.3f}) "
              "rows14=({:.3f},{:.3f}) (n={})",
              item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
              item.char_rows[13 * 4 + 0], item.char_rows[13 * 4 + 1],
              item.char_rows[13 * 4 + 2], item.char_rows[13 * 4 + 3],
              item.char_rows[14 * 4 + 0], item.char_rows[14 * 4 + 1], n);
        }
      }
      was_opaque = (!entity_fade || fade_a > 0.9f) ? 1 : 0;
      if (s_fade_opaque.size() > 1024) {
        s_fade_opaque.clear();
      }
    }
    if (entity_fade && fade_a <= 0.004f && !fade_blink) {
      if (item.lw_alpha >= 0.0f) {
        g_lw_fade0.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    const bool char_fade_blend = char_fade_zwrite(item) && !fade_blink;
    // environment.reflective_trans (fam 13): blended glass canopies, joins
    // the sorted alpha sub-pass whenever its exact branch is live (same
    // shadow_valid gate as cam_pos.w = -fam; the legacy fallback renders it
    // opaque exactly as before classification).
    const bool refl_trans_blend =
        item.env_family == 13 && scene.shadow_valid;
    const auto stamp_route = [&](uint8_t route) {
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ring_frame->items[rit->second].route = route;
        }
      }
    };
    // Occlusion cull (statics only; same classifiability gate as the
    // profiler): the 1 m depth margin on top of the conservative bbox test
    // keeps reveal edges safe at speed - deeply hidden mass sits many
    // meters behind its occluders, so the margin costs almost none of the
    // win. Ring route 3 = culled. Live-submitted culled ctxs (never
    // retained ones: a re-appended item's ctx may already be freed, and a
    // reused address would filter an unrelated new instance) feed the
    // guest-side dispatch filter via the publication below.
    // (Never the foreground preview object: the game draws it depth-ALWAYS
    // precisely because it sits inside geometry; see DrawItem::foreground.)
    if (occl_cull_active && !item.skinned && !item.ropa && !item.foreground && !item.xray &&
        !item.highlight &&
        !item.cloth_quads && item.bones.empty() &&
        ItemOccludedByGrid(item, 1.0f)) {
      g_occl_culled.fetch_add(1, std::memory_order_relaxed);
      // Never dynamic-entity ctxs: a rigid prop's world matrix (and any
      // pending capture) is only filled in by one of its own draws, so
      // keeping it out of the guest dispatch left it pending and dropped
      // it next frame. The guest z-prepass used to supply that draw; with
      // skate3_native_render_scene_skip_guest_zprepass it no longer does.
      if (!item.retained && item.ctx != 0 && !item.dyn_entity) {
        culled_ctxs.push_back(item.ctx);
      }
      stamp_route(3);
      continue;
    }
    // The exact ocean surface (fam 31) is an OPAQUE draw in the game (its
    // PS outputs alpha 0 and it writes z-prepass depth); routing it through
    // the sorted alpha sub-pass would composite the blended horizon sheet
    // (ocean.reflection) UNDER it. The legacy fallback also renders opaque,
    // so the route holds while textures/rows are still resolving.
    const bool ocean_opaque = item.water && item.water_ocean == 1 &&
                              scene.shadow_valid && scene.ocean_valid;
    // water.default (fam 33) is OPAQUE for the same reason: its PS ends
    // `max oC0._001, r0, r0`, which writes w = 1. Leaving it in the sorted
    // alpha sub-pass meant it shared that pass with the skater's hair --
    // both with z-write OFF -- so whenever the pool sorted after the skater
    // the opaque water drew straight over the hair and clipped the
    // silhouette. In the opaque pass the z-buffer resolves it instead.
    // water.alpha is excluded: its PS writes a real alpha (see
    // DrawItem::water_alpha), so it blends in the sorted pass exactly as the
    // game draws it. Only the opaque water_defaultPS variant, whose PS
    // forces w = 1, belongs in the opaque pass.
    const bool waterdef_opaque = item.water && !item.water_flowing &&
                                 !item.water_alpha &&
                                 item.water_ocean == 0 &&
                                 scene.shadow_valid && scene.waterdef_valid;
    // environment.transparent (fam 16, transparentenvironment_defaultPS)
    // is OPAQUE in the game despite the name: captured render states for its
    // draws are ZENABLE 1, ZWRITEENABLE 1, ALPHABLENDENABLE 0,
    // ALPHATESTENABLE 1 (the PS's a^2 output only feeds the alpha test).
    // Sorted into the z-write-off blend pass, overlapping cut-out cards (the
    // parked-vehicle wheels: tyre, hub and far-side wheel cards) blended in
    // bbox order instead of occluding each other.
    // Per draw, not per material: the same shader also draws additively
    // blended sky cloud sheets, which the opaque routing turned into black
    // cutouts. Follow the game's recorded blend state; unrecorded keeps the
    // opaque default.
    const int te_mode = item.env_family == 16 ? GameBlendMode(scene, item) : 0;
    if (item.env_family == 16 && te_mode == 4 && debug_mode == 0) {
      continue;  // the game writes no color for this draw
    }
    const bool transenv_opaque = item.env_family == 16 && item.transparent &&
                                 debug_mode == 0 && scene.shadow_valid &&
                                 scene.ts_valid && (te_mode == 0 || te_mode == 1);
    // Hall of Meat x-ray: drawn last, in its own pass (see hom_items).
    if (item.hom && item.hom_seen && debug_mode == 0 && use_depth) {
      hom_items.push_back(&item);
      continue;
    }
    if ((item.transparent || item.water || hair_blend || glass_blend ||
         cac_alpha_blend || refl_trans_blend || char_fade_blend) &&
        !ocean_opaque && !waterdef_opaque && !transenv_opaque && debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
        stamp_route(2);
      }
      continue;
    }
    if (item.foreground && g_r.pso_fg_inner != nullptr && g_r.pso_fg_outer != nullptr &&
        use_depth) {
      fg_items.push_back(&item);  // drawn after the transparents (game order)
      continue;
    }
    if (item.xray && g_r.pso_xray_a != nullptr && g_r.pso_xray_b != nullptr && use_depth) {
      xray_items.push_back(&item);  // likewise after the transparents
      continue;
    }
    opaque_items.emplace_back(view_dist2(item), &item);
    stamp_route(1);
  }
  // Publish the culled-ctx set every rendered frame, including empty ones:
  // the guest filter must clear promptly when the cull stands down. Ctxs
  // the build-side skip left out of this scene stay culled by re-inclusion
  // (they could not be re-tested; their staggered rebuild frame is when
  // they re-enter scene.items and get a fresh verdict) - but only while
  // the cull is ACTIVE: on stand-down (debug views, the camera-motion
  // gate, teleports) the set must drain so every skipped item resumes
  // building on the next guest frame. (Never from a portrait job: its
  // subset scene says nothing about the frame's visibility.)
  if (!photo_mode) {
    if (occl_cull_active) {
      culled_ctxs.insert(culled_ctxs.end(), scene.occl_build_skipped.begin(),
                         scene.occl_build_skipped.end());
    }
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    std::sort(culled_ctxs.begin(), culled_ctxs.end());
    std::lock_guard<std::mutex> lock(g_occl_pub_mutex);
    g_occl_pub_ctxs.swap(culled_ctxs);
    g_occl_pub_ms.store(now_ms, std::memory_order_relaxed);
  }
  if (REXCVAR_GET(skate3_native_render_scene_sort_opaque) && debug_mode == 0) {
    std::stable_sort(opaque_items.begin(), opaque_items.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  // Wireframe depth prepass (see RendererState::pso_wire_depth): the game
  // draws the scene FILLED with no colour writes first, so the line pass
  // that follows is occluded by solid geometry instead of showing every
  // edge in the level through every wall.
  if (g_r.pso_wire_depth != nullptr) {
    // Only OPAQUE STATIC WORLD geometry lays down depth. Measured from the
    // game's own ZPrePass_defaultVS run (capture 1790959265 draws 154-484):
    // every participant is env_family 1-6 / 14 / 15 with dynobj == 0 and
    // skinned == false. Nothing else is in it -- not characters, not
    // dynamic objects, and none of the alpha-cutout families: fam 7
    // (environmentsimple.alphatest) 0 of 80, fams 9/10 (trees) 0 of 23,
    // fam 16 (cut-out cards: wheels, fences) 0 of 11, fam 11 (proxyworld)
    // 0 of 38. Those all stay see-through in wireframe, which is what the
    // emulated reference shows.
    const auto wire_depth_item = [&](const DrawItem& it) {
      if (it.skinned || it.dynobj != 0 || it.transparent || it.water) {
        return false;
      }
      const uint32_t f = it.env_family;
      const bool opaque_world =
          (f >= 1 && f <= 6) || f == 14 || f == 15;
      return opaque_world && GameWireframeItem(scene, it);
    };
    bool wire_any = false;
    for (const auto& [dist, item] : opaque_items) {
      if (wire_depth_item(*item)) {
        wire_any = true;
        break;
      }
    }
    if (wire_any) {
      forced_pso = g_r.pso_wire_depth;
      for (const auto& [dist, item] : opaque_items) {
        if (wire_depth_item(*item)) {
          timed_draw(*item);
        }
      }
      forced_pso = nullptr;
    }
  }
  // One-pass VR stereo: submit the opaque pass in quarters, so the GPU draws
  // one chunk while the CPU records the next (otherwise the whole frame is
  // recorded and replayed before the GPU starts: CPU + GPU in series).
  const size_t opaque_chunk =
      vr_stereo && skate3::vr::FlushEyes() ? std::max<size_t>(opaque_items.size() / 4, 150)
                                           : SIZE_MAX;
  size_t opaque_n = 0;
  for (const auto& [dist, item] : opaque_items) {
    if (opaque_n != 0 && opaque_n % opaque_chunk == 0 &&
        context.device->FlushSubmission()) {
      restore_main_pass();
    }
    ++opaque_n;
    timed_draw(*item);
  }
  if (opaque_chunk != SIZE_MAX && context.device->FlushSubmission()) {
    restore_main_pass();  // the transparent sub-pass sets only its pipelines
  }
  if (!transparent_items.empty() && g_r.pso_transparent != nullptr) {
    std::stable_sort(transparent_items.begin(), transparent_items.end(),
                     [&](const DrawItem* a, const DrawItem* b) {
                       // Fading entities (z-write blend) draw before every
                       // z-write-off blend so hair/glass composite over them.
                       const bool fa = char_fade_zwrite(*a);
                       const bool fb = char_fade_zwrite(*b);
                       if (fa != fb) {
                         return fa;
                       }
                       // Water surfaces (water.alpha canal water, flowing
                       // water) next, before every other blend. Nothing in
                       // this pass writes depth, and these are long
                       // surfaces whose bbox CENTRE often sits nearer the
                       // camera than the skater's head; sorted by centre
                       // distance they drew after the hair and blended over
                       // it wherever the hair was in front. The water lies
                       // under/behind what composites over it in practice.
                       const bool wa = a->water;
                       const bool wb = b->water;
                       if (wa != wb) {
                         return wa;
                       }
                       return view_dist2(*a) > view_dist2(*b);
                     });
    cmd->SetPipeline(use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
    nrhi::Pipeline* blend_bound = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
    blend_pass_active = true;
    for (const DrawItem* item : transparent_items) {
      // Mid-fade entity pieces: alpha blend with z-write ON (see pso_fade).
      if (use_depth && g_r.pso_fade != nullptr && char_fade_zwrite(*item)) {
        // environment.transparent z-write blends follow the game's cull
        // state (back-to-back sheet pairs z-fight uncull(ed)).
        const int cm = item->env_family == 16 ? GameCullMode(scene, *item) : 0;
        nrhi::Pipeline* const want_f =
            (cm != 0 && g_r.pso_fade_cull[cm - 1] != nullptr)
                ? g_r.pso_fade_cull[cm - 1]
                : g_r.pso_fade;
        if (blend_bound != want_f) {
          cmd->SetPipeline(want_f);
          blend_bound = want_f;
        }
        timed_draw(*item);
        continue;
      }
      const bool hair = item->char_family >= 4 && item->char_family <= 5 &&
                        item->char_rows[14 * 4 + 1] > 0.0f;
      if (hair && use_depth && g_r.pso_hair_a != nullptr && g_r.pso_hair_b != nullptr) {
        // Guest wireframe: hair has its own two-pass PSOs, which bypass the
        // opaque path's wireframe selection entirely -- that left it drawing
        // as a solid cap while everything around it was lines. The game
        // draws defaulthair_defaultPS in the line group like any other
        // main-pass material, so one wireframe pass replaces the pair (the
        // cull split exists to order strand compositing, which has no
        // meaning with nothing filled).
        if (g_r.pso_wireframe != nullptr && GameWireframeItem(scene, *item)) {
          cmd->SetPipeline(g_r.pso_wireframe);
          timed_draw(*item);
          cmd->SetPipeline(blend_bound);
          continue;
        }
        // The game's two hair passes: cull BACK then cull FRONT with the
        // same shader: keeps far-side strands from compositing over
        // near-side ones (one uncull(ed) pass reads as crunchy noise).
        cmd->SetPipeline(g_r.pso_hair_a);
        timed_draw(*item);
        cmd->SetPipeline(g_r.pso_hair_b);
        timed_draw(*item);
        cmd->SetPipeline(blend_bound);
        continue;
      }
      // reflective_trans glass culls like the game (its material family's
      // XML: CULLMODE=FRONT): the canopy panels are double-glazed pane
      // PAIRS a few cm apart; uncull(ed) we composited BOTH panes (an
      // extra a^2 blend layer the emulated frame doesn't have).
      // pso_hair_b IS the transparent state with CULL_FRONT.
      if (item->env_family == 13 && use_depth && g_r.pso_hair_b != nullptr &&
          REXCVAR_GET(skate3_native_render_scene_backface_cull)) {
        const float* w = item->world;
        const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                           w[1] * (w[4] * w[10] - w[6] * w[8]) +
                           w[2] * (w[4] * w[9] - w[5] * w[8]);
        nrhi::Pipeline* want =
            det3 >= 0.0f ? g_r.pso_hair_b
                         : (use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
        if (want != blend_bound) {
          cmd->SetPipeline(want);
          blend_bound = want;
        }
        timed_draw(*item);
        continue;
      }
      nrhi::Pipeline* want_t = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
      // Particles: the system's own blend (see CaptureClothDraw). Factor
      // codes: 1 ONE, 6 SRC_ALPHA, 7 ONE_MINUS_SRC_ALPHA; op 0 = add.
      DrawRegState prs;
      if (use_depth && item->cloth_quads && ItemDrawRs(scene, *item, prs) &&
          ((prs.blend >> 5) & 7u) == 0) {
        const uint32_t cs = prs.blend & 0x1Fu;
        const uint32_t cd = (prs.blend >> 8) & 0x1Fu;
        if (cs == 1 && cd == 1 && g_r.pso_transparent_add != nullptr) {
          want_t = g_r.pso_transparent_add;
        } else if (cs == 6 && cd == 1 && g_r.pso_particle[0] != nullptr) {
          want_t = g_r.pso_particle[0];
        } else if (cs == 1 && cd == 7 && g_r.pso_particle[1] != nullptr) {
          want_t = g_r.pso_particle[1];
        } else if (cs != 6 || cd != 7) {
          static std::atomic<uint32_t> s_unmapped{0};
          if (s_unmapped.fetch_add(1, std::memory_order_relaxed) < 8) {
            REXLOG_INFO("native-scene: particle blend {:08X} unmapped, straight alpha",
                        prs.blend);
          }
        }
      }
      if (use_depth && g_r.pso_transparent_add != nullptr &&
          item->env_family == 16 && GameBlendMode(scene, *item) == 3) {
        want_t = g_r.pso_transparent_add;
      }
      // Fam 18 (transparentincandescent neon / signs): the game's own blend
      // for the draw. Blend off (ONE / ZERO) = an alpha-tested cutout drawn
      // opaque (the restaurant disc signs); straight alpha by default;
      // additive variants as recorded.
      DrawRegState nrs;
      const bool nrs_ok = item->env_family == 18 && ItemDrawRs(scene, *item, nrs);
      if (item->env_family == 18) {
        // DIAGNOSTIC (transparent incandescent signs): the game's own state
        // for each piece, once per mesh.
        static std::unordered_set<uint32_t> s_logged;
        if (s_logged.size() < 64 && s_logged.insert(item->mesh).second) {
          uint32_t w1 = 0;
          GuestTryLoadU32(base, item->diffuse_tex + 8 * 4, &w1);
          REXLOG_INFO(
              "native-scene: incand.transparent mesh={:08X} rs={} own_rs={} blend={:08X} "
              "colorctl={:08X} mask={:X} depth={:08X} mode={:08X} cull={} mult={:.3g} "
              "expo={:.3g} tex_fmt={} draws={}",
              item->mesh, nrs_ok ? 1 : 0, item->game_rs_seen ? 1 : 0, nrs.blend, nrs.color_ctl,
              nrs.color_mask, nrs.depth, nrs.mode_cntl, GameCullMode(scene, *item), item->incand_mult, item->incand_expo, w1 & 0x3Fu,
              item->draws.size());
        }
      }
      if (use_depth && nrs_ok && ((nrs.blend >> 5) & 7u) == 0) {
        const uint32_t cs = nrs.blend & 0x1Fu;
        const uint32_t cd = (nrs.blend >> 8) & 0x1Fu;
        if (cs == 1 && cd == 0 && g_r.pso != nullptr) {
          want_t = g_r.pso;
        } else if (cs == 1 && cd == 1 && g_r.pso_transparent_add != nullptr) {
          want_t = g_r.pso_transparent_add;
        } else if (cs == 6 && cd == 1 && g_r.pso_particle[0] != nullptr) {
          want_t = g_r.pso_particle[0];
        } else if (cs == 1 && cd == 7 && g_r.pso_particle[1] != nullptr) {
          want_t = g_r.pso_particle[1];
        }
        // The game's cull + z-write for the sign (captured: blend straight
        // alpha, z-write ON). The disc signs are double-sided sheet pairs:
        // uncull(ed) and without depth, the far face's mirrored lettering
        // and rim drew through the near one (the white fringe ring).
        const int cm = GameCullMode(scene, *item);
        const bool zw = ((nrs.depth >> 2) & 1u) != 0;
        if (want_t == g_r.pso_transparent) {
          if (zw && g_r.pso_fade != nullptr) {
            want_t = (cm != 0 && g_r.pso_fade_cull[cm - 1] != nullptr) ? g_r.pso_fade_cull[cm - 1]
                                                                         : g_r.pso_fade;
          } else if (cm == 1 && g_r.pso_hair_b != nullptr) {
            want_t = g_r.pso_hair_b;
          } else if (cm == 2 && g_r.pso_hair_a != nullptr) {
            want_t = g_r.pso_hair_a;
          }
        } else if (want_t == g_r.pso) {
          if (cm == 1 && g_r.pso_cullback != nullptr) {
            want_t = g_r.pso_cullback;  // kFront
          } else if (cm == 2 && g_r.pso_cullfront != nullptr) {
            want_t = g_r.pso_cullfront;  // kBack
          }
        }
      }
      // Straight-alpha environment.transparent draws with a game cull state:
      // pso_hair_b / pso_hair_a ARE the transparent state with kFront / kBack.
      if (use_depth && want_t == g_r.pso_transparent && item->env_family == 16 &&
          g_r.pso_hair_a != nullptr && g_r.pso_hair_b != nullptr) {
        const int cm = GameCullMode(scene, *item);
        if (cm == 1) {
          want_t = g_r.pso_hair_b;
        } else if (cm == 2) {
          want_t = g_r.pso_hair_a;
        }
      }
      if (blend_bound != want_t) {
        blend_bound = want_t;
        cmd->SetPipeline(blend_bound);
      }
      timed_draw(*item);
    }
  }
  blend_pass_active = false;
  // Hall of Meat x-ray (DrawItem::hom): the game draws the bones as a plain
  // opaque draw (RB_DEPTHCONTROL 0x708737 = LEQUAL + z-write, blend
  // ONE/ZERO) that nevertheless shows through every surface, i.e. depth-
  // tested only against the bones themselves. Same here: the opaque PSO
  // over the scene colour with a freshly cleared depth buffer of its own
  // (RendererState::hom_depth), then the scene depth is rebound untouched.
  if (!hom_items.empty() && g_r.pso != nullptr && g_r.depth != nullptr) {
    if (g_r.hom_depth == nullptr || g_r.hom_depth_width != g_r.depth_width ||
        g_r.hom_depth_height != g_r.depth_height || g_r.hom_depth_msaa != g_r.msaa) {
      if (g_r.hom_depth != nullptr) {
        g_r.device->DestroyDeferred(g_r.hom_depth);
        g_r.hom_depth = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = g_r.depth_width;
      desc.height = g_r.depth_height;
      desc.format = nrhi::Format::kD32_FLOAT;
      desc.sample_count = g_r.msaa;
      desc.usage = nrhi::kTextureUsageDepthStencil;
      desc.initial_state = nrhi::ResourceState::kDepthWrite;
      desc.clear_depth = 1.0f;
      g_r.hom_depth = g_r.device->CreateTexture(desc);
      g_r.hom_depth_width = g_r.depth_width;
      g_r.hom_depth_height = g_r.depth_height;
      g_r.hom_depth_msaa = g_r.msaa;
    }
    if (g_r.hom_depth != nullptr) {
      cmd->ClearDepth(g_r.hom_depth, 1.0f);
      cmd->SetRenderTargets(scene_color, g_r.hom_depth);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pso);
      scene_pso_bound = g_r.pso;
      for (const DrawItem* item : hom_items) {
        timed_draw(*item);
      }
      cmd->SetRenderTargets(scene_color, g_r.depth);
    }
  }
  // The editor's preview object (DrawItem::foreground), after the
  // transparents as in the game: inner faces depth-ALWAYS + z-write, then
  // the outer faces LEQUAL, so the object shows whole even where it is sunk
  // into the park pieces it will be placed on.
  if (!fg_items.empty()) {
    for (nrhi::Pipeline* fp : {g_r.pso_fg_inner, g_r.pso_fg_outer}) {
      forced_pso = fp;
      for (const DrawItem* item : fg_items) {
        timed_draw(*item);
      }
    }
    forced_pso = nullptr;
  }
  // The editor's selected piece (DrawItem::xray): the x-ray pass then the
  // normal one, in the game's order.
  if (!xray_items.empty()) {
    for (nrhi::Pipeline* xp : {g_r.pso_xray_a, g_r.pso_xray_b}) {
      forced_pso = xp;
      for (const DrawItem* item : xray_items) {
        timed_draw(*item);
      }
    }
    forced_pso = nullptr;
  }
  // The foreground mark (the game's stencil byte under the preview object,
  // which the masked popup blur keeps sharp): re-draw its draws with their
  // exact constants LEQUAL (no write) against the FINAL scene depth, the
  // RenderStencilMark technique. Both game passes REPLACE the stencil on
  // z-pass: the inner faces always (depth ALWAYS), the outer faces only
  // where they beat the scene -- so where an outer face lost to the ground
  // the object is sunk into, nothing marks and the backdrop blur shows
  // there (a no-depth silhouette marked it and left sharp ground). After
  // both passes every marked pixel holds one of the object's own depths, so
  // the same-depth re-draw marks exactly those. Alpha-tested pieces clip
  // (their alpha test kills before any stencil write).
  if (g_r.fg_mark_ready) {
    cmd->Barrier(g_r.fg_mark, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    g_r.fg_mark_ready = false;
  }
  if (!fg_mark_items.empty() && scene.ui_blur_masked && use_depth &&
      g_r.pso_stencil_mark != nullptr && EnsureFgMarkTarget(context)) {
    cmd->FlushBarriers();
    const float mark_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.fg_mark, mark_clear);
    cmd->SetRenderTargets(g_r.fg_mark, g_r.depth);
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    nrhi::Pipeline* mark_bound = nullptr;
    for (const FgMarkItem& fm : fg_mark_items) {
      const bool clip = fm.clip_srv != nullptr && g_r.pso_stencil_mark_clip != nullptr;
      nrhi::Pipeline* want = clip ? g_r.pso_stencil_mark_clip : g_r.pso_stencil_mark;
      if (want != mark_bound) {
        cmd->SetPipeline(want);
        mark_bound = want;
      }
      if (clip) {
        cmd->SetTexture(1, fm.clip_srv);
      }
      cmd->SetRootConstants(0, 52, fm.sm.constants, 0);
      cmd->SetBufferSrv(3, g_r.bone_ring, fm.sm.bone_offset);
      cmd->SetVertexBuffer(fm.sm.vb.buffer, fm.sm.vb.offset, fm.sm.vb.size_bytes,
                           fm.sm.vb.stride);
      cmd->SetIndexBuffer(fm.sm.ib.buffer, fm.sm.ib.offset, fm.sm.ib.size_bytes);
      for (const DrawEntry& draw : fm.sm.item->draws) {
        if (draw.prim == 4) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        } else if (draw.prim == 6) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
        } else {
          continue;
        }
        cmd->DrawIndexedInstanced(draw.index_count, fm.sm.constants[15] > 1.5f ? 2u : 1u,
                                  draw.start_index, draw.base_vertex);
      }
    }
    cmd->Barrier(g_r.fg_mark, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    g_r.fg_mark_ready = true;
    // Back to the scene pass state for what follows.
    cmd->SetRenderTargets(scene_color, g_r.depth);
    scene_pso_bound = nullptr;
  }
  g_pw_items.Add(perf_ns_since(items_t0));
  g_pw_pre.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   items_t0 - render_t0)
                   .count()));
  const auto mid_t0 = PerfClock::now();

  // Guest-texture resolver shared by the spline pass (pre-resolve, in the
  // scene pass) and the HUD pass (post-resolve); both allocate strip/quad
  // vertices from the same per-frame ui_ring region.
  // Per-frame inline budget for HOT content re-decodes (actively-animating
  // elements + video planes): inline keeps their content latency at ONE
  // frame; overflow degrades to the async heal for the rest of the frame.
  int64_t hot_inline_budget_ns = 4'000'000;
  // force_inline: the dormant FMV bracket-fallback resolve stays on the
  // inline decode (fires at most once per plane set).
  // video: the FMV triple resolves; content changes always decode inline
  // (frame-exact playback), and the caller additionally gates serving on
  // last_change_frame >= the movie session start (see the triple loop).
  // live: the in-game browser page (EAWebKit's 1280x720 surface). EAWebKit
  // repaints small regions in place (the selector outline, typed text), which
  // the 8x8 sample-probe grid routinely misses - the page froze on its old
  // selection - so it re-uploads every frame instead of probing.
  const auto resolve_2d_texture_uncached = [&](const uint32_t fetch[6],
                                      bool force_inline = false,
                                      bool video = false,
                                      int font_id = -1,
                                      bool live = false) -> const GuestTexture* {
    if ((fetch[0] & 0x3u) != 2 || fetch[1] == 0) {
      return &g_r.white;
    }
    // Text draw that matched an embedded bitmap font: serve the
    // higher-resolution replacement. Same normalised cells, so the draw's
    // own uvs address it unchanged. Unmatched draws fall through to the
    // game's texture untouched.
    if (font_id >= 0) {
      const GuestTexture* rep = EnsureFontAtlas(context, font_id);
      if (rep != nullptr) {
        return rep;
      }
    }
    const uint64_t key = FetchWordsKey(fetch);
    // 2D-resolver trace (trace_2d cvar, matched on fetch word 1): logs the
    // per-resolve entry state + route below, and registers the words key so
    // the worker-commit trace covers its heals too.
    const bool tr2d = g_trace_2d_w1 != 0 && fetch[1] == g_trace_2d_w1;
    if (tr2d) {
      g_trace_keys.insert(key);
    }
    // Routing (see the 2d_async_px cvar): INLINE-FIRST under the per-frame
    // budget; the run-copy untiler + in-place hot updates made decodes
    // cheap (1280x720 sub-ms; no CreateCommittedResource churn on content
    // changes), so first sightings AND content changes decode inline for
    // 1-frame content latency. The async workers are the burst-overflow
    // valve only: they exist because inline decodes stall the frame AND the
    // guest (swap blocks), which mattered when a screen change brought
    // 100+ ms of per-pixel decode work, but routinely
    // skipping first-sight quads made every APT re-raster (new address =
    // new key) blink its element for 1-3 frames (the menu flicker).
    const int32_t async_px = REXCVAR_GET(skate3_native_render_scene_2d_async_px);
    const uint32_t px_w = (fetch[2] & 0x1FFFu) + 1;
    const uint32_t px_h = ((fetch[2] >> 13) & 0x1FFFu) + 1;
    const bool async_ui = !force_inline && async_px > 0 &&
                          uint64_t(px_w) * px_h > uint64_t(async_px);
    // PLAIN store lookup on purpose: find_words_texture is the 3D poster
    // revalidator; it probes, RE-ARMS recheck_frame and enqueues its own
    // ui=false heal, which made this resolver's liveness block dead code
    // (recheck always freshly re-armed before it ran; astale=0 across whole
    // sessions) and put every 2D content update on that 4-5
    // frame async round trip.
    auto it = g_r.tex_store.find(key);
    bool inline_redecode = false;
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid &&
        (live || (!inter_frame && frame_number >= it->second.recheck_frame &&
                  REXCVAR_GET(skate3_native_render_scene_tex_revalidate)))) {
      // Content liveness for CPU-rewritten UI art: video frames and APT
      // re-rasterized tiles rewrite the same payload with the fetch words
      // unchanged; without the probe the words-keyed cache would freeze
      // them on their first decoded content. Incomplete decodes (truncated
      // tiled-mip copy) re-decode here too, like the 3D route revalidator;
      // the 2D-only art (menu thumbnails) has no other heal path.
      const uint64_t fp = live ? 0 : SampleProbeFingerprint(base, it->second);
      if (live || (fp != 0 && fp != it->second.payload_fp) || it->second.incomplete) {
        // Rolling-capped heal telemetry for menu-class art (the map-switch
        // thumbnail wrong-image reports): names the key, the route taken
        // and the fingerprint transition.
        static std::atomic<uint32_t> s_2d_heal_logs{0};
        const bool log_heal =
            !live && uint64_t(px_w) * px_h >= 32768 &&
            s_2d_heal_logs.fetch_add(1, std::memory_order_relaxed) < 64;
        // Content changes decode INLINE while the per-frame budget lasts
        // (1-frame latency for animating UI); fullscreen-class art gets an
        // extended budget; serving it stale/blank is a whole-screen
        // artifact, and the run-copy untiler made even 1280x720 cheap.
        const bool fullscreen_class = uint64_t(px_w) * px_h >= 460'000;
        inline_redecode =
            video || live || !async_ui || hot_inline_budget_ns > 0 ||
            (fullscreen_class && hot_inline_budget_ns > -4'000'000);
        if (log_heal) {
          REXLOG_INFO(
              "native-scene: 2D content heal {}x{} key={:016X} fp {:016X}->"
              "{:016X} inc={} route={}",
              px_w, px_h, key, it->second.payload_fp, fp,
              it->second.incomplete ? 1 : 0,
              inline_redecode ? "inline" : "async");
        }
        if (inline_redecode) {
          // Same layout = update the committed texture in place (no
          // CreateCommittedResource churn, SRV slot kept, tear-guarded).
          const auto up_t0 = PerfClock::now();
          if (UpdateGuestTexture2DInPlace(context, base, it->second)) {
            const uint64_t up_ns = perf_ns_since(up_t0);
            hot_inline_budget_ns -= int64_t(up_ns);
            g_pw_tex_decode.Add(up_ns);
            it->second.last_change_frame = frame_number;
            return &it->second;
          }
          if (tr2d) {
            REXLOG_INFO("2d-trace: f{} key={:016X} inplace update REFUSED - "
                        "retire + fresh decode",
                        frame_number, key);
          }
          RetireGuestTexture(it->second, context.device->CurrentSubmission());
          g_r.tex_store.erase(it);
          it = g_r.tex_store.end();  // falls into the inline decode below
        } else {
          // Budget exhausted (burst overflow): heal on the workers, serve
          // the stale decode meanwhile.
          EnqueueWordsMiss(key, fetch, /*ui=*/true);
          it->second.recheck_frame = frame_number + 2;
          g_2d_async_stale.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // Menu screens probe on a 2-frame cadence: the skater-portrait
        // boxes are resolved IN PLACE (words unchanged) up to hundreds of
        // ms after the quad first draws (asset streaming first), and a
        // 16-frame recheck left the pre-resolve memory content, the
        // loading-poster flash, on screen for its full window.
        it->second.recheck_frame =
            frame_number + (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      }
    }
    if (it == g_r.tex_store.end()) {
      // First sightings also decode INLINE while the budget lasts. The old
      // always-async policy skipped the quad for the 1-3 worker-round-trip
      // frames, but animating APT art re-rasterizes at NEW addresses, so
      // every animation step was a "first sighting" and menu elements
      // (including fullscreen backdrops) BLINKED through every animation
      // (askip +150/window during navigation, visible
      // menu flicker/brightness pulsing). Async remains the burst-overflow
      // valve only.
      const bool fullscreen_class = uint64_t(px_w) * px_h >= 460'000;
      const bool inline_ok =
          hot_inline_budget_ns > 0 ||
          (fullscreen_class && hot_inline_budget_ns > -4'000'000);
      if (async_ui && !video && !inline_redecode && !inline_ok) {
        EnqueueWordsMiss(key, fetch, /*ui=*/true);
        g_2d_async_skip.fetch_add(1, std::memory_order_relaxed);
        if (tr2d) {
          REXLOG_INFO("2d-trace: f{} key={:016X} SKIP (async first sight)",
                      frame_number, key);
        }
        return nullptr;
      }
      // Small HUD/spline art decodes inline (sub-ms; async would pop
      // HUD elements for no gain).
      const auto hud_t0 = PerfClock::now();
      GuestTexture gt;
      EnsureGuestTextureFromWords(context, base, fetch, gt);
      const uint64_t decode_ns = perf_ns_since(hud_t0);
      hot_inline_budget_ns -= int64_t(decode_ns);
      g_pw_tex_decode.Add(decode_ns);
      // Attribution for residual render-thread stalls: anything still
      // decoding inline for >3 ms should either move over the async
      // threshold or explain itself here.
      if (decode_ns > 3'000'000) {
        static std::atomic<uint32_t> s_slow{0};
        const uint32_t n = s_slow.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: SLOW inline 2D decode {:.1f}ms (copy {:.1f} "
              "create {:.1f} gen {:.1f}) {}x{} fetch=[{:08X} {:08X} {:08X}] "
              "forced={} (n={})",
              double(decode_ns) / 1e6, double(g_tex_dec_copy_ns) / 1e6,
              double(g_tex_dec_create_ns) / 1e6, double(g_tex_dec_gen_ns) / 1e6,
              px_w, px_h, fetch[0], fetch[1], fetch[2], force_inline ? 1 : 0, n);
        }
      }
      if (!gt.valid) {
        static std::unordered_set<uint64_t> logged;
        if (logged.size() < 32 && logged.insert(key).second) {
          REXLOG_INFO(
              "native-scene: 2D texture decode FAILED fetch=[{:08X} {:08X} {:08X} "
              "{:08X} {:08X} {:08X}]",
              fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
        }
      }
      cmd->FlushBarriers();
      gt.last_used_frame = frame_number;
      gt.last_change_frame = frame_number;
      if (tr2d) {
        REXLOG_INFO("2d-trace: f{} key={:016X} INLINE decode valid={} inc={} "
                    "fp={:016X}",
                    frame_number, key, gt.valid ? 1 : 0, gt.incomplete ? 1 : 0,
                    gt.payload_fp);
      }
      it = g_r.tex_store.emplace(key, gt).first;
    }
    if (it != g_r.tex_store.end() &&
        (tr2d || (g_in_menus_frame.load(std::memory_order_relaxed) &&
                  uint64_t(px_w) * px_h >= 32768))) {
      // Menu thumbnail-class serve log: per buffer (fetch word 1), log when
      // EITHER the served cache content (stored fp) OR the live guest
      // content (probe fp) changes. A served-fp that lags the live-fp is
      // exactly the stale-thumbnail bug; the buffer holds the new area's
      // art while we keep serving the previous decode.
      static std::mutex s_2d_log_mutex;
      static std::unordered_map<uint32_t, uint64_t> s_2d_seen;  // w1 -> stored^live
      const uint64_t live_fp = SampleProbeFingerprint(base, it->second);
      const uint64_t sig = it->second.payload_fp ^ (live_fp * 3);
      std::lock_guard<std::mutex> lk(s_2d_log_mutex);
      auto sit = s_2d_seen.find(fetch[1]);
      if (sit == s_2d_seen.end() || sit->second != sig) {
        s_2d_seen[fetch[1]] = sig;
        REXLOG_INFO(
            "2d-thumb: f{} w1={:08X} {}x{} key={:016X} served_fp={:016X} "
            "live_fp={:016X}{} valid={} inc={} recheck=f{}",
            frame_number, fetch[1], px_w, px_h, key, it->second.payload_fp,
            live_fp,
            (live_fp != 0 && live_fp != it->second.payload_fp) ? " STALE" : "",
            it->second.valid ? 1 : 0, it->second.incomplete ? 1 : 0,
            it->second.recheck_frame);
      }
    }
    return it->second.valid ? &it->second : &g_r.white;
  };
  const auto resolve_2d_texture = [&](const uint32_t fetch[6], bool force_inline=false,
                                       bool video=false, int font_id=-1, bool live=false)
                                       -> const GuestTexture* {
    if (skate3::vr::CurrentEye() < 0)
      return resolve_2d_texture_uncached(fetch,force_inline,video,font_id,live);
    std::array<uint32_t,8> key;
    std::copy(fetch,fetch+6,key.begin());
    key[6]=uint32_t(font_id); key[7]=uint32_t(force_inline)|(uint32_t(video)<<1)|(uint32_t(live)<<2);
    auto [it, inserted]=g_vr_overlay_textures.try_emplace(key);
    auto& snap=it->second;
    if (inserted) {
      const GuestTexture* t=resolve_2d_texture_uncached(fetch,force_inline,video,font_id,live);
      snap.kind=t==nullptr ? 0 : t==&g_r.white ? 1 : 2;
      if (snap.kind==2) snap.texture=*t;
    }
    return snap.kind==0 ? nullptr : snap.kind==1 ? &g_r.white : &snap.texture;
  };
  const uint32_t ui_region =
      uint32_t(frame_number % RendererState::kUiRegions) * RendererState::kUiRegionSize;
  uint32_t ui_offset = 0;

  // In-world neon splines (waypoint arrows / marker beams): replayed inside
  // the scene pass, depth-tested against the world like the emulated frame,
  // in submission order (darken backdrop passes precede the additive glow).
  uint32_t drawn_spline = 0;
  if (!photo_mode && REXCVAR_GET(skate3_native_render_scene_splines) &&
      g_r.pso_spline_default != nullptr && g_r.ui_ring_cpu != nullptr) {
    std::vector<SplineDraw> scene_spline;
    if (skate3::vr::CurrentEye()==1) {
      scene_spline=g_vr_eye_splines;
    } else {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_spline = g_scene_spline;
      if (skate3::vr::CurrentEye()==0) g_vr_eye_splines=scene_spline;
    }
    // One-pass VR stereo: world-space verts, so each eye draws them with
    // its own view_proj into its half.
    for (const SplineDraw& s : scene_spline) {
      const uint32_t bytes = uint32_t(s.verts.size());
      if (bytes == 0 || ui_offset + bytes > RendererState::kUiRegionSize) {
        continue;
      }
      std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, s.verts.data(), bytes);
      const GuestTexture* spline_tex = resolve_2d_texture(s.fetch);
      if (spline_tex == nullptr) {
        continue;  // big-art decode in flight on the workers; skip a frame
      }
      // Upload/resolve once, draw the SAME payload for both eyes.
      for (int vr_e=0; vr_e<(vr_stereo?2:1); ++vr_e) {
      const float* const spline_vp=vr_e==1 ? vr_right_eye_vp : scene.view_proj;
      if (vr_stereo) vr_eye_viewport(cmd,vr_e);
      cmd->SetPipeline(s.pass == 1 ? g_r.pso_spline_darken
                                   : g_r.pso_spline_default);
      // Root constants: the scene's (smoothed) view_proj rows; the verts
      // are world-space, then i_intensity as staged (c149).
      float spline_consts[20];
      std::memcpy(spline_consts, spline_vp, sizeof(float) * 16);
      // i_intensity: the VS passes c199 straight through to the PS (max o0,
      // c199, c199).
      std::memcpy(spline_consts + 16, s.consts + 149 * 4, sizeof(float) * 4);
      // intensity.zw = showcase blackout gate (the guest row's unused
      // lanes): z = split x, w = left|right visibility bits. The spline PS
      // draws outside the scene shaders, so the blackout stage must gate
      // it here or the neon lines leak over the black recording bookends.
      const auto side_visible = [](float v) {
        return v < 255.5f || ((uint32_t(v + 0.5f) - 256u) & 1024u) == 0u;
      };
      spline_consts[18] = g_r.showcase_rows[2];
      spline_consts[19] =
          float((side_visible(g_r.showcase_rows[0]) ? 1 : 0) |
                (side_visible(g_r.showcase_rows[1]) ? 2 : 0));
      cmd->SetRootConstants(0, 20, spline_consts, 0);
      cmd->SetTexture(1, spline_tex->srv);
      cmd->SetVertexBuffer(g_r.ui_ring, ui_region + ui_offset, bytes, 28);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
      cmd->Draw(s.count, 0);
      ++drawn_spline;
      }  // vr_e
      ui_offset += bytes;
    }
    if (vr_stereo) {
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
    }
  }
  // SimpleDraw debug primitives (see SimpleDrawCap; e.g. the
  // cv_ShowRenderSkeleton bone lines): world-space triangle lists projected
  // with the scene's view_proj, lines as ~1 px quads, drawn over everything
  // like the emulated frame (the bones show through the character and the
  // world, as the Hall of Meat x-ray does).
  if (!photo_mode && REXCVAR_GET(skate3_native_render_scene_simpledraw) &&
      g_r.pso_sdraw != nullptr &&
      g_r.ui_ring_cpu != nullptr) {
    const uint32_t bytes = uint32_t(scene.sdraw_verts.size() * sizeof(SimpleDrawVert));
    if (bytes != 0 && ui_offset + bytes <= RendererState::kUiRegionSize) {
    std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, scene.sdraw_verts.data(), bytes);
    for (int vr_e = 0; vr_e < (vr_stereo ? 2 : 1); ++vr_e) {
      const float* const sdraw_vp = vr_e == 1 ? vr_right_eye_vp : scene.view_proj;
      const uint32_t sdraw_w = vr_stereo ? pass_w / 2 : pass_w;
      if (vr_stereo) {
        vr_eye_viewport(cmd, vr_e);
      }
      // Already re-timed onto this scene's playback clock at build time
      // (RetimeSimpleDraws), like the character meshes they draw over.
      // Both eyes bind this allocation; do not upload/consume it twice.
      cmd->SetPipeline(g_r.pso_sdraw);
      float dconsts[20];
      std::memcpy(dconsts, sdraw_vp, sizeof(float) * 16);
      // Half line width in pixels: the console's 1 px line at 1152x640,
      // scaled to this pass.
      dconsts[16] = std::max(0.5f, 0.5f * float(pass_h) / 640.0f);
      dconsts[17] = 2.0f / std::max(float(sdraw_w), 1.0f);
      dconsts[18] = 2.0f / std::max(float(pass_h), 1.0f);
      dconsts[19] = 0.0f;
      cmd->SetRootConstants(0, 20, dconsts, 0);
      cmd->SetVertexBuffer(g_r.ui_ring, ui_region + ui_offset, bytes,
                           uint32_t(sizeof(SimpleDrawVert)));
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      cmd->Draw(uint32_t(scene.sdraw_verts.size()), 0);
    }  // vr_e
      ui_offset += bytes;
    }
    if (vr_stereo) {
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
    }
  }

  const bool outline_ready =
      RenderOutlineMask(context, scene, viewport, scissor, msaa_on, scene_color,
                        g_r.depth, use_depth);

  // ---- Game stencil mark (postfx chain input) ----
  // The game's cinematic / photo postfx reads the depth buffer's STENCIL
  // byte through the packed depth (visualfx tf1.z): pixels its draws
  // stencil-marked (characters and pedestrians REPLACE 0xFF, func ALWAYS,
  // alpha test off) get no motion blur and skip the color grade, keeping
  // the camera-followed skater sharp while the world streaks. D32 has no
  // stencil, so re-draw exactly those items (same vs_main, constants, bone
  // palette and streams as their main draw = bit-identical depth) with
  // LESS_EQUAL against the scene depth: the z-pass coverage the game's
  // stencil REPLACE marks.
  if (g_r.stencil_mark_ready) {
    cmd->Barrier(g_r.stencil_mark, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    g_r.stencil_mark_ready = false;
  }
  if (stencil_mark_on && EnsureStencilMarkTarget(context)) {
    cmd->FlushBarriers();
    const float mark_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.stencil_mark, mark_clear);
    if (!stencil_items.empty()) {
      cmd->SetRenderTargets(g_r.stencil_mark, g_r.depth);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pso_stencil_mark);
      for (const StencilMarkItem& sm : stencil_items) {
        cmd->SetRootConstants(0, 52, sm.constants, 0);
        cmd->SetBufferSrv(3, g_r.bone_ring, sm.bone_offset);
        cmd->SetVertexBuffer(sm.vb.buffer, sm.vb.offset, sm.vb.size_bytes,
                             sm.vb.stride);
        cmd->SetIndexBuffer(sm.ib.buffer, sm.ib.offset, sm.ib.size_bytes);
        for (const DrawEntry& draw : sm.item->draws) {
          if (draw.prim == 4) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          } else if (draw.prim == 6) {
            cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
          } else {
            continue;
          }
          cmd->DrawIndexedInstanced(draw.index_count, sm.constants[15] > 1.5f ? 2u : 1u,
                                    draw.start_index, draw.base_vertex);
        }
      }
      if (!msaa_on) {
        cmd->SetRenderTargets(scene_color, use_depth ? g_r.depth : nullptr);
      }
    }
    cmd->Barrier(g_r.stencil_mark, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    g_r.stencil_mark_ready = true;
  }

  // ---- SSR reflection G-buffer (scene.hlsl ps_refl_gbuf) ----
  // Re-render the frame's reflective items (captured with their main-pass
  // constants/textures by draw_item) into the half-res G-buffer while the
  // main binding layout is still live. The march/composite consume it after
  // the resolve + AO passes (ApplySsrPass); it hands off in
  // PIXEL_SHADER_RESOURCE state via ssr_gbuf_ready.
  if (!photo_mode) {
    g_r.ssr_gbuf_ready = false;
  }
  if (!photo_mode && !ssr_items.empty() && EnsureSsrPipeline(context) &&
      EnsureSsrTargets(context)) {
    // Front-to-back for early-z; the pass's own depth buffer resolves
    // overlap (a curved facade's far side shares texels with its near side
    // within one mesh; no draw order can fix that).
    std::stable_sort(ssr_items.begin(), ssr_items.end(),
                     [&](const SsrGbufItem& a, const SsrGbufItem& b) {
                       return view_dist2(*a.item) < view_dist2(*b.item);
                     });
    const float gbuf_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd->ClearRenderTarget(g_r.ssr_gbuf, gbuf_clear);
    cmd->ClearDepth(g_r.ssr_gbuf_depth, 1.0f);
    cmd->SetRenderTargets(g_r.ssr_gbuf, g_r.ssr_gbuf_depth);
    cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(g_r.ssr_width),
                                    float(g_r.ssr_height), 0.0f, 1.0f});
    cmd->SetScissor(
        nrhi::Rect{0, 0, int32_t(g_r.ssr_width), int32_t(g_r.ssr_height)});
    cmd->SetPipeline(g_r.pso_ssr_gbuf);
    uint32_t gbuf_draws = 0;
    for (const SsrGbufItem& si : ssr_items) {
      auto mit = g_r.meshes.find(si.mesh);
      if (mit == g_r.meshes.end() ||
          mit->second.fingerprint != si.fingerprint) {
        continue;  // decode swapped since the main-pass draw; next frame
      }
      cmd->SetRootConstants(0, 52, si.constants, 0);
      cmd->SetTexture(4, si.t3);
      cmd->SetTexturePair(5, si.t4, si.t5);
      cmd->SetVertexBuffer(mit->second.vb_view.buffer,
                           mit->second.vb_view.offset,
                           mit->second.vb_view.size_bytes,
                           mit->second.vb_view.stride);
      cmd->SetIndexBuffer(mit->second.ib_view.buffer,
                          mit->second.ib_view.offset,
                          mit->second.ib_view.size_bytes);
      for (const DrawEntry& draw : si.item->draws) {
        if (draw.prim == 4) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        } else if (draw.prim == 6) {
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
        } else {
          continue;
        }
        cmd->DrawIndexed(draw.index_count, draw.start_index,
                         draw.base_vertex);
        ++gbuf_draws;
      }
    }
    // Restore the pass state the resolve/post paths rely on.
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    if (!msaa_on) {
      cmd->SetRenderTargets(scene_color, use_depth ? g_r.depth : nullptr);
    }
    if (gbuf_draws > 0) {
      cmd->Barrier(g_r.ssr_gbuf, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      g_r.ssr_gbuf_ready = true;
    }
  }

  cmd->ProfileRegion(nrhi::ProfileStage::kResolve);
  if (msaa_on) {
    // Resolve: average the MSAA samples into the 1x scene plane (the float
    // HDR plane, or the guest output on the classic path) with a fullscreen
    // pass, then restore steady-state resource states.
    cmd->Barrier(g_r.msaa_color, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    if (!hdr_on) {
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kGuestOutput,
                   nrhi::ResourceState::kRenderTarget);
    }
    cmd->FlushBarriers();
    cmd->SetRenderTargets(hdr_on ? g_r.hdr_resolved : context.guest_output,
                          nullptr);
    cmd->SetPipeline(g_r.resolve_pso);
    cmd->SetTexture(1, g_r.msaa_srv_slot);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->Draw(3, 0);
    cmd->Barrier(g_r.msaa_color, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
  }

  // Portrait job tail: the game's pass ends here (its resolve), no post.
  // Decode + downsample the photo region into the 1152x640 photo_out and
  // copy it to the readback; PhotoComplete tiles it into guest memory after
  // the command processor drained the GPU.
  if (photo_mode) {
    nrhi::Texture* src_tex = hdr_on ? g_r.hdr_resolved : context.guest_output;
    nrhi::TextureView* src_srv = hdr_on ? g_r.hdr_srv : g_r.output_srv_slot;
    const nrhi::ResourceState src_rest =
        hdr_on ? nrhi::ResourceState::kRenderTarget : nrhi::ResourceState::kGuestOutput;
    cmd->Barrier(src_tex, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
    cmd->SetRenderTargets(g_r.photo_out, nullptr);
    const nrhi::Viewport out_vp{0.0f, 0.0f, float(RendererState::kBlurWidth),
                                float(RendererState::kBlurHeight), 0.0f, 1.0f};
    const nrhi::Rect out_sc{0, 0, int32_t(RendererState::kBlurWidth),
                            int32_t(RendererState::kBlurHeight)};
    cmd->SetViewport(out_vp);
    cmd->SetScissor(out_sc);
    // The game's own pass, in its order and in its 1152x640 target: the colour
    // clear (the raw 8-bit value; the target gets no post), the PhotoEffects
    // 2D layers drawn before the skater (the backdrop), the skater, then the
    // layers drawn after it (colour bars, logos, the rotated gamertag). The
    // layers carry the game's own ortho constants for this target, so they
    // land exactly where the game drew them whatever the display's aspect.
    const PhotoLayers* layers = g_photo_layers.get();
    if (photo_coverage) {
      float bg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      const float* src_bg = layers != nullptr && layers->clear_valid ? layers->clear
                            : scene.clear_valid                      ? scene.clear_color
                                                                     : nullptr;
      if (src_bg != nullptr) {
        for (int k = 0; k < 3; ++k) {
          bg[k] = std::clamp(src_bg[k], 0.0f, 1.0f);
        }
      }
      cmd->ClearRenderTarget(g_r.photo_out, bg);
    }
    // Photo layers use their own UI-ring slice: half a ring ahead of the
    // frame the job shares its number with, upper half of that region.
    const uint32_t photo_ui_region =
        uint32_t((frame_number + RendererState::kUiRegions / 2) % RendererState::kUiRegions) *
        RendererState::kUiRegionSize;
    uint32_t photo_ui_offset = RendererState::kUiRegionSize / 2;
    uint32_t photo_layers_drawn = 0;
    const bool layers_ok = layers != nullptr && g_r.pso_2d != nullptr &&
                           g_r.ui_ring_cpu != nullptr && g_r.ui_ring != nullptr;
    const auto draw_layer = [&](const Draw2d& d) {
      const uint32_t bytes = uint32_t(d.verts.size());
      if (bytes == 0 || d.stride != 28 || (d.color_mask & 0xFu) == 0 ||
          photo_ui_offset + bytes > RendererState::kUiRegionSize) {
        return;
      }
      const GuestTexture* t =
          resolve_2d_texture(d.fetch, /*force_inline=*/false, /*video=*/false, d.font_id);
      if (t == nullptr) {
        return;  // large-art decode in flight
      }
      std::memcpy(g_r.ui_ring_cpu + photo_ui_region + photo_ui_offset, d.verts.data(), bytes);
      float constants[52] = {};
      std::memcpy(constants, d.consts, sizeof(d.consts));
      // Vertex-coloured SimpleDraw layouts take c8 = 1 (see emit_draw).
      const bool vertex_colored =
          (d.src_stride == 20 && (d.flags & 0x10u) == 0 && (d.flags & 0x20u) != 0) ||
          (d.src_stride == 28 && ((d.flags & 0x20u) != 0 || d.flags == 0)) ||
          (d.src_stride == 32 && (d.flags & 0x20u) != 0);
      if (vertex_colored) {
        constants[32] = constants[33] = constants[34] = constants[35] = 1.0f;
      }
      const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                         d.consts[14] == 0.0f && d.consts[15] == 1.0f;
      constants[36] = ortho ? 1.0f : 0.0f;
      constants[37] = float(std::clamp(REXCVAR_GET(skate3_native_render_scene_2d_sharp), 0.0, 2.0));
      cmd->SetPipeline(Get2dBlendPso(context, d.blend_control));
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->SetTexture(1, t->srv);
      cmd->SetVertexBuffer(g_r.ui_ring, photo_ui_region + photo_ui_offset, bytes, d.stride);
      cmd->SetPrimitiveTopology(d.prim == 5 ? nrhi::PrimitiveTopology::kTriangleStrip
                                            : nrhi::PrimitiveTopology::kTriangleList);
      cmd->Draw(d.count, 0);
      photo_ui_offset += bytes;
      ++photo_layers_drawn;
    };
    if (photo_coverage && layers_ok) {
      for (const Draw2d& d : layers->pre) {
        draw_layer(d);
      }
    }
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->SetPipeline(g_r.pso_photo_out);
    // Tap spacing: one output texel's footprint in source texels / 4.
    const float step_x = float(pass_w) / float(RendererState::kBlurWidth) * 0.25f;
    const float step_y = float(pass_h) / float(RendererState::kBlurHeight) * 0.25f;
    // dir.w: composite by the plane's alpha coverage (premultiplied over the
    // clear + backdrop); 0 = opaque (no coverage in the packed HDR format).
    const float photo_consts[8] = {step_x / float(context.guest_output_width),
                                   step_y / float(context.guest_output_height),
                                   hdr_on ? 1.0f : 0.0f,
                                   photo_coverage ? 1.0f : 0.0f,
                                   float(pass_w) / float(context.guest_output_width),
                                   float(pass_h) / float(context.guest_output_height),
                                   0.0f,
                                   0.0f};
    cmd->SetRootConstants(0, 8, photo_consts, 0);
    cmd->SetTexture(1, src_srv);
    cmd->Draw(3, 0);
    if (layers_ok) {
      for (const Draw2d& d : layers->post) {
        draw_layer(d);
      }
    }
    REXLOG_INFO("native-scene: photo composite: {} of {} + {} 2D layers drawn ({})",
                photo_layers_drawn, layers != nullptr ? layers->pre.size() : 0,
                layers != nullptr ? layers->post.size() : 0,
                photo_coverage ? "skater by coverage" : "opaque skater, packed HDR");
    cmd->Barrier(g_r.photo_out, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kCopySource);
    cmd->Barrier(src_tex, nrhi::ResourceState::kPixelShaderResource, src_rest);
    cmd->FlushBarriers();
    cmd->CopyTextureToBuffer(g_r.photo_readback, 0, RendererState::kGrabRowPitch,
                             g_r.photo_out, 0, RendererState::kBlurWidth,
                             RendererState::kBlurHeight);
    cmd->Barrier(g_r.photo_out, nrhi::ResourceState::kCopySource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    return true;
  }

  // Classic path: the outline composites straight onto the resolved guest
  // output. Under HDR it runs after the tonemap below (a gamma-space
  // screen overlay, not scene lighting).
  if (outline_ready && !hdr_on) {
    RenderOutlineComposite(context, scene, context.guest_output, viewport,
                           scissor);
  }

  // One-pass VR stereo (classic path): the scene is complete; submit it
  // before recording the post chain. AO, the only pass below that relies on
  // the scene pass's latched layout, is off in stereo; the rest bind their
  // own state (the layout re-bind is defensive).
  if (vr_stereo && !hdr_on && skate3::vr::FlushEyes() &&
      context.device->FlushSubmission()) {
    cmd->SetBindingLayout(g_r.layout);
  }

  // ---- Screen-space ambient occlusion (ssao.hlsl: GTAO) ----
  // Multiplies the resolved scene by a horizon-based visibility term before
  // any downstream consumer (bloom/tonemap under HDR, photo chain, 2D
  // overlay, blur). Runs after the outline composite, which relies on the
  // main layout latched from the scene pass.
  bool post_ran = false;
  bool ssao_ran = false;
  cmd->ProfileRegion(nrhi::ProfileStage::kAmbientOcclusion);
  // (Skipped in one-pass VR stereo: it reconstructs view positions from the
  // full-width screen UV, which spans both eyes there.)
  if (use_depth && !loading_native && !vr_stereo &&
      REXCVAR_GET(skate3_native_render_scene_ssao)) {
    // 0 = GTAO (ssao.hlsl), 1 = HBAO (hbao.hlsl); see
    // skate3_native_render_scene_ao_technique.
    const bool want_hbao =
        REXCVAR_GET(skate3_native_render_scene_ao_technique) != 0;
    const bool ran = want_hbao
                         ? ApplyHbaoPass(context, cmd, scene, viewport, scissor)
                         : ApplySsaoPass(context, cmd, scene, viewport, scissor);
    if (ran) {
      post_ran = true;
      ssao_ran = true;
    }
  }
  // Both AO passes build the occlusion grid from their linear depth; with
  // AO off the grid gets its own linearize + reduce, so the default-on
  // occlusion culls actually have data (they silently culled nothing).
  // ssao_ran stays false: SSR keeps using its own linear depth.
  if (!ssao_ran && use_depth && !loading_native &&
      RenderOcclusionGrid(context, cmd, scene, viewport, scissor)) {
    post_ran = true;
  }

  // ---- Screen-space reflections (ssr.hlsl) ----
  // March + composite onto the pre-tonemap HDR plane, between the AO pass
  // and the HDR post, so reflections tonemap and bloom exactly like
  // directly-visible scenery. The reflection G-buffer was drawn inside the
  // scene pass (ssr_gbuf_ready); the SSAO linear-depth plane is reused when
  // AO ran this frame.
  cmd->ProfileRegion(nrhi::ProfileStage::kSsr);
  if (g_r.ssr_gbuf_ready &&
      ApplySsrPass(context, cmd, scene, viewport, scissor, ssao_ran)) {
    post_ran = true;
  }

  // ---- Volumetric lighting (hdr.hlsl ps_vol_*) ----
  // Shadow-marched sun shafts (per-step visibility against the CSM atlas +
  // the static world-shadow map), plus the constant staging for the
  // directional haze; both terms join in ps_tonemap after the AO multiply,
  // so they bloom and tonemap like scene light.
  cmd->ProfileRegion(nrhi::ProfileStage::kVolumetrics);
  if (hdr_on && use_depth && !loading_native && !vr_stereo &&
      ApplyVolumetricPass(context, cmd, scene, viewport, scissor, ssao_ran,
                          frame_number)) {
    post_ran = true;
  }

  // ---- HDR post (hdr.hlsl: bloom pyramid + the extracted tonemap) ----
  // Bloom reads the AO-composited float scene; ps_tonemap applies the
  // game's shared tone chain once into the guest output, which every later
  // consumer (photo chain, 2D overlay, blur, grab) reads exactly as on the
  // classic path.
  cmd->ProfileRegion(nrhi::ProfileStage::kBloom);
  if (hdr_on) {
    ApplyHdrPost(context, cmd, viewport, scissor, loading_native,
                 frame_number);
    post_ran = true;
  }

  // ---- Auto-exposure measurement (the game's 1463-1465 chain) ----
  // Over the finished 3D frame (tonemapped, before the photo chain / 2D),
  // exactly where the game measures its pre-uber scene.
  if (!loading_native) {
    ApplyExposureMeasure(context, cmd);
    post_ran = true;
  }

  // ---- The game's postfx: bloom_dof chain + postfx_fast_uber composite ----
  // The scene target above is the game's pre-postfx scene (see ToneOut);
  // this applies the game's own finishing passes to it. The photo-editor
  // chain runs the gameplay composite (postfx_visualfx) itself from the
  // same scene target, so it replaces this pass on its frames.
  // Cinematic-chain frames (bails / replays: the game draws postfx_uberPS
  // instead of postfx_fast_uber) run the chain below; its visualfx takes the
  // frame's bloom as the accumulation input, so the bloom chain still runs
  // here, without the fast_uber composite. (The HDR path keeps its own post.)
  const bool gameplay_chain = scene.photo_fx.valid && scene.photo_fx.gameplay;
  // One-pass VR stereo: the cinematic / photo chain (DOF, fisheye,
  // vignette, grain) is a single-view screen effect; on the side-by-side
  // image it centres on the seam and costs a full chain at stereo size.
  // Composite those frames like gameplay instead.
  const bool photo_chain =
      !vr_stereo && scene.photo_fx.valid && !(gameplay_chain && hdr_on);
  nrhi::Texture* chain_bloom_tex = nullptr;
  nrhi::TextureView* chain_bloom_srv = nullptr;
  // The AO debug view REPLACES the scene with the greyscale AO term, so the
  // game's postfx composite (which rebuilds the frame from its own bloom/uber
  // chain) would overwrite it and show a blown-out white screen instead. The
  // HDR path has no such conflict: there the debug view is fused into
  // ps_tonemap (mode 3), downstream of everything.
  const bool ao_debug_view =
      ssao_ran && REXCVAR_GET(skate3_native_render_scene_ssao_debug);
  if (!hdr_on && !ao_debug_view &&
      (!scene.photo_fx.valid || gameplay_chain || vr_stereo)) {
    ApplyGamePostfx(context, cmd, scene, viewport, scissor, frame_number,
                    /*composite=*/!gameplay_chain || vr_stereo, &chain_bloom_tex,
                    &chain_bloom_srv);
    post_ran = true;
  }

  if (post_ran) {
    // The AO/HDR chains switched binding layouts; restore the main-pass
    // root bindings the later passes latch (same restore as the photo
    // chain's).
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetBindingLayout(g_r.layout);
    if (g_r.shadow_cb != nullptr) {
      const uint32_t cb_offset =
          uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
      cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
      cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
    }
  }

  if (outline_ready && hdr_on) {
    RenderOutlineComposite(context, scene, context.guest_output, viewport,
                           scissor);
  }

  // ---- Photo-editor postfx chain (photo_fx.hlsl: exact ucode ports) ----
  // While the photo-mission photo editor is up and every pass's live
  // constants were captured this frame, apply the game's own chain over the
  // resolved native frame: depth pack -> visualfx (grade/vignette/CoC) ->
  // DOF downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye.
  if (photo_chain &&
      REXCVAR_GET(skate3_native_render_scene_photo_native) &&
      EnsurePhotoFxPipeline(context)) {
    const auto pfx_to_srv = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
    };
    const auto pfx_to_rt = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    };
    const auto pfx_flush = [&] { cmd->FlushBarriers(); };
    // Output-sized targets (visualfx out, uber out, packed depth) and the
    // output/2 DOF halves.
    bool pfx_ok = true;
    if (g_r.pfx_width != context.guest_output_width ||
        g_r.pfx_height != context.guest_output_height || g_r.pfx_full[0] == nullptr ||
        g_r.pfx_half[0] == nullptr) {
      nrhi::Texture** res[5] = {&g_r.pfx_full[0], &g_r.pfx_full[1], &g_r.pfx_depth,
                                &g_r.pfx_half[0], &g_r.pfx_half[1]};
      nrhi::TextureView** views[5] = {&g_r.pfx_srv[0], &g_r.pfx_srv[1],
                                      &g_r.pfx_srv[5], &g_r.pfx_srv[2],
                                      &g_r.pfx_srv[3]};
      const uint32_t half_w = std::max(1u, (context.guest_output_width + 1) / 2);
      const uint32_t half_h = std::max(1u, (context.guest_output_height + 1) / 2);
      nrhi::TextureDesc desc;
      desc.mip_levels = 1;
      desc.format = nrhi::Format::kR8G8B8A8_UNORM;
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      for (int i = 0; i < 5 && pfx_ok; ++i) {
        desc.width = i < 3 ? context.guest_output_width : half_w;
        desc.height = i < 3 ? context.guest_output_height : half_h;
        if (*views[i] != nullptr) {
          g_r.device->DestroyDeferred(*views[i]);
          *views[i] = nullptr;
        }
        if (*res[i] != nullptr) {
          g_r.device->DestroyDeferred(*res[i]);
          *res[i] = nullptr;
        }
        *res[i] = context.device->CreateTexture(desc);
        if (*res[i] == nullptr) {
          pfx_ok = false;
          break;
        }
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        *views[i] = context.device->CreateTextureView(*res[i], vd);
        if (*views[i] == nullptr) {
          pfx_ok = false;
          break;
        }
      }
      if (pfx_ok) {
        g_r.pfx_width = context.guest_output_width;
        g_r.pfx_height = context.guest_output_height;
        g_r.pfx_half_w = half_w;
        g_r.pfx_half_h = half_h;
      }
    }
    if (pfx_ok) {
      // Identity grade-LUT upload (once); the staging buffer is done after
      // the copy is recorded.
      if (!g_r.pfx_lut_uploaded) {
        cmd->CopyBufferToTexture(g_r.pfx_lut, 0, 0, g_r.pfx_lut_upload, 0, 256,
                                 32, 32, 32);
        cmd->Barrier(g_r.pfx_lut, nrhi::ResourceState::kCopyDest,
                     nrhi::ResourceState::kPixelShaderResource);
        g_r.device->DestroyDeferred(g_r.pfx_lut_upload);
        g_r.pfx_lut_upload = nullptr;
        g_r.pfx_lut_uploaded = true;
      }
      // Native depth SRV, rebuilt every time the chain runs (D32 textures
      // view as R32_FLOAT automatically). The depth texture is recreated on
      // output resize and a new texture can reuse the old address, so a
      // pointer comparison could keep a view of the destroyed depth.
      {
        if (true) {
          if (g_r.pfx_srv[7] != nullptr) {
            g_r.device->DestroyDeferred(g_r.pfx_srv[7]);
            g_r.pfx_srv[7] = nullptr;
          }
          nrhi::TextureViewDesc sd;
          if (g_r.msaa > 1) {
            sd.dimension = nrhi::ViewDimension::k2DMS;
          } else {
            sd.dimension = nrhi::ViewDimension::k2D;
            sd.mip_levels = 1;
          }
          g_r.pfx_srv[7] = context.device->CreateTextureView(g_r.depth, sd);
        }
      }
      // (The guest-output view, the finished native frame = the chain's
      // scene input, is g_r.output_srv_slot, owned/re-pointed by
      // EnsurePipeline on output change.)
      // The two static input textures (fetch words captured at the game's
      // own uber/fisheye flushes): the 512x2 vignette gradient + the grain.
      const GuestTexture* vig =
          resolve_2d_texture(scene.photo_fx.vignette_fetch, /*force_inline=*/true);
      const GuestTexture* grain =
          resolve_2d_texture(scene.photo_fx.grain_fetch, /*force_inline=*/true);
      nrhi::TextureView* const vig_slot =
          (vig != nullptr && vig->valid) ? vig->srv : g_r.white.srv;
      nrhi::TextureView* const grain_slot =
          (grain != nullptr && grain->valid) ? grain->srv : g_r.white.srv;

      // Baked literal rows c250..c255 per pass (the shader asset footers the
      // game loads via PM4 LOAD_ALU_CONSTANT to PS rows 252+; values read
      // from capture).
      static constexpr float kPfxLiterals[kPfxPassCount][6][4] = {
          // visualfx (retail postfx_visualfxPS literal block c253..c255)
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-0.20000000298023224f, 1.0f, 1.41412353515625f, 2.0f},
           {1.5199185327219311e-05f, 0.9960938096046448f, 0.0038909914437681437f, 0.003921568859368563f},
           {0.20000000298023224f, 0.30000001192092896f, 0.5f, 1.0f}},
          // dof downsample
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.000868055562f, 0.00156250002f, 0.25f, 0.00100000005f}},
          // tap9dofMotionBlur
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {1.0f, 1.5f, 9.99999975e-05f, 0.00392156886f},
           {1.51991853e-05f, 0.99609381f, 0.00389099144f, 0.0f}},
          // tap9dof
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.100000001f, 0, 0, 0}},
          // uber
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {2.0f, 0.5f, -1.0f, 0.0f},
           {0.015625f, 0.984375f, 0.96875f, -0.96875f},
           {1.0f, 0, 0, 0}},
          // fisheye
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-1.0f, 0.5f, 0, 0},
           {-0.5f, -0.888888896f, 1.0f, 1.77777779f}},
      };
      const uint32_t cb_slot_base = uint32_t(frame_number % 4) * 32;
      uint32_t cb_slot_next = 0;
      const int32_t pfx_debug =
          REXCVAR_GET(skate3_native_render_scene_photo_native_debug);
      // Returns the byte OFFSET into g_r.pfx_cb for SetConstantBuffer(0, ...).
      const auto fill_cb = [&](int pass) -> uint64_t {
        const uint32_t slot = cb_slot_base + (cb_slot_next++);
        uint8_t* dst = g_r.pfx_cb_ptr + size_t(slot) * 4096;
        std::memset(dst, 0, 4096);
        float* rows = reinterpret_cast<float*>(dst);
        if (pass >= 0) {
          std::memcpy(rows, scene.photo_fx.ps[pass], sizeof(scene.photo_fx.ps[pass]));
          std::memcpy(rows + 240 * 4, scene.photo_fx.vs[pass],
                      sizeof(scene.photo_fx.vs[pass]));
          std::memcpy(rows + 250 * 4, kPfxLiterals[pass], sizeof(kPfxLiterals[pass]));
          // dof downsample c255.xy = one SOURCE texel (the game's 1/1152,
          // 1/640 for its 1152x640 visualfx target): the 4 taps + centre
          // box one 2x2 source block into each half texel. Our source is
          // output-sized, so the step is one output texel.
          if (pass == kPfxDofDown) {
            rows[255 * 4 + 0] = 1.0f / float(context.guest_output_width);
            rows[255 * 4 + 1] = 1.0f / float(context.guest_output_height);
          }
          // The game's quad VSs add half a DEST texel to the sampling uv
          // (visualfx c3 = 0.5/1152,0.5/640; dof passes c3 = 0.5/576,
          // 0.5/320; fisheye c2 = 0.5/1280,0.5/720), D3D9/Xenos raster
          // compensation: their pixel centers interpolate uv = i/W, so
          // +half lands on the intended (i+0.5)/W. Our D3D12 fullscreen
          // triangle interpolates (i+0.5)/W ALREADY; keeping the captured
          // offsets shifts each pass's sampling by half a dest texel
          // (content drifts up-left ~1.5 half-res texels across the three
          // half-res DOF passes = the observed "blur starts above the
          // pole" mask misregistration). ZERO them
          // natively. Guarded to the expected sub-pixel magnitudes so a
          // mis-captured row is left alone.
          {
            float* half_px =
                rows + (pass == kPfxFisheye ? 242 : 243) * 4;
            if (std::fabs(half_px[0]) < 0.002f && std::fabs(half_px[1]) < 0.004f) {
              half_px[0] = 0.0f;
              half_px[1] = 0.0f;
            }
          }
        }
        rows[248 * 4 + 0] = float(context.guest_output_width);
        rows[248 * 4 + 1] = float(context.guest_output_height);
        rows[249 * 4 + 0] = float(pfx_debug);
        // Depth pack: the game-stencil mark (t1) is valid this frame.
        rows[249 * 4 + 1] = g_r.stencil_mark_ready ? 1.0f : 0.0f;
        return uint64_t(slot) * 4096;
      };
      // One 8-entry texture table (pfx_layout param 1, registers t0..t7);
      // the argument order keeps the old per-parameter helper's shape (t5
      // last), the gather re-orders into register order.
      const auto pfx_bind_all = [&](nrhi::TextureView* t0, nrhi::TextureView* t1,
                                    nrhi::TextureView* t2, nrhi::TextureView* t3,
                                    nrhi::TextureView* t4, nrhi::TextureView* t6,
                                    nrhi::TextureView* t7, nrhi::TextureView* t5) {
        nrhi::TextureView* views[8] = {t0, t1, t2, t3, t4, t5, t6, t7};
        cmd->SetTextures(1, views, 8);
      };
      nrhi::TextureView* const W = g_r.white.srv;
      const nrhi::Viewport half_vp{0.0f, 0.0f, float(g_r.pfx_half_w),
                                   float(g_r.pfx_half_h), 0.0f, 1.0f};
      const nrhi::Rect half_sc{0, 0, int32_t(g_r.pfx_half_w),
                               int32_t(g_r.pfx_half_h)};
      const nrhi::Viewport quarter_vp{0.0f, 0.0f,
                                      float(RendererState::kPfxQuarterW),
                                      float(RendererState::kPfxQuarterH), 0.0f,
                                      1.0f};
      const nrhi::Rect quarter_sc{0, 0, int32_t(RendererState::kPfxQuarterW),
                                  int32_t(RendererState::kPfxQuarterH)};
      const auto set_rtv = [&](nrhi::Texture* target) {
        cmd->SetRenderTargets(target, nullptr);
      };
      cmd->SetBindingLayout(g_r.pfx_layout);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      const int32_t accum_mode =
          REXCVAR_GET(skate3_native_render_scene_photo_native_accum);
      // Live capture telemetry (~every 2 s while the chain runs): the rows
      // that move with the editor sliders. Lets a session log confirm the
      // draw-time capture tracks DoF (dof c0.x / dofmb c2.x, 0 = slider
      // off), focus (visualfx c2.y), the grade matrix (visualfx c7) and the
      // uber jitter phase against the emulated ground-truth values.
      {
        static uint64_t s_pfx_log_frame = 0;
        if (frame_number - s_pfx_log_frame > 120) {
          s_pfx_log_frame = frame_number;
          const auto& fx = scene.photo_fx;
          REXLOG_DEBUG(
              "native-scene: pfx live rows: visualfx c2=({:.3f},{:.3f}) "
              "c4.x={:.5f} c7=({:.4f},{:.4f},{:.4f},{:.4f}) | dofmb "
              "c2.x={:.3f} | dof c0.x={:.3f} | uber c0.x={:.3f} "
              "c2.x={:.4f} c5.x={:.3f} | vfx vs c3=({:.6f},{:.6f})",
              fx.ps[kPfxVisualFx][2][0], fx.ps[kPfxVisualFx][2][1],
              fx.ps[kPfxVisualFx][4][0], fx.ps[kPfxVisualFx][7][0],
              fx.ps[kPfxVisualFx][7][1], fx.ps[kPfxVisualFx][7][2],
              fx.ps[kPfxVisualFx][7][3], fx.ps[kPfxDofMB][2][0],
              fx.ps[kPfxDof][0][0], fx.ps[kPfxUber][0][0],
              fx.ps[kPfxUber][2][0], fx.ps[kPfxUber][5][0],
              fx.vs[kPfxVisualFx][3][0], fx.vs[kPfxVisualFx][3][1]);
        }
      }

      // 1) Depth pack: native depth (sample 0) -> the console D24-as-8888
      //    layout at output res.
      cmd->Barrier(g_r.depth, nrhi::ResourceState::kDepthWrite,
                   nrhi::ResourceState::kPixelShaderResource);
      pfx_flush();
      set_rtv(g_r.pfx_depth);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[0]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
      // t1 = the game-stencil mark; without one this frame the depth view
      // stands in (same MS/1x dimension, unread: c249.y = 0).
      pfx_bind_all(W,
                   g_r.stencil_mark_ready ? g_r.stencil_mark_srv : g_r.pfx_srv[7],
                   W, W, g_r.pfx_srv[6], W, W, g_r.pfx_srv[7]);
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.depth, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kDepthWrite);
      pfx_to_srv(g_r.pfx_depth);
      // Scene input -> SRV for the rest of the chain.
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      pfx_flush();

      // 2) Accumulation input (visualfx t3). Gameplay cinematic chain: the
      //    frame's bloom (the game binds its bloom_dof result, 288x160, the
      //    same size as pfx_quarter). Photo editor: mode 0 = black, 1 = the
      //    scene downsampled (this frame, the editor scene is frozen).
      if (scene.photo_fx.gameplay) {
        if (chain_bloom_srv != nullptr) {
          set_rtv(g_r.pfx_quarter);
          cmd->SetViewport(quarter_vp);
          cmd->SetScissor(quarter_sc);
          cmd->SetPipeline(g_r.pfx_pso[7]);
          cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
          pfx_bind_all(chain_bloom_srv, W, W, W, g_r.pfx_srv[6], W, W, W);
          cmd->Draw(3, 0);
          pfx_to_rt(chain_bloom_tex);  // handed over by ApplyGamePostfx
          chain_bloom_tex = nullptr;
          chain_bloom_srv = nullptr;
        } else {
          const float black[4] = {0, 0, 0, 0};
          cmd->ClearRenderTarget(g_r.pfx_quarter, black);
        }
      } else if (accum_mode == 0) {
        const float black[4] = {0, 0, 0, 0};
        cmd->ClearRenderTarget(g_r.pfx_quarter, black);
      } else if (accum_mode == 1) {
        set_rtv(g_r.pfx_quarter);
        cmd->SetViewport(quarter_vp);
        cmd->SetScissor(quarter_sc);
        cmd->SetPipeline(g_r.pfx_pso[7]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.output_srv_slot, W, W, W, g_r.pfx_srv[6], W, W, W);
        cmd->Draw(3, 0);
      }
      pfx_to_srv(g_r.pfx_quarter);
      pfx_flush();

      // 3) visualfx (full res): scene + depth + accumulation -> pfx_full[0].
      set_rtv(g_r.pfx_full[0]);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[1]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxVisualFx));
      pfx_bind_all(g_r.output_srv_slot, g_r.pfx_srv[5], W, g_r.pfx_srv[4],
                   g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_full[0]);
      pfx_flush();

      // 4) DOF downsample: pfx_full[0] -> pfx_half[0].
      set_rtv(g_r.pfx_half[0]);
      cmd->SetViewport(half_vp);
      cmd->SetScissor(half_sc);
      cmd->SetPipeline(g_r.pfx_pso[2]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDofDown));
      pfx_bind_all(g_r.pfx_srv[0], W, W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 5) tap9dofMotionBlur: pfx_half[0] + depth -> pfx_half[1].
      set_rtv(g_r.pfx_half[1]);
      cmd->SetPipeline(g_r.pfx_pso[3]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDofMB));
      pfx_bind_all(g_r.pfx_srv[2], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_flush();

      // 6) tap9dof: pfx_half[1] -> pfx_half[0].
      set_rtv(g_r.pfx_half[0]);
      cmd->SetPipeline(g_r.pfx_pso[4]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxDof));
      pfx_bind_all(g_r.pfx_srv[3], W, W, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 7) uber (full res): graded sharp + depth + LUT + grain + blurred
      //    half -> pfx_full[1].
      set_rtv(g_r.pfx_full[1]);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(g_r.pfx_pso[5]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxUber));
      pfx_bind_all(g_r.pfx_srv[0], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6],
                   grain_slot, g_r.pfx_srv[2], W);
      cmd->Draw(3, 0);
      pfx_to_srv(g_r.pfx_full[1]);
      // The finished chain replaces the output: back to RT for fisheye.
      cmd->Barrier(context.guest_output,
                   nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      pfx_flush();

      // 8) fisheye (to screen): lens warp + vignette gradient + tint.
      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetPipeline(g_r.pfx_pso[6]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(kPfxFisheye));
      pfx_bind_all(g_r.pfx_srv[1], W, vig_slot, W, g_r.pfx_srv[6], W, W, W);
      cmd->Draw(3, 0);

      // 8b) Debug view (photo_native_debug): overwrite the output with the
      //     visualfx CoC map (mode 1) or the packed-depth reconstruction
      //     (mode 2): t0 = visualfx out, t1 = packed depth, both still in
      //     SRV state here.
      if (pfx_debug > 0 && g_r.pfx_pso[8] != nullptr) {
        cmd->SetPipeline(g_r.pfx_pso[8]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.pfx_srv[0], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6], W,
                     W, W);
        cmd->Draw(3, 0);
      }

      // 9) Accumulation mode 2: next frame's visualfx t3 = the finished
      //    frame downsampled (pfx_full[1] is still in SRV state here).
      if (accum_mode == 2) {
        pfx_to_rt(g_r.pfx_quarter);
        pfx_flush();
        set_rtv(g_r.pfx_quarter);
        cmd->SetViewport(quarter_vp);
        cmd->SetScissor(quarter_sc);
        cmd->SetPipeline(g_r.pfx_pso[7]);
        cmd->SetConstantBuffer(0, g_r.pfx_cb, fill_cb(-1));
        pfx_bind_all(g_r.pfx_srv[1], W, W, W, g_r.pfx_srv[6], W, W, W);
        cmd->Draw(3, 0);
      }

      // Restore steady states (all pfx color targets idle as RENDER_TARGET)
      // + the main pass's root bindings for the 2D overlay.
      pfx_to_rt(g_r.pfx_full[0]);
      pfx_to_rt(g_r.pfx_full[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_to_rt(g_r.pfx_half[1]);
      if (accum_mode != 2) {
        pfx_to_rt(g_r.pfx_quarter);
      }
      pfx_to_rt(g_r.pfx_depth);
      pfx_flush();
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetBindingLayout(g_r.layout);
      if (g_r.shadow_cb != nullptr) {
        const uint32_t cb_offset =
            uint32_t(frame_number % RendererState::kShadowCbRegions) *
        RendererState::kShadowCbSlice;
        cmd->SetConstantBuffer(6, g_r.shadow_cb, cb_offset);
        cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      }
      static bool s_pfx_first = true;
      if (s_pfx_first) {
        s_pfx_first = false;
        REXLOG_INFO(
            "native-scene: photo editor postfx chain LIVE (native; accum mode {}, "
            "vignette {}, grain {})",
            accum_mode, vig_slot == W ? "WHITE-fallback" : "resolved",
            grain_slot == W ? "WHITE-fallback" : "resolved");
      }
    }
  }
  // The chain did not consume the handed-over bloom (chain disabled or its
  // pipeline unavailable): back to its steady RENDER_TARGET state.
  if (chain_bloom_tex != nullptr) {
    cmd->Barrier(chain_bloom_tex, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
  }

  // Fisheye-family final pass (camera-man / filmer lens, respawn TV noise;
  // captured at the game's own suppressed draw): the finished frame through
  // a copy (lens_tex) and the variant's exact port back out, before the 2D
  // overlay like the game's postfx.
  //
  // The photo-chain exclusion is only about the FISHEYE variants: the photo
  // editor runs its own fisheye inside that chain, so replaying ours as well
  // would double-apply it. The respawn TV noise is not part of the chain (see
  // the pfx_pass_of classifier, which deliberately returns -1 for
  // postfx_basictex_fisheye_noise), yet it fires on exactly the frames where
  // the bail/replay "gameplay cinematic" chain goes live -- same millisecond
  // in the logs -- so a blanket !photo_chain dropped the noise pass every
  // time. Exclude only the fisheye kinds.
  const bool lens_blocked_by_chain =
      photo_chain && scene.lens_fx.kind != FrameScene::LensFx::kNoise;
  if (scene.lens_fx.valid && !lens_blocked_by_chain && !vr_stereo &&
      g_r.output_srv_slot != nullptr &&
      g_r.pso_blur_blit != nullptr && EnsurePhotoFxPipeline(context) &&
      EnsureLensFxPipeline(context)) {
    bool lens_ok = true;
    if (g_r.lens_tex == nullptr || g_r.lens_width != context.guest_output_width ||
        g_r.lens_height != context.guest_output_height ||
        g_r.lens_tex->format() != context.guest_output->format()) {
      if (g_r.lens_srv != nullptr) {
        g_r.device->DestroyDeferred(g_r.lens_srv);
        g_r.lens_srv = nullptr;
      }
      if (g_r.lens_tex != nullptr) {
        g_r.device->DestroyDeferred(g_r.lens_tex);
        g_r.lens_tex = nullptr;
      }
      nrhi::TextureDesc desc;
      desc.width = context.guest_output_width;
      desc.height = context.guest_output_height;
      desc.mip_levels = 1;
      desc.format = context.guest_output->format();
      desc.usage = nrhi::kTextureUsageRenderTarget;
      desc.initial_state = nrhi::ResourceState::kRenderTarget;
      g_r.lens_tex = context.device->CreateTexture(desc);
      if (g_r.lens_tex != nullptr) {
        nrhi::TextureViewDesc vd;
        vd.mip_levels = 1;
        g_r.lens_srv = context.device->CreateTextureView(g_r.lens_tex, vd);
      }
      lens_ok = g_r.lens_tex != nullptr && g_r.lens_srv != nullptr;
      g_r.lens_width = lens_ok ? context.guest_output_width : 0;
      g_r.lens_height = lens_ok ? context.guest_output_height : 0;
    }
    if (lens_ok) {
      const GuestTexture* vig =
          resolve_2d_texture(scene.lens_fx.vignette_fetch, /*force_inline=*/true);
      // tf5 is only the noise pass's input. The fisheye variants never
      // sample it and the game leaves arbitrary, ever-changing textures
      // bound there; binding those made every frame a new view tuple, and
      // each tuple pins a cached shader-visible table until one of its
      // views dies (heap exhausted within seconds -> black screen).
      const GuestTexture* noise =
          scene.lens_fx.kind == FrameScene::LensFx::kNoise
              ? resolve_2d_texture(scene.lens_fx.noise_fetch, /*force_inline=*/true)
              : nullptr;
      nrhi::TextureView* const W = g_r.white.srv;
      nrhi::TextureView* const vig_slot = (vig != nullptr && vig->valid) ? vig->srv : W;
      nrhi::TextureView* const noise_slot =
          (noise != nullptr && noise->valid) ? noise->srv : W;
      // Constant slot: this frame's pfx CB region, the lens pass's slot.
      const uint32_t slot = uint32_t(frame_number % 4) * 32 + 10;
      uint8_t* dst = g_r.pfx_cb_ptr + size_t(slot) * 4096;
      std::memset(dst, 0, 4096);
      float* rows = reinterpret_cast<float*>(dst);
      std::memcpy(rows, scene.lens_fx.ps, sizeof(scene.lens_fx.ps));
      std::memcpy(rows + 240 * 4, scene.lens_fx.vs, sizeof(scene.lens_fx.vs));
      // fisheyePS / fisheye_opaquePS literal rows (identical in both shader
      // asset footers; the noise port carries its own inline).
      static constexpr float kLensLiterals[2][4] = {
          {-1.0f, 0.5f, 0.0f, 0.0f}, {-0.5f, -0.888888896f, 1.0f, 1.77777779f}};
      std::memcpy(rows + 254 * 4, kLensLiterals, sizeof(kLensLiterals));
      const int lens_kind = std::clamp(scene.lens_fx.kind, 0, 2);
      // Same quad VS as the fisheye pass: its c2 half-dest-texel raster
      // offset is already in our fullscreen triangle's interpolation (see
      // the photo chain's fill_cb).
      {
        float* half_px = rows + 242 * 4;
        if (std::fabs(half_px[0]) < 0.002f && std::fabs(half_px[1]) < 0.004f) {
          half_px[0] = 0.0f;
          half_px[1] = 0.0f;
        }
      }
      const uint64_t cb_offset = uint64_t(slot) * 4096;
      // 1) Finished frame -> lens_tex through the main layout's SINGLE-view
      //    table. output_srv_slot is a new view every frame, and the D3D12
      //    backend recycles freed shader-visible slots only for 1-view
      //    tables (SlotAllocator::Alloc: multi-view tables always take fresh
      //    slots), so it must never sit in the 8-view pfx table: that leaked
      //    8 slots per frame and exhausted the heap in seconds (black
      //    screen). Same bind as the gamma pass.
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->FlushBarriers();
      cmd->SetRenderTargets(g_r.lens_tex, nullptr);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetBindingLayout(g_r.layout);
      cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
      cmd->SetPipeline(g_r.pso_blur_blit);
      cmd->SetTexture(1, g_r.output_srv_slot);
      cmd->Draw(3, 0);
      // 2) The variant's port, lens_tex -> output. Every view in this
      //    table persists across frames, so its cached table is reused.
      cmd->Barrier(g_r.lens_tex, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
      cmd->Barrier(context.guest_output, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetBindingLayout(g_r.pfx_layout);
      cmd->SetPipeline(g_r.lens_pso[lens_kind]);
      cmd->SetConstantBuffer(0, g_r.pfx_cb, cb_offset);
      {
        nrhi::TextureView* views[8] = {g_r.lens_srv, W, vig_slot, noise_slot,
                                       g_r.pfx_srv[6], W, W, W};
        cmd->SetTextures(1, views, 8);
      }
      cmd->Draw(3, 0);
      cmd->Barrier(g_r.lens_tex, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
      cmd->FlushBarriers();
      // The main pass's root bindings for the 2D overlay.
      cmd->SetBindingLayout(g_r.layout);
      if (g_r.shadow_cb != nullptr) {
        const uint32_t shadow_cb_offset =
            uint32_t(frame_number % RendererState::kShadowCbRegions) *
            RendererState::kShadowCbSlice;
        cmd->SetConstantBuffer(6, g_r.shadow_cb, shadow_cb_offset);
        cmd->SetConstantBuffer(9, g_r.bone_ring, bone_region);
      }
      static bool s_lens_first[3] = {true, true, true};
      if (s_lens_first[lens_kind]) {
        s_lens_first[lens_kind] = false;
        REXLOG_INFO("native-scene: lens postfx pass LIVE (kind {}, vignette {}, noise {})",
                    lens_kind, vig_slot == W ? "WHITE-fallback" : "resolved",
                    noise_slot == W ? "WHITE-fallback" : "resolved");
      }
    }
  }

  // Hor+ ultrawide: the 2D stream and the guest screenshot raster are
  // authored for the 16:9 frontbuffer. On a wide output the 2D replay
  // scales clip-space X by this factor (centering ortho HUD/menu/FMV draws
  // in a pillarboxed 16:9 band and narrowing perspective SimpleDraw markers
  // to match the widened scene projection - one operation covers both), and
  // the photo grab center-crops the same band.
  // (One-pass VR stereo: per eye; the HUD draws into each eye's half.)
  const float out_aspect2d = (vr_stereo ? 0.5f : 1.0f) * float(context.guest_output_width) /
                             float(context.guest_output_height);
  const float wide_2d_scale =
      out_aspect2d > (16.0f / 9.0f) * 1.01f ? (16.0f / 9.0f) / out_aspect2d : 1.0f;
  // Taller-than-16:9 output: the same band vertically (clip Y), matching
  // the Vert+ scene camera.
  const float tall_2d_scale =
      out_aspect2d < (16.0f / 9.0f) * 0.99f ? out_aspect2d / (16.0f / 9.0f) : 1.0f;

  cmd->ProfileRegion(nrhi::ProfileStage::k2d);

  // ---- Native photo grab (photo_grab_native) ----
  // Consume: CPU-tile the newest fence-completed readback into the guest
  // screenshot target. Produce: downsample this frame's finished output
  // (photo editor = the ported postfx chain result just drawn above; plain
  // TakePhoto window = the plain frame, both the correct grab semantics)
  // into blur_tex[0] via the blur chain's prefiltered downsample (its
  // 1152x640 blur space IS the game's screenshot raster) and enqueue the
  // copy into the free readback buffer. Runs BEFORE the 2D overlay so the
  // photo never contains editor chrome / HUD, and before the blur block so
  // this scratch use of blur_tex[0] is enqueued ahead of any blur rewrite.
  // Keep-warm: outside a photo flow a readback of the current frame is still
  // produced every 8th frame (GPU copy only), so the game's grab execution
  // (SyncWriteNewestGrab, guest thread) always finds a frame at most a few
  // frames old, even when it runs before the armed flow has produced one
  // (the second photo of a session showed the first one's image).
  const bool grab_armed = g_photo_grab_native_armed.load(std::memory_order_relaxed);
  std::unique_lock<std::mutex> grab_lock(g_grab_mutex, std::defer_lock);
  if ((grab_armed || (frame_number & 7u) == 0) &&
      g_r.pso_blur_down != nullptr && g_r.blur_tex[0] != nullptr &&
      g_r.output_srv_allocated && !g_r.grab_failed && grab_lock.try_lock()) {
    // Lazy readback-pair creation (persistently mapped; the copy is never
    // waited on same-frame; the old path's per-resolve GPU drain was the
    // editor's multi-second stall class).
    if (g_r.grab_readback[0] == nullptr) {
      nrhi::BufferDesc desc;
      desc.size =
          uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight;
      desc.heap = nrhi::HeapKind::kReadback;
      for (int i = 0; i < 2 && !g_r.grab_failed; ++i) {
        g_r.grab_readback[i] = g_r.device->CreateBuffer(desc);
        if (g_r.grab_readback[i] == nullptr ||
            (g_r.grab_readback_ptr[i] = static_cast<uint8_t*>(
                 g_r.device->Map(g_r.grab_readback[i]))) == nullptr) {
          g_r.grab_failed = true;
        }
      }
      if (g_r.grab_failed) {
        REXLOG_ERROR(
            "native-scene: photo grab readback creation failed - native "
            "grab disabled (watchdog will fall back)");
      }
    }
    if (!g_r.grab_failed) {
      // Consume the newest completed buffer; retire older completed ones
      // unwritten so stale content never lands after fresh content.
      const uint64_t grab_completed = context.device->CompletedSubmission();
      int newest = -1;
      for (int i = 0; i < 2; ++i) {
        if (g_r.grab_pending[i] && g_r.grab_submission[i] < grab_completed &&
            (newest < 0 ||
             g_r.grab_submission[i] > g_r.grab_submission[newest])) {
          newest = i;
        }
      }
      if (!grab_armed) {
        // Keep-warm only: retire completed buffers unwritten (their content
        // stays readable for SyncWriteNewestGrab until reused).
        if (newest >= 0) {
          const size_t bytes =
              size_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight;
          g_r.device->InvalidateForRead(g_r.grab_readback[newest], 0, bytes);
          g_grab_cache.assign(g_r.grab_readback_ptr[newest],
                              g_r.grab_readback_ptr[newest] + bytes);
        }
        for (int i = 0; i < 2; ++i) {
          if (g_r.grab_pending[i] && g_r.grab_submission[i] < grab_completed) {
            g_r.grab_pending[i] = false;
          }
        }
        newest = -1;
      }
      if (newest >= 0) {
        static bool s_grab_write_warned = false;
        const auto tile_t0 = PerfClock::now();
        // Make the GPU's completed copy visible to the CPU read below (no-op
        // on D3D12; cache invalidate on non-coherent Vulkan memory).
        g_r.device->InvalidateForRead(
            g_r.grab_readback[newest], 0,
            uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight);
        g_grab_cache.assign(
            g_r.grab_readback_ptr[newest],
            g_r.grab_readback_ptr[newest] +
                size_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight);
        GrabTarget targets[2];
        const int target_count = ResolveGrabTargets(base, targets);
        bool wrote = target_count > 0;
        for (int ti = 0; ti < target_count; ++ti) {
          wrote = WriteGrabToGuest(base, g_r.grab_readback_ptr[newest], targets[ti]) && wrote;
        }
        if (wrote) {
          g_r.grab_cpu_us +=
              uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - tile_t0)
                           .count());
          ++g_r.grab_writes;
          g_grab_last_write_ns.store(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  PerfClock::now().time_since_epoch())
                  .count(),
              std::memory_order_relaxed);
          if (g_r.grab_writes == 1) {
            for (int ti = 0; ti < target_count; ++ti) {
              REXLOG_INFO(
                  "native-scene: photo grab - native frame written to guest "
                  "0x{:08X} ({}x{} pitch {} {})",
                  targets[ti].addr, targets[ti].width, targets[ti].height, targets[ti].pitch,
                  targets[ti].tiled ? "tiled" : "linear");
            }
          }
        } else if (!s_grab_write_warned) {
          s_grab_write_warned = true;
          REXLOG_WARN(
              "native-scene: photo grab - no writable screenshot target "
              "(RenderMan chain unreadable or unexpected format); the window "
              "watchdog will fall back to forced readbacks");
        }
        for (int i = 0; i < 2; ++i) {
          if (g_r.grab_pending[i] && g_r.grab_submission[i] < grab_completed) {
            g_r.grab_pending[i] = false;
          }
        }
      }
      // Produce into the free buffer (skip the frame if both are still in
      // flight, never wait).
      const uint32_t w = g_r.grab_write_index;
      if (!g_r.grab_pending[w]) {
        // (The output view g_r.output_srv_slot tracks the current output
        // texture; EnsurePipeline re-points it on output change.)
        cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kPixelShaderResource);
        cmd->FlushBarriers();
        cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
        const nrhi::Viewport grab_vp{0.0f,
                                     0.0f,
                                     float(RendererState::kBlurWidth),
                                     float(RendererState::kBlurHeight),
                                     0.0f,
                                     1.0f};
        const nrhi::Rect grab_sc{0, 0, int32_t(RendererState::kBlurWidth),
                                 int32_t(RendererState::kBlurHeight)};
        cmd->SetViewport(grab_vp);
        cmd->SetScissor(grab_sc);
        cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
        cmd->SetPipeline(g_r.pso_blur_down);
        // Source UV rect (second register): the centered 16:9 band on a
        // wide output; the guest screenshot raster is 16:9.
        const float grab_consts[8] = {1.0f / float(context.guest_output_width),
                                      1.0f / float(context.guest_output_height),
                                      0.0f,
                                      0.0f,
                                      wide_2d_scale,
                                      tall_2d_scale,
                                      (1.0f - wide_2d_scale) * 0.5f,
                                      (1.0f - tall_2d_scale) * 0.5f};
        cmd->SetRootConstants(0, 8, grab_consts, 0);
        cmd->SetTexture(1, g_r.output_srv_slot);
        cmd->Draw(3, 0);
        // blur_tex[0] -> the readback buffer; restore steady states.
        cmd->Barrier(g_r.blur_tex[0], nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kCopySource);
        cmd->Barrier(context.guest_output,
                     nrhi::ResourceState::kPixelShaderResource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        cmd->CopyTextureToBuffer(g_r.grab_readback[w], 0,
                                 RendererState::kGrabRowPitch, g_r.blur_tex[0],
                                 0, RendererState::kBlurWidth,
                                 RendererState::kBlurHeight);
        cmd->Barrier(g_r.blur_tex[0], nrhi::ResourceState::kCopySource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        // Restore the frame's output binding for the passes that follow.
        cmd->SetRenderTargets(context.guest_output, nullptr);
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);
        g_r.grab_submission[w] = context.device->CurrentSubmission();
        g_r.grab_pending[w] = true;
        static bool s_first_produce = false;
        if (!s_first_produce) {
          s_first_produce = true;
          REXLOG_INFO("native-scene: screenshot readback produced ({})",
                      grab_armed ? "armed" : "keep-warm");
        }
        g_r.grab_write_index = 1 - w;
      }
    }
  }

  // Popup background blur: exact port of the game's blur_hBlur/vBlur +
  // postfx_basictex chain (see kBlurShaderSource): H blur of the finished
  // frame into a 1152x640 intermediate, V blur, then a fullscreen bilinear
  // stretch back over the output. Runs only on frames where the game issued
  // the blur draws (scene.ui_blur = captured kernel scale). The popup's own
  // 2D draws follow after and stay sharp.
  if (scene.ui_blur > 0.0f && g_r.pso_blur != nullptr && g_r.pso_blur_blit != nullptr &&
      g_r.pso_blur_down != nullptr && g_r.blur_tex[0] != nullptr) {
    // Temporal smoothing of the captured radius + fade (~50 ms time
    // constant, render thread): the live bank reads can land mid-rewrite
    // (1-frame value flaps = the tinted-backdrop flicker), and smoothing
    // FROM the off state (radius 0, white tint) supplies the pause-open
    // animate-in even when the game's own ramp frames were missed by the
    // capture gate. A >0.25 s gap since the last blur frame = a fresh ON
    // edge (resets the state); at the render rate active frames are ~7 ms
    // apart so the reset can never fire mid-popup.
    static float s_blur_r = 0.0f;
    static float s_blur_tint[3] = {1.0f, 1.0f, 1.0f};
    static int64_t s_blur_ns = 0;
    const int64_t blur_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    PerfClock::now().time_since_epoch())
                                    .count();
    float blur_dt = s_blur_ns != 0 ? float(blur_now_ns - s_blur_ns) * 1e-9f : 1.0f;
    if (blur_dt > 0.25f) {
      s_blur_r = 0.0f;
      s_blur_tint[0] = s_blur_tint[1] = s_blur_tint[2] = 1.0f;
      blur_dt = 0.0f;
    }
    s_blur_ns = blur_now_ns;
    const float blur_a = 1.0f - std::exp(-blur_dt / 0.05f);
    s_blur_r += (scene.ui_blur - s_blur_r) * blur_a;
    for (int a = 0; a < 3; ++a) {
      s_blur_tint[a] += (scene.ui_blur_color[a] - s_blur_tint[a]) * blur_a;
    }
    // (The guest output texture can change between frames; the dedicated
    // view g_r.output_srv_slot is re-pointed by EnsurePipeline on change.)
    const nrhi::Viewport blur_vp{0.0f, 0.0f, float(RendererState::kBlurWidth),
                                 float(RendererState::kBlurHeight), 0.0f, 1.0f};
    const nrhi::Rect blur_sc{0, 0, int32_t(RendererState::kBlurWidth),
                             int32_t(RendererState::kBlurHeight)};
    const auto to_srv = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kRenderTarget,
                   nrhi::ResourceState::kPixelShaderResource);
    };
    const auto to_rt = [&](nrhi::Texture* r) {
      cmd->Barrier(r, nrhi::ResourceState::kPixelShaderResource,
                   nrhi::ResourceState::kRenderTarget);
    };
    const auto flush = [&] { cmd->FlushBarriers(); };
    // The masked variant (blur_hBlurMaskedPS / blur_vBlurMaskedPS, the
    // pick-an-object menu): the horizontal pass only -- the masked "vBlur"
    // ucode is a plain copy (`max oC0, r0, r0`) -- and the foreground
    // preview object's pixels keep the sharp, untinted frame (hBlurMasked
    // 24-39: the tf1 marker within 0.05 of 128/255 selects the centre tap
    // with no c1 multiply). The marker's coverage is the object's
    // silhouette: its first pass draws depth-ALWAYS (see pso_fg_inner).
    // The mark is the foreground draws' own stencil coverage, rendered in
    // the scene pass (see RendererState::fg_mark).
    nrhi::Pipeline* const masked_blit =
        g_r.fg_mark_samples > 1 ? g_r.pso_blur_blit_masked_ms : g_r.pso_blur_blit_masked;
    // Masked mode = the masked chain's shape (horizontal pass only, one c1
    // tint) whether or not a mark exists this frame; without one the plain
    // blit composites (nothing is kept sharp). Falling back to the regular
    // H+V chain tinted twice: the "extra dark filter".
    const bool blur_masked = scene.ui_blur_masked && masked_blit != nullptr;
    const bool mark_live = g_r.fg_mark_ready && g_r.fg_mark_srv != nullptr;
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    // Downsample (prefiltered): output -> blur_tex[0], the game's 1152x640
    // blur space. The H/V passes then run 1:1 like the original chain.
    to_srv(context.guest_output);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
    cmd->SetViewport(blur_vp);
    cmd->SetScissor(blur_sc);
    cmd->SetPipeline(g_r.pso_blur_down);
    // Full-frame source rect: the blur squeezes the whole wide frame into
    // the 1152x640 blur space and the final stretch-back undoes it.
    const float d_consts[8] = {1.0f / float(context.guest_output_width),
                               1.0f / float(context.guest_output_height),
                               0.0f,
                               0.0f,
                               1.0f,
                               1.0f,
                               0.0f,
                               0.0f};
    cmd->SetRootConstants(0, 8, d_consts, 0);
    cmd->SetTexture(1, g_r.output_srv_slot);
    cmd->Draw(3, 0);
    // H: blur_tex[0] -> blur_tex[1].
    to_srv(g_r.blur_tex[0]);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[1], nullptr);
    cmd->SetPipeline(g_r.pso_blur);
    const float h_consts[8] = {1.0f,
                               0.0f,
                               s_blur_r,
                               0.0f,
                               s_blur_tint[0],
                               s_blur_tint[1],
                               s_blur_tint[2],
                               1.0f};
    cmd->SetRootConstants(0, 8, h_consts, 0);
    cmd->SetTexture(1, g_r.blur_srv[0]);
    cmd->Draw(3, 0);
    if (blur_masked) {
      // No vertical pass; stretch the H result back with the mask discard.
      to_srv(g_r.blur_tex[1]);
      to_rt(context.guest_output);
      flush();
      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      cmd->SetPipeline(mark_live ? masked_blit : g_r.pso_blur_blit);
      cmd->SetTexture(1, g_r.blur_srv[1]);
      if (mark_live) {
        cmd->SetTexture(2, g_r.fg_mark_srv);
      }
      cmd->Draw(3, 0);
      to_rt(g_r.blur_tex[0]);
      to_rt(g_r.blur_tex[1]);
    } else {
    // V: blur_tex[1] -> blur_tex[0].
    to_srv(g_r.blur_tex[1]);
    to_rt(g_r.blur_tex[0]);
    flush();
    cmd->SetRenderTargets(g_r.blur_tex[0], nullptr);
    const float v_consts[8] = {0.0f,
                               1.0f,
                               s_blur_r,
                               0.0f,
                               s_blur_tint[0],
                               s_blur_tint[1],
                               s_blur_tint[2],
                               1.0f};
    cmd->SetRootConstants(0, 8, v_consts, 0);
    cmd->SetTexture(1, g_r.blur_srv[1]);
    cmd->Draw(3, 0);
    // Replace: blur_tex[0] stretched over the full output (basictex).
    to_srv(g_r.blur_tex[0]);
    to_rt(context.guest_output);
    flush();
    cmd->SetRenderTargets(context.guest_output, nullptr);
    cmd->SetViewport(viewport);
    cmd->SetScissor(scissor);
    cmd->SetPipeline(g_r.pso_blur_blit);
    cmd->SetTexture(1, g_r.blur_srv[0]);
    cmd->Draw(3, 0);
    // Restore the intermediates' steady state for the next blur frame.
    to_rt(g_r.blur_tex[0]);
    to_rt(g_r.blur_tex[1]);
    }
  }

  // 2D overlay (HUD/APT): replay the frame's captured 2D draws over the
  // resolved output, in submission order, with the game's own transform
  // constants and textures. In gameplay these draws compose the game's
  // full-screen HUD overlay texture at true screen coordinates; drawing
  // them here IS the composite the (suppressed) emulated pass used to do.
  const auto twod_t0 = PerfClock::now();
  uint32_t drawn_2d = 0;
  if (REXCVAR_GET(skate3_native_render_scene_2d) && g_r.pso_2d != nullptr &&
      g_r.ui_ring_cpu != nullptr) {
    std::vector<Draw2d> scene_2d;
    if (skate3::vr::CurrentEye() == 1) {
      scene_2d = g_vr_eye_2d;  // the left eye's stream (see g_vr_eye_scene)
    } else {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_2d = g_scene_2d;
    }
    if (skate3::vr::CurrentEye() == 0) {
      g_vr_eye_2d = scene_2d;
    }
    if (!scene_2d.empty()) {
      cmd->SetPipeline(g_r.pso_2d);
      // One shared draw routine for both the RTT passes and the screen pass.
      // ui_region/ui_offset continue after the spline pass's allocations.
      // yuv != nullptr redirects this draw to the FMV combine: pso_yuv2d
      // with the three movie plane views on params 1/2/5 (t0/t1/t4), the
      // quad's own geometry/transform (so windowed movies place exactly).
      // VR eye pass: the HUD layer hangs as a head-locked panel (ortho
      // draws are mapped onto it) and world-space markers follow the eye.
      // Default: the HUD sits on the game camera's own 16:9 screen (follows
      // the game camera, spans the flat game's field of view). Recomputed per
      // eye in one-pass stereo (refresh_vr_hud under the eye loop below).
      float vr_hud[4] = {};
      bool vr_hud_on = false;
      float vr_hud_m[16];
      bool vr_hud_cam = false;
      const auto refresh_vr_hud = [&]() {
        vr_hud_on = skate3::vr::CurrentEye() >= 0 &&
                    skate3::vr::HudTransform(wide_2d_scale, tall_2d_scale, &vr_hud[0],
                                             &vr_hud[1], &vr_hud[2], &vr_hud[3]);
        // Scene-less frames (boot menus, loading) have no live game camera:
        // their UI goes on the head-locked panel.
        vr_hud_cam = skate3::vr::CurrentEye() >= 0 && !scene.items.empty() &&
                     skate3::vr::HudGameCameraMatrix(wide_2d_scale, tall_2d_scale,
                                                     scene.guest_cam_proj11, vr_hud_m);
      };
      refresh_vr_hud();
      // Curved third-person HUD (skate3_vr_hud_curve_x/_y): pass 0 draws the HUD
      // layer (ortho, not a screen fill, not video) flat into g_vr_hud_tex,
      // exactly as on a 16:9 screen; the texture is then wrapped onto each
      // eye's curved mesh, and the eye pass draws everything else (fades,
      // world-space markers, video) as before. Same content, only the shape.
      bool vr_hud_tex_pass = false;
      float e2d_wide = wide_2d_scale, e2d_tall = tall_2d_scale;
      std::vector<float> vr_curve_mesh[2];
      const bool vr_curved_hud =
          vr_stereo && !scene.items.empty() && EnsureVrHudTexture(context) &&
          skate3::vr::HudCurveMesh(0, scene.guest_cam_proj11, vr_curve_mesh[0]) &&
          skate3::vr::HudCurveMesh(1, scene.guest_cam_proj11, vr_curve_mesh[1]);
      NativeGuestOutputRenderContext vr_hud_ctx = context;  // HUD-texture pipelines
      if (vr_curved_hud) {
        vr_hud_ctx.guest_output = g_vr_hud_tex;
      }
      // Untextured fills that cover the game's whole screen (screen fades:
      // the challenge/transition fade quad, which the game also leaves
      // drawing at its last ramp step, 1/30 black, after the fade ends) are
      // screen-space, not HUD layout: on the console they tint every pixel,
      // so they must cover the whole output here too. Squeezed into the
      // 16:9 band they tinted only the middle of a wide/tall output (a
      // visible band edge). Detected in clip space: the draw's own
      // projection maps its corners to (-1,-1)..(1,1).
      const auto screen_fill_of = [&](const Draw2d& d, bool video) -> bool {
        if (video || (wide_2d_scale == 1.0f && tall_2d_scale == 1.0f) ||
            !(d.src_stride == 16 || d.src_stride == 32 ||
              (d.src_stride == 20 && (d.flags & 0x30u) == 0x20u))) {
          return false;
        }
        float cx0 = 1e9f, cx1 = -1e9f, cy0 = 1e9f, cy1 = -1e9f;
        for (size_t o = 0; o + 28 <= d.verts.size(); o += 28) {
          float p[4];
          std::memcpy(p, d.verts.data() + o, 16);
          const float cx = p[0] * d.consts[0] + p[1] * d.consts[1] +
                           p[2] * d.consts[2] + p[3] * d.consts[3];
          const float cy = p[0] * d.consts[4] + p[1] * d.consts[5] +
                           p[2] * d.consts[6] + p[3] * d.consts[7];
          const float cw = p[0] * d.consts[12] + p[1] * d.consts[13] +
                           p[2] * d.consts[14] + p[3] * d.consts[15];
          if (std::fabs(cw) < 1e-6f) {
            return false;
          }
          cx0 = std::min(cx0, cx / cw);
          cx1 = std::max(cx1, cx / cw);
          cy0 = std::min(cy0, cy / cw);
          cy1 = std::max(cy1, cy / cw);
        }
        return cx0 <= -0.99f && cx1 >= 0.99f && cy0 <= -0.99f && cy1 >= 0.99f;
      };
      // Curved HUD mesh, uploaded once per frame into the UI ring (every
      // stream segment composites the same mesh; see hud_segments).
      uint32_t vr_curve_mesh_offset[2] = {UINT32_MAX, UINT32_MAX};
      const auto emit_draw = [&](const Draw2d& d,
                                 nrhi::TextureView* const* yuv = nullptr) {
        const uint32_t bytes = uint32_t(d.verts.size());
        if (bytes == 0 || d.stride != 28) {
          return;
        }
        if ((d.color_mask & 0xFu) == 0) {
          return;  // RB_COLOR_MASK 0: the game writes no color for this draw
        }
        if (ui_offset + bytes > RendererState::kUiRegionSize) {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, d.verts.data(), bytes);
        nrhi::TextureView* srv_view;
        if (yuv != nullptr) {
          srv_view = yuv[0];
        } else {
          // The browser page quad (see Publish2dDraws: bare SimpleDraw while
          // FE screen 60 is up).
          const bool browser_page = (d.flags & 0x0Fu) == 0 && (d.flags & 0x20u) != 0 &&
                                    g_browser_screen_open.load(std::memory_order_relaxed);
          const GuestTexture* t = resolve_2d_texture(d.fetch, /*force_inline=*/false, /*video=*/false, d.font_id,
                                                   browser_page);
          if (t == nullptr) {
            // Big-art decode in flight on the workers (large-art async
            // routing); skip the quad; it lands 1-3 frames later.
            return;
          }
          srv_view = t->srv;
        }
        for (int vr_e=0; vr_e<(vr_stereo && !vr_hud_tex_pass?2:1); ++vr_e) {
        if (vr_stereo && !vr_hud_tex_pass) {
          skate3::vr::SetCurrentEye(vr_e);
          refresh_vr_hud();
          vr_eye_viewport(cmd,vr_e);
        }
        float constants[52] = {};
        std::memcpy(constants, d.consts, sizeof(d.consts));
        // SimpleDraw colour source, per the game's VS variants: simpledraw
        // p_c / p_c_uv output the VERTEX colour only (max o0, r0, r0) while
        // p_sc / p_sc_uv output c8 only (max o0, c8, c8). ps_main multiplies
        // both, so a vertex-coloured draw (stride 20 pos4+color, stride 28
        // pos4+color+uv) was also tinted by whatever c8 the previous draw
        // left staged; debug-menu boxes changed tone between the title and
        // item rows. Vertex-colour layouts take c8 = 1 (the colour is the
        // vertex's alone); c8-coloured layouts already carry white vertices.
        const bool vertex_colored =
            (d.src_stride == 20 && (d.flags & 0x10u) == 0 && (d.flags & 0x20u) != 0) ||
            (d.src_stride == 28 && ((d.flags & 0x20u) != 0 || d.flags == 0)) ||
            (d.src_stride == 32 && (d.flags & 0x20u) != 0);
        if (vertex_colored && yuv == nullptr) {
          constants[32] = 1.0f;
          constants[33] = 1.0f;
          constants[34] = 1.0f;
          constants[35] = 1.0f;
        }
        const bool screen_fill = screen_fill_of(d, yuv != nullptr);  // see screen_fill_of
        // 2D ortho draws have no translation row in the projection (c3 ==
        // (0,0,0,1)); perspective view-proj rows do. Half-pixel applies to
        // the former only -- and so does the pillarbox/letterbox fit below.
        const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                           d.consts[14] == 0.0f && d.consts[15] == 1.0f;
        // Curved HUD routing (see vr_curved_hud): the HUD layer only in the
        // texture pass, everything else only in the eye pass.
        if (vr_curved_hud && vr_hud_tex_pass != (ortho && !screen_fill && yuv == nullptr)) {
          break;
        }
        // Wide output: scale the staged projection's clip.x row (the VS
        // computes clip.x = dot(wp, c0)) so the 16:9-authored stream lands
        // centered (see wide_2d_scale).
        //
        // ORTHO ONLY. Some SimpleDraw batches are WORLD-space geometry
        // carrying the scene's own perspective view_proj (the skate.Park
        // rotation gizmo's rings are these: capture 1790955000 draws
        // 1637-1639, whose c0..c3 equal the frame's view_proj). Those are
        // not part of the 16:9 UI stream -- they must match the 3D scene,
        // which already renders at the true output aspect. Scaling their
        // clip.x squeezed the gizmo's circles into narrow ellipses on an
        // ultrawide output.
        // (e2d_wide / e2d_tall: the band of this pass; 1 in the curved HUD's
        // texture pass, whose target is exactly 16:9.)
        if (ortho && e2d_wide != 1.0f && !screen_fill) {
          constants[0] *= e2d_wide;
          constants[1] *= e2d_wide;
          constants[2] *= e2d_wide;
          constants[3] *= e2d_wide;
        }
        // Tall output: the clip.y row (c1), centering the stream vertically.
        if (ortho && e2d_tall != 1.0f && !screen_fill) {
          constants[4] *= e2d_tall;
          constants[5] *= e2d_tall;
          constants[6] *= e2d_tall;
          constants[7] *= e2d_tall;
        }
        constants[36] = ortho ? 1.0f : 0.0f;
        // m[9].y: sharp-magnification amount for APT cached-bitmap tiles
        // (see the cvar; the shader gates on actual fetch magnification).
        constants[37] = float(std::clamp(
            REXCVAR_GET(skate3_native_render_scene_2d_sharp), 0.0, 2.0));
        // m[9].z: clip-space X extent of the 2D band, the edge-snap
        // boundary in the VS (0 = full target). m[9].w: unused. The D3D9
        // half-pixel shift is derived in the VS from the draw's own ortho
        // scale (+0.5 GUEST pixel down-right, matching the emulated path's
        // half_pixel_offset reversal); the old up-left OUTPUT-pixel nudge
        // staged here left a see-through sliver along the bottom/right of
        // edge-to-edge loading quads.
        constants[38] = (e2d_wide != 1.0f && !screen_fill) ? e2d_wide : 0.0f;
        // m[9].w: clip-space Y extent of the band on a tall output.
        constants[39] = (e2d_tall != 1.0f && !screen_fill) ? e2d_tall : 0.0f;
        if (vr_hud_tex_pass) {
          // Flat into the HUD texture: no VR placement here (the mesh does it).
        } else if (vr_hud_cam && ortho && !screen_fill) {
          // clip' = clip * M (row form): c'_j = sum_i M[i][j] * c_i.
          float rows[16] = {};
          for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
              const float m = vr_hud_m[i * 4 + j];
              for (int k = 0; k < 4; ++k) rows[j * 4 + k] += m * constants[i * 4 + k];
            }
          }
          std::memcpy(constants, rows, sizeof(rows));
          constants[36] = 0.0f;  // perspective now: no D3D9 half-pixel / edge snap
          constants[38] = 0.0f;
          constants[39] = 0.0f;
        } else if (vr_hud_on && ortho && !screen_fill) {
          for (int k = 0; k < 4; ++k) {
            const float w = constants[12 + k];
            constants[0 + k] = constants[0 + k] * vr_hud[0] + w * vr_hud[2];
            constants[4 + k] = constants[4 + k] * vr_hud[1] + w * vr_hud[3];
          }
          constants[38] = 0.0f;  // no band edge snap on the panel
          constants[39] = 0.0f;
        } else if (vr_hud_on && !ortho) {
          float rows[16];
          if (skate3::vr::RetargetClipRows(constants, rows)) {
            std::memcpy(constants, rows, sizeof(rows));
          }
        }
        if ((skate3::vr::Active() || wide_2d_scale != 1.0f || tall_2d_scale != 1.0f) &&
            ortho && !screen_fill) {
          // APT hides unused elements beyond its original 16:9 canvas
          // (parked there when unloaded); a wide / tall output, whose band
          // leaves room beside the canvas, would show them. Preserve that
          // canvas's homogeneous X/Y/W planes BEFORE band fitting or HUD
          // projection: an eye-space scissor cannot follow the
          // camera-facing panel when the player turns their head.
          // overlay2d's SV_ClipDistance clips crossing primitives too.
          std::memcpy(constants + 40, d.consts, 8 * sizeof(float));
          std::memcpy(constants + 48, d.consts + 12, 4 * sizeof(float));
          // Spread HUD (skate3_apt_spread): elements are moved out by up to
          // (k - 1) half-stages, so the canvas spans k times the stage width.
          const float spread_k = Skate3AptSpreadFactor();
          if (spread_k != 1.0f && !skate3::vr::Active()) {
            for (int k = 40; k < 44; ++k) constants[k] /= spread_k;
          }
        }
        cmd->SetRootConstants(0, 52, constants, 0);
        cmd->SetTexture(1, srv_view);
        // The game's blend state for this draw (video quads keep the FMV
        // combine pipeline below).
        // HUD texture pass: the game's colour blend with alpha accumulated as
        // coverage (ONE / INV_SRC_ALPHA), so the texture composites back as
        // premultiplied alpha; RGBA8 variants (vr_hud_ctx). Without a variant
        // (unsupported factors) the draw is skipped rather than bound to a
        // pipeline of the wrong target format.
        nrhi::Pipeline* const blend_pso =
            yuv != nullptr ? g_r.pso_2d
            : vr_hud_tex_pass
                ? Get2dBlendPso(vr_hud_ctx, (d.blend_control & 0xFFFFu) | 0x07010000u)
                : Get2dBlendPso(context, d.blend_control);
        if (vr_hud_tex_pass && blend_pso == g_r.pso_2d) {
          break;
        }
        if (yuv == nullptr && blend_pso != g_r.pso_2d) {
          cmd->SetPipeline(blend_pso);
        }
        if (yuv != nullptr) {
          cmd->SetPipeline(g_r.pso_yuv2d);
          cmd->SetTexture(2, yuv[1]);
          cmd->SetTexture(5, yuv[2]);
        }
        cmd->SetVertexBuffer(g_r.ui_ring, ui_region + ui_offset, bytes,
                             d.stride);
        cmd->SetPrimitiveTopology(d.prim == 5
                                      ? nrhi::PrimitiveTopology::kTriangleStrip
                                      : nrhi::PrimitiveTopology::kTriangleList);
        cmd->Draw(d.count, 0);
        if (yuv != nullptr || blend_pso != g_r.pso_2d) {
          cmd->SetPipeline(g_r.pso_2d);
        }
        ++drawn_2d;
        }  // vr_e: share allocation and texture decision
        ui_offset += bytes;
      };

      cmd->SetRenderTargets(context.guest_output, nullptr);
      cmd->SetViewport(viewport);
      cmd->SetScissor(scissor);
      // Native FMV substitution, self-contained: a video quad's console
      // shader binds Y at the DRAW's fetch slot 0 and the U/V planes at
      // slots 1/2: three valid distinct textures with the chroma at
      // exactly half the luma dimensions is a video draw, regardless of
      // which UI path drew it (the camera-page previews are plain APT
      // elements; matching against the VideoRenderer-published planes was
      // refuted twice; the UI paths sample APT-side plane COPIES, e.g.
      // 0x1F6xxxxx vs the published 0xA59xxxxx). Resolved
      // triples are cached per frame by the Y address, and their store
      // entries are forced to a per-frame liveness probe (a 30 fps video
      // rewriting its planes every few RENDERED frames would otherwise
      // settle the probe to 16-frame sampling, the "sluggish video").
      const auto yuv_triple = [](const Draw2d& d) -> bool {
        const uint32_t* s0 = d.fetch;
        const uint32_t* s1 = d.fetch + 6;
        const uint32_t* s2 = d.fetch + 12;
        if ((s0[0] & 3u) != 2 || (s1[0] & 3u) != 2 || (s2[0] & 3u) != 2 ||
            s0[1] == 0 || s1[1] == 0 || s2[1] == 0 || s1[1] == s0[1] ||
            s2[1] == s0[1] || s1[1] == s2[1]) {
          return false;
        }
        const uint32_t w0 = (s0[2] & 0x1FFFu) + 1, h0 = ((s0[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w1 = (s1[2] & 0x1FFFu) + 1, h1 = ((s1[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w2 = (s2[2] & 0x1FFFu) + 1, h2 = ((s2[2] >> 13) & 0x1FFFu) + 1;
        const auto half = [](uint32_t full, uint32_t c) {
          return c == full / 2 || c == (full + 1) / 2;
        };
        return w0 >= 32 && h0 >= 32 && half(w0, w1) && half(h0, h1) &&
               w2 == w1 && h2 == h1;
      };
      struct TripleCacheEntry {
        uint32_t y_addr;
        bool ok;
        nrhi::TextureView* slots[3];
      };
      TripleCacheEntry triple_cache[kMaxMovies];
      int triple_count = 0;
      // Backup: the freshest VideoRenderer-published plane set, for a
      // bracketed movie quad without a readable triple (the boot intro
      // rendered through this before the triple detection existed).
      // Resolved lazily; the capped log tracks whether it is still ever
      // needed; if it stays silent across sessions, this path and the
      // OnMovieFrame publish machinery behind it can be retired.
      nrhi::TextureView* fallback_slots[3] = {};
      int fallback_state = 0;  // 0 = unresolved, 1 = ok, -1 = unavailable
      const auto fallback_yuv = [&]() -> nrhi::TextureView* const* {
        if (fallback_state == 0) {
          fallback_state = -1;
          const MoviePlanes* best = nullptr;
          for (const MoviePlanes& m : movies) {
            if (m.ns >= 0 && movie_now_ns - m.ns < 500'000'000 &&
                (best == nullptr || m.ns > best->ns)) {
              best = &m;
            }
          }
          if (best != nullptr) {
            bool ok = true;
            for (int p = 0; p < 3 && ok; ++p) {
              auto hot = g_r.tex_store.find(FetchWordsKey(best->words[p]));
              if (hot != g_r.tex_store.end()) {
                hot->second.recheck_frame = 0;
              }
              const GuestTexture* t =
                  resolve_2d_texture(best->words[p], /*force_inline=*/true);
              ok = t != &g_r.white && t->texture != nullptr;
              fallback_slots[p] = t->srv;
            }
            if (ok) {
              fallback_state = 1;
            }
          }
        }
        return fallback_state == 1 ? fallback_slots : nullptr;
      };
      bool movie_drawn = false;
      // Curved HUD ordering: the texture pass draws a segment's HUD layer
      // before its eye-pass draws, so a screen fade (or video) followed by
      // HUD draws - the challenge map opening over its fade to black - must
      // start a new segment, or the fade covers the menu drawn after it.
      // One segment without fades; flat output: one pass over everything.
      std::vector<std::pair<size_t, size_t>> hud_segments;
      {
        size_t begin = 0;
        bool after_fill = false;
        for (size_t di = 0; vr_curved_hud && di < scene_2d.size(); ++di) {
          const Draw2d& d = scene_2d[di];
          const bool video =
              yuv_triple(d) || (movie_sub && (d.flags & 0x2u) != 0 && d.src_stride == 24);
          const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                             d.consts[14] == 0.0f && d.consts[15] == 1.0f;
          const bool fill = video || screen_fill_of(d, false);
          if (ortho && !fill && after_fill && hud_segments.size() < 7) {
            hud_segments.emplace_back(begin, di);
            begin = di;
            after_fill = false;
          }
          after_fill |= fill;
        }
        hud_segments.emplace_back(begin, scene_2d.size());
      }
      for (const auto& hud_seg : hud_segments) {
      for (int hud_pass = vr_curved_hud ? 0 : 1; hud_pass < 2; ++hud_pass) {
      vr_hud_tex_pass = hud_pass == 0;
      if (vr_hud_tex_pass) {
        e2d_wide = 1.0f;
        e2d_tall = 1.0f;
        cmd->Barrier(g_vr_hud_tex, nrhi::ResourceState::kPixelShaderResource,
                     nrhi::ResourceState::kRenderTarget);
        cmd->FlushBarriers();
        const float hud_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd->ClearRenderTarget(g_vr_hud_tex, hud_clear);
        cmd->SetRenderTargets(g_vr_hud_tex, nullptr);
        cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(kVrHudWidth), float(kVrHudHeight),
                                        0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{0, 0, int32_t(kVrHudWidth), int32_t(kVrHudHeight)});
      }
      for (size_t di = hud_seg.first; di < hud_seg.second; ++di) {
        const Draw2d& d = scene_2d[di];
        nrhi::TextureView* const* yuv = nullptr;
        // Video-quad detection runs regardless of movie_sub: a quad whose
        // own fetch slots form a YUV triple must NEVER draw through ps_main
        // - that renders the raw Y plane (greyscale luma, or the PREVIOUS
        // video's frame while the plane copies are still stale), the
        // video-boundary flash class. If the planes can't resolve yet
        // (async decode in flight, substitution off) the quad is SKIPPED;
        // black under a starting video is what the real thing looks like.
        bool video_quad = false;
        if (yuv_triple(d)) {
          video_quad = true;
          if (movie_sub) {
            TripleCacheEntry* e = nullptr;
            for (int t = 0; t < triple_count && e == nullptr; ++t) {
              if (triple_cache[t].y_addr == d.fetch[1]) {
                e = &triple_cache[t];
              }
            }
            if (e == nullptr && triple_count < kMaxMovies) {
              e = &triple_cache[triple_count++];
              e->y_addr = d.fetch[1];
              e->ok = true;
              bool decode_failed = false;
              for (int p = 0; p < 3 && e->ok; ++p) {
                const uint32_t* w = d.fetch + p * 6;
                // Plain find (find_words_texture would probe + enqueue its
                // own ui=false heal first): force the per-frame probe, the
                // resolve inline-decodes any content change (video=true).
                auto hot = g_r.tex_store.find(FetchWordsKey(w));
                if (hot != g_r.tex_store.end()) {
                  hot->second.recheck_frame = 0;  // content-hot: per-frame probe
                }
                const GuestTexture* t =
                    resolve_2d_texture(w, /*force_inline=*/false, /*video=*/true);
                // Session gate: only content decoded during THIS movie
                // session serves; at a video boundary both the store and
                // guest memory can still hold the PREVIOUS video's last
                // frame (the plane copies keep their addresses); the quad
                // holds black until the new video's first frame lands.
                e->ok = t != nullptr && t != &g_r.white && t->texture != nullptr &&
                        t->last_change_frame >= s_movie_session_frame;
                decode_failed |= t == &g_r.white;
                e->slots[p] = e->ok ? t->srv : nullptr;
              }
              if (decode_failed) {
                static std::atomic<uint32_t> s_triple_failed{0};
                if (s_triple_failed.fetch_add(1, std::memory_order_relaxed) < 8) {
                  REXLOG_INFO(
                      "native-scene: FMV plane triple resolve FAILED "
                      "(y={:08X})",
                      d.fetch[1]);
                }
              }
            }
            if (e != nullptr && e->ok) {
              yuv = e->slots;
            }
          }
        }
        if (yuv == nullptr && movie_sub && (d.flags & 0x2u) != 0 &&
            d.src_stride == 24) {
          // Bracketed movie quad without a readable triple: through ps_main
          // it draws its c8 opaque-black cover, also a video quad.
          video_quad = true;
          yuv = fallback_yuv();
          if (yuv != nullptr) {
            static std::atomic<uint32_t> s_fb_logged{0};
            if (s_fb_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO(
                  "native-scene: FMV bracket fallback served a quad (no "
                  "readable YUV triple on it)");
            }
          }
        }
        if (video_quad) {
          // Consulted by YieldForMovie: a video is only "on screen" while
          // the 2D replay actually carries a video quad. A skipped/dismissed
          // video leaves the decoder tail beating with no quad anywhere -
          // yielding then flashes the (suppressed, stale) emulated buffer
          // for nothing.
          g_movie_quad_last_ns.store(movie_now_ns, std::memory_order_relaxed);
        }
        if (video_quad && yuv == nullptr) {
          static std::atomic<uint32_t> s_vskip{0};
          const uint32_t n = s_vskip.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 511u) == 0) {
            REXLOG_INFO(
                "native-scene: video quad skipped (planes not ready, "
                "y={:08X}, movie_sub={}) (n={})",
                d.fetch[1], movie_sub ? 1 : 0, n);
          }
          continue;
        }
        emit_draw(d, yuv);
        movie_drawn |= yuv != nullptr;
      }
      if (vr_hud_tex_pass) {
        cmd->Barrier(g_vr_hud_tex, nrhi::ResourceState::kRenderTarget,
                     nrhi::ResourceState::kPixelShaderResource);
        cmd->FlushBarriers();
        e2d_wide = wide_2d_scale;
        e2d_tall = tall_2d_scale;
        cmd->SetRenderTargets(context.guest_output, nullptr);
        // The curved HUD into each eye, before the eye pass, so screen
        // fades drawn there still cover it like on a flat screen.
        nrhi::Pipeline* const hud_comp = Get2dBlendPso(context, 0x07010701u);  // premultiplied
        if (hud_comp != nullptr && hud_comp != g_r.pso_2d) {
          cmd->SetPipeline(hud_comp);
          float hc[52] = {};  // identity clip / world rows, white, no 2D extras
          hc[0] = hc[5] = hc[10] = hc[15] = 1.0f;
          hc[16] = hc[21] = hc[26] = hc[31] = 1.0f;
          hc[32] = hc[33] = hc[34] = hc[35] = 1.0f;
          cmd->SetRootConstants(0, 52, hc, 0);
          cmd->SetTexture(1, g_vr_hud_srv);
          cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
          for (int e = 0; e < 2; ++e) {
            const std::vector<float>& m = vr_curve_mesh[e];
            const uint32_t verts = uint32_t(m.size() / 6);
            const uint32_t mesh_bytes = verts * 28u;
            if (verts == 0) {
              continue;
            }
            if (vr_curve_mesh_offset[e] == UINT32_MAX) {
              if (ui_offset + mesh_bytes > RendererState::kUiRegionSize) {
                continue;
              }
              uint8_t* dst = g_r.ui_ring_cpu + ui_region + ui_offset;
              for (uint32_t v = 0; v < verts; ++v) {
                const uint32_t white = 0xFFFFFFFFu;
                std::memcpy(dst + v * 28u, &m[size_t(v) * 6], 24);
                std::memcpy(dst + v * 28u + 24u, &white, 4);
              }
              vr_curve_mesh_offset[e] = ui_offset;
              ui_offset += mesh_bytes;
            }
            vr_eye_viewport(cmd, e);
            cmd->SetVertexBuffer(g_r.ui_ring, ui_region + vr_curve_mesh_offset[e], mesh_bytes,
                                 28);
            cmd->Draw(verts, 0);
          }
          cmd->SetPipeline(g_r.pso_2d);
        }
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);
      }
      }  // hud_pass
      }  // hud_seg
      if (vr_stereo) {
        skate3::vr::SetCurrentEye(skate3::vr::kStereoEye);
        cmd->SetViewport(viewport);
        cmd->SetScissor(scissor);
      }
      if (movie_drawn) {
        g_movie_native_last_ns.store(movie_now_ns, std::memory_order_relaxed);
      }
      if (movie_drawn) {
        static std::atomic<bool> s_movie_logged{false};
        if (!s_movie_logged.exchange(true, std::memory_order_relaxed)) {
          REXLOG_INFO(
              "native-scene: FMV rendering NATIVELY (movie-quad YUV "
              "substitution)");
        }
      }
    }
  }

  // Settings-menu backdrop blur over everything drawn this frame (scene +
  // HUD/2D + FMV). Shared with the emulated-output post-processor path.
  // Host state read directly at RENDER time - routing it through the
  // published FrameScene made the blur depend on fresh guest publishes,
  // which the startup flow doesn't reliably produce (stale scenes rendered
  // with a frozen sigma of 0).
  const float menu_blur_target = g_settings_menu_blur.load(std::memory_order_relaxed)
                                     ? float(REXCVAR_GET(skate3_menu_blur_sigma))
                                     : 0.0f;
  ApplyMenuBlurPass(context, cmd, menu_blur_target,
                    /*output_in_guest_output_state=*/false);
  // The presenter's gamma ramp, last (see ApplyGuestGammaRamp).
  ApplyGuestGammaRamp(context, cmd);

  cmd->Barrier(context.guest_output, nrhi::ResourceState::kRenderTarget,
               nrhi::ResourceState::kGuestOutput);
  cmd->FlushBarriers();
  cmd->ProfileRegion(nrhi::ProfileStage::kTail);

  g_pw_2d.Add(perf_ns_since(twod_t0));
  g_pw_tail.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    twod_t0 - mid_t0)
                    .count()));
  g_pw_render.Add(perf_ns_since(render_t0));
  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  MaybeDumpSceneRing();
  LogFrameStats(scene, frames, drawn, drawn_2d, drawn_spline, shadow_ready,
                shadow_draws);
  return true;
}

// ---- PhotoEffects portrait pass: native resolve job (see PhotoJob) ----
// Command processor thread. The guest's resolve of its portrait pass into
// the screenshot target is claimed; the job's scene renders offscreen
// through RenderScene's photo mode; after the GPU drained, the decoded
// 1152x640 result is tiled into the target's memory, which
// ExtractTextureImageData then CPU-reads exactly as it reads the resolve.
PhotoJob g_photo_job_cur;

// ---- VR (skate3_vr.h) ----------------------------------------------------
// Virtual screen: a finished flat frame (`source`, in kGuestOutput state) on
// the world-locked panel in both own eye targets, which become this swap's
// eye images. Used for frames that are not eye renders: the emulated output
// (frontend, loading, editors) and native frames rendered before the output
// was resized to the eye.
nrhi::Buffer* g_vr_quad_ring = nullptr;
uint8_t* g_vr_quad_ring_cpu = nullptr;
uint32_t g_vr_quad_slot = 0;
nrhi::TextureView* g_vr_source_view = nullptr;

// Explicit compositor color transfer. The guest output contains display-
// encoded colors in R10G10B10A2_UNORM (which has no sRGB view). A Quest 2 /
// Steam Link A/B confirmed that direct R10/Gamma submission washes out the
// headset image, while these same colors in RGBA16F/Linear display correctly.
// Decode exactly once before submission; leave the desktop output intact.
struct VrLinearTarget {
  nrhi::Texture* texture = nullptr;
  nrhi::TextureView* source_view = nullptr;
};
VrLinearTarget g_vr_linear_targets[2];
nrhi::Pipeline* g_vr_linear_pso = nullptr;

// The settings menu in the headset (skate3::vr::MenuVisible): the window's
// presented image, painted UI-only (rex::ui::SetPresentedImageUiOnly: no
// game, transparent black, the UI's coverage in alpha) and copied by the
// presenter into g_vr_menu_tex (rex::ui::SetPresentedImageTarget), blended
// premultiplied on the menu panel of each eye (MenuClipRows), cropped to the
// menu's frame (MenuRect): only the menu's elements float over the world. Its size follows the window; a
// replaced texture is detached from the presenter first, so the deferred
// destroy (this queue's later submission) runs after its last copy.
nrhi::Texture* g_vr_menu_tex = nullptr;
nrhi::TextureView* g_vr_menu_srv = nullptr;
bool g_vr_menu_target_set = false;

void ReleaseVrMenuTexture(nrhi::Device* device) {
  if (g_vr_menu_target_set) {
    rex::ui::SetPresentedImageTarget(nullptr, 0, 0);
    rex::ui::SetPresentedImageUiOnly(false);
    g_vr_menu_target_set = false;
  }
  if (g_vr_menu_srv != nullptr) device->DestroyDeferred(g_vr_menu_srv);
  if (g_vr_menu_tex != nullptr) device->DestroyDeferred(g_vr_menu_tex);
  g_vr_menu_srv = nullptr;
  g_vr_menu_tex = nullptr;
}

void DrawVrMenuPanel(const NativeGuestOutputRenderContext& context, nrhi::Texture* left,
                     nrhi::Texture* right, bool side_by_side) {
  constexpr uint32_t kSlotBytes = 256;
  constexpr uint32_t kSlots = 32;
  nrhi::Device* device = context.device;
  nrhi::Cmd* cmd = context.cmd;
  if (!skate3::vr::MenuVisible() || context.backend != NativeGuestOutputBackend::kD3D12) {
    if (g_vr_menu_target_set) {
      rex::ui::SetPresentedImageTarget(nullptr, 0, 0);  // no copies while closed
      rex::ui::SetPresentedImageUiOnly(false);
      g_vr_menu_target_set = false;
    }
    return;
  }
  uint32_t w = 0, h = 0;
  if (left == nullptr || right == nullptr || !rex::ui::PresentedImageSize(w, h)) {
    return;
  }
  if (g_vr_menu_tex != nullptr &&
      (g_vr_menu_tex->width() != w || g_vr_menu_tex->height() != h)) {
    ReleaseVrMenuTexture(device);
  }
  if (g_vr_menu_tex == nullptr) {
    nrhi::TextureDesc td;
    td.width = w;
    td.height = h;
    td.format = nrhi::Format::kB8G8R8A8_UNORM;
    td.initial_state = nrhi::ResourceState::kPixelShaderResource;
    g_vr_menu_tex = device->CreateTexture(td);
    if (g_vr_menu_tex != nullptr) {
      nrhi::TextureViewDesc vd;
      vd.mip_levels = 1;
      g_vr_menu_srv = device->CreateTextureView(g_vr_menu_tex, vd);
    }
    if (g_vr_menu_srv == nullptr) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("vr: settings menu panel texture unavailable ({}x{})", w, h);
      }
      ReleaseVrMenuTexture(device);
      return;
    }
  }
  if (!g_vr_menu_target_set) {
    rex::ui::SetPresentedImageUiOnly(true);
    rex::ui::SetPresentedImageTarget(device->NativeTextureHandle(g_vr_menu_tex), w, h);
    g_vr_menu_target_set = true;
  }
  if (rex::ui::PresentedImageCopies() == 0) {
    return;  // nothing copied yet: keep the frame as is
  }
  NativeGuestOutputRenderContext eye_context = context;
  eye_context.guest_output = left;
  nrhi::Pipeline* const premul = Get2dBlendPso(eye_context, 0x07010701u);  // ONE / INV_SRC_ALPHA
  if (g_r.layout == nullptr || premul == nullptr || premul == g_r.pso_2d) {
    return;
  }
  // The menu's own frame of the window (its VR layout), else the whole image.
  float rx = 0.0f, ry = 0.0f, rw = float(w), rh = float(h);
  if (!skate3::vr::MenuRect(rx, ry, rw, rh) || rx < 0.0f || ry < 0.0f ||
      rx + rw > float(w) + 0.5f || ry + rh > float(h) + 0.5f) {
    rx = ry = 0.0f;
    rw = float(w);
    rh = float(h);
  }
  const float u0 = rx / float(w), u1 = (rx + rw) / float(w);
  const float v0 = ry / float(h), v1 = (ry + rh) / float(h);
  const float aspect = rw / rh;
  float rows[2][16];
  if (!skate3::vr::MenuClipRows(0, aspect, rows[0]) ||
      !skate3::vr::MenuClipRows(1, aspect, rows[1])) {
    return;
  }
  if (g_vr_quad_ring == nullptr) {
    g_vr_quad_ring = CreateUploadBuffer(device, size_t(kSlotBytes) * kSlots,
                                        nrhi::BufferBindClass::kVertexIndex);
    g_vr_quad_ring_cpu =
        g_vr_quad_ring ? static_cast<uint8_t*>(device->Map(g_vr_quad_ring)) : nullptr;
    if (g_vr_quad_ring_cpu == nullptr) {
      return;
    }
  }
  struct Vert {
    float pos[4];
    float uv[2];
    uint32_t color;
  };
  static_assert(sizeof(Vert) == 28);
  const Vert quad[4] = {{{-1.0f, 1.0f, 0.0f, 1.0f}, {u0, v0}, 0xFFFFFFFFu},
                        {{1.0f, 1.0f, 0.0f, 1.0f}, {u1, v0}, 0xFFFFFFFFu},
                        {{-1.0f, -1.0f, 0.0f, 1.0f}, {u0, v1}, 0xFFFFFFFFu},
                        {{1.0f, -1.0f, 0.0f, 1.0f}, {u1, v1}, 0xFFFFFFFFu}};
  const uint32_t offset = (g_vr_quad_slot++ % kSlots) * kSlotBytes;
  std::memcpy(g_vr_quad_ring_cpu + offset, quad, sizeof(quad));
  nrhi::Texture* const targets[2] = {left, right};
  const int target_count = side_by_side || left == right ? 1 : 2;
  for (int t = 0; t < target_count; ++t) {
    nrhi::Texture* target = targets[t];
    cmd->Barrier(target, nrhi::ResourceState::kGuestOutput, nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    cmd->SetRenderTargets(target, nullptr);
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetPipeline(premul);
    cmd->SetTexture(1, g_vr_menu_srv);
    cmd->SetVertexBuffer(g_vr_quad_ring, offset, sizeof(quad), sizeof(Vert));
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
    for (int e = 0; e < 2; ++e) {
      if (!side_by_side && e != t && target_count == 2) continue;
      if (side_by_side) {
        const uint32_t half = target->width() / 2;
        cmd->SetViewport(nrhi::Viewport{float(half * uint32_t(e)), 0.0f, float(half),
                                        float(target->height()), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{int32_t(half * uint32_t(e)), 0,
                                   int32_t(half * uint32_t(e + 1)), int32_t(target->height())});
      } else {
        cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(target->width()),
                                        float(target->height()), 0.0f, 1.0f});
        cmd->SetScissor(nrhi::Rect{0, 0, int32_t(target->width()), int32_t(target->height())});
      }
      // overlay2d.hlsl: m[0..3] clip rows, m[4..7] identity world, m[8] white.
      float constants[52] = {};
      std::memcpy(constants, rows[e], sizeof(rows[e]));
      constants[16] = constants[21] = constants[26] = constants[31] = 1.0f;
      constants[32] = constants[33] = constants[34] = constants[35] = 1.0f;
      cmd->SetRootConstants(0, 52, constants, 0);
      cmd->Draw(4, 0);
    }
    cmd->Barrier(target, nrhi::ResourceState::kRenderTarget, nrhi::ResourceState::kGuestOutput);
  }
  cmd->FlushBarriers();
}

void SubmitVrFrameTextures(const NativeGuestOutputRenderContext& context,
                          nrhi::Texture* left, nrhi::Texture* right,
                          bool side_by_side = false) {
  auto* device = context.device;
  auto* cmd = context.cmd;
  DrawVrMenuPanel(context, left, right, side_by_side);
  const auto legacy = [&]() {
    skate3::vr::SetFrameTextures(device, left, right, side_by_side);
  };
  if (!REXCVAR_GET(skate3_vr_linear_submit) || left == nullptr || right == nullptr ||
      context.backend != NativeGuestOutputBackend::kD3D12) {
    legacy();
    return;
  }
  const auto fail = [&]() {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("vr: linear handoff unavailable; keeping original Gamma submission");
    }
    legacy();
  };
  if (!EnsureRootSignature(context)) {
    fail();
    return;
  }
  if (g_vr_linear_pso == nullptr) {
    auto* vs = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kVertex, "blur.hlsl", kBlurShaderSource, "vs_main", nullptr, ""));
    auto* ps = device->CreateShader(MakeShaderDesc(
        nrhi::ShaderStage::kPixel, "blur.hlsl", kBlurShaderSource, "ps_vr_linear", nullptr, ""));
    if (vs && ps) {
      nrhi::GraphicsPipelineDesc gp;
      gp.layout = g_r.layout;
      gp.vs = vs;
      gp.ps = ps;
      gp.rtv_format = nrhi::Format::kR16G16B16A16_FLOAT;
      g_vr_linear_pso = device->CreateGraphicsPipeline(gp);
    }
    if (vs) device->DestroyDeferred(vs);
    if (ps) device->DestroyDeferred(ps);
    if (!g_vr_linear_pso) {
      fail();
      return;
    }
  }
  nrhi::Texture* sources[2] = {left, right};
  const int count = left == right ? 1 : 2;
  for (int e = 0; e < count; ++e) {
    auto& target = g_vr_linear_targets[e];
    auto* source = sources[e];
    if (target.texture && (target.texture->width() != source->width() ||
                           target.texture->height() != source->height())) {
      device->DestroyDeferred(target.texture);
      target.texture = nullptr;
    }
    if (!target.texture) {
      nrhi::TextureDesc td;
      td.width = source->width();
      td.height = source->height();
      td.format = nrhi::Format::kR16G16B16A16_FLOAT;
      td.usage = nrhi::kTextureUsageRenderTarget;
      td.initial_state = nrhi::ResourceState::kPixelShaderResource;
      target.texture = device->CreateTexture(td);
    }
    if (target.source_view) device->DestroyDeferred(target.source_view);
    nrhi::TextureViewDesc vd;
    vd.mip_levels = 1;
    target.source_view = device->CreateTextureView(source, vd);
    if (!target.texture || !target.source_view) {
      fail();
      return;
    }
    cmd->Barrier(target.texture, nrhi::ResourceState::kPixelShaderResource,
                 nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    cmd->SetRenderTargets(target.texture, nullptr);
    cmd->SetViewport(nrhi::Viewport{0, 0, float(source->width()), float(source->height()), 0, 1});
    cmd->SetScissor(nrhi::Rect{0, 0, int32_t(source->width()), int32_t(source->height())});
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetPipeline(g_vr_linear_pso);
    cmd->SetTexture(1, target.source_view);
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleList);
    cmd->Draw(3, 0);
    cmd->Barrier(target.texture, nrhi::ResourceState::kRenderTarget,
                 nrhi::ResourceState::kPixelShaderResource);
    cmd->FlushBarriers();
  }
  skate3::vr::SetFrameTextures(device, g_vr_linear_targets[0].texture,
      g_vr_linear_targets[count - 1].texture, side_by_side, /*linear=*/true);
}

bool RenderVrScreen(const NativeGuestOutputRenderContext& context, nrhi::Texture* source) {
  constexpr uint32_t kSlotBytes = 256;
  constexpr uint32_t kSlots = 32;
  nrhi::Device* device = context.device;
  nrhi::Cmd* cmd = context.cmd;
  if (g_r.failed || source == nullptr || source->height() == 0) {
    return false;
  }
  if (g_r.layout == nullptr && !EnsureRootSignature(context)) {
    return false;
  }
  if (g_r.pso_2d == nullptr && !Ensure2dPso(context)) {
    return false;
  }
  if (g_vr_quad_ring == nullptr) {
    g_vr_quad_ring = CreateUploadBuffer(device, size_t(kSlotBytes) * kSlots,
                                        nrhi::BufferBindClass::kVertexIndex);
    g_vr_quad_ring_cpu =
        g_vr_quad_ring ? static_cast<uint8_t*>(device->Map(g_vr_quad_ring)) : nullptr;
    if (g_vr_quad_ring_cpu == nullptr) {
      return false;
    }
  }
  nrhi::Texture* eyes[2] = {skate3::vr::EyeTexture(device, 0, source->format()),
                            skate3::vr::EyeTexture(device, 1, source->format())};
  const float aspect = float(source->width()) / float(source->height());
  float rows[2][16];
  if (eyes[0] == nullptr || eyes[1] == nullptr ||
      !skate3::vr::ScreenClipRows(0, aspect, rows[0]) ||
      !skate3::vr::ScreenClipRows(1, aspect, rows[1])) {
    return false;
  }
  if (g_vr_source_view != nullptr) {
    device->DestroyDeferred(g_vr_source_view);
  }
  nrhi::TextureViewDesc vd;
  vd.mip_levels = 1;
  g_vr_source_view = device->CreateTextureView(source, vd);
  if (g_vr_source_view == nullptr) {
    return false;
  }
  // Panel-local strip: (u, v) in [-1, 1], v = +1 at the top of the image.
  struct Vert {
    float pos[4];
    float uv[2];
    uint32_t color;
  };
  static_assert(sizeof(Vert) == 28);
  const Vert quad[4] = {{{-1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, 0xFFFFFFFFu},
                        {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, 0xFFFFFFFFu},
                        {{-1.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, 0xFFFFFFFFu},
                        {{1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, 0xFFFFFFFFu}};
  const uint32_t offset = (g_vr_quad_slot++ % kSlots) * kSlotBytes;
  std::memcpy(g_vr_quad_ring_cpu + offset, quad, sizeof(quad));
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  for (int e = 0; e < 2; ++e) {
    nrhi::Texture* target = eyes[e];
    cmd->Barrier(target, nrhi::ResourceState::kGuestOutput, nrhi::ResourceState::kRenderTarget);
    cmd->FlushBarriers();
    cmd->ClearRenderTarget(target, black);
    cmd->SetRenderTargets(target, nullptr);
    cmd->SetViewport(nrhi::Viewport{0.0f, 0.0f, float(target->width()),
                                    float(target->height()), 0.0f, 1.0f});
    cmd->SetScissor(nrhi::Rect{0, 0, int32_t(target->width()), int32_t(target->height())});
    cmd->SetBindingLayout(g_r.layout);
    cmd->SetPipeline(g_r.pso_2d);
    // overlay2d.hlsl: m[0..3] clip rows, m[4..7] identity world, m[8] white,
    // m[9] = 0 (no half-pixel shift / sharpening / band snap).
    float constants[52] = {};
    std::memcpy(constants, rows[e], sizeof(rows[e]));
    constants[16] = constants[21] = constants[26] = constants[31] = 1.0f;
    constants[32] = constants[33] = constants[34] = constants[35] = 1.0f;
    cmd->SetRootConstants(0, 52, constants, 0);
    cmd->SetTexture(1, g_vr_source_view);
    cmd->SetVertexBuffer(g_vr_quad_ring, offset, sizeof(quad), sizeof(Vert));
    cmd->SetPrimitiveTopology(nrhi::PrimitiveTopology::kTriangleStrip);
    cmd->Draw(4, 0);
    cmd->Barrier(target, nrhi::ResourceState::kRenderTarget, nrhi::ResourceState::kGuestOutput);
  }
  cmd->FlushBarriers();
  SubmitVrFrameTextures(context, eyes[0], eyes[1]);
  return true;
}

// The registered renderer. Flat: RenderScene. VR: the output is sized to
// the headset eye (the app's output size provider); the left eye renders
// straight into the guest output (the window keeps mirroring it and every
// once-per-frame side effect runs there), the right eye into an own target.
bool RenderSceneVr(const NativeGuestOutputRenderContext& context, void* user_data) {
  if (rex::graphics::IsNativeInterFrame()) {
    // Flat only (InterFrameWant never asks while VR is active); no VR frame
    // pacing for a frame the headset never sees.
    return !skate3::vr::Active() && RenderScene(context, user_data);
  }
  if (!skate3::vr::BeginFrame(context)) {
    return RenderScene(context, user_data);
  }
  // A yielded frame reaches the emulated path's post-processor, which puts
  // it on the virtual screen.
  rex::graphics::RequestNativeGuestOutputPostProcess(true);
  uint32_t out_w = 0, out_h = 0;
  skate3::vr::OutputSize(out_w, out_h);
  if (context.guest_output_width != out_w || context.guest_output_height != out_h) {
    // Not an eye-sized output yet (the SDK applies the size provider only
    // once native frames are being served): show this flat frame.
    const bool rendered = RenderScene(context, user_data);
    if (rendered) {
      RenderVrScreen(context, context.guest_output);
    }
    return rendered;
  }
  g_vr_eye_scene.reset();
  g_vr_eye_2d.clear();
  g_vr_eye_splines.clear();
  g_vr_overlay_textures.clear();
  if (skate3::vr::SinglePass()) {
    // One pass, both eyes, side by side in the guest output (the window
    // mirrors both); submitted as one texture with per-eye UV bounds.
    skate3::vr::SetCurrentEye(skate3::vr::kStereoEye);
    const bool rendered = RenderScene(context, user_data);
    skate3::vr::SetCurrentEye(-1);
    if (rendered) {
      SubmitVrFrameTextures(context, context.guest_output, context.guest_output,
                                   /*side_by_side=*/true);
    }
    return rendered;
  }
  skate3::vr::SetCurrentEye(0);
  if (!RenderScene(context, user_data)) {
    skate3::vr::SetCurrentEye(-1);
    g_vr_eye_scene.reset();
    return false;
  }
  // Hand the left eye to the GPU now: otherwise the whole frame is recorded
  // (both eyes + the deferred-list replay, ~10 ms) before the GPU starts,
  // and CPU + GPU in series miss the headset's frame budget. RenderScene
  // sets its binding layout / pipelines / targets itself, as at a frame
  // start, so the right eye records fine on the fresh command list.
  if (skate3::vr::FlushEyes()) {
    context.device->FlushSubmission();
  }
  nrhi::Texture* right =
      skate3::vr::EyeTexture(context.device, 1, context.guest_output->format());
  bool right_ok = false;
  if (right != nullptr) {
    NativeGuestOutputRenderContext eye_context = context;
    eye_context.guest_output = right;
    skate3::vr::SetCurrentEye(1);
    right_ok = RenderScene(eye_context, user_data);
  }
  skate3::vr::SetCurrentEye(-1);
  g_vr_eye_scene.reset();
  SubmitVrFrameTextures(context, context.guest_output,
                               right_ok ? right : context.guest_output);
  return true;
}

void PostProcessGuestOutputVr(const NativeGuestOutputRenderContext& context, void* user_data) {
  PostProcessGuestOutput(context, user_data);
  if (skate3::vr::BeginFrame(context)) {
    RenderVrScreen(context, context.guest_output);
    rex::graphics::RequestNativeGuestOutputPostProcess(true);
  }
}

bool EnsurePhotoResources(nrhi::Device* device) {
  const uint32_t w = g_r.last_out_w;
  const uint32_t h = g_r.last_out_h;
  if (g_r.photo_color != nullptr &&
      (g_r.photo_color_w != w || g_r.photo_color_h != h ||
       g_r.photo_color_fmt != g_r.last_out_fmt)) {
    device->DestroyDeferred(g_r.photo_color);
    g_r.photo_color = nullptr;
  }
  if (g_r.photo_out != nullptr && g_r.photo_out->format() != g_r.last_out_fmt) {
    device->DestroyDeferred(g_r.photo_out);
    g_r.photo_out = nullptr;
  }
  if (g_r.photo_color == nullptr) {
    // Held in kGuestOutput (= PIXEL_SHADER_RESOURCE during a resolve job)
    // like the real output RenderScene stands it in for.
    nrhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = g_r.last_out_fmt;
    desc.usage = nrhi::kTextureUsageRenderTarget;
    desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
    g_r.photo_color = device->CreateTexture(desc);
    g_r.photo_color_w = w;
    g_r.photo_color_h = h;
    g_r.photo_color_fmt = g_r.last_out_fmt;
  }
  if (g_r.photo_out == nullptr) {
    nrhi::TextureDesc desc;
    desc.width = RendererState::kBlurWidth;
    desc.height = RendererState::kBlurHeight;
    desc.format = g_r.last_out_fmt;
    desc.usage = nrhi::kTextureUsageRenderTarget | nrhi::kTextureUsageCopySource;
    desc.initial_state = nrhi::ResourceState::kRenderTarget;
    g_r.photo_out = device->CreateTexture(desc);
  }
  if (g_r.photo_readback == nullptr) {
    nrhi::BufferDesc desc;
    desc.size = uint64_t(RendererState::kGrabRowPitch) * RendererState::kBlurHeight;
    desc.heap = nrhi::HeapKind::kReadback;
    g_r.photo_readback = device->CreateBuffer(desc);
    g_r.photo_readback_ptr =
        g_r.photo_readback != nullptr ? static_cast<uint8_t*>(device->Map(g_r.photo_readback))
                                      : nullptr;
  }
  return g_r.photo_color != nullptr && g_r.photo_out != nullptr &&
         g_r.photo_readback_ptr != nullptr && g_r.pso_photo_out != nullptr;
}

bool PhotoClaim(uint32_t dest_address, void* /*user_data*/) {
  if (!SceneEnabled()) {
    return false;
  }
  PhotoJob job;
  if (!TakePhotoJob(dest_address, &job)) {
    return false;
  }
  g_photo_job_cur = std::move(job);
  return true;
}

bool PhotoRender(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  if (g_r.failed || g_r.layout == nullptr || g_r.last_out_w == 0 || !g_photo_job_cur.scene ||
      g_r.last_out_w < 16 || g_r.last_out_h < 16) {
    REXLOG_WARN("native-scene: photo pass job NOT rendered (renderer not ready)");
    return false;
  }
  if (!EnsurePhotoResources(context.device)) {
    REXLOG_WARN("native-scene: photo pass job NOT rendered (resource creation failed)");
    return false;
  }
  NativeGuestOutputRenderContext photo = context;
  photo.guest_output = g_r.photo_color;
  photo.guest_output_width = g_r.last_out_w;
  photo.guest_output_height = g_r.last_out_h;
  photo.display_width = g_r.last_display_w;
  photo.display_height = g_r.last_display_h;
  g_photo_mode = true;
  g_photo_scene = g_photo_job_cur.scene;
  g_photo_layers = g_photo_job_cur.layers;
  const bool ok = RenderScene(photo, nullptr);
  g_photo_mode = false;
  g_photo_scene.reset();
  g_photo_layers.reset();
  if (!ok) {
    REXLOG_WARN("native-scene: photo pass job NOT rendered (scene pass declined)");
  }
  return ok;
}

void PhotoComplete(bool rendered, void* /*user_data*/) {
  PhotoJob job = std::move(g_photo_job_cur);
  g_photo_job_cur = PhotoJob{};
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (!rendered || base == nullptr) {
    return;
  }
  g_r.device->InvalidateForRead(g_r.photo_readback, 0,
                                uint64_t(RendererState::kGrabRowPitch) *
                                    RendererState::kBlurHeight);
  uint32_t w[3] = {};
  if (!GuestTryLoadU32(base, job.dest_tex + 0x1C, &w[0]) ||
      !GuestTryLoadU32(base, job.dest_tex + 0x20, &w[1]) ||
      !GuestTryLoadU32(base, job.dest_tex + 0x24, &w[2])) {
    REXLOG_WARN("native-scene: photo pass job - target {:08X} unreadable", job.dest_tex);
    return;
  }
  GrabTarget t;
  t.addr = w[1] & 0x1FFFF000u;
  t.width = (w[2] & 0x1FFFu) + 1;
  t.height = ((w[2] >> 13) & 0x1FFFu) + 1;
  t.tiled = (w[0] >> 31) != 0;
  t.pitch = ((w[0] >> 22) & 0x1FFu) * 32u;
  if (t.pitch < t.width) {
    t.pitch = (t.width + 31u) & ~31u;
  }
  if ((w[1] & 0x3Fu) != 6u || t.width > RendererState::kBlurWidth ||
      t.height > RendererState::kBlurHeight || t.width < 16 || t.height < 16) {
    REXLOG_WARN("native-scene: photo pass job - unexpected target {}x{} fmt {}", t.width,
                t.height, w[1] & 0x3Fu);
    return;
  }
  const bool wrote = WriteGrabToGuest(base, g_r.photo_readback_ptr, t);
  REXLOG_INFO("native-scene: photo pass rendered natively -> {:08X} ({}x{} {}){}", t.addr,
              t.width, t.height, t.tiled ? "tiled" : "linear", wrote ? "" : " WRITE FAILED");
}

}  // namespace

bool SceneFailed() { return g_r.failed; }

void ResetSceneFailure() {
  if (g_r.failed) {
    g_r.failed = false;
    REXLOG_INFO(
        "native-scene: sticky pipeline failure cleared; the next frame "
        "retries the full pipeline build");
  }
}

void Install() {
  // Registered even when the scene cvar starts off: RenderScene yields to the
  // emulated output while disabled, and the runtime toggle (F5) can flip the
  // cvar live at any point after boot.
  rex::graphics::SetNativeGuestOutputRenderer(&RenderSceneVr, nullptr);
  // Settings-menu backdrop blur over frames the EMULATED path produced
  // (boot/startup before the native scene takes over, manual emulated
  // mode); native-rendered frames blur inline in RenderScene.
  rex::graphics::SetNativeGuestOutputPostProcessor(&PostProcessGuestOutputVr, nullptr);
  rex::graphics::NativeResolveJobHooks photo_hooks;
  photo_hooks.claim = &PhotoClaim;
  photo_hooks.render = &PhotoRender;
  photo_hooks.complete = &PhotoComplete;
  rex::graphics::SetNativeResolveJobHooks(photo_hooks);
  rex::graphics::NativeInterFrameHooks inter_frame_hooks;
  inter_frame_hooks.want = &InterFrameWant;
  inter_frame_hooks.allow_async = &AllowAsyncNativeRender;
  inter_frame_hooks.seconds_until_want = &InterFrameSecondsUntilWant;
  inter_frame_hooks.present_at = &InterFramePresentAt;
  rex::graphics::SetNativeInterFrameHooks(inter_frame_hooks);
  REXLOG_INFO("native-scene: guest output renderer registered (scene {})",
              SceneEnabled() ? "on" : "off");
}

}  // namespace skate3::native_scene

#else  // !(REX_HAS_D3D12 || REX_HAS_VULKAN)

namespace skate3::native_scene {
void Install() {}
bool SceneFailed() { return false; }
void ResetSceneFailure() {}
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12 || REX_HAS_VULKAN

