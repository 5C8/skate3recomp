#include "skate3_native_debug_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "skate3_native_scene.h"
#include "skate3_render_resolution.h"

REXCVAR_DEFINE_BOOL(skate3_native_render_mode_indicator, false, "Skate 3",
                    "Small top-right corner readout of which renderer produced the "
                    "last presented frame: NATIVE (native scene renderer) or "
                    "EMULATED (Xenos GPU emulation; menus/loading yields, F5 off). "
                    "Shows automatically while the native scene renderer is "
                    "switched off, regardless of this setting.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Hook-layer master (boot-time), shown read-only.
REXCVAR_DECLARE(bool, skate3_native_render);
REXCVAR_DECLARE(bool, skate3_native_render_scene);
// SDK: emulated-draw suppression while the native output is active.
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);

// Output resolution / aspect (skate3_render_resolution.cpp).
REXCVAR_DECLARE(int32_t, skate3_render_width);
REXCVAR_DECLARE(int32_t, skate3_render_height);
REXCVAR_DECLARE(std::string, skate3_render_aspect);

// Image quality.
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_mips);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_detail_hold);

// Material shading (game-parity families).
REXCVAR_DECLARE(bool, skate3_native_render_scene_lightmaps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_v2);
REXCVAR_DECLARE(double, skate3_native_render_scene_world_v2_tan_sign);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynobj_v2);
REXCVAR_DECLARE(bool, skate3_native_render_scene_macro);
REXCVAR_DECLARE(bool, skate3_native_render_scene_decals);
REXCVAR_DECLARE(bool, skate3_native_render_scene_transparents);
REXCVAR_DECLARE(bool, skate3_native_render_scene_backface_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_refl_bias_auto);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_x);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_y);

// Shadows.
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadows);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_shadow_tile);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_caster_parity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_char_shadow_exact);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadow_pcss);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_sun_deg);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_max_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_blocker_m);
REXCVAR_DECLARE(double, skate3_native_render_scene_shadow_pcss_min_texel);

// Characters & entities.
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_ident);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_fade_serve);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_ropa_world);
REXCVAR_DECLARE(bool, skate3_native_render_scene_palette_serve);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_identity);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_gap_fill);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_palette);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dyn_gap_fill);
REXCVAR_DECLARE(bool, skate3_native_render_scene_char_rows_inst);
REXCVAR_DECLARE(bool, skate3_native_render_scene_caster_refresh_all);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_blend);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ropa_boxcar);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_ropa_bias);

// World content, culling & overlays.
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_retain_offscreen);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull_build);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull_guest);
REXCVAR_DECLARE(bool, skate3_native_render_scene_quadlists);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_selection_outline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_simpledraw);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(double, skate3_native_render_scene_2d_sharp);
REXCVAR_DECLARE(double, skate3_menu_blur_sigma);

// Native coverage (which screens render natively vs yield to emulation).
REXCVAR_DECLARE(bool, skate3_native_render_scene_pause_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_loading_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_boot_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_fmv_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_native);
REXCVAR_DECLARE(bool, skate3_native_render_scene_photo_yield);
REXCVAR_DECLARE(bool, skate3_native_render_scene_cas_yield);

// Smoothness / pacing.
REXCVAR_DECLARE(bool, skate3_native_render_scene_smooth_camera);
REXCVAR_DECLARE(double, skate3_native_render_scene_smooth_camera_filter_ms);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sort_opaque);
REXCVAR_DECLARE(double, skate3_guest_fps_cap);
REXCVAR_DECLARE(bool, skate3_guest_fps_cap_auto);

// Showcase / drone camera.
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_hold);
REXCVAR_DECLARE(double, skate3_native_render_scene_showcase_wipe);
REXCVAR_DECLARE(std::string, skate3_native_render_scene_showcase_order);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(double, skate3_native_render_scene_freecam_speed);
REXCVAR_DECLARE(double, skate3_native_render_scene_freecam_look_speed);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);
// The retired HDR-stack features: no longer offered in this menu, read only
// so the showcase setup can mark their layers unavailable.
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(bool, skate3_native_render_scene_bloom);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shafts);
REXCVAR_DECLARE(bool, skate3_native_render_scene_haze);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssao);
REXCVAR_DECLARE(bool, skate3_native_render_scene_ssr);

// Diagnostics.
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_decal_probe);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_refl_mode);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_lod);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sun_override);
REXCVAR_DECLARE(double, skate3_native_render_scene_sun_azimuth);
REXCVAR_DECLARE(double, skate3_native_render_scene_sun_elevation);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_synthetic_pan);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_rate);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_amp);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_perf_interval);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shader_census);
REXCVAR_DECLARE(bool, skate3_native_render_scene_char_track);

// Caches & streaming.
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_mesh_revalidate);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_tex_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_store_mb);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_prewarm_budget_ms);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_warmup_budget_ms);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_settle_max_frames);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_mesh_decode_budget);

namespace skate3 {
namespace {

bool CvarCheckbox(const char* label, bool value, const char* help = nullptr) {
  bool v = value;
  ImGui::Checkbox(label, &v);
  if (help != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", help);
  }
  return v;
}

// Float-cvar slider (the cvars are hot-reload doubles; edits apply next
// frame). Returns the possibly-changed value for REXCVAR_SET.
double CvarSlider(const char* label, double value, float lo, float hi,
                  const char* fmt, const char* help = nullptr) {
  float v = float(value);
  if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
    value = double(v);
  }
  if (help != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
    ImGui::SetTooltip("%s", help);
  }
  return value;
}

// Int-cvar slider, same contract as CvarSlider.
int32_t CvarSliderInt(const char* label, int32_t value, int lo, int hi,
                      const char* help = nullptr) {
  int v = int(value);
  if (ImGui::SliderInt(label, &v, lo, hi)) {
    value = int32_t(v);
  }
  if (help != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
    ImGui::SetTooltip("%s", help);
  }
  return value;
}

// Combo over a fixed value list (for quality steps that are not a
// continuum: sample counts, map sizes). Returns the possibly-changed value;
// out-of-list current values snap to the nearest entry's label without
// writing back until the user picks one.
int32_t ValueCombo(const char* label, int32_t value, const int32_t* values,
                   const char* const* labels, int count,
                   const char* help = nullptr) {
  int index = 0;
  for (int i = 1; i < count; ++i) {
    const int32_t di = values[i] > value ? values[i] - value : value - values[i];
    const int32_t db = values[index] > value ? values[index] - value
                                             : value - values[index];
    if (di < db) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, labels, count)) {
    value = values[index];
  }
  if (help != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", help);
  }
  return value;
}

// ---- Max quality (photo mode) ---------------------------------------------
// One switch that pushes every resolution/sample-count knob to its maximum
// for capture/recording (showcase runs, promo footage, screenshots) and
// restores the previous values when switched off. Session-scoped: nothing
// is persisted. It only raises fidelity of what the faithful renderer
// already draws; it never turns on look-changing extras (the retired HDR
// post stack stays off), and tuning values keep their settings.

struct MaxQualityState {
  bool active = false;
  // Saved values, restored on switch-off.
  int32_t msaa;
  int32_t shadow_tile;
  bool tex_mips;
  bool shadows;
  bool pcss;
};

MaxQualityState s_max_quality;

void SetMaxQuality(bool enable) {
  MaxQualityState& s = s_max_quality;
  if (enable == s.active) {
    return;
  }
  if (enable) {
    s.msaa = REXCVAR_GET(skate3_native_render_scene_msaa);
    s.shadow_tile = REXCVAR_GET(skate3_native_render_scene_shadow_tile);
    s.tex_mips = REXCVAR_GET(skate3_native_render_scene_tex_mips);
    s.shadows = REXCVAR_GET(skate3_native_render_scene_shadows);
    s.pcss = REXCVAR_GET(skate3_native_render_scene_shadow_pcss);

    REXCVAR_SET(skate3_native_render_scene_msaa, 8);
    REXCVAR_SET(skate3_native_render_scene_shadow_tile, 4096);
    REXCVAR_SET(skate3_native_render_scene_tex_mips, true);
    REXCVAR_SET(skate3_native_render_scene_shadows, true);
    REXCVAR_SET(skate3_native_render_scene_shadow_pcss, true);
  } else {
    REXCVAR_SET(skate3_native_render_scene_msaa, s.msaa);
    REXCVAR_SET(skate3_native_render_scene_shadow_tile, s.shadow_tile);
    REXCVAR_SET(skate3_native_render_scene_tex_mips, s.tex_mips);
    REXCVAR_SET(skate3_native_render_scene_shadows, s.shadows);
    REXCVAR_SET(skate3_native_render_scene_shadow_pcss, s.pcss);
  }
  s.active = enable;
}

REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_maxq_cycle, 0, "Skate 3",
    "DEBUG: auto-toggle MAX QUALITY every N native-scene frames (0 = off). "
    "Stress-tests the hot pipeline/target/shadow-map rebuild path; pair "
    "with Vulkan validation layers when hunting stale bindings.")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

void DrawMaxQualityToggle() {
  bool on = s_max_quality.active;
  if (ImGui::Checkbox("MAX QUALITY (photo mode)", &on)) {
    SetMaxQuality(on);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Push every resolution setting to its maximum for recording: MSAA\n"
        "8x, 4096 shadow tiles, full mip chains, dynamic + soft shadows on.\n"
        "Changes no look (no post effects). GPU-heavy; previous values\n"
        "restore when unchecked. Applies live (a one-frame pipeline\n"
        "rebuild).");
  }
  if (s_max_quality.active) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "ACTIVE");
  }
}

// ---- Showcase setup window ------------------------------------------------
// Editor for skate3_native_render_scene_showcase_order: one row per build-up
// layer, reorderable, each optionally joined to the previous row's wipe. The
// cvar string is the single source of truth ("tok,tok+tok,-tok": ',' starts
// a new wipe, '+' joins, '-' disables in place); the rows parse from it
// every frame and any edit writes it straight back, so the sequencer, this
// window and hand edits in config all stay consistent.

bool s_showcase_setup_open = false;

struct ShowcaseRow {
  const skate3::native_scene::ShowcaseLayer* layer;
  bool enabled;
  bool joined;
};

std::vector<ShowcaseRow> ParseShowcaseRows(const std::string& order) {
  using skate3::native_scene::kShowcaseLayers;
  std::vector<ShowcaseRow> rows;
  std::string tok;
  bool cur_joined = false;  // the separator before the accumulating token
  const auto flush = [&]() {
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
      tok.erase(tok.begin());
    }
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
      tok.pop_back();
    }
    bool enabled = true;
    if (!tok.empty() && tok[0] == '-') {
      enabled = false;
      tok.erase(tok.begin());
    }
    for (char& ch : tok) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = char(ch + 32);
      }
    }
    for (const auto& layer : kShowcaseLayers) {
      if (tok != layer.token) {
        continue;
      }
      bool present = false;
      for (const auto& row : rows) {
        present = present || row.layer == &layer;
      }
      if (!present) {
        rows.push_back({&layer, enabled, !rows.empty() && cur_joined});
      }
      break;
    }
    tok.clear();
  };
  for (char ch : order) {
    if (ch == ',' || ch == '+') {
      flush();
      cur_joined = ch == '+';
    } else {
      tok += ch;
    }
  }
  flush();
  // Layers missing from the string append disabled, so they can always be
  // re-enabled from the window.
  for (const auto& layer : kShowcaseLayers) {
    bool present = false;
    for (const auto& row : rows) {
      present = present || row.layer == &layer;
    }
    if (!present) {
      rows.push_back({&layer, false, false});
    }
  }
  return rows;
}

std::string BuildShowcaseOrder(const std::vector<ShowcaseRow>& rows) {
  std::string out;
  for (size_t k = 0; k < rows.size(); ++k) {
    if (k > 0) {
      out += rows[k].joined ? '+' : ',';
    }
    if (!rows[k].enabled) {
      out += '-';
    }
    out += rows[k].layer->token;
  }
  return out;
}

// Mirrors the sequencer's availability rules (TickShowcase): the material
// looks need nothing; shadow/post layers need their feature (post gates
// live in the HDR tonemap).
bool ShowcaseLayerAvailable(uint32_t bit) {
  const bool hdr_on = REXCVAR_GET(skate3_native_render_scene_hdr);
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
    default:
      return true;
  }
}

// Drone-camera controls (same cvars everywhere the section is embedded;
// edits apply live).
void DrawFreecamControls() {
  ImGui::SeparatorText("Drone camera");
  bool freecam = REXCVAR_GET(skate3_native_render_scene_freecam);
  if (ImGui::Checkbox("Free-fly drone cam (End)", &freecam)) {
    REXCVAR_SET(skate3_native_render_scene_freecam, freecam);
  }
  ImGui::SameLine();
  bool capture = REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
  if (ImGui::Checkbox("capture game input", &capture)) {
    REXCVAR_SET(skate3_native_render_scene_freecam_capture_input, capture);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "While flying, keep keyboard/controller input away from the game so\n"
        "the fly keys don't also steer the skater.");
  }
  REXCVAR_SET(skate3_native_render_scene_freecam_speed,
              CvarSlider("fly speed (m/s)",
                         REXCVAR_GET(skate3_native_render_scene_freecam_speed),
                         0.5f, 60.0f, "%.1f",
                         "Base fly speed; Shift = 4x, Ctrl = 0.2x"));
  REXCVAR_SET(
      skate3_native_render_scene_freecam_look_speed,
      CvarSlider("look speed (deg/s)",
                 REXCVAR_GET(skate3_native_render_scene_freecam_look_speed),
                 20.0f, 240.0f, "%.0f", "Arrow-key look rate"));
  ImGui::TextDisabled(
      "WASD fly, E/Space up, Q/C down, arrows or right-mouse drag look,\n"
      "Z/X zoom, Shift fast, Ctrl slow.");
}

void DrawShowcaseSetupWindow(bool* p_open) {
  using skate3::native_scene::kShowcaseLayers;
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Showcase Setup", p_open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  const bool running = REXCVAR_GET(skate3_native_render_scene_showcase);
  if (ImGui::Button(running ? "Cancel showcase" : "Start showcase (Ctrl+Shift+B)",
                    ImVec2(-1.0f, 0.0f))) {
    REXCVAR_SET(skate3_native_render_scene_showcase, !running);
  }
  REXCVAR_SET(skate3_native_render_scene_showcase_hold,
              CvarSlider("stage hold (s)",
                         REXCVAR_GET(skate3_native_render_scene_showcase_hold),
                         0.0f, 10.0f, "%.1f",
                         "Seconds each layer holds fullscreen between wipes"));
  REXCVAR_SET(skate3_native_render_scene_showcase_wipe,
              CvarSlider("wipe duration (s)",
                         REXCVAR_GET(skate3_native_render_scene_showcase_wipe),
                         0.5f, 10.0f, "%.1f",
                         "Seconds each split wipe takes to cross the screen"));

  ImGui::SeparatorText("Layer order");
  ImGui::TextDisabled(
      "Every run starts from clay geometry. \"join\" reveals a layer\n"
      "inside the previous layer's wipe instead of its own.");
  std::vector<ShowcaseRow> rows =
      ParseShowcaseRows(REXCVAR_GET(skate3_native_render_scene_showcase_order));
  bool changed = false;
  for (int k = 0; k < int(rows.size()); ++k) {
    ImGui::PushID(k);
    bool en = rows[k].enabled;
    if (ImGui::Checkbox("##on", &en)) {
      rows[k].enabled = en;
      changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(k == 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && k > 0) {
      std::swap(rows[k - 1], rows[k]);
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(k + 1 == int(rows.size()));
    if (ImGui::ArrowButton("down", ImGuiDir_Down) && k + 1 < int(rows.size())) {
      std::swap(rows[k], rows[k + 1]);
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(k == 0);
    bool joined = k > 0 && rows[k].joined;
    if (ImGui::Checkbox("join", &joined)) {
      rows[k].joined = joined;
      changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ShowcaseLayerAvailable(rows[k].layer->bit)) {
      ImGui::TextUnformatted(rows[k].layer->label);
    } else {
      ImGui::TextDisabled("%s (feature off)", rows[k].layer->label);
    }
    ImGui::PopID();
  }
  if (changed) {
    if (!rows.empty()) {
      rows[0].joined = false;
    }
    REXCVAR_SET(skate3_native_render_scene_showcase_order,
                BuildShowcaseOrder(rows));
  }
  if (ImGui::Button("Reset to default order")) {
    REXCVAR_SET(skate3_native_render_scene_showcase_order,
                std::string(skate3::native_scene::kShowcaseOrderDefault));
  }

  // Live preview of the resulting run, mirrors the sequencer's step
  // builder (cumulative layer masks; materials subsumes albedo/lighting;
  // a final full-render step covers whatever the list leaves out).
  ImGui::SeparatorText("Run preview");
  uint32_t avail_mask = 0;
  for (const auto& layer : kShowcaseLayers) {
    if (ShowcaseLayerAvailable(layer.bit)) {
      avail_mask |= layer.bit;
    }
  }
  int step_no = 1;
  ImGui::Text("%d. clay geometry", step_no++);
  uint32_t cum = 0;
  size_t k = 0;
  while (k < rows.size()) {
    uint32_t add = 0;
    std::string label;
    size_t j = k;
    do {
      const ShowcaseRow& row = rows[j];
      if (row.enabled && (avail_mask & row.layer->bit) != 0 &&
          (cum & row.layer->bit) == 0 && (add & row.layer->bit) == 0) {
        add |= row.layer->bit;
        if (!label.empty()) {
          label += " & ";
        }
        label += row.layer->label;
      }
      ++j;
    } while (j < rows.size() && rows[j].joined);
    if (add != 0) {
      cum |= add;
      ImGui::Text("%d. + %s", step_no++, label.c_str());
    }
    k = j;
  }
  const bool covered = (cum & 4u) != 0 && ((avail_mask & ~cum) & 0xF8u) == 0u;
  if (!covered) {
    ImGui::Text("%d. full render", step_no++);
  }

  DrawFreecamControls();
  ImGui::End();
}

// ---- F12 menu sections ----------------------------------------------------
// One CollapsingHeader per intent group. The menu tracks the faithful
// renderer: every control drives something the native path actually
// renders the way the game does (or a tuning knob for one of its
// approximations). The retired HDR post stack (bloom, sun shafts, haze,
// SSAO, SSR) is no longer offered here; its cvars still exist for config
// files and the F1 overlay. Capture tools and image quality default open;
// tuning and diagnostics stay collapsed until needed.

void DrawShowcaseCaptureSection() {
  DrawMaxQualityToggle();
  {
    const bool running = REXCVAR_GET(skate3_native_render_scene_showcase);
    if (ImGui::Button(running ? "Cancel showcase" : "Start showcase (Ctrl+Shift+B)")) {
      REXCVAR_SET(skate3_native_render_scene_showcase, !running);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Strip the frame to clay geometry, then rebuild it layer by\n"
          "layer, each layer revealed by a live vertical split wiping\n"
          "across the screen. Layer order, grouping and pacing are edited\n"
          "in the setup window.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Setup...")) {
      s_showcase_setup_open = true;
    }
  }
  DrawFreecamControls();
}

void DrawImageQualitySection() {
  {
    static const int32_t kMsaaValues[] = {1, 2, 4, 8};
    static const char* const kMsaaLabels[] = {"Off (1x)", "2x", "4x", "8x"};
    REXCVAR_SET(skate3_native_render_scene_msaa,
                ValueCombo("MSAA", REXCVAR_GET(skate3_native_render_scene_msaa),
                           kMsaaValues, kMsaaLabels, 4,
                           "Scene multisampling. Distant thin geometry "
                           "(railings, wires) shimmers without it. Applies "
                           "live (pipeline rebuild)."));
  }
  {
    static const int32_t kTileValues[] = {0, 512, 1024, 2048, 4096};
    static const char* const kTileLabels[] = {"Auto (render scale)",
                                              "512 (console)", "1024", "2048",
                                              "4096"};
    REXCVAR_SET(
        skate3_native_render_scene_shadow_tile,
        ValueCombo("shadow tile size",
                   REXCVAR_GET(skate3_native_render_scene_shadow_tile),
                   kTileValues, kTileLabels, 5,
                   "Dynamic-shadow cascade tile resolution. The game's atlas "
                   "is three 512 tiles; Auto = 512 x Resolution Scale, the "
                   "emulated renderer's crispness at the current scale. "
                   "Applies live."));
  }
  REXCVAR_SET(skate3_native_render_scene_tex_mips,
              CvarCheckbox("Texture mip chains",
                           REXCVAR_GET(skate3_native_render_scene_tex_mips),
                           "Upload guest mip chains (off = mip 0 only; "
                           "distant surfaces alias). Flush the texture cache "
                           "after toggling."));
  REXCVAR_SET(
      skate3_native_render_scene_detail_hold,
      CvarSliderInt("texture detail hold (frames)",
                    REXCVAR_GET(skate3_native_render_scene_detail_hold), 0,
                    2000,
                    "Frames a slot keeps its higher-res texture after the "
                    "game's material-detail system flaps it to the smaller "
                    "variant (streaming pressure). 0 = serve the guest "
                    "binding verbatim: the console's own detail pop."));
}

// Output resolution / aspect (skate3_render_resolution.cpp). Presets write
// the three cvars; "Custom" exposes them directly. Everything applies live
// on the next native frame (the output targets resize).
void DrawOutputSection() {
  using skate3::render_resolution::ParseAspect;
  {
    static const char* const kAspectLabels[] = {"Window", "16:9", "16:10",
                                                "4:3",    "21:9", "32:9",
                                                "Custom"};
    static const char* const kAspectValues[] = {"window", "16:9", "16:10",
                                                "4:3",    "21:9", "32:9"};
    const std::string cur = REXCVAR_GET(skate3_render_aspect);
    const double cur_ratio = ParseAspect(cur.c_str());
    int index = 6;
    if (cur_ratio <= 0.0) {
      index = 0;
    } else {
      for (int i = 1; i < 6; ++i) {
        if (std::abs(ParseAspect(kAspectValues[i]) - cur_ratio) < 1e-3) {
          index = i;
          break;
        }
      }
    }
    if (ImGui::Combo("aspect ratio", &index, kAspectLabels, 7) && index < 6) {
      REXCVAR_SET(skate3_render_aspect, std::string(kAspectValues[index]));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Window = the window's own shape (resizable). A forced ratio\n"
          "builds the game camera for it (Hor+ wider than 16:9, Vert+\n"
          "narrower) and letterboxes/pillarboxes the frame in the window.");
    }
    if (index == 6) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
      if (ImGui::InputText("custom ratio (W:H or 2.39)", buf, sizeof(buf),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        REXCVAR_SET(skate3_render_aspect, std::string(buf));
      }
    }
  }
  {
    struct Preset {
      const char* label;
      int32_t w;
      int32_t h;
    };
    static const Preset kPresets[] = {
        {"Window (native pixels)", 0, 0},
        {"720p (height, width from aspect)", 0, 720},
        {"1080p (height, width from aspect)", 0, 1080},
        {"1440p (height, width from aspect)", 0, 1440},
        {"2160p (height, width from aspect)", 0, 2160},
        {"1152x640 (console render)", 1152, 640},
    };
    constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));
    static bool s_custom = false;
    const int32_t w = REXCVAR_GET(skate3_render_width);
    const int32_t h = REXCVAR_GET(skate3_render_height);
    int index = kPresetCount;  // Custom
    if (!s_custom) {
      for (int i = 0; i < kPresetCount; ++i) {
        if (kPresets[i].w == w && kPresets[i].h == h) {
          index = i;
          break;
        }
      }
    }
    const char* preview = index < kPresetCount ? kPresets[index].label : "Custom";
    if (ImGui::BeginCombo("resolution", preview)) {
      for (int i = 0; i <= kPresetCount; ++i) {
        const char* label = i < kPresetCount ? kPresets[i].label : "Custom";
        if (ImGui::Selectable(label, i == index)) {
          s_custom = i == kPresetCount;
          if (!s_custom) {
            REXCVAR_SET(skate3_render_width, kPresets[i].w);
            REXCVAR_SET(skate3_render_height, kPresets[i].h);
          }
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Window = render at the window's pixel size. A forced resolution\n"
          "renders at that size and scales to fit the window. Height-only\n"
          "presets take the width from the aspect ratio above; an exact\n"
          "W x H overrides the aspect setting.");
    }
    if (index == kPresetCount) {
      int cw = w;
      int ch = h;
      ImGui::SetNextItemWidth(110.0f);
      if (ImGui::InputInt("##w", &cw, 0, 0,
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        REXCVAR_SET(skate3_render_width, std::clamp(cw, 0, 16384));
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("x");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      if (ImGui::InputInt("##h", &ch, 0, 0,
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        REXCVAR_SET(skate3_render_height, std::clamp(ch, 0, 16384));
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(0 = auto)");
    }
  }
  uint32_t ow = 0;
  uint32_t oh = 0;
  skate3::render_resolution::LastOutputSize(ow, oh);
  if (ow != 0 && oh != 0) {
    ImGui::TextDisabled("Rendering at %u x %u (%.3f:1)", ow, oh,
                        double(ow) / double(oh));
  }
}

void DrawMaterialShadingSection() {
  REXCVAR_SET(skate3_native_render_scene_lightmaps,
              CvarCheckbox("Lightmaps", REXCVAR_GET(skate3_native_render_scene_lightmaps),
                           "Baked lighting atlas (lm^2) on world materials"));
  REXCVAR_SET(skate3_native_render_scene_world_v2,
              CvarCheckbox("World shading v2 (normal/detail/spec maps)",
                           REXCVAR_GET(skate3_native_render_scene_world_v2),
                           "The game's own GetTangentLight kd + phong terms "
                           "per pixel on the environment families. Off = the "
                           "v1 flat-map constants (A/B)."));
  {
    const bool positive =
        REXCVAR_GET(skate3_native_render_scene_world_v2_tan_sign) >= 0.0;
    int index = positive ? 0 : 1;
    const char* kSigns[] = {"+1", "-1"};
    if (ImGui::Combo("v2 tangent sign", &index, kSigns, 2)) {
      REXCVAR_SET(skate3_native_render_scene_world_v2_tan_sign,
                  index == 0 ? 1.0 : -1.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Polarity relating cross(binormal, normal) x handedness to the\n"
          "game's tangent. Wrong sign flips the sun-direction response of\n"
          "normal-mapped world surfaces (F5 to compare).");
    }
  }
  REXCVAR_SET(skate3_native_render_scene_dynobj_v2,
              CvarCheckbox("Dynamic-object shading v2",
                           REXCVAR_GET(skate3_native_render_scene_dynobj_v2),
                           "Per-pixel normal/detail/spec maps and both phong "
                           "spec variants on the dynamicobject families. Off "
                           "= the v1 flat response (A/B)."));
  REXCVAR_SET(skate3_native_render_scene_macro,
              CvarCheckbox("Macro overlay", REXCVAR_GET(skate3_native_render_scene_macro),
                           "Large-scale grime/crack multiply (ground/wall weathering)"));
  REXCVAR_SET(skate3_native_render_scene_decals,
              CvarCheckbox("Decal art composite",
                           REXCVAR_GET(skate3_native_render_scene_decals),
                           "Graffiti/paint art lerped over environment.decal sections"));
  REXCVAR_SET(skate3_native_render_scene_transparents,
              CvarCheckbox("Transparent sub-pass",
                           REXCVAR_GET(skate3_native_render_scene_transparents),
                           "environment.transparent items (mist sheets, glass, fences)"));
  REXCVAR_SET(skate3_native_render_scene_backface_cull,
              CvarCheckbox("Backface cull (game parity)",
                           REXCVAR_GET(skate3_native_render_scene_backface_cull),
                           "World env materials cull like the game's material "
                           "XMLs (CULLMODE=FRONT); off = cull-none (shows "
                           "interior faces, double glass panes)"));

  ImGui::SeparatorText("Reflective glass");
  REXCVAR_SET(skate3_native_render_scene_refl_bias_auto,
              CvarCheckbox("Auto normal tilt (from material)",
                           REXCVAR_GET(skate3_native_render_scene_refl_bias_auto),
                           "Derive the constant normal tilt from each "
                           "material's own detail texture (hardware-exact "
                           "BC1 decode). Uncheck to drive the sliders below."));
  ImGui::BeginDisabled(REXCVAR_GET(skate3_native_render_scene_refl_bias_auto));
  REXCVAR_SET(skate3_native_render_scene_refl_bias_x,
              CvarSlider("normal tilt X (horizontal)",
                         REXCVAR_GET(skate3_native_render_scene_refl_bias_x),
                         -0.2f, 0.2f, "%.3f",
                         "Manual tilt; default 0.0353 = the hardware BC1 "
                         "fold. 0.01 is ~1 degree of reflection rotation."));
  REXCVAR_SET(skate3_native_render_scene_refl_bias_y,
              CvarSlider("normal tilt Y (vertical)",
                         REXCVAR_GET(skate3_native_render_scene_refl_bias_y),
                         -0.2f, 0.2f, "%.3f",
                         "Manual tilt; default 0.0196 = the hardware BC1 "
                         "fold."));
  ImGui::EndDisabled();
}

void DrawShadowsSection() {
  ImGui::SeparatorText("Game cascades (CSM)");
  REXCVAR_SET(skate3_native_render_scene_shadows,
              CvarCheckbox("Dynamic shadows",
                           REXCVAR_GET(skate3_native_render_scene_shadows),
                           "The game's CSM: skater/NPC/prop shadows onto the "
                           "world, cascade matrices captured per frame"));
  REXCVAR_SET(skate3_native_render_scene_shadow_caster_parity,
              CvarCheckbox("Caster parity",
                           REXCVAR_GET(skate3_native_render_scene_shadow_caster_parity),
                           "Character pieces cast only when the game "
                           "submitted them to its own shadow passes this "
                           "frame (e.g. the CAS trucker hat never casts)"));
  REXCVAR_SET(skate3_native_render_scene_char_shadow_exact,
              CvarCheckbox("Exact character shadow receive",
                           REXCVAR_GET(skate3_native_render_scene_char_shadow_exact),
                           "Characters sample the atlas with the game's math: "
                           "captured per-cascade biases + nine point taps "
                           "/9 (cacstamp_skin_nisPS port). Off = legacy "
                           "single bilinear tap."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss,
              CvarCheckbox(
                  "Soft shadows (PCSS)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss),
                  "Contact-hardening filter. Off = the game's fixed-width "
                  "blurred atlas sampling."));
  ImGui::BeginDisabled(!REXCVAR_GET(skate3_native_render_scene_shadow_pcss));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_sun_deg,
              CvarSlider(
                  "sun angular size (deg)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_sun_deg),
                  0.1f, 8.0f, "%.1f",
                  "Penumbra growth per meter of caster height (the real sun "
                  "is ~0.53 deg)."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_max_m,
              CvarSlider(
                  "max penumbra (m)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_max_m),
                  0.05f, 5.0f, "%.2f"));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_blocker_m,
              CvarSlider(
                  "blocker search (m)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_blocker_m),
                  0.1f, 8.0f, "%.2f",
                  "Must be at least the max penumbra; larger lets unrelated "
                  "nearby casters soften edges."));
  REXCVAR_SET(skate3_native_render_scene_shadow_pcss_min_texel,
              CvarSlider(
                  "min filter (texels)",
                  REXCVAR_GET(skate3_native_render_scene_shadow_pcss_min_texel),
                  0.0f, 4.0f, "%.2f",
                  "Minimum filter radius in atlas texels (keeps hardened "
                  "edges filtered)."));
  ImGui::EndDisabled();
}

void DrawCharactersSection() {
  REXCVAR_SET(skate3_native_render_scene_dynamic_items,
              CvarCheckbox("Dynamic items",
                           REXCVAR_GET(skate3_native_render_scene_dynamic_items),
                           "Characters, movable props, cloth"));
  REXCVAR_SET(skate3_native_render_scene_entity_fade,
              CvarCheckbox("Entity spawn/distance fade",
                           REXCVAR_GET(skate3_native_render_scene_entity_fade),
                           "Honor the game's per-entity opacity (NPC/traffic "
                           "spawn settle + distance fade). Off = opaque from "
                           "the first draw."));

  ImGui::SeparatorText("Entity identity (game-owned data)");
  REXCVAR_SET(skate3_native_render_scene_entity_ident,
              CvarCheckbox("Identity store",
                           REXCVAR_GET(skate3_native_render_scene_entity_ident),
                           "MeshContext -> owning entity map from the game's "
                           "BindConstants walks. Off also disables the serve "
                           "paths below that depend on it."));
  ImGui::BeginDisabled(!REXCVAR_GET(skate3_native_render_scene_entity_ident));
  REXCVAR_SET(skate3_native_render_scene_entity_fade_serve,
              CvarCheckbox("Skater fade from entity",
                           REXCVAR_GET(skate3_native_render_scene_entity_fade_serve),
                           "Skater-family opacity from the entity's own "
                           "field (+496) instead of the captured row"));
  REXCVAR_SET(skate3_native_render_scene_entity_ropa_world,
              CvarCheckbox("Garment world from entity",
                           REXCVAR_GET(skate3_native_render_scene_entity_ropa_world),
                           "Refused ROPA garment worlds served from the owner "
                           "entity's m_MatLtoWTrans"));
  ImGui::EndDisabled();
  REXCVAR_SET(skate3_native_render_scene_palette_serve,
              CvarCheckbox("Bone palettes from game packs",
                           REXCVAR_GET(skate3_native_render_scene_palette_serve),
                           "Serve character palettes from the game's packed "
                           "m_matrices snapshots (the exact bytes its VS "
                           "upload consumes) instead of bank captures"));

  ImGui::SeparatorText("LivingWorld (NPCs / traffic)");
  REXCVAR_SET(skate3_native_render_scene_lw_fade,
              CvarCheckbox("LW fade from entity",
                           REXCVAR_GET(skate3_native_render_scene_lw_fade),
                           "NPC/vehicle fade alpha from the entity store "
                           "instead of the per-draw captured row"));
  REXCVAR_SET(skate3_native_render_scene_lw_identity,
              CvarCheckbox("LW pose rings by MeshContext",
                           REXCVAR_GET(skate3_native_render_scene_lw_identity),
                           "Clone reshuffles can no longer mispair smoothing "
                           "rings (teleport/slide class)"));
  REXCVAR_SET(skate3_native_render_scene_lw_gap_fill,
              CvarCheckbox("LW gap fill",
                           REXCVAR_GET(skate3_native_render_scene_lw_gap_fill),
                           "Republish a live NPC for up to 2 frames when its "
                           "submit records drop out (blinks)"));
  REXCVAR_SET(skate3_native_render_scene_lw_palette,
              CvarCheckbox("LW authoritative palettes",
                           REXCVAR_GET(skate3_native_render_scene_lw_palette),
                           "Edge-of-view vehicles use the entity's own packed "
                           "palette instead of guessed caster banks"));

  ImGui::SeparatorText("Capture robustness");
  REXCVAR_SET(skate3_native_render_scene_dyn_gap_fill,
              CvarCheckbox("Dynamic gap fill",
                           REXCVAR_GET(skate3_native_render_scene_dyn_gap_fill),
                           "Re-publish character/cloth items for up to two "
                           "missed frames (high-fps body flicker)"));
  REXCVAR_SET(skate3_native_render_scene_char_rows_inst,
              CvarCheckbox("Per-instance lighting fallback",
                           REXCVAR_GET(skate3_native_render_scene_char_rows_inst),
                           "Capture-failed items reuse their own last "
                           "validated lighting rows (hair blend flicker)"));
  REXCVAR_SET(skate3_native_render_scene_caster_refresh_all,
              CvarCheckbox("Caster palette refresh (all)",
                           REXCVAR_GET(skate3_native_render_scene_caster_refresh_all),
                           "Refresh every caster-sourced skinned capture by "
                           "palette identity, garments included"));

  ImGui::SeparatorText("Cloth (ROPA)");
  REXCVAR_SET(skate3_native_render_scene_ropa_blend,
              CvarCheckbox("Blend cloth onto play clock",
                           REXCVAR_GET(skate3_native_render_scene_ropa_blend),
                           "Lerp cloth shape generations to the smoothed body "
                           "pose (tee jelly / clip-through)"));
  REXCVAR_SET(skate3_native_render_scene_ropa_boxcar,
              CvarCheckbox("Cloth uses body boxcar kernel",
                           REXCVAR_GET(skate3_native_render_scene_ropa_boxcar),
                           "Same 8-tap filter as the body bones instead of a "
                           "plain 2-generation lerp"));
  REXCVAR_SET(skate3_native_render_scene_ropa_bias,
              CvarSliderInt("cloth pairing bias (poses)",
                            REXCVAR_GET(skate3_native_render_scene_ropa_bias),
                            -2, 2,
                            "+ = fresher cloth shape, - = older; trims a "
                            "residual constant drape lag/lead"));
}

void DrawWorldContentSection() {
  REXCVAR_SET(skate3_native_render_scene_world_items,
              CvarCheckbox("World items", REXCVAR_GET(skate3_native_render_scene_world_items),
                           "Static geometry from the world sort lists"));
  REXCVAR_SET(
      skate3_native_render_scene_retain_offscreen,
      CvarCheckbox("Retain off-screen statics",
                   REXCVAR_GET(skate3_native_render_scene_retain_offscreen),
                   "Keep recently seen statics drawn while the game "
                   "view-culls them (the smoothed camera trails the guest "
                   "pose; without this, geometry tears down at screen edges)"));
  REXCVAR_SET(skate3_native_render_scene_occlusion_cull,
              CvarCheckbox("Occlusion cull",
                           REXCVAR_GET(skate3_native_render_scene_occlusion_cull),
                           "Skip statics provably hidden behind rendered "
                           "geometry (conservative depth grid). Shadow passes "
                           "are exempt."));
  ImGui::BeginDisabled(!REXCVAR_GET(skate3_native_render_scene_occlusion_cull));
  ImGui::Indent();
  REXCVAR_SET(skate3_native_render_scene_occlusion_cull_build,
              CvarCheckbox("skip hidden item rebuilds",
                           REXCVAR_GET(skate3_native_render_scene_occlusion_cull_build),
                           "Skip the per-frame rebuild for hidden statics on "
                           "3 of 4 frames (staggered)"));
  REXCVAR_SET(skate3_native_render_scene_occlusion_cull_guest,
              CvarCheckbox("skip hidden guest dispatch",
                           REXCVAR_GET(skate3_native_render_scene_occlusion_cull_guest),
                           "Also skip the game's own draw-list dispatch for "
                           "hidden items (the frame-time-critical cost)"));
  ImGui::Unindent();
  ImGui::EndDisabled();
  REXCVAR_SET(skate3_native_render_scene_quadlists,
              CvarCheckbox("Particles",
                           REXCVAR_GET(skate3_native_render_scene_quadlists),
                           "The game's particle systems (lionREC quad lists): "
                           "sprite x vertex color, straight alpha, no z-write, "
                           "like lionREC_RenderPS"));
  REXCVAR_SET(skate3_native_render_scene_splines,
              CvarCheckbox("Neon splines", REXCVAR_GET(skate3_native_render_scene_splines),
                           "Death Race gates, Follow the Leader ribbons, "
                           "waypoint arrows, marker beams"));
  REXCVAR_SET(skate3_native_render_scene_selection_outline,
              CvarCheckbox("Selection outline",
                           REXCVAR_GET(skate3_native_render_scene_selection_outline),
                           "Park editor / object mover blue contour "
                           "(postfx_edgedetectstencil port)"));
  REXCVAR_SET(skate3_native_render_scene_simpledraw,
              CvarCheckbox("SimpleDraw primitives",
                           REXCVAR_GET(skate3_native_render_scene_simpledraw),
                           "The game's debug primitives (e.g. "
                           "cv_ShowRenderSkeleton bone lines)"));

  ImGui::SeparatorText("2D / HUD");
  REXCVAR_SET(skate3_native_render_scene_2d,
              CvarCheckbox("2D / HUD replay", REXCVAR_GET(skate3_native_render_scene_2d),
                           "APT/Flash HUD + glyph text + SimpleDraw icons"));
  REXCVAR_SET(skate3_native_render_scene_2d_sharp,
              CvarSlider("HUD sharpen",
                         REXCVAR_GET(skate3_native_render_scene_2d_sharp),
                         0.0f, 2.0f, "%.2f",
                         "Catmull-Rom + clamped unsharp mask on magnified "
                         "720p HUD tiles. 0 = plain bilinear (the console "
                         "look)."));
  REXCVAR_SET(skate3_menu_blur_sigma,
              CvarSlider("settings backdrop blur (1080p px)",
                         REXCVAR_GET(skate3_menu_blur_sigma), 0.0f, 30.0f,
                         "%.1f",
                         "Gaussian sigma behind the host settings overlay"));
}

void DrawCoverageSection() {
  ImGui::TextDisabled(
      "Which screens the native renderer draws, and which yield\n"
      "to the emulated output.");
  REXCVAR_SET(skate3_native_render_scene_pause_native,
              CvarCheckbox("Pause menu native",
                           REXCVAR_GET(skate3_native_render_scene_pause_native),
                           "Keep rendering natively through the in-game pause "
                           "menu (no cache clears on pause entry)"));
  REXCVAR_SET(skate3_native_render_scene_loading_native,
              CvarCheckbox("Loading screens native",
                           REXCVAR_GET(skate3_native_render_scene_loading_native),
                           "Black backdrop + the game's captured 2D loading UI"));
  REXCVAR_SET(skate3_native_render_scene_boot_native,
              CvarCheckbox("Boot flow native",
                           REXCVAR_GET(skate3_native_render_scene_boot_native),
                           "Intro videos, boot frontend and the first load"));
  REXCVAR_SET(skate3_native_render_scene_fmv_native,
              CvarCheckbox("FMVs native",
                           REXCVAR_GET(skate3_native_render_scene_fmv_native),
                           "VP6 YUV planes combined in the 2D replay at the "
                           "movie quad's own geometry"));
  ImGui::BeginDisabled(REXCVAR_GET(skate3_native_render_scene_fmv_native));
  REXCVAR_SET(skate3_native_render_scene_fmv_yield,
              CvarCheckbox("FMV fallback: yield to emulated",
                           REXCVAR_GET(skate3_native_render_scene_fmv_yield),
                           "Used when native FMV is off or its planes fail "
                           "to publish"));
  ImGui::EndDisabled();
  REXCVAR_SET(skate3_native_render_scene_photo_native,
              CvarCheckbox("Photo editor native",
                           REXCVAR_GET(skate3_native_render_scene_photo_native),
                           "The game's own postfx chain (DOF / saturation / "
                           "brightness / contrast / vignette) as exact ucode "
                           "ports driven by its live constants"));
  ImGui::BeginDisabled(REXCVAR_GET(skate3_native_render_scene_photo_native));
  REXCVAR_SET(skate3_native_render_scene_photo_yield,
              CvarCheckbox("Photo editor fallback: yield to emulated",
                           REXCVAR_GET(skate3_native_render_scene_photo_yield)));
  ImGui::EndDisabled();
  REXCVAR_SET(skate3_native_render_scene_cas_yield,
              CvarCheckbox("Create-a-skater: yield to emulated",
                           REXCVAR_GET(skate3_native_render_scene_cas_yield),
                           "Off = CAS renders natively (deck shift-recolor "
                           "masks and the editor DOF not modeled)"));
}

void DrawPacingSection() {
  REXCVAR_SET(skate3_native_render_scene_smooth_camera,
              CvarCheckbox("Smooth camera + entity poses",
                           REXCVAR_GET(skate3_native_render_scene_smooth_camera),
                           "The guest updates its camera/entities on its own ~200 Hz "
                           "sim tick; raw poses judder at high render rates. Re-times "
                           "them on the host clock (1 kHz camera sampler + pose "
                           "interpolation, a few ms of camera latency)."));
  REXCVAR_SET(skate3_native_render_scene_smooth_camera_filter_ms,
              CvarSlider("camera filter (ms, 0 = off)",
                         REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms),
                         0.0f, 100.0f, "%.0f",
                         "Boxcar average over the camera pose signal. The "
                         "game's camera advances in 60 Hz-quantized lumps at "
                         "high fps; 50 ms = three 60 Hz periods cancels it "
                         "for ~25 ms extra camera latency."));
  REXCVAR_SET(skate3_native_render_scene_sort_opaque,
              CvarCheckbox("Front-to-back opaque sort",
                           REXCVAR_GET(skate3_native_render_scene_sort_opaque),
                           "Early-z rejects occluded pixels before the material shading"));
  REXCVAR_SET(skate3_guest_fps_cap_auto,
              CvarCheckbox("Auto fps cap (below display refresh)",
                           REXCVAR_GET(skate3_guest_fps_cap_auto),
                           "Cap a safety margin below the current display's "
                           "refresh (4 fps or 5%, whichever is larger): the "
                           "VRR recipe without per-monitor tuning"));
  ImGui::BeginDisabled(REXCVAR_GET(skate3_guest_fps_cap_auto));
  {
    int cap = int(REXCVAR_GET(skate3_guest_fps_cap));
    if (ImGui::InputInt("Guest fps cap (0 = off)", &cap, 10, 30)) {
      if (cap < 0) cap = 0;
      if (cap > 1000) cap = 1000;
      REXCVAR_SET(skate3_guest_fps_cap, double(cap));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Pace guest frames on an even beat. Set a few fps below the display "
          "refresh (with G-Sync/VRR this is what makes motion read as smooth; "
          "uncapped, the guest's irregular frame times drive the refresh "
          "directly).");
    }
  }
  ImGui::EndDisabled();
}

void DrawDiagnosticsSection() {
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_debug);
    const char* kModes[] = {"0: normal", "1: clear only", "2: solid colors",
                            "3: first 20 items", "4: no depth"};
    if (ImGui::Combo("debug mode", &mode, kModes, 5)) {
      REXCVAR_SET(skate3_native_render_scene_debug, mode);
    }
  }
  // Material/shading diagnostics. Each mode replaces the world surfaces with
  // one of their shading INPUTS, so it is visible at a glance which branch
  // owns a surface and what that branch is actually reading. Rebuilds the
  // scene shaders on change (a compile-time variant), so expect a brief
  // hitch when switching.
  {
    static constexpr int32_t kProbeValues[12] = {0, 1, 2, 3,  4,  5,
                                                 6, 7, 8, 9, 10, 11};
    static constexpr const char* kProbeLabels[12] = {
        "off",
        "1 macro sample (scaled uv)",
        "2 macro overlay result",
        "3 macro inputs (R=gate G=uvscale B=opacity)",
        "4 macro sample (raw uv)",
        "5 family colour key",
        "6 decal alpha (blend weight)",
        "7 decal rgb",
        "8 decal uv3",
        "9 uv3 in range (green=in, red=out)",
        "10 decal alpha at mip 0",
        "11 decal alpha at uv (0.5,0.5)",
    };
    REXCVAR_SET(
        skate3_native_render_scene_decal_probe,
        ValueCombo("material probe",
                   REXCVAR_GET(skate3_native_render_scene_decal_probe),
                   kProbeValues, kProbeLabels, 12,
                   "World-material diagnostic overlay. 5 (family colour key) "
                   "shows which shading family owns each surface: red = "
                   "environment.default, orange = environmentsimple, yellow = "
                   "decal, green = decal_tileable, cyan/blue = reflective, "
                   "magenta = alphatest, dark green = tree, white = "
                   "incandescent. 1-4 inspect the macro (grime) overlay "
                   "inputs on those surfaces."));
  }

  ImGui::SeparatorText("Reflective glass isolation (env fam 5/6/13)");
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_refl_mode);
    const char* kReflModes[] = {"0: normal",
                                "1: cube reflection OFF",
                                "2: cube at absolute LOD (slider)",
                                "3: flat normal (no normal map)",
                                "4: visualize cube sample only",
                                "5: body only (no spec, no cube)",
                                "6: normal-map LOD bias (slider)",
                                "7: visualize lightmap sample",
                                "8: visualize lightmap UV (frac x16)",
                                "9: lightmap resolve status (blue = missing)"};
    if (ImGui::Combo("refl mode", &mode, kReflModes, 10)) {
      REXCVAR_SET(skate3_native_render_scene_refl_mode, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Live isolation of the glass-facade reflection term. Flip F5 to "
          "compare against the emulated frame at each setting:\n"
          "1 removes the cube term; if the artifact survives, it is NOT "
          "the reflection.\n2 + slider finds which mip level (if any) "
          "matches the emulated look.\n3 tests the normal-map perturbation."
          "\n4 shows exactly what the reflection vector samples.\n5 leaves "
          "only diffuse x lightmap.");
    }
    REXCVAR_SET(skate3_native_render_scene_refl_lod,
                CvarSlider("refl LOD / extra bias",
                           REXCVAR_GET(skate3_native_render_scene_refl_lod),
                           -4.0f, 12.0f, "%.1f",
                           "Mode 2: the absolute cube mip level (0 = "
                           "sharpest; ~9 = the face average).\nOther modes: "
                           "extra LOD bias added to the automatic "
                           "640p-parity bias."));
  }

  ImGui::SeparatorText("Lighting lab");
  {
    const bool was_on = REXCVAR_GET(skate3_native_render_scene_sun_override);
    const bool now_on =
        CvarCheckbox("Sun override", was_on,
                     "Move the sun with the sliders below: dynamic CSM "
                     "shadows and their receivers follow. Baked lightmap "
                     "shade and the sky dome's painted sun stay put (game "
                     "content), so this is a tuning tool, not a look.");
    if (now_on && !was_on) {
      // Seed the sliders from the captured sun so enabling the override
      // starts at the true position instead of jumping.
      float sun[3];
      skate3::native_scene::GetCapturedSunDir(sun);
      const float kRad = 57.29577951f;
      REXCVAR_SET(skate3_native_render_scene_sun_azimuth,
                  double(std::fmod(std::atan2(sun[0], sun[2]) * kRad + 360.0f,
                                   360.0f)));
      REXCVAR_SET(
          skate3_native_render_scene_sun_elevation,
          double(std::clamp(std::asin(std::clamp(sun[1], -1.0f, 1.0f)) * kRad,
                            2.0f, 88.0f)));
    }
    REXCVAR_SET(skate3_native_render_scene_sun_override, now_on);
  }
  ImGui::BeginDisabled(!REXCVAR_GET(skate3_native_render_scene_sun_override));
  REXCVAR_SET(skate3_native_render_scene_sun_azimuth,
              CvarSlider("sun azimuth (deg)",
                         REXCVAR_GET(skate3_native_render_scene_sun_azimuth),
                         0.0f, 360.0f, "%.0f"));
  REXCVAR_SET(skate3_native_render_scene_sun_elevation,
              CvarSlider("sun elevation (deg)",
                         REXCVAR_GET(skate3_native_render_scene_sun_elevation),
                         2.0f, 88.0f, "%.0f"));
  ImGui::EndDisabled();

  ImGui::SeparatorText("Logging");
  REXCVAR_SET(skate3_native_render_scene_perf_log,
              CvarCheckbox("Perf breakdown log",
                           REXCVAR_GET(skate3_native_render_scene_perf_log),
                           "Periodic native-renderer performance lines"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  {
    int interval = REXCVAR_GET(skate3_native_render_scene_perf_interval);
    if (ImGui::InputInt("frames##perf_interval", &interval, 60, 600)) {
      REXCVAR_SET(skate3_native_render_scene_perf_interval,
                  std::clamp(interval, 60, 6000));
    }
  }
  REXCVAR_SET(skate3_native_render_scene_shader_census,
              CvarCheckbox("Shader census",
                           REXCVAR_GET(skate3_native_render_scene_shader_census),
                           "Log each (material, game pixel shader) pair once, "
                           "with the native family it got (shader-census: "
                           "lines): finds game shaders not yet implemented"));
  REXCVAR_SET(skate3_native_render_scene_char_track,
              CvarCheckbox("Character track log (heavy)",
                           REXCVAR_GET(skate3_native_render_scene_char_track),
                           "One line per frame following the main character "
                           "and garment through motion smoothing"));

  ImGui::SeparatorText("Judder isolation");
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_synthetic_pan);
    const char* kPanModes[] = {"0: off", "1: time-based (build clock)",
                               "2: fixed step per frame", "3: through smoother"};
    if (ImGui::Combo("Synthetic pan (P)", &mode, kPanModes, 4)) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Judder isolation probe: replaces the camera with a host-generated "
          "constant-rate 360-degree pan. AFTER ENGAGING, SWEEP THE REAL CAMERA "
          "AROUND ONCE WITH THE STICK; the game only submits what its own "
          "frustum sees; the sweep fills a static-world union so the full "
          "circle is populated.\n1 smooth = camera path is fine; 1 judders = "
          "frame pacing / present / display, not the camera.\n2 = constant "
          "angle per FRAME (smooth only if displayed frames are evenly spaced "
          ", the complement of 1).\n3 = feeds synthetic ~200 Hz samples "
          "through the camera smoother and logs reconstruction error "
          "(synthetic-pan: lines).");
    }
    float rate = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate));
    if (ImGui::SliderFloat("pan rate (deg/s)", &rate, 5.0f, 720.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_rate, double(rate));
    }
    float amp = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
    if (ImGui::SliderFloat("pan amplitude (0 = full spin)", &amp, 0.0f, 60.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_amp, double(amp));
    }
    if (ImGui::Button("Record camera signal (8 s)")) {
      skate3::native_scene::RecordCameraSignal(8.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then IMMEDIATELY pan the camera with the right stick at a "
          "steady rate for 8 seconds (synthetic pan should be OFF). Records "
          "every distinct guest camera pose with 1 kHz-sampler timestamps + "
          "the smoothed output, to logs\\cam_signal_<ts>.csv; offline "
          "analysis shows whether the game's own camera signal is jerky at "
          "the source.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Record bone signal (6 s)")) {
      skate3::native_scene::RecordBoneSignal(6.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then skate at a steady speed for 6 seconds. Records every "
          "raw entity pose (bone palettes with timestamps) + the rendered "
          "interpolation output to logs\\bone_signal_<ts>.bin, so wheel-vs-"
          "deck lag/orbit can be measured and candidate fixes replayed "
          "offline against real data.");
    }
  }
}

void DrawCachesSection() {
  REXCVAR_SET(
      skate3_native_render_scene_tex_revalidate,
      CvarCheckbox("Texture payload revalidation",
                   REXCVAR_GET(skate3_native_render_scene_tex_revalidate),
                   "Re-fingerprint cached texture payloads every 16 frames, re-decode "
                   "on change (heals late-composed lightmap pages)"));
  REXCVAR_SET(skate3_native_render_scene_mesh_revalidate,
              CvarCheckbox("Mesh payload revalidation",
                           REXCVAR_GET(skate3_native_render_scene_mesh_revalidate),
                           "Re-decode cached meshes when the guest payload fingerprint "
                           "changes (streaming arena reuse / CPU-animated buffers)"));
  if (ImGui::Button("Flush texture cache")) {
    skate3::native_scene::FlushTextureCache();
  }
  ImGui::SameLine();
  if (ImGui::Button("Flush mesh cache")) {
    skate3::native_scene::FlushMeshCache();
  }

  ImGui::SeparatorText("Budgets");
  REXCVAR_SET(skate3_native_render_scene_tex_store_mb,
              CvarSliderInt("texture store (MB)",
                            REXCVAR_GET(skate3_native_render_scene_tex_store_mb),
                            256, 8192,
                            "GPU byte budget; over it the LRU drains "
                            "oldest-first so VRAM does not pile up across "
                            "map changes"));
  REXCVAR_SET(skate3_native_render_scene_mesh_store_mb,
              CvarSliderInt("mesh store (MB)",
                            REXCVAR_GET(skate3_native_render_scene_mesh_store_mb),
                            256, 8192, "Vertex + index buffer budget"));
  REXCVAR_SET(skate3_native_render_scene_prewarm_budget_ms,
              CvarSliderInt("load-screen prewarm (ms/frame)",
                            REXCVAR_GET(skate3_native_render_scene_prewarm_budget_ms),
                            0, 100,
                            "Decode registered world meshes/textures behind "
                            "loading screens. Higher = less pop-in after map "
                            "changes. 0 = off."));
  REXCVAR_SET(skate3_native_render_scene_warmup_budget_ms,
              CvarSliderInt("post-takeover settle (ms/frame)",
                            REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms),
                            0, 50,
                            "Mops up what prewarm missed right after a load. "
                            "0 disables the takeover gates entirely."));
  REXCVAR_SET(skate3_native_render_scene_settle_max_frames,
              CvarSliderInt("settle cap (frames)",
                            REXCVAR_GET(skate3_native_render_scene_settle_max_frames),
                            0, 3000,
                            "Hard cap on the settle pass after takeover. 0 = "
                            "uncapped."));
  REXCVAR_SET(skate3_native_render_scene_mesh_decode_budget,
              CvarSliderInt("inline mesh decodes / frame",
                            REXCVAR_GET(skate3_native_render_scene_mesh_decode_budget),
                            0, 256,
                            "Dynamic (skinned/cloth) payloads decoded on the "
                            "render thread per frame. 0 = unlimited."));
}

}  // namespace

void MaxQualityAutoCycle(uint64_t frame_number) {
  const int32_t period = REXCVAR_GET(skate3_native_render_scene_maxq_cycle);
  if (period <= 0) {
    return;
  }
  static uint64_t next_frame = 0;
  if (next_frame == 0) {
    next_frame = frame_number + uint64_t(period);
    return;
  }
  if (frame_number >= next_frame) {
    next_frame = frame_number + uint64_t(period);
    SetMaxQuality(!s_max_quality.active);
    REXLOG_INFO("native-scene: max-quality auto-cycle -> {}",
                s_max_quality.active ? "ON" : "OFF");
  }
}

void NativeDebugDialog::Show() {
  visible_ = true;
  SetDrawActive(true);
}

void NativeDebugDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  SetDrawActive(false);
}

void NativeDebugDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void NativeDebugDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!visible_) {
    return;
  }
  bool open = visible_;
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Native Render Debug (F12)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) {
      Hide();
    }
    return;
  }

  // Renderer master switches stay visible above the sections: they gate
  // everything below and are the first thing checked when bisecting.
  if (!REXCVAR_GET(skate3_native_render)) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                       "skate3_native_render hook layer is OFF (boot-time)");
  }
  {
    const bool v = CvarCheckbox("Native scene renderer (F5)",
                                REXCVAR_GET(skate3_native_render_scene),
                                "Full native/emulated switch, same as F5");
    if (v != REXCVAR_GET(skate3_native_render_scene)) {
      skate3::native_scene::ToggleSceneEnabled();
    }
  }
  REXCVAR_SET(native_render_suppress_emulated_draws,
              CvarCheckbox("Suppress emulated draws",
                           REXCVAR_GET(native_render_suppress_emulated_draws),
                           "Skip emulated GPU work for framebuffer-sized passes while "
                           "native output is active (perf). Small-surface passes "
                           "(lightmap page composition) always run."));
  REXCVAR_SET(skate3_native_render_mode_indicator,
              CvarCheckbox("Renderer indicator (NATIVE / EMULATED)",
                           REXCVAR_GET(skate3_native_render_mode_indicator),
                           "Top-right readout of which renderer produced the "
                           "last frame. Always shown while the native scene "
                           "is off."));

  if (ImGui::CollapsingHeader("Showcase & capture",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawShowcaseCaptureSection();
  }
  if (ImGui::CollapsingHeader("Output resolution",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawOutputSection();
  }
  if (ImGui::CollapsingHeader("Image quality",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawImageQualitySection();
  }
  if (ImGui::CollapsingHeader("Material shading")) {
    DrawMaterialShadingSection();
  }
  if (ImGui::CollapsingHeader("Shadows")) {
    DrawShadowsSection();
  }
  if (ImGui::CollapsingHeader("Characters & entities")) {
    DrawCharactersSection();
  }
  if (ImGui::CollapsingHeader("World content & overlays")) {
    DrawWorldContentSection();
  }
  if (ImGui::CollapsingHeader("Native coverage")) {
    DrawCoverageSection();
  }
  if (ImGui::CollapsingHeader("Smoothness & pacing")) {
    DrawPacingSection();
  }
  if (ImGui::CollapsingHeader("Diagnostics")) {
    DrawDiagnosticsSection();
  }
  if (ImGui::CollapsingHeader("Caches & streaming")) {
    DrawCachesSection();
  }

  ImGui::End();
  if (s_showcase_setup_open) {
    DrawShowcaseSetupWindow(&s_showcase_setup_open);
  }
  if (!open) {
    Hide();
  }
}

void RenderModeIndicator::OnDraw(ImGuiIO& io) {
  // Force-show while the native scene renderer is switched off (F5, the
  // settings Renderer row, or the boot-time hook-layer master) or has hit an
  // unrecoverable failure and permanently yields to the emulated output:
  // running emulated is a degraded state the player should be able to see
  // even with the indicator cvar off. Natural per-frame yields while the
  // scene renderer is enabled (menus/loading) do not trigger this.
  const bool scene_off = !REXCVAR_GET(skate3_native_render) ||
                         !REXCVAR_GET(skate3_native_render_scene) ||
                         skate3::native_scene::SceneFailed();
  if (!REXCVAR_GET(skate3_native_render_mode_indicator) && !scene_off) {
    return;
  }
  // Pre-runtime (installer wizards) no guest frame exists yet - there is no
  // renderer to indicate.
  if (rex::ui::Presenter* presenter = imgui_drawer()->presenter()) {
    const rex::ui::Presenter::GuestOutputPaintRect rect =
        presenter->GetLastGuestOutputPaintRect();
    if (rect.width <= 0 || rect.height <= 0) {
      return;
    }
  }
  const bool native = rex::graphics::IsNativeGuestOutputActive();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.35f);
  ImGui::Begin("##render_mode_indicator", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::TextColored(native ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                            : ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                     "%s", native ? "NATIVE" : "EMULATED");
  ImGui::End();
}

}  // namespace skate3
