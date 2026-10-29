# Release notes

## 1.0.2

- **Board shadows:** extend the custom renderer's board contact shadows to
  online players and ambient NPC skaters, supporting seven active skaters.
  Each board has its own placement and interpolation history. Skater identity
  checks prevent stale shadows when a skater disappears or a slot is reused.
- **First-run difficulty menu:** keep the menu's initialization drawing steps
  active while presenting it with the custom renderer. This addresses the
  startup path implicated in reports of missing choices and the Continue bar
  that appeared after briefly switching to the emulated renderer.
- **Fullscreen settings:** defer window resizing until the current settings
  frame has finished drawing. This avoids replacing graphics buffers while
  they are still in use. Both the Fullscreen toggle and Reset use this path.

Board shadows were verified in-game. Automated capture/identity and GPU-packing
checks cover multiple skaters, reused slots and inactive shadows. The difficulty
screen was checked with a fresh local profile; confirmation on the originally
affected remote PC remains pending. The fullscreen regression check reproduces
the old unsafe behavior and passes with the deferred callback, including rapid
toggles and a window closing before the change is applied; in-game verification
of that change is left to player testing.

The complete Windows download retains automatic bundled Title Update 3 setup,
the custom renderer enabled by default, profiles, online play, split screen,
enhanced fonts and SteamVR. Original game files are still supplied by the player.

Community Blaze server: **129.151.200.180**, configured under
**Settings → Online → Server Address**.

## 1.0.1

Automatic installation of the two verified bundled TU3 patch payloads after
retail ISO extraction. Already updated and mixed game folders are recognized,
and the runtime avoids applying an update twice.
