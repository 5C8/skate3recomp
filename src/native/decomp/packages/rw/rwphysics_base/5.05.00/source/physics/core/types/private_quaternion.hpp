// rw::physics::Jacobian_RQD::Create (sk83_na_m 0x82cdcec8, 0x1e0, leaf): the relative-rotation
// jacobian rows of two quaternions. Defined in this header in the original (rwphysics_base
// source/physics/core/types/private_quaternion.hpp). Compiled on its own here (cl /TP): with its
// body in the builders' TU, cl 15 allocates their registers differently from the original.
#ifndef RWP_PRIVATE_QUATERNION_HPP
#define RWP_PRIVATE_QUATERNION_HPP
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
#ifndef RWP_GUEST
#include "vlift_rt.h"
#endif

namespace rw { namespace physics {

// one lane of a member row, stored straight back (rw::math per-element setter)
template <int MASK, int ROT> static __forceinline void RqdSetLane(RWP_VEC &row, __vector4 v)
{
    row = __vrlimi((__vector4)row, v, MASK, ROT);
}

void Jacobian_RQD::Create(const __vector4 &q1, const __vector4 &q2)
{
    // PERM_BEGIN
    // p<k><j> = q1[k] * q2[j]
    __vector4 px = __vmulfp(q2, __vspltw(q1, 0));
    __vector4 py = __vmulfp(q2, __vspltw(q1, 1));
    __vector4 pz = __vmulfp(q2, __vspltw(q1, 2));
    __vector4 pw = __vmulfp(q2, __vspltw(q1, 3));
    __vector4 xx = __vspltw(px, 0), xy = __vspltw(px, 1), xz = __vspltw(px, 2), xw = __vspltw(px, 3);
    __vector4 yx = __vspltw(py, 0), yy = __vspltw(py, 1), yz = __vspltw(py, 2), yw = __vspltw(py, 3);
    __vector4 zx = __vspltw(pz, 0), zy = __vspltw(pz, 1), zz = __vspltw(pz, 2), zw = __vspltw(pz, 3);
    __vector4 wx = __vspltw(pw, 0), wy = __vspltw(pw, 1), wz = __vspltw(pw, 2), ww = __vspltw(pw, 3);
    __vector4 yxxy = __vaddfp(yx, xy);
    __vector4 xzzx = __vaddfp(xz, zx);
    __vector4 wwzz = __vaddfp(ww, zz);

    RqdSetLane<8, 0>(Jwi, __vsubfp(__vsubfp(__vaddfp(ww, xx), yy), zz));
    RqdSetLane<4, 3>(Jwi, __vaddfp(__vaddfp(yxxy, wz), zw));
    RqdSetLane<2, 2>(Jwi, __vsubfp(__vsubfp(xzzx, wy), yw));
    RqdSetLane<1, 1>(Jwi, __vsubfp(__vaddfp(__vsubfp(wx, xw), zy), yz));

    RqdSetLane<8, 0>(Jwj, __vsubfp(__vsubfp(yxxy, wz), zw));
    RqdSetLane<4, 3>(Jwj, __vsubfp(__vsubfp(__vaddfp(ww, yy), zz), xx));
    RqdSetLane<2, 2>(Jwj, __vaddfp(__vaddfp(__vaddfp(yz, zy), wx), xw));
    RqdSetLane<1, 1>(Jwj, __vsubfp(__vaddfp(__vsubfp(wy, yw), xz), zx));

    RqdSetLane<8, 0>(Jwk, __vaddfp(__vaddfp(xzzx, yw), wy));
    RqdSetLane<4, 3>(Jwk, __vsubfp(__vsubfp(__vaddfp(zy, yz), wx), xw));
    RqdSetLane<2, 2>(Jwk, __vsubfp(__vsubfp(wwzz, xx), yy));
    RqdSetLane<1, 1>(Jwk, __vsubfp(__vaddfp(__vsubfp(wz, zw), yx), xy));

    RqdSetLane<8, 0>(Jwr, __vsubfp(__vaddfp(__vsubfp(xw, wx), zy), yz));
    RqdSetLane<4, 3>(Jwr, __vsubfp(__vaddfp(__vsubfp(yw, wy), xz), zx));
    RqdSetLane<2, 2>(Jwr, __vsubfp(__vaddfp(__vsubfp(zw, wz), yx), xy));
    RqdSetLane<1, 1>(Jwr, __vaddfp(__vaddfp(wwzz, yy), xx));
    // PERM_END
}

} }
#endif
