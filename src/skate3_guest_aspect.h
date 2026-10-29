#pragma once

namespace rex::memory { class Memory; }

// Keeps the game's 64-bit 3D aspect ratio double in sync with the native
// renderer's output aspect (16/9 while emulated frames are presented).
void Skate3InitGuestAspect(rex::memory::Memory* memory);
// Scope in which the game sees its own 16/9 (begin/end pairs, guest thread):
// passes whose output is an image of the game's fixed 1152x640 raster, e.g.
// CACPresEntity::SetupSnapshotCamera (the profile-picture camera), must be
// built with the console's projection, not the widened output's.
void Skate3GuestAspectHold16x9(bool begin);
