#pragma once

#include <cstdint>

namespace skate3::native_scene {

// Some custom maps store the allocation size, including up to 15 trailing
// alignment bytes, rather than the exact vertex payload size. DecodeMesh
// consumes only vb_bytes / stride complete vertices.
constexpr bool ValidVertexBufferSize(uint32_t vb_bytes, uint32_t stride) {
  if (stride == 0 || vb_bytes < stride) return false;
  const uint32_t tail = vb_bytes % stride;
  return tail == 0 || ((vb_bytes % 16) == 0 && tail < 16);
}

}  // namespace skate3::native_scene
