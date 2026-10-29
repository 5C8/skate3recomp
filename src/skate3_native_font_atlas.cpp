#include "skate3_native_font_atlas.h"

#include <cmath>
#include <cstring>

#include "generated/skate3_native_font_data.h"

namespace skate3::native_scene {
namespace {

using font_data::FontAsset;
using font_data::kFontCount;
using font_data::kFonts;

}  // namespace

uint32_t FontAtlasMipCount(int index) {
  if (index < 0 || index >= int(kFontCount)) {
    return 1;
  }
  const FontAsset& f = kFonts[index];
  uint32_t levels = 1;
  uint32_t w = f.img_w;
  uint32_t h = f.img_h;
  while (w > f.src_w && h > f.src_h && w > 1 && h > 1) {
    w /= 2;
    h /= 2;
    ++levels;
  }
  return levels;
}

int FontAtlasCount() {
  return int(kFontCount);
}

const char* FontAtlasName(int index) {
  if (index < 0 || index >= int(kFontCount)) {
    return nullptr;
  }
  return kFonts[index].name;
}

namespace {

// The emitted uvs are the cell edges exactly (verified against live draws:
// 'A' in debug.bmpFont sits at x 96 w 11, y 46 h 13, and the quad carries
// 96/128 .. 107/128 and 46/128 .. 59/128). A quarter-texel tolerance absorbs
// float32 rounding only.
bool CellMatches(const FontAsset& f, const font_data::FontCell& c, float u0,
                 float v0, float u1, float v1) {
  if (c.w == 0 || c.h == 0) {
    return false;
  }
  const float tu = 0.25f / float(f.src_w);
  const float tv = 0.25f / float(f.src_h);
  return std::fabs(u0 - float(c.x) / float(f.src_w)) < tu &&
         std::fabs(u1 - float(c.x + c.w) / float(f.src_w)) < tu &&
         std::fabs(v0 - float(c.y) / float(f.src_h)) < tv &&
         std::fabs(v1 - float(c.y + c.h) / float(f.src_h)) < tv;
}

}  // namespace

int MatchFontAtlas(float u0, float v0, float u1, float v1) {
  for (uint32_t i = 0; i < kFontCount; ++i) {
    const FontAsset& f = kFonts[i];
    for (uint32_t k = 0; k < f.all_cell_count; ++k) {
      if (CellMatches(f, f.all_cells[k], u0, v0, u1, v1)) {
        return int(i);
      }
    }
  }
  return -1;
}

uint32_t FontAtlasPayloadSize(int index) {
  if (index < 0 || index >= int(kFontCount)) {
    return 0;
  }
  return kFonts[index].payload_size;
}

bool FontAtlasPayloadMatches(int index, uint64_t sum) {
  if (index < 0 || index >= int(kFontCount)) {
    return false;
  }
  // The guest payload is the .rx2's bytes verbatim, so the sum should be
  // exact; the 0.05% slack only absorbs a stray rewritten block. The fonts
  // that share cells are further apart (glow vs heavy: 12.6% on dev, 1.3%
  // on retail; the generator asserts > 0.2%).
  const uint64_t want = kFonts[index].payload_sum;
  const uint64_t diff = sum > want ? sum - want : want - sum;
  return diff * 2000u <= want;
}

bool DecodeFontAtlas(int index, std::vector<uint8_t>& alpha, uint32_t& width,
                     uint32_t& height) {
  if (index < 0 || index >= int(kFontCount)) {
    return false;
  }
  const FontAsset& f = kFonts[index];
  width = f.img_w;
  height = f.img_h;
  const size_t texels = size_t(f.img_w) * f.img_h;
  alpha.assign(texels, 0);
  // RLE (see tools/gen_font_data.py): a control byte < 0x80 introduces a
  // literal run of (n + 1) bytes; >= 0x80 repeats the following byte
  // (n - 0x80 + 2) times.
  size_t out = 0;
  uint32_t in = 0;
  while (in < f.alpha_rle_size && out < texels) {
    const uint8_t ctrl = f.alpha_rle[in++];
    if (ctrl < 0x80) {
      const uint32_t n = uint32_t(ctrl) + 1;
      if (in + n > f.alpha_rle_size || out + n > texels) {
        return false;
      }
      std::memcpy(alpha.data() + out, f.alpha_rle + in, n);
      in += n;
      out += n;
    } else {
      const uint32_t n = uint32_t(ctrl) - 0x80u + 2u;
      if (in >= f.alpha_rle_size || out + n > texels) {
        return false;
      }
      std::memset(alpha.data() + out, f.alpha_rle[in++], n);
      out += n;
    }
  }
  return out == texels;
}

}  // namespace skate3::native_scene
