# Creating a release package

Build from the clean source plus your locally generated implementation. Package
with the explicit runtime allowlist; never archive the entire build directory.
The packager includes docs, dependency notices and per-file SHA-256 checksums.

```powershell
python tools/package_release.py --build-dir out/build/win-amd64-release --title-update-dir C:/Private/Skate3-TU3 --output skate3recomp-Windows-x64.zip
```

Create a GitHub release from the corresponding source tag and attach the ZIP
and its SHA-256 checksum. Game binaries contain the compiled implementation;
the generated main-game implementation must remain local and ignored. The patch
directory must contain `default.xexp` and `data/webkit/EAWebkit.xexp`. The packager
validates their pinned sizes and SHA-256 hashes before including them under
`title-update/`. Original `.xex` files, ISO images and game content are excluded.
Verify fresh disc images plus bundled updates, existing updated installations,
duplicate sidecars, and corrupted/missing update files before publication.
