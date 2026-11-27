// Shared rw::math vpu helpers of the joint and drive jacobian builders (joint_build.cpp,
// drive_build.cpp): the exact op sequences the sk83 code inlines.
#ifndef RWP_BUILD_MATH_H
#define RWP_BUILD_MATH_H

namespace rw { namespace physics {

// The quaternion-rows permutes are rw::math's (0x820a9fb0/c0/d0, also used by DynamicUpdate):
// the Xbox build references them like the original; the other builds keep a private copy.
#if defined(_M_PPC)
extern const unsigned int sQuatPermUp[4];   // 0x820a9fb0
extern const unsigned int sQuatPermRi[4];   // 0x820a9fc0
extern const unsigned int sQuatPermAt[4];   // 0x820a9fd0
#define BC(i) RWP_CONST((i) == 0 ? sQuatPermUp : (i) == 1 ? sQuatPermRi : sQuatPermAt)
#else
RWP_ALIGN16 static const unsigned int sBuildConst[][4] =
{
    { 0x00010203, 0x18191a1b, 0x00010203, 0x00010203 },   // 0 quaternion rows: up
    { 0x04050607, 0x10111213, 0x00010203, 0x00010203 },   // 1 quaternion rows: ri
    { 0x08090a0b, 0x14151617, 0x00010203, 0x00010203 },   // 2 quaternion rows: at
};
#define BC(i) RWP_CONST(sBuildConst[i])
#endif

// quaternion product a * b, as rw::math vpu evaluates it
static __forceinline __vector4 JQuatMul(__vector4 a, __vector4 b)
{
    __vector4 aw = __vspltw(a, 3), bw = __vspltw(b, 3);
    __vector4 v = __vmaddfp(a, bw, __vmaddfp(b, aw,
                    __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63)));
    return __vrlimi(v, __vsubfp(__vmulfp(aw, bw), __vmsum3fp(a, b)), 1, 1);
}

// rotation rows of a quaternion pre-scaled by sqrt(2) (the rw::math Matrix33(Quaternion) form)
struct JRows { __vector4 ri, up, at; };
static __forceinline JRows JQuatRows(__vector4 s, __vector4 half)
{
    __vector4 sq = __vnmsubfp(s, s, half);
    __vector4 diag = __vaddfp(sq, __vpermwi(sq, 0x60));
    __vector4 p = __vmulfp(s, __vpermwi(s, 0x60));
    __vector4 r = __vmulfp(__vspltw(s, 3), __vpermwi(s, 0x84));
    __vector4 sum = __vaddfp(p, r), dif = __vsubfp(p, r);
    JRows m;
    m.ri = __vrlimi(__vperm(diag, sum, BC(1)), dif, 2, 0);
    m.up = __vrlimi(__vperm(dif, diag, BC(0)), sum, 2, 3);
    m.at = __vrlimi(__vperm(sum, dif, BC(2)), diag, 2, 2);
    return m;
}

// a x b in the permuted form rw::math vpu Cross uses
static __forceinline __vector4 JCross(__vector4 a, __vector4 b)
{
    return __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63);
}

// body-space vector -> world (body rows; no translation)
static __forceinline __vector4 JBodyVec(RWP_PTR(RigidBody) b, __vector4 p)
{
    return __vmaddfp(LOADV(b->mAt), __vspltw(p, 2), __vmaddfp(LOADV(b->mUp), __vspltw(p, 1),
                     __vmulfp(LOADV(b->mRi), __vspltw(p, 0))));
}

} }

#endif
