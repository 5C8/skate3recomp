#pragma once

// SteamVR (OpenVR) output for the native renderer. Shared by sk83_na_m and
// skate3recomp: keep the two copies identical.
//
// The native renderer draws each swap into the SDK guest output. In VR the
// output is sized to the headset eye, the frame is rendered once per eye
// with that eye's camera (left straight into the guest output, which the
// window keeps mirroring; right into an own texture), and both are handed to
// the compositor after the swap's GPU submission. The game camera stays in
// charge: the head pose is applied on top of it (the scene is seen from the
// game camera, leveled, with the head's rotation/translation relative to a
// recentered reference). The HUD (the replayed 2D stream) becomes a
// head-locked panel; frames the native renderer yields to the emulated
// output (frontend, loading, editors) are shown on a world-locked virtual
// screen. VR controllers drive the game as an Xbox pad.
//
// Windows + Direct3D 12 only: openvr_api.dll is loaded at runtime, so a
// machine without SteamVR (or a Vulkan session) just runs flat.

#include <cstdint>
#include <functional>
#include <vector>

#include <rex/graphics/native_guest_renderer.h>

namespace rex::input {
class InputSystem;
}

namespace skate3::vr {

// App setup (OnPostSetup): registers the VR-controller pad driver and the
// post-submit hook. Safe to call with VR off; nothing starts until the
// skate3_vr cvar is on and a swap reaches the renderer.
void Install(rex::input::InputSystem* input);

// The app's settings menu (after Install): `visible` reports whether it is
// open, `toggle` opens / closes it (called on the command-processor thread;
// defer to the UI thread). In the headset the open menu (the window's
// presented image, rex::ui::SetPresentedImageTarget) is drawn by the
// renderer on the world-locked screen panel; left stick click + Y on the VR
// controllers toggles it (a gamepad's own menu chord works as usual).
void SetMenuHooks(std::function<bool()> visible, std::function<void()> toggle);
// The settings menu is open this frame (command-processor thread, after
// BeginFrame).
bool MenuVisible();
// VR layout of the settings menu (UI thread, while Active()): the menu draws
// into a 16:9 frame of the window on an opaque backdrop and reports that
// frame here, in window pixels; the headset panel shows exactly that part
// of the presented image.
void SetMenuRect(float x, float y, float w, float h);
bool MenuRect(float& x, float& y, float& w, float& h);
// The settings menu panel for eye `eye` (clip rows like ScreenClipRows):
// skate3_vr_menu_width wide, skate3_vr_menu_distance in front of where the
// head faced when the menu opened (yaw only, at head height), staying put.
bool MenuClipRows(int eye, float aspect, float rows_out[16]);

// Session is up (headset found, compositor interfaces acquired).
bool Active();

// Per-eye render size while Active(). False when inactive.
bool EyeSize(uint32_t& width, uint32_t& height);
// The native guest output size while Active() (the app's size provider
// returns it): side by side (2 x eye width) for one-pass stereo, one eye
// otherwise.
bool OutputSize(uint32_t& width, uint32_t& height);
// One-pass stereo (skate3_vr_single_pass): RenderScene draws both eyes in
// one pass into the side-by-side output, CurrentEye() == kStereoEye.
bool SinglePass();
inline constexpr int kStereoEye = 2;

// Guest culling widened for head look (consumed by the guest-aspect and
// FOV / frustum-cull hooks of each game).
bool WideCull();
double GuestAspect();
double GuestFovDegrees();
// MSAA ceiling for the native scene while Active() (skate3_vr_msaa).
int32_t MaxMsaa();
// Submit the left eye before recording the right (skate3_vr_flush_eyes).
bool FlushEyes();
// The lens-hidden region of an eye (never visible in the headset) as a
// triangle list in that eye's NDC, x/y pairs (y up). Masked out with depth
// before the main pass so nothing shades there (skate3_vr_hidden_area).
// Empty when unavailable or disabled.
const std::vector<float>& HiddenAreaTriangles(int eye);

// Simulated session timing: a mid-frame submission just happened (the
// first one per frame is reported as "first GPU work").
void NoteMidFrameSubmit();

// Guest per-object culling (DoFrustumCulling, any guest thread): while in
// VR, overwrite the camera's six frustum planes (cam + 5152, big-endian
// float4 n.xyz/d, inside when dot(n, p) + d >= 0; plane 4 is the near plane
// the distance/screen-size cull measures from) with a frustum covering both
// headset eyes plus skate3_vr_cull_margin, apexed at the camera's own view
// point (cam + 5120). Only the main game camera: shadow / reflection
// cameras (view point away from the rendered eye) keep their planes. True
// when the planes were replaced.
bool ApplyGuestCullPlanes(uint8_t* base, uint32_t cam);

// ---- Command-processor thread ---------------------------------------------

// Start of a swap's VR frame: lazy session start/stop, WaitGetPoses (once
// per swap), controller poll. True when the frame should be rendered for
// the headset.
bool BeginFrame(const rex::graphics::NativeGuestOutputRenderContext& context);

// The eye the renderer is currently drawing (-1 = none / flat pass).
int CurrentEye();
void SetCurrentEye(int eye);

struct EyeCamera {
  float view_proj[16];  // row-vector, the renderer's FrameScene convention
  float proj[16];
  float cam_pos[3];
};
// The eye's camera from the published game camera (row-vector view_proj and
// projection). False until a head pose exists.
bool ComputeEyeCamera(int eye, const float game_view_proj[16], const float game_proj[16],
                      EyeCamera* out);

// The game camera's flat |m11| this frame (FrameScene::guest_cam_proj11):
// its field of view, so the headset view zooms when the game zooms
// (skate3_vr_follow_zoom). Call before ComputeEyeCamera / ComputeStereoCamera.
void SetGameFov(float flat_proj11);
// Per-scene local-player anchor. nullptr disables first person for this scene.
// forward = the board axis (stable, but points at the tail after a 180);
// chest = the stance facing (root row 0, square to the board); the view is
// chest + a neck angle. face = the skater's gaze (animated), which only
// decides nose vs tail while standing still.
void SetFirstPersonAnchor(const float* head, const float* forward, const float* chest,
                          const float* face);
bool FirstPersonActive();
const float* FirstPersonHead();

// One-pass stereo camera: both eyes from the published game camera. `left`
// becomes the scene camera; StereoMatrix() is left clip -> right clip
// (row-vector, the scene VS's st_d) and EyeViewProj(1) the right eye's.
bool ComputeStereoCamera(const float game_view_proj[16], const float game_proj[16],
                         EyeCamera* left);
const float* StereoMatrix();
// The eye's view_proj from the last ComputeEyeCamera / ComputeStereoCamera
// (row-vector), nullptr when none.
const float* EyeViewProj(int eye);

// Head-locked HUD for the current eye: maps the 2D stream's clip x/y
// (whose 16:9 band spans +-band_x / +-band_y) onto the panel:
// clip.x' = clip.x * sx + clip.w * ox (y likewise).
bool HudTransform(float band_x, float band_y, float* sx, float* sy, float* ox, float* oy);

// The HUD on the game camera's own 16:9 screen (skate3_vr_hud_follow_camera):
// row-form M with clip_eye = clip_hud * M for the 2D stream's ortho clip
// (16:9 band spanning +-band_x / +-band_y). The band maps onto the game
// camera's flat field of view (flat_proj11 = its |m11| before any output
// widening) at skate3_vr_hud_distance along the game camera, so the HUD
// follows the game camera and covers what the flat screen would.
bool HudGameCameraMatrix(float band_x, float band_y, float flat_proj11, float m_out[16]);

// Curved third-person HUD (skate3_vr_hud_curve_x / _y): the HUD's 16:9
// image bent around the game camera per axis, spanning the same angles the
// flat HUD would (HudGameCameraMatrix's extent), as a triangle list for
// `eye`: 6 floats per vertex, clip x/y/z/w then the HUD texture's u/v (v
// down). False when the HUD does not follow the game camera this frame
// (first person, scene-less frames) or both curvatures are 0 (flat).
bool HudCurveMesh(int eye, float flat_proj11, std::vector<float>& out);

// Retail FOV hook: tan(vr fov / 2) / tan(flat fov / 2) while the VR cull
// FOV replaces the flat one (1 otherwise), so the HUD keeps the flat FOV.
void NoteGuestFovRatio(double ratio);

// Re-targets a world-space 2D-stream draw (SimpleDraw markers carrying the
// game camera's view_proj; rows = clip_i = dot(p, row_i)) from the game
// camera of the last ComputeEyeCamera to the current eye. False when the
// rows are not (world matrix) x (that camera), e.g. a 3D preview with its
// own camera: those are left alone.
bool RetargetClipRows(const float rows_in[16], float rows_out[16]);

// World-locked virtual screen for eye `eye`: clip rows mapping the panel's
// local (u, v, 0, 1), u/v in [-1, 1] (v = +1 top), for a source of
// `aspect` (width / height).
bool ScreenClipRows(int eye, float aspect, float rows_out[16]);

// Own eye targets (guest-output format, eye size); the left one is used on
// frames where the guest output is not an eye image (virtual screen).
rex::graphics::nrhi::Texture* EyeTexture(rex::graphics::nrhi::Device* device, int eye,
                                         rex::graphics::nrhi::Format format);

// The two eye images of this swap (in kGuestOutput / pixel-shader-resource
// state once the frame's commands execute); submitted from the post-submit
// hook.
// side_by_side: one texture holding both eyes (left half / right half).
void SetFrameTextures(rex::graphics::nrhi::Device* device, rex::graphics::nrhi::Texture* left,
                      rex::graphics::nrhi::Texture* right, bool side_by_side = false,
                      bool linear = false);

}  // namespace skate3::vr
