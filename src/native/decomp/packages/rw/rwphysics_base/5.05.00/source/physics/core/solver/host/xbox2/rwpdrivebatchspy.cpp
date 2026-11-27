// rw::physics "Drive Spies" stage: report the solved impulses of flagged constraints, compacting
// the records in place over the jacobian array (every input is read before its record is
// written). sk83_na_m SpyDriveJacobians 0x82cdd0a8 (0x2d4).
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "rwpspy_types.h"
#include <math.h>

namespace rw { namespace physics {

// quaternion product a * b, as rw::math vpu evaluates it
static __forceinline __vector4 SpyQuatMul(__vector4 a, __vector4 b)
{
    __vector4 aw = __vspltw(a, 3), bw = __vspltw(b, 3);
    __vector4 v = __vmaddfp(a, bw, __vmaddfp(b, aw,
                    __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63)));
    return __vrlimi(v, __vsubfp(__vmulfp(aw, bw), __vmsum3fp(a, b)), 1, 1);
}

// body-space point -> world: Com + Ri*x + Up*y + At*z
static __forceinline __vector4 SpyBodyPoint(RWP_PTR(RigidBody) body, __vector4 p)
{
    return __vmaddfp(LOADV(body->mAt), __vspltw(p, 2), __vmaddfp(LOADV(body->mUp), __vspltw(p, 1),
                     __vmaddfp(LOADV(body->mRi), __vspltw(p, 0), LOADV(body->mCom))));
}

// Drives with a spy: force and torque as for joints, plus the current distance between the
// attachment points and the orientation error |1 - qA.qB|.
void Simulation::SpyDriveJacobians()
{
    RWP_PTR(JointJacobian) d = RWP_CAST(JointJacobian, (RWP_PTR(DriveJacobian))m_DJ_Stack);   // same frame
    RWP_PTR(DriveSpy) out = RWP_CAST(DriveSpy, (RWP_PTR(DriveJacobian))m_DJ_Stack);
    __vector4 freq = __vspltw(__lvlx(RWP_ADDR(m_Frequency), 0), 0);
    __vector4 freq2 = __vmulfp(freq, freq);
    for (unsigned int i = 0; i < m_DR_Count; i++, d += 1)
    {
        if (d->m_nb == 0)
            continue;
        // PERM_BEGIN
        RWP_PTR(Drive) drive = RWP_CAST(Drive, (RWP_PTR(Joint))d->m_joint);
        RWP_PTR(DriveFrames) frame = drive->m_skel;
        RWP_PTR(RigidBody) bodyA = drive->m_bodyA;
        RWP_PTR(RigidBody) bodyB = drive->m_bodyB;

        __vector4 sep = __vsubfp(SpyBodyPoint(bodyB, frame->mPosB), SpyBodyPoint(bodyA, frame->mPosA));
        __vector4 len2 = __vmsum3fp(sep, sep);
        const __vector4 one1 = __vspltisw(1);
        const __vector4 half = __vcfsx(one1, 1), one = __vcfsx(one1, 0);
        __vector4 e = __vrsqrtefp(len2);
        e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), one), e);
        e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), one), e);
        const __vector4 zero = __vspltisw(0);
        __vector4 len = __vsel(__vmulfp(len2, e), zero, __vcmpeqfp(zero, len2));

        __vector4 qa = SpyQuatMul(bodyA->mQuat, frame->mQuatA);
        __vector4 qb = SpyQuatMul(bodyB->mQuat, frame->mQuatB);
        __vector4 dot = __vmsum4fp(qa, qb);
        float angErr = 1.0f - VLANE_F(dot, 0);
        __vector4 force = SpyRows(LOADV(d->m_L_ri), LOADV(d->m_L_up), LOADV(d->m_L_at), LOADV(d->m_Pos_Imp));
        __vector4 torque = SpyRows(RWP_VEC3(d->m_T_ri), RWP_VEC3(d->m_T_up), RWP_VEC3(d->m_T_at), LOADV(d->m_Ang_Imp));

        out->m_distance = VLANE_F(len, 0);
        out->m_force = __vmulfp(force, freq2);
        out->m_angleError = fabsf(angErr);
        out->m_torque = __vmulfp(torque, freq2);
        out->m_drive = drive;
        out->m_tag = drive->m_tag;
        m_DS_Count++;
        // PERM_END
        out += 1;
    }
}

} }
