# Building the retail project

Use SDK 0.8.2.95 or newer from the matching [5C8 ReXGlue SDK](https://github.com/5C8/rexglue-skate3/releases),
not an unrelated upstream SDK build. The initial Windows snapshot uses Clang
23.1.0, CMake 3.25+, Ninja, the Visual Studio C++ build tools and a Windows SDK.
Run these commands in a developer shell with `clang`, `clang++`, CMake and Ninja
on PATH. Python 3 is needed by the code-generation helpers.

If CMake finds a Python launcher that cannot run an interpreter, add
`-DPython3_EXECUTABLE=C:/Path/To/Python/python.exe` to the configure commands.

Extract the SDK release to a directory such as `C:/Dev/rex-sdk`. Extract your
matching retail game into `game/`, including `default.xex` and
`data/webkit/EAWebkit.xex`. Obtain the Title Update 3 package from your own
installation and pass its path below; the release runtime expects TU3. Game
files and generated implementation stay ignored and are never needed on GitHub.

TU3 is required independently of whether a package path was passed at build
time (`SKATE3_REQUIRE_TITLE_UPDATE=ON` by default). If your code-generation
inputs are already TU3, a separate package is optional; every remaining base
module must have its matching `.xexp`. The codegen preflight checks both modules
and rejects unpatched inputs. `SKATE3_REQUIRE_TITLE_UPDATE=OFF` is for explicit
legacy base-game development builds, not this TU3 release.

For distribution, supply the two verified patch files to the release packager
using `--title-update-dir`; see [RELEASING.md](RELEASING.md). They are included in
the binary ZIP and installed automatically after ISO extraction. They remain
excluded from Git source history.

```powershell
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/Dev/rex-sdk -DSKATE3_GAME_DATA_ROOT=C:/Games/Skate3 -DSKATE3_TITLE_UPDATE_PACKAGE=C:/Games/Skate3-TU3
cmake --build --preset win-amd64-release --target generate-all --parallel 4
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/Dev/rex-sdk -DSKATE3_GAME_DATA_ROOT=C:/Games/Skate3 -DSKATE3_TITLE_UPDATE_PACKAGE=C:/Games/Skate3-TU3
cmake --build --preset win-amd64-release --parallel 4
```

`generate-all` creates the main-game/module implementation and applies the
checked-in code-generation patches. Reconfigure afterward to load the generated
source lists. Build output is in `out/build/win-amd64-release/`.

Alternatively set `REXSDK_DIR` to a recursive clone of the SDK source tree. The
release SDK package is the simplest way to match the packaged binaries.
`SKATE3_PGO` defaults to AUTO and builds without a private profile when none is
present. Set `SKATE3_PGO=OFF` for a normal build. Optional mimalloc requires
`SKATE3_MIMALLOC_DIR` pointing to its DLL/import-library directory.

For SteamVR, run `python tools/fetch_openvr.py` before building, or copy
`openvr_api.dll` from the matching release into `thirdparty/openvr/`. The runtime
is omitted from Git source. Shader SPIR-V is checked in; after modifying HLSL,
set `SKATE3_DXC` to a SPIR-V-capable DXC executable and regenerate shaders.

Linux/macOS presets and platform code are inherited from upstream. Only Windows
x64 binaries are published and checked in this snapshot; other platforms require
their own matching SDK/toolchain and testing. The Windows-only split-screen and
VR integrations should not be assumed to work there. The upstream reference is
preserved in [UPSTREAM_README.md](UPSTREAM_README.md); its release links/version
claims refer to upstream rather than this snapshot.
