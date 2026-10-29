// Native scene renderer, game-thread half: captures the hooked MeshContext
// stream and guest render state, and builds/publishes the per-frame
// FrameScene. The render-thread RHI half lives in
// skate3_native_scene_gpu.cpp; state shared between the two is in
// skate3_native_scene_state.h.

#include "skate3_vr.h"
#include "skate3_native_font_atlas.h"
#include "skate3_deck_shadow.h"
#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_flat_map.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
// Offline-compiled SPIR-V for the native shaders (compiled from the HLSL
// sources with DXC): the Vulkan RHI backend consumes these blobs; the D3D12
// backend runtime-compiles the embedded HLSL as before.
#include "native/shaders/spirv/skate3_native_shaders_spirv.h"
#include "skate3_native_scene_state.h"

#if (defined(REX_HAS_D3D12) && REX_HAS_D3D12) || (defined(REX_HAS_VULKAN) && REX_HAS_VULKAN)
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#if defined(_WIN32)
#include <windows.h>
#include <cctype>
#include <string>
#include <vector>
#include <set>
#endif
#endif

REXCVAR_DECLARE(std::string, skate3_native_render_snapshot_dir);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene, true, "Skate 3",
                    "Render the game scene natively from the hooked MeshContext stream, "
                    "replacing the emulated GPU output (requires skate3_native_render). "
                    "Hot-toggles live between the native and emulated renderers (F5).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// The per-draw pass census + pass ring (DIAGNOSTIC, see DumpRecentPasses):
// a mutex, a set insert and a ring record on every draw of the guest render
// thread. Opt-in; Release compiles it out entirely.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_pass_census, false, "Skate 3",
                    "Record the per-draw pass census and pass ring (diagnostic)");
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_draw_fetch_adopt, true, "Skate 3",
                    "Adopt the texture bound at draw-time fetch slot 4 as the item's "
                    "diffuse (the Massive event-ad artwork / player-photo billboard "
                    "rebind). The s3/s4 sampler assignments this relies on are this "
                    "retail build's own shader compile (sk83_na_m turns it off: its dev "
                    "shaders put other maps at s4). On by default here; the retail "
                    "renderer always adopted it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lightmaps, true, "Skate 3",
                    "Sample guest lightmap textures in the native scene renderer. The "
                    "old 'lightpages decode black' finding is stale: in gameplay the "
                    "composed atlas payloads are CPU-readable (offline-validated: "
                    "lightmap x2 at the |uv2| unwrap reproduces the emulated baked "
                    "sun/shadow/AO structure), and the texture payload revalidation "
                    "re-decodes pages that were captured before composition.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_menu_blur_sigma, 9.0, "Skate 3",
                      "Settings-menu backdrop gaussian sigma, in 1080p pixels (matches "
                      "the menu design's blur radius; scales with output "
                      "resolution). Only used while the host settings overlay is open.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(skate3_native_render_scene_debug, 0, "Skate 3",
                     "Native scene debug: 0=normal, 1=clear only, 2=solid color per item, "
                     "3=limit to 20 items, 4=depth test disabled")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_msaa, 4, "Skate 3",
                     "MSAA sample count for the native scene (1 = off, 2/4/8). Distant "
                     "thin geometry (railings, wires) shimmers without it; mipmaps only "
                     "fix texture aliasing. Applies live: the scene pipeline family and "
                     "MSAA targets rebuild on change.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_world_v2_tan_sign, 1.0, "Skate 3",
                      "World-shading v2: polarity relating the stored frame's "
                      "cross(binormal, normal) x handedness to the game's tangent "
                      "(+1/-1). Calibrated against the emulated reference; flips the "
                      "kd sun-direction response if wrong.")
    .range(-1.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_world_v2, true, "Skate 3",
                    "World-shading v2: per-pixel normal/detail/spec maps on the exact "
                    "environment families (the game's own GetTangentLight kd + phong "
                    "terms; v1 folded them to the flat-map constants). Off = the v1 "
                    "flat response, for A/B comparison.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynobj_v2, true, "Skate 3",
                    "Dynamicobject shading v2: per-pixel normal/detail/spec maps on the "
                    "exact dynamicobject families (mapped world normal, the game's "
                    "GetTangentLight kd and both phong spec variants; v1 folded them "
                    "to the flat-map constants). Off = the v1 flat response, for A/B "
                    "comparison.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao, false, "Skate 3",
                    "Screen-space ambient occlusion in the native renderer: soft "
                    "contact shading where surfaces meet (under ledges, rails, vehicles, "
                    "the skater). A post pass over the resolved scene depth, an "
                    "enhancement beyond the original game's shading. Technique picked by "
                    "skate3_native_render_scene_ao_technique.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ao_technique, 0, "Skate 3",
                     "Ambient occlusion technique: 0 = GTAO (horizon-angle integral, "
                     "per-pixel noise, optional half-res -- see ssao.hlsl). 1 = HBAO "
                     "(interleaved-rendering horizon march with a simpler per-sample "
                     "falloff, always full-res -- a clean-room implementation of the "
                     "published HBAO+ technique description; see hbao.hlsl). Both share "
                     "the radius/intensity/luma-protect tuning cvars below and the "
                     "same blur/composite tail.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_radius, 0.8, "Skate 3",
                      "SSAO sample radius in world units. Larger radii darken bigger "
                      "cavities but read as wide soft pools around characters; small "
                      "radii keep it to contact shading.")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_luma_protect, 1.2, "Skate 3",
                      "How strongly bright (sun-lit) surfaces resist SSAO darkening, "
                      "a post-tonemap approximation of ambient-only occlusion (direct "
                      "sunlight is not occluded by nearby geometry). 0 = AO applies "
                      "uniformly regardless of lighting.")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssao_intensity, 1.4, "Skate 3",
                      "SSAO strength: exponent on the linear visibility term (0 = "
                      "invisible, 1 = physical, >1 = accentuated).")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao_full_res, false, "Skate 3",
                    "Evaluate SSAO at full output resolution instead of half. Roughly "
                    "4x the GPU cost for a subtle sharpening of the smallest contact "
                    "shadows; the depth-aware blur hides most of the difference.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssao_debug, false, "Skate 3",
                    "Replace the frame with the SSAO visibility term (tuning aid).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ssr, false, "Skate 3",
                    "Screen-space reflections on reflective glass and water: "
                    "on-screen scenery reflected in real time where the original "
                    "game shows only a static environment cube. Rays that miss "
                    "(off-screen content, screen edges) fade back to the cube "
                    "term. Requires the HDR scene intermediate. EXPERIMENTAL: "
                    "off by default pending image-quality calibration (residual "
                    "smearing/noise on some surfaces).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ssr_steps, 48, "Skate 3",
                     "SSR screen-space march taps per ray (half-res pass). More "
                     "taps resolve thinner reflected geometry at proportional "
                     "GPU cost.")
    .range(8, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssr_thickness, 1.0, "Skate 3",
                      "SSR depth thickness (view units): how deep behind a "
                      "visible surface a marching ray still counts as a hit. "
                      "Too small and rays tunnel through thin ledges/rails; too "
                      "large and reflections smear under overhangs.")
    .range(0.05, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_ssr_intensity, 1.0, "Skate 3",
                      "SSR composite strength over the material's cube "
                      "reflection term (0 = effectively off).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ssr_debug, 0, "Skate 3",
                     "SSR debug view: 1 = raw ray color + confidence, 2 = "
                     "reflection G-buffer normals, 3 = reflectivity (red) x "
                     "depth visibility (green), 4 = ray autopsy (green = "
                     "visibility reject, blue = degenerate, cyan = off-screen, "
                     "magenta = near plane, yellow = out of steps, red = "
                     "too-thick crossings, white = hit x confidence).")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_hdr, false, "Skate 3",
                    "Render the 3D scene into a float (HDR) intermediate and apply the "
                    "game's shared tone chain once in a host post pass, the basis for "
                    "real bloom (and later HDR effects). With bloom off the output is "
                    "equivalent to the classic in-material tonemap. Off = the classic "
                    "path, for A/B parity checks.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_bloom, true, "Skate 3",
                    "Real bloom from the HDR scene: a downsample/upsample pyramid driven "
                    "by pre-tonemap brightness (night lamps, neon, the sun and sky "
                    "glow). Requires the HDR intermediate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_threshold, 0.9, "Skate 3",
                      "Bloom brightness threshold in pre-tonemap units (1.0 = the tone "
                      "curve's saturation point; 0.85 corresponds to ~97% display "
                      "white). Sunlit surfaces sit around 0.5-0.65 pre-tonemap; "
                      "thresholds that reach them veil the whole daytime frame in "
                      "glow; near-clipped lamp cores, neon, sky glare and the sun "
                      "stay above 0.85.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_knee, 0.08, "Skate 3",
                      "Bloom soft-knee width around the threshold (0 = hard cutoff).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bloom_intensity, 0.025, "Skate 3",
                      "Bloom strength: the pyramid's contribution added to the "
                      "pre-tonemap scene (0 = off; the pyramid accumulates ~5 levels, "
                      "so small values are already visible around bright lights).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_hdr_packed, true, "Skate 3",
                    "Use the packed R11G11B10 float format for the HDR scene targets "
                    "instead of RGBA16F: halves the scene-pass color bandwidth (most "
                    "of the HDR cost at high resolution + MSAA) at slightly lower "
                    "precision in very dark gradients.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_hdr_debug, 0, "Skate 3",
                     "HDR post debug: 0 = off, 1 = show the bloom term only, 2 = show "
                     "the raw pre-tonemap scene (clamped), 3 = show the fused SSAO "
                     "multiplier plane, 4 = show the sun-shaft plane only, 5 = show "
                     "the directional haze term only.")
    .range(0, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// OFF by default: this march is a native addition, not something the console
// does. The game ships its own shaft shaders (godray_defaultPS /
// godrayspotlight_defaultPS -- a different technique) and draws none of them in
// the captured scenes, so the authentic look carries no shafts of ours. Porting
// the game's own godray pass would be the faithful way to get shafts back.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shafts, false, "Skate 3",
                    "Volumetric sun shafts: the air along each view ray is marched "
                    "against the dynamic CSM atlas and the static world-shadow map, "
                    "and the SHADOWED portion proportionally dims the light seen "
                    "through it: dark crepuscular shafts cut by real shadow "
                    "volumes (buildings, trees, underpasses) into the game's own "
                    "sun glow. Fully lit air is untouched, so open scenes stay "
                    "exactly as authored. Requires the HDR intermediate and this "
                    "frame's shadow pass.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shafts_intensity, 0.6,
                      "Skate 3",
                      "Sun-shaft strength: how strongly fully shadowed air dims "
                      "the scene behind it, scaled by the forward-scatter phase "
                      "(at the default, fully shaded air dims ~30-50% at typical "
                      "sun-facing angles and saturates looking straight at the "
                      "sun; 0 = off).")
    .range(0.0, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shafts_reach, 40.0, "Skate 3",
                      "Sun-shaft march reach (world units): how far in front of "
                      "the camera the air is sampled for sun visibility. Longer "
                      "reaches pick up more distant shadow volumes at coarser "
                      "sampling for the same step count.")
    .range(5.0, 300.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shafts_steps, 64, "Skate 3",
                     "Sun-shaft march step ceiling per pixel; rays shorter than "
                     "the full reach take proportionally fewer steps at the same "
                     "per-meter density. More steps resolve thinner shadow "
                     "volumes at proportional GPU cost.")
    .range(8, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shafts_res, 4, "Skate 3",
                     "Sun-shaft march resolution divisor (2 = half res, 4 = "
                     "quarter res). The march output is low frequency (jittered "
                     "average under a tent blur) and upsamples depth-aware, so "
                     "higher divisors trade little quality for a proportional "
                     "GPU saving.")
    .range(2, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_sun_override, false, "Skate 3",
                    "Lighting lab: replace the captured sun direction with the "
                    "azimuth/elevation cvars. Every per-frame light transform is "
                    "rebuilt around the new direction, so the dynamic CSM shadows, "
                    "the static world-shadow map, material shadow receivers and "
                    "the volumetric shafts all follow the moved sun. Baked "
                    "lightmap shade and the sky dome's painted sun cannot move "
                    "(game content), a tuning tool, not a time-of-day system.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_sun_azimuth, 220.0, "Skate 3",
                      "Overridden sun azimuth in degrees (compass heading the sun "
                      "sits toward; 0 = +Z north, 90 = +X east).")
    .range(0.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_sun_elevation, 35.0, "Skate 3",
                      "Overridden sun elevation in degrees above the horizon (low "
                      "values = long shadows and the most visible shafts).")
    .range(2.0, 88.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_haze, true, "Skate 3",
                    "Directional atmospheric haze: sun-scattered light added with "
                    "view distance, strongest looking toward the sun, tinted by the "
                    "frame's own fog color (so it tracks time of day and fades at "
                    "night). Kept subtle; the game's authored sky and fog carry "
                    "the base atmosphere, and additive terms read as extra fog "
                    "quickly. Requires the HDR intermediate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_haze_intensity, 0.08,
                      "Skate 3",
                      "Directional haze strength (pre-tonemap scale of the "
                      "toward-the-sun scattering excess; 0 = off).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_haze_density, 0.005,
                      "Skate 3",
                      "Directional haze density per view unit: how quickly the "
                      "scattering saturates with distance (0.005 reaches ~63% at "
                      "200 units).")
    .range(0.0, 0.05)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_showcase, false, "Skate 3",
                    "Run the graphics build-up showcase: the screen snaps to "
                    "black, flat clay geometry wipes in, the scene is rebuilt "
                    "layer by layer (each layer revealed by a vertical split "
                    "wiping across the screen with both sides rendered live), "
                    "and the run closes by wiping back to black. The black "
                    "bookends double as cut markers for screen recordings. "
                    "Layer order and grouping come from "
                    "skate3_native_render_scene_showcase_order (edited in the F12 "
                    "showcase setup window). Clears itself when the sequence "
                    "finishes; set to false mid-run to cancel. Also on "
                    "Ctrl+Shift+B by default (bind_skate3_showcase). Layers whose feature "
                    "is disabled (or unavailable without the HDR intermediate) "
                    "are skipped.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    skate3_native_render_scene_showcase_order,
    skate3::native_scene::kShowcaseOrderDefault, "Skate 3",
    "Showcase layer order: comma-separated reveal steps, '+' joins layers "
    "into one wipe, a '-' prefix disables a layer while keeping its "
    "position. Tokens: albedo, lighting, materials, decals, dyn, shadows, "
    "ao, ssr, vol, bloom (the run always starts from clay geometry, and a "
    "final full-render step is appended when the list leaves layers "
    "unrevealed). decals (graffiti/paint art + macro weathering; renders on "
    "the full material look, so place it after materials) and dyn (dynamic "
    "entities: characters, props, cloth) HIDE their content until revealed; "
    "left out or disabled, that content renders from clay onward as usual. "
    "The CSM atlas is shared by both sides of the split, so a dyn step "
    "AFTER shadows shows entity shadows before their owners; order dyn "
    "first. Example: \"albedo,lighting,materials,decals,dyn,shadows+ao\".")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_showcase_hold, 2.5, "Skate 3",
                      "Showcase: seconds each build-up stage holds fullscreen "
                      "between wipes.")
    .range(0.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_showcase_wipe, 3.0, "Skate 3",
                      "Showcase: seconds each split wipe takes to cross the "
                      "screen.")
    .range(0.2, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_freecam, false, "Skate 3",
                    "Detach the render camera from the game (drone / free-fly "
                    "cam): WASD fly, E/Space up, Q/C down, arrow keys or "
                    "right-mouse drag look, Z/X zoom, Shift fast, Ctrl slow. "
                    "The game keeps simulating, but its main render camera is "
                    "taken over (ViewCamera::SetViewMatrix override), so the "
                    "game culls and submits the world around the flown pose "
                    "itself. Also on the End key (bind_skate3_freecam).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_freecam_speed, 8.0, "Skate 3",
                      "Freecam base fly speed in world units (meters) per "
                      "second. Shift = 4x, Ctrl = 0.2x.")
    .range(0.5, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_freecam_look_speed, 90.0,
                      "Skate 3",
                      "Freecam arrow-key look rate in degrees per second.")
    .range(10.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_freecam_capture_input, true,
                    "Skate 3",
                    "While the freecam is engaged, keep keyboard/controller "
                    "input away from the game so flying doesn't also steer "
                    "the skater. Turn off to fly and play at the same time.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_2d, true, "Skate 3",
                    "Replay the game's 2D/APT (Flash HUD) draws as a native overlay pass "
                    "on top of the 3D scene")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_simpledraw, true, "Skate 3",
                    "Replay the game's SimpleDraw debug primitives (simpledraw_* "
                    "shaders: e.g. the cv_ShowRenderSkeleton bone lines) in the "
                    "native scene")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_splines, true, "Skate 3",
                    "Replay the game's in-world neon spline draws (Death Race "
                    "gates, Follow the Leader ribbons, waypoint arrows, "
                    "marker beams; spline_darken/spline_default shaders) inside the "
                    "native scene pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_selection_outline, true, "Skate 3",
                    "Replay the park-editor / object-mover selected-object outline: the "
                    "game stencil-marks the selected object after the sky and a postfx "
                    "edge-detect adds the blue contour (postfx_edgedetectstencil port)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_quadlists, true, "Skate 3",
                    "Render the game's particle systems (lionREC_Render quad-list "
                    "DrawVertices draws) natively: sprite texture x vertex color, straight "
                    "alpha blend, depth test without z-write, exactly like the game's "
                    "lionREC_RenderPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_mesh_decode_budget, 0, "Skate 3",
                     "Max INLINE mesh decodes per rendered frame (0 = unlimited). Only "
                     "dynamic payloads (skinned/cloth/ropa, whose buffers change every "
                     "frame) decode inline on the render thread; static world meshes "
                     "and all textures decode asynchronously on the prewarm workers.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_warmup_budget_ms, 8, "Skate 3",
                     "Per-frame milliseconds of the post-takeover settle decode pass: "
                     "after a loading screen the native output takes over on the first "
                     "substantial post-load scene (the registration prewarm decoded the "
                     "world behind the load), and for a short window this budget mops "
                     "up whatever prewarm missed while the draw path's miss budgets "
                     "are clamped; leftovers render white/skip for a frame instead of "
                     "freezing. 0 disables the takeover gates + settle pass entirely "
                     "(legacy immediate behavior, stale-scene flash and all).")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_settle_max_frames, 900, "Skate 3",
                     "Hard cap on how far past takeover the settle pass may keep "
                     "extending itself (frames). Content that keeps failing "
                     "revalidation (streamed payloads, old-map items over reused "
                     "arenas after a map change) otherwise re-defers work every frame "
                     "and holds the settle pass - and its full per-frame budget - open "
                     "for the rest of the session (the sticky ~80fps state). Past the "
                     "cap the draw path's own miss budgets take over. 0 = uncapped")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_prewarm_budget_ms, 32, "Skate 3",
                     "Per-frame milliseconds spent decoding freshly REGISTERED world "
                     "meshes (AddRenderInstance hook) and their textures while the "
                     "loading screen is up; the heavy lifting happens behind the load, "
                     "so gameplay starts with hot caches and takeover is immediate. "
                     "The loading screen renders emulated at hundreds of fps, so even "
                     "32 ms/frame keeps it ~30 fps; map-change loads register most of "
                     "the world in their FINAL seconds, so the drain rate in that "
                     "window decides how much pop-in survives into gameplay. "
                     "0 disables registration prewarm.")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_photo_yield, true, "Skate 3",
                    "Yield to the emulated output while a photo-mission's photo editor "
                    "(the FE PhotoSelect screen) is up. The editor's depth of field / "
                    "saturation / brightness / contrast / lens vignette are the game's "
                    "own postfx chain, which native rendering suppresses; natively "
                    "the photo showed the raw scene and the effect controls did "
                    "nothing. The scene is frozen there, so emulated-path performance "
                    "is fine. Detected by polling the FrontEndManager NIS push-state "
                    "stack (plus the PhotoReplayController heartbeat as a backup).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_readback, true, "Skate 3",
    "While a photo flow is active (the photo-mission editor, or a few "
    "seconds after any TakePhoto), arm the SDK's forced small-resolve CPU "
    "readback (native_render_force_resolve_readback_max_length) and lift "
    "emulated-draw suppression. The game takes photos by CPU-reading a "
    "resolved 1152x640 PostFX screenshot target from guest memory "
    "(ScreenshotBackEnd::GrabScreenshot -> JPEG); photo missions keep the "
    "gameplay presence context so no readback path ever ran and the grabbed "
    "memory stayed all-zero; the final photo display and the saved photo "
    "were BLACK. Costs one synchronous readback per small resolve, photo-flow "
    "frames only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_grab_native, true, "Skate 3",
    "Fulfill the photo grab NATIVELY while a photo flow is active: keep "
    "emulated-draw suppression ON (the 40-50 fps editor cost was the "
    "unsuppressed emulated pipeline rendering the whole scene + postfx at "
    "scaled res every armed frame, proven by the kFast run: zero readback "
    "drains, same fps) and instead downsample the native output to the "
    "game's 1152x640 screenshot target each armed frame, read it back "
    "(double-buffered, no GPU drain) and CPU-tile it big-endian into guest "
    "memory at 0x04911000 where ScreenshotBackEnd::GrabScreenshot JPEG-"
    "encodes it. In the photo editor the native output IS the ported postfx "
    "chain result; in the plain TakePhoto window it is the plain frame, "
    "both the correct grab semantics. OFF = the emulated fallback: forced "
    "kFull resolve readbacks + suppression lifted for the whole window "
    "(skate3_native_render_scene_photo_readback). If native frames stop "
    "mid-window (mode toggle, unexpected yield), the window watchdog flips "
    "to the emulated fallback by itself.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_display_yield, false, "Skate 3",
    "Yield the photo DISPLAY-CARD screen to the emulated output while the "
    "framed card is up. OFF (default) since the shutter burst: the game "
    "CPU-composes the framed card into the card texture at the grab (the "
    "burst makes its inputs real), and the native 2D replay samples that "
    "same texture; the display screen renders fully native. Turn ON as a "
    "safety hatch if the display screen misrenders natively.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_native, true, "Skate 3",
    "Render the photo-mission photo editor NATIVELY, applying the game's "
    "own postfx chain (depth of field / saturation / brightness / contrast "
    "/ lens vignette) as exact ucode ports (photo_fx.hlsl: visualfx -> DOF "
    "downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye) driven "
    "by the LIVE constants the game stages for its own (suppressed) postfx "
    "draws each frame. Takes precedence over "
    "skate3_native_render_scene_photo_yield; if the pass captures are not "
    "yet fresh (first frames of the editor), the scene renders without the "
    "effects until they land. Known deltas: the motion-accumulation feed "
    "(see photo_native_accum), the bloom pyramid contribution (disabled in "
    "every editor capture), grade LUT served as identity, deck AO.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_photo_native_accum, 0, "Skate 3",
    "Source for the visualfx pass's quarter-res motion-accumulation input "
    "(the game feeds a jitter-accumulated buffer natively unmodeled): 0 = "
    "black, 1 = downsampled scene (pre-grade), 2 = downsampled final frame. "
    "Compare against the emulated editor (F11 pair) and keep the match.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_photo_native_debug, 0, "Skate 3",
    "Photo-editor native postfx DEBUG VIEW drawn instead of the chain's "
    "final output: 1 = the visualfx CoC map (DoF blur amount; black = in "
    "focus, grey/white = blurred), 2 = packed-depth reconstruction (red = "
    "near band saturate((1-d)*100), green/blue = d fraction ramps; a flat "
    "screen here means the native depth is not reaching the chain). 0 = "
    "off.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_pause_native, true, "Skate 3",
                    "Keep the NATIVE renderer active through the in-game pause menu "
                    "instead of yielding to the emulated output. Pause is told apart "
                    "from loading screens / the boot frontend by the world still "
                    "submitting perspective scenes while the presence context reads 0 "
                    "(loads and the frontend stop publishing within ~300 ms, which "
                    "falls back to the yield path and its cache clears). Native pause "
                    "also skips the pause-entry cache clears, so unpausing costs "
                    "nothing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_loading_native, true, "Skate 3",
                    "Render post-startup loading screens natively, black backdrop "
                    "plus the game's own captured 2D loading UI, instead of "
                    "yielding to the emulated output. The loading-screen "
                    "housekeeping (cache clears, registration prewarm, takeover "
                    "arming) is unchanged; only the presented pixels switch source. "
                    "The boot flow before the first gameplay stays emulated.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_boot_native, true, "Skate 3",
                    "Extend native rendering to the game startup flow (intro videos, "
                    "boot frontend, the first load); i.e. drop the first-gameplay "
                    "prerequisite from the native menu/loading modes, and render the "
                    "pre-takeover boot frames as native 2D-over-black instead of "
                    "yielding. With this and the pause/loading modes on, the emulated "
                    "GPU output is never presented.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_native, true, "Skate 3",
                    "Render FMVs natively: the movie player CPU-decodes VP6 into "
                    "three YUV plane textures (VideoRenderer_RwTexture members, "
                    "published per frame by the Render hook), and the captured "
                    "movie quad (the AptMovieIntegration stride-24 draw; through "
                    "the plain 2D shader it rendered as an opaque black cover, its "
                    "c8 is black) is substituted with the ps_yuv2d combine inside "
                    "the 2D replay, order-faithful and at the quad's own geometry "
                    "(windowed movies place exactly). When off (or the planes fail "
                    "to publish), FMVs fall back to the emulated yield "
                    "(skate3_native_render_scene_fmv_yield).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_yield, true, "Skate 3",
                    "Yield to the emulated output while an FMV is playing (intro "
                    "logos, any rw::movie playback). The video frame is CPU-decoded "
                    "into a texture and reaches the screen through the game's postfx "
                    "chain + swap without any capturable 2D draw (F11-proven on the "
                    "boot intro: 15 draws, all postfx passes + fade fills, none "
                    "sampling the video), so the native path has nothing to replay; "
                    "the emulated frame is complete and correct there, same class as "
                    "the photo editor. Detected via the MovieDecoder::Decode "
                    "heartbeat; native rendering resumes within 0.5 s of the last "
                    "decoded frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_cas_yield, false, "Skate 3",
                    "Yield to the emulated output while the create-a-skater editor "
                    "(the CAS 'Edit Skater' screen) is up. OFF by default since the "
                    "editor renders natively: the char-lighting capture understands "
                    "the editor CAC compiles' shifted constant layout (see "
                    "CaptureCharLighting's editor fam-2 retry), the texture-space "
                    "composite passes (cac*_unwrapPS, pitch <= 512) execute under "
                    "the pitch-selective suppression, and the ropa/palette VS layout "
                    "is unchanged. Known native deltas: the deck's shift-recolor "
                    "masks and the editor's own DOF chain are not modeled. Turn ON "
                    "to get the exact emulated editor instead (photo-editor class; "
                    "detected via FE push-state id 15 + the _nis shader heartbeat).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_rtt_passes, true, "Skate 3",
    "While a menu/pause/loading context (presence context 0) renders "
    "natively, drop native_render_suppress_mode to 0 (suppress "
    "framebuffer-sized passes ONLY) so the game's sub-framebuffer "
    "render-to-texture passes execute; the team-menu/Import-Skater skater "
    "portrait boxes are one-shot RTT passes at a surface pitch inside the "
    "mode-2 suppressed band (> 512, != 1024): under mode 2 they never ran "
    "and the boxes stayed empty. The visible frame stays fully native "
    "(framebuffer passes remain suppressed); this is the same "
    "execute-the-composition-passes-emulated class as lightmap pages and "
    "CAS outfit composition. Restored to the configured mode on the first "
    "gameplay frame. Menu-only cost: the midsize postfx-chain passes also "
    "execute there (mode 0 was the long-lived default before mode 2).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_menu_rtt_scope, 1, "Skate 3",
    "When menu_rtt_passes is on, WHEN to hold suppress_mode at 0 during "
    "menus. 0 = the whole menu context (pre-round behavior): the game's "
    "postfx chain then executes emulated at 1152x640 x resolution scale "
    "EVERY menu frame: under the pause menu that is the full world postfx, "
    "on FMV/title screens the movie postfx, pacing the pipeline and "
    "fighting the native renderer for the GPU (the menu sluggishness at a "
    "high fps counter). 1 = only inside a PORTRAIT WINDOW: the FE "
    "push-state stack contains a screen not known steady-safe (known: 0 = "
    "FE root, 56 = pause root, 24 = FMV, 17 = pause challenge map), OR an "
    "unsafe screen was entered/left within the last 3 s (one-shot portrait "
    "RTT renders fire at those transitions), OR the CAS heartbeat is "
    "fresh, OR the stack is unreadable; everything unknown fails OPEN to "
    "mode 0. Outside the window menus run the "
    "gameplay-proven mode-2 suppression (lightmap pages + <= 512 "
    "composites still execute).")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_unsuppress, false, "Skate 3",
    "ESCAPE HATCH, normally unnecessary: while a menu/pause/loading context "
    "(presence context 0) renders natively, temporarily clear "
    "native_render_suppress_emulated_draws so ALL emulated passes execute. "
    "The original motivation, the team-menu skater portrait boxes are "
    "one-shot render-to-texture passes that suppression left forever empty "
    ", is covered without this by the SDK's pitch-selective suppression "
    "(native_render_suppress_mode 2: surfaces <= 512 px wide, incl. the "
    "portrait cards, always execute). Turn on only if small offscreen "
    "composites are missing in menus despite that; costs the full emulated "
    "pipeline's GPU time during menus.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_snapshot_all_draws, false, "Skate 3",
                    "Record the draw stream on every recorded frame instead of 2 of every "
                    "60 (large .draws.bin; for targeted investigations)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadows, true, "Skate 3",
                    "Render the game's dynamic CSM shadows natively (skater, NPCs, "
                    "movable props onto the world). "
                    "Cascade matrices are captured per frame from the game's own "
                    "material constants.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_caster_refresh_all, true, "Skate 3",
                    "Perspective-bank palette refresh covers ALL caster-sourced "
                    "skinned captures: skinned-published ROPA garments join the "
                    "refresh pool (their exclusion left the player's tee/hair riding "
                    "the ortho caster banks, whose fine animation runs ticks stale in "
                    "bursts, the high-fps hair-off-the-head wedge / vanishing "
                    "garment), and the refresh target is picked by palette identity "
                    "(<1.5 m) across every caster-sourced clone instead of "
                    "oldest-only (mesh-sharing NPC clones probed the wrong item, the "
                    "guard refused, and the right clone stayed on its stale caster "
                    "bank, the NPC blink). false = legacy oldest-only, non-ropa "
                    "refresh.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_char_rows_inst, true, "Skate 3",
                    "Per-(mesh,instance) fallback for the character-lighting capture: "
                    "items whose capture failed to validate this frame reuse their own "
                    "last validated rows. Covers instanced pieces and the player, which "
                    "the single-instance mesh fallback and the LW entity cache both "
                    "miss; without it hair flips between the blended sub-pass and the "
                    "legacy opaque path on capture-failed frames (a frame-to-frame "
                    "flicker whose rate scales with fps).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shadow_tile, 0, "Skate 3",
                     "Shadow cascade tile resolution. The game's own atlas is "
                     "three 512 tiles; the blur steps raster texels (the game's "
                     "tfetch offsets, which the emulated GPU applies in scaled "
                     "texels), so higher values sharpen both the silhouette and "
                     "the penumbra like the emulated baseline. 512 = the softer "
                     "original-console look, blocky up close. 0 = auto: 512 x "
                     "the render resolution scale (the Resolution Scale "
                     "setting), matching the emulated renderer's shadow "
                     "crispness at any render resolution. Applies live: the "
                     "atlas chain recreates on change.")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_caster_parity, true,
                    "Skate 3",
                    "Character pieces cast dynamic shadows only when the game "
                    "submitted them to its own shadow passes this frame (per-piece "
                    "caster list; e.g. the CAS trucker hat never casts; natively "
                    "casting it painted a hard brim band across the editor face). "
                    "OFF = every published character piece casts (old behavior).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// OFF by default: a native addition the game does not have. World surfaces
// never sample it (their static shade is baked into the lightmaps, matching
// the game), and characters/props sample the game's own natively
// re-rendered world-shade map (tf1) as soon as it is primed, so this map
// only ever served as their fallback for the frames before that, while
// costing a full static depth pass every frame.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_static_casters, false,
                    "Skate 3",
                    "Native static sun-shadow map (not in the original game; "
                    "the game's own world-shade map covers characters and "
                    "props): render the STATIC world "
                    "(buildings, trees, rails, placed props) into a separate "
                    "camera-centered ortho depth map along the material sun, "
                    "sampled by every lit surface in addition to the game's "
                    "dynamic cascades. Live static shade on characters and "
                    "props (which baked lightmaps cannot shade) and shadows "
                    "that follow a moved sun; receivers keep the darker of "
                    "baked and live shade rather than doubling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_strength, 1.0,
                      "Skate 3",
                      "How dark static-geometry shadows get (1 = the full "
                      "dynamic-shadow clamp, 0 = invisible). Applied at "
                      "receive from the static sun map, so characters and "
                      "props keep full-strength shadows. Moderate values "
                      "keep live static shade believable where it disagrees "
                      "with the baked lighting's own sun.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_radius, 180.0,
                      "Skate 3",
                      "Half-extent in meters of the static sun-shadow map's "
                      "far cascade (camera-centered; the mid and inner "
                      "cascades cover 1/2 and 1/6 of this at 2x and 6x "
                      "the texel density). Larger reaches farther at lower "
                      "texel density; the term fades out at the outer edge.")
    .range(40.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shadow_static_size, 1024,
                     "Skate 3",
                     "Static sun-shadow map resolution per cascade tile "
                     "(the map is three tiles). For scale, the console's own "
                     "budget is 1536x512 for its cascade atlas and 512x512 for "
                     "the baked-shade map its props and characters sample "
                     "(both read off the guest fetch constants). 1024 spends "
                     "4x that and 1/16th of the old 4096 default -- which was "
                     "50 Mpx, the heaviest pass in the renderer and a TDR risk "
                     "on weak GPUs -- while staying far finer than anything "
                     "the game has: ~6 cm texels inner / ~18 cm mid / ~35 cm "
                     "far at the default radius. Applies live: the map "
                     "recreates on change.")
    .range(1024, 8192)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadow_pcss, true, "Skate 3",
                    "Contact-hardening soft shadows (PCSS): a blocker search "
                    "estimates the caster distance per pixel and the filter "
                    "width follows the sun's angular size: crisp at the "
                    "contact point, progressively softer with caster height. "
                    "OFF = the game's fixed-width blurred atlas sampling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_sun_deg, 1.0,
                      "Skate 3",
                      "Sun angular diameter in degrees for the PCSS penumbra "
                      "(the real sun is ~0.53; larger reads softer).")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_max_m, 0.8,
                      "Skate 3",
                      "Maximum PCSS penumbra half-width in meters (caps how "
                      "soft very tall casters can get).")
    .range(0.05, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_blocker_m, 2.5,
                      "Skate 3",
                      "PCSS blocker-search radius in meters. Must be at "
                      "least the max penumbra for distant casters to soften "
                      "fully; larger values let unrelated nearby casters "
                      "soften edges they should not.")
    .range(0.1, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_pcss_min_texel, 1.0,
                      "Skate 3",
                      "Minimum PCSS filter radius in physical atlas texels "
                      "(keeps fully hardened edges filtered).")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_bias, 0.08,
                      "Skate 3",
                      "Static sun-shadow receiver bias in METERS; static "
                      "geometry is its own caster, so its surfaces compare "
                      "against their own map depth. Slope-scaled: grazing "
                      "sun angles get up to several times this. Raise on "
                      "acne (stipple on sunlit ground/walls), lower if "
                      "static shadows visibly detach from their casters.")
    .range(0.0, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_shadow_static_bias_vk, 0.0,
                      "Skate 3",
                      "Additional static sun-shadow receiver bias in METERS "
                      "applied on the Vulkan backend only (a tuning margin "
                      "for backend shader-toolchain rounding differences; "
                      "raise only if acne appears on Vulkan specifically).")
    .range(0.0, 0.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_char_shadow_exact, true,
                    "Skate 3",
                    "Characters sample the CSM atlas with the game's own math: "
                    "per-cascade receive biases captured from the character PS "
                    "banks and NINE point taps at +-1 game-texel averaged /9 "
                    "(cacstamp_skin_nisPS ucode port). OFF = the legacy single "
                    "bilinear tap + coverage term.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Reflective-glass (env families 5/6) isolation controls for the F12
// dialog: live A/B of each stage of the cube-reflection term against the
// emulated look (F5).
REXCVAR_DEFINE_INT32(skate3_native_render_scene_refl_mode, 0, "Skate 3",
                     "Reflective glass debug: 0 normal, 1 cube term off, 2 cube at "
                     "the absolute LOD in refl_lod, 3 flat normal (no normal-map "
                     "perturb), 4 visualize the cube sample only, 5 body only (no "
                     "spec, no cube), 6 normal-map LOD bias from the slider, "
                     "7 visualize the raw lightmap sample, 8 visualize the "
                     "lightmap UV (frac x16), 9 lightmap resolve status "
                     "(red = bound, blue = white fallback).")
    .range(0, 9)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_lod, 0.0, "Skate 3",
                      "Reflective glass debug: mode 2 = absolute cube mip level; "
                      "other modes = EXTRA LOD bias on top of the automatic "
                      "640p-parity bias.")
    .range(-4.0, 12.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Constant tangent-space normal tilt on the reflective glass, live-tunable
// (F12). The defaults are DERIVED, not tuned: the material's 16x16 detail
// texture is a constant BC1 block whose endpoints expand on HARDWARE by bit
// replication: 5-bit red 16 -> (16<<3)|(16>>2) = 132/255, 6-bit green 32
// -> (32<<2)|(32>>4) = 130/255; so the shader's 2*d - 1 fold is exactly
// (0.035294, 0.019608). An earlier fold used our CPU decoder's
// integer-division expansion (131/129 -> 0.028/0.012), leaving a ~0.8 deg
// constant normal tilt = the residual reflection-position offset that was
// dialed out by hand to (0.036, 0.019), matching the hardware value to
// the slider step and confirming the derivation.
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_x, 0.035294, "Skate 3",
                      "Reflective glass: constant tangent-X (horizontal) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_y, 0.019608, "Skate 3",
                      "Reflective glass: constant binormal-Y (vertical) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_refl_bias_auto, true, "Skate 3",
                    "Derive the reflective-glass normal tilt from each material's "
                    "own detail texture (hardware-exact BC1 decode of its constant "
                    "color) instead of the refl_bias_x/y values. The sliders "
                    "remain the fallback/override with auto off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Fine-grained feature gates for the F12 native-render debug dialog: each
// isolates one subsystem so regressions (flicker, wrong shading) can be
// bisected live without rebuilds. All hot-reload, default on.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_macro, true, "Skate 3",
                    "Apply the macrooverlay grime/crack multiply on world materials")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_decal_probe, 0, "Skate 3",
                     "TEMP decal diagnostic: 0 = off, 1 = show the decal "
                     "alpha (blend weight) on environment.decal_tileable "
                     "World-material diagnostic overlay (\"material probe\" "
                     "in the render settings): 0 = off, 1 = the macro "
                     "(grime) sample at the shipping uv, 2 = the resulting "
                     "overlay multiplier, 3 = the macro inputs as colour "
                     "(R = gate, G = uv scale, B = opacity), 4 = the macro "
                     "sample at the raw uv, 5 = a solid colour per shading "
                     "family, 6/7/8 = the decal (grime/paint) layer's alpha "
                     "/ rgb / uvs on the decal families. Rebuilds the scene "
                     "shaders on change.")
    .range(0, 11)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_decals, true, "Skate 3",
                    "Composite environment.decal art (graffiti/paint) over base diffuse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shader_census, false, "Skate 3",
                    "Log each (material name, game pixel shader) pair once: which "
                    "shader the game itself binds for every material the native scene "
                    "renders, plus the native family it was given (\"shader-census:\" "
                    "lines). For mapping game shaders the native renderer does not "
                    "implement yet. Off by default.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_transparents, true, "Skate 3",
                    "Draw environment.transparent items (mist/glass/fences) in the "
                    "alpha-blended sub-pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_world_items, true, "Skate 3",
                    "Publish world sort-list items (static geometry)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_entity_fade, true, "Skate 3",
                    "Honor the game's per-entity spawn/distance fade: LivingWorld "
                    "pres entities (NPCs, traffic vehicles) publish an opacity that "
                    "every character-family PS writes as output alpha (peds c21.x, "
                    "defaultcharacter c13.x, cacstamp c22.x, vehicle body c20.x). "
                    "The game submits their draws at alpha 0 through the whole "
                    "spawn settle (the physics drop) and ramps alpha up afterwards "
                    "/ by distance. Off = the old behavior: entities render fully "
                    "opaque from their first draw (mid-air spawn pop, early "
                    "distant pop-in).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynamic_items, true, "Skate 3",
                    "Publish dynamic entities (characters, props, cloth)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_fade, true, "Skate 3",
    "Serve LivingWorld NPC/vehicle fade alpha from the entity itself "
    "(entity+528 via the LW entity store, mapped per MeshContext) instead "
    "of the per-draw captured constant row. The captured row is a per-draw "
    "inference: capture races serve alpha=1 (opaque mid-air spawns, no "
    "fade-in) or a clone's foreign row (one-frame invisibility blinks). "
    "Off = the pre-store captured-row behavior.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_gap_fill, true, "Skate 3",
    "Republish a LivingWorld NPC for up to 2 frames when its MeshContext "
    "drops out of the submit records while the entity is still alive (the "
    "1-3 frame publish GAPs: an in-view NPC vanishing for a frame reads as "
    "a blink/teleport). Off = gaps render as-is.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_ghost_guard, true, "Skate 3",
    "Never draw a character the game itself is not drawing: drop character "
    "items inside the game camera's view whose mesh had no draw this frame "
    "(hidden / pooled AI skaters, faded-out pedestrians kept alive by the "
    "capture rescues). Off = the old behavior.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_ghost_audit, false, "Skate 3",
    "DIAGNOSTIC: log character / dynamic-prop instances the scene keeps "
    "publishing WITHOUT a live capture (palette rescues, rigid rescues, LW "
    "gap fills, off-screen retention) for many consecutive frames: the "
    "candidates for NPCs drawn where the game shows none.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_identity, true, "Skate 3",
    "Key LivingWorld entities' pose-smoothing rings by their MeshContext "
    "(the game's own per-instance identity) instead of (mesh, occurrence) "
    "pairing. Same-mesh clone reshuffles in the sort lists can then never "
    "mispair a ring (the NPC/prop teleport-slide class). Off = the "
    "positional re-pair heuristics alone.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_inline, false, "Skate 3",
                    "Decode CPU-cloth (ROPA) garment VBs inline on the render thread "
                    "instead of the worker jobs. Measured 4.3ms avg per garment "
                    "decode (committed-resource allocation dominates) AND it does not "
                    "address the jelly; the mismatch is the cloth shape having no "
                    "place on the interpolation play clock, not decode latency. Kept "
                    "for experiments only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_blend, true, "Skate 3",
                    "Lerp CPU-cloth shape generations onto the motion-smoothing play "
                    "clock at draw time (pose<->shape pairing via the interp ring); "
                    "the stepped shape against the interpolated body was the tee "
                    "jelly/clip-through, worse at LOWER fps. OFF = newest decode "
                    "(old behavior).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_2d_async_px, 1, "Skate 3",
    "Pixel-count threshold above which 2D/HUD texture decodes are ELIGIBLE "
    "for the words-miss decode workers. Eligible decodes (first sightings "
    "and content changes alike) still run INLINE while the per-frame inline "
    "budget (4 ms; fullscreen-class art gets double) lasts; the run-copy "
    "untiler made even 1280x720 sub-millisecond, and the old always-async "
    "policy made every APT re-raster (NEW address = new key = first "
    "sighting) skip its quad for the 1-3 worker frames: menu elements and "
    "fullscreen backdrops visibly BLINKED through animations. The workers are "
    "the burst-overflow valve: past the budget, first sightings skip 1-3 "
    "frames and content changes serve the stale decode until the commit. "
    "0 = everything inline unconditionally; larger values exempt small art "
    "from async eligibility entirely.")
    .range(0, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_2d_sharp, 0.0, "Skate 3",
                      "Sharp-magnification amount for the 2D/HUD overlay (0 = plain "
                      "bilinear, up to 2). Much of the APT (Flash) HUD samples "
                      "cached-bitmap tiles whose texel count equals their 720p display "
                      "size (score digits, gauge ring, compass; measured density 1.0 "
                      "in the 2D draw stream) while text batches sample 512x512 glyph "
                      "atlases at 8-10 texels/pixel; at 2-3x output scales the tiles "
                      "blur under bilinear while atlas text stays crisp; the same on "
                      "console/emulated, the content is simply 720p. Catmull-Rom + "
                      "clamped unsharp mask where the fetch is magnified (>1.25x), "
                      "ramped to full by 2x; minified/1:1 fetches untouched. DEFAULT "
                      "0 (off): sharpened ramps read worse than the soft "
                      "bilinear; the honest fix is higher-res source pixels (APT "
                      "cache-tile rasterization scale), not edge-contrast synthesis.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_boxcar, true, "Skate 3",
                    "Blend CPU-cloth shape generations through the SAME 8-tap boxcar "
                    "kernel the body bones/garment world are filtered with (see "
                    "smooth_camera_filter_ms) instead of a plain 2-generation lerp. "
                    "The plain lerp reconstructs the 60Hz limb signal SHARPLY while "
                    "the boxcar rounds the body ~a window; the cloth led the body "
                    "through every direction change by an excursion that scales with "
                    "the guest period (the residual tee jelly after the blend fix). "
                    "OFF = plain bracketing lerp (for A/B).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_bias, 0, "Skate 3",
                     "Shift the ROPA pose<->shape pairing by N ring poses (+ = fresher "
                     "shape, - = older), a live trim for any residual constant drape "
                     "lag/lead while skating. 0 = the paired generation.")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_delay, 0, "Skate 3",
                     "Delay CPU-cloth VB snapshots by N guest frames before the worker "
                     "decode, phase-aligning the cloth SHAPE with the motion-smoothing "
                     "play clock the body renders on (~2 periods behind now). Without "
                     "it the drape is ~2 frames AHEAD of the rendered body; it hangs "
                     "where the body WILL be and leads it through direction changes "
                     "(the tee jelly / clip-through-torso). DEFAULT 0: in practice "
                     "the garment LAGGED, and delay made it worse; the "
                     "phase model was backwards. Kept for experiments.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_trace_mesh, "", "Skate 3",
                      "Hex guest mesh address to trace end-to-end through the "
                      "texture pipeline ('tex-trace:' log lines): per-frame "
                      "served objects + content fingerprints (on change), "
                      "every slot resolve decision (direct/sticky/hold/near-"
                      "black/white), every payload/words poll verdict, and "
                      "every worker commit touching the mesh's objects. "
                      "Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_trace_2d, "", "Skate 3",
                      "Hex fetch word 1 (base|flags) of a 2D overlay texture "
                      "to trace through the 2D resolver ('2d-trace:' log "
                      "lines): per-resolve entry state, liveness probe "
                      "results and the serve/heal route taken, plus the "
                      "worker commits for its words key. Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_detail_hold, 240, "Skate 3",
                     "Frames an item slot keeps serving its previous HIGHER-"
                     "resolution texture after the game's material-detail "
                     "system rebinds a strictly smaller one (streaming "
                     "pressure flaps a nearby mesh's DT material to its UN "
                     "variant for ~0.5 s and back, the visible 'different "
                     "texture set' flash; the detailed decode is still "
                     "cached host-side, so the flap can be invisible). A "
                     "downgrade that persists past the hold is adopted (a "
                     "real demote as you leave the area). 0 = serve the "
                     "guest binding verbatim (the console's own detail pop).")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_tex_store_mb, 1280, "Skate 3",
                     "Texture content-store GPU byte budget in MB. The store "
                     "is count-capped, but per-entry sizes differ per map and "
                     "the idle guards keep a superseded map's working set "
                     "resident for minutes after a switch; over this budget "
                     "the LRU drains oldest-first with a shortened idle "
                     "guard so VRAM does not accumulate across map changes.")
    .range(256, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_mesh_store_mb, 1024, "Skate 3",
                     "Mesh cache GPU byte budget in MB (vertex + index "
                     "buffers of cached decodes). Same byte-pressure LRU "
                     "behavior as the texture-store budget.")
    .range(256, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_retain_offscreen, true, "Skate 3",
                    "Keep recently seen static items in the scene while the game "
                    "view-culls them: the re-timed (smoothed) render camera trails "
                    "the guest pose by up to the filter window, so items leaving "
                    "the guest frustum were visibly torn down right at the screen "
                    "edges during pans/traversal. Items the guest frustum can see "
                    "but stopped submitting (LOD switch, despawn) drop immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_revalidate, true, "Skate 3",
                    "Re-fingerprint cached texture payloads every 16 frames and "
                    "re-decode on change (heals late-composed lightmap pages)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_stretch_guard, true, "Skate 3",
                    "Draw-time stretch veto: skin cached sample verts of each "
                    "skinned mesh's GPU-resident decode with the final palette "
                    "every frame; wider than bind size = the 1-frame mangled-"
                    "ribbon flash; skip the item's draws (blink)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_stretch_guard_dump, false, "Skate 3",
                    "On a stretch veto, dump the full bone palette and probe "
                    "verts to logs/stretch_*.txt (first 6 trips) for offline "
                    "diagnosis of which rows are junk")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_shader_dump_dir, "", "Skate 3",
                      "Dev: write every distinct guest shader's raw big-endian "
                      "Xenos microcode to this directory as ps_/vs_<name>.bin "
                      "(feed the same folder to skate3_disasm_ucode_dir to "
                      "disassemble)");

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lm_dump, false, "Skate 3",
                    "Diagnostic: dump the decoded mip 0 of every generated-mip "
                    "(no-chain composed page) texture to native_texture_dumps/ "
                    "as raw RGBA for offline diffing against the gsnap decode")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_backface_cull, true, "Skate 3",
                    "Backface-cull world env materials like the game does: the "
                    "material XMLs set CULLMODE=FRONT on every environment "
                    "family (banners calibrated game-kept faces = our D3D12 "
                    "BACK faces -> CULL_FRONT). CULL_NONE stacked hidden faces "
                    "into the frame: double glass panes + interior wall faces "
                    "behind the translucent canopy glass = the too-bright "
                    "slope / too-dark awning deltas. Trees/alphatest (fams "
                    "7/9/10) and mirrored instances stay uncull(ed).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_mesh_revalidate, true, "Skate 3",
                    "Re-decode cached meshes when their payload fingerprint changes "
                    "(streaming arena reuse; also picks up CPU-animated buffers)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_char_track, false, "Skate 3",
    "Per-frame diagnostic log line tracking the main character's body and "
    "ropa garment through the motion-smoothing pipeline: pre/post-interp "
    "reference positions, ring state, ingest/reset/claim events, resolved "
    "mode, shape-kernel state, and the garment<->body offset. Heavy "
    "(one line per rendered frame), for artifact diagnosis runs.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_smooth_camera, true, "Skate 3",
                    "Re-time the camera on the host clock: the guest publishes new "
                    "camera poses on its own sim tick (~170-240 Hz, irregular: "
                    "measured streaks of 10 rendered frames on one pose at 400 fps), "
                    "which reads as the world juddering/skipping while panning. "
                    "Interpolates between the last two distinct guest poses, one "
                    "sim-interval behind (a few ms of added camera latency, no "
                    "overshoot). Teleports/cuts snap.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_native_render_scene_smooth_camera_filter_ms, 50.0, "Skate 3",
    "Boxcar filter window (ms) applied to the smoothed camera pose, centered "
    "on the playback point. The game's camera pose VALUES advance in 60 "
    "Hz-quantized lumps at high render rates (measured: +-2.2 deg off a "
    "constant-rate stick pan, velocity CV 0.84); 50 ms = three 60 Hz "
    "periods nulls the quantization at any render rate (measured 185 -> 7 "
    "deg/s rms frame-to-frame velocity jitter) for ~25 ms extra camera "
    "latency. 0 = off (raw sample interpolation).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Synthetic camera pan: a judder-isolation probe, not a feature. Replaces
// the camera with a host-generated constant-rate horizontal pan so each
// pipeline stage can be judged against a mathematically smooth ground truth
// (the guest's own camera path is irregular; smooth output can never be
// eyeballed against it). Modes pick the injection stage:
//   1 = time-based: pose is a pure function of the host clock at scene-build
//       time, bypassing guest pose sampling AND the smoother entirely. Still
//       judders => the fault is downstream (frame pacing / present / display).
//   2 = fixed-step: pose advances a constant angle per PUBLISHED FRAME
//       (ignores time). Smooth only if displayed frames appear at even
//       intervals, the complement of mode 1 (irregular pacing that mode 1's
//       time base compensates for shows up here, and vice versa).
//   3 = through-smoother: the ~1 kHz sampler thread synthesizes guest-like
//       pose samples (~200 Hz, deliberately irregular) and the normal
//       SmoothCamera reconstruction runs on them; reconstruction error vs
//       the known ideal is measured numerically (err_deg in the log line).
//       Judders here but not in mode 1 => the smoother is at fault.
REXCVAR_DEFINE_INT32(skate3_native_render_scene_synthetic_pan, 0, "Skate 3",
                     "Synthetic constant-rate camera pan (judder isolation): 0 = off, "
                     "1 = time-based at scene build, 2 = fixed angle step per frame, "
                     "3 = synthetic samples through the camera smoother.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_rate, 90.0, "Skate 3",
                      "Synthetic pan rate in degrees/second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_amp, 0.0, "Skate 3",
                      "Synthetic pan amplitude in degrees: 0 = continuous full 360 "
                      "rotation (sweep the REAL camera around once with the stick "
                      "after engaging; the probe accumulates a union of every static "
                      "item the game submits, filling in the full surround); > 0 = "
                      "triangle-wave +-amp around the engage heading.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bonesig_auto, 0.0, "Skate 3",
                      "Auto-arm bone-signal recordings of this many seconds "
                      "(entity-pose diagnosis, same output as the F12 button): "
                      "first window ~30 s after the native scene comes up, "
                      "re-armed every 90 s, 3 windows max. 0 = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_sort_opaque, true, "Skate 3",
                    "Draw opaque scene items front-to-back (bbox-center camera "
                    "distance). Early-z rejects occluded pixels before the heavy "
                    "material shading runs; the game's own sort order is by "
                    "render state, not depth.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_mips, true, "Skate 3",
                    "Upload guest texture MIP CHAINS (off = mip 0 only; distant "
                    "surfaces alias but mip-related artifacts disappear). Flush the "
                    "texture cache after toggling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Hook layer master switch, defined in skate3_native_render.cpp. The runtime
// toggle refuses to flip the scene on without it: the hooks that feed the
// scene are only installed when it was set at boot.
REXCVAR_DECLARE(bool, skate3_native_render);
// Defined in native/skate3_native_entity.cpp (the identity store module).
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_ropa_world_primary);
// Defined in native/skate3_native_lw.cpp (the LW entity store module).
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_palette);
// SDK-level emulated-draw suppression (rexglue native_guest_renderer.cpp):
// command processors skip emulated draw/resolve execution while the native
// output is active. Temporarily overridden to false during menu contexts by
// YieldForMenus (see skate3_native_render_scene_menu_unsuppress).
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
// SDK-level suppression PASS FILTER (d3d12 command_processor.cpp): 0 =
// suppress framebuffer-sized passes only (pitch >= 1280), 2 = suppress all
// except lightmap pages (1024) and small composites (<= 512). YieldForMenus
// drops it to 0 during menu contexts so the skater-portrait RTT passes
// (pitch > 512, in the mode-2 suppressed band) execute; see
// skate3_native_render_scene_menu_rtt_passes.
REXCVAR_DECLARE(int32_t, native_render_suppress_mode);
// SDK-level forced resolve readback window (rexglue command_processor.cpp /
// both backends' IssueCopy): when > 0, resolves up to that byte length are
// synchronously read back to CPU-visible guest memory regardless of
// readback_resolve and gameplay state. Armed by UpdatePhotoGrabWindow while
// a photo flow is active; the game CPU-reads the resolved screenshot
// target to build the photo JPEG.
REXCVAR_DECLARE(int32_t, native_render_force_resolve_readback_max_length);
// SDK-level readback-downscale sample point (resolve_downscale shader): when
// set, the scaled->1x extraction samples the CENTER host pixel of each
// scale_x*scale_y block instead of the top-left one, compensating the
// D3D9-style half-pixel offset that becomes a (scale/2)-pixel content shift
// at scaled resolutions. Armed with the photo window so grabbed photos
// sample host pixel centers.
REXCVAR_DECLARE(bool, readback_resolve_half_pixel_offset);
// SDK-level async pipeline compilation (rexglue command_processor.cpp): the
// d3d12 backend SKIPS draws whose pipeline is still compiling. Forced
// synchronous during menu contexts by YieldForMenus so one-shot portrait
// renders can't lose still-compiling pieces (first-run armless skaters).
REXCVAR_DECLARE(bool, async_shader_compilation);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_perf_log, false, "Skate 3",
                    "Log periodic native-renderer performance breakdown lines "
                    "(see skate3_native_render_scene_perf_interval)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(skate3_native_render_scene_perf_interval, 600, "Skate 3",
                     "Frames between native-scene perf/stats log lines. Lower "
                     "values give finer windows for chasing transient frame-"
                     "rate dips at the cost of log volume.")
    .range(60, 6000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull, true, "Skate 3",
                    "Skip drawing static world items whose bounds are "
                    "provably hidden behind already-rendered geometry (depth-"
                    "grid test, conservative: anything unprovable still "
                    "draws). Shadow passes are exempt, so hidden geometry "
                    "keeps casting shadows. Dense areas with extended world "
                    "streaming spend roughly half their per-item CPU on such "
                    "items.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull_build, true,
                    "Skate 3",
                    "Also skip the per-frame scene-item rebuild for statics "
                    "the occlusion cull proved hidden, on 3 of every 4 "
                    "frames (staggered per instance). The every-4th-frame "
                    "rebuild re-enters the item in the rendered scene so "
                    "disocclusion is re-tested and the persistent shadow "
                    "caster cache stays refreshed. Requires "
                    "skate3_native_render_scene_occlusion_cull.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_occlusion_cull_guest, true,
                    "Skate 3",
                    "Also skip the GUEST engine's draw-list dispatch for "
                    "items the occlusion cull proved hidden (the dispatch "
                    "builds command packets the native renderer suppresses "
                    "anyway, and it is the frame-time-critical cost). "
                    "Capture still sees every item, so shadows and scene "
                    "state are unaffected. Requires "
                    "skate3_native_render_scene_occlusion_cull.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_skip_guest_zprepass, true, "Skate 3",
                    "Skip the GUEST engine's depth-only z-prepass of the main "
                    "camera view while the native renderer is drawing the "
                    "world. The pass only fills the emulated GPU's depth "
                    "buffer, which nothing shows in native mode; it is about "
                    "a quarter of the game's draws (guest traversal, D3D "
                    "calls and our draw hook) in dense views. Other views "
                    "(portraits, photos) and emulated mode keep it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_perf_items, false, "Skate 3",
                    "Deep per-item CPU profiling: adds a perf-items log line "
                    "per perf window attributing per-item cost to pipeline "
                    "stages (mesh/texture/constants/submit; cache validate/"
                    "fingerprint/walk/fetch) and splitting draw time between "
                    "items inside vs outside the rendered view frustum. Adds "
                    "measurable overhead at high item counts; leave off "
                    "outside investigations.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ring, false, "Skate 3",
                    "Record a rolling per-frame scene-composition ring (~900 "
                    "frames); F7 dumps it to logs/scene_ring_<ts>.csv for "
                    "diffing 1-2 frame artifacts no capture can catch")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dyn_gap_fill, true, "Skate 3",
                    "Re-publish character/cloth/skinned items for up to two "
                    "missed publish frames (high-frame-rate body flicker)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
namespace skate3::native_scene {
namespace {

// Loose-world (CAS / menu scene) type field, [game globals] + offset (dev:
// +72 + 17488). Retail: LooseWorldPresentation::GetWorldRenderLocation is
// sub_827950A8 (the render-location keys 0x664B0D04... / 0x6568F346...),
// which switches on [this+17496] (1 / 2) and reads the instance at +17512;
// both its callers pass this = [0x83083C38] + 120. So 120 + 17496.
constexpr uint32_t kLooseWorldTypeOffset = 120 + 17496;

// skate.Park piece shading (environmentpark{diffuse,alphatest,decal}). The
// retail park PSes share the dev diffuse / decal shadow registers (CSM c0..4,
// Exp c5..8, biases c9/c10, floors c11/c12, sun c13); the row capture below
// reads retail's layout above that (see the fam >= 4 block).
constexpr bool kRetailParkPortDone = true;

// Verified guest structure offsets.
constexpr uint32_t kCtxEffectList = 0x04;
constexpr uint32_t kEffectListPassCount = 0x44;
// Mesh context draw-list layout, read off the game's own dispatcher
// (sub_8297A5F8). Retail's equivalents were a u16 count at +0x38 with a flat
// entry array at +0x48; this build uses a count plus an island-index array
// that selects into the entry array.
constexpr uint32_t kCtxDrawCountU16 = 0x38;  // u16 draw count (retail)
constexpr uint32_t kCtxDrawList = 0x48;      // 16-byte draw entries (retail)
constexpr uint32_t kMeshMaterial = 0x24;
constexpr uint32_t kMeshVertexDescriptor = 0x28;
constexpr uint32_t kMeshIndexBuffer = 0x30;
constexpr uint32_t kMeshVertexBuffer = 0x34;
constexpr uint32_t kBufferPhysAddr = 0x18;
constexpr uint32_t kVbBytes = 0x20;
constexpr uint32_t kIbCount = 0x20;
constexpr uint32_t kViewCameraFromView = 0x08;
constexpr uint32_t kViewCamViewProj = 0xA0;

// Cross-thread renderer state (published scene, prewarm queues, capture
// rows, telemetry, perf windows) lives in skate3_native_scene_state.h.

float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  return std::bit_cast<float>(bits);
}

uint32_t BSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Draws completed since startup (any guest draw function). The RenderMesh
// hook samples it around the original call: an unchanged sequence means the
// mesh's draws were deferred by the dispatcher and the constant bank still
// belongs to some earlier mesh; its transform must come from the post-draw
// fixup, never from the bank at submit-exit (a leftover identity matrix at
// c4 validates as a plausible world and renders the prop at the origin).
std::atomic<uint64_t> g_draw_seq{0};

// Provenance of the last completed guest draw: (ib_obj << 32 | vb_obj) for
// indexed 3D draws, 0 for everything else. The submit-exit capture may only
// consume the constant bank when the mesh's OWN draw was the last one to
// flush it; `drew_inside` alone proved only that SOME draw ran during the
// submit call. When this mesh's draws were deferred but another entity's
// inline draws ran inside the call, the bank holds that entity's palette,
// and the sample-projection acceptance gate cannot reliably refuse it: a
// vehicle right next to the skater, skinned by the skater's foreign
// palette, still projects on-screen at a plausible spread, and rendered
// glued to his walking bones (the player-becomes-the-vehicle bug).
std::atomic<uint64_t> g_last_draw_ibvb{0};

// Register a dynitem for the per-draw character-lighting refresh (see
// g_frame_char_refresh), once per (buffers, item). The old per-site `< 256`
// gate plus a fresh entry on EVERY caster-bank fixup (the same item re-pops
// on each of its 3-4 shadow submissions) filled the table early in busy
// frames (skaters + pedestrians + parked traffic); the main pass submits
// last, so its items were never registered and kept the SUBMIT-time PS
// bank = the previous draw's constants. Cars sharing one model then showed
// the preceding instance's paint (F11: the game drew the near car with
// yellow c22, ours had the car drawn before it, blue). Runaway backstop only.
void RegisterCharRefresh(uint64_t key, size_t index) {
  auto range = g_frame_char_refresh.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == index) {
      return;
    }
  }
  if (g_frame_char_refresh.size() < 16384) {
    g_frame_char_refresh.emplace(key, index);
    g_frame_char_refresh_keys.insert(key);
  }
}

bool BankIsOrtho(uint8_t* base, uint32_t bank);  // defined below

// Skinned vertex-shader constant layouts, straight from the game's own
// shader constant tables (data/render/shaders/final/*VS.vpo, D3D CTAB): the
// register of i_boneArray (the bone palette), i_vtx_params (the ropa sim
// flag: x > 0 = skin from i_boneArray, else the rigid i_matWorld block),
// i_matWorld and i_customGraphic; -1 = not declared. Every skinned VS the
// game ships is listed. The in-memory shader objects keep only the path +
// microcode (the CTAB is stripped), so the table is baked here.
struct VsLayout {
  const char* name;
  int bone;
  int params;
  int matworld;
  int cg;
};
constexpr VsLayout kVsLayouts[] = {
    {"cac_alpha_defaultVS", 7, -1, -1, -1},
    {"cac_alpha_simpleVS", 4, -1, -1, -1},
    {"cac_cloth_defaultVS", 7, -1, -1, -1},
    {"cac_cloth_ropa_defaultVS", 8, 7, 191, -1},
    {"cac_cloth_ropa_simpleVS", 5, 4, 188, -1},
    {"cac_cloth_simpleVS", 4, -1, -1, -1},
    {"cac_face_defaultVS", 7, -1, -1, -1},
    {"cac_face_simpleVS", 4, -1, -1, -1},
    {"cac_face_unwrapVS", 0, -1, -1, -1},
    {"cac_hair_defaultVS", 7, -1, -1, -1},
    {"cac_hair_ropa_defaultVS", 8, 7, 191, -1},
    {"cac_hair_ropa_simpleVS", 5, 4, 188, -1},
    {"cac_hair_simpleVS", 4, -1, -1, -1},
    {"cacstamp_cloth_defaultVS", 7, -1, -1, 190},
    {"cacstamp_cloth_ropa_defaultVS", 8, 7, 191, 195},
    {"cacstamp_cloth_ropa_simpleVS", 5, 4, 188, -1},
    {"cacstamp_cloth_simpleVS", 4, -1, -1, -1},
    {"cacstamp_shift_defaultVS", 7, -1, -1, 190},
    {"cacstamp_shift_simpleVS", 4, -1, -1, -1},
    {"cacstamp_skin_defaultVS", 7, -1, -1, 190},
    {"cacstamp_skin_simpleVS", 4, -1, -1, -1},
    {"cacstamp_skin_unwrapVS", 0, -1, -1, -1},
    {"character_skin_defaultVS", 7, -1, -1, -1},
    {"character_skin_simpleVS", 4, -1, -1, -1},
    {"character_skin_unwrapVS", 0, -1, -1, -1},
    {"defaultcharacter_defaultVS", 7, -1, -1, -1},
    // Retail-only compiles; same CTAB as defaultcharacter (bones c7 / c4).
    {"defaultcharacter_light_defaultVS", 7, -1, -1, -1},
    {"defaultcharacter_light_simpleVS", 4, -1, -1, -1},
    {"defaultcharacter_ropa_defaultVS", 8, 7, 191, -1},
    {"defaultcharacter_ropa_simpleVS", 5, 4, 188, -1},
    {"defaultcharacter_simpleVS", 4, -1, -1, -1},
    {"defaulthair_defaultVS", 7, -1, -1, -1},
    {"defaulthair_ropa_defaultVS", 8, 7, 191, -1},
    {"defaulthair_ropa_simpleVS", 5, 4, 188, -1},
    {"defaulthair_simpleVS", 4, -1, -1, -1},
    {"defaulthom_defaultVS", 7, -1, -1, -1},
    {"defaultlivingworld_defaultVS", 7, -1, -1, -1},
    {"defaultlivingworld_simpleVS", 4, -1, -1, -1},
    {"livingworld_stamp_defaultVS", 7, -1, -1, -1},
    {"livingworld_stamp_simpleVS", 4, -1, -1, -1},
    {"vehicle_defaultVS", 7, -1, -1, -1},
    {"vehicle_glass_defaultVS", 7, -1, -1, -1},
    {"vehicle_glass_simpleVS", 4, -1, -1, -1},
    {"vehicle_simpleVS", 4, -1, -1, -1},
    {"vehiclelight_defaultVS", 7, -1, -1, -1},
    {"vehiclelight_simpleVS", 4, -1, -1, -1},
};

}  // namespace
uint32_t ShaderPathAddr(uint8_t* base, uint32_t obj);  // defined below
namespace {

// Layout of one shader object by its build path's leaf name (memoised per
// object; guest render thread).
const VsLayout* LookupVsLayout(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return nullptr;
  }
  static thread_local std::unordered_map<uint32_t, const VsLayout*> memo;
  const auto it = memo.find(obj);
  if (it != memo.end()) {
    return it->second;
  }
  const VsLayout* found = nullptr;
  char path[200] = {};
  if (GuestTryCopy(path, base + ShaderPathAddr(base, obj), sizeof(path) - 1)) {
    path[sizeof(path) - 1] = 0;
    const char* leaf = std::strrchr(path, 92);
    leaf = leaf != nullptr ? leaf + 1 : path;
    const char* dot = std::strchr(leaf, '.');
    const size_t len = dot != nullptr ? size_t(dot - leaf) : std::strlen(leaf);
    for (const VsLayout& l : kVsLayouts) {
      if (std::strlen(l.name) == len && std::strncmp(l.name, leaf, len) == 0) {
        found = &l;
        break;
      }
    }
  }
  if (memo.size() > 4096) {
    memo.clear();
  }
  memo[obj] = found;
  return found;
}

// The bound vertex shader's layout (the SetShader trackers carry swapped
// labels in this build, so both are consulted; only VS names are listed).
const VsLayout* BoundVsLayout(uint8_t* base) {
  for (const auto* tracker : {&g_cur_ps_obj, &g_cur_vs_obj}) {
    if (const VsLayout* l = LookupVsLayout(base, tracker->load(std::memory_order_relaxed))) {
      return l;
    }
  }
  return nullptr;
}

// Palette base register of the bank the bound VS consumes: its declared
// i_boneArray register. 0 = the bound VS is not a skinned character shader
// (no guessing from register contents).
uint32_t BankPaletteBase(uint8_t* base, uint32_t bank) {
  (void)bank;
  const VsLayout* l = BoundVsLayout(base);
  return l != nullptr && l->bone > 0 ? uint32_t(l->bone) : 0u;
}

// Rigid transform validation: the game uses (at least) two VS constant
// layouts, verified from recorded draw streams: the pre-pass layout has a
// row-vector 4x4 world at c4..c7 (rotation rows end in 0, translation in
// c7); the main-pass layout has the camera position at c4 and the world
// 4x4 at c8..c11. Older meshes use a column-vector affine 4x3 at c4..c6.
// Sanity-check rows so a camera-position or parameter block is never
// mistaken for a matrix.
bool TryRow4x4(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[16];
  for (int i = 0; i < 16; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f) return false;
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
  }
  if (!(f[12] > -20000.f && f[12] < 20000.f && f[13] > -20000.f && f[13] < 20000.f &&
        f[14] > -20000.f && f[14] < 20000.f)) {
    return false;
  }
  std::memcpy(out, f, 16 * sizeof(float));
  return true;
}

bool TryColAffine(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[12];
  for (int i = 0; i < 12; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
    if (!(f[r * 4 + 3] > -20000.f && f[r * 4 + 3] < 20000.f)) return false;
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out[i * 4 + j] = f[j * 4 + i];
    }
    out[i * 4 + 3] = 0.0f;
  }
  out[12] = f[3];
  out[13] = f[7];
  out[14] = f[11];
  out[15] = 1.0f;
  return true;
}

// Fill a rigid item's world matrix from a constant bank, whichever layout
// the staging draw used.
bool BankRigidWorld(uint8_t* base, uint32_t bank, float* out) {
  return TryRow4x4(base, bank, 4, out) || TryRow4x4(base, bank, 8, out) ||
         TryColAffine(base, bank, 4, out);
}

// Orthographic viewproj at c0..c3 (bottom row exactly (0,0,0,1)): the CSM
// caster-cascade banks, verified in capture (a
// truck's three caster draws vs its perspective main-pass draw). See
// DrawItem::caster_bank.
bool BankIsOrtho(uint8_t* base, uint32_t bank) {
  const float x = LoadGuestF32(base, bank + (3 * 4 + 0) * 4);
  const float y = LoadGuestF32(base, bank + (3 * 4 + 1) * 4);
  const float z = LoadGuestF32(base, bank + (3 * 4 + 2) * 4);
  const float w = LoadGuestF32(base, bank + (3 * 4 + 3) * 4);
  return std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f && std::fabs(z) < 1e-6f &&
         std::fabs(w - 1.0f) < 1e-3f;
}

// Is this ortho bank one of the game's skate.Park world-shadow passes (the
// DMO exponential atlas, see ParkWsDraw)? Returns the tile (0 inner,
// 1 outer) or -1. Its rows are the park receivers' tf2 rows: inner = PS
// c5 / c7 / c8 as sampled, outer = c5 / c7 scaled by c6.xy and offset by
// c6.zw (environmentparkdiffuse ucode 35/42); depth row c2 = (0, dy, 0, w).
// Caller holds g_palette_mutex (g_park_ws_recv).
int ParkWsPassTile(uint8_t* base, uint32_t bank) {
  if (!g_park_ws_recv_have || !BankIsOrtho(base, bank)) {
    return -1;
  }
  float vr[12];
  for (int k = 0; k < 12; ++k) {
    vr[k] = LoadGuestF32(base, bank + uint32_t(k) * 4);
  }
  if (!(std::fabs(vr[8]) < 1e-6f && std::fabs(vr[10]) < 1e-6f && vr[9] < -0.005f)) {
    return -1;
  }
  const float* rc = g_park_ws_recv;  // c5, c6, c7, c8 (c10 at 16)
  const auto close_to = [](float a, float b) {
    return std::fabs(a - b) <= 1e-4f + 1e-3f * std::fabs(b);
  };
  // Only the rows' xyz (axis and scale) are compared: the map follows the
  // camera, so the translations (.w) move every frame, and matching them
  // against the receiver rows latched on the PREVIOUS frame failed whenever
  // the camera moved -- no pass draws recognized, the tile left cleared
  // (lit) for that frame: the park shadows flickered with camera motion.
  for (int t = 0; t < 2; ++t) {
    const float su = t == 0 ? 1.0f : rc[4], sv = t == 0 ? 1.0f : rc[5];
    bool ok = true;
    for (int k = 0; k < 3 && ok; ++k) {
      ok = close_to(vr[k], rc[k] * su) && close_to(vr[4 + k], rc[8 + k] * sv) &&
           close_to(vr[8 + k], rc[12 + k]);
    }
    if (ok) {
      return t;
    }
  }
  return -1;
}

// AUX perspective viewproj (bank c0..c3, column-vector rows): a perspective
// pass whose projection is NOT screen-shaped; the skater-portrait
// render-to-texture passes (team menu boxes / Import Skater card) render a
// tall narrow card (aspect ~0.4-0.5) while every screen view (main, editor,
// water reflection) is >= 4:3 (16:9 default, wider ultrawide). For a
// combined viewproj the projection scales survive as row norms: the view's
// rotation rows are unit, so ||c0.xyz|| = m00 and ||c1.xyz|| = m11, and
// aspect(w/h) = m11/m00 (verified in capture:
// 1.5/0.843 = 1.78 exactly). Captures from aux passes must never enter the
// palette/world stores: the portrait stage sits ~85 m from the player and a
// portrait-pass capture merged onto the player's meshes produced the mixed
// palettes the stretch veto hid (bones 0-1 at the
// player, rows 2+ at the stage). Ortho banks (CSM caster cascades, square
// aspect) are handled by BankIsOrtho and are NOT aux.
bool BankIsAuxPerspective(uint8_t* base, uint32_t bank) {
  if (BankIsOrtho(base, bank)) {
    return false;
  }
  float n0 = 0.0f;
  float n1 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float a = LoadGuestF32(base, bank + (0 * 4 + i) * 4);
    const float b = LoadGuestF32(base, bank + (1 * 4 + i) * 4);
    if (!(a > -1e6f && a < 1e6f && b > -1e6f && b < 1e6f)) {
      return false;  // implausible bank: let the existing gates decide
    }
    n0 += a * a;
    n1 += b * b;
  }
  if (!(n0 > 1e-12f && n1 > 1e-12f)) {
    return false;
  }
  // aspect < 1.2 (n1/n0 < 1.44): narrower than any screen view.
  return n1 < n0 * 1.44f;
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Guarded bulk copy: skate3::native_scene::GuestTryCopy, moved to
// native/skate3_native_guest_read.cpp so every guest reader shares the one
// correctly-built SEH guard. The compiler-trap documentation
// (volatile fn-ptr + noinline: plain __try{memcpy}__except compiles to an
// UNPROTECTED `jmp memcpy`) moved with it.

}  // namespace
// SEH-guarded single-value guest loads for registry-time walks (the
// loading-screen prewarm): unlike the capture-path walks, whose pointers
// the game is actively rendering from, registry probes dereference
// candidate words that may not be pointers at all, and load-time payloads
// that may not be committed yet. GuestReadableApprox is only a null/small
// filter; these actually survive the fault.
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool GuestTryLoadU64(uint8_t* base, uint32_t addr, uint64_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint64_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 8)) {
    return false;
  }
  *out = __builtin_bswap64(raw);
  return true;
}

// Per-instance world straight from the guest MeshContext: the owning model
// object at ctx+0x30 keeps the instance's row-major 4x4 (translation in row
// 3) at +0xA0. Verified 426/426 against placed captures in a park-editor
// venue, 180-degree-rotated clones included. This is the only per-INSTANCE
// transform source available at capture time: constant banks are shared by
// every clone of a mesh (clones share (ib,vb)), so a bank world can belong
// to whichever clone drew last. Validation mirrors TryRow4x4; callers fall
// back to the bank paths when the owner layout does not match.
bool ReadCtxInstanceWorld(uint8_t* base, uint32_t ctx, float* out) {
  uint32_t owner = 0;
  if (ctx == 0 || !GuestTryLoadU32(base, ctx + 0x30, &owner) ||
      owner < 0x10000) {
    return false;
  }
  uint32_t raw[16];
  if (!GuestTryCopy(raw, REX_RAW_ADDR(owner + 0xA0), sizeof(raw))) {
    return false;
  }
  float f[16];
  for (int i = 0; i < 16; ++i) {
    f[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
    if (!(f[i] > -1e7f && f[i] < 1e7f)) {
      return false;
    }
  }
  if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) {
      return false;
    }
  }
  if (!(f[12] > -20000.f && f[12] < 20000.f && f[13] > -20000.f &&
        f[13] < 20000.f && f[14] > -20000.f && f[14] < 20000.f)) {
    return false;
  }
  std::memcpy(out, f, sizeof(f));
  return true;
}

namespace {

// Guarded bounded C-string read (`out` gets up to cap-1 chars + NUL, tail
// zeroed like the old byte-at-a-time loops). Fast path is one bulk guarded
// copy; a short string right before an unmapped page falls back to
// byte-wise guarded reads so it still resolves.
void GuestTryReadString(uint8_t* base, uint32_t addr, char* out, uint32_t cap) {
  std::memset(out, 0, cap);
  if (addr < 0x10000) {
    return;
  }
  if (GuestTryCopy(out, REX_RAW_ADDR(addr), cap - 1)) {
    const size_t n = strnlen(out, cap - 1);
    std::memset(out + n, 0, cap - n);
    return;
  }
  for (uint32_t k = 0; k + 1 < cap; ++k) {
    uint8_t c;
    if (!GuestTryCopy(&c, REX_RAW_ADDR(addr + k), 1) || c == 0) {
      break;
    }
    out[k] = char(c);
  }
}

// Committed-page check for bulk reads (transient ring memory can be
// partially uncommitted, and resources may be released between the game
// thread capturing an address and the render thread reading it; a blind
// memcpy would fault).
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t size) {
#if defined(_WIN32)
  uint8_t* p = base + addr;
  uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    p = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  (void)base;
  (void)addr;
  (void)size;
  return false;
#endif
}

// Payload fingerprint (FNV-1a over bytes sampled across the whole VB/IB) so
// the renderer re-decodes when streaming replaces or fills in the data at
// this address, including middle-of-buffer fills. Guarded reads: capture-
// path payloads are always resident (the game is drawing from them), but
// the registration prewarm fingerprints meshes whose payload pages may not
// be committed yet; the failure defers the mesh instead of faulting the
// thread. Returns false when the payload is unreadable.
bool ComputeItemFingerprint(uint8_t* base, DrawItem& item) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
  };
  mix(item.vb_addr);
  mix(item.ib_addr);
  mix(item.vb_bytes);
  mix(item.ib_count);
  if (item.skinned) {
    // CPU morph jobs rewrite skinned character VBs in place (see
    // g_morph_epoch): a completed morph invalidates every skinned decode.
    mix(g_morph_epoch.load(std::memory_order_relaxed));
  }
  if (item.vb_bytes >= 8 && item.ib_count >= 4) {
    // All 32 samples under one guard (GuestTryGather64), interleaved
    // vb/ib as before; the mix order and byte order are unchanged.
    uint32_t addrs[32];
    uint64_t raw[32];
    for (uint32_t k = 0; k < 16; ++k) {
      const uint32_t vb_off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
      const uint32_t ib_off = uint32_t(uint64_t(item.ib_count * 2 - 8) * k / 15u) & ~7u;
      addrs[k * 2] = item.vb_addr + vb_off;
      addrs[k * 2 + 1] = item.ib_addr + ib_off;
    }
    const void* src[32];
    for (int i = 0; i < 32; ++i) {
      if (addrs[i] < 0x10000) {
        return false;  // GuestTryLoadU64's null-page refusal
      }
      src[i] = REX_RAW_ADDR(addrs[i]);
    }
    if (!GuestTryGather64(src, 32, raw)) {
      return false;
    }
    for (const uint64_t q : raw) {
      mix(__builtin_bswap64(q));
    }
  }
  item.fingerprint = h;
  return true;
}

// Walk one MeshContext into a DrawItem (geometry, material and draw list;
// world left as identity). Returns false if any pointer in the chain is
// implausible. For dynamic entities this MUST run at RenderMesh hook time;
// the whole chain lives in transient per-frame arenas.
}  // namespace
// DIAGNOSTIC: mesh -> AttribulatorMaterialName, recorded by the material
// walk so the dynamic-item capture can report which materials take the
// dynamic paths (bounded; render/guest threads, own mutex).
std::mutex g_mesh_mat_mutex;
std::unordered_map<uint32_t, std::string> g_mesh_mat_names;
void RecordMeshMaterialName(uint32_t mesh, const char* name) {
  std::lock_guard<std::mutex> lock(g_mesh_mat_mutex);
  if (g_mesh_mat_names.size() > 65536) {
    g_mesh_mat_names.clear();
  }
  g_mesh_mat_names[mesh] = name;
}
std::string MeshMaterialName(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_mesh_mat_mutex);
  const auto it = g_mesh_mat_names.find(mesh);
  return it != g_mesh_mat_names.end() ? it->second : std::string("?");
}

// Parse a guest mesh struct (vertex descriptor, buffers, bbox, material
// channels, payload fingerprint) into the item. The draw list and world
// transform are the caller's responsibility (world starts as identity).
bool BuildItemFromMesh(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
  const uint32_t ib = REX_LOAD_U32(mesh + kMeshIndexBuffer);
  const uint32_t vb = REX_LOAD_U32(mesh + kMeshVertexBuffer);
  if (!GuestReadableApprox(base, vdesc) || !GuestReadableApprox(base, ib) ||
      !GuestReadableApprox(base, vb)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Vertex descriptor: find the stream-0 position element and the first
  // stream-0 texcoord (D3DDECLUSAGE 5) for the diffuse map. Read via ONE
  // guarded bulk copy into scratch: descriptors are runtime renderengine
  // objects, and the registration prewarm can reach a mesh before its
  // descriptor is initialized (a raw read there faults the thread; the
  // capture path also gets marginally faster than the per-field volatile
  // loads).
  uint32_t desc_head[3];
  if (!GuestTryCopy(desc_head, REX_RAW_ADDR(vdesc), sizeof(desc_head))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t num_elements = BSwap32(desc_head[2]) >> 16;  // u16 at +8
  if (num_elements == 0 || num_elements > 32) return false;
  // Element table at +0x10 (16 bytes per element) followed by the stride
  // byte at +(num_elements + 1) * 16.
  uint8_t desc_tab[32 * 16 + 1];
  const uint32_t desc_tab_bytes = num_elements * 16 + 1;
  if (!GuestTryCopy(desc_tab, REX_RAW_ADDR(vdesc + 0x10), desc_tab_bytes)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto elem_u16 = [&](uint32_t i, uint32_t off) -> uint32_t {
    return (uint32_t(desc_tab[i * 16 + off]) << 8) | desc_tab[i * 16 + off + 1];
  };
  const auto elem_u32 = [&](uint32_t i, uint32_t off) -> uint32_t {
    uint32_t v;
    std::memcpy(&v, desc_tab + i * 16 + off, 4);
    return BSwap32(v);
  };
  bool have_pos = false;
  bool have_bw = false;
  bool have_bi = false;
  item.uv_offset = 0;
  item.uv_fmt = 0;
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.uv3_offset = 0;
  item.uv3_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.normal_offset = 0;
  item.normal_fmt = 0;
  item.tangent_offset = 0;
  item.binormal_offset = 0;
  item.tb_fmt = 0;
  item.tan_fmt = 0;
  bool have_tan = false;
  bool have_bin = false;
  struct TexcoordElem {
    uint16_t offset;
    uint8_t fmt;
    uint8_t index;
  };
  TexcoordElem tc[4] = {};
  uint32_t tc_count = 0;
  item.skinned = false;
  for (uint32_t i = 0; i < num_elements; ++i) {
    const uint32_t stream = elem_u16(i, 0);
    const uint32_t usage = desc_tab[i * 16 + 9];
    if (stream != 0) continue;
    if (usage == 0 && !have_pos) {
      item.pos_offset = uint16_t(elem_u16(i, 2));
      item.pos_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      have_pos = true;
    } else if (usage == 3 && item.normal_fmt == 0) {
      const uint8_t fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      if (fmt == 16) {  // k_10_11_11 packed normal
        item.normal_offset = uint16_t(elem_u16(i, 2));
        item.normal_fmt = fmt;
      }
    } else if (usage == 5) {
      // Texcoords are matched to the shader by USAGE INDEX (the declaration
      // element's UsageIndex byte, +10), not by their order in the table.
      // Collected here, assigned after the loop.
      if (tc_count < 4) {
        tc[tc_count++] = {uint16_t(elem_u16(i, 2)),
                          uint8_t(elem_u32(i, 4) & 0x3F), desc_tab[i * 16 + 10]};
      }
    } else if (usage == 1 && !have_bw) {  // blend weights u8x4
      item.bw_offset = uint16_t(elem_u16(i, 2));
      have_bw = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 2 && !have_bi) {  // blend indices u8x4
      item.bi_offset = uint16_t(elem_u16(i, 2));
      have_bi = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 6 && !have_tan &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // tangent k_10_11_11
      item.tangent_offset = uint16_t(elem_u16(i, 2));
      have_tan = true;
    } else if (usage == 7 && !have_bin &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // binormal k_10_11_11
      item.binormal_offset = uint16_t(elem_u16(i, 2));
      have_bin = true;
    }
  }
  {
    // uv = TEXCOORD0, uv2 = TEXCOORD1 by usage index; declaration order
    // only as the fallback. The environment.transparent vehicle-wheel cards
    // list their lightmap texcoord first: taken in table order, the diffuse
    // sampled the wheel atlas at the lightmap unwrap (several overlapping
    // wheel images per card, with the card's cut-out alpha in the wrong
    // places).
    int i0 = -1, i1 = -1, i3 = -1;
    for (uint32_t k = 0; k < tc_count; ++k) {
      if (tc[k].index == 0 && i0 < 0) i0 = int(k);
      else if (tc[k].index == 1 && i1 < 0) i1 = int(k);
      else if (tc[k].index == 3 && i3 < 0) i3 = int(k);
    }
    if (i0 < 0) {
      for (uint32_t k = 0; k < tc_count; ++k) {
        if (int(k) != i1) { i0 = int(k); break; }
      }
    }
    if (i1 < 0) {
      for (uint32_t k = 0; k < tc_count; ++k) {
        if (int(k) != i0) { i1 = int(k); break; }
      }
    }
    if (i0 >= 0) {
      item.uv_offset = tc[i0].offset;
      item.uv_fmt = tc[i0].fmt;
    }
    if (i1 >= 0) {
      item.uv2_offset = tc[i1].offset;
      item.uv2_fmt = tc[i1].fmt;
    }
    // Usage index 3 = the environment.decal art's OWN UV set (the game's VS
    // routes it to the interpolator whose .xy its PS samples tf4 with).
    // Without it the decal sampled the .zw half of TEXCOORD0, which on these
    // 5-element / stride-36 layouts is not the decal unwrap at all: the
    // fetch landed on a transparent part of the atlas page (alpha 0), so the
    // paint/grime composite was a no-op and the surfaces rendered perfectly
    // clean. The decal_tileable layouts (4 elements / stride 32) carry no
    // usage-3 element and keep the half4 .zw fallback.
    if (i3 >= 0) {
      item.uv3_offset = tc[i3].offset;
      item.uv3_fmt = tc[i3].fmt;
    }
    if (i0 > 0) {
      static std::atomic<uint32_t> s_tc_swaps{0};
      if (s_tc_swaps.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("native-scene: texcoord table order differs from usage index "
                    "(mesh={:08X}, TEXCOORD0 is element {} of {})",
                    mesh, i0, tc_count);
      }
    }
  }
  if (have_tan && have_bin) {
    item.tb_fmt = 16;
  }
  if (have_tan) {
    item.tan_fmt = 16;  // tangent-only layouts (water meshes) use this
  }
  if (!have_pos) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.skinned = have_bw && have_bi;
  item.stride = desc_tab[num_elements * 16];  // byte at vdesc+(num+1)*16
  if (item.stride == 0) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Mesh BBox = two Vector4s at +0x00/+0x10. Reject NaN/absurd bounds.
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = LoadGuestF32(base, mesh + axis * 4);
    const float hi = LoadGuestF32(base, mesh + 0x10 + axis * 4);
    if (!(hi - lo < 50000.0f)) {
      g_rej_bbox.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    item.bbox_min[axis] = lo;
    item.bbox_max[axis] = hi;
  }

  item.mesh = mesh;
  item.pending = false;
  item.cloth_quads = false;
  item.vb_obj = vb;
  item.ib_obj = ib;
  // Guarded: the registration prewarm can reach buffer objects before they
  // finish initializing (the capture path only ever sees live ones).
  uint32_t vb_words[3] = {}, ib_words[3] = {};
  if (!GuestTryCopy(vb_words, REX_RAW_ADDR(vb + kBufferPhysAddr), sizeof(vb_words)) ||
      !GuestTryCopy(ib_words, REX_RAW_ADDR(ib + kBufferPhysAddr), sizeof(ib_words))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Samplers as bound on an actual water_defaultPS draw (recorded by the
  // constant capture on the guest thread): tf5 carries the spec/reflection
  // masks. tf3 turned out to be the LIGHTMAP, which we already sample.
  item.refract_tex = g_waterdef_tf3;
  item.water_mask = g_waterdef_tf5;
  item.vb_addr = BSwap32(vb_words[0]) & 0xFFFFFFFC;
  item.vb_bytes = BSwap32(vb_words[2]);
  item.ib_addr = BSwap32(ib_words[0]) & 0xFFFFFFFC;
  item.ib_count = BSwap32(ib_words[2]);
  if (item.vb_addr == 0 || item.ib_addr == 0 || item.vb_bytes == 0 ||
      item.ib_count == 0 || item.vb_bytes % item.stride != 0) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Material channels: resolve the "diffuse" and "lightmap" texture guids to
  // registered texture objects.
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.macro_tex = 0;
  item.macro_scale = 1.0f;
  item.macro_opacity = 1.0f;
  item.scroll_u = 0.0f;
  item.scroll_v = 0.0f;
  item.decal_art = 0;
  item.hair = false;
  item.char_family = 0;
  item.char_skin = false;
  item.char_face = false;
  item.face_inj_valid = false;
  item.char_layer_valid = false;
  item.sss = false;
  item.char_cg_valid = false;
  item.hair_alpha_tex = 0;
  item.dirt_tex = 0;
  item.ropa = false;
  item.decal = false;
  item.decal_tileable = false;
  item.decal_addr = 0;
  item.decal_fade = -1.0f;
  item.decal2 = false;
  item.hom = false;
  item.decal2_addr = 0;
  std::memset(item.decal2_fetch, 0, sizeof(item.decal2_fetch));
  item.transparent = false;
  item.water = false;
  item.water_flowing = false;
  item.water_opaque = false;
  item.water_ocean = 0;
  item.water_normal = 0;
  item.water_normal2 = 0;
  item.water_env = 0;
  item.unlit = false;
  item.env_family = 0;
  item.dynobj = 0;
  item.spec_tex = 0;
  item.tint[0] = item.tint[1] = item.tint[2] = item.tint[3] = 0.0f;
  // Material header + channel array read via guarded bulk copies: the
  // registration prewarm walks materials while their arena is still
  // loading, where raw reads fault the thread. (The capture path gets the
  // same copies; one memcpy per material beats ~8 volatile loads per
  // channel anyway.)
  const uint32_t material = REX_LOAD_U32(mesh + kMeshMaterial);
  uint32_t mat_head[3] = {};
  if (GuestReadableApprox(base, material) &&
      GuestTryCopy(mat_head, REX_RAW_ADDR(material), sizeof(mat_head))) {
    const uint32_t num_channels = BSwap32(mat_head[0]);
    const uint32_t channels = BSwap32(mat_head[2]);
    uint8_t chan_buf[32 * 0x20];
    if (num_channels != 0 && num_channels <= 32 && GuestReadableApprox(base, channels) &&
        GuestTryCopy(chan_buf, REX_RAW_ADDR(channels), num_channels * 0x20)) {
      const auto chan_u32 = [&](uint32_t idx, uint32_t off) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, chan_buf + idx * 0x20 + off, 4);
        return BSwap32(v);
      };
      for (uint32_t i = 0; i < num_channels; ++i) {
        const uint32_t name = chan_u32(i, 0);
        char text[20];
        GuestTryReadString(base, name, text, sizeof(text));
        if (text[0] == '\0') continue;
        uint32_t* slot = nullptr;
        if (std::memcmp(text, "diffuse", 8) == 0) {
          slot = &item.diffuse_tex;
        } else if (std::memcmp(text, "lightmap", 9) == 0) {
          slot = &item.lightmap_tex;
        } else if (std::memcmp(text, "macrooverlay", 13) == 0) {
          slot = &item.macro_tex;
        } else if (std::memcmp(text, "decal", 6) == 0) {
          slot = &item.decal_art;
        } else if (std::memcmp(text, "normal", 7) == 0) {
          // Exact match only. Consumed by the water branches (the flowing
          // ripple map / the ocean's first PCA component); other families
          // ignore it.
          slot = &item.water_normal;
        } else if (std::memcmp(text, "normal2", 8) == 0) {
          // The ocean's second PCA normal component map.
          slot = &item.water_normal2;
        } else if (std::memcmp(text, "decal2", 7) == 0) {
          // CAC cloth dirt/grime layer (cac_cloth[_ropa]_defaultPS tf5):
          // albedo = lerp(D^2 * tint, lerp(.., dirt.rgb, dirt.a), c22.y).
          // Env decal2 materials carry the channel too; only the character
          // cloth branch consumes it.
          slot = &item.dirt_tex;
        } else if (std::memcmp(text, "alpha", 6) == 0) {
          // Hair strand coverage (cac_hair/defaulthair tf5, sampled at the
          // second texcoord), the hair alpha-blend term.
          slot = &item.hair_alpha_tex;
        } else if (std::memcmp(text, "environment", 12) == 0) {
          // Environment CUBE map, the water and environment.reflective*
          // reflection term.
          slot = &item.water_env;
        } else if (std::memcmp(text, "detail", 7) == 0) {
          // Exact match ("detailNormalUVScale" is a different channel).
          // Constant detail texture folded into the fam 5/6 normal
          // composition (see DrawItem::detail_tex).
          slot = &item.detail_tex;
        } else if (std::memcmp(text, "specular", 9) == 0 ||
                   std::memcmp(text, "noise", 6) == 0) {
          // Spec/eccentricity/reflection-mask map (environment families) /
          // the animated.tree "noise" tint map, bound at t4 on families
          // that carry no decal art (see DrawItem::spec_tex).
          slot = &item.spec_tex;
        } else if (std::memcmp(text, "detailNormalUVScale", 20) == 0) {
          // Shader-constant channel (the game's VS c8.x, typically 8): the
          // detail normal map's UV multiplier, consumed by the v2 kd term.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (f > 0.0f && f < 1e6f) {
            item.detail_scale = f;
          }
          continue;
        } else if (std::memcmp(text, "uAnimationSpeed", 16) == 0 ||
                   std::memcmp(text, "vAnimationSpeed", 16) == 0) {
          // Shader-constant channels (the game's VS c7.x / c8.x): UV scroll
          // speed in texcoords per second, consumed by the fam-14
          // scrollincandescent branch. Signed; the direction is authored.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (std::fabs(f) < 1e6f) {  // false for NaN too
            (text[0] == 'u' ? item.scroll_u : item.scroll_v) = f;
          }
          continue;
        } else if (std::memcmp(text, "macroOverlayUVScale", 20) == 0 ||
                   std::memcmp(text, "macroOverlayOpacity", 20) == 0) {
          // Shader-constant channel: the float lives in the first guid word.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (f > 0.0f && f < 1e6f) {
            (text[12] == 'U' ? item.macro_scale : item.macro_opacity) = f;
          }
          continue;
        } else if (std::memcmp(text, "Attribul", 8) == 0) {
          // AttribulatorMaterialName: chan+0x18 is the material name string.
          // "character.hair" marks the grayscale hair that needs the
          // per-character tint from the pixel constant bank; "sky.*" draws
          // fullbright; "character.cloth_ropa" is the Ropa cloth-sim VS
          // variant (flag-row-switched skinned/rigid, see CaptureSkinnedState).
          const uint32_t s = chan_u32(i, 0x18);
          if (GuestReadableApprox(base, s)) {
            // 40 bytes: "character.livingworld_vehicles_glass" (36 chars) is
            // the longest name that must be distinguishable; the previous
            // 28-byte buffer truncated both vehicle names into the plain
            // "livingworld" pedestrian prefix.
            char mat_name[40];
            GuestTryReadString(base, s, mat_name, sizeof(mat_name));
            RecordMeshMaterialName(mesh, mat_name);
            // character.*_ropa = the Ropa cloth-sim VS variant (flag-row
            // switched skinned/rigid, see CaptureSkinnedState). The suffix
            // composes with the base family name: cloth_ropa (player tees),
            // default_cloth_ropa (NPC jackets), hair_ropa and
            // default_hair_ropa all exist in the attrib table; so detect it
            // generically and STRIP it, letting the family idioms below see
            // the base name. Matching only the player's character.cloth_ropa
            // left NPC ropa garments on the generic skinned path, where
            // every palette home is refused: the layout's guess register is
            // this VS's FLAG row (scores 0), and the true +1 home fails the
            // (0,0,0,w)/(1,0,0,0) parameter-row signature because this
            // variant's row is (1, junk, junk, junk), the persistent
            // invisible-torso NPC (observed: a
            // character.default_cloth_ropa mesh refused all three captures every
            // frame while the draw-time banks pass the gate at c5/c8).
            item.ropa = false;
            {
              const size_t len = strnlen(mat_name, sizeof(mat_name));
              if (len >= 5 && len < sizeof(mat_name) &&
                  std::memcmp(mat_name, "character.", 10) == 0 &&
                  std::memcmp(mat_name + len - 5, "_ropa", 5) == 0) {
                item.ropa = true;
                mat_name[len - 5] = '\0';
              }
            }
            item.hair = std::memcmp(mat_name, "character.hair", 15) == 0;
            item.unlit = std::memcmp(mat_name, "sky.", 4) == 0;
            // Character shading family (see DrawItem::char_family). Order
            // matters: "default_hair" before the "default" prefix, exact
            // "hair" after (memcmp includes the NUL for exact names).
            if (std::memcmp(mat_name, "character.", 10) == 0) {
              const char* sub = mat_name + 10;
              if (std::memcmp(sub, "default_hair", 13) == 0) {
                item.char_family = 5;
              } else if (std::memcmp(sub, "default", 7) == 0) {
                item.char_family = 1;
              } else if (std::memcmp(sub, "hair", 5) == 0) {
                item.char_family = 4;
              } else if (std::memcmp(sub, "livingworld_vehicles_glass", 27) == 0) {
                item.char_family = 7;  // reflection-only blended windows
              } else if (std::memcmp(sub, "livingworld_vehicles", 21) == 0) {
                item.char_family = 6;  // vehicle.fx paint/spec/cube body
              } else if (std::memcmp(sub, "livingworld", 11) == 0) {
                item.char_family = 3;
              } else {
                item.char_family = 2;  // skin/face/cloth/leather/shift/ropa
              }
              // character.alpha (exact): translucent accessory pieces
              // (sunglass lenses), alpha from the "alpha" channel texture
              // at uv2, blended after every opaque piece (see
              // DrawItem::char_alpha; captured draw order:
              // cac_alpha draws after all opaque CAC, before hair).
              item.char_alpha = std::memcmp(sub, "alpha", 6) == 0;
              // character.skin / character.face: cacstamp_skin_defaultPS,
              // albedo x the composited skin at tf6(uv2) (see
              // DrawItem::char_skin).
              item.char_skin = item.char_family == 2 &&
                               std::memcmp(sub, "skin", 4) == 0;
              item.char_face = item.char_family == 2 &&
                               std::memcmp(sub, "face", 4) == 0;
            }
            // environment.decal / environment.decal_tileable: graffiti and
            // painted-branding overlay meshes (see DrawItem::decal).
            // decal_tileable is only the FALLBACK addressing (wrap) until
            // the game's own tf4 sampler state for the mesh is observed
            // (DrawItem::decal_addr); the two families share identical
            // shader microcode, so the name alone does not decide tiling.
            item.decal = std::memcmp(mat_name, "environment.decal", 17) == 0;
            item.decal_tileable =
                std::memcmp(mat_name, "environment.decal_tileable", 26) == 0 ||
                std::memcmp(mat_name, "environment.decal_simple_tileable", 33) == 0;
            item.decal2 = std::memcmp(mat_name, "environment.decal2", 18) == 0;
            item.hom = std::memcmp(mat_name, "character.default_hom", 22) == 0;
            // environment.transparent: alpha-blended world geometry (mist
            // sheets, glass, fences); see DrawItem::transparent.
            item.transparent =
                std::memcmp(mat_name, "environment.transparent", 23) == 0;
            // water.* (canal) and ocean.* (the sea): transparent sub-pass
            // with the dedicated water shading branch (see DrawItem::water).
            // ocean.default has NO diffuse channel at all (ocean.fx computes
            // color purely from the environment cube x lightmap x fresnel);
            // without this branch it rendered as an 8 km white plane (white
            // fallback diffuse x near-white ocean lightmap x2).
            item.water = std::memcmp(mat_name, "water.", 6) == 0 ||
                         std::memcmp(mat_name, "ocean.", 6) == 0;
            // water.flowing* = the flowingwateralpha shader family (canal /
            // waterfall). Takes the exact water branch when the frame's
            // m_params rows are captured (see FrameScene::water_rows).
            item.water_flowing =
                std::memcmp(mat_name, "water.flowing", 13) == 0;
            item.water_opaque = std::memcmp(mat_name, "water.flowing", 14) == 0;
            // water.alpha (NOT water.flowingalpha, which the flowing
            // branch owns): the alpha-output variant of the same shader.
            item.water_alpha =
                std::memcmp(mat_name, "water.alpha", 11) == 0;
            // ocean.default / ocean.reflection: the sea surface and its
            // baked horizon reflection sheet (see DrawItem::water_ocean).
            if (std::memcmp(mat_name, "ocean.default", 14) == 0) {
              item.water_ocean = 1;
            } else if (std::memcmp(mat_name, "ocean.reflection", 17) == 0) {
              item.water_ocean = 2;
            }
            // Exact world-shading family (see DrawItem::env_family). The
            // attrib <-> pixel-shader-family mapping is 1:1; the shading
            // models were verified per-pixel against the game's own shaders.
            const auto is = [&](const char* s, size_t n) {
              return std::memcmp(mat_name, s, n) == 0;
            };
            if (is("environment.default", 20) || is("model_default", 14)) {
              // model_default: the game draws it with
              // baseenvironment_defaultPS, like environment.default (shader
              // census: its only main-pass shader).
              item.env_family = 1;
            } else if (is("environmentsimple.default", 26)) {
              item.env_family = 2;
            } else if (is("environment.decal_tileable", 26)) {
              item.env_family = 4;  // includes decal_tileable_simple
            } else if (is("environment.decal_simple_tileable", 33)) {
              // decalenvironment_simple_tileable_defaultPS: the decal family
              // without the detail / macro-overlay maps (same constants c0..c11,
              // same tf0 cascade + tf3 4-tap lightmap + s4 decal art; diffuse /
              // normal / specular at s5..s7 instead of s6/s8/s9). Retail's
              // world data uses it for ground decals; it matched no family and
              // took the legacy fallback (no family colour, legacy shadowing:
              // the dark ground decals with dynamic shadows on).
              item.env_family = 4;
            } else if (is("environment.decal_simple", 25)) {
              // decalenvironment_simple_defaultPS (see above).
              item.env_family = 3;
            } else if (is("environment.decal", 18) ||
                       is("environment.decal2", 19)) {
              // decal2environment_defaultPS: the same base diffuse + decal
              // art (uv3, by its alpha) + macro/lightmap/kd/spec chain as
              // decalenvironment (verified against its ucode); its second
              // decal layer rides a third texcoord not decoded yet. The
              // exact-name compare (NUL included) missed "decal2", so
              // these meshes (parked-car wheel cards) took the legacy
              // fallback and drew as opaque grey squares.
              item.env_family = 3;
            } else if (is("environment.reflective_simple", 30)) {
              item.env_family = 6;
            } else if (is("environment.reflective_trans", 29)) {
              // transparentenvironmentreflective: the sloped glass canopy /
              // awning panels. Blended in the sorted alpha sub-pass with the
              // fam-5 shading minus kd/macro, premultiplied body, out a^2
              // (model verified 0.0-error against the ucode).
              item.env_family = 13;
            } else if (is("environment.reflective", 23)) {
              item.env_family = 5;
            } else if (is("environmentsimple.alphatest", 28)) {
              item.env_family = 7;
            } else if (is("environmentsimple.diffuse", 26)) {
              item.env_family = 8;
            } else if (is("tree.default", 13)) {
              item.env_family = 9;
            } else if (is("animated.tree", 14)) {
              item.env_family = 10;
            } else if (is("proxyworld.", 11)) {
              item.env_family = 11;
            } else if (is("incandescent.default", 21)) {
              item.env_family = 12;
            } else if (is("incandescent.backlituvscroll", 29)) {
              // scrollincandescent.fx: emissive diffuse scrolled by
              // g_fAnimationTime x the u/vAnimationSpeed channels (the
              // stadium LED chyron band).
              item.env_family = 14;
            } else if (is("advertisement.default", 22)) {
              // advertisement_defaultPS: diffuse x floored/shadow-clamped
              // lightmap x multiplier through its own tone curve (no fog,
              // no kd). See the fam-15 branch in scene.hlsl.
              item.env_family = 15;
            } else if (is("trafficlight.", 13)) {
              // trafficlight.one / .two: trafficlight_*_defaultVS/PS (both
              // ucode-identical), see DrawItem::traffic_c7.
              item.env_family = 17;
            } else if (is("environment.transparent", 24)) {
              // transparentenvironment_defaultPS (sorted alpha pass; see the
              // fam-16 branch in scene.hlsl). Parked-vehicle wheels ride
              // this material; the legacy empirical transparent shading
              // rendered them wrong.
              item.env_family = 16;
            }
            // DIAGNOSTIC: an environment material no family claims takes the
            // legacy fallback; report each such name once.
            if (item.env_family == 0 && std::memcmp(mat_name, "environment", 11) == 0) {
              static std::mutex s_unk_mutex;
              static std::unordered_set<std::string> s_unk;
              std::lock_guard<std::mutex> lock(s_unk_mutex);
              if (s_unk.size() < 64 && s_unk.insert(mat_name).second) {
                REXLOG_WARN("native-scene: environment material '{}' matched no shading "
                            "family (legacy fallback)",
                            mat_name);
              }
            }
            // dynamicobject.fx props (dispensers, dumpsters, benches, cans):
            // rigid movable objects with their own dual-shadow lit PS
            // (model verified exact against the game's own pixel shader).
            // Separate from the world env families; the lighting rows come
            // from the draw's PS bank, not the frame-global world rows.
            if (is("dynamicobject.alphatest", 24)) {
              item.dynobj = 2;
            } else if (is("dynamicobject", 13)) {
              item.dynobj = 1;
            }
            // environmentpark.*: skate-park editor pieces (ramps, rails,
            // funboxes). They ride the dynobj enum (4/5/6) because they are
            // dynamic objects that happen to move only in the editor, and
            // all three park pixel shaders do the DUAL shadow fetch
            // (cascades + baked world shade) exactly like
            // dynamicobject_defaultPS -- unlike every static family, which
            // does one fetch and has no world term:
            //   ps_environmentparkdiffuse_defaultPS    tf0 tf1 tf2 tf3 tf4
            //   ps_environmentparkalphatest_defaultPS  tf0 tf1 tf3 tf4 tf5 tf6
            //   ps_environmentparkdecal_defaultPS      tf0 tf1 tf2 ... tf7
            //   ps_baseenvironment_defaultPS           tf0 tf3 ... tf8 (no tf1)
            // Previously these matched no family at all (env_family 0,
            // dynobj 0), so they fell through to the legacy static fallback:
            // derivative face shading, one cascade fetch, no world term and
            // no lightmap to stand in for it. That is why they neither cast
            // nor received shadows.
            //
            // Their lighting rows come from FrameScene::park_rows, captured
            // from the park shaders' OWN banks -- an earlier attempt reused
            // dynobj = 1 and hence g_dynobj_rows, which are refreshed only by
            // dynamicobject_defaultPS draws; in a park-only view they went
            // stale and the pieces flickered between lit and shadowed. The
            // layouts differ per variant too, so the offsets cannot be
            // shared (see CapturePark).
            else if (is("environmentpark.alphatest", 26)) {
              item.dynobj = 5;
            } else if (is("environmentpark.decal", 22)) {
              item.dynobj = 6;
            } else if (is("environmentpark.", 16)) {
              item.dynobj = 4;
            }
          }
          continue;
        }
        if (slot != nullptr && *slot == 0) {
          // Prefer the channel's live stream record (chan+0x1C -> word 0 =
          // the renderengine::Texture actually bound): runtime-composed
          // customization textures (CAS face/skin, shoes, deck, wheels)
          // are never registered under an asset GUID. Validate via the
          // fetch-constant type bits before trusting the pointer.
          // Guarded: the prewarm walks materials whose stream records /
          // texture objects may not be loaded yet.
          const uint32_t stream = chan_u32(i, 0x1C);
          uint32_t tex = 0, tex_w0 = 0;
          if (GuestTryLoadU32(base, stream, &tex) &&
              GuestTryLoadU32(base, tex + 7 * 4, &tex_w0) && (tex_w0 & 3u) == 2u) {
            *slot = tex;
          }
          if (*slot == 0) {
            const uint64_t guid =
                (uint64_t(chan_u32(i, 0x10)) << 32) | chan_u32(i, 0x14);
            std::lock_guard<std::mutex> lock(g_texture_map_mutex);
            auto it = g_texture_map.find(guid & kGuidMask);
            if (it != g_texture_map.end()) {
              *slot = it->second;
            }
          }
        }
        // No early-out: channel order varies per material and the float
        // channels ride AFTER their texture (macroOverlayUVScale follows
        // macrooverlay on the plaza asphalt, breaking once the textures
        // resolved left the macro tiling at 1.0 instead of 0.3).
      }
    }
  }

  // Payload fingerprint: see ComputeItemFingerprint (the item cache
  // refreshes it on its own cadence).
  if (!ComputeItemFingerprint(base, item)) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Transform (identity for world geometry, instance matrix for props;
  // characters get their bone array's first matrix, which roughly places
  // them until skinning is implemented).
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  return true;
}

namespace {

// Streamed-artwork override (event ads): the Massive ad system rebinds
// texture fetch slots at draw time to show the current event-ad art in
// place of the asset's default poster. Two observed shapes: a 16x16
// min-mip stub channel whose real art exists ONLY at draw time (the
// ace-of-spades "Big Event" grid), and a full-size default poster (the
// letter-writing frames) whose diffuse is wholesale REPLACED with the
// current ad (the MONDO "THE NEW VIDEO" portrait). Adopt the fetch
// slot-4 words recorded at this mesh's LAST indexed draw this frame,
// but only if that draw's slot 3 is this item's own lightmap
// (mirror-masked base compare), which proves the recorded fetch state
// belongs to the mesh's MAIN-pass draw (the z-prepass leaves another
// material's bindings). Slot targets are family-specific (Skate 2
// shader source): environmentsimple.* sample the diffuse at s4;
// environment.decal samples its decal overlay art at s4 (diffuse rides
// s6); environment.default has DETAIL at s4, so its diffuse adoption
// stays stub-gated; never adopt a detail texture over real poster art.
// Per-frame state (g_frame_draw_fetch), so this runs on every item build;
// it is NOT part of the cacheable item core.
void AdoptDrawFetchOverrides(uint8_t* base, DrawItem& item) {
  if (!REXCVAR_GET(skate3_native_render_scene_draw_fetch_adopt)) {
    return;  // see the cvar (on by default on retail)
  }
  if (item.env_family != 0 && item.diffuse_tex != 0 && item.lightmap_tex != 0) {
    // Physical base of a fetch word-1, collapsing the 0xA0000000-style
    // cached/uncached mirrors (the channel object records the mirror
    // address, the draw-time fetch constant the plain one).
    const auto phys_base = [](uint32_t w1) { return w1 & 0x1FFFF000u; };
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    const std::array<uint32_t, 12>* fit =
        g_frame_draw_fetch.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
    if (fit != nullptr) {
      // Texture-object reads only on a fetch-map hit, and guarded: the map
      // is empty on prewarm/loading frames, and the GUID registry can
      // resolve to a freed previous-map object there (raw reads faulted).
      uint32_t lm_w1 = 0;
      const uint32_t* slot3 = fit->data();
      const uint32_t* slot4 = fit->data() + 6;
      const uint32_t art_w = (slot4[2] & 0x1FFFu) + 1;
      const uint32_t art_h = ((slot4[2] >> 13) & 0x1FFFu) + 1;
      const bool main_pass = (slot3[0] & 3u) == 2u && (slot4[0] & 3u) == 2u &&
                             GuestTryLoadU32(base, item.lightmap_tex + 8 * 4, &lm_w1) &&
                             phys_base(slot3[1]) == phys_base(lm_w1);
      // Dimension floor: never adopt a placeholder-sized s4 (an idle ad
      // rotation slot) over whatever the channel resolves to.
      //
      // Own-channel guard: on families whose s4 carries a surface map
      // rather than art (environment.default binds its detail normal map
      // there), the stub gate alone is not enough. A material demoted by
      // the streamer has a stub-sized diffuse too, and adopting s4 then
      // serves the material's OWN detail/spec/normal map as the diffuse
      // (distant demoted tree trunks rendered as their flat blue normal
      // map). Real event-ad art is never one of the material's own channel
      // textures, so any s4 that matches one is a normal binding, not a
      // rebind worth adopting.
      const auto s4_is_own_channel = [&](uint32_t chan_tex) {
        uint32_t own_w1 = 0;
        return chan_tex != 0 &&
               GuestTryLoadU32(base, chan_tex + 8 * 4, &own_w1) &&
               phys_base(slot4[1]) == phys_base(own_w1);
      };
      const bool s4_is_own = s4_is_own_channel(item.detail_tex) ||
                             s4_is_own_channel(item.spec_tex) ||
                             s4_is_own_channel(item.water_normal);
      if (main_pass && !s4_is_own && (art_w >= 32 || art_h >= 32)) {
        if (item.env_family == 3) {
          uint32_t da_w1 = 0;
          if (item.decal_art != 0 &&
              GuestTryLoadU32(base, item.decal_art + 8 * 4, &da_w1) &&
              phys_base(slot4[1]) != phys_base(da_w1)) {
            std::memcpy(item.decal_fetch, slot4, 6 * sizeof(uint32_t));
          }
        } else {
          uint32_t diff_w2 = 0, diff_w1 = 0;
          if (!GuestTryLoadU32(base, item.diffuse_tex + 9 * 4, &diff_w2) ||
              !GuestTryLoadU32(base, item.diffuse_tex + 8 * 4, &diff_w1)) {
            diff_w2 = 0;
            diff_w1 = slot4[1];  // unreadable diffuse object: adopt nothing
          }
          const uint32_t diff_w = (diff_w2 & 0x1FFFu) + 1;
          const uint32_t diff_h = ((diff_w2 >> 13) & 0x1FFFu) + 1;
          const bool diff_stub = diff_w <= 32 && diff_h <= 32;
          const bool s4_is_diffuse = item.env_family == 2 ||
                                     item.env_family == 7 ||
                                     item.env_family == 8;
          if ((s4_is_diffuse || diff_stub) &&
              phys_base(slot4[1]) != phys_base(diff_w1)) {
            std::memcpy(item.diffuse_fetch, slot4, 6 * sizeof(uint32_t));
          }
        }
      }
    }
  }

}

// The current item's g_islands entries (one per island, <= 16), resolved
// once per item build under g_palette_mutex by BuildItemGeometry and read
// by the Apply* functions below while that lock is held. Guest render
// thread only.
std::vector<IslandState*> g_item_islands;
std::vector<uint64_t> g_item_island_keys;  // DrawKey of each g_item_islands entry

// Decal art binding (DrawItem::decal_addr / decal_fetch): register this
// decal item's buffers so the indexed-draw hook records the tf4 fetch
// constant of its main-pass draw, and adopt whatever the game bound there
// last. The game picks wrap vs clamp per material through its Attribulator
// sampler-state definitions (cStateFactory::FindStateInAttribulator
// <cSamplerStateDefinition>), not through the shader (decalenvironment and
// decalenvironment_tileable are the same microcode) and not through the
// texture header (renderengine rewrites all three clamp fields on every
// bind). The TEXTURE it binds there is likewise the ground truth: when it is
// not the material's `decal` channel object (streamed / rotated art), the
// draw-time words replace it. Per-frame, never cached.
void ApplyDecalDrawAddressing(uint8_t* base, DrawItem& item) {
  item.decal_addr = 0;
  item.decal_fade = -1.0f;
  item.decal2_addr = 0;
  std::memset(item.decal2_fetch, 0, sizeof(item.decal2_fetch));
  if (!item.decal) {
    return;
  }
  uint32_t words[6] = {};
  uint32_t words2[6] = {};
  float fade_floor = -1.0f;
  // Register every island of the item (the culled list varies per frame)
  // and adopt the first one the hook has seen drawn (ForEachIsland: one map
  // operation per island shared with the other per-island states).
  {
    bool got = false;
    for (IslandState* st : g_item_islands) {
      st->reg |= kIslDecal;
      const DecalDrawAddr& e = st->decal;
      if (!got && e.words[1] != 0) {
        std::memcpy(words, e.words, sizeof(words));
        std::memcpy(words2, e.words2, sizeof(words2));
        fade_floor = e.fade_floor;
        got = true;
      }
    }
  }
  item.decal_fade = fade_floor;
  // decal2 layer 2: drawn only from the game's own tf5 binding (texture and
  // sampler state), never guessed.
  if (item.decal2 && words2[1] != 0) {
    std::memcpy(item.decal2_fetch, words2, sizeof(words2));
    item.decal2_addr = uint16_t(0x8000u | ((words2[0] >> 10) & 7u) |
                                (((words2[0] >> 13) & 7u) << 3) | ((words2[5] & 3u) << 6));
  }
  if (words[1] == 0) {
    return;  // not observed yet: the family default addressing applies
  }
  item.decal_addr = uint16_t(0x8000u | ((words[0] >> 10) & 7u) |
                             (((words[0] >> 13) & 7u) << 3) | ((words[5] & 3u) << 6));
  // A different texture than the channel's own object (physical base compare,
  // collapsing the cached/uncached mirrors): serve the game's binding.
  uint32_t art_w1 = 0;
  const bool own_art = item.decal_art != 0 &&
                       GuestTryLoadU32(base, item.decal_art + 8 * 4, &art_w1) &&
                       (art_w1 & 0x1FFFF000u) == (words[1] & 0x1FFFF000u);
  if (!own_art) {
    std::memcpy(item.decal_fetch, words, sizeof(words));
  }
}

// trafficlight per-draw constants (DrawItem::traffic_c7): register this
// item's islands for the hook and adopt the first one it has seen drawn.
// Per-frame, never cached.
void ApplyTrafficDrawState(DrawItem& item) {
  item.traffic_seen = false;
  item.traffic_c3y = 1.0f;
  std::memset(item.traffic_c7, 0, sizeof(item.traffic_c7));
  if (item.env_family != 17) {
    return;
  }
  for (IslandState* st : g_item_islands) {
    st->reg |= kIslTraffic;
    const TrafficDrawState& t = st->traffic;
    if (!item.traffic_seen && t.seen) {
      std::memcpy(item.traffic_c7, t.c7, sizeof(t.c7));
      item.traffic_c3y = t.c3y;
      item.traffic_seen = true;
    }
  }
}

// Hall of Meat PS constants (DrawItem::hom): register this item's islands
// for the hook and adopt the first one it has seen drawn. Per-frame.
void ApplyHomDrawState(DrawItem& item) {
  item.hom_seen = false;
  if (!item.hom) {
    return;
  }
  for (IslandState* st : g_item_islands) {
    st->reg |= kIslHom;
    const HomDrawState& hs = st->hom;
    if (!item.hom_seen && hs.seen) {
      std::memcpy(item.hom_c2, hs.c2, sizeof(hs.c2));
      std::memcpy(item.hom_c7, hs.c7, sizeof(hs.c7));
      item.hom_c1y = hs.c1y;
      item.hom_seen = true;
    }
  }
}

// Flowing-water material rows (DrawItem::water_rows): register this item's
// islands for the hook and adopt the first one it has seen drawn.
// Per-frame, never cached.
void ApplyFlowWaterState(DrawItem& item) {
  item.water_rows_seen = false;
  item.water_tf4 = 0;
  if (!item.water_flowing) {
    return;
  }
  for (IslandState* st : g_item_islands) {
    st->reg |= kIslFlow;
    const FlowWaterDrawState& fs = st->flow;
    if (!item.water_rows_seen && fs.seen) {
      std::memcpy(item.water_rows, fs.rows, sizeof(fs.rows));
      item.water_tf4 = fs.tf4;
      item.water_rows_seen = true;
    }
  }
}

// Advertisement rows + art (DrawItem::ad_rows / ad_fetch): register this
// item's islands for the hook and adopt the first one it has seen drawn.
// Per-frame.
void ApplyAdDrawState(DrawItem& item) {
  item.ad_seen = false;
  std::memset(item.ad_fetch, 0, sizeof(item.ad_fetch));
  std::memset(item.ad_lm_fetch, 0, sizeof(item.ad_lm_fetch));
  if (item.env_family != 15) {
    return;
  }
  for (IslandState* st : g_item_islands) {
    st->reg |= kIslAd;
    const AdDrawState& as = st->ad;
    if (!item.ad_seen && as.seen) {
      std::memcpy(item.ad_rows, as.rows, sizeof(as.rows));
      std::memcpy(item.ad_fetch, as.tf4, sizeof(as.tf4));
      std::memcpy(item.ad_lm_fetch, as.tf3, sizeof(as.tf3));
      item.ad_seen = true;
    }
  }
}

// World render state (DrawItem::game_rs): register this item's islands for
// the hook and adopt the first one it has seen drawn. Per-frame.
void ApplyWorldDrawRs(DrawItem& item) {
  item.game_rs_seen = false;
  if (item.char_family != 0) {
    return;
  }
  for (IslandState* st : g_item_islands) {
    st->reg |= kIslRs;
    const WorldDrawRs& w = st->rs;
    if (!item.game_rs_seen && w.seen) {
      std::memcpy(item.game_rs, w.rs, sizeof(w.rs));
      item.game_rs_seen = true;
    }
  }
}

// Leaf of a shader object's debug path ("baseterrain_defaultPS"), or "" when
// unreadable. Strips the directory and the ".updb"-style suffix.
std::string ShaderLeafName(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return {};
  }
  char path[321] = {};
  if (!GuestTryCopy(path, base + ShaderPathAddr(base, obj), 320)) {
    return {};
  }
  path[320] = 0;
  const char* leaf = std::strrchr(path, '\\');
  std::string out(leaf != nullptr ? leaf + 1 : path);
  const size_t dot = out.find('.');
  if (dot != std::string::npos) {
    out.resize(dot);
  }
  return out;
}

// The mesh's AttribulatorMaterialName (channel string at +0x18), or "".
std::string MeshMaterialName(uint8_t* base, uint32_t mesh) {
  uint32_t material = 0;
  if (!GuestTryLoadU32(base, mesh + kMeshMaterial, &material) ||
      !GuestReadableApprox(base, material)) {
    return {};
  }
  uint32_t head[3] = {};
  if (!GuestTryCopy(head, REX_RAW_ADDR(material), sizeof(head))) {
    return {};
  }
  const uint32_t num = BSwap32(head[0]);
  const uint32_t channels = BSwap32(head[2]);
  for (uint32_t i = 0; i < num && i < 32; ++i) {
    uint32_t name = 0, str = 0;
    if (!GuestTryLoadU32(base, channels + i * 0x20, &name) ||
        !GuestTryLoadU32(base, channels + i * 0x20 + 0x18, &str)) {
      return {};
    }
    char text[20];
    GuestTryReadString(base, name, text, sizeof(text));
    if (std::memcmp(text, "Attribul", 8) == 0 && GuestReadableApprox(base, str)) {
      char mat_name[64];
      GuestTryReadString(base, str, mat_name, sizeof(mat_name));
      return mat_name;
    }
  }
  return {};
}

// Shader census (see skate3_native_render_scene_shader_census): log every
// new (material, game pixel shader) pair this item's buffers were drawn
// with, alongside the native family the item was assigned.
void ShaderCensus(uint8_t* base, const DrawItem& item) {
  if (!REXCVAR_GET(skate3_native_render_scene_shader_census)) {
    return;
  }
  // Union of the shaders the game drew this item's islands with.
  uint32_t objs[8] = {};
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    for (const DrawEntry& d : item.draws) {
      const auto it = g_census_ps.find(
          DrawKey(item.ib_obj, item.vb_obj, d.base_vertex, d.start_index));
      if (it == g_census_ps.end()) {
        continue;
      }
      for (uint32_t o : it->second.obj) {
        for (uint32_t& slot : objs) {
          if (o == 0 || slot == o) break;
          if (slot == 0) {
            slot = o;
            break;
          }
        }
      }
    }
  }
  static std::mutex s_seen_mutex;
  static std::unordered_set<std::string> s_seen;
  std::string material;
  for (uint32_t obj : objs) {
    if (obj == 0) {
      continue;
    }
    const std::string leaf = ShaderLeafName(base, obj);
    if (leaf.empty()) {
      continue;
    }
    if (material.empty()) {
      material = MeshMaterialName(base, item.mesh);
    }
    {
      std::lock_guard<std::mutex> lock(s_seen_mutex);
      if (s_seen.size() >= 8192 || !s_seen.insert(material + "|" + leaf).second) {
        continue;
      }
    }
    REXLOG_INFO(
        "shader-census: material=\"{}\" ps={} env_family={} dynobj={} char_family={} "
        "water={} transparent={} decal={} decal2={} unlit={} mesh={:08X}",
        material, leaf, item.env_family, item.dynobj, item.char_family,
        item.water ? 1 : 0, item.transparent ? 1 : 0, item.decal ? 1 : 0,
        item.decal2 ? 1 : 0, item.unlit ? 1 : 0, item.mesh);
  }
}

// ---- World-item cache ------------------------------------------------------
// BuildItemFromMesh walks the descriptor, the material channels (string
// reads) and the payload fingerprint, and used to run for EVERY visible
// item EVERY frame on the guest render thread. The walk's result is stable
// while a mesh stays loaded, so it is cached per mesh:
//   - every frame: 4-pointer structural validation + buffer address check
//     (streaming reuses arena addresses; re-inits rebuild)
//   - every 4th frame (or every frame for skinned/ropa payloads, whose
//     buffers the CPU sim rewrites): fingerprint refresh
//   - every 32nd frame: full rebuild (material channels re-resolve: late
//     CAS composites, streamed textures binding into channel records)
// Invalidated when the mesh re-registers (tRModelData::Fixup /
// AddRenderInstance -> OnMeshRegistered), on the menus/loading flip and by
// the mesh-cache debug flush. Guarded by its own mutex: builders run on the
// guest render thread, invalidation on loader threads.
struct CachedItemCore {
  DrawItem item;  // draws empty, world identity, fetch overrides zero
  uint64_t fp_frame = 0;       // next fingerprint refresh (guest frames)
  uint64_t rebuild_frame = 0;  // next full rebuild
  uint64_t mat_sig = 0;        // MaterialSignature at build time
};
std::mutex g_item_cache_mutex;
PooledFlatMap<uint32_t, CachedItemCore> g_item_cache;

// Material identity of a mesh: its material pointer, the first 64 bytes of
// the material record (channel count + channel texture pointers) and the
// diffuse texture's base-address fetch word. Retail's release allocator
// hands a freed mesh / buffer / material straight back out: swapping a
// create-a-skater piece builds the new piece at the OLD piece's addresses,
// so the VB/IB pointer check below still matched and the cached item kept
// the previous piece's textures (eyes / cloth flashing the wrong art) until
// the periodic rebuild. sk83_na_m's dev (debug) allocator does not reuse
// that eagerly, which is why it never showed there. Character-form meshes
// (tRMeshData) never pass through the registration invalidation, so this
// check is what catches their reuse. 0 = unreadable.
uint64_t MaterialSignature(uint8_t* base, uint32_t mesh, uint32_t diffuse_tex) {
  uint32_t mat = 0;
  if (!GuestTryLoadU32(base, mesh + kMeshMaterial, &mat) || mat < 0x10000) {
    return 0;
  }
  uint32_t words[16];
  if (!GuestTryCopy(words, REX_RAW_ADDR(mat), sizeof(words))) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
  };
  mix(mat);
  for (uint32_t w : words) {
    mix(w);
  }
  uint32_t tex_w1 = 0;
  if (diffuse_tex >= 0x10000 && GuestTryLoadU32(base, diffuse_tex + 8 * 4, &tex_w1)) {
    mix(tex_w1);
  }
  return h | 1;  // never 0 when readable
}

void InvalidateCachedItem(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.erase(mesh);
}

}  // namespace
void ClearItemCache() {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.clear();
}

namespace {

bool BuildItemFromMeshCached(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint64_t frame = g_guest_frame;
  const bool prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  {
    std::lock_guard<std::mutex> lock(g_item_cache_mutex);
    CachedItemCore* hit = g_item_cache.find(mesh);
    if (hit != nullptr && frame < hit->rebuild_frame) {
      CachedItemCore& core = *hit;
      // Static payloads re-validate every OTHER frame, staggered by mesh:
      // these checks are guest-memory cache misses per world item per frame
      // (the mesh header, the material signature, both buffer descriptors),
      // and reuse of a freed mesh's address is caught at registration
      // (InvalidateCachedItem) regardless; a streamed buffer move or a
      // material swap is picked up at most one frame later. CPU-rewritten
      // payloads (skinned, ropa) keep their per-frame checks.
      if (!core.item.skinned && !core.item.ropa && ((frame + (mesh >> 4)) & 1u) != 0) {
        item = core.item;
        g_item_cache_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      // Structural validation: the mesh's pointer fields must still match
      // the cached walk.
      const bool intact =
          REX_LOAD_U32(mesh + kMeshVertexBuffer) == core.item.vb_obj &&
          REX_LOAD_U32(mesh + kMeshIndexBuffer) == core.item.ib_obj &&
          MaterialSignature(base, mesh, core.item.diffuse_tex) == core.mat_sig;
      // Buffer payload address/size can move under the same objects
      // (streaming re-init), cheap guarded re-read every frame.
      uint32_t vb_words[3] = {}, ib_words[3] = {};
      if (intact &&
          GuestTryCopy(vb_words, REX_RAW_ADDR(core.item.vb_obj + kBufferPhysAddr),
                       sizeof(vb_words)) &&
          GuestTryCopy(ib_words, REX_RAW_ADDR(core.item.ib_obj + kBufferPhysAddr),
                       sizeof(ib_words)) &&
          (BSwap32(vb_words[0]) & 0xFFFFFFFC) == core.item.vb_addr &&
          BSwap32(vb_words[2]) == core.item.vb_bytes &&
          (BSwap32(ib_words[0]) & 0xFFFFFFFC) == core.item.ib_addr &&
          BSwap32(ib_words[2]) == core.item.ib_count) {
        // Fingerprint cadence: per frame for CPU-rewritten payloads
        // (skinned/ropa), every 4th frame otherwise (mesh_revalidate heals
        // streamed fills within that window; address reuse is caught by the
        // registration invalidation immediately).
        const bool dynamic_payload = core.item.skinned || core.item.ropa;
        if (dynamic_payload || frame >= core.fp_frame) {
          const auto fp_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
          const bool fp_ok = ComputeItemFingerprint(base, core.item);
          if (prof) {
            g_pw_bi_fp.Add(PerfNsSince(fp_t0));
          }
          if (!fp_ok) {
            g_rej_geom.fetch_add(1, std::memory_order_relaxed);
            g_item_cache.erase(mesh);
            return false;
          }
          core.fp_frame = frame + 4;
        }
        item = core.item;
        g_item_cache_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      g_item_cache.erase(mesh);
    }
  }
  const auto walk_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  const bool walked = BuildItemFromMesh(base, mesh, item);
  if (prof) {
    g_pw_bi_walk.Add(PerfNsSince(walk_t0));
  }
  if (!walked) {
    return false;
  }
  g_item_cache_builds.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  if (g_item_cache.size() >= 16384) {
    // Runaway growth backstop (streaming churn over a long session): drop
    // everything and let the next frames rebuild, one frame of full walks.
    g_item_cache.clear();
  }
  CachedItemCore core;
  core.item = item;
  core.fp_frame = frame + 4;
  core.mat_sig = MaterialSignature(base, mesh, item.diffuse_tex);
  // Materials with an unresolved diffuse retry the full walk quickly: CAS
  // composites and streamed channel textures bind shortly after first sight
  // (ocean.default legitimately has no diffuse and just rebuilds often,
  // a handful of items).
  core.rebuild_frame = frame + (item.diffuse_tex != 0 ? 32 : 2);
  g_item_cache[mesh] = std::move(core);
  return true;
}

bool BuildItemGeometry(uint8_t* base, uint32_t ctx, DrawItem& item) {
  const uint32_t record = REX_LOAD_U32(ctx);
  if (!GuestReadableApprox(base, record)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t mesh = REX_LOAD_U32(record);
  if (!GuestReadableApprox(base, mesh)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const bool prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  const auto core_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  const bool built = BuildItemFromMeshCached(base, mesh, item);
  if (prof) {
    g_pw_bi_core.Add(PerfNsSince(core_t0));
  }
  if (!built) {
    return false;
  }
  // Draw-time fetch overrides are per-frame state, applied after the cached
  // core, on every build.
  const auto fetch_t0 = prof ? PerfClock::now() : PerfClock::time_point{};
  AdoptDrawFetchOverrides(base, item);
  if (prof) {
    g_pw_bi_fetch.Add(PerfNsSince(fetch_t0));
  }
  // Identity for per-instance consumers (the occlusion cull's guest-side
  // dispatch filter keys on it; the dynamic capture path overwrites it with
  // its own value).
  item.ctx = ctx;

  // Culled island draw list from the context. Retail layout: a u16 count at
  // +0x38 and a flat array of 16-byte draw entries at +0x48. (sk83_na_m's
  // dev build selects entries indirectly through an island-index array at
  // +0x24/+0x34/+0x3C instead.)
  const uint32_t draw_count = REX_LOAD_U16(ctx + kCtxDrawCountU16);
  const uint32_t draw_list = REX_LOAD_U32(ctx + kCtxDrawList);
  if (draw_count == 0 || draw_count > 512 || !GuestReadableApprox(base, draw_list)) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.draws.reserve(draw_count);
  for (uint32_t i = 0; i < draw_count; ++i) {
    const uint32_t d = draw_list + i * 16;
    DrawEntry entry{REX_LOAD_U32(d), REX_LOAD_U32(d + 4), REX_LOAD_U32(d + 8),
                    REX_LOAD_U32(d + 12)};
    if (entry.index_count == 0 || entry.index_count > item.ib_count) continue;
    item.draws.push_back(entry);
  }
  if (item.draws.empty()) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Per-draw game state keyed by the item's islands (needs item.draws):
  // ONE lock and ONE g_islands operation per island for every per-island
  // state (they used to take a lock and a map insert each, per item, per
  // frame). Items that register nothing (characters, non-decal non-special
  // world items take only the render state) skip the map entirely.
  const bool want_islands = item.decal || item.env_family == 17 || item.hom ||
                            item.water_flowing || item.env_family == 15 ||
                            item.char_family == 0;
  {
    std::unique_lock<std::mutex> lock(g_palette_mutex, std::defer_lock);
    g_item_islands.clear();
    g_item_island_keys.clear();
    if (want_islands) {
      lock.lock();
      if (g_islands.size() >= 32768) {
        g_islands.clear();  // streamed-out meshes; live ones re-register
        g_islands_hot.clear();
      }
      const size_t n = std::min<size_t>(item.draws.size(), 16);
      for (size_t i = 0; i < n; ++i) {
        const DrawEntry& d = item.draws[i];
        const uint64_t ikey = DrawKey(item.ib_obj, item.vb_obj, d.base_vertex, d.start_index);
        IslandState& st = g_islands[ikey];
        if (st.mesh != item.mesh) {
          st = IslandState{};  // island reused by another mesh: forget it
          st.mesh = item.mesh;
        }
        g_item_islands.push_back(&st);
        g_item_island_keys.push_back(ikey);
      }
    }
    ApplyDecalDrawAddressing(base, item);
    ApplyTrafficDrawState(item);
    ApplyHomDrawState(item);
    ApplyFlowWaterState(item);
    ApplyWorldDrawRs(item);
    ApplyAdDrawState(item);
    // Islands the hook must see on every draw (see g_islands_hot).
    for (size_t i = 0; i < g_item_islands.size(); ++i) {
      const IslandState* st = g_item_islands[i];
      if ((st->reg & kIslPerDraw) != 0 || ((st->reg & kIslRs) != 0 && !st->rs.seen)) {
        g_islands_hot.insert(g_item_island_keys[i]);
      }
    }
    g_item_islands.clear();
  }
  ShaderCensus(base, item);
  return true;
}

}  // namespace

bool Enabled() { return SceneEnabled(); }

void SetSettingsMenuBlur(bool enabled) {
  // VR: the menu floats on its own panel in front of the world; blurring
  // the whole headset view behind it only smears the surroundings.
  enabled = enabled && !skate3::vr::Active();
  g_settings_menu_blur.store(enabled, std::memory_order_relaxed);
  REXLOG_INFO("native-scene: settings-menu blur {} (post-processor {})", enabled ? "ON" : "off",
              enabled ? "armed" : "disarms after ease-out");
  if (enabled) {
    g_post_blur_log_count.store(0, std::memory_order_relaxed);
    // Arms the emulated-output post-processor (boot frames / manual emulated
    // mode). It disarms itself once the blur has fully eased out.
    rex::graphics::RequestNativeGuestOutputPostProcess(true);
  }
}

void FlushTextureCache() { g_flush_textures.store(true, std::memory_order_relaxed); }

void RequestSceneRingDump() {
  g_scene_ring_dump.store(true, std::memory_order_release);
}
void FlushMeshCache() { g_flush_meshes.store(true, std::memory_order_relaxed); }

// RecordBoneSignal / RecordCameraSignal: native/skate3_native_diagnostics.cpp.

bool ToggleSceneEnabled() {
  if (!REXCVAR_GET(skate3_native_render)) {
    REXLOG_WARN(
        "native-scene: renderer toggle ignored; the skate3_native_render hook layer "
        "is off (set it and restart; it installs the capture hooks the scene needs)");
    return false;
  }
  // The raw cvar: SceneEnabled() also covers the native -> emulated hand-off.
  const bool enabled = !REXCVAR_GET(skate3_native_render_scene);
  if (enabled) {
    // Re-enabling doubles as the retry gesture after a hard pipeline
    // failure (which yields to the emulated output while the cvars stay
    // on): clear the latch so EnsurePipeline runs again.
    ResetSceneFailure();
    // Capture idles while the emulated renderer is active, so anything still
    // published is from before the switch away (stale camera). Drop it:
    // RenderScene yields to the emulated frame until the capture hooks
    // publish a fresh scene (the next frame).
    {
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      g_scene.reset();
    }
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      g_scene_2d.clear();
      g_scene_spline.clear();
    }
    // Warm up before taking over: after a long emulated stretch the decode
    // caches can be cold/stale, and the takeover frame would pay the whole
    // decode burst at once. A warm cache completes warmup in one frame.
    g_warmup_armed.store(true, std::memory_order_relaxed);
    // The off-screen retention map is equally stale after the gap.
    g_retained_clear.store(true, std::memory_order_relaxed);
  }
  // Switching off starts the aspect hand-off (see g_scene_handoff_until_ms):
  // the native renderer keeps presenting until the game's camera is back to
  // 16:9, so the emulated output never shows the wide-projection frames
  // squeezed. Switching on cancels any pending hand-off.
  g_scene_handoff_until_ms.store(enabled ? 0 : SceneHandoffNowMs() + 500,
                                 std::memory_order_relaxed);
  REXCVAR_SET(skate3_native_render_scene, enabled);
  REXLOG_INFO("native-scene: switched to the {} renderer (runtime toggle)",
              enabled ? "NATIVE" : "EMULATED");
  return enabled;
}

void OnRegisterTexture(uint64_t guid, uint32_t texture) {
  if (texture == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_texture_map_mutex);
  g_texture_map[guid & kGuidMask] = texture;
}

void OnMeshRegistered(uint8_t* base, uint32_t mesh) {
  if (mesh == 0) {
    return;
  }
  // A (re-)registration is the "content changed at this address" signal;
  // drop the cached item core so the next frame re-walks it.
  InvalidateCachedItem(mesh);
  // Publish the guest base here too: at boot the loading-screen prewarm
  // workers run before the first gameplay frame would otherwise publish it.
  g_guest_base.store(base, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  const uint64_t now = g_frames_rendered.load(std::memory_order_relaxed);
  const auto [it, fresh] = g_prewarm_seen.try_emplace(mesh, now);
  // 300-frame (~2 s) dedup window: activation bursts re-register the same
  // model once per clone instance: queue it once; a re-registration past
  // the window is a re-stream (content changed at this address) and must
  // re-decode BEFORE its first draw or it pops in as a visible miss.
  if (!fresh) {
    if (now - it->second < 300) {
      return;
    }
    it->second = now;
  }
  if (g_prewarm_queue.size() < 65536) {
    g_prewarm_queue.push_back({mesh, 60});
    g_prewarm_cv.notify_one();
  }
}

// Validate a candidate tRModelData against the layout the game's OWN
// AddRenderInstance (sub_82791290) walks: mesh table pointer at model+0x24,
// mesh COUNT as a u16 at model+0x32 (`lhz r10,50(r11)`). Table entries are
// 8 bytes: word0 = mesh, word1 = 0 for the optimesh world form (its
// material lives at mesh+0x24, which is also why a {mesh, material} pair
// scan finds nothing) / non-zero for the tRMeshData character/prop form.
// Every dereference is SEH-guarded: the offset probe feeds this arbitrary
// instance words (bbox floats, guids) as candidates.
static bool PlausibleRModel(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return false;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;  // u16 at +0x32
  if (num_meshes == 0 || num_meshes > 512) {
    return false;
  }
  // Entry 0: the mesh must carry a plausible material (channel count sane).
  uint32_t mesh0 = 0, mesh0_mat = 0, mat_channels = 0;
  return GuestTryLoadU32(base, table, &mesh0) &&
         GuestTryLoadU32(base, mesh0 + kMeshMaterial, &mesh0_mat) &&
         mesh0_mat >= 0x10000 &&
         GuestTryLoadU32(base, mesh0_mat, &mat_channels) && mat_channels != 0 &&
         mat_channels <= 64;
}

// Queue every optimesh-form mesh of a validated tRModelData for the prewarm
// decode workers.
static void QueueModelMeshes(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;
  for (uint32_t i = 0; i < num_meshes && i < 512; ++i) {
    uint32_t mesh = 0, entry_mat = 0;
    if (!GuestTryLoadU32(base, table + i * 8, &mesh) ||
        !GuestTryLoadU32(base, table + i * 8 + 4, &entry_mat)) {
      continue;
    }
    if (entry_mat != 0) {
      continue;  // tRMeshData form (characters/props), wrong offsets
    }
    OnMeshRegistered(base, mesh);
  }
}

void OnModelFixup(uint8_t* base, uint32_t model) {
  // Fires per model during the load's DISK-STREAMING phase (arena fixup),
  // the early prewarm source. The validation gate keeps non-render models
  // (and any layout drift) out of the queue.
  if (model != 0 && PlausibleRModel(base, model)) {
    QueueModelMeshes(base, model);
  }
}

void OnAddRenderInstance(uint8_t* base, uint32_t instance) {
  if (instance == 0) {
    return;
  }
  // Resolve tInstance::m_pRModel. The game's own AddRenderInstance reads it
  // at +0x80 (`lwz r11,128(r4)`); the offset is still confirmed by the
  // validation probe before first use so an image-version drift degrades to
  // "prewarm off" instead of queueing garbage.
  uint32_t off = g_instance_rmodel_offset.load(std::memory_order_relaxed);
  if (off == 0) {
    for (uint32_t cand = 0x80; cand < 0xC0; cand += 4) {
      uint32_t model = 0;
      if (GuestTryLoadU32(base, instance + cand, &model) &&
          PlausibleRModel(base, model)) {
        g_instance_rmodel_offset.store(cand, std::memory_order_relaxed);
        REXLOG_INFO("native-scene: tInstance::m_pRModel offset confirmed at +0x{:X}",
                    cand);
        off = cand;
        break;
      }
    }
    if (off == 0) {
      return;  // CModel-only / embedded instance, or the model is not ready
    }
  }
  uint32_t model = 0;
  if (!GuestTryLoadU32(base, instance + off, &model) ||
      !PlausibleRModel(base, model)) {
    return;  // instances without a renderable model are normal
  }
  QueueModelMeshes(base, model);
}

struct PfxCapture {
  float ps[32][4];
  float vs[8][4];
  uint32_t fetch[8][6];
  int64_t ps_ns;  // PerfClock stamp of the last PS-row capture (0 = never)
  bool vs_seen;
};
PfxCapture g_pfx_cap[kPfxPassCount] = {};


void OnPhotoGrabRequest() {
  g_photo_grab_request_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnPhotoGrabDone() {
  g_photo_grab_done_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}

// True while the photo display-card quad is in the live 2D stream during an
// armed photo flow; the compose auto-trace trigger
// (skate3_native_render.cpp) keys on it.
bool PhotoCardVisible() {
  if (!g_photo_flow_frame.load(std::memory_order_relaxed)) {
    return false;
  }
  const int64_t seen = g_photo_card_seen_ns.load(std::memory_order_relaxed);
  if (seen < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  return now_ns - seen < 250'000'000;
}

// Guest address of a shader object's compiled-source debug path.
//
// Retail carries it inline at +0x54 on every shader object. This build lays
// the two shader types out differently: the object's first word is the type,
// 7 for the pixel-shader layout (path at +0x54, as retail) and 6 for the
// vertex-shader layout (path at +0x394). Reading +0x54 for both, as the
// ported code did, left every VERTEX shader classifying off garbage -- which
// is why the classifiers here hedge by testing the pixel and vertex trackers
// both ways.
//
// The offset is confirmed by content rather than trusted from the type word:
// a real entry is a printable path carrying the .up debug suffix.
uint32_t ShaderPathAddr(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return obj + 0x54;
  }
  // The probe below costs two 160-byte guest copies plus a strstr, and the
  // answer is a fixed property of the object's layout. It is called ~8 times
  // per draw, so memoise it. thread_local keeps this correct without a lock
  // regardless of which thread resolves a shader.
  struct PathMemo {
    uint32_t obj;
    uint32_t addr;
  };
  static thread_local PathMemo memo[256] = {};
  PathMemo& slot = memo[(obj >> 4) & 0xFFu];
  if (slot.obj == obj) {
    return slot.addr;
  }
  static const uint32_t kCandidates[] = {0x54, 0x394};
  for (uint32_t off : kCandidates) {
    char buf[160];
    if (!GuestTryCopy(buf, base + obj + off, sizeof(buf))) {
      continue;
    }
    buf[sizeof(buf) - 1] = 0;
    if (buf[0] >= 0x20 && buf[0] < 0x7F && std::strstr(buf, ".up") != nullptr) {
      slot = {obj, obj + off};
      return slot.addr;
    }
  }
  slot = {obj, obj + 0x54};  // unknown layout: retail's offset, as before
  return slot.addr;
}

// Debug-path classification, cached per shader object (guest render thread
// only, like the blur classifier). -1 = not a photo postfx shader.
int ClassifyPfxShader(uint8_t* base, uint32_t obj) {
  if (obj == 0 || !GuestReadableApprox(base, obj)) {
    return -1;
  }
  static std::unordered_map<uint32_t, int> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[112] = {};
  const uint32_t shader_path = ShaderPathAddr(base, obj);
  for (int k = 0; k < 111; ++k) {
    text[k] = char(REX_LOAD_U8(shader_path + k));
    if (text[k] == '\0') break;
  }
  int pass = -1;
  if (std::strstr(text, "postfx_visualfxPS") != nullptr) {
    pass = kPfxVisualFx;
  } else if (std::strstr(text, "bloom_dof_motionblur_dof_dowsample") != nullptr) {
    // sic: the game's own shader name carries the 'dowsample' typo.
    pass = kPfxDofDown;
  } else if (std::strstr(text, "bloom_dof_tap9dofMotionBlur") != nullptr) {
    pass = kPfxDofMB;
  } else if (std::strstr(text, "bloom_dof_tap9dofPS") != nullptr) {
    pass = kPfxDof;
  } else if (std::strstr(text, "postfx_uberPS") != nullptr) {
    pass = kPfxUber;
  } else if (std::strstr(text, "postfx_basictex_fisheye_noise") != nullptr) {
    pass = -1;  // the respawn TV-noise pass, not part of the photo chain
  } else if (std::strstr(text, "postfx_basictex_fisheye") != nullptr) {
    pass = kPfxFisheye;
  }
  if (cache.size() < 4096) {
    cache.emplace(obj, pass);
  }
  return pass;
}

// Guest frame of the game's last postfx_uberPS draw: the cinematic chain
// replaced postfx_fast_uberPS on that frame (guest thread).
uint64_t g_pfx_uber_frame = 0;

// DIAGNOSTIC: the game's own tap9dofMotionBlur blur vector, evaluated from
// each captured pass's constants exactly as the shader does (ucode /*11*/..
// /*13*/: v = c0.x * (W*uv + (X, Y)), X/Y/W = d * c1.xyz + view ray, ray =
// uv.x*vs c0 + uv.y*vs c1 + vs c2) at two fixed probes (screen bottom
// center and bottom-left, ground 3 m away: d = 1.00001 - 0.1/3 in the
// game's depth convention). Aggregated once a second: how many guest
// frames carried zero camera motion versus the step sizes, to tell a
// bursty guest camera cadence apart from a steady blur (guest thread).
void LogPfxMotionStats(const PfxCapture& cap) {
  static std::vector<float> s_mag;
  static uint32_t s_zero = 0;
  static uint64_t s_first_frame = 0;
  static int64_t s_window_ns = 0;
  const float d = 1.00001f - 0.1f / 3.0f;
  float mag[2] = {};
  const float probe[2][2] = {{0.5f, 0.9f}, {0.15f, 0.85f}};
  for (int p = 0; p < 2; ++p) {
    const float u = probe[p][0], v = probe[p][1];
    float ray[3];
    for (int k = 0; k < 3; ++k) {
      ray[k] = u * cap.vs[0][k] + v * cap.vs[1][k] + cap.vs[2][k];
    }
    const float x = d * cap.ps[1][0] + ray[0];
    const float y = d * cap.ps[1][1] + ray[1];
    const float w = d * cap.ps[1][2] + ray[2];
    const float vx = cap.ps[0][0] * (w * u + x);
    const float vy = cap.ps[0][0] * (w * v + y);
    mag[p] = std::sqrt(vx * vx + vy * vy);
  }
  const float m = std::max(mag[0], mag[1]);
  if (s_mag.empty()) {
    s_first_frame = g_guest_frame;
    s_window_ns = cap.ps_ns;
  }
  s_mag.push_back(m);
  if (m < 0.05f) {
    ++s_zero;
  }
  if (cap.ps_ns - s_window_ns < 1'000'000'000) {
    return;
  }
  std::vector<float> sorted = s_mag;
  std::sort(sorted.begin(), sorted.end());
  const auto pct = [&](float q) {
    return sorted[std::min(sorted.size() - 1, size_t(q * float(sorted.size())))];
  };
  std::string tail;
  for (size_t i = s_mag.size() > 24 ? s_mag.size() - 24 : 0; i < s_mag.size(); ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), " %.2f", s_mag[i]);
    tail += buf;
  }
  REXLOG_INFO(
      "native-scene: pfx motion (game MB vector @ground 3m, max of 2 probes): {} MB frames / "
      "{} guest frames in {:.2f} s, {} still (<0.05), p50 {:.2f} p90 {:.2f} max {:.2f} | "
      "c0.x {:.1f} | last:{}",
      s_mag.size(), g_guest_frame - s_first_frame,
      double(cap.ps_ns - s_window_ns) * 1e-9, s_zero, pct(0.5f), pct(0.9f),
      sorted.back(), cap.ps[0][0], tail);
  s_mag.clear();
  s_zero = 0;
}

void CapturePfxConstants(uint8_t* base, uint32_t bank_ptr, uint32_t device,
                         bool pixel) {
  // Shader labels can be swapped in the OnSetShader hook; classify both.
  int pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
  if (pass < 0) {
    pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
  }
  if (pass < 0) {
    return;
  }
  PfxCapture& cap = g_pfx_cap[pass];
  // First-hit diagnostics: which passes ever capture, and on which side.
  static uint8_t s_seen[kPfxPassCount][2] = {};
  if (!s_seen[pass][pixel ? 0 : 1]) {
    s_seen[pass][pixel ? 0 : 1] = 1;
    REXLOG_INFO("native-scene: pfx capture first hit pass={} {} bank={:08X}", pass,
                pixel ? "PS" : "VS", bank_ptr);
  }
  if (pixel) {
    for (int r = 0; r < 32; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.ps[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    if (device != 0 && GuestReadableApprox(base, device + 0x480)) {
      for (int s = 0; s < 8; ++s) {
        for (int w = 0; w < 6; ++w) {
          cap.fetch[s][w] = REX_LOAD_U32(device + 0x480 + uint32_t(s * 6 + w) * 4);
        }
      }
    }
    cap.ps_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PerfClock::now().time_since_epoch())
                    .count();
    if (pass == kPfxUber) {
      g_pfx_uber_frame = g_guest_frame;
    }
#ifndef SK83_RELEASE_BUILD
    if (pass == kPfxDofMB) {
      LogPfxMotionStats(cap);
    }
#endif
  } else {
    for (int r = 0; r < 8; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.vs[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    cap.vs_seen = true;
  }
}

void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device) {
  (void)base;
  (void)mask;
  if (device != 0) {
    g_device.store(device, std::memory_order_relaxed);
  }
  if (!SceneEnabled() || ptr == 0) {
    return;
  }
  if (bank == 0x4400) {
    g_ps_bank.store(ptr, std::memory_order_relaxed);
    return;
  }
  if (bank != 0x4000) {
    return;
  }
  g_vs_uploads.fetch_add(1, std::memory_order_relaxed);
  g_vs_bank.store(ptr, std::memory_order_relaxed);
  // NOTE: the photo postfx constant capture used to also run here (upload
  // time), but upload-time snapshots can attribute rows staged for the NEXT
  // pass to the previous pass's shader (the game stages constants before
  // rebinding), overwriting the correct draw-time capture taken by the
  // OnDrawDone hook; the F11 per-draw records read the same banks at the
  // draw hook and are correct for every postfx pass, so the draw hook is
  // the sole capture site now.
}


void OnPhotoReplayUpdate() {
  g_photo_replay_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnTakePhoto() {
  g_take_photo_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnMovieDecode() {
  g_movie_decode_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}


void OnMovieFrame(uint8_t* base, uint32_t renderer) {
  // Plane texture members of VideoRenderer_RwTexture, from sk83_na_m's
  // recompiled Render body (0x82798DD8, Lock/FillTextureData/Unlock trios):
  // [this+12] = Y (filled from the renderable's +8 buffer, full size),
  // [this+164] = U (+12 buffer, half size), [this+88] = V (+16 buffer, half
  // size). Retail's class is smaller (+12/+124/+68); those offsets read
  // non-texture members here, so the planes never published and every FMV
  // fell back to the emulated output.
  static constexpr uint32_t kPlaneOfs[3] = {12, 124, 68};
  MoviePlanes mf;
  for (int p = 0; p < 3; ++p) {
    uint32_t obj = 0;
    if (!GuestTryLoadU32(base, renderer + kPlaneOfs[p], &obj) || obj < 0x10000) {
      return;
    }
    // Stable fetch-words read at tex+0x1C (the D3D12 section's
    // ReadStableTexWords, inlined to keep this compiled guest-side).
    uint32_t raw[6], raw2[6];
    if (!GuestTryCopy(raw, base + obj + 0x1C, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + obj + 0x1C, sizeof(raw2)) ||
        std::memcmp(raw, raw2, sizeof(raw)) != 0) {
      return;
    }
    for (int i = 0; i < 6; ++i) {
      mf.words[p][i] = BSwap32(raw[i]);
    }
    if ((mf.words[p][0] & 3u) != 2 || mf.words[p][1] == 0) {
      return;  // not a live texture fetch constant
    }
  }
  mf.y_addr = mf.words[0][1];
  mf.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PerfClock::now().time_since_epoch())
              .count();
  bool new_key = false;
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    int slot = 0;
    for (int m = 0; m < kMaxMovies; ++m) {
      if (g_movies[m].y_addr == mf.y_addr) {
        slot = m;
        break;
      }
      if (g_movies[m].ns < g_movies[slot].ns) {
        slot = m;  // evict the stalest entry for a new video
      }
    }
    new_key = g_movies[slot].y_addr != mf.y_addr;
    g_movies[slot] = mf;
  }
  OnMovieDecode();  // shared FMV heartbeat (the emulated-yield fallback)
  static std::atomic<uint32_t> s_logged{0};
  if (new_key && s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO(
        "native-scene: FMV planes published (y=[{:08X} {:08X}] u=[{:08X} "
        "{:08X}] v=[{:08X} {:08X}])",
        mf.words[0][0], mf.words[0][1], mf.words[1][0], mf.words[1][1],
        mf.words[2][0], mf.words[2][1]);
  }
}

void On2dPhase(uint32_t bit, bool enter) {
  if (bit >= 6) {
    return;
  }
  if (enter) {
    g_phase2d_depth[bit].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_phase2d_depth[bit].fetch_sub(1, std::memory_order_relaxed);
  }
}

uint32_t Phase2dFlags() {
  uint32_t flags = 0;
  for (uint32_t bit = 0; bit < 6; ++bit) {
    if (g_phase2d_depth[bit].load(std::memory_order_relaxed) != 0) {
      flags |= 1u << bit;
    }
  }
  return flags;
}



// Guest render thread only: per-object cached debug-path classifier
// (same pattern as ClassifySplineShader).
bool IsCasEditorPs(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return false;
  }
  static std::unordered_map<uint32_t, bool> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[201] = {};  // 77-char debug-path prefix + leaf
  if (!GuestTryCopy(text, base + ShaderPathAddr(base, obj), 200)) {
    return false;  // not cached: unreadable now may be readable later
  }
  text[200] = '\0';
  const char* leaf = std::strrchr(text, '\\');
  leaf = leaf ? leaf + 1 : text;
  const bool cas =
      std::strstr(leaf, "_nis") != nullptr &&
      (std::strncmp(leaf, "cacstamp_", 9) == 0 || std::strncmp(leaf, "cac_", 4) == 0 ||
       std::strncmp(leaf, "defaultcharacter_", 17) == 0);
  if (cache.size() > 4096) {
    cache.clear();
  }
  cache.emplace(obj, cas);
  return cas;
}

// The bound PS is one of the front-end ("_nis") defaultcharacter compiles
// (trick guide, create-a-skater, replay editor), whose constant layout
// differs from the gameplay defaultcharacter[_ropa]_defaultPS:
//   1 = defaultcharacter_nisPS, 2 = defaultcharacter_ropa_nisPS, 0 = other.
//   3 = a pass that does NOT carry the item's lighting bank: the skin's
//       texture-space unwrap (character_skin_unwrapPS), its blur
//       (postfx_*) and the shadow casters (*_shadowPS). The front-end skin
//       runs unwrap + postfx_jitterBlur on the same index buffer right before
//       character_skin_nisPS (trick guide capture, draws 17-19); the unwrap
//       bank (key (2,0,0), ambMult 0, "alpha" 0.3) passed the row gates and
//       darkened / tinted the face.
// Checks BOTH shader trackers: the hook's pixel/vertex labels are swapped
// (see OnSetShader). Guest render thread only; cached per shader object.
// The bound PS is an alphatestdynamicobject compile (both shader trackers,
// labels are swapped; cached per object). Its constant layout sits one row
// later than dynamicobject_defaultPS for the material multiplier / ambient /
// bounce (see the dynobj row capture). Guest render thread only.
bool BoundAlphatestDynobjPs(uint8_t* base) {
  static std::unordered_map<uint32_t, bool> cache;
  const auto classify = [&](uint32_t obj) -> bool {
    if (obj < 0x10000) {
      return false;
    }
    auto it = cache.find(obj);
    if (it != cache.end()) {
      return it->second;
    }
    char text[201] = {};  // 77-char debug-path prefix + leaf
    if (!GuestTryCopy(text, base + ShaderPathAddr(base, obj), 200)) {
      return false;
    }
    text[200] = '\0';
    const char* leaf = std::strrchr(text, '\\');
    leaf = leaf ? leaf + 1 : text;
    const bool hit = std::strncmp(leaf, "alphatestdynamicobject_", 23) == 0;
    if (cache.size() > 4096) {
      cache.clear();
    }
    cache.emplace(obj, hit);
    return hit;
  };
  return classify(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
         classify(g_cur_vs_obj.load(std::memory_order_relaxed));
}

int NisDefaultCharacterPsKind(uint8_t* base) {
  static std::unordered_map<uint32_t, int> cache;
  const auto classify = [&](uint32_t obj) -> int {
    if (obj < 0x10000) {
      return 0;
    }
    auto it = cache.find(obj);
    if (it != cache.end()) {
      return it->second;
    }
    char text[201] = {};  // 77-char debug-path prefix + leaf
    if (!GuestTryCopy(text, base + ShaderPathAddr(base, obj), 200)) {
      return 0;  // not cached: unreadable now may be readable later
    }
    text[200] = '\0';
    const char* leaf = std::strrchr(text, '\\');
    leaf = leaf ? leaf + 1 : text;
    int kind = 0;
    if (std::strncmp(leaf, "defaultcharacter_ropa_nisPS", 27) == 0) {
      kind = 2;
    } else if (std::strncmp(leaf, "defaultcharacter_nisPS", 22) == 0) {
      kind = 1;
    } else if (std::strstr(leaf, "_unwrapPS") != nullptr ||
               std::strncmp(leaf, "postfx_", 7) == 0 ||
               std::strstr(leaf, "_shadowPS") != nullptr) {
      kind = 3;
    }
    if (cache.size() > 4096) {
      cache.clear();
    }
    cache.emplace(obj, kind);
    return kind;
  };
  const int a = classify(g_cur_ps_obj.load(std::memory_order_relaxed));
  const int b = classify(g_cur_vs_obj.load(std::memory_order_relaxed));
  if (a == 3 || b == 3) {
    return 3;
  }
  return a != 0 ? a : b;
}

// DIAGNOSTIC rolling pass record (see DumpRecentPasses).
struct PassRecord {
  uint64_t frame = 0;
  uint8_t kind = 0;  // 0 draw, 1 resolve, 2 marker (label, args, lr)
  const char* label = nullptr;
  uint8_t func = 0;
  uint16_t flags2d = 0;
  uint16_t vw = 0, vh = 0;
  uint32_t shader_a = 0, shader_b = 0;
  uint32_t tf[3] = {};  // draw: tf0/tf1/tf3 addr; resolve: addr, fmt, dims
  float aspect = 0.0f;
  bool ortho = false;
  uint32_t rs_blend = 0, rs_color_ctl = 0, rs_mask = 0;  // draw: 0x2201/0x2202/0x2104
};
std::mutex g_pass_ring_mutex;
PassRecord g_pass_ring[8192];
uint32_t g_pass_ring_pos = 0;

void RecordPass(const PassRecord& r) {
#ifdef SK83_RELEASE_BUILD
  // Diagnostic ring (read only by DumpRecentPasses): no per-draw mutex in
  // release.
  (void)r;
  return;
#endif
  std::lock_guard<std::mutex> lock(g_pass_ring_mutex);
  g_pass_ring[g_pass_ring_pos++ & 8191u] = r;
}

// ---- PhotoEffects portrait pass capture (see PhotoJob) ----
// Guest thread: Apply scope -> the pass's colour clear, camera (VS c0..c3 of
// its perspective draws: the view-projection rows) and the (ib,vb) of every
// piece it draws; at the pass's resolve into the screenshot target the job
// is built from the latest published scene's matching items.
struct PhotoCapture {
  bool active = false;
  bool clear_valid = false;
  float clear[4] = {};
  bool cam_valid = false;
  float rows[4][4] = {};
  std::vector<uint64_t> draw_keys;
  uint32_t overlay_draws = 0;
};
PhotoCapture g_photo_cap;  // guest render thread only
std::mutex g_photo_jobs_mutex;
std::vector<PhotoJob> g_photo_jobs;

void OnPhotoApply(bool enter) {
  if (enter) {
    g_photo_cap = PhotoCapture{};
    g_photo_cap.active = SceneEnabled();
    return;
  }
  if (g_photo_cap.active) {
    REXLOG_WARN("native-scene: photo pass ended without its resolve (no job; {} draws)",
                g_photo_cap.draw_keys.size());
  }
  g_photo_cap.active = false;
}

bool TakePhotoJob(uint32_t dest_phys, PhotoJob* out) {
  std::lock_guard<std::mutex> lock(g_photo_jobs_mutex);
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  // A job whose resolve never reached the command processor (pass dropped)
  // must not satisfy a later resolve of the same target.
  std::erase_if(g_photo_jobs, [&](const PhotoJob& j) { return now_ms - j.created_ms > 2000; });
  for (auto it = g_photo_jobs.begin(); it != g_photo_jobs.end(); ++it) {
    if (it->dest_phys == (dest_phys & 0x1FFFF000u)) {
      *out = std::move(*it);
      g_photo_jobs.erase(it);
      return true;
    }
  }
  return false;
}

// Camera position of a view-projection given as the four clip rows (clip_i
// = dot(row_i, (p, 1))): the point with clip x = y = w = 0.
static bool CameraFromClipRows(const float rows[4][4], float out[3]) {
  const float* a = rows[0];
  const float* b = rows[1];
  const float* c = rows[3];
  const float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                    a[2] * (b[0] * c[1] - b[1] * c[0]);
  if (!(std::fabs(det) > 1e-12f)) {
    return false;
  }
  const float r[3] = {-a[3], -b[3], -c[3]};
  out[0] = (r[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r[1] * c[2] - b[2] * r[2]) +
            a[2] * (r[1] * c[1] - b[1] * r[2])) / det;
  out[1] = (a[0] * (r[1] * c[2] - b[2] * r[2]) - r[0] * (b[0] * c[2] - b[2] * c[0]) +
            a[2] * (b[0] * r[2] - r[1] * c[0])) / det;
  out[2] = (a[0] * (b[1] * r[2] - r[1] * c[1]) - a[1] * (b[0] * r[2] - r[1] * c[0]) +
            r[0] * (b[0] * c[1] - b[1] * c[0])) / det;
  return true;
}

// The pass resolved into its target: build the job.
static void BuildPhotoJob(uint8_t* base, uint32_t dest_tex) {
  PhotoCapture cap = std::move(g_photo_cap);
  g_photo_cap = PhotoCapture{};
  std::shared_ptr<const FrameScene> latest;
  {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    latest = g_scene;
  }
  if (!latest || !cap.cam_valid) {
    REXLOG_WARN("native-scene: photo pass - no job ({}{})", latest ? "" : "no published scene ",
                cap.cam_valid ? "" : "no camera");
    return;
  }
  auto scene = std::make_shared<FrameScene>(*latest);
  scene->photo_job = true;
  // The pass's pieces: the latest scene's items drawn with the same buffers.
  std::vector<DrawItem> items;
  std::sort(cap.draw_keys.begin(), cap.draw_keys.end());
  for (const DrawItem& it : latest->items) {
    const uint64_t key = (uint64_t(it.ib_obj) << 32) | it.vb_obj;
    if (std::binary_search(cap.draw_keys.begin(), cap.draw_keys.end(), key)) {
      items.push_back(it);
    }
  }
  scene->items = std::move(items);
  // Camera: clip_i = dot(row_i, v) <=> row-vector view_proj[k][i] = row_i[k].
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      scene->view_proj[k * 4 + i] = cap.rows[i][k];
    }
  }
  float cam[3] = {};
  if (CameraFromClipRows(cap.rows, cam)) {
    std::memcpy(scene->cam_pos, cam, sizeof(cam));
  }
  scene->clear_valid = cap.clear_valid;
  if (cap.clear_valid) {
    std::memcpy(scene->clear_color, cap.clear, sizeof(scene->clear_color));
  }
  PhotoJob job;
  const uint32_t w1 = REX_LOAD_U32(dest_tex + 0x20);
  job.dest_phys = w1 & 0x1FFFF000u;
  job.dest_tex = dest_tex;
  job.created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  REXLOG_INFO(
      "native-scene: photo pass job -> {:08X}: {} pieces of {} drawn keys, clear {} "
      "({:.3f},{:.3f},{:.3f}), cam ({:.2f},{:.2f},{:.2f}) aspect {:.3f}, {} overlay draws",
      job.dest_phys, scene->items.size(), cap.draw_keys.size(), cap.clear_valid ? "yes" : "NO",
      cap.clear[0], cap.clear[1], cap.clear[2], cam[0], cam[1], cam[2],
      // |row1.xyz| / |row0.xyz| = m11 / m00 (the view rotation keeps lengths)
      std::sqrt((cap.rows[1][0] * cap.rows[1][0] + cap.rows[1][1] * cap.rows[1][1] +
                 cap.rows[1][2] * cap.rows[1][2]) /
                std::max(1e-12f, cap.rows[0][0] * cap.rows[0][0] +
                                     cap.rows[0][1] * cap.rows[0][1] +
                                     cap.rows[0][2] * cap.rows[0][2])),
      cap.overlay_draws);
  job.scene = std::move(scene);
  std::lock_guard<std::mutex> lock(g_photo_jobs_mutex);
  g_photo_jobs.push_back(std::move(job));
}

void RecordPassMarker(const char* label, uint32_t a, uint32_t b, uint32_t lr) {
  PassRecord pr;
  pr.frame = g_guest_frame;
  pr.kind = 2;
  pr.label = label;
  pr.tf[0] = a;
  pr.tf[1] = b;
  pr.tf[2] = lr;
  RecordPass(pr);
}

void DumpRecentPasses(uint8_t* base, const char* why) {
  std::vector<PassRecord> recs;
  {
    std::lock_guard<std::mutex> lock(g_pass_ring_mutex);
    const uint32_t n = std::min<uint32_t>(g_pass_ring_pos, 8192u);
    recs.reserve(n);
    for (uint32_t i = g_pass_ring_pos - n; i != g_pass_ring_pos; ++i) {
      recs.push_back(g_pass_ring[i & 8191u]);
    }
  }
  if (recs.empty()) {
    return;
  }
  const uint64_t last = recs.back().frame;
  const auto leaf = [&](uint32_t obj) -> std::string {
    if (obj < 0x10000) {
      return "-";
    }
    char path[200] = {};
    if (!GuestTryCopy(path, base + ShaderPathAddr(base, obj), sizeof(path) - 1)) {
      return "?";
    }
    const char* l = std::strrchr(path, 92);
    std::string out(l != nullptr ? l + 1 : path);
    const size_t dot = out.find(".updb");
    return dot == std::string::npos ? out : out.substr(0, dot);
  };
  REXLOG_INFO("native-scene: DIAG pass dump ({}) frames {}..{}", why, last >= 3 ? last - 3 : 0,
              last);
  size_t i = 0;
  while (i < recs.size()) {
    const PassRecord& r = recs[i];
    if (r.frame + 3 < last) {
      ++i;
      continue;
    }
    if (r.kind == 3) {
      REXLOG_INFO("  f{} CLEAR flags={:X} rgba=({:.3f},{:.3f},{:.3f},{:.3f})", r.frame, r.func,
                  r.tf[0] / 1000.0f, r.tf[1] / 1000.0f, r.tf[2] / 1000.0f, r.aspect);
      ++i;
      continue;
    }
    if (r.kind == 2) {
      REXLOG_INFO("  f{} MARK {} a={:08X} b={:08X} lr={:08X}", r.frame, r.label, r.tf[0],
                  r.tf[1], r.tf[2]);
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < recs.size() && recs[j].kind == r.kind && recs[j].frame == r.frame &&
           recs[j].vw == r.vw && recs[j].vh == r.vh && recs[j].shader_a == r.shader_a &&
           recs[j].shader_b == r.shader_b && recs[j].tf[0] == r.tf[0] &&
           recs[j].tf[2] == r.tf[2] && recs[j].flags2d == r.flags2d) {
      ++j;
    }
    if (r.kind == 0) {
      REXLOG_INFO("  f{} draw x{} {}x{} func={} 2d={:X} {} asp={:.2f} [{} | {}] tf0={:08X} "
                  "tf1={:08X} tf3={:08X} blend={:08X} colctl={:08X} mask={:08X}",
                  r.frame, j - i, r.vw, r.vh, r.func, r.flags2d, r.ortho ? "ortho" : "persp",
                  r.aspect, leaf(r.shader_a), leaf(r.shader_b), r.tf[0], r.tf[1], r.tf[2],
                  r.rs_blend, r.rs_color_ctl, r.rs_mask);
    } else {
      REXLOG_INFO("  f{} RESOLVE x{} -> {:08X} fmt={} {}x{} flags={:X}", r.frame, j - i,
                  r.tf[0], r.tf[1], r.tf[2] & 0xFFFF, r.tf[2] >> 16, r.func);
    }
    i = j;
  }
}

void OnResolve(uint8_t* base, uint32_t flags, uint32_t dest_tex) {
  // The portrait pass's colour resolve into the full-size screenshot target
  // (RenderMan+1276 -> +664, the texture ExtractTextureImageData reads).
  if (g_photo_cap.active && (flags & 0x4u) == 0 && dest_tex >= 0x10000) {
    // Retail: the full-size screenshot target is the fixed 0x04911000
    // surface (see ResolveGrabTargets); match the resolve by its fetch base.
    uint32_t fetch1 = 0;
    if (GuestTryLoadU32(base, dest_tex + 0x20, &fetch1) &&
        (fetch1 & 0x1FFFF000u) == 0x04911000u) {
      GuestReadRecoveryScope rec_scope(base);
      BuildPhotoJob(base, dest_tex);
    }
  }
  if (dest_tex >= 0x10000 && GuestReadableApprox(base, dest_tex)) {
    GuestReadRecoveryScope rec_scope(base);
    PassRecord pr;
    pr.frame = g_guest_frame;
    pr.kind = 1;
    pr.func = uint8_t(flags);
    const uint32_t f1 = REX_LOAD_U32(dest_tex + 0x20);
    const uint32_t f2 = REX_LOAD_U32(dest_tex + 0x24);
    pr.tf[0] = f1 & 0x1FFFF000u;
    pr.tf[1] = f1 & 0x3Fu;
    pr.tf[2] = ((f2 & 0x1FFFu) + 1) | ((((f2 >> 13) & 0x1FFFu) + 1) << 16);
    RecordPass(pr);
  }
  // The skate.Park world-shadow map (tf2, see ParkWsDraw): any resolve into
  // it means the game rebuilt it this frame.
  const uint32_t pws = g_park_ws_tex_addr.load(std::memory_order_relaxed);
  if (pws != 0 && dest_tex >= 0x10000 && GuestReadableApprox(base, dest_tex)) {
    GuestReadRecoveryScope rec_scope(base);
    for (uint32_t k = 0; k < 16; ++k) {
      const uint32_t w = REX_LOAD_U32(dest_tex + k * 4);
      if ((w & 0x1FFFF000u) == (pws & 0x1FFFF000u)) {
        std::lock_guard<std::mutex> lock(g_palette_mutex);
        g_frame_park_ws_resolved = true;
        break;
      }
    }
  }
  // The world-shade pass resolves its DEPTH target (D3DRESOLVE_DEPTHSTENCIL).
  const uint32_t ws = g_ws_tex_addr.load(std::memory_order_relaxed);
  if (ws == 0 || (flags & 0x4u) == 0 || dest_tex < 0x10000 ||
      !GuestReadableApprox(base, dest_tex)) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  // Texture objects keep fetch word 1 with the 0xA0000000 mirror bit while
  // the device fetch shadow holds the plain address: compare physical.
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t w = REX_LOAD_U32(dest_tex + k * 4);
    if ((w & 0x1FFFF000u) == (ws & 0x1FFFF000u)) {
      std::lock_guard<std::mutex> lock(g_palette_mutex);
      g_frame_ws_resolved = true;
      return;
    }
  }
}

std::atomic<int> g_diag_cam_log{0};
void DiagArmCameraLog() { g_diag_cam_log.store(1, std::memory_order_relaxed); }

void OnClearF(uint8_t* base, uint32_t device, uint32_t flags, uint32_t color_ptr) {
  if (g_photo_cap.active && (flags & 0x0Fu) != 0 && color_ptr >= 0x10000 &&
      GuestReadableApprox(base, color_ptr)) {
    for (int c = 0; c < 4; ++c) {
      g_photo_cap.clear[c] = LoadGuestF32(base, color_ptr + uint32_t(c) * 4u);
    }
    g_photo_cap.clear_valid = true;
  }
  if (color_ptr >= 0x10000 && GuestReadableApprox(base, color_ptr)) {
    PassRecord pr;
    pr.frame = g_guest_frame;
    pr.kind = 3;
    pr.func = uint8_t(flags);
    for (int c = 0; c < 3; ++c) {
      const float v = LoadGuestF32(base, color_ptr + uint32_t(c) * 4u);
      pr.tf[c] = uint32_t(std::clamp(v, 0.0f, 4.0f) * 1000.0f + 0.5f);
    }
    pr.aspect = LoadGuestF32(base, color_ptr + 12u);
    RecordPass(pr);
  }
  // D3DCLEAR_TARGET0..3 = bits 0..3; depth/stencil-only clears don't count.
  if ((flags & 0x0Fu) == 0 || color_ptr < 0x10000 || device < 0x10000) {
    return;
  }
  // Bound target dimensions, read the way ClearF builds its full-target
  // rect: surface = device+12616 (else +12632), packed size at surface+36.
  uint32_t surface = REX_LOAD_U32(device + 12616);
  if (surface == 0) {
    surface = REX_LOAD_U32(device + 12632);
  }
  if (surface < 0x10000) {
    return;
  }
  const uint32_t packed = REX_LOAD_U32(surface + 36);
  const uint64_t w = ((packed >> 18) & 0x3FFFu) + 1u;
  const uint64_t h = ((packed >> 3) & 0x7FFFu) + 1u;
  const uint64_t area = w * h;
  if (area < g_clear_frame_area) {
    return;
  }
  float rgb[3];
  for (int c = 0; c < 3; ++c) {
    rgb[c] = LoadGuestF32(base, color_ptr + uint32_t(c) * 4u);
    if (!(rgb[c] >= 0.0f && rgb[c] <= 4.0f)) {
      return;
    }
  }
  std::memcpy(g_clear_frame_rgb, rgb, sizeof(rgb));
  g_clear_frame_area = area;
}


void OnSetTexture(uint32_t slot, uint32_t tex) {
  if (slot < 16) {
    g_tex_slot[slot] = tex;
  }
}

void OnSetShader(bool pixel, uint32_t obj) {
  // Shader dump: write every distinct shader's Xenos microcode to
  // skate3_native_render_shader_dump_dir as raw big-endian *.bin, ready for
  // the offline disassembler (set skate3_disasm_ucode_dir to the same folder
  // on a later run). The guest shader object holds the code address in GPU
  // space (0xA...), readable as base + addr like texture payloads: objects
  // tagged 7 in word 0 keep it at +24, objects tagged 6 at +32.
  {
    const std::string dump_dir(
        REXCVAR_GET(skate3_native_render_shader_dump_dir));
    if (!dump_dir.empty() && obj >= 0x10000) {
      static std::mutex s_dump_mutex;
      static std::set<std::string> s_dumped;
      uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
      char path[321] = {};
      if (base != nullptr &&
          GuestTryCopy(path, base + ShaderPathAddr(base, obj), 320)) {
        path[320] = 0;
        const char* bs = std::strrchr(path, 92);
        std::string leaf(bs ? bs + 1 : path);
        for (char& c : leaf) {
          if (!std::isalnum(uint8_t(c)) && c != '_' && c != '.' && c != '-') c = '_';
        }
        if (leaf.size() > 4 && leaf.size() < 80) {
          bool fresh = false;
          {
            std::lock_guard<std::mutex> lk(s_dump_mutex);
            fresh = s_dumped.insert(leaf).second;
          }
          if (fresh) {
            uint32_t hw[256] = {};
            if (GuestTryCopy(hw, base + obj, sizeof(hw))) {
              const uint32_t tag = __builtin_bswap32(hw[0]);
              // The object holds SEVERAL GPU-space pointers and which one is
              // the microcode is not consistent across shader types: the
              // pointer picked by the old tag==7 ? +24 : +32 rule landed on
              // the literal CONSTANT pool for some shaders (dumped floats:
              // 0.25, 3.0, -1.0, 256.0), which is why every disassembly came
              // back empty. Dump every candidate and let the disassembler
              // identify the real code block.
              for (uint32_t wi = 0; wi < 256; ++wi) {
                const uint32_t code = __builtin_bswap32(hw[wi]);
                if (code < 0xA0000000u || code >= 0xC0000000u) {
                  continue;
                }
                // Dump a generous fixed window; the disassembler stops at the
                // shader's own end-of-program marker.
                std::vector<uint8_t> buf(8192);
                uint32_t got = 0;
                for (uint32_t want = 8192; want >= 256; want /= 2) {
                  if (GuestTryCopy(buf.data(), base + code, want)) {
                    got = want;
                    break;
                  }
                }
                if (got != 0) {
                  const bool is_vs = (tag != 7);
                  std::error_code ec;
                  std::filesystem::create_directories(dump_dir, ec);
                  const std::string out =
                      dump_dir + "/" + (is_vs ? "vs_" : "ps_") + leaf +
                      fmt::format("@{}", wi * 4) + ".bin";
                  std::ofstream f(out, std::ios::binary);
                  f.write(reinterpret_cast<const char*>(buf.data()), got);
                  REXLOG_INFO(
                      "native-scene: SHADERDUMP {} tag={} +{} code={:08X} {} bytes",
                      leaf, tag, wi * 4, code, got);
                }
              }
            }
          }
        }
      }
    }
  }
  (pixel ? g_cur_ps_obj : g_cur_vs_obj).store(obj, std::memory_order_relaxed);
  // Classify BOTH labels: the hook's pixel/vertex flags are SWAPPED
  // relative to the real shader types (see ClassifySplineShader, which
  // checks both trackers for the same reason); gating on `pixel` alone
  // left the heartbeat dead (the *_nisPS debug paths arrive on the other
  // label), which blackholed the startup-flow editor.
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr && IsCasEditorPs(base, obj)) {
    g_cas_ps_last_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_relaxed);
  }
}

// The CAS editor is ON SCREEN now: its own "_nis" pixel shaders were set
// within the last 0.5 s (see IsCasEditorPs). Consumed by the portrait-pass
// publish gate and the takeover gates; the STARTUP-flow editor's scene is
// small (a skater-only scene) and can look like a portrait pass, so the
// gates need to know the editor is up.
bool CasEditorHeartbeatFresh() {
  const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ns - last_ns < 500'000'000;
}

// Full CAS-editor detection: FE push-state screen id 15 (present in BOTH
// flows, pause: [0,56,63,15], startup new-game: [0,67,15], verified in
// capture) OR the _nis shader heartbeat as backup. Cheap
// (a handful of guarded u32 reads); stateless per frame.
bool CasEditorActive(uint8_t* base) {
  if (CasEditorHeartbeatFresh()) {
    return true;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
      GuestTryLoadU32(base, mgr + 0x210, &beg) &&
      GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
      end - beg <= 20 * 16) {
    const uint32_t n = (end - beg) / 20;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t f0 = 0;
      if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
        return true;
      }
    }
  }
  return false;
}

// Should the emulated sub-framebuffer RTT passes execute right now (the
// menus' suppress_mode -> 0 forcing, see menu_rtt_scope)? The one-shot
// skater-portrait RTT renders are the only reason menus need mode 0, and
// they fire at SCREEN TRANSITIONS (screen open / accepted edit), but
// holding mode 0 for the whole menu context kept the game's postfx chain
// executing emulated at scale every menu frame (full world postfx under
// the pause menu; movie postfx on FMV screens), pacing the pipeline
// against the native renderer. Window = TRUE (mode 0) when:
//  - the FE push-state stack is unreadable (fail open = old behavior), or
//  - it contains any screen id NOT known steady-safe (0 = FE root, 56 =
//    pause root, 24 = FMV, surveyed across captured sessions), or
//  - it CHANGED within the last 3 s (covers unknown screens' entry
//    one-shots and the open-race before the id lands), or
//  - the CAS editor heartbeat is fresh.
// Render thread only (statics). Every stack change logs its ids (capped)
// so unknown portrait screens name themselves in the session log.
bool PortraitRttWindowActive() {
  if (CasEditorHeartbeatFresh()) {
    return true;
  }
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return true;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (!GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) || mgr == 0 ||
      !GuestTryLoadU32(base, mgr + 0x210, &beg) ||
      !GuestTryLoadU32(base, mgr + 0x214, &end) || beg > end ||
      end - beg > 20 * 16) {
    return true;
  }
  const uint32_t n = (end - beg) / 20;
  uint32_t ids[16] = {};
  bool unknown_screen = false;
  uint64_t sig = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t f0 = 0;
    if (!GuestTryLoadU32(base, beg + i * 20, &f0)) {
      return true;
    }
    ids[i] = f0;
    sig = (sig ^ f0) * 1099511628211ull;
    // Steady-safe screens (no one-shot portrait RTT renders): 0 = FE root,
    // 56 = pause root, 24 = FMV, 17 = pause challenge map (the game FLAPS
    // [0,56]<->[0,17] ~1/s while the map is open, with 17
    // unclassified every flap re-armed the grace window and held mode 0,
    // pinning the map screen at 62-94 fps), 59 = skate-reel browser
    // (measured A/B: the edit-skater flow pushes
    // [0,56,63]->[0,56,63,15] and legitimately needs the window; the reel
    // screen pushes [0,56,59] and its panels are VIDEOS served by the
    // native FMV substitution, not portrait RTTs; the fail-open window
    // there bought only its costs, 73 fps + 167 ms sync-compile stalls).
    if (f0 != 0 && f0 != 56 && f0 != 24 && f0 != 17 && f0 != 59) {
      unknown_screen = true;
    }
  }
  static uint64_t s_last_sig = 0;
  static int64_t s_change_ns = -1;
  static bool s_prev_unknown = false;
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (sig != s_last_sig) {
    s_last_sig = sig;
    // Grace only when an UNSAFE screen is involved; entering one is
    // already covered by presence (fail-open), so the window's real job is
    // the tail after LEAVING one (the accepted-edit portrait re-render
    // fires as the editor pops). Safe<->safe transitions (pause <->
    // challenge map flapping) must not arm it.
    if (unknown_screen || s_prev_unknown) {
      s_change_ns = now_ns;
    }
    s_prev_unknown = unknown_screen;
    static uint32_t s_logs = 0;
    if (s_logs < 64) {
      ++s_logs;
      char buf[128] = "";  // n == 0 leaves the loop unrun; print "[]"
      int off = 0;
      for (uint32_t i = 0; i < n && off < int(sizeof(buf)) - 8; ++i) {
        off += std::snprintf(buf + off, sizeof(buf) - off, "%s%u", i ? "," : "", ids[i]);
      }
      REXLOG_INFO(
          "native-scene: FE stack changed: [{}] (portrait window {}; "
          "steady-safe = 0/56/24/17/59)",
          buf, unknown_screen ? "OPEN - unclassified screen" : "steady");
    }
  }
  if (s_change_ns >= 0 && now_ns - s_change_ns < 3'000'000'000ll) {
    return true;
  }
  return unknown_screen;
}

// In-game browser detection (see g_browser_screen_open). FE screen 60 is the
// EAWebKit browser: across every captured session it is pushed ([0,56,60])
// ~20 ms before the browser's EAWebKit module loads and popped when the page
// closes, and it never appears otherwise (EAWebKit's background loads at
// sign-in happen with no 60 on the stack and draw nothing). Render thread,
// once per frame; an unreadable stack keeps the previous state.
void UpdateBrowserScreenState() {
  constexpr uint32_t kBrowserScreenId = 60;
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (!GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) || mgr == 0 ||
      !GuestTryLoadU32(base, mgr + 0x210, &beg) ||
      !GuestTryLoadU32(base, mgr + 0x214, &end) || beg > end ||
      end - beg > 20 * 16) {
    return;
  }
  bool open = false;
  for (uint32_t i = 0; i < (end - beg) / 20; ++i) {
    uint32_t id = 0;
    if (!GuestTryLoadU32(base, beg + i * 20, &id)) {
      return;
    }
    if (id == kBrowserScreenId) {
      open = true;
    }
  }
  if (open == g_browser_screen_open.load(std::memory_order_relaxed)) {
    return;
  }
  g_browser_screen_open.store(open, std::memory_order_relaxed);
  REXLOG_INFO("native-scene: in-game browser {}", open ? "opened" : "closed");
}

void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr) {
  (void)mask;
  if (ptr == 0) {
    return;
  }
  // Log each distinct bank id once, the AluConstants analog of discovering
  // 0x4000/0x4400. Tiny lock-free seen-set (at most a handful of banks).
  static std::atomic<uint32_t> seen[8];
  for (auto& slot : seen) {
    uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == bank) {
      break;
    }
    if (cur == 0) {
      uint32_t expected = 0;
      if (slot.compare_exchange_strong(expected, bank, std::memory_order_relaxed)) {
        REXLOG_INFO("native-scene: render-state bank id={:#x} ptr={:08X}", bank, ptr);
        break;
      }
      if (expected == bank) {
        break;
      }
    }
  }
  g_rs_bank.store(ptr, std::memory_order_relaxed);
  if (bank == 0x2100) {
    g_rs_ptr_2100.store(ptr, std::memory_order_relaxed);
  } else if (bank == 0x2200) {
    g_rs_ptr_2200.store(ptr, std::memory_order_relaxed);
  }
}

void OnSetViewport(uint8_t* base, uint32_t viewport_ptr) {
  if (viewport_ptr == 0) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  for (int i = 0; i < 6; ++i) {
    g_cur_viewport[i].store(REX_LOAD_U32(viewport_ptr + i * 4), std::memory_order_relaxed);
  }
}

void OnSetScissor(uint8_t* base, uint32_t rect_ptr) {
  if (rect_ptr == 0) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  for (int i = 0; i < 4; ++i) {
    g_cur_scissor[i].store(REX_LOAD_U32(rect_ptr + i * 4), std::memory_order_relaxed);
  }
}

// The character.hair pixel shader keeps the per-character hair color at PS
// c17 (fixed layout for that shader; verified from recorded PS banks;
// c16/c17 hold the dark ambient/diffuse hair color pair while other
// registers carry lighting globals). Used as captured.
void CaptureHairTint(uint8_t* base, DrawItem& item) {
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  float rgb[3];
  for (int i = 0; i < 3; ++i) {
    rgb[i] = LoadGuestF32(base, ps + (17 * 4 + i) * 4);
    if (!(rgb[i] >= 0.0f && rgb[i] <= 4.0f)) {
      return;  // implausible bank contents; keep the previous/no tint
    }
  }
  for (int i = 0; i < 3; ++i) {
    item.tint[i] = rgb[i];
  }
  item.tint[3] = 1.0f;
}

// Character-family lighting capture: reads the family-specific rows of the
// PIXEL constant bank into the canonical block the scene PS character branch
// consumes (cbuffer CH at b2). Row maps come from the disassembled Skate 3
// pixel shaders (offline-validated by running the actual ucode per pixel):
//   defaultcharacter (fam 1): light c0, key c6, ambMult c10.w, SH c14..c22
//     scaled by c12.y, exposure c4.z, alpha c13.x.
//   cacstamp/cac_* (fam 2): light c9, key c15, ambMult c19.w, SH c24..c32
//     scaled by c21.y, exposure c13.z, alpha c22.x, diffuse tint c23.
//   livingworld_stamp (fam 3): light c9, key c15, FLAT ambient = c19.w *
//     (0.1, 0.175, 0.3) (shader literal), exposure c13.z, alpha c21.x, stamp
//     recolor tints c22 (red mask) / c23 (blue mask).
//   cac_hair (fam 4): light c4, key c16, ambient c14.w * 0.25, fresnel tint
//     c17 with power c11.w, exposure c8.z, strand-alpha scale c15.x.
//   defaulthair (fam 5): light c4, key c15, ambient c14.w * 0.25, fresnel
//     tint c10 with power c9.z, exposure c8.z, strand-alpha scale c16.x.
//   vehicle (fam 6, character.livingworld_vehicles): light c9, key c15
//     (fresnel power in c15.w), FLAT ambient = c19.w * (0.1, 0.175, 0.3)
//     (same literal as livingworld), exposure c13.z, alpha c20.x (the
//     per-entity spawn/distance fade), phong spec color c16
//     with power c16.w (stored in the unused SH row 0), paint recolor
//     colorize_red c21 / colorize_blue c22 (vehicle.fx: where diffuse green
//     is below the mask threshold, rgb = r * red_tint + b * blue_tint;
//     that is the taxi yellow; validated by executing the captured
//     vehicle_defaultPS offline).
//   vehicle glass (fam 7, vehicle_glass_defaultPS): the body's rows with
//     the SH block two rows lower (c21..c29), the glass tint c18.rgb in the
//     tintA slot (multiplies ambient and key), alpha out = c20.x * c18.w.
// The SH irradiance evaluation is sat(c_base + s*(N.x*r1 + N.y*r2 + N.z*r3)
// + s^2*(NxNz*r4 + NzNy*r5 + NyNx*r6) + (3 s^2 Nz^2 - 1)*r7 + s^2*(Nx^2 -
// Ny^2)*r8); the scale and the -1 are folded into the stored rows so the
// shader evaluates a plain 9-row basis.
//
// Canonical block (18 float4 rows): [0] = light dir + hair fresnel power,
// [1] = key color + exposure, [2] = flat ambient rgb + SH-ambient
// multiplier (hair ambient scalar in w), [3..11] = SH rows, [12] = tintA
// (w = apply), [13] = tintB + strand-alpha scale (fams 1/2: w = the
// material multiplier m_params[0].y instead), [14].x = alpha out,
// [14].y = family (0 = capture failed validation -> legacy shading),
// [14].z = lens-alpha flag, [14].w = rim fresnel power, [15] = key spec
// color + power, [16] = rim spec color + power, [17] = rim light color +
// key-spec fresnel power. The spec/rim rows (defaultcharacter: spec c7,
// rim spec c8, rim c11, fresnel powers c6.w/c5.z; cacstamp: c16/c17/c20,
// c15.w/c14.z [+ the editor row shift]) exist on fams 1/2 only; [15].w
// stays 0 when they fail their range gates and the PS terms vanish.

// Does either bound shader's name contain `needle`? The SetShader hook's
// pixel/vertex labels are swapped in this build (see OnSetShader), so both
// trackers have to be consulted.
bool BoundPsNameContains(uint8_t* base, const char* needle) {
  // Called once per character draw (~100k per perf window), so the 320-byte
  // guest copy + strstr must not run every time: the answer is a fixed
  // property of the shader object. Memoise per object, thread_local so no
  // lock is needed. (Doing this unmemoised froze the game.)
  // The memo is keyed by (object, needle): callers ask different questions
  // about the same shader ("stamp", "cac_cloth", "cac_face_..."), and a
  // needle-blind slot served one question's answer to the next (the face
  // injury probe, run first on every character draw, cached "no" for the
  // livingworld stamp and CAC cloth shaders: pedestrians lost their tints,
  // garments their dirt layer). Needles are string literals, so the pointer
  // identifies them.
  struct NameMemo {
    uint32_t obj;
    const char* needle;
    bool hit;
  };
  static thread_local NameMemo memo[512] = {};
  for (const auto* tracker : {&g_cur_ps_obj, &g_cur_vs_obj}) {
    const uint32_t obj = tracker->load(std::memory_order_relaxed);
    if (obj < 0x10000) {
      continue;
    }
    const uint32_t h =
        ((obj >> 4) ^ uint32_t(reinterpret_cast<uintptr_t>(needle) >> 3) * 0x9E3779B1u) &
        0x1FFu;
    NameMemo& slot = memo[h];
    if (slot.obj == obj && slot.needle == needle) {
      if (slot.hit) {
        return true;
      }
      continue;
    }
    char name[321] = {};
    bool hit = false;
    if (GuestTryCopy(name, base + ShaderPathAddr(base, obj), 320)) {
      name[320] = 0;
      hit = std::strstr(name, needle) != nullptr;
    }
    slot = {obj, needle, hit};
    if (hit) {
      return true;
    }
  }
  return false;
}

void CaptureCharLighting(uint8_t* base, DrawItem& item) {
  if (item.char_family == 0) {
    return;
  }
  // Unwrap / blur / caster passes share the item's buffers but not its
  // lighting bank: keep the rows a real lighting draw staged.
  if (NisDefaultCharacterPsKind(base) == 3) {
    return;
  }
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  g_char_attempts.fetch_add(1, std::memory_order_relaxed);
  const auto row = [&](uint32_t r, uint32_t c) {
    return LoadGuestF32(base, ps + (r * 4 + c) * 4);
  };
  // Build locally and commit only on success: the capture can run again on a
  // later draw with the same buffers (the caster-pass bank is stale; its
  // shadowPS touches no PS constants), and a failed refresh must not wipe
  // rows a previous successful capture staged.
  float local[72];
  float* d = local;
  std::memset(local, 0, sizeof(local));
  uint8_t fam = item.char_family;
  // `plus` = register-row shift for the create-a-skater EDITOR compiles of
  // the fam-2 shaders (cacstamp_/cac_*_nisPS + the ropa variants): the whole
  // cacstamp layout sits ONE ROW HIGHER there: light c10, key c16, exposure
  // c14.z, ambMult c20.w, SH c25..c33 scaled by c22.y, alpha c23.x, tint c24
  // (ucode-proven from cac_cloth_nisPS, offline-validated on every fam-2
  // draw in capture: skin tint (0.80,0.68,0.64),
  // key (1.0,0.95,0.9), expo 1.5, unit light at c10). The editor's VS/ropa
  // palette layout is UNCHANGED (flag c7, palette c8, rigid c191 - draw-41
  // VS disasm), and the editor hair compile keeps the gameplay fam-4 rows,
  // so lighting rows are the only editor delta.
  const auto rows_valid = [&](uint8_t f, uint32_t plus, float* light, float* expo,
                              float* key) {
    uint32_t light_r = 9, key_r = 15, expo_r = 13, expo_c = 2;
    switch (f) {
      case 1: light_r = 0; key_r = 6; expo_r = 4; expo_c = 2; break;
      case 2: case 3: case 6: case 7: break;  // defaults above
      // Retail registers (tools/sk83_port/ctab_diff.md). sk83_na_m's dev
      // compiles of both hair shaders sit ONE ROW LOWER (light c3, key
      // c15/c14, exposure c7.z); the notes below describe that build.
      // fam 4 (cac_hair) is shifted one row lower too, same as fam 5
      // (data/render/shaders/final/cac_hair_defaultPS.fpo):
      //   dp3   r0.x, r0.zxyy, c3.zxyy    light    = c3    (retail c4)
      //   mul   r2.xyz, r0.zzzz, c15.xyzz key      = c15   (retail c16)
      //   mad   ..., c7.zzzz              exposure = c7.z  (retail c8.z)
      case 4: light_r = 4; key_r = 16; expo_r = 8; expo_c = 2; break;
      // fam 5 (defaulthair) sits ONE ROW LOWER in this build than in retail.
      // From data/render/shaders/final/defaulthair_defaultPS.fpo:
      //   dp3_sat r0.y, r3.zxyy, c3.zxyy    light    = c3   (retail c4)
      //   mulsc  r1.x, c8.z, r0.x  + log/exp  power  = c8.z (retail c9.z)
      //   mad    r0.xyz, r0.xyz, c7.zzzz     exposure = c7.z (retail c8.z)
      //   mul    oC0.w, r1.wwww, c15.xxxx    strand a = c15.x (retail c16.x)
      // The old rows made the gate read c4 (not a light row here), so hair
      // validated 0 times in 60589 attempts and always fell back to legacy
      // shading -- the wrong-looking hair.
      case 5: light_r = 4; key_r = 15; expo_r = 8; expo_c = 2; break;
    }
    light_r += plus;
    key_r += plus;
    expo_r += plus;
    float norm2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
      light[i] = row(light_r, uint32_t(i));
      if (!(light[i] > -4.0f && light[i] < 4.0f)) return false;
      norm2 += light[i] * light[i];
    }
    // A real light-direction row carries w = 0; the world materials'
    // shadow-transform rows are ALSO unit in xyz but keep the light-space
    // translation (hundreds of meters) in w; reject those banks.
    const float light_w = row(light_r, 3);
    if (!(light_w > -1.0f && light_w < 1.0f)) return false;
    *expo = row(expo_r, expo_c);
    for (int i = 0; i < 3; ++i) {
      key[i] = row(key_r, uint32_t(i));
      if (!(key[i] >= 0.0f && key[i] < 64.0f)) return false;
    }
    // The bank can hold another pass's constants at our capture moment
    // (characters render a shadow-caster pass first); the light-dir norm
    // and exposure gates reject those; the item then keeps its previous /
    // legacy shading.
    return norm2 > 0.25f && norm2 < 2.25f && *expo > 0.25f && *expo < 16.0f;
  };
  float light[3];
  float key[3];
  float expo = 0.0f;
  uint32_t plus = 0;

  // Two different livingworld pixel shaders share the fam-3 material name,
  // and they have nothing in common register-wise:
  //   livingworld_stamp_defaultPS  - lit: light c9, key c15, amb c19.w,
  //                                  expo c13.z, tints c23/c24, alpha c22.x
  //   defaultlivingworld_defaultPS - UNLIT shadow receiver: c0/c1/c2 project
  //                                  into the shadow map (4 PCF taps on tf1),
  //                                  expo c5.z, tints c14/c15, alpha c13.x,
  //                                  and NO light or SH rows at all
  // defaultlivingworld is the majority of fam-3 draws, and because the gate
  // looks for a light row it can never hold, ~90% of fam 3 failed validation
  // and distant NPCs kept rendering their raw recolor masks.
  const bool lw_unlit = (fam == 3) && !BoundPsNameContains(base, "stamp");
  if (lw_unlit) {
    const float e = row(5u, 2);
    if (!(e > 0.25f && e < 16.0f)) {
      return;
    }
    // No lighting rows exist in this compile: hand the shader a neutral
    // key/ambient so the recolor and exposure carry the look, which is what
    // the guest shader itself does.
    d[0 * 4 + 2] = 1.0f;                       // light dir = +z, w = 0
    d[1 * 4 + 0] = d[1 * 4 + 1] = d[1 * 4 + 2] = 1.0f;  // key = white
    d[1 * 4 + 3] = e;                          // exposure c5.z
    d[2 * 4 + 0] = d[2 * 4 + 1] = d[2 * 4 + 2] = 1.0f;  // flat ambient
    for (int c = 0; c < 3; ++c) {
      d[12 * 4 + c] = std::clamp(row(14u, uint32_t(c)), 0.0f, 4.0f);
      d[13 * 4 + c] = std::clamp(row(15u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[12 * 4 + 3] = 1.0f;
    d[14 * 4 + 0] = std::clamp(row(13u, 0), 0.0f, 1.0f);
    // defaultlivingworld_defaultPS lighting rows (ucode lines 22-25):
    // light = c6.y * c11.w * saturate(world_shadow + c12.w); no N.L, no
    // ambient, no CSM. ch_amb.w = c6.y * c11.w, ch_amb.x = c12.w (shadow
    // floor), ch_misc.z = 2 marks this variant for the exact branch.
    {
      const float light = row(6u, 1) * row(11u, 3);
      const float floor_w = row(12u, 3);
      if (light >= 0.0f && light < 16.0f && floor_w > -4.0f && floor_w < 4.0f) {
        d[2 * 4 + 3] = light;
        d[2 * 4 + 0] = floor_w;
        d[14 * 4 + 2] = 2.0f;
      }
    }
    d[14 * 4 + 1] = float(fam);
    std::memcpy(item.char_rows, local, sizeof(local));
    item.char_family = fam;
    g_char_valid.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Layout candidates for this bank, tried in order; the first whose light /
  // key / exposure rows read plausibly wins.
  //   {1,0} / {2,0}  the gameplay compiles.
  //   {1,0} for a fam-2 item: NPC skin (character_skin_defaultPS) shares the
  //     "character.skin" attribulator name with the CAC player skin
  //     (cacstamp_skin) but uses the DEFAULTCHARACTER register layout; the two
  //     banks are mutually exclusive on the light-row position (the other
  //     layout's slot holds non-unit data).
  //   {2,1}  the create-a-skater EDITOR shift (+1 row, see `plus` above).
  //   {2,6} / {1,2}  the SKATE.PARK compiles. A park world binds
  //     *_skateparkPS for every body piece, and their whole lighting block
  //     sits higher: +6 rows for cacstamp/cac (light c9 -> c15, key c15 ->
  //     c21, SH scale c21.y -> c27.y, SH c24 -> c30, alpha c22 -> c28) and +2
  //     for defaultcharacter / character_skin / their ropa variants (light c0
  //     -> c2, key c6 -> c8, SH c14 -> c16, alpha c13 -> c15), all read off
  //     the shipped ucode. Without these every character capture in a park was
  //     REJECTED: no lighting rows and no alpha for any body piece, while the
  //     hair kept working because no *hair* park compile exists.
  struct LayoutCandidate {
    uint8_t fam;
    uint32_t plus;
  };
  LayoutCandidate cands[5];
  uint32_t cand_count = 0;
  if (fam == 1) {
    cands[cand_count++] = {1, 0};
    cands[cand_count++] = {1, 2};
  } else if (fam == 2) {
    cands[cand_count++] = {2, 0};
    cands[cand_count++] = {1, 0};
    cands[cand_count++] = {2, 1};
    cands[cand_count++] = {2, 6};
    cands[cand_count++] = {1, 2};
  } else {
    cands[cand_count++] = {fam, 0};
  }
  bool layout_ok = false;
  for (uint32_t c = 0; c < cand_count && !layout_ok; ++c) {
    if (rows_valid(cands[c].fam, cands[c].plus, light, &expo, key)) {
      fam = cands[c].fam;
      plus = cands[c].plus;
      layout_ok = true;
    }
  }
  if (!layout_ok) {
    {
      static std::atomic<uint32_t> rej_log{0};
      const uint32_t n = rej_log.fetch_add(1, std::memory_order_relaxed);
      if (n < 16 || (n & 2047u) == 0) {
        REXLOG_DEBUG(
            "native-scene: char capture REJECTED fam={} light=({:.3f},{:.3f},{:.3f}) "
            "expo={:.3f} key=({:.3f},{:.3f},{:.3f})",
            fam, light[0], light[1], light[2], expo, key[0], key[1], key[2]);
      }
      return;
    }
  }
  d[0] = light[0]; d[1] = light[1]; d[2] = light[2];
  d[4] = key[0]; d[5] = key[1]; d[6] = key[2];
  d[7] = expo;
  if (fam == 1 || fam == 2) {
    // Front-end defaultcharacter compiles (trick guide / CAS / replay
    // editor), from their own ucode:
    //   defaultcharacter_nisPS: light c0, key c6, exposure c4.z, spec
    //     c7/c8/c11, fresnel c6.w/c5.z, m0y c5.y, ambMult c10.w and SH scale
    //     c12.y are UNCHANGED, but the nine SH rows sit one row lower
    //     (const c13, x c14, y c15, z c16, xz c17, zy c18, yx c19,
    //     3z^2-1 c20, x^2-y^2 c21) and alpha is computed in the shader
    //     (no `max oC0.w, c13.x`): c13 is the SH constant row there.
    //   defaultcharacter_ropa_nisPS: no c12.y scale; SH terms are scaled by
    //     literal registers instead (see below).
    // RETAIL: both _nis compiles (defaultcharacter_nisPS and _ropa_nisPS)
    // share the gameplay layout exactly -- m_params c5..12, i_params c13
    // with the alpha out "max oC0.w, c13.x", SH c14..22 (CTAB + ucode 113);
    // they only add WorldShadow / g_CSMSelfBiasNIS rows above c27. The dev
    // layouts described above (SH constant at c13, literal-scaled ropa SH)
    // do not exist here: decoding them read i_params (1, 0, 0, 0) as the SH
    // constant colour and lit eyes, trucks and wheels pure red in the
    // replay editor / create-a-skater. NisDefaultCharacterPsKind and the
    // bank heuristic stay unused on this build.
    int nis_kind = 0;
    const uint32_t sh_base = (fam == 1 ? (nis_kind == 1 ? 13u : 14u) : 24u) + plus;
    const float s = row((fam == 1 ? 12u : 21u) + plus, 1);
    const float s2 = s * s;
    const float amb_mult = row((fam == 1 ? 10u : 19u) + plus, 3);
    if (!(s > -8.0f && s < 8.0f) || !(amb_mult >= 0.0f && amb_mult < 16.0f)) {
      return;
    }
    d[2 * 4 + 3] = amb_mult;
    for (int r = 0; r < 9; ++r) {
      const float scale = r == 0 ? 1.0f : (r <= 3 ? s : (r == 7 ? 3.0f * s2 : s2));
      for (int c = 0; c < 3; ++c) {
        float v = row(sh_base + uint32_t(r), uint32_t(c)) * scale;
        if (!(v > -64.0f && v < 64.0f)) v = 0.0f;
        d[(3 + r) * 4 + c] = v;
      }
    }
    for (int c = 0; c < 3; ++c) {
      // Fold the (3 s^2 Nz^2 - 1) term's -1 into the base row.
      d[3 * 4 + c] -= row(sh_base + 7u, uint32_t(c));
    }
    if (nis_kind == 2) {
      // defaultcharacter_ropa_nisPS ucode lines 74-89 (normal r3, light dot
      // r3.xzy . c0.zxy), mapped onto the gameplay basis order:
      //   const  = c12 * L251.y - c18 * L250.z
      //   x/y/z  = (c15, c13, c14) * L250.y
      //   xz/zy/yx = (c19, c17, c16) * L251.w
      //   z^2 coefficient = c18 * L251.x      x^2-y^2 = c20 * L253.w
      // L = the compile's literal registers. The game loads them straight
      // from the shader's own constant table when it binds the shader
      // (never into the shadow PS bank this capture reads, so reading them
      // there always failed: the piece lost its lighting rows and fell to
      // the legacy shading, e.g. the black, print-less NIS t-shirt in the
      // 03_Reda_Leaves_Shingo cinematic). This build's values, from
      // shader_dump/ps_defaultcharacter_ropa_nisPS.updb@24.bin:
      //   c250 = (0.99, 1.02333, 0.247708, 0.75)
      //   c251 = (0.743125, 0.886227, 1, 0.858086)
      //   c253 = (0.166667, -0.5, 0.5, 0.429043)
      // (the SH irradiance factors: sqrt(pi)/2, 2pi/3 * Y1, the band-2
      // terms and the 3z^2-1 / x^2-y^2 splits).
      const float l250y = 1.02332795f, l250z = 0.247707993f;
      const float l251x = 0.743125021f, l251y = 0.886227012f, l251w = 0.85808599f;
      const float l253w = 0.429042995f;
      const uint32_t src[9] = {12u, 15u, 13u, 14u, 19u, 17u, 16u, 18u, 20u};
      const float lit[9] = {l251y, l250y, l250y, l250y, l251w, l251w, l251w, l251x, l253w};
      for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 3; ++c) {
          float v = row(src[r], uint32_t(c)) * lit[r];
          if (!(v > -64.0f && v < 64.0f)) v = 0.0f;
          d[(3 + r) * 4 + c] = v;
        }
      }
      for (int c = 0; c < 3; ++c) {
        d[3 * 4 + c] -= row(18u, uint32_t(c)) * l250z;
      }
    }
    if (fam == 2) {
      // CAC diffuse/skin tint (c23; editor c24), multiplies the squared
      // diffuse.
      bool tint_ok = true;
      float tint[3];
      for (int c = 0; c < 3; ++c) {
        tint[c] = row(23u + plus, uint32_t(c));
        tint_ok = tint_ok && tint[c] >= 0.0f && tint[c] < 16.0f;
      }
      if (tint_ok) {
        d[12 * 4 + 0] = tint[0];
        d[12 * 4 + 1] = tint[1];
        d[12 * 4 + 2] = tint[2];
        d[12 * 4 + 3] = 1.0f;
      }
      // Cloth dirt amount c22.y (cac_cloth_defaultPS / _ropa_defaultPS
      // line 28: albedo += (lerp(base, dirt.rgb, dirt.a) - base) * c22.y;
      // 0.247 on a stained gameplay tee). Rides the fam-2-unused tintB.x.
      // Only the cloth compiles define it; other fam-2 PSes keep 0 = clean.
      if (BoundPsNameContains(base, "cac_cloth") ||
          BoundPsNameContains(base, "cacstamp_cloth")) {
        const float dirt = row(22u + plus, 1);
        if (dirt >= 0.0f && dirt <= 1.0f) {
          d[13 * 4 + 0] = dirt;
        }
      }
    }
    // Material multiplier m_params[0].y (defaultcharacter c5.y / cacstamp
    // c14.y): the PS multiplies the lit color by it before the tone chain
    // (1.2 on the gameplay banks). Rides the fam-1/2-unused tintB.w row;
    // out-of-range keeps 0 and the PS applies 1.
    {
      const float m0y = row((fam == 1 ? 5u : 14u) + plus, 1);
      if (m0y > 0.25f && m0y < 4.0f) {
        d[13 * 4 + 3] = m0y;
      }
    }
    // Rim light + key/rim phong spec rows (see the header comment; observed
    // gameplay banks: key spec white pow 5, rim spec gray pow 67/255, rim
    // 0.2, fresnel powers 0.2-2.0). All-or-nothing behind range gates;
    // a bank that fails them keeps [15].w at 0 and the PS skips the terms.
    {
      const uint32_t ks_r = (fam == 1 ? 7u : 16u) + plus;
      const uint32_t rs_r = (fam == 1 ? 8u : 17u) + plus;
      const uint32_t rim_r = (fam == 1 ? 11u : 20u) + plus;
      const float kfres = row((fam == 1 ? 6u : 15u) + plus, 3);
      const float rfres = row((fam == 1 ? 5u : 14u) + plus, 2);
      float sr[12];
      bool spec_ok = kfres >= 0.0f && kfres < 512.0f &&
                     rfres >= 0.0f && rfres < 512.0f;
      for (int r = 0; r < 3 && spec_ok; ++r) {
        const uint32_t src = r == 0 ? ks_r : (r == 1 ? rs_r : rim_r);
        for (int c = 0; c < 3; ++c) {
          sr[r * 4 + c] = row(src, uint32_t(c));
          spec_ok = spec_ok && sr[r * 4 + c] >= 0.0f && sr[r * 4 + c] < 64.0f;
        }
        sr[r * 4 + 3] = row(src, 3);
      }
      spec_ok = spec_ok && sr[3] >= 0.5f && sr[3] < 512.0f &&
                sr[7] >= 0.5f && sr[7] < 512.0f;
      if (spec_ok) {
        for (int c = 0; c < 4; ++c) {
          d[15 * 4 + c] = sr[c];
          d[16 * 4 + c] = sr[4 + c];
        }
        d[17 * 4 + 0] = sr[8];
        d[17 * 4 + 1] = sr[9];
        d[17 * 4 + 2] = sr[10];
        d[17 * 4 + 3] = kfres;
        d[14 * 4 + 3] = rfres;
      }
    }
    // The _nis compiles compute alpha in the shader (opaque): no alpha row.
    d[14 * 4 + 0] = nis_kind != 0
                        ? 1.0f
                        : std::clamp(row((fam == 1 ? 13u : 22u) + plus, 0), 0.0f, 1.0f);
    // character.alpha accessory (sunglass lens): tell the PS fam-1/2 branch
    // to take alpha from the coverage texture at t4/uv2 (ch_misc.z); the
    // item is routed to the blended sub-pass (see DrawItem::char_alpha).
    if (item.char_alpha && item.hair_alpha_tex != 0) {
      d[14 * 4 + 2] = 1.0f;
    }
  } else if (fam == 3) {
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 0] = amb * 0.1f;
    d[2 * 4 + 1] = amb * 0.175f;
    d[2 * 4 + 2] = amb * 0.3f;
    // Rows read from THIS build's own ucode
    // (data/render/shaders/final/livingworld_stamp_defaultPS.fpo), not
    // retail's: the recolor tints and the alpha output all sit one row
    // higher here, which is why every NPC rendered its raw red/blue
    // colorize masks instead of its per-character clothing tints.
    //   mulsc r8.x, c24.z, r6.w       //   mulsc r8.y, c24.y, r6.w    > blue-mask tint = c24 (retail c23)
    //   mulsc r8.z, c24.x, r6.w   /
    //   mad   r8.xyz, r6.xxxx, c23.zyxx, r8.xyzz  red-mask tint = c23 (c22)
    //   max   oC0.w, c22.xxxx, c22.xxxx           alpha out    = c22 (c21)
    // Light (c9), key (c15), ambient multiplier (c19.w) and exposure
    // (c13.z) are unchanged from retail -- verified in the same disassembly.
    for (int c = 0; c < 3; ++c) {
      d[12 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
      d[13 * 4 + c] = std::clamp(row(23u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[12 * 4 + 3] = 1.0f;
    d[14 * 4 + 0] = std::clamp(row(21u, 0), 0.0f, 1.0f);
    // The exact stamp branch (ch_misc.z = 1, see scene_char.hlsli): the
    // shader is the cacstamp lighting, so the same rows as fam 2 except the
    // SH block, one row higher (two tints at c23/c24):
    //   46/49: SH scale c21.y ; 68-88: SH c25 (const), c26 x, c27 y, c28 z,
    //          c29 xz, c30 zy, c31 yx, c32 (3z^2 - 1), c33 (x^2 - y^2)
    //   102: ambient multiplier c19.w ; 97: key c15
    //   99-106: key spec c16 (w = power), fresnel power c15.w
    //   94-100: rim spec c17 (w = power), rim fresnel power c14.z
    //   105: rim light c20.rgb ; 95: shadow floor c20.w
    //   110b: material multiplier c14.y ; 111: flat-term blend c34.x
    // RETAIL livingworld_stamp_defaultPS (tools/sk83_port/disasm_retail)
    // is this same lighting minus the SH block the dev build added:
    //   74: amb = c19.w * c255.xyz, c255 = (0.1, 0.175, 0.3) (a constant,
    //       so the SH set reduces to its DC row)
    //   83/85: shadow = min(cascade, max(static world shade, c8.w))
    //   no flat-term blend (dev c34.x); key c15 (fresnel c15.w), key spec
    //   c16, rim spec c17, rim light c20, rim fresnel c14.z, multiplier
    //   c14.y, exposure c13.z, alpha c21.x -- the dev registers, unchanged.
    // All-or-nothing: any failed gate keeps the legacy fam-3 shading.
    {
      const float floor_static = row(8u, 3);
      const float m0y = row(14u, 1);
      const float kfres = row(15u, 3);
      const float rfres = row(14u, 2);
      bool ok = BoundPsNameContains(base, "livingworld_stamp_defaultPS") &&
                amb >= 0.0f && amb < 16.0f && floor_static >= 0.0f &&
                floor_static <= 1.0f && m0y > 0.25f && m0y < 4.0f &&
                kfres >= 0.0f && kfres < 512.0f && rfres >= 0.0f &&
                rfres < 512.0f;
      float sr[12];
      for (int r = 0; r < 3 && ok; ++r) {
        const uint32_t src = r == 0 ? 16u : (r == 1 ? 17u : 20u);
        for (int c = 0; c < 3; ++c) {
          sr[r * 4 + c] = row(src, uint32_t(c));
          ok = ok && sr[r * 4 + c] >= 0.0f && sr[r * 4 + c] < 64.0f;
        }
        sr[r * 4 + 3] = row(src, 3);
      }
      ok = ok && sr[3] >= 0.5f && sr[3] < 512.0f && sr[7] >= 0.5f &&
           sr[7] < 512.0f;
      if (ok) {
        // SH rows 3..11: DC = the retail ambient tint, every directional
        // term zero (the shader saturates the sum, as the game does).
        for (int r = 0; r < 9; ++r) {
          for (int c = 0; c < 3; ++c) {
            d[(3 + r) * 4 + c] = 0.0f;
          }
        }
        d[3 * 4 + 0] = 0.1f;
        d[3 * 4 + 1] = 0.175f;
        d[3 * 4 + 2] = 0.3f;
        d[2 * 4 + 0] = 0.0f;           // flat-term floor (unused: blend 0)
        d[2 * 4 + 1] = 0.0f;           // flat-term blend (dev c34.x)
        d[2 * 4 + 2] = floor_static;   // static world-shade floor c8.w
        d[2 * 4 + 3] = amb;
        for (int c = 0; c < 4; ++c) {
          d[15 * 4 + c] = sr[c];
          d[16 * 4 + c] = sr[4 + c];
        }
        d[17 * 4 + 0] = sr[8];
        d[17 * 4 + 1] = sr[9];
        d[17 * 4 + 2] = sr[10];
        d[17 * 4 + 3] = kfres;
        d[14 * 4 + 3] = rfres;
        d[13 * 4 + 3] = m0y;
        d[14 * 4 + 2] = 1.0f;
      }
    }
  } else if (fam == 6) {
    // vehicle_defaultPS, rows straight from its ucode:
    //   c9 light, c15.rgb key color, c15.w fresnel power, c13.z exposure,
    //   c16.rgb spec color, c16.w spec power, c19.w SH ambient multiplier,
    //   c21/c22 red/blue paint-mask tints, c14.y material multiplier,
    //   c20.x alpha out, SH irradiance c23 (const), c24 x, c25 y, c26 z,
    //   c27 xz, c28 zy, c29 yx, c30 (3z^2 - 1), c31 (x^2 - y^2).
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 3] = amb;
    d[3] = std::clamp(row(15u, 3), 1.0f, 64.0f);  // fresnel power c15.w
    // Retail vehicle_defaultPS / vehicle_glass_defaultPS have no SH block
    // (the dev build's i_shmult): ambient = c19.w * c254.xyz with c254 =
    // (0.1, 0.175, 0.3) (tools/sk83_port/disasm_retail, line 68 / 60). The
    // SH set therefore reduces to its DC row.
    for (int r = 0; r < 9; ++r) {
      for (int c = 0; c < 3; ++c) {
        d[(3 + r) * 4 + c] = 0.0f;
      }
    }
    d[3 * 4 + 0] = 0.1f;
    d[3 * 4 + 1] = 0.175f;
    d[3 * 4 + 2] = 0.3f;
    for (int c = 0; c < 3; ++c) {
      d[15 * 4 + c] = std::clamp(row(16u, uint32_t(c)), 0.0f, 64.0f);
      d[12 * 4 + c] = std::clamp(row(21u, uint32_t(c)), 0.0f, 4.0f);
      d[13 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[15 * 4 + 3] = std::clamp(row(16u, 3), 0.5f, 512.0f);  // spec power c16.w
    d[12 * 4 + 3] = 1.0f;
    {
      const float m = row(14u, 1);  // material multiplier c14.y
      d[13 * 4 + 3] = (m > 0.0f && m < 4.0f) ? m : 1.0f;
    }
    // Entity opacity: vehicle_defaultPS ends `max oC0.w, c20.x, c20.x`
    // (the LivingWorld spawn/distance fade).
    d[14 * 4 + 0] = std::clamp(row(20u, 0), 0.0f, 1.0f);
    // Cascade receive biases: retail vehicle shaders read only c8.w; the
    // per-cascade biases are literals (c255 = -0.02, -0.006, -0.01 added to
    // the c4.w reference, overwritten outer -> mid -> finest): finest
    // 0.006, mid 0.01, outer 0.02. (sk83_na_m's dev compile reads c8.xyz.)
    d[16 * 4 + 0] = 0.006f;
    d[16 * 4 + 1] = 0.01f;
    d[16 * 4 + 2] = 0.02f;
    d[16 * 4 + 3] = 1.0f;
  } else if (fam == 7) {
    // vehicle_glass_defaultPS, rows straight from its ucode (identical to
    // sk83_na_m's own .fpo; literals c250..c255): the vehicle_defaultPS
    // structure with the window tint in place of the paint recolor:
    //   c9 light, c15.rgb key color, c15.w fresnel power, c13.z exposure,
    //   c16.rgb spec color, c16.w spec power, c19.w SH ambient multiplier,
    //   c18.rgb glass tint (multiplies ambient AND key; (0.12,0.12,0.15) in
    //   capture), c14.y material multiplier, alpha out = c20.x * c18.w
    //   (entity fade * glass opacity 0.75), c8 cascade biases, SH
    //   irradiance TWO rows lower than the body: c21 (const), c22 x, c23 y,
    //   c24 z, c25 xz, c26 zy, c27 yx, c28 (3z^2 - 1), c29 (x^2 - y^2)
    //   (ucode lines 68-88).
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 3] = amb;
    d[3] = std::clamp(row(15u, 3), 1.0f, 64.0f);  // fresnel power c15.w
    // Retail vehicle_defaultPS / vehicle_glass_defaultPS have no SH block
    // (the dev build's i_shmult): ambient = c19.w * c254.xyz with c254 =
    // (0.1, 0.175, 0.3) (tools/sk83_port/disasm_retail, line 68 / 60). The
    // SH set therefore reduces to its DC row.
    for (int r = 0; r < 9; ++r) {
      for (int c = 0; c < 3; ++c) {
        d[(3 + r) * 4 + c] = 0.0f;
      }
    }
    d[3 * 4 + 0] = 0.1f;
    d[3 * 4 + 1] = 0.175f;
    d[3 * 4 + 2] = 0.3f;
    for (int c = 0; c < 3; ++c) {
      d[15 * 4 + c] = std::clamp(row(16u, uint32_t(c)), 0.0f, 64.0f);
      d[12 * 4 + c] = std::clamp(row(18u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[15 * 4 + 3] = std::clamp(row(16u, 3), 0.5f, 512.0f);  // spec power c16.w
    d[12 * 4 + 3] = 1.0f;
    {
      const float m = row(14u, 1);  // material multiplier c14.y
      d[13 * 4 + 3] = (m > 0.0f && m < 4.0f) ? m : 1.0f;
    }
    d[14 * 4 + 0] = std::clamp(row(20u, 0) * row(18u, 3), 0.0f, 1.0f);
    // Cascade receive biases: retail vehicle shaders read only c8.w; the
    // per-cascade biases are literals (c255 = -0.02, -0.006, -0.01 added to
    // the c4.w reference, overwritten outer -> mid -> finest): finest
    // 0.006, mid 0.01, outer 0.02. (sk83_na_m's dev compile reads c8.xyz.)
    d[16 * 4 + 0] = 0.006f;
    d[16 * 4 + 1] = 0.01f;
    d[16 * 4 + 2] = 0.02f;
    d[16 * 4 + 3] = 1.0f;
  } else {
    // Hair: flat ambient scalar + fresnel rim tint, strand-alpha scale.
    const float amb = row(14u, 3) * 0.25f;
    d[2 * 4 + 3] = std::clamp(amb, 0.0f, 4.0f);
    d[3] = std::clamp(row(fam == 4 ? 11u : 9u, fam == 4 ? 3u : 2u), 1.0f, 64.0f);
    const uint32_t fres_r = fam == 4 ? 17u : 10u;
    for (int c = 0; c < 3; ++c) {
      d[13 * 4 + c] = std::clamp(row(fres_r, uint32_t(c)), 0.0f, 8.0f);
    }
    d[13 * 4 + 3] = std::clamp(row(fam == 4 ? 15u : 16u, 0), 0.0f, 4.0f);
    d[14 * 4 + 0] = 1.0f;
  }
  d[14 * 4 + 1] = float(fam);
  // Frame-global character CSM receive biases: one row below the light row
  // in the fam-2 layouts (gameplay light c9 -> biases c8; editor light c10
  // -> biases c9; verified in capture: (0.005,0.014,0.020)
  // with the same values in every char bank). Captured once per frame from
  // any validating fam-2 bank; consumed by the exact 9-tap char shadow
  // sampling (FrameScene::char_shadow_bias).
  // Cascade order from the ucode (cacstamp_skin_defaultPS 51/54/57,
  // defaultcharacter_defaultPS 43/46/49, defaultcharacter_nisPS 34/37/40):
  // the reference is overwritten outer -> mid -> finest, so finest = x,
  // mid = y, outer = z. The values are NOT monotonic (create-a-skater bank
  // c8 = (0.005, 0.010, 0.002)); the old x <= y <= z && z < 0.1 gate
  // rejected every real bank, leaving characters on the world-bias fallback
  // (c5.x = -0.05): a NEGATIVE bias that self-shadowed every lit surface.
  // fam 2 (cacstamp/cac) rows sit at c8 (+editor shift), fam 1 at c31
  // (gameplay compile) / c27 (_nis compile).
  // The park compiles move this row by a DIFFERENT amount than the lighting
  // block above: fam 1 c31 -> c34 (+3) and fam 2 c8 -> c9 (+1), read off the
  // same ucode. Using `plus` here would land on an unrelated register.
  uint32_t bias_r = 0;
  if (fam == 2) {
    bias_r = 8u + (plus == 6u ? 1u : plus);
  } else if (fam == 1) {
    const int nk = NisDefaultCharacterPsKind(base);
    const bool nis_layout = nk == 1 || ((row(13u, 1) != 0.0f || row(13u, 2) != 0.0f) &&
                                        row(22u, 3) != 0.0f);
    bias_r = nk == 2 ? 0u : (nis_layout ? 27u : (plus == 2u ? 34u : 31u));
  }
  if (bias_r != 0) {
    const float bx = row(bias_r, 0);
    const float by = row(bias_r, 1);
    const float bz = row(bias_r, 2);
    if (bx > 0.0f && by > 0.0f && bz > 0.0f && bx < 0.5f && by < 0.5f && bz < 0.5f) {
      g_char_shadow_bias[0] = bx;
      g_char_shadow_bias[1] = by;
      g_char_shadow_bias[2] = bz;
      g_char_shadow_bias_frame = g_guest_frame;
    }
  }
  std::memcpy(item.char_rows, local, sizeof(item.char_rows));
  g_char_valid.fetch_add(1, std::memory_order_relaxed);
  // Remember the validated rows per garment for the cross-frame fallback
  // (see g_char_rows_cache). Runaway-growth backstop only; mesh keys are
  // bounded by loaded character content in practice.
  if (g_char_rows_cache.size() > 4096) {
    g_char_rows_cache.clear();
  }
  std::memcpy(g_char_rows_cache[item.mesh].data(), local, sizeof(local));
}

// The game's per-entity spawn/streaming fade. LivingWorld presentation
// entities (NPCs, traffic vehicles, spawned props) publish an opacity:
// 0 through the whole spawn settle (the physics drop after
// CensusMan::SpawnPedestrian/SpawnVehicle), ramping up afterwards and by
// distance (cLivingWorldPresEntity::EvaluateOpacityDistance), which every
// character-family PS writes as its output alpha (the "alpha out" row of
// CaptureCharLighting: peds c21.x, defaultcharacter c13.x, cacstamp c22.x,
// vehicle body c20.x, glass c20.x * c18.w, hair strand * scale). Returns
// that alpha for items whose validated capture carries it, 1.0 otherwise
// (legacy-shaded items keep the old always-opaque behavior).
float CharFadeAlpha(const DrawItem& item) {
  // LW-mapped items: the entity's own
  // opacity is authoritative; it is the exact value the game serves this
  // ctx's shader as output alpha, independent of whether the per-draw row
  // capture validated (or captured a clone's foreign row). Families whose
  // shader COMPOSES the entity fade with another factor (hair strand-scale,
  // vehicle-glass tint alpha) keep their captured value bounded by it.
  if (item.lw_alpha >= 0.0f) {
    const float a = std::clamp(item.lw_alpha, 0.0f, 1.0f);
    switch (item.char_family) {
      case 1:
      case 2:
      case 3:
      case 6:
        return a;
      case 4:
      case 5:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f))
                   : a;
      case 7:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f))
                   : a;
      default:
        break;
    }
  }
  if (item.char_rows[14 * 4 + 1] <= 0.0f) {
    return 1.0f;
  }
  switch (item.char_family) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 7:
      return std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f);
    case 4:
    case 5:
      // Hair alpha = strand coverage * scale; the scale constant carries the
      // entity fade, so a near-zero scale means the whole garment is
      // invisible this frame.
      return std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f);
    default:
      return 1.0f;
  }
}

// Returns false when the bank could not be consumed for this item (ropa
// rigid matrix implausible or off-clip = stale bank); the caller must
// leave/mark the item pending so a later matching draw re-captures it.
// The bank carries a real viewproj at c0..c3: every palette layout the
// capture understands (caster pre-pass: viewproj + palette c4; main pass:
// viewproj + camera c4 + palette c7) starts with one. Ortho = the CSM caster
// cascades (BankIsOrtho); perspective = the projection's z and w rows share
// the view direction, so c2.xyz and c3.xyz are parallel (create-a-skater
// capture, real skin draw: c2 = (0.9996, -0.0034, -0.0265, 3.550),
// c3 = (0.9996, -0.0034, -0.0265, 3.600)).
// The skin/face texture-space UNWRAP pass (cac*_unwrapVS,
// character_skin_unwrapVS) has NO viewproj: its palette starts at c0 (draw
// 35: c2 = (0.661, 0.137, -0.737), c3 = (-0.453, -0.710, -0.540), two bone
// rows, dot ~0), and BankPaletteBase still finds bone-shaped rows at c4/c7
// there, i.e. a row-shifted palette that stretched/hid the skin and face.
// Decided from the bank itself: the shader trackers can lag the draw, and a
// tracker-based gate also refused the cloth/hair draws that follow the unwrap
// blur (persistent shirt holes, stretched ponytail).
bool BankHasViewProj(uint8_t* base, uint32_t bank) {
  if (BankIsOrtho(base, bank)) {
    return true;
  }
  float r2[3];
  float r3[3];
  float n2 = 0.0f;
  float n3 = 0.0f;
  float dot = 0.0f;
  for (int i = 0; i < 3; ++i) {
    r2[i] = LoadGuestF32(base, bank + (2 * 4 + uint32_t(i)) * 4);
    r3[i] = LoadGuestF32(base, bank + (3 * 4 + uint32_t(i)) * 4);
    if (!(r2[i] > -1e6f && r2[i] < 1e6f && r3[i] > -1e6f && r3[i] < 1e6f)) {
      return false;
    }
    n2 += r2[i] * r2[i];
    n3 += r3[i] * r3[i];
    dot += r2[i] * r3[i];
  }
  if (!(n2 > 1e-12f && n3 > 1e-12f)) {
    return false;
  }
  return dot > 0.995f * std::sqrt(n2 * n3);
}

bool CaptureSkinnedState(uint8_t* base, uint32_t bank, uint32_t palette_base,
                         DrawItem& item) {
  // Never take a palette from a bank without a viewproj (the texture-space
  // unwrap pass, see BankHasViewProj); the caller keeps the item pending and
  // the real draw of the same buffers captures it.
  if (!BankHasViewProj(base, bank)) {
    return false;
  }
  // Refuse captures staged by AUX perspective passes (skater-portrait RTTs):
  // the portrait pass draws the SAME meshes as the on-screen player at the
  // off-map portrait stage, and accepting its palette poisons the mesh/ctx
  // stores (see BankIsAuxPerspective). The item stays pending; a later
  // main-view draw of these buffers resolves it, and portrait-only frames
  // publish nothing (BuildFrameScene's aux-view gate).
  if (BankIsAuxPerspective(base, bank)) {
    static std::atomic<uint64_t> s_aux_refused{0};
    const uint64_t n = s_aux_refused.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (n & 4095u) == 0) {
      REXLOG_INFO(
          "native-scene: skinned capture refused - aux perspective pass "
          "(portrait RTT) mesh={:08X} (n={})",
          item.mesh, n);
    }
    return false;
  }
  // Layout from the bound VS's own constant table (see kVsLayouts): the bank
  // is the one this VS consumes, so its declared registers ARE the palette /
  // sim flag / rigid world. Unknown shader = not a skinned character draw.
  const VsLayout* layout = BoundVsLayout(base);
  if (layout == nullptr || palette_base == 0 || palette_base != uint32_t(layout->bone)) {
    return false;
  }
  if (layout->params >= 0 && layout->matworld >= 0) {
    // Ropa VS: `setp_gt i_vtx_params.x` - x > 0 skins from i_boneArray,
    // otherwise the whole garment is placed by the rigid i_matWorld block
    // (3 affine rows + the (0,0,0,1) tail).
    const float flag_x =
        LoadGuestF32(base, bank + (uint32_t(layout->params) * 4 + 0) * 4);
    if (!(flag_x > 0.0f)) {
      const uint32_t m = uint32_t(layout->matworld);
      float rows[12];
      for (int r = 0; r < 3; ++r) {
        for (int i = 0; i < 4; ++i) {
          rows[r * 4 + i] = LoadGuestF32(base, bank + ((m + uint32_t(r)) * 4 + uint32_t(i)) * 4);
        }
      }
      // Column-vector [R | t] rows -> item.world (row-vector, t in row 3).
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          item.world[i * 4 + j] = rows[j * 4 + i];
        }
        item.world[i * 4 + 3] = 0.0f;
        item.world[12 + i] = rows[i * 4 + 3];
      }
      item.world[15] = 1.0f;
      item.skinned = false;
      item.bones.clear();
      g_ropa_rigid.fetch_add(1, std::memory_order_relaxed);
      if (item.hair) {
        CaptureHairTint(base, item);
      }
      CaptureCharLighting(base, item);
      return true;
    }
  }
  constexpr uint32_t kPaletteFloats = 84 * 12;  // c4..c255 = up to 84 bones
  item.bones.resize(kPaletteFloats);
  {
    // One translated source pointer and a plain loop the compiler
    // vectorizes (byte swap + range clamp), instead of 1008 volatile
    // scalar guest loads that each redo the address translation. Same
    // unguarded read as LoadGuestF32: the bank is the bound VS bank.
    const uint8_t* src = REX_RAW_ADDR(bank + palette_base * 16);
    float* dst = item.bones.data();
    for (uint32_t i = 0; i < kPaletteFloats; ++i) {
      uint32_t w;
      std::memcpy(&w, src + size_t(i) * 4, 4);
      const float f = std::bit_cast<float>(__builtin_bswap32(w));
      dst[i] = (f > -1e7f && f < 1e7f) ? f : 0.0f;
    }
  }
  if (item.hair) {
    CaptureHairTint(base, item);
  }
  CaptureCharLighting(base, item);
  return true;
}

uint64_t DrawSequence() { return g_draw_seq.load(std::memory_order_relaxed); }

void OnWorldShadowRender(uint8_t* base, uint32_t owner) {
  // Retail sub_827FD170: "lwz r11,20204(r3); addi r25,r3,20200" (the dev
  // build's list sits at +20216 / +20220).
  constexpr uint32_t kWsListBegin = 20200;
  constexpr uint32_t kWsListEnd = 20204;
  if (!SceneEnabled() || owner < 0x10000 || !GuestReadableApprox(base, owner + kWsListBegin)) {
    return;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t begin = REX_LOAD_U32(owner + kWsListBegin);
  const uint32_t end = REX_LOAD_U32(owner + kWsListEnd);
  if (begin < 0x10000 || end < begin || (end - begin) % 4 != 0 || (end - begin) / 4 > 8192 ||
      !GuestReadableApprox(base, begin)) {
    return;
  }
  const size_t n = (end - begin) / 4;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  for (size_t k = 0; k < n && g_frame_ws_ctxs.size() < 8192; ++k) {
    const uint32_t c = REX_LOAD_U32(begin + uint32_t(k) * 4);
    if (c >= 0x10000) {
      g_frame_ws_ctxs.push_back(c);
    }
  }
}

uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path,
                             bool drew_inside) {
  if (!SceneEnabled()) {
    return 0;
  }
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return 0;
  }
  // Streaming can revoke any pointer in the ctx/record/mesh chain between
  // the game queueing the draw and this walk; recover raw-load read faults
  // for the whole capture (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // Items drawn inside an AUX perspective pass (skater-portrait RTTs) never
  // enter the frame at all: they share (ib,vb) buffers with the on-screen
  // player, so letting them sit PENDING lets them steal the player's own
  // post-draw fixups (FIFO oldest-pending) and publish a ghost at the
  // player's pose. drew_inside guarantees the bank is the pass's own, so
  // its c0..c3 viewproj identifies the pass (see BankIsAuxPerspective).
  if (drew_inside && BankIsAuxPerspective(base, bank)) {
    return 0;
  }
  // Guest-thread capture cost telemetry (folded per frame in BuildFrameScene).
  const auto perf_t0 = PerfClock::now();
  struct PerfFold {
    PerfClock::time_point t0;
    ~PerfFold() {
      g_capture_frame_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count());
    }
  } perf_fold{perf_t0};
  if (world_path) {
    // Cheap pre-check so the world path (~800 calls/frame) only pays the
    // full capture for the meshes that need per-draw transforms: skinned
    // (LOD pedestrians, jointed props) and rigid MODEL-SPACE props (position
    // fmt 32/26). Movable props (vending machines) can reach the frame ONLY
    // through the sort lists, rendered as absolute world geometry they
    // collapse at the origin. Absolute meshes (fmt 57) stay on the identity
    // world path.
    const uint32_t record = REX_LOAD_U32(ctx);
    if (!GuestReadableApprox(base, record)) return 0;
    const uint32_t mesh = REX_LOAD_U32(record);
    if (!GuestReadableApprox(base, mesh)) return 0;
    const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
    if (!GuestReadableApprox(base, vdesc)) return 0;
    const uint32_t num_elements = REX_LOAD_U16(vdesc + 8);
    if (num_elements == 0 || num_elements > 32) return 0;
    bool have_bw = false;
    bool have_bi = false;
    uint32_t pos_fmt = 0;
    bool have_pos = false;
    for (uint32_t i = 0; i < num_elements; ++i) {
      const uint32_t e = vdesc + 0x10 + i * 16;
      if (REX_LOAD_U16(e) != 0) continue;
      const uint32_t usage = REX_LOAD_U8(e + 9);
      if (usage == 0 && !have_pos) {
        pos_fmt = REX_LOAD_U32(e + 4) & 0x3F;
        have_pos = true;
      }
      if (usage == 1 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bw = true;
      if (usage == 2 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bi = true;
    }
    const bool skinned = have_bw && have_bi;
    const bool model_space = pos_fmt == 32 || pos_fmt == 26;
    if (!skinned && !model_space) {
      return 0;
    }
  }
  DrawItem item;
  if (!BuildItemGeometry(base, ctx, item)) {
    return 0;
  }
  // DIAGNOSTIC: every distinct material that takes a dynamic path (RenderMesh
  // or the sort-list world path), once, with the item's classification.
  {
    static std::mutex s_seen_mutex;
    static std::unordered_set<std::string> s_seen;
    const std::string mat = MeshMaterialName(item.mesh);
    const std::string key = std::string(world_path ? "W:" : "R:") + mat;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lock(s_seen_mutex);
      fresh = s_seen.size() < 512 && s_seen.insert(key).second;
    }
    if (fresh) {
      REXLOG_INFO("native-scene: dynamic capture [{}] material '{}' mesh {:08X} env_family {} "
                  "dynobj {} skinned {} pos_fmt {}",
                  world_path ? "world-path" : "RenderMesh", mat, item.mesh,
                  int(item.env_family), int(item.dynobj), item.skinned ? 1 : 0,
                  int(item.pos_fmt));
    }
  }
  item.ctx = ctx;  // identity key for the palette serve / entity store
  // The bank only provably holds THIS mesh's constants when the last draw
  // that flushed it bound this mesh's buffers (see g_last_draw_ibvb);
  // `drew_inside` alone also accepted banks left by another entity's inline
  // draws while this mesh's own draws were deferred (the walking-vehicle /
  // origin-vending-machine captures). Deferring those to the post-draw
  // (ib,vb) fixup pairs the state with the mesh's own real draw.
  const bool own_draw_last =
      drew_inside && g_last_draw_ibvb.load(std::memory_order_relaxed) ==
                         ((uint64_t(item.ib_obj) << 32) | item.vb_obj);
  if (drew_inside && !own_draw_last) {
    g_capture_foreign_bank.fetch_add(1, std::memory_order_relaxed);
  }
  // Rigid transform: deferred (multi-pass) rigid props draw later; the
  // bank belongs to some earlier mesh, and a leftover identity matrix at c4
  // VALIDATES as a plausible world (verified from recorded draw streams:
  // 4 of 6 vending-machine clones captured exact identity and rendered
  // invisibly at the origin). Defer those to the post-draw fixup, like
  // skinned palettes.
  if (!item.skinned) {
    if (!world_path && item.ctx != 0) {
      // Mark the mesh as dynamically dispatched (see g_dyn_dispatch_meshes):
      // far clones of it arriving through the static sort lists need their
      // instance matrix, not the world-item identity.
      if (g_dyn_dispatch_meshes.size() > 16384) {
        g_dyn_dispatch_meshes.clear();
      }
      g_dyn_dispatch_meshes[item.mesh] = g_guest_frame;
    }
    // The instance's own matrix (ReadCtxInstanceWorld) outranks the bank:
    // own_draw_last only proves SOME clone of this mesh drew last (clones
    // share (ib,vb)), so under batched dispatch the bank world routinely
    // belongs to another clone - pieces visibly swap placements and flip
    // per frame (the park-editor shuffle). The bank still wins while it
    // AGREES with the instance matrix (movables: the bank is the exact
    // value the pass consumed this frame); a disagreement beyond small
    // motion is the wrong-clone signature.
    float ctx_world[16];
    const bool have_ctx_world =
        ReadCtxInstanceWorld(base, item.ctx, ctx_world);
    // Aux perspective passes (portrait RTTs) stage worlds at the off-map
    // portrait stage; defer like a foreign bank (see BankIsAuxPerspective).
    const bool bank_ok = own_draw_last && !BankIsAuxPerspective(base, bank) &&
                         BankRigidWorld(base, bank, item.world);
    if (bank_ok && have_ctx_world) {
      // Translation-only trigger: wrong-clone worlds sit meters apart;
      // physics-animated props (dumpsters) can carry live ROTATION in the
      // bank that the instance matrix trails, so same-spot rotation
      // deltas keep the bank.
      const float dx = item.world[12] - ctx_world[12];
      const float dy = item.world[13] - ctx_world[13];
      const float dz = item.world[14] - ctx_world[14];
      if (dx * dx + dy * dy + dz * dz > 0.0625f) {
        std::memcpy(item.world, ctx_world, sizeof(ctx_world));
        g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (have_ctx_world) {
      std::memcpy(item.world, ctx_world, sizeof(ctx_world));
      g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
    } else if (!bank_ok) {
      item.pending = true;
      g_rigid_pending.fetch_add(1, std::memory_order_relaxed);
    }
    if (!item.pending && item.ctx != 0) {
      if (g_rigid_world_cache.size() > 32768) {
        g_rigid_world_cache.clear();  // map-change growth backstop
      }
      RigidWorldCache& rc = g_rigid_world_cache[item.ctx];
      rc.mesh = item.mesh;
      std::memcpy(rc.world, item.world, sizeof(rc.world));
      rc.frame = g_guest_frame;
    }
  }

  if (item.skinned) {
    // Copy the whole possible palette span (c4..c255 = 84 bones) verbatim:
    // 3 float4 rows per bone, column-vector affine [R | t], model space
    // directly to world space (verified live: bone translations match the
    // camera focus). The shader applies rows with explicit dot products,
    // sidestepping HLSL matrix packing. Registers beyond the mesh's real
    // bone count hold stale bank data but are never indexed. World stays
    // identity.
    //
    // The bank only holds this mesh's palette at c4 if its own draws ran
    // inside the call AND used the pre-pass layout. Multi-pass (deferred)
    // meshes draw later, and main-pass-layout banks have the camera at c4;
    // in both cases leave the palette pending for the post-draw fixup.
    const uint32_t effect_list = REX_LOAD_U32(ctx + kCtxEffectList);
    uint32_t passes = 1;
    if (GuestReadableApprox(base, effect_list)) {
      passes = REX_LOAD_U32(effect_list + kEffectListPassCount);
    }
    const uint32_t palette_base = BankPaletteBase(base, bank);
    // World-path captures come from the sort-list hook BEFORE any of the
    // mesh's draws ran; the bank belongs to some other mesh; always defer
    // to the post-draw fixup. Same when the last draw inside the submit
    // call was not this mesh's own (deferred mesh, foreign inline draws):
    // a stale/foreign bank can still hold plausible bone rows.
    item.pending = world_path || !own_draw_last || (passes > 1 && passes < 16) ||
                   palette_base == 0;
    if (!item.pending) {
      if (CaptureSkinnedState(base, bank, palette_base, item)) {
        g_palette_snapshots.fetch_add(1, std::memory_order_relaxed);
        item.dbg_src = 1;
        item.caster_bank = BankIsOrtho(base, bank);
      } else {
        item.pending = true;
      }
    }
    g_skinned_items.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_recording.load(std::memory_order_relaxed)) {
    // Preserve the buffer payload for offline decode (streaming arenas are
    // recycled long before the end-of-window memory snapshot).
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const uint64_t key = uint64_t(item.vb_addr) * 1099511628211ull ^ item.fingerprint;
    if (item.vb_bytes <= (1u << 20) && g_recorded_buffer_bytes < (512u << 20) &&
        g_recorded_buffer_keys.insert(key).second) {
      RecordedBuffer buf;
      buf.vb_addr = item.vb_addr;
      buf.ib_addr = item.ib_addr;
      buf.fingerprint = item.fingerprint;
      buf.vb.resize(item.vb_bytes);
      std::memcpy(buf.vb.data(), base + item.vb_addr, item.vb_bytes);
      buf.ib.resize(size_t(item.ib_count) * 2);
      std::memcpy(buf.ib.data(), base + item.ib_addr, buf.ib.size());
      g_recorded_buffer_bytes += buf.vb.size() + buf.ib.size();
      g_recorded_buffers.push_back(std::move(buf));
    }
  }
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  // Record shadow-pass submissions per ctx (even refused/pending captures):
  // the game's per-piece caster list = exactly the ctxs it submits through
  // ortho banks; the native atlas pass mirrors it (see g_frame_ortho_ctx).
  // Stamp the item copy too; the game draws its shadow passes FIRST, so
  // every later copy (main pass, fixups, rescues, LW store ingest) carries
  // the flag; a final pre-publish pass ORs the set over merged items.
  // The skate.Park world-shadow passes (the DMO exponential atlas) are ortho
  // too, but they are not cascade passes: the game's cascades hold only the
  // characters there (capture 1790871823: every environmentpark*_shadowPS
  // draw is a DMO pass), and the park pieces' shadows come from the DMO map
  // alone. Counting those submissions cast every park piece into every
  // cascade (no row norm matches a cascade, so caster_cascades stayed 0 =
  // all): a second, camera-following copy of their shadows.
  const bool ortho_submit =
      !world_path && BankIsOrtho(base, bank) && ParkWsPassTile(base, bank) < 0;
  if (ortho_submit && ctx != 0) {
    g_frame_ortho_ctx.insert(ctx);
    const float bx = LoadGuestF32(base, bank + 0 * 4);
    const float by = LoadGuestF32(base, bank + 1 * 4);
    const float bz = LoadGuestF32(base, bank + 2 * 4);
    g_frame_ortho_mag.emplace(ctx, std::sqrt(bx * bx + by * by + bz * bz));
  }
  item.shadow_caster =
      ortho_submit || (ctx != 0 && g_frame_ortho_ctx.count(ctx) != 0);
  item.dyn_entity = true;
  g_frame_dynitems.push_back(std::move(item));
  const size_t index = g_frame_dynitems.size() - 1;
  if (g_frame_dynitems[index].pending ||
      (g_frame_dynitems[index].skinned && g_frame_dynitems[index].caster_bank &&
       !g_frame_dynitems[index].ropa)) {
    // Pending items wait for their first fixup; caster-bank captures stay
    // registered so the mesh's later main-pass draw REFRESHES the palette
    // (see DrawItem::caster_bank, stale wheel spin in the shadow banks).
    const DrawItem& d = g_frame_dynitems[index];
    g_frame_pending_by_buffers.emplace((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
    g_frame_pending_keys.insert((uint64_t(d.ib_obj) << 32) | d.vb_obj);
  } else if (g_frame_dynitems[index].char_family != 0) {
    // Character captured at submit-exit: the PS bank there can predate this
    // character's main pass; refresh the lighting rows on its later draws
    // (last successful capture wins).
    const DrawItem& d = g_frame_dynitems[index];
    RegisterCharRefresh((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
  }
  return uint32_t(index + 1);
}

uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key) {
  (void)r7;
  // sk83_na_m D3DDevice_DrawVertices(device, prim, start, count): the
  // particle renderer (lionREC_Render) draws prim 13 quad lists. The old
  // r6 == 0x80000000 gate matched another build's argument layout and
  // rejected every particle draw here.
  if (!SceneEnabled() || r4 != 13 || r6 < 4 ||
      !REXCVAR_GET(skate3_native_render_scene_quadlists)) {
    return 0;
  }
  // Quad-list draw: stream 0 is a dynamic ping-pong object whose vertex
  // fetch block (+0x18 dword0 = base|flags, +0x20 = size in bytes) points at
  // the CPU-simulated vertices for this frame: stride 24 = {float3 world
  // position (BE), packed normal, float2 uv}, quad-list topology (verified
  // offline from recorded payloads). NOTE: every capture examined so far is
  // a PARTICLE system (disjoint 2-4cm sprites), which is why rendering is
  // gated off by default; see the cvar.
  const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
  if (!GuestReadableApprox(base, vb_obj)) {
    return 0;
  }
  GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFC;
  const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
  constexpr uint32_t kStride = 24;
  if (addr < 0x10000 || size < kStride * 4 || size > (1u << 20) || size % kStride != 0) {
    return 0;
  }
  // lionREC_RenderVS layout (ucode + updb formats): stride 24 = float3
  // world position, u8x4 color (fetched .zyxw), float2 uv. The draw covers
  // vertices [r5, r5 + r6) of the ring.
  const uint32_t start = r5;
  const uint32_t ring_verts = size / kStride;
  if (start >= ring_verts) {
    return 0;
  }
  const uint32_t verts = std::min(r6, ring_verts - start);
  const uint32_t quads = verts / 4;
  if (quads == 0) {
    return 0;
  }

  const uint32_t garment_key = vb_obj ^ (start * 2654435761u);
  DrawItem item{};
  item.mesh = garment_key;
  item.vb_obj = vb_obj;
  item.ib_obj = 0;
  item.vb_addr = addr + start * kStride;
  item.vb_bytes = (quads * 4) * kStride;
  item.ib_addr = 0;
  item.ib_count = quads * 6;
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.pos_offset = 0;
  item.pos_fmt = 57;  // float3
  item.uv_offset = 16;
  item.uv_fmt = 38;  // float2
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.uv3_offset = 0;
  item.uv3_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.stride = kStride;
  item.skinned = false;
  item.pending = false;
  item.cloth_quads = true;
  // lionREC_RenderPS: oC0 = tf0(uv) * vertex color, depth test without
  // z-write. The sprite rides fetch slot 0 (draw-time words). The BLEND is
  // per particle system: the landing dust is straight alpha (0x00010706),
  // sparks/glows add. Record this draw's own render state (this hook runs
  // right after the guest's DrawVertices, like OnDrawDone's capture) so the
  // render pass blends each system the way the game does; drawing additive
  // sprites (black surround, alpha 1) with straight alpha showed them as
  // dark squares.
  item.transparent = true;
  {
    const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed);
    const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
    if (p2200 != 0 && p2100 != 0) {
      item.game_rs[0] = REX_LOAD_U32(p2200 + 0 * 4);    // RB_DEPTHCONTROL
      item.game_rs[1] = REX_LOAD_U32(p2200 + 1 * 4);    // RB_BLENDCONTROL0
      item.game_rs[2] = REX_LOAD_U32(p2200 + 2 * 4);    // RB_COLORCONTROL
      item.game_rs[3] = REX_LOAD_U32(p2100 + 4 * 4);    // RB_COLOR_MASK
      item.game_rs[4] = REX_LOAD_U32(p2100 + 0xD * 4);  // RB_STENCILREFMASK
      item.game_rs[5] = REX_LOAD_U32(p2200 + 5 * 4);    // PA_SU_SC_MODE_CNTL
      item.game_rs_seen = item.game_rs[1] != 0;
    }
  }
  if (const uint32_t device = g_device.load(std::memory_order_relaxed)) {
    for (int k = 0; k < 6; ++k) {
      item.diffuse_fetch[k] = REX_LOAD_U32(device + 0x480 + uint32_t(k) * 4);
    }
  }
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  // Real world-space bounds of the sprites (identity world): the blended
  // pass sorts by bbox centre, and the old +-20000 box put every particle
  // system at the world origin, so its order against glass/hair/other
  // systems followed the camera's distance to the origin.
  {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const uint32_t nv = std::min(quads * 4u, 65536u);
    for (uint32_t v = 0; v < nv; ++v) {
      const uint32_t va = item.vb_addr + v * kStride;
      for (int axis = 0; axis < 3; ++axis) {
        const float c = std::bit_cast<float>(REX_LOAD_U32(va + uint32_t(axis) * 4));
        if (std::isfinite(c) && std::fabs(c) < 1e6f) {
          lo[axis] = std::min(lo[axis], c);
          hi[axis] = std::max(hi[axis], c);
        }
      }
    }
    const bool ok = lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2];
    for (int axis = 0; axis < 3; ++axis) {
      item.bbox_min[axis] = ok ? lo[axis] : -20000.0f;
      item.bbox_max[axis] = ok ? hi[axis] : 20000.0f;
    }
  }
  item.draws.push_back({4, 0, 0, quads * 6});

  // Content fingerprint over THIS draw's vertices (item.vb_addr, every
  // word; simulation output changes every frame -> the renderer re-decodes
  // each frame). It used to hash the ring's base address and 16 samples of
  // the ring's FIRST vb_bytes: every system past the ring head then kept a
  // stale decode whenever the head did not change, and the mesh key (ring
  // offset) is reused by whichever system lands there next, so a dust
  // system could draw with a spark system's vertex colours - alpha 1, the
  // "particles sometimes fully opaque" report.
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(item.vb_addr);
  mix(item.vb_bytes);
  const uint32_t words = std::min(item.vb_bytes / 8u, 65536u);
  for (uint32_t k = 0; k < words; ++k) {
    mix(REX_LOAD_U64(item.vb_addr + k * 8u));
  }
  item.fingerprint = h;

  // Distinct draw ranges within one ring buffer are distinct garments.
  *out_key = garment_key;
  item.dyn_entity = true;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  g_frame_dynitems.push_back(std::move(item));
  return uint32_t(g_frame_dynitems.size());
}

namespace {

// Guest shader objects carry their compiled-source debug path (see
// ShaderPathAddr: +0x54 for pixel shaders, +0x394 for vertex shaders)
// ("D:\P4\xbox2-ww-f\...\spline_darkenPS.updb"); the spline renderer's pixel
// shaders identify the in-world neon guide elements. Cached per object;
// the current pixel/vertex object trackers are checked both ways because the
// hook labels are swapped relative to the real shader types.
uint32_t ClassifySplineShader(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  static std::mutex mu;
  static std::unordered_map<uint32_t, uint32_t> cache;
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  // 200 bytes: the debug path is a ~77-char build-directory prefix plus the
  // leaf, so a shorter buffer both truncated the name this match needs and,
  // being filled with a 200-byte copy into a 97-byte array, smashed the stack
  // of whatever guest thread issued the draw.
  char path[201] = {};
  uint32_t kind = 0;
  if (GuestTryCopy(path, base + ShaderPathAddr(base, obj), 200)) {
    path[200] = '\0';
    // Pixel-shader names only: both passes bind the SAME vertex shader,
    // spline_defaultVS, and the swapped tracker checked first holds it, so
    // a bare "spline_default" match classified every darken draw as the
    // additive glow pass (drawn with the darken texture, whose alpha falls
    // to 0 at the band edges: rgb / a blew its red up into a red rim).
    if (std::strstr(path, "spline_darkenPS") != nullptr) {
      kind = 1;
    } else if (std::strstr(path, "spline_defaultPS") != nullptr) {
      kind = 2;
    }
  }
  cache.emplace(obj, kind);
  return kind;
}

}  // namespace

void OnSetIndices(uint32_t ib_obj) { g_cur_ib.store(ib_obj, std::memory_order_relaxed); }

void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride) {
  if (stream == 0) {
    g_cur_vb.store(vb_obj, std::memory_order_relaxed);
    g_cur_vb_offset.store(offset, std::memory_order_relaxed);
    g_cur_vb_stride.store(stride, std::memory_order_relaxed);
  }
  if (stream < 4) {
    g_cur_streams[stream][0].store(vb_obj, std::memory_order_relaxed);
    g_cur_streams[stream][1].store(offset, std::memory_order_relaxed);
    g_cur_streams[stream][2].store(stride, std::memory_order_relaxed);
  }
}

// Per-shader-object name classification for OnDrawDone's family row
// captures (flowing water, scroll, ocean, transparent environment, ads, ocean
// reflection). Those checks used to copy the guest debug path byte by byte
// and strstr it on EVERY main-pass draw, for both shader objects, while their
// family had not been seen this frame -- i.e. every draw of every frame in a
// scene without that family (~60% of the per-draw hook cost). The result is a
// fixed property of the shader object: memoised here, same 119-char read and
// same substring tests; empty / unreadable paths are not cached. Guest render
// thread only.
enum : uint32_t {
  kSnFlowWater = 1u << 0,
  kSnScroll = 1u << 1,
  kSnOcean = 1u << 2,
  kSnTransEnv = 1u << 3,
  kSnAd = 1u << 4,
  kSnOceanRefl = 1u << 5,
};
uint32_t ShaderNameFlags(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
    return 0;
  }
  struct Memo {
    uint32_t obj;
    uint32_t bits;
  };
  static thread_local Memo memo[256] = {};
  Memo& m = memo[((obj >> 4) ^ (obj >> 12)) & 255u];
  if (m.obj == obj) {
    return m.bits;
  }
  char text[120] = {};
  const uint32_t shader_path = ShaderPathAddr(base, obj);
  for (int k = 0; k < 119; ++k) {
    text[k] = char(REX_LOAD_U8(shader_path + k));
    if (text[k] == '\0') break;
  }
  if (text[0] == '\0') {
    return 0;
  }
  uint32_t bits = 0;
  // Both flowing-water shaders (flowingwater_defaultPS for water.flowing,
  // flowingwateralpha_defaultPS for water.flowingalpha): same register
  // layout, they differ only in the alpha output.
  if (std::strstr(text, "\\flowingwater") != nullptr) bits |= kSnFlowWater;
  if (std::strstr(text, "\\scrollincandescent") != nullptr) bits |= kSnScroll;
  if (std::strstr(text, "\\ocean_default") != nullptr) bits |= kSnOcean;
  if (std::strstr(text, "\\transparentenvironment_default") != nullptr) bits |= kSnTransEnv;
  if (std::strstr(text, "\\advertisement") != nullptr) bits |= kSnAd;
  if (std::strstr(text, "\\oceanreflection") != nullptr) bits |= kSnOceanRefl;
  m = {obj, bits};
  return bits;
}

// CAC face / cloth / skin main-pass classification for the injury and
// dirt-layer probes in OnDrawDone: one memoised bitmask per shader object
// instead of up to eight BoundPsNameContains() calls per draw. Same 320-char
// path copy and substring tests as BoundPsNameContains, OR-ed over the pixel
// and vertex shader objects (a failed copy caches "no match", as there).
// Guest render thread only.
enum : uint32_t {
  kCsFaceDefault = 1u << 0,        // "cac_face_defaultPS"
  kCsFaceNis = 1u << 1,            // "cac_face_nisPS"
  kCsClothDefault = 1u << 2,       // "cac_cloth_default"
  kCsClothNis = 1u << 3,           // "cac_cloth_nis"
  kCsClothRopaDefault = 1u << 4,   // "cac_cloth_ropa_default"
  kCsClothRopaNis = 1u << 5,       // "cac_cloth_ropa_nis"
  kCsStampClothDefault = 1u << 6,  // "cacstamp_cloth_default"
  kCsStampClothRopa = 1u << 7,     // "cacstamp_cloth_ropa_default"
  kCsStampSkinDefault = 1u << 8,   // "cacstamp_skin_default"
  kCsStampSkinNis = 1u << 9,       // "cacstamp_skin_nis"
};
uint32_t CharShaderFlagsOf(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  struct Memo {
    uint32_t obj;
    uint32_t bits;
  };
  static thread_local Memo memo[256] = {};
  Memo& m = memo[((obj >> 4) ^ (obj >> 12)) & 255u];
  if (m.obj == obj) {
    return m.bits;
  }
  char name[321] = {};
  uint32_t bits = 0;
  if (GuestTryCopy(name, base + ShaderPathAddr(base, obj), 320)) {
    name[320] = 0;
    static const struct {
      const char* needle;
      uint32_t bit;
    } kNeedles[] = {
        {"cac_face_defaultPS", kCsFaceDefault},
        {"cac_face_nisPS", kCsFaceNis},
        {"cac_cloth_default", kCsClothDefault},
        {"cac_cloth_nis", kCsClothNis},
        {"cac_cloth_ropa_default", kCsClothRopaDefault},
        {"cac_cloth_ropa_nis", kCsClothRopaNis},
        {"cacstamp_cloth_default", kCsStampClothDefault},
        {"cacstamp_cloth_ropa_default", kCsStampClothRopa},
        {"cacstamp_skin_default", kCsStampSkinDefault},
        {"cacstamp_skin_nis", kCsStampSkinNis},
    };
    for (const auto& n : kNeedles) {
      if (std::strstr(name, n.needle) != nullptr) {
        bits |= n.bit;
      }
    }
  }
  m = {obj, bits};
  return bits;
}
uint32_t BoundCharShaderFlags(uint8_t* base) {
  return CharShaderFlagsOf(base, g_cur_ps_obj.load(std::memory_order_relaxed)) |
         CharShaderFlagsOf(base, g_cur_vs_obj.load(std::memory_order_relaxed));
}

// Is this shader object a `_skatepark` permutation of a WORLD pixel shader
// (see g_frame_dmo_keys)? The character / CAC, dynamicobject and
// environmentpark permutations keep other register layouts and shading
// models and are excluded, and so is defaultenvironment's (no tf2 fetch).
// Memoised per object like CharShaderFlagsOf
// (the paths run past ShaderNameFlags' 119-char window: "...\xbox2\
// baseenvironmentreflective_skateparkPS.updb" is 118). Guest render thread
// only.
bool SkateparkWorldPsOf(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return false;
  }
  struct Memo {
    uint32_t obj;
    bool hit;
  };
  static thread_local Memo memo[256] = {};
  Memo& m = memo[((obj >> 4) ^ (obj >> 12)) & 255u];
  if (m.obj == obj) {
    return m.hit;
  }
  char name[321] = {};
  bool hit = false;
  if (GuestTryCopy(name, base + ShaderPathAddr(base, obj), 320)) {
    name[320] = 0;
    hit = std::strstr(name, "_skateparkPS") != nullptr &&
          std::strstr(name, "\\cac") == nullptr &&
          std::strstr(name, "character") == nullptr &&
          std::strstr(name, "dynamicobject") == nullptr &&
          std::strstr(name, "environmentpark") == nullptr &&
          // The one world permutation without the park map: its ucode
          // never fetches tf2 (the binding merely persists from the
          // previous draw). Its alphatest / transparent siblings do.
          std::strstr(name, "\\defaultenvironment_skateparkPS") == nullptr;
  }
  m = {obj, hit};
  return hit;
}
bool BoundSkateparkWorldPs(uint8_t* base) {
  return SkateparkWorldPsOf(base, g_cur_ps_obj.load(std::memory_order_relaxed)) ||
         SkateparkWorldPsOf(base, g_cur_vs_obj.load(std::memory_order_relaxed));
}

// Is this shader object a main-pass environment-decal pixel shader
// (decalenvironment[_simple][_tileable] / decal2environment, _defaultPS or
// _skateparkPS)? All of them fetch the decal art at tf4 (shader_dump);
// returns 2 for decal2environment (second art layer at tf5), 1 for the
// other decal families, 3 for trafficlight_one/two, 4 for defaulthom, 5 for
// flowingwater[alpha], 6 for advertisement, 0 otherwise. Callers
// test BOTH shader trackers (the hook labels are swapped in this build);
// only the pixel shader's name carries the PS suffix this matches.
// Excludes environmentparkdecal (park pieces, art at tf3) and the heatmap
// debug permutations. Memoised per object like SkateparkWorldPsOf. Guest
// render thread only.
int WorldPsKindOf(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  struct Memo {
    uint32_t obj;
    int hit;
  };
  static thread_local Memo memo[256] = {};
  Memo& m = memo[((obj >> 4) ^ (obj >> 12)) & 255u];
  if (m.obj == obj) {
    return m.hit;
  }
  char name[321] = {};
  int hit = 0;
  if (GuestTryCopy(name, base + ShaderPathAddr(base, obj), 320)) {
    name[320] = 0;
    const bool main_pass = std::strstr(name, "_defaultPS") != nullptr ||
                           std::strstr(name, "_skateparkPS") != nullptr;
    if (main_pass && std::strstr(name, "\\decal2environment") != nullptr) {
      hit = 2;
    } else if (main_pass && std::strstr(name, "\\decalenvironment") != nullptr) {
      hit = 1;
    } else if (main_pass && std::strstr(name, "\\trafficlight_") != nullptr) {
      hit = 3;
    } else if (main_pass && std::strstr(name, "\\defaulthom_") != nullptr) {
      hit = 4;
    } else if (main_pass && std::strstr(name, "\\flowingwater") != nullptr) {
      hit = 5;  // flowingwater_defaultPS / flowingwateralpha_defaultPS
    } else if (main_pass && std::strstr(name, "\\advertisement_") != nullptr) {
      hit = 6;  // advertisement_defaultPS / _skateparkPS
    }
  }
  m = {obj, hit};
  return hit;
}

// Shader census classification, memoised per object: 1 = a pixel shader
// worth recording (name ends in "PS"), 2 = a prepass / caster / occlusion
// pixel shader (filtered), 0 = not a pixel shader or unreadable.
int CensusPsKind(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  struct Memo {
    uint32_t obj;
    int kind;
  };
  static thread_local Memo memo[512] = {};
  // Higher address bits folded in: shader objects allocated at a large
  // alignment share their low bits and would evict each other every draw
  // (each miss re-reads the shader path).
  Memo& m = memo[((obj >> 4) ^ (obj >> 13)) & 511u];
  if (m.obj == obj) {
    return m.kind;
  }
  const std::string leaf = ShaderLeafName(base, obj);
  int kind = 0;
  if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "PS") == 0) {
    kind = (leaf.find("ZPrePass") != std::string::npos ||
            leaf.find("WorldShadow") != std::string::npos ||
            leaf.find("occlusionPass") != std::string::npos ||
            leaf.find("_shadowPS") != std::string::npos)
               ? 2
               : 1;
  }
  m = {obj, kind};
  return kind;
}

// DIAGNOSTIC (perf_log only): average guest draws per frame by pass, to see
// which passes a busy view's draw count comes from. Pixel-shader name class,
// memoised per object: 1 main, 2 z-prepass, 3 world-shade, 4 occlusion,
// 5 caster (_shadowPS), 0 unknown.
int PassPsClass(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  struct Memo {
    uint32_t obj;
    int cls;
  };
  static thread_local Memo memo[512] = {};
  Memo& m = memo[((obj >> 4) ^ (obj >> 13)) & 511u];
  if (m.obj == obj) {
    return m.cls;
  }
  const std::string leaf = ShaderLeafName(base, obj);
  int cls = 0;
  if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "PS") == 0) {
    cls = leaf.find("ZPrePass") != std::string::npos        ? 2
          : leaf.find("WorldShadow") != std::string::npos   ? 3
          : leaf.find("occlusionPass") != std::string::npos ? 4
          : leaf.find("_shadowPS") != std::string::npos     ? 5
                                                            : 1;
  }
  m = {obj, cls};
  return cls;
}

void DrawPassCensus(uint8_t* base, uint32_t flags2d, bool bank_ortho, uint32_t bank) {
  // [0] 2d, [1] aux perspective, [2..7] perspective by class, [8..13] ortho.
  static const char* const kNames[14] = {"2d",       "aux",       "p:unk",   "p:main",
                                         "p:zpre",   "p:wshade",  "p:occl",  "p:cast",
                                         "o:unk",    "o:main",    "o:zpre",  "o:wshade",
                                         "o:occl",   "o:cast"};
  static uint64_t s_count[14] = {};
  static uint64_t s_first_frame = 0;
  int k;
  if (flags2d != 0) {
    k = 0;
  } else if (!bank_ortho && BankIsAuxPerspective(base, bank)) {
    k = 1;
  } else {
    int cls = PassPsClass(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (cls == 0) {
      cls = PassPsClass(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    k = (bank_ortho ? 8 : 2) + cls;
    if (cls == 0) {
      // First distinct unclassified (PS, VS) pairs, by name.
      static FlatSet<uint64_t> s_unk_seen;
      const uint32_t ps = g_cur_ps_obj.load(std::memory_order_relaxed);
      const uint32_t vs = g_cur_vs_obj.load(std::memory_order_relaxed);
      if (s_unk_seen.size() < 24 && s_unk_seen.insert((uint64_t(ps) << 32) | vs).second) {
        REXLOG_INFO("native-scene: draws-by-pass unclassified {} ps={:08X} '{}' vs={:08X} '{}'",
                    bank_ortho ? "ortho" : "persp", ps, ps >= 0x10000 ? ShaderLeafName(base, ps) : "",
                    vs, vs >= 0x10000 ? ShaderLeafName(base, vs) : "");
      }
    }
  }
  ++s_count[k];
  // Per dispatching sort list (offset into the view) x pass class.
  static FlatMap<uint64_t, uint64_t> s_by_list;
  if (s_by_list.size() < 256) {
    ++s_by_list[(uint64_t(g_dispatch_list_offset) << 8) | uint64_t(k)];
  }
  const uint64_t frames = g_guest_frame - s_first_frame;
  if (s_first_frame == 0 || frames > 100000) {
    s_first_frame = g_guest_frame;
    std::fill(std::begin(s_count), std::end(s_count), 0);
  } else if (frames >= 600) {
    std::string line;
    for (int i = 0; i < 14; ++i) {
      if (s_count[i] != 0) {
        line += fmt::format(" {}={:.0f}", kNames[i], double(s_count[i]) / double(frames));
      }
    }
    REXLOG_INFO("native-scene: draws per frame by pass:{}", line);
    std::string lists;
    s_by_list.for_each([&](uint64_t key, uint64_t n) {
      const double per = double(n) / double(frames);
      if (per >= 1.0) {
        lists += fmt::format(" {}@{}={:.0f}", kNames[key & 0xFF], key >> 8, per);
      }
    });
    REXLOG_INFO("native-scene: draws per frame by sort list:{}", lists);
    s_by_list.clear();
    s_first_frame = g_guest_frame;
    std::fill(std::begin(s_count), std::end(s_count), 0);
  }
}

// A `_foregroundPS` park / dynamicobject variant (the editor's preview
// object, see DrawItem::foreground): the first VS register of the world
// rows its paired _defaultVS reads (environmentparkdecal c11..c14,
// environmentparkdiffuse c9..c12, environmentparkalphatest / dynamicobject /
// alphatestdynamicobject c8..c11 -- each ucode's 9-11 mad chain), else -1;
// | 0x100 for the dynamicobject variants.
// Memoised per shader object. Guest render thread only.
// `variant` picks the pixel-shader variant suffix: "_foregroundPS" (the
// pick-an-object preview) or "_defaultPS" (the editor's selected piece, see
// DrawItem::xray).
int ParkWorldRegOf(uint8_t* base, uint32_t obj, const char* variant) {
  if (obj < 0x10000) {
    return -1;
  }
  struct Memo {
    uint32_t obj;
    const char* variant;
    int reg;
  };
  static thread_local Memo memo[256] = {};
  Memo& m = memo[((obj >> 4) ^ (uint32_t(reinterpret_cast<uintptr_t>(variant)) >> 3)) & 255u];
  if (m.obj == obj && m.variant == variant) {
    return m.reg;
  }
  char name[321] = {};
  int reg = -1;
  if (GuestTryCopy(name, base + ShaderPathAddr(base, obj), 320)) {
    name[320] = 0;
    if (std::strstr(name, variant) != nullptr) {
      if (std::strstr(name, "environmentparkdecal_") != nullptr) {
        reg = 11;
      } else if (std::strstr(name, "environmentparkdiffuse_") != nullptr) {
        reg = 9;
      } else if (std::strstr(name, "environmentparkalphatest_") != nullptr) {
        reg = 8;
      } else if (std::strstr(name, "dynamicobject_") != nullptr) {
        // 0x100: a dynamicobject variant (see the row capture); 0x200: the
        // alphatest one, whose compact bank shifts the rows.
        reg = 8 | 0x100 | (std::strstr(name, "alphatestdynamicobject_") != nullptr ? 0x200 : 0);
      }
    }
  }
  m = {obj, variant, reg};
  return reg;
}
int BoundForegroundWorldReg(uint8_t* base) {
  static const char* const kV = "_foregroundPS";
  const int a = ParkWorldRegOf(base, g_cur_ps_obj.load(std::memory_order_relaxed), kV);
  return a >= 0 ? a : ParkWorldRegOf(base, g_cur_vs_obj.load(std::memory_order_relaxed), kV);
}
int BoundParkHighlightWorldReg(uint8_t* base) {
  static const char* const kV = "_highlightPS";
  const int a = ParkWorldRegOf(base, g_cur_ps_obj.load(std::memory_order_relaxed), kV);
  return a >= 0 ? a : ParkWorldRegOf(base, g_cur_vs_obj.load(std::memory_order_relaxed), kV);
}
int BoundParkDefaultWorldReg(uint8_t* base) {
  static const char* const kV = "_defaultPS";
  const int a = ParkWorldRegOf(base, g_cur_ps_obj.load(std::memory_order_relaxed), kV);
  return a >= 0 ? a : ParkWorldRegOf(base, g_cur_vs_obj.load(std::memory_order_relaxed), kV);
}

// DIAGNOSTIC OnDrawDone section profiler (perf_log only; guest render thread):
// cycles per section via __rdtsc, attributed to the section running at each
// boundary or return. Read out by the guest-frame breakdown
// (DrawSectionShares).
constexpr int kDrawSectCount = 21;
const char* const kDrawSectNames[kDrawSectCount] = {
    "prolog", "census", "regstate", "fogsky", "pfx", "fisheye", "bloom",
    "uiblur", "outline", "2d", "spline", "record", "tail",
    "t:isl", "t:census", "t:wsrecv", "t:props", "t:fam", "t:9154", "t:charref", "t:pending"};
inline uint64_t DrawSectCycles() {
#if defined(_WIN32)
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
bool g_draw_sect_on = false;
uint64_t g_draw_sect_cyc[kDrawSectCount] = {};
int g_draw_sect_cur = 0;
uint64_t g_draw_sect_t = 0;
inline void DrawSectMark(int next) {
  if (!g_draw_sect_on) return;
  const uint64_t now = DrawSectCycles();
  g_draw_sect_cyc[g_draw_sect_cur] += now - g_draw_sect_t;
  g_draw_sect_cur = next;
  g_draw_sect_t = now;
}
struct DrawSectScope {
  DrawSectScope() {
    if (g_draw_sect_on) {
      g_draw_sect_cur = 0;
      g_draw_sect_t = DrawSectCycles();
    }
  }
  ~DrawSectScope() { DrawSectMark(0); }
};

void EnableDrawSectProfiling(bool on) { g_draw_sect_on = on; }

std::string DrawSectionShares(bool enable_next) {
  uint64_t total = 0;
  for (uint64_t c : g_draw_sect_cyc) total += c;
  std::string out;
  if (total != 0) {
    for (int i = 0; i < kDrawSectCount; ++i) {
      const double pct = 100.0 * double(g_draw_sect_cyc[i]) / double(total);
      if (pct >= 0.5) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %s=%.0f%%", kDrawSectNames[i], pct);
        out += buf;
      }
    }
  }
  for (uint64_t& c : g_draw_sect_cyc) c = 0;
  g_draw_sect_on = enable_next;
  return out;
}

// flowingwater[alpha]_defaultPS material rows from the PIXEL bank, in the
// FrameScene::water_rows layout, plus the range gate (see the frame capture
// in OnDrawDone for the per-row ucode references).
bool ReadFlowWaterRows(uint8_t* base, uint32_t ps_bank, float rows[32]) {
  // Retail Skate 3 flowingwater[alpha]_defaultPS (tools/sk83_port/
  // disasm_retail). The constant file is c11..c14 = m_params[0..3] and
  // c15.x = g_fAnimationTime; the terms the sk83_na_m dev build reads from
  // its longer g_envattributes block (spec tint c12, lightmap floor c10.w,
  // reflection gate c14.yzw, reflection strength c13.w) are compile-time
  // literals in retail, filled in below from the shader's literal block.
  // Row slots keep the layout the constant packing and ShadeFlowingWater
  // consume.
  std::memset(rows, 0, 32 * sizeof(float));
  const auto wrow = [&](uint32_t r, uint32_t c) {
    return LoadGuestF32(base, ps_bank + (r * 4 + c) * 4);
  };
  for (int c = 0; c < 4; ++c) {
    rows[0 + c] = wrow(11u, uint32_t(c));   // wat_p0 = m_params[0]
    rows[4 + c] = wrow(12u, uint32_t(c));   // scroll speeds   (ucode 8:  mul r6, c12, c15.x)
    rows[8 + c] = wrow(13u, uint32_t(c));   // tap uv scales   (ucode 9:  mad r6, uv, c13, r6)
    rows[24 + c] = wrow(11u, uint32_t(c));  // normal scales c11.w/z/x + multiplier c11.y
  }                                         //   (ucode 26: mul r11, r11.zyx, c11.wzx; 73: c11.y)
  rows[16] = wrow(15u, 0);  // animation time c15.x
  rows[19] = wrow(14u, 1);  // mask threshold c14.y (ucode 16/24)
  rows[17] = wrow(14u, 2);  // spec power c14.z (ucode 49-51: log, mul c14.z, exp)
  // Alpha floor: flowingwateralpha writes max(alpha, c14.w) (ucode 94);
  // flowingwater_defaultPS writes alpha 1 (oC0._001), which the shading
  // already selects per family.
  rows[18] = wrow(14u, 3);
  // Literal-block terms (c248..c255, identical in both water shaders):
  //   reflection gate 0.3 * sat(4 * mask - 2.6) = x * sat(y * (mask - z))
  //   with (x, y, z) = (0.3, 4, 0.65); strength 1.5 (c254.z);
  //   spec tint (2.1, 1.8, 1.5) (c249.xy, c254.z), stored /3 because the
  //   packing multiplies by the dev shader's literal 3; lightmap floor 0.1
  //   (c254.w).
  rows[13] = 0.3f;
  rows[14] = 4.0f;
  rows[15] = 0.65f;
  rows[28] = 1.5f;
  rows[21] = 2.1f / 3.0f;
  rows[22] = 1.8f / 3.0f;
  rows[23] = 1.5f / 3.0f;
  rows[20] = 0.1f;
  // Range gate: multiplier small positive, threshold / floor blend
  // factors, spec power a real exponent, speeds/scales bounded, time
  // non-negative.
  bool sane = rows[25] > 0.0f && rows[25] <= 8.0f &&    // c11.y multiplier
              rows[19] >= 0.0f && rows[19] <= 1.0f &&   // c14.y threshold
              rows[17] >= 1.0f && rows[17] <= 4096.0f && // c14.z power
              rows[18] >= 0.0f && rows[18] <= 1.0f &&   // c14.w floor
              rows[16] >= 0.0f && rows[16] < 1e7f;      // time
  for (int i = 0; i < 4 && sane; ++i) {
    sane = std::fabs(rows[4 + i]) <= 64.0f &&   // c12 speeds
           std::fabs(rows[8 + i]) <= 256.0f;    // c13 scales
  }
  for (int i : {0, 2, 3}) {  // c11.x/z/w normal scales
    sane = sane && std::fabs(rows[24 + i]) <= 8.0f;
  }
  return sane;
}

// SimpleDraw vertex-shader layout of a shader object (see SimpleDrawCap):
// 1 = simpledraw_p_sc / p_n_sc (constant colour c8), 2 = simpledraw_p_c
// (per-vertex float4 colour at +16), 3 = another simpledraw shader (PS, or a
// textured / depth-only VS: not replayed), 0 = not SimpleDraw. Memoised;
// guest render thread only.
int SimpleDrawKindOf(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  static std::unordered_map<uint32_t, int> memo;
  const auto it = memo.find(obj);
  if (it != memo.end()) {
    return it->second;
  }
  const std::string leaf = ShaderLeafName(base, obj);
  int kind = 0;
  if (leaf.rfind("simpledraw", 0) == 0 || leaf.rfind("SimpleDraw", 0) == 0) {
    kind = 3;
    if (leaf == "simpledraw_p_sc" || leaf == "simpledraw_p_n_sc") {
      kind = 1;
    } else if (leaf == "simpledraw_p_c") {
      kind = 2;
    }
  }
  if (memo.size() < 4096) {
    memo.emplace(obj, kind);
  }
  return kind;
}

// The bound SimpleDraw layout for the current draw (either tracker: the hook
// labels are swapped in this build): 1/2 = replayable (SimpleDrawKindOf),
// 3 = SimpleDraw but not replayed, 0 = not SimpleDraw.
int BoundSimpleDrawKind(uint8_t* base) {
  const int a = SimpleDrawKindOf(base, g_cur_ps_obj.load(std::memory_order_relaxed));
  const int b = SimpleDrawKindOf(base, g_cur_vs_obj.load(std::memory_order_relaxed));
  if (a == 1 || a == 2) return a;
  if (b == 1 || b == 2) return b;
  return std::max(a, b);
}

// Records one SimpleDraw inline draw (see SimpleDrawCap). The vertex / index
// payload is read at frame end.
void CaptureSimpleDraw(uint8_t* base, int layout, uint32_t prim, uint32_t vcount,
                       uint32_t stride, uint32_t vaddr, uint32_t icount, uint32_t iaddr) {
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0 || vaddr < 0x10000 || vcount == 0 || vcount > 65536 || stride < 16 ||
      stride > 256 || (layout == 2 && stride < 32) || (icount != 0 && iaddr < 0x10000) ||
      icount > 196608) {
    return;
  }
  SimpleDrawCap c;
  c.prim = prim;
  c.vcount = vcount;
  c.stride = stride;
  c.vaddr = vaddr;
  c.icount = icount;
  c.iaddr = iaddr;
  c.layout = uint32_t(layout);
  for (int i = 0; i < 40; ++i) {
    c.consts[i] = LoadGuestF32(base, bank + uint32_t(i) * 4);
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  if (g_frame_sdraw.size() < 8192) {
    g_frame_sdraw.push_back(c);
  }
}

// Frame end: read each captured SimpleDraw payload, transform it by its
// draw's world rows (c4..c7) and expand it into the published triangle list
// (lines become screen-space quads in the replay VS, see spline.hlsl
// vs_debug). Projected by the scene's view_proj on the render side, like
// the splines.
void PublishSimpleDraws(uint8_t* base) {
  std::vector<SimpleDrawCap> caps;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    caps.swap(g_frame_sdraw);
  }
  std::vector<SimpleDrawVert> out;
  const auto ld = [&](uint32_t addr) {
    uint32_t w = 0;
    GuestTryCopy(&w, base + addr, 4);
    w = BSwap32(w);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
  };
  for (const SimpleDrawCap& c : caps) {
    if (!GuestRangeReadable(base, c.vaddr, c.vcount * c.stride)) {
      continue;
    }
    // Indices: 16-bit unless one of them would not fit the vertex range,
    // then 32-bit (the index format argument's encoding is unverified).
    std::vector<uint32_t> idx;
    if (c.icount != 0) {
      bool ok16 = GuestRangeReadable(base, c.iaddr, c.icount * 2);
      if (ok16) {
        idx.resize(c.icount);
        for (uint32_t i = 0; i < c.icount; ++i) {
          uint16_t v = 0;
          GuestTryCopy(&v, base + c.iaddr + i * 2, 2);
          idx[i] = uint16_t((v >> 8) | (v << 8));
          if (idx[i] >= c.vcount) {
            ok16 = false;
            break;
          }
        }
      }
      if (!ok16) {
        if (!GuestRangeReadable(base, c.iaddr, c.icount * 4)) {
          continue;
        }
        idx.resize(c.icount);
        bool ok32 = true;
        for (uint32_t i = 0; i < c.icount; ++i) {
          uint32_t v = 0;
          GuestTryCopy(&v, base + c.iaddr + i * 4, 4);
          idx[i] = BSwap32(v);
          ok32 = ok32 && idx[i] < c.vcount;
        }
        if (!ok32) {
          continue;
        }
      }
    } else {
      idx.resize(c.vcount);
      for (uint32_t i = 0; i < c.vcount; ++i) idx[i] = i;
    }
    struct V {
      float p[4];
      float col[4];
    };
    std::vector<V> v(c.vcount);
    const float* k = c.consts;
    for (uint32_t i = 0; i < c.vcount; ++i) {
      const uint32_t a = c.vaddr + i * c.stride;
      float p[4];
      for (int e = 0; e < 4; ++e) p[e] = ld(a + uint32_t(e) * 4);
      for (int r = 0; r < 4; ++r) {
        const float* row = k + (4 + r) * 4;
        v[i].p[r] = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3] * p[3];
      }
      for (int e = 0; e < 4; ++e) {
        v[i].col[e] = c.layout == 2 ? ld(a + 16 + uint32_t(e) * 4) : k[8 * 4 + e];
      }
      // UtRGBA expanded as 0..255 would read > 1; normalise if so.
      if (v[i].col[0] > 1.5f || v[i].col[1] > 1.5f || v[i].col[2] > 1.5f) {
        for (int e = 0; e < 4; ++e) v[i].col[e] *= 1.0f / 255.0f;
      }
    }
    struct {
      std::vector<SimpleDrawVert> verts;
    } b;
    const auto tri_vert = [&](const V& x) {
      SimpleDrawVert o{};
      std::memcpy(o.a, x.p, sizeof(o.a));
      std::memcpy(o.b, x.p, sizeof(o.b));
      std::memcpy(o.color, x.col, sizeof(o.color));
      b.verts.push_back(o);
    };
    const auto line = [&](const V& x, const V& y) {
      static constexpr float kCorner[6][2] = {{0, -1}, {0, 1}, {1, 1},
                                              {0, -1}, {1, 1}, {1, -1}};
      for (const auto& cr : kCorner) {
        SimpleDrawVert o{};
        std::memcpy(o.a, x.p, sizeof(o.a));
        std::memcpy(o.b, y.p, sizeof(o.b));
        const V& src = cr[0] < 0.5f ? x : y;
        std::memcpy(o.color, src.col, sizeof(o.color));
        o.corner[0] = cr[0];
        o.corner[1] = cr[1];
        b.verts.push_back(o);
      }
    };
    const size_t n = idx.size();
    switch (c.prim) {
      case 2:  // line list
        for (size_t i = 0; i + 1 < n; i += 2) line(v[idx[i]], v[idx[i + 1]]);
        break;
      case 3:  // line strip
        for (size_t i = 0; i + 1 < n; ++i) line(v[idx[i]], v[idx[i + 1]]);
        break;
      case 4:  // triangle list
        for (size_t i = 0; i + 2 < n; i += 3) {
          tri_vert(v[idx[i]]);
          tri_vert(v[idx[i + 1]]);
          tri_vert(v[idx[i + 2]]);
        }
        break;
      case 5:  // triangle fan
        for (size_t i = 1; i + 1 < n; ++i) {
          tri_vert(v[idx[0]]);
          tri_vert(v[idx[i]]);
          tri_vert(v[idx[i + 1]]);
        }
        break;
      case 6:  // triangle strip
        for (size_t i = 0; i + 2 < n; ++i) {
          tri_vert(v[idx[i]]);
          tri_vert(v[idx[i + 1 + (i & 1)]]);
          tri_vert(v[idx[i + 2 - (i & 1)]]);
        }
        break;
      case 13:  // quad list
        for (size_t i = 0; i + 3 < n; i += 4) {
          const uint32_t q[6] = {idx[i], idx[i + 1], idx[i + 2],
                                 idx[i], idx[i + 2], idx[i + 3]};
          for (uint32_t qi : q) tri_vert(v[qi]);
        }
        break;
      default:
        break;
    }
    if (b.verts.empty()) {
      continue;
    }
    out.insert(out.end(), b.verts.begin(), b.verts.end());
  }
  // A guest frame without SimpleDraw output keeps the previous lines for a
  // few frames (the debug draw need not run on every frame the scene is
  // built); longer gaps (the cvar turned off) clear them.
  if (!caps.empty()) {
    g_last_sdraw = std::move(out);
    g_sdraw_idle_frames = 0;
  } else if (++g_sdraw_idle_frames > 4) {
    g_last_sdraw.clear();
  }
}

void OnInlineIndexedDraw(uint8_t* base, uint32_t prim, uint32_t num_verts,
                         uint32_t index_count, uint32_t stride, uint32_t iaddr,
                         uint32_t vaddr) {
  if (!SceneEnabled()) {
    return;
  }
  // Perspective SimpleDraw draws only (see OnDrawDone): orthographic ones
  // are UI.
  const int sd = BoundSimpleDrawKind(base);
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if ((sd == 1 || sd == 2) && bank != 0 && !BankIsOrtho(base, bank) &&
      REXCVAR_GET(skate3_native_render_scene_simpledraw)) {
    CaptureSimpleDraw(base, sd, prim, num_verts, stride, vaddr, index_count, iaddr);
  }
}

void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7) {
  DrawSectScope draw_sect_scope;
  g_draw_seq.fetch_add(1, std::memory_order_relaxed);
  const uint32_t flags2d = Phase2dFlags();
  // SimpleDraw debug primitives through the inline path (see
  // SimpleDrawCap): the PERSPECTIVE ones (world debug draws such as the
  // render skeleton) are replayed by the scene pass and kept out of the 2D
  // overlay capture below, which would draw them with the HUD transform.
  // Orthographic SimpleDraw draws are UI (the debug menu, game HUD
  // elements) and stay on the 2D path untouched.
  bool simple_draw_3d = false;
  if (func == 2 && SceneEnabled() && REXCVAR_GET(skate3_native_render_scene_simpledraw)) {
    const int sd = BoundSimpleDrawKind(base);
    const uint32_t sd_bank = g_vs_bank.load(std::memory_order_relaxed);
    if ((sd == 1 || sd == 2) && sd_bank != 0 && !BankIsOrtho(base, sd_bank)) {
      CaptureSimpleDraw(base, sd, r4, r5, r6, r7, 0, 0);
      simple_draw_3d = true;
    }
  }
  // Last-draw provenance for the submit-exit capture (see g_last_draw_ibvb):
  // only an indexed 3D draw leaves a bank the palette/world capture may
  // trust, keyed by the buffers it bound.
  g_last_draw_ibvb.store(
      (func == 0 && flags2d == 0)
          ? ((uint64_t(g_cur_ib.load(std::memory_order_relaxed)) << 32) |
             g_cur_vb.load(std::memory_order_relaxed))
          : 0,
      std::memory_order_relaxed);
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return;
  }
  // The bank / PS-bank / pending-fixup reads below are raw loads; recover
  // read faults for the whole post-draw path (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // The VS bank is fixed for the whole post-draw path: classify it once.
  const bool bank_ortho = BankIsOrtho(base, bank);
  if (g_draw_sect_on) {
    DrawPassCensus(base, flags2d, bank_ortho, bank);
  }
  // Portrait pass (see PhotoCapture): its camera + the pieces it draws.
  if (g_photo_cap.active) {
    if (flags2d == 0 && func == 0 && !bank_ortho) {
      if (!g_photo_cap.cam_valid) {
        for (int k = 0; k < 4; ++k) {
          for (int c = 0; c < 4; ++c) {
            g_photo_cap.rows[k][c] = LoadGuestF32(base, bank + uint32_t(k * 4 + c) * 4);
          }
        }
        g_photo_cap.cam_valid = true;
      }
      const uint64_t key = g_last_draw_ibvb.load(std::memory_order_relaxed);
      if (key != 0 && g_photo_cap.draw_keys.size() < 256) {
        g_photo_cap.draw_keys.push_back(key);
      }
    } else if (flags2d != 0 && g_photo_cap.cam_valid) {
      ++g_photo_cap.overlay_draws;
    }
  }
  // DIAGNOSTIC: the first perspective 3D draw after DiagArmCameraLog logs
  // its VS constant rows c0..c7 (camera rows of the character layouts).
  if (flags2d == 0 && func == 0 && g_diag_cam_log.exchange(0, std::memory_order_relaxed)) {
    float r[8][4];
    for (int k = 0; k < 8; ++k) {
      for (int c = 0; c < 4; ++c) {
        r[k][c] = LoadGuestF32(base, bank + uint32_t(k * 4 + c) * 4);
      }
    }
    for (int k = 0; k < 8; ++k) {
      REXLOG_INFO("native-scene: DIAG cam f{} vs c{} = ({:.4f}, {:.4f}, {:.4f}, {:.4f})",
                  g_guest_frame, k, r[k][0], r[k][1], r[k][2], r[k][3]);
    }
  }
  // DIAGNOSTIC aux-perspective (portrait / create-a-skater snapshot) passes:
  // per-second summary + the resolves that follow them (see OnResolve).
  if (flags2d == 0 && func == 0 && BankIsAuxPerspective(base, bank)) {
    static uint64_t s_aux_frame = 0;
    static uint32_t s_aux_draws = 0;
    static int64_t s_aux_log_ms = 0;
    if (g_guest_frame != s_aux_frame) {
      s_aux_frame = g_guest_frame;
      s_aux_draws = 0;
    }
    ++s_aux_draws;
    g_aux_pass_frame.store(g_guest_frame, std::memory_order_relaxed);
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
    if (p2100 != 0 && now_ms - s_aux_log_ms > 1000) {
      s_aux_log_ms = now_ms;
      float n0 = 0.0f, n1 = 0.0f;
      for (int i = 0; i < 3; ++i) {
        const float a = LoadGuestF32(base, bank + (0 * 4 + i) * 4);
        const float b = LoadGuestF32(base, bank + (1 * 4 + i) * 4);
        n0 += a * a;
        n1 += b * b;
      }
      REXLOG_INFO(
          "native-scene: DIAG aux perspective pass frame={} draws_so_far={} vp={}x{} "
          "aspect={:.3f} ib={:08X}",
          g_guest_frame, s_aux_draws,
          uint32_t(std::fabs(LoadGuestF32(base, p2100 + 0xF * 4)) * 2.0f + 0.5f),
          uint32_t(std::fabs(LoadGuestF32(base, p2100 + 0x11 * 4)) * 2.0f + 0.5f),
          n0 > 0.0f ? std::sqrt(n1 / n0) : 0.0f, g_cur_ib.load(std::memory_order_relaxed));
    }
  }
  DrawSectMark(1);
  // DIAGNOSTIC pass census (removing the emulated GPU: which passes compose
  // what). Each distinct (viewport, VS, PS, 2D bracket) logs once with its
  // bound textures and the colour target's pitch. Per draw: a mutex, a set
  // insert and the pass-ring record; compiled out of Release.
#ifndef SK83_RELEASE_BUILD
  if (REXCVAR_GET(skate3_native_render_scene_pass_census)) {
    const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
    const uint32_t dev = g_device.load(std::memory_order_relaxed);
    if (p2100 != 0 && dev != 0) {
      const uint32_t vw = uint32_t(std::fabs(LoadGuestF32(base, p2100 + 0xF * 4)) * 2.0f + 0.5f);
      const uint32_t vh = uint32_t(std::fabs(LoadGuestF32(base, p2100 + 0x11 * 4)) * 2.0f + 0.5f);
      if (vw > 0) {
        const auto leaf = [&](uint32_t obj) -> std::string {
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return "-";
          }
          char path[200] = {};
          if (!GuestTryCopy(path, base + ShaderPathAddr(base, obj), sizeof(path) - 1)) {
            return "?";
          }
          const char* l = std::strrchr(path, 92);
          return std::string(l != nullptr ? l + 1 : path);
        };
        const uint32_t so_a = g_cur_ps_obj.load(std::memory_order_relaxed);
        const uint32_t so_b = g_cur_vs_obj.load(std::memory_order_relaxed);
        {
          PassRecord pr;
          pr.frame = g_guest_frame;
          pr.func = uint8_t(func);
          pr.flags2d = uint16_t(flags2d);
          pr.vw = uint16_t(vw);
          pr.vh = uint16_t(vh);
          pr.shader_a = so_a;
          pr.shader_b = so_b;
          pr.tf[0] = REX_LOAD_U32(dev + 0x480 + 0 * 24 + 4) & 0x1FFFF000u;
          pr.tf[1] = REX_LOAD_U32(dev + 0x480 + 1 * 24 + 4) & 0x1FFFF000u;
          pr.tf[2] = REX_LOAD_U32(dev + 0x480 + 3 * 24 + 4) & 0x1FFFF000u;
          pr.ortho = bank_ortho;
          float n0 = 0.0f, n1 = 0.0f;
          for (int k = 0; k < 3; ++k) {
            const float a0 = LoadGuestF32(base, bank + (0 * 4 + k) * 4);
            const float b0 = LoadGuestF32(base, bank + (1 * 4 + k) * 4);
            n0 += a0 * a0;
            n1 += b0 * b0;
          }
          pr.aspect = n0 > 0.0f ? std::sqrt(n1 / n0) : 0.0f;
          if (const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed)) {
            pr.rs_blend = REX_LOAD_U32(p2200 + 1 * 4);
            pr.rs_color_ctl = REX_LOAD_U32(p2200 + 2 * 4);
          }
          pr.rs_mask = REX_LOAD_U32(p2100 + 4 * 4);
          RecordPass(pr);
        }
        const uint64_t key = (uint64_t(vw) << 48) ^ (uint64_t(vh) << 36) ^
                             (uint64_t(so_a) << 16) ^ uint64_t(so_b) ^ (uint64_t(func) << 60) ^
                             (uint64_t(flags2d) << 40);
        static std::mutex s_census_mutex;
        static std::unordered_set<uint64_t> s_census;
        bool fresh = false;
        {
          std::lock_guard<std::mutex> lock(s_census_mutex);
          fresh = s_census.size() < 2048 && s_census.insert(key).second;
        }
        if (fresh) {
          std::string tex;
          for (uint32_t s = 0; s < 4; ++s) {
            const uint32_t w1 = REX_LOAD_U32(dev + 0x480 + s * 24 + 4);
            const uint32_t w2 = REX_LOAD_U32(dev + 0x480 + s * 24 + 8);
            tex += fmt::format(" tf{}={:08X}/f{}/{}x{}", s, w1 & 0xFFFFF000u, w1 & 0x3Fu,
                               (w2 & 0x1FFFu) + 1, ((w2 >> 13) & 0x1FFFu) + 1);
          }
          REXLOG_INFO("native-scene: DIAG pass {}x{} func={} 2d={:X} shaders=[{} | {}]{}",
                      vw, vh, func, flags2d, leaf(so_a), leaf(so_b), tex);
        }
      }
    }
  }
#endif  // SK83_RELEASE_BUILD
  DrawSectMark(2);
  // Drain any console edits here: this runs on a GUEST thread, which is
  // what the game's Set_cVar*Ref needs (GuestToHostFunction requires a live
  // ThreadState). The console itself queues from the UI thread.
  // The game's render states for this perspective 3D draw (blend / alpha
  // test / color mask / depth), straight from the device register arrays.
  // Materials such as environment.transparent draw the same shader both as
  // alpha-tested opaque cards (vehicle wheels) and additively blended cards
  // (sky cloud sheets); the routing follows the draw, not the material.
  // draw_rs_now: these registers for THIS draw, kept for the per-island
  // capture in the tail (IslandState::rs) so it never re-reads them.
  DrawRegState draw_rs_now;
  bool draw_rs_valid = false;
  if (func == 0 && flags2d == 0) {
    const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed);
    const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
    const uint32_t ib_key = g_cur_ib.load(std::memory_order_relaxed);
    if (p2200 != 0 && p2100 != 0 && ib_key != 0 && !bank_ortho) {
      DrawRegState& st = draw_rs_now;
      draw_rs_valid = true;
      st.depth = REX_LOAD_U32(p2200 + 0 * 4);
      st.blend = REX_LOAD_U32(p2200 + 1 * 4);
      st.color_ctl = REX_LOAD_U32(p2200 + 2 * 4);
      st.color_mask = REX_LOAD_U32(p2100 + 4 * 4);
      st.stencil = REX_LOAD_U32(p2100 + 0xD * 4);
      st.mode_cntl = REX_LOAD_U32(p2200 + 5 * 4);
      std::lock_guard<std::mutex> lock(g_palette_mutex);
      if (g_frame_draw_rs.size() < 32768) {
        g_frame_draw_rs[ib_key] = st;
      }
    }
  }
  DrawSectMark(3);
  // Fog rows: grab c5/c6 from the first 3D draw whose bank c4 row matches
  // the last built scene's camera (main-pass layout; tolerant of one frame
  // of camera motion). See g_fog_rows. The same camera-keyed draws also
  // carry the frame's CSM shadow constants in the PIXEL bank (c0..c8,
  // pass-global on environment-family draws), captured here with an
  // independent done-flag: the first main-pass draw can be a character/tree
  // whose PS allocates differently (rejected by the sanity gate below).
  // Sky dome viewpos (see g_sky_height): every draw bound to sky_defaultVS.
  if (func == 0 && flags2d == 0 && SceneEnabled()) {
    const auto is_sky = [&](uint32_t obj) -> int {  // 0 unknown, 1 no, 2 yes
      if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
        return 0;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[120] = {};
      const uint32_t shader_path = ShaderPathAddr(base, obj);
      for (int k = 0; k < 119; ++k) {
        text[k] = char(REX_LOAD_U8(shader_path + k));
        if (text[k] == '\0') break;
      }
      const int result = text[0] == '\0' ? 0 : (std::strstr(text, "\\sky_") ? 2 : 1);
      if (result != 0 && cache.size() < 4096) {
        cache.emplace(obj, result);
      }
      return result;
    };
    int sky_kind = is_sky(g_cur_vs_obj.load(std::memory_order_relaxed));
    if (sky_kind == 0) {
      sky_kind = is_sky(g_cur_ps_obj.load(std::memory_order_relaxed));
    }
    if (sky_kind == 2) {
      const float sx = LoadGuestF32(base, bank + 16 * 4);
      const float sy = LoadGuestF32(base, bank + 17 * 4);
      const float sz = LoadGuestF32(base, bank + 18 * 4);
      const float ddx = sx - g_fog_cam[0];
      const float ddz = sz - g_fog_cam[2];
      if (std::isfinite(sy) && std::fabs(sy) < 100000.0f &&
          ddx * ddx + ddz * ddz < 400.0f &&
          (!g_sky_frame_any || sy > g_sky_frame_y)) {
        g_sky_frame_y = sy;
        g_sky_frame_any = true;
        // Same draw, PIXEL bank (sky_defaultPS layout): c0.xyz = light dir
        // (unit, toward the sun), c4.x = sun angular scale, c4.y = sky
        // pre-tone multiplier, c3.x = scene exposure. Taken from the
        // shader-identified sky draw: the old "c4.y 50..2000 above the
        // camera" signature never matched once the camera rose, the rows
        // stayed uncaptured and the dome kept the legacy fullbright path.
        const uint32_t sky_ps = g_ps_bank.load(std::memory_order_relaxed);
        if (sky_ps != 0) {
          float dir[3];
          for (int k = 0; k < 3; ++k) {
            dir[k] = LoadGuestF32(base, sky_ps + uint32_t(k) * 4);
          }
          const float n2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
          const float scale = LoadGuestF32(base, sky_ps + (4 * 4 + 0) * 4);
          const float mult = LoadGuestF32(base, sky_ps + (4 * 4 + 1) * 4);
          const float expo = LoadGuestF32(base, sky_ps + (3 * 4 + 0) * 4);
          if (n2 > 0.8f && n2 < 1.2f && scale > 1e-3f && scale < 100.0f &&
              mult > 1e-3f && mult < 100.0f && expo > 0.01f && expo < 100.0f) {
            g_sky_sun[0] = dir[0];
            g_sky_sun[1] = dir[1];
            g_sky_sun[2] = dir[2];
            g_sky_sun[3] = scale;
            g_sky_sun[4] = mult;
            g_sky_sun[5] = expo;
            g_sky_sun_have = true;
          }
        }
      }
    }
  }
  if ((!g_fog_frame_done || !g_shadow_frame_done || !g_sky_frame_done ||
       !g_tree_frame_done || !g_proxy_frame_done || !g_dynobj_frame_done ||
       !g_park_frame_done || !g_water_frame_done || !g_waterdef_frame_done ||
       !g_ocean_frame_done ||
       !g_oceanrefl_frame_done || !g_scroll_frame_done || !g_ad_frame_done ||
       !g_ts_frame_done) &&
      func == 0 && flags2d == 0 && SceneEnabled() &&
      (g_fog_cam[0] != 0.0f || g_fog_cam[1] != 0.0f || g_fog_cam[2] != 0.0f)) {
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    // The sky draw's bank: c4.xz == camera, c4.y = the fixed level sky
    // elevation (dy ~ +160). dy > 50 excludes normal draws (dy == 0) and
    // any reflection pass (y mirrored DOWN); the bound keeps out garbage.
    if (!g_sky_frame_done && dx * dx + dz * dz < 25.0f && dy > 50.0f && dy < 2000.0f) {
      // (Sky sun rows now come from the shader-identified sky draw above.)
      g_sky_frame_done = true;
    }
    if (dx * dx + dy * dy + dz * dz < 25.0f) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      // POSITIVE family check for the receiver-row capture, by shader debug
      // path: the value gates below cannot fully discriminate;
      // flowingwater_defaultPS keeps the ENTIRE baseenvironment receiver
      // layout (CSM rows, sun c6, camera c7, dim c8) but its material
      // multiplier c11.y is 0.2, not 1.0. When the canal was in view its
      // draw won the first-pass race and the whole world tone chain ran at
      // x0.2 linear, the "world goes dark at some camera rotations" bug
      // (native exactly 0.50x emulated, uniform). Only the environment
      // families that share the c10.x/c11.y layout are eligible.
      const auto env_receiver_ps = [&]() -> bool {
        const auto check = [&](uint32_t obj) -> int {  // 0 unknown, 1 no, 2 yes
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          char text[120] = {};
          const uint32_t shader_path = ShaderPathAddr(base, obj);
          for (int k = 0; k < 119; ++k) {
            text[k] = char(REX_LOAD_U8(shader_path + k));
            if (text[k] == '\0') break;
          }
          const bool hit = std::strstr(text, "\\baseenvironment") != nullptr ||
                           std::strstr(text, "\\defaultenvironment") != nullptr ||
                           std::strstr(text, "\\decalenvironment") != nullptr;
          // Empty/garbled paths stay unknown (0) and are not cached-in as
          // negatives forever.
          const int result = text[0] == '\0' ? 0 : (hit ? 2 : 1);
          if (result != 0 && cache.size() < 4096) {
            cache.emplace(obj, result);
          }
          return result;
        };
        const int a = check(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (a != 0) {
          return a == 2;
        }
        return check(g_cur_vs_obj.load(std::memory_order_relaxed)) == 2;
      };
      // Fog rows (VS c5 ramp / c6 color): POSITIVE family gate like the
      // receiver rows below, for the same reason: the camera-keyed c4 check
      // alone lets ANY main-pass-layout draw win the first-draw race, and
      // the dam spillway's water shaders keep the camera at c4 with a
      // water-teal where fog c6 lives, values that PASSED the range gate
      // and tinted every distance-fogged surface (the bank mist band, the
      // far dirt hill) saturated blue for exactly the one frame that
      // capture served (the approach-flicker blue flash; F7 scene-ring
      // proved composition/textures identical across the artifact frame).
      // Same failure class as the flowingwater tone hijack documented at
      // env_receiver_ps.
      if (!g_fog_frame_done && env_receiver_ps()) {
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        // Range gate (kept as a second line of defense): ramp scale is a
        // tiny per-meter slope, the exponent is a small power, the fog color
        // is a dim linear-space rgb and the transmittance scale small.
        const bool sane = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                          rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                          rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                          rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (sane) {
          std::memcpy(g_fog_rows, rows, sizeof(rows));
          g_fog_have = true;
          g_fog_frame_done = true;
        }
      } else if (!g_fog_frame_done) {
        // Confirmation probe for the blue-flash fix: a NON-env draw whose
        // rows would have passed the old value-only gate with a fog color
        // far from the current one is exactly the frame that used to flash
        // - each hit here is one prevented flash, naming the hijacker.
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        const bool would = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                           rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                           rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                           rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (would) {
          const float dr = rows[4] - g_fog_rows[4];
          const float dg = rows[5] - g_fog_rows[5];
          const float db = rows[6] - g_fog_rows[6];
          if (dr * dr + dg * dg + db * db > 0.01f) {
            // Rolling cap (the flat 32 burned at the load screen).
            static std::atomic<uint32_t> s_fog_rejects{0};
            static std::atomic<int64_t> s_fog_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t win = s_fog_win.load(std::memory_order_relaxed);
            if (now_s - win >= 5 &&
                s_fog_win.compare_exchange_strong(win, now_s)) {
              s_fog_rejects.store(0, std::memory_order_relaxed);
            }
            if (s_fog_rejects.fetch_add(1, std::memory_order_relaxed) < 8) {
              REXLOG_INFO(
                  "native-scene: fog capture REJECTED by family gate "
                  "(prevented flash): vs_obj={:08X} ps_obj={:08X} "
                  "color=({:.3f},{:.3f},{:.3f}) vs current ({:.3f},{:.3f},{:.3f})",
                  g_cur_vs_obj.load(std::memory_order_relaxed),
                  g_cur_ps_obj.load(std::memory_order_relaxed), rows[4], rows[5],
                  rows[6], g_fog_rows[4], g_fog_rows[5], g_fog_rows[6]);
            }
          }
        }
      }
      // water.default (water_defaultPS) rows. This build's canal/pool water
      // binds THIS shader, not flowingwateralpha, so the capture above never
      // fired and the native water path never ran at all -- water fell back
      // to the hand-tuned approximation (whose own comment notes it has no
      // cube map and substitutes the fog colour for the reflection).
      // Rows read from the ucode:
      //   mul r4, c24.zwxy, c25.xxxx / mul r3, c23, c26.xxxx
      //   mad r6, r4.zwxy, r0.xyxy, r3   -> uv = uv*(c24*c25.x) + c23*c26.x
      //   dp4 ... c9..c14                -> PCA normal decode (as the ocean)
      //   mul r4, r4, c22.zwzw           -> per-tap normal strengths
      // NOT gated by g_waterdef_frame_done: the rows are per MATERIAL and a
      // level can show several water surfaces at once, so every water draw
      // must refresh its own slot. Gating on the frame let one surface's
      // constants stand in for another's, which is what blew the normal up.
      if (ps_bank != 0) {
        const auto waterdef_ps = [&]() -> bool {
          // Memoised per shader object: this now runs on EVERY draw (the
          // rows are per material, so each water draw must refresh its own
          // slot), and a guest string read per draw is exactly the cost that
          // halved the framerate when ShaderPathAddr was called unhoisted.
          const auto check = [&](uint32_t obj) -> bool {
            struct Memo {
              uint32_t obj;
              bool is_waterdef;
              bool set;
            };
            static thread_local Memo memo[64];
            Memo& m = memo[(obj >> 4) & 63u];
            if (m.set && m.obj == obj) {
              return m.is_waterdef;
            }
            const bool r = [&]() -> bool {
            if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
              return false;
            }
            char text[200] = {};
            if (!GuestTryCopy(text, base + ShaderPathAddr(base, obj), 199)) {
              return false;
            }
            text[199] = '\0';
            // Two shaders share this constant layout and this port: the
            // canal/pool water (water_defaultPS) and the ALPHA variant
            // (wateralpha_defaultPS), which some maps use instead -- their
            // disassemblies differ only in register allocation and a
            // permuted component layout. Matching only the first left every
            // water surface in those maps with no captured rows, so
            // waterdef_valid stayed false and they fell back to the legacy
            // approximation and rendered dark.
            // (The "\water_default" written here was meant to anchor on the
            // path separator, but "\w" is an invalid escape that folds to
            // 'w', so this has always been a bare substring test -- which is
            // why it also matches the flowing* variants.)
            return std::strstr(text, "water_default") != nullptr ||
                   std::strstr(text, "wateralpha_default") != nullptr;
            }();
            m = {obj, r, true};
            return r;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (waterdef_ps()) {
          const auto wrow = [&](uint32_t r, uint32_t c) {
            return LoadGuestF32(base, ps_bank + (r * 4 + c) * 4);
          };
          float rows[56];
          for (uint32_t c = 0; c < 4; ++c) {
            rows[0 + c] = wrow(8u, c);
            for (uint32_t k = 0; k < 6; ++k) {
              rows[4 + k * 4 + c] = wrow(9u + k, c);
            }
            // Retail water[alpha]_defaultPS: m_params[0..3] = c18..c21,
            // g_fAnimationTime = c22 (the dev build has them at c22..c26
            // behind a longer g_envattributes block).
            rows[28 + c] = wrow(18u, c);  // wd_str
            rows[32 + c] = wrow(19u, c);  // wd_speed
            rows[36 + c] = wrow(20u, c);  // wd_scale
            rows[40 + c] = wrow(21u, c);  // wd_misc
            rows[44 + c] = wrow(22u, c);  // wd_time
          }
          // wd_refl.w / wd_fres.yzw: the dev build reads the reflection
          // strength and gate from g_envattributes rows retail does not
          // have; retail bakes them (tools/sk83_port/disasm_retail
          // ps_water_defaultPS 102/115-118): strength 1.5 (c255.z), gate
          // 0.3 * sat(4 * mask - 2.6) = 0.3 * sat(4 * (mask - 0.65)).
          rows[48] = rows[49] = rows[50] = 0.0f;
          rows[51] = 1.5f;
          rows[52] = 0.0f;
          rows[53] = 0.3f;
          rows[54] = 4.0f;
          rows[55] = 0.65f;
          // Range gate, same spirit as the flowingwateralpha one: scales and
          // strengths O(1)-ish, spec power a real exponent, time sane.
          bool sane = rows[40] > 0.0f && rows[40] <= 64.0f &&      // c25.x
                      rows[42] >= 1.0f && rows[42] <= 4096.0f &&   // c25.z pow
                      rows[44] >= 0.0f && rows[44] < 1e7f;         // c26.x time
          for (int k = 0; k < 4 && sane; ++k) {
            sane = std::fabs(rows[32 + k]) <= 64.0f &&
                   std::fabs(rows[36 + k]) <= 256.0f &&
                   std::fabs(rows[28 + k]) <= 16.0f;
          }
          if (sane) {
            // Same draw: record the samplers the game has bound for it.
            g_waterdef_tf3 = g_tex_slot[3];
            g_waterdef_tf5 = g_tex_slot[5];
            g_waterdef_tf6 = g_tex_slot[6];
            g_waterdef_tf7 = g_tex_slot[7];
            // Per-material set, keyed by this draw's tf6 normal map.
            {
              const uint32_t key = g_tex_slot[6];
              int slot = -1;
              for (uint32_t k = 0; k < kWaterdefSets; ++k) {
                if (g_waterdef_set_have[k] && g_waterdef_set_key[k] == key) {
                  slot = int(k);
                  break;
                }
              }
              if (slot < 0) {
                for (uint32_t k = 0; k < kWaterdefSets; ++k) {
                  if (!g_waterdef_set_have[k]) {
                    slot = int(k);
                    break;
                  }
                }
              }
              if (slot < 0) {
                // Every slot taken by another material: evict round-robin
                // rather than always clobbering slot 0, which made two
                // on-screen surfaces fight over it every frame.
                slot = int(g_waterdef_set_next % kWaterdefSets);
                ++g_waterdef_set_next;
              }
              std::memcpy(g_waterdef_set_rows[slot], rows, sizeof(rows));
              g_waterdef_set_key[slot] = key;
              g_waterdef_set_have[slot] = true;
            }
            const bool first = !g_waterdef_have;
            std::memcpy(g_waterdef_rows, rows, sizeof(rows));
            g_waterdef_have = true;
            // Deliberately NOT latching g_waterdef_frame_done: every water
            // draw must refresh its own material slot.
            if (first) {
              // Dump every captured row once: the c20/c21/c22 roles were
              // inferred from usage, and the result blew out to white.
              static const char* kNames[14] = {
                  "c8 mean", "c9",  "c10", "c11", "c12", "c13", "c14",
                  "c18 str", "c19 speed", "c20 scale", "c21 misc",
                  "c22 time", "refl lit", "fres lit"};
              for (int r = 0; r < 14; ++r) {
                REXLOG_INFO(
                    "native-scene: WDROW {:>9} = ({:.4g}, {:.4g}, {:.4g}, {:.4g})",
                    kNames[r], rows[r * 4 + 0], rows[r * 4 + 1],
                    rows[r * 4 + 2], rows[r * 4 + 3]);
              }
              REXLOG_INFO(
                  "native-scene: water.default params captured: "
                  "uvscale=({:.3g},{:.3g},{:.3g},{:.3g}) "
                  "speed=({:.3g},{:.3g},{:.3g},{:.3g}) mult={:.3g} "
                  "pow={:.3g} time={:.3g} refl={:.3g}",
                  rows[36], rows[37], rows[38], rows[39], rows[32], rows[33],
                  rows[34], rows[35], rows[40], rows[42], rows[44], rows[51]);
            }
          }
        }
      }
      // flowingwateralpha m_params + animation time (PS c11..c14, c15.x),
      // POSITIVE debug-path gate like the receiver rows: the water bank
      // keeps the whole baseenvironment receiver layout, so only the name
      // discriminates. One material's rows serve the frame; every
      // flowingwateralpha draw in a capture carried identical m_params, and
      // the animation time is pass-global anyway.
      if (!g_water_frame_done && ps_bank != 0) {
        const auto water_ps = [&]() -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            return (ShaderNameFlags(base, obj) & kSnFlowWater) != 0;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (water_ps()) {
          // Rows measured from THIS build's own ucode
          // (data/render/shaders/final/flowingwateralpha_defaultPS.fpo):
          //   mul r6, c16, c19.xxxx   scroll speeds = c16, TIME = c19.x
          //   mad r6, r0.xyxy, c17, r6  uv scales   = c17
          //   dp3 r1.z, c11.zxyy, ...   normal scale= c11  (as retail)
          //   mulsc_sat r0.x, c14.z, .. thr/pow/floor= c14 (as retail)
          // Retail keeps speeds at c12, scales at c13 and the time at c15.x,
          // so the scroll and animation rows were read from the wrong
          // registers here -- the water animated off the wrong constants.
          float rows[32];
          const bool sane = ReadFlowWaterRows(base, ps_bank, rows);
          if (sane) {
            g_water_tf4 = g_tex_slot[4];  // the surface colour map
            const bool first = !g_water_have;
            std::memcpy(g_water_rows, rows, sizeof(rows));
            g_water_have = true;
            g_water_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: water m_params captured: p0=({:.3g},{:.3g},"
                  "{:.3g},{:.3g}) speed=({:.3g},{:.3g},{:.3g},{:.3g}) "
                  "scale=({:.3g},{:.3g},{:.3g},{:.3g}) thr={:.3g} pow={:.3g} "
                  "floor={:.3g} t={:.2f} | c18=({:.3g},{:.3g},{:.3g}) "
                  "c10.w={:.3g} c12=({:.3g},{:.3g},{:.3g}) "
                  "c15=({:.3g},{:.3g},{:.3g},{:.3g})",
                  rows[0], rows[1], rows[2], rows[3], rows[4], rows[5],
                  rows[6], rows[7], rows[8], rows[9], rows[10], rows[11],
                  rows[13], rows[14], rows[15], rows[16], rows[19],
                  rows[17], rows[18], rows[20], rows[21], rows[22],
                  rows[23], rows[24], rows[25], rows[26], rows[27]);
            }
          } else {
            static std::atomic<int> s_water_rejects{0};
            if (s_water_rejects.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO(
                  "native-scene: water m_params REJECTED by range gate: "
                  "mult={:.3g} thr={:.3g} pow={:.3g} floor={:.3g} t={:.3g}",
                  rows[25], rows[19], rows[17], rows[18], rows[16]);
            }
          }
        }
      }
      // scrollincandescent.fx (the emissive time-scrolled LED chyron):
      // g_fAnimationTime from the VERTEX bank (c9.x) and the material
      // multiplier m_params[0].y from the PIXEL bank (c3.y). Same POSITIVE
      // debug-path gate as the water rows; the bank values alone cannot
      // discriminate this family.
      if (!g_scroll_frame_done && ps_bank != 0) {
        const auto scroll_ps = [&]() -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            return (ShaderNameFlags(base, obj) & kSnScroll) != 0;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (scroll_ps()) {
          const float t = LoadGuestF32(base, bank + 36 * 4);        // VS c9.x
          const float mult = LoadGuestF32(base, ps_bank + 13 * 4);  // PS c3.y
          // Range gate: the animation clock is a non-negative seconds
          // counter, the multiplier a small positive scale.
          if (t >= 0.0f && t < 1e7f && mult > 0.0f && mult <= 8.0f) {
            const bool first = !g_scroll_have;
            g_scroll_rows[0] = t;
            g_scroll_rows[1] = mult;
            g_scroll_have = true;
            g_scroll_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: scroll rows captured: t={:.2f} mult={:.3g}",
                  t, mult);
            }
          }
        }
      }
      // ocean_defaultPS material rows (PCA mean/weights at c2..c8,
      // m_params[0..2] at c12..c14) and the oceanreflection sheet row (c3).
      // Same POSITIVE debug-path gating as the water rows.
      if ((!g_ocean_frame_done || !g_oceanrefl_frame_done || !g_ad_frame_done ||
           !g_ts_frame_done) &&
          ps_bank != 0) {
        const auto ps_name_has = [&](uint32_t bit) -> bool {
          const auto check = [&](uint32_t obj) -> bool {
            return (ShaderNameFlags(base, obj) & bit) != 0;
          };
          return check(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
                 check(g_cur_vs_obj.load(std::memory_order_relaxed));
        };
        if (!g_ocean_frame_done && ps_name_has(kSnOcean)) {
          float c[64];
          for (int i = 0; i < 64; ++i) {
            c[i] = LoadGuestF32(base, ps_bank + (8 + i) * 4);  // c2..c17
          }
          const float* m0 = c + (12 - 2) * 4;  // c12
          const float* m1 = c + (13 - 2) * 4;  // c13
          const float* m2 = c + (14 - 2) * 4;  // c14
          bool sane = m1[1] > 0.0f && m1[1] <= 8.0f &&   // multiplier
                      m1[2] >= 0.0f && m1[2] <= 8.0f &&  // tonedown scale
                      m1[3] > 0.0f && m1[3] <= 4096.0f &&  // uv scale
                      m2[0] > 0.01f && m2[0] <= 1000.0f &&  // spread u
                      m2[1] > 0.01f && m2[1] <= 1000.0f &&  // spread v
                      m2[2] > 0.001f && m2[2] <= 100.0f;    // roughness
          for (int i = 0; i < 4 && sane; ++i) {
            sane = c[i] >= -2.0f && c[i] <= 2.0f;  // PCA mean row
          }
          if (sane) {
            float* o = g_ocean_rows;
            // mean pre-swizzled for (R,G,B): PS c2.x, c2.z, c2.y
            o[0] = c[0];
            o[1] = c[2];
            o[2] = c[1];
            o[3] = 0.0f;
            // weight rows in model order: c3, c4, c7, c8, c5, c6
            const int worder[6] = {3, 4, 7, 8, 5, 6};
            for (int w = 0; w < 6; ++w) {
              std::memcpy(o + 4 + w * 4, c + (worder[w] - 2) * 4,
                          4 * sizeof(float));
            }
            std::memcpy(o + 28, m0, 4 * sizeof(float));
            std::memcpy(o + 32, m1, 4 * sizeof(float));
            std::memcpy(o + 36, m2, 4 * sizeof(float));
            g_ocean_tf7 = g_tex_slot[7];  // the cube the shader samples

            const bool first = !g_ocean_have;
            g_ocean_have = true;
            g_ocean_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: ocean rows captured: mult={:.3g} "
                  "tonedown={:.3g} uvscale={:.3g} ward=({:.3g},{:.3g},{:.3g})",
                  m1[1], m1[2], m1[3], m2[0], m2[1], m2[2]);
            }
          }
        }
        if (!g_ts_frame_done && ps_name_has(kSnTransEnv)) {
          // RETAIL transparentenvironment_defaultPS (tools/sk83_port/
          // disasm_retail): m_params[0] = c10, so the material multiplier is
          // c10.y (42) and the exposure c9.x (45/47). The spec light, power
          // and tint the dev compile reads from its longer g_envattributes
          // (c10.w/c11/c12) are literals here: world light (-0.14, 0.5, 0.9)
          // (c253, 30-32), power 290 * ecc + 10 (c251.w / c255.w, 30/36),
          // tint (2.1, 1.8, 1.5) (c252, 40) -- stored /3 since the shader
          // scales the tint by the dev compile's literal 3.
          const auto rowf = [&](uint32_t r, uint32_t c) {
            return LoadGuestF32(base, ps_bank + (r * 4 + c) * 4);
          };
          const float lx = -0.14f, ly = 0.5f, lz = 0.9f;
          const float llen = std::sqrt(lx * lx + ly * ly + lz * lz);
          const float mult = rowf(10, 1), expo = rowf(9, 0);
          const float tr = 2.1f / 3.0f, tg = 1.8f / 3.0f, tb = 1.5f / 3.0f;
          const float p_scale = 290.0f, p_bias = 10.0f;
          const bool sane = llen > 0.5f && llen < 1.5f && mult > 0.0f && mult < 16.0f &&
                            expo > 0.01f && expo < 100.0f && tr >= 0.0f && tr < 16.0f &&
                            tg >= 0.0f && tg < 16.0f && tb >= 0.0f && tb < 16.0f &&
                            std::fabs(p_scale) < 5000.0f && std::fabs(p_bias) < 5000.0f;
          if (sane) {
            g_ts_rows[0] = lx; g_ts_rows[1] = ly; g_ts_rows[2] = lz;
            g_ts_rows[3] = mult;
            g_ts_rows[4] = tr; g_ts_rows[5] = tg; g_ts_rows[6] = tb;
            g_ts_rows[7] = expo;
            g_ts_rows[8] = p_scale; g_ts_rows[9] = p_bias;
            const bool first = !g_ts_have;
            g_ts_have = true;
            g_ts_frame_done = true;
            if (first) {
              REXLOG_INFO("native-scene: transparentenvironment rows captured: "
                          "light=({:.3g},{:.3g},{:.3g}) mult={:.3g} tint=({:.3g},{:.3g},"
                          "{:.3g}) exposure={:.3g} power={:.3g}*ecc+{:.3g}",
                          lx, ly, lz, mult, tr, tg, tb, expo, p_scale, p_bias);
            }
          }
        }
        if (!g_ad_frame_done && ps_name_has(kSnAd)) {
          // advertisement_defaultPS rows, straight off its ucode:
          //   32: max r0.xyz, r0.xyzz, c6.zzzz     lightmap floor
          //   34: mul r0.xyz, r0.xyzz, c10.yyyy    material multiplier
          //   35: mad_sat r0.xyz, -r0.xyzz, c9.xxxx, c254.wwww   exposure
          const float floor_z = LoadGuestF32(base, ps_bank + (6 * 4 + 2) * 4);
          const float mult = LoadGuestF32(base, ps_bank + (10 * 4 + 1) * 4);
          const float expo = LoadGuestF32(base, ps_bank + (9 * 4 + 0) * 4);
          if (floor_z >= 0.0f && floor_z <= 1.0f && mult > 0.0f && mult < 16.0f &&
              expo > 0.01f && expo < 100.0f) {
            g_ad_rows[0] = floor_z;
            g_ad_rows[1] = mult;
            g_ad_rows[2] = expo;
            const bool first = !g_ad_have;
            g_ad_have = true;
            g_ad_frame_done = true;
            if (first) {
              REXLOG_INFO("native-scene: advertisement rows captured: floor={:.3g} "
                          "mult={:.3g} exposure={:.3g}",
                          floor_z, mult, expo);
            }
          }
        }
        if (!g_oceanrefl_frame_done && ps_name_has(kSnOceanRefl)) {
          // As the game's shaders consume them (oceanreflection_defaultPS /
          // _defaultVS ucode): x = PS c3.x, the colour multiplier (0 is a
          // real value: the submerged canal-wall meshes draw with 0 and are
          // invisible); y/z = VS c7.y/c7.z, the alpha fade bounds
          // (o0.z = sat((y - c7.z) / (c7.y - c7.z))). Reading the bounds
          // from PS c3.yz and rejecting a 0 multiplier kept a stale row from
          // another sheet, which drew those walls as a dark band fighting
          // the canal wall they coincide with (F11: the edge flicker).
          const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
          float row[4] = {LoadGuestF32(base, ps_bank + 12 * 4), 0.0f, 0.0f, 0.0f};
          if (vs_bank != 0) {
            row[1] = LoadGuestF32(base, vs_bank + (7 * 4 + 1) * 4);  // c7.y
            row[2] = LoadGuestF32(base, vs_bank + (7 * 4 + 2) * 4);  // c7.z
          }
          if (vs_bank != 0 && std::isfinite(row[0]) && row[0] >= 0.0f && row[0] <= 8.0f &&
              std::isfinite(row[1]) && std::isfinite(row[2]) &&
              std::fabs(row[1] - row[2]) > 1e-4f) {
            std::memcpy(g_oceanrefl_rows, row, sizeof(row));
            const bool first = !g_oceanrefl_have;
            g_oceanrefl_have = true;
            g_oceanrefl_frame_done = true;
            if (first) {
              REXLOG_INFO(
                  "native-scene: oceanreflection row captured: mult={:.3g} "
                  "fade=({:.3g},{:.3g})",
                  row[0], row[1], row[2]);
            }
          }
        }
      }
      // Not gated on the shadows cvar: the captured rows also carry the
      // scene exposure / material multiplier / sun direction consumed by the
      // exact world shading (rows 40/45/24..26); shading must not die when
      // dynamic shadows are toggled off.
      if (!g_shadow_frame_done && ps_bank != 0 && env_receiver_ps()) {
        float rows[48];
        for (int i = 0; i < 48; ++i) {
          rows[i] = LoadGuestF32(base, ps_bank + i * 4);
        }
        // skate.Park worlds draw every environment surface through the
        // `_skatepark` permutations, whose bank keeps the CSM rows c0..c4
        // but puts the exponential-shadow rows at c5..c8 and the rest
        // higher (baseenvironment_skateparkPS CTAB: g_CSMBlurBias c9, sun
        // c13, camera c14, g_envattributes c15..17 -> exposure c17.x,
        // m_params c18). Read as the gameplay layout, the sun-axis check on
        // c6 failed every park frame: no cascade rows, so the native shadow
        // atlas never ran in a park and the skater cast no shadow. Normalise
        // into the gameplay slots (the shadow colour is the retail literal;
        // see sh_color).
        const bool park_bank = BoundSkateparkWorldPs(base);
        if (park_bank) {
          const auto R = [&](uint32_t reg, uint32_t comp) {
            return LoadGuestF32(base, ps_bank + (reg * 4 + comp) * 4);
          };
          for (uint32_t c = 0; c < 4; ++c) {
            rows[20 + c] = R(9, c);   // c5  <- g_CSMBlurBias
            rows[24 + c] = R(13, c);  // c6  <- sun
            rows[28 + c] = R(14, c);  // c7  <- camera
            rows[44 + c] = R(18, c);  // c11 <- m_params (multiplier .y)
          }
          rows[32] = 0.05f;
          rows[33] = 0.09f;
          rows[34] = 0.13f;
          rows[40] = R(17, 0);  // c10.x <- exposure
        }
        // Shadow-colour register. Retail's environment receiver keeps it at
        // c8; on this build c8 holds (0.01, 0.0085, 1, 0.1) -- blue pinned at
        // 1.0, not a colour -- while c9 carries the dim triple the shading
        // expects. The comment below already notes this register is
        // family-dependent (advertisement keeps it at c7), so resolve it by
        // content: the first candidate whose rgb all sit in [0, 0.4]. The
        // winner is normalised into the c8 slot so every downstream consumer
        // (all of which index c8) is unchanged.
        {
          const auto dim_triple = [&rows](uint32_t b) {
            return rows[b] >= 0.0f && rows[b] <= 0.4f && rows[b + 1] >= 0.0f &&
                   rows[b + 1] <= 0.4f && rows[b + 2] >= 0.0f && rows[b + 2] <= 0.4f;
          };
          if (!dim_triple(32) && dim_triple(36)) {
            rows[32] = rows[36];
            rows[33] = rows[37];
            rows[34] = rows[38];
          }
        }
        // Receiver-layout sanity:
        // c0/c3 are the light-space X/Y rows with equal magnitude (the
        // cascade-0 extent), c1/c2 the square cascade scale+offsets with
        // scale2 < scale1 < 1, c4 the depth row (a pure height ramp: x/z
        // components tiny), c8 a dim shadow color in [0,1]. Environment
        // family only; other families fail these checks.
        const float mx2 = rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2];
        const float my2 = rows[12] * rows[12] + rows[13] * rows[13] + rows[14] * rows[14];
        const float s1 = rows[4], s2 = rows[8];
        // Depth row c4. The world sun's is a height ramp ((0, -0.167, 0) in
        // every outdoor capture), and gameplay keeps that strict test
        // (interior venues such as the park-editor warehouses must keep
        // failing it). The front-end loose worlds' light (the CAS room behind
        // the editor and the character viewer) projects along z ((0, 0, 1, 0),
        // measured in capture 1790709166): requiring a height ramp rejected
        // every loose-world bank, and those scenes ran on the gameplay
        // world's stale rows. While a loose world is loaded any
        // non-degenerate depth row passes; the family stays pinned by the
        // shader path and the sun-axis / camera / exposure checks below.
        const bool height_ramp = std::fabs(rows[16]) < 0.02f && std::fabs(rows[18]) < 0.02f &&
                                 std::fabs(rows[17]) > 0.005f && std::fabs(rows[17]) < 1.0f;
        const auto loose_world_depth = [&]() {
          const float mz2 = rows[16] * rows[16] + rows[17] * rows[17] + rows[18] * rows[18];
          uint32_t world = 0, lw_type = 0;
          return mz2 > 0.005f * 0.005f && mz2 < 4.0f &&
                 GuestTryLoadU32(base, 0x83083C38, &world) && world != 0 &&
                 kLooseWorldTypeOffset != 0 &&
                 GuestTryLoadU32(base, world + kLooseWorldTypeOffset, &lw_type) && lw_type != 0;
        };
        // skate.Park: the park's sun is not guaranteed to be the outdoor
        // height ramp; with the bank recognised by its shader path, any
        // non-degenerate depth row passes (the sun-axis / camera / exposure
        // checks below still pin it).
        const auto park_depth = [&]() {
          const float mz2 = rows[16] * rows[16] + rows[17] * rows[17] + rows[18] * rows[18];
          return park_bank && mz2 > 0.005f * 0.005f && mz2 < 4.0f;
        };
        bool sane =
            mx2 > 0.01f && mx2 < 4.0f && std::fabs(mx2 - my2) < 0.05f * mx2 &&
            s1 > 0.0f && s1 < 1.0f && std::fabs(rows[5] - s1) < 1e-4f &&
            s2 > 0.0f && s2 < s1 && std::fabs(rows[9] - s2) < 1e-4f &&
            (height_ramp || loose_world_depth() || park_depth()) &&
            rows[32] >= 0.0f && rows[32] <= 1.0f && rows[33] >= 0.0f &&
            rows[33] <= 1.0f && rows[34] >= 0.0f && rows[34] <= 1.0f;
        // c5..c8 are NOT pass-global like c0..c4: only the baseenvironment /
        // defaultenvironment-style bank keeps (bias, ..., sun dir, camera,
        // shadow color) there. Verified in capture: the
        // dynamicobject/livingworld/cacstamp banks pass the geometric checks
        // above with garbage in those rows (c8 ~ (0.0025,0.005,0.012) -> a
        // near-black shadow, c5.x = 0.0245 -> a 29 cm receiver height bias
        // that culled the feet/board shadow); advertisement keeps the color
        // at c7 (c8 = 0 -> pure black); wateralpha has c8 ~ (0.5,0.99,0.5)
        // (-> no shadow at all). Discriminate on the family-specific rows:
        // c6 = unit sun direction with normalize(cross(c0,c3)) == -c6 (the
        // oblique projection axis), c7 = the camera position, c8 dim.
        if (sane) {
          const float cxx = rows[1] * rows[14] - rows[2] * rows[13];
          const float cxy = rows[2] * rows[12] - rows[0] * rows[14];
          const float cxz = rows[0] * rows[13] - rows[1] * rows[12];
          const float cn = std::sqrt(cxx * cxx + cxy * cxy + cxz * cxz);
          const float sn = std::sqrt(rows[24] * rows[24] + rows[25] * rows[25] +
                                     rows[26] * rows[26]);
          const float align =
              cn > 1e-6f && sn > 1e-6f
                  ? (cxx * rows[24] + cxy * rows[25] + cxz * rows[26]) / (cn * sn)
                  : 0.0f;
          const float dcx = rows[28] - g_fog_cam[0];
          const float dcy = rows[29] - g_fog_cam[1];
          const float dcz = rows[30] - g_fog_cam[2];
          sane = align < -0.9f && sn > 0.9f && sn < 1.1f &&
                 dcx * dcx + dcy * dcy + dcz * dcz < 25.0f && rows[32] <= 0.4f &&
                 rows[33] <= 0.4f && rows[34] <= 0.4f &&
                 // c10.x = scene exposure, c11.y = material multiplier,
                 // consumed by the exact world tone chain.
                 rows[40] > 0.1f && rows[40] < 16.0f && rows[45] > 0.0f &&
                 rows[45] < 16.0f;
        }
        if (sane) {
          // Scene-exposure transition log (c10.x): the game's auto-exposure
          // holds this at the per-zone maximum (2.5 in daytime zones) while
          // luminance readbacks are unavailable, enforced by the
          // skate3_autoexposure_pin hook. Transitions are rare and mark
          // either a zone change or an adaptation leak, so log them.
          if (g_shadow_have &&
              std::fabs(rows[40] - g_shadow_rows[40]) > 0.02f * rows[40]) {
            REXLOG_INFO(
                "native-scene: world scene exposure changed {:.3f} -> {:.3f}",
                g_shadow_rows[40], rows[40]);
          }
          std::memcpy(g_shadow_rows, rows, sizeof(rows));
          g_shadow_have = true;
          g_shadow_frame_done = true;
          g_shadow_rows_frame = g_guest_frame;
        }
      }
      // tree / proxyworld frame rows (see FrameScene::family_rows): their PS
      // banks keep family-global lighting constants in different registers
      // than the environment layout. Identified by the shader debug path
      // (cached, like the blur trigger); the camera-keyed VS c4 gate above
      // already filtered to main-pass draws.
      if ((!g_tree_frame_done || !g_proxy_frame_done ||
           !g_dynobj_frame_done || !g_park_frame_done) &&
          ps_bank != 0) {
        const auto world_family = [&](uint32_t obj) -> int {
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          // 200 chars: this build's debug paths are
          // "D:\P4\Code\sk83\main\build\sk83\configure\render\shaders\final\
          // object\xbox2\<name>.updb" (77-char prefix). The old 95-char copy
          // cut "dynamicobject_defaultPS" / "alphatestdynamicobject_defaultPS"
          // off mid-name, so the family never matched, the dynobj rows were
          // never captured (dynobj[valid=false] every frame) and every prop
          // took the legacy shading (sunlit benches/hurdles rendered dark).
          char text[200] = {};
          const uint32_t shader_path = ShaderPathAddr(base, obj);
          for (int k = 0; k < 199; ++k) {
            text[k] = char(REX_LOAD_U8(shader_path + k));
            if (text[k] == '\0') break;
          }
          int fam = 0;
          if (std::strstr(text, "\\tree_defaultPS") != nullptr ||
              std::strstr(text, "\\treeanimate_defaultPS") != nullptr) {
            fam = 1;
          } else if (std::strstr(text, "\\proxyworld_defaultPS") != nullptr) {
            fam = 2;
          } else if (std::strstr(text, "\\dynamicobject_defaultPS") != nullptr ||
                     std::strstr(text, "\\alphatestdynamicobject_defaultPS") !=
                         nullptr) {
            fam = 3;
          } else if (kRetailParkPortDone && std::strstr(text, "\\environmentparkdiffuse_defaultPS") !=
                     nullptr) {
            // 4/5/6 mirror DrawItem::dynobj for the park variants; each
            // has its own bank layout (see the fam >= 4 capture below).
            fam = 4;
          } else if (kRetailParkPortDone && std::strstr(
                         text, "\\environmentparkalphatest_defaultPS") != nullptr) {
            fam = 5;
          } else if (kRetailParkPortDone && std::strstr(text, "\\environmentparkdecal_defaultPS") !=
                     nullptr) {
            fam = 6;
          }
          if (cache.size() < 4096) {
            cache.emplace(obj, fam);
          }
          return fam;
        };
        int fam = world_family(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (fam == 0) {
          fam = world_family(g_cur_vs_obj.load(std::memory_order_relaxed));
        }
        if (fam == 3 && !g_dynobj_frame_done) {
          // Rows from this build's own ucode (F10 capture banks verified):
          //   dynamicobject_defaultPS: sun c9, exposure c13.x (105-107),
          //     material multiplier c16.y (102), ambient c17.xyz (97),
          //     bounce scale c17.w (94);
          //   alphatestdynamicobject_defaultPS: the same but multiplier
          //     c17.y (line 92-93 block), ambient c18.xyz (92), bounce c18.w
          //     (c16 is its fixed world spec light (-0.14, 0.5, 0.9)).
          // Both keep c14 = GetTangentLight (0.58, 0.62, 0.39) + spec power
          // 290 and c15 = spec tint (0.7, 0.6, 0.5) + base exponent 10; the
          // old c14.y / c15.xyz / c15.w reads served 0.62 as the multiplier
          // and the spec tint as ambient: props rendered "in shadow" in sun.
          // Static world-shadow floor c8.w in both.
          // Retail (tools/sk83_port/disasm_retail): BOTH dynamicobject and
          // alphatestdynamicobject read multiplier c14.y, ambient c15.xyz and
          // bounce c15.w (m_params[0..1]); the dev build's longer
          // g_envattributes push them to c16/c17 (alphatest c17/c18).
          constexpr uint32_t kMultRow = 14u, kAmbRow = 15u;
          float sun[3];
          float n2 = 0.0f;
          for (int k = 0; k < 3; ++k) {
            sun[k] = LoadGuestF32(base, ps_bank + (9 * 4 + k) * 4);
            n2 += sun[k] * sun[k];
          }
          const float expo = LoadGuestF32(base, ps_bank + (13 * 4 + 0) * 4);
          const float mult = LoadGuestF32(base, ps_bank + (kMultRow * 4 + 1) * 4);
          if (n2 > 0.9f && n2 < 1.1f && expo > 0.1f && expo < 16.0f &&
              mult > 0.0f && mult < 16.0f) {
            g_dynobj_rows[0] = sun[0];
            g_dynobj_rows[1] = sun[1];
            g_dynobj_rows[2] = sun[2];
            g_dynobj_rows[3] = expo;
            for (int k = 0; k < 3; ++k) {
              g_dynobj_rows[4 + k] =
                  LoadGuestF32(base, ps_bank + (kAmbRow * 4 + uint32_t(k)) * 4);
            }
            g_dynobj_rows[7] = LoadGuestF32(base, ps_bank + (kAmbRow * 4 + 3) * 4);
            g_dynobj_rows[8] = mult;
            g_dynobj_rows[9] = LoadGuestF32(base, ps_bank + (8 * 4 + 3) * 4);
            // Per-cascade CSM receive biases c8.x/y/z (dynamicobject_defaultPS
            // 38/41/44: reference = depth - c8[cascade], finest = x). Kept
            // only when all three are small positive depths.
            {
              float b[3];
              bool ok = true;
              // Retail dynamicobject / alphatestdynamicobject read only c8.w;
              // the per-cascade biases are literals (c248.w = 0.007 finest,
              // c247.y = 0.015 mid/outer), which is exactly the shader's
              // uncaptured fallback -- so serve that fallback.
              ok = false;
              for (int k = 0; k < 3; ++k) {
                b[k] = 0.0f;
              }
              for (int k = 0; k < 3; ++k) {
                g_dynobj_rows[10 + k] = ok ? b[k] : 0.0f;
              }
            }
            // Static world-shadow transform (c5 = u row, c6 = v row,
            // c7 = ray depth row): consumed by the native world-shadow
            // re-render + the PS world term. Region-stable rows; a change
            // re-primes the native map (see RenderShadowAtlas).
            float ws[12];
            bool ws_ok = true;
            float uvmag = 0.0f;
            for (int k = 0; k < 12; ++k) {
              ws[k] = LoadGuestF32(base, ps_bank + ((5 + k / 4) * 4 + k % 4) * 4);
              if (!(ws[k] == ws[k]) || std::fabs(ws[k]) > 1e6f) {
                ws_ok = false;
              }
              if (k < 8 && (k % 4) < 3) {
                uvmag += std::fabs(ws[k]);
              }
            }
            if (ws_ok && uvmag > 1e-6f) {
              std::memcpy(g_dynobj_ws, ws, sizeof(ws));
              g_dynobj_ws_have = true;
            }
            if (!g_dynobj_have) {
              REXLOG_INFO(
                  "native-scene: dynamicobject rows captured sun=({:.3f},{:.3f},"
                  "{:.3f}) expo={:.2f} amb=({:.3f},{:.3f},{:.3f}) bounce={:.3f} "
                  "mult={:.2f} floor={:.3f}",
                  g_dynobj_rows[0], g_dynobj_rows[1], g_dynobj_rows[2],
                  g_dynobj_rows[3], g_dynobj_rows[4], g_dynobj_rows[5],
                  g_dynobj_rows[6], g_dynobj_rows[7], g_dynobj_rows[8],
                  g_dynobj_rows[9]);
            }
            g_dynobj_have = true;
            g_dynobj_frame_done = true;
          }
        } else if (fam >= 4 && !g_park_model_done[fam == 5 ? 1 : 0]) {
          // environmentpark.* bank layouts, read off this build's own ucode
          // and cross-checked against an F11 capture of a park-filled map.
          // All three share c0..c4 (the light-space rows) and c5..c8 (the
          // baked world-shade transform); they diverge from c9 on:
          //
          //   diffuse   sun c13, cascade bias c9.x, world biases c10.x/y,
          //             shadow floors c11/c12, tone c16.x, tangent light
          //             c17, multiplier c18.y, alpha c19.x, kd tint c20
          //   decal     identical through c13, then ONE extra register
          //             (c14 = camera position) and TWO spec registers
          //             (c19 tint/exponent, c20 fixed world spec light)
          //             that diffuse does not have: tone c17.x, tangent
          //             light c18, multiplier c21.y, alpha c22.x, kd c23
          //   alphatest a different shape entirely -- it is the
          //             dynamicobject model, register for register the
          //             same as alphatestdynamicobject_defaultPS: per-
          //             cascade biases c8.xyz, sun c9, tone c13.x, tangent
          //             light c14, multiplier c17.y, and an ADDITIVE flat
          //             ambient c18.xyz with bounce scale c18.w in place
          //             of the floor-lerp pair (ucode 92/94).
          //
          // The two shapes are carried through park_rows[23] as the model
          // selector; ShadePark implements both.
          // RETAIL layouts (ctab_diff + ucode; the dev notes above describe
          // sk83_na_m's compiles). All three retail park PSes use the SAME
          // floor-lerp / exponential-shadow model: CSM rows c0..c4, Exp rows
          // c5..c8, biases c9 / c10, floors c11 (dynamic) / c12
          // (exponential), sun c13, camera c14. They differ only above that:
          //   diffuse    g_envattributes c15..17 (tone c17.x), m_params c18
          //   decal      the same: tone c17.x, multiplier c18.y
          //   alphatest  projected-lightmap transform c15..16, so
          //              g_envattributes c17..19 (tone c19.x), m_params
          //              c20..21 (multiplier c20.y)
          // The tangent light (0.58, 0.62, 0.39) and spec power 290 are
          // literals in every retail compile (environmentparkdiffuse c253 /
          // c251), the dev i_params rows (alpha, kd tint) are gone: the
          // pieces are opaque in the PS and the per-piece tint is the VS's
          // i_tint_colour c13 (environmentparkdiffuse_defaultVS 27: o6 =
          // c13). So the alphatest variant is model 0 too, not the dev
          // dynamicobject shape.
          const bool at = fam == 5;
          const auto C = [&](uint32_t reg, uint32_t comp) {
            return LoadGuestF32(base, ps_bank + (reg * 4 + comp) * 4);
          };
          float sun[3];
          float n2 = 0.0f;
          for (int k = 0; k < 3; ++k) {
            sun[k] = C(13u, uint32_t(k));
            n2 += sun[k] * sun[k];
          }
          const float expo = C(at ? 19u : 17u, 0);
          const float mult = C(at ? 20u : 18u, 1);
          if (n2 > 0.9f && n2 < 1.1f && expo > 0.1f && expo < 16.0f &&
              mult > 0.0f && mult < 16.0f) {
            float rows[24] = {};
            rows[0] = sun[0];
            rows[1] = sun[1];
            rows[2] = sun[2];
            rows[3] = expo;
            rows[7] = mult;
            // The retail kd tint is per PIECE (VS i_tint_colour c13), not a
            // frame row: latching it from whichever park draw came first
            // painted every piece with that one draw's c13 (pure red with
            // some view directions). Frame rows keep the neutral tint.
            const float tint[3] = {1.0f, 1.0f, 1.0f};
            for (int k = 0; k < 3; ++k) {
              rows[4 + k] = C(11, uint32_t(k));
              rows[8 + k] = C(12, uint32_t(k));
              rows[12 + k] = tint[k];
            }
            rows[11] = 1.0f;
            rows[15] = 0.0f;
            rows[16] = 0.58f;
            rows[17] = 0.62f;
            rows[18] = 0.39f;
            rows[19] = 290.0f;
            // One cascade bias (c9.x) for every cascade, as the game set it
            // (the park pieces are not cascade casters -- see the caster
            // selection in RenderShadowAtlas).
            for (int k = 0; k < 3; ++k) {
              const float b = C(9, 0);
              rows[20 + k] = std::isfinite(b) && std::fabs(b) < 0.5f ? b : 0.0f;
            }
            rows[23] = 0.0f;
            std::memcpy(g_park_rows[at ? 1 : 0], rows, sizeof(rows));
            // Model 0 light cookie: VS c5/c6 (the planar uv, see
            // environmentpark{decal,diffuse}_defaultVS 23/31/32: o0.zw =
            // dot(c.xyz, world) + c.w) and the PS tf1 fetch words.
            if (!at) {
              const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
              const uint32_t dev = g_device.load(std::memory_order_relaxed);
              if (vs_bank != 0 && dev != 0 && GuestReadableApprox(base, dev + 0x480)) {
                float lm[8];
                bool ok = true;
                for (int k = 0; k < 8; ++k) {
                  lm[k] = LoadGuestF32(base, vs_bank + uint32_t((5 * 4 + k) * 4));
                  ok = ok && std::isfinite(lm[k]);
                }
                uint32_t fw[6];
                for (int k = 0; k < 6; ++k) {
                  fw[k] = REX_LOAD_U32(dev + 0x480 + uint32_t((1 * 6 + k) * 4));
                }
                // A 2D texture with a real base address and a non-zero
                // projection (dword0 type bits 0-1 = 2 = texture).
                ok = ok && (fw[0] & 3u) == 2u && (fw[1] & 0xFFFFF000u) != 0 &&
                     (lm[0] != 0.0f || lm[1] != 0.0f || lm[2] != 0.0f);
                if (ok) {
                  std::memcpy(g_park_lm, lm, sizeof(lm));
                  std::memcpy(g_park_lm_fetch, fw, sizeof(fw));
                  if (!g_park_lm_have) {
                    REXLOG_INFO(
                        "native-scene: environmentpark light cookie captured tf1={:08X} "
                        "u=({:.6f},{:.6f},{:.6f},{:.4f}) v=({:.6f},{:.6f},{:.6f},{:.4f})",
                        fw[1] & 0xFFFFF000u, lm[0], lm[1], lm[2], lm[3], lm[4], lm[5],
                        lm[6], lm[7]);
                  }
                  g_park_lm_have = true;
                }
              }
              // World term (tf2, see ParkWsDraw): the receivers' own
              // sampling constants c5 / c6 / c7 / c8 / c10 (shared by the
              // diffuse and decal banks, which agree through c13) and the
              // map's physical base, so the passes that fill it and the
              // resolve into it can be recognized.
              float wr[20];
              bool wok = true;
              for (int k = 0; k < 16; ++k) {
                wr[k] = C(5u + uint32_t(k / 4), uint32_t(k % 4));
                wok = wok && std::isfinite(wr[k]) && std::fabs(wr[k]) < 1e6f;
              }
              for (int k = 0; k < 4; ++k) {
                wr[16 + k] = C(10, uint32_t(k));
                wok = wok && std::isfinite(wr[16 + k]);
              }
              // c8 must be the pure-height ray row (0, dy, 0, w) and c10.z the
              // positive exponential-compare scale.
              wok = wok && std::fabs(wr[12]) < 1e-6f && std::fabs(wr[14]) < 1e-6f &&
                    wr[13] < 0.0f && wr[18] > 0.0f;
              if (wok && dev != 0 && GuestReadableApprox(base, dev + 0x480)) {
                const uint32_t w1 = REX_LOAD_U32(dev + 0x480 + 2 * 24 + 4);
                const uint32_t w2 = REX_LOAD_U32(dev + 0x480 + 2 * 24 + 8);
                if ((w1 & 0x3Fu) == 25u && (w2 & 0x1FFFu) == 1023u &&
                    ((w2 >> 13) & 0x1FFFu) == 511u) {
                  g_park_ws_tex_addr.store(w1 & 0xFFFFF000u, std::memory_order_relaxed);
                  std::memcpy(g_park_ws_recv, wr, sizeof(wr));
                  if (!g_park_ws_recv_have) {
                    REXLOG_INFO(
                        "native-scene: park world-shadow receivers captured tf2={:08X} "
                        "u=({:.4f},{:.4f},{:.4f},{:.4f}) ray=({:.4f},{:.4f},{:.4f},{:.4f}) "
                        "outer=({:.3f},{:.3f},{:.4f},{:.4f}) bias=({:.4f},{:.4f}) k={:.1f}",
                        w1 & 0xFFFFF000u, wr[0], wr[1], wr[2], wr[3], wr[12], wr[13], wr[14],
                        wr[15], wr[4], wr[5], wr[6], wr[7], wr[16], wr[17], wr[18]);
                  }
                  g_park_ws_recv_have = true;
                }
              }
            }
            if (!g_park_have[at ? 1 : 0]) {
              REXLOG_INFO(
                  "native-scene: environmentpark rows captured (model {}) "
                  "sun=({:.3f},{:.3f},{:.3f}) expo={:.2f} mult={:.2f} "
                  "floorA=({:.4f},{:.4f},{:.4f}) floorB=({:.4f},{:.4f},{:.4f})"
                  " kd=({:.2f},{:.2f},{:.2f}) alpha={:.2f}",
                  int(rows[23]), rows[0], rows[1], rows[2], rows[3], rows[7],
                  rows[4], rows[5], rows[6], rows[8], rows[9], rows[10],
                  rows[12], rows[13], rows[14], rows[11]);
            }
            g_park_have[at ? 1 : 0] = true;
            g_park_model_done[at ? 1 : 0] = true;
            g_park_frame_done = g_park_model_done[0] && g_park_model_done[1];
          }
        } else if (fam == 1 && !g_tree_frame_done) {
          const float c0x = LoadGuestF32(base, ps_bank + 0 * 4);
          const float c0y = LoadGuestF32(base, ps_bank + 1 * 4);
          const float c4y = LoadGuestF32(base, ps_bank + 17 * 4);
          if (c0x > 0.0f && c0x < 4.0f && c0y >= 0.0f && c0y < 1.0f &&
              c4y > 0.0f && c4y < 8.0f) {
            g_family_rows[0] = c0x;
            g_family_rows[1] = c0y;
            g_family_rows[2] = c4y;
            g_tree_frame_done = true;
          }
        } else if (fam == 2 && !g_proxy_frame_done) {
          const float c3y = LoadGuestF32(base, ps_bank + 13 * 4);
          if (c3y > 0.0f && c3y < 4.0f) {
            g_family_rows[3] = c3y;
            g_proxy_frame_done = true;
          }
        }
      }
    }
  }
  DrawSectMark(4);
  // Photo-editor postfx capture (SOLE site): read the game's staging banks
  // at draw time, the same reads the F11 per-draw records use, verified
  // correct for every postfx pass in captures 1783957072/1783963636. The
  // former SetPending upload-time capture mixed neighbouring passes' rows
  // (the game stages the next pass's constants before rebinding shaders)
  // and has been removed. Captured on every frame the game runs these
  // passes, not only in the photo flow: bails / replays run the same
  // cinematic chain in gameplay (see FrameScene::PhotoFx::gameplay).
  if (flags2d == 0 && SceneEnabled()) {
    int pfx_pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (pfx_pass < 0) {
      pfx_pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (pfx_pass >= 0) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
      const uint32_t dev = g_device.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        CapturePfxConstants(base, ps_bank, dev, /*pixel=*/true);
      }
      if (vs_bank != 0) {
        CapturePfxConstants(base, vs_bank, dev, /*pixel=*/false);
      }
    }
  }
  DrawSectMark(5);
  // Fisheye-family final pass: the camera-man / filmer lens
  // (postfx_basictex_fisheyePS, setup sub_82F96538; _opaquePS, sub_82F966C8)
  // and the respawn TV noise (postfx_basictex_fisheye_noisePS, sub_82F96858)
  // are the game's last postfx pass while active. Native rendering
  // suppresses them, so read the rows + fetch words at the draw like the
  // photo passes and replay the variant natively (photo_fx.hlsl ports).
  if (flags2d == 0 && SceneEnabled()) {
    // -1 = not a lens pass, else FrameScene::LensFx::Kind.
    const auto lens_kind_of = [&](uint32_t obj) -> int {
      if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
        return -1;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[160] = {};
      const uint32_t shader_path = ShaderPathAddr(base, obj);
      for (int k = 0; k < 159; ++k) {
        text[k] = char(REX_LOAD_U8(shader_path + k));
        if (text[k] == '\0') break;
      }
      int kind = -1;
      if (std::strstr(text, "postfx_basictex_fisheye_noisePS") != nullptr) {
        kind = FrameScene::LensFx::kNoise;
      } else if (std::strstr(text, "postfx_basictex_fisheye_opaquePS") != nullptr) {
        kind = FrameScene::LensFx::kFisheyeOpaque;
      } else if (std::strstr(text, "postfx_basictex_fisheyePS") != nullptr) {
        kind = FrameScene::LensFx::kFisheye;
      }
      if (cache.size() < 4096) {
        cache.emplace(obj, kind);
      }
      return kind;
    };
    int lens_kind = lens_kind_of(g_cur_ps_obj.load(std::memory_order_relaxed));
    if (lens_kind < 0) {
      lens_kind = lens_kind_of(g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (lens_kind >= 0) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
      const uint32_t dev = g_device.load(std::memory_order_relaxed);
      if (ps_bank != 0 && dev != 0 && GuestReadableApprox(base, dev + 0x480)) {
        for (int r = 0; r < 8; ++r) {
          for (int i = 0; i < 4; ++i) {
            g_lens_fx_ps[r][i] = LoadGuestF32(base, ps_bank + uint32_t(r * 4 + i) * 4);
            g_lens_fx_vs[r][i] =
                vs_bank != 0 ? LoadGuestF32(base, vs_bank + uint32_t(r * 4 + i) * 4) : 0.0f;
          }
        }
        for (int s = 0; s < 8; ++s) {
          for (int w = 0; w < 6; ++w) {
            g_lens_fx_fetch[s][w] = REX_LOAD_U32(dev + 0x480 + uint32_t(s * 6 + w) * 4);
          }
        }
        g_lens_fx_kind = lens_kind;
        const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed);
        g_lens_fx_blend = p2200 != 0 ? REX_LOAD_U32(p2200 + 1 * 4) : 0;
        g_lens_fx_seen = true;
      }
    }
  }
  DrawSectMark(6);
  // The game's bloom chain (see FrameScene::GameBloom): a bloom_dof PS name
  // gates, the pass comes from the render-target size (PA_CL_VPORT_XSCALE 0x210F = half
  // the target width; the game's chain runs at fixed 288x160 / 72x40 /
  // 32x16 off its 1152x640 scene) and keep its PS rows c0..c10.
  if (flags2d == 0 && SceneEnabled()) {
    // 0 = not bloom, 1 threshold, 2 tap4, 3 tap9, 4 combine3, 5 ghost.
    const auto bloom_kind_of = [&](uint32_t obj) -> int {
      if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
        return 0;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[160] = {};
      const uint32_t shader_path = ShaderPathAddr(base, obj);
      for (int k = 0; k < 159; ++k) {
        text[k] = char(REX_LOAD_U8(shader_path + k));
        if (text[k] == '\0') break;
      }
      int kind = 0;
      if (std::strstr(text, "bloom_dof_tap4_minusthresholdPS") != nullptr) {
        kind = 1;
      } else if (std::strstr(text, "bloom_dof_tap4.") != nullptr) {
        kind = 2;
      } else if (std::strstr(text, "bloom_dof_tap9PS") != nullptr) {
        kind = 3;
      } else if (std::strstr(text, "bloom_dof_tap9_combine3PS") != nullptr) {
        kind = 4;
      } else if (std::strstr(text, "bloom_dof_bloom_ghostPS") != nullptr) {
        kind = 5;
      }
      if (cache.size() < 4096) {
        cache.emplace(obj, kind);
      }
      return kind;
    };
    int bloom_kind = bloom_kind_of(g_cur_ps_obj.load(std::memory_order_relaxed));
    if (bloom_kind == 0) {
      bloom_kind = bloom_kind_of(g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
    const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
    // func 1 = DrawVertices, the game's fullscreen postfx quads.
    if (func == 1 && p2100 != 0 && ps_bank != 0) {
      const float xscale = LoadGuestF32(base, p2100 + 0xF * 4);
      const int rt_w = int(std::lround(std::fabs(xscale) * 2.0f));
      const float dir_x = LoadGuestF32(base, ps_bank + 4 * 4);  // tap9 c1.x
      // The bound-shader label is stale across these back-to-back fullscreen
      // draws (F11: the tap4 downsamples carried the tap9 blurs' PS object),
      // so the name only says "a bloom pass"; which one comes from the
      // game's own data: tap9's rows start (1, 2, 3, level), the 72x40 tap4
      // downsample reads the 288-wide level (fetch slot 0 width), and the
      // threshold / combine / ghost passes all target 288x160.
      const bool tap9_sig = LoadGuestF32(base, ps_bank + 0) == 1.0f &&
                            LoadGuestF32(base, ps_bank + 4) == 2.0f &&
                            LoadGuestF32(base, ps_bank + 8) == 3.0f;
      const uint32_t dev = g_device.load(std::memory_order_relaxed);
      const int tf0_w =
          dev != 0 ? int((REX_LOAD_U32(dev + 0x480 + 2 * 4) & 0x1FFFu) + 1) : 0;
      int slot = -1;
      using GB = FrameScene::GameBloom;
      // The 72x40 tap4 downsample needs no name: its draw's bound-shader
      // label is not a bloom shader at all (log: every other pass matched,
      // this one never did). The only other game pass of that shape, the
      // exposure chain's 288->72 step, carries the same +-1 texel offsets,
      // which are the only rows tap4 reads. Every other pass keeps the
      // bloom-name gate.
      const bool downb_shape = !tap9_sig && rt_w == 72 && tf0_w == 288;
      if (bloom_kind == 0 && !downb_shape) {
        // not a bloom pass
      } else if (tap9_sig) {
        if (rt_w == 288) {
          slot = GB::kBlurAH;
        } else if (rt_w == 72) {
          slot = dir_x != 0.0f ? GB::kBlurBH : GB::kBlurBV;
        }
      } else if (rt_w == 72 && tf0_w == 288) {
        slot = GB::kDownB;
      } else if (rt_w == 288 && tf0_w == 1152) {
        slot = GB::kThreshold;  // reads the 1152x640 scene
      } else if (rt_w == 288 && tf0_w == 288) {
        // bloom_ghost's rows start with its negative ghost UV scales;
        // tap9_combine3's with its positive level weights.
        const bool ghost_sig = LoadGuestF32(base, ps_bank + 0) < 0.0f &&
                               LoadGuestF32(base, ps_bank + 4 * 4) < 0.0f;
        slot = ghost_sig ? GB::kGhost : GB::kCombine;
      }
      if (slot >= 0) {
        for (int r = 0; r < 11; ++r) {
          for (int i = 0; i < 4; ++i) {
            g_game_bloom_rows[slot][r][i] =
                LoadGuestF32(base, ps_bank + uint32_t(r * 4 + i) * 4);
          }
        }
        g_game_bloom_seen |= 1u << slot;
      }
    }
  }
  DrawSectMark(7);
  // UI background blur: while a frontend popup is up, the game appends a
  // dedicated pass chain after the postfx uber: blur_hBlurPS + blur_vBlurPS
  // (func-2 inline quads OUTSIDE the 2D phase) then a postfx_basictex
  // fullscreen replace. The blur_hBlurPS draw itself is the trigger; its
  // PS c0.x is the kernel scale (8 in every capture). See kBlurShaderSource.
  if (func == 2 && flags2d == 0 && SceneEnabled()) {
    const auto is_hblur = [&](uint32_t obj) -> bool {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return false;
      }
      // Guest-render-thread only (like the fog capture globals).
      static std::unordered_map<uint32_t, int> cache;  // 0 no, 1 plain, 2 masked
      auto it = cache.find(obj);
      int kind = 0;
      if (it != cache.end()) {
        kind = it->second;
      } else {
        // Debug path (see ShaderPathAddr), e.g. ".../blur_hBlurPS.updb".
        char text[200] = {};
        const uint32_t shader_path = ShaderPathAddr(base, obj);
        for (int k = 0; k < 199; ++k) {
          text[k] = char(REX_LOAD_U8(shader_path + k));
          if (text[k] == '\0') break;
        }
        // The masked variant (the pick-an-object menu; see
        // FrameScene::ui_blur_masked) carries the same c0 kernel / c1 tint.
        kind = std::strstr(text, "blur_hBlurPS") != nullptr         ? 1
               : std::strstr(text, "blur_hBlurMaskedPS") != nullptr ? 2
                                                                    : 0;
        if (cache.size() < 1024) {
          cache.emplace(obj, kind);
        }
      }
      if (kind == 2) {
        g_ui_blur_masked_seen = true;
      }
      return kind != 0;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    if (is_hblur(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
        is_hblur(g_cur_vs_obj.load(std::memory_order_relaxed))) {
      // Kernel scale read LIVE under a plausibility gate (k0 == k1, in
      // (0, 8]) so the game's own pause-open ramp animates the native blur
      // like the emulated frame; the old pin-to-8 popped the backdrop to
      // full blur on the first frame, and its exact-(8,8) gate on the fade
      // rejected every mid-ramp c1 (the tint then snapped in late). A
      // failed gate keeps the last values (stale bank mid-rewrite); raw
      // 1-frame value flaps (the tinted-backdrop flicker) are absorbed by
      // the render-thread smoothing at the blur pass.
      g_ui_blur_seen = true;
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        const float k0 = LoadGuestF32(base, ps_bank + 0);
        const float k1 = LoadGuestF32(base, ps_bank + 4);
        float c1[3];
        for (int a = 0; a < 3; ++a) {
          c1[a] = LoadGuestF32(base, ps_bank + 16 + a * 4);
        }
        const bool kernel_ok = k0 == k1 && k0 > 0.0f && k0 <= 8.0f;
        const bool fade_ok = c1[0] >= 0.0f && c1[0] <= 1.0f && c1[1] >= 0.0f &&
                             c1[1] <= 1.0f && c1[2] >= 0.0f && c1[2] <= 1.0f;
        if (kernel_ok) {
          g_ui_blur = k0;
          if (fade_ok) {
            std::memcpy(g_ui_blur_color, c1, sizeof(g_ui_blur_color));
          }
        }
        // Open-ramp tracer (guest render thread only): the first draws
        // after each blur-ON edge print the RAW kernel/fade so the game's
        // actual animate-in curve lands in the log (for an exactness pass
        // if the smoothed approximation reads wrong).
        static int64_t s_last_draw_ns = 0;
        static uint32_t s_edge_logged = 0;
        static uint32_t s_total_logged = 0;
        const int64_t trace_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
        if (trace_now - s_last_draw_ns > 1'000'000'000ll) {
          s_edge_logged = 0;
        }
        s_last_draw_ns = trace_now;
        if (s_edge_logged < 20 && s_total_logged < 100) {
          ++s_edge_logged;
          ++s_total_logged;
          REXLOG_DEBUG(
              "native-scene: blur ramp k=({:.3f},{:.3f}) c1=({:.3f},{:.3f},{:.3f}) {}",
              k0, k1, c1[0], c1[1], c1[2], kernel_ok ? "accepted" : "held");
        }
      }
    }
  }
  DrawSectMark(8);
  // Selected-object outline capture (see g_frame_selected): the sky draw
  // opens the post-sky window; environmentpark/dynamicobject draws inside it
  // are the selection re-draws (the game excludes the selected object from
  // the main pass and stencil-marks it here). The postfx_edgedetectstencil
  // draw refreshes the outline color.
  if (flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_selection_outline)) {
    // Debug-path family, cached per shader object (guest render thread only,
    // like the blur classifier). 1 = sky, 2 = park piece / dynamic object,
    // 3 = postfx_edgedetectstencil.
    const auto family_of = [&](uint32_t obj) -> int {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return 0;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[120] = {};
      const uint32_t shader_path = ShaderPathAddr(base, obj);
      for (int k = 0; k < 119; ++k) {
        text[k] = char(REX_LOAD_U8(shader_path + k));
        if (text[k] == '\0') break;
      }
      int fam = 0;
      if (std::strstr(text, "\\sky_") != nullptr) {
        fam = 1;
      } else if (std::strstr(text, "\\environmentpark") != nullptr ||
                 std::strstr(text, "\\dynamicobject") != nullptr) {
        fam = 2;
      } else if (std::strstr(text, "postfx_edgedetectstencil") != nullptr) {
        fam = 3;
      }
      if (cache.size() < 4096) {
        cache.emplace(obj, fam);
      }
      return fam;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    int fam = family_of(g_cur_ps_obj.load(std::memory_order_relaxed));
    if (fam == 0) {
      fam = family_of(g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (func == 0 && fam == 1) {
      g_sky_seen_this_frame = true;
    } else if (func == 0 && fam == 2 && g_sky_seen_this_frame) {
      // World matrix in the VS bank: 3 rotation rows (w == 0) followed by
      // the translation row (w == 1): c11..c14 on the decal variant,
      // c9..c12 on the diffuse variant. Scan for the first such group.
      for (int r = 8; r <= 16; ++r) {
        if (LoadGuestF32(base, bank + (r * 4 + 3) * 4) != 1.0f) continue;
        bool rot = true;
        for (int p = 1; p <= 3 && rot; ++p) {
          rot = LoadGuestF32(base, bank + ((r - p) * 4 + 3) * 4) == 0.0f;
        }
        if (!rot) continue;
        float t[3];
        for (int a = 0; a < 3; ++a) {
          t[a] = LoadGuestF32(base, bank + (r * 4 + a) * 4);
        }
        if (std::fabs(t[0]) < 100000.0f && std::fabs(t[1]) < 100000.0f &&
            std::fabs(t[2]) < 100000.0f) {
          const uint32_t ib = g_cur_ib.load(std::memory_order_relaxed);
          const uint32_t vb = g_cur_vb.load(std::memory_order_relaxed);
          bool merged = false;
          for (SelectedDrawKey& k : g_frame_selected) {
            if (k.ib == ib && k.vb == vb && std::fabs(k.t[0] - t[0]) < 0.05f &&
                std::fabs(k.t[1] - t[1]) < 0.05f && std::fabs(k.t[2] - t[2]) < 0.05f) {
              ++k.count;
              merged = true;
              break;
            }
          }
          if (!merged && g_frame_selected.size() < 64) {
            g_frame_selected.push_back({ib, vb, {t[0], t[1], t[2]}, 1});
          }
        }
        break;
      }
    } else if (fam == 3) {
      // postfx edge-detect: the presence of this draw is what authorizes the
      // native outline this frame (see g_outline_edge_seen); its PS c0 = the
      // outline color as staged (the park-editor blue (0.216, 0.647, 1.0) in
      // every capture).
      g_outline_edge_seen = true;
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        float c[4];
        for (int a = 0; a < 4; ++a) {
          c[a] = LoadGuestF32(base, ps_bank + a * 4);
        }
        if (c[0] >= 0.0f && c[0] <= 8.0f && c[1] >= 0.0f && c[1] <= 8.0f &&
            c[2] >= 0.0f && c[2] <= 8.0f) {
          std::memcpy(g_outline_color, c, sizeof(g_outline_color));
        }
      }
    }
  }
  DrawSectMark(9);
  // Live 2D overlay capture: the HUD renders exclusively through the
  // BeginVertices inline path (func 2). Vertex payloads are read at frame
  // end; everything else (transform constants, texture fetch) is staged now.
  // (An "unbracketed capture" for out-of-bracket boot draws lived here
  // briefly, removed: every real boot/menu UI draw is bracketed, including
  // the intro video quad, and it demonstrably ingested blur/postfx pass
  // quads during takeover-hold windows.)
  if (flags2d != 0 && SceneEnabled() && REXCVAR_GET(skate3_native_render_scene_2d) &&
      !simple_draw_3d) {
    if (func == 2) {
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      if (device != 0 && r7 >= 0x10000 && r5 != 0 && r5 <= 65536 && r6 >= 8 &&
          r6 <= 256 && (r6 & 3) == 0) {
        Draw2d d;
        d.prim = r4;
        d.count = r5;
        d.stride = r6;
        d.addr = r7;
        d.flags = flags2d;
        // Slots 0-2 (18 dwords): slot 0 is the draw's texture; slots 1-2
        // matter only for video quads, whose YUV shader binds the U and V
        // planes there (the self-contained YUV-triple detection at replay).
        for (int i = 0; i < 18; ++i) {
          d.fetch[i] = REX_LOAD_U32(device + 0x480 + i * 4);
        }
        for (int i = 0; i < 36; ++i) {
          d.consts[i] = LoadGuestF32(base, bank + i * 4);
        }
        // The draw's blend / color-write state, straight from the device's
        // register arrays: the debug menu's title bar draws opaque (blend
        // off) while its item bars blend at c8.a = 0.8; one fixed blend
        // for every 2D draw lost that.
        if (const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed)) {
          d.blend_control = REX_LOAD_U32(p2200 + (0x2201 - 0x2200) * 4);
        }
        if (const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed)) {
          d.color_mask = REX_LOAD_U32(p2100 + (0x2104 - 0x2100) * 4);
        }
        std::lock_guard<std::mutex> lock(g_2d_mutex);
        if (g_frame_2d.size() < 4096) {
          g_frame_2d.push_back(std::move(d));
        } else {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
    }
  }
  DrawSectMark(10);
  // In-world neon spline capture (see SplineDraw): a DrawVertices strip of
  // 12-byte float3 params on the spline shaders, bound VB object carrying
  // the payload fetch block (+0x18 base | flags, +0x20 size).
  //
  // D3DDevice_DrawVertices(PrimitiveType, StartVertex, VertexCount) puts the
  // count in r6, not r5 (r5 is the start vertex). Gating on r5 rejected every
  // draw that starts at vertex 0, which is all of them: the Death Race gates
  // and the Follow the Leader ribbons come through here as prim-6 strips of
  // 232-544 vertices with StartVertex 0 (each gate/ribbon is its own draw,
  // sharing the param buffer and carrying its own control points in the
  // constant bank), so none of this was ever captured.
  const uint32_t spline_start = r5;
  const uint32_t spline_count = r6;
  if (func == 1 && flags2d == 0 && SceneEnabled() && r4 == 6 &&
      REXCVAR_GET(skate3_native_render_scene_splines) && spline_count >= 4 &&
      spline_count <= 8192) {
    uint32_t kind = ClassifySplineShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (kind == 0) {
      kind = ClassifySplineShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
    if (kind != 0 && GuestReadableApprox(base, vb_obj)) {
      const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFCu;
      const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
      if (addr >= 0x10000 &&
          uint64_t(size) >= (uint64_t(spline_start) + spline_count) * 12) {
        SplineDraw s;
        s.pass = kind;
        s.count = spline_count;
        s.verts.resize(size_t(spline_count) * 12);
        if (GuestTryCopy(s.verts.data(), base + addr + spline_start * 12,
                         s.verts.size())) {
          const uint32_t device = g_device.load(std::memory_order_relaxed);
          for (int i = 0; i < 6; ++i) {
            // The gradient texture lives in fetch shadow slot 3 (24 bytes
            // per slot; the spline PS samples tf3).
            s.fetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + 3 * 24 + i * 4) : 0;
          }
          for (int i = 0; i < 153 * 4; ++i) {
            s.consts[i] = LoadGuestF32(base, bank + i * 4);
          }
          g_draws_spline.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lock(g_2d_mutex);
          // 64 was hit every frame in Death Race (its gates alone exceed
          // it), dropping the later gates' glow passes.
          if (g_frame_spline.size() < 512) {
            g_frame_spline.push_back(std::move(s));
          }
        }
      }
    }
  }
  const uint32_t cur_ib = g_cur_ib.load(std::memory_order_relaxed);
  const uint32_t cur_vb = g_cur_vb.load(std::memory_order_relaxed);
  DrawSectMark(11);
  if (g_recording.load(std::memory_order_relaxed)) {
    // Sample the full draw stream on a couple of recorded frames out of
    // every 60, spread across the window (ground-truth coverage for the
    // whole recording), and only for frames that are themselves recorded.
    // 2D-phase draws are recorded on EVERY recorded frame; the HUD stream
    // is small and is exactly what the 2D reconstruction needs.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const bool frame_recorded = (g_frames_seen + 1) % g_record_stride == 0;
    const bool all_draws = REXCVAR_GET(skate3_native_render_snapshot_all_draws);
    // 600k: a 360-frame all-draws photo-compose trace is ~470k records (a
    // 200k cap cuts such a trace around frame 160, before the
    // late-firing card compose).
    const size_t draw_cap = all_draws ? 600000 : 32768;
    if (frame_recorded && (all_draws || flags2d != 0 || (g_record_frame % 60) < 2) &&
        g_recorded_draws.size() < draw_cap) {
      auto rec = std::make_unique<RecordedDraw>();
      rec->func = func;
      rec->ib = cur_ib;
      rec->vb = cur_vb;
      rec->vb_offset = g_cur_vb_offset.load(std::memory_order_relaxed);
      rec->vb_stride = g_cur_vb_stride.load(std::memory_order_relaxed);
      for (int s = 0; s < 4; ++s) {
        for (int k = 0; k < 3; ++k) {
          rec->streams[s][k] = g_cur_streams[s][k].load(std::memory_order_relaxed);
        }
      }
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      for (int i = 0; i < 12; ++i) {
        rec->vfetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->args[0] = r4;
      rec->args[1] = r5;
      rec->args[2] = r6;
      rec->args[3] = r7;
      for (uint32_t i = 0; i < 1024; ++i) {
        rec->bank[i] = LoadGuestF32(base, bank + i * 4);
      }
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < 256; ++i) {
        rec->ps[i] = ps_bank != 0 ? LoadGuestF32(base, ps_bank + i * 4) : 0.0f;
      }
      rec->frame = g_record_frame;
      rec->flags2d = flags2d;
      rec->ps_obj = g_cur_ps_obj.load(std::memory_order_relaxed);
      rec->vs_obj = g_cur_vs_obj.load(std::memory_order_relaxed);
      for (int i = 0; i < 6; ++i) {
        rec->viewport[i] = g_cur_viewport[i].load(std::memory_order_relaxed);
      }
      for (int i = 0; i < 4; ++i) {
        rec->scissor[i] = g_cur_scissor[i].load(std::memory_order_relaxed);
      }
      // rstates[0..63] = registers 0x2200 + i (RB_DEPTHCONTROL,
      // RB_BLENDCONTROL0 0x2201, RB_COLORCONTROL 0x2202, ...), [64..127] =
      // 0x2100 + i (RB_COLOR_MASK 0x2104, RB_ALPHA_REF 0x210E, ...): the
      // device's current register arrays at this draw (see g_rs_ptr_2200).
      // The last-uploaded bank alone was whichever group changed last.
      const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed);
      const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
      for (int i = 0; i < 256; ++i) {
        rec->rstates[i] = 0;
      }
      for (int i = 0; i < 64; ++i) {
        rec->rstates[i] = p2200 != 0 ? REX_LOAD_U32(p2200 + i * 4) : 0;
        rec->rstates[64 + i] = p2100 != 0 ? REX_LOAD_U32(p2100 + i * 4) : 0;
      }
      for (int i = 0; i < 192; ++i) {
        rec->vfetch_all[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->vb_dump = ~0u;
      rec->ib_dump = ~0u;
      if (flags2d != 0) {
        // 2D payload capture. The 2D pass runs on transient dynamic buffers
        // (glyph/shape vertices regenerated per frame); dump them now.
        // One dump per (guest address, frame); recording mode, so the
        // GuestRangeReadable VAD cost is acceptable.
        const auto dump_buffer = [&](uint32_t obj, uint32_t size_off) -> uint32_t {
          if (obj == 0) {
            return ~0u;
          }
          const uint32_t addr = REX_LOAD_U32(obj + 0x18) & 0xFFFFFFFCu;
          if (addr < 0x10000) {
            return ~0u;
          }
          uint32_t bytes = REX_LOAD_U32(obj + size_off);
          if (bytes < 4 || bytes > (8u << 20)) {
            return ~0u;
          }
          const uint64_t key = (uint64_t(g_record_frame) << 32) | addr;
          auto it = g_frame_dump_ids.find(key);
          if (it != g_frame_dump_ids.end()) {
            return it->second;
          }
          if (g_recorded_buffer_bytes + bytes > (512u << 20) ||
              !GuestRangeReadable(base, addr, bytes)) {
            return ~0u;
          }
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = key;
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
          g_frame_dump_ids.emplace(key, dump_id);
          return dump_id;
        };
        if (func == 0) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
          rec->ib_dump = dump_buffer(cur_ib, 0x1C);
        } else if (func == 1) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
        } else if (func == 2 && r7 >= 0x10000 && r5 != 0 && r6 != 0 &&
                   uint64_t(r5) * r6 <= (4u << 20)) {
          // Inline-ring vertices (r5 = count, r6 = stride, r7 = the write
          // pointer BeginVertices returned): the CPU writes them after the
          // call; dump at frame end.
          g_pending_inline_dumps.push_back({r7, r5 * r6, g_recorded_draws.size()});
        }
      }
      if (func == 1 && cur_vb != 0 && flags2d == 0) {
        // Non-indexed (cloth) draw: the bound object holds two ping-pong
        // vertex fetch blocks whose ring payloads are recycled long before
        // frame end; dump both now, keyed back to this draw via rec->ib.
        const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
        for (uint32_t block = 0; block < 2; ++block) {
          const uint32_t fetch0 = REX_LOAD_U32(cur_vb + 0x18 + block * 0x30);
          const uint32_t addr = fetch0 & 0xFFFFFFFC;
          if (addr < 0x10000) continue;
          uint32_t bytes = REX_LOAD_U32(cur_vb + 0x20 + block * 0x30);
          if (bytes < 64 || bytes > (256u << 10)) bytes = 128u << 10;
          if (g_recorded_buffer_bytes + bytes > (512u << 20)) break;
          while (bytes >= 4096 && !GuestRangeReadable(base, addr, bytes)) {
            bytes /= 2;
          }
          if (bytes < 4096) continue;
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = (uint64_t(g_record_frame) << 32) | dump_id | (block << 30);
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
        }
        rec->ib = 0x80000000u | dump_id;
      }
      g_recorded_draws.push_back(std::move(rec));
    }
  }
  DrawSectMark(12);
  if (func != 0) {
    // CAS skin jitter blur (see g_skin_blur_c0x): the game's quad into the
    // 128x128 texture-space target (viewport x scale 64); its PS c0.xy is
    // the tap-offset scale.
    if (flags2d == 0 && SceneEnabled()) {
      const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      if (p2100 != 0 && psb != 0 &&
          std::fabs(std::fabs(LoadGuestF32(base, p2100 + 0xF * 4)) - 64.0f) < 0.5f) {
        const float cx = LoadGuestF32(base, psb + 0 * 4);
        const float cy = LoadGuestF32(base, psb + 1 * 4);
        if (cx > 0.01f && cx < 8.0f && cy > 0.01f && cy < 8.0f) {
          uint32_t bx, by;
          std::memcpy(&bx, &cx, 4);
          std::memcpy(&by, &cy, 4);
          g_skin_blur_c0x.store(bx, std::memory_order_relaxed);
          g_skin_blur_c0y.store(by, std::memory_order_relaxed);
        }
      }
    }
    return;  // palette fixup below matches indexed draws only
  }
  DrawSectMark(13);
  const uint64_t key = (uint64_t(cur_ib) << 32) | cur_vb;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  // World render state per island (IslandState::rs): the same registers
  // the ib-keyed draw_rs records, from a perspective draw whose bound pixel
  // shader is a main-pass one (no z-prepass / caster / occlusion shader).
  // One g_islands lookup per draw serves the render state below and every
  // per-family capture after it.
  IslandState* isl = nullptr;
  const uint64_t ikey = DrawKey(cur_ib, cur_vb, r5, r6);
  // Captured render-state-only islands are needed on their refresh frame
  // alone (below); everything else per draw is in g_islands_hot.
  const bool isl_refresh = ((g_guest_frame + (ikey >> 4) + ikey) & 31u) == 0;
  if (flags2d == 0 && SceneEnabled() && !g_islands.empty() &&
      (isl_refresh || g_islands_hot.contains(ikey))) {
    isl = g_islands.find(ikey);
  }
  // The registers come from the regstate section above (draw_rs_now, same
  // perspective-bank test). A surface's render state practically never
  // changes, so after its first capture each island refreshes once every
  // 32 frames (staggered by island), which still picks up the debug menu's
  // wireframe toggle within a fraction of a second; the main-pass shader
  // test only runs on those frames.
  if (isl != nullptr && (isl->reg & kIslRs) != 0 && draw_rs_valid) {
    WorldDrawRs& w = isl->rs;
    if ((!w.seen || isl_refresh) &&
        (CensusPsKind(base, g_cur_vs_obj.load(std::memory_order_relaxed)) == 1 ||
         CensusPsKind(base, g_cur_ps_obj.load(std::memory_order_relaxed)) == 1)) {
      w.rs[0] = draw_rs_now.depth;
      w.rs[1] = draw_rs_now.blend;
      w.rs[2] = draw_rs_now.color_ctl;
      w.rs[3] = draw_rs_now.color_mask;
      w.rs[4] = draw_rs_now.stencil;
      w.rs[5] = draw_rs_now.mode_cntl;
      if (!w.seen && (isl->reg & kIslPerDraw) == 0) {
        g_islands_hot.erase(ikey);  // captured: refresh frames only from now
      }
      w.seen = true;
    }
  }
  if (isl != nullptr && (isl->reg & (kIslDecal | kIslTraffic | kIslHom | kIslFlow | kIslAd)) != 0) {
    // Per-draw game state for registered islands (IslandState::reg), taken
    // only while the matching main-pass pixel shader is bound: the decal
    // art binding (every decal[2]environment PS fetches the art at tf4),
    // the trafficlight / Hall of Meat / flowing-water / advertisement
    // constants.
    DecalDrawAddr* dit = (isl->reg & kIslDecal) != 0 ? &isl->decal : nullptr;
    TrafficDrawState* tit = (isl->reg & kIslTraffic) != 0 ? &isl->traffic : nullptr;
    HomDrawState* hit_hom = (isl->reg & kIslHom) != 0 ? &isl->hom : nullptr;
    FlowWaterDrawState* fit_flow = (isl->reg & kIslFlow) != 0 ? &isl->flow : nullptr;
    AdDrawState* ait = (isl->reg & kIslAd) != 0 ? &isl->ad : nullptr;
    const uint32_t dev = g_device.load(std::memory_order_relaxed);
    // Both trackers: the SetShader hook's pixel/vertex labels are SWAPPED in
    // this build (rex_D3DDevice_SetVertexShader_82B7F408 stores into g_cur_ps_obj;
    // see OnSetShader / BoundPsNameContains), so the real pixel shader sits
    // in g_cur_vs_obj. Reading g_cur_ps_obj alone saw only the
    // ..._defaultVS name and never captured a single decal draw.
    const int ps_kind =
        dev != 0
            ? std::max(WorldPsKindOf(base, g_cur_ps_obj.load(std::memory_order_relaxed)),
                       WorldPsKindOf(base, g_cur_vs_obj.load(std::memory_order_relaxed)))
            : 0;
    // trafficlight_*: the lamp states (VS c7.xyzw; positional bank) and
    // the emissive scale (PS c3.y) of this draw.
    if (ps_kind == 3 && tit != nullptr) {
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      TrafficDrawState& t = *tit;
      bool ok = true;
      float c7[4];
      for (uint32_t k = 0; k < 4; ++k) {
        c7[k] = LoadGuestF32(base, bank + (7u * 4u + k) * 4u);
        ok = ok && std::isfinite(c7[k]) && std::fabs(c7[k]) < 1e6f;
      }
      const float c3y = psb != 0 ? LoadGuestF32(base, psb + (3u * 4u + 1u) * 4u) : 1.0f;
      if (ok && std::isfinite(c3y) && std::fabs(c3y) < 1e6f) {
        std::memcpy(t.c7, c7, sizeof(c7));
        t.c3y = c3y;
        t.seen = true;
      }
    }
    // defaulthom: its PS constants c1.y (output scale), the bone tint rgb +
    // fresnel power, and the (mix scale, texture floor) pair. Retail split
    // the dev m_params[1]/[6] into named parameters (CTAB: m_params c1..2,
    // i_boneClr c3, i_colorParams c4; ucode 7/11/12/16): tint = c3.xyz,
    // power = c2.w, mix/floor = c4.xy. The dev rows were c2.xyzw and c7.xy;
    // reading c2.xyz on retail tinted the skeleton with an unrelated row.
    if (ps_kind == 4 && hit_hom != nullptr) {
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      if (psb != 0) {
        float v[7] = {LoadGuestF32(base, psb + (1u * 4u + 1u) * 4u),
                      LoadGuestF32(base, psb + (3u * 4u + 0u) * 4u),
                      LoadGuestF32(base, psb + (3u * 4u + 1u) * 4u),
                      LoadGuestF32(base, psb + (3u * 4u + 2u) * 4u),
                      LoadGuestF32(base, psb + (2u * 4u + 3u) * 4u),
                      LoadGuestF32(base, psb + (4u * 4u + 0u) * 4u),
                      LoadGuestF32(base, psb + (4u * 4u + 1u) * 4u)};
        bool ok = true;
        for (float f : v) {
          ok = ok && std::isfinite(f) && std::fabs(f) < 1e6f;
        }
        if (ok) {
          HomDrawState& hs = *hit_hom;
          hs.c1y = v[0];
          std::memcpy(hs.c2, v + 1, sizeof(hs.c2));
          std::memcpy(hs.c7, v + 5, sizeof(hs.c7));
          hs.seen = true;
        }
      }
    }
    // flowingwater[alpha]: this draw's own material rows and colour map
    // (tf4), see DrawItem::water_rows.
    if (ps_kind == 5 && fit_flow != nullptr) {
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      float rows[32];
      if (psb != 0 && ReadFlowWaterRows(base, psb, rows)) {
        FlowWaterDrawState& fs = *fit_flow;
        std::memcpy(fs.rows, rows, sizeof(rows));
        fs.tf4 = g_tex_slot[4];
        fs.seen = true;
      }
    }
    // advertisement_*: this draw's own rows (same registers and range gate
    // as the frame capture: c6.z floor, c10.y multiplier, c9.x exposure)
    // and its tf4 / tf3 fetch constants (the art, ucode line 4, and the
    // lightmap, lines 19-22).
    if (ps_kind == 6 && ait != nullptr) {
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      if (psb != 0) {
        const float floor_z = LoadGuestF32(base, psb + (6 * 4 + 2) * 4);
        const float mult = LoadGuestF32(base, psb + (10 * 4 + 1) * 4);
        const float expo = LoadGuestF32(base, psb + (9 * 4 + 0) * 4);
        if (floor_z >= 0.0f && floor_z <= 1.0f && mult > 0.0f && mult < 16.0f &&
            expo > 0.01f && expo < 100.0f) {
          AdDrawState& as = *ait;
          as.rows[0] = floor_z;
          as.rows[1] = mult;
          as.rows[2] = expo;
          uint32_t w[6];
          for (int k = 0; k < 6; ++k) {
            w[k] = REX_LOAD_U32(dev + 0x480 + 4 * 24 + uint32_t(k) * 4);
          }
          if ((w[0] & 3u) == 2u && w[1] != 0) {
            std::memcpy(as.tf4, w, sizeof(w));
          }
          for (int k = 0; k < 6; ++k) {
            w[k] = REX_LOAD_U32(dev + 0x480 + 3 * 24 + uint32_t(k) * 4);
          }
          if ((w[0] & 3u) == 2u && w[1] != 0) {
            std::memcpy(as.tf3, w, sizeof(w));
          }
          as.seen = true;
        }
      }
    }
    const int decal_ps =
        dit != nullptr && (ps_kind == 1 || ps_kind == 2) ? ps_kind : 0;
    if (decal_ps != 0) {
      uint32_t w[6];
      for (int k = 0; k < 6; ++k) {
        w[k] = REX_LOAD_U32(dev + 0x480 + 4 * 24 + uint32_t(k) * 4);
      }
      if ((w[0] & 3u) == 2u && w[1] != 0) {
        std::memcpy(dit->words, w, sizeof(w));
      }
      // decal2environment: its second art layer at tf5 (PS 17 / 19).
      if (decal_ps == 2) {
        for (int k = 0; k < 6; ++k) {
          w[k] = REX_LOAD_U32(dev + 0x480 + 5 * 24 + uint32_t(k) * 4);
        }
        if ((w[0] & 3u) == 2u && w[1] != 0) {
          std::memcpy(dit->words2, w, sizeof(w));
        }
      }
      // VS c10.x (positional bank: register 10 at +10*16): the macro fade
      // floor, see DecalDrawAddr::fade_floor.
      if (bank != 0) {
        const float f = LoadGuestF32(base, bank + 10u * 16u);
        if (std::isfinite(f) && f >= 0.0f && f < 1e3f) {
          dit->fade_floor = f;
        }
      }
    }
  }
  DrawSectMark(14);
  if (flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_shader_census)) {
    // Shader census (see g_census_ps): the pixel shader of this draw, from
    // whichever tracker holds it (the hook labels are swapped in this
    // build), minus depth-prepass / caster / occlusion permutations.
    for (const uint32_t obj : {g_cur_vs_obj.load(std::memory_order_relaxed),
                               g_cur_ps_obj.load(std::memory_order_relaxed)}) {
      const int kind = CensusPsKind(base, obj);
      if (kind != 1) {
        continue;
      }
      const uint64_t dkey = DrawKey(cur_ib, cur_vb, r5, r6);
      if (g_census_ps.size() >= 65536) {
        break;
      }
      CensusPs& e = g_census_ps[dkey];
      for (uint32_t& slot : e.obj) {
        if (slot == obj) {
          break;
        }
        if (slot == 0) {
          slot = obj;
          break;
        }
      }
    }
  }
  DrawSectMark(15);
  if (flags2d == 0 && SceneEnabled()) {
    // The receivers' tf1 = the world-shade map (see OnResolve).
    const uint32_t dev = g_device.load(std::memory_order_relaxed);
    if (dev != 0) {
      const uint32_t w1 = REX_LOAD_U32(dev + 0x480 + 1 * 24 + 4);
      const uint32_t w2 = REX_LOAD_U32(dev + 0x480 + 1 * 24 + 8);
      if ((w1 & 0x3Fu) == 22u && (w2 & 0x1FFFu) == 511u && ((w2 >> 13) & 0x1FFFu) == 511u) {
        g_ws_tex_addr.store(w1 & 0xFFFFF000u, std::memory_order_relaxed);
        // A receiver draw: its PS c5/c6/c7 are the rows it samples tf1
        // with (uv = c5.p / c6.p, ray = c7.p; the depth row's form
        // (0, ~-1.6e-4, 0, w) identifies a PS that really declares them).
        const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
        if (psb != 0) {
          float rr[12];
          for (int k = 0; k < 12; ++k) {
            rr[k] = LoadGuestF32(base, psb + ((5 + k / 4) * 4 + k % 4) * 4);
          }
          bool ok = std::fabs(rr[8]) < 1e-6f && std::fabs(rr[10]) < 1e-6f && rr[9] < 0.0f &&
                    rr[9] > -0.005f && std::fabs(rr[11]) < 16.0f;
          for (int k = 0; k < 12 && ok; ++k) {
            ok = std::isfinite(rr[k]) && std::fabs(rr[k]) < 1e6f;
          }
          if (ok) {
            std::memcpy(g_frame_ws_recv_rows, rr, sizeof(rr));
            g_frame_ws_recv_have = true;
          }
        }
      }
    }
  }
  // The game's world-shade pass (see g_frame_ws_keys): ortho bank, depth row
  // (VS c2) = (0, ~-1.6e-4, 0, w).
  if (flags2d == 0 && SceneEnabled() && bank_ortho) {
    const float dx = LoadGuestF32(base, bank + (2 * 4 + 0) * 4);
    const float dy = LoadGuestF32(base, bank + (2 * 4 + 1) * 4);
    const float dz = LoadGuestF32(base, bank + (2 * 4 + 2) * 4);
    if (std::fabs(dx) < 1e-6f && std::fabs(dz) < 1e-6f && dy < 0.0f && dy > -0.005f) {
      if (g_frame_ws_keys.size() < 8192) {
        g_frame_ws_keys.insert(key);
      }
      // The pass's own draw (see g_frame_ws_entries); indexed draws only.
      if (func == 0 && r7 != 0 && g_frame_ws_entries.size() < 8192) {
        std::vector<DrawEntry>& ents = g_frame_ws_entries[key];
        const DrawEntry e{r4, r5, r6, r7};
        bool dup = false;
        for (const DrawEntry& x : ents) {
          dup = dup || (x.prim == e.prim && x.base_vertex == e.base_vertex &&
                        x.start_index == e.start_index && x.index_count == e.index_count);
        }
        if (!dup && ents.size() < 512) {
          ents.push_back(e);
        }
      }
      for (int k = 0; k < 12; ++k) {
        g_frame_ws_rows[k] = LoadGuestF32(base, bank + uint32_t(k) * 4);
      }
      g_frame_ws_rows_have = true;
      ++g_frame_ws_draws;
    }
  }
  // The game's skate.Park world-shadow passes (see ParkWsDraw): ortho bank
  // whose rows are the park receivers' tf2 rows -- inner = PS c5 / c7 / c8,
  // outer = c5 / c7 scaled by c6 -- and the piece's world matrix in c4..c7.
  if (flags2d == 0 && SceneEnabled()) {
    const int tile = ParkWsPassTile(base, bank);
    {
      float vr[16];
      for (int k = 0; k < 16; ++k) {
        vr[k] = LoadGuestF32(base, bank + uint32_t(k) * 4);
      }
      if (tile >= 0 && g_frame_park_ws.size() < 8192) {
        ParkWsDraw d;
        d.key = key;
        d.tile = uint8_t(tile);
        for (int k = 0; k < 16; ++k) {
          d.world[k] = LoadGuestF32(base, bank + uint32_t(16 + k) * 4);  // c4..c7
        }
        if (std::fabs(d.world[15] - 1.0f) < 1e-4f) {
          g_frame_park_ws.push_back(d);
          std::memcpy(g_frame_park_ws_rows[tile], vr, sizeof(g_frame_park_ws_rows[tile]));
          g_frame_park_ws_rows_have[tile] = true;
        }
      }
    }
  }
  // The editor's preview object (see DrawItem::foreground): its world is
  // this draw's own VS world rows (row-vector, translation in the 4th).
  DrawSectMark(16);
  if (flags2d == 0 && SceneEnabled()) {
    const int wr_tag = BoundForegroundWorldReg(base);
    const int wr = wr_tag & 0xFF;
    // A foreground PROP (dynamicobject_foregroundPS / alphatest...): its
    // compact bank also carries the dynamicobject lighting rows the native
    // branch needs, which a park session (no regular props drawn) never
    // captures otherwise -- the preview prop then fell back to the legacy
    // shading, with no alpha cutout. Read off this build's ucode: sun c0,
    // exposure c4.x in both; alphatestdynamicobject_foregroundPS keeps the
    // material multiplier in c8.y (7) and ambient + bounce in c9 (43-45),
    // dynamicobject_foregroundPS one register lower -- multiplier c7.y (53),
    // ambient + bounce c8 (47-48) -- with its output alpha in c9.x (64).
    // (Reading c9 as ambient on the plain variant served (1, 0.5, 0.46):
    // the red-tinted preview prop.) No shadow terms: floor / biases 0.
    if (wr_tag >= 0 && (wr_tag & 0x100) != 0 && !g_dynobj_frame_done) {
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      if (psb != 0) {
        const auto C = [&](uint32_t reg, uint32_t comp) {
          return LoadGuestF32(base, psb + (reg * 4 + comp) * 4);
        };
        const float sx = C(0, 0), sy = C(0, 1), sz = C(0, 2);
        const float n2 = sx * sx + sy * sy + sz * sz;
        const uint32_t at = (wr_tag & 0x200) != 0 ? 1u : 0u;
        const float expo = C(4, 0), mult = C(7 + at, 1);
        if (n2 > 0.9f && n2 < 1.1f && expo > 0.1f && expo < 16.0f && mult > 0.0f &&
            mult < 16.0f) {
          g_dynobj_rows[0] = sx;
          g_dynobj_rows[1] = sy;
          g_dynobj_rows[2] = sz;
          g_dynobj_rows[3] = expo;
          for (uint32_t k = 0; k < 4; ++k) {
            g_dynobj_rows[4 + k] = C(8 + at, k);
          }
          g_dynobj_rows[8] = mult;
          g_dynobj_rows[9] = 0.0f;
          g_dynobj_rows[10] = g_dynobj_rows[11] = g_dynobj_rows[12] = 0.0f;
          g_dynobj_have = true;  // not frame_done: a real prop bank still wins
        }
      }
    }
    // Every park / prop _defaultPS draw's instance (see g_frame_default_inst).
    if (wr_tag < 0 && g_frame_default_inst.size() < 4096) {
      const int dr_tag = BoundParkDefaultWorldReg(base);
      const int dr = dr_tag & 0xFF;
      if (dr_tag >= 0) {
        DefaultInst di{};
        bool ok = true;
        for (uint32_t c = 0; c < 3; ++c) {
          di.t[c] = LoadGuestF32(base, bank + uint32_t((dr + 3) * 4 + int(c)) * 4);
          ok = ok && std::isfinite(di.t[c]);
        }
        if (ok && dr == 9 && (dr_tag & 0x100) == 0) {
          const uint32_t dev = g_device.load(std::memory_order_relaxed);
          if (dev != 0 && GuestReadableApprox(base, dev + 0x480)) {
            for (uint32_t k = 0; k < 6; ++k) {
              di.fetch[k] = REX_LOAD_U32(dev + 0x480 + (3 * 6 + k) * 4);
            }
            if ((di.fetch[0] & 3u) != 2u) {
              di.fetch[1] = 0;
            }
          }
        }
        if (ok) {
          std::vector<DefaultInst>& v = g_frame_default_inst[key];
          bool dup = false;
          for (const DefaultInst& x : v) {
            dup = dup || (std::fabs(x.t[0] - di.t[0]) < 1e-3f && std::fabs(x.t[1] - di.t[1]) < 1e-3f &&
                          std::fabs(x.t[2] - di.t[2]) < 1e-3f);
          }
          if (!dup && v.size() < 256) {
            v.push_back(di);
          }
        }
      }
    }
    // The selected piece's overlap highlight (DrawItem::highlight): the
    // _highlightPS variant, world in the VS rows, warning colour in PS c24.
    if (wr_tag < 0 && g_frame_hl_world.size() < 256) {
      const int hr_tag = BoundParkHighlightWorldReg(base);
      const int hr = hr_tag & 0xFF;
      if (hr_tag >= 0) {
        std::array<float, 16> w{};
        bool ok = true;
        for (int k = 0; k < 16 && ok; ++k) {
          w[size_t(k)] = LoadGuestF32(base, bank + uint32_t(hr * 4 + k) * 4);
          ok = std::isfinite(w[size_t(k)]) && std::fabs(w[size_t(k)]) < 1e5f;
        }
        ok = ok && std::fabs(w[3]) < 1e-4f && std::fabs(w[7]) < 1e-4f &&
             std::fabs(w[11]) < 1e-4f && std::fabs(w[15] - 1.0f) < 1e-4f;
        const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
        if (ok && psb != 0) {
          g_frame_hl_world[key] = w;
          for (uint32_t c = 0; c < 3; ++c) {
            const float v = LoadGuestF32(base, psb + (24 * 4 + c) * 4);
            g_frame_hl_rgb[c] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
          }
          if (func == 0 && r7 != 0) {
            std::vector<DrawEntry>& ents = g_frame_hl_entries[key];
            const DrawEntry e{r4, r5, r6, r7};
            bool dup = false;
            for (const DrawEntry& x : ents) {
              dup = dup || (x.prim == e.prim && x.base_vertex == e.base_vertex &&
                            x.start_index == e.start_index && x.index_count == e.index_count);
            }
            if (!dup && ents.size() < 64) {
              ents.push_back(e);
            }
          }
        }
      }
    }
    // The editor's selected piece (see DrawItem::xray): a park / prop
    // _defaultPS draw whose RB_DEPTHCONTROL is the x-ray pass -- stencil on,
    // zfunc ALWAYS (capture 1790895957, draws 1483/1484: 0x708773).
    if (wr_tag < 0 && g_frame_xray_world.size() < 256) {
      const uint32_t p2200 = g_rs_ptr_2200.load(std::memory_order_relaxed);
      uint32_t dc = 0;
      if (p2200 != 0 && GuestTryLoadU32(base, p2200, &dc) && (dc & 1u) != 0 &&
          ((dc >> 4) & 7u) == 7u) {
        const int xr_tag = BoundParkDefaultWorldReg(base);
        const int xr = xr_tag & 0xFF;
        if (xr_tag >= 0) {
          std::array<float, 16> w{};
          bool ok = true;
          for (int k = 0; k < 16 && ok; ++k) {
            w[size_t(k)] = LoadGuestF32(base, bank + uint32_t(xr * 4 + k) * 4);
            ok = std::isfinite(w[size_t(k)]) && std::fabs(w[size_t(k)]) < 1e5f;
          }
          ok = ok && std::fabs(w[3]) < 1e-4f && std::fabs(w[7]) < 1e-4f &&
               std::fabs(w[11]) < 1e-4f && std::fabs(w[15] - 1.0f) < 1e-4f;
          if (ok) {
            g_frame_xray_world[key] = w;
            if (func == 0 && r7 != 0) {
              std::vector<DrawEntry>& ents = g_frame_xray_entries[key];
              const DrawEntry e{r4, r5, r6, r7};
              bool dup = false;
              for (const DrawEntry& x : ents) {
                dup = dup || (x.prim == e.prim && x.base_vertex == e.base_vertex &&
                              x.start_index == e.start_index && x.index_count == e.index_count);
              }
              if (!dup && ents.size() < 64) {
                ents.push_back(e);
              }
            }
          }
        }
      }
    }
    if (wr_tag >= 0 && g_frame_fg_world.size() < 256) {
      std::array<float, 16> w{};
      bool ok = true;
      for (int k = 0; k < 16 && ok; ++k) {
        w[size_t(k)] = LoadGuestF32(base, bank + uint32_t(wr * 4 + k) * 4);
        ok = std::isfinite(w[size_t(k)]) && std::fabs(w[size_t(k)]) < 1e5f;
      }
      ok = ok && std::fabs(w[3]) < 1e-4f && std::fabs(w[7]) < 1e-4f && std::fabs(w[11]) < 1e-4f &&
           std::fabs(w[15] - 1.0f) < 1e-4f;
      if (ok) {
        g_frame_fg_world[key] = w;
        if (wr == 9 && (wr_tag & 0x100) == 0) {
          const uint32_t dev = g_device.load(std::memory_order_relaxed);
          if (dev != 0 && GuestReadableApprox(base, dev + 0x480)) {
            std::array<uint32_t, 6> fw{};
            for (uint32_t k = 0; k < 6; ++k) {
              fw[k] = REX_LOAD_U32(dev + 0x480 + (3 * 6 + k) * 4);
            }
            if ((fw[0] & 3u) == 2u && (fw[1] & 0xFFFFF000u) != 0) {
              g_frame_fg_fetch[key] = fw;
            }
          }
        }
        // Both game passes issue the same draws: keep each once.
        if (func == 0 && r7 != 0) {
          std::vector<DrawEntry>& ents = g_frame_fg_entries[key];
          const DrawEntry e{r4, r5, r6, r7};
          bool dup = false;
          for (const DrawEntry& x : ents) {
            dup = dup || (x.prim == e.prim && x.base_vertex == e.base_vertex &&
                          x.start_index == e.start_index && x.index_count == e.index_count);
          }
          if (!dup && ents.size() < 64) {
            ents.push_back(e);
          }
        }
      }
    }
  }
  // The world's skate.Park receivers (see g_frame_dmo_keys): a main-pass draw
  // through a `_skatepark` world pixel shader with the park map (k_16_16
  // 1024x512) at tf2. Their banks share the park pieces' c5..c8 / c10
  // sampling rows, so they latch those too (a park can be on screen with no
  // park-piece receiver drawn), plus their own floors c11 / c12 and cascade
  // bias c9.x.
  DrawSectMark(17);
  if (flags2d == 0 && SceneEnabled()) {
    const uint32_t dev = g_device.load(std::memory_order_relaxed);
    const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
    if (dev != 0 && psb != 0 && GuestReadableApprox(base, dev + 0x480)) {
      const uint32_t w1 = REX_LOAD_U32(dev + 0x480 + 2 * 24 + 4);
      const uint32_t w2 = REX_LOAD_U32(dev + 0x480 + 2 * 24 + 8);
      if ((w1 & 0x3Fu) == 25u && (w2 & 0x1FFFu) == 1023u && ((w2 >> 13) & 0x1FFFu) == 511u &&
          BoundSkateparkWorldPs(base)) {
        const auto C = [&](uint32_t reg, uint32_t comp) {
          return LoadGuestF32(base, psb + (reg * 4 + comp) * 4);
        };
        float wr[20];
        bool wok = true;
        for (int k = 0; k < 16; ++k) {
          wr[k] = C(5u + uint32_t(k / 4), uint32_t(k % 4));
          wok = wok && std::isfinite(wr[k]) && std::fabs(wr[k]) < 1e6f;
        }
        for (int k = 0; k < 4; ++k) {
          wr[16 + k] = C(10, uint32_t(k));
          wok = wok && std::isfinite(wr[16 + k]);
        }
        wok = wok && std::fabs(wr[12]) < 1e-6f && std::fabs(wr[14]) < 1e-6f && wr[13] < 0.0f &&
              wr[18] > 0.0f;
        float dr[12] = {};
        for (int k = 0; k < 3; ++k) {
          dr[k] = C(11, uint32_t(k));
          dr[4 + k] = C(12, uint32_t(k));
          wok = wok && dr[k] >= 0.0f && dr[k] <= 1.0f && dr[4 + k] >= 0.0f && dr[4 + k] <= 1.0f;
        }
        dr[8] = C(9, 0);
        wok = wok && std::isfinite(dr[8]) && std::fabs(dr[8]) < 1.0f;
        if (wok) {
          if (g_frame_dmo_keys.size() < 8192) {
            g_frame_dmo_keys.insert(key);
          }
          g_park_ws_tex_addr.store(w1 & 0xFFFFF000u, std::memory_order_relaxed);
          std::memcpy(g_park_ws_recv, wr, sizeof(wr));
          g_park_ws_recv_have = true;
          std::memcpy(g_park_dmo_rows, dr, sizeof(dr));
          if (!g_park_dmo_have) {
            REXLOG_INFO(
                "native-scene: skate.Park world receivers captured tf2={:08X} "
                "floorA=({:.4f},{:.4f},{:.4f}) floorB=({:.4f},{:.4f},{:.4f}) bias={:.4f}",
                w1 & 0xFFFFF000u, dr[0], dr[1], dr[2], dr[4], dr[5], dr[6], dr[8]);
          }
          g_park_dmo_have = true;
        }
      }
    }
  }
  // Record this draw's texture fetch slots 3+4 (lightmap + diffuse on the
  // environment families) for the frame-end streamed-artwork diffuse
  // override (see g_frame_draw_fetch). Last writer wins: the z-prepass
  // draws these buffers first with stale bindings, the main pass overwrites.
  DrawSectMark(18);
  if (flags2d == 0 && SceneEnabled()) {
    const uint32_t device = g_device.load(std::memory_order_relaxed);
    if (device != 0 && g_frame_draw_fetch.size() < 4096) {
      // Only AdoptDrawFetchOverrides reads the map, and only with
      // draw_fetch_adopt on: skip the per-draw node
      // allocation + 12 fetch-word copy otherwise (on by default on retail).
      // Perspective draws only: the map serves visible world items, whose
      // main pass is the authoritative writer, and the ortho shadow-caster
      // passes (a large share of all draws) carry no texture binding a
      // visible item should adopt.
      if (REXCVAR_GET(skate3_native_render_scene_draw_fetch_adopt) && !bank_ortho) {
        auto& w = g_frame_draw_fetch[key];
        for (int i = 0; i < 12; ++i) {
          w[size_t(i)] = REX_LOAD_U32(device + 0x480 + 3 * 24 + uint32_t(i) * 4);
        }
      }
      // CAS skin custom graphic (see g_frame_skin_graphic): the
      // cacstamp_skin main pass is the draw whose tf6 CLAMPS TO BORDER on
      // both axes (fetch dword0 clamp_x = clamp_y = 6, every captured skin
      // draw; no other character tf6 is bound that way). Perspective banks
      // only (the caster passes bind no PS textures of their own).
      const uint32_t t6w0 = REX_LOAD_U32(device + 0x480 + 6 * 24);
      if (((t6w0 >> 10) & 0x3Fu) == 0x36u && !bank_ortho &&
          g_frame_skin_graphic.size() < 1024) {
        SkinGraphic& sg = g_frame_skin_graphic[key];
        for (int k = 0; k < 2; ++k) {
          for (int a = 0; a < 3; ++a) {
            sg.cg[k * 3 + a] =
                LoadGuestF32(base, bank + ((190u + uint32_t(k)) * 4 + uint32_t(a)) * 4);
          }
        }
        for (int k = 0; k < 6; ++k) {
          sg.tf6[k] = REX_LOAD_U32(device + 0x480 + 6 * 24 + uint32_t(k) * 4);
        }
      }
      // Face injury layer (see g_frame_face_injury): the face's main-pass
      // compiles bind the skater's injury map at tf6 and scale it by c22.z.
      const uint32_t csf = BoundCharShaderFlags(base);
      // Retail's _nis compiles (front-end / replay editor) insert
      // g_CSMSelfBiasNIS at c9, so every row from g_vLightDir up sits ONE
      // higher than in the default compile (ctab_diff: cac_face_nisPS
      // i_params c23, m_params c15..22, g_charattributes c12..14; the same
      // for cac_cloth*_nis / cacstamp_skin_nis). The dev _nis compiles have
      // no such row, which is what these fixed row numbers were read from.
      const uint32_t nis_shift =
          (csf & (kCsFaceNis | kCsClothNis | kCsClothRopaNis | kCsStampSkinNis)) != 0 ? 1u
                                                                                    : 0u;
      if (!bank_ortho && g_frame_face_injury.size() < 256 &&
          (csf & (kCsFaceDefault | kCsFaceNis)) != 0) {
        const uint32_t fpsb = g_ps_bank.load(std::memory_order_relaxed);
        if (fpsb != 0) {
          FaceInjury& fi = g_frame_face_injury[key];
          for (int k = 0; k < 6; ++k) {
            fi.tf6[k] = REX_LOAD_U32(device + 0x480 + 6 * 24 + uint32_t(k) * 4);
          }
          fi.amount = LoadGuestF32(base, fpsb + ((22 + nis_shift) * 4 + 2) * 4);
        }
      }
      // CAC cloth dirt / skin injury layer (see g_frame_char_layer).
      if (!bank_ortho && g_frame_char_layer.size() < 512) {
        const bool cloth = (csf & (kCsClothDefault | kCsClothNis | kCsClothRopaDefault |
                                   kCsClothRopaNis)) != 0;
        // Printed (stamped) garments: cacstamp_cloth[_ropa]_defaultPS binds
        // the STAMP at tf5 (second texcoord) and the dirt map at tf6 (10/
        // 28-33, the same lerp x c22.y as cac_cloth).
        const bool stamp_cloth =
            !cloth && (csf & (kCsStampClothDefault | kCsStampClothRopa)) != 0;
        const bool skin = !cloth && !stamp_cloth &&
                          (csf & (kCsStampSkinDefault | kCsStampSkinNis)) != 0;
        const uint32_t lpsb = g_ps_bank.load(std::memory_order_relaxed);
        if ((cloth || stamp_cloth || skin) && lpsb != 0) {
          CharLayer& cl = g_frame_char_layer[key];
          const uint32_t slot = cloth ? 5u : stamp_cloth ? 6u : 7u;
          for (int k = 0; k < 6; ++k) {
            cl.words[k] = REX_LOAD_U32(device + 0x480 + slot * 24 + uint32_t(k) * 4);
          }
          cl.amount =
              LoadGuestF32(base, lpsb + ((22 + nis_shift) * 4 + (skin ? 2u : 1u)) * 4);
        }
      }
      // CAS subsurface scattering (see g_frame_skin_sss): the unwrap draw
      // renders into the 128x128 texture-space target (viewport x scale 64,
      // perspective bank); the main pass is the last full-view draw.
      const uint32_t p2100 = g_rs_ptr_2100.load(std::memory_order_relaxed);
      const uint32_t psb = g_ps_bank.load(std::memory_order_relaxed);
      if (p2100 != 0 && !bank_ortho && g_frame_skin_sss.size() < 1024) {
        const float rt_w = 2.0f * std::fabs(LoadGuestF32(base, p2100 + 0xF * 4));
        if (std::fabs(rt_w - 128.0f) < 1.0f) {
          g_frame_skin_sss[key].unwrap = true;
        } else if (rt_w >= 512.0f) {
          const auto it = g_frame_skin_sss.find(key);
          if (it != g_frame_skin_sss.end() && it->second.unwrap) {
            SkinSssRecord& r = it->second;
            r.main = true;
            for (int k = 0; k < 6; ++k) {
              r.mask7[k] = REX_LOAD_U32(device + 0x480 + 7 * 24 + uint32_t(k) * 4);
              r.mask8[k] = REX_LOAD_U32(device + 0x480 + 8 * 24 + uint32_t(k) * 4);
              r.mask6[k] = REX_LOAD_U32(device + 0x480 + 6 * 24 + uint32_t(k) * 4);
            }
            r.amount =
                psb != 0 ? LoadGuestF32(base, psb + ((21 + nis_shift) * 4 + 0) * 4) : 0.0f;
            r.amount12 =
                psb != 0 ? LoadGuestF32(base, psb + ((12 + nis_shift) * 4 + 0) * 4) : 0.0f;
          }
        }
      }
    }
  }
  // Character-lighting refresh: items already fixed up re-read their PS rows
  // from later draws with the same buffers (commit-on-success, so the
  // main-pass draw's rows win over the stale caster-pass bank). CLONES share
  // (ib,vb) with per-instance constants (livingworld stamp tints, hair
  // colors, SH rows); refreshing every registered item on every matching
  // draw collapsed all clones onto the LAST instance's rows (the teal-vest
  // twins). Pair draw to instance by the bone palette: the draw's VS bank
  // holds the instance's palette, which matches exactly one item's captured
  // bones.
  DrawSectMark(19);
  auto refresh = g_frame_char_refresh_keys.contains(key)
                     ? g_frame_char_refresh.equal_range(key)
                     : std::make_pair(g_frame_char_refresh.end(), g_frame_char_refresh.end());
  if (refresh.first != refresh.second) {
    const uint32_t pb = BankPaletteBase(base, bank);
    float row0[4] = {};
    if (pb != 0) {
      for (int i = 0; i < 4; ++i) {
        row0[i] = LoadGuestF32(base, bank + (pb * 4 + uint32_t(i)) * 4);
      }
    }
    // How many distinct items this (ib,vb) key covers this frame. Rigid
    // sub-meshes (board deck/trucks/wheels - anything that took the ROPA
    // "rigid" accept path) have no bone palette, so the match check below
    // is skipped for them entirely. With a single owner that's fine: this
    // buffer pair can only belong to that one item, so any draw using it
    // is trivially "ours". With MULTIPLE owners - e.g. two skaters riding
    // the same stock board model in multiplayer, whose deck/trucks/wheels
    // share the identical GPU mesh and therefore the identical key - it is
    // NOT fine: without this guard, every draw of EITHER player's board
    // re-stamps BOTH players' items from whatever PS bank happens to be
    // bound at that instant, so one player's momentarily-misclassified
    // bank (see the nis_kind / c13.x=0.08 note in CaptureCharLighting)
    // gets written onto the other player's board. That cross-instance
    // bleed is what made remote skaters' boards (and by extension the
    // whole skater, since the alpha row rides the same commit) render
    // transparent in multiplayer while the native renderer, which does
    // none of this heuristic re-derivation, stayed correct.
    size_t owner_count = 0;
    for (auto it = refresh.first; it != refresh.second; ++it) {
      if (it->second < g_frame_dynitems.size()) ++owner_count;
    }
    // Record this draw's own rows against its own palette position (see
    // g_frame_char_draw_rows). Perspective banks only: the caster passes'
    // shadowPS uploads no PS constants, so their bank is a previous draw's.
    if (pb != 0 && owner_count != 0 && !bank_ortho &&
        g_frame_char_draw_rows.size() < 4096) {
      size_t first = SIZE_MAX;
      for (auto it = refresh.first; it != refresh.second; ++it) {
        if (it->second < g_frame_dynitems.size()) {
          first = it->second;
          break;
        }
      }
      DrawItem probe = g_frame_dynitems[first];
      std::memset(probe.char_rows, 0, sizeof(probe.char_rows));
      CaptureCharLighting(base, probe);
      if (probe.char_rows[14 * 4 + 1] > 0.0f) {
        CharDrawRows cd;
        for (int k = 0; k < 2; ++k) {
          for (int a = 0; a < 3; ++a) {
            cd.t[k][a] = LoadGuestF32(
                base, bank + ((pb + uint32_t(k) + uint32_t(a)) * 4 + 3) * 4);
          }
        }
        std::memcpy(cd.rows, probe.char_rows, sizeof(cd.rows));
        cd.seq = ++g_frame_char_draw_seq;
        g_frame_char_draw_rows.emplace(key, cd);
      }
    }
    for (auto it = refresh.first; it != refresh.second; ++it) {
      if (it->second >= g_frame_dynitems.size()) continue;
      DrawItem& d = g_frame_dynitems[it->second];
      if (d.bones.size() >= 4 && pb != 0) {
        bool match = true;
        for (int i = 0; i < 4 && match; ++i) {
          // The refined palette can sit one register past BankPaletteBase
          // (cloth/morph layouts); compare against both candidate rows.
          match = std::fabs(d.bones[size_t(i)] - row0[i]) < 1e-4f;
        }
        if (!match) {
          bool match1 = true;
          for (int i = 0; i < 4 && match1; ++i) {
            match1 = std::fabs(d.bones[size_t(i)] -
                               LoadGuestF32(base, bank + ((pb + 1) * 4 + uint32_t(i)) * 4)) <
                     1e-4f;
          }
          match = match1;
        }
        if (!match) continue;
      } else if (owner_count > 1) {
        // Rigid item with no bone palette to verify against, sharing this
        // key with at least one other instance: we cannot prove this draw
        // is really this item's. Don't guess - keep the rows it last
        // validated rather than risk stamping another skater's data onto
        // it.
        continue;
      }
      CaptureCharLighting(base, d);
    }
  }
  DrawSectMark(20);
  if (!g_frame_pending_keys.contains(key)) {
    return;
  }
  auto range = g_frame_pending_by_buffers.equal_range(key);
  if (range.first == range.second) {
    return;
  }
  // This draw's constants belong to the OLDEST still-PENDING item with
  // these buffers (clones share mesh assets; the deferred list draws in
  // submit order, so FIFO one-shot pairing keeps clones' palettes apart).
  // With none pending, the oldest CASTER-sourced item instead gets a
  // REFRESH from this later draw: palettes captured from the ortho
  // caster-cascade banks carry stale fine animation (vehicle wheel spin,
  // ~40 ms behind in bursts); publishing them made the car's pose stream
  // jump, tripping the smoothing ring's discontinuity reset (the traffic
  // judder). The refresh entry is only retired by a perspective-bank
  // (z/main-pass) capture.
  auto oldest = range.second;
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second >= g_frame_dynitems.size() ||
        !g_frame_dynitems[it->second].pending) {
      continue;
    }
    if (oldest == range.second || it->second < oldest->second) {
      oldest = it;
    }
  }
  bool caster_refresh = false;
  if (oldest == range.second) {
    if (REXCVAR_GET(skate3_native_render_scene_caster_refresh_all)) {
      // Pick the refresh target by PALETTE IDENTITY, across every
      // caster-sourced skinned candidate, including skinned-published ROPA
      // garments. Two structural gaps hid here: (1) ropa items were excluded
      // outright, so the player's tee/hair kept their ortho-bank captures all
      // frame; the caster banks' fine animation runs ticks stale in bursts
      // (measured 0.47-0.60 m translation deltas), so those pieces published
      // poses visibly behind the body (the high-fps hair wedge / vanishing
      // garment: head-joint pieces 0.3-0.4 m behind the same frame's own
      // banks while the body matched bit-exact); (2) oldest-only selection
      // probed the wrong clone for
      // mesh-sharing NPCs, the <1.5 m guard refused, and the right clone
      // never refreshed (the NPC blink). Rank by the new bank's bone-0
      // translation; the commit block below re-guards with the full capture.
      const uint32_t pb_sel = BankPaletteBase(base, bank);
      if (pb_sel == 0) {
        return;
      }
      const float nt[3] = {
          LoadGuestF32(base, bank + (pb_sel * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((pb_sel + 1) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((pb_sel + 2) * 4 + 3) * 4)};
      float best_d2 = 2.25f;  // same 1.5 m identity bound as the commit guard
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.caster_bank || !c.skinned || c.bones.size() < 12) {
          continue;  // rigid-resolved ropa has no palette; world capture owns it
        }
        const float dx = c.bones[3] - nt[0];
        const float dy = c.bones[7] - nt[1];
        const float dz = c.bones[11] - nt[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
          best_d2 = d2;
          oldest = it;
        }
      }
    } else {
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.caster_bank || !c.skinned || c.ropa) {
          continue;
        }
        if (oldest == range.second || it->second < oldest->second) {
          oldest = it;
        }
      }
    }
    if (oldest == range.second) {
      return;
    }
    caster_refresh = true;
  }
  // Rigid clone disambiguation: same-mesh piece clones share these buffers,
  // and under batched dispatch (park-editor venues: every capture defers,
  // 100+ pending rigids per frame) the FIFO capture order is NOT the draw
  // order; pairing a draw with the wrong clone swaps piece worlds frame to
  // frame (pieces visibly shuffle and flip 180 degrees). Prefer the pending
  // clone whose LAST PUBLISHED world sits nearest this draw's world; FIFO
  // stays the fallback for first-sight clones with no history. Movables stay
  // matchable: they drift well under the 0.5 m bound per frame and the cache
  // re-stamps on every resolved draw.
  if (!caster_refresh && oldest->second < g_frame_dynitems.size() &&
      !g_frame_dynitems[oldest->second].skinned) {
    float w[16];
    if (!BankIsAuxPerspective(base, bank) && BankRigidWorld(base, bank, w)) {
      float best_d2 = 0.25f;
      auto best = range.second;
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second >= g_frame_dynitems.size()) {
          continue;
        }
        const DrawItem& c = g_frame_dynitems[it->second];
        if (!c.pending || c.skinned || c.ctx == 0) {
          continue;
        }
        const auto cit = g_rigid_world_cache.find(c.ctx);
        if (cit == g_rigid_world_cache.end() || cit->second.mesh != c.mesh) {
          continue;
        }
        const float dx = cit->second.world[12] - w[12];
        const float dy = cit->second.world[13] - w[13];
        const float dz = cit->second.world[14] - w[14];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
          best_d2 = d2;
          best = it;
        }
      }
      if (best != range.second) {
        oldest = best;
      }
    }
  }
  DrawItem& d = g_frame_dynitems[oldest->second];
  if (caster_refresh) {
    // Refresh of a caster-sourced capture from a later (ideally main-pass)
    // draw. Re-capture into a PROBE and require the fresher palette to be
    // THIS entity's pose: same-mesh clones share these buffers, and a far
    // twin's later draw otherwise refreshes the near car onto the twin's
    // position (bone-signal recorded 151 m palette teleports; the near
    // car renders across the map, i.e. invisible where it stands).
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    // Reused scratch (guest render thread only): copy-assign keeps the
    // vectors' capacity, so a refresh costs no heap traffic.
    static DrawItem s_probe;
    DrawItem& probe = s_probe;
    probe = d;
    if (!CaptureSkinnedState(base, bank, palette_base, probe)) {
      return;
    }
    if (probe.bones.size() < 12 || probe.bones.size() != d.bones.size()) {
      return;
    }
    const float dx = probe.bones[3] - d.bones[3];
    const float dy = probe.bones[7] - d.bones[7];
    const float dz = probe.bones[11] - d.bones[11];
    if (dx * dx + dy * dy + dz * dz > 2.25f) {
      return;  // > 1.5 m: a twin's draw, not this entity's
    }
    probe.caster_bank = bank_ortho;
    probe.pending = false;
    probe.dbg_src = 2;
    std::swap(d, probe);
    if (d.char_family != 0) {
      RegisterCharRefresh(key, oldest->second);
    }
    if (!d.caster_bank) {
      g_frame_pending_by_buffers.erase(oldest);
    }
    return;
  }
  if (d.skinned) {
    // Locate the palette for this draw's layout; a bank without plausible
    // bone rows (parameter blocks, camera rows) must not be consumed; that
    // was the source of screen-wide stretched characters.
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    if (!CaptureSkinnedState(base, bank, palette_base, d)) {
      return;  // stale bank refused: wait for a later draw with these buffers
    }
    d.caster_bank = bank_ortho;
  } else {
    // Deferred rigid prop: the instance's own matrix disambiguates clones
    // (they share these buffers, so the bank can be another clone's; see
    // ReadCtxInstanceWorld), while a same-spot bank keeps winning for its
    // live physics rotation. Bank layout: pre-pass c4..c7, main-pass
    // c8..c11. Neither plausible -> wait for a later draw with these
    // buffers. Aux perspective passes (portrait RTTs) stage off-map
    // portrait-stage worlds; those never serve.
    float bank_world[16];
    const bool fb_bank = !BankIsAuxPerspective(base, bank) &&
                         BankRigidWorld(base, bank, bank_world);
    const bool fb_ctx = ReadCtxInstanceWorld(base, d.ctx, d.world);
    if (fb_bank && fb_ctx) {
      const float dx = bank_world[12] - d.world[12];
      const float dy = bank_world[13] - d.world[13];
      const float dz = bank_world[14] - d.world[14];
      if (dx * dx + dy * dy + dz * dz <= 0.0625f) {
        std::memcpy(d.world, bank_world, sizeof(bank_world));
      } else {
        g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (fb_bank) {
      std::memcpy(d.world, bank_world, sizeof(bank_world));
    } else if (fb_ctx) {
      g_rigid_ctx_world.fetch_add(1, std::memory_order_relaxed);
    } else {
      return;
    }
    d.caster_bank = false;
    if (d.ctx != 0) {
      RigidWorldCache& rc = g_rigid_world_cache[d.ctx];
      rc.mesh = d.mesh;
      std::memcpy(rc.world, d.world, sizeof(rc.world));
      rc.frame = g_guest_frame;
    }
  }
  d.pending = false;
  d.dbg_src = 2;
  if (d.char_family != 0) {
    RegisterCharRefresh(key, oldest->second);
  }
  if (!d.caster_bank) {
    g_frame_pending_by_buffers.erase(oldest);
  }
}

// ---- Camera re-timing (skate3_native_render_scene_smooth_camera) ----------
// The guest publishes camera poses on its own sim tick (~170-240 Hz,
// irregular, measured cam[chg/rep] streaks of 10 rendered frames on one
// pose at 400 fps) while the render loop free-runs: several rendered frames
// reuse one pose, then the camera steps; the world "judders / skips to
// catch up" while panning. The native renderer owns the view transform, so
// each rendered frame samples a TIME-CORRECT pose instead: interpolate
// between the last two distinct guest poses at (now - one sim interval).
// A few ms of camera latency, no overshoot; teleports/cuts snap.

// Standard 3x3 <-> quaternion pair (self-consistent on the view's row-vector
// rotation; only round-tripping matters).
void QuatFromView(const float view[16], float q[4]) {
  const float m00 = view[0], m01 = view[1], m02 = view[2];
  const float m10 = view[4], m11 = view[5], m12 = view[6];
  const float m20 = view[8], m21 = view[9], m22 = view[10];
  const float tr = m00 + m11 + m22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q[3] = 0.25f * s;
    q[0] = (m21 - m12) / s;
    q[1] = (m02 - m20) / s;
    q[2] = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q[3] = (m21 - m12) / s;
    q[0] = 0.25f * s;
    q[1] = (m01 + m10) / s;
    q[2] = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q[3] = (m02 - m20) / s;
    q[0] = (m01 + m10) / s;
    q[1] = 0.25f * s;
    q[2] = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q[3] = (m10 - m01) / s;
    q[0] = (m02 + m20) / s;
    q[1] = (m12 + m21) / s;
    q[2] = 0.25f * s;
  }
}

void ViewRotFromQuat(const float q[4], float view[16]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  view[0] = 1 - 2 * (y * y + z * z);
  view[1] = 2 * (x * y - z * w);
  view[2] = 2 * (x * z + y * w);
  view[4] = 2 * (x * y + z * w);
  view[5] = 1 - 2 * (x * x + z * z);
  view[6] = 2 * (y * z - x * w);
  view[8] = 2 * (x * z - y * w);
  view[9] = 2 * (y * z + x * w);
  view[10] = 1 - 2 * (x * x + y * y);
}

void QuatSlerp(const float a[4], const float b_in[4], float t, float out[4]) {
  float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0f) {
    for (float& v : b) v = -v;
    dot = -dot;
  }
  float wa = 1.0f - t, wb = t;
  if (dot < 0.9995f) {
    const float theta = std::acos(std::min(dot, 1.0f));
    const float s = std::sin(theta);
    wa = std::sin((1.0f - t) * theta) / s;
    wb = std::sin(t * theta) / s;
  }
  float n2 = 0.0f;
  for (int i = 0; i < 4; ++i) {
    out[i] = wa * a[i] + wb * b[i];
    n2 += out[i] * out[i];
  }
  const float inv = n2 > 1e-12f ? 1.0f / std::sqrt(n2) : 1.0f;
  for (int i = 0; i < 4; ++i) out[i] *= inv;
}

struct CamPose {
  double t = 0.0;
  float q[4] = {0, 0, 0, 1};  // view rotation (rows 0..2)
  float c[3] = {};            // camera world position
};

// Shared playback clock (guest render thread): set by SmoothCamera each
// frame it produces a re-timed pose; dynamic items (bone palettes, rigid
// worlds) interpolate at the SAME time so the skater/NPCs/props stay in
// phase with the smoothed camera; smoothing only the camera concentrated
// all the sim stepping on the skater ("he definitively judders now").
double g_smooth_play = 0.0;
bool g_smooth_active = false;
// Last pose SmoothCamera produced (guest render thread), read by the
// synthetic-pan probe (mode 3) to measure reconstruction error against the
// known ideal pose.
CamPose g_smooth_pose;

// Synthetic-pan probe state/helpers: native/skate3_native_diag.h (the
// engage/step orchestration stays in BuildFrameScene below; it reads the
// live camera and scene locals).

// ---- Off-screen retention (edge-of-view teardown fix) ----------------------
// The game culls its submission stream to ITS camera pose; the native frame
// renders with the re-timed (smoothed) pose, which TRAILS the guest pose by
// up to the boxcar window (~25-50 ms). During a pan the guest stops
// submitting items that left its (leading) frustum while the (trailing)
// rendered view still contains them; world geometry visibly tore down right
// at the screen edges (the emulated frame can never show this: it IS the
// guest frame). Recently seen statics stay retained here and are re-appended
// while the RAW guest frustum can NOT see them (i.e. the cull was
// view-driven). An item the guest frustum CAN see but did not submit was
// really removed (LOD switch, despawn, streaming) and drops immediately;
// retaining those would z-fight the replacement LOD. Statics only: dynamic
// poses go stale the moment they stop being captured. Guest render thread
// only (BuildFrameScene); flips request a clear via the atomic.
// Hot/cold split: the per-frame pass (one entry per published static, then
// one per retained entry) reads only this small record; the ~1 KB item copy
// lives in g_retained_pool and is touched only when it changes or is
// re-appended. (A node map of full items cost a heap node per entry and a
// cache miss into every 1 KB copy per frame.)
struct RetainedItem {
  uint64_t last_seen = 0;  // g_guest_frame of the last live submission
  uint64_t fingerprint = 0;
  float world[16];
  float bbox_min[3];
  float bbox_max[3];
  uint32_t slot = 0;  // g_retained_pool index
};
FlatMap<uint64_t, RetainedItem> g_retained_items;
std::vector<DrawItem> g_retained_pool;
std::vector<uint32_t> g_retained_free;

void ClearRetainedItems() {
  g_retained_items.clear();
  g_retained_pool.clear();
  g_retained_free.clear();
}
// Dynamic sibling for traffic VEHICLES (char families 6/7): a vehicle
// passing CLOSE leaves the guest frustum while the trailing rendered pose
// still shows it; its captures stop and it tore down in view (and the
// frames just before that alternated perspective / stale caster-bank
// captures; see the caster ingest guard in InterpolateDynamicItems).
// Entries hold the last live NON-caster capture; matched by mesh +
// bone-derived position (clones). Guest render thread only. Characters/
// NPCs are deliberately excluded: their ropa garments cannot be retained
// coherently (a body without its shirt is worse than the teardown).
struct DynRetained {
  DrawItem item;
  uint64_t last_seen = 0;
  float pos[3] = {};  // plausible-bone average (world)
  float half = 2.0f;  // bbox-diagonal half-extent for the frustum test
};
std::vector<DynRetained> g_dyn_retained;

// True when all 8 world-space corners fall outside one clip plane of `vp`
// (row-vector view*proj). `margin` scales the tested frustum: < 1 shrinks
// it (bounds poking just inside an edge still count as outside; the
// game's own cull volumes are tighter than a mesh bbox), > 1 widens it
// (only clearly-outside counts).
bool CornersOutsideFrustum(const float (&corners)[8][3], const float vp[16],
                           float margin) {
  int outside[6] = {};
  for (int c = 0; c < 8; ++c) {
    const float* p = corners[c];
    float clip[4];
    for (int k = 0; k < 4; ++k) {
      clip[k] = p[0] * vp[0 * 4 + k] + p[1] * vp[1 * 4 + k] + p[2] * vp[2 * 4 + k] +
                vp[3 * 4 + k];
    }
    // D3D clip volume: -w <= x <= w, -w <= y <= w, 0 <= z <= w. The game's
    // negative projection x-scale only swaps left/right; the tests are
    // symmetric. Corners behind the camera land in z < 0.
    const float m = margin * clip[3];
    if (clip[0] < -m) ++outside[0];
    if (clip[0] > m) ++outside[1];
    if (clip[1] < -m) ++outside[2];
    if (clip[1] > m) ++outside[3];
    if (clip[2] < 0.0f) ++outside[4];
    if (clip[2] > clip[3]) ++outside[5];
  }
  for (int k = 0; k < 6; ++k) {
    if (outside[k] == 8) {
      return true;
    }
  }
  return false;
}

// The item's world-space bbox against the frustum (statics: bbox is
// mesh-local, world transforms it; fmt-57 absolute geometry carries an
// identity world with world-space bounds).
bool BoundsOutsideFrustum(const float world[16], const float bbox_min[3],
                          const float bbox_max[3], const float vp[16], float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    const float l[3] = {c & 1 ? bbox_max[0] : bbox_min[0],
                        c & 2 ? bbox_max[1] : bbox_min[1],
                        c & 4 ? bbox_max[2] : bbox_min[2]};
    const float* w = world;
    for (int k = 0; k < 3; ++k) {
      corners[c][k] = l[0] * w[0 * 4 + k] + l[1] * w[1 * 4 + k] +
                      l[2] * w[2 * 4 + k] + w[3 * 4 + k];
    }
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

bool ItemOutsideFrustum(const DrawItem& it, const float vp[16], float margin) {
  return BoundsOutsideFrustum(it.world, it.bbox_min, it.bbox_max, vp, margin);
}

// Axis-aligned cube of half-extent `half` around `center` against the
// frustum, the skinned-item variant (a palette's world position lives in
// its bone translations, not the identity item world).
bool BoxOutsideFrustum(const float center[3], float half, const float vp[16],
                       float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    corners[c][0] = center[0] + (c & 1 ? half : -half);
    corners[c][1] = center[1] + (c & 2 ? half : -half);
    corners[c][2] = center[2] + (c & 4 ? half : -half);
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

// SynPanAngleDeg / SynPanView: native/skate3_native_diagnostics.cpp.

// ---- High-rate camera sampler ---------------------------------------------
// Pose samples taken once per RENDERED frame alias as soon as the render
// rate drops near/below the sim's camera tick (~200 Hz): at the 140 fps
// pacing cap some frames contain one sim step of rotation and some two, all
// timestamped on the even frame grid; the smoother then faithfully renders
// a 140-vs-200 Hz beat as CONSTANT judder. This thread samples the guest
// ViewCamera at ~1 kHz with precise host timestamps, so the reconstruction
// signal is always finer than the sim tick regardless of render cadence.
// Torn reads (the sim thread writes the matrix concurrently) are rejected
// by a double-read compare.
struct RawCamSample {
  double t;
  float view[16];
  float proj[16];
};
std::atomic<uint32_t> g_sampler_viewcam{0};  // published by BuildFrameScene
std::mutex g_cam_samples_mutex;
std::vector<RawCamSample> g_cam_samples;  // pending; drained by SmoothCamera
std::atomic<bool> g_cam_sampler_started{false};
std::atomic<uint64_t> g_cam_sampler_pushes{0};  // telemetry

void CamSamplerLoop() {
  float last_view[16] = {};
  double syn_next_emit = 0.0;
  int syn_cadence_i = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    // Synthetic pan mode 3: synthesize guest-like pose samples instead of
    // reading the guest camera, ~200 Hz with a deliberately irregular
    // cadence (the real sim tick is irregular; the smoother must cope). The
    // downstream path (ring, period EMA, playback clock, slerp) runs
    // unmodified on them, and its output is compared numerically against
    // the ideal pose in BuildFrameScene.
    if (g_synpan_active.load(std::memory_order_acquire) == 3) {
      static constexpr double kCadenceMs[6] = {4.0, 5.0, 6.0, 4.0, 6.0, 5.0};
      const double now =
          std::chrono::duration<double>(PerfClock::now().time_since_epoch()).count();
      if (syn_next_emit == 0.0) {
        syn_next_emit = now;
      }
      if (now < syn_next_emit) {
        continue;
      }
      syn_next_emit += kCadenceMs[syn_cadence_i++ % 6] * 1e-3;
      if (syn_next_emit < now) {
        syn_next_emit = now;  // fell behind (OS scheduling hitch): re-anchor
      }
      RawCamSample s;
      s.t = now;
      {
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
        const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
        SynPanView(SynPanAngleDeg((now - g_synpan_t0) * rate, amp), s.view);
        std::memcpy(s.proj, g_synpan_proj0, sizeof(s.proj));
      }
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      if (g_cam_samples.size() < 256) {
        g_cam_samples.push_back(s);
        g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    syn_next_emit = 0.0;
    const uint32_t vc = g_sampler_viewcam.load(std::memory_order_relaxed);
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (vc == 0 || base == nullptr) {
      continue;
    }
    // view at +0x20, proj at +0x60: one 128-byte guarded copy covers both.
    uint32_t raw_a[32], raw_b[32];
    if (!GuestTryCopy(raw_a, base + vc + 0x20, sizeof(raw_a)) ||
        !GuestTryCopy(raw_b, base + vc + 0x20, sizeof(raw_b)) ||
        std::memcmp(raw_a, raw_b, sizeof(raw_a)) != 0) {
      continue;  // unreadable or torn mid-write: try again next tick
    }
    const double now = std::chrono::duration<double>(
                           PerfClock::now().time_since_epoch())
                           .count();
    RawCamSample s;
    s.t = now;
    for (int i = 0; i < 16; ++i) {
      s.view[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[i]));
      s.proj[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[16 + i]));
    }
    // Sanity: perspective proj + a plausible rotation row (stale viewcam
    // addresses after a map change read garbage until re-published).
    const float r0n = s.view[0] * s.view[0] + s.view[1] * s.view[1] +
                      s.view[2] * s.view[2];
    if (s.proj[2 * 4 + 3] != 1.0f || r0n < 0.9f || r0n > 1.1f) {
      continue;
    }
    if (std::memcmp(last_view, s.view, sizeof(last_view)) == 0) {
      continue;  // pose unchanged
    }
    std::memcpy(last_view, s.view, sizeof(last_view));
    // Camera-signal recorder: every DISTINCT pose the guest produced, with
    // the sampler's precise host timestamp (kind 0).
    if (now < g_camsig_deadline.load(std::memory_order_relaxed)) {
      CamSigPush(now, 0.0, YawFromViewRows(s.view), 0);
    }
    std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
    if (g_cam_samples.size() < 256) {
      g_cam_samples.push_back(s);
      g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void EnsureCamSampler() {
  if (g_cam_sampler_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::thread(CamSamplerLoop).detach();
  REXLOG_INFO("native-scene: camera sampler thread started (~1 kHz)");
}

// Rebuild a row-vector view matrix from pose (rows 0..2 = R, row 3 = -c*R)
// and produce view_proj = view x proj.
void ComposeViewProj(const CamPose& p, const float proj[16], float vp_out[16],
                     float cam_out[3]) {
  float v[16] = {};
  ViewRotFromQuat(p.q, v);
  v[3] = v[7] = v[11] = 0.0f;
  for (int k = 0; k < 3; ++k) {
    v[12 + k] = -(p.c[0] * v[0 * 4 + k] + p.c[1] * v[1 * 4 + k] + p.c[2] * v[2 * 4 + k]);
  }
  v[15] = 1.0f;
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += v[r * 4 + k] * proj[k * 4 + col];
      }
      vp_out[r * 4 + col] = sum;
    }
  }
  std::memcpy(cam_out, p.c, 3 * sizeof(float));
}

// Drone / free-fly camera: overrides the published pose with a user-flown
// one while the game keeps simulating on its own camera. Engaged from the
// current guest pose (roll is dropped; the gameplay camera never rolls),
// the projection is frozen at engage so gameplay FOV changes don't disturb
// the shot, and both linear and angular key input drive exponentially
// smoothed velocities for drone-like motion.
//
// Guest-side culling is handled by taking over the game's own render
// camera: the frame build publishes the flown view matrix below, and the
// Sk8::Render::ViewCamera::SetViewMatrix override (bottom of this file)
// rewrites the game's main-camera argument with it, so the game derives
// its view-proj, frustum planes and cull/submit lists from the drone pose
// itself; statics, characters and props are all submitted exactly as if
// the game were looking there. (A draw-item union like the synthetic-pan
// probe's cannot do this: it can only re-append items the game already
// submitted from poses it actually rendered, and rigid props that ride
// entity animation smear at their stale transforms.)
std::mutex g_freecam_guest_mutex;
float g_freecam_guest_view[16] = {};  // row-vector view, host endianness
float g_freecam_guest_pos[3] = {};    // camera world position
std::atomic<uint32_t> g_freecam_guest_active{0};
std::atomic<uint64_t> g_freecam_guest_rewrites{0};
struct FreecamState {
  bool engaged = false;
  double yaw = 0.0, pitch = 0.0;  // radians: yaw about world Y, then pitch
  double pos[3] = {};
  double vel[3] = {};                     // smoothed world-space velocity
  double yaw_vel = 0.0, pitch_vel = 0.0;  // smoothed key-look rates (rad/s)
  double zoom = 1.0;                      // projection scale (Z/X keys)
  float proj0[16] = {};                   // projection frozen at engage
  // Basis and screen-direction signs resolved numerically at engage (the
  // view right/up column signs against cross-product candidates, and the
  // projection's x/y scale signs), so no handedness convention is assumed;
  // the game's projection carries a negative x scale.
  float sign_right = 1.0f, sign_up = 1.0f;
  double look_sign_x = 1.0, look_sign_y = 1.0;
  double last_t = 0.0;
#if defined(_WIN32)
  bool mouse_anchored = false;
  POINT mouse_last = {};
#endif
};
FreecamState g_freecam;

// Guest render thread only. Returns true while engaged; scene.view_proj,
// scene.proj and scene.cam_pos then hold the flown pose.
bool UpdateFreecam(FrameScene& scene, const float cam_view[16], double now) {
  FreecamState& fc = g_freecam;
  if (!REXCVAR_GET(skate3_native_render_scene_freecam)) {
    if (fc.engaged) {
      fc.engaged = false;
      g_freecam_guest_active.store(0, std::memory_order_release);
      REXLOG_INFO(
          "native-scene freecam: off (guest camera restored; {} guest "
          "SetViewMatrix rewrites while engaged)",
          g_freecam_guest_rewrites.exchange(0, std::memory_order_relaxed));
    }
    return false;
  }
  if (!fc.engaged) {
    // Engage from this frame's raw guest pose. The view's upper-3x3 columns
    // are the camera axes in world space (row-vector convention):
    // 0 = right, 1 = up, 2 = forward.
    const float f0[3] = {cam_view[2], cam_view[6], cam_view[10]};
    fc.yaw = std::atan2(double(f0[0]), double(f0[2]));
    fc.pitch = std::asin(std::clamp(double(f0[1]), -1.0, 1.0));
    for (int j = 0; j < 3; ++j) {
      fc.pos[j] = -(cam_view[12] * cam_view[j * 4 + 0] +
                    cam_view[13] * cam_view[j * 4 + 1] +
                    cam_view[14] * cam_view[j * 4 + 2]);
    }
    // Right candidate cross(world_up, forward) = (f.z, 0, -f.x): its sign
    // against the live right column fixes the basis handedness.
    fc.sign_right = f0[2] * cam_view[0] - f0[0] * cam_view[8] >= 0.0f ? 1.0f : -1.0f;
    const float r0[3] = {f0[2] * fc.sign_right, 0.0f, -f0[0] * fc.sign_right};
    const float u0[3] = {f0[1] * r0[2] - f0[2] * r0[1],
                         f0[2] * r0[0] - f0[0] * r0[2],
                         f0[0] * r0[1] - f0[1] * r0[0]};
    fc.sign_up = u0[0] * cam_view[1] + u0[1] * cam_view[5] + u0[2] * cam_view[9] >= 0.0f
                     ? 1.0f
                     : -1.0f;
    std::memcpy(fc.proj0, scene.proj, sizeof(fc.proj0));
    // Screen-relative look/strafe directions: view +x maps to screen right
    // only when the projection x scale is positive (it isn't in this game),
    // so fold the projection signs in once. +yaw rotates forward toward +x
    // (the right column direction times sign_right), hence the product.
    fc.look_sign_x = double(fc.sign_right) * (fc.proj0[0] >= 0.0f ? 1.0 : -1.0);
    fc.look_sign_y = double(fc.sign_up) * (fc.proj0[5] >= 0.0f ? 1.0 : -1.0);
    fc.vel[0] = fc.vel[1] = fc.vel[2] = 0.0;
    fc.yaw_vel = fc.pitch_vel = 0.0;
    fc.zoom = 1.0;
    fc.last_t = now;
#if defined(_WIN32)
    fc.mouse_anchored = false;
#endif
    fc.engaged = true;
    REXLOG_INFO(
        "native-scene freecam: ENGAGED at ({:.1f}, {:.1f}, {:.1f}); WASD fly, "
        "E/Space up, Q/C down, arrows/right-drag look, Z/X zoom, Shift fast, "
        "Ctrl slow",
        fc.pos[0], fc.pos[1], fc.pos[2]);
  }
  const double dt = std::clamp(now - fc.last_t, 0.0, 0.1);
  fc.last_t = now;

  double mv_f = 0.0, mv_r = 0.0, mv_u = 0.0;  // move intent (camera-relative)
  double lk_yaw = 0.0, lk_pitch = 0.0;        // arrow-key look intent
  double mouse_yaw = 0.0, mouse_pitch = 0.0;  // right-drag deltas (radians)
  double speed_mult = 1.0, zoom_dir = 0.0;
#if defined(_WIN32)
  const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  mv_f = (down('W') ? 1.0 : 0.0) - (down('S') ? 1.0 : 0.0);
  mv_r = (down('D') ? 1.0 : 0.0) - (down('A') ? 1.0 : 0.0);
  mv_u = ((down('E') || down(VK_SPACE)) ? 1.0 : 0.0) -
         ((down('Q') || down('C')) ? 1.0 : 0.0);
  lk_yaw = (down(VK_RIGHT) ? 1.0 : 0.0) - (down(VK_LEFT) ? 1.0 : 0.0);
  lk_pitch = (down(VK_UP) ? 1.0 : 0.0) - (down(VK_DOWN) ? 1.0 : 0.0);
  zoom_dir = (down('Z') ? 1.0 : 0.0) - (down('X') ? 1.0 : 0.0);
  if (down(VK_SHIFT)) {
    speed_mult = 4.0;
  } else if (down(VK_CONTROL)) {
    speed_mult = 0.2;
  }
  if (down(VK_RBUTTON)) {
    POINT p;
    if (GetCursorPos(&p)) {
      if (fc.mouse_anchored) {
        constexpr double kRadPerPixel = 0.0022;  // ~0.13 deg per pixel
        mouse_yaw = (p.x - fc.mouse_last.x) * kRadPerPixel;
        mouse_pitch = -(p.y - fc.mouse_last.y) * kRadPerPixel;
      }
      fc.mouse_last = p;
      fc.mouse_anchored = true;
    }
  } else {
    fc.mouse_anchored = false;
  }
#endif

  // Look: arrow keys drive a smoothed angular velocity (cinematic ease-in/
  // out); mouse deltas apply directly. Pitch is clamped short of the poles.
  const double look_rate =
      REXCVAR_GET(skate3_native_render_scene_freecam_look_speed) *
      (3.14159265358979323846 / 180.0);
  const double ang_k = dt > 0.0 ? 1.0 - std::exp(-dt / 0.08) : 0.0;
  fc.yaw_vel += (lk_yaw * look_rate - fc.yaw_vel) * ang_k;
  fc.pitch_vel += (lk_pitch * look_rate - fc.pitch_vel) * ang_k;
  fc.yaw += (fc.yaw_vel * dt + mouse_yaw) * fc.look_sign_x;
  fc.pitch += (fc.pitch_vel * dt + mouse_pitch) * fc.look_sign_y;
  constexpr double kPitchMax = 89.0 * 3.14159265358979323846 / 180.0;
  fc.pitch = std::clamp(fc.pitch, -kPitchMax, kPitchMax);
  if (zoom_dir != 0.0) {
    // Zoom-in only: the guest camera (and therefore the game's culling)
    // keeps the unzoomed projection, so a narrower rendered FOV is always a
    // subset of what was culled; zooming wider would show unculled edges.
    fc.zoom = std::clamp(fc.zoom * std::exp(zoom_dir * dt), 1.0, 4.0);
  }

  // Basis from yaw/pitch (unit length by construction, signs from engage).
  const double cy = std::cos(fc.yaw), sy = std::sin(fc.yaw);
  const double cp = std::cos(fc.pitch), sp = std::sin(fc.pitch);
  const float fwd[3] = {float(sy * cp), float(sp), float(cy * cp)};
  const float right[3] = {float(cy) * fc.sign_right, 0.0f,
                          float(-sy) * fc.sign_right};
  const float up[3] = {float(-sp * sy) * fc.sign_right * fc.sign_up,
                       float(cp) * fc.sign_right * fc.sign_up,
                       float(-sp * cy) * fc.sign_right * fc.sign_up};

  // Move: forward along the view direction (true drone flight), strafe
  // along screen right, vertical along world up; combined intent normalized
  // so diagonals don't fly faster. Exponential ease toward the target.
  const double ilen = std::sqrt(mv_f * mv_f + mv_r * mv_r + mv_u * mv_u);
  const double speed = REXCVAR_GET(skate3_native_render_scene_freecam_speed) *
                       speed_mult * (ilen > 1.0 ? 1.0 / ilen : 1.0);
  const double strafe = mv_r * fc.look_sign_x * speed;
  const double tvel[3] = {mv_f * speed * fwd[0] + strafe * right[0],
                          mv_f * speed * fwd[1] + mv_u * speed,
                          mv_f * speed * fwd[2] + strafe * right[2]};
  const double vel_k = dt > 0.0 ? 1.0 - std::exp(-dt / 0.25) : 0.0;
  for (int j = 0; j < 3; ++j) {
    fc.vel[j] += (tvel[j] - fc.vel[j]) * vel_k;
    fc.pos[j] += fc.vel[j] * dt;
  }

  // Compose and publish: view columns = right/up/forward, translation row
  // = -pos * R; projection = the engage-frozen one with the zoom folded in
  // (published to scene.proj too so depth-based post passes unproject with
  // what was actually rendered).
  float view[16] = {};
  for (int i = 0; i < 3; ++i) {
    view[i * 4 + 0] = right[i];
    view[i * 4 + 1] = up[i];
    view[i * 4 + 2] = fwd[i];
  }
  const float posf[3] = {float(fc.pos[0]), float(fc.pos[1]), float(fc.pos[2])};
  for (int k = 0; k < 3; ++k) {
    view[12 + k] = -(posf[0] * view[0 * 4 + k] + posf[1] * view[1 * 4 + k] +
                     posf[2] * view[2 * 4 + k]);
  }
  view[15] = 1.0f;
  // Hand the flown view to the guest-camera takeover (the SetViewMatrix
  // override): the game's next camera update culls/submits with this pose.
  {
    std::lock_guard<std::mutex> lock(g_freecam_guest_mutex);
    std::memcpy(g_freecam_guest_view, view, sizeof(view));
    std::memcpy(g_freecam_guest_pos, posf, sizeof(posf));
  }
  g_freecam_guest_active.store(1, std::memory_order_release);
  float pr[16];
  std::memcpy(pr, fc.proj0, sizeof(pr));
  pr[0] *= float(fc.zoom);
  pr[5] *= float(fc.zoom);
  std::memcpy(scene.proj, pr, sizeof(pr));
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += view[r * 4 + k] * pr[k * 4 + col];
      }
      scene.view_proj[r * 4 + col] = sum;
    }
  }
  std::memcpy(scene.cam_pos, posf, sizeof(posf));
  return true;
}

// While the freecam is engaged, fills out_pos with the flown camera world
// position and returns true. Consumed by the draw-distance hooks to
// recenter the guest distance culls on the drone.
bool FreecamGuestPose(float out_pos[3]) {
  if (g_freecam_guest_active.load(std::memory_order_acquire) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_freecam_guest_mutex);
  std::memcpy(out_pos, g_freecam_guest_pos, 3 * sizeof(float));
  return true;
}

bool LoadingOrFrontendActive() {
  if (rex::kernel::guest_presence::GameplayContextValue() != 0) {
    return false;
  }
  // Same publish-staleness window as YieldForMenus: the pause menu keeps
  // the world resubmitting perspective scenes behind it, loading screens
  // and the frontend stop publishing.
  const int64_t last_ns = g_last_publish_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return true;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ns - last_ns >= 300'000'000;
}

// Guest render thread only. Returns true when vp_out/cam_out hold a
// re-timed pose for `now`; false = keep the raw guest pose (no history yet,
// or a cut/teleport snapped).
//
// Dejitter structure (the naive last-two-samples lerp still juddered):
// sample DETECTION times are quantized to render frames (+-2.5 ms of noise
// on a ~4.5 ms camera tick), and re-basing the timeline on each new sample
// jerks the output whenever sample spacing varies. Instead: a pose RING
// timestamped at detection, an EMA of the sample period, and a playback
// clock that advances on real frame time and only slews gently toward
// (newest - 2 periods). The bracketing ring samples are slerped at the
// playback time; sample-time noise is absorbed by the slew instead of
// feeding straight into the pose velocity.
// Most recent camera sim-tick time (guest render thread only): entities that
// changed pose this frame changed on the same sim tick; timestamping their
// pose rings with it instead of the frame time avoids the same aliasing.
double g_latest_cam_tick = 0.0;


bool SmoothCamera(const float view[16], const float proj[16], const float raw_vp[16],
                  const float raw_cam[3], double now, float vp_out[16],
                  float cam_out[3]) {
  constexpr int kRing = 64;  // ~1 kHz sampler: cover well past the lag window
  static CamPose s_ring[kRing];
  static int s_count = 0;  // -1 = convention drift, never smooth this run
  static int s_newest = 0;
  static float s_last_view[16] = {};
  static double s_period = 1.0 / 200.0;  // EMA of the guest camera tick
  static double s_play = 0.0;            // playback clock (guest-pose time axis)
  static double s_last_now = 0.0;
  static bool s_play_valid = false;

  const auto push_pose = [&](const CamPose& p) {
    if (s_count < 0) {
      return;
    }
    const CamPose& latest = s_ring[s_newest];
    const float dot = std::fabs(p.q[0] * latest.q[0] + p.q[1] * latest.q[1] +
                                p.q[2] * latest.q[2] + p.q[3] * latest.q[3]);
    const float dx = p.c[0] - latest.c[0], dy = p.c[1] - latest.c[1],
                dz = p.c[2] - latest.c[2];
    const float dist2 = dx * dx + dy * dy + dz * dz;
    if (s_count == 0 || p.t - latest.t > 0.1 || dist2 > 25.0f || dot < 0.9f) {
      // First sample / stale history / teleport / hard cut: snap.
      s_ring[0] = p;
      s_newest = 0;
      s_count = 1;
      s_play_valid = false;
    } else {
      const double dt = p.t - latest.t;
      if (dt <= 0.0) {
        return;  // out-of-order/duplicate
      }
      if (dt > 0.0005 && dt < 0.05) {
        s_period = s_period * 0.9 + dt * 0.1;
      }
      s_newest = (s_newest + 1) % kRing;
      s_ring[s_newest] = p;
      s_count = std::min(s_count + 1, kRing);
    }
    g_latest_cam_tick = p.t;
  };

  // Primary source: the ~1 kHz sampler thread (precise timestamps: pose
  // samples taken per RENDERED frame alias against the ~200 Hz sim tick as
  // soon as the render rate is paced down; see CamSamplerLoop).
  {
    static std::vector<RawCamSample> s_pending;
    {
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      s_pending.swap(g_cam_samples);
    }
    for (const RawCamSample& s : s_pending) {
      CamPose p;
      p.t = s.t;
      QuatFromView(s.view, p.q);
      for (int j = 0; j < 3; ++j) {
        p.c[j] = -(s.view[12] * s.view[j * 4 + 0] + s.view[13] * s.view[j * 4 + 1] +
                   s.view[14] * s.view[j * 4 + 2]);
      }
      push_pose(p);
    }
    s_pending.clear();
  }

  if (std::memcmp(s_last_view, view, sizeof(s_last_view)) != 0) {
    std::memcpy(s_last_view, view, sizeof(s_last_view));
    CamPose p;
    p.t = now;
    QuatFromView(view, p.q);
    std::memcpy(p.c, raw_cam, sizeof(p.c));
    // One-shot reconstruction self-check: the rebuilt view_proj at this
    // exact pose must match the guest's own (catches any convention drift).
    static bool s_checked = false;
    if (!s_checked) {
      s_checked = true;
      float test_vp[16], test_cam[3];
      ComposeViewProj(p, proj, test_vp, test_cam);
      float max_rel = 0.0f;
      for (int i = 0; i < 16; ++i) {
        const float mag = std::max(1.0f, std::fabs(raw_vp[i]));
        max_rel = std::max(max_rel, std::fabs(test_vp[i] - raw_vp[i]) / mag);
      }
      REXLOG_INFO("native-scene: smooth-camera reconstruction check max_rel={:.6f}{}",
                  max_rel, max_rel > 0.01f ? " (BAD - smoothing disabled this run)" : "");
      if (max_rel > 0.01f) {
        s_count = -1;
      }
    }
    // Fallback only: if the sampler has fed the ring recently, the frame-
    // grid pose is a stale duplicate of a sampler pose; pushing it would
    // corrupt the timestamps.
    if (s_count >= 0 && (s_count == 0 || now - s_ring[s_newest].t > 0.05)) {
      push_pose(p);
    }
  }
  if (s_count < 3 || now - s_ring[s_newest].t > 0.1) {
    s_play_valid = false;
    g_smooth_active = false;
    return false;
  }

  // Playback clock: advance by real frame time; slew gently toward the
  // target lag point so detection-time noise never becomes pose velocity.
  const double frame_dt =
      s_play_valid ? std::clamp(now - s_last_now, 0.0, 0.05) : 0.0;
  s_last_now = now;
  // Boxcar pose filter (see below): the playback point must lag far enough
  // that the CENTERED window has ring samples on both sides.
  const double filter_w = std::clamp(
      REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
      1e-3;
  const double lag = std::max(2.0 * s_period, filter_w * 0.5 + s_period);
  const double target = s_ring[s_newest].t - lag;
  if (!s_play_valid) {
    s_play = target;
    s_play_valid = true;
  } else {
    s_play += frame_dt;
    const double err = target - s_play;
    if (err > 0.1 || err < -0.1) {
      s_play = target;  // fell far behind/ahead (hitch): snap the clock
    } else {
      s_play += err * 0.06;
    }
  }
  // Never play past the newest sample: when panning STOPS, samples stop
  // arriving while the clock keeps advancing; extrapolating overshot the
  // stop pose and the staleness cutoff then snapped back ("catch-up judder
  // when I stop panning"). Parked at the newest sample, the smoothed pose
  // settles exactly onto the raw stop pose and every later handoff is
  // seamless.
  s_play = std::min(s_play, s_ring[s_newest].t);

  // Evaluate the piecewise-linear pose signal at an arbitrary time by
  // bracketing in the ring (samples are time-ordered oldest -> newest) and
  // slerping. Clamps to the ring's ends.
  const auto eval_ring = [&](double t_eval) {
    int hi = s_newest;
    int lo = (s_newest + kRing - 1) % kRing;
    for (int step = 1; step < s_count - 1; ++step) {
      if (s_ring[lo].t <= t_eval) {
        break;
      }
      hi = lo;
      lo = (lo + kRing - 1) % kRing;
    }
    const CamPose& p0 = s_ring[lo];
    const CamPose& p1 = s_ring[hi];
    const double span = std::max(p1.t - p0.t, 0.0005);
    const double alpha = std::clamp((t_eval - p0.t) / span, 0.0, 1.0);
    CamPose r;
    r.t = t_eval;
    QuatSlerp(p0.q, p1.q, float(alpha), r.q);
    for (int k = 0; k < 3; ++k) {
      r.c[k] = p0.c[k] + (p1.c[k] - p0.c[k]) * float(alpha);
    }
    return r;
  };

  CamPose p;
  if (filter_w > 0.0005 && s_count >= 4) {
    // Boxcar pose filter: average the interpolated signal over a window
    // CENTERED on the playback point (zero phase error at constant
    // velocity). Root-caused need (camera-signal recordings): the
    // game's camera pose VALUES advance in 60 Hz-quantized lumps at high
    // render rates; during a measured constant stick pan the per-tick
    // angular velocity had sd 144 deg/s on a 172 deg/s mean, +-2.2 deg off
    // a constant-rate path. A 50 ms window (three 60 Hz periods) nulls the
    // quantization at any render rate: measured frame-to-frame velocity
    // jitter fell 185 -> 7 deg/s rms on the recorded signal. Taps past the
    // newest sample clamp to it, so a pan STOP still settles exactly onto
    // the raw stop pose (never extrapolates).
    constexpr int kTaps = 16;
    double qacc[4] = {};
    double cacc[3] = {};
    float qref[4] = {};
    for (int k = 0; k < kTaps; ++k) {
      const double tt = std::min(s_play - filter_w * 0.5 + (k + 0.5) * filter_w / kTaps,
                                 s_ring[s_newest].t);
      const CamPose s = eval_ring(tt);
      float sq[4] = {s.q[0], s.q[1], s.q[2], s.q[3]};
      if (k == 0) {
        std::memcpy(qref, sq, sizeof(qref));
      } else if (sq[0] * qref[0] + sq[1] * qref[1] + sq[2] * qref[2] +
                     sq[3] * qref[3] <
                 0.0f) {
        for (float& v : sq) v = -v;
      }
      for (int j = 0; j < 4; ++j) qacc[j] += sq[j];
      for (int j = 0; j < 3; ++j) cacc[j] += s.c[j];
    }
    const double qn = std::sqrt(qacc[0] * qacc[0] + qacc[1] * qacc[1] +
                                qacc[2] * qacc[2] + qacc[3] * qacc[3]);
    p.t = s_play;
    for (int j = 0; j < 4; ++j) {
      p.q[j] = float(qacc[j] / std::max(qn, 1e-12));
    }
    for (int j = 0; j < 3; ++j) {
      p.c[j] = float(cacc[j] / kTaps);
    }
  } else {
    p = eval_ring(s_play);
  }
  ComposeViewProj(p, proj, vp_out, cam_out);
  g_smooth_play = s_play;
  g_smooth_pose = p;
  g_smooth_active = true;
  return true;
}

// Minimum time spacing (seconds) between dynamic-pose ring ingests. The
// ring is consumed per CHANGE, and above ~200 fps the guest re-packs poses
// every rendered frame; without a spacing floor the ring's fixed capacity
// covers ever less real time as the frame rate climbs, until the smoothing
// kernel's ~55 ms reach falls off the old end (the high-fps skater/board
// rubberband). 3.5 ms keeps the kept signal finer than the guest's
// ~200 Hz pose tick while bounding capacity use; at ~285 fps and below
// every change still ingests, so the tuned mid-rate behavior is untouched.
// Applies to POSE ingests only: ropa cloth-decode enqueues must NOT be
// decimated (see the enqueue block; pose/shape pairing relies on their
// per-frame cadence).
constexpr double kMinDynIngestSpacing = 0.0035;

// Interpolate the DYNAMIC items' per-draw state (bone palettes; rigid
// non-identity world matrices) at the camera's playback time. Without this
// the smoothed camera glides while the skater/NPCs/props snap at the guest
// sim tick; the stepping that used to be hidden (camera and entities
// stepped IN PHASE) becomes visible entity judder. History pairs poses by
// (mesh, k-th occurrence this frame): clones publish in stable submit
// order, so the k-th copy pairs with last frame's k-th copy. Componentwise
// lerp is exact enough at adjacent-sim-tick deltas (~5 ms of motion).
// Guest render thread only.
void InterpolateDynamicItems(uint8_t* base, FrameScene& scene, double now) {
  // Per-entity pose RING, like the camera's: the playback clock sits ~2 sim
  // periods behind `now`, so a two-pose history never brackets it (the
  // interpolation alpha pinned at 0 and entities rendered STALE STEPPED
  // poses, visible judder and motion blur). The camera filter
  // pushes the play clock ~(W/2 + period) back AND the entity boxcar
  // (below) reaches another W/2 past that (~55 ms total at the default
  // 50 ms window). With ingests spaced at least kMinDynIngestSpacing apart,
  // 24 slots guarantee >= ~80 ms of coverage at ANY render rate. The shared
  // g_smooth_play keeps entities in phase with the camera.
  constexpr int kRing = 24;
  struct DynPose {
    double t = 0.0;
    std::vector<float> b;  // bone palette (skinned), raw captured rows
    float w[16] = {};      // world (rigid)
    // ROPA: the newest cloth-shape generation (dyn job seq) that existed
    // when this pose was captured, the shape that belongs WITH this pose
    // (constant enqueue offset; the draw lerps the bracketing generations).
    uint64_t shape_seq = 0;
  };
  struct DynHist {
    DynPose ring[kRing];
    int count = 0;
    int newest = 0;
    uint64_t seen = 0;
    // EMA of this entity's own pose-change period. Characters/board update
    // at the 60 Hz sim value cadence (~16.7 ms); traffic vehicles update
    // SLOWER (their own sim rate); those entities need their evaluation
    // point delayed by one own-period or the playback clock runs past
    // their newest sample and they render raw stepped poses (see play_e).
    double period = 0.0;
    // Per-bone skin-weighted vertex centroids in BIND space (w, x, y, z),
    // lazily computed from the mesh's own vertex buffer the first time a
    // bone of this entity trips the spin-collapse guard. The centroid IS
    // the wheel's geometric center (verified in capture:
    // wheel-mesh bone centroids match the motion-solved spin pivot to a
    // millimeter), so the collapse guard can pin it to the boxcar path
    // exactly, no runtime estimation (every estimator variant tested was
    // noisier than the artifact it fixed).
    uint32_t cen_vb = 0;
    uint32_t cen_bytes = 0;
    std::vector<std::array<float, 4>> cen;
    // Timestamp of the last PERSPECTIVE-sourced (non-caster, non-retained)
    // sample ingested: decides whether a caster pose tracks (caster-only
    // stream) or holds (perspective stream fresh; interleaving the two
    // sawtooths the wheel phase).
    double last_persp_t = 0.0;
  };
  static std::unordered_map<uint64_t, DynHist> s_hist;
  static uint64_t s_frame = 0;
  ++s_frame;
  // Drop the pose rings of character meshes rebuilt at a reused address (see
  // BuildFrameScene's rebuild check): interpolating the new garment against
  // its predecessor's poses stretched it. Keys embed the mesh either as
  // (mesh << 8 | occurrence) or, for identity-keyed rings, the low 32 bits.
  if (!g_rebuilt_meshes.empty()) {
    std::erase_if(s_hist, [](const auto& e) {
      const uint64_t key = e.first;
      const uint32_t mesh = (key >> 63) != 0 ? uint32_t(key) : uint32_t(key >> 8);
      return std::find(g_rebuilt_meshes.begin(), g_rebuilt_meshes.end(), mesh) !=
             g_rebuilt_meshes.end();
    });
    g_rebuilt_meshes.clear();
  }
  // Bone-signal recorder window state (see BoneSigAppend). Written on a
  // detached thread once the window closes.
  const bool bs_rec = BoneSigTick(now);
  static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // ---- char_track diagnostics (skate3_native_render_scene_char_track) ----
  // One log line per frame following the main character's ropa garment and
  // body through this function: which events fired, what the ring held, and
  // where the item actually rendered. Selection: garment = nearest ropa
  // item to the camera; body = the highest-bone-count skinned non-ropa
  // character item within 5 m (bone count separates the body from the
  // board/accessories at equal distance).
  struct CharTrack {
    int idx = -1;
    float raw[3] = {};     // pre-interp reference position (bone 0 / world t)
    int ing = 0;           // 1 = pose ingested, 2 = change skipped (spacing)
    int rst = 0;           // discontinuity reset fired
    int clm = 0;           // 1 = claimed another ring, 2 = fresh ring slot
    int hold = 0;          // caster/retained hold replaced the pose
    int raw_render = 0;    // 0 = interpolated; 1 = ring<3/stale, 2 = size
                           // mismatch, 3 = hidden (unrepairable)
    int ring_n = 0;        // ring occupancy at entry
    float age_ms = -1.0f;  // ring-newest age at entry
    float per_ms = -1.0f;  // ring period EMA
    float ba = -1.0f;      // pair-lerp alpha at the playback point
    int box = 0;           // took the 8-tap boxcar
    int detach = 0;        // ropa DETACH repair fired
    float sw = 0.0f;       // shape-kernel total weight (garment only)
  };
  CharTrack trk_shirt, trk_body;
  const bool trk_on = REXCVAR_GET(skate3_native_render_scene_char_track);
  if (trk_on) {
    float best_s = 25.0f;  // 5 m gate, squared
    uint32_t best_b_bones = 0;
    float best_b_d2 = 25.0f;
    for (size_t i = 0; i < scene.items.size(); ++i) {
      const DrawItem& it = scene.items[i];
      const bool skn = it.skinned && it.bones.size() >= 12;
      float p[3];
      if (skn) {
        p[0] = it.bones[3];
        p[1] = it.bones[7];
        p[2] = it.bones[11];
      } else {
        p[0] = it.world[12];
        p[1] = it.world[13];
        p[2] = it.world[14];
      }
      const float dx = p[0] - scene.cam_pos[0];
      const float dy = p[1] - scene.cam_pos[1];
      const float dz = p[2] - scene.cam_pos[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (it.ropa) {
        if (d2 < best_s) {
          best_s = d2;
          trk_shirt.idx = int(i);
          std::memcpy(trk_shirt.raw, p, sizeof(p));
        }
      } else if (skn && it.char_family >= 1 && it.char_family <= 5 &&
                 d2 < 25.0f) {
        const uint32_t nb = uint32_t(it.bones.size() / 12);
        if (nb > best_b_bones || (nb == best_b_bones && d2 < best_b_d2)) {
          best_b_bones = nb;
          best_b_d2 = d2;
          trk_body.idx = int(i);
          std::memcpy(trk_body.raw, p, sizeof(p));
        }
      }
    }
  }
  std::unordered_map<uint32_t, uint32_t> occurrence;
  for (DrawItem& item : scene.items) {
    CharTrack* trk = nullptr;
    if (trk_on) {
      const int idx = int(&item - scene.items.data());
      if (idx == trk_shirt.idx) {
        trk = &trk_shirt;
      } else if (idx == trk_body.idx) {
        trk = &trk_body;
      }
    }
    const bool skinned = item.skinned && !item.bones.empty();
    const bool rigid_dyn =
        !skinned && std::memcmp(item.world, kIdent, sizeof(kIdent)) != 0;
    if (!skinned && !rigid_dyn) {
      continue;
    }
    const uint32_t k = occurrence[item.mesh]++;
    // Skinned pose-to-pose translation distance^2 at a NON-SPINNING
    // reference. Fast wheel bones sweep multi-meter circles between ~8 ms
    // samples (the model->world affine's translation carries the axle-
    // pivot compensation; measured 2.9 m single-sample swings on traffic
    // at speed), and bone 0 IS a wheel on the vehicle meshes, so keying
    // the teleport gate on it reset the ring every few samples and
    // traffic spent most of its time on raw stepped poses ("the whole
    // vehicle lags then jumps to catch up"; the board's centimeter wheel
    // offsets never trip it). A genuine teleport/mispair moves EVERY
    // bone, spin moves only the wheels: with the entity's vertex-weighted
    // bone set known (the centroid table), take the MINIMUM jump over
    // real bones; until it exists, bone 0.
    const auto skinned_dist2 = [&](const std::vector<float>& a,
                                   const std::vector<float>& b,
                                   const DynHist& hh) -> float {
      const auto bone_d2 = [&](size_t bone) -> float {
        const size_t bi = bone * 12;
        const float dx = a[bi + 3] - b[bi + 3];
        const float dy = a[bi + 7] - b[bi + 7];
        const float dz = a[bi + 11] - b[bi + 11];
        return dx * dx + dy * dy + dz * dz;
      };
      const size_t nbones = a.size() / 12;
      float best = 1e30f;
      const size_t ncen = std::min(hh.cen.size(), nbones);
      for (size_t bone = 0; bone < ncen; ++bone) {
        if (hh.cen[bone][0] > 0.5f) {
          best = std::min(best, bone_d2(bone));
        }
      }
      return best < 1e30f ? best : bone_d2(0);
    };
    // Distance^2 from this item's new pose to a history's newest pose;
    // 1e30 when the history is empty, stale, or a different palette size.
    const auto hist_dist2 = [&](const DynHist& hh) -> float {
      if (hh.count == 0) {
        return 1e30f;
      }
      const DynPose& lp = hh.ring[hh.newest];
      if (now - lp.t > 0.1) {
        return 1e30f;
      }
      if (skinned) {
        if (lp.b.size() != item.bones.size() || lp.b.size() < 12) {
          return 1e30f;
        }
        return skinned_dist2(item.bones, lp.b, hh);
      }
      const float dx = item.world[12] - lp.w[12];
      const float dy = item.world[13] - lp.w[13];
      const float dz = item.world[14] - lp.w[14];
      const float d2 = dx * dx + dy * dy + dz * dz;
      // Rotation-aware for rigid: mirrored clone twins can sit at the SAME
      // translation (railing pairs), and a translation-only metric pairs
      // their rings freely - the interpolator then lerps across the flip.
      // Fold the rotation delta in as an equivalent distance; a mispair
      // (elementwise delta up to 2.0) lands far outside every claim cap,
      // while a physics prop's one-tick rotation stays well inside.
      float rd = 0.0f;
      for (int r = 0; r < 3; ++r) {
        for (int c2 = 0; c2 < 3; ++c2) {
          rd = std::max(rd, std::fabs(item.world[r * 4 + c2] -
                                      lp.w[r * 4 + c2]));
        }
      }
      return std::max(d2, rd * rd * 4.0f);
    };
    // One-time bind-space vertex-centroid table for this entity (see the
    // pivot-boxcar comment at the collapse guard). Also computed EAGERLY on
    // an entity's first pose so the teleport gate's non-spinning reference
    // (skinned_dist2) exists before the first interpolated frame; a car
    // first seen at full speed otherwise reset its ring off the wheel
    // swings forever and never reached the collapse path that used to
    // build this table.
    const auto ensure_cen = [&](DynHist& hh, size_t nbones) {
          if ((hh.cen_vb != item.vb_addr || hh.cen_bytes != item.vb_bytes) &&
              item.stride != 0 && item.bw_offset != 0 && item.bi_offset != 0) {
            // One-time bind-space centroid pass over this entity's vertex
            // buffer (guest thread; raw guest reads are safe here, same
            // as RefinePaletteBase). u8x4 attributes are big-endian per
            // 32-bit word: component k is byte (24 - 8k) of the host-order
            // load.
            hh.cen_vb = item.vb_addr;
            hh.cen_bytes = item.vb_bytes;
            hh.cen.assign(nbones, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            const uint32_t vcount =
                std::min<uint32_t>(item.vb_bytes / item.stride, 200000);
            for (uint32_t vtx = 0; vtx < vcount; ++vtx) {
              SkinSampleVert sv;
              if (!ReadSkinVertGuest(base, item, vtx, &sv)) {
                break;  // unsupported position format
              }
              for (int k = 0; k < 4; ++k) {
                const uint32_t wgt = sv.w[k];
                if (wgt == 0) {
                  continue;
                }
                const uint32_t bone = sv.bone[k];
                if (bone >= nbones) {
                  continue;
                }
                auto& cb = hh.cen[bone];
                const float wf = float(wgt) * (1.0f / 255.0f);
                cb[0] += wf;
                cb[1] += wf * sv.p[0];
                cb[2] += wf * sv.p[1];
                cb[3] += wf * sv.p[2];
              }
            }
            for (auto& cb : hh.cen) {
              if (cb[0] > 0.5f) {
                cb[1] /= cb[0];
                cb[2] /= cb[0];
                cb[3] /= cb[0];
              }
            }
          }
    };
    // LW-mapped items key their ring by the game's own per-instance
    // identity (the MeshContext); sort-
    // list clone reshuffles cannot mispair an identity key, so the whole
    // positional claim search below is skipped for them. Mesh stays in the
    // key so a model/LOD swap on the same instance starts a fresh ring
    // (pose sizes differ). RIGID items with a ctx take the identity key
    // too: park-editor venues submit dozens of same-mesh piece clones,
    // including mirrored twins at the SAME translation, and any camera pan
    // reshuffles the occurrence ordinals - the positional claim below then
    // swaps the twins' rings every frame (translation distance 0) and the
    // interpolator lerps their 180-degree-apart rotations, the visible
    // piece spin. Legacy (mesh, occurrence) pairing continues to serve
    // everything without a usable ctx (player, CAC, ctx-less captures).
    bool lw_keyed = item.ctx != 0 &&
                    ((item.lw_entity != 0 &&
                      REXCVAR_GET(skate3_native_render_scene_lw_identity)) ||
                     !skinned);
    uint64_t key = lw_keyed ? ((1ull << 63) | (uint64_t(item.ctx) << 32) |
                               uint64_t(item.mesh))
                            : ((uint64_t(item.mesh) << 8) | (k & 0xFF));
    DynHist* hp = &s_hist[key];
    if (lw_keyed && hp->seen == s_frame) {
      // One ctx published twice in a frame (should not happen; dyn_slot
      // dedups per ctx): fall back to the legacy pairing for this copy
      // rather than double-ingesting the identity ring.
      lw_keyed = false;
      key = (uint64_t(item.mesh) << 8) | (k & 0xFF);
      hp = &s_hist[key];
    }
    const float own_d2 = hp->seen == s_frame ? 1e30f : hist_dist2(*hp);
    if (!lw_keyed && own_d2 > 1e-4f) {
      // The k-th slot mispairs: the game's sort lists RESHUFFLE same-mesh
      // clones as they and the camera move. For static props the
      // reset-on-jump guard below was enough (a mispair rendered raw for a
      // few frames), but driving traffic reshuffles CONSTANTLY; the rings
      // never accumulated 3 poses and every moving car rendered raw
      // stepped poses (the vehicle judder/catch-up). Re-pair by POSITION
      // instead: claim the unclaimed history of this mesh whose newest
      // pose is nearest, within the same 1.5 m one-tick jump gate.
      // The search runs whenever the slot is not an (almost) EXACT
      // continuation, not only past the 1.5 m gate: clone placements
      // CLOSER than 1.5 m (the paired newspaper holders, 0.78 m apart)
      // otherwise inherit each other's history on every reshuffle, and the
      // interpolator renders them sliding between the two placements ("the
      // props jitter in and out of position"). Seeding `best` with the own
      // slot's distance keeps a claim strictly-nearer-only, so a genuinely
      // moving entity still prefers its own ring.
      // RIGID claims get a far tighter cap than the vehicles' 1.5 m: when a
      // close clone's OWN ring goes stale (its submission flapped for a few
      // frames), strictly-nearer alone still claims the TWIN's fresh ring
      // (own = 1e30) and slides once, the residual single jitter seen
      // after the strictly-nearer fix. No rigid prop moves 0.3 m in one sim
      // tick (18 m/s at 60 Hz), while clone placements sit >= 0.78 m apart;
      // a rigid item with no history inside 0.3 m starts a fresh ring and
      // renders raw at its correct placement instead.
      const float claim_cap = skinned || item.ropa ? 2.25f : 0.09f;
      float best = std::min(own_d2, claim_cap);
      DynHist* alt = nullptr;
      uint64_t alt_key = key;
      uint32_t fresh_k = 256;  // first index past the dense key range
      for (uint32_t k2 = 0; k2 < 256; ++k2) {
        const uint64_t key2 = (uint64_t(item.mesh) << 8) | k2;
        const auto it2 = s_hist.find(key2);
        if (it2 == s_hist.end()) {
          fresh_k = k2;  // occurrence keys are dense per mesh
          break;
        }
        if (k2 == (k & 0xFF) || it2->second.seen == s_frame) {
          continue;
        }
        const float d2 = hist_dist2(it2->second);
        if (d2 < best) {
          best = d2;
          alt = &it2->second;
          alt_key = key2;
        }
      }
      if (alt != nullptr) {
        hp = alt;
        key = alt_key;
        if (trk != nullptr) {
          trk->clm = 1;
        }
      } else if (hp->seen == s_frame && fresh_k < 256) {
        if (trk != nullptr) {
          trk->clm = 2;
        }
        // The k-th slot already belongs to another clone this frame and no
        // history matches: start a fresh ring instead of corrupting the
        // claimed one with interleaved poses.
        key = (uint64_t(item.mesh) << 8) | fresh_k;
        hp = &s_hist[key];
      }
    }
    DynHist& h = *hp;
    h.seen = s_frame;
    if (trk != nullptr) {
      trk->ring_n = h.count;
      trk->per_ms = float(h.period * 1e3);
      if (h.count > 0) {
        trk->age_ms = float((now - h.ring[h.newest].t) * 1e3);
      }
    }
    if (skinned) {
      ensure_cen(h, item.bones.size() / 12);
    }
    const DynPose& latest = h.ring[h.newest];
    // Per-bone palette repair (vehicles): the sighting captures carried
    // garbage on SOME weighted bones, world positions in the rotation
    // rows, bone 0 gliding along the vehicle's own path, while every
    // sampled-vert gate upstream judged only the bones its samples happen
    // to reference. The survivors rendered as mangled vehicles with zero
    // refusals in the telemetry. Repair the insane bones from the ring's
    // newest pose (the body keeps its live motion; a repaired wheel
    // freezes for the frames its rows are junk); with no sane source for
    // a weighted bone, hide the item for the frame instead.
    if (skinned && (item.char_family == 6 || item.char_family == 7) &&
        !h.cen.empty()) {
      const size_t nbones = item.bones.size() / 12;
      const size_t ncen = std::min(h.cen.size(), nbones);
      const bool ring_ok = h.count > 0 && latest.b.size() == item.bones.size();
      const auto rows_sane = [](const float* bones, size_t bi) {
        for (int r = 0; r < 3; ++r) {
          const float* row = bones + bi + size_t(r) * 4;
          const float n2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
          if (n2 < 0.04f || n2 > 25.0f) {
            return false;
          }
        }
        return true;
      };
      uint32_t repaired = 0;
      bool unrepairable = false;
      for (size_t b = 0; b < ncen && !unrepairable; ++b) {
        if (h.cen[b][0] <= 0.5f) {
          continue;  // not vertex-weighted: staging leftovers are normal
        }
        const size_t bi = b * 12;
        if (rows_sane(item.bones.data(), bi)) {
          continue;
        }
        if (ring_ok && rows_sane(latest.b.data(), bi)) {
          std::memcpy(item.bones.data() + bi, latest.b.data() + bi,
                      12 * sizeof(float));
          ++repaired;
        } else {
          unrepairable = true;
        }
      }
      if (repaired != 0 || unrepairable) {
        static std::atomic<uint64_t> s_repairs{0};
        const uint64_t n = s_repairs.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle palette {} mesh={:08X} fam={} src={} "
              "caster={} bones={} (n={})",
              unrepairable ? "UNREPAIRABLE (hidden)" : "bone-repair",
              item.mesh, item.char_family, item.dbg_src,
              item.caster_bank ? 1 : 0, repaired, n);
        }
        if (unrepairable) {
          item.draws.clear();
          continue;
        }
      }
    }
    // Caster-bank palettes carry ~40 ms-stale FINE animation but a fresh
    // GROSS pose, when they are the only capture stream they must keep
    // TRACKING (the old 0.5 s ring-hold froze passing vehicles mid-motion
    // in plain view, and the seed/hold/drift-out cycle re-seeded every
    // ~100 ms: "cars stop in their tracks and vanish"). Only while
    // PERSPECTIVE samples are fresh does a caster pose hold the ring pose
    // instead (ingesting both interleaves a wheel-phase sawtooth), a
    // <= 50 ms hold until the next perspective sample, imperceptible.
    // Retained re-publishes always hold: their stored pose is frames old
    // and ingesting it would step the ring backward. Ropa garments
    // included: the gate requires SKINNED mode with an identical palette
    // size, so the substitution cannot mix modes.
    // Stale caster bank: the ortho banks occasionally hold a genuinely OLD
    // palette (not just 40 ms of wheel phase), rendered raw, the vehicle
    // momentarily ghosted 10-20 m back along its own trail. A pose > 3 m
    // from a ring pose younger than 100 ms is physically impossible
    // (> 30 m/s of error); hold the fresh ring pose for the frame. The
    // 30 m ceiling keeps clone ring-swaps (45-75 m in the sighting logs,
    // valid poses of DIFFERENT vehicles) rendering raw.
    const bool caster_stale_jump = [&] {
      if (!skinned || !item.caster_bank || h.count == 0 ||
          latest.b.size() != item.bones.size() || now - latest.t > 0.1) {
        return false;
      }
      const float d2 = skinned_dist2(item.bones, latest.b, h);
      return d2 > 9.0f && d2 < 900.0f;
    }();
    // dbg_src 10 = LW-authoritative palette substitution: the pose IS the
    // entity's current sim tick; holding the ring's (older) pose over it
    // would re-stale exactly what the substitution fixed.
    if (skinned && h.count > 0 && latest.b.size() == item.bones.size() &&
        item.dbg_src != 10 &&
        ((item.caster_bank && now - h.last_persp_t <= 0.05) || item.retained ||
         caster_stale_jump)) {
      if (caster_stale_jump) {
        static std::atomic<uint64_t> s_stale_caster{0};
        const uint64_t n = s_stale_caster.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: stale caster pose held mesh={:08X} fam={} "
              "ring_age_ms={:.0f} (n={})",
              item.mesh, item.char_family, (now - latest.t) * 1e3, n);
        }
      }
      item.bones = latest.b;
      if (trk != nullptr) {
        trk->hold = caster_stale_jump ? 2 : 1;
      }
    }
    const bool changed =
        h.count == 0 ||
        (skinned ? latest.b != item.bones
                 : std::memcmp(latest.w, item.world, sizeof(item.world)) != 0);
    // Timestamp for a new ring pose: the camera sampler's latest sim tick
    // when fresh; this entity's pose changed on the same sim tick, and
    // frame-grid timestamps alias against the sim rate once the render
    // loop is paced (the same problem the camera sampler solves). Frame
    // time otherwise.
    const double stamp =
        (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02) ? g_latest_cam_tick : now;
    // Ingest spacing floor: past ~285 fps the pose changes every rendered
    // frame, FASTER than the ~200 Hz camera tick, so consecutive changed
    // poses would stamp with the SAME tick time (degenerate interpolation
    // spans: the collapsed-bone / cloth-shape pair-lerp alpha pins and the
    // output steps once per slot) while the ring's real-time coverage
    // shrinks below the kernel's reach. Changes closer than the floor to
    // the newest slot are not ingested (their bytes still feed the repair
    // and teleport gates); the next qualifying change carries the signal.
    if (changed &&
        !(h.count > 0 && stamp - latest.t < kMinDynIngestSpacing)) {
      // Discontinuities reset the history (pose-size change = LOD/garment
      // swap; long gap = the entity was gone). CRUCIALLY also a translation
      // jump no entity makes in one sim tick (> 1.5 m): clones pair by
      // occurrence order, and the game's sort lists RESHUFFLE clones as the
      // camera moves; without this guard a vending machine inherited its
      // twin's history and visibly slid/teleported between the two
      // placements. Mispairs now render the raw pose (a no-op for the
      // static props where it was visible).
      if (h.count > 0) {
        bool discontinuity =
            now - latest.t > 0.1 ||
            (skinned && latest.b.size() != item.bones.size());
        if (!discontinuity) {
          // Translation jump at the non-spinning reference (see
          // skinned_dist2; bone 0 is a WHEEL on vehicles); rigid worlds
          // carry t in row 3.
          float d2;
          if (skinned) {
            d2 = skinned_dist2(item.bones, latest.b, h);
          } else {
            const float dx = item.world[12] - latest.w[12];
            const float dy = item.world[13] - latest.w[13];
            const float dz = item.world[14] - latest.w[14];
            d2 = dx * dx + dy * dy + dz * dz;
            // Same rotation fold as hist_dist2: a same-spot mirrored-twin
            // mispair must reset the ring, not lerp across the flip. Ropa
            // garments skip it for the same reason they skip the tight
            // translation bound below (a character's garment has no twin):
            // the body's skinned test is translation-only, and a skater
            // turning > ~45 degrees per ring pose (fast replay-editor
            // scrubs) reset ONLY the garment's ring every ingest, so the
            // shirt rendered raw/zero-lag while the body kept its smoothed
            // lag: the shirt racing ahead of the body in the scrub
            // direction.
            if (!item.ropa) {
              float rd = 0.0f;
              for (int r = 0; r < 3; ++r) {
                for (int c2 = 0; c2 < 3; ++c2) {
                  rd = std::max(rd, std::fabs(item.world[r * 4 + c2] -
                                              latest.w[r * 4 + c2]));
                }
              }
              d2 = std::max(d2, rd * rd * 4.0f);
            }
          }
          // Rigid uses the same tight one-tick bound as the claim cap
          // above: 0.78 m clone placements sit inside the vehicles' 1.5 m
          // gate, and lerping across a mispair IS the prop jitter. Ropa
          // garments are EXEMPT from the tight bound even in rigid mode:
          // they ride a character (verified entity-exact worlds, no clone
          // twins to mispair with), and with ingests spaced at the floor a
          // fast skater's world legitimately moves > 0.3 m between ring
          // poses; the tight gate then resets the ring every ingest at
          // speed and the garment renders raw/zero-lag while the body keeps
          // its smoothed lag (the speed-scaled garment jumping). Genuine
          // teleports/respawns still clear the 1.5 m character gate.
          discontinuity = d2 > (skinned || item.ropa ? 2.25f : 0.09f);
          if (discontinuity && !skinned && d2 < 2.25f) {
            // A rigid step that only the tightened bound caught = a
            // close-clone mispair that would have LERPED (the prop jitter).
            static std::atomic<uint64_t> s_rigid_mispair{0};
            const uint64_t n =
                s_rigid_mispair.fetch_add(1, std::memory_order_relaxed);
            if (n < 24 || (n & 255u) == 0) {
              REXLOG_INFO(
                  "native-scene: rigid close-clone mispair reset mesh={:08X} "
                  "k={} d={:.2f}m (n={})",
                  item.mesh, k, std::sqrt(d2), n);
            }
          }
        }
        if (discontinuity) {
          h.count = 0;
          if (trk != nullptr) {
            trk->rst = 1;
          }
        }
      }
      if (trk != nullptr) {
        trk->ing = 1;
      }
      const double prev_t = h.count > 0 ? h.ring[h.newest].t : 0.0;
      h.newest = h.count == 0 ? 0 : (h.newest + 1) % kRing;
      DynPose& p = h.ring[h.newest];
      p.t = stamp;
      if (prev_t > 0.0) {
        // Track the entity's OWN pose-change period (see DynHist::period).
        const double dt = p.t - prev_t;
        if (dt > 0.0005 && dt < 0.1) {
          h.period = h.period == 0.0 ? dt : h.period * 0.75 + dt * 0.25;
        }
      }
      p.b = item.bones;
      std::memcpy(p.w, item.world, sizeof(p.w));
      p.shape_seq = 0;
      if (item.ropa) {
        // Guest thread only, like the enqueue that writes it.
        const auto sit = g_ropa_last_seq.find(item.mesh);
        p.shape_seq = sit != g_ropa_last_seq.end() ? sit->second : 0;
      }
      h.count = std::min(h.count + 1, kRing);
      if (skinned && !item.caster_bank && !item.retained) {
        h.last_persp_t = p.t;
      }
      if (bs_rec) {
        BoneSigAppend(0, key, p.t, 0.0,
                      skinned ? item.bones.data() : item.world,
                      skinned ? uint32_t(item.bones.size()) : 16u);
      }
    } else if (changed && trk != nullptr) {
      trk->ing = 2;  // change withheld by the ingest spacing floor
    }
    if (h.count < 3 || now - h.ring[h.newest].t > 0.1) {
      if (trk != nullptr) {
        trk->raw_render = 1;
      }
      continue;  // not enough history yet: raw stepped pose (one-time snap)
    }
    // Evaluate the ring's piecewise-linear pose signal at time tt into
    // `out_b` (skinned, weighted-accumulated) / `out_w` (rigid), scaled by
    // `weight`. Returns false on a palette-size mismatch inside the window
    // (LOD/garment swap); the caller keeps the raw pose that frame.
    const auto accum_at = [&](double tt, float weight, float* out_b,
                              float out_w[16]) {
      int hi = h.newest;
      int lo = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo].t <= tt) {
          break;
        }
        hi = lo;
        lo = (lo + kRing - 1) % kRing;
      }
      const DynPose& p0 = h.ring[lo];
      const DynPose& p1 = h.ring[hi];
      if (skinned && (p0.b.size() != item.bones.size() ||
                      p1.b.size() != item.bones.size())) {
        return false;
      }
      const double span = std::max(p1.t - p0.t, 0.0005);
      // Clamp at 1.0 (no extrapolation): entities that stop moving must
      // settle exactly onto their raw pose, like the camera.
      const float a = float(std::clamp((tt - p0.t) / span, 0.0, 1.0));
      if (skinned) {
        for (size_t i = 0; i < item.bones.size(); ++i) {
          out_b[i] += (p0.b[i] + (p1.b[i] - p0.b[i]) * a) * weight;
        }
      } else {
        for (int i = 0; i < 16; ++i) {
          out_w[i] += (p0.w[i] + (p1.w[i] - p0.w[i]) * a) * weight;
        }
      }
      return true;
    };
    // Boxcar filter, same as the camera's: the guest's ANIMATION poses are
    // 60 Hz-quantized like its camera; once the camera glides, the 60-vs-
    // render-rate pose alternation reads as skater judder/ghosting against
    // the smooth background. Averaging bone affines componentwise over the
    // window slightly shrinks fast-swinging limb rotations (a subtle motion
    // blur), the trade the game's own 60 Hz presentation makes anyway.
    const double filter_w = std::clamp(
        REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
        1e-3;
    // Per-entity playback point. Entities whose own pose stream is SLOWER
    // than the 60 Hz character cadence (traffic vehicles tick on their own
    // sim rate) pin the shared playback clock past their newest sample;
    // alpha clamps at 1.0, the whole smoothing machinery degenerates to
    // raw stepped poses, and every new sample renders as a visible jump
    // (the vehicle judder/catch-up that survived the entity boxcar).
    // Evaluating one own-period earlier keeps the eval point BRACKETED by
    // samples: the staircase renders as continuous piecewise-linear
    // motion, at the cost of that entity lagging one of ITS sim updates
    // behind the world, invisible for background traffic, and never
    // applied to 60 Hz entities (the skater/NPCs keep the shared clock).
    const double play_e =
        g_smooth_play -
        (h.period > 0.020 ? std::min(h.period - 1.0 / 60.0, 0.1) : 0.0);
    static std::vector<float> acc;  // guest render thread only
    float wacc[16] = {};
    bool ok = true;
    // Did the rigid world take the 8-tap boxcar this frame (vs the plain
    // pair-lerp)? The ROPA shape kernel below must match it exactly.
    bool rigid_world_boxcar = false;
    if (filter_w > 0.0005 && h.count >= 4) {
      constexpr int kTaps = 8;
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      // The taps first fold into one weight per ring pose (accum_at's lerp
      // expanded: p0 * (1 - a) * w + p1 * a * w; same bracket walk, same
      // size check), then every pose is accumulated ONCE. Neighbouring taps
      // share brackets, so this reads the palettes about half as often as
      // eight accum_at passes of two poses each did -- that loop was the
      // guest render thread's hottest native code.
      float slot_w[kRing] = {};
      bool slot_used[kRing] = {};
      for (int tap = 0; tap < kTaps; ++tap) {
        const double tt =
            std::min(play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kTaps,
                     h.ring[h.newest].t);
        int hi = h.newest;
        int lo = (h.newest + kRing - 1) % kRing;
        for (int step = 1; step < h.count - 1; ++step) {
          if (h.ring[lo].t <= tt) {
            break;
          }
          hi = lo;
          lo = (lo + kRing - 1) % kRing;
        }
        const double span = std::max(h.ring[hi].t - h.ring[lo].t, 0.0005);
        const float a = float(std::clamp((tt - h.ring[lo].t) / span, 0.0, 1.0));
        slot_used[lo] = true;
        slot_used[hi] = true;
        slot_w[lo] += (1.0f - a) * (1.0f / kTaps);
        slot_w[hi] += a * (1.0f / kTaps);
      }
      for (int s = 0; s < kRing && ok; ++s) {
        ok = !slot_used[s] || !skinned || h.ring[s].b.size() == item.bones.size();
      }
      for (int s = 0; s < kRing && ok; ++s) {
        const float ws = slot_w[s];
        if (ws == 0.0f) {
          continue;
        }
        if (skinned) {
          const float* __restrict src = h.ring[s].b.data();
          float* __restrict dst = acc.data();
          // Live rows only (DrawItem::bone_rows): the dead tail stays 0,
          // which is what a served palette holds there anyway.
          const size_t n = item.bone_rows != 0
                               ? std::min<size_t>(acc.size(), size_t(item.bone_rows) * 12)
                               : acc.size();
          for (size_t i = 0; i < n; ++i) {
            dst[i] += src[i] * ws;
          }
        } else {
          for (int i = 0; i < 16; ++i) {
            wacc[i] += h.ring[s].w[i] * ws;
          }
        }
      }
    } else {
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      ok = accum_at(play_e, 1.0f, acc.data(), wacc);
    }
    if (!ok) {
      if (trk != nullptr) {
        trk->raw_render = 2;
      }
      continue;  // palette-size mismatch in the window: raw pose this frame
    }
    if (trk != nullptr) {
      trk->box = filter_w > 0.0005 && h.count >= 4 ? 1 : 0;
    }
    // Fast-spinning bones (skateboard wheels: hundreds of degrees inside
    // the window) COLLAPSE under componentwise averaging; the rotation
    // entries cancel toward zero and the wheel shrinks into the truck /
    // sinks into the ground. Guard per bone: if the averaged 3x3's
    // Frobenius norm dropped versus a raw sample's, the window spans too
    // much rotation; fall back to the plain TWO-ADJACENT-SAMPLE lerp at
    // the playback point for that bone (the pre-filter behavior: samples
    // ~7.5 ms apart lerp with only a few % shrink, and rotation +
    // translation come from the SAME pose pair). Failed alternatives, do
    // not revisit: whole-bone nearest-sample snap = wheels lag the filtered
    // board and snap to catch up; nearest ROTATION + averaged TRANSLATION =
    // wheels ORBIT the axle ~10 cm (palette affines are model->world; the
    // rotation pivots about the MODEL origin and the translation carries
    // the axle-pivot compensation; the pair must stay consistent). Slow
    // bones (the judder sources) pass untouched.
    int b_lo = h.newest, b_hi = h.newest;
    {
      int hi2 = h.newest;
      int lo2 = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo2].t <= play_e) {
          break;
        }
        hi2 = lo2;
        lo2 = (lo2 + kRing - 1) % kRing;
      }
      b_lo = lo2;
      b_hi = hi2;
    }
    const DynPose& q0 = h.ring[b_lo];
    const DynPose& q1 = h.ring[b_hi];
    const double bspan = std::max(q1.t - q0.t, 0.0005);
    const float ba = float(std::clamp((play_e - q0.t) / bspan, 0.0, 1.0));
    if (trk != nullptr) {
      trk->ba = ba;
    }
    if (skinned) {
      if (q0.b.size() == item.bones.size() && q1.b.size() == item.bones.size()) {
        // Pass 1: flag collapsed bones (the churning staged-constant rows
        // past the real skeleton, e.g. rows 53..83 of the 84-row character
        // bank, flag every frame too; they are unreferenced by vertices,
        // so rewriting them is harmless).
        static std::vector<uint8_t> s_collapsed;  // guest render thread only
        const size_t nbones =
            (item.bone_rows != 0 ? std::min<size_t>(acc.size(), size_t(item.bone_rows) * 12)
                                 : acc.size()) /
            12;
        s_collapsed.assign(nbones, 0);
        size_t ncollapsed = 0;
        for (size_t b = 0; b < nbones; ++b) {
          const size_t bi = b * 12;
          double fa = 0.0, fr = 0.0;
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
              const float av = acc[bi + r * 4 + c];
              const float rv = q1.b[bi + r * 4 + c];
              fa += double(av) * av;
              fr += double(rv) * rv;
            }
          }
          if (fa < fr * 0.94) {  // norm ratio < ~0.97: rotation collapsed
            s_collapsed[b] = 1;
            ++ncollapsed;
          }
        }
        if (ncollapsed > 0) {
          // Collapsed bone (rotating too fast inside the window to
          // average, spinning wheels, and briefly fast-swinging limbs).
          // Base fallback: plain adjacent-sample lerp at the playback point
          // (R and t from the SAME pose pair, no orbit; ~7.5 ms spacing
          // keeps shrink to a few %). Bones whose mesh vertices yield a
          // skin-weighted BIND-SPACE CENTROID get the exact PIVOT-BOXCAR
          // form instead: render the orthonormalized lerp rotation
          // translated so the centroid rides the same boxcar path as every
          // other filtered bone: t_out = tbar + Rbar*c - Ro*c, where
          // Rbar/tbar is the boxcar affine already in acc[] (its action on
          // ANY fixed bind-space point IS that point's smoothed path). The
          // centroid is the wheel's geometric center (a wheel is symmetric
          // about its axle; verified in capture: mesh
          // centroids match the motion-solved spin pivot to a millimeter),
          // so the wheel is exact at its center and sub-mm across its
          // ~4 cm extent; a limb pins its own centroid to its smoothed
          // path (full-norm rotation, no lerp shrink). Offline-validated:
          // wheel-vs-deck
          // high-frequency wobble 1.3 -> 0.02 cm rms at constant speed /
          // 1.0 -> 0.29 cm at 25 m/s with speed changes, better than the
          // game's own 60 Hz output (0.05-0.56 cm). DO NOT replace the
          // centroid with a motion-ESTIMATED pivot: every estimator shape
          // tried (shared / per-window velocity elimination, quadratic
          // motion models, theta gates, consistency resets, axis
          // projection) was noisier than the artifact it fixed.
          // Junk palette rows
          // (unreferenced by vertices) get no centroid weight and keep the
          // lerp.
          ensure_cen(h, nbones);
          uint64_t pivot_upgraded = 0;
          for (size_t b = 0; b < nbones; ++b) {
            if (!s_collapsed[b]) {
              continue;
            }
            const size_t bi = b * 12;
            float lp[12];
            for (int i = 0; i < 12; ++i) {
              lp[i] = q0.b[bi + i] + (q1.b[bi + i] - q0.b[bi + i]) * ba;
            }
            const bool upgraded = [&]() {
              if (filter_w <= 0.0005 || b >= h.cen.size() ||
                  h.cen[b][0] <= 0.5f) {
                return false;  // no centroid (junk row / undecodable VB)
              }
              const float c[3] = {h.cen[b][1], h.cen[b][2], h.cen[b][3]};
              const float* bar = acc.data() + bi;
              // UNDERSAMPLED spin: traffic wheels at speed turn > 90 deg
              // between adjacent ~8 ms samples; the pair-lerp rotation is
              // then meaningless, and with a car wheel's bind centroid
              // meters from the model origin the (Rbar - Ro)*c pin wobbled
              // the wheel ~1 m around its well (bone-signal measured; the
              // board's slow wheels never hit this). When the pair spans
              // more than ~50 deg (Frobenius dot of the 3x3s: trace(R0^T
              // R1) = 1 + 2cos(theta) for rotations), render the pair's
              // NEWEST rotation instead; spin phase snaps once per
              // sample, invisible at those rev rates, while the centroid
              // pin still holds the wheel exactly on its smoothed path.
              double pair_tr = 0.0;
              for (int i = 0; i < 12; ++i) {
                if ((i & 3) == 3) {
                  continue;  // translation column
                }
                pair_tr += double(q0.b[bi + i]) * q1.b[bi + i];
              }
              const bool snap_spin = pair_tr < 2.28;  // 1 + 2cos(50 deg)
              const float* rsrc = snap_spin ? q1.b.data() + bi : lp;
              // Orthonormalize the source rotation (rows): recovers the
              // full-norm midpoint rotation from the lerp's shrunk chord
              // (or just cleans up the raw newest sample in snap mode).
              double r0[3] = {rsrc[0], rsrc[1], rsrc[2]};
              double r1[3] = {rsrc[4], rsrc[5], rsrc[6]};
              double n0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
              if (n0 < 1e-4) {
                return false;
              }
              for (double& v : r0) v /= n0;
              const double d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
              for (int i = 0; i < 3; ++i) r1[i] -= d01 * r0[i];
              const double n1 =
                  std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
              if (n1 < 1e-4) {
                return false;
              }
              for (double& v : r1) v /= n1;
              const double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1],
                                    r0[2] * r1[0] - r0[0] * r1[2],
                                    r0[0] * r1[1] - r0[1] * r1[0]};
              const double* rows[3] = {r0, r1, r2};
              // t_out = tbar + Rbar*c - Ro*c: the pivot lands exactly on
              // the boxcar path (in phase with the deck and every other
              // filtered bone), spin phase comes from the lerp pair.
              for (int r = 0; r < 3; ++r) {
                double rb = 0.0, ro = 0.0;
                for (int c2 = 0; c2 < 3; ++c2) {
                  rb += double(bar[r * 4 + c2]) * c[c2];
                  ro += rows[r][c2] * c[c2];
                }
                acc[bi + r * 4 + 0] = float(rows[r][0]);
                acc[bi + r * 4 + 1] = float(rows[r][1]);
                acc[bi + r * 4 + 2] = float(rows[r][2]);
                acc[bi + r * 4 + 3] = float(double(bar[r * 4 + 3]) + rb - ro);
              }
              return true;
            }();
            if (upgraded) {
              ++pivot_upgraded;
            } else {
              for (int i = 0; i < 12; ++i) {
                acc[bi + i] = lp[i];
              }
            }
          }
          // Sparse telemetry: how many bones ride the pivot upgrade.
          static uint64_t s_pivot_frames = 0, s_pivot_bones = 0;
          s_pivot_bones += pivot_upgraded;
          if (++s_pivot_frames % 3000 == 0 && s_pivot_bones > 0) {
            REXLOG_INFO("native-scene pivot: {} bone-upgrades / 3000 frames",
                        s_pivot_bones);
            s_pivot_bones = 0;
          }
        }
      }
      // The dead tail (past bone_rows) keeps the item's own (zero) rows.
      const size_t live = item.bone_rows != 0
                              ? std::min<size_t>(acc.size(), size_t(item.bone_rows) * 12)
                              : acc.size();
      std::copy(acc.begin(), acc.begin() + ptrdiff_t(live), item.bones.begin());
    } else {
      double fa = 0.0, fr = 0.0;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          fa += double(wacc[r * 4 + c]) * wacc[r * 4 + c];
          fr += double(q1.w[r * 4 + c]) * q1.w[r * 4 + c];
        }
      }
      if (fa < fr * 0.94) {
        for (int i = 0; i < 16; ++i) {
          item.world[i] = q0.w[i] + (q1.w[i] - q0.w[i]) * ba;
        }
      } else {
        std::memcpy(item.world, wacc, sizeof(wacc));
        rigid_world_boxcar = filter_w > 0.0005 && h.count >= 4;
      }
      // Post-interpolation identity anchor (ropa garments): the published
      // world and the guest draws are proven bit-exact against the owner
      // entity, so a rendered world grossly diverged from the entity here
      // can only be ring corruption (mispaired claim / stale pose blended
      // in). Motion smoothing legitimately offsets by centimeters; the
      // thresholds (2.5 m translation, ~60 deg rotation) only catch the
      // detached/perpendicular class. Repair = the entity's live matrix
      // (raw for one frame beats a detached garment), and the log names
      // the ring state for diagnosis.
      if (item.ropa) {
        float ent_rows[12];
        if (skate3::native_entity::ReadEntityWorldRows(base, item.ctx,
                                                       ent_rows)) {
          const float dx = item.world[12] - ent_rows[3];
          const float dy = item.world[13] - ent_rows[7];
          const float dz = item.world[14] - ent_rows[11];
          float rot = 0.0f;
          for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
              const float d = item.world[i * 4 + j] - ent_rows[j * 4 + i];
              rot += d * d;
            }
          }
          if (dx * dx + dy * dy + dz * dz > 2.5f * 2.5f || rot > 1.5f) {
            static std::atomic<uint32_t> s_fix{0};
            const uint32_t ln = s_fix.fetch_add(1, std::memory_order_relaxed);
            if (ln < 16 || (ln & 255u) == 0) {
              REXLOG_INFO(
                  "native-scene: ropa interp DETACH repaired mesh={:08X} "
                  "ctx={:08X} dt=({:.2f},{:.2f},{:.2f}) rot2={:.2f} ba={:.2f} "
                  "ring[n={} q0t=({:.1f},{:.1f},{:.1f}) "
                  "q1t=({:.1f},{:.1f},{:.1f})] (n={})",
                  item.mesh, item.ctx, dx, dy, dz, rot, ba, h.count, q0.w[12],
                  q0.w[13], q0.w[14], q1.w[12], q1.w[13], q1.w[14], ln);
            }
            for (int i = 0; i < 3; ++i) {
              for (int j = 0; j < 3; ++j) {
                item.world[i * 4 + j] = ent_rows[j * 4 + i];
              }
              item.world[i * 4 + 3] = 0.0f;
              item.world[12 + i] = ent_rows[i * 4 + 3];
            }
            item.world[15] = 1.0f;
            rigid_world_boxcar = false;
            if (trk != nullptr) {
              trk->detach = 1;
            }
          }
        }
      }
    }
    // ROPA shape pairing: express the EXACT temporal kernel the garment's
    // world (and the body's bones) were just evaluated with as weights over
    // the decoded shape generations (DynDecodeJob seq). The previous plain
    // 2-generation lerp removed the stepping but reconstructed the 60 Hz
    // limb signal SHARPLY against the 50 ms boxcar-rounded body, two
    // different frequency responses to the same signal, diverging at every
    // piecewise-linear corner by an excursion that scales with the per-tick
    // step size (the residual tee jelly, worse at a 60 fps guest cap;
    // emulated applies no filter at all and shows none). The kernel follows
    // the world's LIVE decision: boxcar taps only when the world actually
    // took the boxcar this frame (not the spin-collapse pair-lerp
    // fallback), so cloth and body always ride the same filter.
    if (item.ropa && !skinned) {
      item.shape_count = 0;
      const int bias = std::clamp<int>(
          REXCVAR_GET(skate3_native_render_scene_ropa_bias), -2, 2);
      // Bracket the ring at tt exactly like accum_at (same walk, same span
      // clamp) and return the bracketing pair's shape generations + alpha.
      const auto shape_at = [&](double tt, uint64_t& s0, uint64_t& s1,
                                float& a) {
        int hi = h.newest;
        int lo = (h.newest + kRing - 1) % kRing;
        for (int step = 1; step < h.count - 1; ++step) {
          if (h.ring[lo].t <= tt) {
            break;
          }
          hi = lo;
          lo = (lo + kRing - 1) % kRing;
        }
        // Live pairing trim (skate3_native_render_scene_ropa_bias): step
        // the shape source N ring poses fresher/older than the bracket.
        int blo = lo, bhi = hi;
        for (int b = 0; b < bias; ++b) {
          if (bhi == h.newest) break;
          blo = bhi;
          bhi = (bhi + 1) % kRing;
        }
        for (int b = 0; b > bias; --b) {
          bhi = blo;
          blo = (blo + kRing - 1) % kRing;
        }
        const DynPose& p0 = h.ring[blo];
        const DynPose& p1 = h.ring[bhi];
        if (p0.shape_seq == 0 || p1.shape_seq == 0) {
          return false;
        }
        const double span = std::max(h.ring[hi].t - h.ring[lo].t, 0.0005);
        s0 = p0.shape_seq;
        s1 = p1.shape_seq;
        a = float(std::clamp((tt - h.ring[lo].t) / span, 0.0, 1.0));
        return true;
      };
      const auto add_gen = [&](uint64_t seq, float wgt) {
        if (wgt <= 0.0f) {
          return;
        }
        for (int k = 0; k < item.shape_count; ++k) {
          if (item.shape_seq[k] == seq) {
            item.shape_w[k] += wgt;
            return;
          }
        }
        if (item.shape_count < DrawItem::kShapeGens) {
          item.shape_seq[item.shape_count] = seq;
          item.shape_w[item.shape_count] = wgt;
          ++item.shape_count;
        }
        // Unreachable: the 8 taps contribute at most two distinct
        // generations each, and kShapeGens holds all 16.
      };
      if (rigid_world_boxcar &&
          REXCVAR_GET(skate3_native_render_scene_ropa_boxcar)) {
        constexpr int kShapeTaps = 8;  // == the body kernel's kTaps
        for (int tap = 0; tap < kShapeTaps; ++tap) {
          const double tt = std::min(
              play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kShapeTaps,
              h.ring[h.newest].t);
          uint64_t s0 = 0, s1 = 0;
          float a = 0.0f;
          if (shape_at(tt, s0, s1, a)) {
            add_gen(s0, (1.0f - a) / kShapeTaps);
            add_gen(s1, a / kShapeTaps);
          }
        }
      } else {
        uint64_t s0 = 0, s1 = 0;
        float a = 0.0f;
        if (shape_at(play_e, s0, s1, a)) {
          add_gen(s0, 1.0f - a);
          add_gen(s1, a);
        }
      }
      if (trk != nullptr) {
        for (int k = 0; k < item.shape_count; ++k) {
          trk->sw += item.shape_w[k];
        }
      }
    }
    if (bs_rec) {
      BoneSigAppend(1, key, now, play_e,
                    skinned ? item.bones.data() : item.world,
                    skinned ? uint32_t(item.bones.size()) : 16u);
    }
  }
  // char_track emit: two lines per frame (body, then garment). rp = the
  // pre-interp capture, fp = the post-interp (rendered) reference position,
  // off = |garment - body| rendered offset; its frame-to-frame stability
  // IS the artifact metric.
  if (trk_on && (trk_shirt.idx >= 0 || trk_body.idx >= 0)) {
    const auto fpos = [&](const CharTrack& t, const DrawItem*& it,
                          float p[3]) {
      it = nullptr;
      p[0] = p[1] = p[2] = 0.0f;
      if (t.idx < 0 || size_t(t.idx) >= scene.items.size()) {
        return;
      }
      it = &scene.items[size_t(t.idx)];
      if (it->skinned && it->bones.size() >= 12) {
        p[0] = it->bones[3];
        p[1] = it->bones[7];
        p[2] = it->bones[11];
      } else {
        p[0] = it->world[12];
        p[1] = it->world[13];
        p[2] = it->world[14];
      }
    };
    const DrawItem* si = nullptr;
    const DrawItem* bi = nullptr;
    float sp[3], bp[3];
    fpos(trk_shirt, si, sp);
    fpos(trk_body, bi, bp);
    if (bi != nullptr) {
      const CharTrack& t = trk_body;
      REXLOG_INFO(
          "char-track f={} body mesh={:08X} ctx={:08X} fam={} src={} cb={} "
          "rp=({:.3f},{:.3f},{:.3f}) fp=({:.3f},{:.3f},{:.3f}) ring[n={} "
          "age={:.1f} per={:.1f} ing={} rst={} clm={} hold={} raw={}] "
          "ba={:.2f} box={} play={:.1f}",
          s_frame, bi->mesh, bi->ctx, bi->char_family, bi->dbg_src,
          bi->caster_bank ? 1 : 0, t.raw[0], t.raw[1], t.raw[2], bp[0], bp[1],
          bp[2], t.ring_n, t.age_ms, t.per_ms, t.ing, t.rst, t.clm, t.hold,
          t.raw_render, t.ba, t.box, (now - g_smooth_play) * 1e3);
    }
    if (si != nullptr) {
      const CharTrack& t = trk_shirt;
      float ent_t[3] = {0.0f, 0.0f, 0.0f};
      int ent_ok = 0;
      if (si->ctx != 0) {
        float rows[12];
        if (skate3::native_entity::ReadEntityWorldRows(base, si->ctx, rows)) {
          ent_t[0] = rows[3];
          ent_t[1] = rows[7];
          ent_t[2] = rows[11];
          ent_ok = 1;
        }
      }
      const float off =
          bi != nullptr
              ? std::sqrt((sp[0] - bp[0]) * (sp[0] - bp[0]) +
                          (sp[1] - bp[1]) * (sp[1] - bp[1]) +
                          (sp[2] - bp[2]) * (sp[2] - bp[2]))
              : -1.0f;
      REXLOG_INFO(
          "char-track f={} shirt mesh={:08X} ctx={:08X} fam={} src={} skn={} "
          "cb={} rt={} rp=({:.3f},{:.3f},{:.3f}) fp=({:.3f},{:.3f},{:.3f}) "
          "ent=({:.3f},{:.3f},{:.3f},{}) ring[n={} age={:.1f} per={:.1f} "
          "ing={} rst={} clm={} hold={} raw={}] ba={:.2f} box={} sc={} "
          "sw={:.2f} det={} off={:.3f}",
          s_frame, si->mesh, si->ctx, si->char_family, si->dbg_src,
          si->skinned && !si->bones.empty() ? 1 : 0, si->caster_bank ? 1 : 0,
          si->retained ? 1 : 0, t.raw[0], t.raw[1], t.raw[2], sp[0], sp[1],
          sp[2], ent_t[0], ent_t[1], ent_t[2], ent_ok, t.ring_n, t.age_ms,
          t.per_ms, t.ing, t.rst, t.clm, t.hold, t.raw_render, t.ba, t.box,
          si->shape_count, t.sw, t.detach, off);
    }
  }
  // ---- Deck (skateboard) contact-shadow placement --------------------
  // The guest publishes the board's world matrix from its own
  // GetDeckMatrix, on the sim's clock. Handing that raw matrix to the
  // renderer put the decal AHEAD of the skater at speed: the board itself
  // renders from the re-timed pose above, so the shadow has to ride the
  // same clock. Same ring, same playback point, same boxcar as a rigid
  // entity -- the board is a 60 Hz entity, so no per-entity period offset.
  {
    constexpr int kDeckRing = 24;
    struct DeckPose {
      double t = 0.0;
      float m[16] = {};
    };
    static DeckPose s_ring[kDeckRing];
    static int s_count = 0;
    static int s_newest = 0;
    skate3::deck_shadow::DeckPlacement dp = {};
    if (!skate3::deck_shadow::GetPlacement(0, dp)) {
      scene.deck_valid = false;
      s_count = 0;
    } else {
      const DeckPose& latest = s_ring[s_newest];
      bool changed = s_count == 0;
      for (int i = 0; i < 16 && !changed; ++i) {
        changed = std::abs(latest.m[i] - dp.world[i]) > 1e-6f;
      }
      // A discontinuity (respawn, bail reset, camera cut) must not lerp
      // across the map; restart the ring instead.
      if (s_count > 0) {
        const float dx = dp.world[12] - latest.m[12];
        const float dy = dp.world[13] - latest.m[13];
        const float dz = dp.world[14] - latest.m[14];
        if (dx * dx + dy * dy + dz * dz > 25.0f) {
          s_count = 0;
          changed = true;
        }
      }
      // Stamp with the camera sampler's latest sim tick, exactly as the
      // entity rings above do: the board's matrix changed on that tick, and
      // stamping with the FRAME time instead aliases against the sim rate
      // once the render loop is paced. That alias is what made the decal
      // trail the board at low frame rates -- the board renders from a
      // sim-tick-stamped pose while the decal was stamped up to a whole
      // render frame later, so evaluating both at the same playback point
      // handed the decal the older pose.
      const double stamp =
          (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02) ? g_latest_cam_tick
                                                                       : now;
      if (changed && !(s_count > 0 && stamp - latest.t < kMinDynIngestSpacing)) {
        s_newest = s_count == 0 ? 0 : (s_newest + 1) % kDeckRing;
        s_ring[s_newest].t = stamp;
        std::memcpy(s_ring[s_newest].m, dp.world, sizeof(dp.world));
        s_count = std::min(s_count + 1, kDeckRing);
      }
      const double filter_w = std::clamp(
          REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0,
          200.0) * 1e-3;
      const auto deck_at = [&](double tt, float weight, float out[16]) {
        int hi = s_newest;
        int lo = (s_newest + kDeckRing - 1) % kDeckRing;
        for (int step = 1; step < s_count - 1; ++step) {
          if (s_ring[lo].t <= tt) {
            break;
          }
          hi = lo;
          lo = (lo + kDeckRing - 1) % kDeckRing;
        }
        const DeckPose& p0 = s_ring[lo];
        const DeckPose& p1 = s_ring[hi];
        const double span = std::max(p1.t - p0.t, 0.0005);
        const float a = float(std::clamp((tt - p0.t) / span, 0.0, 1.0));
        for (int i = 0; i < 16; ++i) {
          out[i] += (p0.m[i] + (p1.m[i] - p0.m[i]) * a) * weight;
        }
      };
      float m[16] = {};
      if (s_count >= 4 && filter_w > 0.0005) {
        constexpr int kTaps = 8;
        for (int tap = 0; tap < kTaps; ++tap) {
          deck_at(std::min(g_smooth_play - filter_w * 0.5 +
                               (tap + 0.5) * filter_w / kTaps,
                           s_ring[s_newest].t),
                  1.0f / kTaps, m);
        }
      } else if (s_count >= 2) {
        deck_at(g_smooth_play, 1.0f, m);
      } else {
        std::memcpy(m, dp.world, sizeof(m));
      }
      // Componentwise averaging shrinks the basis while the board spins
      // (the same effect the bone path guards against); the decal's
      // footprint is 1/|axis|, so renormalize the three axes to keep it
      // the size the guest's c4.xy sets. Position is unaffected.
      for (int r = 0; r < 3; ++r) {
        float* a = m + r * 4;
        const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (len > 1e-6f) {
          a[0] /= len;
          a[1] /= len;
          a[2] /= len;
        }
      }
      std::memcpy(scene.deck_world, m, sizeof(m));
      scene.deck_valid = true;
    }
  }
  // Prune entities not seen recently (map otherwise grows with streaming).
  if (s_hist.size() > 2048) {
    for (auto it = s_hist.begin(); it != s_hist.end();) {
      it = it->second.seen + 60 < s_frame ? s_hist.erase(it) : std::next(it);
    }
  }
}

// Publish the frame's 2D overlay draws (BuildFrameScene, before any early
// return; menu and empty frames still carry 2D). The inline-ring vertex
// payloads are complete by frame end; convert them to little-endian and
// expand quad lists into triangle lists so the render side stays trivial.
void Publish2dDraws(uint8_t* base) {
  std::vector<Draw2d> frame_2d;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    frame_2d.swap(g_frame_2d);
  }
  static thread_local std::vector<uint8_t> scratch_2d;
  std::vector<Draw2d> published;
  published.reserve(frame_2d.size());
  for (Draw2d& d : frame_2d) {
    // OFFSCREEN COMPOSITION draws: bracket bits carrying ONLY SimpleDraw
    // (0x20) / font (0x10) with none of the screen-pass brackets (bit 0
    // FrontEndManager::Render2D, bit 1 AptMovieIntegration, bit 2
    // DrawRenderingUnit, bit 3 the HUD render-to-texture pass) are the
    // game's internal render-target helpers, not screen UI; the
    // skater-portrait generator composes the card through bare SimpleDraw
    // quads in the TARGET's coordinate space (traced:
    // flags=20 fullscreen 1152x640 postfx blit + centered
    // 324x640 portrait compose quads on textures 03f47054/03f3f054,
    // replayed on screen they were the centered "poster" flash on every
    // menu entry / skater switch, sampling whatever the resolve arena
    // still held). Every real UI draw in gameplay, pause and the frontend
    // carries at least one screen bracket (observed flags 0d/19/29/2b/2d).
    // flags == 0 (unbracketed boot/loading capture) keeps its own gate.
    // The one exception is the in-game browser: its page is EAWebKit's
    // 1280x720 surface drawn by the browser screen (FE 60) itself as a single
    // bare SimpleDraw quad (flags=20, 24-byte pos4+uv, identity c4..c7; the
    // only flags=20 draw in an all-draws capture of the Team Finder page), not
    // through APT, so while that screen is up bare-SimpleDraw draws are screen
    // UI and replay.
    if (d.flags != 0 && (d.flags & 0x0Fu) == 0 &&
        !g_browser_screen_open.load(std::memory_order_relaxed)) {
      static std::atomic<uint32_t> s_offscreen_2d{0};
      const uint32_t n = s_offscreen_2d.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 2047u) == 0) {
        REXLOG_INFO(
            "native-scene: offscreen 2D compose draw dropped (flags={:02x} "
            "tex={:08x} count={}) (n={})",
            d.flags, d.fetch[1], d.count, n);
      }
      g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint32_t bytes = d.count * d.stride;
    scratch_2d.resize(bytes);
    if (!GuestTryCopy(scratch_2d.data(), base + d.addr, bytes)) {
      continue;
    }
    // Guest dwords are big-endian.
    for (size_t i = 0; i + 4 <= scratch_2d.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, scratch_2d.data() + i, 4);
      v = BSwap32(v);
      std::memcpy(scratch_2d.data() + i, &v, 4);
    }
    // Verified capture-side vertex layouts, all normalized to one 28-byte
    // renderer layout {float4 pos, float2 uv, u32 rgba}:
    //   24-byte {float4 pos, float2 uv}          - APT elements
    //   20-byte {float3 pos, float2 uv}          - glyph text (bit 4)
    //   20-byte {float4 pos, u32 color}          - SimpleDraw untextured
    //   28-byte {float4 pos, u32 color, float2 uv} - SimpleDraw textured
    //   16-byte {float4 pos}                     - SimpleDraw solid fill
    //     (color rides in VS c8 = m[8]; the popup panel strips, Rewards
    //     title bar / teal body / tan footer, are these)
    //   32-byte {float4 pos, float4 color}       - SimpleDraw float-colour
    //     quads (QuadListColoured: DrawParameters::Draw writes the position
    //     vector's 4 words then the colour vector's; the challenge-start
    //     screen fade, sub_827FEFD0, is a fullscreen one of these)
    // (SimpleDraw = bit 5; its DrawParameters ctor orders colours before
    // texcoords.)
    // Identify which bitmap font a text batch draws from, ONCE per batch
    // (not per vertex): the quads' uvs ARE the .bmpFont's glyph cells
    // normalised, so the first quad's rect pins the font down. Guest atlas
    // addresses move between runs and the four futura atlases are all
    // 512x512, so neither address nor size could do it. The cell rects are
    // unique across the embedded fonts (the generator asserts it) but NOT
    // across the game's others, so a cell match is confirmed against the
    // guest atlas's payload sum below. The renderer then serves the
    // higher-resolution replacement for exactly this draw, and anything
    // unmatched keeps the game's own texture.
    if ((d.flags & 0x10u) != 0 && d.stride == 20 && d.count >= 4) {
      // Memoised per atlas, direct-mapped on the fetch words the resolver
      // keys on anyway: the match walks every cell of every font, which is
      // fine once per atlas but not once per batch.
      struct FontMemo {
        uint32_t w0, w1;
        int32_t id;
        bool set;
      };
      // Indexed by the atlas ADDRESS (fetch word 1 bits 12+). The low bits
      // are format/endian, the same for every font atlas: indexing on them
      // put all fonts in one slot, and two fonts in a frame re-summed a
      // whole atlas payload from guest memory per text batch.
      static thread_local FontMemo memo[64];
      FontMemo& m = memo[((d.fetch[1] >> 12) * 0x9E3779B1u) >> 26];
      if (m.set && m.w0 == d.fetch[0] && m.w1 == d.fetch[1]) {
        d.font_id = m.id;
      } else {
        // scratch_2d is already dword-byteswapped above, so the uv floats
        // read native-endian here.
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (uint32_t k = 0; k < 4; ++k) {
          const uint8_t* q = scratch_2d.data() + size_t(k) * d.stride;
          float uu = 0.0f;
          float vv = 0.0f;
          std::memcpy(&uu, q + 12, 4);
          std::memcpy(&vv, q + 16, 4);
          u0 = std::min(u0, uu);
          u1 = std::max(u1, uu);
          v0 = std::min(v0, vv);
          v1 = std::max(v1, vv);
        }
        // Only a real cell decides, and only a real cell is remembered: a
        // degenerate quad (the game emits one per space) would otherwise
        // memoise "no font" for an atlas that does have one.
        if (u1 > u0 && v1 > v0) {
          int id = MatchFontAtlas(u0, v0, u1, v1);
          // The cell only names a CANDIDATE: futuraglow reuses
          // futuraheavy's layout (166 shared cells), and the glow pass
          // served the crisp white heavy replacement drew a second hard
          // glyph where the game draws a soft pale-blue halo -- the
          // "whiter, sharper" native text. Confirm against the guest
          // atlas's own bytes: their sum is unaffected by tiling and endian
          // swaps, and separates every font that shares cells.
          if (id >= 0) {
            const uint32_t size = FontAtlasPayloadSize(id);
            static thread_local std::vector<uint8_t> payload;
            payload.resize(size);
            const uint32_t addr = 0xA0000000u | (d.fetch[1] & 0xFFFFF000u);
            uint64_t sum = 0;
            const bool read =
                size != 0 && GuestTryCopy(payload.data(), base + addr, size);
            if (read) {
              for (uint8_t b : payload) {
                sum += b;
              }
            }
            const bool ok = read && FontAtlasPayloadMatches(id, sum);
            static std::atomic<uint32_t> s_font_logs{0};
            if (s_font_logs.fetch_add(1, std::memory_order_relaxed) < 32) {
              REXLOG_INFO(
                  "native-scene: font atlas {:08x} cells match '{}', payload "
                  "sum {} -> {}",
                  addr, FontAtlasName(id), read ? int64_t(sum) : int64_t(-1),
                  ok ? "replaced" : "kept the game's atlas");
            }
            if (!ok) {
              id = -1;
            }
          }
          d.font_id = id;
          m = {d.fetch[0], d.fetch[1], d.font_id, true};
        }
      }
    }
    {
      static thread_local std::vector<uint8_t> norm_2d;
      norm_2d.resize(size_t(d.count) * 28);
      const float one = 1.0f;
      const float zero = 0.0f;
      const uint32_t white = 0xFFFFFFFFu;
      const bool font = (d.flags & 0x10u) != 0;
      const bool simple = (d.flags & 0x20u) != 0;
      bool ok = true;
      for (uint32_t v = 0; v < d.count && ok; ++v) {
        uint8_t* dst = norm_2d.data() + size_t(v) * 28;
        const uint8_t* src = scratch_2d.data() + size_t(v) * d.stride;
        // Unbracketed captures (d.flags == 0, native loading/boot frames)
        // take the SimpleDraw layout readings for 16/28: EA's inline-quad
        // helpers order color before texcoords everywhere.
        if (d.stride == 16 && (simple || d.flags == 0)) {  // pos4, untextured
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, &zero, 4);
          std::memcpy(dst + 20, &zero, 4);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 24) {  // APT: pos4 + uv (with or without the
          // SimpleDraw bracket; the compass needle/icons are 24-byte
          // quads issued through SimpleDraw::Draw inside the HUD pass,
          // same simpledraw_SimpleDrawUVSC shader as plain APT elements;
          // requiring !simple here dropped them, a regression from adding
          // bracket bit 5)
          std::memcpy(dst, src, 24);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 20 && font) {  // glyphs: pos3 + uv
          std::memcpy(dst, src, 12);
          std::memcpy(dst + 12, &one, 4);
          std::memcpy(dst + 16, src + 12, 8);
          std::memcpy(dst + 24, &white, 4);
        } else if (d.stride == 20 && simple) {  // SimpleDraw: pos4 + color
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, &zero, 4);
          std::memcpy(dst + 20, &zero, 4);
          // The dword byteswap reversed the color's guest byte order
          // (r,g,b,a); restore it for R8G8B8A8_UNORM.
          dst[24] = src[19];
          dst[25] = src[18];
          dst[26] = src[17];
          dst[27] = src[16];
        } else if (d.stride == 32 && simple) {  // pos4 + float4 color
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, &zero, 4);
          std::memcpy(dst + 20, &zero, 4);
          for (int c = 0; c < 4; ++c) {
            float f;
            std::memcpy(&f, src + 16 + c * 4, 4);
            f = std::clamp(f, 0.0f, 1.0f);
            dst[24 + c] = uint8_t(f * 255.0f + 0.5f);
          }
        } else if (d.stride == 28 && (simple || d.flags == 0)) {  // pos4+color+uv
          std::memcpy(dst, src, 16);
          std::memcpy(dst + 16, src + 20, 8);
          dst[24] = src[19];
          dst[25] = src[18];
          dst[26] = src[17];
          dst[27] = src[16];
        } else {
          ok = false;
        }
      }
      if (!ok) {
        g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (d.stride == 16 || d.stride == 32) {
        // Untextured fill: the captured fetch words are leftover state from
        // the previous textured draw; zero them so the replay binds the
        // white texture instead of tinting the fill with a stale texel.
        std::memset(d.fetch, 0, sizeof(d.fetch));
      }
      scratch_2d = norm_2d;
      d.src_stride = d.stride;
      d.stride = 28;
    }
    if (d.prim == 13 && d.count % 4 == 0) {
      // Quad list -> triangle list (v0,v1,v2)(v0,v2,v3).
      const uint32_t quads = d.count / 4;
      d.verts.resize(size_t(quads) * 6 * d.stride);
      static constexpr uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
      for (uint32_t q = 0; q < quads; ++q) {
        for (uint32_t t = 0; t < 6; ++t) {
          std::memcpy(d.verts.data() + (size_t(q) * 6 + t) * d.stride,
                      scratch_2d.data() + (size_t(q) * 4 + kOrder[t]) * d.stride,
                      d.stride);
        }
      }
      d.prim = 4;
      d.count = quads * 6;
    } else if (d.prim == 6 && d.count >= 3) {
      // Triangle STRIP -> triangle list, odd triangles winding-flipped:
      // (v0,v1,v2) (v2,v1,v3) (v2,v3,v4) ...
      //
      // sk83_na_m is a debug build and its debug overlay draws each menu row's
      // background box as a 4-vertex strip. Retail's UI never used this
      // topology on the 2D path, so it fell through to the bare `continue`
      // below and every box was discarded before reaching the renderer -- the
      // text drew, the boxes behind it did not.
      //
      // Expanded here rather than passed through because the replay's topology
      // map only special-cases prim 5; a 6 arriving there would draw as a
      // list. Treating it as a FAN instead ((v0,vi,vi+1)) puts the second
      // triangle on the wrong corner and each box renders with a diagonal
      // wedge missing from one side.
      const uint32_t tris = d.count - 2;
      d.verts.resize(size_t(tris) * 3 * d.stride);
      for (uint32_t t = 0; t < tris; ++t) {
        uint32_t idx[3];
        if ((t & 1u) != 0) {
          idx[0] = t + 1;
          idx[1] = t;
          idx[2] = t + 2;
        } else {
          idx[0] = t;
          idx[1] = t + 1;
          idx[2] = t + 2;
        }
        for (uint32_t k = 0; k < 3; ++k) {
          std::memcpy(d.verts.data() + (size_t(t) * 3 + k) * d.stride,
                      scratch_2d.data() + size_t(idx[k]) * d.stride, d.stride);
        }
      }
      d.prim = 4;
      d.count = tris * 3;
    } else if (d.prim == 4 || d.prim == 5) {
      d.verts.assign(scratch_2d.begin(), scratch_2d.end());
    } else {
      continue;
    }
    // BIG-QUAD TRACER (reported symptom: an unrelated "mongo poster" texture
    // flashes portrait-shaped at screen center on team/import entry and
    // skater switches; idle F11s never catch it). Edge-triggered: log each
    // TEXTURED replayed 2D draw whose transformed extent covers >= 8% of
    // the 1280x720 APT space (the portrait box itself is ~8%), once per
    // texture base per 5 s window. Names the quad's texture / bracket /
    // geometry / timing for the fix.
    if (d.fetch[0] != 0) {
      float mn[2] = {1e9f, 1e9f};
      float mx[2] = {-1e9f, -1e9f};
      const float* m = d.consts;  // c0..c8; c4..c7 = 2D transform rows
      const uint32_t nv = d.count;
      for (uint32_t v = 0; v < nv; ++v) {
        const uint8_t* p = d.verts.data() + size_t(v) * d.stride;
        float pos[4];
        std::memcpy(pos, p, 16);
        for (int c = 0; c < 2; ++c) {
          const float t = pos[0] * m[16 + c] + pos[1] * m[20 + c] +
                          pos[2] * m[24 + c] + pos[3] * m[28 + c];
          mn[c] = std::min(mn[c], t);
          mx[c] = std::max(mx[c], t);
        }
      }
      const float w = mx[0] - mn[0];
      const float h = mx[1] - mn[1];
      if (w > 0.0f && h > 0.0f && w * h >= 0.08f * 1280.0f * 720.0f &&
          w * h < 1e8f) {
        static std::unordered_set<uint32_t> s_seen;
        static int64_t s_window_s = 0;
        static std::atomic<uint32_t> s_big{0};
        const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        if (now_s - s_window_s >= 5) {
          s_window_s = now_s;
          s_seen.clear();
        }
        if (s_seen.size() < 24 && s_seen.insert(d.fetch[1]).second) {
          REXLOG_DEBUG(
              "native-scene: BIG 2D quad tex=({:08x},{:08x},{:08x}) flags={:02x} "
              "src_stride={} count={} bbox=({:.0f},{:.0f})-({:.0f},{:.0f}) "
              "c8=({:.2f},{:.2f},{:.2f},{:.2f}) (n={})",
              d.fetch[0], d.fetch[1], d.fetch[2], d.flags, d.src_stride, d.count,
              mn[0], mn[1], mx[0], mx[1], m[32], m[33], m[34], m[35],
              s_big.fetch_add(1, std::memory_order_relaxed));
        }
      }
    }
    // TRANSITION-FADE TRACER: the game's screen-to-screen fades are
    // fullscreen SimpleDraw fills (RenderMan::FinalQuadFade ->
    // Draw_QuadListColoured, stride 16, color+ramping alpha in VS c8; the
    // fade color/alpha global sits at [0x83083C38]+31376+848/864, enable
    // byte +880). A mid-ramp alpha logged here proves the fade is captured
    // and replaying natively; if transitions still look like hard cuts
    // with these lines present, the problem is render pacing, not capture.
    // Rolling-capped: 6 lines per 2 s window.
    if (d.src_stride == 16 && d.count >= 4) {
      float alpha = d.consts[35];
      if (alpha > 0.02f && alpha < 0.98f) {
        float x0, y0, x1, y1;
        std::memcpy(&x0, d.verts.data(), 4);
        std::memcpy(&y0, d.verts.data() + 4, 4);
        std::memcpy(&x1, d.verts.data() + size_t(2) * d.stride, 4);
        std::memcpy(&y1, d.verts.data() + size_t(2) * d.stride + 4, 4);
        if (std::fabs(x1 - x0) >= 1200.0f && std::fabs(y1 - y0) >= 680.0f) {
          static std::atomic<uint32_t> s_fade_logs{0};
          static std::atomic<int64_t> s_fade_win{0};
          const int64_t now_s =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          int64_t win = s_fade_win.load(std::memory_order_relaxed);
          if (now_s - win >= 2 && s_fade_win.compare_exchange_strong(win, now_s)) {
            s_fade_logs.store(0, std::memory_order_relaxed);
          }
          if (s_fade_logs.fetch_add(1, std::memory_order_relaxed) < 6) {
            REXLOG_DEBUG(
                "native-scene: transition fade fill alpha={:.2f} "
                "rgb=({:.2f},{:.2f},{:.2f}) flags={:02x}",
                alpha, d.consts[32], d.consts[33], d.consts[34], d.flags);
          }
        }
      }
    }
    // Photo display-card sighting (see g_photo_card_seen_ns): fetch word2
    // encodes exactly 504x640, the JPEG-decode card texture's unique dims.
    if (g_photo_flow_frame.load(std::memory_order_relaxed) &&
        (d.fetch[2] & 0x03FFFFFFu) == 0x004FE1F7u && d.src_stride == 24) {
      g_photo_card_seen_ns.store(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              PerfClock::now().time_since_epoch())
              .count(),
          std::memory_order_relaxed);
    }
    published.push_back(std::move(d));
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  g_scene_2d = std::move(published);
  ++g_scene_2d_generation;
}

// Publish the frame's in-world spline draws: evaluate the guest B-spline
// VS on the CPU (see SplineDraw for the decoded algorithm) into WORLD-space
// strip vertices; the render side projects them with the scene's (smoothed)
// view_proj like every other world item, keeping them in phase with the
// re-timed camera.
void PublishSplineDraws(uint8_t* base) {
  std::vector<SplineDraw> frame_spline;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    frame_spline.swap(g_frame_spline);
  }
  std::vector<SplineDraw> published;
  published.reserve(frame_spline.size());
  for (SplineDraw& s : frame_spline) {
    const float* c = s.consts;
    const auto row = [&](int r) { return c + r * 4; };
    std::vector<uint8_t> out(size_t(s.count) * 28);
    float* dst = reinterpret_cast<float*>(out.data());
    const uint8_t* src = s.verts.data();
    bool ok = true;
    for (uint32_t v = 0; v < s.count; ++v, src += 12, dst += 7) {
      float p[3];
      for (int k = 0; k < 3; ++k) {
        uint32_t w;
        std::memcpy(&w, src + k * 4, 4);
        w = BSwap32(w);
        std::memcpy(&p[k], &w, 4);
      }
      if (!(p[0] >= 0.0f && p[0] < 142.0f)) {
        ok = false;
        break;
      }
      const int idx = int(p[0]);
      const float t = p[0] - float(idx);
      const int side = p[2] >= 0.5f ? 1 : 0;
      // Uniform cubic B-spline basis (matches the shader's embedded
      // coefficients exactly).
      const float t2 = t * t;
      const float t3 = t2 * t;
      const float wgt[4] = {1.0f - 3.0f * t + 3.0f * t2 - t3,
                            3.0f * t3 - 6.0f * t2 + 4.0f,
                            -3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f, t3};
      // Blend the world-transformed control points (world columns c4..c6,
      // translation in .w), /6, plus the world-rotated extrusion offset.
      float wp[3] = {0.0f, 0.0f, 0.0f};
      for (int k = 0; k < 4; ++k) {
        const float* cp = row(7 + idx + k);
        for (int a = 0; a < 3; ++a) {
          const float* wr = row(4 + a);
          wp[a] += wgt[k] *
                   (wr[0] * cp[0] + wr[1] * cp[1] + wr[2] * cp[2] + wr[3] * cp[3]);
        }
      }
      // Retail spline_defaultVS adds the world-rotated extrusion offset
      // i_spline_pos_offset (c151 / c152 per ribbon side) to the /6 blend.
      // (sk83_na_m's dev compile has no offset rows: its control points
      // carry both rails.)
      const float* off = row(151 + side);
      for (int a = 0; a < 3; ++a) {
        const float* wr = row(4 + a);
        wp[a] = wp[a] * (1.0f / 6.0f) +
                (wr[0] * off[0] + wr[1] * off[1] + wr[2] * off[2] + wr[3] * off[3]);
      }
      // Fade still evaluates against the draw's own clip z (the ramps
      // span hundreds of meters; the ~30 ms offset from the smoothed
      // camera is invisible), but the PUBLISHED position is WORLD-space:
      // the render side projects with the scene's (smoothed) view_proj so
      // the neon signs ride the exact same camera timeline as the world.
      // Baking the guest VP here was the "waypoint sign judders / lags
      // and catches up" bug once smooth_camera re-timed everything else.
      float clip_z;
      {
        const float* pr = row(2);
        clip_z = pr[0] * wp[0] + pr[1] * wp[1] + pr[2] * wp[2] + pr[3];
      }
      // Near/far fade against i_clipvalues (clip-space z, pre-divide). A
      // zero range degenerates to a step, like the shader's rcp(0) = inf.
      // i_clipvalues: (near_start, near_range, far_start, far_range) at c150.
      const float* cv = row(150);
      const auto ramp = [](float z, float start, float range) {
        if (range > 1e-20f || range < -1e-20f) {
          const float r = (z - start) / range;
          return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        }
        return z - start > 0.0f ? 1.0f : 0.0f;
      };
      const float fade =
          std::min(ramp(clip_z, cv[0], cv[1]), 1.0f - ramp(clip_z, cv[2], cv[3]));
      dst[0] = wp[0];
      dst[1] = wp[1];
      dst[2] = wp[2];
      dst[3] = 1.0f;
      dst[4] = p[1];
      dst[5] = p[2];
      dst[6] = fade;
    }
    if (!ok) {
      continue;
    }
    s.verts.swap(out);
    published.push_back(std::move(s));
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  g_scene_spline = std::move(published);
}

void GetCapturedSunDir(float out[3]) {
  out[0] = g_sun_captured[0].load(std::memory_order_relaxed);
  out[1] = g_sun_captured[1].load(std::memory_order_relaxed);
  out[2] = g_sun_captured[2].load(std::memory_order_relaxed);
}

// Rebuild a world->light affine row (xyz = axis * scale, w = translation)
// around a new axis, preserving the row's scale and its value at the camera
// - cascade centering and bias conventions carry over unchanged.
static void RebasisLightRow(float* row, const float axis[3],
                            const float cam[3]) {
  const float scale = std::sqrt(row[0] * row[0] + row[1] * row[1] +
                                row[2] * row[2]);
  if (scale < 1e-8f) {
    return;
  }
  const float at_cam =
      row[0] * cam[0] + row[1] * cam[1] + row[2] * cam[2] + row[3];
  row[0] = axis[0] * scale;
  row[1] = axis[1] * scale;
  row[2] = axis[2] * scale;
  row[3] = at_cam - (row[0] * cam[0] + row[1] * cam[1] + row[2] * cam[2]);
}

// Rebuild the frame's captured shadow transforms around the given unit
// direction TOWARD the sun: the light basis rows via RebasisLightRow
// (preserving each row's scale, so cascade sizes and bias conventions
// carry over) plus a re-center of the base cascade and the sub-boxes
// (c1/c2 scale.xy + offset.zw) around the camera, biased along the shadow
// direction; the game fits its offsets for ITS axis each frame, and
// under a rotated basis the finest cascade drifts off the player and
// their shadow (fallback to the coarser tiles reads visibly blocky).
// Caster pass and receivers share these rows, so the re-fit stays
// self-consistent. Sole caller: the lighting-lab sun override.
static void RebasisShadowRows(FrameScene& scene, const float sun[3],
                              bool include_ws) {
  // Light basis: depth axis points away from the sun; X/Y span the shadow
  // plane. Handedness is irrelevant; casters and receivers share the rows.
  const float zl[3] = {-sun[0], -sun[1], -sun[2]};
  float xl[3] = {zl[2], 0.0f, -zl[0]};  // cross((0,1,0), zl)
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
  const float* cam = scene.cam_pos;
  // Shadows extend along the sun's ground projection, so each cascade
  // centers ~35% of its radius in that direction instead of symmetrically
  // on the camera; a rotated low sun otherwise stretches character
  // shadows out of the fine cascade into the 4x/16x coarser tiles within
  // a few meters (the moved-sun blocky shadow). Near-vertical suns get no
  // bias (shadows stay under their casters).
  float sd[3] = {-sun[0], 0.0f, -sun[2]};
  const float sdl = std::sqrt(sd[0] * sd[0] + sd[2] * sd[2]);
  if (sdl > 0.1f) {
    sd[0] /= sdl;
    sd[2] /= sdl;
  } else {
    sd[0] = 0.0f;
    sd[2] = 0.0f;
  }
  if (scene.shadow_valid) {
    RebasisLightRow(scene.shadow_rows + 0, xl, cam);    // c0 light-space X
    RebasisLightRow(scene.shadow_rows + 12, yl, cam);   // c3 light-space Y
    RebasisLightRow(scene.shadow_rows + 16, zl, cam);   // c4 depth ramp
    float* rows = scene.shadow_rows;
    const float xlen =
        std::sqrt(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);
    const float ylen = std::sqrt(rows[12] * rows[12] + rows[13] * rows[13] +
                                 rows[14] * rows[14]);
    // Base cascade: center on the camera pushed along the shadow
    // direction by 35% of its own radius (the base tile's world radius is
    // 1 / row scale).
    const float rbase = 1.0f / std::max(std::max(xlen, ylen), 1e-8f);
    const float pb[3] = {cam[0] + sd[0] * 0.35f * rbase, cam[1],
                         cam[2] + sd[2] * 0.35f * rbase};
    rows[3] = -(rows[0] * pb[0] + rows[1] * pb[1] + rows[2] * pb[2]);
    rows[15] = -(rows[12] * pb[0] + rows[13] * pb[1] + rows[14] * pb[2]);
    // Sub-boxes: same construction at their own (wider) radii.
    for (int c = 0; c < 2; ++c) {
      const float sx = rows[4 + c * 4];
      const float sy = rows[5 + c * 4];
      const float rc =
          1.0f / std::max(std::max(xlen * std::fabs(sx), ylen * std::fabs(sy)),
                          1e-8f);
      const float pc[3] = {cam[0] + sd[0] * 0.35f * rc, cam[1],
                           cam[2] + sd[2] * 0.35f * rc};
      const float lsx =
          rows[0] * pc[0] + rows[1] * pc[1] + rows[2] * pc[2] + rows[3];
      const float lsy =
          rows[12] * pc[0] + rows[13] * pc[1] + rows[14] * pc[2] + rows[15];
      rows[6 + c * 4] = -lsx * sx;   // offset.x
      rows[7 + c * 4] = -lsy * sy;   // offset.y
    }
  }
  if (include_ws && scene.dynobj_ws_valid) {
    RebasisLightRow(scene.dynobj_ws + 0, xl, cam);
    RebasisLightRow(scene.dynobj_ws + 4, yl, cam);
    RebasisLightRow(scene.dynobj_ws + 8, zl, cam);
  }
}

// Lighting-lab sun override (skate3_native_render_scene_sun_override):
// rotates every captured per-frame light transform in the published scene
// to the azimuth/elevation-driven direction. The native CSM caster pass,
// the material receivers, the static world-shadow map (which re-primes
// automatically when its rows change) and the volumetric shafts all read
// these rows, so the whole dynamic lighting stack follows the moved sun.
// Baked lightmap shade and the sky dome's painted sun are game content and
// stay put.
static void ApplySunOverride(FrameScene& scene) {
  if (!REXCVAR_GET(skate3_native_render_scene_sun_override)) {
    return;
  }
  const float kDeg = 0.01745329252f;
  const float az = float(REXCVAR_GET(skate3_native_render_scene_sun_azimuth)) * kDeg;
  const float el =
      float(REXCVAR_GET(skate3_native_render_scene_sun_elevation)) * kDeg;
  // Unit vector TOWARD the sun (y up; azimuth 0 = +Z, 90 = +X).
  const float sun[3] = {std::cos(el) * std::sin(az), std::sin(el),
                        std::cos(el) * std::cos(az)};
  RebasisShadowRows(scene, sun, /*include_ws=*/true);
  if (scene.shadow_valid) {
    scene.shadow_rows[24] = sun[0];  // c6 sun direction
    scene.shadow_rows[25] = sun[1];
    scene.shadow_rows[26] = sun[2];
  }
  if (scene.dynobj_valid) {
    scene.dynobj_rows[0] = sun[0];
    scene.dynobj_rows[1] = sun[1];
    scene.dynobj_rows[2] = sun[2];
  }
  if (scene.sky_sun_valid) {
    scene.sky_sun[0] = sun[0];
    scene.sky_sun[1] = sun[1];
    scene.sky_sun[2] = sun[2];
  }
}

// Hor+ ultrawide: clip-space X scale that widens the game's screen-shaped
// projection to the wide guest-output aspect (see the SDK's
// ApplyNativeGuestOutputWideAspect). 1.0 while the output is not wide. The
// projection aspect is read off the matrix itself (|m11/m00|) so the FOV
// override and freecam zoom compose naturally.
float WideOutputHorScale(const float proj[16]) {
  // The native output follows the window at any aspect (see the app's
  // guest-output size provider); a wider-than-camera output widens the view.
  const double out_aspect = g_native_output_aspect.load(std::memory_order_relaxed);
  if (out_aspect <= 0.0 || !rex::graphics::IsNativeGuestOutputActive()) {
    return 1.0f;
  }
  const float m00 = std::fabs(proj[0]);
  const float m11 = std::fabs(proj[5]);
  if (!(m00 > 1e-6f) || !(m11 > 1e-6f)) {
    return 1.0f;
  }
  const double base_aspect = double(m11) / double(m00);
  if (out_aspect <= base_aspect + 0.01) {
    return 1.0f;
  }
  return float(base_aspect / out_aspect);
}

// Vert+ counterpart for outputs TALLER than the camera aspect (a portrait-ish
// window): clip-space Y scale keeping the game's horizontal field of view and
// extending the vertical one. 1.0 otherwise.
float WideOutputVertScale(const float proj[16]) {
  const double out_aspect = g_native_output_aspect.load(std::memory_order_relaxed);
  if (out_aspect <= 0.0 || !rex::graphics::IsNativeGuestOutputActive()) {
    return 1.0f;
  }
  const float m00 = std::fabs(proj[0]);
  const float m11 = std::fabs(proj[5]);
  if (!(m00 > 1e-6f) || !(m11 > 1e-6f)) {
    return 1.0f;
  }
  const double base_aspect = double(m11) / double(m00);
  if (out_aspect >= base_aspect - 0.01) {
    return 1.0f;
  }
  return float(out_aspect / base_aspect);
}

// Applies the Hor+ scale to the published camera: column 0 of the row-vector
// projection and view*proj (clip.x = dot(v, column 0)). Every consumer (draw
// MVPs, post-pass unprojection, splines, frustum tests) reads these published
// matrices, so the frame stays self-consistent, exactly as if the game camera
// itself had the wide aspect.
void WidenPublishedCamera(FrameScene& scene, float scale, float vert_scale) {
  for (int r = 0; r < 4; ++r) {
    scene.proj[r * 4 + 0] *= scale;
    scene.view_proj[r * 4 + 0] *= scale;
    scene.proj[r * 4 + 1] *= vert_scale;
    scene.view_proj[r * 4 + 1] *= vert_scale;
  }
}

// Does `cam` look like a Sk8::Render::ViewCamera -- is there a well-formed
// 4x4 projection matrix at +0x60? A real projection has a distinctive last
// column: perspective is m[2][3] == 1 with m[3][3] == 0, orthographic the
// reverse. Together with finite, non-degenerate m00/m11 that is specific
// enough to pick the camera pointer out of the surrounding header fields.
bool PlausibleViewCamera(uint8_t* base, uint32_t cam) {
  if (cam == 0 || !GuestReadableApprox(base, cam)) {
    return false;
  }
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadGuestF32(base, cam + 0x60 + i * 4);
    if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e6f) {
      return false;
    }
  }
  if (!(std::fabs(m[0]) > 1e-6f) || !(std::fabs(m[5]) > 1e-6f)) {
    return false;
  }
  const bool persp = m[11] == 1.0f && m[15] == 0.0f;
  const bool ortho = m[11] == 0.0f && m[15] == 1.0f;
  return persp || ortho;
}

// SceneRenderView::m_pViewCamera. Retail's +0x08 is tried first and kept if
// it validates; otherwise the view header is scanned, the same way
// tInstance::m_pRModel is resolved in OnAddRenderInstance. sk83_na_m is a
// different build of the game, so any offset baked in from retail's compiled
// layout has to be confirmed against this image before first use rather than
// assumed -- the view's sort-list field already moved 20160 -> 20176.
uint32_t ResolveViewCameraOffset(uint8_t* base, uint32_t view) {
  static std::atomic<uint32_t> s_off{0};
  uint32_t off = s_off.load(std::memory_order_relaxed);
  if (off != 0) {
    return off;
  }
  for (uint32_t i = 0; i < 25; ++i) {
    const uint32_t cand = (i == 0) ? kViewCameraFromView : (i - 1) * 4;
    if (i != 0 && cand == kViewCameraFromView) {
      continue;
    }
    uint32_t cam = 0;
    if (!GuestTryLoadU32(base, view + cand, &cam) || !PlausibleViewCamera(base, cam)) {
      continue;
    }
    s_off.store(cand, std::memory_order_relaxed);
    return cand;
  }
  return 0;
}

// DIAGNOSTIC BuildFrameScene phase profiler (perf_log only; guest render
// thread), same scheme as the OnDrawDone one (see DrawSectMark).
constexpr int kBuildSectCount = 12;
const char* const kBuildSectNames[kBuildSectCount] = {
    "head", "walk", "palette", "charrows", "gapfill", "camera",
    "dyndecode", "lwstore", "smooth", "retain", "publish", "tail"};
uint64_t g_build_sect_cyc[kBuildSectCount] = {};
int g_build_sect_cur = 0;
uint64_t g_build_sect_t = 0;
inline void BuildSectMark(int next) {
  if (!g_draw_sect_on) return;
  const uint64_t now = DrawSectCycles();
  g_build_sect_cyc[g_build_sect_cur] += now - g_build_sect_t;
  g_build_sect_cur = next;
  g_build_sect_t = now;
}
struct BuildSectScope {
  BuildSectScope() {
    if (g_draw_sect_on) {
      g_build_sect_cur = 0;
      g_build_sect_t = DrawSectCycles();
    }
  }
  ~BuildSectScope() { BuildSectMark(0); }
};
std::string BuildSectionShares() {
  uint64_t total = 0;
  for (uint64_t c : g_build_sect_cyc) total += c;
  std::string out;
  if (total != 0) {
    for (int i = 0; i < kBuildSectCount; ++i) {
      const double pct = 100.0 * double(g_build_sect_cyc[i]) / double(total);
      if (pct >= 0.5) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " %s=%.0f%%", kBuildSectNames[i], pct);
        out += buf;
      }
    }
  }
  for (uint64_t& c : g_build_sect_cyc) c = 0;
  return out;
}

// Re-time the SimpleDraw lines (FrameScene::sdraw_verts, the guest's latest
// world-space vertices) exactly like InterpolateDynamicItems re-times a
// character's bone palette: a ring of time-stamped samples (same stamp
// source and ingest spacing floor), evaluated at the shared playback clock
// g_smooth_play with the same boxcar (smooth_camera_filter_ms, 8 taps, each
// a pair-lerp of the bracketing samples, clamped at the newest sample, no
// extrapolation). The skeleton then lags and filters identically to the
// skater mesh it is drawn over. A vertex-count change or a > 100 ms gap
// resets the ring; with < 3 samples the raw lines draw. Guest render
// thread only.
void RetimeSimpleDraws(FrameScene& scene, double now) {
  constexpr int kRing = 24;
  struct Sample {
    double t = 0.0;
    std::vector<SimpleDrawVert> v;
  };
  static Sample ring[kRing];
  static int count = 0;
  static int newest = 0;
  std::vector<SimpleDrawVert>& cur = scene.sdraw_verts;
  if (cur.empty()) {
    count = 0;
    return;
  }
  const Sample& last = ring[newest];
  const bool changed =
      count == 0 || last.v.size() != cur.size() ||
      std::memcmp(last.v.data(), cur.data(), cur.size() * sizeof(SimpleDrawVert)) != 0;
  const double stamp =
      (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02) ? g_latest_cam_tick : now;
  if (changed && !(count > 0 && stamp - last.t < kMinDynIngestSpacing)) {
    if (count > 0 && (now - last.t > 0.1 || last.v.size() != cur.size())) {
      count = 0;
    }
    newest = count == 0 ? 0 : (newest + 1) % kRing;
    ring[newest].t = stamp;
    ring[newest].v = cur;
    count = std::min(count + 1, kRing);
  }
  if (count < 3 || now - ring[newest].t > 0.1 || ring[newest].v.size() != cur.size()) {
    return;  // not enough history: raw lines (as the items render raw)
  }
  // Pair-lerp of the ring at time tt, accumulated with `weight`.
  const size_t n = cur.size();
  static std::vector<float> acc;  // a.xyz, b.xyz per vertex
  const auto accum_at = [&](double tt, float weight) {
    int hi = newest;
    int lo = (newest + kRing - 1) % kRing;
    for (int step = 1; step < count - 1; ++step) {
      if (ring[lo].t <= tt) {
        break;
      }
      hi = lo;
      lo = (lo + kRing - 1) % kRing;
    }
    const Sample& p0 = ring[lo];
    const Sample& p1 = ring[hi];
    if (p0.v.size() != n || p1.v.size() != n) {
      return false;
    }
    const double span = std::max(p1.t - p0.t, 0.0005);
    const float al = float(std::clamp((tt - p0.t) / span, 0.0, 1.0));
    for (size_t i = 0; i < n; ++i) {
      for (int k = 0; k < 3; ++k) {
        const float a0 = p0.v[i].a[k], a1 = p1.v[i].a[k];
        const float b0 = p0.v[i].b[k], b1 = p1.v[i].b[k];
        acc[i * 6 + k] += (a0 + (a1 - a0) * al) * weight;
        acc[i * 6 + 3 + k] += (b0 + (b1 - b0) * al) * weight;
      }
    }
    return true;
  };
  const double filter_w = std::clamp(
      REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) * 1e-3;
  acc.assign(n * 6, 0.0f);
  bool ok = true;
  if (filter_w > 0.0005 && count >= 4) {
    constexpr int kTaps = 8;
    for (int tap = 0; tap < kTaps && ok; ++tap) {
      const double tt = std::min(g_smooth_play - filter_w * 0.5 + (tap + 0.5) * filter_w / kTaps,
                                 ring[newest].t);
      ok = accum_at(tt, 1.0f / kTaps);
    }
  } else {
    ok = accum_at(g_smooth_play, 1.0f);
  }
  if (!ok) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    for (int k = 0; k < 3; ++k) {
      cur[i].a[k] = acc[i * 6 + k];
      cur[i].b[k] = acc[i * 6 + 3 + k];
    }
  }
}

// DIAGNOSTIC (skate3_native_render_scene_ghost_audit): per instance (ctx,
// mesh), the run of consecutive guest frames it was published from a
// non-live source: palette rescue (dbg_src 3 / 11), rigid rescue (12), LW
// gap fill (9) or off-screen retention. Legitimate fills are 1-3 frames;
// a long run is an instance the game no longer draws.
void GhostAudit(uint8_t* base, const FrameScene& scene,
                const FlatMap<uint32_t, DrawRegState>& frame_draw_rs) {
  struct Run {
    uint64_t last = 0;
    uint32_t len = 0;
    uint8_t src = 0;
  };
  static FlatMap<uint64_t, Run> s_runs;
  static uint64_t s_sweep = 0;
  const uint64_t now = g_guest_frame;
  for (const DrawItem& it : scene.items) {
    if (it.char_family == 0 && !it.dyn_entity) {
      continue;
    }
    const bool fill = it.dbg_src == 3 || it.dbg_src == 9 || it.dbg_src == 11 ||
                      it.dbg_src == 12 || it.retained;
    const uint64_t key = (uint64_t(it.ctx) << 32) | it.mesh;
    if (!fill) {
      if (Run* r = s_runs.find(key)) {
        r->len = 0;
      }
      continue;
    }
    Run& r = s_runs[key];
    r.len = (r.last + 1 == now) ? r.len + 1 : 1;
    r.last = now;
    r.src = it.retained ? 255 : it.dbg_src;
    if (r.len == 30 || r.len == 300 || r.len == 3000) {
      float pos[3] = {it.world[12], it.world[13], it.world[14]};
      if (it.skinned && it.bones.size() >= 12) {
        pos[0] = it.bones[3];
        pos[1] = it.bones[7];
        pos[2] = it.bones[11];
      }
      float alpha = -1.0f;
      uint32_t entity = 0;
      const bool lw_alive =
          it.ctx != 0 && skate3::native_lw::LookupLwCtx(it.ctx, &alpha, &entity);
      // Did ANY perspective draw this frame use the item's index buffer
      // (draw_rs records every main/z-pass draw's ib)? No = the game drew
      // nothing of this mesh, so whatever we show is ours alone.
      const bool drawn = frame_draw_rs.find(it.ib_obj) != nullptr;
      const bool in_view = !BoxOutsideFrustum(pos, 1.0f, scene.view_proj, 1.0f);
      const float dx = pos[0] - scene.cam_pos[0];
      const float dy = pos[1] - scene.cam_pos[1];
      const float dz = pos[2] - scene.cam_pos[2];
      REXLOG_WARN(
          "native-scene: GHOST? ctx={:08X} mesh={:08X} fam={} dyn={} skinned={} src={} "
          "run={} frames pos=({:.1f},{:.1f},{:.1f}) dist={:.1f}m lw_alive={} alpha={:.2f} "
          "entity={:08X} lw_alpha={:.2f} game_drew_mesh={} in_view={}",
          it.ctx, it.mesh, it.char_family, it.dyn_entity, it.skinned, r.src, r.len, pos[0],
          pos[1], pos[2], std::sqrt(dx * dx + dy * dy + dz * dz), lw_alive, alpha, entity,
          it.lw_alpha, drawn, in_view);
    }
  }
  if (now - s_sweep > 600) {
    s_sweep = now;
    std::vector<uint64_t> old;
    s_runs.for_each([&](uint64_t k, const Run& r) {
      if (now - r.last > 600) old.push_back(k);
    });
    for (uint64_t k : old) s_runs.erase(k);
  }
  (void)base;
}

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  BuildSectScope build_sect_scope;
  if (!SceneEnabled()) {
    return;
  }
  // The frame-end walks below chase captured pointers whose ranges world
  // streaming may have revoked during the frame; recover raw-load read
  // faults for the whole build (POSIX; no-op on Windows).
  GuestReadRecoveryScope guest_read_recovery(base);
  // Published every frame (not just on world submissions): boot/menu frames
  // carry only 2D, and the render thread's 2D texture decodes need the
  // guest base from the very first natively rendered boot frame.
  g_guest_base.store(base, std::memory_order_relaxed);
  ++g_guest_frame;  // paces the world-item cache revalidation
  // Perf telemetry: guest frame interval + this frame's capture-hook cost.
  static PerfClock::time_point s_last_frame_tp{};
  // Previous frame's phase costs, for the slow-frame attribution below (a
  // frame's dt is only known at the NEXT frame's entry).
  static uint64_t s_prev_build_ns = 0, s_prev_b2d_ns = 0, s_prev_bspl_ns = 0,
                  s_prev_bpal_ns = 0, s_prev_pal_tail_ns = 0, s_prev_cap_ns = 0;
  const auto build_t0 = PerfClock::now();
  if (s_last_frame_tp.time_since_epoch().count() != 0) {
    const uint64_t dt_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(build_t0 - s_last_frame_tp)
            .count());
    g_pw_guest_dt.Add(dt_ns);
    const double dt_ms = double(dt_ns) * 1e-6;
    g_dt_hist[dt_ms < 3.6 ? 0 : dt_ms < 4.5 ? 1 : dt_ms < 6.0 ? 2 : dt_ms < 10.0 ? 3 : 4]
        .fetch_add(1, std::memory_order_relaxed);
    // Attribute dips: the previous frame stretched the interval - log its
    // phase breakdown. "rest" = the game's own frame work + hook overhead
    // outside the build (dt minus our measured blocks).
    if (dt_ms >= 4.5 && dt_ms < 100.0 &&
        g_slow_frame_log_budget.load(std::memory_order_relaxed) > 0 &&
        g_slow_frame_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const double ours_ms =
          double(s_prev_build_ns + s_prev_cap_ns) * 1e-6;
      REXLOG_DEBUG(
          "native-scene: slow guest frame dt={:.2f}ms prev[cap={:.2f} build={:.2f} "
          "(2d={:.2f} spl={:.2f} pal={:.2f} ptail={:.2f} walk={:.2f}) rest={:.2f}]ms",
          dt_ms, double(s_prev_cap_ns) * 1e-6, double(s_prev_build_ns) * 1e-6,
          double(s_prev_b2d_ns) * 1e-6, double(s_prev_bspl_ns) * 1e-6,
          double(s_prev_bpal_ns) * 1e-6, double(s_prev_pal_tail_ns) * 1e-6,
          double(s_prev_build_ns - std::min(s_prev_build_ns,
                                            s_prev_b2d_ns + s_prev_bspl_ns +
                                                s_prev_bpal_ns + s_prev_pal_tail_ns)) *
              1e-6,
          dt_ms - ours_ms);
    }
  }
  s_last_frame_tp = build_t0;
  g_pw_capture.Add(g_capture_frame_ns);
  s_prev_cap_ns = g_capture_frame_ns;
  g_capture_frame_ns = 0;
  g_frame_b2d_ns = 0;
  g_frame_bspl_ns = 0;
  g_frame_bpal_ns = 0;
  g_frame_pal_tail_ns = 0;
  struct BuildPerf {
    PerfClock::time_point t0;
    uint64_t* prev_build_ns;
    uint64_t* prev_b2d_ns;
    uint64_t* prev_bspl_ns;
    uint64_t* prev_bpal_ns;
    uint64_t* prev_pal_tail_ns;
    ~BuildPerf() {
      const uint64_t build_ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count());
      g_pw_build.Add(build_ns);
      *prev_build_ns = build_ns;
      *prev_b2d_ns = g_frame_b2d_ns;
      *prev_bspl_ns = g_frame_bspl_ns;
      *prev_bpal_ns = g_frame_bpal_ns;
      *prev_pal_tail_ns = g_frame_pal_tail_ns;
    }
  } build_perf{build_t0, &s_prev_build_ns, &s_prev_b2d_ns, &s_prev_bspl_ns,
               &s_prev_bpal_ns, &s_prev_pal_tail_ns};
  if (g_recording.load(std::memory_order_relaxed)) {
    // Flush this frame's deferred inline-ring payloads (2D BeginVertices
    // draws): the CPU has finished writing them by frame end, and the ring
    // has not yet been reused.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    for (const PendingInlineDump& p : g_pending_inline_dumps) {
      if (p.draw_index >= g_recorded_draws.size() ||
          g_recorded_buffer_bytes + p.bytes > (512u << 20) ||
          !GuestRangeReadable(base, p.addr, p.bytes)) {
        continue;
      }
      RecordedBuffer buf;
      buf.vb_addr = p.addr;
      buf.ib_addr = 0;
      buf.fingerprint = (uint64_t(g_recorded_draws[p.draw_index]->frame) << 32) | p.addr;
      buf.vb.resize(p.bytes);
      std::memcpy(buf.vb.data(), base + p.addr, p.bytes);
      g_recorded_draws[p.draw_index]->vb_dump = uint32_t(g_recorded_buffers.size());
      g_recorded_buffer_bytes += p.bytes;
      g_recorded_buffers.push_back(std::move(buf));
    }
    g_pending_inline_dumps.clear();
  }
  {
    const auto t0 = PerfClock::now();
    Publish2dDraws(base);
    const auto t1 = PerfClock::now();
    PublishSplineDraws(base);
    PublishSimpleDraws(base);
    const auto t2 = PerfClock::now();
    g_frame_b2d_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_frame_bspl_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    g_pw_b2d.Add(g_frame_b2d_ns);
    g_pw_bspl.Add(g_frame_bspl_ns);
  }
  // Take this frame's hook-time dynamic items regardless of how we exit,
  // leaving them in place across an early return (no perspective view, empty
  // frame) would desynchronize the indices stored in the records.
  std::vector<DrawItem> dynitems;
  FlatSet<uint32_t> ortho_ctx;
  std::unordered_multimap<uint32_t, float> ortho_mag;
  std::unordered_map<uint64_t, SkinGraphic> skin_graphic;
  std::unordered_map<uint64_t, FaceInjury> face_injury;
  std::unordered_map<uint64_t, CharLayer> char_layer;
  std::unordered_map<uint64_t, SkinSssRecord> skin_sss;
  FlatSet<uint64_t> ws_keys;
  std::unordered_map<uint64_t, std::vector<DrawEntry>> ws_entries;
  uint32_t ws_draws = 0;
  std::vector<uint32_t> ws_draw_ctxs;
  std::vector<ParkWsDraw> park_ws_draws;
  float park_ws_rows[2][12] = {};
  bool park_ws_rows_have[2] = {false, false};
  bool park_ws_resolved = false;
  FlatSet<uint64_t> dmo_keys;
  std::unordered_map<uint64_t, std::array<float, 16>> fg_world;
  std::unordered_map<uint64_t, std::vector<DrawEntry>> fg_entries;
  std::unordered_map<uint64_t, std::array<uint32_t, 6>> fg_fetch;
  std::unordered_map<uint64_t, std::vector<DefaultInst>> default_inst;
  std::unordered_map<uint64_t, std::array<float, 16>> xray_world;
  std::unordered_map<uint64_t, std::vector<DrawEntry>> xray_entries;
  std::unordered_map<uint64_t, std::array<float, 16>> hl_world;
  std::unordered_map<uint64_t, std::vector<DrawEntry>> hl_entries;
  float hl_rgb[3] = {0.0f, 0.0f, 0.0f};
  FlatMap<uint32_t, DrawRegState> draw_rs;
  std::unordered_multimap<uint64_t, CharDrawRows> char_draw_rows;
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    dynitems.swap(g_frame_dynitems);
    // The next frame's captures land in a fresh vector: pre-size it so it
    // does not regrow (moving every ~1 KB item) during the draws.
    g_frame_dynitems.reserve(dynitems.size() + 32);
    ortho_ctx.swap(g_frame_ortho_ctx);
    ortho_mag.swap(g_frame_ortho_mag);
    skin_graphic.swap(g_frame_skin_graphic);
    face_injury.swap(g_frame_face_injury);
    char_layer.swap(g_frame_char_layer);
    skin_sss.swap(g_frame_skin_sss);
    ws_keys.swap(g_frame_ws_keys);
    ws_entries.swap(g_frame_ws_entries);
    g_frame_ws_entries.clear();
    ws_draws = g_frame_ws_draws;
    g_frame_ws_draws = 0;
    ws_draw_ctxs.swap(g_frame_ws_ctxs);
    g_frame_ws_ctxs.clear();
    park_ws_draws.swap(g_frame_park_ws);
    g_frame_park_ws.clear();
    std::memcpy(park_ws_rows, g_frame_park_ws_rows, sizeof(park_ws_rows));
    park_ws_rows_have[0] = g_frame_park_ws_rows_have[0];
    park_ws_rows_have[1] = g_frame_park_ws_rows_have[1];
    g_frame_park_ws_rows_have[0] = g_frame_park_ws_rows_have[1] = false;
    park_ws_resolved = g_frame_park_ws_resolved;
    g_frame_park_ws_resolved = false;
    dmo_keys.swap(g_frame_dmo_keys);
    g_frame_dmo_keys.reserve(dmo_keys.size());
    fg_world.swap(g_frame_fg_world);
    g_frame_fg_world.clear();
    fg_entries.swap(g_frame_fg_entries);
    g_frame_fg_entries.clear();
    fg_fetch.swap(g_frame_fg_fetch);
    g_frame_fg_fetch.clear();
    default_inst.swap(g_frame_default_inst);
    g_frame_default_inst.clear();
    xray_world.swap(g_frame_xray_world);
    g_frame_xray_world.clear();
    xray_entries.swap(g_frame_xray_entries);
    g_frame_xray_entries.clear();
    hl_world.swap(g_frame_hl_world);
    g_frame_hl_world.clear();
    hl_entries.swap(g_frame_hl_entries);
    g_frame_hl_entries.clear();
    std::memcpy(hl_rgb, g_frame_hl_rgb, sizeof(hl_rgb));

    draw_rs.swap(g_frame_draw_rs);
    // The swapped-in maps start empty and take ~2000 inserts per frame from
    // OnDrawDone: pre-size them to last frame's count so the per-draw
    // inserts never rehash.
    g_frame_draw_rs.reserve(draw_rs.size());
    g_frame_skin_sss.reserve(skin_sss.size());
    g_frame_ws_keys.reserve(ws_keys.size());
    g_frame_pending_by_buffers.clear();
    g_frame_pending_keys.clear();
    g_frame_char_refresh.clear();
    g_frame_char_refresh_keys.clear();
    char_draw_rows.swap(g_frame_char_draw_rows);
    g_frame_char_draw_rows.clear();
  }
  // Character rebuild hygiene. Create-a-skater replaces EVERY character
  // piece on any outfit change (all debug-view hash colors change), and the
  // new pieces reuse freed mesh/ctx addresses. Every rescue cache below is
  // keyed by those addresses alone, so a rebuilt garment inherited its
  // predecessor's palette, rows, ropa state and pose ring: holed shirts and
  // stretched ponytails that persisted until the caches aged out. Identity =
  // buffer objects + sizes + stride (NOT payload addresses: dynamic/ropa
  // payloads move every frame); a key matching neither of the last two seen
  // for that address is a rebuild (tolerates a two-buffer ping-pong).
  {
    struct LayoutHist {
      uint64_t k0 = 0;
      uint64_t k1 = 0;
    };
    static std::unordered_map<uint32_t, LayoutHist> s_mesh_layout;
    static std::unordered_map<uint32_t, LayoutHist> s_ctx_layout;
    if (s_mesh_layout.size() > 16384) s_mesh_layout.clear();
    if (s_ctx_layout.size() > 16384) s_ctx_layout.clear();
    const auto changed = [](std::unordered_map<uint32_t, LayoutHist>& m, uint32_t addr,
                            uint64_t k) -> bool {
      auto [it, fresh] = m.try_emplace(addr);
      LayoutHist& h = it->second;
      if (fresh) {
        h.k0 = k;
        return false;
      }
      if (k == h.k0 || k == h.k1) {
        if (k == h.k1) std::swap(h.k0, h.k1);
        return false;
      }
      h.k1 = h.k0;
      h.k0 = k;
      return true;
    };
    for (const DrawItem& d : dynitems) {
      if ((d.char_family == 0 && !d.ropa && !d.skinned) || d.mesh == 0 || d.cloth_quads) {
        continue;
      }
      uint64_t lk = 1469598103934665603ull;
      const auto mix = [&lk](uint64_t v) { lk = (lk ^ v) * 1099511628211ull; };
      mix(d.vb_obj);
      mix(d.ib_obj);
      mix(d.vb_bytes);
      mix(d.ib_count);
      mix(d.stride);
      const bool rebuilt_mesh = changed(s_mesh_layout, d.mesh, lk);
      const bool rebuilt_ctx =
          d.ctx != 0 &&
          changed(s_ctx_layout, d.ctx, lk ^ (uint64_t(d.mesh) * 0x9E3779B97F4A7C15ull));
      if (rebuilt_mesh) {
        g_char_rows_cache.erase(d.mesh);
        g_ropa_state_cache.erase(d.mesh);
        g_ropa_last_seq.erase(d.mesh);
        InvalidateCachedItem(d.mesh);
        {
          std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
          g_skin_probe.erase(d.mesh);
        }
        std::erase_if(g_char_rows_cache_inst,
                      [&](const auto& e) { return uint32_t(e.first >> 32) == d.mesh; });
      }
      if (rebuilt_ctx) {
        g_bones_cache_ctx.erase(d.ctx);
        g_char_rows_cache_ctx.erase(d.ctx);
        g_rigid_world_cache.erase(d.ctx);
        g_lw_last_items.erase(d.ctx);
        std::erase_if(g_char_rows_cache_inst,
                      [&](const auto& e) { return uint32_t(e.first) == d.ctx; });
      }
      if (rebuilt_mesh || rebuilt_ctx) {
        skate3::native_palette::PurgeRebuiltMesh(d.mesh, rebuilt_ctx ? d.ctx : 0);
        if (g_rebuilt_meshes.size() < 4096) {
          g_rebuilt_meshes.push_back(d.mesh);
        }
        if (g_rebuilt_meshes_pub.size() < 4096) {
          g_rebuilt_meshes_pub.push_back(d.mesh);
        }
        static std::atomic<uint64_t> s_rebuilds{0};
        const uint64_t n = s_rebuilds.fetch_add(1, std::memory_order_relaxed);
        if (n < 32 || (n & 1023u) == 0) {
          REXLOG_INFO("native-scene: character piece rebuilt mesh={:08X} ctx={:08X} "
                      "fam={} ropa={} (mesh={} ctx={}) - caches purged (n={})",
                      d.mesh, d.ctx, d.char_family, d.ropa ? 1 : 0, rebuilt_mesh ? 1 : 0,
                      rebuilt_ctx ? 1 : 0, n);
        }
      }
    }
  }
  // Take this frame's selection re-draw captures and re-arm the post-sky
  // window (must happen on every exit path, like the dynitems swap).
  std::vector<SelectedDrawKey> frame_selected;
  frame_selected.swap(g_frame_selected);
  g_sky_seen_this_frame = false;
  const bool outline_edge_seen = g_outline_edge_seen;
  g_outline_edge_seen = false;
  if (count == 0) {
    return;
  }

  // Multiple SceneRenderViews can submit per frame (main, shadow cascades,
  // reflections). Pick the perspective one (proj[2][3] == 1 in row-vector
  // convention) and only take its items, deduplicated (the same context can
  // appear in several of the view's sort lists).
  uint32_t view = 0;
  uint32_t viewcam = 0;
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    if (r.kind != 1 || r.c == 0) {
      continue;
    }
    const uint32_t cam_off = ResolveViewCameraOffset(base, r.c);
    if (cam_off == 0) {
      continue;
    }
    uint32_t cam = 0;
    if (!GuestTryLoadU32(base, r.c + cam_off, &cam) || !GuestReadableApprox(base, cam)) {
      continue;
    }
    const float persp_w = LoadGuestF32(base, cam + 0x60 + (2 * 4 + 3) * 4);
    if (persp_w == 1.0f) {
      // Screen-shaped views only. The skater-portrait render-to-texture
      // passes (team menu boxes, Import Skater card) submit their OWN
      // perspective SceneRenderView with a tall narrow projection; picking
      // it here published the portrait as the world scene: the skater
      // flashed FULL SCREEN behind the menu on every entry/scroll, and the
      // publish refreshed g_last_publish_ns so the mode flapped
      // pause-native <-> loading for the 300 ms freshness window each time.
      // aspect(w/h) = m11/m00 of the
      // raw projection; every real screen view is >= 4:3.
      const float m00 = std::fabs(LoadGuestF32(base, cam + 0x60 + 0 * 4));
      const float m11 = std::fabs(LoadGuestF32(base, cam + 0x60 + (1 * 4 + 1) * 4));
      if (!(m00 > 1e-6f) || m11 < m00 * 1.2f) {
        static std::atomic<uint64_t> s_aux_views{0};
        const uint64_t n = s_aux_views.fetch_add(1, std::memory_order_relaxed);
        if (n < 4 || (n & 255u) == 0) {
          REXLOG_DEBUG(
              "native-scene: aux perspective view skipped (portrait RTT "
              "pass, proj aspect {:.2f}) (n={})",
              m00 > 1e-6f ? m11 / m00 : 0.0f, n);
        }
        continue;
      }
      view = r.c;
      viewcam = cam;
      break;
    }
  }
  if (view == 0) {
    return;
  }
  g_main_scene_view.store(view, std::memory_order_relaxed);

  FrameScene scene;
  scene.sdraw_verts = g_last_sdraw;
  // Records plus the off-screen retention appends (section 9): a reallocation
  // moves every ~1 KB item.
  // Last frame's final count covers the rescues, gap fill and retained
  // appends the record count misses (a reallocation moves every item).
  static size_t s_items_last = 0;
  scene.items.reserve(std::max(count + std::min<size_t>(g_retained_items.size(), 1024) + 64,
                               s_items_last + 64));
  static FlatSet<uint32_t> seen;
  BuildSectMark(1);
  // Pre-size the per-frame bookkeeping: these fill with thousands of
  // entries every frame, and growing from empty rehashes repeatedly.
  seen.reset(count);
  // Dynamic contexts are submitted several times per frame (once per pass);
  // each submission carries that pass's culled island list. Keep the fullest
  // one; a shadow-pass list can be missing body parts the main view needs.
  std::unordered_map<uint32_t, size_t> dyn_slot;
  // First refused/pending ropa capture per mesh, candidates for the
  // post-merge rescue, which must restore last frame's resolved MODE
  // (rigid vs skinned), never blindly re-skin (see g_ropa_state_cache).
  std::unordered_map<uint32_t, const DrawItem*> pending_ropa_by_mesh;
  // Per-INSTANCE skinned candidates (ctx-keyed, character families): a
  // refused capture re-publishes with the instance's own last palette
  // (see g_bones_cache_ctx).
  std::unordered_map<uint32_t, const DrawItem*> pending_skinned_by_ctx;
  // Per-instance rigid candidates whose post-draw world fixup never landed
  // this frame; rescued with their cached world (see g_rigid_world_cache).
  std::unordered_map<uint32_t, const DrawItem*> pending_rigid_by_ctx;
  // Sort-list records of dynamically-dispatched piece meshes (see
  // g_dyn_dispatch_meshes): built here, published AFTER the record loop with
  // the ctx owner's instance matrix, and only for ctxs no live dynamic
  // capture already covered this frame.
  std::unordered_map<uint32_t, DrawItem> sortlist_local_by_ctx;
  const auto total_indices = [](const DrawItem& d) {
    uint64_t n = 0;
    for (const DrawEntry& e : d.draws) n += e.index_count;
    return n;
  };
  const bool wloop_prof = REXCVAR_GET(skate3_native_render_scene_perf_items);
  const auto wloop_t0 = wloop_prof ? PerfClock::now() : PerfClock::time_point{};
  if (wloop_prof) {
    g_bi_records.fetch_add(count, std::memory_order_relaxed);
  }
  // Build-side occlusion skip: the render thread's culled-ctx set (the same
  // sticky snapshot the guest dispatch filter uses, expiring with it when
  // the native render idles or the cull's motion gate stands down). A ctx
  // in it skips the whole item rebuild on 3 of every 4 frames; the
  // staggered rebuild frame re-enters it in scene.items so the render side
  // re-tests occlusion (disocclusion drops it from the set) and the shadow
  // caster caches keep refreshing.
  static std::vector<uint32_t> s_build_culled;  // guest render thread only
  s_build_culled.clear();
  if (REXCVAR_GET(skate3_native_render_scene_occlusion_cull_build)) {
    CopyOcclusionCulledCtxs(s_build_culled);
    if (!s_build_culled.empty()) {
      scene.occl_build_skipped.reserve(s_build_culled.size());
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    // Primary opaque list of the chosen view only; other lists (shadow
    // culling, transparents, z-prepass) duplicate the same geometry through
    // different MeshContext objects and z-fight.
    if (r.kind == 1 && (r.c != view || r.b != 20160)) {
      continue;
    }
    if (r.kind == 2 && r.b != view) {
      // World-path capture from another view (shadow cascade): rendering it
      // duplicates the entity as a ghost.
      seen.insert(r.a);
      continue;
    }
    if (r.kind == 0 || r.kind == 2 || r.kind == 3) {
      // Dynamic entity (kind 0), main-view world-path capture (kind 2) or
      // quad-list capture (kind 3): the complete item was built at hook time.
      seen.insert(r.a);
      if (r.c == 0) {
        g_rej_no_dynstate.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (r.c > dynitems.size()) {
        g_rej_dyn_range.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const DrawItem& cand = dynitems[r.c - 1];
      if (cand.pending) {
        // Deferred mesh whose draw never came, or a capture the palette
        // acceptance gates refused. Remember it: if the instance does not
        // publish this frame, the post-merge rescue re-publishes it with
        // LAST frame's palette; one frame of pose lag beats a
        // one-frame-missing hat.
        if (cand.ropa) {
          pending_ropa_by_mesh.try_emplace(cand.mesh, &cand);
        } else if (cand.skinned && cand.ctx != 0 && cand.char_family != 0) {
          pending_skinned_by_ctx.try_emplace(cand.ctx, &cand);
        } else if (!cand.skinned && !cand.cloth_quads && cand.ctx != 0) {
          pending_rigid_by_ctx.try_emplace(cand.ctx, &cand);
        }
        (cand.skinned ? g_skinned_skipped : g_rigid_dropped)
            .fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
        continue;
      }
      auto [slot, inserted] = dyn_slot.try_emplace(r.a, scene.items.size());
      if (inserted) {
        scene.items.push_back(cand);
      } else {
        // Merge the per-pass captures of one context: PALETTE from the
        // perspective (z/main-pass) bank; the ortho caster-cascade banks
        // carry stale fine animation (see DrawItem::caster_bank), but
        // GEOMETRY from the fullest culled island list. The two must be
        // decided independently: a shadow list can be missing body parts
        // the main view needs, and conversely the main view CULLS most of
        // a vehicle the camera is standing inside/next to (wholesale
        // "prefer main pass" replacement published that partial list and
        // near vehicles went invisible).
        DrawItem& cur = scene.items[slot->second];
        const bool fresher = !cand.caster_bank && cur.caster_bank;
        const bool staler = cand.caster_bank && !cur.caster_bank;
        const bool fuller = total_indices(cand) > total_indices(cur);
        // State/geometry grafts require the same resolved mode: ropa
        // garments flip between rigid and skinned per capture, and mixing
        // one copy's palette with another's interpretation is the
        // mangled-ribbon bug. Same-mode ropa IS graftable; excluding ropa
        // wholesale meant a fuller caster-cascade list wholesale-won the
        // merge and the garment published the ortho bank's ~40 ms-stale
        // bone rows while the body meshes (graftable) got fresh palettes:
        // the garment rode ~10 cm off the moving body (speed x 40 ms), the
        // visible successor of the invisible-torso bug once the near-camera
        // acceptance landed. The mode equality check is what prevents the
        // ribbon, not the ropa flag.
        const bool graftable = cand.skinned == cur.skinned &&
                               cand.ropa == cur.ropa && cand.mesh == cur.mesh;
        if (fresher && fuller) {
          cur = cand;
        } else if (fresher && graftable) {
          // Fresher palette, smaller list: adopt the state, keep the
          // fuller geometry (same mesh and buffers, lists differ only in
          // which islands each pass kept).
          cur.bones = cand.bones;
          std::memcpy(cur.world, cand.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, cand.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, cand.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (staler && fuller && graftable) {
          // Fuller caster list vs a fresher partial item: keep the fresh
          // palette, adopt the full geometry.
          const DrawItem state = cur;
          cur = cand;
          cur.bones = state.bones;
          std::memcpy(cur.world, state.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, state.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, state.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (!fresher && !staler && fuller) {
          cur = cand;  // same pass class: fullest wins, as before
        } else if (staler && fuller) {
          cur = cand;  // ungraftable (ropa): pre-arbitration fullest-wins
        }
      }
      continue;
    }
    if (!seen.insert(r.a).second) {
      continue;
    }
    if (!s_build_culled.empty() &&
        ((g_guest_frame + ((r.a >> 4) * 2654435761u >> 8)) & 3u) != 0u &&
        std::binary_search(s_build_culled.begin(), s_build_culled.end(),
                           r.a)) {
      scene.occl_build_skipped.push_back(r.a);
      g_occl_build_skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!REXCVAR_GET(skate3_native_render_scene_world_items)) {
      continue;
    }
    // Built straight into the published list: a DrawItem is ~1 KB of plain
    // fields, and building into a local then moving it in copied them twice
    // per world item per frame. Every non-publishing path pops it again.
    DrawItem& item = scene.items.emplace_back();
    if (BuildItemGeometry(base, r.a, item)) {
      if (item.skinned) {
        // Skinned meshes reached through the world sort lists (flags,
        // banners) have no captured palette, bind-pose garbage; skip.
        g_skinned_skipped.fetch_add(1, std::memory_order_relaxed);
        scene.items.pop_back();
        continue;
      }
      if (item.pos_fmt != 57) {
        // Model-space prop (vending machines, dispensers): its vertices need
        // the per-draw transform, which only the kind-2 hook-time capture
        // has; rendered here with identity it collapses at the world
        // origin. The capture handles it (or it is dropped when pending).
        g_world_props.fetch_add(1, std::memory_order_relaxed);
        scene.items.pop_back();
        continue;
      }
      const auto dit = g_dyn_dispatch_meshes.find(item.mesh);
      if (dit != g_dyn_dispatch_meshes.end() &&
          g_guest_frame - dit->second <= 1800) {
        // Instance-transformed piece batched through the static sort lists
        // (the game hands FAR clones of dynamically-dispatched pieces to
        // the batcher): fmt-57 but MESH-LOCAL, so the identity world of
        // this path collapses it at the origin. Defer; the post-loop
        // publish serves the ctx owner's instance matrix, and only for
        // ctxs no live dynamic capture covered this frame.
        sortlist_local_by_ctx.try_emplace(r.a, std::move(item));
        scene.items.pop_back();
        continue;
      }
    } else {
      scene.items.pop_back();
      // Silent world-item drop attribution (the F7 rings show world meshes
      // MISSING for 8-11 frame episodes, the mesh
      // flicker): a transient BuildItemGeometry failure (guarded reads
      // fail while the streamer relocates the mesh/material structures)
      // is indistinguishable from a game-culled record without this log.
      static std::atomic<uint32_t> s_wbuild_logs{0};
      static std::atomic<int64_t> s_wbuild_win{0};
      const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      int64_t win = s_wbuild_win.load(std::memory_order_relaxed);
      if (now_s - win >= 5 && s_wbuild_win.compare_exchange_strong(win, now_s)) {
        s_wbuild_logs.store(0, std::memory_order_relaxed);
      }
      if (s_wbuild_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("native-scene: world item build FAILED ctx={:08X}", r.a);
      }
    }
  }
  if (wloop_prof) {
    g_pw_bi_wloop.Add(PerfNsSince(wloop_t0));
  }
  BuildSectMark(2);
  // Cross-frame palette rescue + cache refresh (see g_bones_cache_ctx and
  // g_ropa_state_cache): published copies refresh their cache entries; a
  // refused/pending capture re-publishes with the cached palette (one frame
  // of pose lag instead of a one-frame disappearance).
  if (REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
    // Authoritative palette serve: give every mapped skinned character item
    // its instance's own packed m_matrices rows (snapshotted at the game's
    // Pack/UpdateBoneTransforms exits, the exact bytes the VS upload
    // consumes; per piece, one sim tick, coherent by construction). Runs
    // FIRST so the sanity gate below validates the authoritative rows and
    // the rescue caches learn them; unmapped items keep the bank pipeline.
    const auto pal_t0 = PerfClock::now();
    g_pal_served_frame = skate3::native_palette::ServeAuthoritativePalettes(base, scene);
    // Pair lighting/paint rows with the SERVED pose. The draw-time refresh
    // paired rows to the item's CAPTURED bones, and a capture can take
    // another instance's bank (same mesh, previous draw's constants still
    // bound): the serve above then fixes the pose but the paint stays the
    // other car's (F11: parked cars sharing one model, the near car drawn
    // by the game with yellow c22 showed the preceding instance's blue).
    // Take the rows of this frame's main-pass draw whose OWN palette sits
    // at the served bone 0 (nearest within the 1.5 m twin radius; same-mesh
    // instances are metres apart).
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.dbg_src != 11 || item.bones.size() < 12) {
        continue;
      }
      const auto range =
          char_draw_rows.equal_range((uint64_t(item.ib_obj) << 32) | item.vb_obj);
      // Every pass of one piece sits at the same pose, and a piece can draw
      // more than once per frame (the PS bank of an earlier pass can still
      // hold the PREVIOUS piece's constants): among draws at the served pose
      // the LAST one wins (the main pass, as in the draw-time refresh), and
      // only rows of the item's own shading family qualify (a foreign
      // family's layout rendered the tee invisible after create-a-skater).
      const float item_fam = item.char_rows[14 * 4 + 1];
      const CharDrawRows* best = nullptr;
      float best_d2 = 1.5f * 1.5f;
      for (auto it = range.first; it != range.second; ++it) {
        if (item_fam > 0.0f && it->second.rows[14 * 4 + 1] != item_fam) {
          continue;
        }
        for (int k = 0; k < 2; ++k) {
          const float dx = it->second.t[k][0] - item.bones[3];
          const float dy = it->second.t[k][1] - item.bones[7];
          const float dz = it->second.t[k][2] - item.bones[11];
          const float d2 = dx * dx + dy * dy + dz * dz;
          const bool tie = best != nullptr && std::fabs(d2 - best_d2) < 1e-4f;
          if (tie ? it->second.seq > best->seq : d2 < best_d2) {
            best_d2 = std::min(d2, best_d2);
            best = &it->second;
          }
        }
      }
      if (best != nullptr) {
        std::memcpy(item.char_rows, best->rows, sizeof(item.char_rows));
      }
    }
    // CAS skin custom graphic (see g_frame_skin_graphic): a per-piece CAS
    // setting, not a pose property, so every skin item takes its buffers'
    // latest record whatever path published it. Held across frames that
    // miss the draw (the capture races the publish).
    {
      static std::unordered_map<uint64_t, std::pair<SkinGraphic, uint64_t>> s_skin_graphic;
      static uint64_t s_sg_frame = 0;
      ++s_sg_frame;
      if (s_skin_graphic.size() > 1024) {
        s_skin_graphic.clear();
      }
      for (const auto& [k, sg] : skin_graphic) {
        s_skin_graphic[k] = {sg, s_sg_frame};
      }
      for (DrawItem& item : scene.items) {
        if (!item.char_skin) {
          continue;
        }
        const auto it =
            s_skin_graphic.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
        if (it != s_skin_graphic.end() && s_sg_frame - it->second.second <= 60) {
          std::memcpy(item.char_cg, it->second.first.cg, sizeof(item.char_cg));
          std::memcpy(item.char_cg_fetch, it->second.first.tf6, sizeof(item.char_cg_fetch));
          item.char_cg_valid = true;
        }
      }
    }
    // CAC cloth dirt / skin injury layer (see g_frame_char_layer), held
    // like the skin graphic.
    {
      static std::unordered_map<uint64_t, std::pair<CharLayer, uint64_t>> s_char_layer;
      static uint64_t s_cl_frame = 0;
      ++s_cl_frame;
      if (s_char_layer.size() > 1024) {
        s_char_layer.clear();
      }
      for (const auto& [k, cl] : char_layer) {
        s_char_layer[k] = {cl, s_cl_frame};
      }
      for (DrawItem& item : scene.items) {
        if (item.char_family != 2 || item.char_face || item.char_alpha) {
          continue;
        }
        const auto it = s_char_layer.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
        if (it != s_char_layer.end() && s_cl_frame - it->second.second <= 60) {
          const CharLayer& cl = it->second.first;
          if (cl.words[1] != 0 && cl.amount > 0.0f && cl.amount <= 4.0f) {
            std::memcpy(item.char_layer_fetch, cl.words, sizeof(item.char_layer_fetch));
            item.char_layer_amount = cl.amount;
            item.char_layer_valid = true;
          }
        }
      }
    }
    // Face injury layer (see g_frame_face_injury): held across frames that
    // miss the draw, like the skin graphic.
    {
      static std::unordered_map<uint64_t, std::pair<FaceInjury, uint64_t>> s_face_inj;
      static uint64_t s_fi_frame = 0;
      ++s_fi_frame;
      if (s_face_inj.size() > 256) {
        s_face_inj.clear();
      }
      for (const auto& [k, fi] : face_injury) {
        s_face_inj[k] = {fi, s_fi_frame};
      }
      for (DrawItem& item : scene.items) {
        if (!item.char_face) {
          continue;
        }
        const auto it = s_face_inj.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
        if (it != s_face_inj.end() && s_fi_frame - it->second.second <= 60) {
          const FaceInjury& fi = it->second.first;
          if (fi.tf6[1] != 0 && fi.amount > 0.0f && fi.amount <= 4.0f) {
            std::memcpy(item.face_inj_fetch, fi.tf6, sizeof(item.face_inj_fetch));
            item.face_inj_amount = fi.amount;
            item.face_inj_valid = true;
          }
        }
      }
    }
    // The game's world-shade pass casters (see g_frame_ws_keys): flag the
    // published items it drew; the native map renders exactly those.
    {
      scene.ws_draws = ws_draws;
      scene.ws_matched = 0;
      static FlatSet<uint64_t> matched;
      matched.reset(256);
      for (DrawItem& item : scene.items) {
        item.ws_caster = false;
        if (ws_keys.empty()) {
          continue;
        }
        const uint64_t k = (uint64_t(item.ib_obj) << 32) | item.vb_obj;
        if (ws_keys.count(k) != 0) {
          item.ws_caster = true;
          matched.insert(k);
        }
      }
      // The pass's caster list (the MeshContexts its render function draws)
      // is the complete set, including geometry the camera view culled.
      // Static world meshes are absolute (fmt 57, one placement per mesh
      // object; the pass's WorldShadow_defaultVS has no world matrix).
      scene.ws_items.clear();
      static size_t s_ws_items_last = 0;  // guest render thread only
      scene.ws_items.reserve(s_ws_items_last + 16);
      // The pass's own caster list (see OnWorldShadowRender).
      const std::vector<uint32_t>& ws_view_ctxs = ws_draw_ctxs;
      if (!ws_entries.empty()) {
        // Exact rebuild (see g_frame_ws_entries): every buffer pair the pass
        // drew becomes one caster with exactly the pass's own draws.
        // Geometry from the published copy when there is one, else from the
        // pass's caster contexts by mesh -- never their island selection,
        // which by frame end is the main camera's cull.
        static FlatMap<uint64_t, size_t> pub_key;
        pub_key.reset(scene.items.size());
        for (size_t i = 0; i < scene.items.size(); ++i) {
          DrawItem& it = scene.items[i];
          it.ws_caster = false;
          if (!it.skinned && it.mesh != 0) {
            auto [slot, inserted] =
                pub_key.try_emplace((uint64_t(it.ib_obj) << 32) | it.vb_obj);
            if (inserted) *slot = i;
          }
        }
        std::unordered_map<uint64_t, uint32_t> ctx_mesh;  // key -> mesh
        std::unordered_set<uint32_t> ctx_seen;
        for (const uint32_t wctx : ws_view_ctxs) {
          uint32_t record = 0, mesh = 0;
          if (!GuestTryLoadU32(base, wctx, &record) || !GuestReadableApprox(base, record) ||
              !GuestTryLoadU32(base, record, &mesh) || !GuestReadableApprox(base, mesh) ||
              !ctx_seen.insert(mesh).second) {
            continue;
          }
          uint32_t ib = 0, vb = 0;
          if (GuestTryLoadU32(base, mesh + kMeshIndexBuffer, &ib) &&
              GuestTryLoadU32(base, mesh + kMeshVertexBuffer, &vb)) {
            ctx_mesh.emplace((uint64_t(ib) << 32) | vb, mesh);
          }
        }
        uint32_t from_pub = 0, from_ctx = 0, unresolved = 0;
        for (auto& [k, ents] : ws_entries) {
          DrawItem wi;
          if (const size_t* pi = pub_key.find(k)) {
            wi = scene.items[*pi];
            ++from_pub;
          } else {
            const auto cit = ctx_mesh.find(k);
            if (cit == ctx_mesh.end() || !BuildItemFromMeshCached(base, cit->second, wi) ||
                wi.skinned) {
              ++unresolved;
              continue;
            }
            ++from_ctx;
          }
          wi.draws.clear();
          for (const DrawEntry& e : ents) {
            if (e.index_count != 0 && e.index_count <= wi.ib_count) {
              wi.draws.push_back(e);
            }
          }
          if (wi.draws.empty()) {
            ++unresolved;
            continue;
          }
          // The pass draws without a world matrix: absolute positions.
          static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
          std::memcpy(wi.world, kIdentity, sizeof(kIdentity));
          wi.ws_caster = true;
          scene.ws_items.push_back(std::move(wi));
        }
        static uint32_t s_wse_log = 0;
        if ((s_wse_log++ % 600u) == 0u) {
          REXLOG_INFO("native-scene: world-shade casters from the pass's own draws: {} keys -> "
                      "{} published + {} by caster-list mesh, {} unresolved",
                      ws_entries.size(), from_pub, from_ctx, unresolved);
        }
      } else if (!ws_view_ctxs.empty()) {
        std::unordered_map<uint32_t, size_t> pub_mesh;
        for (size_t i = 0; i < scene.items.size(); ++i) {
          pub_mesh.emplace(scene.items[i].mesh, i);
        }
        std::unordered_set<uint32_t> ws_seen;
        for (const uint32_t wctx : ws_view_ctxs) {
          DrawItem wi;
          if (!BuildItemGeometry(base, wctx, wi) || wi.skinned || wi.pos_fmt != 57 ||
              !ws_seen.insert(wi.mesh).second) {
            continue;
          }
          const auto dit = g_dyn_dispatch_meshes.find(wi.mesh);
          if (dit != g_dyn_dispatch_meshes.end() && g_guest_frame - dit->second <= 1800) {
            continue;  // mesh-local batched piece: no absolute placement here
          }
          const auto pit = pub_mesh.find(wi.mesh);
          if (pit != pub_mesh.end()) {
            DrawItem& pub = scene.items[pit->second];
            if (!pub.ws_caster) {
              pub.ws_caster = true;
              matched.insert((uint64_t(pub.ib_obj) << 32) | pub.vb_obj);
            }
            continue;
          }
          wi.ws_caster = true;
          scene.ws_items.push_back(std::move(wi));
        }
      }
      s_ws_items_last = scene.ws_items.size();
      scene.ws_matched = uint32_t(matched.size() + scene.ws_items.size());
      static uint32_t s_ws_log = 0;
      if ((!ws_keys.empty() || !ws_view_ctxs.empty()) && (s_ws_log++ % 600u) == 0u) {
        REXLOG_INFO("native-scene: world-shade pass {} draws, {} distinct buffers; "
                    "caster list {} entries -> {} published casters + {} off-view casters",
                    ws_draws, ws_keys.size(), ws_view_ctxs.size(), matched.size(),
                    scene.ws_items.size());
      }
    }
    // The game's skate.Park world-shadow passes (see ParkWsDraw): rebuild
    // exactly the piece instances they drew. A pass draw names its instance
    // by buffers (each park piece owns its own) and carries its world
    // matrix; the mesh to draw comes from this frame's published copy, or --
    // for a piece the camera view culled -- from the last time it was
    // published (park pieces only move in the editor, and the pass's own
    // matrix is used regardless).
    {
      struct ParkMesh {
        uint32_t mesh = 0;
        std::vector<DrawEntry> draws;
        uint64_t frame = 0;
        uint32_t clip_tex = 0;  // alphatest pieces' diffuse (see ParkWsCaster)
      };
      static std::unordered_map<uint64_t, ParkMesh> s_park_mesh;
      static uint64_t s_park_frame = 0;
      ++s_park_frame;
      for (const DrawItem& item : scene.items) {
        if (item.dynobj < 4 || item.dynobj > 6 || item.mesh == 0 || item.draws.empty()) {
          continue;
        }
        ParkMesh& pm = s_park_mesh[(uint64_t(item.ib_obj) << 32) | item.vb_obj];
        if (pm.mesh != item.mesh || pm.draws.size() != item.draws.size()) {
          pm.mesh = item.mesh;
          pm.draws = item.draws;
        }
        pm.clip_tex = item.dynobj == 5 ? item.diffuse_tex : 0u;
        pm.frame = s_park_frame;
      }
      // Pieces unseen for a long while are gone (editor deletes, map change).
      if (s_park_mesh.size() > 4096) {
        for (auto it = s_park_mesh.begin(); it != s_park_mesh.end();) {
          it = s_park_frame - it->second.frame > 3600 ? s_park_mesh.erase(it) : std::next(it);
        }
      }
      scene.park_ws_casters.clear();
      static size_t s_park_casters_last = 0;  // guest render thread only
      scene.park_ws_casters.reserve(s_park_casters_last + 16);
      scene.park_ws_draws = uint32_t(park_ws_draws.size());
      // One caster per INSTANCE = (buffers, world matrix). Identical pieces
      // placed several times share their buffers (capture 1790871823: 543
      // pass draws, 33 buffer pairs, 378 placements -- a rail drawn 51
      // times); keyed by buffers alone, every copy but the first was lost.
      std::unordered_map<uint64_t, size_t> by_key;
      for (const ParkWsDraw& d : park_ws_draws) {
        const auto pit = s_park_mesh.find(d.key);
        if (pit == s_park_mesh.end()) {
          continue;
        }
        uint64_t ik = d.key * 1099511628211ull;
        for (float f : d.world) {
          uint32_t u;
          std::memcpy(&u, &f, 4);
          ik = (ik ^ u) * 1099511628211ull;
        }
        auto [it, inserted] = by_key.try_emplace(ik, scene.park_ws_casters.size());
        if (inserted) {
          FrameScene::ParkWsCaster c;
          c.mesh = pit->second.mesh;
          c.draws = pit->second.draws;
          c.clip_tex = pit->second.clip_tex;
          std::memcpy(c.world, d.world, sizeof(c.world));
          scene.park_ws_casters.push_back(std::move(c));
        }
        scene.park_ws_casters[it->second].tiles |= uint8_t(1u << d.tile);
      }
      scene.park_ws_matched = uint32_t(scene.park_ws_casters.size());
      s_park_casters_last = scene.park_ws_casters.size();
      std::memcpy(scene.park_ws_pass_rows, park_ws_rows, sizeof(park_ws_rows));
      scene.park_ws_tile_have[0] = park_ws_rows_have[0];
      scene.park_ws_tile_have[1] = park_ws_rows_have[1];
      scene.park_ws_build = park_ws_resolved || !park_ws_draws.empty();
      scene.park_ws_recv_valid = g_park_ws_recv_have;
      if (g_park_ws_recv_have) {
        std::memcpy(scene.park_ws_recv, g_park_ws_recv, sizeof(scene.park_ws_recv));
      }
      static uint32_t s_park_ws_log = 0;
      if (scene.park_ws_build && (s_park_ws_log++ % 600u) == 0u) {
        REXLOG_INFO("native-scene: park world-shadow passes {} draws (inner {}, outer {}), "
                    "{} instances matched, resolved={}",
                    park_ws_draws.size(), int(park_ws_rows_have[0]), int(park_ws_rows_have[1]),
                    scene.park_ws_casters.size(), int(park_ws_resolved));
      }
      // The world's skate.Park receivers (see g_frame_dmo_keys). Held for a
      // few builds after the last such draw: an item this build carries over
      // without the game having drawn it must not blink to the default
      // shading model and back.
      static std::unordered_map<uint64_t, uint32_t> s_dmo_seen;
      static uint32_t s_dmo_build = 0;
      ++s_dmo_build;
      dmo_keys.for_each([&](uint64_t k) { s_dmo_seen[k] = s_dmo_build; });
      if ((s_dmo_build & 63u) == 0u) {
        for (auto it = s_dmo_seen.begin(); it != s_dmo_seen.end();) {
          it = s_dmo_build - it->second > 30u ? s_dmo_seen.erase(it) : std::next(it);
        }
      }
      scene.park_dmo_valid = g_park_dmo_have && g_park_ws_recv_have;
      if (scene.park_dmo_valid) {
        std::memcpy(scene.park_dmo_rows, g_park_dmo_rows, sizeof(scene.park_dmo_rows));
      }
      scene.park_dmo_items = 0;
      for (DrawItem& item : scene.items) {
        item.dmo_recv = false;
        if (!scene.park_dmo_valid || s_dmo_seen.empty() || item.env_family == 0) {
          continue;
        }
        const auto it = s_dmo_seen.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
        if (it != s_dmo_seen.end() && s_dmo_build - it->second <= 30u) {
          item.dmo_recv = true;
          ++scene.park_dmo_items;
        }
      }
      // The editor's preview object (see DrawItem::foreground / g_frame_fg_world).
      // The preview is the SAME mesh as every placed instance of that piece,
      // so the buffer key alone also caught the park's placed copies (they
      // were all moved onto the preview). Only the preview's own copy is
      // taken -- published with an identity world (its placement exists only
      // in the foreground draw's VS rows) or already at the draw's world --
      // and when no such copy was published (the piece is already placed in
      // the park: only placed copies exist) one is cloned from any copy of
      // the mesh. Its draws are the foreground draw's own (fg_entries), not a
      // placed copy's camera-culled island list.
      // The same for the editor's selected piece (see DrawItem::xray): its
      // draws carry the world in their VS rows too, and its published copy
      // is the identity one.
      {
        for (DrawItem& item : scene.items) {
          item.foreground = false;
          item.xray = false;
          item.highlight = false;
        }
        std::vector<DrawItem> fg_clones;
        const auto apply = [&](const std::unordered_map<uint64_t, std::array<float, 16>>& worlds,
                               const std::unordered_map<uint64_t, std::vector<DrawEntry>>& entries,
                               int kind) {  // 0 foreground, 1 xray, 2 highlight
          if (worlds.empty()) {
            return;
          }
          std::unordered_set<uint64_t> done;
          const auto adopt = [&](DrawItem& it, uint64_t k, const float* fw) {
            (kind == 1 ? it.xray : kind == 2 ? it.highlight : it.foreground) = true;
            if (kind == 2) {
              std::memcpy(it.highlight_rgb, hl_rgb, sizeof(it.highlight_rgb));
            }
            // Runtime art bound in place of the material diffuse (a sign's
            // logo; see g_frame_fg_fetch): adopted when it is not the
            // material's own texture.
            if (kind == 0) {
              const auto ffit = fg_fetch.find(k);
              uint32_t own_w1 = 0;
              if (ffit != fg_fetch.end() && it.diffuse_tex != 0 &&
                  GuestTryLoadU32(base, it.diffuse_tex + 8 * 4, &own_w1) &&
                  (ffit->second[1] & 0x1FFFF000u) != (own_w1 & 0x1FFFF000u)) {
                std::memcpy(it.diffuse_fetch, ffit->second.data(), sizeof(it.diffuse_fetch));
              }
            }
            std::memcpy(it.world, fw, sizeof(it.world));
            const auto eit = entries.find(k);
            if (eit != entries.end()) {
              std::vector<DrawEntry> ds;
              for (const DrawEntry& e : eit->second) {
                if (e.index_count != 0 && e.index_count <= it.ib_count) {
                  ds.push_back(e);
                }
              }
              if (!ds.empty()) {
                it.draws = std::move(ds);
              }
            }
          };
          for (DrawItem& item : scene.items) {
            const uint64_t k = (uint64_t(item.ib_obj) << 32) | item.vb_obj;
            const auto fit = worlds.find(k);
            if (fit == worlds.end() || item.foreground || item.xray || item.highlight) {
              continue;
            }
            const float* fw = fit->second.data();
            bool identity = true, same = true;
            for (int c = 0; c < 16; ++c) {
              const float id = (c % 5 == 0) ? 1.0f : 0.0f;
              identity = identity && std::fabs(item.world[c] - id) < 1e-4f;
              same = same && std::fabs(item.world[c] - fw[c]) < 1e-3f;
            }
            if (identity || same) {
              adopt(item, k, fw);
              done.insert(k);
            }
          }
          for (const auto& [k, w] : worlds) {
            if (done.count(k) != 0) {
              continue;
            }
            for (const DrawItem& item : scene.items) {
              if (((uint64_t(item.ib_obj) << 32) | item.vb_obj) == k && !item.skinned) {
                DrawItem clone = item;
                clone.retained = false;
                clone.selected = false;
                clone.foreground = false;
                clone.xray = false;
                clone.highlight = false;
                // Not the placed copy's instance: keep ctx-keyed systems
                // (interpolation, the culled-ctx guest filter) off it.
                clone.ctx = 0;
                adopt(clone, k, w.data());
                fg_clones.push_back(std::move(clone));
                break;
              }
            }
          }
        };
        apply(fg_world, fg_entries, 0);
        apply(xray_world, xray_entries, 1);
        apply(hl_world, hl_entries, 2);
        const auto inst_at = [&](uint64_t k, const float* w) -> const DefaultInst* {
          const auto it = default_inst.find(k);
          if (it == default_inst.end()) {
            return nullptr;
          }
          for (const DefaultInst& di : it->second) {
            if (std::fabs(di.t[0] - w[12]) < 0.05f && std::fabs(di.t[1] - w[13]) < 0.05f &&
                std::fabs(di.t[2] - w[14]) < 0.05f) {
              return &di;
            }
          }
          return nullptr;
        };
        // A preview / selected piece's mesh: copies the game did NOT draw
        // this frame are not in the world (the menu's object only appears
        // there once chosen); a world-space leftover of the preview drew a
        // second, background copy. Genuinely placed copies are drawn by the
        // game's own _defaultPS pass and keep their place.
        if (!fg_world.empty() || !xray_world.empty() || !hl_world.empty()) {
          const auto involved = [&](uint64_t k) {
            return fg_world.count(k) != 0 || xray_world.count(k) != 0 || hl_world.count(k) != 0;
          };
          scene.items.erase(
              std::remove_if(scene.items.begin(), scene.items.end(),
                             [&](const DrawItem& it) {
                               if (it.foreground || it.xray || it.highlight) {
                                 return false;
                               }
                               const uint64_t k = (uint64_t(it.ib_obj) << 32) | it.vb_obj;
                               return involved(k) && inst_at(k, it.world) == nullptr;
                             }),
              scene.items.end());
        }
        // Per-instance runtime art on placed park diffuse pieces (a placed
        // sign's logo; see g_frame_default_inst), adopted when it is not the
        // material's own texture.
        if (!default_inst.empty()) {
          for (DrawItem& it : scene.items) {
            if (it.dynobj != 4 || it.diffuse_tex == 0 || it.foreground) {
              continue;
            }
            const DefaultInst* di =
                inst_at((uint64_t(it.ib_obj) << 32) | it.vb_obj, it.world);
            uint32_t own_w1 = 0;
            if (di != nullptr && di->fetch[1] != 0 &&
                GuestTryLoadU32(base, it.diffuse_tex + 8 * 4, &own_w1) &&
                (di->fetch[1] & 0x1FFFF000u) != (own_w1 & 0x1FFFF000u)) {
              std::memcpy(it.diffuse_fetch, di->fetch, sizeof(it.diffuse_fetch));
            }
          }
        }
        for (DrawItem& c : fg_clones) {
          scene.items.push_back(std::move(c));
        }
      }
      static uint32_t s_dmo_log = 0;
      if (!dmo_keys.empty() && (s_dmo_log++ % 600u) == 0u) {
        REXLOG_INFO("native-scene: skate.Park world receivers {} draws, {} items", dmo_keys.size(),
                    scene.park_dmo_items);
      }
    }
    // CAS subsurface scattering (see g_frame_skin_sss / DrawItem::sss): a
    // skin or face piece the game ran its texture-space lighting pass for.
    // Held a couple of builds (the capture races the publish); outside the
    // editor the game draws the plain compiles and the record ages out.
    {
      static std::unordered_map<uint64_t, std::pair<SkinSssRecord, uint64_t>> s_skin_sss;
      static uint64_t s_sss_frame = 0;
      ++s_sss_frame;
      if (s_skin_sss.size() > 1024) {
        s_skin_sss.clear();
      }
      for (const auto& [k, r] : skin_sss) {
        if (r.unwrap && r.main) {
          s_skin_sss[k] = {r, s_sss_frame};
        }
      }
      for (DrawItem& item : scene.items) {
        item.sss = false;
        // CAS skin / face, or a cutscene NPC's defaultcharacter-family skin
        // piece (character_skin_*: the only fam-1 pieces the game runs a
        // texture-space lighting pass for, so an unwrap record identifies
        // them).
        const bool npc_skin = !item.char_skin && !item.char_face && item.char_family == 1;
        if (!item.char_skin && !item.char_face && !npc_skin) {
          continue;
        }
        const auto it = s_skin_sss.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
        if (it == s_skin_sss.end() || s_sss_frame - it->second.second > 2) {
          continue;
        }
        const SkinSssRecord& r = it->second.first;
        const uint32_t* mask = npc_skin ? r.mask6 : item.char_face ? r.mask7 : r.mask8;
        const float amount = npc_skin ? r.amount12 : r.amount;
        if (mask[1] == 0 || !(amount > 0.0f && amount <= 4.0f)) {
          continue;
        }
        item.sss = true;
        item.sss_amount = amount;
        std::memcpy(item.sss_mask, mask, sizeof(item.sss_mask));
      }
    }
    g_frame_bpal_ns =uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   PerfClock::now() - pal_t0)
                                   .count());
    g_pw_bpal.Add(g_frame_bpal_ns);
    g_pal_served_total.fetch_add(g_pal_served_frame, std::memory_order_relaxed);
    std::unordered_map<uint32_t, uint32_t> pub_count;
    for (const DrawItem& item : scene.items) {
      // Ropa garments count in EITHER resolved mode (a rigid-resolved copy
      // has no bones but is very much alive; rescuing a pending clone next
      // to it would draw a duplicate garment).
      if (item.ropa || (item.skinned && !item.bones.empty())) {
        ++pub_count[item.mesh];
      }
      if (item.ropa && item.caster_bank) {
        g_ropa_caster.fetch_add(1, std::memory_order_relaxed);
      }
    }
    for (const DrawItem& item : scene.items) {
      if (!(item.ropa || (item.skinned && !item.bones.empty())) ||
          pub_count[item.mesh] != 1) {
        continue;
      }
      if (item.draws.empty()) {
        // Vetoed/dropped this frame (incoherent-palette trip): whatever
        // state it carries must not become the "last good" rescue entry.
        continue;
      }
      if (item.ropa) {
        // Remember the resolved mode + transform (see g_ropa_state_cache).
        if (g_ropa_state_cache.size() < 512) {
          RopaResolvedState& c = g_ropa_state_cache[item.mesh];
          c.skinned = item.skinned && !item.bones.empty();
          std::memcpy(c.world, item.world, sizeof(c.world));
          c.bones = item.bones;
          c.frame = g_guest_frame;
        }
      }
    }
    // Per-instance palette cache refresh (see g_bones_cache_ctx): every
    // published skinned character-family item keeps its OWN last palette
    // keyed by ctx: no pub_count gate needed, the key IS the instance.
    for (const DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.empty() || item.caster_bank ||
          item.ctx == 0 || item.char_family == 0 || item.ropa ||
          item.draws.empty()) {
        continue;
      }
      if (g_bones_cache_ctx.size() > 2048) {
        for (auto it = g_bones_cache_ctx.begin();
             it != g_bones_cache_ctx.end();) {
          it = g_guest_frame - it->second.frame > 60
                   ? g_bones_cache_ctx.erase(it)
                   : std::next(it);
        }
      }
      CachedBones& cb = g_bones_cache_ctx[item.ctx];
      cb.bones = item.bones;
      cb.frame = g_guest_frame;
    }
    // Ghost guard for the non-live publish paths below (palette rescues,
    // rigid rescues, the LW gap fill): they exist to bridge frames where the
    // game DID draw an instance but our capture missed it. The game keeps
    // submitting and animating instances it does not draw (pooled / hidden
    // AI skaters, fully faded-out pedestrians); republishing those drew a
    // character where the game shows none, standing where it last was. So:
    // nothing to republish when the instance's LW fade is fully out, or when
    // it sits inside the guest camera's view and no perspective draw this
    // frame used its mesh's index buffer (draw_rs records every one; a
    // visible instance of this mesh would have been drawn). Out-of-view
    // instances keep the rescue (their shadows can still be on screen).
    float rescue_vp[16];
    for (int i = 0; i < 16; ++i) {
      rescue_vp[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
    }
    const auto ghost_at = [&](const DrawItem& c, const float pos[3]) {
      if (c.ctx != 0) {
        float lw_alpha = 1.0f;
        uint32_t lw_entity = 0;
        if (skate3::native_lw::LookupLwCtx(c.ctx, &lw_alpha, &lw_entity) && lw_alpha <= 0.0f) {
          return true;  // faded out: the game draws nothing of it
        }
      }
      if (draw_rs.find(c.ib_obj) != nullptr) {
        return false;  // the game drew this mesh this frame
      }
      return !BoxOutsideFrustum(pos, 1.5f, rescue_vp, 1.0f);
    };
    const auto ghost_skinned = [&](const DrawItem& c, const std::vector<float>& bones) {
      if (bones.size() < 12) {
        return false;
      }
      const float pos[3] = {bones[3], bones[7], bones[11]};
      if (!ghost_at(c, pos)) {
        return false;
      }
      g_ghost_suppressed.fetch_add(1, std::memory_order_relaxed);
      return true;
    };
    // Per-instance rescue: a refused/pending skinned character capture
    // whose ctx did not publish this frame re-publishes with ITS OWN last
    // palette (<= 10 frames fresh), regardless of how many clones of the
    // mesh are alive.
    for (const auto& [ctxk, cand] : pending_skinned_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      // The game's own packed matrices for this instance first (exact
      // ctx -> instance link, no capture needed); the cached palette only
      // when the instance did not pack.
      std::vector<float> served_bones;
      const bool from_pack =
          skate3::native_palette::ServeCtxPalette(ctxk, &served_bones);
      const auto bit = g_bones_cache_ctx.find(ctxk);
      if (!from_pack && (bit == g_bones_cache_ctx.end() ||
                         g_guest_frame - bit->second.frame > 10)) {
        continue;  // stale = an old pose; one missing frame beats a teleport
      }
      if (ghost_skinned(*cand, from_pack ? served_bones : bit->second.bones)) {
        continue;  // the game is not drawing this instance (see ghost_at)
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.bones = from_pack ? std::move(served_bones) : bit->second.bones;
      rescued.pending = false;
      rescued.caster_bank = false;
      rescued.dbg_src = from_pack ? 11 : 3;
      ++pub_count[rescued.mesh];
      // Mark the ctx published so the LW gap fill below cannot double-
      // publish the same instance this frame.
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_lw_ctx_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // Rigid clone rescue: a piece whose world-bearing draw was missed this
    // frame (culled, or claimed by another clone of the mesh) re-publishes
    // with its own last published world instead of vanishing for a frame.
    // Statics never move, and movables at most drift one frame behind; the
    // mesh check keeps a reused ctx address from wearing a stale placement.
    for (const auto& [ctxk, cand] : pending_rigid_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      const auto cit = g_rigid_world_cache.find(ctxk);
      if (cit == g_rigid_world_cache.end() || cit->second.mesh != cand->mesh ||
          g_guest_frame - cit->second.frame > 600) {
        continue;  // no trusted placement; a dropped frame beats an origin ghost
      }
      {
        const float pos[3] = {cit->second.world[12], cit->second.world[13],
                              cit->second.world[14]};
        if (ghost_at(*cand, pos)) {
          g_ghost_suppressed.fetch_add(1, std::memory_order_relaxed);
          continue;  // the game is not drawing this instance (see ghost_at)
        }
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      std::memcpy(rescued.world, cit->second.world, sizeof(rescued.world));
      rescued.pending = false;
      rescued.dbg_src = 12;
      ++pub_count[rescued.mesh];
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_rigid_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // Sort-list copies of dynamically-dispatched piece meshes (see
    // g_dyn_dispatch_meshes): the game batches FAR clones through the
    // static lists, where no per-entity capture carries their placement.
    // Publish with the ctx owner's instance matrix; a live dynamic capture
    // for the same ctx always wins (near clones arrive through both paths
    // in the same frame, and double-publishing would z-fight).
    for (auto& [ctxk, item] : sortlist_local_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      if (!ReadCtxInstanceWorld(base, ctxk, item.world)) {
        g_world_props.fetch_add(1, std::memory_order_relaxed);
        continue;  // no placement: dropping beats an origin ghost
      }
      ++pub_count[item.mesh];
      scene.items.push_back(std::move(item));
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_sortlist_local_pub.fetch_add(1, std::memory_order_relaxed);
    }
    // Refused ropa captures re-publish last frame's resolved state: mode,
    // world AND palette together (mixing frames' interpretations is the
    // mangled-ribbon bug; see g_ropa_state_cache).
    for (const auto& [mesh, cand] : pending_ropa_by_mesh) {
      if (pub_count.find(mesh) != pub_count.end()) {
        continue;  // a live copy published; nothing to rescue
      }
      // Dropped-garment gate: when the owning entity's garment table no
      // longer claims this VB (the game removed the cloth model; its
      // pieces render skinned with the body now), the cached rigid state
      // is a ghost: the retained drape tracks the live character as a
      // floating garment. Suppress instead of rescuing.
      if (skate3::native_entity::RopaGarmentDropped(base, cand->ctx,
                                                    cand->vb_obj)) {
        // The game renders a dropped garment SKINNED with the body (its
        // sim-inactive path); the cached rigid state is a ghost. Resolve
        // with the owning instance's own live packed palette; when that
        // fails, suppress (invisible beats floating).
        float rows[96 * 12];
        const uint32_t n = skate3::native_entity::ServeInstancePalette(
            base, cand->ctx, rows, 96);
        static std::atomic<uint32_t> s_drop_logged{0};
        const uint32_t ln =
            s_drop_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 511u) == 0) {
          REXLOG_INFO(
              "native-scene: ropa rescue DROPPED-GARMENT {} mesh={:08X} "
              "ctx={:08X} vb={:08X} rows={} (n={})",
              n ? "resolved SKINNED" : "suppressed", cand->mesh, cand->ctx,
              cand->vb_obj, n, ln);
        }
        if (n != 0) {
          scene.items.push_back(*cand);
          DrawItem& resolved = scene.items.back();
          resolved.skinned = true;
          resolved.bones.assign(rows, rows + size_t(n) * 12);
          resolved.pending = false;
          resolved.caster_bank = false;
          resolved.dbg_src = 4;
          if (resolved.ctx != 0) {
            dyn_slot.try_emplace(resolved.ctx, scene.items.size() - 1);
          }
          g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
        }
        continue;
      }
      const auto rit = g_ropa_state_cache.find(mesh);
      if (rit == g_ropa_state_cache.end() ||
          g_guest_frame - rit->second.frame > 30) {
        continue;
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.skinned = rit->second.skinned;
      rescued.bones = rit->second.bones;
      std::memcpy(rescued.world, rit->second.world, sizeof(rescued.world));
      rescued.pending = false;
      rescued.dbg_src = 4;
      if (rescued.ctx != 0) {
        dyn_slot.try_emplace(rescued.ctx, scene.items.size() - 1);
      }
      g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // LW gap fill (see g_lw_last_items): republish live entities' items
    // whose ctx skipped this frame's records entirely, the class the
    // pending rescues cannot see (no capture happened at all).
    if (REXCVAR_GET(skate3_native_render_scene_lw_gap_fill)) {
      const uint64_t now = g_guest_frame;
      for (auto it = g_lw_last_items.begin(); it != g_lw_last_items.end();) {
        if (dyn_slot.find(it->first) != dyn_slot.end()) {
          ++it;  // published live this frame; refreshed below
          continue;
        }
        LwRetained& r = it->second;
        float alpha = 1.0f;
        uint32_t entity = 0;
        if (now - r.frame > 2 ||
            !skate3::native_lw::LookupLwCtx(it->first, &alpha, &entity) ||
            ghost_skinned(r.item, r.item.bones)) {
          it = g_lw_last_items.erase(it);
          continue;
        }
        scene.items.push_back(r.item);
        scene.items.back().dbg_src = 9;  // gap fill (refresh below skips it)
        g_lw_gap_filled.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_fill_logged{0};
        const uint32_t ln = s_fill_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: LW gap fill ctx={:08X} mesh={:08X} fam={} "
              "age={} alpha={:.2f} (n={})",
              it->first, r.item.mesh, r.item.char_family, now - r.frame,
              alpha, ln);
        }
        ++it;
      }
      for (const DrawItem& item : scene.items) {
        // NOTE: lw_entity is not stamped yet here (the stamp pass runs just
        // before the smoothing block); LW membership is enforced at FILL
        // time by the store lookup; non-LW entries simply expire unused.
        if (item.char_family == 0 || item.ctx == 0 || item.ropa ||
            item.pending || item.caster_bank || item.dbg_src == 9 ||
            !item.skinned || item.bones.empty() || item.draws.empty()) {
          continue;
        }
        if (g_lw_last_items.size() > 512 &&
            g_lw_last_items.find(item.ctx) == g_lw_last_items.end()) {
          continue;  // growth backstop
        }
        LwRetained& r = g_lw_last_items[item.ctx];
        r.item = item;
        r.frame = now;
      }
    } else if (!g_lw_last_items.empty()) {
      g_lw_last_items.clear();
    }
  }
  BuildSectMark(3);
  // Per-INSTANCE character-lighting fallback (see g_char_rows_cache_inst):
  // the general rescue for capture-failed frames, keyed (mesh,ctx) and
  // entity-checked. Runs before the legacy single-instance mesh fallback so
  // instanced pieces (and the player, which is not LW-mapped) hold their
  // last validated rows, keeping hair in the blended sub-pass instead of
  // flipping to the legacy opaque path (the high-fps hair/garment flicker).
  if (REXCVAR_GET(skate3_native_render_scene_char_rows_inst)) {
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      float lw_alpha_unused = 1.0f;
      uint32_t lw_entity = 0;
      skate3::native_lw::LookupLwCtx(item.ctx, &lw_alpha_unused, &lw_entity);
      const uint64_t key = (uint64_t(item.mesh) << 32) | item.ctx;
      if (item.char_rows[14 * 4 + 1] > 0.0f) {
        if (g_char_rows_cache_inst.size() > 8192) {
          g_char_rows_cache_inst.clear();
        }
        CharRowsInst& ci = g_char_rows_cache_inst[key];
        std::memcpy(ci.rows.data(), item.char_rows, sizeof(item.char_rows));
        ci.entity = lw_entity;
        if (item.ropa) {
          skate3::native_entity::CtxInfo ident;
          if (skate3::native_entity::LookupCtx(item.ctx, &ident)) {
            if (g_char_rows_cache_ent.size() > 4096) {
              g_char_rows_cache_ent.clear();
            }
            CharRowsEnt& ce = g_char_rows_cache_ent[ident.entity];
            std::memcpy(ce.rows.data(), item.char_rows,
                        sizeof(item.char_rows));
            ce.fam = uint8_t(item.char_family);
          }
        }
      } else {
        const auto iit = g_char_rows_cache_inst.find(key);
        if (iit != g_char_rows_cache_inst.end() &&
            iit->second.entity == lw_entity) {
          std::memcpy(item.char_rows, iit->second.rows.data(),
                      sizeof(item.char_rows));
          g_char_rows_inst_served.fetch_add(1, std::memory_order_relaxed);
        } else if (item.ropa) {
          // Sim-switch bridge: rows from the garment's sibling renderable
          // via the common owning entity (see g_char_rows_cache_ent). The
          // row layout is family-specific, so a cross-family serve routes
          // the receiver through the donor family's shading path too.
          skate3::native_entity::CtxInfo ident;
          if (skate3::native_entity::LookupCtx(item.ctx, &ident)) {
            const auto eit = g_char_rows_cache_ent.find(ident.entity);
            if (eit != g_char_rows_cache_ent.end()) {
              std::memcpy(item.char_rows, eit->second.rows.data(),
                          sizeof(item.char_rows));
              if (eit->second.fam != 0 &&
                  eit->second.fam != item.char_family) {
                static std::atomic<uint32_t> s_fam_logged{0};
                const uint32_t ln =
                    s_fam_logged.fetch_add(1, std::memory_order_relaxed);
                if (ln < 8 || (ln & 1023u) == 0) {
                  REXLOG_DEBUG(
                      "native-scene: rows bridge cross-family serve "
                      "mesh={:08X} fam {}->{} (n={})",
                      item.mesh, item.char_family, eit->second.fam, ln);
                }
                item.char_family = eit->second.fam;
              }
              g_char_rows_ent_served.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
  }
  // Cross-frame character-lighting fallback (see g_char_rows_cache): items
  // whose capture chain never validated THIS frame reuse their garment's
  // last good rows instead of dropping to the empirical look (and, for
  // hair, out of the blended sub-pass). Single-instance meshes only;
  // clones carry per-instance rows.
  {
    std::unordered_map<uint32_t, uint32_t> char_mesh_count;
    for (const DrawItem& item : scene.items) {
      if (item.char_family != 0) {
        ++char_mesh_count[item.mesh];
      }
    }
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.char_rows[14 * 4 + 1] > 0.0f ||
          char_mesh_count[item.mesh] != 1) {
        continue;
      }
      const auto cit = g_char_rows_cache.find(item.mesh);
      if (cit != g_char_rows_cache.end()) {
        std::memcpy(item.char_rows, cit->second.data(), sizeof(item.char_rows));
        g_char_rows_reused.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  BuildSectMark(4);
  // Publish-gap telemetry and fill ("skater flickered invisible for a
  // moment"): a skinned/ropa/character mesh that published recently can
  // vanish from the publish stream for 1-2 built frames at high render
  // rates (the body serve races the sim; more frequent the faster the
  // renderer runs, so Vulkan flickered worst). Re-publish the previous
  // frame's capture for up to two missed frames - a one-frame-old pose is
  // imperceptible at those rates, and a real despawn stops the fill at the
  // cap. The rate-limited log keeps naming mesh and gap so sightings stay
  // diagnosable.
  {
    struct PubTrack {
      uint64_t frame = 0;
      DrawItem item;
    };
    static std::unordered_map<uint32_t, PubTrack> s_last_pub;
    static uint64_t s_pub_frame = 0;
    ++s_pub_frame;
    if (s_last_pub.size() > 4096) {
      s_last_pub.clear();
    }
    for (DrawItem& item : scene.items) {
      if (!(item.ropa || item.char_family != 0 ||
            (item.skinned && !item.bones.empty()))) {
        continue;
      }
      PubTrack& t = s_last_pub[item.mesh];
      const uint64_t gap = t.frame != 0 ? s_pub_frame - t.frame : 0;
      if (item.char_family != 0 && item.caster_bank) {
        // The main-pass fixup missed and this piece reached publish as its
        // raw ortho-bank capture (shadow-pass constants, no lighting rows):
        // consumers reading its rows misjudge it for the frame (the caster
        // fade gate skipped the piece, blinking the skater's shadow off).
        // Serve the previous frame's main-view capture instead, and never
        // store shadow-bank state as the good copy.
        if (t.frame != 0 && gap <= 2 && !t.item.caster_bank) {
          item = t.item;
          t.frame = s_pub_frame;
          static std::atomic<uint64_t> s_bank_swaps{0};
          const uint64_t n = s_bank_swaps.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 255u) == 0) {
            REXLOG_INFO(
                "native-scene: caster-bank publish swapped for last main-view "
                "capture mesh={:08X} fam={} (n={})",
                item.mesh, item.char_family, n + 1);
          }
        }
        continue;
      }
      t.frame = s_pub_frame;
      t.item = item;
      if (gap >= 2 && gap <= 4) {
        g_dyn_gap.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> gap_logged{0};
        const uint32_t ln = gap_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: dyn publish GAP mesh={:08X} missing {} frame(s) "
              "ropa={} skinned={} fam={} src={}",
              item.mesh, gap - 1, item.ropa ? 1 : 0, item.skinned ? 1 : 0,
              item.char_family, item.dbg_src);
        }
      }
    }
    if (REXCVAR_GET(skate3_native_render_scene_dyn_gap_fill)) {
      for (auto& [mesh, t] : s_last_pub) {
        if (t.frame == 0 || t.frame == s_pub_frame) {
          continue;  // published this frame (or placeholder)
        }
        const uint64_t gap = s_pub_frame - t.frame;
        if (gap <= 2) {
          // t.frame stays at the last REAL publish, so the fill self-limits
          // to two frames and the telemetry above still sees the true gap
          // when the stream resumes.
          scene.items.push_back(t.item);
          static std::atomic<uint64_t> s_gap_filled{0};
          const uint64_t n = s_gap_filled.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 1023u) == 0) {
            REXLOG_INFO(
                "native-scene: dyn gap fill mesh={:08X} age={} ropa={} fam={} (n={})",
                mesh, gap, t.item.ropa ? 1 : 0, t.item.char_family, n + 1);
          }
        }
      }
    }
  }
  // Ghost guard, every publish path (live caster-bank captures, served
  // palettes, rescues, gap fills): a character the game does not draw is
  // not on screen in the game either. Drop a character / skinned item that
  // sits inside the GUEST camera's view while no perspective draw this
  // frame used its mesh's index buffer (draw_rs records each one). The
  // guest frustum, not the rendered one: instances the game culled because
  // they left ITS view (the trailing-pose retention) are unaffected.
  if (REXCVAR_GET(skate3_native_render_scene_ghost_guard)) {
    float gvp[16];
    for (int i = 0; i < 16; ++i) {
      gvp[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
    }
    const size_t before = scene.items.size();
    std::erase_if(scene.items, [&](const DrawItem& it) {
      if (it.cloth_quads || it.retained ||
          !(it.char_family != 0 || (it.skinned && !it.bones.empty()))) {
        return false;
      }
      if (draw_rs.find(it.ib_obj) != nullptr) {
        return false;  // the game drew this mesh this frame
      }
      float pos[3] = {it.world[12], it.world[13], it.world[14]};
      if (it.skinned && it.bones.size() >= 12) {
        pos[0] = it.bones[3];
        pos[1] = it.bones[7];
        pos[2] = it.bones[11];
      }
      if (BoxOutsideFrustum(pos, 1.5f, gvp, 1.0f)) {
        return false;
      }
      if (REXCVAR_GET(skate3_native_render_scene_ghost_audit)) {
        static uint32_t s_logged = 0;
        if (s_logged < 400 && (it.char_family != 0)) {
          ++s_logged;
          float lw_alpha = -1.0f;
          uint32_t lw_entity = 0;
          const bool lw = it.ctx != 0 &&
                          skate3::native_lw::LookupLwCtx(it.ctx, &lw_alpha, &lw_entity);
          const float dx = pos[0] - scene.cam_pos[0];
          const float dy = pos[1] - scene.cam_pos[1];
          const float dz = pos[2] - scene.cam_pos[2];
          REXLOG_INFO(
              "native-scene: ghost guard dropped ctx={:08X} mesh={:08X} ib={:08X} fam={} "
              "lw={} alpha={:.2f} caster_bank={} pending={} dist={:.1f}m",
              it.ctx, it.mesh, it.ib_obj, it.char_family, lw, lw_alpha, it.caster_bank,
              it.pending, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
      }
      return true;
    });
    g_ghost_suppressed.fetch_add(before - scene.items.size(), std::memory_order_relaxed);
  }
  if (scene.items.empty()) {
    return;
  }
  BuildSectMark(5);
  // Selected-object outline: flag items matching this frame's post-sky
  // re-draw captures. >= 2 identical draws = the stencil-marking pair; a
  // single occurrence is a legitimately late-drawn object, not a selection.
  // BOTH signals are required: the guest's own postfx_edgedetectstencil draw
  // must have run this frame too (park editor with an active selection);
  // gameplay draws some small props twice after the sky as well, which used
  // to outline distant objects during normal play.
  {
    uint32_t outline_items = 0;
    for (const SelectedDrawKey& k : frame_selected) {
      if (!outline_edge_seen || k.count < 2) {
        continue;
      }
      for (DrawItem& item : scene.items) {
        if (item.ib_obj == k.ib && item.vb_obj == k.vb &&
            std::fabs(item.world[12] - k.t[0]) < 0.05f &&
            std::fabs(item.world[13] - k.t[1]) < 0.05f &&
            std::fabs(item.world[14] - k.t[2]) < 0.05f && !item.selected) {
          item.selected = true;
          ++outline_items;
        }
      }
    }
    std::memcpy(scene.outline_color, g_outline_color, sizeof(scene.outline_color));
    static uint32_t s_outline_items = 0;
    if (outline_items != s_outline_items) {
      REXLOG_INFO("native-scene: selection outline {} item(s) (captures={})",
                  outline_items, frame_selected.size());
      s_outline_items = outline_items;
    }
  }
  for (int i = 0; i < 16; ++i) {
    scene.view_proj[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
  }
  // The raw projection (+0x60) rides along for the depth-based post passes
  // (SSAO linearize/unproject). Camera smoothing below replaces only the
  // view; the projection is whatever the game rendered this frame with.
  for (int i = 0; i < 16; ++i) {
    scene.proj[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
  }
  // RAW guest view*proj, the pose the game culled its submissions with,
  // kept for the off-screen retention frustum tests below (smoothing
  // replaces scene.view_proj with the re-timed pose).
  float guest_vp[16];
  std::memcpy(guest_vp, scene.view_proj, sizeof(guest_vp));
  // Camera cadence telemetry (see g_cam_changes): does the guest publish a
  // NEW camera every rendered frame, or step it on a slower sim tick?
  {
    static float s_last_vp[16] = {};
    static uint32_t s_streak = 0;
    if (std::memcmp(s_last_vp, scene.view_proj, sizeof(s_last_vp)) == 0) {
      ++s_streak;
      g_cam_repeats.fetch_add(1, std::memory_order_relaxed);
      uint64_t prev = g_cam_max_streak.load(std::memory_order_relaxed);
      while (s_streak > prev && !g_cam_max_streak.compare_exchange_weak(
                                    prev, s_streak, std::memory_order_relaxed)) {
      }
    } else {
      s_streak = 0;
      g_cam_changes.fetch_add(1, std::memory_order_relaxed);
      std::memcpy(s_last_vp, scene.view_proj, sizeof(s_last_vp));
    }
  }
  // Camera position from the view matrix (+0x20, row-vector convention):
  // cam = -t * R^T.
  float cam_view[16];
  for (int i = 0; i < 16; ++i) {
    cam_view[i] = LoadGuestF32(base, viewcam + 0x20 + i * 4);
  }
  for (int j = 0; j < 3; ++j) {
    scene.cam_pos[j] =
        -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
          cam_view[14] * cam_view[j * 4 + 2]);
  }
  // The game's projection uses a negative x scale which already yields
  // correct D3D NDC orientation; use the view*proj matrix as captured.
  // (Negating column 0 here mirrors the image left-right.)

  // Vehicle retention (see g_dyn_retained): re-publish the last live
  // capture of a view-culled vehicle for a short window. Runs BEFORE the
  // smoothing block on purpose; the re-published copy re-claims its pose
  // ring in InterpolateDynamicItems (position re-pairing) and renders the
  // ring's coherent history instead of a frozen step.
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      REXCVAR_GET(skate3_native_render_scene_dynamic_items) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0 &&
      !REXCVAR_GET(skate3_native_render_scene_freecam)) {
    // Covers the camera-smoothing lag only; vehicle pose data just ages.
    constexpr uint64_t kDynRetainFrames = 10;
    const uint64_t rnow = g_guest_frame;
    // World center from the palette: average the plausible bone
    // translations (junk rows past the real skeleton carry garbage; gate
    // on finiteness and distance to bone 0).
    const auto bone_center = [](const DrawItem& it, float out[3]) -> bool {
      const size_t nb = it.bones.size() / 12;
      if (nb == 0) {
        return false;
      }
      const float b0[3] = {it.bones[3], it.bones[7], it.bones[11]};
      double sum[3] = {0.0, 0.0, 0.0};
      int n = 0;
      for (size_t b = 0; b < nb; ++b) {
        const float t[3] = {it.bones[b * 12 + 3], it.bones[b * 12 + 7],
                            it.bones[b * 12 + 11]};
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) ||
            !std::isfinite(t[2])) {
          continue;
        }
        const float dx = t[0] - b0[0], dy = t[1] - b0[1], dz = t[2] - b0[2];
        if (dx * dx + dy * dy + dz * dz > 100.0f) {
          continue;  // > 10 m from bone 0: junk row
        }
        sum[0] += t[0];
        sum[1] += t[1];
        sum[2] += t[2];
        ++n;
      }
      if (n == 0) {
        return false;
      }
      out[0] = float(sum[0] / n);
      out[1] = float(sum[1] / n);
      out[2] = float(sum[2] / n);
      return true;
    };
    struct LivePos {
      uint32_t mesh;
      float p[3];
    };
    std::vector<LivePos> live;
    live.reserve(16);
    for (const DrawItem& it : scene.items) {
      if ((it.char_family != 6 && it.char_family != 7) || !it.skinned ||
          it.bones.empty() || it.pending || it.retained) {
        continue;
      }
      float p[3];
      if (!bone_center(it, p)) {
        continue;
      }
      live.push_back({it.mesh, {p[0], p[1], p[2]}});
      if (it.caster_bank) {
        continue;  // stale ortho pose: never store as the rescue state
      }
      DynRetained* slot = nullptr;
      for (DynRetained& r : g_dyn_retained) {
        const float dx = r.pos[0] - p[0], dy = r.pos[1] - p[1],
                    dz = r.pos[2] - p[2];
        if (r.item.mesh == it.mesh && dx * dx + dy * dy + dz * dz < 4.0f) {
          slot = &r;
          break;
        }
      }
      if (slot == nullptr) {
        if (g_dyn_retained.size() >= 64) {
          continue;
        }
        g_dyn_retained.emplace_back();
        slot = &g_dyn_retained.back();
      }
      slot->item = it;
      slot->item.retained = true;
      slot->item.selected = false;
      std::memcpy(slot->pos, p, sizeof(slot->pos));
      const float ex = it.bbox_max[0] - it.bbox_min[0];
      const float ey = it.bbox_max[1] - it.bbox_min[1];
      const float ez = it.bbox_max[2] - it.bbox_min[2];
      slot->half = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
      slot->last_seen = rnow;
    }
    for (size_t i = 0; i < g_dyn_retained.size();) {
      DynRetained& r = g_dyn_retained[i];
      if (r.last_seen == rnow) {
        ++i;
        continue;
      }
      bool keep = rnow - r.last_seen <= kDynRetainFrames;
      bool publish = keep;
      if (keep) {
        for (const LivePos& lp : live) {
          const float dx = lp.p[0] - r.pos[0], dy = lp.p[1] - r.pos[1],
                      dz = lp.p[2] - r.pos[2];
          if (lp.mesh == r.item.mesh && dx * dx + dy * dy + dz * dz < 36.0f) {
            // A live copy nearby (caster-only capture frame, or the 2 m
            // matcher missed a fast mover): keep the entry warm for the
            // frame the captures stop, but never double the vehicle.
            r.last_seen = rnow;
            publish = false;
            break;
          }
        }
      }
      if (publish) {
        // Unsubmitted + visible to the guest camera = really gone
        // (despawn); only view-culled vehicles re-publish.
        if (!BoxOutsideFrustum(r.pos, r.half, guest_vp, 0.97f)) {
          keep = false;
        }
      }
      if (!keep) {
        g_dyn_retained[i] = std::move(g_dyn_retained.back());
        g_dyn_retained.pop_back();
        continue;
      }
      if (publish) {
        scene.items.push_back(r.item);
        static std::atomic<uint64_t> s_dyn_retained_pub{0};
        const uint64_t n =
            s_dyn_retained_pub.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 1023u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle retention re-publish mesh={:08X} fam={} "
              "age_frames={} (n={})",
              r.item.mesh, r.item.char_family, rnow - r.last_seen, n);
        }
      }
      ++i;
    }
  }

  // (Moved BEFORE the smoothing block: the interp ring's pose ingestion
  // pairs each pose with g_ropa_last_seq; the shape snapshot of the SAME
  // frame must be enqueued first or every pose pairs with the previous
  // frame's shape, a constant one-period drape lag. The snapshot also now
  // reads RAW captured items, matching the raw VB it snapshots.)
  BuildSectMark(6);
  // Dynamic cloth decode jobs (see DynDecodeJob): snapshot CHANGED (or
  // first-seen) skinned/ropa payloads for the workers; the render thread
  // never decodes them inline; a first-sight NPC/garment appears 1-2 frames
  // late instead of hitching the frame it streams in on. GuestTryCopy is
  // safe here: the game just drew from these payloads this frame.
  {
    // mesh -> last enqueued dedup key (guest render thread only). Static
    // skinned meshes enqueue once; ropa re-enqueues whenever its cloth
    // payload changed: EVERY rendered frame while the sim runs. That
    // cadence is load-bearing: each pose the interp ring ingests pairs with
    // the SAME-frame generation via g_ropa_last_seq (a constant zero
    // enqueue offset). Do not decimate these enqueues on a timer of their
    // own; an enqueue clock that slips independently of the pose-ring
    // ingest clock makes the pairing offset jitter by a frame, and the
    // drape blends against shapes from the wrong instant (speed-scaled
    // garment flicker). High-rate capacity lives in the CONSUMERS instead
    // (the resident generation ring and kShapeGens).
    static std::unordered_map<uint32_t, uint64_t> s_dyn_fp_sent;
    static uint64_t s_dyn_seq = 0;
    if (s_dyn_fp_sent.size() > 4096) {
      s_dyn_fp_sent.clear();
    }
    std::vector<DynDecodeJob> jobs;
    for (const DrawItem& item : scene.items) {
      if (!(item.skinned || item.ropa) || item.cloth_quads || item.ib_addr == 0) {
        continue;
      }
      if (item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline)) {
        continue;  // ropa decodes inline on the render thread (zero-lag)
      }
      if (item.ropa && item.dbg_src == 4) {
        // Rescued ropa re-publishes LAST frame's resolved state (mode +
        // palette + world); decoding THIS frame's payload under it mixes
        // frames, at a sim flip that is the ribbon. Keep the previous
        // decode on the GPU: a fully coherent N-1 garment (the draw path
        // tolerates the fingerprint mismatch for dynamic payloads).
        continue;
      }
      // The decode is MODE-dependent for ropa (rigid decodes zero the blend
      // weight/index attributes, see DecodeMesh), so a mode flip must
      // re-enqueue even when the payload bytes did not change: fold the
      // resolved mode into the dedup key.
      const uint64_t fp_key =
          item.fingerprint ^
          ((item.ropa && item.skinned && !item.bones.empty()) ? 1u : 0u);
      const auto prev = s_dyn_fp_sent.find(item.mesh);
      const bool first_sight = prev == s_dyn_fp_sent.end();
      if (!first_sight && prev->second == fp_key) {
        continue;
      }
      DynDecodeJob job;
      job.item = item;
      job.item.bones.clear();
      job.seq = ++s_dyn_seq;
      job.vb.resize(item.vb_bytes);
      if (!GuestTryCopy(job.vb.data(), base + item.vb_addr, item.vb_bytes)) {
        continue;
      }
      job.ib.resize(size_t(item.ib_count) * 2);
      if (!GuestTryCopy(job.ib.data(), base + item.ib_addr, job.ib.size())) {
        continue;
      }
      s_dyn_fp_sent[item.mesh] = fp_key;
      if (item.ropa) {
        // The pose <-> shape pairing key (see DynPose::shape_seq). Recorded
        // at CREATION (the delay queue below postpones submission, not
        // identity).
        g_ropa_last_seq[item.mesh] = job.seq;
        if (g_ropa_last_seq.size() > 256) {
          g_ropa_last_seq.clear();
        }
      }
      jobs.push_back(std::move(job));
    }
    // ROPA phase alignment (skate3_native_render_scene_ropa_delay): the
    // body renders on the motion-smoothing play clock, ~2 guest periods
    // behind now; a cloth snapshot submitted immediately decodes into a
    // shape ~2 frames AHEAD of the rendered body (the drape hangs where
    // the body WILL be; jelly / clip-through on direction changes; the
    // console pairs body N with shape N). Ropa snapshots pass through a
    // small per-mesh delay queue so the committed shape lands on the same
    // clock as the interpolated body.
    {
      const int32_t delay = REXCVAR_GET(skate3_native_render_scene_ropa_delay);
      static std::unordered_map<uint32_t, std::deque<DynDecodeJob>> s_ropa_delay;
      if (delay > 0) {
        std::vector<DynDecodeJob> ready;
        ready.reserve(jobs.size());
        for (DynDecodeJob& j : jobs) {
          if (!j.item.ropa) {
            ready.push_back(std::move(j));
            continue;
          }
          auto& q = s_ropa_delay[j.item.mesh];
          q.push_back(std::move(j));
          while (q.size() > size_t(delay)) {
            ready.push_back(std::move(q.front()));
            q.pop_front();
          }
        }
        jobs.swap(ready);
        if (s_ropa_delay.size() > 256) {
          s_ropa_delay.clear();  // outfit-change growth backstop
        }
      } else if (!s_ropa_delay.empty()) {
        s_ropa_delay.clear();
      }
    }
    if (!jobs.empty()) {
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      for (DynDecodeJob& j : jobs) {
        if (g_dyn_jobs.size() >= 32) {
          break;  // workers behind: the cloth skips a sim frame
        }
        g_dyn_jobs.push_back(std::move(j));
      }
      g_prewarm_cv.notify_all();
    }
  }

  BuildSectMark(7);
  // LivingWorld entity store stamp: map
  // each character-family item's MeshContext to its owning LW entity and
  // stamp the entity's authoritative opacity + per-instance identity. Runs
  // after every dyn publish path (captures, merges, rescues, vehicle
  // retention) and before the smoothing block so the pose rings can key by
  // identity. Items with no fresh store entry (player skater - a different
  // fade system, CAC entities, despawned/retained leftovers) keep the
  // captured-row behavior untouched.
  if (REXCVAR_GET(skate3_native_render_scene_lw_fade) ||
      REXCVAR_GET(skate3_native_render_scene_lw_identity)) {
    // Fade serving also honors the master entity-fade switch: with it off
    // the routing ignores fades entirely, so the alpha row must keep its
    // captured value (the shader still reads it on some paths).
    const bool serve_fade =
        REXCVAR_GET(skate3_native_render_scene_lw_fade) &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const bool serve_id = REXCVAR_GET(skate3_native_render_scene_lw_identity);
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      float alpha = 1.0f;
      uint32_t entity = 0;
      if (!skate3::native_lw::LookupLwCtx(item.ctx, &alpha, &entity)) {
        continue;
      }
      g_lw_stamped.fetch_add(1, std::memory_order_relaxed);
      if (serve_id) {
        item.lw_entity = entity;
      }
      // Per-ctx lighting/paint rows (see g_char_rows_cache_ctx): refresh
      // from validated captures, serve on capture-failed frames, entity-
      // checked so a recycled instance never inherits foreign paint. This
      // keeps edge-of-view vehicles (caster-only capture stretches, every
      // read rejected) on their own shading instead of legacy flat.
      if (item.char_rows[14 * 4 + 1] > 0.0f) {
        if (g_char_rows_cache_ctx.size() > 4096) {
          g_char_rows_cache_ctx.clear();
        }
        CharRowsCtx& cr = g_char_rows_cache_ctx[item.ctx];
        std::memcpy(cr.rows.data(), item.char_rows, sizeof(item.char_rows));
        cr.entity = entity;
      } else {
        const auto rit = g_char_rows_cache_ctx.find(item.ctx);
        if (rit != g_char_rows_cache_ctx.end() &&
            rit->second.entity == entity) {
          std::memcpy(item.char_rows, rit->second.rows.data(),
                      sizeof(item.char_rows));
          g_lw_rows_served.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Authoritative palette for every vehicle pose that did NOT come
      // from a fresh perspective capture: caster-only frames (ortho banks
      // = GUESSED palette base + ~40 ms-stale animation, the edge-of-view
      // mangle class), retention re-publishes (last capture aging up to 10
      // frames; at driving speed a stale republish renders a GHOST
      // meters BEHIND the live position right after the vehicle exits the
      // view; a garbage last capture makes the ghost sideways), rescues
      // and gap fills. The entity's OWN packed palette (m_matrices,
      // written by the pack writer this sim tick) is its true current
      // pose: a genuinely-exited vehicle lands off-screen (no ghost), a
      // pan-trailing retention case lands exactly right.
      if (item.skinned && !item.ropa &&
          (item.char_family == 6 || item.char_family == 7) &&
          (item.caster_bank || item.retained || item.dbg_src == 3 ||
           item.dbg_src == 6 || item.dbg_src == 9) &&
          !item.bones.empty() &&
          REXCVAR_GET(skate3_native_render_scene_lw_palette)) {
        float rows[96 * 12];
        const uint32_t n =
            skate3::native_lw::LookupLwPalette(item.ctx, rows, 96 * 12);
        if (n != 0 && size_t(n) * 12 <= item.bones.size()) {
          std::memcpy(item.bones.data(), rows, size_t(n) * 12 * sizeof(float));
          item.caster_bank = false;
          item.dbg_src = 10;  // lw palette substitution
          g_lw_pal_sub.fetch_add(1, std::memory_order_relaxed);
          static std::atomic<uint32_t> s_sub_logged{0};
          const uint32_t ln =
              s_sub_logged.fetch_add(1, std::memory_order_relaxed);
          if (ln < 16 || (ln & 511u) == 0) {
            REXLOG_DEBUG(
                "native-scene: LW palette substituted ctx={:08X} mesh={:08X} "
                "fam={} rows={} (n={})",
                item.ctx, item.mesh, item.char_family, n, ln);
          }
        }
      }
      if (serve_fade) {
        item.lw_alpha = alpha;
        // The exact shading path reads the alpha ROW (cbuffer CH row 14.x)
        // - overwrite it with the entity value for the families where that
        // row IS the raw entity fade on console (c13.x / c21.x / c22.x /
        // c20.x). Hair (strand-scale composed) and vehicle glass
        // (tint-composed) keep their captured rows; CharFadeAlpha bounds
        // them by the entity alpha instead.
        if (item.char_rows[14 * 4 + 1] > 0.0f &&
            (item.char_family == 1 || item.char_family == 2 ||
             item.char_family == 3 || item.char_family == 6)) {
          item.char_rows[14 * 4 + 0] = std::clamp(alpha, 0.0f, 1.0f);
        }
      }
    }
  }

  // Presentation-entity identity store observer
  // (native/skate3_native_entity.h): per-item coverage of the
  // ctx -> entity map built from the game's BindConstants walks, and the
  // skater-family entity+496 opacity vs the alpha the routing decided on.
  // Read-only: the ident[] stats line is the evidence for flipping any
  // serve over to the direct fields.
  {
    const bool serve_ent_fade =
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      // Skater-family fade from the entity's own opacity (+496): the
      // value the game binds as the shader's alpha parameter. Same serving
      // contract as the LW store (lw_alpha preferred by CharFadeAlpha;
      // families whose shader composes the fade keep their captured rows
      // bounded by it). LW-mapped items were already stamped above.
      float ent_alpha = 1.0f;
      if (serve_ent_fade && item.lw_alpha < 0.0f &&
          skate3::native_entity::ReadSkaterFade(base, item.ctx, &ent_alpha)) {
        item.lw_alpha = std::clamp(ent_alpha, 0.0f, 1.0f);
        if (item.char_rows[14 * 4 + 1] > 0.0f &&
            (item.char_family == 1 || item.char_family == 2 ||
             item.char_family == 3 || item.char_family == 6)) {
          item.char_rows[14 * 4 + 0] = item.lw_alpha;
        }
      }
      skate3::native_entity::ObserveCharItem(base, item.ctx,
                                             item.char_family,
                                             item.lw_entity != 0,
                                             CharFadeAlpha(item));
    }
    skate3::native_entity::EmitStats();
  }

  BuildSectMark(8);
  // Camera re-timing (see SmoothCamera): replace the guest's sim-stepped
  // pose with a host-clock-interpolated one so panning is smooth at render
  // rate. All consumers (items, sky follow, sorting, outline) use the
  // smoothed pose coherently.
  if (REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
    // Publish the ViewCamera for the ~1 kHz sampler thread and make sure it
    // runs (see CamSamplerLoop).
    g_sampler_viewcam.store(viewcam, std::memory_order_relaxed);
    EnsureCamSampler();
    float proj[16];
    for (int i = 0; i < 16; ++i) {
      proj[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
    }
    const double now_s =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    // Auto-armed bone-signal recordings (diagnosis, see the cvar): first
    // window ~30 s after the scene comes up, re-armed every 90 s, 3 max.
    {
      const double auto_s = REXCVAR_GET(skate3_native_render_scene_bonesig_auto);
      if (auto_s > 0.0) {
        static int s_auto_count = 0;
        static double s_auto_next = 0.0;
        if (s_auto_next == 0.0) {
          s_auto_next = now_s + 30.0;
        } else if (s_auto_count < 3 && now_s >= s_auto_next) {
          ++s_auto_count;
          s_auto_next = now_s + 90.0;
          REXLOG_INFO("native-scene: auto bone-signal recording {} ({} s)",
                      s_auto_count, auto_s);
          RecordBoneSignal(std::min(auto_s, 30.0));
        }
      }
    }
    // Walking-vehicle detector (diagnosis): a livingworld_vehicles item
    // whose bone-0 translation sits on top of a character item's bone-0 is
    // the "player becomes the vehicle" bug. Log the coincidence WITH the
    // first bone rows of both palettes: identical rows = the same bank was
    // captured for both (a capture-attribution bug); distinct rows = the
    // vehicle's own palette tracks the character (a different mechanism).
    {
      static double s_last_attach_log = 0.0;
      if (now_s - s_last_attach_log > 2.0) {
        bool logged = false;
        for (const DrawItem& v : scene.items) {
          if ((v.char_family != 6 && v.char_family != 7) || !v.skinned ||
              v.bones.size() < 12) {
            continue;
          }
          for (const DrawItem& c : scene.items) {
            if (&c == &v || !c.skinned || c.bones.size() < 12 ||
                c.char_family == 0 || c.char_family >= 6) {
              continue;
            }
            const float dx = v.bones[3] - c.bones[3];
            const float dy = v.bones[7] - c.bones[7];
            const float dz = v.bones[11] - c.bones[11];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 4.0f) {
              const bool same_rows =
                  std::memcmp(v.bones.data(), c.bones.data(),
                              12 * sizeof(float)) == 0;
              REXLOG_INFO(
                  "native-scene ATTACH: vehicle mesh={:08X} fam={} src={} "
                  "pend={} d={:.2f} char mesh={:08X} fam={} src={} "
                  "same_bone0={} v_r0=({:.3f},{:.3f},{:.3f},{:.2f}) "
                  "c_r0=({:.3f},{:.3f},{:.3f},{:.2f})",
                  v.mesh, v.char_family, v.dbg_src, v.pending, std::sqrt(d2),
                  c.mesh, c.char_family, c.dbg_src, same_rows, v.bones[0],
                  v.bones[1], v.bones[2], v.bones[3], c.bones[0], c.bones[1],
                  c.bones[2], c.bones[3]);
              s_last_attach_log = now_s;
              logged = true;
              break;
            }
          }
          if (logged) {
            break;
          }
        }
      }
    }
    float smooth_vp[16], smooth_cam[3];
    if (SmoothCamera(cam_view, proj, scene.view_proj, scene.cam_pos, now_s, smooth_vp,
                     smooth_cam)) {
      std::memcpy(scene.view_proj, smooth_vp, sizeof(smooth_vp));
      std::memcpy(scene.cam_pos, smooth_cam, sizeof(smooth_cam));
      // Keep the skater/NPCs/props in phase with the smoothed camera:
      // interpolate their palettes/worlds at the same playback time.
      InterpolateDynamicItems(base, scene, now_s);
      // The SimpleDraw lines (the debug skeleton) on the same playback
      // clock as the skater they draw.
      RetimeSimpleDraws(scene, now_s);
    }
  }
  // Snapshot the local player, independent of the board and chase camera.
  // Retimed on the same clock as dynamic meshes, so the body does not trail
  // the first-person viewpoint. Reset on despawn/entity change/teleport.
  {
    struct Sample { double t; float p[3]; };
    static std::vector<Sample> history;
    static uint32_t owner = 0;
    skate3::deck_shadow::PlayerPose pose;
    if (skate3::deck_shadow::GetPlayerPose(base, pose)) {
      const double now = std::chrono::duration<double>(build_t0.time_since_epoch()).count();
      float d2 = 0.0f;
      if (!history.empty()) for (int k=0;k<3;++k) {
        const float d = pose.head[k] - history.back().p[k]; d2 += d*d;
      }
      if (owner != pose.entity || d2 > 25.0f ||
          (!history.empty() && now-history.back().t > 0.25)) history.clear();
      owner = pose.entity;
      const double stamp = g_latest_cam_tick > 0.0 && now-g_latest_cam_tick < 0.02
                               ? g_latest_cam_tick : now;
      if (history.empty() || (d2 > 1e-12f && stamp > history.back().t + 0.0001)) {
        Sample sample{stamp, {pose.head[0], pose.head[1], pose.head[2]}};
        history.push_back(sample);
        if (history.size() > 24) history.erase(history.begin());
      }
      std::memcpy(scene.player_head, pose.head, sizeof(pose.head));
      if (REXCVAR_GET(skate3_native_render_scene_smooth_camera) && history.size() > 1 &&
          g_smooth_play >= history.front().t && g_smooth_play <= now) {
        const double width = std::clamp(
            REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0)*1e-3;
        std::fill(std::begin(scene.player_head), std::end(scene.player_head), 0.0f);
        for (int tap=0; tap<8; ++tap) {
          const double t = std::clamp(g_smooth_play + ((tap+0.5)/8.0-0.5)*width,
                                      history.front().t, history.back().t);
          size_t hi=1;
          while (hi+1<history.size() && history[hi].t<t) ++hi;
          const auto& a=history[hi-1]; const auto& b=history[hi];
          const float f=float(std::clamp((t-a.t)/std::max(b.t-a.t,0.0001),0.0,1.0));
          for (int k=0;k<3;++k) scene.player_head[k] += (a.p[k]+(b.p[k]-a.p[k])*f)/8.0f;
        }
      }
      scene.player_pose_valid = true;
      scene.player_entity = pose.entity;
      std::memcpy(scene.player_forward, pose.forward, sizeof(pose.forward));
      std::memcpy(scene.player_chest, pose.chest, sizeof(pose.chest));
      std::memcpy(scene.player_face, pose.face, sizeof(pose.face));
    } else {
      history.clear(); owner=0;
    }
  }

  // Deck (board) shadow placement: InterpolateDynamicItems fills it on the
  // re-timed path; unsmoothed frames (smooth_camera off, or no camera
  // history yet) take the game's own placement for this frame, which is
  // already consistent with the raw camera and skater.
  if (!scene.deck_valid) {
    skate3::deck_shadow::DeckPlacement dp = {};
    if (skate3::deck_shadow::GetPlacement(0, dp)) {
      std::memcpy(scene.deck_world, dp.world, sizeof(dp.world));
      for (int r = 0; r < 3; ++r) {
        float* a = scene.deck_world + r * 4;
        const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (len > 1e-6f) {
          a[0] /= len;
          a[1] /= len;
          a[2] /= len;
        }
      }
      scene.deck_valid = true;
    }
  }

  // Hor+ ultrawide scale for this frame's camera (1.0 when the output is not
  // wide). The matrices are widened after the camera overrides below; the
  // retention frustum guards divide by it here so their tests already match
  // the frame's real (wider) view.
  const float wide_hor_scale = WideOutputHorScale(scene.proj);
  const float wide_vert_scale = WideOutputVertScale(scene.proj);
  const float wide_min_scale = std::min(wide_hor_scale, wide_vert_scale);

  BuildSectMark(9);
  // Off-screen retention (see g_retained_items): re-append statics the game
  // view-culled this frame while the trailing rendered pose can still see
  // them. Runs AFTER the smoothing block so retained copies never enter the
  // dynamic pose histories. Stands down while the synthetic-pan probe
  // maintains its own full-surround union, and while the freecam has taken
  // over the guest camera (the game then culls around the rendered pose
  // directly, so re-appends would only risk double draws).
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0 &&
      !REXCVAR_GET(skate3_native_render_scene_freecam)) {
    const auto retain_t0 = PerfClock::now();
    uint64_t retain_appended = 0;
    if (g_retained_clear.exchange(false, std::memory_order_relaxed)) {
      ClearRetainedItems();
      g_dyn_retained.clear();
    }
    const uint64_t now = g_guest_frame;
    const size_t published = scene.items.size();
    for (size_t i = 0; i < published; ++i) {
      const DrawItem& it = scene.items[i];
      if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
          !it.bones.empty()) {
        continue;
      }
      const uint64_t key = SynPanItemKey(it);
      if (g_retained_items.size() >= 20000 && !g_retained_items.contains(key)) {
        continue;  // growth backstop
      }
      auto [r, inserted] = g_retained_items.try_emplace(key);
      if (inserted) {
        if (!g_retained_free.empty()) {
          r->slot = g_retained_free.back();
          g_retained_free.pop_back();
        } else {
          r->slot = uint32_t(g_retained_pool.size());
          g_retained_pool.emplace_back();
        }
      }
      r->last_seen = now;
      // Copy the item core on first sight and whenever its payload identity
      // or transform moved on; a retained copy with a stale fingerprint
      // skips at draw (see draw_item's retained gate) and would re-tear.
      if (inserted || r->fingerprint != it.fingerprint ||
          std::memcmp(r->world, it.world, sizeof(it.world)) != 0) {
        r->fingerprint = it.fingerprint;
        std::memcpy(r->world, it.world, sizeof(r->world));
        std::memcpy(r->bbox_min, it.bbox_min, sizeof(r->bbox_min));
        std::memcpy(r->bbox_max, it.bbox_max, sizeof(r->bbox_max));
        DrawItem& copy = g_retained_pool[r->slot];
        copy = it;
        copy.retained = true;
        copy.selected = false;
      }
    }
    // TTL bounds how long a never-resubmitted entry lives (streaming reuses
    // arena addresses; the render side additionally fingerprint-gates
    // retained draws). ~90 guest frames = 0.6-1.5 s across fps caps,
    // far beyond the smoothing lag it needs to cover.
    constexpr uint64_t kRetainTtlFrames = 90;
    static std::vector<uint64_t> s_drop;  // erased after the walk
    s_drop.clear();
    g_retained_items.for_each([&](uint64_t key, const RetainedItem& r) {
      if (r.last_seen == now) {
        return;  // submitted this frame
      }
      // Unsubmitted + visible to the guest camera = the game really removed
      // it. The 0.97 margin shrinks the tested frustum so bounds poking
      // just inside an edge still count as view-culled.
      if (now - r.last_seen > kRetainTtlFrames ||
          !BoundsOutsideFrustum(r.world, r.bbox_min, r.bbox_max, guest_vp,
                                0.97f / wide_min_scale)) {
        s_drop.push_back(key);
        return;
      }
      // Draw it only if the RENDERED pose can actually see it (widened
      // frustum: only clearly-outside skips); after a fast 180 the trail
      // behind the camera stays retained but costs nothing.
      if (!BoundsOutsideFrustum(r.world, r.bbox_min, r.bbox_max, scene.view_proj,
                                1.05f / wide_min_scale)) {
        scene.items.push_back(g_retained_pool[r.slot]);
        ++retain_appended;
      }
    });
    for (const uint64_t key : s_drop) {
      if (const RetainedItem* r = g_retained_items.find(key)) {
        g_retained_free.push_back(r->slot);
        g_retained_items.erase(key);
      }
    }
    g_pw_bi_retain.Add(PerfNsSince(retain_t0));
    g_retained_appended.fetch_add(retain_appended, std::memory_order_relaxed);
    g_retained_live.store(g_retained_items.size(), std::memory_order_relaxed);
  }

  // Camera-signal recorder: per-frame raw + smoothed heading while the
  // window is open; write + reset when it closes (CamSigFrameTick).
  if (g_camsig_deadline.load(std::memory_order_relaxed) > 0.0) {
    const double rec_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    float rot[16] = {};
    const float* smoothed = nullptr;
    if (g_smooth_active) {
      ViewRotFromQuat(g_smooth_pose.q, rot);
      smoothed = rot;
    }
    CamSigFrameTick(rec_now, cam_view, smoothed, g_smooth_play);
  }

  // Synthetic camera pan probe (see the synthetic_pan cvar comment for the
  // mode semantics). Runs AFTER the smoothing block: modes 1/2 override the
  // published pose outright (their point is to bypass the guest pose path);
  // mode 3 leaves the smoothed pose in place, the sampler thread is feeding
  // the smoother synthetic samples, and measures reconstruction error
  // against the known ideal.
  {
    const double syn_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    int syn_mode =
        std::clamp(int(REXCVAR_GET(skate3_native_render_scene_synthetic_pan)), 0, 3);
    if (syn_mode == 3 && !REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        REXLOG_WARN(
            "native-scene synthetic-pan: mode 3 needs smooth_camera ON; "
            "running mode 1 instead");
      }
      syn_mode = 1;
    }
    static int s_engaged_mode = 0;
    if (syn_mode != s_engaged_mode) {
      s_engaged_mode = syn_mode;
      if (syn_mode == 0) {
        g_synpan_active.store(0, std::memory_order_release);
        g_synpan_union.clear();
        REXLOG_INFO("native-scene synthetic-pan: off (guest camera restored)");
      } else {
        // (Re-)engage from THIS frame's raw guest pose: heading, position
        // and projection are frozen; only the synthetic yaw moves.
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        std::memcpy(g_synpan_view0, cam_view, sizeof(g_synpan_view0));
        for (int i = 0; i < 16; ++i) {
          g_synpan_proj0[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
        }
        for (int j = 0; j < 3; ++j) {
          g_synpan_c0[j] =
              -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
                cam_view[14] * cam_view[j * 4 + 2]);
        }
        g_synpan_t0 = syn_now;
        g_synpan_step_phase = 0.0;
        g_synpan_ema_dt = 0.0;
        g_synpan_frames = 0;
        g_synpan_last_build = 0.0;
        g_synpan_dt_sum = g_synpan_dt_sum2 = 0.0;
        g_synpan_dt_min = g_synpan_dt_max = 0.0;
        g_synpan_err_sum2 = g_synpan_err_max = 0.0;
        g_synpan_err_n = 0;
        g_synpan_union.clear();
        g_synpan_active.store(syn_mode, std::memory_order_release);
        static const char* kModeNames[] = {"off", "time-based", "fixed-step",
                                           "through-smoother"};
        REXLOG_INFO(
            "native-scene synthetic-pan: ENGAGED mode={} ({}) rate={:.1f} deg/s "
            "amp=+-{:.1f} deg",
            syn_mode, kModeNames[syn_mode],
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate),
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
      }
    }
    if (syn_mode != 0) {
      const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
      const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
      const double prev_build = g_synpan_last_build;
      g_synpan_last_build = syn_now;
      const double dt = prev_build > 0.0 ? std::clamp(syn_now - prev_build, 0.0, 0.05) : 0.0;
      if (dt > 0.0) {
        g_synpan_dt_sum += dt;
        g_synpan_dt_sum2 += dt * dt;
        g_synpan_dt_min = g_synpan_dt_min == 0.0 ? dt : std::min(g_synpan_dt_min, dt);
        g_synpan_dt_max = std::max(g_synpan_dt_max, dt);
      }
      if (syn_mode == 1 || syn_mode == 2) {
        double phase;
        if (syn_mode == 1) {
          phase = (syn_now - g_synpan_t0) * rate;
        } else {
          // Fixed step: constant angle per published frame. The step is
          // rate * (slow EMA of dt) so deg/s stays roughly honest while the
          // per-frame advance is effectively constant over any short window.
          if (dt > 0.0) {
            g_synpan_ema_dt =
                g_synpan_ema_dt == 0.0 ? dt : g_synpan_ema_dt * 0.995 + dt * 0.005;
          }
          g_synpan_step_phase += rate * g_synpan_ema_dt;
          phase = g_synpan_step_phase;
        }
        float sview[16];
        SynPanView(SynPanAngleDeg(phase, amp), sview);
        CamPose pose;
        QuatFromView(sview, pose.q);
        std::memcpy(pose.c, g_synpan_c0, sizeof(pose.c));
        ComposeViewProj(pose, g_synpan_proj0, scene.view_proj, scene.cam_pos);
      } else if (g_smooth_active) {
        // Mode 3: the smoother just reconstructed a pose from the synthetic
        // samples at playback time g_smooth_play; compare against the ideal
        // pose at that exact time (both are functions of the same clock).
        float iview[16];
        SynPanView(SynPanAngleDeg((g_smooth_play - g_synpan_t0) * rate, amp), iview);
        float qi[4];
        QuatFromView(iview, qi);
        const float dq =
            std::fabs(qi[0] * g_smooth_pose.q[0] + qi[1] * g_smooth_pose.q[1] +
                      qi[2] * g_smooth_pose.q[2] + qi[3] * g_smooth_pose.q[3]);
        const double err_deg =
            2.0 * std::acos(std::min(dq, 1.0f)) * (180.0 / 3.14159265358979323846);
        g_synpan_err_sum2 += err_deg * err_deg;
        g_synpan_err_max = std::max(g_synpan_err_max, err_deg);
        ++g_synpan_err_n;
      }
      // World union: accumulate this frame's static items and append every
      // previously seen one the game didn't submit this frame (it culls to
      // ITS frustum; the probe camera looks elsewhere). Statics only:
      // skinned/cloth poses go stale immediately.
      {
        std::unordered_set<uint64_t> cur;
        cur.reserve(scene.items.size());
        const size_t published = scene.items.size();
        for (size_t i = 0; i < published; ++i) {
          const DrawItem& it = scene.items[i];
          if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
              !it.bones.empty()) {
            continue;
          }
          const uint64_t key = SynPanItemKey(it);
          cur.insert(key);
          if (g_synpan_union.size() < 20000) {
            auto [slot, inserted] = g_synpan_union.try_emplace(key, it);
            if (!inserted &&
                std::memcmp(slot->second.world, it.world, sizeof(it.world)) != 0) {
              slot->second = it;  // a movable prop moved: refresh
            }
          }
        }
        for (const auto& [key, it] : g_synpan_union) {
          if (cur.find(key) == cur.end()) {
            scene.items.push_back(it);
          }
        }
      }
      if (++g_synpan_frames % 600 == 0) {
        const double n = std::max<double>(1.0, double(g_synpan_frames - 1));
        const double avg = g_synpan_dt_sum / n;
        const double sd =
            std::sqrt(std::max(0.0, g_synpan_dt_sum2 / n - avg * avg));
        if (syn_mode == 3) {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode=3 frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} smoother_err_deg[rms/max]={:.4f}/{:.4f} (n={}) "
              "union={}",
              g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3, g_synpan_dt_max * 1e3,
              sd * 1e3,
              std::sqrt(g_synpan_err_sum2 / std::max<uint64_t>(1, g_synpan_err_n)),
              g_synpan_err_max, g_synpan_err_n, g_synpan_union.size());
        } else {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode={} frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} union={}",
              syn_mode, g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3,
              g_synpan_dt_max * 1e3, sd * 1e3, g_synpan_union.size());
        }
      }
    }
  }

  BuildSectMark(10);
  // Drone / free-fly camera (skate3_native_render_scene_freecam, End key):
  // runs after the smoothing and synthetic-pan blocks so the flown pose
  // wins while engaged. No draw-item union here; the SetViewMatrix
  // override hands the flown pose to the game, whose own culling then
  // submits exactly what the drone sees (statics AND animated entities).
  UpdateFreecam(scene, cam_view,
                std::chrono::duration<double>(build_t0.time_since_epoch()).count());

  // Hor+ ultrawide: widen the published camera to the wide output aspect.
  // Applied after every camera override (smoothing, synthetic pan, freecam)
  // so it survives their view_proj/proj rewrites.
  // The game camera's own aspect, recorded before the widening rewrites proj
  // (the F5 hand-off keys on it; the widened proj always reads as the window).
  {
    const float m00 = std::fabs(scene.proj[0]);
    const float m11 = std::fabs(scene.proj[5]);
    scene.guest_cam_aspect = m00 > 1e-6f ? m11 / m00 : 0.0f;
    scene.guest_cam_proj11 = m11;
  }
  WidenPublishedCamera(scene, wide_hor_scale, wide_vert_scale);

  // Publish the frame's captured fog rows and re-arm the OnDrawDone capture
  // (keyed to this frame's camera) for the next frame.
  if (g_fog_have) {
    std::memcpy(scene.fog_ramp, g_fog_rows, 4 * sizeof(float));
    std::memcpy(scene.fog_color, g_fog_rows + 4, 4 * sizeof(float));
  }
  if (g_water_have) {
    std::memcpy(scene.water_rows, g_water_rows, sizeof(scene.water_rows));
    scene.water_valid = true;
  }
  if (g_waterdef_have) {
    std::memcpy(scene.waterdef_rows, g_waterdef_rows,
                sizeof(scene.waterdef_rows));
    scene.waterdef_tf7 = g_waterdef_tf7;
    scene.water_tf4 = g_water_tf4;
    scene.waterdef_tf6 = g_waterdef_tf6;
    for (uint32_t k = 0; k < kWaterdefSets; ++k) {
      std::memcpy(scene.waterdef_set_rows[k], g_waterdef_set_rows[k],
                  sizeof(scene.waterdef_set_rows[k]));
      scene.waterdef_set_key[k] = g_waterdef_set_key[k];
      scene.waterdef_set_have[k] = g_waterdef_set_have[k];
    }
    scene.waterdef_valid = true;
  }
  if (g_ocean_have) {
    std::memcpy(scene.ocean_rows, g_ocean_rows, sizeof(scene.ocean_rows));
    scene.ocean_tf7 = g_ocean_tf7;
    scene.ocean_valid = true;
  }
  if (g_ad_have) {
    std::memcpy(scene.ad_rows, g_ad_rows, sizeof(scene.ad_rows));
    scene.ad_valid = true;
  }
  if (g_ts_have) {
    std::memcpy(scene.ts_rows, g_ts_rows, sizeof(scene.ts_rows));
    scene.ts_valid = true;
  }
  if (g_oceanrefl_have) {
    std::memcpy(scene.oceanrefl_rows, g_oceanrefl_rows,
                sizeof(scene.oceanrefl_rows));
    scene.oceanrefl_valid = true;
  }
  if (g_scroll_have) {
    std::memcpy(scene.scroll_rows, g_scroll_rows, sizeof(scene.scroll_rows));
    scene.scroll_valid = true;
  }
  if (g_shadow_have) {
    std::memcpy(scene.shadow_rows, g_shadow_rows, sizeof(g_shadow_rows));
    scene.shadow_valid = true;
    // Same freshness pattern as the char CSM biases below: outdoor scenes
    // re-capture every frame, so anything past a couple of seconds means the
    // current venue's environment shader never produces a sane bank.
    scene.shadow_fresh = g_guest_frame - g_shadow_rows_frame <= 120;
  }
  // Front-end loose worlds (LooseWorldPresentation, world+72: the CAS room
  // behind the editor and behind the character viewer of the smartphone
  // Freeskate / Skate With character select, the trick guide) are a separate
  // scene with their own light: their room's environment bank carries its
  // own cascade / sun / exposure rows, captured above every frame. Two
  // things of the GAMEPLAY world must not reach them:
  //  - the native static sun map (world static casters around the camera,
  //    a native term the game has no counterpart of in these scenes): off;
  //  - held gameplay rows, should a loose-world frame ever lack its own
  //    environment receiver: the skaters then picked up the world shadows
  //    of wherever the player last stood; serve no world shadow state.
  {
    uint32_t world = 0, lw_type = 0;
    if (GuestTryLoadU32(base, 0x83083C38, &world) && world != 0 &&
        kLooseWorldTypeOffset != 0 &&
                 GuestTryLoadU32(base, world + kLooseWorldTypeOffset, &lw_type) && lw_type != 0) {
      scene.shadow_fresh = false;
      if (scene.shadow_valid && g_shadow_rows_frame + 1 != g_guest_frame) {
        scene.shadow_valid = false;
        static uint64_t s_logged_frame = 0;
        if (g_guest_frame - s_logged_frame > 600) {
          s_logged_frame = g_guest_frame;
          REXLOG_INFO("native-scene: loose world (type {}) without its own shadow rows - "
                      "world shadow state not served (rows {} frames old)",
                      lw_type, g_guest_frame - g_shadow_rows_frame);
        }
      }
    }
  }
  // Character CSM receive biases (see the header comment): frame-coherent,
  // served while fresh enough that a few capture-less frames don't flap the
  // char shadow between the exact and legacy paths.
  if (g_char_shadow_bias[0] > 0.0f &&
      g_guest_frame - g_char_shadow_bias_frame <= 120) {
    std::memcpy(scene.char_shadow_bias, g_char_shadow_bias,
                sizeof(scene.char_shadow_bias));
  }
  {
    const uint32_t bx = g_skin_blur_c0x.load(std::memory_order_relaxed);
    const uint32_t by = g_skin_blur_c0y.load(std::memory_order_relaxed);
    std::memcpy(&scene.skin_blur_c0[0], &bx, 4);
    std::memcpy(&scene.skin_blur_c0[1], &by, 4);
  }
  std::memcpy(scene.family_rows, g_family_rows, sizeof(g_family_rows));
  for (int m = 0; m < 2; ++m) {
    if (g_park_have[m]) {
      std::memcpy(scene.park_rows[m], g_park_rows[m], sizeof(g_park_rows[m]));
      scene.park_valid[m] = true;
    }
  }
  if (g_park_lm_have) {
    std::memcpy(scene.park_lm, g_park_lm, sizeof(g_park_lm));
    std::memcpy(scene.park_lm_fetch, g_park_lm_fetch, sizeof(g_park_lm_fetch));
    scene.park_lm_valid = true;
  }
  if (g_dynobj_have) {
    std::memcpy(scene.dynobj_rows, g_dynobj_rows, sizeof(g_dynobj_rows));
    scene.dynobj_valid = true;
    if (g_dynobj_ws_have) {
      std::memcpy(scene.dynobj_ws, g_dynobj_ws, sizeof(g_dynobj_ws));
      scene.dynobj_ws_valid = true;
    }
  }
  {
    // The world-shade rows are refreshed from EVERY receiver draw (character,
    // vehicle, prop), not only dynamicobject props: the prop-only copy went
    // stale where no prop was on screen (F11: map ~150 m off the skater).
    // Two row sets, as in the game: the pass renders the map with its own
    // VS rows; each receiver samples it with its own PS c5/c6/c7 (equal in
    // gameplay, different in create-a-skater, where the pass is empty).
    static float s_pass_rows[12] = {};
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    if (g_frame_ws_rows_have) {
      std::memcpy(s_pass_rows, g_frame_ws_rows, sizeof(s_pass_rows));
      g_frame_ws_rows_have = false;
    }
    std::memcpy(scene.ws_pass_rows, s_pass_rows, sizeof(scene.ws_pass_rows));
    if (g_frame_ws_recv_have) {
      std::memcpy(g_dynobj_ws, g_frame_ws_recv_rows, sizeof(g_dynobj_ws));
      g_dynobj_ws_have = true;
      g_frame_ws_recv_have = false;
    }
    if (g_dynobj_ws_have) {
      std::memcpy(scene.dynobj_ws, g_dynobj_ws, sizeof(scene.dynobj_ws));
      scene.dynobj_ws_valid = true;
    }
    scene.ws_resolved = g_frame_ws_resolved;
    g_frame_ws_resolved = false;
  }
  scene.clear_valid = g_clear_frame_area > 0;
  std::memcpy(scene.clear_color, g_clear_frame_rgb, sizeof(scene.clear_color));
  if (g_sky_frame_any) {
    if (!g_sky_have || std::fabs(g_sky_frame_y - g_sky_height) > 1.0f) {
      static int s_sky_logs = 0;  // TEMP probe (SKYY)
      if (s_sky_logs < 30) {
        ++s_sky_logs;
        REXLOG_INFO("SKYY sky viewpos y {:.2f} -> {:.2f} (cam y {:.2f})", g_sky_height,
                    g_sky_frame_y, scene.cam_pos[1]);
      }
    }
    g_sky_height = g_sky_frame_y;
    g_sky_have = true;
  }
  if (g_sky_have) {
    scene.sky_height = g_sky_height;
  }
  if (g_sky_sun_have) {
    std::memcpy(scene.sky_sun, g_sky_sun, sizeof(g_sky_sun));
    scene.sky_sun_valid = true;
  }
  // Publish the captured sun direction BEFORE any override (the debug
  // dialog seeds the override sliders from it).
  {
    const float* cap = scene.shadow_valid ? scene.shadow_rows + 24
                                          : scene.sky_sun;
    if (scene.shadow_valid || scene.sky_sun_valid) {
      g_sun_captured[0].store(cap[0], std::memory_order_relaxed);
      g_sun_captured[1].store(cap[1], std::memory_order_relaxed);
      g_sun_captured[2].store(cap[2], std::memory_order_relaxed);
    }
  }
  ApplySunOverride(scene);
  // The game's own popup blur chain (pause menu etc.), untouched by the
  // host settings menu, whose gaussian reads host state directly at render
  // time (see ApplyMenuBlurPass call sites).
  const bool blur_active = g_ui_blur_seen || g_ui_blur_hold > 0;
  scene.ui_blur = blur_active ? g_ui_blur : 0.0f;
  // Latched with the blur itself, so the hold frames keep the variant.
  static bool s_ui_blur_masked = false;
  if (g_ui_blur_seen) {
    s_ui_blur_masked = g_ui_blur_masked_seen;
  }
  scene.ui_blur_masked = blur_active && s_ui_blur_masked;
  std::memcpy(scene.ui_blur_color, g_ui_blur_color, sizeof(scene.ui_blur_color));
  if (g_ui_blur_seen) {
    g_ui_blur_hold = 2;
  } else if (g_ui_blur_hold > 0) {
    --g_ui_blur_hold;
  }
  {
    static bool s_blur_was_active = false;
    if (blur_active != s_blur_was_active) {
      REXLOG_INFO(
          "native-scene: popup background blur {} (kernel scale {:.1f}, fade "
          "{:.2f}/{:.2f}/{:.2f})",
          blur_active ? "ON" : "off", scene.ui_blur, scene.ui_blur_color[0],
          scene.ui_blur_color[1], scene.ui_blur_color[2]);
      s_blur_was_active = blur_active;
      if (!blur_active) {
        // Don't carry one popup's fade into the next popup's first frame
        // if its own c1 read misses the coherence gate.
        g_ui_blur_color[0] = g_ui_blur_color[1] = g_ui_blur_color[2] = 1.0f;
      }
    }
  }
  g_ui_blur_seen = false;
  g_ui_blur_masked_seen = false;
  // Fisheye-family final pass (see the OnDrawDone capture).
  scene.lens_fx.valid = g_lens_fx_seen || g_lens_fx_hold > 0;
  if (scene.lens_fx.valid) {
    scene.lens_fx.kind = g_lens_fx_kind;
    std::memcpy(scene.lens_fx.ps, g_lens_fx_ps, sizeof(scene.lens_fx.ps));
    std::memcpy(scene.lens_fx.vs, g_lens_fx_vs, sizeof(scene.lens_fx.vs));
    std::memcpy(scene.lens_fx.vignette_fetch, g_lens_fx_fetch[2],
                sizeof(scene.lens_fx.vignette_fetch));
    std::memcpy(scene.lens_fx.noise_fetch, g_lens_fx_fetch[5],
                sizeof(scene.lens_fx.noise_fetch));
  }
  {
    static const char* const kLensNames[3] = {"respawn TV noise", "fisheye lens",
                                              "fisheye lens (opaque)"};
    static int s_lens_was = -1;
    const int lens_now = scene.lens_fx.valid ? g_lens_fx_kind : -1;
    if (lens_now != s_lens_was) {
      const int named = lens_now >= 0 ? lens_now : s_lens_was;
      s_lens_was = lens_now;
      REXLOG_INFO(
          "native-scene: {} {} (blend {:08X} warp c1=({:.3f},{:.3f},{:.3f}) "
          "c4=({:.3f},{:.3f}) noise tex=[{:08X} {:08X}] vignette=[{:08X} {:08X}])",
          kLensNames[named], lens_now >= 0 ? "ON" : "off", g_lens_fx_blend,
          g_lens_fx_ps[1][0],
          g_lens_fx_ps[1][1], g_lens_fx_ps[1][2], g_lens_fx_ps[4][0],
          g_lens_fx_ps[4][1], g_lens_fx_fetch[5][0], g_lens_fx_fetch[5][1],
          g_lens_fx_fetch[2][0], g_lens_fx_fetch[2][1]);
      REXLOG_INFO("native-scene: DIAG lens rows c0=({:.3f},{:.3f},{:.3f},{:.3f}) "
                  "c2=({:.3f},{:.3f},{:.3f},{:.3f})",
                  g_lens_fx_ps[0][0], g_lens_fx_ps[0][1], g_lens_fx_ps[0][2],
                  g_lens_fx_ps[0][3], g_lens_fx_ps[2][0], g_lens_fx_ps[2][1],
                  g_lens_fx_ps[2][2], g_lens_fx_ps[2][3]);
    }
  }
  g_lens_fx_hold = g_lens_fx_seen ? 1 : (g_lens_fx_hold > 0 ? g_lens_fx_hold - 1 : 0);
  g_lens_fx_seen = false;
  // The game's bloom chain (see the OnDrawDone capture): valid while every
  // consumed pass drew within the last two publishes. The chain runs at the
  // end of the guest frame and can straddle a publish, so a per-publish
  // "all seven at once" test never passed.
  {
    uint32_t recent = 0;
    for (int p = 0; p < 7; ++p) {
      if ((g_game_bloom_seen >> p) & 1u) {
        g_game_bloom_age[p] = 0;
      } else if (g_game_bloom_age[p] < 255) {
        ++g_game_bloom_age[p];
      }
      if (g_game_bloom_age[p] <= 2) {
        recent |= 1u << p;
      }
    }
    // The chain's spine is threshold -> downsample -> combine; the tap9
    // blurs and the ghosts run only on some frames (log: stretches of
    // "63" = no blurs, combine weights (1, 1)), so they are per-pass.
    using GBP = FrameScene::GameBloom;
    constexpr uint32_t kSpine =
        (1u << GBP::kThreshold) | (1u << GBP::kDownB) | (1u << GBP::kCombine);
    scene.game_bloom.valid = (recent & kSpine) == kSpine;
    scene.game_bloom.passes = recent;
    if (scene.game_bloom.valid) {
      std::memcpy(scene.game_bloom.rows, g_game_bloom_rows,
                  sizeof(scene.game_bloom.rows));
    }
    static bool s_gb_was = false;
    if (scene.game_bloom.valid != s_gb_was) {
      s_gb_was = scene.game_bloom.valid;
      REXLOG_INFO(
          "native-scene: game bloom chain {} (threshold c4=({:.2f},{:.2f},{:.2f},{:.2f}) "
          "combine c0=({:.3f},{:.3f}) passes seen {:02X})",
          scene.game_bloom.valid ? "ON" : "off", g_game_bloom_rows[0][4][0],
          g_game_bloom_rows[0][4][1], g_game_bloom_rows[0][4][2],
          g_game_bloom_rows[0][4][3], g_game_bloom_rows[5][0][0],
          g_game_bloom_rows[5][0][1], recent);
    }
    g_game_bloom_seen = 0;
  }
  // Photo-editor postfx: publish the live pass captures when the photo
  // EDITOR itself is up (not the wider TakePhoto readback window: the
  // chain must never run over ordinary gameplay frames) and every pass has
  // fresh rows (the game stages its postfx constants each frame regardless
  // of draw suppression). The vignette and grain fetch words come from the
  // fisheye/uber fetch-shadow snapshots (slot 2 / slot 6 per the
  // ring-verified bindings).
  // Gameplay frames that run the same cinematic chain (bails / replays:
  // postfx_uberPS drawn this or the previous guest frame instead of
  // postfx_fast_uberPS) publish it too, with a tight freshness window so
  // the chain ends with the game's.
  uint8_t* pfx_base = g_guest_base.load(std::memory_order_relaxed);
  const bool pfx_editor = pfx_base != nullptr &&
                          g_photo_flow_frame.load(std::memory_order_relaxed) &&
                          PhotoEditorSignal(pfx_base) != nullptr;
  const bool pfx_gameplay = pfx_base != nullptr && !pfx_editor && g_pfx_uber_frame != 0 &&
                            g_guest_frame - g_pfx_uber_frame <= 2;
  if (pfx_editor || pfx_gameplay) {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    const int64_t window_ns = pfx_editor ? 1'000'000'000 : 150'000'000;
    bool fresh = true;
    for (int p = 0; p < kPfxPassCount; ++p) {
      if (g_pfx_cap[p].ps_ns == 0 || now_ns - g_pfx_cap[p].ps_ns > window_ns ||
          !g_pfx_cap[p].vs_seen) {
        fresh = false;
      }
    }
    if (!fresh) {
      // Name the missing pass(es), once per ~2 s while in the window.
      static int64_t s_diag_ns = 0;
      if (now_ns - s_diag_ns > 2'000'000'000) {
        s_diag_ns = now_ns;
        char buf[160];
        int off = 0;
        for (int p = 0; p < kPfxPassCount; ++p) {
          const int64_t age_ms =
              g_pfx_cap[p].ps_ns == 0 ? -1 : (now_ns - g_pfx_cap[p].ps_ns) / 1'000'000;
          off += std::snprintf(buf + off, sizeof(buf) - off, " p%d[ps%lldms vs%d]", p,
                               (long long)age_ms, g_pfx_cap[p].vs_seen ? 1 : 0);
          if (off >= int(sizeof(buf)) - 24) break;
        }
        REXLOG_INFO("native-scene: pfx captures NOT fresh:{}", buf);
      }
    }
    if (fresh) {
      scene.photo_fx.valid = true;
      scene.photo_fx.gameplay = pfx_gameplay;
      static bool s_gameplay_logged = false;
      if (pfx_gameplay && !s_gameplay_logged) {
        s_gameplay_logged = true;
        REXLOG_INFO("native-scene: gameplay cinematic postfx chain LIVE (postfx_uberPS "
                    "frame: bail / replay)");
      }
      for (int p = 0; p < kPfxPassCount; ++p) {
        std::memcpy(scene.photo_fx.ps[p], g_pfx_cap[p].ps,
                    sizeof(scene.photo_fx.ps[p]));
        std::memcpy(scene.photo_fx.vs[p], g_pfx_cap[p].vs,
                    sizeof(scene.photo_fx.vs[p]));
      }
      std::memcpy(scene.photo_fx.vignette_fetch, g_pfx_cap[kPfxFisheye].fetch[2],
                  sizeof(scene.photo_fx.vignette_fetch));
      std::memcpy(scene.photo_fx.grain_fetch, g_pfx_cap[kPfxUber].fetch[6],
                  sizeof(scene.photo_fx.grain_fetch));
      static bool s_pfx_logged = false;
      if (!s_pfx_logged) {
        s_pfx_logged = true;
        REXLOG_INFO(
            "native-scene: photo postfx captures LIVE (visualfx c0=({:.3f},{:.3f},"
            "{:.3f},{:.3f}) dof c0.x={:.4f} uber c5=({:.3f},{:.3f},{:.3f},{:.3f}) "
            "fisheye c1=({:.3f},{:.3f},{:.3f}) vignette=[{:08X} {:08X}] "
            "grain=[{:08X} {:08X}])",
            g_pfx_cap[kPfxVisualFx].ps[0][0], g_pfx_cap[kPfxVisualFx].ps[0][1],
            g_pfx_cap[kPfxVisualFx].ps[0][2], g_pfx_cap[kPfxVisualFx].ps[0][3],
            g_pfx_cap[kPfxDof].ps[0][0], g_pfx_cap[kPfxUber].ps[5][0],
            g_pfx_cap[kPfxUber].ps[5][1], g_pfx_cap[kPfxUber].ps[5][2],
            g_pfx_cap[kPfxUber].ps[5][3], g_pfx_cap[kPfxFisheye].ps[1][0],
            g_pfx_cap[kPfxFisheye].ps[1][1], g_pfx_cap[kPfxFisheye].ps[1][2],
            scene.photo_fx.vignette_fetch[0], scene.photo_fx.vignette_fetch[1],
            scene.photo_fx.grain_fetch[0], scene.photo_fx.grain_fetch[1]);
      }
    }
  }
  std::memcpy(g_fog_cam, scene.cam_pos, sizeof(g_fog_cam));
  g_fog_frame_done = false;
  g_shadow_frame_done = false;
  g_sky_frame_done = false;
  g_sky_frame_any = false;
  g_clear_frame_area = 0;
  g_tree_frame_done = false;
  g_proxy_frame_done = false;
  g_dynobj_frame_done = false;
  g_park_frame_done = false;
  g_park_model_done[0] = g_park_model_done[1] = false;
  g_water_frame_done = false;
  g_waterdef_frame_done = false;
  g_ocean_frame_done = false;
  g_ad_frame_done = false;
  g_ts_frame_done = false;
  g_oceanrefl_frame_done = false;
  g_scroll_frame_done = false;


  if (g_recording.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    if (++g_frames_seen % g_record_stride == 0) {
      RecordedFrame rf;
      rf.generation = g_generation + 1;
      std::memcpy(rf.view_proj, scene.view_proj, sizeof(rf.view_proj));
      std::memcpy(rf.cam_pos, scene.cam_pos, sizeof(rf.cam_pos));
      std::memcpy(rf.shadow_rows, scene.shadow_rows, sizeof(rf.shadow_rows));
      std::memcpy(rf.char_shadow_bias, scene.char_shadow_bias, sizeof(rf.char_shadow_bias));
      rf.shadow_valid = scene.shadow_valid;
      rf.dynitems = dynitems;
      rf.items = scene.items;
      // scene.draw_rs is only assigned at the end of this build, so take the
      // frame's own map (swapped out of g_frame_draw_rs above) directly.
      rf.draw_rs = draw_rs;
      g_recorded_frames.push_back(std::move(rf));
      ++g_record_frame;
    }
  }

  BuildSectMark(11);
  // The draw-time fetch map served this frame's item builds (streamed-artwork
  // diffuse override); next frame's draws repopulate it. Cleared HERE, not at
  // the top with the other per-frame structures; the world items that
  // consume it are built above, after that swap.
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    g_frame_draw_fetch.clear();
    // Per frame like the fetch map: buffer objects are recycled for newly
    // streamed meshes, so a persistent census attributed a previous
    // occupant's shaders to the new mesh.
    g_census_ps.clear();
  }

  // Frame tail: rotate the bone-palette snapshot ring. Timed as its own
  // block in the perf line; this tail runs on the guest render thread,
  // where spikes are visible stutter.
  {
    const auto pal_tail_t0 = PerfClock::now();
    skate3::native_palette::OnFrameBuilt();
    g_frame_pal_tail_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 PerfClock::now() - pal_tail_t0)
                                 .count());
    g_pw_pal_tail.Add(g_frame_pal_tail_ns);
  }

  // AUX-publish gate: the skater-portrait render-to-texture passes submit a
  // real perspective SceneRenderView, and on the frontend menu screens
  // (team boxes, Import Skater) it is the ONLY publisher; publishing it
  // rendered the portrait FULL SCREEN behind the menu for the whole
  // pause-freshness window on every entry/scroll (flapping
  // pause-native <-> loading in 300 ms cycles). Its projection is
  // screen-shaped (the aspect gate at view selection above never fired), so
  // identify it by CONTENT: a menu-context (presence 0) scene consisting of
  // nothing but character-family pieces (skater + board, every material
  // "character.*") is a portrait pass, never the visible frame. Real menu
  // backdrops always carry world geometry (the pause plaza ~700 items, the
  // CAS editor room has env-family walls). Skipped publishes also skip the
  // freshness stamp, so the mode never flips.
  // CAS-editor exemption: the STARTUP-flow editor's scene is skater-only
  // (no garage world pre-gameplay: 10 all-char items, exactly a portrait
  // pass's shape; without the exemption this gate ate it, 3D black
  // behind a live menu). The editor's _nis shader heartbeat separates the
  // two: editor draws stamp it every frame, portrait passes use the
  // _default compiles and never do.
  if (rex::kernel::guest_presence::GameplayContextValue() == 0 &&
      !scene.items.empty() && scene.items.size() <= 48 &&
      !CasEditorActive(base)) {
    bool all_char = true;
    for (const DrawItem& it : scene.items) {
      if (it.char_family == 0) {
        all_char = false;
        break;
      }
    }
    if (all_char) {
      static std::atomic<uint32_t> s_aux_pub{0};
      const uint32_t n = s_aux_pub.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 255u) == 0) {
        const float m00 = LoadGuestF32(base, viewcam + 0x60 + 0 * 4);
        const float m11 = LoadGuestF32(base, viewcam + 0x60 + (1 * 4 + 1) * 4);
        REXLOG_INFO(
            "native-scene: portrait-pass publish skipped ({} char items, cam "
            "({:.1f},{:.1f},{:.1f}), proj m00={:.3f} m11={:.3f}) (n={})",
            scene.items.size(), scene.cam_pos[0], scene.cam_pos[1],
            scene.cam_pos[2], m00, m11, n);
      }
      return;
    }
  }
  scene.play_time = g_smooth_active ? g_smooth_play : 0.0;
  if (REXCVAR_GET(skate3_native_render_scene_ghost_audit)) {
    GhostAudit(base, scene, draw_rs);
  }
  s_items_last = scene.items.size();
  scene.draw_rs = std::move(draw_rs);
  scene.rebuilt_meshes.swap(g_rebuilt_meshes_pub);
  g_rebuilt_meshes_pub.clear();
  // Game shadow-pass parity (see g_frame_ortho_ctx): mark the items whose
  // ctx the game submitted through an ortho caster bank. Runs after every
  // item source (merge, rescues, gap fill) and is OR-only. The ortho
  // capture races the publish (more often the higher the render rate), and
  // a fresh capture defaults false: marking from THIS frame's set alone
  // blinked the skater's shadow off for single frames while the body kept
  // rendering. A ctx therefore counts as a caster while it was
  // ortho-submitted within the last kOrthoHoldFrames build frames;
  // never-submitted pieces (the trucker hat) stay non-casters forever.
  {
    static std::unordered_map<uint32_t, uint64_t> s_ortho_last;
    static uint64_t s_ortho_frame = 0;
    ++s_ortho_frame;
    if (s_ortho_last.size() > 4096) {
      s_ortho_last.clear();
    }
    ortho_ctx.for_each([&](uint32_t ctx) { s_ortho_last[ctx] = s_ortho_frame; });
    // Per-cascade caster parity (see g_frame_ortho_mag): classify each
    // submission's row norm against the frame's receiver rows (finest X
    // row c0, cascade scales c1.x / c2.x). A tile counts while the ctx was
    // submitted to it within the last kTileHoldFrames builds: the ortho
    // capture races the publish, so one frame's set can hold only part of
    // a straddling piece's tiles. Casting every piece into every tile
    // self-shadowed the CAS face wherever its pixels fell past the finest
    // cascade's edge (a coarse nose/brow shadow the game never draws).
    static std::unordered_map<uint32_t, std::array<uint64_t, 3>> s_ortho_tile_last;
    if (s_ortho_tile_last.size() > 4096) {
      s_ortho_tile_last.clear();
    }
    // (The lighting-lab sun override re-fits the cascades around its own
    // basis, so the game's tiles no longer correspond: all tiles then.)
    if (scene.shadow_valid && !REXCVAR_GET(skate3_native_render_scene_sun_override)) {
      const float* sr = scene.shadow_rows;
      const float l0 = std::sqrt(sr[0] * sr[0] + sr[1] * sr[1] + sr[2] * sr[2]);
      const float ref[3] = {l0, l0 * std::fabs(sr[4]), l0 * std::fabs(sr[8])};
      for (const auto& [ctx, m] : ortho_mag) {
        for (int k = 0; k < 3; ++k) {
          if (ref[k] > 1e-6f && std::fabs(m - ref[k]) <= 0.03f * ref[k]) {
            s_ortho_tile_last[ctx][size_t(k)] = s_ortho_frame;
          }
        }
      }
    }
    constexpr uint64_t kTileHoldFrames = 2;
    constexpr uint64_t kOrthoHoldFrames = 60;
    for (DrawItem& item : scene.items) {
      if (item.ctx == 0) {
        continue;
      }
      item.caster_cascades = 0;
      const auto it = s_ortho_last.find(item.ctx);
      if (it != s_ortho_last.end() &&
          s_ortho_frame - it->second <= kOrthoHoldFrames) {
        item.shadow_caster = true;
        const auto mt = s_ortho_tile_last.find(item.ctx);
        if (mt != s_ortho_tile_last.end()) {
          for (int k = 0; k < 3; ++k) {
            const uint64_t f = mt->second[size_t(k)];
            if (f != 0 && s_ortho_frame - f <= kTileHoldFrames) {
              item.caster_cascades |= uint8_t(1u << k);
            }
          }
        }
      }
    }
  }
  g_last_publish_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count(),
                          std::memory_order_relaxed);
  std::shared_ptr<const FrameScene> replaced;
  {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    scene.generation = ++g_generation;
    replaced = std::move(g_scene);
    // Non-const allocation: a VR eye pass swaps the camera in place while it
    // renders (VrEyeCamera, skate3_native_scene_gpu.cpp).
    g_scene = std::make_shared<FrameScene>(std::move(scene));
  }
  if (replaced) {
    std::lock_guard<std::mutex> lock(g_scene_retire_mutex);
    if (g_scene_retire.size() < 8) {
      g_scene_retire.push_back(std::move(replaced));
    }
  }
}

// StartRecording / WriteRecording: native/skate3_native_diagnostics.cpp.

}  // namespace skate3::native_scene

// Sk8::Render::ViewCamera::SetViewMatrix(const rw::math::Matrix44&): the
// game passes each frame's freshly computed view matrix here and derives
// the stored view (+0x20), its transpose (+0xE0), the view-proj (+0xA0) and
// the frustum data used for culling from the argument. While the freecam is
// engaged, rewrite the incoming matrix for the MAIN scene camera (the one
// the frame build publishes as g_sampler_viewcam) with the flown view
// before the game consumes it; culling, submission and every derived
// consumer then operate around the drone pose natively. Other ViewCameras
// (shadow cascades, portrait render-to-texture passes) pass through
// untouched.
// Sk8::Render::ViewCamera::SetViewMatrix(const rw::math::Matrix44&): the
// game passes each frame's freshly computed view matrix here and derives
// the stored view (+0x20), its transpose (+0xE0), the view-proj (+0xA0) and
// the frustum data used for culling from the argument. While the freecam is
// engaged, rewrite the incoming matrix for the MAIN scene camera (the one
// the frame build publishes as g_sampler_viewcam) with the flown view
// before the game consumes it; culling, submission and every derived
// consumer then operate around the drone pose natively. Other ViewCameras
// (shadow cascades, portrait render-to-texture passes) pass through
// untouched.
// Sk8::Flow::StateLoading::OnEnterState / OnLeaveState: the game's own
// loading bracket. Everything between them is a world that is not ready to
// be shown -- streaming, hull build, character prepare, the exit-to-world
// fade -- and OnLeaveState is the point its own log calls "GAME LOAD END".
// The native takeover gate rides this instead of guessing from the scene.
#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(rex_Sk8_Flow_StateLoading_OnEnterState_MAAXXZ_8286C010) {
  skate3::native_scene::g_guest_loading.store(true, std::memory_order_release);
  __imp__rex_Sk8_Flow_StateLoading_OnEnterState_MAAXXZ_8286C010(ctx, base);
}
#endif  // TODO(retail port)


#if 0  // TODO(retail port): retail address not verified yet
extern "C" REX_FUNC(rex_Fesl_TheaterServiceImpl_Connect_EAAXPAVTransaction_2_PBD1111_Z_8286C150) {
  __imp__rex_Fesl_TheaterServiceImpl_Connect_EAAXPAVTransaction_2_PBD1111_Z_8286C150(ctx, base);
  skate3::native_scene::g_guest_loading.store(false, std::memory_order_release);
}
#endif  // TODO(retail port)


extern "C" REX_FUNC(rex_Sk8_Render_ViewCamera_SetViewMatrix_QAAXABVMatrix44_vpu_math_rw_Z_82802A00) {
  namespace ns = skate3::native_scene;
  const uint32_t cam = ctx.r3.u32;
  const uint32_t mtx = ctx.r4.u32;
  if (ns::g_freecam_guest_active.load(std::memory_order_acquire) != 0 &&
      mtx != 0 &&
      cam == ns::g_sampler_viewcam.load(std::memory_order_relaxed)) {
    float v[16];
    {
      std::lock_guard<std::mutex> lock(ns::g_freecam_guest_mutex);
      std::memcpy(v, ns::g_freecam_guest_view, sizeof(v));
    }
    for (int i = 0; i < 16; ++i) {
      uint32_t raw;
      std::memcpy(&raw, &v[i], 4);
      raw = __builtin_bswap32(raw);
      std::memcpy(base + mtx + i * 4, &raw, 4);
    }
    ns::g_freecam_guest_rewrites.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__rex_Sk8_Render_ViewCamera_SetViewMatrix_QAAXABVMatrix44_vpu_math_rw_Z_82802A00(ctx, base);
}

