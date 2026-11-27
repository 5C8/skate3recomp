// rw::physics "Dynamic Update" stage: Simulation::BatchIntegrator and RigidBody::DynamicUpdate.
// sk83_na_m: BatchIntegrator 0x82cde1a8 (0x44, matched), DynamicUpdate 0x82cddc00 (0x46c, leaf).
// Job: SimulationManager "Dynamic Update" entry 0x830ac440 -> BatchIntegrator.
//
// Per active body: integrate velocities (force/torque * dt), move by the velocity plus the
// solver's corrections (ReactionForces), step the quaternion q += 0.5 (w,0) q and renormalise
// (rsqrte + 2 Newton steps), rebuild the orientation rows (sqrt(2)*q trick), transform the
// inverse inertia to world space, apply drag, clamp linear/angular speed, update the sleep
// counter, clear the solver block and reset force (to gravity) and torque.
// Every fpu Vector3 slot carries another member in its w lane; stores keep it (vrlimi).
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"           // x86 speed benchmark (tools/equiv/bench_*.cpp)
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "simulation.h"

#ifndef RW_FMADDS
#define RW_FMADDS(a, b, c) ((a) * (b) + (c))
#endif

namespace rw { namespace physics {

RWP_ALIGN16 static const unsigned int sSqrt2[4] = { 0x3fb504f3, 0x3fb504f3, 0x3fb504f3, 0x3fb504f3 };  // 0x822ab7a0
// Three permute constants from another translation unit (rw::math), so each is loaded with its
// own lis/addi. The Xbox build only declares them, like the original; the other builds define them.
#if defined(_M_PPC)
extern const unsigned int sQuatPermUp[4];   // 0x820a9fb0
extern const unsigned int sQuatPermRi[4];   // 0x820a9fc0
extern const unsigned int sQuatPermAt[4];   // 0x820a9fd0
#else
RWP_ALIGN16 static const unsigned int sQuatPermUp[4] = { 0x00010203, 0x18191a1b, 0x00010203, 0x00010203 };
RWP_ALIGN16 static const unsigned int sQuatPermRi[4] = { 0x04050607, 0x10111213, 0x00010203, 0x00010203 };
RWP_ALIGN16 static const unsigned int sQuatPermAt[4] = { 0x08090a0b, 0x14151617, 0x00010203, 0x00010203 };
#endif

// fpu Vector3 slot = vector, keeping the slot's w (another member)
static __forceinline void SetKeepW(RWP_V3 &dst, __vector4 v)
{
    STOREV(dst, __vrlimi(v, LOADV(dst), 1, 0));
}

// rsqrte + two Newton steps
static __forceinline __vector4 RsqrtNR(__vector4 x, __vector4 half, __vector4 one)
{
    __vector4 e = __vrsqrtefp(x);
    __vector4 dpt7 = __vmulfp(e, half);
    e = __vmaddfp(dpt7, __vnmsubfp(x, __vmulfp(e, e), one), e);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(x, __vmulfp(e, e), one), e);
    return e;
}

void RigidBody::DynamicUpdate()
{
    // PERM_BEGIN
    RWP_PTR(ReactionForces) rf = mRF;

    const __vector4 one1 = __vspltisw(1);
    const __vector4 allOnes = __vspltisw(-1);
    const __vector4 zero = __vspltisw(0);
    const __vector4 dt = __vspltw(__lvlx(RWP_ADDR(mStasis->m_TimeStep), 0), 0);

    // integrate velocities, then positions with the solver's corrections
    __vector4 vel = __vmaddfp(LOADV(mForce), dt, LOADV(mVel));
    __vector4 omega = __vmaddfp(LOADV(mTorque), dt, LOADV(mOmega));
    __vector4 lin = __vmaddfp(vel, dt, rf->m_plin);
    __vector4 ang = __vmaddfp(omega, dt, rf->m_pang);
    const __vector4 freq = __vspltw(__lvlx(RWP_ADDR(mStasis->m_Frequency), 0), 0);
    const __vector4 half = __vcfsx(one1, 1);
    const __vector4 one = __vcfsx(one1, 0);
    const __vector4 nhalf = __vxor(half, __vslw(allOnes, allOnes));
    __vector4 com = LOADV(mCom);
    SetKeepW(mCom, __vaddfp(__vaddfp(com, rf->m_vlin), lin));

    // q += 0.5 * (w, 0) * q, normalised
    __vector4 q = mQuat;
    __vector4 w = __vaddfp(ang, rf->m_vang);
    __vector4 dot = __vmsum3fp(w, q);
    __vector4 c = __vnmsubfp(__vpermwi(w, 0x63), q, __vmulfp(w, __vpermwi(q, 0x63)));
    __vector4 dq = __vmulfp(__vmaddfp(w, __vspltw(q, 3), __vpermwi(c, 0x63)), half);
    dq = __vrlimi(dq, __vmulfp(dot, nhalf), 1, 0);
    q = __vaddfp(q, dq);
    q = __vmulfp(q, RsqrtNR(__vmsum4fp(q, q), half, one));
    mQuat = q;

    __vector4 s = __vmulfp(q, RWP_CONST(sSqrt2));
    __vector4 sq = __vnmsubfp(s, s, half);
    __vector4 p = __vmulfp(s, __vpermwi(s, 0x60));
    __vector4 dpt5 = __vpermwi(s, 0x84);
    __vector4 r = __vmulfp(__vspltw(s, 3), dpt5);
    // orientation rows from the quaternion
    __vector4 diag = __vaddfp(sq, __vpermwi(sq, 0x60));
    __vector4 dif = __vsubfp(p, r);
    __vector4 sum = __vaddfp(p, r);
    __vector4 ri = __vrlimi(__vperm(diag, sum, RWP_CONST(sQuatPermRi)), dif, 2, 0);
    __vector4 at = __vrlimi(__vperm(sum, dif, RWP_CONST(sQuatPermAt)), diag, 2, 2);
    __vector4 dpt3 = RWP_CONST(sQuatPermUp);
    __vector4 up = __vrlimi(__vperm(dif, diag, dpt3), sum, 2, 3);
    SetKeepW(mAt, at);
    SetKeepW(mUp, up);
    // world inverse inertia R * diag(InvTens) * R^T: diagonal and off-diagonal parts

    SetKeepW(mRi, ri);
    __vector4 it = mInertia->mInvTens;
    ri = LOADV(mRi);
    __vector4 at0 = __vmulfp(at, __vspltw(it, 2));
    __vector4 ri0 = __vmulfp(__vspltw(it, 0), ri);
    up = LOADV(mUp);
    __vector4 up0 = __vmulfp(__vspltw(it, 1), up);
    __vector4 splt = __vmaddfp(__vpermwi(ri, 0x97), __vpermwi(ri0, 0x9b),
                               __vmulfp(__vpermwi(up0, 0x9b), __vpermwi(up, 0x97)));
    __vector4 full = __vmaddfp(ri0, __vspltw(ri, 0), __vmulfp(__vspltw(up, 0), up0));
    full = __vmaddfp(__vspltw(at, 0), at0, full);
    SetKeepW(mIfull, full);
    splt = __vmaddfp(__vpermwi(at, 0x97), __vpermwi(at0, 0x9b), splt);
    SetKeepW(mIsplt, splt);

    // drag
    __vector4 angDamp = __vsubfp(freq, VSPLAT_F(mInertia->mAngularDrag));
    __vector4 linDamp = __vsubfp(freq, VSPLAT_F(mInertia->mLinearDrag));
    float maxOmega = mInertia->mMaxOmega;
    float maxW2 = maxOmega * maxOmega;
    angDamp = __vsel(zero, angDamp, __vcmpgtfp(angDamp, zero));
    linDamp = __vsel(zero, linDamp, __vcmpgtfp(linDamp, zero));
    __vector4 newW = __vmulfp(ang, angDamp);
    __vector4 newV = __vmulfp(lin, linDamp);

    // speed clamps
    float w2 = VLANE_F(__vmsum3fp(newW, newW), 0);
    float wE = w2;
    if (w2 > maxW2)
    {
        __vector4 k = VSPLAT_LX(maxW2 / w2);
        wE = maxW2;
        __vector4 root = __vmulfp(k, RsqrtNR(k, half, one));
        newW = __vmulfp(newW, __vsel(root, zero, __vcmpeqfp(zero, k)));
    }
    float maxVel = mInertia->mMaxVelocity;
    float maxV2 = maxVel * maxVel;
    float v2 = VLANE_F(__vmsum3fp(newV, newV), 0);
    if (v2 > maxV2)
    {
        __vector4 k = VSPLAT_LX(maxV2 / v2);
        v2 = maxV2;
        __vector4 root = __vmulfp(k, RsqrtNR(k, half, one));
        newV = __vmulfp(newV, __vsel(root, zero, __vcmpeqfp(zero, k)));
    }

    // kinetic energy and the sleep counter
    float spherical = mInertia->mSpherical;
    float invm = mInvm;
    float kine = RW_FMADDS(spherical * invm, wE, v2);
    if (kine >= (float)mStasis->m_MinEnergy)
        mCool = 0;
    else
    {
        if (!(kine > (float)mKine))
            mCool++;
        if (mCool > mStasis->m_CoolDown)
            mCool = mStasis->m_CoolDown;
    }
    mKine = kine;

    // store velocities, clear the solver block, reset the accumulators
    SetKeepW(mVel, newV);
    SetKeepW(mOmega, newW);
    const __vector4 z = __vspltisb(0);
    rf->m_vang = z;
    rf->m_pang = z;
    rf->m_vlin = z;
    rf->m_plin = z;
    SetKeepW(mForce, mStasis->m_Gravity);
    SetKeepW(mTorque, zero);
    // PERM_END
}

} }
