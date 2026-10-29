// Deck (skateboard) contact shadow -- the state the guest publishes for it.
//
// The console draws this as a deferred decal: PostFX::CalculateDeckShadowMatrix
// builds a projection from the board's world matrix, and the postfx_deck_AO
// pixel shader shades a 12-triangle box volume, reconstructing each pixel's
// position from the depth buffer, projecting it into deck space and sampling a
// static 64x128 5_6_5 texture. That texture is a soft dark blob on WHITE, so it
// MODULATES the scene: the border is a no-op and the centre darkens.
//
// The hooks in skate3_deck_shadow.cpp publish the two things the native
// renderer needs to reproduce it against its own (smoothed) camera: the board's
// world transform, and the decal texture's fetch constants. Everything else --
// the extents, the falloff -- is derived from the guest's own shader constants,
// captured from the deck draw:
//
//   c4.xy = (1.6, 0.9)  ->  decal half-extents 0.5 / c4.xy = 0.3125 m across
//                           the board's width and 0.5556 m along its length.
//
// Reproducing the guest's c0..c2 directly would bake in the GUEST camera, and
// our scene renders through a re-timed/smoothed one, so the decal would swim
// during camera motion. Driving it from the board's world matrix keeps the
// console's placement while staying locked to our own view.

#pragma once

#include <cstdint>

namespace skate3::deck_shadow {

inline constexpr uint32_t kMaxSkaters = 7;

// One skater's board placement for this frame.
struct DeckPlacement {
  // Board world transform, row-major as the guest stores it: rows 0..2 are the
  // orthonormal right/up/forward axes, row 3 the world position. The board's
  // plane is its local XZ; local Y is up.
  float world[16];
  bool valid = false;
  uint32_t owner = 0;
};

// Latest board transform published by the GetDeckMatrix hook. Returns false
// until the guest has placed a board this session (menus, loading), and
// while that skater does not exist (index >= the game's skater count, which
// is what the deck-shadow pass loops over) -- e.g. in the skate.Park editor,
// where the skater is despawned. Respawning brings it straight back.
bool GetPlacement(uint32_t skater_index, DeckPlacement& out);

// Local player's blended head in world space, captured on the guest render thread.
// This follows walking/crouching/bails independently of the detached board.
struct PlayerPose {
  float head[3] = {};
  float forward[3] = {};  // board / travel axis (presentation root row 2)
  float chest[3] = {};    // stance facing (presentation root row 0)
  float face[3] = {};     // gaze: the eye bones' row 1 in world (animated)
  uint32_t entity = 0;
  uint32_t index = 0;
};
bool GetPlayerPose(uint8_t* base, PlayerPose& out);

// The decal texture's six guest fetch constants, as bound by the deck draw
// (64x128, format 4 = 5_6_5, tiled). Returns false until the pass has run once;
// the renderer decodes these through its normal guest-texture path.
bool GetDecalFetch(uint32_t words_out[6]);

// Decal half-extents in metres, derived from the guest shader's c4.xy.
inline constexpr float kHalfWidth = 0.5f / 1.6f;   // across the board: 0.3125 m
inline constexpr float kHalfLength = 0.5f / 0.9f;  // along the board:  0.5556 m

}  // namespace skate3::deck_shadow
