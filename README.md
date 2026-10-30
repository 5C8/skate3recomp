# skate3recomp

Retail Skate 3 recompilation with a native renderer and PC integration.

This is the `5C8` publication snapshot of the working projects, based on
[mchughalex/skate3recomp](https://github.com/mchughalex/skate3recomp) and the
[Skate 3 ReXGlue fork](https://github.com/mchughalex/rexglue-skate3).
It uses the companion [5C8 SDK](https://github.com/5C8/rexglue-skate3).

## Download and play

[**Windows x64 releases**](https://github.com/5C8/skate3recomp/releases/latest)

Version **1.0.4** fixes the first-run setup transition into the opening movie
and keeps diagnostic logging from signing out your profile. It also includes
the custom-map geometry and alpha-cutout fixes from 1.0.3.
See the [release notes](docs/CHANGELOG.md).

1. Download and fully extract **skate3recomp-Windows-x64.zip** from Releases.
2. Start `skate3.exe` and use the installer to select your own retail Skate 3 ISO.
3. Complete the game installation prompts. Setup installs the bundled Title
   Update 3 patches automatically, then starts the game.

The retail ISO supplies the base game. Both required TU3 `.xexp` patches are
included under `title-update/` in the Windows download. You do not need to find
a prepatched XEX or download TU3 separately. Keep that folder with the executable.
Already updated installations are recognized, and updates are not applied twice.

The release contains the application and its runtime dependencies. Provide your
own matching game files. Windows x64 with an x86-64-v3 CPU (AVX2/BMI2), a
DirectX 12 or Vulkan GPU, and the Visual C++ x64 runtime is required. Linux/ARM
and macOS binaries are not published in this snapshot.

## Included features

- Near 1:1 visual parity with the emulated renderer in gameplay comparisons.
- Custom/native DirectX 12 and Vulkan rendering, enabled by default; **F5**
  switches to/from the emulated renderer.
- Board contact shadows for the local player, online players and ambient NPC
  skaters, with independent placement and motion smoothing.
- Video enhancements, enhanced fonts, ultrawide HUD/world-marker corrections,
  display settings, controller/keyboard input, and SteamVR integration.
- Separate player profiles and saves, compatible community-server online play,
  and a Windows multi-window split-screen launcher with controller assignment.

Open Settings with **Escape/F1** or **RB + Start**. To launch split screen, run
`skate3.exe --splitscreen`, assign connected controllers to screens, then select
different profiles and host/join a compatible online session.

**SteamVR startup:** in Steam, right-click the game's Library entry and open
**Properties > Launch Options**, then enter `--skate3_vr=true` before launching.
See the [SteamVR setup guide](docs/FEATURES.md#steamvr) for the full steps.

## Online server

5C8's community Blaze server: **`129.151.200.180`**.

Open **Settings (Escape/F1) → Online → Server Address**, enter
`129.151.200.180`, and press **Enter** to save. Then connect through the game's
online menus. The server supports both projects; use the same game build as
the other players when joining a session.

## Guides

- [Features, controls, online play, split screen, VR and troubleshooting](docs/FEATURES.md)
- [Release notes](docs/CHANGELOG.md)
- [Build from source](docs/BUILDING.md)
- [All project configuration options and defaults](docs/CONFIGURATION.md)
- [What is included/excluded in this clean source snapshot](docs/SOURCE_CONTENTS.md)
- [Credits and dependency notices](THIRD_PARTY_NOTICES.md)

The generated main-game implementation, original game executables/data,
decompiler dumps, user data, backups, build caches and unused investigation tools
are excluded. The enhanced embedded font is retained. See the source-contents
guide for the intentional ZD debug-source inclusion in the milestone project.
