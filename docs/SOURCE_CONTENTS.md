# Published source contents

Includes handwritten integration and renderer code, shaders, enhanced embedded
font data, game address/name metadata, and the tools used by this build.
The embedded enhanced font is an intentional inclusion.

The Windows binary release also intentionally includes the two matching TU3
`.xexp` patch payloads. These are not tracked in the source repository. Base game
files must still be supplied by the user.

Excludes original game executables/data, the main-game generated implementation
under top-level `generated/`, decompiler dumps, historical backups, local user
data, build outputs, and unused investigation tools. Generate the main-game
implementation locally from your matching game files before compiling.
