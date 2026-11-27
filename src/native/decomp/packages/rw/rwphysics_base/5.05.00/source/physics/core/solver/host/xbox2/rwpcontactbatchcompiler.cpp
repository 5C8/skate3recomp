// rw::physics "Contact Compiler" stage: Simulation::ContactBatchBuild.
// sk83_na_m 0x82cdb1c8 (0x53c, leaf). Job: SimulationManager "Contact Compiler" 0x830ac410.
//
// The collision side writes each contact into its ContactJacobian slot as a ContactInput
// (below); this stage turns it into the jacobian in place:
//   - ra/rb: the contact point relative to each body's centre of mass
//   - three rows: the normal (ri) and two tangents (up, at) as r x n, and I^-1 (r x n) per body,
//     I^-1 from the body's symmetric inverse tensor (diagonal + off-diagonal), used only for a
//     dynamic body (flag bit 2 in the off-diagonal's w)
//   - the effective mass K = sum (r x n) . I^-1 (r x n) + 1/m, inverted (vrefp)
//   - the position error and the bias/restitution terms, scaled by dt and dt^2
// Every input is read before the first store (the jacobian reuses the same 0x100 bytes).
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

// The contact as the collision side leaves it in the jacobian's slot (sk83; no PDB type).
struct ContactInput                 // size 0x100
{
    RWP_VEC m_pointA;               // 0x00 contact point on A (w kept as the jacobian's m_ia)
    RWP_VEC m_pointB;               // 0x10 contact point on B
    RWP_VEC m_normal;               // 0x20 row ri (w: the jacobian's m_C_ri w)
    RWP_VEC m_tangent0;             // 0x30 row up (w: mus)
    RWP_VEC m_tangent1;             // 0x40 row at (w: mud)
    RWP_VEC m_bias;                 // 0x50 position bias (w kept in m_C_at)
    RWP_VEC m_comA;                 // 0x60 (w: ReactionForces* of A)
    RWP_VEC m_comB;                 // 0x70 (w: ReactionForces* of B)
    RWP_VEC m_invInertiaA;          // 0x80 diagonal (w: inverse mass)
    RWP_VEC m_invInertiaB;          // 0x90
    RWP_VEC m_invInertiaOffA;       // 0xa0 off-diagonal (w: body flags)
    RWP_VEC m_invInertiaOffB;       // 0xb0
    RWP_VEC m_velA;                 // 0xc0
    RWP_VEC m_velB;                 // 0xd0
    RWP_VEC m_omegaA;               // 0xe0
    RWP_VEC m_omegaB;               // 0xf0
};

// Shared constant block at 0x822b9d50 (horus.cpp's sHorusConst starts at the next entry).
RWP_ALIGN16 static const unsigned int sContactConst[10][4] =
{
    { 0xffffffff, 0x00000000, 0x00000000, 0x00000000 },   // 0x00 x mask
    { 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000 },   // 0x10 xyz mask
    { 0x08090a0b, 0x00010203, 0x04050607, 0x0c0d0e0f },   // 0x20 zxyw
    { 0x04050607, 0x08090a0b, 0x00010203, 0x0c0d0e0f },   // 0x30 yzxw
    { 0x00010203, 0x04050607, 0x08090a0b, 0x00010203 },   // 0x40
    { 0x08090a0b, 0x18191a1b, 0x10111213, 0x0c0d0e0f },   // 0x50 tensor row 2
    { 0x04050607, 0x14151617, 0x18191a1b, 0x0c0d0e0f },   // 0x60 tensor row 1
    { 0x08090a0b, 0x0c0d0e0f, 0x14151617, 0x10111213 },   // 0x70 transpose col 1
    { 0x00010203, 0x04050607, 0x18191a1b, 0x10111213 },   // 0x80 transpose col 2
    { 0x00010203, 0x04050607, 0x10111213, 0x14151617 },   // 0x90 transpose col 0 (0x822b9de0)
};

// a x b with the yzx/zxy permutes, as the xex evaluates it
#define CONTACT_CROSS(a, b) \
    __vnmsubfp(__vperm(a, a, zxy), __vperm(b, b, yzx), __vmaddfp(__vperm(a, a, yzx), __vperm(b, b, zxy), zero))
// M * v for a matrix held as three column vectors
#define CONTACT_MUL(c0, c1, c2, v) \
    __vmaddfp(c2, __vspltw(v, 2), __vmaddfp(c1, __vspltw(v, 1), __vmaddfp(c0, __vspltw(v, 0), zero)))

void Simulation::ContactBatchBuild()
{
    const __vector4 dt = __vspltw(__lvlx(RWP_ADDR(m_TimeStep), 0), 0);
    const __vector4 dt2 = __vmulfp(dt, dt);
    RWP_PTR(ContactJacobian) cj = m_CJ_Stack;
    if (m_CT_Count == 0)
        return;
    const __vector4 zero = __vspltisb(0);
    const __vector4 four = __vspltisw(4);
    const __vector4 eight = __vspltisw(8);
    const __vector4 row1 = RWP_CONST(sContactConst[6]);
    const __vector4 col0 = RWP_CONST(sContactConst[9]);
    const __vector4 col1 = RWP_CONST(sContactConst[7]);
    const __vector4 col2 = RWP_CONST(sContactConst[8]);
    const __vector4 xMask = RWP_CONST(sContactConst[0]);
    const __vector4 xyzMask = RWP_CONST(sContactConst[1]);
    const __vector4 row2 = RWP_CONST(sContactConst[5]);
    const __vector4 yzx = RWP_CONST(sContactConst[3]);
    const __vector4 zxy = RWP_CONST(sContactConst[2]);

    unsigned int i = 0;
    do
    {
        RWP_PTR(ContactInput) in = RWP_CAST(ContactInput, cj);
        __dcbt(0x100, &*cj);
        __dcbt(0x180, &*cj);
        __vector4 pointA = in->m_pointA, pointB = in->m_pointB;
        __vector4 normal = in->m_normal, tangent0 = in->m_tangent0, tangent1 = in->m_tangent1;
        __vector4 bias = in->m_bias, comA = in->m_comA, comB = in->m_comB;
        __vector4 invA = in->m_invInertiaA, invB = in->m_invInertiaB;
        __vector4 offA = in->m_invInertiaOffA, offB = in->m_invInertiaOffB;
        __vector4 velA = in->m_velA, velB = in->m_velB, omegaA = in->m_omegaA, omegaB = in->m_omegaB;

        __vector4 ra = __vsubfp(pointA, comA);
        __vector4 rb = __vsubfp(pointB, comB);

        // inverse inertia of each body as three columns (zero unless the body is dynamic)
        __vector4 flagsA = __vspltw(offA, 3);
        __vector4 flagsB = __vspltw(offB, 3);
        __vector4 dynA = __vcmpequw(__vand(flagsA, four), four);
        __vector4 dynB = __vcmpequw(__vand(flagsB, four), four);
        __vector4 ia0 = __vsel(zero, invA, dynA), iaOff = __vsel(zero, offA, dynA);
        __vector4 ib0 = __vsel(zero, invB, dynB), ibOff = __vsel(zero, offB, dynB);
        __vector4 ia1 = __vperm(ia0, iaOff, row1), ia2 = __vperm(ia0, iaOff, row2);
        __vector4 ib1 = __vperm(ib0, ibOff, row1), ib2 = __vperm(ib0, ibOff, row2);

        __vector4 aUp = CONTACT_CROSS(ra, tangent0), bUp = CONTACT_CROSS(rb, tangent0);
        __vector4 aAt = CONTACT_CROSS(ra, tangent1), bAt = CONTACT_CROSS(rb, tangent1);
        __vector4 aRi = CONTACT_CROSS(ra, normal), bRi = CONTACT_CROSS(rb, normal);
        __vector4 IaUp = CONTACT_MUL(ia0, ia1, ia2, aUp), IbUp = CONTACT_MUL(ib0, ib1, ib2, bUp);
        __vector4 IaAt = CONTACT_MUL(ia0, ia1, ia2, aAt), IbAt = CONTACT_MUL(ib0, ib1, ib2, bAt);
        __vector4 IaRi = CONTACT_MUL(ia0, ia1, ia2, aRi), IbRi = CONTACT_MUL(ib0, ib1, ib2, bRi);

        // effective mass per row, inverted
        __vector4 kRi = __vmaddfp(bRi, IbRi, __vmaddfp(aRi, IaRi, zero));
        __vector4 kUp = __vmaddfp(bUp, IbUp, __vmaddfp(aUp, IaUp, zero));
        __vector4 kAt = __vmaddfp(bAt, IbAt, __vmaddfp(aAt, IaAt, zero));
        __vector4 kh = __vmrghw(kRi, kUp);
        __vector4 kSum = __vaddfp(__vaddfp(__vperm(__vmrglw(kRi, kUp), kAt, col2),
                                           __vaddfp(__vperm(kh, kAt, col0), __vspltw(ia0, 3))),
                                  __vaddfp(__vperm(kh, kAt, col1), __vspltw(ib0, 3)));
        __vector4 invK = __vrefp(kSum);

        // the rows as a matrix (transpose of normal, tangent0, tangent1)
        __vector4 nh = __vmrghw(normal, tangent0);
        __vector4 t0 = __vperm(nh, tangent1, col0);
        __vector4 t1 = __vperm(nh, tangent1, col1);
        __vector4 t2 = __vperm(__vmrglw(normal, tangent0), tangent1, col2);

        __vector4 sep = __vsubfp(pointB, pointA);
        __vector4 sepImp = __vmaddfp(CONTACT_MUL(t0, t1, t2, sep), invK, zero);
        __vector4 dtBias = __vmaddfp(dt, bias, zero);
        __vector4 vA = __vsel(zero, __vmaddfp(__vperm(ra, ra, zxy), __vperm(omegaA, omegaA, yzx),
                                              __vnmsubfp(__vperm(ra, ra, yzx), __vperm(omegaA, omegaA, zxy), velA)), dynA);
        __vector4 vB = __vsel(zero, __vmaddfp(__vperm(rb, rb, zxy), __vperm(omegaB, omegaB, yzx),
                                              __vnmsubfp(__vperm(rb, rb, yzx), __vperm(omegaB, omegaB, zxy), velB)), dynB);
        __vector4 err = __vaddfp(dtBias, __vaddfp(sep, __vmaddfp(__vsubfp(vB, vA), dt2, zero)));
        __vector4 errImp = __vmaddfp(CONTACT_MUL(t0, t1, t2, err), invK, zero);
        __vector4 negBias = __vnmsubfp(dtBias, __vspltw(normal, 3), zero);
        __vector4 restImp = __vmaddfp(CONTACT_MUL(t0, t1, t2, negBias), invK, zero);

        // normal impulse limit (x lane only): clamp the position impulse
        __vector4 lim = __vsel(__vsel(__vsel(__vaddfp(sepImp, restImp), zero, __vcmpgefp(restImp, zero)),
                                      restImp, __vcmpgefp(sepImp, zero)),
                               zero, __vcmpgefp(zero, errImp));

        STOREV(cj->m_C_at, __vsel(bias, tangent1, xyzMask));
        STOREV(cj->m_C_up, __vsel(pointB, tangent0, xyzMask));
        STOREV(cj->m_C_ri, __vsel(pointA, normal, xyzMask));
        STOREV(cj->m_ra, __vsel(comA, ra, xyzMask));
        STOREV(cj->m_rb, __vsel(comB, rb, xyzMask));
        STOREV(cj->m_Pos_Imp, zero);
        STOREV(cj->m_IRAC_up, IaUp);
        STOREV(cj->m_IRBC_up, IbUp);
        STOREV(cj->m_IRAC_at, IaAt);
        STOREV(cj->m_IRAC_ri, __vsel(ia0, IaRi, xyzMask));
        STOREV(cj->m_IRBC_ri, __vsel(ib0, IbRi, xyzMask));
        STOREV(cj->m_IRBC_at, IbAt);
        STOREV(cj->m_CT_ri, __vsel(__vand(eight, __vor(flagsA, flagsB)), __vmaddfp(t0, invK, zero), xyzMask));
        STOREV(cj->m_CT_up, __vsel(tangent0, __vmaddfp(t1, invK, zero), xyzMask));
        STOREV(cj->m_CT_at, __vsel(tangent1, __vmaddfp(t2, invK, zero), xyzMask));
        STOREV(cj->m_Pos_Lim, __vsel(__vspltw(sepImp, 0), __vsubfp(errImp, __vand(lim, xMask)), xyzMask));
        cj += 1;
    } while (++i < m_CT_Count);
}

} }
