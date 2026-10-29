#!/usr/bin/env python3
"""Generate the embedded font-atlas data for the native renderer.

The game draws its text as one quad per character out of a small bitmap
atlas (128x128 for debug, 512x512 for the futura family), far below the
native renderer's output resolution. Replacement atlases keep the SAME
normalised glyph layout, so the game's own uvs index them unchanged -- only
the texel density changes.

Inputs per font:
  <fonts>/<name>.bmpFont    the game's text metrics file; its GlyphMetricsMap
                            gives every glyph's cell in guest-atlas texels,
                            which is how a draw is matched to its font
  <images>/<name>.(png|tga) the replacement image (the extracted original, or
                            an upscaled version of it), white RGB + coverage
                            alpha

Output: src/generated/skate3_native_font_data.cpp -- alpha only, RLE packed
(these atlases are constant-white RGB and mostly empty, so alpha RLE is ~10x
smaller than the source image and needs no decompression library).

Usage: python tools/gen_font_data.py [fonts_dir] [images_dir]

Retail ships no loose font files; extract them from the archive first:
  python tools/eb_extract.py <game>/data/big/miscboot.big <fonts_dir> futura debug.
Retail's fonts are NOT dev's: different payloads (so different sums) and a
few extra glyphs, so always generate from the retail extraction.
"""
import os
import re
import struct
import sys
import zlib

# (font name, replacement image, downsample factor).
#
# Only fonts listed here are replaced; everything else keeps the game's own
# atlas. futuraglow and futurashadow are deliberately absent -- their
# originals read better than an upscale.
#
# The downsample halves 4096x4096 upscales to 2048x2048: that is still 4x the
# guest atlas, far beyond the texel density any on-screen glyph needs, and it
# cuts both the embedded data and the runtime texture to a quarter (16 MB
# instead of 67 MB of VRAM per font).
FONTS = [
    ('debug', 'debug_upscaled.png', 1),
    ('futuraheavy', 'futuraheavy_upscaled.png', 2),
    ('futuraoverprint', 'futuraoverprint_upscaled.png', 2),
]


def parse_bmpfont(path):
    txt = open(path, 'r', encoding='latin-1').read()

    def field(name):
        m = re.search(r'^' + name + r':\s*(.*)$', txt, re.M)
        return m.group(1).strip() if m else None

    # "GlyphMetricsMap: <count> <records, comma separated>", each record
    # "index page x y_bottom w h bearing_x bearing_y advance".
    gm = re.search(r'GlyphMetricsMap:\s*(\d+)\s*(.*?)\nCharMapSet:', txt, re.S)
    count = int(gm.group(1))
    glyphs = []
    for rec in gm.group(2).replace('\n', ' ').split(','):
        f = rec.split()
        if len(f) >= 9:
            glyphs.append(tuple(int(v) for v in f[:9]))
    assert len(glyphs) == count, (path, len(glyphs), count)

    cm = re.search(r'CharMapSet:\s*(\d+)\s*(.*?)\n(?:KerningMap|Texture0|End):',
                   txt, re.S)
    charmap = {}
    for rec in cm.group(2).replace('\n', ' ').split(','):
        f = rec.split()
        if len(f) == 2:
            charmap[int(f[0])] = int(f[1])

    tex = field('Texture0').split()  # "<bytes> <w> <h> <name>"
    return glyphs, charmap, int(tex[1]), int(tex[2])


def font_cells(glyphs, charmap):
    """Every distinct cell a font defines, (x, y_top, w, h) in guest texels."""
    return sorted({(glyphs[gi][2] + glyphs[gi][6], glyphs[gi][3] - glyphs[gi][7],
                    glyphs[gi][4], glyphs[gi][5])
                   for gi in charmap.values()
                   if gi < len(glyphs) and glyphs[gi][4] > 0
                   and glyphs[gi][5] > 0})


# The .rx2 next to each .bmpFont is the guest atlas exactly as the game loads
# it: a header (0x228 or 0x238 bytes, it grows with the texture name; it
# embeds the Xenos fetch constant -- format 0x14 DXT4/5, one byte per texel)
# and then the tiled payload the texture fetch points at, which ends the file.
RX2_HEADER_RANGE = (0x200, 0x300)


def payload_sum(fonts_dir, name, src_w, src_h):
    """Byte sum of the guest atlas payload: the runtime's content check.

    Cell rects alone cannot identify a font -- futuraglow is futuraheavy's
    layout with a blurred glow image (167 of 169 cells identical), and
    serving the futuraheavy replacement for the glow pass turned the soft
    pale-blue halo into a second crisp white glyph. The sum of the payload
    bytes tells them apart, and it is unaffected by tiling (a permutation of
    the blocks) and by the loader's endian swap (a permutation within a
    word), so the runtime can take it straight from guest memory.
    """
    path = os.path.join(fonts_dir, name + '.rx2')
    size = src_w * src_h
    d = open(path, 'rb').read()
    assert RX2_HEADER_RANGE[0] <= len(d) - size < RX2_HEADER_RANGE[1], (
        path, len(d), size)
    return size, sum(d[len(d) - size:])


def read_tga(path):
    d = open(path, 'rb').read()
    idlen, imgtype = d[0], d[2]
    w, h = struct.unpack('<HH', d[12:16])
    bpp, desc = d[16], d[17]
    assert imgtype == 2 and bpp == 32, (path, imgtype, bpp)
    px = d[18 + idlen:18 + idlen + w * h * 4]
    alpha = bytearray(px[3::4])
    if not (desc & 0x20):  # bottom-up origin: flip to top-down
        rows = [alpha[y * w:(y + 1) * w] for y in range(h)]
        alpha = bytearray(b''.join(reversed(rows)))
    return w, h, bytes(alpha)


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h, nch = 8, b'', None, None, None
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h, depth, color, comp, filt, inter = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and inter == 0 and color in (0, 2, 4, 6), \
                (path, depth, color, inter)
            nch = {0: 1, 2: 3, 4: 2, 6: 4}[color]
            has_alpha = color in (4, 6)
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                b = prev[i]
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # Coverage lives in the alpha channel when there is one. The upscaled
    # images have none -- alpha was flattened to black before upscaling so
    # the upscaler had real luminance to work with -- so take channel 0.
    return w, h, bytes(out[(nch - 1) if has_alpha else 0::nch])


def downsample(alpha, w, h, factor):
    """Box filter by an integer factor. The normalised glyph layout is
    unchanged, so the game's uvs still address the same cells."""
    if factor <= 1:
        return w, h, alpha
    assert w % factor == 0 and h % factor == 0, (w, h, factor)
    ow, oh = w // factor, h // factor
    out = bytearray(ow * oh)
    inv = factor * factor
    for y in range(oh):
        for x in range(ow):
            acc = 0
            for dy in range(factor):
                row = (y * factor + dy) * w
                for dx in range(factor):
                    acc += alpha[row + x * factor + dx]
            out[y * ow + x] = acc // inv
    return ow, oh, bytes(out)


# Upscalers leave a haze of near-zero values across what should be empty
# atlas (debug_upscaled.png has 29% of its texels at exactly 1) and stop just
# short of opaque inside strokes. Snapping both ends is invisible on screen
# and is worth 4x on the packed size, because it restores the long constant
# runs the empty atlas should have.
CLEANUP = bytes(0 if v <= 3 else (255 if v >= 252 else v) for v in range(256))


def rle(data):
    """0x00..0x7F: literal run of n+1 bytes.
    0x80..0xFF: (n - 0x80 + 2) copies of the byte that follows."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 129:
            run += 1
        if run >= 2:
            out.append(0x80 + run - 2)
            out.append(data[i])
            i += run
        else:
            j = i
            lit = bytearray()
            while j < n and len(lit) < 128:
                if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                    break
                lit.append(data[j])
                j += 1
            out.append(len(lit) - 1)
            out += lit
            i = j
    return bytes(out)


def carray(name, data):
    lines = ['static const unsigned char %s[] = {' % name]
    for off in range(0, len(data), 24):
        lines.append('    ' + ''.join('%d,' % b for b in data[off:off + 24]))
    lines.append('};')
    return '\n'.join(lines)


HEADER = (
    '// GENERATED by tools/gen_font_data.py -- do not edit.\n'
    '//\n'
    '// Replacement bitmap-font atlases (alpha only, RLE packed; the originals\n'
    '// are constant-white RGB) plus each font glyph cell table read from its\n'
    '// .bmpFont, which is how a text draw is matched to the font it belongs to.\n'
    '#include "skate3_native_font_data.h"\n\n'
    'namespace skate3::native_scene::font_data {\n\n')


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fonts_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, 'out', 'build', 'win-amd64-relwithdebinfo', 'game', 'data', 'big',
        'data', 'fe', 'fonts')
    img_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.expanduser('~'), 'Downloads', 'Rx2_Tool (2)', 'TGA')
    out_path = os.path.join(root, 'src', 'generated',
                            'skate3_native_font_data.cpp')

    blobs, entries, norm_cells, sums = [], [], [], {}
    for name, image, factor in FONTS:
        bmp = os.path.join(fonts_dir, name + '.bmpFont')
        if not os.path.exists(bmp):
            print('skip (no bmpFont): %s' % name)
            continue
        glyphs, charmap, src_w, src_h = parse_bmpfont(bmp)
        img = None
        for cand in (os.path.join(img_dir, image),
                     os.path.join(img_dir, name + '.png'),
                     os.path.join(img_dir, name + '.tga')):
            if os.path.exists(cand):
                img = cand
                break
        if img is None:
            print('skip (no image): %s' % name)
            continue
        w, h, alpha = (read_png if img.lower().endswith('.png')
                       else read_tga)(img)
        w, h, alpha = downsample(alpha, w, h, factor)
        assert w % src_w == 0 and h % src_h == 0, (name, w, h, src_w, src_h)
        alpha = alpha.translate(CLEANUP)
        packed = rle(alpha)
        # char -> cell, in GUEST atlas texels with y measured from the top.
        # (x, y) is the PEN ORIGIN on the baseline, not the cell corner: the
        # ink box starts at x + bearing_x and its top is y - bearing_y.
        # Deriving the top as y - h instead is only right for glyphs whose
        # bearing_y equals their height (caps and x-height letters), and it
        # put every descender and every glyph with a left side bearing in the
        # wrong place -- verified against live draws, 18/18 cells match this
        # way and 3/18 the other.
        cells = [(0, 0, 0, 0)] * 256
        for code, gi in charmap.items():
            if code < 256 and gi < len(glyphs):
                gx, gy, gw, gh = glyphs[gi][2:6]
                bx, by = glyphs[gi][6:8]
                if gw > 0 and gh > 0:
                    cells[code] = (gx + bx, gy - by, gw, gh)
        # Every distinct cell the font defines, for identifying a batch whose
        # characters we do not have: these fonts are Unicode-mapped (codes up
        # to 65535) and the wide-char DrawstringLocal never hands us a string
        # at all, so matching has to work from the uv rect alone.
        allcells = font_cells(glyphs, charmap)
        psize, psum = payload_sum(fonts_dir, name, src_w, src_h)
        sums[name] = (psize, psum)
        blobs.append(carray('k_%s_alpha' % name, packed))
        blobs.append('static const FontCell k_%s_cells[256] = {' % name +
                     chr(10) +
                     chr(10).join('    {%d,%d,%d,%d},' % c for c in cells) +
                     chr(10) + '};')
        blobs.append('static const FontCell k_%s_all[%d] = {'
                     % (name, len(allcells)) + chr(10) +
                     chr(10).join('    {%d,%d,%d,%d},' % c for c in allcells) +
                     chr(10) + '};')
        norm_cells.append((name, {
            (c[0] / src_w, c[1] / src_h, (c[0] + c[2]) / src_w,
             (c[1] + c[3]) / src_h) for c in allcells}))
        entries.append('    {"%s", %d, %d, %d, %d, k_%s_alpha, %d, k_%s_cells, '
                       'k_%s_all, %d, %du, %dull},'
                       % (name, src_w, src_h, w, h, name, len(packed), name,
                          name, len(allcells), psize, psum))
        print('%-16s guest %dx%d  image %dx%d (%dx)  alpha %d -> rle %d bytes'
              % (name, src_w, src_h, w, h, w // src_w, len(alpha),
                 len(packed)))

    # Runtime identification is by cell rect ALONE (no character is
    # available for wide-char text), so two fonts sharing a normalised
    # cell would make a batch ambiguous. Checked here rather than hoped
    # for: it holds for the current set with room to spare, but a newly
    # added font could break it silently.
    for a in range(len(norm_cells)):
        for b in range(a + 1, len(norm_cells)):
            shared = norm_cells[a][1] & norm_cells[b][1]
            assert not shared, (
                'ambiguous cells between %s and %s: %s'
                % (norm_cells[a][0], norm_cells[b][0], sorted(shared)[:4]))
    # The game's OTHER fonts can share an embedded font's cells -- that is
    # exactly what made the glow pass draw as futuraheavy -- so the cell match
    # is confirmed at runtime by the payload sum. Check here that the sum
    # really does separate every font that collides, with a margin well
    # beyond the runtime tolerance (0.05%, FontAtlasPayloadMatches). The
    # margin is thin on retail -- its glow and heavy payloads sum only 1.3%
    # apart (dev: 12.6%) -- which is fine because the guest bytes ARE the
    # file bytes and the sum matches exactly.
    embedded = {n for n, _ in norm_cells}
    for fn in sorted(os.listdir(fonts_dir)):
        other = fn[:-len('.bmpFont')] if fn.endswith('.bmpFont') else None
        if other is None or other in embedded:
            continue
        og, ocm, ow, oh = parse_bmpfont(os.path.join(fonts_dir, fn))
        ocells = {(c[0] / ow, c[1] / oh, (c[0] + c[2]) / ow, (c[1] + c[3]) / oh)
                  for c in font_cells(og, ocm)}
        osize, osum = payload_sum(fonts_dir, other, ow, oh)
        for name, cells in norm_cells:
            shared = len(cells & ocells)
            if not shared:
                continue
            size, ssum = sums[name]
            ratio = abs(osum - ssum) / float(ssum)
            print('  %s shares %d cells with %s: payload sums %d vs %d '
                  '(%.1f%% apart)' % (other, shared, name, osum, ssum,
                                      ratio * 100))
            assert osize != size or ratio > 0.002, (
                'payload sum cannot separate %s from %s' % (other, name))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        f.write('\n\n'.join(blobs))
        f.write('\n\nconst FontAsset kFonts[] = {\n' + '\n'.join(entries) +
                '\n};\nconst unsigned int kFontCount = %d;\n\n' % len(entries))
        f.write('}  // namespace skate3::native_scene::font_data\n')
    print('wrote %s (%d bytes)' % (out_path, os.path.getsize(out_path)))


main()
