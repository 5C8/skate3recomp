# Features and controls

## Rendering and display

The custom/native renderer is enabled by default. **F5** switches between the
native and emulated renderers while running. The emulated renderer remains useful
for checking visual differences. On Windows, the Video settings select DirectX
12 or Vulkan; graphics API changes require a restart.

Video settings cover resolution/render scale, fullscreen, aspect ratio, frame
limit, VSync/VRR, multisampling, enhanced sun shadows, ambient occlusion, bloom,
and volumetric lighting. Ultrawide renders a wider view with corrected HUD and
world-space NPC arrow/online nametag positioning. The renderer also includes
enhanced embedded fonts. Features and available controls vary with the game
build; the in-game description says whether a restart is needed.

The milestone build includes native board-shadow handling for multiple online
skaters. Renderer parity remains a work in progress: streaming, customization,
Hall of Meat and park-editor visuals can still differ from the emulated path.
The milestone build does not include the retail draw-distance/demo-path hooks
whose addresses are still unresolved.

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
project-registered setting, its source default, and description. Some entries
are experiments or diagnostic controls; their presence is not a promise of
complete visual parity.

## Profiles, online play and split screen

Profiles have separate identities and saves. Disable automatic sign-in in Online
settings to choose/create a profile each launch. Use a different profile for
each local player.

The Online settings let you choose a compatible community Blaze server address
and gamertag. After changing the server, restart when requested and sign in,
then use the game's online/session menus. The server must support the particular
retail or milestone build; simply selecting the same hostname does not guarantee
cross-build matchmaking. Test with both players on the same build first.
No community server credentials or personal server address are bundled.

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

The Windows packages include the OpenVR runtime. Install/start SteamVR, connect
your headset, and enable VR in Settings. The VR page controls eye resolution,
MSAA, world scale, horizon/position tracking, HUD placement/curvature and camera
behavior. Controller settings include motion-controller bindings and body yaw.
The settings page describes the recenter shortcut. Start at a modest eye scale
and raise it after checking frame rate; VR renders two views. Flat play does not
require SteamVR.

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
