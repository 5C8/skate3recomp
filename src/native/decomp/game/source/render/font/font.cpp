// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8336a44c
// game/source/render/font/font.cpp (sk83 assert path): Sk8::Render::cFont.
//   cFont::FindChar(unsigned short)   sk83_na_m 0x82999480 (0xac)
// Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"

namespace eastl {
void HashFindU16(unsigned int result, unsigned int table, unsigned int keyp);
void HashFindS32(unsigned int result, unsigned int table, unsigned int keyp);
}

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define g_DefaultGlyph 0x5fff0000u

// character -> glyph index (+0x28, a hash map of u16 -> int), index -> glyph (+8, int -> tGlyph);
// a character the font lacks gives the default glyph
unsigned int cFont_FindChar(unsigned int self, unsigned int ch)
{
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_ST8(FRAME_AT(frame, 0x8e), (ch >> 8) & 0xffu);          // sth r4, 0x8e(r1)
    RWP_ST8(FRAME_AT(frame, 0x8f), ch & 0xffu);
    eastl::HashFindU16(FRAME_AT(frame, 0x50), self + 0x28u, FRAME_AT(frame, 0x8e));
    const unsigned int it = RWP_LD32(FRAME_AT(frame, 0x50));
    if (it == RWP_LD32(RWP_LD32(self + 0x2cu) + 4u * RWP_LD32(self + 0x30u)))
        return g_DefaultGlyph;
    RWP_ST32(FRAME_AT(frame, 0x50), RWP_LD32(it + 4u));
    eastl::HashFindS32(FRAME_AT(frame, 0x58), self + 8u, FRAME_AT(frame, 0x50));
    const unsigned int g = RWP_LD32(FRAME_AT(frame, 0x58));
    if (g == RWP_LD32(RWP_LD32(self + 0xcu) + 4u * RWP_LD32(self + 0x10u)))
        return g_DefaultGlyph;
    return RWP_LD32(g + 4u);
}

} }
