# Features and controls

## ISO installation and Title Update 3

Select your retail ISO in setup. Its base game files are extracted, then setup
validates and installs the two bundled TU3 patch payloads from `title-update/`.
The loader applies those patches when loading the game and EAWebkit executables.
The ISO itself is left unchanged. No prepatched XEX or separate update download
is required by the complete Windows package. Existing TU3 executables are
recognized, including mixed installations where only EAWebkit needs a patch.
The game cannot proceed with missing, mismatched or damaged required updates.

## Rendering and display

The custom/native renderer is enabled by default. **F5** switches between the
native and emulated renderers while running. The emulated renderer remains useful
for checking visual differences. On Windows, the Video settings select DirectX
12 or Vulkan; graphics API changes require a restart. Gameplay comparisons
have shown near 1:1 visual parity with the emulated renderer.

Video settings cover resolution/render scale, fullscreen, aspect ratio, frame
limit, VSync/VRR, multisampling, enhanced sun shadows, ambient occlusion, bloom,
and volumetric lighting. Ultrawide renders a wider view with corrected HUD and
world-space NPC arrow/online nametag positioning. The renderer also includes
enhanced embedded fonts. Features and available controls vary with the game
build; the in-game description says whether a restart is needed.

Board contact shadows cover the local player, online players and ambient NPC
skaters, supporting up to seven active skaters. Each shadow follows its own
board, including when motion smoothing is enabled; despawned skaters and reused
slots do not leave an old shadow behind.

Fullscreen changes in Settings apply after the current menu frame finishes
drawing. The first-run difficulty selector keeps its initialization drawing
steps active while the custom renderer presents the menu.

## Settings and input

- **Escape** or **F1**: player settings. The default controller shortcut is
  **RB + Start** and can be rebound.
- **F5**: switch renderer.
- **F12**: native-renderer developer controls.
- **F10** (retail build): insert a diagnostic marker in the log.

Xbox controllers work through XInput. The SDL controller backend supports other
controllers; change the backend in Controls and restart when requested. Keyboard
and mouse input, bindings, camera controls, and SteamVR settings are available in
the player settings. The SDK also exposes developer configuration controls.
See [the generated configuration reference](CONFIGURATION.md) for every
project-registered setting, its source default, and description. Experimental
and diagnostic entries are identified in their descriptions.

## Profiles, online play and split screen

Profiles have separate identities and saves. Disable automatic sign-in in Online
settings to choose/create a profile each launch. Use a different profile for
each local player.

The Online settings let you choose a compatible community Blaze server address
and gamertag. After changing the server, restart when requested and sign in,
then use the game's online/session menus. The server must support the particular
retail or milestone build; simply selecting the same hostname does not guarantee
cross-build matchmaking. Test with both players on the same build first.
5C8's community Blaze server address is **`129.151.200.180`**. Open
**Settings (Escape/F1) → Online → Server Address**, enter `129.151.200.180`,
and press **Enter** to save. The change applies to the next connection. This
server supports both projects; players joining the same session should use
the same game build.

On Windows, run `EXE --splitscreen` (replace EXE with the executable name).
Connect controllers, assign each to a monitor/part of a monitor on the launcher
screen, then start. The launcher supports up to eight players overall and four
per monitor, subject to your PC's resources. Each seat runs a separate game
process with its own assigned controller, profile and save identity; secondary
windows are muted. The launcher coordinates profile selection before gameplay.
It does not automatically merge the game sessions: host/join the same compatible
online session through the game. Multiple processes need a server that supports
their distinct local endpoints. This Windows launcher is not a native shared
simulation or a replacement for the multiplayer server.

## SteamVR

**Required Steam launch option:** `--skate3_vr=true`. Without this option,
the game starts in flat mode rather than booting into VR.

1. Install/start SteamVR and connect your headset.
2. Add `skate3.exe` to your Steam Library as a **Non-Steam Game**, if you
   have not already added it.
3. In your Steam Library, right-click that game entry, open **Properties**,
   and enter the following in **Launch Options**:

   ```text
   --skate3_vr=true
   ```

   Enter only the option above in that field. If you already use other launch
   options, add this one separated by a space.
4. Close Properties and launch the game from Steam. Use **DirectX 12** with
   the custom/native renderer for VR.

The Windows packages include the OpenVR runtime. Once in VR, the in-game VR
settings control eye resolution, MSAA, world scale, horizon/position tracking,
HUD placement/curvature and camera behavior. Start at a modest eye scale and raise it after checking
frame rate; VR renders two views. Flat play does not require SteamVR.

## Files, saves and diagnostics

Keep the whole release folder together, including its DLLs. Saves/settings are
stored in the application's user-data location unless portable mode is used.
Create `portable.txt` beside the executable before launch to keep user data
there. Existing saves are not migrated automatically. Keep a backup when moving
saves or switching between retail and milestone builds.

When reporting a problem, include the build/release name, graphics API, renderer,
aspect ratio, relevant setting changes, and the end of the log. If a visual is
missing, compare with F5. If online joining fails, confirm distinct profiles,
the same game build, and compatible server support before changing graphics
settings. For missing-DLL errors, extract the complete release rather than
copying only the executable. This release targets Windows x64 CPUs with AVX2,
BMI2 and the other x86-64-v3 features. A DirectX 12/Vulkan-capable GPU and current
graphics drivers are required. Install the Microsoft Visual C++ x64 runtime if
Windows reports a missing MSVC runtime DLL.
