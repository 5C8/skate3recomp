// rw::physics::JointJacobian::Build (sk83_na_m 0x82cdf068, 0xe60). Hand-decompiled from the
// sk83 code with the Skate 2 PDB types/names and Skate 2's Hex-Rays structure as the guide.
//
// Builds the 0x180-byte joint jacobian the Horus iterations consume:
//   frames      qA = bodyA.q * skel.mQuatA, qB = bodyB.q * skel.mQuatB, qL = bodyB.q * skel.mQuatL
//   relative    J = d(qA*qB)/dq (Jacobian_RQD); the w lanes of its rows are the relative
//               quaternion, whose rotation rows (relRi/relUp/relAt) drive the angular limits
//   linear      lever arms ra/rb (body rows * mPosA/mPosB), rows L = rows(qL), position limits
//               from the predicted separation projected on L and clamped by mPprism/mVprism*dt
//   angular     rows T = rows(qB) (T_ri depends on the twist type), limits per swing/twist type
//   masses      effective linear (1/K) and angular (0.5/K) masses along the rows
// Note: m_nb is written and then overwritten by the LT_ri vector store, as in the original.
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "simulation.h"
#include "build_math.h"

#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
extern const __vector4 MAX_VECFLOAT;        // rw::math, initialised at startup
extern const __vector4 VPU_LARGE_VECFLOAT;  // rw::math (retail 0x830bde60, dev 0x833a8cf0), initialised at startup
#endif

namespace rw { namespace physics {

RWP_ALIGN16 static const unsigned int sJointConst[][4] =
{
    { 0x08090a0b, 0x00010203, 0x04050607, 0x0c0d0e0f },   //  0 zxyw
    { 0x04050607, 0x08090a0b, 0x00010203, 0x0c0d0e0f },   //  1 yzxw
    { 0x00010203, 0x14151617, 0x00010203, 0x00010203 },   //  2 (a.x, b.y, .., ..): row-projection pack
    { 0x00010203, 0x00010203, 0x0c0d0e0f, 0x1c1d1e1f },   //  3 (.., .., a.w, b.w)
    { 0x0c0d0e0f, 0x1c1d1e1f, 0x00010203, 0x00010203 },   //  4 (a.w, b.w, .., ..)
    { 0x00010203, 0x18191a1b, 0x00010203, 0x00010203 },   //  5 quaternion rows: up
    { 0x04050607, 0x10111213, 0x00010203, 0x00010203 },   //  6 quaternion rows: ri
    { 0x08090a0b, 0x14151617, 0x00010203, 0x00010203 },   //  7 quaternion rows: at
    { 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000 },   //  8 xyz mask
    { 0x04050607, 0x14151617, 0x18191a1b, 0x0c0d0e0f },   //  9 inverse inertia column 1
    { 0x08090a0b, 0x18191a1b, 0x10111213, 0x0c0d0e0f },   // 10 inverse inertia column 2
    { 0x00010203, 0x04050607, 0x10111213, 0x14151617 },   // 11 transpose column 0
    { 0x08090a0b, 0x0c0d0e0f, 0x14151617, 0x10111213 },   // 12 transpose column 1
    { 0x00010203, 0x04050607, 0x18191a1b, 0x10111213 },   // 13 transpose column 2
    { 0x00010203, 0x04050607, 0x08090a0b, 0x10111213 },   // 14 xyz of a, w = b.x
    { 0x00010203, 0x04050607, 0x08090a0b, 0x18191a1b },   // 15 xyz of a, w = b.z
    { 0x00010203, 0x04050607, 0x08090a0b, 0x14151617 },   // 16 xyz of a, w = b.y
    { 0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000 },   // 17 0.5
    { 0x3fb504f3, 0x3fb504f3, 0x3fb504f3, 0x3fb504f3 },   // 18 sqrt(2)
};
#if defined(_M_PPC)
// constants the original references through their own symbols (one lis/addi each)
extern const unsigned int sJointRelPack0[4];   // pool[23] 0x822b9ec0
extern const unsigned int sJointRelPack1[4];   // 0x8220a430
extern const unsigned int sJointSqrt2[4];      // 0x822ab7a0
#define JC_OWN3 RWP_CONST(sJointRelPack0)
#define JC_OWN4 RWP_CONST(sJointRelPack1)
#define JC_OWN18 RWP_CONST(sJointSqrt2)
#else
#define JC_OWN3 JC(3)
#define JC_OWN4 JC(4)
#define JC_OWN18 JC(18)
#endif
#define JC(i) RWP_CONST(sJointConst[i])

// a x b, with the operands' zxy/yzx permutes precomputed for a
#define JCROSS(aZXY, aYZX, b) __vnmsubfp(aZXY, __vperm(b, b, yzx), __vmaddfp(aYZX, __vperm(b, b, zxy), zero))
// M * v for an inverse inertia held as three columns
#define JMUL(c0, c1, c2, v) __vmaddfp(c2, __vspltw(v, 2), __vmaddfp(c1, __vspltw(v, 1), __vmaddfp(c0, __vspltw(v, 0), zero)))

// constants and negation as the original builds them: afresh at every use
// rw::math Negate: the sign mask is built in a local, in place, at every call
static __forceinline __vector4 JNegate(__vector4 v)
{
    __vector4 signMask;
    signMask = __vspltisw(-1);
    signMask = __vslw(signMask, signMask);
    return __vxor(v, signMask);
}
#define JNEG(x) JNegate(x)
#define JTWO __vcfsx(__vspltisw(2), 0)
#define JONE __vcfsx(__vspltisw(1), 0)

void JointJacobian::Build(const Joint &jtx, Simulation *stasis)
{
    RWP_PTR(JointFrames) skel = jtx.m_skel;
    RWP_PTR(JointLimits) limit = jtx.m_limit;
    RWP_PTR(RigidBody) bodyA = jtx.m_bodyA;
    RWP_PTR(RigidBody) bodyB = jtx.m_bodyB;

    // frames and the relative rotation
    const __vector4 dt = __vspltw(__lvlx(RWP_ADDR(stasis->m_TimeStep), 0), 0);
    __vector4 qA = JQuatMul(bodyA->mQuat, skel->mQuatA);
    __vector4 qB = JQuatMul(bodyB->mQuat, skel->mQuatB);
    Jacobian_RQD J;
    J.Create(qA, __vector4(qB));
    const __vector4 zero = __vspltisb(0);
    const __vector4 zxy = JC(0), yzx = JC(1), pack = JC(2), xyzMask = JC(8);
    const __vector4 one1 = __vspltisw(1);
    const __vector4 half = __vcfsx(one1, 1);
    const __vector4 sqrt2 = JC_OWN18;
    __vector4 Jwi = J.Jwi, Jwj = J.Jwj, Jwk = J.Jwk, Jwr = J.Jwr;
    JRows rel = JQuatRows(__vmulfp(sqrt2, __vsldoi(__vperm(Jwi, Jwj, JC_OWN3), __vperm(Jwk, Jwr, JC_OWN4), 8)), half);
    float relRiX = VLANE_F(rel.ri, 0);

    // lever arms, predicted motion of both attachment points over dt
    __vector4 ra = JBodyVec(bodyA, skel->mPosA);
    __vector4 rb = JBodyVec(bodyB, skel->mPosB);
    __vector4 raZXY = __vperm(ra, ra, zxy), raYZX = __vperm(ra, ra, yzx);
    __vector4 rbZXY = __vperm(rb, rb, zxy), rbYZX = __vperm(rb, rb, yzx);
    __vector4 torqueB = LOADV(bodyB->mTorque), omegaB = LOADV(bodyB->mOmega);
    __vector4 dispB = __vmulfp(__vmaddfp(__vmaddfp(rbZXY, __vperm(torqueB, torqueB, yzx),
                                                   __vnmsubfp(rbYZX, __vperm(torqueB, torqueB, zxy), LOADV(bodyB->mForce))),
                                         dt,
                                         __vmaddfp(rbZXY, __vperm(omegaB, omegaB, yzx),
                                                   __vnmsubfp(rbYZX, __vperm(omegaB, omegaB, zxy), LOADV(bodyB->mVel)))),
                               dt);
    __vector4 torqueA = LOADV(bodyA->mTorque), omegaA = LOADV(bodyA->mOmega);
    __vector4 dispA = __vmulfp(__vmaddfp(dt,
                                         __vmaddfp(raZXY, __vperm(torqueA, torqueA, yzx),
                                                   __vnmsubfp(raYZX, __vperm(torqueA, torqueA, zxy), LOADV(bodyA->mForce))),
                                         __vmaddfp(raZXY, __vperm(omegaA, omegaA, yzx),
                                                   __vnmsubfp(raYZX, __vperm(omegaA, omegaA, zxy), LOADV(bodyA->mVel)))),
                               dt);
    __vector4 comA = LOADV(bodyA->mCom), comB = LOADV(bodyB->mCom);
    __vector4 sep = __vsubfp(__vaddfp(__vaddfp(comB, rb), dispB), __vaddfp(__vaddfp(comA, ra), dispA));

    // linear rows L (the joint's limit frame) and the prismatic position limits
    JRows L = JQuatRows(__vmulfp(sqrt2, JQuatMul(bodyB->mQuat, skel->mQuatL)), half);
    __vector4 sepL = __vrlimi(__vperm(__vmsum3fp(sep, L.ri), __vmsum3fp(sep, L.up), pack), __vmsum3fp(sep, L.at), 2, 0);
    __vector4 prism = limit->mPprism;
    __vector4 hiPos = __vaddfp(sepL, prism);
    __vector4 dispL = __vsubfp(dispB, dispA);
    __vector4 moveL = __vrlimi(__vperm(__vmsum3fp(dispL, L.ri), __vmsum3fp(dispL, L.up), pack), __vmsum3fp(dispL, L.at), 2, 0);
    __vector4 vPrism = __vmulfp(limit->mVprism, dt);
    __vector4 loPos = __vsubfp(sepL, prism);
    __vector4 posMax = __vminfp(hiPos, __vmaxfp(__vaddfp(moveL, vPrism), loPos));
    __vector4 posMin = __vmaxfp(loPos, __vminfp(__vsubfp(moveL, vPrism), hiPos));

    // angular rows T = rows(qB); T_ri is replaced per twist type below
    JRows Tm = JQuatRows(__vmulfp(sqrt2, qB), half);
    __vector4 TRi = Tm.ri, TUp = Tm.up, TAt = Tm.at;
    __vector4 qARi = JQuatRows(__vmulfp(sqrt2, qA), half).ri;

    // angular limits (x: twist, y/z: swing), from 0 / -0
    // unlimited axes start at +/- the rw::math "huge" vector (0x833a8cf0, set at startup; it is
    // 0 in a raw xex image, which is why tests on an image dump must not hard-code it)
    const __vector4 huge = RWP_GLOBAL_VEC(0x830bde60, VPU_LARGE_VECFLOAT);
    __vector4 angMax = __vrlimi(__vperm(huge, huge, pack), huge, 2, 0);
    __vector4 angMin = JNEG(huge);
    int twistType = limit->mTwistf;
    switch ((int)limit->mSwingf)
    {
    case SWING_LOCKED:
    {
        const __vector4 two = JTWO;
        angMax = __vrlimi(angMax, __vmulfp(two, VSPLAT_F(VLANE_F(Jwj, 3))), 4, 0);
        angMin = __vrlimi(angMin, __vmulfp(two, VSPLAT_F(VLANE_F(Jwj, 3))), 4, 0);
        angMax = __vrlimi(angMax, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
        angMin = __vrlimi(angMin, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
        TUp = __vrlimi(__vperm(Jwj, Jwj, pack), Jwj, 2, 0);
        TAt = __vrlimi(__vperm(Jwk, Jwk, pack), Jwk, 2, 0);
        break;
    }
    case SWING_CONE:
    {
        // skip when cos(angle) is already past MAX_VECFLOAT (never, in practice)
        if (VALL_GE(__vspltw(rel.ri, 0), RWP_GLOBAL_VEC(0x830bde50, MAX_VECFLOAT)))
            break;
        __vector4 c = VSPLAT_LX((float)limit->mSwingc - relRiX);
        __vector4 u = __vspltw(rel.up, 0), a = __vspltw(rel.at, 0);
        __vector4 len2 = __vmaddfp(u, u, __vmulfp(a, a));
        __vector4 e = __vrsqrtefp(len2);
        e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), JONE), e);
        __vector4 len = __vsel(__vmulfp(len2, __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), JONE), e)),
                               zero, __vcmpeqfp(zero, len2));
        __vector4 inv = __vrefp(len);
        inv = __vmaddfp(inv, __vnmsubfp(inv, len, JONE), inv);
        inv = __vmaddfp(inv, __vnmsubfp(inv, len, JONE), inv);
        __vector4 invOne = __vmulfp(inv, JONE);
        angMin = __vrlimi(angMin, __vmulfp(c, invOne), 4, 0);
        // the swing axes: normalised qA.ri x TRi, and the third axis
        TUp = __vmulfp(JCross(qARi, TRi), invOne);
        TAt = JCross(TUp, qARi);
        break;
    }
    case SWING_HINGE:
    {
        if (twistType == 0)
        {
            const __vector4 two = JTWO;
            angMax = __vrlimi(angMax, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
            angMin = __vrlimi(angMin, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
            TAt = __vrlimi(__vperm(Jwk, Jwk, pack), Jwk, 2, 0);
        }
        else
        {
            angMax = __vrlimi(angMax, JNEG(__vspltw(rel.up, 0)), 2, 0);
            angMin = __vrlimi(angMin, JNEG(__vspltw(rel.up, 0)), 2, 0);
            TAt = JCross(qARi, TUp);
        }
        // swing limit about the hinge: (mSwingc - relRi.x) / relAt.x, to max or min by sign
        __vector4 d = __vspltw(rel.at, 0);
        if (VALL_EQ(d, zero))
            break;
        __vector4 inv = __vrefp(d);
        __vector4 c = VSPLAT_LX((float)limit->mSwingc - relRiX);
        bool neg = VALL_GT(zero, d);
        inv = __vmaddfp(inv, __vnmsubfp(inv, d, JONE), inv);
        inv = __vmaddfp(inv, __vnmsubfp(inv, d, JONE), inv);
        __vector4 lim = __vmulfp(inv, c);
        if (neg) angMax = __vrlimi(angMax, lim, 4, 0);
        else angMin = __vrlimi(angMin, lim, 4, 0);
        break;
    }
    case SWING_AXLE:
    {
        if (twistType == 0)
        {
            const __vector4 two = JTWO;
            angMax = __vrlimi(angMax, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
            angMin = __vrlimi(angMin, __vmulfp(two, VSPLAT_F(VLANE_F(Jwk, 3))), 2, 0);
            TAt = __vrlimi(__vperm(Jwk, Jwk, pack), Jwk, 2, 0);
        }
        else
        {
            angMax = __vrlimi(angMax, JNEG(__vspltw(rel.up, 0)), 2, 0);
            angMin = __vrlimi(angMin, JNEG(__vspltw(rel.up, 0)), 2, 0);
            TAt = JCross(qARi, TUp);
        }
        break;
    }
    default:
        break;
    }
    switch (twistType)
    {
    case TWIST_LOCKED:
    {
        const __vector4 two = JTWO;
        TRi = __vrlimi(__vperm(Jwi, Jwi, pack), Jwi, 2, 0);
        angMax = __vrlimi(angMax, __vmulfp(two, VSPLAT_F(VLANE_F(Jwi, 3))), 8, 0);
        angMin = __vrlimi(angMin, __vmulfp(two, VSPLAT_F(VLANE_F(Jwi, 3))), 8, 0);
        break;
    }
    case TWIST_ARC:
    {
        TRi = qARi;
        // twist limit: ((1 + relRi.x) * mTwistc - (relUp.y + relAt.z)) / (relUp.z - relAt.y)
        __vector4 d = __vsubfp(__vspltw(rel.up, 2), __vspltw(rel.at, 1));
        if (VALL_EQ(d, zero))
            break;
        __vector4 inv = __vrefp(d);
        __vector4 tw = VSPLAT_F((float)limit->mTwistc);
        bool neg = VALL_GT(zero, d);
        __vector4 num = __vsubfp(__vmulfp(__vaddfp(JONE, __vspltw(rel.ri, 0)), tw),
                                 __vaddfp(__vspltw(rel.up, 1), __vspltw(rel.at, 2)));
        inv = __vmaddfp(inv, __vnmsubfp(inv, d, JONE), inv);
        inv = __vmaddfp(inv, __vnmsubfp(inv, d, JONE), inv);
        __vector4 lim = __vmulfp(inv, num);
        if (neg) angMax = __vrlimi(angMax, lim, 8, 0);
        else angMin = __vrlimi(angMin, lim, 8, 0);
        break;
    }
    default:        // TWIST_FREE: T_ri stays rows(qB).ri
        break;
    }

    // angular velocity limits: relative rotation over dt along T, clamped by mVtwist/mVswing*dt
    __vector4 turn = __vmulfp(__vsubfp(__vmaddfp(torqueB, dt, __vsubfp(omegaB, omegaA)), __vmulfp(torqueA, dt)), dt);
    __vector4 turnT = __vrlimi(__vperm(__vmsum3fp(TRi, turn), __vmsum3fp(TUp, turn), pack), __vmsum3fp(TAt, turn), 2, 0);
    __vector4 lo = __vaddfp(angMin, turnT), hi = __vaddfp(angMax, turnT);
    float vTwist = limit->mVtwist, vSwing = limit->mVswing;
    __vector4 loV = __vminfp(__vsubfp(turnT, __vmulfp(RWP_VEC3F(vTwist, vSwing, vSwing), dt)), hi);
    __vector4 hiV = __vmaxfp(__vmaddfp(RWP_VEC3F(vTwist, vSwing, vSwing), dt, turnT), lo);

    __dcbz128(0, this);
    __dcbz128(0x80, this);
    __dcbz128(0x100, this);
    __dcbt(0, &*bodyA);
    __dcbt(0, &*bodyB);
    // inverse inertia of each body as three columns (zero unless the body is dynamic)
    const __vector4 four = __vspltisw(4);
    __vector4 isA = LOADV(bodyA->mIsplt), isB = LOADV(bodyB->mIsplt);
    __vector4 dynA = __vcmpequw(__vand(__vspltw(isA, 3), four), four);
    __vector4 dynB = __vcmpequw(__vand(__vspltw(isB, 3), four), four);
    __vector4 ia0 = __vsel(zero, LOADV(bodyA->mIfull), dynA), iaOff = __vsel(zero, isA, dynA);
    __vector4 ib0 = __vsel(zero, LOADV(bodyB->mIfull), dynB), ibOff = __vsel(zero, isB, dynB);
    __vector4 ia1 = __vperm(ia0, iaOff, JC(9)), ia2 = __vperm(ia0, iaOff, JC(10));
    __vector4 ib1 = __vperm(ib0, ibOff, JC(9)), ib2 = __vperm(ib0, ibOff, JC(10));

    // linear effective mass along L
    __vector4 bRi = JCROSS(rbZXY, rbYZX, L.ri), aRi = JCROSS(raZXY, raYZX, L.ri);
    __vector4 kRi = __vmaddfp(bRi, JMUL(ib0, ib1, ib2, bRi), __vmaddfp(aRi, JMUL(ia0, ia1, ia2, aRi), zero));
    __vector4 bUp = JCROSS(rbZXY, rbYZX, L.up), aUp = JCROSS(raZXY, raYZX, L.up);
    __vector4 kUp = __vmaddfp(bUp, JMUL(ib0, ib1, ib2, bUp), __vmaddfp(aUp, JMUL(ia0, ia1, ia2, aUp), zero));
    __vector4 bAt = JCROSS(rbZXY, rbYZX, L.at), aAt = JCROSS(raZXY, raYZX, L.at);
    __vector4 kAt = __vmaddfp(bAt, JMUL(ib0, ib1, ib2, bAt), __vmaddfp(aAt, JMUL(ia0, ia1, ia2, aAt), zero));
    __vector4 kh = __vmrghw(kRi, kUp);
    __vector4 linMass = __vrefp(__vaddfp(__vaddfp(__vperm(__vmrglw(kRi, kUp), kAt, JC(13)), __vperm(kh, kAt, JC(12))),
                                         __vaddfp(__vperm(kh, kAt, JC(11)), __vaddfp(__vspltw(ia0, 3), __vspltw(ib0, 3)))));

    // angular effective mass along T (times 0.5)
    __vector4 jRi = __vmaddfp(__vaddfp(JMUL(ia0, ia1, ia2, TRi), JMUL(ib0, ib1, ib2, TRi)), TRi, zero);
    __vector4 jUp = __vmaddfp(__vaddfp(JMUL(ia0, ia1, ia2, TUp), JMUL(ib0, ib1, ib2, TUp)), TUp, zero);
    __vector4 jAt = __vmaddfp(__vaddfp(JMUL(ia0, ia1, ia2, TAt), JMUL(ib0, ib1, ib2, TAt)), TAt, zero);
    __vector4 jh = __vmrghw(jRi, jUp);
    __vector4 angMass = __vmaddfp(__vrefp(__vaddfp(__vperm(__vmrglw(jRi, jUp), jAt, JC(13)),
                                                   __vaddfp(__vperm(jh, jAt, JC(11)), __vperm(jh, jAt, JC(12))))),
                                  JC(17), zero);

    __vector4 angLo = __vmaddfp(__vmaxfp(lo, loV), angMass, zero);
    __vector4 angHi = __vmaddfp(__vminfp(hi, hiV), angMass, zero);

    // the jacobian
    STOREV(m_Pos_Imp, zero);
    STOREV(m_Ang_Imp, zero);
    STOREV(m_ra, __vsel(comA, ra, xyzMask));
    STOREV(m_rb, __vsel(comB, rb, xyzMask));
    STOREV(m_InvInA_ri, ia0);
    STOREV(m_InvInB_ri, ib0);
    STOREV(m_InvInB_up, ib1);
    STOREV(m_InvInA_up, ia1);
    STOREV(m_L_ri, L.ri);
    STOREV(m_L_up, L.up);
    STOREV(m_L_at, L.at);
    STOREV(m_T_ri, TRi);
    STOREV(m_T_up, TUp);
    STOREV(m_T_at, TAt);
    STOREV(m_InvInA_at, ia2);
    STOREV(m_InvInB_at, ib2);
    m_joint = RWP_SELF_PTR(Joint, jtx);
    m_nb = jtx.m_spy;
    __vector4 lh = __vmrghw(L.ri, L.up), th = __vmrghw(TRi, TUp);
    STOREV(m_LT_ri, __vmaddfp(__vperm(lh, L.at, JC(11)), linMass, zero));
    STOREV(m_TT_ri, __vmaddfp(__vperm(th, TAt, JC(11)), angMass, zero));
    STOREV(m_LT_up, __vperm(__vmaddfp(__vperm(lh, L.at, JC(12)), linMass, zero), angLo, JC(14)));
    STOREV(m_LT_at, __vperm(__vmaddfp(__vperm(__vmrglw(L.ri, L.up), L.at, JC(13)), linMass, zero), angHi, JC(14)));
    STOREV(m_Pos_Max, __vperm(__vmaddfp(posMin, linMass, zero), angLo, JC(15)));
    STOREV(m_Pos_Min, __vperm(__vmaddfp(posMax, linMass, zero), angHi, JC(15)));
    STOREV(m_TT_up, __vperm(__vmaddfp(__vperm(th, TAt, JC(12)), angMass, zero), angLo, JC(16)));
    STOREV(m_TT_at, __vperm(__vmaddfp(__vperm(__vmrglw(TRi, TUp), TAt, JC(13)), angMass, zero), angHi, JC(16)));
}

} }
