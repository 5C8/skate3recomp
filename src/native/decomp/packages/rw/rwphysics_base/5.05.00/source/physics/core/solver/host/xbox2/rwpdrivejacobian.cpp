// rw::physics::DriveJacobian::Build (sk83_na_m 0x82cde360, 0xd08). Hand-decompiled from the
// sk83 code with the Skate 2 PDB types/names (DriveJacobian, DriveDynamics::Params) and Skate 2's
// Hex-Rays structure as the guide.
//
// A drive pulls bodyB's frame towards bodyA's, per axis group, as a damped spring:
//   frames      qA = bodyA.q * skel.mQuatA, qB = bodyB.q * skel.mQuatB; J = Jacobian_RQD(qA, qB),
//               whose w lanes (Jwi.w, Jwj.w, Jwk.w) are the vector part of the relative rotation
//   linear      rows L = rows(qB); target = separation * kp + relative velocity*dt * kd
//               + relative force*dt^2 * ka, along L (mLinear)
//   angular     rows T: the Jwi/Jwj/Jwk rows, renormalised by 1/sqrt(1 - w^2) (or rows(qB) when
//               the rotation is 180 degrees about x); error = 2*w/sqrt(1 - w^2) (mAngular)
//   gains       DRIVE_SPRING: r = 1/(1 + dt^2*mSpring + dt*mDamping),
//               kp = r*dt^2*mSpring, kd = r*dt*mDamping, ka = r*(dt^2*mSpring + dt*mDamping)
//               otherwise: kp = 1/(1 + dt*mDamping), kd = ka = 1;
//               DRIVE_STRENGTH also clamps the error length to dt*mSpring
//   masses      effective linear and angular masses along L and T; limits dt^2*mStrength/K
// The w lanes carry the per-row limits (m_Pos_Lim_*, m_Ang_Lim_*) and the gains (m_lindyn,
// m_angdyn) the Horus drive row reads.
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
extern const __vector4 VPU_SPLAT_X_PERMUTE;  // rw::math permute (retail 0x830bdb40, dev 0x833a8880), initialised at startup
#endif

namespace rw { namespace physics {

RWP_ALIGN16 static const unsigned int sDriveConst[][4] =
{
    { 0x08090a0b, 0x00010203, 0x04050607, 0x0c0d0e0f },   // 0 zxyw
    { 0x04050607, 0x08090a0b, 0x00010203, 0x0c0d0e0f },   // 1 yzxw
    { 0x00010203, 0x14151617, 0x00010203, 0x00010203 },   // 2 (a.x, b.y, .., ..): row-projection pack
    { 0x04050607, 0x14151617, 0x18191a1b, 0x0c0d0e0f },   // 3 inverse inertia column 1
    { 0x08090a0b, 0x18191a1b, 0x10111213, 0x0c0d0e0f },   // 4 inverse inertia column 2
    { 0x00010203, 0x10111213, 0x00010203, 0x00010203 },   // 5 (a.x, b.x): transpose column 0
    { 0x04050607, 0x14151617, 0x00010203, 0x00010203 },   // 6 (a.y, b.y): transpose column 1
    { 0x08090a0b, 0x18191a1b, 0x00010203, 0x00010203 },   // 7 (a.z, b.z): transpose column 2
    { 0x3fb504f3, 0x3fb504f3, 0x3fb504f3, 0x3fb504f3 },   // 8 sqrt(2)
};
#if defined(_M_PPC)
// how the original references them: zxy/yzx and the inverse-inertia columns are rows of the
// rwpcore constant pool (0x822b9d50, one base); the rest each through its own symbol
extern const unsigned int sRwpPool[27][4];
extern const unsigned int sDrvPack[4];    // 0x820aa630 (rw::math)
extern const unsigned int sDrvTcol0[4];   // 0x820dfa40 (rw::math)
extern const unsigned int sDrvTcol1[4];   // pool[24] 0x822b9ed0
extern const unsigned int sDrvTcol2[4];   // pool[25] 0x822b9ee0
extern const unsigned int sDrvSqrt2[4];   // 0x822ab7a0
#define DC_0 RWP_CONST(sRwpPool[2])
#define DC_1 RWP_CONST(sRwpPool[3])
#define DC_2 RWP_CONST(sDrvPack)
#define DC_3 RWP_CONST(sRwpPool[6])
#define DC_4 RWP_CONST(sRwpPool[5])
#define DC_5 RWP_CONST(sDrvTcol0)
#define DC_6 RWP_CONST(sDrvTcol1)
#define DC_7 RWP_CONST(sDrvTcol2)
#define DC_8 RWP_CONST(sDrvSqrt2)
#define DC(i) DC_##i
#else
#define DC(i) RWP_CONST(sDriveConst[i])
#endif

// 1/d, two Newton steps (rw::math vpu Reciprocal)
static __forceinline __vector4 DRecip(__vector4 d, __vector4 one)
{
    __vector4 x = __vrefp(d);
    x = __vmaddfp(x, __vnmsubfp(x, d, one), x);
    return __vmaddfp(x, __vnmsubfp(x, d, one), x);
}

// 1/sqrt(d), two Newton steps (rw::math vpu InvSqrt)
static __forceinline __vector4 DInvSqrt(__vector4 d, __vector4 half, __vector4 one)
{
    __vector4 e = __vrsqrtefp(d);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
    return __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
}

// lin + r x ang (velocity or force of a point at lever r), r's permutes precomputed
static __forceinline __vector4 DPointRate(__vector4 rZXY, __vector4 rYZX, __vector4 lin, __vector4 ang,
                                          __vector4 zxy, __vector4 yzx)
{
    return __vmaddfp(rZXY, __vperm(ang, ang, yzx), __vnmsubfp(rYZX, __vperm(ang, ang, zxy), lin));
}

// M * v for an inverse inertia held as three columns
static __forceinline __vector4 DInertiaMul(__vector4 c0, __vector4 c1, __vector4 c2, __vector4 v)
{
    return __vmaddfp(c2, __vspltw(v, 2), __vmaddfp(c1, __vspltw(v, 1), __vmulfp(c0, __vspltw(v, 0))));
}

// (a.x, b.x, c.x) style projection pack of three dot products
static __forceinline __vector4 DPack3(__vector4 x, __vector4 y, __vector4 z, __vector4 pack)
{
    return __vrlimi(__vperm(x, y, pack), z, 2, 0);
}

// row of a Jacobian_RQD vector with its w moved out: (x, y, z, x)
static __forceinline __vector4 DRow(__vector4 j, __vector4 pack)
{
    return __vrlimi(__vperm(j, j, pack), j, 2, 0);
}

// one body's contribution to the effective masses along the linear rows (lever r) and the
// angular rows; returns the body's inverse inertia columns (column 0's w = mInvm)
struct DBodyMass { __vector4 c0, c1, c2, lin, ang; };
static __forceinline DBodyMass DBodyK(RWP_PTR(RigidBody) b, __vector4 r, const JRows &L,
                                      __vector4 Tri, __vector4 Tup, __vector4 Tat,
                                      __vector4 col1, __vector4 col2, __vector4 pack)
{
    DBodyMass m;
    __vector4 I0 = LOADV(b->mIfull), Is = LOADV(b->mIsplt);
    m.c1 = __vperm(I0, Is, col1);
    m.c2 = __vperm(I0, Is, col2);
    __vector4 invm = __vspltw(__lvlx(RWP_ADDR(b->mInvm), 0), 0);
    m.c0 = __vrlimi(I0, invm, 1, 0);
    __vector4 rRi = JCross(r, L.ri), rUp = JCross(r, L.up), rAt = JCross(r, L.at);
    m.ang = DPack3(__vmsum3fp(DInertiaMul(I0, m.c1, m.c2, Tri), Tri),
                   __vmsum3fp(DInertiaMul(I0, m.c1, m.c2, Tup), Tup),
                   __vmsum3fp(DInertiaMul(I0, m.c1, m.c2, Tat), Tat), pack);
    m.lin = __vaddfp(DPack3(__vmsum3fp(DInertiaMul(I0, m.c1, m.c2, rRi), rRi),
                            __vmsum3fp(DInertiaMul(I0, m.c1, m.c2, rUp), rUp),
                            __vmsum3fp(DInertiaMul(I0, m.c1, m.c2, rAt), rAt), pack),
                     invm);
    return m;
}

void DriveJacobian::Build(const Drive &drv, Simulation *stasis)
{
    // the jacobian's three cache lines are zeroed first
    __dcbz128(0, this);
    __dcbz128(0x80, this);
    __dcbz128(0x100, this);
    RWP_PTR(DriveFrames) skel = drv.m_skel;
    RWP_PTR(DriveDynamics) crtl = drv.m_crtl;
    RWP_PTR(RigidBody) bodyA = drv.m_bodyA;
    RWP_PTR(RigidBody) bodyB = drv.m_bodyB;
    const __vector4 zero = __vspltisw(0);
    const __vector4 one1 = __vspltisw(1);
    const __vector4 one = __vcfsx(one1, 0), two = __vaddfp(one, one);

    const __vector4 dt = __vspltw(__lvlx(RWP_ADDR(stasis->m_TimeStep), 0), 0);
    const __vector4 linDamp = __vmulfp(dt, VSPLAT_F((float)crtl->mLinear.mDamping));
    const __vector4 angDamp = __vmulfp(dt, VSPLAT_F((float)crtl->mAngular.mDamping));
    const __vector4 linLimit = __vmulfp(__vmulfp(dt, VSPLAT_F((float)crtl->mLinear.mStrength)), dt);
    const __vector4 angLimit = __vmulfp(__vmulfp(dt, VSPLAT_F((float)crtl->mAngular.mStrength)), dt);

    // frames and the relative rotation
    __vector4 qA = JQuatMul(bodyA->mQuat, skel->mQuatA);
    __vector4 qB = JQuatMul(bodyB->mQuat, skel->mQuatB);
    Jacobian_RQD J;
    J.Create(qA, qB);
    __vector4 Jwi = J.Jwi, Jwj = J.Jwj, Jwk = J.Jwk;
    const __vector4 half = __vcfsx(one1, 1);

    // lever arms, linear rows L = rows(qB)
    __vector4 ra = JBodyVec(bodyA, skel->mPosA);
    JRows L = JQuatRows(__vmulfp(DC(8), qB), half);
    __vector4 rb = JBodyVec(bodyB, skel->mPosB);
    const __vector4 zxy = DC(0), yzx = DC(1);
    __vector4 raZXY = __vperm(ra, ra, zxy), raYZX = __vperm(ra, ra, yzx);
    __vector4 rbZXY = __vperm(rb, rb, zxy), rbYZX = __vperm(rb, rb, yzx);

    // motion of both attachment points: velocity*dt and force*dt^2
    __vector4 omegaA = LOADV(bodyA->mOmega), torqueA = LOADV(bodyA->mTorque);
    __vector4 omegaB = LOADV(bodyB->mOmega), torqueB = LOADV(bodyB->mTorque);
    __vector4 pA = __vaddfp(LOADV(bodyA->mCom), ra);
    __vector4 pB = __vaddfp(LOADV(bodyB->mCom), rb);
    __vector4 accA = __vmulfp(__vmulfp(DPointRate(raZXY, raYZX, LOADV(bodyA->mForce), torqueA, zxy, yzx), dt), dt);
    __vector4 velA = __vmulfp(DPointRate(raZXY, raYZX, LOADV(bodyA->mVel), omegaA, zxy, yzx), dt);
    __vector4 accB = __vmulfp(__vmulfp(DPointRate(rbZXY, rbYZX, LOADV(bodyB->mForce), torqueB, zxy, yzx), dt), dt);
    __vector4 velB = __vmulfp(DPointRate(rbZXY, rbYZX, LOADV(bodyB->mVel), omegaB, zxy, yzx), dt);

    // linear gains
    int linType = crtl->mLinear.mType;
    __vector4 linKp, linKd, linKa;
    if (linType == DRIVE_SPRING)
    {
        __vector4 k = __vmulfp(__vmulfp(dt, VSPLAT_F((float)crtl->mLinear.mSpring)), dt);
        __vector4 kd = __vaddfp(k, linDamp);
        linKp = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), linDamp), one), k);
        linKd = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), linDamp), one), linDamp);
        linKa = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), linDamp), one), kd);
    }
    else
    {
        linKp = __vmulfp(DRecip(__vaddfp(one, linDamp), one), one);
        linKd = one;
        linKa = one;
    }
    __vector4 sep = __vsubfp(pB, pA);
    if (linType == DRIVE_STRENGTH)
    {
        // clamp the separation to dt*mSpring
        __vector4 len2 = __vmsum3fp(sep, sep);
        __vector4 lim = __vmulfp(dt, VSPLAT_F((float)crtl->mLinear.mSpring));
        __vector4 l2 = VSPLAT_F(VLANE_F(len2, 0));
        __vector4 scale = __vmulfp(lim, DInvSqrt(l2, half, one));
        sep = __vmulfp(sep, __vsel(one, scale, __vcmpgtfp(l2, __vmulfp(lim, lim))));
    }

    // angular rows T: the relative-rotation rows over sqrt(1 - w^2); rows(qB) at 180 degrees
    __vector4 w = __vspltisw(0);
    VLANE_SET4(w, VLANE_F(Jwi, 3), VLANE_F(Jwj, 3), VLANE_F(Jwk, 3), 0.0f);
    __vector4 dVel = __vsubfp(velB, velA);
    __vector4 dAcc = __vsubfp(accB, accA);
    __vector4 dOmega = __vmulfp(__vsubfp(omegaB, omegaA), dt);
    __vector4 dTorque = __vmulfp(__vmulfp(__vsubfp(torqueB, torqueA), dt), dt);
    __vector4 w2 = __vminfp(__vmaxfp(__vmulfp(w, w), zero), one);
    __vector4 flip = __vperm(__vcmpeqfp(w2, one), __vcmpeqfp(w2, one), RWP_GLOBAL_VEC(0x830bdb40, VPU_SPLAT_X_PERMUTE));
    __vector4 useL = __vcmpequw(flip, zero);
    useL = __vnor(useL, useL);
    __vector4 linTarget = __vmaddfp(dAcc, linKa, __vmaddfp(sep, linKp, __vmulfp(dVel, linKd)));
    __vector4 linErr = DPack3(__vmsum3fp(linTarget, L.ri), __vmsum3fp(linTarget, L.up), __vmsum3fp(linTarget, L.at), DC(2));
    __vector4 s = DInvSqrt(__vsel(__vsubfp(one, w2), one, useL), half, one);
    __vector4 Tri = __vmulfp(__vsel(DRow(Jwi, DC(2)), L.ri, useL), __vspltw(s, 0));
    __vector4 Tup = __vmulfp(__vsel(DRow(Jwj, DC(2)), L.up, useL), __vspltw(s, 1));
    __vector4 Tat = __vmulfp(__vsel(DRow(Jwk, DC(2)), L.at, useL), __vspltw(s, 2));
    __vector4 angErr = __vmulfp(__vmulfp(w, s), two);
    __vector4 angVel = DPack3(__vmsum3fp(Tri, dOmega), __vmsum3fp(Tup, dOmega), __vmsum3fp(Tat, dOmega), DC(2));
    __vector4 angAcc = DPack3(__vmsum3fp(Tri, dTorque), __vmsum3fp(Tup, dTorque), __vmsum3fp(Tat, dTorque), DC(2));

    // angular gains
    int angType = crtl->mAngular.mType;
    __vector4 angKp, angKd, angKa;
    if (angType == DRIVE_SPRING)
    {
        __vector4 k = __vmulfp(__vmulfp(dt, VSPLAT_F((float)crtl->mAngular.mSpring)), dt);
        __vector4 kd = __vaddfp(k, angDamp);
        angKp = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), angDamp), one), k);
        angKd = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), angDamp), one), angDamp);
        angKa = __vmulfp(DRecip(__vaddfp(__vaddfp(one, k), angDamp), one), kd);
    }
    else
    {
        angKp = __vmulfp(DRecip(__vaddfp(one, angDamp), one), one);
        angKd = one;
        angKa = one;
    }
    if (angType == DRIVE_STRENGTH)
    {
        // clamp the rotation error to dt*mSpring
        __vector4 len2 = __vmsum3fp(angErr, angErr);
        __vector4 lim = __vmulfp(dt, VSPLAT_F((float)crtl->mAngular.mSpring));
        __vector4 scale = __vmulfp(lim, DInvSqrt(len2, half, one));
        angErr = __vmulfp(angErr, __vsel(one, scale, __vcmpgtfp(len2, __vmulfp(lim, lim))));
    }
    __vector4 angTarget = __vmaddfp(angAcc, angKa, __vmaddfp(angErr, angKp, __vmulfp(angVel, angKd)));

    STOREV(m_ra, ra);
    m_ia = bodyA->mRF;
    STOREV(m_rb, rb);
    m_ib = bodyB->mRF;
    STOREV(m_L_ri, L.ri);
    m_drive = RWP_SELF_PTR(Drive, drv);

    // effective masses (only active bodies contribute)
    __vector4 kLin = zero, kAng = zero;
    __vector4 iA0 = zero, iA1 = zero, iA2 = zero, iB0 = zero, iB1 = zero, iB2 = zero;
    if (bodyA->mState & ACTIVE_BODY)
    {
        DBodyMass m = DBodyK(bodyA, ra, L, Tri, Tup, Tat, DC(3), DC(4), DC(2));
        iA0 = m.c0; iA1 = m.c1; iA2 = m.c2;
        kLin = m.lin;
        kAng = m.ang;
    }
    if (bodyB->mState & ACTIVE_BODY)
    {
        DBodyMass m = DBodyK(bodyB, rb, L, Tri, Tup, Tat, DC(3), DC(4), DC(2));
        iB0 = m.c0; iB1 = m.c1; iB2 = m.c2;
        kAng = __vaddfp(kAng, m.ang);
        kLin = __vaddfp(kLin, m.lin);
    }
    __vector4 invLin = __vmulfp(DRecip(kLin, one), one);
    __vector4 invAng = __vmulfp(DRecip(kAng, one), one);

    // the jacobian
    STOREV(m_L_up, L.up);
    STOREV(m_L_at, L.at);
    STOREV(m_InvInA_ri, iA0);
    STOREV(m_InvInA_up, iA1);
    STOREV(m_InvInA_at, iA2);
    STOREV(m_InvInB_ri, iB0);
    STOREV(m_InvInB_up, iB1);
    STOREV(m_InvInB_at, iB2);
    STOREV(m_T_ri, Tri);
    STOREV(m_T_up, Tup);
    STOREV(m_T_at, Tat);
    STOREV(m_Pos_Off, __vrlimi(__vmulfp(invLin, linErr), __vmulfp(__vspltw(invLin, 2), linLimit), 1, 0));
    STOREV(m_Ang_Off, __vrlimi(__vmulfp(invAng, angTarget), __vmulfp(__vspltw(invAng, 2), angLimit), 1, 0));
    STOREV(m_LT_ri, __vmulfp(__vrlimi(__vperm(L.ri, L.up, DC(5)), L.at, 2, 2), invLin));
    m_nb = drv.m_spy;
    STOREV(m_LT_up, __vrlimi(__vmulfp(__vrlimi(__vperm(L.ri, L.up, DC(6)), L.at, 2, 3), invLin),
                             __vmulfp(__vspltw(invLin, 0), linLimit), 1, 0));
    STOREV(m_LT_at, __vrlimi(__vmulfp(__vrlimi(__vperm(L.ri, L.up, DC(7)), L.at, 2, 0), invLin),
                             __vmulfp(__vspltw(invAng, 0), angLimit), 1, 0));
    STOREV(m_TT_ri, __vmulfp(__vrlimi(__vperm(Tri, Tup, DC(5)), Tat, 2, 2), invAng));
    STOREV(m_TT_up, __vrlimi(__vmulfp(__vrlimi(__vperm(Tri, Tup, DC(6)), Tat, 2, 3), invAng),
                             __vmulfp(__vspltw(invLin, 1), linLimit), 1, 0));
    STOREV(m_TT_at, __vrlimi(__vmulfp(__vrlimi(__vperm(Tri, Tup, DC(7)), Tat, 2, 0), invAng),
                             __vmulfp(__vspltw(invAng, 1), angLimit), 1, 0));
    STOREV(m_Pos_Imp, __vrlimi(zero, linKa, 1, 0));     // w: m_lindyn
    STOREV(m_Ang_Imp, __vrlimi(zero, angKa, 1, 0));     // w: m_angdyn
}

} }
