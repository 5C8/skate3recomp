#include "skate3_render_resolution.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(skate3_render_width, 0, "Skate 3",
                     "Forced native render width in pixels (0 = automatic). With "
                     "skate3_render_height also set, the output is exactly "
                     "width x height and skate3_render_aspect is ignored; alone, "
                     "the height follows the aspect (forced, or the window's). "
                     "A size that does not match the window is scaled to fit and "
                     "letterboxed (present_letterbox).")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_render_height, 0, "Skate 3",
                     "Forced native render height in pixels (0 = automatic). "
                     "Alone, the width follows the aspect (forced, or the "
                     "window's): e.g. 1080 + 16:9 = 1920x1080 in any window. "
                     "640 = the console's own render height.")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_render_aspect, "window", "Skate 3",
                      "Forced output aspect ratio: \"window\" (follow the window, "
                      "the default), \"W:H\" (e.g. 16:9, 4:3, 21:9) or a decimal "
                      "(e.g. 2.39). The game camera is built for this aspect "
                      "(Hor+ wider than 16:9, Vert+ narrower) and the frame is "
                      "letterboxed/pillarboxed to fit the window. Ignored when "
                      "both skate3_render_width and skate3_render_height are set.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::render_resolution {
namespace {

std::atomic<uint32_t> g_last_width{0};
std::atomic<uint32_t> g_last_height{0};

uint32_t ClampDim(double v) {
  return uint32_t(std::clamp(std::lround(v), 2l, 16384l));
}

}  // namespace

double ParseAspect(const char* text) {
  if (text == nullptr) {
    return 0.0;
  }
  const std::string s(text);
  const size_t sep = s.find_first_of(":xX/");
  double ratio = 0.0;
  if (sep != std::string::npos) {
    const double a = std::atof(s.substr(0, sep).c_str());
    const double b = std::atof(s.substr(sep + 1).c_str());
    if (a > 0.0 && b > 0.0) {
      ratio = a / b;
    }
  } else {
    ratio = std::atof(s.c_str());
  }
  // Implausible ratios (and "window", which parses as 0) follow the window.
  return std::isfinite(ratio) && ratio >= 0.5 && ratio <= 8.0 ? ratio : 0.0;
}

bool ComputeOutputSize(uint32_t window_width, uint32_t window_height,
                       uint32_t& width, uint32_t& height) {
  if (window_width < 2 || window_height < 2) {
    return false;
  }
  const int32_t forced_w = REXCVAR_GET(skate3_render_width);
  const int32_t forced_h = REXCVAR_GET(skate3_render_height);
  const double forced_aspect =
      ParseAspect(REXCVAR_GET(skate3_render_aspect).c_str());
  const double aspect = forced_aspect > 0.0
                            ? forced_aspect
                            : double(window_width) / double(window_height);
  if (forced_w > 0 && forced_h > 0) {
    width = ClampDim(forced_w);
    height = ClampDim(forced_h);
  } else if (forced_h > 0) {
    height = ClampDim(forced_h);
    width = ClampDim(forced_h * aspect);
  } else if (forced_w > 0) {
    width = ClampDim(forced_w);
    height = ClampDim(forced_w / aspect);
  } else if (forced_aspect > 0.0) {
    // Largest rectangle of the forced aspect inside the window: pixels stay
    // 1:1 with the screen, the remainder is letterbox/pillarbox.
    if (double(window_width) / double(window_height) > forced_aspect) {
      height = window_height;
      width = ClampDim(window_height * forced_aspect);
    } else {
      width = window_width;
      height = ClampDim(window_width / forced_aspect);
    }
  } else {
    width = window_width;
    height = window_height;
  }
  // The SDK rounds down to even; report what will actually render.
  g_last_width.store(width & ~1u, std::memory_order_relaxed);
  g_last_height.store(height & ~1u, std::memory_order_relaxed);
  return true;
}

void LastOutputSize(uint32_t& width, uint32_t& height) {
  width = g_last_width.load(std::memory_order_relaxed);
  height = g_last_height.load(std::memory_order_relaxed);
}

}  // namespace skate3::render_resolution
