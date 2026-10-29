// Embedded replacement bitmap-font atlases. Generated data lives in
// skate3_native_font_data.cpp; regenerate with tools/gen_font_data.py after
// replacing any of the source images.
#pragma once

#include <cstdint>

namespace skate3::native_scene::font_data {

// One glyph's cell in the GUEST atlas, in texels, y measured from the top
// (the .bmpFont stores y at the cell's BOTTOM). w == 0 means the font has no
// glyph for that character code. Signed: a few glyphs are authored with a
// cell that starts one texel outside the atlas (debug.bmpFont's "'" is at
// x = -1), and the game's own uvs carry that as-is.
struct FontCell {
  int16_t x, y, w, h;
};

struct FontAsset {
  const char* name;
  uint32_t src_w, src_h;  // guest atlas size: normalises the cells for matching
  uint32_t img_w, img_h;  // replacement image size (any multiple works)
  const uint8_t* alpha_rle;
  uint32_t alpha_rle_size;
  const FontCell* cells;  // 256 entries, indexed by character code
  // Every distinct cell the font defines, for batches whose characters are
  // unknown to us: these fonts map codes up to 65535, and the wide-char
  // DrawstringLocal hands over no string at all, so identification has to be
  // possible from a uv rect alone.
  const FontCell* all_cells;
  uint32_t all_cell_count;
  // The guest atlas payload this replacement stands in for: its size in
  // bytes and the plain sum of those bytes. Cells alone are ambiguous (the
  // glow font reuses futuraheavy's layout), the sum is not, and it survives
  // tiling and endian swaps because both only permute bytes.
  uint32_t payload_size;
  uint64_t payload_sum;
};

extern const FontAsset kFonts[];
extern const unsigned int kFontCount;

}  // namespace skate3::native_scene::font_data
