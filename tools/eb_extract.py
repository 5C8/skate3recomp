#!/usr/bin/env python3
"""Extract files from an EA "EB" v3 .big archive (RefPack-compressed entries).

Retail keeps its fonts inside game/data/big/miscboot.big rather than as loose
files, so tools/gen_font_data.py needs them extracted first:

  python tools/eb_extract.py <game>/data/big/miscboot.big <outdir> futura debug.
  python tools/gen_font_data.py <outdir> <images_dir>

Layout (big-endian): header 0x30 bytes -- "EB", u16 version 3, u32 file
count, ..., u32 name table offset at 0x0C, u8 name entry size at 0x14; then
one 16-byte entry per file {u32 offset >> 6, u32 compressed size (0 = stored),
u32 size, u32 name hash}; the name table holds one fixed-size entry per file
in the same order, {u16 folder index, NUL-padded name}.
"""
import os
import struct
import sys


def refpack(src):
    hdr = struct.unpack('>H', src[0:2])[0]
    n = 4 if hdr & 0x8000 else 3
    p = 2
    size = int.from_bytes(src[p:p + n], 'big')
    p += n
    if hdr & 0x0100:  # compressed-size field present
        p += n
    out = bytearray()
    while p < len(src):
        b0 = src[p]
        if b0 < 0x80:
            b1 = src[p + 1]
            p += 2
            lit = b0 & 3
            cnt = ((b0 >> 2) & 7) + 3
            off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = src[p + 1], src[p + 2]
            p += 3
            lit = b1 >> 6
            cnt = (b0 & 0x3F) + 4
            off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[p + 1], src[p + 2], src[p + 3]
            p += 4
            lit = b0 & 3
            cnt = ((b0 & 0x0C) << 6) + b3 + 5
            off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1
            lit = ((b0 & 0x1F) << 2) + 4
            cnt = off = 0
        else:  # stop code, up to 3 trailing literals
            p += 1
            out += src[p:p + (b0 & 3)]
            break
        out += src[p:p + lit]
        p += lit
        for _ in range(cnt):
            out.append(out[-off])
    assert len(out) == size, (len(out), size)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    big, outdir, pats = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = open(big, 'rb').read()
    assert d[:2] == b'EB' and struct.unpack('>H', d[2:4])[0] == 3, big
    count = struct.unpack('>I', d[4:8])[0]
    names_off = struct.unpack('>I', d[12:16])[0]
    name_len = d[20]
    os.makedirs(outdir, exist_ok=True)
    for i in range(count):
        off, csize, size, _ = struct.unpack('>IIII', d[0x30 + i * 16:0x40 + i * 16])
        o = names_off + i * name_len
        name = d[o + 2:o + name_len].split(b'\0')[0].decode('latin-1')
        if pats and not any(p in name for p in pats):
            continue
        raw = d[off << 6:(off << 6) + (csize or size)]
        data = refpack(raw) if csize else raw
        assert len(data) == size, (name, len(data), size)
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        print('extracted %s (%d bytes)' % (name, size))
    return 0


sys.exit(main())
