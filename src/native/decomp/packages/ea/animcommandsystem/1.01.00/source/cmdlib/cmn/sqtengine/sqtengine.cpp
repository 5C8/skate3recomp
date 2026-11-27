// AnimCmdSys::ACSsqtBlend_job (sk83_na_m 0x82a5aa90, 0xcfc): the animation command system's
// SQT job. Runs a list of SQTOpData over numBones bones each (scale, quaternion, translation).
// Hand-decompiled from the sk83 code with Skate 2's PDB types/names (sqtBlendSQTs::SQTJobData,
// SQTOpData) and its Hex-Rays output as the guide. Builds the three ways rwp_config.h describes.
//
// cmdtype
//   0  blend:      out = lerp(in1, in2, alpha); quaternions nlerp on the shorter arc
//   1  weighted:   as 0 with a per-bone weight clamp(alpha * trans.w, 0, 1), trans.w taken from
//                  in1 (ArbitraryInt != 0) or in2
//   2  n-blend:    out = sum coef[k] * in[k] over ArbitraryInt inputs, quaternions renormalised
//   3  mirror:     out = in1, then per BoneMirrorList pair (bone, partner): sign flips and a swap
//   8  compose:    out = in2 * in1 (scale product, in2 rotation applied to in1's translation)
//   9  copy range: count = int(alpha * 160 + 1) bones of in2 at ArbitraryInt, the rest from in1
//   10 scale:      selected bones' translation (and scale) times alpha
//   4..7: nothing. Every op then copies the output to extraOutput when that is set.
// Copies are XMemCpy calls in the original (plain non-overlapping copies).
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "anim_types.h"
#include <string.h>

#ifndef RW_FMADDS
#define RW_FMADDS(a, b, c) ((a) * (b) + (c))   // XDK: fmadds
#endif

namespace AnimCmdSys {
#ifdef RWP_GUEST
using namespace ::rwp;      // the recompiler's vector lowering
#endif

using sqtBlendSQTs::SQTJobData;
using sqtBlendSQTs::SQTOpData;

RWP_ALIGN16 static const unsigned int sSqtConst[][4] =
{
    { 0x00010203, 0x14151617, 0x00010203, 0x00010203 },   // 0 (a.x, b.y, .., ..)
    { 0x00010203, 0x00010203, 0x00010203, 0x14151617 },   // 1 (a.x, a.x, a.x, b.y)
    { 0x08090a0b, 0x1c1d1e1f, 0x00010203, 0x00010203 },   // 2 (a.z, b.w, .., ..)
    { 0x00000000, 0x3f800000, 0x00000000, 0x00000000 },   // 3 quaternion (0, 1, 0, 0)
};
#define SC(i) RWP_CONST(sSqtConst[i])
#if defined(_M_PPC)
// rw::math constants (another TU), reloaded at every use like the original
extern const unsigned int sSqtPack[4];   // 0x820aa630
extern const unsigned int sSqtP1[4];     // 0x820ad0f0
extern const unsigned int sSqtP2[4];     // 0x820ad100
#define SQ_PACK RWP_CONST(sSqtPack)
#define SQ_P1 RWP_CONST(sSqtP1)
#define SQ_P2 RWP_CONST(sSqtP2)
#else
#define SQ_PACK SC(0)
#define SQ_P1 SC(1)
#define SQ_P2 SC(2)
#endif

typedef RWP_PTR(SQTDataInternal) SqtPtr;

static __forceinline void SqtCopy(SqtPtr dst, SqtPtr src, unsigned int size)
{
#ifdef RWP_GUEST
    memcpy(rwp::g_base + RWP_ADDR_OF(dst), rwp::g_base + RWP_ADDR_OF(src), size);
#else
    memcpy((void *)dst, (const void *)src, size);
#endif
}

// a x b in the permuted form rw::math vpu Cross uses
static __forceinline __vector4 SqtCross(__vector4 a, __vector4 b)
{
    return __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63);
}

// quaternion product a * b, as rw::math vpu evaluates it
static __forceinline __vector4 SqtQuatMul(__vector4 a, __vector4 b)
{
    __vector4 aw = __vspltw(a, 3), bw = __vspltw(b, 3);
    __vector4 v = __vmaddfp(a, bw, __vmaddfp(b, aw, SqtCross(a, b)));
    return __vrlimi(v, __vsubfp(__vmulfp(aw, bw), __vmsum3fp(a, b)), 1, 1);
}

// v / |v| (4D), two Newton steps on the reciprocal square root
static __forceinline __vector4 SqtNormalize(__vector4 v, __vector4 half, __vector4 one)
{
    __vector4 d = __vmsum4fp(v, v);
    __vector4 e = __vrsqrtefp(d);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
    return __vmulfp(v, e);
}

// normalised lerp from q1 towards q2 or -q2, whichever is nearer
static __forceinline __vector4 SqtNlerp(__vector4 q1, __vector4 q2, __vector4 w, __vector4 zero,
                                        __vector4 half, __vector4 one)
{
    __vector4 near = __vcmpgtfp(__vmsum4fp(q1, q2), zero);
    __vector4 toward = __vmaddfp(__vsubfp(q2, q1), w, q1);
    __vector4 away = __vnmsubfp(__vaddfp(q2, q1), w, q1);
    return SqtNormalize(__vsel(away, toward, near), half, one);
}

// one bone of a blend: scale, rotation, translation, in that order
static __forceinline void SqtBlendBone(SqtPtr out, SqtPtr in1, SqtPtr in2, __vector4 w, __vector4 zero,
                                       __vector4 half, __vector4 one)
{
    __vector4 s1 = LOADV(in1->mScale);
    STOREV(out->mScale, __vmaddfp(__vsubfp(LOADV(in2->mScale), s1), w, s1));
    STOREV(out->mQuat, SqtNlerp(LOADV(in1->mQuat), LOADV(in2->mQuat), w, zero, half, one));
    __vector4 t1 = LOADV(in1->mTrans);
    STOREV(out->mTrans, __vmaddfp(__vsubfp(LOADV(in2->mTrans), t1), w, t1));
}

// negate one lane by multiplying it by -1 (mask: 8 x, 4 y, 2 z)
template <int lane, int mask>
static __forceinline __vector4 SqtNegate(__vector4 v, __vector4 minusOne)
{
    (void)minusOne;   // every negation builds its own VecFloat(-1.0f), as the original does
    return __vrlimi(v, __vmulfp(__vspltw(v, lane), VSPLAT_F(-1.0f)), mask, 0);
}

// mirror flips
// one lane of a member vector negated in place and stored straight back (rw::math per-element
// setter, with its own VecFloat(-1))
template <int lane, int mask>
static __forceinline void SqtNegLane(RWP_VEC &v)
{
    __vector4 x = v;
    v = __vrlimi(x, __vmulfp(__vspltw(x, lane), VSPLAT_F(-1.0f)), mask, 0);
}
static __forceinline void SqtFlipXY_Z(SqtPtr b, __vector4 m1)     // quat (-x, -y), trans -z
{
    (void)m1;
    SqtNegLane<0, 8>(b->mQuat);
    SqtNegLane<1, 4>(b->mQuat);
    SqtNegLane<2, 2>(b->mTrans);
}
static __forceinline void SqtFlipYZ_X(SqtPtr b, __vector4 m1)     // quat (-y, -z), trans -x
{
    (void)m1;
    SqtNegLane<1, 4>(b->mQuat);
    SqtNegLane<2, 2>(b->mQuat);
    SqtNegLane<0, 8>(b->mTrans);
}

static __forceinline int SqtTruncToInt(float f)    // fctiwz
{
#if defined(_M_PPC)
    return (int)f;    // fctiwz saturates by itself
#endif
    if (f != f) return (int)0x80000000;
    if (f >= 2147483648.0f) return 0x7fffffff;
    if (f < -2147483648.0f) return (int)0x80000000;
    return (int)f;
}

struct SqtVF { __vector4 v; };   // a rw::math VecFloat object (kept in memory)

// a reference proxy holding a pointer (the original's ?: over such objects goes through memory)
struct SqtVecRef
{
    RWP_PTR(RWP_VEC) p;
    __forceinline SqtVecRef(RWP_PTR(RWP_VEC) q) : p(q) {}
};

void ACSsqtBlend_job(unsigned int spuID, RWP_PTR(SQTJobData) data, unsigned int dataSize, unsigned int extraparams)
{
    (void)spuID; (void)dataSize;
    unsigned int numOps = data->numSQTOps; (void)extraparams;
    RWP_PTR(SQTOpData) op = data->jobData;
    if (numOps == 0)
        return;
    for (; numOps; numOps--, op++)
    {
        SqtPtr extra = op->extraOutput;
        const unsigned int cmdtype = op->cmdtype;
        switch (cmdtype)
        {
        case 0:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
            __vector4 a = VSPLAT_LX((float)op->alpha);
            if (numBones <= 0)
                break;
            const __vector4 zero = __vspltisw(0);
            for (int i = 0; i < numBones; i++)
                SqtBlendBone(out + i, in1 + i, in2 + i, a, zero, half, one);
            break;
        }
        case 1:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            const bool fromIn1 = arbInt != 0;
            SqtVF oneV, zeroV;
            oneV.v = VSPLAT_REF(1.0f);
            __vector4 a = VSPLAT_REF(op->alpha);
            zeroV.v = VSPLAT_REF(0.0f);
            if (numBones <= 0)
                break;
            const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
            const __vector4 zero = __vspltisw(0);
            for (int i = 0; i < numBones; i++)
            {
                SqtVecRef sref = fromIn1 ? SqtVecRef(RWP_ADDR(in1[i].mTrans)) : SqtVecRef(RWP_ADDR(in2[i].mTrans));
                __vector4 src = *sref.p;
                SqtVF w;
                w.v = __vmulfp(a, __vspltw(src, 3));
                w = VALL_GT(w.v, VSPLAT_F(1.0f)) ? oneV : w;
                w = VALL_GT(VSPLAT_F(0.0f), w.v) ? zeroV : w;
                SqtBlendBone(out + i, in1 + i, in2 + i, w.v, zero, half, one);
            }
            break;
        }
        case 2:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            SqtPtr src = op->NBlendSQTPtrs[0];
            RWP_PTR(BlendCoef) coef = op->NBlendCoefList;
            __vector4 c = __vspltw(__lvlx(RWP_ADDR(coef[0].v), 0), 0);
            for (int i = 0; i < numBones; i++)
            {
                __vector4 dpt1 = LOADV(src[i].mScale);
                STOREV(out[i].mScale, __vmulfp(c, dpt1));
                STOREV(out[i].mQuat, __vmulfp(c, LOADV(src[i].mQuat)));
                STOREV(out[i].mTrans, __vmulfp(c, LOADV(src[i].mTrans)));
            }
            if ((int)arbInt > 1)
            {
                for (unsigned int k = 1; k < arbInt; k++)
                {
                    __vector4 ck = VSPLAT_F((float)coef[k].v);
                    SqtPtr in = op->NBlendSQTPtrs[k];
                    for (int i = 0; i < numBones; i++)
                    {
                        STOREV(out[i].mScale, __vmaddfp(ck, LOADV(in[i].mScale), LOADV(out[i].mScale)));
                        STOREV(out[i].mQuat, __vmaddfp(LOADV(in[i].mQuat), ck, LOADV(out[i].mQuat)));
                        STOREV(out[i].mTrans, __vmaddfp(ck, LOADV(in[i].mTrans), LOADV(out[i].mTrans)));
                    }
                }
            }
            if (numBones > 0)
            {
                const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
                for (int i = 0; i < numBones; i++)
                    STOREV(out[i].mQuat, SqtNormalize(LOADV(out[i].mQuat), half, one));
            }
            break;
        }
        case 3:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            RWP_PTR(BoneIndex) mirror = data->BoneMirrorList;
            if (out != in1)
                SqtCopy(out, in1, (unsigned int)numBones * 0x30);
            if (numBones <= 0)
                break;
            const __vector4 m1 = VSPLAT_F(-1.0f);
            for (int i = 0; i < numBones; i++)
            {
                int m = mirror[i].v;
                int partner = mirror[i + numBones].v;
                if (partner < i)
                    continue;
                if (arbShort == 1)
                {
                    if (i != 0 && m != 0)
                        SqtFlipXY_Z(out + i, m1);
                    else
                    {
                        if (m == 0)
                            STOREV((out + i)->mQuat, SqtQuatMul(LOADV((out + i)->mQuat), SC(3)));
                        SqtFlipYZ_X(out + i, m1);
                    }
                }
                else
                    SqtFlipXY_Z(out + i, m1);
                if (partner > i)
                {
                    SqtPtr p = out + partner;
                    __vector4 s = LOADV((out + i)->mScale), q = LOADV((out + i)->mQuat), t = LOADV((out + i)->mTrans);
                    STOREV((out + i)->mScale, LOADV(p->mScale));
                    STOREV((out + i)->mQuat, LOADV(p->mQuat));
                    STOREV((out + i)->mTrans, LOADV(p->mTrans));
                    STOREV(p->mScale, s);
                    STOREV(p->mQuat, q);
                    STOREV(p->mTrans, t);
                    SqtFlipXY_Z(out + i, m1);
                }
            }
            break;
        }
        case 8:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            const bool fromIn1 = arbInt != 0;
            if (numBones <= 0)
                break;
            const __vector4 two = __vcfsx(__vspltisw(2), 0);
            for (int i = 0; i < numBones; i++)
            {
                STOREV(out[i].mScale, __vmulfp(LOADV(in2[i].mScale), LOADV(in1[i].mScale)));
                __vector4 t1 = LOADV(in1[i].mTrans), t2 = LOADV(in2[i].mTrans), q2 = LOADV(in2[i].mQuat);
                __vector4 T1 = __vrlimi(__vperm(t1, t1, SQ_PACK), t1, 2, 0);
                __vector4 T2 = __vrlimi(__vperm(t2, t2, SQ_PACK), t2, 2, 0);
                __vector4 u = __vmaddfp(T1, __vspltw(q2, 3), SqtCross(q2, T1));
                __vector4 r = __vaddfp(__vmaddfp(two, SqtCross(q2, u), T1), T2);
                SqtVecRef wref = fromIn1 ? SqtVecRef(RWP_ADDR(in1[i].mTrans)) : SqtVecRef(RWP_ADDR(in2[i].mTrans));
                __vector4 wsrc = *wref.p;
                STOREV(out[i].mTrans, __vsldoi(__vperm(r, r, SQ_P1), __vperm(r, wsrc, SQ_P2), 8));
                STOREV(out[i].mQuat, SqtQuatMul(LOADV(in2[i].mQuat), LOADV(in1[i].mQuat)));
            }
            break;
        }
        case 9:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            const int count = SqtTruncToInt(RW_FMADDS((float)op->alpha, 160.0f, 1.0f));
            const unsigned int nb = (unsigned int)numBones;
            const unsigned int start = arbInt;
            if (arbShort == 0)
            {
                if (out != in1)
                    SqtCopy(out, in1, nb * 0x30);
                SqtCopy(out + start, in2, (unsigned int)count * 0x30);
            }
            else
            {
                if (out != in2)
                    SqtCopy(out + start, in2, (unsigned int)count * 0x30);
                if (start != 0)
                    SqtCopy(out, in1, (start - 1) * 0x30);
                const unsigned int end = start + (unsigned int)count;
                if (end < nb)
                    SqtCopy(out + end, in1 + end, (nb - (unsigned int)count) * 0x30);
            }
            break;
        }
        case 10:
        {
            const int numBones = data->numBones;
            const unsigned int arbShort = op->ArbitraryShort;
            const unsigned int arbInt = op->ArbitraryInt;
            SqtPtr in1 = op->SQTInput1;
            SqtPtr in2 = op->SQTInput2;
            SqtPtr out = op->SQTOutput;
            const int traj = data->TrajNode;
            RWP_PTR(BoneIndex) list = data->BoneMirrorList;
            const float alpha = op->alpha;
            if (out != in1)
                SqtCopy(out, in1, (unsigned int)numBones * 0x30);
            const bool trajTrans = arbShort != 0;
            for (int i = 0; i < numBones; i++)
            {
                if (i == traj ? trajTrans : i == (int)arbInt)
                {
                    STOREV(out[i].mTrans, __vmulfp(LOADV(in1[i].mTrans), VSPLAT_F(alpha)));
                }
                else if (list[i].v == (int)arbInt)
                {
                    STOREV(out[i].mScale, __vmulfp(LOADV(in1[i].mScale), VSPLAT_F(alpha)));
                    STOREV(out[i].mTrans, __vmulfp(LOADV(in1[i].mTrans), VSPLAT_F(alpha)));
                }
            }
            break;
        }
        default:
            break;
        }
        if (extra)
        {
            SqtPtr res = op->SQTOutput;
            SqtCopy(extra, res, (unsigned int)data->numBones * 0x30);
        }
    }
}

}
