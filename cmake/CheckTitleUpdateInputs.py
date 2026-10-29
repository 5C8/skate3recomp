"""Reject code generation from an image that does not match this TU3 build."""
import argparse
import hashlib
from pathlib import Path
import struct

TITLE_ID = 0x454108E6
BASE_VERSION = 0x00000003
TU3_VERSION = 0x00000303
PAYLOADS = {
    "default.xex": (1701888, "eb9ef9109dfa6d940df2e156e7eaeda4603d2b2319ca6451f324b1c27f2b1f4c"),
    "data/webkit/EAWebkit.xex": (4096, "5d4a308d2a6c768fc27c8b62ccb6661171dc504f8c0011a3e116cf0074e09438"),
}

def execution_info(path):
    with path.open("rb") as stream:
        length = stream.seek(0, 2)
        def word(offset):
            if offset < 0 or offset + 4 > length:
                raise ValueError(f"Invalid XEX header offset in {path.name}")
            stream.seek(offset)
            return struct.unpack(">I", stream.read(4))[0]
        if length < 24 or word(0) != 0x58455832:
            raise ValueError(f"Not an XEX2 file: {path.name}")
        count = word(20)
        if count > (length - 24) // 8:
            raise ValueError(f"Invalid optional-header count in {path.name}")
        for i in range(count):
            if word(24 + i * 8) == 0x00040006:
                offset = word(28 + i * 8)
                return word(offset + 4), word(offset + 12)
    raise ValueError(f"Missing execution info in {path.name}")

def validate(root):
    for relative, (size, digest) in PAYLOADS.items():
        executable = root / relative
        version, title = execution_info(executable)
        if title != TITLE_ID or version not in (BASE_VERSION, TU3_VERSION):
            raise ValueError(f"{relative} is not the supported base/TU3 Skate 3 image")
        patch = executable.with_suffix(".xexp")
        if version == TU3_VERSION and not patch.exists():
            continue
        if not patch.is_file() or patch.stat().st_size != size or hashlib.sha256(patch.read_bytes()).hexdigest() != digest:
            raise ValueError(f"{relative} requires the matching TU3 patch. Supply SKATE3_TITLE_UPDATE_PACKAGE or use matching updated game files; an unpatched ISO alone is insufficient for TU3 codegen.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        validate(args.game_root)
    except (OSError, ValueError) as error:
        parser.exit(1, f"TU3 input check: {error}\n")
    print("TU3 code-generation inputs verified.")
