#pragma once

namespace skate3::vr {
// One toggle per deliberate chord; consume the face button until released
// even when the modifier is released first. Disconnect clears the latch.
struct ViewToggleChord {
  bool latched = false;
  bool Update(bool connected, bool modifier, bool face) {
    if (!connected || (!modifier && !face)) { latched=false; return false; }
    if (!latched && modifier && face) { latched=true; return true; }
    return false;
  }
  bool ConsumeFace(bool face) const { return latched && face; }
};
}  // namespace skate3::vr
