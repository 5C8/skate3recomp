# Release notes

## 1.0.6

- **Create-a-skater pictures:** the custom renderer now draws the skater
  pictures the game takes when you leave Edit a Skater. It follows the game's
  `PhotoEffects` pipeline: the 2D layers in front of and behind the skater
  (backdrop, team-profile frame, bars, logo and rotated gamertag), the pose the
  game sets for the picture, the picture's own lighting without world sun
  shadows, and the game's crop at any window aspect ratio.
- **Import Skater pictures:** skater pieces in the import screen's pictures
  are decoded before the picture is drawn, instead of streaming in over later
  frames and leaving only the background.
- **Neon and incandescent signs:** the game's `transparentincandescent`
  material (cinema neon, film-reel strip, marquee lettering, restaurant disc
  signs) now has its own shading path, ported from the game's shader. It uses
  each draw's live multiplier and exposure, keeps the sign's alpha, and follows
  the game's blend, culling and depth-write state per draw, so double-sided
  signs no longer show their mirrored back face through the front.
- **Waving flags:** `animated.flag` cloth now uses a port of the game's
  `vertexanimate_defaultVS/PS`. It uses the same per-vertex wind seeds and
  animation clock as the trees with the flag's own wave constants, draws both
  sides as the game does, and shows in the family colour key.
- **Settings text fields:** edits in text fields such as Server Address are
  now saved however you leave the field. Previously some ways of leaving it
  dropped the change.
## 1.0.5

- **Board stickers:** draw deck stickers in the custom renderer. The game
  renders them through a separate `character.shift` material that samples the
  graphic from its own texture with a dedicated UV transform. That texture and
  placement are now captured and composited in the character shader; the
  earlier `default_skateboard` decal path is removed.
- **Printed shirt logos:** draw printed garment graphics from the game's
  `cacstamp_cloth` stamp texture and placement, separately from the cloth dirt
  layer. ROPA stamp shaders use their own placement rows.
- **Custom team-logo tattoos:** match the game's tattoo blend, which uses the
  logo's alpha to blend its colour against neutral white before applying it to
  the skin. Transparent areas of custom logos no longer appear as a dark
  backing; opaque tattoos are unchanged.
- **Live Sign In:** add a Live Sign In checkbox to Settings > Online. It shows
  and changes the selected profile's `live_signed_in` value and applies on the
  next launch. A profile saved with this off signs in locally only, so the game
  reports that an Xbox LIVE sign-in is required and online play is unavailable.
  The row's reset action turns it on.
- **Animated tree leaves:** reproduce the game's `treeanimate_defaultVS`
  sway in the custom renderer. The renderer now uses the animated-tree
  vertices' wind seeds and the live shader constants, so these leaves move
  with the game's per-frame animation instead of remaining static. This is
  supported by both Direct3D 12 and Vulkan paths.

## 1.0.4

- **First-run setup:** remove the forced movie-completion override entirely,
  including the automation path. Confirmation input after choosing difficulty and
  camera could prematurely advance the movie state, leaving a broken difficulty
  screen instead of progressing. The game now controls its movie lifecycle.
  Existing `skate3_intro_movie_skip = true` configs cannot re-enable the unsafe
  override. Existing generated builds automatically have the old patch removed.
- **Profile sign-in during diagnostics:** the frontend logging probe preserves
  the selected profile. A logging-only run previously inherited automation's
  signed-out startup unless an additional flag was supplied.

The affected player confirmed that disabling the old movie-skip override allowed
setup to proceed. The successful log reaches gameplay and opens the save again.
Regression checks exercise the actual profile callback with normal/probe/automation
modes and verify that both existing and newly generated builds use the original
movie call, including when old configs enable the retired flag. No renderer
or save-file format changes are needed, and existing saves can be kept.

## 1.0.3

- **Custom-map cutout textures:** preserve diffuse alpha testing on tree,
  animated-tree and alpha-test materials when a specular/normal map is bound.
  These bindings previously suppressed cutouts, making some custom-map trees,
  fences and power lines appear solid. Explicit game alpha-test overrides and
  decal compositing retain their behavior.
- **Custom-map geometry:** accept vertex buffers with up to 15 trailing bytes
  of 16-byte allocation padding. San Vanelona uses these buffers extensively;
  the custom renderer previously rejected them when the allocation size was
  not an exact multiple of the vertex stride, leaving floors and walls missing.
  Only complete vertices are decoded.

Regression checks cover real San Vanelona buffer-size/index metadata, malformed
buffers, optional specular/normal bindings, game alpha-test overrides, and
character/decal exclusions. An additional local check exercised 969 validated
mesh records. All 15 Direct3D scene shader variants compiled, and all 106 Vulkan
variants were regenerated and their binding layouts checked. These are automated
and captured-data checks; final visual comparisons remain player testing.

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
