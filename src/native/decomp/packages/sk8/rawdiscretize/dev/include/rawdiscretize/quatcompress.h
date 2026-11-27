// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (2); comments keep the dev addresses.
// packages/sk8/rawdiscretize/dev/include/rawdiscretize/quatcompress.h (Skate 2 PDB line tables;
// sk83 has no assert in these, so no sk83 path): the replay pose codecs. Inline in the header; sk83
// keeps one out-of-line copy of each, called from Sk8::SkaterPresentation::ReadSkater (0x82914be0)
// when a replay / restored frame is posed.
//   ANIM_CODEC::RawDiscretize::QuatDecompressQuantize3   sk83_na_m 0x82905658 (0xd8)
//   ANIM_CODEC::RawDiscretize::QuatDecompressQuantize4   sk83_na_m 0x82905730 (0xa8)
//   ANIM_CODEC::RawDiscretize::TransDecompress           sk83_na_m 0x829057d8 (0x8c)
// Written from the sk83 recompiled code (the Skate 2 bodies are the same up to register choice);
// not yet replay-verified (snapshot set: restoreframe / restoreframe1).
#ifndef RAWDISCRETIZE_QUATCOMPRESS_H
#define RAWDISCRETIZE_QUATCOMPRESS_H
#include "rwp_guestcall.h"

namespace ANIM_CODEC { namespace RawDiscretize {
#ifdef RWP_GUEST
using namespace ::rwp;
// a 16-byte load from an unaligned address (lvx, lvx +15, lvsl, vperm)
#define RD_LOADU(addr) rwp::LVU(rwp::g_base, (addr))
#define RD_VSPLTISH(n) RWP_F(simde_mm_set1_epi16((short)(n)))
#else
#define RD_LOADU(addr) __vperm(__lvx((const void *)(size_t)(addr), 0), __lvx((const void *)(size_t)(addr), 15), __lvsl((const void *)(size_t)(addr), 0))
#define RD_VSPLTISH(n) __vspltish(n)
#endif

// tQuatQuantize3 { s16 x, y, z } (6 bytes), tQuatQuantize4 { s16 x, y, z, w } (8 bytes),
// tTransCompressed { s16 x, y, z } (6 bytes); outputs are 16-byte vectors.
#define kQuantInv     0x822f8600u     // 1 / 32767 (float)
#define kQuantMult    0x82310af0u     // QuatDecompressQuantize3::multiplier { 32768 x4 }

// s16 lanes x, y, z(, w) -> exact floats in [-1, 1): each halfword becomes the high half of a word
// whose low half is the same value shifted left once (vslw 1, vmrghh), converted with 2^-31.
static __forceinline __vector4 RdUnpack(unsigned int addr)
{
    const __vector4 raw = RD_LOADU(addr);
    return __vcfsx(__vmrghh(raw, __vslw(raw, RD_VSPLTISH(1))), 31);
}

// 1/sqrt(x): the estimate plus two Newton steps
static __forceinline __vector4 RdInvSqrt(__vector4 x, __vector4 one, __vector4 half)
{
    __vector4 r = __vrsqrtefp(x);
    r = __vmaddfp(__vmulfp(r, half), __vnmsubfp(x, __vmulfp(r, r), one), r);
    r = __vmaddfp(__vmulfp(r, half), __vnmsubfp(x, __vmulfp(r, r), one), r);
    return r;
}

// three-component quaternions: x, y, z scaled to [-1, 1], w = sqrt(1 - |xyz|^2) (0 when that is
// 0 or negative)
inline void QuatDecompressQuantize3(int n, unsigned int v, unsigned int qParam)
{
    const __vector4 scale = VSPLAT_REF(RWP_GLOBAL(RWP_F32, kQuantInv));   // lvlx + vspltw
    const __vector4 mult = RWP_GLOBAL(RWP_VEC, kQuantMult);
    if (n == 0)
        return;
    RWP_VMX_MODE();
    const __vector4 zero = __vspltisw(0);
    const __vector4 one = __vcfsx(__vspltisw(1), 0);
    const __vector4 half = __vcfsx(__vspltisw(1), 1);
    do
    {
        const __vector4 xyz = __vmulfp(__vrlimi(__vmulfp(RdUnpack(v), mult), zero, 1, 0), scale);
        v += 6u;
        const __vector4 ww = __vsubfp(one, __vmsum4fp(xyz, xyz));
        const __vector4 s = __vmulfp(ww, RdInvSqrt(ww, one, half));
        const __vector4 w = __vsel(__vsel(s, zero, __vcmpeqfp(zero, ww)), zero, __vcmpgtfp(zero, ww));
        RWP_GLOBAL(RWP_VEC, qParam) = __vrlimi(xyz, w, 1, 0);
        qParam += 16u;
    } while (--n);
}

// four-component quaternions, renormalised
inline void QuatDecompressQuantize4(int n, unsigned int v, unsigned int qParam)
{
    const __vector4 scale = VSPLAT_REF(RWP_GLOBAL(RWP_F32, kQuantInv));   // lvlx + vspltw
    const __vector4 mult = RWP_GLOBAL(RWP_VEC, kQuantMult);
    if (n == 0)
        return;
    RWP_VMX_MODE();
    const __vector4 one = __vcfsx(__vspltisw(1), 0);
    const __vector4 half = __vcfsx(__vspltisw(1), 1);
    do
    {
        const __vector4 q = __vmulfp(__vmulfp(RdUnpack(v), mult), scale);
        v += 8u;
        const __vector4 d = __vmsum4fp(q, q);
        RWP_GLOBAL(RWP_VEC, qParam) = __vmulfp(q, RdInvSqrt(d, one, half));
        qParam += 16u;
    } while (--n);
}

// translations: x, y, z * range, w = 1
inline void TransDecompress(int n, unsigned int vParam, unsigned int tParam, float range)
{
    RWP_FPU_MODE();
    const float r = range * (float)RWP_GLOBAL(RWP_F32, kQuantInv);   // fmuls (f1 is already single)
#ifdef RWP_GUEST
    rwp::GStore32(rwp::g_ctx->r1.u32 - 16u, std::bit_cast<u32>(r));   // stfs f0, -16(r1): the splat goes through the stack
#endif
    const __vector4 scale = VSPLAT_LX(r);
    const __vector4 mult = RWP_GLOBAL(RWP_VEC, kQuantMult);
    if (n == 0)
        return;
    RWP_VMX_MODE();
    const __vector4 one = __vcfsx(__vspltisw(1), 0);
    do
    {
        const __vector4 t = __vmulfp(__vmulfp(RdUnpack(vParam), mult), scale);
        vParam += 6u;
        RWP_GLOBAL(RWP_VEC, tParam) = __vrlimi(t, one, 1, 0);
        tParam += 16u;
    } while (--n);
}

#undef kQuantInv
#undef kQuantMult
} }

#endif
