// Replacement bitmap-font atlases for the native renderer.
//
// The game draws text as one quad per character out of a small bitmap atlas
// (128x128 for the debug font, 512x512 for the futura family), well below
// the native renderer's output resolution. The embedded replacements keep
// the SAME normalised glyph layout, so the game's own uvs index them
// unchanged and no geometry is touched -- only the texel density changes.
//
// A text draw is matched to its font by its glyph cells: the .bmpFont files
// give every character's cell in guest-atlas texels, and the uvs a
// DrawstringLocal batch emits are exactly those cells normalised, so one
// quad narrows it to a candidate (the four futura atlases are all 512x512,
// and guest addresses are not stable across runs, so neither size nor
// address can do it), and the guest atlas's payload byte sum confirms it.
#pragma once

#include <cstdint>
#include <vector>

namespace skate3::native_scene {

// Index of the font that defines a glyph at exactly this normalised cell,
// or -1 if none does. No character is needed, which matters because the
// wide-char DrawstringLocal hands over no string, and because these fonts map
// codes up to 65535. The generator verifies that no two EMBEDDED fonts share
// a normalised cell -- but the game's other fonts can: futuraglow is
// futuraheavy's layout with a blurred image. A cell match is therefore only a
// candidate; confirm it with FontAtlasPayloadMatches before serving the
// replacement.
int MatchFontAtlas(float u0, float v0, float u1, float v1);

// Size in bytes of the guest atlas payload font `index` replaces (0 if out
// of range): how much to sum from the draw's texture fetch base.
uint32_t FontAtlasPayloadSize(int index);

// Whether a guest atlas whose payload bytes sum to `sum` is the one font
// `index` replaces. Serving futuraheavy's crisp white replacement for the
// glow pass is what turned the game's pale-blue halo into flat white text.
bool FontAtlasPayloadMatches(int index, uint64_t sum);

// Expands font `index` into its 8-bit coverage plane. Alpha only: these
// atlases are constant-white RGB, so carrying three redundant channels
// through the decode and the mip chain would quadruple the memory traffic
// for nothing -- the upload synthesises them.
bool DecodeFontAtlas(int index, std::vector<uint8_t>& alpha, uint32_t& width,
                     uint32_t& height);

// Number of mip levels the replacement for font `index` should carry: the
// chain stops at the GUEST atlas's own resolution. Going below it would make
// minified text blurrier than the game's original renderer ever was, which
// is never an improvement; above it the extra levels are what keep magnified
// and minified text from aliasing.
uint32_t FontAtlasMipCount(int index);

// Number of embedded fonts.
int FontAtlasCount();

// Name of font `index` (for logging); nullptr if out of range.
const char* FontAtlasName(int index);

}  // namespace skate3::native_scene
