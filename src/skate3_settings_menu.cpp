// Player-facing settings screen; see skate3_settings_menu.h.
//
// Layout, palette, draw helpers, input handling and OnDraw are carried over
// from the SDK's SimpleSettingsDialog (rexglue ui/overlay/
// simple_settings_overlay.cpp) unchanged except for the pending-restart
// wording; the option tables, cvar plumbing and rows are this build's own.

#include "skate3_vr.h"
#include "skate3_settings_menu.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <rex/audio/audio_driver.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "skate3_profiles.h"
#include "skate3_native_scene.h"  // ToggleSceneEnabled
#include "skate3_render_resolution.h"

namespace skate3 {

// The carried-over draw code names SDK types unqualified (Presenter,
// Window, GetGraphicsDeviceList, ...).
using namespace rex::ui;

namespace {

// X_INPUT_GAMEPAD_* button bits, mirrored locally to keep the UI overlay
// decoupled from the kernel input headers.
constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadLShoulder = 0x0100;
constexpr uint16_t kPadRShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;


// Navigation repeat pacing (seconds).
constexpr float kRepeatDelay = 0.42f;
constexpr float kRepeatRate = 0.11f;

// Menu-only scale on top of the viewport scale: the rail/content/description
// columns and title shrink together while the footer legend stays at the
// base size.
constexpr float kMenuScale = 0.92f;

// The content view never grows past the classic 12 row slots, so the smaller
// menu buys breathing room rather than extra rows.
constexpr int kMaxViewRows = 12;

// ---- Palette -------------------------------------------------------------
// Frosted-panel color scheme (iterated against
// real gameplay captures): teal frosted side panels with white text over the
// blurred scene, WHITE setting rows with near-black text, black focused row
// with a lime ring, lime section/description bars, magenta steppers/slider.

constexpr ImU32 kColSelFill = IM_COL32(10, 12, 12, 255);
constexpr ImU32 kColSelText = IM_COL32(250, 252, 252, 255);
// Rows are OPAQUE (no scene bleed-through) but tuned to the tone the
// raw white would render (~235 vs 252) so they don't read as
// too bright or too contrasty.
constexpr ImU32 kColPanel = IM_COL32(235, 236, 236, 255);
constexpr ImU32 kColPanelHover = IM_COL32(248, 250, 250, 255);
constexpr ImU32 kColPanelBorder = IM_COL32(0, 0, 0, 26);
constexpr ImU32 kColRowText = IM_COL32(15, 17, 18, 255);
constexpr ImU32 kColRailPanel = IM_COL32(14, 56, 53, 140);
constexpr ImU32 kColRailPanelHover = IM_COL32(24, 90, 86, 166);
constexpr ImU32 kColRailBorder = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kColText = IM_COL32(235, 242, 241, 255);
constexpr ImU32 kColTextDim = IM_COL32(224, 235, 234, 191);
constexpr ImU32 kColTextFaint = IM_COL32(150, 158, 158, 255);
constexpr ImU32 kColAccent = IM_COL32(213, 235, 10, 255);
constexpr ImU32 kColAccentDark = IM_COL32(13, 15, 5, 255);
constexpr ImU32 kColInteract = IM_COL32(230, 0, 120, 255);
constexpr ImU32 kColInteractHover = IM_COL32(255, 25, 145, 255);
constexpr ImU32 kColDanger = IM_COL32(233, 88, 76, 255);
constexpr ImU32 kColWarn = IM_COL32(245, 197, 87, 255);
constexpr ImU32 kColDescPanel = IM_COL32(11, 46, 43, 140);
constexpr ImU32 kColChevDisabled = IM_COL32(0, 0, 0, 56);
// The disabled chevron on the FOCUSED (black) row needs a light variant -
// black-alpha disappears there.
// White-25% disc - keeps the disabled stepper
// visible on the focused black row.
constexpr ImU32 kColChevDisabledOnDark = IM_COL32(255, 255, 255, 64);
constexpr ImU32 kColLegendChip = IM_COL32(238, 240, 240, 255);
constexpr ImU32 kColLegendText = IM_COL32(15, 18, 20, 255);
constexpr ImU32 kColLegendLabel = IM_COL32(228, 236, 235, 255);
constexpr ImU32 kColWhite = IM_COL32(255, 255, 255, 255);

// ---- Option tables ---------------------------------------------------------

// Categories shown in the left rail.
struct CategoryInfo {
  const char* name;
  const char* desc;
};
constexpr std::array<CategoryInfo, 6> kCategories = {{
    {"Video", "Display, resolution, framerate and graphics settings. Changes apply "
              "immediately unless marked Next launch."},
    {"Controls", "Controller, mouse and keyboard input settings."},
    {"Audio", "Sound output settings."},
    {"Online", "Custom server, your gamertag and profile sign-in."},
    {"System", "Game language, debugging features and session settings."},
    {"VR", "Headset view and HUD settings. Apply immediately while in VR."},
}};

// Draw distance (skate3recomp): one multiplier for both the world detail cull
// and the character LOD switch ranges.
constexpr std::array<const char*, 4> kDrawDistanceLabels = {"Original", "2x", "3x", "5x"};
constexpr std::array<double, 4> kDrawDistanceScales = {1.0, 2.0, 3.0, 5.0};

int NearestDrawDistanceIndex(double value) {
  int best = 0;
  for (int i = 1; i < int(kDrawDistanceScales.size()); ++i) {
    if (std::abs(value - kDrawDistanceScales[i]) < std::abs(value - kDrawDistanceScales[best])) {
      best = i;
    }
  }
  return best;
}

// Host names the game resolves for online play (per build, from
// SettingsMenuEnv::server_hosts); the server address row points all of them
// at the custom server (net_host_overrides).
std::vector<std::string> g_server_hosts;

// Output resolution presets (skate3_render_width / _height; 0 = auto).
// Height-only presets take the width from the aspect ratio row.
struct ResolutionPreset {
  const char* label;
  int32_t width;
  int32_t height;
};
constexpr std::array<ResolutionPreset, 6> kResolutionPresets = {{
    {"Window", 0, 0},
    {"720p", 0, 720},
    {"1080p", 0, 1080},
    {"1440p", 0, 1440},
    {"2160p (4K)", 0, 2160},
    {"1152 x 640 (console)", 1152, 640},
}};

// Aspect presets (skate3_render_aspect); "window" follows the window shape.
constexpr std::array<const char*, 6> kAspectSpecs = {"window", "16:9", "16:10",
                                                     "4:3",    "21:9", "32:9"};
constexpr std::array<const char*, 6> kAspectLabels = {"Window", "16:9", "16:10",
                                                      "4:3",    "21:9", "32:9"};

// Framerate cap: Auto (below display refresh), fixed rates, Unlimited, or a
// Custom rate typed in (any value in [kFrameCapMin, kFrameCapMax]).
constexpr std::array<double, 6> kFrameCapRates = {60.0, 90.0, 120.0, 144.0, 165.0, 240.0};
constexpr int kFrameCapAutoIndex = 0;
constexpr int kFrameCapUnlimitedIndex = 1 + int(kFrameCapRates.size());
constexpr int kFrameCapCustomIndex = kFrameCapUnlimitedIndex + 1;
constexpr double kFrameCapMin = 20.0;
constexpr double kFrameCapMax = 1000.0;

// MSAA sample counts for the native scene renderer.
constexpr std::array<const char*, 4> kMsaaLabels = {"Off", "2x", "4x", "8x"};
constexpr std::array<int32_t, 4> kMsaaSamples = {1, 2, 4, 8};

// Shadow quality: dynamic-shadow toggle + cascade tile resolution. The
// game's own atlas is three 512 tiles; tile 0 = auto (512 per 720 output
// lines, the emulated renderer's raster at the same resolution).
constexpr std::array<const char*, 6> kShadowQualityLabels = {
    "Off", "Auto", "512 (console)", "1024", "2048", "4096"};
constexpr std::array<int32_t, 6> kShadowQualityTiles = {0, 0, 512, 1024, 2048, 4096};

// Audio device buffer sizes in sample frames (0 = backend default).
constexpr std::array<const char*, 4> kAudioBufferLabels = {
    "Auto", "512 (low latency)", "1024", "2048 (most stable)"};
constexpr std::array<int32_t, 4> kAudioBufferFrames = {0, 512, 1024, 2048};

// Xbox 360 XLanguage ids 1..12 (user_language cvar).
constexpr std::array<const char*, 12> kLanguageLabels = {
    "English",    "Japanese",           "German",     "French",
    "Spanish",    "Italian",            "Korean",     "Traditional Chinese",
    "Portuguese", "Simplified Chinese", "Polish",     "Russian"};

// Settings-menu controller chord presets (menu_chord cvar spec strings).
constexpr std::array<const char*, 4> kMenuChordSpecs = {"rb+start", "lb+rb+start", "back+start",
                                                        "l3+r3"};
constexpr std::array<const char*, 4> kMenuChordLabels = {"RB + Start", "LB + RB + Start",
                                                         "Back + Start", "L3 + R3"};

constexpr std::array<const char*, 5> kMonitorLabels = {"Auto", "Monitor 1", "Monitor 2",
                                                       "Monitor 3", "Monitor 4"};

constexpr const char* kNextLaunch = "Takes effect the next time the game starts";

// ---- CVar plumbing ---------------------------------------------------------

bool HasCvar(std::string_view name) {
  return rex::cvar::GetFlagInfo(name) != nullptr;
}

std::optional<std::string> CvarDefault(std::string_view name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  if (!info) {
    return std::nullopt;
  }
  return info->default_value;
}

bool CvarDefaultBool(std::string_view name, bool fallback) {
  auto value = CvarDefault(name);
  if (!value) {
    return fallback;
  }
  return *value == "true" || *value == "1";
}

double CvarDefaultDouble(std::string_view name, double fallback) {
  auto value = CvarDefault(name);
  if (!value || value->empty()) {
    return fallback;
  }
  return std::strtod(value->c_str(), nullptr);
}

bool QueryBool(std::string_view name, bool fallback = false) {
  return HasCvar(name) ? rex::cvar::Query<bool>(name) : fallback;
}

void SetBoolCvar(std::string_view name, bool value) {
  if (HasCvar(name)) {
    rex::cvar::SetFlagByName(name, value ? "true" : "false");
  }
}

void SetCvar(std::string_view name, const std::string& value) {
  if (HasCvar(name)) {
    rex::cvar::SetFlagByName(name, value);
  }
}

void CopyToBuffer(char* buffer, size_t buffer_size, const std::string& value) {
  size_t count = std::min(value.size(), buffer_size - 1);
  std::copy_n(value.data(), count, buffer);
  buffer[count] = '\0';
}

bool D3D12Active() {
  return GetGraphicsDeviceList().cvar_name == "d3d12_adapter";
}

// Every cvar this menu owns, persisted to the config on each change.
std::vector<std::string_view> GetSettingsCvars() {
  static constexpr std::array<std::string_view, 40> kNames = {
      "net_host_overrides",
      "skate3_native_render_scene",
      "skate3_native_render_scene_msaa",
      "skate3_native_render_scene_shadows",
      "skate3_native_render_scene_shadow_tile",
      "skate3_native_render_scene_shadow_pcss",
      "skate3_native_render_mode_indicator",
      "skate3_render_width",
      "skate3_render_height",
      "skate3_render_aspect",
      "skate3_guest_fps_cap",
      "skate3_guest_fps_cap_auto",
      "skate3_native_render_scene_interframes",
      "present_letterbox",
      "fullscreen",
      "vsync",
      "monitor",
      "show_fps_counter",
      "zd_debug",
      "d3d12_allow_variable_refresh_rate_and_tearing",
      "vulkan_allow_present_mode_immediate",
      "vulkan_allow_present_mode_mailbox",
      "vulkan_allow_present_mode_fifo_relaxed",
      "mnk_mode",
      "mnk_capture_mouse",
      "mnk_sensitivity",
      "hid_rumble_enabled",
      "menu_chord",
      "input_backend",
      "audio_mute",
      "audio_device_sample_frames",
      "user_language",
      "gpu_backend",
      "skate3_vr_first_person",
      "skate3_vr_hud_curve_x",
      "skate3_vr_hud_curve_y",
      "skate3_vr_fov",
      "skate3_vr_hud_scale",
      "skate3_vr_render_scale",
      "skate3_vr_msaa",
  };
  std::vector<std::string_view> names;
  for (std::string_view name : kNames) {
    if (HasCvar(name)) {
      names.push_back(name);
    }
  }
  // Features only some builds have (skate3recomp: camera FOV, draw distance).
  for (std::string_view name : {std::string_view("skate3_field_of_view"),
                                std::string_view("skate3_draw_distance_scale"),
                                std::string_view("skate3_lod_distance_scale")}) {
    if (HasCvar(name)) {
      names.push_back(name);
    }
  }
  const std::string device_cvar = GetGraphicsDeviceList().cvar_name;
  if (device_cvar == "d3d12_adapter" && HasCvar("d3d12_adapter")) {
    names.push_back("d3d12_adapter");
  } else if (device_cvar == "vulkan_device" && HasCvar("vulkan_device")) {
    names.push_back("vulkan_device");
  }
  return names;
}

// Boot-only settings: the engine reads these once at startup. Their boot
// values are snapshotted so the menu can tell when a restart would change
// something (and only then offer it).
std::vector<std::string> BootOnlyCvars() {
  std::vector<std::string> names = {"gpu_backend",   "monitor",
                                    "input_backend", "audio_device_sample_frames",
                                    "user_language"};
  names.push_back(GetGraphicsDeviceList().cvar_name);
  if (D3D12Active()) {
    // The D3D12 swap chain takes its tearing flag at creation.
    names.push_back("d3d12_allow_variable_refresh_rate_and_tearing");
  }
  return names;
}

std::map<std::string, std::string>& BootValues() {
  static std::map<std::string, std::string> values;
  return values;
}

std::string CvarString(const std::string& name) {
  return HasCvar(name) ? rex::cvar::GetFlagByName(name) : std::string();
}

// ---- Value <-> row index helpers ----------------------------------------

bool HasGraphicsApiChoice() {
  return HasCvar("gpu_backend") && HasCvar("d3d12_adapter") && HasCvar("vulkan_device");
}

// 0 = Direct3D 12, 1 = Vulkan. "auto" reads as the backend actually running.
int GraphicsApiIndexFromCvar() {
  const std::string value = rex::cvar::Query<std::string>("gpu_backend");
  if (value == "vulkan") {
    return 1;
  }
  if (value == "d3d12") {
    return 0;
  }
  return D3D12Active() ? 0 : 1;
}

int DeviceIndexFromCvar(const GraphicsDeviceList& device_list) {
  if (device_list.device_names.empty() || !HasCvar(device_list.cvar_name)) {
    return 0;
  }
  int32_t current = rex::cvar::Query<int32_t>(device_list.cvar_name);
  if (current >= 0 && current < static_cast<int32_t>(device_list.device_names.size())) {
    return current;
  }
  return std::clamp(device_list.active_index, 0,
                    static_cast<int32_t>(device_list.device_names.size()) - 1);
}

// kResolutionPresets.size() = a hand-set size the presets don't cover.
int ResolutionIndexFromCvars() {
  const int32_t w = rex::cvar::Query<int32_t>("skate3_render_width");
  const int32_t h = rex::cvar::Query<int32_t>("skate3_render_height");
  for (int i = 0; i < int(kResolutionPresets.size()); ++i) {
    if (kResolutionPresets[i].width == w && kResolutionPresets[i].height == h) {
      return i;
    }
  }
  return int(kResolutionPresets.size());
}

// kAspectSpecs.size() = a hand-set ratio the presets don't cover.
int AspectIndexFromCvar() {
  const double ratio = render_resolution::ParseAspect(
      rex::cvar::Query<std::string>("skate3_render_aspect").c_str());
  if (ratio <= 0.0) {
    return 0;
  }
  for (int i = 1; i < int(kAspectSpecs.size()); ++i) {
    if (std::abs(render_resolution::ParseAspect(kAspectSpecs[i]) - ratio) < 1e-3) {
      return i;
    }
  }
  return int(kAspectSpecs.size());
}

int FrameCapIndexFromRate(double rate) {
  if (rate < 1.0) {
    return kFrameCapUnlimitedIndex;
  }
  for (int i = 0; i < int(kFrameCapRates.size()); ++i) {
    if (std::abs(kFrameCapRates[i] - rate) < 0.01) {
      return 1 + i;
    }
  }
  return kFrameCapCustomIndex;  // a hand-set rate: shown as Custom, never snapped
}

// "72", "72.5": the shortest form of a cap rate for the menu.
std::string FormatFrameCap(double rate) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", rate);
  std::string out = text;
  while (!out.empty() && out.back() == '0') out.pop_back();
  if (!out.empty() && out.back() == '.') out.pop_back();
  return out;
}

// The typed custom rate, or 0 when the field is empty / not a number.
double ParseFrameCap(const char* text) {
  char* end = nullptr;
  const double rate = std::strtod(text, &end);
  if (end == text || !(rate > 0.0)) {
    return 0.0;
  }
  return std::clamp(rate, kFrameCapMin, kFrameCapMax);
}

int FrameCapIndexFromCvars() {
  if (QueryBool("skate3_guest_fps_cap_auto")) {
    return kFrameCapAutoIndex;
  }
  return FrameCapIndexFromRate(rex::cvar::Query<double>("skate3_guest_fps_cap"));
}

// custom_rate: the Custom entry's typed value (0 = none typed yet: uncapped
// until one is entered).
void ApplyFrameCapIndex(int index, double custom_rate = 0.0) {
  SetBoolCvar("skate3_guest_fps_cap_auto", index == kFrameCapAutoIndex);
  const double rate = index >= 1 && index < kFrameCapUnlimitedIndex ? kFrameCapRates[index - 1]
                      : index == kFrameCapCustomIndex              ? custom_rate
                                                                   : 0.0;
  SetCvar("skate3_guest_fps_cap", std::to_string(rate));
  // Never run both pacers (the host limiter drops presents on an irregular
  // beat, the judder the guest cap exists to remove).
  SetBoolCvar("d3d12_present_frame_limiter", false);
}

int MsaaIndexFromSamples(int32_t samples) {
  int best = 0;
  for (int i = 0; i < int(kMsaaSamples.size()); ++i) {
    if (samples >= kMsaaSamples[i]) {
      best = i;
    }
  }
  return best;
}

int ShadowQualityIndexFrom(bool enabled, int32_t tile) {
  if (!enabled) {
    return 0;
  }
  if (tile <= 0) {
    return 1;
  }
  if (tile <= 512) {
    return 2;
  }
  if (tile <= 1024) {
    return 3;
  }
  return tile <= 2048 ? 4 : 5;
}

void ApplyShadowQualityIndex(int index) {
  index = std::clamp(index, 0, int(kShadowQualityTiles.size()) - 1);
  SetBoolCvar("skate3_native_render_scene_shadows", index != 0);
  if (index != 0) {
    SetCvar("skate3_native_render_scene_shadow_tile",
            std::to_string(kShadowQualityTiles[index]));
  }
}

int AudioBufferIndexFromFrames(int32_t frames) {
  if (frames <= 0) {
    return 0;
  }
  if (frames <= 512) {
    return 1;
  }
  return frames <= 1024 ? 2 : 3;
}

int MenuChordIndexFromCvar(std::string* custom_spec) {
  std::string current = rex::cvar::Query<std::string>("menu_chord");
  for (int i = 0; i < int(kMenuChordSpecs.size()); ++i) {
    if (current == kMenuChordSpecs[i]) {
      return i;
    }
  }
  if (custom_spec) {
    *custom_spec = std::move(current);
  }
  return int(kMenuChordSpecs.size());
}

// ---- Custom server (net_host_overrides) ----------------------------------
// The cvar is "host=target" pairs separated by ',' or ';' and is re-read on
// every name lookup, so an edit applies to the next connection.

std::string Trim(std::string v) {
  const size_t b = v.find_first_not_of(" \t");
  if (b == std::string::npos) {
    return {};
  }
  return v.substr(b, v.find_last_not_of(" \t") - b + 1);
}

std::vector<std::pair<std::string, std::string>> ParseHostOverrides(const std::string& spec) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (size_t pos = 0; pos < spec.size();) {
    size_t end = spec.find_first_of(",;", pos);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string entry = spec.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = entry.find('=');
    if (eq != std::string::npos) {
      std::string host = Trim(entry.substr(0, eq));
      std::string target = Trim(entry.substr(eq + 1));
      if (!host.empty()) {
        pairs.emplace_back(std::move(host), std::move(target));
      }
    }
  }
  return pairs;
}

bool IsServerHost(const std::string& host) {
  for (const std::string& h : g_server_hosts) {
    if (host == h) {
      return true;
    }
  }
  return false;
}

// The address the online hosts currently point at (the redirector's
// target), or empty when no override is set.
std::string ServerAddressFromCvar() {
  if (!HasCvar("net_host_overrides")) {
    return {};
  }
  for (const auto& [host, target] :
       ParseHostOverrides(rex::cvar::Query<std::string>("net_host_overrides"))) {
    if (IsServerHost(host)) {
      return target;
    }
  }
  return {};
}

// Points every online host at `address`, keeping any unrelated overrides.
// An empty address removes the online overrides (the game's own hosts).
void SetServerAddress(const std::string& address) {
  std::string spec;
  const auto append = [&spec](const std::string& host, const std::string& target) {
    if (!spec.empty()) {
      spec += "; ";
    }
    spec += host + "=" + target;
  };
  for (const auto& [host, target] :
       ParseHostOverrides(rex::cvar::Query<std::string>("net_host_overrides"))) {
    if (!IsServerHost(host)) {
      append(host, target);
    }
  }
  if (!address.empty()) {
    for (const std::string& host : g_server_hosts) {
      append(host, address);
    }
  }
  SetCvar("net_host_overrides", spec);
}

bool TearingFromCvar() {
  if (HasCvar("d3d12_allow_variable_refresh_rate_and_tearing") && D3D12Active()) {
    return rex::cvar::Query<bool>("d3d12_allow_variable_refresh_rate_and_tearing");
  }
  return QueryBool("vulkan_allow_present_mode_immediate");
}

void SetTearingCvars(bool value) {
  SetBoolCvar("d3d12_allow_variable_refresh_rate_and_tearing", value);
  SetBoolCvar("vulkan_allow_present_mode_immediate", value);
  SetBoolCvar("vulkan_allow_present_mode_mailbox", value);
  SetBoolCvar("vulkan_allow_present_mode_fifo_relaxed", value);
}

// ---- Small draw helpers ----------------------------------------------------

// Snap to whole pixels. Fractional origins under bilinear filtering smear
// glyph edges and turn 1px rules into soft 2px lines - browsers pixel-snap
// text and borders, so snapping to whole pixels keeps them crisp.
float Snap(float value) {
  return std::floor(value + 0.5f);
}

std::string TruncateToWidth(ImFont* font, float size, float max_width, std::string text) {
  if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
    return text;
  }
  while (!text.empty() &&
         font->CalcTextSizeA(size, FLT_MAX, 0.0f, (text + "...").c_str()).x > max_width) {
    text.pop_back();
  }
  return text + "...";
}

void AddTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                     const char* text) {
  ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size,
              ImVec2(Snap(center.x - extent.x * 0.5f), Snap(center.y - extent.y * 0.5f)), col,
              text);
}

// Optical centering for short glyph labels (pad button circles, key chips):
// AddTextCentered centers the line box, but a capital's visual ink sits
// asymmetrically inside it (descender slack below, bearings on the sides), so
// the letter lands ~1px down-right of a small circle's center. Vertical
// centers the CAP-HEIGHT BAND taken from 'H'
// rather than each glyph's own ink: per-glyph raster boxes round
// independently (A has a pointed-apex overshoot, Y doesn't), which lands
// adjacent buttons on different subpixel rows - the eye catches that
// inconsistency more than any absolute offset. One shared band = identical
// placement for every label, overshoots hanging evenly, which is also what
// the browser's hinted rasterizer converges to. Horizontal centers the ink
// for single glyphs (circles) or the advance box when ink_x is false -
// multi-glyph chip labels match the browser's text-box centering that way.
// ASCII labels only.
void AddTextCenteredCap(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                        const char* text, bool ink_x) {
  ImFontBaked* baked = font->GetFontBaked(size);
  float pen = 0.0f;
  float min_x = FLT_MAX, max_x = -FLT_MAX;
  for (const char* p = text; *p; ++p) {
    const ImFontGlyph* glyph = baked->FindGlyph(ImWchar(uint8_t(*p)));
    if (!glyph) {
      continue;
    }
    if (glyph->Visible) {
      min_x = std::min(min_x, pen + glyph->X0);
      max_x = std::max(max_x, pen + glyph->X1);
    }
    pen += glyph->AdvanceX;
  }
  if (min_x > max_x) {
    return;  // no visible ink
  }
  float band_top = 0.0f;
  float band_bottom = size;
  if (const ImFontGlyph* cap = baked->FindGlyphNoFallback(ImWchar('H'))) {
    band_top = cap->Y0;
    band_bottom = cap->Y1;
  }
  const float x = ink_x ? center.x - (min_x + max_x) * 0.5f : center.x - pen * 0.5f;
  // Round-half-DOWN for the vertical position: the cap band regularly has a
  // half-integer center (e.g. 18px band in a 52px circle), and Snap's
  // round-half-up resolved every tie downward - measured exactly 1px low
  // versus the browser, which resolves the same tie upward (the typographic
  // convention: when a glyph can't center exactly, err high).
  const float y = center.y - (band_top + band_bottom) * 0.5f;
  dl->AddText(font, size, ImVec2(Snap(x), std::ceil(y - 0.5f)), col, text);
}

void AddTextVCentered(ImDrawList* dl, ImFont* font, float size, float x, float center_y,
                      ImU32 col, const char* text) {
  ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(Snap(x), Snap(center_y - extent.y * 0.5f)), col, text);
}

// Justified paragraph (browser text-align: justify): wraps words to wrap_w
// and stretches the word gaps of every full line so both edges align; the
// last line of the paragraph stays ragged-left. Returns the height consumed.
float AddTextJustified(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float wrap_w,
                       ImU32 col, const char* text) {
  std::vector<std::string_view> words;
  for (const char* p = text; *p;) {
    while (*p == ' ') {
      ++p;
    }
    const char* start = p;
    while (*p && *p != ' ') {
      ++p;
    }
    if (p > start) {
      words.emplace_back(start, size_t(p - start));
    }
  }
  if (words.empty()) {
    return 0.0f;
  }
  const float space_w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
  auto word_w = [&](std::string_view w) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, w.data(), w.data() + w.size()).x;
  };
  float y = pos.y;
  size_t i = 0;
  while (i < words.size()) {
    float line_w = word_w(words[i]);
    size_t j = i + 1;
    while (j < words.size()) {
      const float w = word_w(words[j]);
      if (line_w + space_w + w > wrap_w) {
        break;
      }
      line_w += space_w + w;
      ++j;
    }
    const bool last_line = j == words.size();
    float gap = space_w;
    if (!last_line && j - i > 1) {
      const float words_only = line_w - space_w * float(j - i - 1);
      const float stretched = (wrap_w - words_only) / float(j - i - 1);
      // Conditional justify: only stretch when the gaps stay modest -
      // beyond ~1.6x a normal space the stretched line reads worse than a
      // ragged one (the "big spaces" objection to naive justification).
      if (stretched <= space_w * 1.6f) {
        gap = stretched;
      }
    }
    float x = pos.x;
    for (size_t k = i; k < j; ++k) {
      dl->AddText(font, size, ImVec2(Snap(x), Snap(y)), col, words[k].data(),
                  words[k].data() + words[k].size());
      x += word_w(words[k]) + gap;
    }
    y += size;
    i = j;
  }
  return y - pos.y;
}

constexpr float kPi = 3.14159265358979323846f;

// Rounded ring band around [p_min,p_max] spanning inset e0..e1 outside the
// rect. The browser rasterizes its stacked box-shadows with pixel-hard
// straight edges (a 4px lime band is EXACTLY 4 full-coverage rows) and
// antialiases only the corner arcs. ImGui's stroked AddRect can't do that:
// PathRect offsets the centerline by 0.5px and thick AA polylines feather
// 1px on both sides, so every horizontal band edge straddled two rows across
// the full row width - measured as 5 soft rows spanning the whole focused
// row (web: zero). Straight runs are therefore drawn as unrounded
// AddRectFilled (PrimRect - no AA fringe) on snapped coordinates; only the
// corner quadrants go through the AA arc stroker.
void DrawHardRingBand(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float e0, float e1,
                      float corner_r, ImU32 col) {
  const float x0 = Snap(p_min.x), y0 = Snap(p_min.y);
  const float x1 = Snap(p_max.x), y1 = Snap(p_max.y);
  const float rr = Snap(corner_r);
  const float o0 = Snap(e0), o1 = Snap(e1);
  const float a0 = x0 + rr, a1 = x1 - rr;  // straight-run x span
  const float b0 = y0 + rr, b1 = y1 - rr;  // straight-run y span
  dl->AddRectFilled(ImVec2(a0, y0 - o1), ImVec2(a1, y0 - o0), col);  // top
  dl->AddRectFilled(ImVec2(a0, y1 + o0), ImVec2(a1, y1 + o1), col);  // bottom
  dl->AddRectFilled(ImVec2(x0 - o1, b0), ImVec2(x0 - o0, b1), col);  // left
  dl->AddRectFilled(ImVec2(x1 + o0, b0), ImVec2(x1 + o1, b1), col);  // right
  // Corner quadrants: annulus arcs, stroke centered mid-band.
  const float rm = rr + (o0 + o1) * 0.5f;
  const float t = o1 - o0;
  const struct {
    ImVec2 c;
    float ang0;
  } corners[4] = {
      {ImVec2(a0, b0), kPi},
      {ImVec2(a1, b0), kPi * 1.5f},
      {ImVec2(a1, b1), 0.0f},
      {ImVec2(a0, b1), kPi * 0.5f},
  };
  for (const auto& c : corners) {
    dl->PathArcTo(c.c, rm, c.ang0, c.ang0 + kPi * 0.5f);
    dl->PathStroke(col, 0, t);
  }
}

// Rounded fill with pixel-hard straight edges (browser border-radius
// behavior: only the corner arcs antialias). PathFillConvex would feather
// every edge 1px, leaving a soft seam where the fill meets the lime band.
void DrawHardRoundedFill(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float corner_r, ImU32 col) {
  const float x0 = Snap(p_min.x), y0 = Snap(p_min.y);
  const float x1 = Snap(p_max.x), y1 = Snap(p_max.y);
  const float rr = Snap(corner_r);
  const float a0 = x0 + rr, a1 = x1 - rr;
  const float b0 = y0 + rr, b1 = y1 - rr;
  dl->AddRectFilled(ImVec2(x0, b0), ImVec2(x1, b1), col);  // full-width middle
  dl->AddRectFilled(ImVec2(a0, y0), ImVec2(a1, b0), col);  // top strip
  dl->AddRectFilled(ImVec2(a0, b1), ImVec2(a1, y1), col);  // bottom strip
  const struct {
    ImVec2 c;
    float ang0;
  } corners[4] = {
      {ImVec2(a0, b0), kPi},
      {ImVec2(a1, b0), kPi * 1.5f},
      {ImVec2(a1, b1), 0.0f},
      {ImVec2(a0, b1), kPi * 0.5f},
  };
  for (const auto& c : corners) {
    dl->PathLineTo(c.c);
    dl->PathArcTo(c.c, rr, c.ang0, c.ang0 + kPi * 0.5f);
    dl->PathFillConvex(col);
  }
}

// Focused-item highlight: rounded black fill with a lime ring and a thicker
// black outer edge, both drawn OUTSIDE the box (stacked
// box-shadows). Total ring = 6*s beyond the rect on every side; callers keep
// a matching margin in their clip rects.
void DrawFocusHighlight(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float s) {
  const float radius = 6.0f * s;
  DrawHardRoundedFill(dl, p_min, p_max, radius, kColSelFill);
  // Lime band: box edge -> +2*s (its corner arc rides at radius + 1*s).
  DrawHardRingBand(dl, p_min, p_max, 0.0f, 2.0f * s, radius, kColAccent);
  // Black outer edge: +2*s -> +6*s (corner arc at radius + 4*s).
  DrawHardRingBand(dl, p_min, p_max, 2.0f * s, 6.0f * s, radius, kColSelFill);
}

// Stepper chevron: a triangle in a circle.
void DrawChevron(ImDrawList* dl, ImVec2 center, float radius, bool points_left, ImU32 circle_col,
                 ImU32 tri_col, bool filled) {
  center = ImVec2(Snap(center.x), Snap(center.y));
  if (filled) {
    dl->AddCircleFilled(center, radius, circle_col, 24);
  } else {
    dl->AddCircle(center, radius, circle_col, 24, 1.5f);
  }
  // Tip-anchored triangle (tip reaches further from center than the back
  // edge). A bbox-centered variant was tried and looked worse in playtest -
  // keep this geometry.
  float t = radius * 0.42f;
  float dir = points_left ? -1.0f : 1.0f;
  ImVec2 tip(center.x + dir * t, center.y);
  ImVec2 a(center.x - dir * t * 0.6f, center.y - t);
  ImVec2 b(center.x - dir * t * 0.6f, center.y + t);
  dl->AddTriangleFilled(tip, a, b, tri_col);
}

struct LegendGlyph {
  const char* glyph;
  const char* label;
  bool circle;  // pad face button -> circle, everything else -> key chip
};

}  // namespace

void CaptureSettingsBootValues() {
  auto& values = BootValues();
  values.clear();
  for (const std::string& name : BootOnlyCvars()) {
    if (HasCvar(name)) {
      values[name] = CvarString(name);
    }
  }
}

// One setting row (or section header) in the content column.
struct SettingsMenu::RowSpec {
  enum Kind { kHeader, kEnum, kSlider, kAction, kText };
  Kind kind = kEnum;
  const char* label = nullptr;
  const char* desc = nullptr;
  std::string desc_extra;            // dynamic addendum shown under desc
  const char* value_note = nullptr;  // short warning shown in the description
  bool enabled = true;
  bool danger = false;
  // kEnum (options step left/right; either index or flag backs the value)
  std::vector<std::string> options;
  int* index = nullptr;
  bool* flag = nullptr;
  bool checkbox = false;  // show a square check box instead of enum steppers
  std::function<void(int)> on_enum_change;
  // kSlider
  float* value = nullptr;
  float min = 0.0f;
  float max = 1.0f;
  float step = 1.0f;
  const char* fmt = "%.0f";
  std::function<void()> on_value_change;
  // kAction
  std::function<void()> action;
  // kText
  char* text_buf = nullptr;
  size_t text_buf_size = 0;
  // Fired when an edit is confirmed (Enter / focus leaves the field);
  // Escape cancels and restores the saved value instead.
  std::function<void()> on_text_commit;
  // Shown in gray while the field is empty (an unset value).
  std::string placeholder;
  // Reset-to-default handler (Y / R), unset when the row has no default.
  std::function<void()> reset;
};

// Per-frame navigation intents merged from pad and keyboard.
struct SettingsMenu::NavIntents {
  int move_x = 0;
  int move_y = 0;
  bool select = false;
  bool back = false;
  bool category_prev = false;
  bool category_next = false;
  bool reset_default = false;
  bool apply_restart = false;
};

SettingsMenu::SettingsMenu(ImGuiDrawer* drawer, std::filesystem::path config_path,
                           Callback close_settings, Callback close_game,
                           Callback restart_game, PollGamepadCallback poll_gamepad,
                           SettingsMenuEnv env)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      close_settings_(std::move(close_settings)),
      close_game_(std::move(close_game)),
      restart_game_(std::move(restart_game)),
      poll_gamepad_(std::move(poll_gamepad)),
      env_(std::move(env)) {
  g_server_hosts = env_.server_hosts;
  if (BootValues().empty()) {
    CaptureSettingsBootValues();
  }
  LoadSettingsFromCvars();
  SetDrawActive(false);
}

SettingsMenu::~SettingsMenu() = default;

void SettingsMenu::Show() {
  visible_ = true;
  SetDrawActive(true);
  LoadSettingsFromCvars();
  zone_ = FocusZone::kRail;
  rail_sel_ = category_;
  row_index_ = 0;
  content_scroll_ = 0.0f;
  content_scroll_anim_ = 0.0f;
  editing_text_ = false;
  highlight_anim_y_ = -1.0f;
  rail_anim_y_ = -1.0f;
  prev_pad_buttons_ = 0xFFFF;  // swallow buttons already held at open
  just_shown_ = true;          // swallow the stale cursor delta too
}

void SettingsMenu::LoadSettingsFromCvars() {
  zd_debug_features_ = HasCvar("zd_debug") && rex::cvar::Query<bool>("zd_debug");
  device_list_ = GetGraphicsDeviceList();
  graphics_api_index_ = HasGraphicsApiChoice() ? GraphicsApiIndexFromCvar() : 0;
  device_index_ = DeviceIndexFromCvar(device_list_);
  resolution_index_ = ResolutionIndexFromCvars();
  aspect_index_ = AspectIndexFromCvar();
  frame_cap_index_ = FrameCapIndexFromCvars();
  {
    const double cap = rex::cvar::Query<double>("skate3_guest_fps_cap");
    CopyToBuffer(frame_cap_buf_, sizeof(frame_cap_buf_), cap >= 1.0 ? FormatFrameCap(cap) : "");
  }
  interframes_index_ =
      HasCvar("skate3_native_render_scene_interframes")
          ? std::clamp(rex::cvar::Query<int32_t>("skate3_native_render_scene_interframes"), 0, 3)
          : 0;
  msaa_index_ = MsaaIndexFromSamples(
      rex::cvar::Query<int32_t>("skate3_native_render_scene_msaa"));
  shadow_quality_index_ = ShadowQualityIndexFrom(
      rex::cvar::Query<bool>("skate3_native_render_scene_shadows"),
      rex::cvar::Query<int32_t>("skate3_native_render_scene_shadow_tile"));
  monitor_index_ = HasCvar("monitor")
                       ? std::clamp(rex::cvar::Query<int32_t>("monitor"), 0,
                                    int(kMonitorLabels.size()) - 1)
                       : 0;
  audio_buffer_index_ =
      HasCvar("audio_device_sample_frames")
          ? AudioBufferIndexFromFrames(rex::cvar::Query<int32_t>("audio_device_sample_frames"))
          : 0;
  language_index_ =
      HasCvar("user_language")
          ? std::clamp(int(rex::cvar::Query<uint32_t>("user_language")), 1,
                       int(kLanguageLabels.size())) -
                1
          : 0;
  chord_custom_.clear();
  chord_index_ = HasCvar("menu_chord") ? MenuChordIndexFromCvar(&chord_custom_) : 0;
  input_backend_index_ = 0;  // 0 = auto, 1 = xinput, 2 = sdl
  if (HasCvar("input_backend")) {
    const std::string backend = rex::cvar::Query<std::string>("input_backend");
    input_backend_index_ = backend == "xinput" ? 1 : backend == "sdl" ? 2 : 0;
  }
  fullscreen_ = env_.is_fullscreen ? env_.is_fullscreen() : QueryBool("fullscreen");
  vsync_ = QueryBool("vsync");
  tearing_ = TearingFromCvar();
  letterbox_ = QueryBool("present_letterbox", true);
  renderer_native_ = QueryBool("skate3_native_render_scene", true);
  shadow_pcss_ = QueryBool("skate3_native_render_scene_shadow_pcss", true);
  mode_indicator_ = QueryBool("skate3_native_render_mode_indicator");
  fps_counter_ = QueryBool("show_fps_counter");
  mnk_mode_ = QueryBool("mnk_mode");
  mnk_capture_mouse_ = QueryBool("mnk_capture_mouse", true);
  audio_mute_ = QueryBool("audio_mute");
  rumble_ = QueryBool("hid_rumble_enabled", true);
  field_of_view_ =
      HasCvar("skate3_field_of_view")
          ? float(std::clamp(rex::cvar::Query<double>("skate3_field_of_view"), 40.0, 120.0))
          : 60.0f;
  vr_first_person_ = QueryBool("skate3_vr_first_person");
  const auto vr_percent = [](std::string_view name, double fallback, double lo, double hi) {
    return float(std::clamp(
        (HasCvar(name) ? rex::cvar::Query<double>(name) : fallback) * 100.0, lo, hi));
  };
  vr_curve_x_ = vr_percent("skate3_vr_hud_curve_x", 0.5, 0.0, 100.0);
  vr_curve_y_ = vr_percent("skate3_vr_hud_curve_y", 0.25, 0.0, 100.0);
  vr_fov_ = HasCvar("skate3_vr_fov")
                ? float(std::clamp(rex::cvar::Query<double>("skate3_vr_fov"), 50.0, 150.0))
                : 100.0f;
  vr_hud_scale_ = vr_percent("skate3_vr_hud_scale", 1.0, 30.0, 150.0);
  vr_render_scale_ = vr_percent("skate3_vr_render_scale", 1.0, 30.0, 200.0);
  {
    const int32_t samples =
        HasCvar("skate3_vr_msaa") ? rex::cvar::Query<int32_t>("skate3_vr_msaa") : 2;
    vr_msaa_index_ = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
  }
  draw_distance_index_ =
      HasCvar("skate3_draw_distance_scale")
          ? NearestDrawDistanceIndex(rex::cvar::Query<double>("skate3_draw_distance_scale"))
          : 0;
  mnk_sensitivity_ =
      HasCvar("mnk_sensitivity")
          ? float(std::clamp(rex::cvar::Query<double>("mnk_sensitivity"), 0.1, 5.0))
          : 1.0f;
  CopyToBuffer(server_buf_, sizeof(server_buf_), ServerAddressFromCvar());
  // The signed-in profile (skate3_profiles): its gamertag and the store's
  // auto sign-in switch.
  CopyToBuffer(username_buf_, sizeof(username_buf_),
               profiles::SelectedGamertag(env_.profiles_path));
  auto_sign_in_ = profiles::AutoSignIn(env_.profiles_path);
}

bool SettingsMenu::HasSettingsChanges() const {
  for (const auto& [name, boot_value] : BootValues()) {
    if (CvarString(name) != boot_value) {
      return true;
    }
  }
  return false;
}

void SettingsMenu::SaveConfig() {
  rex::cvar::SaveConfigValues(config_path_, GetSettingsCvars());
}

void SettingsMenu::Toggle() {
  if (visible_) {
    Hide();
    return;
  }
  Show();
}

void SettingsMenu::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  editing_text_ = false;
  SetDrawActive(false);
  // Slider drags write their cvar per step; persist once on close.
  SaveConfig();
  if (close_settings_) {
    close_settings_();
  }
}

void SettingsMenu::NavigateBack() {
  if (!visible_) {
    return;
  }
  if (editing_text_) {
    // Cancel: drop the uncommitted text, back to the saved values.
    editing_text_ = false;
    LoadSettingsFromCvars();
    return;
  }
  if (zone_ == FocusZone::kContent) {
    zone_ = FocusZone::kRail;
    return;
  }
  Hide();
}

void SettingsMenu::ApplyAndRestart() {
  SaveConfig();
  if (restart_game_) {
    restart_game_();
  }
}

void SettingsMenu::BuildRows(std::vector<RowSpec>& rows, int category) {
  rows.clear();
  const bool pending = HasSettingsChanges();

  // Section bars only SEPARATE groups: a category's first group starts
  // directly with its rows, so a leading header is dropped.
  auto header = [&rows](const char* label) {
    if (rows.empty()) {
      return;
    }
    RowSpec row;
    row.kind = RowSpec::kHeader;
    row.label = label;
    rows.push_back(std::move(row));
  };
  // Live on/off row bound to one bool cvar.
  auto bool_row = [this, &rows](const char* label, const char* desc, bool* flag,
                                const char* cvar, bool fallback_default,
                                std::vector<std::string> options = {"Off", "On"}) {
    RowSpec row;
    row.kind = RowSpec::kEnum;
    row.label = label;
    row.desc = desc;
    row.options = std::move(options);
    row.flag = flag;
    row.on_enum_change = [this, cvar](int value) {
      SetBoolCvar(cvar, value != 0);
      SaveConfig();
    };
    row.reset = [this, flag, cvar, fallback_default] {
      *flag = CvarDefaultBool(cvar, fallback_default);
      SetBoolCvar(cvar, *flag);
      SaveConfig();
    };
    rows.push_back(std::move(row));
  };

  switch (category) {
    case 0: {  // Video
      header("Display");
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Fullscreen";
        row.desc = "Borderless fullscreen on the current monitor, or a resizable window. "
                   "Same as Alt+Enter. Applies immediately.";
        row.options = {"Off", "On"};
        row.flag = &fullscreen_;
        row.on_enum_change = [this](int value) {
          if (env_.set_fullscreen) env_.set_fullscreen(value != 0);
          SetBoolCvar("fullscreen", value != 0);  // also the next launch
          SaveConfig();
        };
        row.reset = [this] {
          fullscreen_ = CvarDefaultBool("fullscreen", true);
          if (env_.set_fullscreen) env_.set_fullscreen(fullscreen_);
          SetBoolCvar("fullscreen", fullscreen_);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      {
        // Live resolution: the native renderer re-sizes on the next frame.
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Resolution";
        row.desc =
            "Resolution the game renders at. Window renders at your window's exact "
            "pixel size. A fixed resolution is scaled to fit the window; the "
            "height presets take their width from the Aspect Ratio setting. "
            "Applies immediately.";
        for (const auto& preset : kResolutionPresets) {
          row.options.push_back(preset.label);
        }
        if (resolution_index_ == int(kResolutionPresets.size())) {
          row.options.push_back(
              "Custom (" + std::to_string(rex::cvar::Query<int32_t>("skate3_render_width")) +
              " x " + std::to_string(rex::cvar::Query<int32_t>("skate3_render_height")) + ")");
        }
        uint32_t ow = 0;
        uint32_t oh = 0;
        render_resolution::LastOutputSize(ow, oh);
        if (ow != 0 && oh != 0) {
          row.desc_extra = "Currently rendering at " + std::to_string(ow) + " x " +
                           std::to_string(oh) + ".";
        }
        row.index = &resolution_index_;
        row.on_enum_change = [this](int value) {
          if (value < int(kResolutionPresets.size())) {
            SetCvar("skate3_render_width", std::to_string(kResolutionPresets[value].width));
            SetCvar("skate3_render_height", std::to_string(kResolutionPresets[value].height));
            SaveConfig();
          }
        };
        row.reset = [this] {
          resolution_index_ = 0;
          SetCvar("skate3_render_width", "0");
          SetCvar("skate3_render_height", "0");
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Aspect Ratio";
        row.desc =
            "Window follows the window's shape. A fixed ratio builds the game camera "
            "for it (wider than 16:9 shows more to the sides, narrower more above "
            "and below) and fits the picture in the window. The HUD stays 16:9. "
            "Applies immediately.";
        for (const char* label : kAspectLabels) {
          row.options.push_back(label);
        }
        if (aspect_index_ == int(kAspectSpecs.size())) {
          row.options.push_back("Custom (" +
                                rex::cvar::Query<std::string>("skate3_render_aspect") + ")");
        }
        row.index = &aspect_index_;
        row.on_enum_change = [this](int value) {
          if (value < int(kAspectSpecs.size())) {
            SetCvar("skate3_render_aspect", kAspectSpecs[value]);
            SaveConfig();
          }
        };
        row.reset = [this] {
          aspect_index_ = 0;
          SetCvar("skate3_render_aspect", "window");
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("present_letterbox")) {
        bool_row("Scaling",
                 "How a picture whose shape differs from the window is shown: kept "
                 "in proportion with black bars, or stretched to fill. Applies "
                 "immediately.",
                 &letterbox_, "present_letterbox", true, {"Stretch", "Black bars"});
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Framerate Cap";
        row.desc =
            "Limit how fast the game runs. A steady cap slightly below your display's "
            "refresh rate gives the smoothest pacing on variable-refresh displays; "
            "Auto tracks the current display and does exactly that. Applies "
            "immediately.";
        const float auto_cap = Window::AutoFrameCapHz(Window::CachedDisplayRefreshHz());
        row.options.push_back(auto_cap > 0.0f
                                  ? "Auto (" + std::to_string(int(auto_cap)) + " FPS)"
                                  : std::string("Auto"));
        for (double rate : kFrameCapRates) {
          row.options.push_back(std::to_string(int(rate)) + " FPS");
        }
        row.options.push_back("Unlimited");
        const double custom = ParseFrameCap(frame_cap_buf_);
        row.options.push_back(custom > 0.0 ? "Custom (" + FormatFrameCap(custom) + " FPS)"
                                           : std::string("Custom"));
        row.index = &frame_cap_index_;
        row.on_enum_change = [this](int value) {
          ApplyFrameCapIndex(value, ParseFrameCap(frame_cap_buf_));
          SaveConfig();
        };
        row.reset = [this] {
          frame_cap_index_ =
              CvarDefaultBool("skate3_guest_fps_cap_auto", false)
                  ? kFrameCapAutoIndex
                  : FrameCapIndexFromRate(CvarDefaultDouble("skate3_guest_fps_cap", 0.0));
          ApplyFrameCapIndex(frame_cap_index_, ParseFrameCap(frame_cap_buf_));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (frame_cap_index_ == kFrameCapCustomIndex) {
        RowSpec row;
        row.kind = RowSpec::kText;
        row.label = "Custom Framerate Cap";
        row.desc =
            "Type any cap in frames per second (20 to 1000, decimals allowed), then "
            "Enter. With frame interpolation, half your display's refresh rate gives "
            "exactly two presents per game frame (72 on a 144 Hz display).";
        row.placeholder = "e.g. 72";
        row.text_buf = frame_cap_buf_;
        row.text_buf_size = sizeof(frame_cap_buf_);
        row.on_text_commit = [this] {
          const double rate = ParseFrameCap(frame_cap_buf_);
          // Show what was applied (clamped, normalized), or clear a non-number.
          CopyToBuffer(frame_cap_buf_, sizeof(frame_cap_buf_),
                       rate > 0.0 ? FormatFrameCap(rate) : "");
          ApplyFrameCapIndex(kFrameCapCustomIndex, rate);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_native_render_scene_interframes")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Frame Interpolation";
        row.desc =
            "Presents extra GENERATED frames between the game's real frames, with the "
            "camera and moving objects blended between the last two real frames, to "
            "fill the gaps up to your monitor's refresh rate. It switches itself on "
            "only when the game runs below the refresh rate (for example with a frame "
            "cap) and uses at most the number chosen here; at higher frame rates it "
            "generates nothing. These are not real game frames: the game does not run "
            "faster, and while active it adds about one game frame of latency. Native "
            "renderer only. Applies immediately.";
        row.options = {"Off", "Up to 1 generated frame", "Up to 2 generated frames",
                       "Up to 3 generated frames"};
        row.index = &interframes_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("skate3_native_render_scene_interframes", std::to_string(value));
          SaveConfig();
        };
        row.reset = [this] {
          const auto def = CvarDefault("skate3_native_render_scene_interframes");
          interframes_index_ =
              def ? std::clamp(int(std::strtol(def->c_str(), nullptr, 10)), 0, 3) : 0;
          SetCvar("skate3_native_render_scene_interframes", std::to_string(interframes_index_));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("vsync")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "V-Sync";
        row.desc =
            "Waits for the display before presenting frames. Adds latency; prefer "
            "the Framerate Cap with VRR. Applies immediately.";
        row.value_note = "Not recommended";
        row.options = {"Off", "On"};
        row.flag = &vsync_;
        row.on_enum_change = [this](int value) {
          SetBoolCvar("vsync", value != 0);
          SaveConfig();
        };
        row.reset = [this] {
          vsync_ = CvarDefaultBool("vsync", false);
          SetBoolCvar("vsync", vsync_);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "VRR / Tearing";
        row.desc =
            "Allows uncapped presentation on variable-refresh-rate displays "
            "(G-Sync / FreeSync). Recommended on.";
        if (D3D12Active()) {
          row.value_note = kNextLaunch;
        }
        row.options = {"Off", "On"};
        row.flag = &tearing_;
        row.on_enum_change = [this](int value) {
          SetTearingCvars(value != 0);
          SaveConfig();
        };
        row.reset = [this] {
          tearing_ = D3D12Active()
                         ? CvarDefaultBool("d3d12_allow_variable_refresh_rate_and_tearing", true)
                         : CvarDefaultBool("vulkan_allow_present_mode_immediate", true);
          SetTearingCvars(tearing_);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("monitor")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Startup Monitor";
        row.desc =
            "Which display the game window opens on. Auto uses the primary display. "
            "To move the running game, drag the window or use Win+Shift+Arrow.";
        row.value_note = kNextLaunch;
        for (const char* label : kMonitorLabels) {
          row.options.push_back(label);
        }
        row.index = &monitor_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("monitor", std::to_string(value));
          SaveConfig();
        };
        row.reset = [this] {
          monitor_index_ = int(CvarDefaultDouble("monitor", 0.0));
          SetCvar("monitor", std::to_string(monitor_index_));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasGraphicsApiChoice()) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Graphics API";
        row.desc =
            "Which graphics API the game renders through. Both perform similarly; "
            "try the other one if you run into driver issues.";
        row.value_note = kNextLaunch;
        row.options = {"DirectX 12", "Vulkan"};
        row.index = &graphics_api_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("gpu_backend", value == 1 ? "vulkan" : "d3d12");
          SaveConfig();
        };
        row.reset = [this] {
          graphics_api_index_ = D3D12Active() ? 0 : 1;
          SetCvar("gpu_backend", CvarDefault("gpu_backend").value_or("auto"));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (!device_list_.device_names.empty() && HasCvar(device_list_.cvar_name)) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Graphics Device";
        row.desc = "Which GPU renders the game. The fastest adapter is picked automatically.";
        row.value_note = kNextLaunch;
        for (size_t i = 0; i < device_list_.device_names.size(); ++i) {
          row.options.push_back(std::to_string(i) + ": " + device_list_.device_names[i]);
        }
        row.index = &device_index_;
        row.on_enum_change = [this](int value) {
          // Re-picking the automatically chosen adapter keeps the cvar on
          // automatic, so new hardware still re-picks the best one.
          SetCvar(device_list_.cvar_name,
                  value == device_list_.active_index ? "-1" : std::to_string(value));
          SaveConfig();
        };
        row.reset = [this] {
          device_index_ = std::max(device_list_.active_index, 0);
          SetCvar(device_list_.cvar_name, "-1");
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }

      header("Graphics");
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Renderer";
        row.desc =
            "Native draws the game directly on your GPU, the way the console's "
            "shaders do (much faster); Emulated replays the console GPU command by "
            "command. Same as F5. Applies immediately.";
        row.options = {"Emulated", "Native"};
        row.flag = &renderer_native_;
        row.enabled = QueryBool("skate3_native_render", false);
        if (!row.enabled) {
          row.desc_extra = "The native renderer is disabled for this session.";
        }
        row.on_enum_change = [this](int value) {
          if ((value != 0) != QueryBool("skate3_native_render_scene", true)) {
            native_scene::ToggleSceneEnabled();
          }
          renderer_native_ = QueryBool("skate3_native_render_scene", true);
          SaveConfig();
        };
        row.reset = [this] {
          const bool want = CvarDefaultBool("skate3_native_render_scene", true);
          if (want != QueryBool("skate3_native_render_scene", true)) {
            native_scene::ToggleSceneEnabled();
          }
          renderer_native_ = QueryBool("skate3_native_render_scene", true);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Anti-Aliasing (MSAA)";
        row.desc =
            "Multisampling for the native renderer. Smooths distant thin geometry "
            "(railings, wires) that shimmers otherwise. Applies immediately.";
        for (const char* label : kMsaaLabels) {
          row.options.push_back(label);
        }
        row.index = &msaa_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("skate3_native_render_scene_msaa", std::to_string(kMsaaSamples[value]));
          SaveConfig();
        };
        row.reset = [this] {
          msaa_index_ = MsaaIndexFromSamples(
              int32_t(CvarDefaultDouble("skate3_native_render_scene_msaa", 4.0)));
          SetCvar("skate3_native_render_scene_msaa", std::to_string(kMsaaSamples[msaa_index_]));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Shadow Quality";
        row.desc =
            "Resolution of the game's character and prop shadows. 512 is the "
            "original console resolution; Auto scales it with the render "
            "resolution, matching the emulated renderer. Applies immediately.";
        uint32_t ow = 0;
        uint32_t oh = 0;
        render_resolution::LastOutputSize(ow, oh);
        for (size_t i = 0; i < kShadowQualityLabels.size(); ++i) {
          if (i == 1 && oh != 0) {
            const uint32_t auto_tile = std::min(512u * std::max(1u, (oh + 719u) / 720u), 4096u);
            row.options.push_back("Auto (" + std::to_string(auto_tile) + ")");
          } else {
            row.options.push_back(kShadowQualityLabels[i]);
          }
        }
        row.index = &shadow_quality_index_;
        row.on_enum_change = [this](int value) {
          ApplyShadowQualityIndex(value);
          SaveConfig();
        };
        row.reset = [this] {
          shadow_quality_index_ = ShadowQualityIndexFrom(
              CvarDefaultBool("skate3_native_render_scene_shadows", true),
              int32_t(CvarDefaultDouble("skate3_native_render_scene_shadow_tile", 0.0)));
          ApplyShadowQualityIndex(shadow_quality_index_);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      bool_row("Shadow Softness",
               "Original uses the game's fixed-width shadow filter. Contact-hardening "
               "keeps shadows crisp where they touch their caster and softens them "
               "with distance. Applies immediately.",
               &shadow_pcss_, "skate3_native_render_scene_shadow_pcss", true,
               {"Original", "Contact-hardening"});

      if (HasCvar("skate3_draw_distance_scale") && HasCvar("skate3_lod_distance_scale")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Draw Distance";
        row.desc =
            "How far away small world objects, foliage and character detail stay "
            "visible, as a multiple of the original console distances. Higher "
            "settings draw more of the world and cost some performance. Applies "
            "immediately.";
        for (const char* label : kDrawDistanceLabels) {
          row.options.push_back(label);
        }
        row.index = &draw_distance_index_;
        row.on_enum_change = [this](int value) {
          value = std::clamp(value, 0, int(kDrawDistanceScales.size()) - 1);
          const std::string scale = std::to_string(kDrawDistanceScales[size_t(value)]);
          SetCvar("skate3_draw_distance_scale", scale);
          SetCvar("skate3_lod_distance_scale", scale);
          SaveConfig();
        };
        row.reset = [this] {
          draw_distance_index_ =
              NearestDrawDistanceIndex(CvarDefaultDouble("skate3_draw_distance_scale", 2.0));
          const std::string scale =
              std::to_string(kDrawDistanceScales[size_t(draw_distance_index_)]);
          SetCvar("skate3_draw_distance_scale", scale);
          SetCvar("skate3_lod_distance_scale", scale);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("skate3_field_of_view")) {
        RowSpec row;
        row.kind = RowSpec::kSlider;
        row.label = "Field of View";
        row.desc = "Camera field of view in degrees. Applies immediately.";
        row.value = &field_of_view_;
        row.min = 40.0f;
        row.max = 120.0f;
        row.step = 2.0f;
        row.fmt = "%.0f";
        row.on_value_change = [this] {
          field_of_view_ = std::clamp(field_of_view_, 40.0f, 120.0f);
          SetCvar("skate3_field_of_view", std::to_string(field_of_view_));
          SaveConfig();
        };
        row.reset = [this] {
          field_of_view_ = float(CvarDefaultDouble("skate3_field_of_view", 60.0));
          SetCvar("skate3_field_of_view", std::to_string(field_of_view_));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      header("Interface");
      if (HasCvar("show_fps_counter")) {
        bool_row("FPS Counter", "Show the framerate overlay in the corner. Applies immediately.",
                 &fps_counter_, "show_fps_counter", false);
      }
      bool_row("Renderer Indicator",
               "Small corner readout of which renderer produced the last frame (NATIVE "
               "or EMULATED). Always shown while the Renderer is Emulated. Applies "
               "immediately.",
               &mode_indicator_, "skate3_native_render_mode_indicator", false);
      break;
    }
    case 1: {  // Controls
      header("Mouse & Keyboard");
      bool_row("Mouse & Keyboard Mode",
               "Emulates a controller from mouse and keyboard input. Applies immediately.",
               &mnk_mode_, "mnk_mode", false);
      bool_row("Capture Mouse",
               "Locks the mouse cursor to the game window while Mouse & Keyboard Mode "
               "is active. Applies immediately.",
               &mnk_capture_mouse_, "mnk_capture_mouse", true);
      if (HasCvar("mnk_sensitivity")) {
        RowSpec row;
        row.kind = RowSpec::kSlider;
        row.label = "Mouse Sensitivity";
        row.desc = "Mouse-look speed while Mouse & Keyboard Mode is active. Applies immediately.";
        row.value = &mnk_sensitivity_;
        row.min = 0.1f;
        row.max = 5.0f;
        row.step = 0.1f;
        row.fmt = "%.1f";
        row.on_value_change = [this] {
          mnk_sensitivity_ = std::clamp(mnk_sensitivity_, 0.1f, 5.0f);
          SetCvar("mnk_sensitivity", std::to_string(mnk_sensitivity_));
        };
        row.reset = [this] {
          mnk_sensitivity_ = float(CvarDefaultDouble("mnk_sensitivity", 1.0));
          SetCvar("mnk_sensitivity", std::to_string(mnk_sensitivity_));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      header("Controller");
      if (HasCvar("hid_rumble_enabled")) {
        bool_row("Vibration", "Controller rumble. Applies immediately.", &rumble_,
                 "hid_rumble_enabled", true);
      }
      if (HasCvar("menu_chord")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Settings Menu Shortcut";
        row.desc = "Controller button chord that opens this menu. Applies immediately.";
        for (const char* label : kMenuChordLabels) {
          row.options.push_back(label);
        }
        if (!chord_custom_.empty()) {
          row.options.push_back("Custom: " + chord_custom_);
        }
        row.index = &chord_index_;
        row.on_enum_change = [this](int value) {
          if (value < int(kMenuChordSpecs.size())) {
            SetCvar("menu_chord", kMenuChordSpecs[value]);
          } else if (!chord_custom_.empty()) {
            SetCvar("menu_chord", chord_custom_);
          }
          SaveConfig();
        };
        row.reset = [this] {
          chord_index_ = 0;
          SetCvar("menu_chord", kMenuChordSpecs[0]);
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("input_backend")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Controller Backend";
        row.desc =
            "Which API reads controllers. Auto uses XInput for Xbox controllers "
            "and SDL for everything else (DualSense, DualShock 4, Switch Pro and "
            "most generic controllers, USB or Bluetooth) at the same time. "
            "XInput: Xbox controllers only. SDL: every controller through SDL.";
        row.value_note = kNextLaunch;
        row.options = {"Auto", "XInput", "SDL"};
        row.index = &input_backend_index_;
        row.on_enum_change = [this](int value) {
          static constexpr const char* kValues[] = {"auto", "xinput", "sdl"};
          SetCvar("input_backend", kValues[std::clamp(value, 0, 2)]);
          SaveConfig();
        };
        row.reset = [this] {
          input_backend_index_ = 0;
          SetCvar("input_backend", "auto");
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 2: {  // Audio
      if (HasCvar("audio_mute")) {
        bool_row("Mute", "Silence all game audio. Applies immediately.", &audio_mute_,
                 "audio_mute", false);
      }
      if (HasCvar("audio_device_sample_frames")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Audio Buffer Size";
        row.desc =
            "Audio device buffer length. Larger buffers add a little latency but fix "
            "crackling on machines with scheduling hiccups.";
        row.value_note = kNextLaunch;
        for (size_t i = 0; i < kAudioBufferLabels.size(); ++i) {
          if (i == 0) {
            const int32_t auto_frames = rex::audio::AutoDeviceSampleFrames();
            row.options.push_back(auto_frames > 0
                                      ? "Auto (" + std::to_string(auto_frames) + ")"
                                      : std::string("Auto"));
          } else {
            row.options.push_back(kAudioBufferLabels[i]);
          }
        }
        row.index = &audio_buffer_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("audio_device_sample_frames", std::to_string(kAudioBufferFrames[value]));
          SaveConfig();
        };
        row.reset = [this] {
          audio_buffer_index_ = AudioBufferIndexFromFrames(
              int32_t(CvarDefaultDouble("audio_device_sample_frames", 0.0)));
          SetCvar("audio_device_sample_frames",
                  std::to_string(kAudioBufferFrames[audio_buffer_index_]));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 3: {  // Online
      if (HasCvar("net_host_overrides")) {
        RowSpec row;
        row.kind = RowSpec::kText;
        row.label = "Server Address";
        row.desc =
            "IP address or host name of the custom Skate 3 server. Select to edit, "
            "Enter to save; leave empty to use the game's original servers. "
            "Applies to the next connection.";
        row.desc_extra = "Points ";
        for (size_t i = 0; i < g_server_hosts.size(); ++i) {
          row.desc_extra += std::string(i ? ", " : "") + g_server_hosts[i];
        }
        row.desc_extra += " at this address.";
        row.text_buf = server_buf_;
        row.text_buf_size = sizeof(server_buf_);
        row.on_text_commit = [this] {
          SetServerAddress(Trim(server_buf_));
          CopyToBuffer(server_buf_, sizeof(server_buf_), ServerAddressFromCvar());
          SaveConfig();
        };
        // No reset: the cvar default is "no override", which would silently
        // disconnect the game from the custom server.
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kText;
        row.label = "Username";
        row.desc =
            "The gamertag of the signed-in profile: the name the server logs you in "
            "as and other players see. Select to edit, Enter to save (up to 15 "
            "letters, numbers or spaces). Saves stay with the profile.";
        row.desc_extra =
            "Applies to the next connection to the server. Profiles are created, "
            "switched and deleted on the profile screen at startup.";
        row.text_buf = username_buf_;
        row.text_buf_size = sizeof(username_buf_);
        row.on_text_commit = [this] {
          if (profiles::RenameSelected(env_.profiles_path, username_buf_) &&
              env_.profile_changed) {
            env_.profile_changed();
          }
          // Show what was stored (sanitized), or the old name if rejected.
          CopyToBuffer(username_buf_, sizeof(username_buf_),
                       profiles::SelectedGamertag(env_.profiles_path));
        };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Sign In Automatically";
        row.desc =
            "On: the last profile signs in by itself when the game starts. Off: the "
            "profile screen opens on every start to choose, create, rename or delete "
            "profiles.";
        row.value_note = kNextLaunch;
        row.options = {"Off", "On"};
        row.flag = &auto_sign_in_;
        row.on_enum_change = [this](int value) {
          profiles::SetAutoSignIn(env_.profiles_path, value != 0);
        };
        row.reset = [this] {
          auto_sign_in_ = true;
          profiles::SetAutoSignIn(env_.profiles_path, true);
        };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 4: {  // System
      if (HasCvar("user_language")) {
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Game Language";
        row.desc = "Language the game boots in. Skate 3 reads it once at startup.";
        row.value_note = kNextLaunch;
        for (const char* label : kLanguageLabels) {
          row.options.push_back(label);
        }
        row.index = &language_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("user_language", std::to_string(value + 1));
          SaveConfig();
        };
        row.reset = [this] {
          language_index_ = std::clamp(int(CvarDefaultDouble("user_language", 1.0)), 1,
                                       int(kLanguageLabels.size())) -
                            1;
          SetCvar("user_language", std::to_string(language_index_ + 1));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      if (HasCvar("zd_debug")) {
        header("Debugging");
        bool_row("ZD debugging features",
                 "Enable the extra debug menus, visualizations and diagnostics from "
                 "the debug build. Changes apply immediately and are saved.",
                 &zd_debug_features_, "zd_debug", true);
        rows.back().checkbox = true;
      }
      header("Session");
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Restart Now";
        row.desc = pending
                       ? "Restart the game so the settings marked Next launch take effect. "
                         "Unsaved game progress is lost."
                       : "Every setting is already in effect; nothing needs a restart.";
        row.enabled = pending;
        row.action = [this] { ApplyAndRestart(); };
        rows.push_back(std::move(row));
      }
      {
        RowSpec row;
        row.kind = RowSpec::kAction;
        row.label = "Close Settings";
        row.desc = "Return to the game.";
        row.action = [this] { Hide(); };
        rows.push_back(std::move(row));
      }
      break;
    }
    case 5: {  // VR
      bool_row("First-Person View",
               "See through the skater's eyes instead of the game camera. Also toggled with "
               "left stick click + X on the VR controllers or a gamepad.",
               &vr_first_person_, "skate3_vr_first_person", false);
      // Percent slider over a 0..1 (or percent) cvar.
      auto percent_row = [this, &rows](const char* label, const char* desc, float* value,
                                       const char* cvar, float lo, float hi, float step,
                                       double scale, double fallback) {
        RowSpec row;
        row.kind = RowSpec::kSlider;
        row.label = label;
        row.desc = desc;
        row.value = value;
        row.min = lo;
        row.max = hi;
        row.step = step;
        row.fmt = "%.0f%%";
        row.on_value_change = [this, value, cvar, lo, hi, scale] {
          *value = std::clamp(*value, lo, hi);
          SetCvar(cvar, std::to_string(double(*value) / scale));
          SaveConfig();
        };
        row.reset = [this, value, cvar, scale, fallback] {
          *value = float(CvarDefaultDouble(cvar, fallback) * scale);
          SetCvar(cvar, std::to_string(double(*value) / scale));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      };
      percent_row("Field of View",
                  "How much of the world the headset shows at the game's normal camera. "
                  "100% = the headset's own view; higher zooms out, lower zooms in. Game "
                  "camera zooms still apply on top.",
                  &vr_fov_, "skate3_vr_fov", 50.0f, 150.0f, 5.0f, 1.0, 100.0);
      header("Performance");
      percent_row("Render Resolution",
                  "Per-eye resolution relative to SteamVR's recommended size (which already "
                  "includes SteamVR's own supersampling). VR renders two eye images every "
                  "frame, so this is the main frame-rate lever: lower it first if the headset "
                  "drops frames. Applies immediately.",
                  &vr_render_scale_, "skate3_vr_render_scale", 30.0f, 200.0f, 5.0f, 100.0, 1.0);
      {
        static constexpr int32_t kVrMsaa[4] = {1, 2, 4, 8};
        RowSpec row;
        row.kind = RowSpec::kEnum;
        row.label = "Anti-Aliasing";
        row.desc =
            "Multisample anti-aliasing in the headset (the flat Anti-Aliasing setting still "
            "applies when it is lower). Both eyes render at headset resolution, so 4x / 8x "
            "cost far more than on a monitor. Applies immediately.";
        row.options = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        row.index = &vr_msaa_index_;
        row.on_enum_change = [this](int value) {
          SetCvar("skate3_vr_msaa", std::to_string(kVrMsaa[std::clamp(value, 0, 3)]));
          SaveConfig();
        };
        row.reset = [this] {
          const int32_t samples = int32_t(CvarDefaultDouble("skate3_vr_msaa", 2.0));
          vr_msaa_index_ = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
          SetCvar("skate3_vr_msaa", std::to_string(kVrMsaa[vr_msaa_index_]));
          SaveConfig();
        };
        rows.push_back(std::move(row));
      }
      header("HUD");
      percent_row("HUD Curvature X",
                  "How much the third-person HUD wraps around you horizontally. 0% = a flat "
                  "panel, 100% = a sphere centred on the camera.",
                  &vr_curve_x_, "skate3_vr_hud_curve_x", 0.0f, 100.0f, 5.0f, 100.0, 0.5);
      percent_row("HUD Curvature Y",
                  "How much the third-person HUD wraps around you vertically. 0% = flat, "
                  "100% = a sphere centred on the camera.",
                  &vr_curve_y_, "skate3_vr_hud_curve_y", 0.0f, 100.0f, 5.0f, 100.0, 0.25);
      percent_row("HUD Size",
                  "Size of the third-person HUD relative to the game's own screen. Lower "
                  "keeps corner elements out of the lens edges.",
                  &vr_hud_scale_, "skate3_vr_hud_scale", 30.0f, 150.0f, 5.0f, 100.0, 1.0);
      break;
    }
    default:
      break;
  }
}

SettingsMenu::NavIntents SettingsMenu::GatherInput(ImGuiIO& io) {
  NavIntents in;

  // ---- Gamepad ----
  SimpleSettingsGamepad pad;
  if (poll_gamepad_) {
    pad = poll_gamepad_();
  }
  // prev_pad_buttons_ starts 0xFFFF on Show so buttons held while opening
  // (e.g. the Back+Start chord) don't fire actions on the first frame.
  const uint16_t pressed = pad.buttons & ~prev_pad_buttons_;
  prev_pad_buttons_ = pad.buttons;

  int dir_x = 0;
  int dir_y = 0;
  if (pad.connected) {
    if (pad.buttons & kPadDpadLeft) {
      dir_x -= 1;
    }
    if (pad.buttons & kPadDpadRight) {
      dir_x += 1;
    }
    if (pad.buttons & kPadDpadUp) {
      dir_y -= 1;
    }
    if (pad.buttons & kPadDpadDown) {
      dir_y += 1;
    }
    constexpr int16_t kDeadzone = 12000;
    if (dir_x == 0) {
      dir_x = pad.thumb_lx <= -kDeadzone ? -1 : (pad.thumb_lx >= kDeadzone ? 1 : 0);
    }
    if (dir_y == 0) {
      // Stick up is positive Y in XInput; menu "up" is -1.
      dir_y = pad.thumb_ly >= kDeadzone ? -1 : (pad.thumb_ly <= -kDeadzone ? 1 : 0);
    }
  }

  auto repeat_axis = [&io](int dir, int& held, float& timer) -> int {
    if (dir == 0) {
      held = 0;
      timer = 0.0f;
      return 0;
    }
    if (dir != held) {
      held = dir;
      timer = 0.0f;
      return dir;  // initial press
    }
    timer += io.DeltaTime;
    if (timer >= kRepeatDelay) {
      timer -= kRepeatRate;
      return dir;
    }
    return 0;
  };
  in.move_x = repeat_axis(dir_x, held_dir_x_, repeat_timer_x_);
  in.move_y = repeat_axis(dir_y, held_dir_y_, repeat_timer_y_);

  if (pressed & kPadA) {
    in.select = true;
  }
  if (pressed & kPadB) {
    in.back = true;
  }
  if (pressed & kPadY) {
    in.reset_default = true;
  }
  if (pressed & kPadX) {
    in.apply_restart = true;
  }
  if (pressed & kPadLShoulder) {
    in.category_prev = true;
  }
  if (pressed & kPadRShoulder) {
    in.category_next = true;
  }
  if (pressed != 0 || dir_x != 0 || dir_y != 0) {
    pad_active_ = true;
  }

  // ---- Keyboard (Escape arrives via the keybind -> NavigateBack) ----
  if (!editing_text_) {
    bool kb = false;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
      in.move_y -= 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
      in.move_y += 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
      in.move_x -= 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
      in.move_x += 1;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      in.select = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
      in.category_prev = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
      in.category_next = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
      in.reset_default = true;
      kb = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      in.apply_restart = true;
      kb = true;
    }
    if (kb) {
      pad_active_ = false;
    }
  }

  // Mouse motion switches the legend back to keyboard/mouse.
  if (std::abs(io.MousePos.x - mouse_x_) > 2.0f || std::abs(io.MousePos.y - mouse_y_) > 2.0f) {
    if (mouse_x_ >= 0.0f) {
      pad_active_ = false;
    }
  }

  return in;
}

void SettingsMenu::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    return;
  }

  ImFont* font = imgui_drawer()->ui_font() ? imgui_drawer()->ui_font() : ImGui::GetFont();
  ImFont* bold = imgui_drawer()->ui_font_semibold() ? imgui_drawer()->ui_font_semibold() : font;
  // Coverage-thinned variants for dark text on light panels (rows, section
  // bars, legend chips) - matches the browser's per-polarity text gamma.
  ImFont* bold_ol = imgui_drawer()->ui_font_semibold_on_light()
                        ? imgui_drawer()->ui_font_semibold_on_light()
                        : bold;

  // Layout frame: the letterboxed game image when the presenter reports one,
  // the full window otherwise (boot, no guest output yet). On non-16:9
  // displays the game letterboxes to 16:9 while the window is taller/wider -
  // scaling and anchoring the menu to the window made it read visibly larger
  // than on a 16:9 screen (user report, 16:10 Mac). On 16:9 displays the
  // rectangle equals the window and nothing changes. The blur/backdrop wash
  // still covers the whole window.
  ImVec2 frame_pos(0.0f, 0.0f);
  ImVec2 frame_size = io.DisplaySize;
  if (Presenter* presenter = imgui_drawer()->presenter()) {
    const Presenter::GuestOutputPaintRect rect = presenter->GetLastGuestOutputPaintRect();
    if (rect.width > 0 && rect.height > 0) {
      frame_pos = ImVec2(float(rect.x), float(rect.y));
      frame_size = ImVec2(float(rect.width), float(rect.height));
    }
  }
  // VR (skate3::vr::Active()): the headset shows the menu on its own panel
  // (skate3_vr MenuClipRows), cut from the window image: lay it out in a
  // 16:9 frame of the window (the window then mirrors the eyes, whose
  // letterbox rect is no layout frame) and report that frame.
  const bool vr_layout = skate3::vr::Active();
  if (vr_layout) {
    const float fw = std::min(io.DisplaySize.x, io.DisplaySize.y * (16.0f / 9.0f));
    const float fh = fw * (9.0f / 16.0f);
    frame_pos = ImVec2(std::floor(0.5f * (io.DisplaySize.x - fw)),
                       std::floor(0.5f * (io.DisplaySize.y - fh)));
    frame_size = ImVec2(fw, fh);
    skate3::vr::SetMenuRect(frame_pos.x, frame_pos.y, frame_size.x, frame_size.y);
  }
  const float frame_bottom = frame_pos.y + frame_size.y;

  // Uniform scale: design metrics are authored for 1080p. The menu proper
  // renders at a slightly reduced scale; the footer legend
  // keeps the base viewport scale.
  const float base_s = std::clamp(frame_size.y / 1080.0f, 0.6f, 3.0f);
  const float s = base_s * kMenuScale;

  // Quantize font sizes so the EM (size * upm / (hhea ascent - descent) -
  // the browser's font-size) lands on whole pixels. A fractional em renders
  // measurably wider letter spacing than the browser reference, which snaps
  // css font sizes to the device grid (+1.3% run width at 4K, 370px vs
  // 365px on "Vertical Synchronisation").
  constexpr float kEmPerSize = 2048.0f / 2478.0f;  // Inter upm / (asc - desc)
  auto font_px = [](float size) {
    const float em_quantized = std::round(size * kEmPerSize) / kEmPerSize;
    // Snap to the 1/64px grid GetFontBaked quantizes to (rexglue imgui
    // patch), so the baked size equals the drawn size EXACTLY and RenderText
    // never rescales glyph quads. Stock imgui rounded bakes to whole pixels:
    // the em-quantized 37.51 request baked at 38 and every glyph was
    // GPU-bilinear-minified by 0.987 - the measured ~0.4px vertical smear
    // that kept the menu slightly softer than intended. The 1/64
    // snap moves the em by <0.02% - spacing shift is sub-0.1px per run.
    return std::round(em_quantized * 64.0f) / 64.0f;
  };

  if (just_shown_) {
    // Ignore the pre-open cursor position delta (opening warps/reveals the
    // cursor): it otherwise reads as motion and steals focus from the rail
    // to whatever row happens to sit under the cursor.
    mouse_x_ = io.MousePos.x;
    mouse_y_ = io.MousePos.y;
    just_shown_ = false;
  }

  // ---- Input ----
  NavIntents in = GatherInput(io);
  if (editing_text_) {
    // The text field owns navigation; pad B cancels the edit.
    if (in.back) {
      editing_text_ = false;
    }
    in = NavIntents{};
  }

  const int category_count = static_cast<int>(kCategories.size());
  // The rail holds the categories plus a pinned Close Game entry at the
  // bottom (rail_sel_ == category_count).
  const int quit_rail_index = category_count;
  if (in.category_prev || in.category_next) {
    category_ = (category_ + (in.category_next ? 1 : category_count - 1)) % category_count;
    rail_sel_ = category_;
    row_index_ = 0;
    content_scroll_ = 0.0f;
    content_scroll_anim_ = 0.0f;
    highlight_anim_y_ = -1.0f;
  }
  if (zone_ == FocusZone::kRail && in.move_y != 0) {
    rail_sel_ = std::clamp(rail_sel_ + in.move_y, 0, quit_rail_index);
    if (rail_sel_ < category_count && rail_sel_ != category_) {
      category_ = rail_sel_;
      row_index_ = 0;
      content_scroll_ = 0.0f;
      content_scroll_anim_ = 0.0f;
      highlight_anim_y_ = -1.0f;
    }
  }

  std::vector<RowSpec> rows;
  BuildRows(rows, category_);

  // Selectable row indices (headers and disabled rows are skipped by nav).
  std::vector<int> selectable;
  selectable.reserve(rows.size());
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].kind != RowSpec::kHeader && rows[i].enabled) {
      selectable.push_back(i);
    }
  }
  auto clamp_focus_to_selectable = [&]() {
    if (selectable.empty()) {
      row_index_ = -1;
      return;
    }
    int best = selectable[0];
    int best_delta = 1 << 30;
    for (int idx : selectable) {
      int delta = std::abs(idx - row_index_);
      if (delta < best_delta) {
        best = idx;
        best_delta = delta;
      }
    }
    row_index_ = best;
  };
  clamp_focus_to_selectable();

  auto enum_value = [](const RowSpec& row) -> int {
    return row.flag ? (*row.flag ? 1 : 0) : (row.index ? *row.index : 0);
  };
  auto set_enum_value = [](RowSpec& row, int value) {
    if (row.flag) {
      *row.flag = value != 0;
    } else if (row.index) {
      *row.index = value;
    }
    if (row.on_enum_change) {
      row.on_enum_change(value);
    }
  };
  // Range semantics: directional stepping (dpad/arrows/chevrons)
  // CLAMPS at the ends - the matching chevron greys out there - while
  // activation (A/Enter/row click) cycles with wrap-around.
  auto step_row = [&](RowSpec& row, int dir) {
    if (row.kind == RowSpec::kEnum) {
      int count = static_cast<int>(row.options.size());
      if (count <= 0) {
        return;
      }
      int value = std::clamp(enum_value(row), 0, count - 1);
      set_enum_value(row, std::clamp(value + dir, 0, count - 1));
    } else if (row.kind == RowSpec::kSlider && row.value) {
      *row.value = std::clamp(*row.value + dir * row.step, row.min, row.max);
      if (row.on_value_change) {
        row.on_value_change();
      }
      SaveConfig();
    }
  };
  auto activate_row = [&](RowSpec& row) {
    if (!row.enabled) {
      return;
    }
    switch (row.kind) {
      case RowSpec::kEnum: {
        int count = static_cast<int>(row.options.size());
        if (count > 0) {
          int value = std::clamp(enum_value(row), 0, count - 1);
          set_enum_value(row, (value + 1) % count);
        }
        break;
      }
      case RowSpec::kAction:
        if (row.action) {
          row.action();
        }
        break;
      case RowSpec::kText:
        editing_text_ = true;
        text_edit_focus_pending_ = true;
        break;
      default:
        break;
    }
  };

  // ---- Apply navigation intents ----
  bool row_activated_this_frame = false;
  if (zone_ == FocusZone::kRail) {
    if (rail_sel_ == quit_rail_index) {
      if (in.select && close_game_) {
        Hide();
        close_game_();
      }
    } else if ((in.select || in.move_x > 0) && !selectable.empty()) {
      zone_ = FocusZone::kContent;
      row_index_ = selectable[0];
    }
    if (in.back) {
      Hide();
    }
  } else {
    if (in.move_y != 0 && !selectable.empty()) {
      int pos = 0;
      for (int i = 0; i < static_cast<int>(selectable.size()); ++i) {
        if (selectable[i] == row_index_) {
          pos = i;
          break;
        }
      }
      pos = std::clamp(pos + in.move_y, 0, static_cast<int>(selectable.size()) - 1);
      row_index_ = selectable[pos];
    }
    if (in.move_x != 0 && row_index_ >= 0) {
      step_row(rows[row_index_], in.move_x);
    }
    if (in.select && row_index_ >= 0) {
      activate_row(rows[row_index_]);
      row_activated_this_frame = true;
    }
    if (in.back) {
      zone_ = FocusZone::kRail;
    }
  }
  if (in.reset_default && zone_ == FocusZone::kContent && row_index_ >= 0 &&
      rows[row_index_].reset) {
    rows[row_index_].reset();
  }
  if (in.apply_restart && HasSettingsChanges()) {
    ApplyAndRestart();
  }
  if (!visible_) {
    return;  // an action above closed the dialog
  }
  // Actions can invalidate row layout (pending-state rows); rebuild.
  if (row_activated_this_frame || in.reset_default || in.apply_restart) {
    BuildRows(rows, category_);
    selectable.clear();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      if (rows[i].kind != RowSpec::kHeader && rows[i].enabled) {
        selectable.push_back(i);
      }
    }
    clamp_focus_to_selectable();
  }

  const bool pending = HasSettingsChanges();

  // ---- Layout ----
  // Positional metrics are snapped to whole pixels (see Snap).
  const float margin_x = Snap(std::max(56.0f * s, frame_size.x * 0.05f));
  const float title_size = font_px(42.0f * s);
  // Rail and description columns are equal-width.
  const float rail_w = Snap(300.0f * s);
  const float desc_w = Snap(300.0f * s);
  const float col_gap = Snap(14.0f * s);
  const float footer_h = Snap(96.0f * base_s);
  const float content_bottom = Snap(frame_bottom - footer_h - 18.0f * s);
  float content_w = frame_size.x - 2.0f * margin_x - rail_w - desc_w - 2.0f * col_gap;
  content_w = Snap(std::clamp(content_w, 300.0f * s, 800.0f * s));
  // The column widths are capped, so on wide displays the block would hug the
  // left margin - center the whole menu instead.
  const float menu_total_w = rail_w + content_w + desc_w + 2.0f * col_gap;
  const float rail_x =
      Snap(frame_pos.x + std::max(margin_x, (frame_size.x - menu_total_w) * 0.5f));
  const float content_x = rail_x + rail_w + col_gap;
  const float desc_x = content_x + content_w + col_gap;
  const float menu_right = desc_x + desc_w;

  const float row_h = Snap(52.0f * s);
  const float header_h = row_h;  // section bars match setting rows
  // Gap is 1 design px tighter than the 6*s focus ring, so the ring slightly
  // overlaps neighbours - it draws above them.
  const float row_gap = Snap(5.0f * s);
  const float rail_item_h = Snap(52.0f * s);
  const float label_size = font_px(22.0f * s);
  const float value_size = font_px(22.0f * s);
  const float desc_size = font_px(20.0f * s);
  // The content view holds an exact whole number of row slots: the
  // bottom edge pulls UP to the last full slot, and the scroll target locks
  // to the same pitch, so a resting list always shows complete rows evenly.
  // One slot fewer than fits: playtest verdict - the list otherwise runs too
  // close to the footer. Capacity is measured from the classic flow anchor
  // (title at 13% of the frame) so the slot count keeps its tuned value...
  const float row_pitch = row_h + row_gap;
  const float columns_y_flow = Snap(Snap(frame_pos.y + frame_size.y * 0.13f) + 74.0f * s);
  const int view_rows = std::clamp(
      static_cast<int>((content_bottom - columns_y_flow + row_gap) / row_pitch) - 1, 1,
      kMaxViewRows);
  const float content_view_h = view_rows * row_pitch - row_gap;
  // ...but the block itself is re-anchored so those slots sit vertically
  // CENTERED in the frame (the flow layout read visibly low). The title rides
  // 74*s above the block, the footer stays put; clamps keep the title
  // on-screen and the block clear of the footer.
  const float columns_y = Snap(std::clamp(
      frame_pos.y + (frame_size.y - content_view_h) * 0.5f,
      std::min(frame_pos.y + 98.0f * s, columns_y_flow), content_bottom - content_view_h));
  const float title_y = Snap(columns_y - 74.0f * s);
  const float content_view_bottom = columns_y + content_view_h;
  // Description panel height - also anchors Close Game's bottom edge. When
  // the category rail is long enough to push Close Game below the panel's
  // tuned 320*s height, the panel stretches so its bottom stays level with
  // the button's (the two columns read as one aligned baseline).
  const float rail_close_bottom =
      Snap(columns_y + category_count * (rail_item_h + row_gap) - row_gap +
           45.0f * s) +
      rail_item_h;
  const float desc_panel_h = Snap(std::min(
      content_bottom - columns_y,
      std::max(320.0f * s, rail_close_bottom - columns_y)));

  const ImVec2 mouse = io.MousePos;
  const bool mouse_moved =
      std::abs(mouse.x - mouse_x_) > 2.0f || std::abs(mouse.y - mouse_y_) > 2.0f;
  mouse_x_ = mouse.x;
  mouse_y_ = mouse.y;
  const bool clicked = ImGui::IsMouseClicked(0);
  auto mouse_in = [&mouse](float x0, float y0, float x1, float y1) {
    return mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1;
  };

  // ---- Window ----
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  if (!ImGui::Begin("##skate3_settings_overlay", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ---- Backdrop: uniform slightly-dark tint over the whole screen. The
  // native renderer blurs the finished frame while the menu is open
  // (SetSettingsMenuBlur), so the tint lands on a frosted scene.
  // VR: none - the headset panel shows the menu's own elements floating
  // over the world (the presenter paints the UI alone, see DrawVrMenuPanel).
  if (!vr_layout) {
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(6, 9, 11, 133));
  }

  // ---- Title ----
  dl->AddText(bold, title_size, ImVec2(Snap(rail_x), title_y), kColText, "Settings");
  if (pending) {
    const char* chip_text = "RESTART TO APPLY";
    float chip_size = font_px(14.0f * s);
    ImVec2 extent = bold->CalcTextSizeA(chip_size, FLT_MAX, 0.0f, chip_text);
    float chip_pad = 10.0f * s;
    float chip_x1 = menu_right;
    float chip_x0 = Snap(chip_x1 - extent.x - 2.0f * chip_pad);
    float chip_y0 = Snap(title_y + title_size * 0.5f - extent.y * 0.5f - 6.0f * s);
    float chip_y1 = Snap(chip_y0 + extent.y + 12.0f * s);
    dl->AddRect(ImVec2(chip_x0, chip_y0), ImVec2(chip_x1, chip_y1), kColWarn, 0.0f, 0,
                1.5f);
    dl->AddText(bold, chip_size, ImVec2(Snap(chip_x0 + chip_pad), Snap(chip_y0 + 6.0f * s)),
                kColWarn, chip_text);
  }
  // ---- Left rail ----
  float rail_focus_target = -1.0f;
  for (int i = 0; i < category_count; ++i) {
    float y0 = columns_y + i * (rail_item_h + row_gap);
    float y1 = y0 + rail_item_h;
    bool is_current = category_ == i;
    bool hovered = mouse_in(rail_x, y0, rail_x + rail_w, y1);
    if (hovered && clicked) {
      category_ = i;
      rail_sel_ = i;
      zone_ = FocusZone::kRail;
      row_index_ = 0;
      content_scroll_ = 0.0f;
      content_scroll_anim_ = 0.0f;
      is_current = true;
    }
    if (is_current && zone_ == FocusZone::kRail && rail_sel_ == i) {
      rail_focus_target = y0;
    }
    // The rounded focus fill covers this item while rail-focused; skip the
    // square panel underneath so its corners don't poke past the radius -
    // but only once the sliding fill (last frame's position: the anim
    // updates just below this loop) has ARRIVED. Skipping - or painting the
    // black selected fill - the moment rail_sel_ changes blanked the target
    // item out from under the still-traveling animation; in flight it keeps
    // its normal teal panel and the fill visibly brings the selection over.
    bool rail_focused = zone_ == FocusZone::kRail && rail_sel_ == i;
    bool fill_arrived = rail_anim_y_ >= 0.0f && std::abs(rail_anim_y_ - y0) < 0.5f;
    if (!(rail_focused && fill_arrived)) {
      ImU32 bg = (is_current && !rail_focused)
                     ? kColSelFill
                     : (hovered ? kColRailPanelHover : kColRailPanel);
      dl->AddRectFilled(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), bg, 0.0f);
      dl->AddRect(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), kColRailBorder, 0.0f);
    }
  }
  // Focused rail item: sliding black fill with the lime focus ring, drawn
  // over the panels, then labels.
  if (rail_focus_target >= 0.0f) {
    if (rail_anim_y_ < 0.0f || std::abs(rail_anim_y_ - rail_focus_target) > 160.0f * s) {
      rail_anim_y_ = rail_focus_target;
    }
    rail_anim_y_ += (rail_focus_target - rail_anim_y_) * std::min(1.0f, io.DeltaTime * 22.0f);
    if (std::abs(rail_anim_y_ - rail_focus_target) < 0.5f) {
      rail_anim_y_ = rail_focus_target;  // settle exactly so arrival tests are crisp
    }
    const float fill_y = Snap(rail_anim_y_);
    DrawFocusHighlight(dl, ImVec2(rail_x, fill_y),
                       ImVec2(rail_x + rail_w, fill_y + rail_item_h), s);
  } else {
    rail_anim_y_ = -1.0f;
  }
  for (int i = 0; i < category_count; ++i) {
    float y0 = columns_y + i * (rail_item_h + row_gap);
    bool is_current = category_ == i;
    // Focused text styling waits for the sliding fill to mostly cover the
    // item (same anti-vanish rule as the content rows).
    bool focused = is_current && zone_ == FocusZone::kRail && rail_sel_ == i &&
                   rail_anim_y_ >= 0.0f && std::abs(rail_anim_y_ - y0) < rail_item_h * 0.5f;
    ImU32 text_col = focused ? kColSelText : (is_current ? kColText : kColTextDim);
    AddTextVCentered(dl, bold, label_size, rail_x + 20.0f * s,
                     y0 + rail_item_h * 0.5f, text_col, kCategories[i].name);
    if (is_current && zone_ == FocusZone::kContent) {
      // Arrow hinting that focus lives in the content column.
      float ax = rail_x + rail_w - 18.0f * s;
      float ay = y0 + rail_item_h * 0.5f;
      dl->AddTriangleFilled(ImVec2(ax, ay - 6.0f * s), ImVec2(ax, ay + 6.0f * s),
                            ImVec2(ax + 8.0f * s, ay), kColAccent);
    }
  }

  // Close Game entry: below the categories, set apart by an extra gap.
  {
    // Bottom edge aligned with the description panel's bottom, but
    // never closer to the last category than the 45*s gap the 4-category
    // layout had - with more categories the rail wins and pushes it down.
    const float rail_bottom = columns_y + category_count * (rail_item_h + row_gap) - row_gap;
    float y0 = Snap(std::max(columns_y + desc_panel_h - rail_item_h,
                             rail_bottom + 45.0f * s));
    float y1 = y0 + rail_item_h;
    bool focused = zone_ == FocusZone::kRail && rail_sel_ == quit_rail_index;
    bool hovered = mouse_in(rail_x, y0, rail_x + rail_w, y1);
    if (hovered && clicked && close_game_) {
      Hide();
      close_game_();
    }
    ImU32 bg = focused ? kColDanger : (hovered ? kColRailPanelHover : kColRailPanel);
    dl->AddRectFilled(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1), bg, 0.0f);
    dl->AddRect(ImVec2(rail_x, y0), ImVec2(rail_x + rail_w, y1),
                focused ? kColDanger : kColRailBorder, 0.0f);
    AddTextVCentered(dl, bold, label_size, rail_x + 20.0f * s, (y0 + y1) * 0.5f,
                     focused ? IM_COL32(255, 250, 249, 255) : kColDanger, "Close Game");
  }

  // ---- Content column ----
  // Row geometry (in unscrolled space).
  std::vector<float> row_y(rows.size());
  std::vector<float> row_hgt(rows.size());
  {
    float y = 0.0f;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      row_y[i] = y;
      row_hgt[i] = rows[i].kind == RowSpec::kHeader ? header_h : row_h;
      y += row_hgt[i] + row_gap;
    }
  }
  const float rows_total_h = rows.empty() ? 0.0f : row_y.back() + row_hgt.back();
  const float max_scroll = std::max(0.0f, rows_total_h - content_view_h);

  // Wheel scroll steps whole rows while over the content column. High-
  // resolution wheels and touchpads report fractional deltas per event, so
  // accumulate them into whole notches first - scaling each tiny delta by
  // the row pitch and then locking to row boundaries would round every one
  // of them straight back to the current position.
  if (mouse_in(content_x, columns_y, content_x + content_w, content_view_bottom)) {
    wheel_accum_ += io.MouseWheel;
    const int notches = static_cast<int>(std::trunc(wheel_accum_));
    if (notches != 0) {
      content_scroll_ -= notches * row_pitch;
      wheel_accum_ -= static_cast<float>(notches);
    }
  } else {
    wheel_accum_ = 0.0f;
  }
  // Keep the focused row in view when navigating.
  if (zone_ == FocusZone::kContent && row_index_ >= 0 && (in.move_y != 0 || in.select)) {
    float y0 = row_y[row_index_];
    float y1 = y0 + row_hgt[row_index_];
    if (y0 - content_scroll_ < 0.0f) {
      content_scroll_ = y0;
    } else if (y1 - content_scroll_ > content_view_h) {
      content_scroll_ = y1 - content_view_h;
    }
  }
  // Lock the resting position onto whole-row boundaries (rows and the view
  // share the same pitch, so max_scroll is itself a pitch multiple).
  content_scroll_ = std::clamp(content_scroll_, 0.0f, max_scroll);
  content_scroll_ =
      std::clamp(std::round(content_scroll_ / row_pitch) * row_pitch, 0.0f, max_scroll);
  // The drawn offset chases the locked target so scrolling slides smoothly
  // instead of rows popping between aligned positions.
  if (content_scroll_anim_ < 0.0f) {
    content_scroll_anim_ = content_scroll_;
  }
  content_scroll_anim_ +=
      (content_scroll_ - content_scroll_anim_) * std::min(1.0f, io.DeltaTime * 16.0f);
  if (std::abs(content_scroll_anim_ - content_scroll_) < 0.5f) {
    content_scroll_anim_ = content_scroll_;  // settle exactly on the boundary
  }
  const float scroll = Snap(content_scroll_anim_);

  // Clip window padded by the focus-ring extent (6*s) so the ring can sit
  // fully OUTSIDE the focused row without being cut off at the edges.
  const float ring_pad = 6.0f * s;
  dl->PushClipRect(ImVec2(content_x - ring_pad, columns_y - ring_pad),
                   ImVec2(content_x + content_w + ring_pad + 1.0f,
                          content_view_bottom + ring_pad),
                   true);

  // Sliding selection highlight (white fill under the focused row).
  const bool content_focus = zone_ == FocusZone::kContent && row_index_ >= 0;
  if (content_focus) {
    float target = row_y[row_index_];
    if (highlight_anim_y_ < 0.0f || std::abs(highlight_anim_y_ - target) > 160.0f * s) {
      highlight_anim_y_ = target;
    }
    highlight_anim_y_ += (target - highlight_anim_y_) * std::min(1.0f, io.DeltaTime * 22.0f);
    if (std::abs(highlight_anim_y_ - target) < 0.5f) {
      highlight_anim_y_ = target;  // settle exactly so arrival tests are crisp
    }
  } else {
    highlight_anim_y_ = -1.0f;
  }

  const float value_w = std::min(390.0f * s, content_w * 0.5f);
  const float chevron_r = 13.0f * s;

  // Rows draw whenever they touch the view: at rest the pitch-locked scroll
  // means nothing is ever cut, and mid-animation the edge rows slide in and
  // out under the clip rect instead of popping.
  auto row_in_view = [&](float y0, float y1) {
    return y1 >= columns_y && y0 <= content_view_bottom;
  };

  // Backgrounds first, then per-row content in TWO phases around the sliding
  // highlight: non-focused content below it, the focused row's content above
  // it - the ring overlaps neighbours (gap is 1 design px tighter than the
  // ring) and must read as the forefront element.
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    const RowSpec& row = rows[i];
    if (row.kind == RowSpec::kHeader) {
      continue;
    }
    float y0 = columns_y + Snap(row_y[i]) - scroll;
    float y1 = y0 + row_hgt[i];
    if (!row_in_view(y0, y1)) {
      continue;
    }
    bool hovered = mouse_in(content_x, y0, content_x + content_w, y1);
    if (hovered && mouse_moved && row.enabled) {
      zone_ = FocusZone::kContent;
      row_index_ = i;
    }
    // The rounded highlight covers the focused row's panel entirely; skip it
    // so its square corners don't poke past the radius - but only once the
    // sliding fill has ARRIVED. Skipping the moment row_index_ changes
    // blanked the target row out from under the still-traveling animation.
    if (content_focus && row_index_ == i &&
        std::abs(highlight_anim_y_ - row_y[i]) < 0.5f) {
      continue;
    }
    ImU32 bg = hovered && row.enabled ? kColPanelHover : kColPanel;
    dl->AddRectFilled(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), bg, 0.0f);
    dl->AddRect(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), kColPanelBorder,
                0.0f);
  }

  auto draw_row_content = [&](int i) {
    RowSpec& row = rows[i];
    float y0 = columns_y + Snap(row_y[i]) - scroll;
    float y1 = y0 + row_hgt[i];
    if (!row_in_view(y0, y1)) {
      return;
    }
    float cy = (y0 + y1) * 0.5f;

    if (row.kind == RowSpec::kHeader) {
      // Section header: solid accent bar spanning the content
      // column, dark bold text, label column-aligned with the row labels.
      dl->AddRectFilled(ImVec2(content_x, y0), ImVec2(content_x + content_w, y1), kColAccent,
                        0.0f);
      AddTextVCentered(dl, bold_ol, label_size, content_x + 18.0f * s, cy, kColAccentDark,
                       row.label);
      return;
    }

    // Focused STYLING (white text, polarity-matched weight) waits until the
    // sliding fill mostly covers the row: flipping on row_index_ alone drew
    // white-on-white while the black fill was still in flight.
    const bool focused = content_focus && row_index_ == i &&
                         std::abs(highlight_anim_y_ - row_y[i]) < row_hgt[i] * 0.5f;
    const bool hovered = mouse_in(content_x, y0, content_x + content_w, y1);
    ImFont* row_bold = focused ? bold : bold_ol;  // polarity-matched weight
    ImU32 label_col = focused ? kColSelText
                              : (!row.enabled ? kColTextFaint
                                              : (row.danger ? kColDanger : kColRowText));
    ImU32 value_col = focused ? kColSelText : (row.enabled ? kColRowText : kColTextFaint);

    // Label.
    AddTextVCentered(dl, row_bold, label_size, content_x + 18.0f * s, cy, label_col, row.label);

    // Value area on the right side of the row.
    float vx1 = content_x + content_w - 12.0f * s;
    float vx0 = content_x + content_w - value_w;

    switch (row.kind) {
      case RowSpec::kEnum: {
        int count = static_cast<int>(row.options.size());
        int value = count > 0 ? std::clamp(enum_value(row), 0, count - 1) : 0;
        if (row.checkbox) {
          const float box = 24.0f * s;
          const ImVec2 top_left(Snap(vx1 - box - 8.0f * s), Snap(cy - box * 0.5f));
          const ImVec2 bottom_right(top_left.x + box, top_left.y + box);
          dl->AddRectFilled(top_left, bottom_right,
                            value ? kColInteract : (focused ? kColSelFill : kColPanel),
                            3.0f * s);
          dl->AddRect(top_left, bottom_right, value_col, 3.0f * s, 0, 2.0f * s);
          if (value) {
            const ImVec2 a(top_left.x + box * 0.22f, top_left.y + box * 0.52f);
            const ImVec2 b(top_left.x + box * 0.43f, top_left.y + box * 0.73f);
            const ImVec2 c(top_left.x + box * 0.79f, top_left.y + box * 0.27f);
            dl->AddLine(a, b, kColWhite, 2.5f * s);
            dl->AddLine(b, c, kColWhite, 2.5f * s);
          }
          if (clicked && row.enabled && hovered) {
            activate_row(row);
          }
          break;
        }
        ImVec2 left_center(vx0 + chevron_r, cy);
        ImVec2 right_center(vx1 - chevron_r, cy);
        // Magenta filled stepper circles with white triangles;
        // a circle greys out (and stops responding) at its end of the range.
        const bool can_left = row.enabled && value > 0;
        const bool can_right = row.enabled && value < count - 1;
        // Hit areas are tracked separately from enablement: a click on a
        // greyed-out stepper must be swallowed, not fall through to the
        // row-body wrap-around cycle below.
        const bool left_hit =
            mouse_in(left_center.x - chevron_r - 4.0f, cy - chevron_r - 4.0f,
                     left_center.x + chevron_r + 4.0f, cy + chevron_r + 4.0f);
        const bool right_hit =
            mouse_in(right_center.x - chevron_r - 4.0f, cy - chevron_r - 4.0f,
                     right_center.x + chevron_r + 4.0f, cy + chevron_r + 4.0f);
        bool left_hover = can_left && left_hit;
        bool right_hover = can_right && right_hit;
        const ImU32 chev_disabled = focused ? kColChevDisabledOnDark : kColChevDisabled;
        DrawChevron(dl, left_center, chevron_r, true,
                    can_left ? (left_hover ? kColInteractHover : kColInteract)
                             : chev_disabled,
                    kColWhite, true);
        DrawChevron(dl, right_center, chevron_r, false,
                    can_right ? (right_hover ? kColInteractHover : kColInteract)
                              : chev_disabled,
                    kColWhite, true);
        if (count > 0) {
          float text_max_w = (right_center.x - chevron_r) - (left_center.x + chevron_r) -
                             16.0f * s;
          // Free-form values (GPU adapter names) can outgrow the slot: step
          // the value text down toward 18 design px first (each candidate
          // em-quantized like every other size), only then ellipsize.
          float fit_size = value_size;
          for (float design = 21.0f;
               design >= 18.0f &&
               row_bold->CalcTextSizeA(fit_size, FLT_MAX, 0.0f, row.options[value].c_str()).x >
                   text_max_w;
               design -= 1.0f) {
            fit_size = font_px(design * s);
          }
          std::string text =
              TruncateToWidth(row_bold, fit_size, text_max_w, row.options[value]);
          AddTextCentered(dl, row_bold, fit_size,
                          ImVec2((left_center.x + right_center.x) * 0.5f, cy), value_col,
                          text.c_str());
        }
        if (clicked && row.enabled) {
          if (left_hit) {
            if (can_left) {
              step_row(row, -1);
            }
          } else if (right_hit) {
            if (can_right) {
              step_row(row, 1);
            }
          } else if (hovered) {
            activate_row(row);  // row-body click cycles with wrap-around
          }
        }
        break;
      }
      case RowSpec::kSlider: {
        float number_w = 52.0f * s;
        float track_x0 = vx0 + 6.0f * s;
        float track_x1 = vx1 - number_w;
        float t = row.max > row.min ? (*row.value - row.min) / (row.max - row.min) : 0.0f;
        float knob_x = track_x0 + t * (track_x1 - track_x0);
        ImU32 track_col = focused ? IM_COL32(255, 255, 255, 77) : IM_COL32(0, 0, 0, 56);
        dl->AddRectFilled(ImVec2(track_x0, cy - 2.0f * s), ImVec2(track_x1, cy + 2.0f * s),
                          track_col, 0.0f);
        dl->AddRectFilled(ImVec2(track_x0, cy - 2.0f * s), ImVec2(knob_x, cy + 2.0f * s),
                          kColInteract, 0.0f);
        dl->AddCircleFilled(ImVec2(Snap(knob_x), Snap(cy)), 8.0f * s, kColInteract, 24);
        char value_text[32];
        std::snprintf(value_text, sizeof(value_text), row.fmt, double(*row.value));
        ImVec2 extent = row_bold->CalcTextSizeA(value_size, FLT_MAX, 0.0f, value_text);
        dl->AddText(row_bold, value_size, ImVec2(Snap(vx1 - extent.x), Snap(cy - extent.y * 0.5f)),
                    value_col, value_text);
        // Mouse drag on the track.
        ImGui::SetCursorScreenPos(ImVec2(track_x0 - 10.0f * s, cy - 14.0f * s));
        char slider_id[48];
        std::snprintf(slider_id, sizeof(slider_id), "##slider_%d_%d", category_, i);
        ImGui::InvisibleButton(slider_id,
                               ImVec2(track_x1 - track_x0 + 20.0f * s, 28.0f * s));
        if (ImGui::IsItemActive() && row.enabled) {
          float nt = std::clamp((mouse.x - track_x0) / std::max(1.0f, track_x1 - track_x0),
                                0.0f, 1.0f);
          float new_value = row.min + nt * (row.max - row.min);
          // Quantize to step for tidy values.
          new_value = row.min + std::round((new_value - row.min) / row.step) * row.step;
          new_value = std::clamp(new_value, row.min, row.max);
          if (new_value != *row.value) {
            *row.value = new_value;
            if (row.on_value_change) {
              row.on_value_change();
            }
          }
          zone_ = FocusZone::kContent;
          row_index_ = i;
        }
        if (ImGui::IsItemDeactivated()) {
          SaveConfig();
        }
        break;
      }
      case RowSpec::kAction: {
        // No right-side 'Select' / '-' hint: the footer legend covers
        // activation, and disabled rows read from their dim label alone.
        if (clicked && hovered && row.enabled && !row_activated_this_frame) {
          activate_row(row);
          row_activated_this_frame = true;
        }
        break;
      }
      case RowSpec::kText: {
        const bool editing_this = editing_text_ && focused;
        if (editing_this) {
          ImGui::SetCursorScreenPos(ImVec2(vx0 + 6.0f * s, cy - 14.0f * s));
          ImGui::SetNextItemWidth(vx1 - vx0 - 12.0f * s);
          // value_size is already physical; divide out the global widget-font
          // DPI scale (it exists for logical-unit dialogs, see ImGuiDrawer).
          ImGui::PushFont(font, value_size / std::max(0.01f, ImGui::GetStyle().FontScaleDpi));
          // Edit field sits on the BLACK focused row: light text, light frame.
          ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.15f));
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImVec4(0.98f, 0.99f, 0.99f, 1.0f));
          if (text_edit_focus_pending_) {
            ImGui::SetKeyboardFocusHere();
            text_edit_focus_pending_ = false;
          }
          bool committed = ImGui::InputTextWithHint(
              "##settings_text_edit", row.placeholder.c_str(), row.text_buf,
              row.text_buf_size, ImGuiInputTextFlags_EnterReturnsTrue);
          if (committed || (!ImGui::IsItemActive() && !text_edit_focus_pending_ &&
                            ImGui::IsItemDeactivated())) {
            editing_text_ = false;
            if (row.on_text_commit) {
              row.on_text_commit();
            }
          }
          ImGui::PopStyleColor(2);
          ImGui::PopFont();
        } else {
          const bool empty = !row.text_buf || !row.text_buf[0];
          if (empty && !row.placeholder.empty()) {
            // Unset value: the placeholder in gray, regular weight, so it
            // never reads as a value the player entered.
            const char* hint = row.placeholder.c_str();
            ImVec2 extent = font->CalcTextSizeA(value_size, FLT_MAX, 0.0f, hint);
            dl->AddText(font, value_size,
                        ImVec2(Snap(vx1 - extent.x), Snap(cy - extent.y * 0.5f)),
                        kColTextFaint, hint);
          } else {
            const char* text = empty ? "-" : row.text_buf;
            ImVec2 extent = row_bold->CalcTextSizeA(value_size, FLT_MAX, 0.0f, text);
            dl->AddText(row_bold, value_size,
                        ImVec2(Snap(vx1 - extent.x), Snap(cy - extent.y * 0.5f)), value_col,
                        text);
          }
          if (clicked && hovered && row.enabled) {
            zone_ = FocusZone::kContent;
            row_index_ = i;
            editing_text_ = true;
            text_edit_focus_pending_ = true;
          }
        }
        break;
      }
      default:
        break;
    }
  };
  // Phase 1: everything except the focused row.
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (!(content_focus && row_index_ == i)) {
      draw_row_content(i);
    }
  }
  // Phase 2: the sliding highlight above neighbours...
  if (content_focus && highlight_anim_y_ >= 0.0f) {
    float y0 = columns_y + Snap(highlight_anim_y_) - scroll;
    DrawFocusHighlight(dl, ImVec2(content_x, y0), ImVec2(content_x + content_w, y0 + row_h),
                       s);
  }
  // ...phase 3: the focused row's own content above the highlight.
  if (content_focus && row_index_ >= 0 && row_index_ < static_cast<int>(rows.size())) {
    draw_row_content(row_index_);
  }
  dl->PopClipRect();

  // Scrollbar hint when the list overflows.
  if (max_scroll > 0.0f) {
    float bar_x = content_x + content_w + 4.0f * s;
    float track_h = content_view_h;
    float bar_h = std::max(24.0f * s, track_h * (content_view_h / rows_total_h));
    float bar_y = columns_y + (track_h - bar_h) * (content_scroll_anim_ / max_scroll);
    dl->AddRectFilled(ImVec2(bar_x, columns_y), ImVec2(bar_x + 3.0f * s, columns_y + track_h),
                      IM_COL32(255, 255, 255, 18), 0.0f);
    dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + 3.0f * s, bar_y + bar_h),
                      IM_COL32(255, 255, 255, 90), 0.0f);
  }

  // ---- Description panel ----
  {
    const float panel_h = desc_panel_h;
    const float header_bar_h = row_h;  // header bar matches a settings row
    dl->AddRectFilled(ImVec2(desc_x, columns_y), ImVec2(desc_x + desc_w, columns_y + header_bar_h),
                      kColAccent, 0.0f);
    AddTextVCentered(dl, bold_ol, label_size, desc_x + 18.0f * s, columns_y + header_bar_h * 0.5f,
                     kColAccentDark, "Description");
    dl->AddRectFilled(ImVec2(desc_x, columns_y + header_bar_h),
                      ImVec2(desc_x + desc_w, columns_y + panel_h), kColDescPanel, 0.0f);
    dl->AddRect(ImVec2(desc_x, columns_y), ImVec2(desc_x + desc_w, columns_y + panel_h),
                kColRailBorder, 0.0f);

    const char* desc_text = kCategories[category_].desc;
    if (zone_ == FocusZone::kRail && rail_sel_ == quit_rail_index) {
      desc_text = "Quit to the desktop. Unsaved game progress is lost.";
    }
    const char* note_text = nullptr;
    const std::string* extra_text = nullptr;
    if (zone_ == FocusZone::kContent && row_index_ >= 0 &&
        row_index_ < static_cast<int>(rows.size())) {
      const RowSpec& row = rows[row_index_];
      if (row.desc) {
        desc_text = row.desc;
      }
      note_text = row.value_note;
      if (!row.desc_extra.empty()) {
        extra_text = &row.desc_extra;
      }
    }
    float text_x = Snap(desc_x + 14.0f * s);
    float text_y = Snap(columns_y + header_bar_h + 14.0f * s);
    float wrap_w = desc_w - 28.0f * s;
    text_y = Snap(text_y +
                  AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w,
                                   kColText, desc_text) +
                  10.0f * s);
    if (extra_text) {
      text_y = Snap(text_y +
                    AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w,
                                     kColTextDim, extra_text->c_str()) +
                    10.0f * s);
    }
    if (note_text) {
      text_y = Snap(text_y +
                    AddTextJustified(dl, bold, desc_size, ImVec2(text_x, text_y), wrap_w,
                                     kColWarn, note_text) +
                    10.0f * s);
    }
    if (pending) {
      AddTextJustified(dl, font, desc_size, ImVec2(text_x, text_y), wrap_w, kColWarn,
                       "Some changes take effect on the next launch. Restart when ready.");
    }
  }

  // ---- Footer button legend (base viewport scale, not the menu scale) ----
  {
    const float fs = base_s;
    float legend_y = Snap(frame_bottom - footer_h + 14.0f * fs);
    std::vector<LegendGlyph> glyphs;
    if (pad_active_) {
      glyphs.push_back({"A", "Select", true});
      glyphs.push_back({"B", "Back", true});
      glyphs.push_back({"Y", "Reset to Default", true});
      glyphs.push_back({"LB / RB", "Category", false});
      if (pending) {
        glyphs.push_back({"X", "Restart Now", true});
      }
    } else {
      glyphs.push_back({"Enter", "Select", false});
      glyphs.push_back({"Esc", "Back", false});
      glyphs.push_back({"R", "Reset to Default", false});
      glyphs.push_back({"Q / E", "Category", false});
      if (pending) {
        glyphs.push_back({"F", "Restart Now", false});
      }
    }
    float x = rail_x;
    float glyph_size = font_px(15.0f * fs);
    float label_text_size = font_px(16.0f * fs);
    float chip_h = Snap(26.0f * fs);
    for (const LegendGlyph& glyph : glyphs) {
      ImVec2 glyph_extent = bold->CalcTextSizeA(glyph_size, FLT_MAX, 0.0f, glyph.glyph);
      float cy = legend_y + chip_h * 0.5f;
      x = Snap(x);
      if (glyph.circle) {
        float r = chip_h * 0.5f;
        dl->AddCircleFilled(ImVec2(x + r, cy), r, kColLegendChip, 28);
        AddTextCenteredCap(dl, bold_ol, glyph_size, ImVec2(x + r, cy), kColLegendText,
                           glyph.glyph, /*ink_x=*/true);
        x += 2.0f * r + 8.0f * fs;
      } else {
        float chip_w = Snap(glyph_extent.x + 18.0f * fs);
        // Hard straight edges + AA corners, like the browser's border-radius
        // (rounded AddRectFilled feathers every edge 1px).
        DrawHardRoundedFill(dl, ImVec2(x, legend_y), ImVec2(x + chip_w, legend_y + chip_h),
                            4.0f * fs, kColLegendChip);
        AddTextCenteredCap(dl, bold_ol, glyph_size, ImVec2(x + chip_w * 0.5f, cy),
                           kColLegendText, glyph.glyph, /*ink_x=*/false);
        x += chip_w + 8.0f * fs;
      }
      ImVec2 label_extent = bold->CalcTextSizeA(label_text_size, FLT_MAX, 0.0f, glyph.label);
      dl->AddText(bold, label_text_size, ImVec2(Snap(x), Snap(cy - label_extent.y * 0.5f)),
                  kColLegendLabel, glyph.label);
      x += label_extent.x + 26.0f * fs;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
}


}  // namespace skate3

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace skate3 {

bool RelaunchGame() {
#if defined(_WIN32)
  wchar_t executable_path[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
    REXLOG_WARN("Restart requested, but the executable path could not be resolved");
    return false;
  }
  // Same arguments and working directory as this session.
  std::wstring command_line = GetCommandLineW();
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup_info, &process_info)) {
    REXLOG_WARN("Restart requested, but launching a new process failed ({})",
                GetLastError());
    return false;
  }
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return true;
#else
  return false;
#endif
}

}  // namespace skate3
