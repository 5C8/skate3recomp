#pragma once

// Shared state of the native scene renderer, used by both of its translation
// units: skate3_native_scene.cpp (game-thread capture + frame build) and
// skate3_native_scene_gpu.cpp (render-thread RHI drawing). Everything here is
// written on one thread and read on another as documented per member; inline
// variables keep a single instance across the TUs.

#include <set>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_native_scene.h"

#include "native/skate3_flat_map.h"

REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ring);

namespace skate3::native_scene {

inline std::mutex g_scene_mutex;
// Published scene: immutable once published (BuildFrameScene builds a fresh
// one per guest frame). shared_ptr so the render thread grabs a reference
// under the mutex instead of deep-copying ~800 items (with per-item heap
// vectors) every frame.
inline std::shared_ptr<const FrameScene> g_scene;
inline uint64_t g_generation = 0;
// Scenes replaced by a publish, released on the render thread (RenderScene)
// instead of the guest render thread: tearing down ~800 items with their
// per-item vectors was ~2-3% of the guest render thread, which bounds the
// frame rate, while the render thread has slack. Own mutex (g_scene_mutex is
// taken per draw-side lookup). Capped: with no render thread draining it
// (native renderer off/idle) the publisher frees inline as before.
inline std::mutex g_scene_retire_mutex;
inline std::vector<std::shared_ptr<const FrameScene>> g_scene_retire;
// Steady-clock stamp of the last scene publish (BuildFrameScene only
// publishes when a perspective view submitted this frame). YieldForMenus
// uses its freshness to tell the in-game pause menu (the world keeps
// resubmitting behind the menu) apart from loading screens and the boot
// frontend (publishes stop), for skate3_native_render_scene_pause_native.
inline std::atomic<int64_t> g_last_publish_ns{-1};
// Debug-dialog cache flushes: consumed at the top of RenderScene so texture/
// mesh-affecting toggles (mip chains, 565 fixes, ...) take effect immediately
// instead of only for newly streamed content.
inline std::atomic<bool> g_flush_textures{false};
inline std::atomic<bool> g_flush_meshes{false};
// Off-screen retention clear request (see g_retained_items further down):
// set on the menus/loading flip (render thread) and the F5 re-enable,
// consumed at the top of BuildFrameScene's retention block (guest thread).
inline std::atomic<bool> g_retained_clear{false};

// Camera/bone signal recorders + synthetic-pan probe + offline recording:
// state and file writers live in native/skate3_native_diagnostics.cpp
// (native/skate3_native_diag.h); capture call sites below read per-frame
// locals and stay here.
inline std::atomic<uint8_t*> g_guest_base{nullptr};
inline std::atomic<uint64_t> g_frames_rendered{0};

// Loading -> gameplay takeover. Armed while the game reports menus/loading
// (and by the F5 enable toggle, hence the atomic); the loading-screen
// prewarm (below) does the decode heavy lifting behind the load, and the
// FIRST substantial post-load scene renders natively immediately, no
// emulated-gameplay stretch. The frames right after takeover run a budgeted
// "settle" decode pass for whatever prewarm missed (dynamic entities,
// late-registered textures); the draw path's miss budgets are clamped while
// settling so leftovers render white/skip for a frame instead of freezing
// the takeover frame. Everything except the armed flag is touched on the
// render thread only, inside RenderScene.
inline std::atomic<bool> g_warmup_armed{true};
// Sk8::Flow::StateLoading, mirrored from the game's own state machine: true
// between StateLoading::OnEnterState and StateLoading::OnLeaveState. This is
// the game's authoritative "the world is not ready to show" signal -- the
// same transition its logs bracket with "GAME LOAD END" -- and it is what
// the takeover gate waits on. Earlier versions guessed at readiness from the
// captured item count, which cannot distinguish a world still filling in
// from one that is simply small, so sparse views and little test maps were
// held on black indefinitely. Size never enters into it now.
inline std::atomic<bool> g_guest_loading{false};
// BuildFrameScene stops publishing during loading (no submissions / no
// perspective view), so g_scene holds the PREVIOUS map's scene through the
// whole loading screen. Only scenes published at or after this generation
// (stamped while the loading screen shows) are eligible for takeover;
// rendering the stale scene shows old-map garbage at the flip.
inline uint64_t g_warmup_fresh_generation = 0;
// Settle window: frames (of the native frame counter) still running the
// per-frame settle decode pass after a takeover.
inline uint64_t g_settle_until_frame = 0;

// Loading-screen prewarm queue: world meshes pushed by the
// tROptiMeshData::Unfix hook (the rw-arena LOAD-time pointer resolve; it
// fires once per world mesh as its arena streams in, i.e. exactly during
// loading screens and gameplay world streaming). The render thread drains
// the queue in RenderScene: with the prewarm budget behind the loading
// screen (where the heavy lifting belongs), with the warmup budget's
// leftover while warming, and with a small fixed slice during gameplay
// (streamed-in areas decode before their first draw instead of hitching
// it). Entries whose buffer objects are not initialized yet retry a bounded
// number of drains.
struct PrewarmEntry {
  uint32_t mesh;
  uint16_t retries;
  // Environment-cube entry (mesh == 0, tex != 0): the one object-keyed
  // texture path left (see EnqueueCubeMiss).
  uint32_t tex = 0;
  // Fetch-words entry (mesh == 0, tex == 0, wkey != 0): a content-store
  // miss decoded from the captured stable words snapshot (see
  // EnqueueWordsMiss): every 2D/3D texture miss and heal.
  uint64_t wkey = 0;
  uint32_t words[6] = {};
  // Environment-cube entry (tex != 0 && cube): a cube-cache miss: a single
  // cube decode measured up to ~100 ms, the largest remaining traversal
  // hitch when a vehicle / reflective area streamed in.
  bool cube = false;
  // Draw-path miss (vs speculative prewarm registration): the result is
  // visible RIGHT NOW, so the commit takes it this frame regardless of the
  // gameplay per-frame commit cap.
  bool miss = false;
  // 2D/HUD overlay miss (large-art async routing, see
  // skate3_native_render_scene_2d_async_px): the commit skips the
  // payload-stability verify for these; APT re-rasterizes animating UI
  // art every guest frame, so "payload moved between decode and commit" is
  // the NORMAL state mid-animation and the verify would reject every
  // commit, freezing the element (spinners, ramping fades). The per-frame
  // content probe in the 2D resolve is the heal path for torn reads, the
  // same exposure the old inline decode had.
  bool ui = false;
};
inline std::mutex g_prewarm_mutex;
inline std::condition_variable g_prewarm_cv;  // wakes the decode workers
inline std::vector<PrewarmEntry> g_prewarm_queue;
// Draw-path misses (EnqueueMeshMiss/EnqueueWordsMiss/EnqueueCubeMiss):
// content that is visible RIGHT NOW (white / skipped),
// served FIFO with strict priority over the speculative registration
// backlog in g_prewarm_queue. Guarded by g_prewarm_mutex.
inline std::vector<PrewarmEntry> g_miss_queue;
// Steady-state draw-path misses currently being decoded on the workers (one
// in-flight decode per key; the commit erases). Guarded by g_prewarm_mutex.
// This is the panning-hitch fix: a texture decode averages ~10 ms (max ~70 ms
// measured) and a pan surfaces dozens of new textures/meshes in one frame;
// decoding them inline on the render thread was the lag spike. Misses now
// render white / keep the previous decode for the 1-3 frames the workers
// need.
inline std::unordered_set<uint32_t> g_miss_inflight_mesh;
inline std::unordered_set<uint32_t> g_miss_inflight_tex;
inline std::unordered_set<uint64_t> g_miss_inflight_words;
// Dynamic-payload decode jobs: cloth-sim garments (ropa) have their vertex
// buffers rewritten by the CPU sim EVERY frame, so their mesh decode used to
// run inline on the render thread every frame (~0.7 ms per garment; a
// 4-garment outfit = ~2.9 ms = the 160 fps cap during real play once
// everything else was fixed; the demo character wears no ropa, which hid
// it). The guest thread snapshots the changed payload at publish time; a
// worker converts it into fresh upload buffers; PrewarmCommit swaps the mesh
// cache entry. The render thread keeps drawing the previous decode, one
// frame of cloth lag, invisible (the ropa sim is frame-paced anyway).
struct DynDecodeJob {
  DrawItem item;            // offsets/formats/fingerprint (bones not needed)
  uint64_t seq = 0;         // enqueue order; the commit drops stale results
  std::vector<uint8_t> vb;  // big-endian guest payload snapshots
  std::vector<uint8_t> ib;
};
inline std::vector<DynDecodeJob> g_dyn_jobs;  // FIFO; guarded by g_prewarm_mutex
// Push-side dedupe: clone instances share one tRModelData, so the same mesh
// registers many times per load, but a permanent per-address set silently
// DISABLED the prewarm for re-streamed content (the streamer reuses the
// same mesh/texture object addresses all session): after a few minutes
// every re-registered mesh was "seen", its re-decode was skipped, and every
// streamed texture surfaced as a draw-path first-sight miss instead:
// 388 white-flash textures in one 45 s traversal = the
// residual medium-distance pop-in. Now a mesh -> last-queued-frame map:
// clone bursts still dedupe inside the window, re-streams past it re-queue
// (registration is the game's own "content changed here" signal, see
// InvalidateCachedItem above). Cleared when the game enters menus/loading.
inline std::unordered_map<uint32_t, uint64_t> g_prewarm_seen;
// Texture-object dedupe for the workers (they cannot read the render
// thread's g_r caches): address -> fetch-words key at last staging; a
// words change at a reused address re-stages, an unchanged one skips.
inline std::unordered_map<uint32_t, uint64_t> g_prewarm_tex_seen;
inline std::atomic<uint64_t> g_prewarm_done{0};     // entries fully warm-decoded
inline std::atomic<uint64_t> g_prewarm_dropped{0};  // entries given up on
inline std::atomic<bool> g_prewarm_workers_started{false};
// tInstance::m_pRModel offset, confirmed at runtime by the material
// cross-check probe in OnAddRenderInstance (Skate 2 layout: +0x78).
inline std::atomic<uint32_t> g_instance_rmodel_offset{0};

// guid -> guest renderengine::Texture, from the RegisterTexture hook. Keys
// masked of the top bit: material channel guids carry an extra flag bit
// relative to the registered cAssetIDs.
constexpr uint64_t kGuidMask = 0x7FFFFFFFFFFFFFFFull;
inline std::mutex g_texture_map_mutex;
inline std::unordered_map<uint64_t, uint32_t> g_texture_map;

// VS constant staging bank (bank id 0x4000, device+1920).
// D3D::SetPending_AluConstants(device, dirty group mask, bank, ptr) is
// called from inside the guest Draw* functions; ptr is the device's
// positional 256-register shadow (partial uploads leave the rest intact).
// Two verified layouts: pre-pass keeps the per-draw transform / bone
// palette at c4; main-pass keeps camera at c4, palette at c7, rigid world
// at c8..c11 (see BankPaletteBase / BankRigidWorld). Because the dynamic
// capture runs right after RenderMesh returns, the bank holds exactly that
// mesh's constants when its draws ran inline.
inline std::atomic<uint32_t> g_vs_bank{0};
// Pixel constant shadow bank (bank id 0x4400, device+6016): material colors
// (e.g. the CAS hair tint) are staged here per draw.
inline std::atomic<uint32_t> g_ps_bank{0};
// Global per-frame fog rows (see FrameScene::fog_ramp): main-pass VS banks
// keep the camera at c4 and the frame's fog params at c5 (ramp) / c6 (color)
// - identical across every main-pass draw of a frame (verified from a
// recorded draw stream). OnDrawDone grabs them from the first 3D draw whose
// c4 matches the last built scene's camera; BuildFrameScene publishes them
// and re-arms the capture. All on the guest render thread.
inline float g_fog_cam[3] = {};
inline float g_fog_rows[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
inline bool g_fog_have = false;
inline bool g_fog_frame_done = false;
// Sky-dome viewpos height (see FrameScene::sky_height): sky.fx's VS adds
// g_vViewPos to every dome vertex, but the game feeds the SKY draw a viewpos
// whose Y is a fixed level elevation (165.0 in every Port Carverton capture)
// instead of the camera's; the baked skyline must not bob with the skater.
// Captured from the draws bound to sky_defaultVS (debug path "\\sky_"),
// whose ucode is world = model + c4.xyz (add r2.xyz, r0.yzw, c4.xyz). Every
// sky draw of the frame is a candidate; the HIGHEST c4.y wins, because a
// water-reflection pass mirrors the dome below the water plane. The old
// "c4.y 50..2000 above the camera" signature stopped capturing once the
// camera rose within 50 of the sky elevation and then served a stale height.
inline float g_sky_height = 165.0f;  // every Port Carverton capture to date
inline bool g_sky_have = false;
inline bool g_sky_frame_done = false;  // sun rows only
inline float g_sky_frame_y = 0.0f;     // best candidate this frame
inline bool g_sky_frame_any = false;
// The game's own render-target clear (D3DDevice_ClearF) this frame: the
// color of the LARGEST cleared target (the main scene target; shadow maps
// and small RTT passes are smaller). Scenes without a sky (trick guide, CAS,
// replay editor) show exactly this behind the character.
inline float g_clear_frame_rgb[3] = {};
inline uint64_t g_clear_frame_area = 0;
// Sky sun rows from the same draw's PIXEL bank (see FrameScene::sky_sun):
// light dir (c0.xyz), sun angular scale (c4.x), pre-tone multiplier (c4.y),
// exposure (c3.x). Persist like g_sky_height once captured.
inline float g_sky_sun[6] = {};
inline bool g_sky_sun_have = false;
// The CAPTURED (un-overridden) sun direction, published each frame for the
// debug dialog: enabling the sun override seeds its azimuth/elevation from
// this so the sun starts at its true position instead of jumping.
inline std::atomic<float> g_sun_captured[3] = {0.0f, 1.0f, 0.0f};
// UI background blur (see FrameScene::ui_blur and kBlurShaderSource): while
// a frontend popup is up the game appends blur_hBlur/vBlur + basictex passes
// after the postfx uber. g_ui_blur holds the captured kernel scale (PS c0.x,
// 8 in every capture); g_ui_blur_seen latches per frame on the blur_hBlurPS
// draw and is cleared at publish; the pass chain only exists while the
// popup is actually up, so this can never stick on.
inline float g_ui_blur = 8.0f;
inline bool g_ui_blur_seen = false;
// ... and this frame's trigger was blur_hBlurMaskedPS (FrameScene::ui_blur_masked).
inline bool g_ui_blur_masked_seen = false;
// Foreground draws this frame (see DrawItem::foreground): key (ib_obj << 32 |
// vb_obj) -> the draw's world matrix from its own VS bank (g_palette_mutex).
inline std::unordered_map<uint64_t, std::array<float, 16>> g_frame_fg_world;
// ... and the draws it issued per key (prim, base_vertex, start_index,
// index_count): the preview's own island set, not a placed copy's culled one.
inline std::unordered_map<uint64_t, std::vector<DrawEntry>> g_frame_fg_entries;
// ... and, for the environmentpark.diffuse variant, its albedo fetch words
// (tf3): runtime-provided art (a sign's logo) is bound there in place of the
// material's own diffuse; adopted through DrawItem::diffuse_fetch.
inline std::unordered_map<uint64_t, std::array<uint32_t, 6>> g_frame_fg_fetch;
// Every park / prop _defaultPS main-pass draw this frame, per key: the
// instance's translation (its VS world rows) and, for the
// environmentpark.diffuse variant, its albedo fetch words (tf3). Instances
// share buffers, so the translation is what tells them apart: (a) which
// copies the game really drew this frame (a preview's leftover world copy
// is not), (b) per-instance runtime art (each placed sign its own logo).
struct DefaultInst {
  float t[3];
  uint32_t fetch[6];  // fetch[1] == 0: none recorded
};
inline std::unordered_map<uint64_t, std::vector<DefaultInst>> g_frame_default_inst;
// The selected piece's x-ray draws (see DrawItem::xray): key -> world from
// the draw's VS rows / its draws (g_palette_mutex).
inline std::unordered_map<uint64_t, std::array<float, 16>> g_frame_xray_world;
inline std::unordered_map<uint64_t, std::vector<DrawEntry>> g_frame_xray_entries;
// ... and its overlap-highlight draws (DrawItem::highlight), with c24.
inline std::unordered_map<uint64_t, std::array<float, 16>> g_frame_hl_world;
inline std::unordered_map<uint64_t, std::vector<DrawEntry>> g_frame_hl_entries;
inline float g_frame_hl_rgb[3] = {0.0f, 0.0f, 0.0f};
// Blur modulate color (the blur passes' PS c1): both blur ucodes end in
// `mul oC0, r0, c1`: the game's menu fade. Gameplay popups stage (1,1,1)
// (why the exact port originally had no multiply); the pause menu stages
// ~(0.35,0.33,0.32), squared across the H+V passes = the darkened pause
// backdrop. Read live on the blur_hBlurPS draw, accepted only when the
// bank's c0 kernel row reads exactly (8,8); a stale mid-stage bank fails
// that gate (the staleness class that pulsed the blur radius when c0.x was
// fed live, see g_ui_blur above).
inline float g_ui_blur_color[3] = {1.0f, 1.0f, 1.0f};
// Hold the blur across publishes that carry no blur draw: the game does not
// issue the pass chain on every swap while the popup is up, and publishing
// the raw per-swap flag alternated blur on/off: a visible brightness
// shimmer. Two publishes of hysteresis bridges the gaps; on popup close the
// blur lingers ~2 guest frames, imperceptible.
inline int g_ui_blur_hold = 0;
// The game's final fisheye-family postfx pass (postfx_basictex_fisheye*:
// the camera-man / filmer lens, and the respawn "TV noise" variant): which
// variant (FrameScene::LensFx::Kind), its PS/VS rows and fetch words, read
// at the guest draw (OnDrawDone) and published as FrameScene::lens_fx. One
// publish of hold bridges a swap that carries no such draw.
inline bool g_lens_fx_seen = false;
inline int g_lens_fx_kind = 0;
inline uint32_t g_lens_fx_blend = 0;  // the draw's blend-control register
// The game's bloom chain (bloom_dof_* passes, composited by postfx_fast_uber
// / postfx_visualfx as scene * 1.41 + bloom): PS rows c0..c10 of each pass,
// read at its guest draw, slot = FrameScene::GameBloom::Pass. Bit per slot
// in g_game_bloom_seen; published as FrameScene::game_bloom.
inline float g_game_bloom_rows[7][11][4] = {};
inline uint32_t g_game_bloom_seen = 0;
// Publishes since each pass last drew (the chain can straddle a publish).
inline uint8_t g_game_bloom_age[7] = {255, 255, 255, 255, 255, 255, 255};
inline int g_lens_fx_hold = 0;
inline float g_lens_fx_ps[8][4] = {};
inline float g_lens_fx_vs[8][4] = {};
inline uint32_t g_lens_fx_fetch[8][6] = {};
// Host settings-overlay backdrop: forces the same blur chain while the SDK
// settings menu is open (the game issues no popup blur then). Set from the
// app UI thread, read at scene publish.
inline std::atomic<bool> g_settings_menu_blur{false};
// Emulated-post blur diagnostics: reset on each menu open so every open
// logs a fresh burst.
inline std::atomic<uint32_t> g_post_blur_log_count{0};
// Selected-object outline capture (see DrawItem::selected): in the park
// editor / object mover the game EXCLUDES the selected object from the main
// color pass and re-draws it right after the sky, twice, back to back, the
// stencil-marking passes the postfx edge-detect turns into the blue contour
// (verified in capture: consecutive environmentpark decal +
// diffuse parts, identical VS banks, vs the same mesh at OTHER placements in
// the main pass). Hook side records each post-sky environmentpark/
// dynamicobject draw's (ib, vb, world translation); entries seen >= 2 times
// mark the matching published items. Guest render thread only, like the fog
// capture state.
struct SelectedDrawKey {
  uint32_t ib;
  uint32_t vb;
  float t[3];
  uint32_t count;
};
inline bool g_sky_seen_this_frame = false;
inline std::vector<SelectedDrawKey> g_frame_selected;
// The guest issued its postfx_edgedetectstencil draw this frame: the game's
// own "an outline is being drawn" signal (park editor / object mover with an
// active selection). Post-sky double-draws alone are NOT sufficient evidence
// of a selection: normal gameplay draws some small props twice after the sky
// (far LOD/imposter passes), which used to sprout phantom outlines on distant
// objects. Guest render thread only; reset per frame in BuildFrameScene.
inline bool g_outline_edge_seen = false;
// Outline color, refreshed from the guest postfx_edgedetectstencil draw's
// PS c0 (the park-editor blue in every capture) whenever that pass runs.
inline float g_outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
// PIXEL banks keep the CSM constants at c0..c8, pass-global (identical on
// every environment-family draw of the pass; character/hair/tree PSes
// allocate differently and are rejected by the sanity gate). Captured on the
// same camera-keyed main-pass draws as the fog rows.
inline float g_shadow_rows[48] = {};
inline bool g_shadow_have = false;
inline bool g_shadow_frame_done = false;
// Guest frame of the last SANE capture. Interior venues (the park-editor
// warehouses) run a different environment shader whose bank never passes the
// sanity gate (near-zero sun, exposure ~0.001), so the rows latch stale
// outdoor state there; consumers that must not act on stale sun state key
// off this freshness stamp (see FrameScene::shadow_fresh).
inline uint64_t g_shadow_rows_frame = 0;
// Frame-global rows of the tree / proxyworld shader families (see
// FrameScene::family_rows), captured from their PS banks when a draw with
// the matching debug path runs. Guest render thread only.
inline float g_family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
// treeanimate_defaultVS c7..c10, refreshed from the actual animated-tree
// draw bank and copied into FrameScene. The first row's x component is the
// game's animation time; the remaining rows define the sway field.
inline std::mutex g_tree_anim_mutex;
inline float g_tree_anim_rows[16] = {};
inline bool g_tree_anim_have = false;
// vertexanimate_defaultVS c7..c10 + its PS c0 (animated.flag), under
// g_tree_anim_mutex.
inline float g_flag_anim_rows[20] = {};
inline bool g_flag_anim_have = false;
inline bool g_tree_frame_done = false;
inline bool g_proxy_frame_done = false;
// flowingwateralpha m_params + animation-time rows (see
// FrameScene::water_rows), captured from a flowingwateralpha PS bank
// (debug-path classified). Guest render thread only. 32 floats: the
// capture fills 29 of them and FrameScene::water_rows copies all 32 (it
// was declared 17, so the capture wrote past its end and the c18 / c10.w /
// c12 / c15 / c13.w rows were read back from whatever followed it).
inline float g_water_rows[32] = {};
inline bool g_water_have = false;
inline bool g_water_frame_done = false;
// ocean_defaultPS / oceanreflection_defaultPS rows (see
// FrameScene::ocean_rows / oceanrefl_rows). Guest render thread only.
// Guest D3D sampler slots -> bound guest texture pointer, recorded by the
// D3DDevice_SetTexture hook. water_defaultPS refracts the framebuffer from
// tf3, which the GAME resolves into a texture and binds here -- that copy is
// the shader's real colour input, so we read it rather than inventing one.
inline uint32_t g_tex_slot[16] = {};

// Sampler 3 / 5 as bound ON AN ACTUAL water_defaultPS DRAW (recorded by the
// water constant capture, which runs on the guest thread at draw time).
// Reading g_tex_slot during the frame build instead picked up whatever the
// last unrelated draw had bound, which never decoded.
inline uint32_t g_waterdef_tf3 = 0;
inline uint32_t g_waterdef_tf5 = 0;
// flowingwateralpha's tf4: the surface COLOUR map. The ucode fetches it at
// the base uv, squares it (this engine's gamma convention) and multiplies
// the body by it (lines 13/27/80). water.flowingalpha carries no diffuse
// channel, so without this the body is just white x lighting.
inline uint32_t g_water_tf4 = 0;
// ocean_defaultPS's tf7: the environment CUBE the game binds for the ocean
// draw (tfetchCube r4, r3.xyz, tf7). The material's "environment" channel
// is NOT what the shader samples -- in Industrial it pointed at a black-
// backed studio reflection cube, whose black +Y star showed through the
// ripples as clusters of black pixels.
inline uint32_t g_ocean_tf7 = 0;
inline uint32_t g_waterdef_tf6 = 0;
// water.default constants are PER MATERIAL, not per frame: a level can hold
// several water surfaces whose PCA bases differ (observed means of
// (0.0100, 0.0085, 1.0000) and (0.5002, 0.9876, 0.5002) in one canal). The
// old single global kept whichever draw came first each frame, so a surface
// was decoded through the OTHER material's basis whenever the draw order
// flipped -- which is what blew the normal up and made the artifact track
// the view angle. Keyed by tf6 (the material's own normal map, which equals
// item.water_normal, so the draw can look its set up).
inline constexpr uint32_t kWaterdefSets = 8;
inline float g_waterdef_set_rows[kWaterdefSets][56] = {};
// Round-robin victim when every slot is taken by a different material.
inline uint32_t g_waterdef_set_next = 0;
inline uint32_t g_waterdef_set_key[kWaterdefSets] = {};
inline bool g_waterdef_set_have[kWaterdefSets] = {};
inline uint32_t g_waterdef_tf7 = 0;

// water.default (water_defaultPS) rows; see FrameScene::waterdef_rows.
inline float g_waterdef_rows[56] = {};
inline bool g_waterdef_have = false;
inline bool g_waterdef_frame_done = false;
inline float g_ocean_rows[40] = {};
inline bool g_ocean_have = false;
inline bool g_ocean_frame_done = false;
// advertisement_defaultPS rows (see FrameScene::ad_rows): c6.z lightmap
// floor, c10.y material multiplier, c9.x exposure. Guest render thread.
inline float g_ad_rows[3] = {};
inline bool g_ad_have = false;
inline bool g_ad_frame_done = false;
// transparentenvironment_defaultPS rows (see FrameScene::ts_rows).
inline float g_ts_rows[10] = {};
inline bool g_ts_have = false;
inline bool g_ts_frame_done = false;
inline float g_oceanrefl_rows[4] = {};
inline bool g_oceanrefl_have = false;
inline bool g_oceanrefl_frame_done = false;
// scrollincandescent.fx rows (see FrameScene::scroll_rows): [0] =
// g_fAnimationTime (VS c9.x), [1] = the material multiplier m_params[0].y
// (PS c3.y). Captured from a scrollincandescent draw (debug-path
// classified). Guest render thread only.
inline float g_scroll_rows[2] = {};
inline bool g_scroll_have = false;
inline bool g_scroll_frame_done = false;
// dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows),
// captured from a dynamicobject/alphatestdynamicobject PS bank (debug-path
// classified). Guest render thread only.
inline float g_dynobj_rows[13] = {};
inline bool g_dynobj_have = false;
inline bool g_dynobj_frame_done = false;
// dynamicobject static world-shadow transform rows (PS c5/c6/c7), captured
// alongside g_dynobj_rows, see FrameScene::dynobj_ws.
inline float g_dynobj_ws[12] = {};
inline bool g_dynobj_ws_have = false;
// environmentpark.* frame rows (see FrameScene::park_rows), captured from
// the park pixel shaders' own banks. Kept separate from g_dynobj_rows: the
// park draws never refresh those, so sharing them made the pieces flicker
// whenever no real dynamicobject was on screen. Guest render thread only.
// Two sets, one per shading model ([0] = diffuse / decal floor-lerp, [1] =
// alphatest additive; see FrameScene::park_rows): one shared set took the
// model of whichever park draw came first in the frame, and an alphatest
// piece drawn first shaded every floor / rail / pergola with the alphatest
// model (its tf1 world-shade lookup reading the tf2 park map: long garbage
// streaks across the park, capture 1790873558).
inline float g_park_rows[2][24] = {};
inline bool g_park_have[2] = {false, false};
inline bool g_park_model_done[2] = {false, false};
// The model-0 park light cookie (see FrameScene::park_lm).
inline float g_park_lm[8] = {};
inline uint32_t g_park_lm_fetch[6] = {};
inline bool g_park_lm_have = false;
inline bool g_park_frame_done = false;
// Dynamic entities (characters, movable props) live entirely in transient
// per-frame arenas: the context, its record, mesh pointers, transforms and
// bone palettes are all recycled before frame end. The complete DrawItem is
// therefore built at RenderMesh hook time and stored here.
inline std::mutex g_palette_mutex;
inline std::vector<DrawItem> g_frame_dynitems;
// (ib_obj << 32 | vb_obj) -> indices of pending items in g_frame_dynitems,
// for the post-draw state fixup (deferred / world-path captures draw after
// their capture; the fixup fills palette or world from the actual draw).
inline std::unordered_multimap<uint64_t, size_t> g_frame_pending_by_buffers;
// Keys ever inserted into g_frame_pending_by_buffers this frame: the
// indexed-draw hook rejects the (vast majority of) draws with nothing
// pending through this small flat set instead of a multimap probe per
// draw. Erased keys may linger (they only cost the multimap lookup).
inline FlatSet<uint64_t> g_frame_pending_keys;
// Contexts submitted through an ORTHO (shadow caster-cascade) bank this
// frame, i.e. items the GAME drew into its own shadow passes. The game's
// caster list is per-piece: the CAS trucker hat submits ONLY to the main
// view (verified in capture: every char piece has shadow-pass draws
// except the hat); natively casting it painted a hard low-sun brim band
// across the editor face that the emulated frame never shows. The native
// atlas pass renders char pieces only when their ctx is in this set
// (rescued/gap items inherit last frame's flag). Guarded by
// g_palette_mutex; cleared at the end of BuildFrameScene.
inline FlatSet<uint32_t> g_frame_ortho_ctx;
// ctx -> ||c0.xyz|| of each ortho caster bank it was submitted through
// this frame: the cascade the game drew it into. The game culls casters
// PER CASCADE (F11: the CAS skater's pieces drew into tile 0 only while
// a straddling pose drew tiles 0 and 1), and each cascade's caster bank
// carries the receiver X row scaled by that cascade's PS c1.x / c2.x
// (1, 0.25, 0.0625 of the finest), so the row norm names the tile. Same
// guard and lifetime as g_frame_ortho_ctx.
inline std::unordered_multimap<uint32_t, float> g_frame_ortho_mag;
// ib_obj -> the game render states of its perspective draw this frame
// (FrameScene::draw_rs). Guarded by g_palette_mutex.
inline FlatMap<uint32_t, DrawRegState> g_frame_draw_rs;
// (ib_obj << 32 | vb_obj) -> dynitem indices whose character-lighting rows
// should refresh on every later matching draw this frame. The one-shot
// palette fixup usually pops on the shadow-CASTER draw, whose PS bank is
// stale (shadowPS uploads no PS constants); re-capturing on the main-pass
// draw (last writer) lands the real per-instance rows (stamp tints etc.).
inline std::unordered_multimap<uint64_t, size_t> g_frame_char_refresh;
// Keys of g_frame_char_refresh (same per-draw prefilter as
// g_frame_pending_keys).
inline FlatSet<uint64_t> g_frame_char_refresh_keys;
// (ib_obj << 32 | vb_obj) -> every main-pass (perspective bank) draw of a
// character/vehicle mesh this frame: bone-0 translation of the DRAW'S OWN
// palette (at the bank's palette base and one row later, the cloth/morph
// layout) plus the lighting/paint rows derived from that draw's PS bank.
// Consumed after the authoritative palette serve: served items take the
// rows of the draw whose palette sits at their served pose (see
// BuildFrameScene), because the captured bones the draw-time refresh pairs
// against can belong to another instance of the same mesh.
struct CharDrawRows {
  float t[2][3];
  float rows[72];
  uint32_t seq;  // draw order within the frame (later = the main pass)
};
inline uint32_t g_frame_char_draw_seq = 0;
// (ib_obj << 32 | vb_obj) -> the CAS skin custom graphic at this frame's
// last cacstamp_skin main-pass draw with those buffers: VS c190.xyz /
// c191.xyz placement rows + the tf6 fetch words (see DrawItem::char_skin).
// Recorded in the every-indexed-draw hook: the character-lighting refresh
// only runs for some pieces, and the graphic then never arrived on most
// frames. Guarded by g_palette_mutex; swapped out by BuildFrameScene.
struct SkinGraphic {
  float cg[6];
  uint32_t tf6[6];
};
inline std::unordered_map<uint64_t, SkinGraphic> g_frame_skin_graphic;
// (ib_obj << 32 | vb_obj) -> the board's selected graphic at the last
// cacstamp_shift main-pass draw: c190/c191 placement plus tf5 fetch words.
struct ShiftGraphic {
  float cg[6];
  uint32_t tf5[6];
};
inline std::unordered_map<uint64_t, ShiftGraphic> g_frame_shift_graphic;
// CAC printed garments use their own tf5 artwork sampler and c190/c191
// placement, independent of both the skin tattoo and its tf7 injury layer.
struct ClothStampGraphic {
  float cg[6];
  uint32_t tf5[6];
};
inline std::unordered_map<uint64_t, ClothStampGraphic> g_frame_cloth_stamp_graphic;
// (ib_obj << 32 | vb_obj) -> the face injury layer at this frame's last
// cac_face_{default,nis}PS main-pass draw with those buffers: tf6 = the
// skater's injury map (bruises / scrapes: rgb colour, a mask) and PS c22.z
// = its strength (face ucode 12/20-23: albedo = lerp(albedo, tf6.rgb,
// tf6.a * c22.z)). Guarded by g_palette_mutex; swapped out by
// BuildFrameScene.
struct FaceInjury {
  uint32_t tf6[6];
  float amount;
};
inline std::unordered_map<uint64_t, FaceInjury> g_frame_face_injury;
// (ib_obj << 32 | vb_obj) -> the runtime overlay layer the game binds at
// this frame's last main-pass draw of a CAC cloth / skin piece: cloth =
// tf5, the skater's accumulated dirt / wear map (cac_cloth[_ropa]_*PS 10/
// 19-28, lerp by dirt.a x c22.y; printed cacstamp_cloth garments bind it at
// tf6, their tf5 being the stamp); skin = tf7, the body injury map
// (cacstamp_skin_{default,nis}PS 13/18-28: lerp(albedo, tf7.rgb, tf7.a x
// c22.z)). Both are per-skater textures the engine binds at draw time, not
// the material's channels. Guarded by g_palette_mutex.
struct CharLayer {
  uint32_t words[6];
  float amount;
  uint32_t slot;
};
inline std::unordered_map<uint64_t, CharLayer> g_frame_char_layer;
// (ib_obj << 32 | vb_obj) -> {PS c2.x exposure, PS c3.y material multiplier}
// of the piece's base/transparent incandescent draw this frame (see
// DrawItem::incand_expo).
inline std::unordered_map<uint64_t, std::array<float, 2>> g_frame_incand;
// (ib_obj << 32 | vb_obj) -> the CAS subsurface-scattering state of those
// buffers this frame (cac_face_nisPS / cacstamp_skin_nisPS): unwrap = the
// game drew the piece's texture-space lighting pass (*_unwrapPS, a 128x128
// target); mask7/mask8 = fetch slots 7/8 at its main-pass draw (the
// "blurmask": tf7 on the face, tf8 on the body skin); amount = that draw's
// PS c21.x (nis line 68: a = mask.x * c21.x). Recorded in the
// every-indexed-draw hook; guarded by g_palette_mutex; swapped out by
// BuildFrameScene.
struct SkinSssRecord {
  bool unwrap = false;
  bool main = false;
  uint32_t mask7[6] = {};
  uint32_t mask8[6] = {};
  float amount = 0.0f;
  // Cutscene NPC skin (character_skin_unwrapPS -> character_skin_nisPS):
  // the same scatter mix with its own registers, blurmask = tf6 and
  // a = mask.x * c12.x (nis 56/62; c12.x = 0.65 in the captured cinematic).
  uint32_t mask6[6] = {};
  float amount12 = 0.0f;
};
inline std::unordered_map<uint64_t, SkinSssRecord> g_frame_skin_sss;
// The game's world-shade pass this frame (the map its character / vehicle /
// dynamicobject PSes read as tf1): the (ib_obj << 32 | vb_obj) keys it drew,
// recognized by the pass's ortho bank whose depth row is the world c7 (a
// ~1.6e-4 per-metre height ramp; the CSM cascades' is ~0.5). The native
// world-shade map renders exactly these. Guarded by g_palette_mutex;
// swapped out by BuildFrameScene.
inline FlatSet<uint64_t> g_frame_ws_keys;
// ... and the exact draws it issued per key (prim, base_vertex, start_index,
// index_count = the DrawIndexedVertices arguments): the island set the pass
// itself culled for its own view. The casters' contexts carry, by frame
// end, the MAIN camera's island selection, which dropped every building
// outside the camera view (and every off-view island of a visible one)
// from the native map (g_palette_mutex).
inline std::unordered_map<uint64_t, std::vector<DrawEntry>> g_frame_ws_entries;
inline uint32_t g_frame_ws_draws = 0;
// The MeshContexts the game's world-shade pass renders this frame (see
// OnWorldShadowRender). Guarded by g_palette_mutex.
inline std::vector<uint32_t> g_frame_ws_ctxs;
// The world-shade pass's own projection rows this frame (its VS c0/c1/c2 =
// the receivers' c5/c6/c7), recorded at its draws. Guarded by
// g_palette_mutex.
inline float g_frame_ws_rows[12] = {};
// The receivers' own world-shade rows this frame (PS c5/c6/c7 of a draw
// whose tf1 is the world-shade texture). Guarded by g_palette_mutex.
inline float g_frame_ws_recv_rows[12] = {};
inline bool g_frame_ws_recv_have = false;
inline bool g_frame_ws_rows_have = false;
// The world-shade texture's physical base (learned from the receivers' tf1
// fetch: k_24_8 512x512) and whether the game resolved into it this frame
// (g_palette_mutex).
inline std::atomic<uint32_t> g_ws_tex_addr{0};
// DIAGNOSTIC: guest frame of the last aux-perspective (portrait / snapshot)
// pass draw.
inline std::atomic<uint64_t> g_aux_pass_frame{0};
inline bool g_frame_ws_resolved = false;
// The game's skate.Park world-shadow map: the tf2 every environmentpark
// {diffuse,decal}_defaultPS reads its world term from. A 1024x512 k_16_16
// target holding two 512x512 tiles (inner left, outer right; viewport X
// offsets 256 / 768), rendered every frame by two ortho passes that draw
// the park pieces (PA_SU_SC_MODE_CNTL 0x18006 = cull back) with the
// receivers' own rows: inner = PS c5 / c7 / c8, outer = the same scaled by
// c6 (verified register for register in a skate.Park F11 capture). The
// receivers compare it exponentially:
//   world = sat(exp(c10.z * (stored - (dot(c8, p) - bias))) + edge_fade)
// with bias c10.x (inner) / c10.y (outer). Each pass draw carries the
// piece's own world matrix in VS c4..c7, so the native map rebuilds exactly
// the instances the game drew into each tile.
struct ParkWsDraw {
  uint64_t key = 0;  // ib_obj << 32 | vb_obj
  uint8_t tile = 0;  // 0 inner, 1 outer
  float world[16] = {};
};
// This frame's park world-shadow pass draws (g_palette_mutex).
inline std::vector<ParkWsDraw> g_frame_park_ws;
// The two passes' projection rows (VS c0 / c1 / c2) as drawn this frame.
inline float g_frame_park_ws_rows[2][12] = {};
inline bool g_frame_park_ws_rows_have[2] = {false, false};
// The receivers' sampling constants (park PS c5, c6, c7, c8, c10), latched
// from the park banks; the passes are classified against them.
inline float g_park_ws_recv[20] = {};
inline bool g_park_ws_recv_have = false;
// tf2's physical base (learned from the receivers' fetch: k_16_16 1024x512)
// and whether the game resolved into it this frame (g_palette_mutex).
inline std::atomic<uint32_t> g_park_ws_tex_addr{0};
inline bool g_frame_park_ws_resolved = false;
// The world receivers of the same map: while a skate.Park is loaded the game
// draws the world with the `_skatepark` permutations of its world pixel
// shaders (baseenvironment, baseterrain, decal*, transparent*, advertisement,
// alphatestdefaultenvironment, ...; not defaultenvironment, which has no
// tf2). Every one of them shades with
//   min(lightmap^2, lerp(c11, 1, cascade), lerp(c12, 1, world))
// (baseenvironment_skateparkPS 72/87/88/90), the world term being the tf2
// exponential compare above with the same c5..c8 / c10 rows and the cascade
// term the coverage-aware tf0 fetch with bias c9.x. Keys (ib_obj << 32 |
// vb_obj) of this frame's main-pass draws through such a shader, and the
// banks' c11.xyz / c12.xyz / c9.x as [0..2] / [4..6] / [8] (g_palette_mutex).
inline FlatSet<uint64_t> g_frame_dmo_keys;
inline float g_park_dmo_rows[12] = {};
inline bool g_park_dmo_have = false;
// postfx_jitterBlurPS c0.xy (the tap-offset scale; 0.75 in every capture),
// read from the game's own blur draw (a BeginVertices quad into the 128x128
// target). Bit patterns; 0 = not seen yet.
inline std::atomic<uint32_t> g_skin_blur_c0x{0};
inline std::atomic<uint32_t> g_skin_blur_c0y{0};
inline std::unordered_multimap<uint64_t, CharDrawRows> g_frame_char_draw_rows;
// (ib_obj << 32 | vb_obj) -> device fetch-shadow slots 3+4 (12 dwords) at
// this frame's LAST indexed 3D draw with those buffers (the z-prepass draws
// first, so the main pass wins). Consumed at frame end by the
// streamed-artwork diffuse override (DrawItem::diffuse_fetch): poster/advert
// materials keep a 16x16 min-mip placeholder in the material channel while
// the engine binds the streamed art via the fetch constants at draw time.
// Guarded by g_palette_mutex; cleared at the end of BuildFrameScene.
inline FlatMap<uint64_t, std::array<uint32_t, 12>> g_frame_draw_fetch;
// One game draw's identity: its buffers plus the island's base vertex and
// start index (the DrawIndexedVertices r5/r6 = DrawEntry fields). Buffers
// alone are NOT a mesh identity: the game packs several meshes with
// different materials into one vertex/index buffer pair and draws each as
// its own island range (the shader census saw tree / decal / ocean shaders
// all drawn from the same buffers).
inline uint64_t DrawKey(uint32_t ib, uint32_t vb, uint32_t base_vertex,
                        uint32_t start_index) {
  uint64_t k = 1469598103934665603ull;
  for (uint64_t v : {uint64_t(ib), uint64_t(vb), uint64_t(base_vertex), uint64_t(start_index)}) {
    k = (k ^ v) * 1099511628211ull;
  }
  return k;
}
// DrawKey -> the decal art binding (texture + sampler state)
// the game made at tf4 on that environment-decal mesh's main-pass draw (see
// DrawItem::decal_addr / decal_fetch). Registered by ApplyDecalDrawAddressing
// at item build; the indexed-draw hook fills `words` only from a draw whose
// bound pixel shader is a decal[2]environment main-pass permutation (all of
// them fetch the art at tf4: shader_dump *_defaultPS / *_skateparkPS), so
// z-prepass / caster draws with stale bindings never write it. Persistent
// across frames (the binding is per material; items the game skips this
// frame keep the last one). Guarded by g_palette_mutex.
struct DecalDrawAddr {
  uint32_t mesh = 0;           // owning mesh (buffer reuse resets the entry)
  uint32_t words[6] = {};      // tf4 fetch constant as bound, words[1] != 0 = seen
  uint32_t words2[6] = {};     // tf5 (decal2environment's second art layer)
  // VS c10.x of that draw: the floor of the macro-overlay fade under the
  // art (decal[2]environment VS 17: o4.w = c10.x; PS 23/27/60:
  // ov' = lerp(1, ov, max(1 - art.a, c10.x))). < 0 = not captured.
  float fade_floor = -1.0f;
};
// DrawKey -> a trafficlight item's per-draw constants (see
// DrawItem::traffic_c7), registered by ApplyTrafficDrawState, filled by the
// indexed-draw hook while a trafficlight_* pixel shader is bound.
// Persistent like g_decal_draw_addr. Guarded by g_palette_mutex.
struct TrafficDrawState {
  uint32_t mesh = 0;
  bool seen = false;
  float c7[4] = {};
  float c3y = 1.0f;
};
// DrawKey -> a Hall of Meat item's PS constants (see DrawItem::hom),
// registered by ApplyHomDrawState, filled by the indexed-draw hook while a
// defaulthom PS is bound. Guarded by g_palette_mutex.
struct HomDrawState {
  uint32_t mesh = 0;
  bool seen = false;
  float c2[4] = {};
  float c7[2] = {};
  float c1y = 1.0f;
};
// DrawKey -> a flowing-water draw's own material rows (FrameScene::
// water_rows layout) and colour map (tf4), see DrawItem::water_rows.
// Registered by ApplyFlowWaterState, filled by the indexed-draw hook while a
// flowingwater / flowingwateralpha PS is bound. Guarded by g_palette_mutex.
struct FlowWaterDrawState {
  uint32_t mesh = 0;
  bool seen = false;
  float rows[32] = {};
  uint32_t tf4 = 0;
};
// DrawKey -> a world item's own main-pass render-state registers (see
// DrawItem::game_rs), registered by ApplyWorldDrawRs, filled by the
// indexed-draw hook on perspective draws with a main-pass pixel shader.
// Guarded by g_palette_mutex.
struct WorldDrawRs {
  uint32_t mesh = 0;
  bool seen = false;
  uint32_t rs[6] = {};
};
// DrawKey -> an advertisement item's own PS rows + tf4 binding (see
// DrawItem::ad_rows), registered by ApplyAdDrawState, filled by the
// indexed-draw hook while an advertisement_* PS is bound. Guarded by
// g_palette_mutex.
struct AdDrawState {
  uint32_t mesh = 0;
  bool seen = false;
  float rows[3] = {};
  uint32_t tf4[6] = {};
  uint32_t tf3[6] = {};
};
// DrawKey -> everything the indexed-draw hook records per island, in ONE
// map (one lookup per draw in the hook, one per island per item build;
// six separate maps cost six of each). `mesh` is the owning mesh (buffer
// reuse resets the entry); `reg` says which per-family states the item
// registered (kIsl*), so the hook only reads / writes those. The sub-
// states' own `mesh` fields are unused. Guarded by g_palette_mutex.
enum : uint8_t {
  kIslDecal = 1,
  kIslTraffic = 2,
  kIslHom = 4,
  kIslFlow = 8,
  kIslAd = 16,
  kIslRs = 32,
  // The families the hook captures on EVERY draw of the island.
  kIslPerDraw = kIslDecal | kIslTraffic | kIslHom | kIslFlow | kIslAd,
};
struct IslandState {
  uint32_t mesh = 0;
  uint8_t reg = 0;
  WorldDrawRs rs;
  DecalDrawAddr decal;
  TrafficDrawState traffic;
  HomDrawState hom;
  AdDrawState ad;
  FlowWaterDrawState flow;
};
inline PooledFlatMap<uint64_t, IslandState> g_islands;
// The islands the indexed-draw hook must look up on every draw: a per-draw
// family registered (kIslPerDraw), or a render state not captured yet. All
// other islands only take the staggered 1-in-32-frames render-state refresh,
// so the hook skips the g_islands lookup (a cache miss into a table of up
// to 32k entries, then another into the ~300-byte state) for 31 of every
// 32 of their draws. A stale extra key only costs a lookup. Maintained by the
// item build (insert) and the hook (erase once captured); cleared with
// g_islands. Guarded by g_palette_mutex.
inline FlatSet<uint64_t> g_islands_hot;
// Shader census (skate3_native_render_scene_shader_census): DrawKey -> the
// distinct pixel-shader objects the game drew that island with (main-pass permutations; prepass / caster shaders filtered). Read at
// item build to log each (material name, game pixel shader) pair once.
// Per frame (cleared with g_frame_draw_fetch). Guarded by g_palette_mutex.
struct CensusPs {
  uint32_t obj[4] = {};
};
inline std::unordered_map<uint64_t, CensusPs> g_census_ps;
// mesh -> last VALIDATED character-lighting rows (see CaptureCharLighting).
// The per-frame capture depends on a fragile draw-order chain (capture at
// submit-exit -> pending fixup -> per-draw refresh, commit-on-success); on
// frames where no draw in the chain carried this character's main-pass PS
// bank the item ends with INVALID rows and the renderer falls back to the
// legacy empirical shading; the player visibly flickered between the two
// looks while moving, and the hair left the blended sub-pass (opaque "hair
// helmet" covering the cap crown). The rows are frame-coherent (sun/ambient
// move slowly), so reusing the previous valid capture is visually seamless.
// TRAP: NPC clones share a mesh with PER-INSTANCE rows (stamp tints, the
// teal-vest twins), so the fallback applies only to meshes with a single
// instance in the frame. Guest-render-thread only (capture hooks +
// BuildFrameScene), no lock needed.
inline std::unordered_map<uint32_t, std::array<float, 72>> g_char_rows_cache;
// Frame-global character CSM receive biases (see FrameScene::char_shadow_bias)
// + the guest frame they were captured on (served while <120 frames stale;
// they only change with the scene lighting setup). Guest render thread only.
inline float g_char_shadow_bias[3] = {};
inline uint64_t g_char_shadow_bias_frame = 0;
inline std::atomic<uint64_t> g_char_rows_reused{0};
// Cached palette entry for the per-instance rescue below. A refused
// capture re-publishes with the cached palette: one frame of pose lag is
// invisible; a one-frame disappearance is not. Guest-render-thread only.
struct CachedBones {
  std::vector<float> bones;
  // g_guest_frame at refresh: rescues/heals must be FRESH; a close-pass
  // refusal resurrecting a seconds-old palette rendered the vehicle 10-20 m
  // BEHIND its live position (the momentary ghost-back).
  uint64_t frame = 0;
};
// ctx -> last published palette for skinned character-family items. The
// MeshContext is the game's own per-instance identity, so this cache
// rescues each instance with ITS OWN last palette regardless of how many
// clones are alive.
inline std::unordered_map<uint32_t, CachedBones> g_bones_cache_ctx;
inline std::atomic<uint64_t> g_lw_ctx_rescued{0};
// Non-live republishes refused because the game is not drawing the
// instance (faded out, or in view with no draw of its mesh): ghost NPCs.
inline std::atomic<uint64_t> g_ghost_suppressed{0};
// LW entity store consumption counters (stats line lw[...]).
inline std::atomic<uint64_t> g_lw_stamped{0};
inline std::atomic<uint64_t> g_lw_fade0{0};
inline std::atomic<uint64_t> g_lw_gap_filled{0};
// Authoritative caster-palette substitutions + per-ctx lighting-rows serves
// (edge-of-view vehicles; see the stamp pass).
inline std::atomic<uint64_t> g_lw_pal_sub{0};
inline std::atomic<uint64_t> g_lw_rows_served{0};
// ctx -> last VALIDATED lighting/paint rows, entity-checked (a recycled
// instance must not inherit the previous occupant's paint). The mesh-keyed
// g_char_rows_cache fallback is gated to single-instance meshes; cloned
// traffic never qualified, so a caster-only stretch (main view culls the
// vehicle at the screen edge before it leaves the screen) dropped to
// legacy flat shading: the "vehicle loses its texture/color at the edge"
// sighting.
struct CharRowsCtx {
  std::array<float, 72> rows;
  uint32_t entity = 0;
};
inline std::unordered_map<uint32_t, CharRowsCtx> g_char_rows_cache_ctx;
// (mesh,ctx) -> last VALIDATED rows for EVERY character item with an
// instance context: the general rescue the two caches above each cover
// only a slice of: the mesh-keyed cache is gated to single-instance meshes
// (character pieces also publish caster items of the same mesh, so gameplay
// pieces rarely qualify) and the ctx cache is gated to LW-mapped entities
// (the PLAYER is not one). A capture-failed frame without a rescue drops
// hair out of the blended sub-pass onto the legacy opaque path: a
// frame-to-frame hair/garment appearance flip (the
// failure rate scales with guest fps, so it reads as a high-fps
// flicker). Entity-stamped like CharRowsCtx (0 for non-LW instances) so a
// recycled ctx never inherits the previous occupant's rows.
struct CharRowsInst {
  std::array<float, 72> rows;
  uint32_t entity = 0;
};
inline std::unordered_map<uint64_t, CharRowsInst> g_char_rows_cache_inst;
// Entity-keyed lighting rows for ROPA garments only: the sim on/off switch
// swaps between a garment's two renderables (cloth-target vs original
// model, different mesh AND ctx), so the (mesh,ctx) caches never carry
// rows across the switch and the freshly resolved model rendered dark/flat
// until its own capture validated. Both models map to the same owning
// entity in the identity store, and they are the same garment; sharing
// rows across the pair is exact. Garment-to-garment only; body pieces keep
// their per-piece caches.
struct CharRowsEnt {
  std::array<float, 72> rows;
  uint8_t fam = 0;  // donor family: the row layout is family-specific
};
inline std::unordered_map<uint32_t, CharRowsEnt> g_char_rows_cache_ent;
inline std::atomic<uint64_t> g_char_rows_ent_served{0};
inline std::atomic<uint64_t> g_char_rows_inst_served{0};
// Items served authoritative pack palettes this frame / total (see
// native_palette::ServeAuthoritativePalettes).
inline uint32_t g_pal_served_frame = 0;
inline std::atomic<uint64_t> g_pal_served_total{0};
// Route-flip probe: counts hair/character-alpha pieces whose pass route
// (blended sub-pass vs legacy opaque) changed between consecutive frames;
// each count is one visible flicker event.
inline std::atomic<uint64_t> g_hair_route_flips{0};
// A,B,A,B alternation probes: bone palettes (per piece) and the shadow
// cascade state (scene-wide) returning EXACTLY to the previous-but-one
// frame's value (legitimate animation/lighting drift never does).
inline std::atomic<uint64_t> g_hair_bone_alternations{0};
inline std::atomic<uint64_t> g_shadow_alternations{0};
// ctx -> last PUBLISHED item of a live LW entity (skinned character
// families, non-ropa): when a ctx drops out of the submit records for a
// frame or two while its entity is still alive in the store (observed as
// "dyn publish GAP" incidents on fam-3/5 ped meshes, 1-3
// frames each: an in-view NPC vanishing for a frame reads as a blink or,
// moving, a small teleport), the whole item republishes. Console behavior:
// a live entity never skips a frame. Self-limiting: entries expire after 2
// unpublished frames and refresh only from LIVE publishes (never from a
// fill, dbg_src 9), so a real despawn shows at most 2 filled frames, at
// the entity's own served alpha.
struct LwRetained {
  DrawItem item;
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, LwRetained> g_lw_last_items;
// mesh -> last RESOLVED ropa garment state (rigid world OR skinned palette).
// Ropa items must NOT ride the palette-only rescue above: a ropa capture is
// refused exactly when the bank is stale (g_ropa_stale), and while the cloth
// sim is ACTIVE the VB holds sim-deformed root-local positions; skinning
// those with a cached palette is the mangled-ribbon interpretation (verified
// 0/31 in-clip; on-screen as map-length stretched strips, with matching
// shadows since the caster pass shares the item). And with no rescue at all
// a refused frame drops the garment (the momentary invisible torso). The
// dyn decode workers already keep the drawn cloth VB one frame behind the
// sim, so re-publishing last frame's resolved MODE + transform is exactly
// age-consistent with the vertices being drawn. Single-instance meshes only
// - clones share meshes with per-instance transforms. Guest-render-thread
// only.
struct RopaResolvedState {
  bool skinned = false;
  float world[16] = {};
  std::vector<float> bones;
  // Publish frame of the cached state: the rescues below exist to bridge
  // 1-3 frame capture gaps, and re-publishing an unbounded-age state pins a
  // ghost of the garment at a stale pose for as long as captures keep
  // failing (the frozen-in-the-air garment behind a skater whose capture
  // path was refusing every frame).
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, RopaResolvedState> g_ropa_state_cache;
inline std::atomic<uint64_t> g_ropa_rescued{0};
// mesh -> sample verts of the most recent DECODE of a skinned mesh (bind-
// space position + raw blend attribs, ~32 evenly spaced), written by
// DecodeMesh on whichever thread decodes (workers / render), read by the
// guest thread's draw-time STRETCH VETO in BuildFrameScene. The veto skins
// these samples with the FINAL post-interpolation palette, i.e. it judges
// what the GPU will ACTUALLY draw. Every upstream gate (capture acceptance,
// publish coherence) judges the LIVE guest VB instead, so a decode-content
// vs palette pairing mismatch, or junk introduced by the interpolation
// substitutions, passes all of them and still flashes the 1-frame
// map-length ribbon (observed with every ropa[] counter clean).
// `fp` = the decode's payload fingerprint so the veto can log whether the
// drawn content even matches the frame's payload (content lag vs junk
// palette: the decisive diagnostic split).
struct SkinProbeSample {
  float p[3];
  uint32_t bw;
  uint32_t bi;
};
struct SkinProbe {
  std::vector<SkinProbeSample> s;
  uint64_t fp = 0;
};
inline std::mutex g_skin_probe_mutex;
inline std::unordered_map<uint32_t, SkinProbe> g_skin_probe;
inline std::atomic<uint32_t> g_cur_ib{0};
inline std::atomic<uint32_t> g_cur_vb{0};
inline std::atomic<uint32_t> g_cur_vb_offset{0};
inline std::atomic<uint32_t> g_cur_vb_stride{0};
// Guest D3D::CDevice: the fetch-constant shadow lives at device+0x480,
// 6 dwords per fetch group (from recompiled SetPending_FetchConstants).
inline std::atomic<uint32_t> g_device{0};
// 2D/APT phase depth counters (see On2dPhase). All HUD/menu 2D elements are
// Flash SWFs converted to APT; draws issued inside these brackets are the 2D
// draw stream.
inline std::atomic<uint32_t> g_phase2d_depth[6];
inline std::atomic<uint64_t> g_draws_2d{0};
// 2D draws seen through paths the overlay does not replay yet
// (DrawIndexedVertices / DrawVertices in the 2D phase; gameplay HUD uses
// only the BeginVertices inline path, verified by capture).
inline std::atomic<uint64_t> g_draws_2d_other{0};
inline std::atomic<uint64_t> g_draws_2d_dropped{0};
// Large-art async decode routing (see skate3_native_render_scene_2d_async_px):
// quads skipped while their first decode is in flight on the workers, and
// stale decodes served while a content-change re-decode is in flight.
inline std::atomic<uint64_t> g_2d_async_skip{0};
inline std::atomic<uint64_t> g_2d_async_stale{0};

// One captured 2D draw (verified layout): BeginVertices
// inline vertices, stride 24 = {float x, y, z, w; float u, v} in 1280x720
// APT movie space; VS c0..c3 = ortho projection rows, c4..c7 = the
// element's 2D transform, c8 = color multiplier; diffuse texture in fetch
// shadow slot 0. Vertex payload is written by the CPU after BeginVertices
// returns, so it is read at frame end (addr) rather than at capture.
// NOTE on render-to-texture: in gameplay the ENTIRE HUD renders inside
// AptRenderingIntegration::UpdateRenderToTexture (bracket bit 3) into a
// screen-sized overlay texture, at true screen coordinates; the game
// composites that overlay over the 3D frame in a later (emulated,
// suppressed) pass. Replaying the draws directly onto the native output IS
// that composite, so bit 3 is diagnostic only, not a routing signal. The
// overlay content is straight-alpha art (verified by decoding the clock
// face/needle/sheen textures offline).
struct Draw2d {
  uint32_t prim;    // xenos primitive: 4 trilist, 5 tristrip, 13 quadlist
  uint32_t count;   // vertex count
  uint32_t stride;  // bytes per vertex at capture (see publish normalization)
  // Capture-time stride, preserved across the publish normalization (which
  // rewrites stride to the renderer's 28). The FMV substitution keys on it:
  // the movie quad is the stride-24 textured layout inside the
  // AptMovieIntegration bracket (bit 1).
  uint32_t src_stride = 0;
  uint32_t addr;    // guest inline-ring write pointer
  uint32_t flags;   // bracket bits at capture (layout disambiguation)
  // Index into the embedded replacement atlases (see
  // skate3_native_font_atlas.h), or -1 for "not a text batch / unknown
  // font", in which case the game's own texture is used unchanged.
  int32_t font_id = -1;
  // Texture fetch constants, shadow slots 0-2 (6 dwords each). Slot 0 is
  // the draw's own texture (all the replay paths read only [0..5]); slots
  // 1-2 exist for the VIDEO quads, whose console YUV shader binds the U and
  // V planes there; the replay's YUV-triple detection reads them.
  uint32_t fetch[18];
  float consts[36];   // VS c0..c8
  // The game's RB_BLENDCONTROL0 / RB_COLOR_MASK for this draw (see
  // g_rs_ptr_2200). Defaults = straight alpha, all channels.
  uint32_t blend_control = 0x07060706u;
  uint32_t color_mask = 0xFu;
  std::vector<uint8_t> verts;  // filled at frame end (little-endian dwords)
};
inline std::mutex g_2d_mutex;
// The create-a-skater photo's 2D composition (see PhotoJob): the PhotoEffects
// layers are bare SimpleDraw / font draws into the 1152x640 photo target, in
// that target's own coordinate space. The skater is drawn between them: the
// backdrop layer before it, colour bars / logos / the rotated gamertag after.
struct PhotoLayers {
  std::vector<Draw2d> pre;   // after the pass's colour clear, before the skater
  std::vector<Draw2d> post;  // after the skater
  float clear[4] = {};       // the colour clear, as the game's raw 8-bit value
  bool clear_valid = false;
};
// Reads a captured draw's vertex payload and normalizes it for the replay.
bool Normalize2dDraw(uint8_t* base, Draw2d& d);
inline std::vector<Draw2d> g_frame_2d;  // capture in submission order
inline std::vector<Draw2d> g_scene_2d;  // published at frame end
inline uint64_t g_scene_2d_generation = 0;

// One captured in-world neon spline draw (waypoint arrows / marker beams;
// decoded from a live capture + the game's own spline.fx source): a
// DrawVertices triangle strip whose 12-byte float3 "vertices" are only
// parameters: x = control-point index + fractional t, y = U texcoord,
// z = side flag (0/1) selecting the extrusion offset and V texcoord. The
// guest VS (43309A8C) evaluates a uniform cubic B-spline through i_cp[]
// (VS c7..) transformed by world columns c4..c6, adds the world-rotated
// extrusion offset c[151 + z], projects by VP columns c0..c3, and fades by
// clip-z against i_clipvalues (c150) with i_intensity (c149) as the pass
// gain. Two passes per element: spline_darkenPS (straight alpha) then
// spline_defaultPS (additive glow), both depth-tested with no z-write,
// replayed inside the native scene pass with the spline evaluated on the
// CPU at publish time.
struct SplineDraw {
  uint32_t pass;      // 1 = darken (straight alpha), 2 = default (additive)
  uint32_t count;     // strip vertex count
  uint32_t fetch[6];  // texture fetch slot 3 (the neon gradient)
  // VS c0..c200 as staged: the control points live at c[7+a0] (a0 <= 141,
  // so c151 at most), the colour at c199 and the near/far clip values at
  // c200 -- this build's spline VS reads both of those high rows.
  float consts[201 * 4];
  // capture: raw big-endian float3 params; publish: evaluated little-endian
  // {float4 clip_pos, float2 uv, float fade} (28-byte stride).
  std::vector<uint8_t> verts;
};
inline std::vector<SplineDraw> g_frame_spline;  // guarded by g_2d_mutex
inline std::vector<SplineDraw> g_scene_spline;
// Sk8::Render::SimpleDraw debug primitives (simpledraw_* shaders), e.g. the
// render skeleton SkaterPresentation::DebugDraw draws bone by bone with
// SimpleDraw::Line when cv_ShowRenderSkeleton is on. The game writes the
// vertices inline (D3DDevice_BeginVertices / BeginIndexedVertices) after the
// call returns, so the capture keeps the ring pointers and the payload is
// read at frame end (PublishSimpleDraws). VS (simpledraw_p_*):
//   world = (c4 . p, c5 . p, c6 . p, c7 . p); clip = c0..c3 . world
//   colour = the vertex's float4 (p_c, at +16) or the constant c8 (p_sc,
//   p_n_sc: its Lambert term is approximated by the flat colour).
struct SimpleDrawCap {
  uint32_t prim = 0;     // D3DPRIMITIVETYPE (2 lines, 3 strip, 4 tris, ...)
  uint32_t vcount = 0;
  uint32_t stride = 0;
  uint32_t vaddr = 0;    // guest vertex pointer (written after the call)
  uint32_t icount = 0;   // 0 = non-indexed
  uint32_t iaddr = 0;
  uint32_t layout = 0;   // 1 = p_sc / p_n_sc (colour c8), 2 = p_c (per vertex)
  float consts[10 * 4] = {};  // VS c0..c9
};
inline std::vector<SimpleDrawCap> g_frame_sdraw;  // guarded by g_2d_mutex
// The last published SimpleDraw triangle list (FrameScene::sdraw_verts) and
// the guest frames since it last held a capture: a frame without one keeps
// it briefly instead of blinking it out. Guest render thread only.
inline std::vector<SimpleDrawVert> g_last_sdraw;
inline uint32_t g_sdraw_idle_frames = 0;
inline std::atomic<uint64_t> g_draws_spline{0};
// Current guest shader objects and render-state shadow bank, for the 2D
// recon recording (grouping the stream by shader / reading blend state).
inline std::atomic<uint32_t> g_cur_ps_obj{0};
inline std::atomic<uint32_t> g_cur_vs_obj{0};
inline std::atomic<uint32_t> g_rs_bank{0};
// The device's register state arrays behind D3D::SetPending_RenderStates:
// for bank B at ptr, register B + i lives at ptr + i * 4 (the upload walks
// the 64-bit dirty mask from the top bit, value = ptr[clz]). Latched per
// bank so the 2D capture reads the CURRENT RB_BLENDCONTROL0 (0x2201) and
// RB_COLOR_MASK (0x2104) at each draw.
inline std::atomic<uint32_t> g_rs_ptr_2100{0};
inline std::atomic<uint32_t> g_rs_ptr_2200{0};
inline std::atomic<uint32_t> g_cur_viewport[6];
inline std::atomic<uint32_t> g_cur_scissor[4];
// All four vertex streams (vb, offset, stride); cloth binds its simulated
// vertices on a stream other than 0.
inline std::atomic<uint32_t> g_cur_streams[4][3];

// Offline-analysis recording structs/state: native/skate3_native_diag.h.
inline std::atomic<uint64_t> g_vs_uploads{0};
inline std::atomic<uint64_t> g_palette_snapshots{0};
// Palettes captured at base+1 (the cloth/morph VS layout with an extra
// parameter row before the palette, see RefinePaletteBase).
inline std::atomic<uint64_t> g_palette_base_plus1{0};
// Reflective-glass (fam 5/6) normal-map pair telemetry: pair = draws with
// the masks+normal t4/t5 descriptor pair bound (overlay.w == 4), flat =
// spec-bound reflective draws still on the flat-normal path, gate = the
// last no-pair reason bitmask (1 no spec masks, 2 no normal channel, 4
// masks SRV recipe missing, 8 normal unresolved/white, 16 normal invalid,
// 32 normal recipe missing, 64 pair slots exhausted).
inline std::atomic<uint64_t> g_refl_pair{0};
inline std::atomic<uint64_t> g_refl_flat{0};
inline std::atomic<uint32_t> g_refl_gate{0};

// ---- F7 scene-composition ring (see RequestSceneRingDump in the header) ----
struct SceneRingItem {
  uint32_t ctx;
  uint32_t mesh;
  uint32_t diffuse;
  uint32_t lightmap;
  uint32_t vb_obj;
  // Full material texture set: a one-frame spec/normal/macro/decal-art
  // resolution glitch changes shading (e.g. an unmasked sky reflection on a
  // reflective bank face) with the diffuse/lightmap identical; the first
  // ring revision could not see those.
  uint32_t spec;
  uint32_t macro;
  uint32_t decal_art;
  uint32_t wnormal;
  uint32_t indices;      // summed index counts of the item's draw entries
  uint16_t drawn;        // draw calls actually issued (0 = skipped at draw)
  uint8_t env_family;
  uint8_t char_family;
  uint8_t flags;  // 1 transparent 2 water 4 skinned 8 retained 16 pending
                  // 32 caster_bank 64 decal
  uint8_t route;  // 0 skipped pre-pass, 1 opaque pass, 2 blended sub-pass
  // SERVED-content fingerprints, stamped in draw_item after the resolves:
  // the object pointers above cannot see an in-place content swap (a heal
  // commit changes what a texture shows with every pointer identical);
  // a fp that changes A->B->A across the flash frame names the texture,
  // the slot, and both contents. 0 = white fallback / not resolved.
  uint64_t fp_diffuse;
  uint64_t fp_lightmap;
  uint64_t fp_macro;
  uint64_t fp_decal;
};
struct SceneRingFrame {
  uint64_t frame = 0;
  float cam[3] = {};
  // Per-frame captured globals: a one-frame glitch in any of these shifts
  // shading on every consumer with the composition identical.
  float fog[6] = {};        // ramp xyz + color rgb
  float family_rows[4] = {};
  float sky_height = 0.0f;
  bool shadow_valid = false;
  // Shadow-atlas outcome for the frame: a character shadow blinking off
  // for one frame with the item composition identical lives in this pass,
  // and nothing else in the ring could see it.
  bool shadow_ready = false;
  uint16_t shadow_draws = 0;
  bool static_sun_valid = false;
  // The frame's captured cascade rows: receivers project through these, so
  // a single-frame foreign capture moves every dynamic shadow off-target
  // with the composition and the atlas identical. They vary legitimately
  // with the camera, so only a recorded per-frame trace can tell an
  // outlier from drift.
  float shadow_rows[48] = {};
  std::vector<SceneRingItem> items;
};
inline std::deque<SceneRingFrame> g_scene_ring;  // render thread only
constexpr size_t kSceneRingFrames = 2400;
inline std::atomic<bool> g_scene_ring_dump{false};

// Render thread, frame end: write the whole ring as CSV. One F line per
// frame, then one I line per item (hex object addresses to match every
// other diagnostic).
inline void MaybeDumpSceneRing() {
  if (!g_scene_ring_dump.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  char path[128];
  std::snprintf(path, sizeof(path), "logs/scene_ring_%lld.csv",
                static_cast<long long>(std::time(nullptr)));
  std::ofstream f(path);
  if (!f) {
    REXLOG_WARN("native-scene ring: cannot open {}", path);
    return;
  }
  f << "kind,frame,ctx,mesh,diffuse,lightmap,vb,spec,macro,decal_art,"
       "wnormal,indices,drawn,env_fam,char_fam,flags,route,fp_diffuse,"
       "fp_lightmap,fp_macro,fp_decal\n";
  char line[256];
  for (const SceneRingFrame& fr : g_scene_ring) {
    std::snprintf(line, sizeof(line),
                  "F,%llu,cam,%.2f,%.2f,%.2f,items,%zu,fog,%.5f,%.4f,"
                  "%.3f,%.4f,%.4f,%.4f,fam,%.4f,%.4f,%.4f,%.4f,sky,%.1f,"
                  "shadow,%d,atlas,%d,%u,nsm,%d\n",
                  static_cast<unsigned long long>(fr.frame), double(fr.cam[0]),
                  double(fr.cam[1]), double(fr.cam[2]),
                  fr.items.size(), double(fr.fog[0]), double(fr.fog[1]),
                  double(fr.fog[2]), double(fr.fog[3]), double(fr.fog[4]),
                  double(fr.fog[5]), double(fr.family_rows[0]),
                  double(fr.family_rows[1]), double(fr.family_rows[2]),
                  double(fr.family_rows[3]), double(fr.sky_height),
                  fr.shadow_valid ? 1 : 0, fr.shadow_ready ? 1 : 0,
                  unsigned(fr.shadow_draws), fr.static_sun_valid ? 1 : 0);
    f << line;
    f << "S," << fr.frame;
    for (float v : fr.shadow_rows) {
      std::snprintf(line, sizeof(line), ",%.6g", double(v));
      f << line;
    }
    f << "\n";
    for (const SceneRingItem& it : fr.items) {
      std::snprintf(line, sizeof(line),
                    "I,%llu,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,"
                    "%u,%u,%u,%u,%u,%u,%016llX,%016llX,%016llX,%016llX\n",
                    static_cast<unsigned long long>(fr.frame), it.ctx, it.mesh,
                    it.diffuse, it.lightmap, it.vb_obj, it.spec, it.macro,
                    it.decal_art, it.wnormal, it.indices, unsigned(it.drawn),
                    unsigned(it.env_family), unsigned(it.char_family),
                    unsigned(it.flags), unsigned(it.route),
                    static_cast<unsigned long long>(it.fp_diffuse),
                    static_cast<unsigned long long>(it.fp_lightmap),
                    static_cast<unsigned long long>(it.fp_macro),
                    static_cast<unsigned long long>(it.fp_decal));
      f << line;
    }
  }
  REXLOG_INFO("native-scene ring: wrote {} frames -> {}", g_scene_ring.size(),
              path);
}
// Character-lighting capture telemetry: attempts vs validated captures per
// family (see CaptureCharLighting).
inline std::atomic<uint64_t> g_char_attempts{0};
inline std::atomic<uint64_t> g_char_valid{0};
inline std::atomic<uint64_t> g_char_drawn{0};
inline std::atomic<uint64_t> g_dynobj_drawn{0};
inline std::atomic<uint64_t> g_water_drawn{0};  // exact-branch water draws
// character.cloth_ropa items captured in the sim-active RIGID mode (world
// from c188/c191 instead of a bone palette, see CaptureSkinnedState).
inline std::atomic<uint64_t> g_ropa_rigid{0};
// ropa rigid captures REFUSED because the bank's c188/c191 was implausible
// or projected the garment off-clip (stale bank from another mesh's draw);
// the item stays pending for the post-draw fixup.
inline std::atomic<uint64_t> g_ropa_stale{0};
// ropa captures accepted through the RELAXED near-camera criterion (all
// samples in FRONT of the projection inside a loose 6x guard band + sane
// spread) after the strict half-in-clip score refused them. Up close the
// garment fills/overflows the screen and most of the 6 sample verts clip;
// the strict gate refused the CORRECT palette every frame, and with no
// cached resolved state to rescue from the torso stayed invisible until the
// NPC was far enough away again (log signature: `ropa mesh=... score=2
// flag=(1.000,...)` repeating while stale climbed with rescued flat).
inline std::atomic<uint64_t> g_ropa_relaxed{0};
// mesh -> newest enqueued cloth-shape generation (DynDecodeJob seq). GUEST
// render thread only: written by the dyn-job enqueue, read by the interp
// ring's pose ingestion (the pose <-> shape pairing).
inline std::unordered_map<uint32_t, uint64_t> g_ropa_last_seq;
// Shape blends served / skipped (generation missing from the ring) per
// stats window.
inline std::atomic<uint64_t> g_ropa_blend_drawn{0};
inline std::atomic<uint64_t> g_ropa_blend_miss{0};
// Ropa garments PUBLISHED with a caster-cascade (ortho) bank's palette:
// the ~40 ms-stale rows behind the garment-offset-from-body symptom. The
// same-mode graft in the dyn_slot merge should keep this at ~0 whenever a
// perspective capture exists in the same frame.
inline std::atomic<uint64_t> g_ropa_caster{0};
// Published skinned items whose palette FAILED the dense 32-sample
// publish-time coherence gate (see PublishedPaletteSane): the 1-frame
// junk-palette ribbon flash that passes every 6-sample capture gate. The
// item re-publishes its cached state instead (or drops for one frame).
inline std::atomic<uint64_t> g_pub_incoherent{0};
// Draw-time STRETCH VETO trips (see g_skin_probe): the final palette skins
// the resident decode's sample verts wider than bind size; the item's
// draws are cleared for the frame (blink, not ribbon) and the first events
// dump full palette + samples to logs/stretch_*.txt for offline diagnosis.
inline std::atomic<uint64_t> g_stretch_veto{0};
// skinned/ropa meshes that published after a 1-3 frame publish GAP (was on
// screen, vanished, came back): the "flickered invisible" telemetry.
inline std::atomic<uint64_t> g_dyn_gap{0};
inline std::atomic<uint64_t> g_skinned_items{0};
inline std::atomic<uint64_t> g_skinned_skipped{0};
// Rigid items whose transform had to wait for the post-draw fixup (deferred
// draws), and those still pending at frame end (dropped).
inline std::atomic<uint64_t> g_rigid_pending{0};
inline std::atomic<uint64_t> g_rigid_dropped{0};
// Pending rigid clones re-published with their cached world at merge time
// (see g_rigid_world_cache).
inline std::atomic<uint64_t> g_rigid_rescued{0};
// Rigid worlds served from the guest instance matrix (ReadCtxInstanceWorld)
// instead of a constant bank.
inline std::atomic<uint64_t> g_rigid_ctx_world{0};
// Sort-list piece copies published with the instance matrix (see
// g_dyn_dispatch_meshes).
inline std::atomic<uint64_t> g_sortlist_local_pub{0};
// Last published world per rigid dynamic context. Guest render thread only.
// Same-mesh piece clones (park-editor venues submit dozens) share (ib,vb),
// and their hook-time captures defer to the post-draw fixup whenever the
// venue batches dispatch (own_draw_last false); FIFO pairing there scrambles
// clone worlds whenever capture order and draw order diverge - pieces
// visibly swap placements and flip 180 degrees frame to frame. The fixup
// pairs each draw to the pending clone whose cached world matches instead,
// and clones whose draw never landed re-publish their cached world at merge.
struct RigidWorldCache {
  uint32_t mesh = 0;
  float world[16] = {};
  uint64_t frame = 0;
};
inline std::unordered_map<uint32_t, RigidWorldCache> g_rigid_world_cache;
// Character meshes detected as REBUILT this frame (a new buffer layout at a
// reused mesh address or instance ctx: create-a-skater swaps every piece on
// any outfit change). Filled by BuildFrameScene's rebuild check, drained by
// the pose-interpolation pass (its per-mesh rings are function-local).
// Guest render thread only.
inline std::vector<uint32_t> g_rebuilt_meshes;
// Same meshes for the render thread (FrameScene::rebuilt_meshes): moved into
// the next published scene. Guest render thread only.
inline std::vector<uint32_t> g_rebuilt_meshes_pub;
// Meshes seen on the per-entity dynamic dispatch (kind-0 captures), stamped
// with the guest frame of the last sighting. The game batches FAR instances
// of such pieces through the static sort lists instead of dispatching them
// per entity; their fmt-57 vertices are MESH-LOCAL, so the world-item
// path's identity world collapses them at the origin (the far-corner
// "teardown": one sub-mesh of each distant piece went invisible). A
// sort-list record whose mesh is fresh here publishes with the ctx owner's
// instance matrix instead. The freshness window doubles as map-change
// hygiene: a mesh address reused by a later load expires out on its own.
// Guest render thread only.
inline std::unordered_map<uint32_t, uint64_t> g_dyn_dispatch_meshes;
// Model-space props in the world sort lists, excluded from the identity
// world path (their placed copies come from the kind-2 capture).
inline std::atomic<uint64_t> g_world_props{0};
inline std::atomic<uint64_t> g_rej_no_dynstate{0};
inline std::atomic<uint64_t> g_rej_dyn_range{0};
// Render-side silent-skip diagnostics: a scene item that decodes badly or
// loses its bone binding produces no (or wrong) pixels with no other trace.
inline std::atomic<uint64_t> g_rr_decode_fail{0};
inline std::atomic<uint64_t> g_rr_no_bones{0};
// Decodes pushed past the per-frame budget (item/texture appears a few
// frames late instead of hitching the render thread).
inline std::atomic<uint64_t> g_rr_mesh_deferred{0};
inline std::atomic<uint64_t> g_rr_tex_deferred{0};
inline std::atomic<uint64_t> g_rej_chain{0};
inline std::atomic<uint64_t> g_rej_geom{0};
inline std::atomic<uint64_t> g_rej_draws{0};
inline std::atomic<uint64_t> g_rej_bbox{0};

// ---- Perf telemetry (windowed; logged + reset with the interval stats) ----
// Written by the guest render thread (capture/build) and the command
// processor thread (render segments); read by the render thread's stats log.
// Atomics with relaxed ordering: telemetry, not synchronization.
using PerfClock = std::chrono::steady_clock;
inline uint64_t PerfNsSince(PerfClock::time_point t0) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
          .count());
}
struct PerfWindow {
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint64_t> count{0};
  void Add(uint64_t v) {
    ns.fetch_add(v, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = max_ns.load(std::memory_order_relaxed);
    while (v > prev &&
           !max_ns.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
  }
  double AvgMs() const {
    const uint64_t c = count.load(std::memory_order_relaxed);
    return c != 0 ? double(ns.load(std::memory_order_relaxed)) / double(c) * 1e-6
                  : 0.0;
  }
  double MaxMs() const { return double(max_ns.load(std::memory_order_relaxed)) * 1e-6; }
  void Reset() {
    ns.store(0, std::memory_order_relaxed);
    max_ns.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
  }
};
// Guest frame counter (guest render thread only; incremented at
// BuildFrameScene entry): paces the world-item cache's revalidation.
inline uint64_t g_guest_frame = 0;

// Guest render thread: per-frame sums (one Add per guest frame).
inline PerfWindow g_pw_capture;   // hook-time capture work (sort lists + RenderMesh)
inline PerfWindow g_pw_build;     // BuildFrameScene
inline PerfWindow g_pw_pal_tail;  // frame tail: palette snapshot-ring rotation
inline PerfWindow g_pw_guest_dt;  // guest frame interval (guest fps + spike max)
inline uint64_t g_capture_frame_ns = 0;  // guest render thread only; folded per frame
// Command processor thread: per-RenderScene-call segments.
inline PerfWindow g_pw_render;       // whole native render
inline PerfWindow g_pw_items;        // opaque + transparent item loop (incl. decodes)
inline PerfWindow g_pw_shadow;       // dynamic-shadow atlas pass
inline PerfWindow g_pw_mesh_decode;  // inline DecodeMesh calls (count = decodes)
inline PerfWindow g_pw_tex_decode;   // inline texture-ish decodes (cubes, HUD words)
inline PerfWindow g_pw_commit;       // PrewarmCommit batches (count = non-empty runs)
// RenderScene phase attribution (the sustained "render=13ms, items=0.7ms"
// Vulkan mystery): everything render= covers that items=/shadow=
// do not. pre = EnsurePipeline..prewarm+evict+retire before the scene pass;
// twod = the 2D/HUD overlay replay (incl. inline decodes + video planes);
// tail = MSAA resolve, outline, pfx chain, photo grab, popup blur, final
// transitions: RenderScene minus everything else.
inline PerfWindow g_pw_pre;
inline PerfWindow g_pw_2d;
inline PerfWindow g_pw_tail;
// Settle-pass attribution (the sticky ~80fps post-map-change state:
// pre=8.4ms == the whole warmup budget, GPU idle at
// P0): the takeover settle pass extends itself +8 frames whenever a frame's
// warm walk leaves work deferred, so content that keeps failing revalidation
// (old-map items over reused arenas, streamed payloads) can hold it open for
// the rest of the session, burning the full budget every frame. settle= times
// the block; wdec/wdef count warm-path decodes/deferrals; the offender logs
// name what churns.
inline PerfWindow g_pw_settle;
// Guest-thread build-phase attribution (the standing-still 320->250 fps
// oscillation): build= runs on the guest render thread and its
// variance IS guest frame-time variance. b2d/bspl/bpal split the phases;
// the item walk is the remainder. dt histogram + slow-frame log quantify
// and attribute the dips.
inline PerfWindow g_pw_b2d;   // Publish2dDraws
inline PerfWindow g_pw_bspl;  // PublishSplineDraws
inline PerfWindow g_pw_bpal;  // ServeAuthoritativePalettes
// This frame's phase costs (guest render thread only - no atomics needed).
inline uint64_t g_frame_b2d_ns = 0;
inline uint64_t g_frame_bspl_ns = 0;
inline uint64_t g_frame_bpal_ns = 0;
inline uint64_t g_frame_pal_tail_ns = 0;
inline std::atomic<uint32_t> g_dt_hist[5] = {};  // <3.6 / <4.5 / <6 / <10 / >=10 ms
inline std::atomic<uint32_t> g_slow_frame_log_budget{0};
inline std::atomic<uint64_t> g_warm_decodes{0};
inline std::atomic<uint64_t> g_warm_deferred{0};
inline std::atomic<uint32_t> g_warm_mesh_log_budget{0};
inline std::atomic<uint32_t> g_warm_tex_log_budget{0};
inline uint64_t g_takeover_frame = 0;
// Camera-update cadence (the "world judders while panning at 320 fps"
// question): if the guest updates its camera on a fixed sim tick while the
// render loop free-runs, view_proj repeats for several rendered frames and
// then steps, visible as the surroundings skipping to catch up. Counted per
// published scene on the guest render thread.
inline std::atomic<uint64_t> g_cam_changes{0};
inline std::atomic<uint64_t> g_cam_repeats{0};
inline std::atomic<uint64_t> g_cam_max_streak{0};

// True while the in-game browser (EAWebKit, FE screen 60) is on the front-end
// stack; its page quad is a bare SimpleDraw draw, which Publish2dDraws would
// otherwise drop as offscreen composition. Updated per frame by
// UpdateBrowserScreenState() on the render thread.
inline std::atomic<bool> g_browser_screen_open{false};

// Native -> emulated F5 hand-off: switching the renderer off restores the
// game's 16:9 camera aspect (skate3_guest_aspect) at once, but the game keeps
// rendering frames already built with the WIDE projection for a moment; the
// emulated output would show them squeezed. Until the published camera is
// back to 16:9 (checked in RenderScene) or this deadline passes, capture and
// the native renderer keep running (the native frame widens a 16:9 camera
// itself), then the emulated output takes over with a correct projection.
inline std::atomic<int64_t> g_scene_handoff_until_ms{0};
inline int64_t SceneHandoffNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline bool SceneHandoffActive() {
  const int64_t until = g_scene_handoff_until_ms.load(std::memory_order_relaxed);
  return until != 0 && SceneHandoffNowMs() < until;
}
inline bool SceneEnabled() {
  return REXCVAR_GET(skate3_native_render_scene) || SceneHandoffActive();
}

// ---- Photo / movie / CAS-editor state shared with the render thread ----

// drew_inside captures whose bank provably belonged to another mesh's draw
// (now deferred to the post-draw fixup instead of consumed).
inline std::atomic<uint64_t> g_capture_foreign_bank{0};

inline std::atomic<uint64_t> g_item_cache_hits{0};
inline std::atomic<uint64_t> g_item_cache_builds{0};

// ---- Per-item pipeline profiling (skate3_native_render_scene_perf_items) --
// Deep attribution of the per-item CPU cost, logged as a perf-items line on
// the perf-log interval. Two questions: where the per-item time goes (mesh
// serve, texture serve, constants, submit on the render side; cache
// validate, fingerprint, full walk, fetch overrides on the build side), and
// whether it is spent on items the rendered frustum can actually see (in
// vs out draw windows, indices, retained-item share). All of it is opt-in:
// the extra clock reads cost real time at 2000+ items/frame.
// Render thread (command processor): per-draw_item totals split by the
// item's bbox-vs-frustum verdict, and per-stage sections of completed draws.
inline PerfWindow g_pw_di_in;      // draw_item total, item potentially visible
inline PerfWindow g_pw_di_occ;     // draw_item total, in-frustum but occlusion-grid-proven hidden
inline PerfWindow g_pw_di_out;     // draw_item total, bbox fully outside the rendered frustum
inline PerfWindow g_pw_di_mesh;    // mesh store lookup/heal (incl. rare inline decodes)
inline PerfWindow g_pw_di_tex;     // texture route/serve section
inline PerfWindow g_pw_di_const;   // constants assembly
inline PerfWindow g_pw_di_submit;  // buffer binds + DrawIndexed recording
inline std::atomic<uint64_t> g_vis_in_indices{0};   // indices submitted, visible items
inline std::atomic<uint64_t> g_vis_occ_indices{0};  // indices submitted, occluded items
inline std::atomic<uint64_t> g_vis_out_indices{0};  // indices submitted, out-of-frustum items
inline std::atomic<uint64_t> g_vis_out_retained{0};  // out-of-frustum draws from retention
// Session-cumulative count of statics skipped by the occlusion cull (the
// skate3_native_render_scene_occlusion_cull cvar; reported on the frame
// stats line like the other cumulative counters there).
inline std::atomic<uint64_t> g_occl_culled{0};
// Session-cumulative count of sort-list entries the guest-side dispatch
// filter removed before the guest's draw-list renderer ran (see
// skate3_native_render_scene_occlusion_cull_guest).
inline std::atomic<uint64_t> g_occl_guest_skipped{0};
// Session-cumulative count of item rebuilds the build-side occlusion skip
// left out (see skate3_native_render_scene_occlusion_cull_build).
inline std::atomic<uint64_t> g_occl_build_skipped{0};
// Guest render thread: decomposition of the per-item build walk.
inline PerfWindow g_pw_bi_core;    // BuildItemFromMeshCached total (hit validate + misses)
inline PerfWindow g_pw_bi_fp;      // ComputeItemFingerprint on the cache-hit path
inline PerfWindow g_pw_bi_walk;    // full BuildItemFromMesh rebuilds (cache miss)
inline PerfWindow g_pw_bi_fetch;   // AdoptDrawFetchOverrides per-frame guest reads
// Whole submit-record loop of BuildFrameScene (one Add per frame). Contains
// bi_core/bi_fetch plus everything they do not: per-record hash-set
// bookkeeping, the dynamic merge, draw-list copies and DrawItem churn.
// build - b2d - bspl - wloop - bpal - retain - remainder = the post-loop
// tail (camera smoothing, capture blocks, publish).
inline PerfWindow g_pw_bi_wloop;
inline std::atomic<uint64_t> g_bi_records{0};  // submit records per window
// Off-screen retention pass (always measured, one Add per frame: two clock
// reads): its cost and how many items it re-appends beyond the guest's own
// submission are core visibility-culling telemetry, not deep profiling.
inline PerfWindow g_pw_bi_retain;
inline std::atomic<uint64_t> g_retained_appended{0};  // re-appended items this window
inline std::atomic<uint64_t> g_retained_live{0};      // retained-map size after the sweep

// ---- Photo-editor postfx capture (FrameScene::PhotoFx / photo_fx.hlsl) ----
// While a photo flow is active (g_photo_flow_frame, armed by
// UpdatePhotoGrabWindow), the OnDrawDone hook snapshots each postfx pass's
// PS/VS constant rows (g_ps_bank/g_vs_bank, the game's staging banks) plus
// the device fetch-constant shadow (device+0x480) at DRAW time, the same
// reads the F11 per-draw records use, verified correct for every postfx
// pass in captures 1783957072 and 1783963636. Upload-time (SetPending)
// capture was removed: the game stages the next pass's rows while the
// previous pass's shader is still bound, so upload-time snapshots mix
// neighbouring passes' constants.
enum PfxPass {
  kPfxVisualFx = 0,
  kPfxDofDown,
  kPfxDofMB,
  kPfxDof,
  kPfxUber,
  kPfxFisheye,
  kPfxPassCount
};

inline std::atomic<bool> g_photo_flow_frame{false};

// Photo display-card sighting stamp (see the photo-grab helpers for the
// full story): stamped by Publish2dDraws on the guest thread whenever the
// card-signature quad is in the 2D stream; read by YieldForPhotoDisplay.
inline std::atomic<int64_t> g_photo_card_seen_ns{-1};

// Last ScreenshotBackEnd grab REQUEST (PerfClock ns): the game arms a grab
// 1-2 frames before it renders the shot + card-composite frame sequence and
// CPU-reads the resolves (GrabScreenshot -> OnScreenShot compose). The
// shutter burst (UpdatePhotoGrabWindow) keys on this: the ONLY moment the
// card-build's inputs can be made real under the native path.
inline std::atomic<int64_t> g_photo_grab_request_ns{-1};

// GrabScreenshot returned: the grab CPU-read the resolved shot, and the
// OnScreenShot card compose that follows (same guest thread, same Update)
// only reads memory the burst already copied. The burst closes here:
// event-driven, no timers (a short grace in the window logic covers the
// aux-target grab some flows issue on the following frame).
inline std::atomic<int64_t> g_photo_grab_done_ns{-1};

// Last PhotoReplayController::Update heartbeat, nanoseconds on PerfClock
// (steady). -1 until the first heartbeat. Written from the guest thread,
// read by RenderScene on the render thread.
inline std::atomic<int64_t> g_photo_replay_last_ns{-1};

// Last FrontEndState_Replay2::TakePhoto heartbeat: a photo was just taken
// (replay editor or photo mission). The frames that follow render and
// CPU-grab the 1152x640 screenshot target, so the forced-readback window
// (UpdatePhotoGrabWindow) stays armed for a few seconds after it. Same
// thread contract as the photo-replay heartbeat above.
inline std::atomic<int64_t> g_take_photo_last_ns{-1};

// Last rw::movie::MovieDecoder::Decode heartbeat (FMV playback: intro
// logos, any full-motion video). Same contract as the photo-replay
// heartbeat above.
inline std::atomic<int64_t> g_movie_decode_last_ns{-1};

// Last frame the 2D replay actually SUBSTITUTED a movie quad (ps_yuv2d).
// Consulted by YieldForMovie: while the native substitution is serving the
// video, the emulated yield must never engage; the emulated framebuffer
// is STALE under draw suppression, and presenting it flashed the previous
// video's last frame at every video boundary.
inline std::atomic<int64_t> g_movie_native_last_ns{-1};

// Last frame the 2D replay CONTAINED a video quad at all (substituted or
// not; detection runs regardless of movie_sub). Consulted by YieldForMovie:
// with no quad on screen there is nothing a yield could display - a
// skipped/force-completed video (intro skip) leaves the decoder heartbeat
// alive ~0.5 s with no quad anywhere, and yielding then just flashed the
// stale emulated buffer (the "momentary emulated switch" at intro skip).
inline std::atomic<int64_t> g_movie_quad_last_ns{-1};

// YUV plane fetch words of the FMV frames handed to
// VideoRenderer_RwTexture::Render (Y full res, U/V half res, CPU-filled via
// Texture::Lock). Multiple videos play at once (the camera-angle select
// page runs TWO preview movies), so this is a small table keyed by the Y
// plane's fetch-words hash; the 2D replay matches each captured draw's own
// slot-0 fetch against it; a video quad samples its Y plane there (the
// camera-page previews rendered as slow GREYSCALE through the plain 2D
// shader: exactly that luma). Movie threads publish under the mutex;
// RenderScene copies per frame.
struct MoviePlanes {
  // Y plane identity = the fetch constant's ADDRESS dword (words[0][1]).
  // NOT a full-words hash: the words staged into the device shadow at draw
  // time carry per-draw sampling flags that differ from the texture
  // object's stored constant (observed: both camera-page videos
  // published, exact-words match still 0 hits): the address dword is the
  // physical identity both sides agree on.
  uint32_t y_addr = 0;
  uint32_t words[3][6];  // Y, U, V fetch constants
  int64_t ns = -1;
};
constexpr int kMaxMovies = 4;
inline std::mutex g_movie_mutex;
inline MoviePlanes g_movies[kMaxMovies];

// Heartbeat of the create-a-skater editor's OWN pixel shaders: the editor's
// in-view skater draws use "_nis" compiles (cacstamp_skin_nisPS,
// cac_cloth_nisPS, cac_face_nisPS, cac_hair_nisPS, defaultcharacter_nisPS,
// cacstamp_shift_nisPS) that exist nowhere else in the game; a fresh
// sighting means the CAS editor is on screen NOW. Backs the FE-stack id-15
// detection in YieldForCasEditor: the startup new-game flow reaches the
// editor through a different FE screen (observed: the indicator stayed
// NATIVE there). Deliberately NOT matched: the "_unwrap" texture-space
// composite variants; outfit composition also runs mid-gameplay after an
// outfit change and must never yield a gameplay frame.
inline std::atomic<int64_t> g_cas_ps_last_ns{-1};

// Capture-side helpers the render thread calls (defined in
// skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
void ClearItemCache();
bool CasEditorActive(uint8_t* base);
bool DifficultyScreenActive();
bool PortraitRttWindowActive();
void UpdateBrowserScreenState();
// Parse a guest mesh struct into a DrawItem (prewarm decode workers build
// items off the queue without a live capture).
bool BuildItemFromMesh(uint8_t* base, uint32_t mesh, DrawItem& item);

// Render-side helper the capture thread calls (defined in
// skate3_native_scene_gpu.cpp): photo-editor detection shared by the yield
// and the photo-grab readback window: nullptr when inactive, else the name
// of the active signal.
const char* PhotoEditorSignal(uint8_t* base);

}  // namespace skate3::native_scene
