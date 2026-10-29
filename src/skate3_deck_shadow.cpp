// Deck (skateboard) shadow: the guest state the native renderer needs for it.
//
// The console draws the board's contact shadow as a deferred decal, not as
// shadow-map geometry. PostFX::CalculateDeckShadowMatrix (0x82993A20) builds a
// projection from the deck's world matrix (SkaterPresentation::GetDeckMatrix)
// and the camera, stores it at postfx+2128, and sub_827EB0A8 uploads those four
// rows as effect parameters -435838951..-435838948 -- the deck_AO shader's
// c0..c3. The driver is rex_Sk8_Render_AntiFrustumManager_DEBUG_DrawAntiFrustums_QAAXXZ_82994100, one pass per skater:
//
//     for (i = 0; i < skater_count; ++i) {
//       CalculateDeckShadowMatrix(postfx, i);
//       DispatchEffect(postfx[173], 1);   // draws the deck_AO box volume
//     }
//
// The shader itself (shader_dump/ps_postfx_deck_AO) reconstructs each pixel
// from the depth buffer, projects it into deck space and samples a static
// 64x128 5_6_5 texture that is a dark blob on WHITE, so the pass MODULATES the
// scene. scene_shadows.hlsli's DeckShadow() is the port of it.
//
// What this file publishes is the two things that port cannot derive: the
// board's world transform, and the decal texture's fetch constants. It does not
// replay the guest's c0..c2, which are view-space rows built against the GUEST
// camera -- our scene renders through a smoothed, re-timed one, so the decal
// would swim. InterpolateDynamicItems re-times the transform below onto the
// same playback clock the board itself renders from.

#include "generated/skate3_init.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <bit>
#include <cstring>
#include <stdlib.h>
#include <cstdint>

#include "skate3_deck_shadow.h"
#include <rex/logging.h>

namespace {

// The guest D3D device pointer (dword_833A5EA0 in the decompilation): its
// fetch-constant shadow at +0x480 carries the textures a draw bound.
constexpr uint32_t kDeviceGlobal = 0x8308446Cu;

float LoadF32(uint8_t* base, uint32_t addr) {
  return std::bit_cast<float>(REX_LOAD_U32(addr));
}

// Published state (written on the guest render thread, read by the renderer).
// A seqlock keeps a torn matrix from ever reaching the renderer: the readers
// retry while the counter is odd or moved.
constexpr uint32_t kMaxSkaters = 4;

struct PublishedPlacement {
  std::atomic<uint32_t> seq{0};
  float world[16] = {};
  std::atomic<bool> valid{false};
};

PublishedPlacement g_placements[kMaxSkaters];

std::atomic<uint32_t> g_decal_seq{0};
uint32_t g_decal_words[6] = {};
std::atomic<bool> g_decal_valid{false};

// Whether a skater currently exists. GetDeckMatrix only runs for live
// skaters (it asserts skaterIndex < SkaterPresentation+1264), so when the
// skater goes away -- the skate.Park editor despawns it ("Skaters: 0") -- the
// last published transform simply stays behind and the native decal stayed
// on the ground at the board's last position. The game's deck-shadow pass
// (rex_Sk8_Render_AntiFrustumManager_DEBUG_DrawAntiFrustums_QAAXXZ_82994100) loops over exactly this skater count, so a board past the
// count gets no shadow on the console either. Read live from guest memory
// on every query: the count follows despawn AND respawn immediately, unlike
// watching the pass itself, which only runs on some PostFX paths
// (postfx+3156 bits 0x100 / 0x1).
//   Retail: the presentation is the `this` GetDeckMatrix receives (the dev
//   build's game globals + 163216); its count getter sub_8277EBC0 reads
//   +1240 (dev +1264).
constexpr uint32_t kSkaterCountOffset = 1240;

std::atomic<uint8_t*> g_base{nullptr};  // guest memory base, from the hooks
std::atomic<uint32_t> g_presentation{0};  // SkaterPresentation, from GetDeckMatrix

uint32_t ReadGuestU32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(addr), 4);
  return _byteswap_ulong(v);
}

bool SkaterExists(uint32_t index) {
  uint8_t* base = g_base.load(std::memory_order_acquire);
  if (base == nullptr) {
    return true;  // nothing published yet; GetPlacement's valid flag decides
  }
  const uint32_t presentation = g_presentation.load(std::memory_order_acquire);
  if (presentation < 0x10000) {
    return false;
  }
  const uint32_t count = ReadGuestU32(base, presentation + kSkaterCountOffset);
  return index < count && count <= 16;
}


// Capture the presentation update, not the optional deck-shadow pass.
// Based on Dumbad's first-person implementation: bones are model-space and
// must be composed with the live entity m_MatLtoW, NOT DataPostBlend.root.
std::mutex g_player_mutex;
uint32_t g_player_presentation = 0, g_player_entity = 0, g_player_blend = 0;
std::chrono::steady_clock::time_point g_player_time{};

bool GuestHeap(uint32_t p) { return p >= 0x10000000u && p < 0xC0000000u; }

uint32_t LocalPlayerEntity(uint8_t* base, uint32_t presentation) {
  if (!GuestHeap(presentation)) return 0;
  // Full SkaterPresentation object in BOTH builds. Retail's deck getter
  // receives the interface at +24, which is why its offsets differ.
  const uint32_t count = ReadGuestU32(base, presentation + 1264);
  if (count > 7) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t flags = ReadGuestU32(base, presentation + 188 + i * 176);
    const uint32_t id = flags >> 28;
    if (id >= 7) continue;
    const uint32_t entity = ReadGuestU32(base, presentation + 1840 + id * 4);
    // The reference identifies the local skater by its zero 64-bit peer ID.
    // Do not require Data's 0x04000000 bit: live local gameplay was observed
    // with flags 0x006FF000, so that test rejected every camera anchor.
    constexpr uint32_t kPeerId = 1928;
    if (GuestHeap(entity) && ReadGuestU32(base, entity + kPeerId) == 0 &&
        ReadGuestU32(base, entity + kPeerId + 4) == 0) return entity;
  }
  return 0;
}

}  // namespace

// The deck_AO draw wrapper. The pass binds its static decal at tf0, so once it
// has run, the device's fetch-constant shadow still holds the words the
// renderer needs to decode that texture through its normal path.
extern "C" REX_FUNC(sub_827EB0A8) {
  const uint32_t rows = ctx.r5.u32;
  __imp__sub_827EB0A8(ctx, base);
  if (rows == 0 || g_decal_valid.load(std::memory_order_acquire)) {
    return;
  }
  const uint32_t device = REX_LOAD_U32(kDeviceGlobal);
  if (device == 0) {
    return;
  }
  uint32_t words[6];
  for (uint32_t i = 0; i < 6; ++i) {
    words[i] = REX_LOAD_U32(device + 0x480 + i * 4);
  }
  // Only the exact texture this pass is known to bind: a stale fetch slot
  // during a world load otherwise hands the renderer a garbage decode.
  const uint32_t width = (words[2] & 0x1FFFu) + 1;
  const uint32_t height = ((words[2] >> 13) & 0x1FFFu) + 1;
  if ((words[1] & 0x3Fu) != 4 || width != 64 || height != 128) {
    return;
  }
  g_decal_seq.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (uint32_t i = 0; i < 6; ++i) {
    g_decal_words[i] = words[i];
  }
  std::atomic_thread_fence(std::memory_order_release);
  g_decal_seq.fetch_add(1, std::memory_order_relaxed);
  g_decal_valid.store(true, std::memory_order_release);
}

// Sk8::SkaterPresentation::GetDeckMatrix(&out, this, skaterIndex): the board's
// world transform, rows 0..2 the right/up/forward axes and row 3 the position.
// Runs whether or not the postfx pass does, and only for live skaters, so
// GetPlacement also checks the skater still exists (see SkaterExists).
//
// Retail: sub_8277EBC8, a leaf reached only through the presentation's vtable
// (slot +24; the deck pass sub_827F2460 calls it as (r1+80, this, i) right
// before CalculateDeckShadowMatrix sub_827F1710 and the deck_AO draw
// sub_827EB0A8). Same 176-byte per-skater stride as the dev 0x82981318, its
// fields 24 bytes lower (bone table +1552, deck matrix +88); the release build
// drops the dev assert. Its vtable neighbour sub_8277EBC0 is the skater-count
// getter (lwz r3,1240(r3)).
extern "C" REX_FUNC(sub_8277EBC8) {
  const uint32_t out = ctx.r3.u32;
  const uint32_t presentation = ctx.r4.u32;
  const uint32_t index = ctx.r5.u32;
  __imp__sub_8277EBC8(ctx, base);
  if (out == 0 || index >= kMaxSkaters) {
    return;
  }
  g_base.store(base, std::memory_order_release);
  g_presentation.store(presentation, std::memory_order_release);
  PublishedPlacement& slot = g_placements[index];
  slot.seq.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (int i = 0; i < 16; ++i) {
    slot.world[i] = LoadF32(base, out + uint32_t(i) * 4);
  }
  std::atomic_thread_fence(std::memory_order_release);
  slot.seq.fetch_add(1, std::memory_order_relaxed);
  slot.valid.store(true, std::memory_order_release);
}


// SkaterPresentation::UpdateSkater. Retail dropped one argument: output
// DataPostBlend is r7 in retail, r8 in the development build.
extern "C" REX_FUNC(sub_8278D5A0) {
  const uint32_t presentation = ctx.r3.u32;
  const uint32_t entity = ctx.r4.u32;
  const uint32_t blend = ctx.r7.u32;
  __imp__sub_8278D5A0(ctx, base);
  const uint32_t local = LocalPlayerEntity(base, presentation);
  static std::atomic<unsigned> samples{0};
  if (samples.fetch_add(1) < 4) {
    REXLOG_INFO("vr-anchor: update presentation={:08X} entity={:08X} blend={:08X} local={:08X} count={} flags={:08X}",
                presentation, entity, blend, local,
                GuestHeap(presentation) ? ReadGuestU32(base, presentation+1264) : 0,
                GuestHeap(presentation) ? ReadGuestU32(base, presentation+188) : 0);
  }
  if (!GuestHeap(entity) || entity != local) return;
  std::lock_guard<std::mutex> lock(g_player_mutex);
  g_player_presentation = presentation;
  g_player_entity = entity;
  g_player_blend = blend;
  g_player_time = std::chrono::steady_clock::now();
}

namespace skate3::deck_shadow {

bool GetPlayerPose(uint8_t* base, PlayerPose& out) {
  // Called by BuildFrameScene on the guest render thread, after presentation
  // has completed. Snapshot values into FrameScene; never retain live guest
  // pointers in the renderer or modify the simulation camera.
  std::lock_guard<std::mutex> lock(g_player_mutex);
  static uint32_t reported = 0;
  const auto fail = [&](unsigned reason, uint32_t value = 0) {
    if (!(reported & (1u << reason))) {
      reported |= 1u << reason;
      REXLOG_INFO("vr-anchor: unavailable reason={} value={:08X} entity={:08X} blend={:08X}",
                  reason, value, g_player_entity, g_player_blend);
    }
    return false;
  };
  if (!GuestHeap(g_player_entity)) return fail(0);
  if (!GuestHeap(g_player_blend)) return fail(1);
  if (std::chrono::steady_clock::now() - g_player_time > std::chrono::milliseconds(250)) return fail(2);
  if (LocalPlayerEntity(base, g_player_presentation) != g_player_entity) return fail(3);
  const uint32_t bones = ReadGuestU32(base, g_player_blend + 4);
  if (!GuestHeap(bones)) return fail(4, bones);
  // Verified in SkaterPresentationDB.abin: OFFSET_LEFTEYE=92,
  // OFFSET_RIGHTEYE=97. Their midpoint is the anatomical eye anchor.
  // The reference's 103 is OFFSET_TONGUETIP, not HEAD (8).
  constexpr uint32_t kLeftEye = 92, kRightEye = 97;
  float root[16], head[16];
  for (int i = 0; i < 16; ++i) {
    root[i] = LoadF32(base, g_player_entity + 352 + i * 4);
    head[i] = 0.5f * (LoadF32(base, bones + kLeftEye * 64 + i * 4) +
                      LoadF32(base, bones + kRightEye * 64 + i * 4));
    if (!std::isfinite(root[i]) || !std::isfinite(head[i])) return fail(5, i);
  }
  if (std::fabs(root[15]-1.0f) > 0.01f) return fail(6, std::bit_cast<uint32_t>(root[15]));
  if (std::fabs(head[15]-1.0f) > 0.01f) return fail(7, std::bit_cast<uint32_t>(head[15]));
  // Heading: DataPostBlend.root (+16), the presentation's base orientation,
  // NOT the hips (+80, which GetHipsMatrix multiplies onto it). The root is
  // the skater's placement in the world, untouched by the bone animation:
  // measured while pushing, its heading held within a degree per stroke
  // while the hips swung up to ~85 degrees (and the entity matrix at +352,
  // which equals hips x root, swung with them). Row 1 is world up, row 2
  // the board / travel axis (the view looks down the board, where the
  // skater rides), row 0 the sideways stance facing (90 degrees off).
  float presentation_root[9];
  for (int k = 0; k < 3; ++k) {
    for (int j = 0; j < 3; ++j) {
      presentation_root[k*3+j] = LoadF32(base, g_player_blend + 16 + k*16 + j*4);
      if (!std::isfinite(presentation_root[k*3+j])) return fail(8, k*3+j);
    }
  }
  for (int k = 0; k < 3; ++k) {
    out.head[k] = head[12]*root[k] + head[13]*root[4+k] +
                  head[14]*root[8+k] + root[12+k];
    out.forward[k] = presentation_root[6+k];  // row 2
    out.chest[k] = presentation_root[k];      // row 0
    // Eye bones: row 0 = top of the head, row 1 = gaze (measured: on the
    // travel line, pitched down at the ground ahead, also when riding
    // fakie), row 2 = ear to ear.
    out.face[k] = head[4]*root[k] + head[5]*root[4+k] + head[6]*root[8+k];
    if (!std::isfinite(out.head[k]) || std::fabs(out.head[k]) > 100000.0f) return false;
  }
  static auto next_heading_sample = std::chrono::steady_clock::time_point{};
  static unsigned heading_samples = 0;
  if (heading_samples < 8 && std::chrono::steady_clock::now() >= next_heading_sample) {
    ++heading_samples;
    next_heading_sample = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    float hw[9] = {};
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) for (int k=0;k<3;++k)
      hw[i*3+j] += LoadF32(base,g_player_blend+80+i*16+k*4)*LoadF32(base,g_player_blend+16+k*16+j*4);
    REXLOG_INFO("vr-heading: forward(root row2)=({:.3f},{:.3f},{:.3f}) head=({:.2f},{:.2f},{:.2f}) hipsWorld rows=({:.3f},{:.3f},{:.3f}) ({:.3f},{:.3f},{:.3f}) ({:.3f},{:.3f},{:.3f})",
                out.forward[0],out.forward[1],out.forward[2],out.head[0],out.head[1],out.head[2],hw[0],hw[1],hw[2],hw[3],hw[4],hw[5],hw[6],hw[7],hw[8]);
  }
  out.entity = g_player_entity;
  return true;
}

bool GetPlacement(uint32_t skater_index, DeckPlacement& out) {
  if (skater_index >= kMaxSkaters) {
    return false;
  }
  PublishedPlacement& slot = g_placements[skater_index];
  if (!slot.valid.load(std::memory_order_acquire) || !SkaterExists(skater_index)) {
    return false;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint32_t before = slot.seq.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;  // a write is in flight
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int i = 0; i < 16; ++i) {
      out.world[i] = slot.world[i];
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_acquire) == before) {
      out.valid = true;
      return true;
    }
  }
  return false;
}

bool GetDecalFetch(uint32_t words_out[6]) {
  if (!g_decal_valid.load(std::memory_order_acquire)) {
    return false;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint32_t before = g_decal_seq.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    for (uint32_t i = 0; i < 6; ++i) {
      words_out[i] = g_decal_words[i];
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_decal_seq.load(std::memory_order_acquire) == before) {
      return true;
    }
  }
  return false;
}

}  // namespace skate3::deck_shadow
