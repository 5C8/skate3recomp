// SteamVR (OpenVR) output for the native renderer (see skate3_vr.h).
// Shared by sk83_na_m and skate3recomp: keep the two copies identical.
//
// The OpenVR C function-table API (openvr_capi.h) is used with
// openvr_api.dll loaded at runtime, the same way the Skate It VR port does:
// no import library, and a machine without SteamVR runs flat.
//
// Conventions. The renderer's matrices are row-vector (v' = v * M, element
// [r * 4 + c]). OpenVR poses are column-vector 3x4 (p_tracking = M * p),
// right-handed, +Y up, -Z forward. The game camera space is the view's
// output: +Z forward (projection m23 = 1), and screen right / up are
// sign(P00) * x / sign(P11) * y (this game's P00 is negative), so OpenVR
// directions map into it through C = diag(sign(P00), sign(P11), -1).

#include "skate3_vr.h"
#include "skate3_vr_controls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>

REXCVAR_DEFINE_BOOL(skate3_vr, false, "VR",
                    "Render to a SteamVR headset (Direct3D 12 only). The native renderer "
                    "draws the scene once per eye from the game camera with your head pose "
                    "on top; the HUD hangs as a head-locked panel and menus/loading show on "
                    "a virtual screen. VR controllers work as an Xbox pad (hold the left "
                    "stick click for Start / Back / D-pad, + right stick click recenters).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_first_person, false, "VR",
                    "First-person VR. Toggle with left thumbstick click + X (left A on Index).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_fp_neck_limit, 110.0, "VR",
                      "First person: how far the view may turn from the skater's chest, in "
                      "degrees (a neck). Riding needs 90 (skaters stand sideways); past the "
                      "limit the view turns with the body.")
    .range(90.0, 170.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_fp_neck_speed, 540.0, "VR",
                      "First person: how fast the view turns from one shoulder to the other "
                      "(deg/s), e.g. after a 180.")
    .range(90.0, 2000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_eye_vertical_offset, 0.0, "VR",
                      "Additional vertical eye offset from the blended head pose, in game metres.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);


REXCVAR_DEFINE_BOOL(skate3_vr_simulate, false, "VR",
                    "Run the VR render path without a headset (performance testing): Quest "
                    "2-like eye resolution and optics, a still head, nothing sent to SteamVR. "
                    "The window shows the left eye.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(skate3_vr_perf_capture, "", "VR",
                      "Performance testing: write a perf counter capture (the F8 capture, "
                      "perf_capture_frames long) to this path skate3_vr_perf_capture_delay "
                      "seconds after the VR session starts.");
REXCVAR_DEFINE_INT32(skate3_vr_perf_capture_delay, 45, "VR",
                     "Seconds into the VR session before skate3_vr_perf_capture starts.")
    .range(1, 3600);
REXCVAR_DEFINE_BOOL(skate3_vr_flush_eyes, true, "VR",
                    "Submit the left eye's GPU work before recording the right eye, so "
                    "the GPU renders one eye while the CPU records the other.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_single_pass, true, "VR",
                    "Draw both eyes in one pass (instanced, side by side): roughly halves "
                    "the CPU draw recording and per-draw GPU cost. Off = one full render "
                    "per eye. Screen-space AO / reflections / volumetrics are skipped in "
                    "one-pass mode.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_hidden_area, true, "VR",
                    "Skip shading the parts of each eye image the headset's lenses never "
                    "show (OpenVR hidden-area mesh, masked with depth before the scene).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_render_scale, 1.0, "VR",
                      "Per-eye resolution relative to SteamVR's recommended size (which "
                      "already includes SteamVR's own supersampling setting). The scene is "
                      "rendered twice per frame, so lower this first if the headset drops "
                      "frames.")
    .range(0.3, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_vr_msaa, 2, "VR",
                     "MSAA ceiling while in VR (the flat setting still applies when lower). "
                     "Both eyes render at headset resolution, so 4x/8x costs far more than "
                     "on a monitor.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_world_scale, 1.0, "VR",
                      "Game units per real-world metre for head movement (1 = life size).")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_level_horizon, true, "VR",
                    "Remove the game camera's pitch and roll so the horizon stays level; "
                    "only its heading still turns you (look down to see the skater).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_positional, true, "VR",
                    "Apply head translation (lean in, look around) on top of the game camera.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_hud_follow_camera, true, "VR",
                    "Put the HUD on the game camera's 16:9 screen: it follows the game "
                    "camera and spans the flat game's field of view, as on a monitor. Off = "
                    "a head-locked panel (skate3_vr_hud_width / _drop).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_follow_zoom, true, "VR",
                    "Zoom the headset view when the game camera zooms (narrows its field of "
                    "view relative to skate3_vr_zoom_base_fov), centred on the lens axis.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_zoom_base_fov, 67.38, "VR",
                      "The game camera's normal vertical field of view (degrees) as its "
                      "projection encodes it: no zoom at this FOV. The game's default 60 "
                      "degree setting builds |m11| = 1.5, i.e. 67.38 here (measured).")
    .range(20.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_zoom_min, 1.0, "VR",
                      "Lowest zoom applied (< 1 lets a game camera WIDER than normal shrink "
                      "the view too; 1 = zoom in only, widening is ignored).")
    .range(0.5, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_zoom_max, 4.0, "VR",
                      "Highest zoom applied when following the game camera.")
    .range(1.0, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_scale, 1.0, "VR",
                      "Size of the HUD relative to the flat game's field of view "
                      "(skate3_vr_hud_follow_camera). 1 = exactly the flat screen's extent, "
                      "which nearly fills each eye: UI at the screen corners (the replay "
                      "editor toolbar) then lands where the lenses cut off; ~0.8 keeps it "
                      "all in view.")
    .range(0.3, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_curve_x, 0.5, "VR",
                      "Horizontal curvature of the third-person HUD: 0 = flat, 1 = a "
                      "circle centred on the game camera (HUD distance as radius). The "
                      "HUD keeps its extent and layout; its sides wrap around the viewer.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_curve_y, 0.25, "VR",
                      "Vertical curvature of the third-person HUD (see "
                      "skate3_vr_hud_curve_x).")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_fov, 100.0, "VR",
                      "Headset field of view at the game's normal camera, in percent of the "
                      "headset's own (100). Higher shows more of the world (zoomed out), "
                      "lower zooms in; game camera zooms (skate3_vr_follow_zoom) apply on "
                      "top.")
    .range(50.0, 150.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_distance, 2.0, "VR",
                      "Distance of the HUD, in metres (sets its stereo depth; with "
                      "skate3_vr_hud_follow_camera its angular size stays the game's FOV).")
    .range(0.5, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_width, 1.7, "VR",
                      "Width of the HUD panel (the game's 16:9 2D layer), in metres.")
    .range(0.2, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_hud_drop, 0.1, "VR",
                      "How far below eye level the HUD panel's centre sits, in metres.")
    .range(-5.0, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_screen_distance, 2.5, "VR",
                      "Distance of the virtual screen used for menus, loading screens and "
                      "editors (frames the native renderer hands to the emulated output).")
    .range(0.5, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_screen_width, 2.8, "VR",
                      "Width of the virtual screen, in metres.")
    .range(0.2, 40.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_menu_distance, 1.4, "VR",
                      "Distance of the settings menu panel, in metres; it opens straight "
                      "ahead of where you look.")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_menu_width, 2.0, "VR",
                      "Width of the settings menu panel, in metres.")
    .range(0.3, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_wide_cull, true, "VR",
                    "Make the game cull against where you are looking: its per-object "
                    "frustum test uses the headset's view (both eyes) instead of the flat "
                    "camera's, so turning your head finds geometry and nothing outside the "
                    "view is drawn.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_cull_margin, 12.0, "VR",
                      "Degrees added around the headset view for the game's culling: covers "
                      "head motion between the game frame and its display. Too small shows "
                      "geometry popping in at the view edges on fast head turns.")
    .range(0.0, 45.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_guest_aspect, 2.4, "VR",
                      "Aspect ratio the game camera builds and culls with while in VR "
                      "(skate3_vr_wide_cull).")
    .range(1.7, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_vr_guest_fov, 100.0, "VR",
                      "Field of view the game camera culls with while in VR, where the build "
                      "has a field-of-view hook (skate3_vr_wide_cull).")
    .range(40.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_vr_controllers, true, "VR",
                    "Drive the game with the VR controllers as an Xbox pad (a regular pad "
                    "keeps working alongside).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include "openvr/openvr_capi.h"

namespace skate3::vr {
namespace {

namespace nrhi = rex::graphics::nrhi;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
using Clock = std::chrono::steady_clock;

// ---- small math (double) ---------------------------------------------------

struct Pose {  // column-vector: p' = R * p + t
  double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  double t[3] = {};
};

Pose FromHmd(const HmdMatrix34_t& m) {
  Pose p;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) p.R[i][j] = m.m[i][j];
    p.t[i] = m.m[i][3];
  }
  return p;
}

Pose Compose(const Pose& a, const Pose& b) {  // a * b
  Pose o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      o.R[i][j] = a.R[i][0] * b.R[0][j] + a.R[i][1] * b.R[1][j] + a.R[i][2] * b.R[2][j];
    }
    o.t[i] = a.R[i][0] * b.t[0] + a.R[i][1] * b.t[1] + a.R[i][2] * b.t[2] + a.t[i];
  }
  return o;
}

Pose Inverse(const Pose& a) {
  Pose o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) o.R[i][j] = a.R[j][i];
  }
  for (int i = 0; i < 3; ++i) {
    o.t[i] = -(o.R[i][0] * a.t[0] + o.R[i][1] * a.t[1] + o.R[i][2] * a.t[2]);
  }
  return o;
}

void Mul4(const double* a, const double* b, double* o) {
  double r[16];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                     a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    }
  }
  std::memcpy(o, r, sizeof(r));
}

bool Invert4(const double* m, double* out) {
  double inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(std::fabs(det) > 1e-30)) {
    return false;
  }
  for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
  return true;
}

void ToDouble(const float* f, double* d) {
  for (int i = 0; i < 16; ++i) d[i] = f[i];
}
void ToFloat(const double* d, float* f) {
  for (int i = 0; i < 16; ++i) f[i] = float(d[i]);
}

// ---- OpenVR session ----------------------------------------------------------

typedef intptr_t (*PFN_InitInternal)(EVRInitError*, EVRApplicationType);
typedef void (*PFN_ShutdownInternal)();
typedef bool (*PFN_IsHmdPresent)();
typedef intptr_t (*PFN_GetGenericInterface)(const char*, EVRInitError*);

struct EyeOptics {
  Pose eye_to_head;
  // Frustum tangents in display terms: x in [l, r], y in [d, u] (up
  // positive). OpenVR's raw "top" is the downward tangent and "bottom" the
  // upward one (its y-down naming; see IVRSystem::GetProjectionRaw).
  double l = -1, r = 1, d = -1, u = 1;
};

struct Session {
  bool simulated = false;  // skate3_vr_simulate: no OpenVR at all
  HMODULE dll = nullptr;
  PFN_InitInternal init = nullptr;
  PFN_ShutdownInternal shutdown = nullptr;
  PFN_IsHmdPresent hmd_present = nullptr;
  PFN_GetGenericInterface get_interface = nullptr;
  VR_IVRSystem_FnTable* sys = nullptr;
  VR_IVRCompositor_FnTable* comp = nullptr;
  bool tried = false;  // one start attempt per enable
};

Session g_s;
rex::input::InputSystem* g_input = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_eye_w{0}, g_eye_h{0};
// Unscaled per-eye size (SteamVR's recommendation, or the simulated
// headset's): skate3_vr_render_scale is applied on top every frame, so the
// setting takes effect live.
uint32_t g_base_eye_w = 0, g_base_eye_h = 0;
uint32_t ScaledEyeDim(uint32_t base) {
  return std::clamp(uint32_t(double(base) * REXCVAR_GET(skate3_vr_render_scale)) & ~1u, 256u,
                    8192u);
}

// Per-swap frame state (command-processor thread only).
bool g_frame_open = false;
Clock::time_point g_frame_wait_time{};
bool g_have_head = false;
Pose g_head;              // HMD, tracking space
EyeOptics g_optics[2];
Pose g_eye_world[2];      // eye poses, tracking space
Pose g_eye_rel[2];        // eye poses relative to the recentered reference
bool g_have_ref = false;
Pose g_ref;               // yaw-only head pose at recenter
std::atomic<bool> g_recenter_request{false};
int g_current_eye = -1;
bool g_first_person_scene = false;
float g_first_person_forward[3] = {0,0,1};
float g_first_person_head[3] = {};
bool g_first_person_frame = false;

// Headset view for the guest cull (written by the left-eye ComputeEyeCamera
// on the command-processor thread, read by guest threads): display axes in
// world space and the eye position.
struct CullView {
  bool valid = false;
  double right[3] = {}, up[3] = {}, fwd[3] = {}, pos[3] = {}, game_pos[3] = {};
  double l = -1, r = 1, d = -1, u = 1;  // both eyes' extents
  Clock::time_point time{};
};
std::mutex g_cull_mutex;
CullView g_cull;

// The game camera of the frame (unleveled view, axis signs) for the HUD.
double g_game_view[16] = {};
double g_game_sx = 1.0, g_game_sy = 1.0;
bool g_game_view_valid = false;
std::atomic<uint64_t> g_fov_ratio_bits{0};  // double; 0 = 1.0

// Headset zoom following the game camera's FOV (SetGameFov).
double g_zoom = 1.0;

// Settings menu in the headset (SetMenuHooks): the renderer draws the open
// menu on the world-locked screen panel (MenuVisible).
std::function<bool()> g_menu_visible;
std::function<void()> g_menu_toggle;
bool g_menu_shown = false;  // command-processor thread
std::atomic<bool> g_menu_toggle_request{false};
Pose g_menu_ref;  // yaw-only head pose when the menu opened
std::mutex g_menu_rect_mutex;  // SetMenuRect (UI thread) vs the renderer
float g_menu_rect[4] = {};
bool g_menu_rect_valid = false;

// Last ComputeEyeCamera inputs/outputs (for the 2D re-target).
double g_game_vp[16] = {};
double g_eye_vp[2][16] = {};
bool g_eye_vp_valid[2] = {false, false};
float g_eye_vp_f[2][16] = {};
float g_stereo_d[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Simulated session: the frame's last submission, waited on at the next
// frame start like the compositor's WaitGetPoses gate (so the measured
// frame time is begin -> GPU done, the number that decides 90 vs 45 Hz).
ID3D12Fence* g_sim_fence = nullptr;
HANDLE g_sim_fence_event = nullptr;
uint64_t g_sim_fence_value = 0;
uint64_t g_sim_wait_submission = 0;  // fence value to wait for, 0 = none
Clock::time_point g_sim_frame_begin{};
double g_sim_latency_sum = 0.0;
uint32_t g_sim_latency_n = 0;
double g_sim_first_submit_ms = -1.0;  // this frame
double g_sim_first_sum = 0.0, g_sim_final_sum = 0.0;

// Submission of this swap.
struct FrameTextures {
  void* left = nullptr;
  void* right = nullptr;
  void* queue = nullptr;
  bool valid = false;
  bool linear = false;
  bool side_by_side = false;  // one texture, eyes in its halves
};
FrameTextures g_submit;

// Hidden-area meshes per eye (eye NDC triangles), built at session start.
std::vector<float> g_hidden_area[2];

void BuildHiddenArea() {
  for (int e = 0; e < 2; ++e) {
    std::vector<float>& out = g_hidden_area[e];
    out.clear();
    if (g_s.simulated) {
      // Quest-like: everything outside an ellipse slightly larger than the
      // frustum's inscribed circle (the corners), as a ring of quads from
      // the ellipse out to the frustum square.
      constexpr int kSegments = 48;
      constexpr float kRadius = 1.12f;
      const auto ellipse = [](int k, float* x, float* y) {
        const float a = float(k) * (6.28318530718f / float(kSegments));
        *x = kRadius * std::cos(a);
        *y = kRadius * std::sin(a);
      };
      const auto square = [](float x, float y, float* sx, float* sy) {
        const float m = std::max(std::fabs(x), std::fabs(y));
        *sx = x / m;
        *sy = y / m;
      };
      for (int k = 0; k < kSegments; ++k) {
        float x0, y0, x1, y1, sx0, sy0, sx1, sy1;
        ellipse(k, &x0, &y0);
        ellipse(k + 1, &x1, &y1);
        square(x0, y0, &sx0, &sy0);
        square(x1, y1, &sx1, &sy1);
        // Clamp the inner edge to the square: where the ellipse lies outside
        // the frustum there is nothing hidden.
        const auto clamp1 = [](float v) { return std::clamp(v, -1.0f, 1.0f); };
        const float pts[12] = {clamp1(x0), clamp1(y0), sx0, sy0, sx1, sy1,
                               clamp1(x0), clamp1(y0), sx1, sy1, clamp1(x1), clamp1(y1)};
        out.insert(out.end(), pts, pts + 12);
      }
      continue;
    }
    if (g_s.sys == nullptr) continue;
    const HiddenAreaMesh_t mesh =
        g_s.sys->GetHiddenAreaMesh(EVREye(e), EHiddenAreaMeshType_k_eHiddenAreaMesh_Standard);
    if (mesh.pVertexData == nullptr || mesh.unTriangleCount == 0) continue;
    out.reserve(size_t(mesh.unTriangleCount) * 6);
    for (uint32_t i = 0; i < mesh.unTriangleCount * 3; ++i) {
      // UV space (0..1, v down) -> NDC.
      out.push_back(mesh.pVertexData[i].v[0] * 2.0f - 1.0f);
      out.push_back(1.0f - mesh.pVertexData[i].v[1] * 2.0f);
    }
  }
  REXLOG_INFO("vr: hidden-area mesh {} / {} triangles", g_hidden_area[0].size() / 6,
              g_hidden_area[1].size() / 6);
}

// Own eye targets.
struct EyeTargets {
  nrhi::Texture* tex[2] = {nullptr, nullptr};
  uint32_t w = 0, h = 0;
  nrhi::Format format = nrhi::Format::kUnknown;
};
EyeTargets g_targets;
nrhi::Device* g_device = nullptr;
bool g_targets_release = false;

// ---- controller pad (shared with the guest input thread) ----

struct PadState {
  bool connected = false;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_pad_mutex;
PadState g_pad;
uint32_t g_pad_packet = 0;
std::atomic<uint32_t> g_rumble_bits{0};  // float strength 0..1

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float BitsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

bool StartSession() {
  if (REXCVAR_GET(skate3_vr_simulate)) {
    // Quest 2 at SteamVR's 100%: 2244 x 2352 per eye.
    g_base_eye_w = 2244;
    g_base_eye_h = 2352;
    g_eye_w.store(ScaledEyeDim(g_base_eye_w));
    g_eye_h.store(ScaledEyeDim(g_base_eye_h));
    g_s.simulated = true;
    g_have_ref = false;
    g_have_head = false;
    g_frame_open = false;
    g_on.store(true, std::memory_order_release);
    REXLOG_INFO("vr: SIMULATED session (no headset) - per eye {}x{}", g_eye_w.load(),
                g_eye_h.load());
    BuildHiddenArea();
    return true;
  }
  if (!g_s.dll) {
    g_s.dll = LoadLibraryA("openvr_api.dll");
    if (!g_s.dll) {
      REXLOG_WARN("vr: openvr_api.dll not found next to the exe - running flat");
      return false;
    }
    g_s.init = reinterpret_cast<PFN_InitInternal>(GetProcAddress(g_s.dll, "VR_InitInternal"));
    g_s.shutdown =
        reinterpret_cast<PFN_ShutdownInternal>(GetProcAddress(g_s.dll, "VR_ShutdownInternal"));
    g_s.hmd_present =
        reinterpret_cast<PFN_IsHmdPresent>(GetProcAddress(g_s.dll, "VR_IsHmdPresent"));
    g_s.get_interface = reinterpret_cast<PFN_GetGenericInterface>(
        GetProcAddress(g_s.dll, "VR_GetGenericInterface"));
  }
  if (!g_s.init || !g_s.get_interface) {
    REXLOG_WARN("vr: openvr_api.dll is missing the VR_* entry points - running flat");
    return false;
  }
  if (g_s.hmd_present && !g_s.hmd_present()) {
    REXLOG_WARN("vr: no headset present - running flat");
    return false;
  }
  EVRInitError err = EVRInitError_VRInitError_None;
  g_s.init(&err, EVRApplicationType_VRApplication_Scene);
  if (err != EVRInitError_VRInitError_None) {
    REXLOG_WARN("vr: VR_InitInternal failed ({}) - running flat", int(err));
    return false;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "FnTable:%s", IVRSystem_Version);
  g_s.sys = reinterpret_cast<VR_IVRSystem_FnTable*>(g_s.get_interface(name, &err));
  std::snprintf(name, sizeof(name), "FnTable:%s", IVRCompositor_Version);
  g_s.comp = reinterpret_cast<VR_IVRCompositor_FnTable*>(g_s.get_interface(name, &err));
  if (!g_s.sys || !g_s.comp) {
    REXLOG_WARN("vr: no IVRSystem/IVRCompositor ({} / {}) - running flat", IVRSystem_Version,
                IVRCompositor_Version);
    if (g_s.shutdown) g_s.shutdown();
    g_s.sys = nullptr;
    g_s.comp = nullptr;
    return false;
  }
  uint32_t rw = 0, rh = 0;
  g_s.sys->GetRecommendedRenderTargetSize(&rw, &rh);
  if (rw == 0 || rh == 0) {
    rw = 2016;
    rh = 2240;
  }
  const double scale = REXCVAR_GET(skate3_vr_render_scale);
  g_base_eye_w = rw;
  g_base_eye_h = rh;
  g_eye_w.store(ScaledEyeDim(rw), std::memory_order_relaxed);
  g_eye_h.store(ScaledEyeDim(rh), std::memory_order_relaxed);
  g_have_ref = false;
  g_have_head = false;
  g_frame_open = false;
  g_on.store(true, std::memory_order_release);
  REXLOG_INFO("vr: SteamVR session up - per eye {}x{} (recommended {}x{}, scale {:.2f})",
              g_eye_w.load(), g_eye_h.load(), rw, rh, scale);
  BuildHiddenArea();
  return true;
}

void StopSession() {
  if (!g_on.exchange(false)) {
    return;
  }
  g_menu_shown = false;
  if (g_input != nullptr) {
    g_input->SetIgnoreFocus(false);
  }
  if (g_s.shutdown && !g_s.simulated) g_s.shutdown();
  g_s.simulated = false;
  g_s.sys = nullptr;
  g_s.comp = nullptr;
  g_targets_release = true;
  g_submit = {};
  g_frame_open = false;
  g_have_head = false;
  g_have_ref = false;
  {
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    g_pad = {};
  }
  REXLOG_INFO("vr: SteamVR session closed");
}

void ReleaseTargets(nrhi::Device* device) {
  for (nrhi::Texture*& t : g_targets.tex) {
    if (t && device) device->DestroyDeferred(t);
    t = nullptr;
  }
  g_targets.w = g_targets.h = 0;
}

// Yaw-only pose of `head`: forward projected onto the floor, world up kept.
Pose YawPose(const Pose& head) {
  const double fx = -head.R[0][2], fz = -head.R[2][2];
  const double fl = std::sqrt(fx * fx + fz * fz);
  const double f[3] = {fl > 1e-6 ? fx / fl : 0.0, 0.0, fl > 1e-6 ? fz / fl : -1.0};
  // Columns: right = forward x up = (-f.z, 0, f.x), up, back = -forward.
  const double right[3] = {-f[2], 0.0, f[0]};
  Pose p;
  for (int i = 0; i < 3; ++i) {
    p.R[i][0] = right[i];
    p.R[i][1] = i == 1 ? 1.0 : 0.0;
    p.R[i][2] = -f[i];
    p.t[i] = head.t[i];
  }
  return p;
}

void Recenter() {
  g_ref = YawPose(g_head);
  g_have_ref = true;
  REXLOG_INFO("vr: recentered at ({:.2f}, {:.2f}, {:.2f})", g_ref.t[0], g_ref.t[1], g_ref.t[2]);
}

void UpdateEyes() {
  for (int e = 0; e < 2; ++e) {
    EyeOptics& o = g_optics[e];
    float l = -1, r = 1, top = -1, bottom = 1;
    if (g_s.simulated) {
      // Quest 2-like canted-out frusta, 64 mm IPD.
      o.eye_to_head = Pose{};
      o.eye_to_head.t[0] = e == 0 ? -0.032 : 0.032;
      l = e == 0 ? -1.39f : -1.15f;
      r = e == 0 ? 1.15f : 1.39f;
      top = -1.28f;
      bottom = 1.25f;
    } else {
      o.eye_to_head = FromHmd(g_s.sys->GetEyeToHeadTransform(EVREye(e)));
      g_s.sys->GetProjectionRaw(EVREye(e), &l, &r, &top, &bottom);
    }
    o.l = l;
    o.r = r;
    o.u = bottom;
    o.d = top;
    g_eye_world[e] = Compose(g_head, o.eye_to_head);
    if (g_have_ref) {
      Pose head = g_head;
      if (!REXCVAR_GET(skate3_vr_positional)) {
        for (int i = 0; i < 3; ++i) head.t[i] = g_ref.t[i];
      }
      g_eye_rel[e] = Compose(Inverse(g_ref), Compose(head, o.eye_to_head));
    }
  }
}

// ---- controllers -------------------------------------------------------------

struct HandInput {
  bool have = false;
  float sx = 0, sy = 0, trig = 0;
  bool a = false, b = false, grip = false, click = false;
};

struct DeviceAxes {
  bool known = false;
  int stick = 0;    // axis carrying the thumbstick (or trackpad)
  int trigger = 1;  // analog trigger axis
  bool index = false;  // Valve Index ("knuckles"): A/B arrive as grip/menu bits
};
DeviceAxes g_axes[k_unMaxTrackedDeviceCount];

const DeviceAxes& AxesFor(TrackedDeviceIndex_t i) {
  DeviceAxes& a = g_axes[i];
  if (a.known) {
    return a;
  }
  a.known = true;
  int pad = -1, stick = -1, trig = -1;
  for (int k = 0; k < 5; ++k) {
    ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
    const int32_t type = g_s.sys->GetInt32TrackedDeviceProperty(
        i, ETrackedDeviceProperty(ETrackedDeviceProperty_Prop_Axis0Type_Int32 + k), &perr);
    if (perr != ETrackedPropertyError_TrackedProp_Success) continue;
    if (type == EVRControllerAxisType_k_eControllerAxis_Joystick && stick < 0) stick = k;
    if (type == EVRControllerAxisType_k_eControllerAxis_TrackPad && pad < 0) pad = k;
    if (type == EVRControllerAxisType_k_eControllerAxis_Trigger && trig < 0) trig = k;
  }
  a.stick = stick >= 0 ? stick : (pad >= 0 ? pad : 0);
  a.trigger = trig >= 0 ? trig : 1;
  char type[64] = {};
  ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
  g_s.sys->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_ControllerType_String,
                                          type, sizeof(type), &perr);
  a.index = std::strstr(type, "knuckles") != nullptr;
  REXLOG_INFO("vr: controller {} type '{}' stick axis {} trigger axis {}", i, type, a.stick,
              a.trigger);
  return a;
}

// Command-processor thread, once per swap: the controller chord's toggle
// and the menu state for this frame.
void UpdateMenu() {
  if (g_menu_toggle_request.exchange(false) && g_menu_toggle) {
    g_menu_toggle();
  }
  const bool visible = g_menu_visible && g_menu_visible();
  if (visible != g_menu_shown) {
    g_menu_shown = visible;
    if (visible) {
      g_menu_ref = g_have_head ? YawPose(g_head) : g_ref;  // open where the head faces
    }
    REXLOG_INFO("vr: settings menu {} in the headset", visible ? "shown" : "hidden");
  }
}

void PollGamepadViewToggle() {
  static ViewToggleChord chord;
  rex::input::X_INPUT_GAMEPAD pad{};
  const bool connected = g_input && g_input->GetUiGamepadState(&pad);
  const uint16_t buttons = pad.buttons;
  if (chord.Update(connected,
      (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) != 0,
      (buttons & rex::input::X_INPUT_GAMEPAD_X) != 0)) {
    const bool enabled = !REXCVAR_GET(skate3_vr_first_person);
    REXCVAR_SET(skate3_vr_first_person, enabled);
    REXLOG_INFO("vr: first person {} (gamepad L3 + X)", enabled ? "enabled" : "disabled");
  }
}

void PollControllers() {
  HandInput hands[2];  // 0 = left, 1 = right
  const float rumble = BitsFloat(g_rumble_bits.load(std::memory_order_relaxed));
  for (TrackedDeviceIndex_t i = 1; i < k_unMaxTrackedDeviceCount; ++i) {
    if (g_s.sys->GetTrackedDeviceClass(i) !=
        ETrackedDeviceClass_TrackedDeviceClass_Controller) {
      g_axes[i].known = false;
      continue;
    }
    VRControllerState_t st;
    if (!g_s.sys->GetControllerState(i, &st, sizeof(st))) {
      continue;
    }
    const ETrackedControllerRole role = g_s.sys->GetControllerRoleForTrackedDeviceIndex(i);
    int h = role == ETrackedControllerRole_TrackedControllerRole_LeftHand ? 0 : 1;
    if (hands[h].have) {
      h = 1 - h;  // two unassigned controllers: second one is the other hand
      if (hands[h].have) continue;
    }
    const DeviceAxes& ax = AxesFor(i);
    const uint64_t b = st.ulButtonPressed;
    const auto bit = [b](int n) { return ((b >> n) & 1ull) != 0; };
    HandInput& in = hands[h];
    in.have = true;
    in.sx = st.rAxis[ax.stick].x;
    in.sy = st.rAxis[ax.stick].y;
    in.trig = st.rAxis[ax.trigger].x;
    in.click = bit(32 + ax.stick);
    in.b = bit(1);  // ApplicationMenu: B / Y (Touch, Index), menu (Vive)
    if (ax.index) {
      in.a = bit(2);  // Index A arrives on the legacy grip bit
      in.grip = st.rAxis[2].x > 0.7f;
    } else {
      in.a = bit(7);
      in.grip = bit(2) || st.rAxis[2].x > 0.7f;
    }
    if (rumble > 0.0f) {
      // One-shot of <= 3999 us: retriggered every frame while the pad rumbles.
      g_s.sys->TriggerHapticPulse(i, 0, uint16_t(std::clamp(rumble, 0.0f, 1.0f) * 3999.0f));
    }
  }

  // Map to the pad. Left stick click is a shift: while held, right A / B
  // are Start / Back, the right stick is the D-pad and the right stick click
  // recenters; released without using any of those it sends a short L3.
  static bool shift_used = false, shift_prev = false, r3_prev = false, menu_prev = false;
  static Clock::time_point l3_pulse_until{};
  static skate3::vr::ViewToggleChord view_toggle;
  const Clock::time_point now = Clock::now();
  PadState pad;
  pad.connected = (hands[0].have || hands[1].have) && REXCVAR_GET(skate3_vr_controllers);
  if (pad.connected) {
    const auto axis = [](float v) {
      return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    };
    const auto trig = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    const HandInput& L = hands[0];
    const HandInput& R = hands[1];
    const bool shift = L.have && L.click;
    uint16_t btn = 0;
    pad.lx = axis(L.sx);
    pad.ly = axis(L.sy);
    pad.lt = trig(L.trig);
    const bool first_person_chord = shift && L.a;
    if (view_toggle.Update(true, shift, L.a)) {
      const bool enabled = !REXCVAR_GET(skate3_vr_first_person);
      REXCVAR_SET(skate3_vr_first_person, enabled);
      REXLOG_INFO("vr: first person {} (left stick + X)", enabled ? "enabled" : "disabled");
    }
    if (first_person_chord) shift_used = true;
    if (L.a && !shift && !view_toggle.ConsumeFace(L.a)) btn |= rex::input::X_INPUT_GAMEPAD_X;
    // Left stick click + Y: the settings menu (shown on its overlay).
    const bool menu_chord = shift && L.b;
    if (menu_chord && !menu_prev) {
      g_menu_toggle_request.store(true, std::memory_order_relaxed);
    }
    if (menu_chord) shift_used = true;
    menu_prev = L.b && (shift || menu_prev);  // hold Y past the shift release
    if (L.b && !menu_prev) btn |= rex::input::X_INPUT_GAMEPAD_Y;
    if (L.grip) btn |= rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
    pad.rt = trig(R.trig);
    if (R.grip) btn |= rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (shift) {
      if (R.a) btn |= rex::input::X_INPUT_GAMEPAD_START;
      if (R.b) btn |= rex::input::X_INPUT_GAMEPAD_BACK;
      if (R.sy > 0.6f) btn |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
      if (R.sy < -0.6f) btn |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
      if (R.sx < -0.6f) btn |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
      if (R.sx > 0.6f) btn |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
      if (R.click && !r3_prev) {
        g_recenter_request.store(true, std::memory_order_relaxed);
      }
      if ((btn & 0x003Fu) != 0 || R.click) shift_used = true;
    } else {
      pad.rx = axis(R.sx);
      pad.ry = axis(R.sy);
      if (R.a) btn |= rex::input::X_INPUT_GAMEPAD_A;
      if (R.b) btn |= rex::input::X_INPUT_GAMEPAD_B;
      if (R.click) btn |= rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
      if (shift_prev && !shift_used) l3_pulse_until = now + std::chrono::milliseconds(120);
      shift_used = false;
    }
    if (now < l3_pulse_until) btn |= rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
    pad.buttons = btn;
    shift_prev = shift;
    r3_prev = R.click;
  } else {
    view_toggle.Update(false, false, false);
    shift_used = shift_prev = r3_prev = menu_prev = false;
  }
  std::lock_guard<std::mutex> lock(g_pad_mutex);
  if (std::memcmp(&pad, &g_pad, sizeof(pad)) != 0) {
    ++g_pad_packet;
  }
  g_pad = pad;
}

class VrPadDriver final : public rex::input::InputDriver {
 public:
  explicit VrPadDriver(rex::ui::Window* window) : InputDriver(window, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                           rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0 || !Connected()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, rex::input::X_INPUT_STATE* out_state) override {
    if (user_index != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    if (!g_pad.connected) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_state) {
      out_state->packet_number = g_pad_packet;
      out_state->gamepad.buttons = g_pad.buttons;
      out_state->gamepad.left_trigger = g_pad.lt;
      out_state->gamepad.right_trigger = g_pad.rt;
      out_state->gamepad.thumb_lx = g_pad.lx;
      out_state->gamepad.thumb_ly = g_pad.ly;
      out_state->gamepad.thumb_rx = g_pad.rx;
      out_state->gamepad.thumb_ry = g_pad.ry;
    }
    return X_ERROR_SUCCESS;
  }

  // Records the rumble for the controllers and declines, so the next driver
  // (a regular pad) still gets it.
  X_RESULT SetState(uint32_t user_index, rex::input::X_INPUT_VIBRATION* vibration) override {
    if (user_index != 0 || vibration == nullptr) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    const uint16_t lo = vibration->left_motor_speed;
    const uint16_t hi = vibration->right_motor_speed;
    g_rumble_bits.store(FloatBits(float(std::max(lo, hi)) / 65535.0f),
                        std::memory_order_relaxed);
    return Connected() ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetKeystroke(uint32_t, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static bool Connected() {
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    return g_pad.connected;
  }
};

void OnSubmitted(NativeGuestOutputBackend backend, void* /*user_data*/) {
  const FrameTextures f = g_submit;
  g_submit = {};
  g_frame_open = false;
  if (g_s.simulated && f.valid) {
    const double final_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - g_sim_frame_begin).count();
    g_sim_final_sum += final_ms;
    g_sim_first_sum += g_sim_first_submit_ms >= 0.0 ? g_sim_first_submit_ms : final_ms;
  }
  if (g_s.simulated && f.valid && f.queue != nullptr) {
    // A fence right behind the frame's work on the queue: what the
    // compositor waits on before it can show the frame.
    auto* queue = static_cast<ID3D12CommandQueue*>(f.queue);
    if (g_sim_fence == nullptr) {
      ID3D12Device* dev = nullptr;
      if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_sim_fence));
        dev->Release();
      }
      g_sim_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    if (g_sim_fence != nullptr && SUCCEEDED(queue->Signal(g_sim_fence, ++g_sim_fence_value))) {
      g_sim_wait_submission = g_sim_fence_value;
    }
  }
  if (!g_on.load(std::memory_order_acquire) || backend != NativeGuestOutputBackend::kD3D12 ||
      !f.valid || f.queue == nullptr || g_s.simulated) {
    return;
  }
  void* const images[2] = {f.left, f.right};
  for (int e = 0; e < 2; ++e) {
    VRTextureBounds_t bounds = {0.0f, 0.0f, 1.0f, 1.0f};
    if (f.side_by_side) {
      bounds.uMin = e == 0 ? 0.0f : 0.5f;
      bounds.uMax = e == 0 ? 0.5f : 1.0f;
    }
    D3D12TextureData_t data;
    data.m_pResource = static_cast<struct ID3D12Resource*>(images[e]);
    if (e == 0 && data.m_pResource) {
      static bool logged = false;
      if (!logged) {
          logged = true;
  
          const D3D12_RESOURCE_DESC desc =
              data.m_pResource->GetDesc();
  
          REXLOG_INFO(
              "VR texture: DXGI format={}, width={}, height={}",
              static_cast<int>(desc.Format),
              static_cast<unsigned long long>(desc.Width),
              desc.Height
          );
      }
    }
    data.m_pCommandQueue = static_cast<struct ID3D12CommandQueue*>(f.queue);
    data.m_nNodeMask = 0;
    Texture_t tex;
    tex.handle = &data;
    tex.eType = ETextureType_TextureType_DirectX12;
    tex.eColorSpace = f.linear ? EColorSpace_ColorSpace_Linear : EColorSpace_ColorSpace_Gamma;
    if (e == 0) {
      static int last_format = -1, last_space = -1;
      const auto desc = data.m_pResource->GetDesc();
      if (int(desc.Format) != last_format || int(tex.eColorSpace) != last_space) {
        last_format = int(desc.Format);
        last_space = int(tex.eColorSpace);
        REXLOG_INFO("vr: handoff DXGI={} colorspace={} size={}x{} side_by_side={}",
                    last_format, f.linear ? "Linear" : "Gamma", desc.Width,
                    desc.Height, f.side_by_side);
      }
    }
    const EVRCompositorError err =
        g_s.comp->Submit(EVREye(e), &tex, &bounds, EVRSubmitFlags_Submit_Default);
    if (err != EVRCompositorError_VRCompositorError_None) {
      static int s_logged = 0;
      if (s_logged < 8) {
        ++s_logged;
        REXLOG_WARN("vr: Submit(eye {}) failed ({})", e, int(err));
      }
    }
  }
}

double Sign(double v) { return v < 0.0 ? -1.0 : 1.0; }

}  // namespace

void Install(rex::input::InputSystem* input) {
  g_input = input;
  rex::graphics::SetNativeGuestOutputSubmitHook(&OnSubmitted, nullptr);
  if (input != nullptr) {
    input->PrependDriver(std::make_unique<VrPadDriver>(input->window()));
  }
  REXLOG_INFO("vr: installed (skate3_vr={}; needs SteamVR + Direct3D 12)",
              REXCVAR_GET(skate3_vr));
}

void SetMenuHooks(std::function<bool()> visible, std::function<void()> toggle) {
  g_menu_visible = std::move(visible);
  g_menu_toggle = std::move(toggle);
}

bool MenuVisible() { return Active() && g_menu_shown; }

void SetMenuRect(float x, float y, float w, float h) {
  std::lock_guard<std::mutex> lock(g_menu_rect_mutex);
  g_menu_rect[0] = x;
  g_menu_rect[1] = y;
  g_menu_rect[2] = w;
  g_menu_rect[3] = h;
  g_menu_rect_valid = w > 1.0f && h > 1.0f;
}

bool MenuRect(float& x, float& y, float& w, float& h) {
  std::lock_guard<std::mutex> lock(g_menu_rect_mutex);
  x = g_menu_rect[0];
  y = g_menu_rect[1];
  w = g_menu_rect[2];
  h = g_menu_rect[3];
  return g_menu_rect_valid;
}

bool Active() { return g_on.load(std::memory_order_acquire); }

bool EyeSize(uint32_t& width, uint32_t& height) {
  if (!Active()) {
    return false;
  }
  width = g_eye_w.load(std::memory_order_relaxed);
  height = g_eye_h.load(std::memory_order_relaxed);
  return width != 0 && height != 0;
}

bool OutputSize(uint32_t& width, uint32_t& height) {
  if (!EyeSize(width, height)) {
    return false;
  }
  if (SinglePass()) {
    width *= 2;
  }
  return true;
}

bool SinglePass() { return REXCVAR_GET(skate3_vr_single_pass); }

bool WideCull() { return Active() && REXCVAR_GET(skate3_vr_wide_cull); }
double GuestAspect() { return REXCVAR_GET(skate3_vr_guest_aspect); }
double GuestFovDegrees() { return REXCVAR_GET(skate3_vr_guest_fov); }
int32_t MaxMsaa() { return REXCVAR_GET(skate3_vr_msaa); }
bool FlushEyes() { return REXCVAR_GET(skate3_vr_flush_eyes); }
const std::vector<float>& HiddenAreaTriangles(int eye) {
  static const std::vector<float> kNone;
  if (eye < 0 || eye > 1 || !Active() || !REXCVAR_GET(skate3_vr_hidden_area)) {
    return kNone;
  }
  return g_hidden_area[eye];
}
void NoteMidFrameSubmit() {
  if (g_s.simulated && g_sim_first_submit_ms < 0.0) {
    g_sim_first_submit_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - g_sim_frame_begin).count();
  }
}

bool BeginFrame(const NativeGuestOutputRenderContext& context) {
  if (g_targets_release && context.device != nullptr) {
    ReleaseTargets(context.device);
    g_targets_release = false;
  }
  if (!REXCVAR_GET(skate3_vr)) {
    StopSession();
    g_s.tried = false;
    return false;
  }
  if (context.backend != NativeGuestOutputBackend::kD3D12) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      REXLOG_WARN("vr: skate3_vr needs the Direct3D 12 backend (gpu_backend=d3d12) - running flat");
    }
    return false;
  }
  if (!Active()) {
    if (g_s.tried) {
      return false;
    }
    g_s.tried = true;
    if (!StartSession()) {
      return false;
    }
    if (g_input != nullptr && !g_s.simulated) {
      g_input->SetIgnoreFocus(true);
    }
  }
  g_device = context.device;
  // Live render scale (settings menu): the eye targets and the guest output
  // follow the new size on this frame (EyeTexture / OutputSize).
  if (g_base_eye_w != 0 && g_base_eye_h != 0) {
    const uint32_t w = ScaledEyeDim(g_base_eye_w);
    const uint32_t h = ScaledEyeDim(g_base_eye_h);
    if (w != g_eye_w.load(std::memory_order_relaxed) ||
        h != g_eye_h.load(std::memory_order_relaxed)) {
      g_eye_w.store(w, std::memory_order_relaxed);
      g_eye_h.store(h, std::memory_order_relaxed);
      REXLOG_INFO("vr: render scale {:.2f} - per eye {}x{}", REXCVAR_GET(skate3_vr_render_scale),
                  w, h);
    }
  }
  const Clock::time_point now = Clock::now();
  {
    static Clock::time_point s_session_start{};
    static bool s_captured = false;
    if (s_session_start == Clock::time_point{}) s_session_start = now;
    const std::string path(REXCVAR_GET(skate3_vr_perf_capture));
    if (!s_captured && !path.empty() &&
        now - s_session_start > std::chrono::seconds(REXCVAR_GET(skate3_vr_perf_capture_delay))) {
      s_captured = true;
      const bool ok = rex::perf::StartCapture(path, path + ".fingerprints.csv");
      REXLOG_INFO("vr: perf capture {} -> {}", ok ? "started" : "REFUSED", path);
    }
  }
  // Once per swap: a yielded native frame and the emulated post-process of
  // the same swap both land here. The age check recovers a swap that never
  // reached the submit hook.
  if (g_s.simulated) {
    if (!g_frame_open || now - g_frame_wait_time > std::chrono::milliseconds(100)) {
      if (g_sim_wait_submission != 0 && g_sim_fence != nullptr) {
        if (g_sim_fence->GetCompletedValue() < g_sim_wait_submission &&
            SUCCEEDED(g_sim_fence->SetEventOnCompletion(g_sim_wait_submission,
                                                        g_sim_fence_event))) {
          WaitForSingleObject(g_sim_fence_event, 100);
        }
        g_sim_latency_sum +=
            std::chrono::duration<double, std::milli>(Clock::now() - g_sim_frame_begin).count();
        if (++g_sim_latency_n == 300) {
          REXLOG_INFO(
              "vr-sim: frame begin -> GPU done avg {:.2f} ms (first submit {:.2f}, final "
              "submit {:.2f}; flush_eyes={} single_pass={})",
              g_sim_latency_sum / g_sim_latency_n, g_sim_first_sum / g_sim_latency_n,
              g_sim_final_sum / g_sim_latency_n, REXCVAR_GET(skate3_vr_flush_eyes),
              REXCVAR_GET(skate3_vr_single_pass));
          g_sim_latency_sum = 0.0;
          g_sim_first_sum = 0.0;
          g_sim_final_sum = 0.0;
          g_sim_latency_n = 0;
        }
        g_sim_wait_submission = 0;
      }
      g_sim_frame_begin = Clock::now();
      g_sim_first_submit_ms = -1.0;
      g_frame_open = true;
      g_frame_wait_time = now;
      g_submit = {};
      g_head = Pose{};
      g_head.t[1] = 1.2;  // standing, looking straight ahead
      g_have_head = true;
      if (!g_have_ref) Recenter();
      UpdateEyes();
      PollGamepadViewToggle();
      UpdateMenu();
    }
    return true;
  }
  if (!g_frame_open || now - g_frame_wait_time > std::chrono::milliseconds(100)) {
    TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    g_s.comp->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
    g_frame_open = true;
    g_frame_wait_time = now;
    g_submit = {};
    if (poses[0].bPoseIsValid) {
      g_head = FromHmd(poses[0].mDeviceToAbsoluteTracking);
      g_have_head = true;
      if (!g_have_ref || g_recenter_request.exchange(false)) {
        Recenter();
      }
    }
    if (g_have_head) {
      UpdateEyes();
    }
    PollControllers();
    PollGamepadViewToggle();
    UpdateMenu();
  }
  return g_have_head && g_have_ref;
}

int CurrentEye() { return g_current_eye; }
void SetCurrentEye(int eye) { g_current_eye = eye; }

// First-person heading = the skater's chest direction (presentation root
// row 0: stable, not animated) plus a neck angle (UpdateFirstPersonHeading).
// The neck aims at the board end that points forward (the travel direction
// while the board is off the travel line mid-spin), within what a neck can
// turn (skate3_vr_fp_neck_limit) and at a neck's speed, moving only through
// the front: changing shoulders after a 180 swings the view across the
// chest, never around the back, and a body spinning past the limit takes
// the view with it. While riding the board is square to the chest, so the
// neck angle is constant and the view turns exactly with the skater.
struct FirstPersonHeading {
  bool valid = false;
  double neck = 0.0;         // camera yaw - chest yaw, within the neck limit
  double end = 0.0;          // forward board end: 0 = nose (row 2), pi = tail
  double backwards_s = 0.0;  // time the gaze has disagreed (standing still)
  Clock::time_point last{};
  // Travel: head position sampled every ~0.15 s.
  double anchor_pos[2] = {};
  Clock::time_point anchor_time{};
  bool have_travel = false;
  double travel = 0.0;       // yaw of the motion
};
FirstPersonHeading g_fp_heading;

double WrapAngle(double a) {
  constexpr double kPi = 3.14159265358979323846;
  a = std::fmod(a + kPi, 2.0 * kPi);
  if (a < 0.0) a += 2.0 * kPi;
  return a - kPi;
}

// Camera yaw (atan2(x, z) on the floor) from the board and chest yaws, the
// gaze and the head position (world units; ~10 per second while pushing).
double UpdateFirstPersonHeading(double board, double chest, bool have_face, double face,
                                double head_x, double head_z) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kOffLine = 0.7071;   // |sin| of the board vs travel angle: 45 deg
  constexpr double kMinSpeed = 1.5;     // units/s for the travel direction to count
  constexpr double kGazeHold = 0.3;     // s of disagreeing gaze (standing) before turning
  const double limit =
      std::clamp(REXCVAR_GET(skate3_vr_fp_neck_limit), 90.0, 170.0) * (kPi / 180.0);
  const double speed =
      std::clamp(REXCVAR_GET(skate3_vr_fp_neck_speed), 90.0, 2000.0) * (kPi / 180.0);
  FirstPersonHeading& h = g_fp_heading;
  const Clock::time_point now = Clock::now();
  const double dt = h.valid ? std::chrono::duration<double>(now - h.last).count() : 1.0;
  h.last = now;
  const bool restart = !h.valid || dt > 0.25;
  if (restart) {
    // (Re)start: face the end of the board the skater looks toward.
    h = FirstPersonHeading{};
    h.valid = true;
    h.last = h.anchor_time = now;
    h.anchor_pos[0] = head_x;
    h.anchor_pos[1] = head_z;
    h.end = have_face && std::cos(face - board) < 0.0 ? kPi : 0.0;
  }
  const double step = std::clamp(dt, 1e-4, 0.1);

  // Direction of travel over the last ~0.15 s.
  const double since_anchor = std::chrono::duration<double>(now - h.anchor_time).count();
  if (since_anchor >= 0.15) {
    const double vx = (head_x - h.anchor_pos[0]) / since_anchor;
    const double vz = (head_z - h.anchor_pos[1]) / since_anchor;
    h.have_travel = since_anchor < 0.5 && std::sqrt(vx * vx + vz * vz) > kMinSpeed;
    if (h.have_travel) h.travel = std::atan2(vx, vz);
    h.anchor_pos[0] = head_x;
    h.anchor_pos[1] = head_z;
    h.anchor_time = now;
  }

  // Where the head wants to look.
  double desired;
  if (h.have_travel && std::fabs(std::sin(board - h.travel)) > kOffLine) {
    desired = h.travel;  // mid-spin / sliding: keep looking down the path
    h.backwards_s = 0.0;
  } else {
    if (h.have_travel) {
      h.end = std::cos(board - h.travel) >= 0.0 ? 0.0 : kPi;
      h.backwards_s = 0.0;
    } else if (have_face && std::cos(face - (board + h.end)) < -0.25) {
      h.backwards_s += step;  // standing still: the gaze picks the end
      if (h.backwards_s >= kGazeHold) {
        h.end = kPi - h.end;
        h.backwards_s = 0.0;
      }
    } else {
      h.backwards_s = 0.0;
    }
    desired = board + h.end;
  }
  // The neck angle for it, within the limit; moved linearly inside
  // [-limit, limit] (not wrapped), so it always passes the front.
  const double goal = std::clamp(WrapAngle(desired - chest), -limit, limit);
  if (restart) {
    h.neck = goal;
  } else {
    const double turn = goal - h.neck;
    if (std::fabs(turn) > 0.5 * kPi && std::fabs(h.neck) > 0.25 * kPi) {
      static Clock::time_point s_logged{};
      if (now > s_logged) {
        s_logged = now + std::chrono::seconds(1);
        REXLOG_INFO("vr: first person looks over the other shoulder ({:.0f} -> {:.0f} deg)",
                    h.neck * (180.0 / kPi), goal * (180.0 / kPi));
      }
    }
    const double max_step = speed * step;
    h.neck += std::clamp(turn, -max_step, max_step);
  }
  return chest + h.neck;
}

void SetFirstPersonAnchor(const float* head, const float* forward, const float* chest,
                          const float* face) {
  // Latch the mode once for the pair, even if a hot-reload happens mid-frame.
  if (g_current_eye != 1) g_first_person_frame = REXCVAR_GET(skate3_vr_first_person);
  static bool waiting = false;
  const bool missing = g_first_person_frame && head == nullptr;
  if (missing && !waiting) REXLOG_INFO("vr: first person requested; waiting for local skater head pose");
  waiting = missing;
  const bool was_active = g_first_person_scene;
  g_first_person_scene = head != nullptr && g_first_person_frame;
  if (was_active != g_first_person_scene) {
    REXLOG_INFO("vr: first-person anchor {}", g_first_person_scene ? "ready" : "inactive (third-person/menu fallback)");
  }
  if (g_first_person_scene) {
    std::memcpy(g_first_person_head, head, sizeof(g_first_person_head));
    g_first_person_head[1] += float(std::clamp(REXCVAR_GET(skate3_vr_eye_vertical_offset), -0.2, 0.2));
    // The skater's heading from the presentation root (not a bone: see
    // GetPlayerPose), projected onto the floor: chest + neck
    // (UpdateFirstPersonHeading). Latched once for the stereo pair;
    // ComputeEyeCamera still applies each live HMD pose on top.
    if (g_current_eye != 1) {
      const double fx = forward[0], fz = forward[2];
      const double cx = chest ? chest[0] : 0.0, cz = chest ? chest[2] : 0.0;
      if (std::sqrt(fx * fx + fz * fz) > 1e-4 && std::sqrt(cx * cx + cz * cz) > 1e-4) {
        // The gaze looks down at the ground ahead; only a clearly
        // horizontal part of it votes.
        const double gx = face ? face[0] : 0.0, gz = face ? face[2] : 0.0;
        const bool have_face = std::sqrt(gx * gx + gz * gz) > 0.3;
        const double yaw = UpdateFirstPersonHeading(
            std::atan2(fx, fz), std::atan2(cx, cz), have_face,
            have_face ? std::atan2(gx, gz) : 0.0, head[0], head[2]);
        g_first_person_forward[0] = float(std::sin(yaw));
        g_first_person_forward[1] = 0.0f;
        g_first_person_forward[2] = float(std::cos(yaw));
      }
    }
  } else {
    g_fp_heading.valid = false;
  }
}
bool FirstPersonActive() { return Active() && g_first_person_scene; }
const float* FirstPersonHead() { return g_first_person_head; }

bool ComputeEyeCamera(int eye, const float game_view_proj[16], const float game_proj[16],
                      EyeCamera* out) {
  if (eye < 0 || eye > 1 || !g_have_ref) {
    return false;
  }
  double vp[16], pr[16], pr_inv[16];
  ToDouble(game_view_proj, vp);
  ToDouble(game_proj, pr);
  if (!Invert4(pr, pr_inv)) {
    return false;
  }
  std::memcpy(g_game_vp, vp, sizeof(vp));
  double view[16];
  Mul4(vp, pr_inv, view);  // V = VP * P^-1

  const double sx = Sign(pr[0]);  // screen right = sx * camera x
  const double sy = Sign(pr[5]);  // screen up    = sy * camera y
  std::memcpy(g_game_view, view, sizeof(view));
  g_game_sx = sx;
  g_game_sy = sy;
  g_game_view_valid = true;

  if (FirstPersonActive()) {
    // Upright yaw frame: animation pitch/roll and chase-camera translation
    // never move the horizon. Body yaw supplies heading; HMD supplies look.
    double fx=g_first_person_forward[0], fz=g_first_person_forward[2];
    double len=std::hypot(fx,fz);
    if (len < 1e-6) { fx=0; fz=1; len=1; }
    fx/=len; fz/=len;
    // Preserve handedness independently of the angle between the body
    // and chase camera; their dot product can change sign on a 180 turn.
    const double det = view[0]*(view[5]*view[10]-view[6]*view[9]) -
                       view[1]*(view[4]*view[10]-view[6]*view[8]) +
                       view[2]*(view[4]*view[9]-view[5]*view[8]);
    const double right_sign = Sign(det)*sy;
    std::fill(std::begin(view),std::end(view),0.0);
    view[0]=right_sign*fz; view[8]=-right_sign*fx;
    view[5]=sy; view[2]=fx; view[10]=fz; view[15]=1.0;
    for (int j=0;j<3;++j) for (int k=0;k<3;++k)
      view[12+j] -= double(g_first_person_head[k])*view[k*4+j];
  } else if (REXCVAR_GET(skate3_vr_level_horizon)) {
    // World up (+Y) as seen in camera space is the view's row 1; rotate the
    // camera space by the shortest rotation taking it to screen-up. Removes
    // pitch and roll, keeps the heading and the camera position.
    double a[3] = {view[4], view[5], view[6]};
    const double al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (al > 1e-6) {
      for (double& c : a) c /= al;
      const double b[3] = {0.0, sy, 0.0};
      const double k[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                           a[0] * b[1] - a[1] * b[0]};
      const double s2 = k[0] * k[0] + k[1] * k[1] + k[2] * k[2];
      const double c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      if (s2 > 1e-12 && c > 0.0) {
        // Rodrigues, column form R a = b: R = I + K + K^2 (1 - c) / s^2.
        const double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}};
        double R[3][3];
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            double kk = 0.0;
            for (int m = 0; m < 3; ++m) kk += K[i][m] * K[m][j];
            R[i][j] = (i == j ? 1.0 : 0.0) + K[i][j] + kk * (1.0 - c) / s2;
          }
        }
        // Row-vector form v' = v * R^T.
        double Q[16] = {};
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) Q[i * 4 + j] = R[j][i];
        }
        Q[15] = 1.0;
        Mul4(view, Q, view);
      }
    }
  }

  // Eye relative to the reference, mapped into camera space:
  // Rc = C Rrel C, pc = C trel * scale, C = diag(sx, sy, -1).
  const Pose& rel = g_eye_rel[eye];
  const double C[3] = {sx, sy, -1.0};
  const double scale = REXCVAR_GET(skate3_vr_world_scale);
  double Rc[3][3], pc[3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) Rc[i][j] = C[i] * rel.R[i][j] * C[j];
    pc[i] = C[i] * rel.t[i] * scale;
  }
  // Camera space -> eye space (row form): v_eye = (v_cam - pc) * Rc.
  double T[16] = {};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) T[i * 4 + j] = Rc[i][j];
  }
  for (int j = 0; j < 3; ++j) {
    T[12 + j] = -(pc[0] * Rc[0][j] + pc[1] * Rc[1][j] + pc[2] * Rc[2][j]);
  }
  T[15] = 1.0;
  double eye_view[16];
  Mul4(view, T, eye_view);

  // Headset frustum in the game's conventions (axis signs, depth rows).
  const EyeOptics& o = g_optics[eye];
  double P[16] = {};
  // g_zoom scales the tangents about the lens axis (the off-centre terms
  // stay), like the game camera narrowing its field of view.
  P[0] = 2.0 * sx / (o.r - o.l) * g_zoom;
  P[8] = -(o.r + o.l) / (o.r - o.l);
  P[5] = 2.0 * sy / (o.u - o.d) * g_zoom;
  P[9] = -(o.u + o.d) / (o.u - o.d);
  P[10] = pr[10];
  P[11] = pr[11];
  P[14] = pr[14];
  P[15] = pr[15];
  if (FirstPersonActive() && std::fabs(pr[11]-1.0) < 1e-4 && std::fabs(pr[15]) < 1e-4) {
    // Standard D3D perspective: preserve far plane, use a 5 cm near plane
    // for the body/board. No game-camera FOV zoom in first person.
    const double far_z = pr[10] > 1.000001 ? -pr[14]/(pr[10]-1.0) : 10000.0;
    const double near_z = 0.05*std::clamp(scale,0.01,100.0);
    if (far_z > near_z) {
      P[10]=far_z/(far_z-near_z); P[14]=-near_z*P[10];
    }
  }

  double eye_vp[16], inv_view[16];
  Mul4(eye_view, P, eye_vp);
  if (!Invert4(eye_view, inv_view)) {
    return false;
  }
  ToFloat(eye_vp, out->view_proj);
  ToFloat(P, out->proj);
  for (int i = 0; i < 3; ++i) out->cam_pos[i] = float(inv_view[12 + i]);
  std::memcpy(g_eye_vp[eye], eye_vp, sizeof(eye_vp));
  ToFloat(eye_vp, g_eye_vp_f[eye]);
  g_eye_vp_valid[eye] = true;
  if (eye == 0) {
    // Eye axes in world space are the eye view's columns; display right /
    // up are sx / sy times camera x / y.
    CullView cv;
    for (int i = 0; i < 3; ++i) {
      cv.right[i] = sx * eye_view[i * 4 + 0];
      cv.up[i] = sy * eye_view[i * 4 + 1];
      cv.fwd[i] = eye_view[i * 4 + 2];
      cv.pos[i] = inv_view[12 + i];
    }
    double original_inv[16];
    if (Invert4(g_game_view, original_inv))
      for (int i=0;i<3;++i) cv.game_pos[i]=original_inv[12+i];
    cv.l = std::min(g_optics[0].l, g_optics[1].l);
    cv.r = std::max(g_optics[0].r, g_optics[1].r);
    cv.d = std::min(g_optics[0].d, g_optics[1].d);
    cv.u = std::max(g_optics[0].u, g_optics[1].u);
    cv.valid = true;
    cv.time = Clock::now();
    std::lock_guard<std::mutex> lock(g_cull_mutex);
    g_cull = cv;
  }
  return true;
}

void SetGameFov(float flat_proj11) {
  double zoom = 1.0;
  if (!FirstPersonActive() && REXCVAR_GET(skate3_vr_follow_zoom) && flat_proj11 > 1e-4f) {
    uint64_t bits = g_fov_ratio_bits.load(std::memory_order_relaxed);
    double ratio = 1.0;  // retail: the VR cull FOV replaced the flat one
    if (bits != 0) std::memcpy(&ratio, &bits, sizeof(ratio));
    const double p11 = double(flat_proj11) * ratio;
    const double base =
        1.0 / std::tan(0.5 * REXCVAR_GET(skate3_vr_zoom_base_fov) * (3.14159265358979323846 / 180.0));
    zoom = std::clamp(p11 / base, REXCVAR_GET(skate3_vr_zoom_min), REXCVAR_GET(skate3_vr_zoom_max));
    static double s_logged = 1.0;
    if (std::fabs(zoom - s_logged) > 0.1 * s_logged) {
      s_logged = zoom;
      REXLOG_INFO("vr: game camera vfov {:.1f} deg -> headset zoom x{:.2f}",
                  2.0 * std::atan(1.0 / p11) * (180.0 / 3.14159265358979323846), zoom);
    }
  }
  // The user's field of view (percent of the headset's own): a wider view is
  // a zoom below 1.
  zoom *= 100.0 / std::clamp(REXCVAR_GET(skate3_vr_fov), 50.0, 150.0);
  g_zoom = std::clamp(zoom, 0.25, std::max(1.0, REXCVAR_GET(skate3_vr_zoom_max)));
}

bool ComputeStereoCamera(const float game_view_proj[16], const float game_proj[16],
                         EyeCamera* left) {
  EyeCamera right;
  if (!ComputeEyeCamera(0, game_view_proj, game_proj, left) ||
      !ComputeEyeCamera(1, game_view_proj, game_proj, &right)) {
    return false;
  }
  // st_d = inverse(VP_left) * VP_right (double: VP spans depth-scale rows).
  double inv_left[16], d[16];
  if (!Invert4(g_eye_vp[0], inv_left)) {
    return false;
  }
  Mul4(inv_left, g_eye_vp[1], d);
  ToFloat(d, g_stereo_d);
  return true;
}

const float* StereoMatrix() { return g_stereo_d; }

const float* EyeViewProj(int eye) {
  return eye >= 0 && eye < 2 && g_eye_vp_valid[eye] ? g_eye_vp_f[eye] : nullptr;
}

bool ApplyGuestCullPlanes(uint8_t* base, uint32_t cam) {
  if (base == nullptr || cam == 0 || !WideCull() ||
      !rex::graphics::IsNativeGuestOutputActive()) {
    return false;
  }
  CullView cv;
  {
    std::lock_guard<std::mutex> lock(g_cull_mutex);
    cv = g_cull;
  }
  if (!cv.valid || Clock::now() - cv.time > std::chrono::milliseconds(250)) {
    return false;
  }
  const auto load_be = [base](uint32_t addr) {
    const uint8_t* p = base + addr;
    const uint32_t u = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    return double(BitsFloat(u));
  };
  const auto store_be = [base](uint32_t addr, double v) {
    const uint32_t u = FloatBits(float(v));
    uint8_t* p = base + addr;
    p[0] = uint8_t(u >> 24);
    p[1] = uint8_t(u >> 16);
    p[2] = uint8_t(u >> 8);
    p[3] = uint8_t(u);
  };
  const double apex[3] = {load_be(cam + 5120), load_be(cam + 5124), load_be(cam + 5128)};
  double d2 = 0.0;
  for (int i = 0; i < 3; ++i) d2 += (apex[i] - cv.game_pos[i]) * (apex[i] - cv.game_pos[i]);
  // The rendered eye trails the guest camera by the smoothing window and
  // sits a head offset away; shadow and reflection cameras are far off.
  const double reach = 6.0 * REXCVAR_GET(skate3_vr_world_scale);
  if (!(d2 < reach * reach)) {
    return false;
  }
  // Both eyes' extents, widened by the margin angle (clamped short of 90).
  const double m = REXCVAR_GET(skate3_vr_cull_margin) * (3.14159265358979323846 / 180.0);
  const auto widen = [](double tangent, double delta) {
    const double lim = 85.0 * (3.14159265358979323846 / 180.0);
    return std::tan(std::clamp(std::atan(tangent) + delta, -lim, lim));
  };
  const double L = widen(cv.l, -m);
  const double R = widen(cv.r, m);
  const double D = widen(cv.d, -m);
  const double U = widen(cv.u, m);
  double n[6][3];
  for (int i = 0; i < 3; ++i) {
    n[0][i] = cv.right[i] - L * cv.fwd[i];  // X >= L * Z
    n[1][i] = R * cv.fwd[i] - cv.right[i];  // X <= R * Z
    n[2][i] = cv.up[i] - D * cv.fwd[i];     // Y >= D * Z
    n[3][i] = U * cv.fwd[i] - cv.up[i];     // Y <= U * Z
    n[4][i] = cv.fwd[i];                    // in front (the depth plane)
    n[5][i] = cv.fwd[i];                    // no far plane: the distance cull stays
  }
  for (int k = 0; k < 6; ++k) {
    const double guest_d = -(n[k][0]*apex[0]+n[k][1]*apex[1]+n[k][2]*apex[2]);
    const double eye_d = -(n[k][0]*cv.pos[0]+n[k][1]*cv.pos[1]+n[k][2]*cv.pos[2]);
    const double d = std::max(guest_d,eye_d) + 0.1*REXCVAR_GET(skate3_vr_world_scale)*std::sqrt(
        n[k][0]*n[k][0]+n[k][1]*n[k][1]+n[k][2]*n[k][2]);
    const uint32_t at = cam + 5152u + uint32_t(k) * 16u;
    store_be(at + 0, n[k][0]);
    store_be(at + 4, n[k][1]);
    store_be(at + 8, n[k][2]);
    store_be(at + 12, d);
  }
  return true;
}

bool HudTransform(float band_x, float band_y, float* sx, float* sy, float* ox, float* oy) {
  const int eye = g_current_eye;
  if (eye < 0 || eye > 1 || !g_have_head) {
    return false;
  }
  const EyeOptics& o = g_optics[eye];
  const double dist = REXCVAR_GET(skate3_vr_hud_distance);
  const double half_w = 0.5 * REXCVAR_GET(skate3_vr_hud_width);
  const double drop = REXCVAR_GET(skate3_vr_hud_drop);
  // Panel centre in head space, then in this eye's space.
  const double c[3] = {0.0, -drop, -dist};
  const Pose inv = Inverse(o.eye_to_head);
  double q[3];
  for (int i = 0; i < 3; ++i) {
    q[i] = inv.R[i][0] * c[0] + inv.R[i][1] * c[1] + inv.R[i][2] * c[2] + inv.t[i];
  }
  const double z = -q[2];
  if (!(z > 0.05)) {
    return false;
  }
  *ox = float((2.0 * q[0] / z - (o.r + o.l)) / (o.r - o.l));
  *oy = float((2.0 * q[1] / z - (o.u + o.d)) / (o.u - o.d));
  *sx = float((2.0 * half_w / z) / (o.r - o.l) / std::max(band_x, 1e-3f));
  *sy = float((2.0 * half_w * (9.0 / 16.0) / z) / (o.u - o.d) / std::max(band_y, 1e-3f));
  return true;
}

bool HudGameCameraMatrix(float band_x, float band_y, float flat_proj11, float m_out[16]) {
  const int eye = g_current_eye;
  if (eye < 0 || eye > 1 || !g_eye_vp_valid[eye] || !g_game_view_valid ||
      FirstPersonActive() || !REXCVAR_GET(skate3_vr_hud_follow_camera) || !(flat_proj11 > 1e-4f)) {
    return false;
  }
  uint64_t bits = g_fov_ratio_bits.load(std::memory_order_relaxed);
  double ratio = 1.0;
  if (bits != 0) std::memcpy(&ratio, &bits, sizeof(ratio));
  const double p11 = double(flat_proj11) * ratio;  // the flat game's |m11|
  const double p00 = p11 * (9.0 / 16.0);           // its 16:9 screen
  const double d = REXCVAR_GET(skate3_vr_hud_distance) * REXCVAR_GET(skate3_vr_world_scale);
  // Band clip -> game camera space: x = x_band / band_x * d / P00 (signed
  // like the camera's own projection), y likewise, z = d.
  double H[16] = {};
  const double hs = REXCVAR_GET(skate3_vr_hud_scale);
  H[0] = hs * d / (g_game_sx * p00 * std::max(double(band_x), 1e-3));
  H[5] = hs * d / (g_game_sy * p11 * std::max(double(band_y), 1e-3));
  H[14] = d;
  H[15] = 1.0;
  double inv_view[16], M[16];
  if (!Invert4(g_game_view, inv_view)) {
    return false;
  }
  Mul4(H, inv_view, M);         // -> world
  Mul4(M, g_eye_vp[eye], M);    // -> this eye's clip
  ToFloat(M, m_out);
  return true;
}

// One axis of the curved HUD: the panel bent around a centre of curvature
// on the view axis at radius d / c (c = 1: on the camera, every element at
// distance d; c -> 0: flat). The image runs uniformly along the arc and its
// edge sits at the flat HUD's edge angle atan(t): the same view is covered,
// the middle comes forward and the sides wrap around the viewer.
struct HudCurveAxis {
  bool flat = true;
  double t = 0.0;  // flat half extent (view tangent)
  double r = 0.0;  // radius
  double edge = 0.0;  // arc half angle
};

HudCurveAxis MakeHudCurveAxis(double c, double t, double d) {
  HudCurveAxis a;
  a.t = t;
  if (!(c > 1e-3)) {
    return a;
  }
  a.flat = false;
  a.r = d / std::min(c, 1.0);
  // Seen angle of the arc point at arc angle th (monotonic, >= th since the
  // camera sits between the centre and the surface): solve for atan(t).
  const double target = std::atan(t);
  double lo = 0.0, hi = target;
  for (int k = 0; k < 48; ++k) {
    const double m = 0.5 * (lo + hi);
    const double seen = std::atan2(a.r * std::sin(m), d - a.r * (1.0 - std::cos(m)));
    (seen < target ? lo : hi) = m;
  }
  a.edge = 0.5 * (lo + hi);
  return a;
}

// s in [-1, 1] along the axis -> lateral offset and depth lost to the bend.
void HudCurvePoint(const HudCurveAxis& a, double s, double d, double* lateral, double* dz) {
  if (a.flat) {
    *lateral = s * a.t * d;
    *dz = 0.0;
    return;
  }
  const double th = s * a.edge;
  *lateral = a.r * std::sin(th);
  *dz = a.r * (1.0 - std::cos(th));
}

bool HudCurveMesh(int eye, float flat_proj11, std::vector<float>& out) {
  out.clear();
  const double cx = REXCVAR_GET(skate3_vr_hud_curve_x);
  const double cy = REXCVAR_GET(skate3_vr_hud_curve_y);
  if (eye < 0 || eye > 1 || !g_eye_vp_valid[eye] || !g_game_view_valid ||
      FirstPersonActive() || !REXCVAR_GET(skate3_vr_hud_follow_camera) ||
      !(cx > 1e-3 || cy > 1e-3) || !(flat_proj11 > 1e-4f)) {
    return false;
  }
  uint64_t bits = g_fov_ratio_bits.load(std::memory_order_relaxed);
  double ratio = 1.0;
  if (bits != 0) std::memcpy(&ratio, &bits, sizeof(ratio));
  const double p11 = double(flat_proj11) * ratio;
  const double hs = REXCVAR_GET(skate3_vr_hud_scale);
  // Half extents of the flat 16:9 HUD as view tangents (HudGameCameraMatrix's
  // extent).
  const double tx = hs * (16.0 / 9.0) / p11;
  const double ty = hs / p11;
  const double d = REXCVAR_GET(skate3_vr_hud_distance) * REXCVAR_GET(skate3_vr_world_scale);
  double inv_view[16], to_clip[16];
  if (!Invert4(g_game_view, inv_view)) {
    return false;
  }
  Mul4(inv_view, g_eye_vp[eye], to_clip);  // game camera space -> this eye's clip
  const HudCurveAxis ax = MakeHudCurveAxis(cx, tx, d);
  const HudCurveAxis ay = MakeHudCurveAxis(cy, ty, d);
  constexpr int kCols = 48, kRows = 27;
  const auto vertex = [&](int i, int j, float* v) {
    const double u = double(i) / kCols * 2.0 - 1.0;  // -1 left .. +1 right
    const double w = 1.0 - double(j) / kRows * 2.0;  // +1 top .. -1 bottom
    // Both bends combined (a bowl for equal curvatures: the HUD's rows and
    // columns visibly arc around the viewer). Display axes (x right, y up,
    // z forward), then camera space with the projection's axis signs.
    double lx, dzx, ly, dzy;
    HudCurvePoint(ax, u, d, &lx, &dzx);
    HudCurvePoint(ay, w, d, &ly, &dzy);
    const double z = d - dzx - dzy;
    const double p[4] = {g_game_sx * lx, g_game_sy * ly, z, 1.0};
    for (int c = 0; c < 4; ++c) {
      v[c] = float(p[0] * to_clip[c] + p[1] * to_clip[4 + c] + p[2] * to_clip[8 + c] +
                   p[3] * to_clip[12 + c]);
    }
    v[4] = float(i) / kCols;
    v[5] = float(j) / kRows;
  };
  out.reserve(size_t(kCols) * kRows * 6 * 6);
  float a[6], b[6], c[6], e[6];
  for (int j = 0; j < kRows; ++j) {
    for (int i = 0; i < kCols; ++i) {
      vertex(i, j, a);
      vertex(i + 1, j, b);
      vertex(i, j + 1, c);
      vertex(i + 1, j + 1, e);
      for (const float* q : {a, b, c, b, e, c}) out.insert(out.end(), q, q + 6);
    }
  }
  return true;
}

void NoteGuestFovRatio(double ratio) {
  uint64_t bits = 0;
  if (std::isfinite(ratio) && ratio > 0.0 && std::fabs(ratio - 1.0) > 1e-6) {
    std::memcpy(&bits, &ratio, sizeof(bits));
  }
  g_fov_ratio_bits.store(bits, std::memory_order_relaxed);
}

bool RetargetClipRows(const float rows_in[16], float rows_out[16]) {
  const int eye = g_current_eye;
  if (eye < 0 || eye > 1 || !g_eye_vp_valid[eye]) {
    return false;
  }
  // rows: clip_i = dot(p, row_i), i.e. the row-form matrix B = rows^T.
  double B[16], game_inv[16], W[16];
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 4; ++k) B[k * 4 + i] = rows_in[i * 4 + k];
  }
  if (!Invert4(g_game_vp, game_inv)) {
    return false;
  }
  Mul4(B, game_inv, W);  // B = W * VP_game
  // Only an affine world matrix means the draw used the game camera.
  const double lim = 1e-3 * (std::fabs(W[15]) + 1.0);
  if (std::fabs(W[3]) > lim || std::fabs(W[7]) > lim || std::fabs(W[11]) > lim ||
      !(std::fabs(W[15]) > 1e-6)) {
    return false;
  }
  double Be[16];
  Mul4(W, g_eye_vp[eye], Be);
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 4; ++k) rows_out[i * 4 + k] = float(Be[k * 4 + i]);
  }
  return true;
}

// A world-locked panel's clip rows for eye `eye`: `width` metres wide,
// `dist` ahead of the yaw-only pose `ref` at its height.
bool PanelClipRows(int eye, const Pose& ref, double dist, double width, float aspect,
                   float rows_out[16]) {
  if (eye < 0 || eye > 1 || !g_have_ref || !(aspect > 0.1f)) {
    return false;
  }
  const double half_w = 0.5 * width;
  const double half_h = half_w / aspect;
  // Panel -> tracking (column form): u along the reference right, v along
  // world up, centred `dist` ahead of the reference at eye height.
  double M[16] = {};
  for (int i = 0; i < 3; ++i) {
    M[i * 4 + 0] = ref.R[i][0] * half_w;
    M[i * 4 + 1] = ref.R[i][1] * half_h;
    M[i * 4 + 3] = ref.t[i] - ref.R[i][2] * dist;
  }
  M[15] = 1.0;
  // Tracking -> eye (column form), then to display axes (forward = +Z).
  const Pose view = Inverse(g_eye_world[eye]);
  double V[16] = {};
  for (int i = 0; i < 3; ++i) {
    const double flip = i == 2 ? -1.0 : 1.0;
    for (int j = 0; j < 3; ++j) V[i * 4 + j] = flip * view.R[i][j];
    V[i * 4 + 3] = flip * view.t[i];
  }
  V[15] = 1.0;
  // Display projection; z = w / 2 keeps every vertex inside the depth range
  // (drawn without depth).
  const EyeOptics& o = g_optics[eye];
  double P[16] = {};
  P[0] = 2.0 / (o.r - o.l);
  P[2] = -(o.r + o.l) / (o.r - o.l);
  P[5] = 2.0 / (o.u - o.d);
  P[6] = -(o.u + o.d) / (o.u - o.d);
  P[10] = 0.5;
  P[14] = 1.0;
  double A[16];
  Mul4(V, M, A);
  Mul4(P, A, A);
  ToFloat(A, rows_out);  // column form: row i of A is clip_i's row
  return true;
}

bool ScreenClipRows(int eye, float aspect, float rows_out[16]) {
  return PanelClipRows(eye, g_ref, REXCVAR_GET(skate3_vr_screen_distance),
                       REXCVAR_GET(skate3_vr_screen_width), aspect, rows_out);
}

bool MenuClipRows(int eye, float aspect, float rows_out[16]) {
  return PanelClipRows(eye, g_menu_ref, REXCVAR_GET(skate3_vr_menu_distance),
                       REXCVAR_GET(skate3_vr_menu_width), aspect, rows_out);
}

nrhi::Texture* EyeTexture(nrhi::Device* device, int eye, nrhi::Format format) {
  if (eye < 0 || eye > 1 || device == nullptr || !Active()) {
    return nullptr;
  }
  const uint32_t w = g_eye_w.load(std::memory_order_relaxed);
  const uint32_t h = g_eye_h.load(std::memory_order_relaxed);
  if (g_targets.w != w || g_targets.h != h || g_targets.format != format) {
    ReleaseTargets(device);
    g_targets.w = w;
    g_targets.h = h;
    g_targets.format = format;
  }
  if (g_targets.tex[eye] == nullptr) {
    nrhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = format;
    desc.usage = nrhi::kTextureUsageRenderTarget | nrhi::kTextureUsageCopySource;
    // The guest-output contract: idle in the pixel-shader-resource state
    // (kGuestOutput on D3D12), which is also what the compositor expects.
    desc.initial_state = nrhi::ResourceState::kPixelShaderResource;
    g_targets.tex[eye] = device->CreateTexture(desc);
    if (g_targets.tex[eye] == nullptr) {
      REXLOG_ERROR("vr: eye target {}x{} creation failed", w, h);
    }
  }
  return g_targets.tex[eye];
}

void SetFrameTextures(nrhi::Device* device, nrhi::Texture* left, nrhi::Texture* right,
                      bool side_by_side, bool linear) {
  if (device == nullptr || left == nullptr || right == nullptr) {
    g_submit = {};
    return;
  }
  g_submit.left = device->NativeTextureHandle(left);
  g_submit.right = device->NativeTextureHandle(right);
  g_submit.queue = device->NativeQueueHandle();
  g_submit.valid = g_submit.left != nullptr && g_submit.right != nullptr;
  g_submit.side_by_side = side_by_side;
  g_submit.linear = linear;
}

}  // namespace skate3::vr

#else  // !_WIN32: VR compiled out, every entry point answers "flat".

namespace skate3::vr {
void Install(rex::input::InputSystem*) {}
void SetMenuHooks(std::function<bool()>, std::function<void()>) {}
bool MenuVisible() { return false; }
bool Active() { return false; }
bool EyeSize(uint32_t&, uint32_t&) { return false; }
bool OutputSize(uint32_t&, uint32_t&) { return false; }
bool SinglePass() { return false; }
bool ComputeStereoCamera(const float*, const float*, EyeCamera*) { return false; }
void SetGameFov(float) {}
const float* StereoMatrix() { return nullptr; }
const float* EyeViewProj(int) { return nullptr; }
bool WideCull() { return false; }
double GuestAspect() { return 16.0 / 9.0; }
double GuestFovDegrees() { return 60.0; }
int32_t MaxMsaa() { return 8; }
bool FlushEyes() { return false; }
void NoteMidFrameSubmit() {}
const std::vector<float>& HiddenAreaTriangles(int) {
  static const std::vector<float> kNone;
  return kNone;
}
bool BeginFrame(const rex::graphics::NativeGuestOutputRenderContext&) { return false; }
int CurrentEye() { return -1; }
void SetCurrentEye(int) {}
bool ComputeEyeCamera(int, const float*, const float*, EyeCamera*) { return false; }
void SetFirstPersonAnchor(const float*, const float*, const float*, const float*) {}
bool FirstPersonActive() { return false; }
const float* FirstPersonHead() { static const float p[3] = {}; return p; }
bool ApplyGuestCullPlanes(uint8_t*, uint32_t) { return false; }
bool HudTransform(float, float, float*, float*, float*, float*) { return false; }
bool HudGameCameraMatrix(float, float, float, float*) { return false; }
bool HudCurveMesh(int, float, std::vector<float>& out) {
  out.clear();
  return false;
}
void NoteGuestFovRatio(double) {}
bool RetargetClipRows(const float*, float*) { return false; }
bool ScreenClipRows(int, float, float*) { return false; }
void SetMenuRect(float, float, float, float) {}
bool MenuRect(float&, float&, float&, float&) { return false; }
bool MenuClipRows(int, float, float*) { return false; }
rex::graphics::nrhi::Texture* EyeTexture(rex::graphics::nrhi::Device*, int,
                                         rex::graphics::nrhi::Format) {
  return nullptr;
}
void SetFrameTextures(rex::graphics::nrhi::Device*, rex::graphics::nrhi::Texture*,
                      rex::graphics::nrhi::Texture*, bool, bool) {}
}  // namespace skate3::vr

#endif  // _WIN32
