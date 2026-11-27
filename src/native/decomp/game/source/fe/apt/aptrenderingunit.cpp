// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (1); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 82a3f710 832d8794 8336cfcc
// game/source/fe/apt/aptrenderingunit.cpp: Sk8::FE::AptTextureUnit (file from the Skate 2 PDB,
// code/ng/fe/apt/aptrenderingunit.cpp; the only sk83 assert inlined here is singletonholder.h).
//   Sk8::FE::AptTextureUnit::Draw(const Matrix44 &, tRGBA &, tRGBA &, bool, float)   sk83_na_m 0x827467a0 (0x1b8)
// with the two xcore::tRGBA operators it calls (out-of-line copies in sk83, no path of their own):
//   xcore::tRGBA::operator*(const tRGBA &) const   sk83_na_m 0x8273d858 (0x80, leaf)
//   xcore::tRGBA::operator+(const tRGBA &) const   sk83_na_m 0x8273d7d0 (0x80, leaf)
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace Render {
void SimpleDraw_Draw_TriListTextured_Fasttrack(unsigned int self, unsigned int numVerts, unsigned int verts,
                                               unsigned int uvs, unsigned int tex, unsigned int colour,
                                               unsigned int m);   // game/source/render/simpledraw.cpp
} }

namespace Sk8 { namespace FE {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int AptTexUnit_NewArray(unsigned long long size, unsigned int category, unsigned long long name, unsigned int flags);
void AptTexUnit_DeleteArray(unsigned int p);
#endif

#define kAptTexUnitZero          0x5fff0000u   // 0.0f
#define g_kMemCategory_FE        0x5fff0000u   // kMemCategory_FE.mCatId
#define g_SimpleDrawInstance     0x5fff0000u   // SingletonHolder<Sk8::Render::SimpleDraw>::sInstance

// xcore::tRGBA *, + : component-wise VMX product / sum of the two colours (splats of each lane in
// the original, same lanes), staged through r1-0x10 with lfs/stfs and stored with stvx.
static __forceinline void TRgbaOp(unsigned int out, unsigned int a, unsigned int b, bool mul)
{
    RWP_VMX_MODE();
    const __vector4 va = (__vector4)RWP_GLOBAL(RWP_VEC, a & ~15u), vb = (__vector4)RWP_GLOBAL(RWP_VEC, b & ~15u);
    const __vector4 r = mul ? __vmulfp(va, vb) : __vaddfp(va, vb);
#if defined(RWP_GUEST)
    RWP_GLOBAL(RWP_VEC, (g_ctx->r1.u32 - 0x10u) & ~15u) = r;
#endif
    RWP_GLOBAL(RWP_VEC, out & ~15u) = r;
}
unsigned int TRgba_Mul(unsigned int out, unsigned int self, unsigned int other) { TRgbaOp(out, self, other, true); return out; }
unsigned int TRgba_Add(unsigned int out, unsigned int self, unsigned int other) { TRgbaOp(out, self, other, false); return out; }

// AptTextureUnit (Skate 2 layout): +8 m_strip {NumVerts, pPositions +0xc, pDrawUVs +0x10},
// +0x20 m_colour, +0x80 m_pTextureInfo (texture at +0). With a texture: colour = m_colour *
// scale + translate; fully transparent (alpha <= 0) draws nothing. With overrideZDepth the
// positions go to a temporary array (new[] in the FE category) whose z is zDepth -- the first three
// only, as in the original (a 3-iteration loop of 16-byte copies) -- freed after the draw.
void AptTextureUnit_Draw(unsigned int self, unsigned int vertexTransform, unsigned int colourScale,
                         unsigned int colourTranslate, unsigned int overrideZDepth, double zDepth)
{
    RWP_SAVEGPRLR(27);
    RWP_SAVEF31(0x38);
    RWP_GUEST_FRAME(frame, 0xb0);
    RWP_NV(30, self);
    RWP_NV(27, vertexTransform);
    RWP_NVF31(zDepth);
    RWP_NV(31, colourTranslate);
    RWP_NV(28, overrideZDepth);
    const unsigned int info = RWP_LD32(self + 0x80u);
    if (!info || !RWP_LD32(info))
        return;
    const unsigned int t50 = FRAME_AT(frame, 0x50), t60 = FRAME_AT(frame, 0x60);
    const unsigned int c = TRgba_Mul(t50, self + 0x20u, colourScale);
    TRgba_Add(t60, c, colourTranslate);
    RWP_FPU_MODE();
    const float zero = RWP_GLOBAL(RWP_F32, kAptTexUnitZero);
    RWP_NV(29, 0u);
    RWP_GLOBAL(RWP_F32, t50) = zero;
    RWP_ST32(t50 + 4u, 0u);
    RWP_ST32(t50 + 8u, 0u);
    RWP_ST32(t50 + 0xcu, 0u);
    RWP_VMX_MODE();
    if (VALL_GE(__vspltw((__vector4)RWP_GLOBAL(RWP_VEC, t50), 0), __vspltw((__vector4)RWP_GLOBAL(RWP_VEC, t60), 3)))
        return;
    const bool override = (overrideZDepth & 0xffu) != 0;
    RWP_NV(28, overrideZDepth & 0xffu);
    unsigned int positions = RWP_LD32(self + 0xcu);
    RWP_NV(31, positions);
    if (override)
    {
        const unsigned int n = RWP_LD32(self + 8u);
        const unsigned long long size = n > 0x0fffffffu ? ~0ull : (unsigned long long)(n << 4);
        positions = RWP_CALL(0x5fff0000, AptTexUnit_NewArray, size, RWP_LD32(g_kMemCategory_FE),
                             0xffffffff82129d2cull /* "AptTextureUnitTemporaryStrips" */, 0u);
        RWP_NV(31, positions);
        for (unsigned int k = 0; k < 0x30u; k += 0x10u)
        {
            RWP_FPU_MODE();
            RWP_GLOBAL(RWP_F32, t50) = (float)zDepth;
            RWP_ST32(t50 + 4u, 0u);
            RWP_ST32(t50 + 8u, 0u);
            RWP_ST32(t50 + 0xcu, 0u);
            const unsigned int src = (RWP_LD32(self + 0xcu) + k) & ~15u, dst = (positions + k) & ~15u;
            const unsigned long long p0 = RWP_LD64(src), p1 = RWP_LD64(src + 8u);
            RWP_ST64(dst, p0);
            RWP_ST64(dst + 8u, p1);
            RWP_ST32(dst + 8u, RWP_LD32(t50));                                  // vrlimi: z = zDepth
        }
    }
    RWP_NV(29, 0xffffffff83370000ull);
    RWP_ST64(t50, RWP_LD64(t60));
    RWP_ST64(t50 + 8u, RWP_LD64(t60 + 8u));
    const unsigned int sd = RWP_LD32(g_SimpleDrawInstance);   // (singletonholder.h assert when null)
    Sk8::Render::SimpleDraw_Draw_TriListTextured_Fasttrack(sd, RWP_LD32(self + 8u), positions, RWP_LD32(self + 0x10u),
                                                           RWP_LD32(RWP_LD32(self + 0x80u)), t50, vertexTransform);
    if (override)
        RWP_CALL(0x82c5ebd8, AptTexUnit_DeleteArray, positions);
}

} }
