// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (5); comments keep the dev addresses.
// NFSMixShape, the mix-map's shape helpers (EA Nicotine; Skate 2 PDB: packages/versioned/nicotine/
// 1.02.00/source/nfsmixshape.cpp). GetPitchMultFromCents / GetCurveOutput are in nfsmixmap.cpp.
//   NFSMixShape::GetCentsFromPitchMult(float)                  sk83_na_m 0x82b0b438 (0x560)
//   NFSMixShape::GetAzimShapeOutput(eMIXTABLEID, eMIXTABLEID, int *, int) sk83_na_m 0x82b0bc58 (0x70)
// Written from the sk83 recompiled code (Skate 2 Hex-Rays as the guide); not yet replay-verified.
#include "rwp_guestcall.h"
#include "rwp_ppc_ops.h"

namespace NFSMixShape {
unsigned int GetCurveOutput_(unsigned int table, unsigned int x);   // nfsmixmap.cpp
unsigned int GetAzimShapeOutput_(unsigned int table1, unsigned int table2, unsigned int pDistances, unsigned int mixRatio);
unsigned int GetCentsFromPitchMult_(double ratio);

#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define g_NfsShapeOne       0x8231a844u     // 1.0f
#define g_NfsShapeQ15       0x821747fcu     // 32767.0f
#define g_NfsShapeCentsUp   0x822f8600u     // 1.993157f: millibels -> cents (ratio <= 1)
#define g_NfsShapeCentsDown 0x822f8600u     // -1.993157f (ratio > 1)
#define g_Q15todBXForm      0x82fdbfb0u     // Q15 linear -> millibels by octave (the same table as NFSMixMap's)

// Q15 linear -> millibels (the g_Q15todBXForm switch EA inlines wherever it converts; nfsmixmap.cpp
// has the same one): -10000 for anything outside 1..0x7fff.
static __forceinline unsigned int Q15todB(unsigned int x)
{
    const unsigned int c = (unsigned int)RwpCntlzw(x) - 17u;
    const unsigned int t = g_Q15todBXForm;
    if (c > 14u)
        return (unsigned int)-10000;
    switch (c)
    {
    case 0: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x4000u) >> 5)) - 602u;
    case 1: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x2000u) >> 4)) - 1204u;
    case 2: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x1000u) >> 3)) - 1806u;
    case 3: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x800u) >> 2)) - 2408u;
    case 4: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x400u) >> 1)) - 3010u;
    case 5: return RWP_LD32(t + 4u * (x - 0x200u)) - 3612u;
    case 6: return RWP_LD32(t + RwpRlwimi(4u, (x << 1) - 0x1ffu, 2, 0, 0x1c)) - 4214u;
    case 7: return RWP_LD32(t + RwpRlwimi(0xcu, (x << 2) - 0x1fdu, 2, 0, 0x1b)) - 4816u;
    case 8: return RWP_LD32(t + RwpRlwimi(0x1cu, (x << 3) - 0x1f9u, 2, 0, 0x1a)) - 5418u;
    case 9: return RWP_LD32(t + RwpRlwimi(0x3cu, (x << 4) - 0x1f1u, 2, 0, 0x19)) - 6020u;
    case 10: return RWP_LD32(t + RwpRlwimi(0x7cu, (x << 5) - 0x1e1u, 2, 0, 0x18)) - 6622u;
    case 11: return RWP_LD32(t + RwpRlwimi(0xfcu, (x << 6) - 0x1c1u, 2, 0, 0x17)) - 7224u;
    case 12: return RWP_LD32(t + RwpRlwimi(0x1fcu, (x << 7) - 0x181u, 2, 0, 0x16)) - 7826u;
    case 13: return RWP_LD32(t + RwpRlwimi(0x3fcu, (x << 8) - 0x101u, 2, 0, 0x15)) - 8428u;
    default: return RWP_LD32(t + RwpRlwimi(0x7fcu, (x << 9) - 1u, 2, 0, 0x14)) - 9030u;
    }
}

// frequency ratio -> cents: the ratio as a Q15 fraction (32767 / ratio above 1, else ratio * 32767),
// its log in millibels, scaled to cents (negative above 1). A leaf: its fctiwz scratch is -16(r1).
unsigned int GetCentsFromPitchMult_(double ratio)
{
    RWP_FPU_MODE();
    const float one = RWP_GLOBAL(RWP_F32, g_NfsShapeOne), q15 = RWP_GLOBAL(RWP_F32, g_NfsShapeQ15);
    float lin, scale;
    if (ratio > (double)one)
    {
        lin = (float)((double)q15 / ratio);                                  // fdivs
        scale = RWP_GLOBAL(RWP_F32, g_NfsShapeCentsDown);
    }
    else
    {
        lin = (float)(ratio * (double)q15);                                  // fmuls
        scale = RWP_GLOBAL(RWP_F32, g_NfsShapeCentsUp);
    }
    const unsigned int mb = Q15todB((unsigned int)RwpFctiwz((double)lin));
    const unsigned long long cents = RwpFctiwz((double)((float)(int)mb * scale));
#if defined(RWP_GUEST)
    GStore64(g_ctx->r1.u32 - 16u, cents);                                    // stfd f10, -16(r1)
#endif
    return (unsigned int)cents;
}

// The rolloff of a 3D control between two azimuth quadrants: table1's curve at both quadrant
// distances (the second only when the mix ratio is non-zero; table2 is not used), crossfaded by
// 2 * ratio in Q15.
unsigned int GetAzimShapeOutput_(unsigned int table1, unsigned int table2, unsigned int pDistances, unsigned int mixRatio)
{
    (void)table2;
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    const unsigned int out0 = GetCurveOutput_(table1, RWP_LD32(pDistances));
    unsigned int out1 = 0x7fffu;
    if (mixRatio)
        out1 = GetCurveOutput_(table1, RWP_LD32(pDistances + 4u));
    const unsigned int w = mixRatio << 1;
    return (unsigned int)((int)((0x7fffu - w) * out0) >> 15) + (unsigned int)((int)(w * out1) >> 15);
}

}
