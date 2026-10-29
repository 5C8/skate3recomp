// Guest 3D aspect ratio (the 64-bit double the game's camera builds its
// projection from). Revived from the removed ultrawide patch
// (src/old/skate3_dynamic_aspect.cpp) WITHOUT its HUD writes: the native
// renderer keeps the 2D stream in a centered 16:9 band itself.
//
// While the native renderer serves frames the double follows the output
// (window) aspect, so the game builds -- and culls against -- a Hor+ camera
// for wide windows. Outputs narrower than 16:9 keep 16/9 so the renderer's
// Vert+ extension still applies. Emulated frames get 16/9 back, so the
// original renderer stays exactly 16:9.

#include "skate3_guest_aspect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/graphics/native_guest_renderer.h>
#include <rex/system/xmemory.h>

#include "skate3_native_scene.h"
#include "skate3_vr.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, skate3_native_render_scene);

namespace {

// 16/9 big-endian double (0x3FFC71C71C71C71C). Retail reads it in
// ViewCamera::SetPerspectiveMatrixRadians (rex_Sk8_Render_ViewCamera_SetPerspectiveMatrixRadians_QAAXMMMMM_Z_82802B98) as
// "lfd f8,3144(r30)" off its constant block r30 = 0x822F8600 (sk83_na_m's
// own load is the absolute 0x8220A5F0); the block base itself is not the
// slot. The poll thread refuses to write unless the slot holds exactly 16/9,
// so a wrong address only disables the feature.
constexpr uint32_t kAspectSlotGuestF64 = 0x822F8600u + 3144u;
constexpr uint64_t kAspect16x9Bits = 0x3FFC71C71C71C71CULL;
constexpr double kBaseAspect = 16.0 / 9.0;

uint64_t ReadBE64(const uint8_t* p) {
  uint64_t raw;
  std::memcpy(&raw, p, 8);
  return __builtin_bswap64(raw);
}

void WriteBE64(uint8_t* p, uint64_t bits) {
  const uint64_t raw = __builtin_bswap64(bits);
  std::memcpy(p, &raw, 8);
}

// The slot is accepted only while it holds 16/9 or a plausible aspect we
// wrote earlier, so a wrong address never gets clobbered.
bool SlotLooksValid(uint64_t bits) {
  if (bits == kAspect16x9Bits) return true;
  double d;
  std::memcpy(&d, &bits, 8);
  return std::isfinite(d) && d >= 1.0 && d <= 8.0;
}

// Guest-thread scopes that must see the game's own 16:9 (see
// Skate3GuestAspectHold16x9); the slot writes are serialized so the poll
// thread can never land a widened value inside such a scope.
std::mutex g_slot_mutex;
int g_hold_16x9 = 0;
uint8_t* g_slot = nullptr;

double TargetAspect() {
  if (g_hold_16x9 > 0) {
    return kBaseAspect;
  }
  // F5 off restores 16:9 immediately (not when the renderer finally yields):
  // the native renderer holds the output during the hand-off until the
  // game's camera is back to 16:9, see g_scene_handoff_until_ms.
  if (!REXCVAR_GET(skate3_native_render_scene) ||
      !rex::graphics::IsNativeGuestOutputActive()) {
    return kBaseAspect;
  }
  // VR: the output is an eye image; the camera only has to cull wide
  // enough for head look (the eye passes bring their own projection).
  if (skate3::vr::WideCull()) {
    return std::clamp(skate3::vr::GuestAspect(), kBaseAspect, 8.0);
  }
  const double out = skate3::native_scene::g_native_output_aspect.load(std::memory_order_relaxed);
  if (!(out > 0.0)) {
    return kBaseAspect;
  }
  return std::clamp(out, kBaseAspect, 8.0);
}

void PollThread(rex::memory::Memory* memory) {
  auto* slot = memory->TranslateVirtual<uint8_t*>(kAspectSlotGuestF64);
  // Claim the slot only if it holds EXACTLY the game's 16/9 constant at
  // startup: on retail the address comes from code matching, not a symbol,
  // and any other plausible double must never be overwritten.
  if (slot == nullptr || ReadBE64(slot) != kAspect16x9Bits) {
    REXLOG_WARN("guest-aspect: slot {:08X} does not hold 16/9; aspect sync disabled",
                kAspectSlotGuestF64);
    return;
  }
  REXLOG_INFO("guest-aspect: aspect slot {:08X} confirmed (16/9)", kAspectSlotGuestF64);
  {
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    g_slot = slot;
  }
  for (;;) {
    std::unique_lock<std::mutex> lock(g_slot_mutex);
    const uint64_t current = ReadBE64(slot);
    if (!SlotLooksValid(current)) {
      g_slot = nullptr;
      return;  // something else owns the bytes now; stop writing
    }
    double target = TargetAspect();
    // Snap near-16:9 outputs to the exact constant the game ships with.
    if (std::abs(target - kBaseAspect) < 0.005) {
      target = kBaseAspect;
    }
    uint64_t bits;
    std::memcpy(&bits, &target, 8);
    if (target == kBaseAspect) {
      bits = kAspect16x9Bits;
    }
    if (bits != current) {
      WriteBE64(slot, bits);
    }
    lock.unlock();
    // 4 ms: a renderer switch reaches the game's camera within a frame.
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

}  // namespace

void Skate3GuestAspectHold16x9(bool begin) {
  std::lock_guard<std::mutex> lock(g_slot_mutex);
  g_hold_16x9 = std::max(0, g_hold_16x9 + (begin ? 1 : -1));
  if (begin && g_slot != nullptr && SlotLooksValid(ReadBE64(g_slot))) {
    WriteBE64(g_slot, kAspect16x9Bits);  // effective before the scope runs
  }
}

void Skate3InitGuestAspect(rex::memory::Memory* memory) {
  if (memory == nullptr) return;
  std::thread(PollThread, memory).detach();
}
