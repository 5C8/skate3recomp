#pragma once

#include <cstdint>

// Native renderer output size: follows the window's physical client size by
// default (1:1 pixels at any aspect), or a forced resolution and/or aspect
// ratio (skate3_render_width / skate3_render_height / skate3_render_aspect).
// A forced output that does not match the window is scaled to fit and
// letterboxed/pillarboxed by the presenter (present_letterbox), keeping its
// aspect. The renderer's Hor+/Vert+ camera and 16:9 HUD band follow the
// output aspect, so a forced aspect is what the game camera builds against.
namespace skate3::render_resolution {

// Resolves this frame's output size from the window's physical client size.
// Returns false (keep the default sizing) when the window is degenerate.
bool ComputeOutputSize(uint32_t window_width, uint32_t window_height,
                       uint32_t& width, uint32_t& height);

// Last size ComputeOutputSize produced (0 x 0 before the first native frame);
// for UI readouts.
void LastOutputSize(uint32_t& width, uint32_t& height);

// Parses "W:H", "WxH" or a decimal ratio ("2.39"). Empty / "window" / anything
// unparsable yields 0 (follow the window).
double ParseAspect(const char* text);

}  // namespace skate3::render_resolution
