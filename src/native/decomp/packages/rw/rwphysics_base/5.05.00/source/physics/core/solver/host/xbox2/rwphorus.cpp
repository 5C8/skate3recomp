// rw::physics Horus solver (rwphysics_base rwpcore unity build).
// Status: functionally equivalent (tools/equiv), not byte-matching (register allocation).
// Decode notes (sk83_na_m 0x82cdb708, 0x920 bytes, leaf: no calls):
//   while (iterations--) { contacts (stride 0x100); joints (0x180); drives (0x180) }
//   Every jacobian holds two body pointers (ContactJacobian +0x0c m_ia, +0x1c m_ib) to a
//   4-vector body block: +0x00 v, +0x10 v (position pass), +0x20 w, +0x30 w (position pass).
//   One shared 0x180-byte constant block at 0x822b9d60 (also used by the island version).
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

namespace rw { namespace physics {

struct HorusBody
{
    RWP_VEC v;    // +0x00 linear velocity
    RWP_VEC vp;   // +0x10 linear pseudo velocity (w lane = position pass)
    RWP_VEC w;    // +0x20 angular velocity
    RWP_VEC wp;   // +0x30 angular pseudo velocity
};

struct HorusContact            // == ContactJacobian (sk82 PDB names)
{
    RWP_VEC ra;      // +0x00 w: HorusBody *ia
    RWP_VEC rb;      // +0x10 w: HorusBody *ib
    RWP_VEC CT_ri;   // +0x20
    RWP_VEC CT_up;   // +0x30 w: mus
    RWP_VEC CT_at;   // +0x40 w: mud
    RWP_VEC Imp;     // +0x50 xyz: Pos_Imp, w: Pst_Imp
    RWP_VEC Lim;     // +0x60 xyz: Pos_Lim, w: dist
    RWP_VEC C_ri;    // +0x70
    RWP_VEC IRAC_ri; // +0x80 w: ima
    RWP_VEC IRBC_ri; // +0x90 w: imb
    RWP_VEC C_up;    // +0xa0
    RWP_VEC IRAC_up; // +0xb0
    RWP_VEC IRBC_up; // +0xc0
    RWP_VEC C_at;    // +0xd0
    RWP_VEC IRAC_at; // +0xe0
    RWP_VEC IRBC_at; // +0xf0
};

struct HorusJoint             // == JointJacobian / DriveJacobian (same 0x180 frame)
{
    RWP_VEC ra;       // +0x00 w: HorusBody *ia
    RWP_VEC rb;       // +0x10 w: HorusBody *ib
    RWP_VEC PosImp;   // +0x20 (drive w: lindyn)
    RWP_VEC AngImp;   // +0x30 (drive w: angdyn)
    RWP_VEC LT_ri;    // +0x40
    RWP_VEC TT_ri;    // +0x50
    RWP_VEC LT_up;    // +0x60 w: Ang_X_Max  | drive Pos_Lim_X
    RWP_VEC TT_up;    // +0x70 w: Ang_Y_Max  | drive Pos_Lim_Y
    RWP_VEC LT_at;    // +0x80 w: Ang_X_Min  | drive Ang_Lim_X
    RWP_VEC TT_at;    // +0x90 w: Ang_Y_Min  | drive Ang_Lim_Y
    RWP_VEC PosMax;   // +0xa0 w: Ang_Z_Max  | drive Pos_Off, Pos_Lim_Z
    RWP_VEC PosMin;   // +0xb0 w: Ang_Z_Min  | drive Ang_Off, Ang_Lim_Z
    RWP_VEC L_ri;     // +0xc0
    RWP_VEC L_up;     // +0xd0
    RWP_VEC L_at;     // +0xe0
    RWP_VEC T_ri;     // +0xf0
    RWP_VEC T_up;     // +0x100
    RWP_VEC T_at;     // +0x110
    RWP_VEC InvInA_ri;// +0x120 w: ima
    RWP_VEC InvInA_up;// +0x130
    RWP_VEC InvInA_at;// +0x140
    RWP_VEC InvInB_ri;// +0x150 w: imb
    RWP_VEC InvInB_up;// +0x160
    RWP_VEC InvInB_at;// +0x170
};

// 0x822b9d60
RWP_ALIGN16 static const unsigned int sHorusConst[24][4] =
{
    { 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000 },   // 0x00 xyz mask
    { 0x08090a0b, 0x00010203, 0x04050607, 0x0c0d0e0f },   // 0x10 zxyw
    { 0x04050607, 0x08090a0b, 0x00010203, 0x0c0d0e0f },   // 0x20 yzxw
    { 0x00010203, 0x04050607, 0x08090a0b, 0x00010203 },   // 0x30 xyzx
    { 0x08090a0b, 0x18191a1b, 0x10111213, 0x0c0d0e0f },   // 0x40
    { 0x04050607, 0x14151617, 0x18191a1b, 0x0c0d0e0f },   // 0x50
    { 0x08090a0b, 0x0c0d0e0f, 0x14151617, 0x10111213 },   // 0x60
    { 0x00010203, 0x04050607, 0x18191a1b, 0x10111213 },   // 0x70
    { 0x00010203, 0x04050607, 0x10111213, 0x14151617 },   // 0x80
    { 0x00010203, 0x00010203, 0x00010203, 0x10111213 },   // 0x90 a.xxx b.x
    { 0x04050607, 0x04050607, 0x04050607, 0x14151617 },   // 0xa0 a.yyy b.y
    { 0x08090a0b, 0x08090a0b, 0x08090a0b, 0x18191a1b },   // 0xb0 a.zzz b.z
    { 0x10111213, 0x00010203, 0x00010203, 0x10111213 },   // 0xc0 b.x a.x a.x b.x
    { 0x00010203, 0x04050607, 0x08090a0b, 0x10111213 },   // 0xd0
    { 0x00010203, 0x04050607, 0x08090a0b, 0x14151617 },   // 0xe0
    { 0x00010203, 0x04050607, 0x08090a0b, 0x18191a1b },   // 0xf0
    { 0x08090a0b, 0x0c0d0e0f, 0x1c1d1e1f, 0x1c1d1e1f },   // 0x100
    { 0x3f800000, 0x00000000, 0x00000000, 0x00000000 },   // 0x110 (1,0,0,0)
    { 0x00000000, 0x00000000, 0x3f800000, 0x00000000 },   // 0x120 (0,0,1,0)
    { 0x80000000, 0x80000000, 0x80000000, 0x80000000 },   // 0x130 sign
    { 0x7f7fffff, 0x00000000, 0x00000000, 0x7f7fffff },   // 0x140 (FLT_MAX,0,0,FLT_MAX)
    { 0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000 },   // 0x150 0.5
    { 0x00010203, 0x00010203, 0x0c0d0e0f, 0x1c1d1e1f },   // 0x160
    { 0x04050607, 0x14151617, 0x00010203, 0x00010203 },   // 0x170
};
#define HC(i) RWP_CONST(sHorusConst[i])

struct HV { __vector4 v; };

// Constants the three solver loops share. Declared at the top of both pipeline functions,
// after the iteration guard; the xex keeps the five HV ones in stack homes.
#define HORUS_CONSTANTS                         \
    const __vector4 k16 = HC(16);               \
    const __vector4 zxy = HC(1);                \
    const __vector4 yzx = HC(2);                \
    HV kXYZX = { HC(3) };                       \
    HV kX = { HC(9) }, kY = { HC(10) }, kZ = { HC(11) }, kN = { HC(12) }; \
    const __vector4 bounds = HC(20);            \
    const __vector4 xyzMask = HC(0);

#define BODY(j, off) RWP_LOAD_PTR(HorusBody, (const char *)&*(j) + (off))

// One contact row (ContactJacobian, 0x100 bytes): velocity + position pass, friction cone.
static __forceinline void HorusSolveContact(RWP_PTR(HorusContact) c, __vector4 zxy, __vector4 yzx,
    const HV &kXYZX, const HV &kX, const HV &kY, const HV &kZ, const HV &kN, __vector4 bounds)
{
    const __vector4 zero = __vspltisb(0);
    __vector4 ra = c->ra;
    __vector4 rb = c->rb;
    __dcbt(0x200, &*c);
    __dcbt(0x280, &*c);
    RWP_PTR(HorusBody) a = BODY(c, 0x0c);
    RWP_PTR(HorusBody) b = BODY(c, 0x1c);

    // velocity of each body at the contact point: v + w x r (position pass first)
    __vector4 awp = a->wp, bwp = b->wp, aw = a->w, avp = a->vp, bvp = b->vp, av = a->v, bw = b->w, bv = b->v;
    __vector4 vap = __vnmsubfp(__vperm(awp, awp, zxy), __vperm(ra, ra, yzx), __vmaddfp(__vperm(awp, awp, yzx), __vperm(ra, ra, zxy), avp));
    __vector4 vbp = __vnmsubfp(__vperm(bwp, bwp, zxy), __vperm(rb, rb, yzx), __vmaddfp(__vperm(bwp, bwp, yzx), __vperm(rb, rb, zxy), bvp));
    __vector4 va  = __vnmsubfp(__vperm(aw, aw, zxy), __vperm(ra, ra, yzx), __vmaddfp(__vperm(aw, aw, yzx), __vperm(ra, ra, zxy), av));
    __vector4 vb  = __vnmsubfp(__vperm(bw, bw, zxy), __vperm(rb, rb, yzx), __vmaddfp(__vperm(bw, bw, yzx), __vperm(rb, rb, zxy), bv));

    __vector4 CT_ri = c->CT_ri, CT_up = c->CT_up, CT_at = c->CT_at;
    __vector4 oldImp = c->Imp, lim = c->Lim;
    __vector4 C_ri = c->C_ri, IRAC_ri = c->IRAC_ri, IRBC_ri = c->IRBC_ri;
    __vector4 C_up = c->C_up, IRAC_up = c->IRAC_up, IRBC_up = c->IRBC_up;
    __vector4 C_at = c->C_at, IRAC_at = c->IRAC_at, IRBC_at = c->IRBC_at;

    __vector4 dvp = __vsubfp(vbp, vap);
    __vector4 dv = __vaddfp(__vsubfp(vb, va), dvp);

    __vector4 imp = __vaddfp(oldImp, lim);
    imp = __vmaddfp(__vperm(CT_ri, CT_ri, kXYZX.v), __vperm(dv, dvp, kX.v), imp);
    imp = __vmaddfp(__vperm(CT_up, CT_up, kXYZX.v), __vperm(dv, dvp, kY.v), imp);
    imp = __vmaddfp(__vperm(CT_at, CT_at, kXYZX.v), __vperm(dv, dvp, kZ.v), imp);

    // friction cone: normal >= 0, |friction| <= mus*N else mud*N
    __vector4 n = __vperm(oldImp, zero, kN.v);
    __vector4 mus = __vspltw(CT_up, 3);
    __vector4 mud = __vspltw(CT_at, 3);
    __vector4 sLo = __vnmsubfp(mus, n, zero);
    __vector4 sHi = __vmaddfp(mus, n, bounds);
    __vector4 dLo = __vnmsubfp(mud, n, zero);
    __vector4 dHi = __vmaddfp(mud, n, bounds);
    __vector4 newImp = __vsel(dHi, __vsel(dLo, imp, __vcmpgefp(imp, sLo)), __vcmpgefp(sHi, imp));
    __vector4 d = __vsubfp(newImp, oldImp);
    c->Imp = newImp;

    __vector4 dx = __vspltw(d, 0);
    __vector4 dy = __vspltw(d, 1);
    __vector4 dz = __vspltw(d, 2);
    __vector4 dw = __vspltw(d, 3);
    __vector4 ima = __vspltw(IRAC_ri, 3);
    __vector4 imb = __vspltw(IRBC_ri, 3);

    a->wp = __vmaddfp(IRAC_ri, dw, awp);
    b->wp = __vnmsubfp(IRBC_ri, dw, bwp);
    __vector4 pp = __vmaddfp(C_ri, dw, zero);
    a->vp = __vmaddfp(pp, ima, avp);
    b->vp = __vnmsubfp(pp, imb, bvp);
    a->w = __vmaddfp(IRAC_at, dz, __vmaddfp(IRAC_up, dy, __vmaddfp(IRAC_ri, dx, aw)));
    b->w = __vnmsubfp(IRBC_at, dz, __vnmsubfp(IRBC_up, dy, __vnmsubfp(IRBC_ri, dx, bw)));
    __vector4 p = __vmaddfp(C_at, dz, __vmaddfp(C_up, dy, __vmaddfp(C_ri, dx, zero)));
    a->v = __vmaddfp(p, ima, av);
    b->v = __vnmsubfp(p, imb, bv);
}

// Shared tail of joint and drive rows: apply the impulse deltas dP (linear) / dA (angular).
static __forceinline void HorusApplyJoint(RWP_PTR(HorusJoint) j, RWP_PTR(HorusBody) a, RWP_PTR(HorusBody) b,
    __vector4 ra, __vector4 rb, __vector4 av, __vector4 aw, __vector4 bv, __vector4 bw,
    __vector4 dP, __vector4 dA, __vector4 zero, __vector4 zxy, __vector4 yzx)
{
    __dcbt(0x180, &*j);
    __dcbt(0x200, &*j);
    __dcbt(0x280, &*j);

    __vector4 L_ri = j->L_ri, L_up = j->L_up, L_at = j->L_at;
    __vector4 T_ri = j->T_ri, T_up = j->T_up, T_at = j->T_at;
    __vector4 IA_ri = j->InvInA_ri, IA_up = j->InvInA_up, IA_at = j->InvInA_at;
    __vector4 IB_ri = j->InvInB_ri, IB_up = j->InvInB_up, IB_at = j->InvInB_at;
    __vector4 ima = __vspltw(IA_ri, 3);
    __vector4 imb = __vspltw(IB_ri, 3);
    __vector4 P = __vmaddfp(L_at, __vspltw(dP, 2), __vmaddfp(L_up, __vspltw(dP, 1), __vmaddfp(L_ri, __vspltw(dP, 0), zero)));
    __vector4 T = __vmaddfp(T_at, __vspltw(dA, 2), __vmaddfp(T_up, __vspltw(dA, 1), __vmaddfp(T_ri, __vspltw(dA, 0), zero)));
    a->v = __vmaddfp(P, ima, av);
    b->v = __vnmsubfp(P, imb, bv);
    __vector4 TA = __vnmsubfp(__vperm(ra, ra, zxy), __vperm(P, P, yzx), __vmaddfp(__vperm(ra, ra, yzx), __vperm(P, P, zxy), T));
    __vector4 TB = __vnmsubfp(__vperm(rb, rb, zxy), __vperm(P, P, yzx), __vmaddfp(__vperm(rb, rb, yzx), __vperm(P, P, zxy), T));
    a->w = __vmaddfp(IA_at, __vspltw(TA, 2), __vmaddfp(IA_up, __vspltw(TA, 1), __vmaddfp(IA_ri, __vspltw(TA, 0), aw)));
    b->w = __vnmsubfp(IB_at, __vspltw(TB, 2), __vnmsubfp(IB_up, __vspltw(TB, 1), __vnmsubfp(IB_ri, __vspltw(TB, 0), bw)));
}

// One joint row (JointJacobian, 0x180 bytes): bilateral limits max(Max+i,0) + min(Min+i,0).
static __forceinline void HorusSolveJoint(RWP_PTR(HorusJoint) j, __vector4 zxy, __vector4 yzx, __vector4 k16)
{
    const __vector4 zero = __vspltisw(0);
    RWP_PTR(HorusBody) a = BODY(j, 0x0c);
    RWP_PTR(HorusBody) b = BODY(j, 0x1c);
    __dcbt(0, a);
    __dcbt(0, b);
    __vector4 ra = j->ra, rb = j->rb;
    __vector4 av = a->v, aw = a->w, bv = b->v, bw = b->w;
    __vector4 va = __vnmsubfp(__vperm(aw, aw, zxy), __vperm(ra, ra, yzx), __vmaddfp(__vperm(aw, aw, yzx), __vperm(ra, ra, zxy), av));
    __vector4 vb = __vnmsubfp(__vperm(bw, bw, zxy), __vperm(rb, rb, yzx), __vmaddfp(__vperm(bw, bw, yzx), __vperm(rb, rb, zxy), bv));
    __vector4 dv = __vsubfp(vb, va);
    __vector4 dw = __vsubfp(bw, aw);
    __vector4 pos = j->PosImp, ang = j->AngImp;
    __vector4 angMin = __vperm(__vmrglw(j->LT_at, j->TT_at), j->PosMin, k16);
    __vector4 angMax = __vperm(__vmrglw(j->LT_up, j->TT_up), j->PosMax, k16);
    __vector4 newAng = __vmaddfp(j->TT_at, __vspltw(dw, 2), __vmaddfp(j->TT_up, __vspltw(dw, 1), __vmaddfp(j->TT_ri, __vspltw(dw, 0), ang)));
    newAng = __vaddfp(__vmaxfp(__vaddfp(angMax, newAng), zero), __vminfp(__vaddfp(angMin, newAng), zero));
    __vector4 newPos = __vmaddfp(j->LT_at, __vspltw(dv, 2), __vmaddfp(j->LT_up, __vspltw(dv, 1), __vmaddfp(j->LT_ri, __vspltw(dv, 0), pos)));
    newPos = __vaddfp(__vmaxfp(__vaddfp(j->PosMax, newPos), zero), __vminfp(__vaddfp(j->PosMin, newPos), zero));
    __vector4 dA = __vsubfp(newAng, ang);
    j->AngImp = newAng;
    __vector4 dP = __vsubfp(newPos, pos);
    j->PosImp = newPos;
    HorusApplyJoint(j, a, b, ra, rb, av, aw, bv, bw, dP, dA, zero, zxy, yzx);
}

// One drive row (DriveJacobian, 0x180 bytes): (imp + TT*dw)*dyn + Off clamped to +/-Lim.
static __forceinline void HorusSolveDrive(RWP_PTR(HorusJoint) j, __vector4 zxy, __vector4 yzx, __vector4 k16, __vector4 xyzMask)
{
    const __vector4 zero = __vspltisw(0);
    RWP_PTR(HorusBody) a = BODY(j, 0x0c);
    RWP_PTR(HorusBody) b = BODY(j, 0x1c);
    __dcbt(0, a);
    __dcbt(0, b);
    __vector4 ra = j->ra, rb = j->rb;
    __vector4 av = a->v, aw = a->w, bv = b->v, bw = b->w;
    __vector4 va = __vnmsubfp(__vperm(aw, aw, zxy), __vperm(ra, ra, yzx), __vmaddfp(__vperm(aw, aw, yzx), __vperm(ra, ra, zxy), av));
    __vector4 vb = __vnmsubfp(__vperm(bw, bw, zxy), __vperm(rb, rb, yzx), __vmaddfp(__vperm(bw, bw, yzx), __vperm(rb, rb, zxy), bv));
    __vector4 dv = __vsubfp(vb, va);
    __vector4 dw = __vsubfp(bw, aw);
    __vector4 pos = j->PosImp, ang = j->AngImp;
    __vector4 posLim = __vperm(__vmrglw(j->LT_up, j->TT_up), j->PosMax, k16);
    __vector4 angLim = __vperm(__vmrglw(j->LT_at, j->TT_at), j->PosMin, k16);
    __vector4 newAng = __vmaddfp(j->TT_at, __vspltw(dw, 2), __vmaddfp(j->TT_up, __vspltw(dw, 1), __vmaddfp(j->TT_ri, __vspltw(dw, 0), ang)));
    newAng = __vmaddfp(newAng, __vspltw(ang, 3), j->PosMin);
    newAng = __vmaxfp(__vminfp(newAng, angLim), __vsubfp(zero, angLim));
    __vector4 newPos = __vmaddfp(j->LT_at, __vspltw(dv, 2), __vmaddfp(j->LT_up, __vspltw(dv, 1), __vmaddfp(j->LT_ri, __vspltw(dv, 0), pos)));
    newPos = __vmaddfp(newPos, __vspltw(pos, 3), j->PosMax);
    newPos = __vmaxfp(__vminfp(newPos, posLim), __vsubfp(zero, posLim));
    __vector4 dA = __vsubfp(newAng, ang);
    __vector4 dP = __vsubfp(newPos, pos);
    j->PosImp = __vsel(pos, newPos, xyzMask);
    j->AngImp = __vsel(ang, newAng, xyzMask);
    HorusApplyJoint(j, a, b, ra, rb, av, aw, bv, bw, dP, dA, zero, zxy, yzx);
}

#define HORUS_CONTACT(c) HorusSolveContact(c, zxy, yzx, kXYZX, kX, kY, kZ, kN, bounds)

// sk83_na_m 0x82cdb708. Functionally equivalent (tools/equiv); not byte-matching.
void HorusPipeline(unsigned int numContacts, RWP_PTR(ContactJacobian) contacts,
                   unsigned int numJoints, RWP_PTR(JointJacobian) joints,
                   unsigned int numDrives, RWP_PTR(DriveJacobian) drives,
                   unsigned int iterations)
{
    if (iterations == 0) return;
    HORUS_CONSTANTS

    while (iterations--)
    {
        RWP_PTR(HorusContact) c = RWP_CAST(HorusContact, contacts);
        __dcbt(0x000, &*c);
        __dcbt(0x080, &*c);
        __dcbt(0x100, &*c);
        __dcbt(0x180, &*c);
        for (unsigned int i = 0; i < numContacts; i++, c++)
            HORUS_CONTACT(c);

        RWP_PTR(HorusJoint) j = RWP_CAST(HorusJoint, joints);
        __dcbt(0x000, &*j);
        __dcbt(0x080, &*j);
        __dcbt(0x100, &*j);
        for (unsigned int i = 0; i < numJoints; i++, j++)
            HorusSolveJoint(j, zxy, yzx, k16);

        RWP_PTR(HorusJoint) d = RWP_CAST(HorusJoint, drives);
        __dcbt(0x000, &*d);
        __dcbt(0x080, &*d);
        __dcbt(0x100, &*d);
        for (unsigned int i = 0; i < numDrives; i++, d++)
            HorusSolveDrive(d, zxy, yzx, k16, xyzMask);
    }
}

// sk83_na_m 0x82cdc050: same rows over per-island lists of jacobian pointers; every row is
// prefetched on its own.
void Simulation::HorusPipelineIslandUsingIndirectJacobians(unsigned int island)
{
    unsigned int numContacts = m_islandContactsCount[island];
    unsigned int contacts = m_islandContactJacobians[island];
    unsigned int numJoints = m_islandJointsCount[island];
    unsigned int joints = m_islandJointJacobians[island];
    unsigned int numDrives = m_islandDrivesCount[island];
    unsigned int drives = m_islandDriveJacobians[island];
    unsigned int iterations = m_MaxIteration;
    if (iterations == 0) return;
    HORUS_CONSTANTS

    while (iterations--)
    {
        for (unsigned int i = 0; i < numContacts; i++)
        {
            RWP_PTR(HorusContact) c = RWP_LIST_GET(HorusContact, contacts, i);
            __dcbt(0x000, &*c);
            __dcbt(0x080, &*c);
            __dcbt(0x100, &*c);
            __dcbt(0x180, &*c);
            HORUS_CONTACT(c);
        }
        for (unsigned int i = 0; i < numJoints; i++)
        {
            RWP_PTR(HorusJoint) j = RWP_LIST_GET(HorusJoint, joints, i);
            __dcbt(0x000, &*j);
            __dcbt(0x080, &*j);
            __dcbt(0x100, &*j);
            HorusSolveJoint(j, zxy, yzx, k16);
        }
        for (unsigned int i = 0; i < numDrives; i++)
        {
            RWP_PTR(HorusJoint) d = RWP_LIST_GET(HorusJoint, drives, i);
            __dcbt(0x000, &*d);
            __dcbt(0x080, &*d);
            __dcbt(0x100, &*d);
            HorusSolveDrive(d, zxy, yzx, k16, xyzMask);
        }
    }
}

// sk83_na_m 0x82cdc028
void Simulation::Horus_Pipeline()
{
    unsigned int numContacts = m_CT_Count;
    HorusPipeline(numContacts, m_CJ_Stack, m_JT_Count, m_JJ_Stack, m_DR_Count, m_DJ_Stack,
                  m_MaxIteration);
}

} }
