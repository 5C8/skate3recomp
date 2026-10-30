#include "native/skate3_native_mesh_layout.h"

#include <cassert>
#include <cstdint>

int main() {
  using skate3::native_scene::ValidVertexBufferSize;
  struct MeshSize {
    uint32_t bytes;
    uint32_t stride;
    uint32_t vertices;
    uint32_t maximum_index;
  };
  // Metadata from San Vanelona meshes whose indices and sampled positions
  // were checked against the vertex payload and mesh bounds. No game data.
  constexpr MeshSize padded[] = {
      {480, 36, 13, 12}, {1200, 36, 33, 32},
      {1520, 36, 42, 41}, {2240, 36, 62, 61},
      {8720, 36, 242, 241}, {1408, 36, 39, 38},
  };
  for (const auto& mesh : padded) {
    assert(ValidVertexBufferSize(mesh.bytes, mesh.stride));
    // Trailing bytes must never turn into an extra vertex.
    assert(mesh.bytes / mesh.stride == mesh.vertices);
    assert(mesh.maximum_index < mesh.vertices);
    assert(mesh.bytes % mesh.stride != 0);  // Old guard rejected every case.
  }

  // Existing tightly packed meshes remain valid, including non-aligned sizes.
  assert(ValidVertexBufferSize(1728, 36));
  assert(ValidVertexBufferSize(108, 36));
  assert(ValidVertexBufferSize(114, 38));
  assert(ValidVertexBufferSize(80, 36));   // Eight bytes of alignment padding.
  assert(ValidVertexBufferSize(64, 49));   // Maximum 15-byte alignment tail.

  assert(!ValidVertexBufferSize(0, 36));
  assert(!ValidVertexBufferSize(36, 0));
  assert(!ValidVertexBufferSize(32, 36));  // No complete vertex.
  assert(!ValidVertexBufferSize(73, 36));  // Not alignment padding.
  assert(!ValidVertexBufferSize(64, 48));  // 16-byte tail is not padding.
  assert(!ValidVertexBufferSize(64, 36));  // Aligned, but excessive tail.
  assert(!ValidVertexBufferSize(0xffffffffu, 36));
}
