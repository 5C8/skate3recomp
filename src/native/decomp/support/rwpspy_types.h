// Spy records written by the rw::physics spy stages (sk83 layouts) and their shared helper.
#ifndef RWP_SPY_TYPES_H
#define RWP_SPY_TYPES_H
#include "simulation.h"

namespace rw { namespace physics {

// The records (sk83; no PDB types).
struct SpyFrame                         // rw::math::vpu::Matrix44Affine
{
    RWP_VEC xAxis, yAxis, zAxis, wAxis;
};
struct ContactSpy                       // size 0x70 (PDB)
{
    SpyFrame mFrame;                    // 0x00 x/y/z: the jacobian's C rows, w: mass-weighted contact point
    RWP_VEC mNimp;                      // 0x40 normal impulse
    RWP_VEC mFric;                      // 0x50 friction impulse
    RWP_FIELD_PTR(RigidBody) mBodyA;    // 0x60
    RWP_FIELD_PTR(RigidBody) mBodyB;    // 0x64
    RWP_U32 mTag;                       // 0x68
    RWP_U32 m_pad6c;
};
struct JointSpy                         // size 0x30
{
    RWP_VEC m_force;                    // 0x00
    RWP_VEC m_torque;                   // 0x10
    RWP_FIELD_PTR(Joint) m_joint;       // 0x20
    RWP_U32 m_tag;                      // 0x24
    RWP_U32 m_pad[2];
};
struct DriveSpy                         // size 0x30
{
    RWP_VEC m_force;                    // 0x00
    RWP_VEC m_torque;                   // 0x10
    RWP_F32 m_distance;                 // 0x20 between the two attachment points
    RWP_F32 m_angleError;               // 0x24 |1 - qA.qB|
    RWP_FIELD_PTR(Drive) m_drive;       // 0x28
    RWP_U32 m_tag;                      // 0x2c
};

// row impulse -> world vector through three row vectors
static __forceinline __vector4 SpyRows(__vector4 r0, __vector4 r1, __vector4 r2, __vector4 imp)
{
    return __vmaddfp(r2, __vspltw(imp, 2), __vmaddfp(r1, __vspltw(imp, 1), __vmulfp(r0, __vspltw(imp, 0))));
}

} }
#endif
