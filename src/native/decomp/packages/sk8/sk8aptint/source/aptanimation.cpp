// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c
// Sk8 Apt integration package (Skate 2 PDB: packages/sk8/sk8aptint/1.00.05/source/aptanimation.cpp;
// no asserts here in sk83, so no sk83 path -- filed under the package name).
//   Sk8::FE::AptShape::Draw(const Matrix44 &, tRGBA &, tRGBA &, bool, float)   sk83_na_m 0x82f43890 (0xdc)
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace FE {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
#endif

#define kAptShapeZero 0x5fff0000u   // 0.0f

// AptShape (Skate 2 layout): +0xc NumUnits, +0x10 ppRenderingUnits. Fully transparent shapes
// (colour scale alpha <= 0: the VMX compare of a zero-filled splat against the alpha splat) draw
// nothing; otherwise every non-null rendering unit draws (vtable +4) with the same arguments.
void AptShape_Draw(unsigned int self, unsigned int vertexTransform, unsigned int colourScale,
                   unsigned int colourTranslate, unsigned int overrideZDepth, double zDepth)
{
    RWP_SAVEGPRLR(25);
    RWP_SAVEF31(0x48);
    RWP_GUEST_FRAME(frame, 0xb0);
    RWP_NV(31, 0u);
    RWP_NVF31(zDepth);
    RWP_NV(30, self);
    RWP_NV(27, vertexTransform);
    RWP_NV(29, colourScale);
    RWP_NV(26, colourTranslate);
    RWP_NV(25, overrideZDepth);
    // xcore::tRGBA zero(0.0f): stfs + three zero words at +0x50
    const unsigned int t50 = FRAME_AT(frame, 0x50);
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, t50) = (float)RWP_GLOBAL(RWP_F32, kAptShapeZero);
    RWP_ST32(t50 + 4u, 0u);
    RWP_ST32(t50 + 8u, 0u);
    RWP_ST32(t50 + 0xcu, 0u);
    RWP_VMX_MODE();
    const __vector4 alpha = __vspltw((__vector4)RWP_GLOBAL(RWP_VEC, colourScale & ~15u), 3);
    const __vector4 zero = __vspltw((__vector4)RWP_GLOBAL(RWP_VEC, t50), 0);
    if (VALL_GE(zero, alpha))
        return;
    RWP_NV(28, 0u);
    if ((int)RWP_LD32(self + 0xcu) <= 0)
        return;
    unsigned int off = 0;
    for (int i = 0; i < (int)RWP_LD32(self + 0xcu); i++, off += 4u)
    {
        const unsigned int unit = RWP_LD32(RWP_LD32(self + 0x10u) + off);
        if (unit)
            RWP_VCALL(unit, 4, vertexTransform, colourScale, colourTranslate, overrideZDepth, RWP_FLT(zDepth));
        RWP_NV(28, (unsigned int)(i + 1));
        RWP_NV(31, off + 4u);
    }
}

} }
