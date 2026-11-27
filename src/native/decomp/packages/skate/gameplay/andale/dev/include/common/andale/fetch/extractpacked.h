// Andale::TrajectoryCalculation (sk83_na_m 0x82feaeb8, 0x17c, leaf) and the rw::math helpers the
// fetch shares; defined in this header in the original (its asserts name
// packages/skate/gameplay/Andale/dev/include/common/andale/fetch/extractpacked.h).
#ifndef ANDALE_FETCH_EXTRACTPACKED_H
#define ANDALE_FETCH_EXTRACTPACKED_H
#include "andale_types.h"

namespace Andale {
#ifdef RWP_GUEST
using namespace ::rwp;      // the recompiler's vector lowering
#endif

typedef RWP_PTR(SQTData) SqtPtr;
typedef RWP_PTR(const unsigned char) BytePtr;

RWP_ALIGN16 static const unsigned int sAndaleConst[][4] =
{
    { 0x00010203, 0x14151617, 0x00010203, 0x00010203 },   // 0 (a.x, b.y, a.x, a.x)
};
#if defined(_M_PPC)
extern const unsigned int sAndalePack[4];   // rw::math permute 0x820aa630 (another TU)
#define AN_PACK RWP_CONST(sAndalePack)
#else
#define AN_PACK RWP_CONST(sAndaleConst[0])
#endif

// rw::math Vector3(v.GetX(), v.GetY(), v.GetZ()): w ends up a copy of x
static __forceinline __vector4 AnV3(__vector4 v)
{
    return __vrlimi(__vperm(__vspltw(v, 0), __vspltw(v, 1), AN_PACK), __vspltw(v, 2), 2, 0);
}

// a x b in the permuted form rw::math vpu Cross uses
static __forceinline __vector4 AnCross(__vector4 a, __vector4 b)
{
    return __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63);
}

// v rotated by the unit quaternion q: v + 2 q.xyz x (q.w v + q.xyz x v)
static __forceinline __vector4 AnRotate(__vector4 q, __vector4 v, __vector4 two)
{
    __vector4 c = __vmaddfp(__vspltw(q, 3), v, AnCross(q, v));
    return __vmaddfp(two, AnCross(q, c), v);
}

// quaternion product a * b, as rw::math vpu evaluates it
static __forceinline __vector4 AnQuatMul(__vector4 a, __vector4 b)
{
    __vector4 aw = __vspltw(a, 3), bw = __vspltw(b, 3);
    __vector4 v = __vmaddfp(a, bw, __vmaddfp(b, aw, AnCross(a, b)));
    return __vrlimi(v, __vsubfp(__vmulfp(aw, bw), __vmsum3fp(a, b)), 1, 1);
}

// The trajectory bone relative to the previous frame's: translation = conj(prevQ) applied to
// (deltaT rotated by the bone's rotation + its translation - prevT), w = 1; rotation =
// deltaQ * rotation * conj(prevQ); channel animations then put the coefficient in trans.w.
void TrajectoryCalculation(SqtPtr trajBone, bool channelAnim, float channelCoef, __vector4 prevTrajQuat,
                           __vector4 prevTrajTrans, __vector4 deltaQ, __vector4 deltaT)
{
    const __vector4 two = __vcfsx(__vspltisw(2), 0);
    const __vector4 conjMask = __vrlimi(__vslw(__vspltisw(-1), __vspltisw(-1)), __vspltisw(0), 1, 1);
    const __vector4 prevConj = __vxor(prevTrajQuat, conjMask);
    __vector4 q = LOADV(trajBone->mQuat);
    __vector4 t = __vsubfp(__vaddfp(AnRotate(q, deltaT, two), AnV3(LOADV(trajBone->mTrans))), prevTrajTrans);
    t = AnRotate(prevConj, t, two);
    VLANE_SETW(t, 1.0f);
    STOREV(trajBone->mTrans, t);
    STOREV(trajBone->mQuat, AnQuatMul(AnQuatMul(deltaQ, LOADV(trajBone->mQuat)), prevConj));
    if (channelAnim)
    {
        __vector4 tw = LOADV(trajBone->mTrans);
        VLANE_SETW(tw, channelCoef);
        STOREV(trajBone->mTrans, tw);
    }
}

}
#endif
