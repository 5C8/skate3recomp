#pragma once

// Player-facing settings screen (Escape / F1 / controller menu chord).
//
// Ported from the SDK's SimpleSettingsDialog (skate3recomp's menu): same
// console-style layout, frosted panels and controller/keyboard/mouse
// navigation, with the rows rebuilt for this build:
//  - every setting this build can change at runtime applies the moment it
//    is changed (and is saved to the config right away): no staging, no
//    Apply step;
//  - the few that the engine only reads at boot (graphics API / GPU, VRR on
//    D3D12, monitor, controller backend, audio buffer, game language) are
//    saved immediately and marked "Next launch"; a Restart Now action
//    appears only while one of them differs from what this session booted
//    with;
//  - rows for features this build does not have are gone; FOV and draw
//    distance appear only where their cvars exist (skate3recomp);
//  - an Online tab edits the custom server address (net_host_overrides),
//    the signed-in profile's gamertag and auto sign-in (skate3_profiles).
// The full cvar list stays available in the SDK overlay (Ctrl+F1) and the
// renderer tuning menu on F12.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rex/ui/graphics_device_list.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/simple_settings_overlay.h>  // SimpleSettingsGamepad

namespace skate3 {

// Snapshot the boot-only settings as this session started with them. Call
// once after the config is loaded (OnPostSetup); the menu compares against
// it to decide whether a restart is pending.
void CaptureSettingsBootValues();

// Starts a new instance of the game with this session's command line
// (the caller then closes this one). False if the launch failed.
bool RelaunchGame();

// Build-specific inputs (the menu source is shared by sk83_na_m and
// skate3recomp; keep the two copies identical).
struct SettingsMenuEnv {
  std::vector<std::string> server_hosts;  // net_host_overrides host names
  std::filesystem::path profiles_path;    // skate3_profiles store
  std::function<bool()> is_fullscreen;
  std::function<void(bool)> set_fullscreen;
  std::function<void()> profile_changed;  // signed-in profile edited
};

class SettingsMenu final : public rex::ui::ImGuiDialog {
 public:
  using Callback = std::function<void()>;
  using PollGamepadCallback = std::function<rex::ui::SimpleSettingsGamepad()>;

  SettingsMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
               Callback close_settings, Callback close_game, Callback restart_game,
               PollGamepadCallback poll_gamepad, SettingsMenuEnv env);
  ~SettingsMenu();

  void Show();
  void Toggle();
  void Hide();
  // One "back" step (Escape / pad B): text edit -> row focus -> category
  // rail -> closed.
  void NavigateBack();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class FocusZone { kRail, kContent };

  struct RowSpec;
  struct NavIntents;

  void LoadSettingsFromCvars();
  // A boot-only setting differs from the value this session started with.
  bool HasSettingsChanges() const;
  void SaveConfig();
  void ApplyAndRestart();

  void BuildRows(std::vector<RowSpec>& rows, int category);
  NavIntents GatherInput(ImGuiIO& io);

  std::filesystem::path config_path_;
  Callback close_settings_;
  Callback close_game_;
  Callback restart_game_;
  PollGamepadCallback poll_gamepad_;
  SettingsMenuEnv env_;
  bool visible_ = false;

  // Row values (mirrors of the cvars; every change writes straight back).
  rex::ui::GraphicsDeviceList device_list_;
  int graphics_api_index_ = 0;
  int device_index_ = 0;
  int resolution_index_ = 0;
  int aspect_index_ = 0;
  int frame_cap_index_ = 0;
  char frame_cap_buf_[16] = {};  // Custom framerate cap text field
  int interframes_index_ = 0;
  int msaa_index_ = 2;
  int shadow_quality_index_ = 1;
  int monitor_index_ = 0;
  int audio_buffer_index_ = 0;
  int language_index_ = 0;
  int chord_index_ = 0;
  int input_backend_index_ = 0;
  std::string chord_custom_;
  bool fullscreen_ = false;
  bool vsync_ = false;
  bool tearing_ = true;
  bool letterbox_ = true;
  bool renderer_native_ = true;
  bool shadow_pcss_ = true;
  bool mode_indicator_ = false;
  bool fps_counter_ = false;
  bool zd_debug_features_ = false;
  bool mnk_mode_ = false;
  bool mnk_capture_mouse_ = true;
  bool audio_mute_ = false;
  bool rumble_ = true;
  float mnk_sensitivity_ = 1.0f;
  float field_of_view_ = 60.0f;
  // VR tab (skate3_vr_*): curvatures in percent, FOV in percent of the headset's.
  bool vr_first_person_ = false;
  float vr_curve_x_ = 50.0f;
  float vr_curve_y_ = 25.0f;
  float vr_fov_ = 100.0f;
  float vr_hud_scale_ = 100.0f;
  float vr_render_scale_ = 100.0f;  // percent of SteamVR's recommended eye size
  int vr_msaa_index_ = 1;  // Off / 2x / 4x / 8x
  int draw_distance_index_ = 0;
  // Online tab text fields (custom server address, profile gamertag).
  char server_buf_[64] = {};
  char username_buf_[32] = {};
  bool auto_sign_in_ = true;

  // Navigation state.
  FocusZone zone_ = FocusZone::kRail;
  int category_ = 0;
  int rail_sel_ = 0;  // 0..category count-1 = categories; count = Close Game
  int row_index_ = 0;
  bool editing_text_ = false;
  bool text_edit_focus_pending_ = false;
  bool pad_active_ = false;
  uint16_t prev_pad_buttons_ = 0;
  int held_dir_x_ = 0;
  int held_dir_y_ = 0;
  float repeat_timer_x_ = 0.0f;
  float repeat_timer_y_ = 0.0f;
  float highlight_anim_y_ = -1.0f;
  float rail_anim_y_ = -1.0f;
  float content_scroll_ = 0.0f;
  float content_scroll_anim_ = -1.0f;
  float wheel_accum_ = 0.0f;
  bool just_shown_ = false;
  float mouse_x_ = -1.0f;
  float mouse_y_ = -1.0f;
};

}  // namespace skate3
