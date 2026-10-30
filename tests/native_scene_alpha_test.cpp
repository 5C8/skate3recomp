#include "native/shaders/scene_alpha.hlsli"

#include <cassert>
#include <initializer_list>

int main() {
  // Trees, animated trees and alpha-tested fence/wire materials must keep
  // their cutouts with no mask, a specular map, or a specular+normal pair.
  for (float family : {-7.0f, -7.25f, -9.0f, -10.0f}) {
    for (float binding : {0.0f, 3.0f, 4.0f}) {
      assert(SceneDiffuseAlphaTestEnabled(0, 0, binding, family));
      // Captured alpha-test OFF and character/cloth gloss still take priority.
      assert(!SceneDiffuseAlphaTestEnabled(0, -1, binding, family));
      assert(!SceneDiffuseAlphaTestEnabled(0.25f, 0, binding, family));
      assert(!SceneDiffuseAlphaTestEnabled(1, 0, binding, family));
    }
  }
  // Composited decal surfaces keep their existing alpha behavior.
  assert(!SceneDiffuseAlphaTestEnabled(0, 0, 1.5f, -4));
  assert(!SceneDiffuseAlphaTestEnabled(0, 0, 1.5f, -3));
  assert(SceneDiffuseAlphaTestEnabled(0, 0, 0, -4));
  // Other shader branches retain their existing gates.
  assert(!SceneDiffuseAlphaTestEnabled(0, 0, 3, -5));
  assert(SceneDiffuseAlphaTestEnabled(0, 0, 0, -2));
  assert(!SceneDiffuseAlphaTestEnabled(0, 0, 0, -21));
  assert(!SceneDiffuseAlphaTestEnabled(0, 0, 0, -30));
}
