// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 8335b9f4 8335bda0 8335bf20 8335c0e0
// game/source/fe/apt/aptrenderingintegration.cpp: Sk8::FE::AptRenderingIntegration, the Apt
// legacy transform callbacks (CallLegacyTransformCallbacks, packages/ea/apt/aptrender.cpp, calls
// them through the Apt render callback table +0x6c / +0x70). No asserts in either function; the
// file is the Skate 2 PDB's (code/ng/fe/apt/aptrenderingintegration.cpp), same functions.
//   Sk8::FE::AptRenderingIntegration::SetVertexMatrix(AptMatrix *)       sk83_na_m 0x8273b6d8 (0x10)
//   Sk8::FE::AptRenderingIntegration::SetColourTransform(AptCXForm *)    sk83_na_m 0x82746420 (0x108)
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

void AptColorHelper_CopyToFloatArray4(unsigned int self, unsigned int out);   // packages/ea/apt/aptcxform.cpp

namespace Sk8 { namespace FE {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
void AptIntegration_SetAptMatrix(unsigned int matrix, unsigned int pMatrix);   // aptintegration.cpp

#define g_AptVertexMatrix      0x5fff0000u   // AptRenderingIntegration's vertex matrix (Matrix44)
#define g_AptColourScale       0x5fff0000u   // AptRenderingIntegration::ColourScale (vector)
#define g_AptColourTranslate   0x5fff0000u   // AptRenderingIntegration::ColourTranslate (vector)
#define g_AptVisualDirector    0x5fff0000u   // AptRenderingIntegration::mVisualDirector
#define kAptRiZero             0x5fff0000u   // 0.0f

// The 2D transform callback: tail call into SetAptMatrix with the vertex matrix.
void AptRenderingIntegration_SetVertexMatrix(unsigned int pMatrix)
{
    AptIntegration_SetAptMatrix(g_AptVertexMatrix, pMatrix);
}

// The colour transform callback: scale and translate (+0 / +8, AptColorHelper each) as float
// vectors with the first component moved to the end (g, b, a, r order of the copy). While the
// visual director's presentation state fades (+0 -> byte +0x30c0 == 1, value +0x30c4 > 0) the
// scale's last component is zeroed.
void AptRenderingIntegration_SetColourTransform(unsigned int cxform)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x90);
    RWP_NV(31, cxform);
    const unsigned int t50 = FRAME_AT(frame, 0x50), t60 = FRAME_AT(frame, 0x60), t70 = FRAME_AT(frame, 0x70);
    AptColorHelper_CopyToFloatArray4(cxform, t50);
    RWP_FPU_MODE();
    {
        const float a = RWP_GLOBAL(RWP_F32, t50 + 4u), b = RWP_GLOBAL(RWP_F32, t50 + 8u);
        const float c = RWP_GLOBAL(RWP_F32, t50 + 0xcu), d = RWP_GLOBAL(RWP_F32, t50);
        RWP_GLOBAL(RWP_F32, t60) = a;
        RWP_GLOBAL(RWP_F32, t60 + 4u) = b;
        RWP_GLOBAL(RWP_F32, t60 + 8u) = c;
        RWP_GLOBAL(RWP_F32, t60 + 0xcu) = d;
    }
    const unsigned long long scale01 = RWP_LD64(t60);                       // v63 (lvx +0x60)
    const unsigned int scale2 = RWP_LD32(t60 + 8u), scale3 = RWP_LD32(t60 + 0xcu);
    RWP_ST64(g_AptColourScale, scale01);
    RWP_ST32(g_AptColourScale + 8u, scale2);
    RWP_ST32(g_AptColourScale + 0xcu, scale3);
    const unsigned int vd = RWP_LD32(g_AptVisualDirector);
    if (vd)
    {
        const unsigned int state = RWP_LD32(vd);
        if (RWP_LD8(state + 0x30c0u) == 1u)
        {
            const float zero = RWP_GLOBAL(RWP_F32, kAptRiZero);
            if ((float)RWP_GLOBAL(RWP_F32, state + 0x30c4u) > zero)
            {
                RWP_GLOBAL(RWP_F32, t60) = zero;
                RWP_ST32(t60 + 4u, 0u);
                RWP_ST32(t60 + 8u, 0u);
                RWP_ST32(t60 + 0xcu, 0u);
                // vspltw of the zero + vrlimi into w: the stored scale keeps x, y, z
                RWP_ST64(g_AptColourScale, scale01);
                RWP_ST32(g_AptColourScale + 8u, scale2);
                RWP_ST32(g_AptColourScale + 0xcu, RWP_LD32(t60));
            }
        }
    }
    AptColorHelper_CopyToFloatArray4(cxform + 8u, t70);
    RWP_FPU_MODE();
    {
        const float a = RWP_GLOBAL(RWP_F32, t70 + 4u), b = RWP_GLOBAL(RWP_F32, t70 + 8u);
        const float c = RWP_GLOBAL(RWP_F32, t70 + 0xcu), d = RWP_GLOBAL(RWP_F32, t70);
        RWP_GLOBAL(RWP_F32, t70) = a;
        RWP_GLOBAL(RWP_F32, t70 + 4u) = b;
        RWP_GLOBAL(RWP_F32, t70 + 0xcu) = d;
        RWP_GLOBAL(RWP_F32, t70 + 8u) = c;
    }
    RWP_ST64(g_AptColourTranslate, RWP_LD64(t70));
    RWP_ST64(g_AptColourTranslate + 8u, RWP_LD64(t70 + 8u));
}

} }
