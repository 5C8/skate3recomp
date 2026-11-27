// AnimCore::Anim_Tools::PoseWithMatMult (sk83_na_m 0x82a52fe0, 0x5d0), the work of the
// AnimCmdSys "pose with matrix multiply" job (ACSposeMatMult_job 0x82a53920).
// Hand-decompiled from the sk83 code with Skate 2's PDB types/names (SQTDataInternal) and its
// Hex-Rays output as the guide. Builds the three ways rwp_config.h describes.
//
//   1. local matrices: 4 bones per pass, SoA. Rotation rows from the quaternion
//      (1 - 2(y^2 + z^2), 2(xy + wz), 2(xz - wy), 0) etc., each row scaled by its scale lane;
//      row 3 = (trans.xyz, 1). The pass count is ceil(numBones / 4), so up to 3 matrices past
//      the end are written, and every pass zeroes the 128-byte line at L + bone*64 + 0x100
//      (a dcbzl prefetch-zero; on the last pass that line is past the written bones).
//   2. hierarchy: G[i] = L[i] for roots (parent -1) and children of the trajectory node,
//      else G[parent] * L[i] (affine). Bone 0 is copied first, even when numBones is 0.
//   3. GM[i] = G[i] * MatMult[i] when MatMult is given.
//   L = WorkBuffer (or GM); G = WorkBuffer when both WorkBuffer and MatMult are given, else GM.
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "anim_types.h"
#include <string.h>

namespace AnimCore { namespace Anim_Tools {
#ifdef RWP_GUEST
using namespace ::rwp;      // the recompiler's vector lowering
#endif

struct PoseMatrix44 { RWP_VEC r0, r1, r2, r3; };   // rw::math::vpu::Matrix44
struct PoseParent { RWP_S32 index; };

// a * b for affine rows: out.k = a.r0*b.k.x + a.r1*b.k.y + a.r2*b.k.z (+ a.r3 for row 3)
static __forceinline void PoseMul(RWP_PTR(PoseMatrix44) out, __vector4 a0, __vector4 a1, __vector4 a2,
                                  __vector4 a3, __vector4 b0, __vector4 b1, __vector4 b2, __vector4 b3)
{
    __vector4 r0 = __vmaddfp(a2, __vspltw(b0, 2), __vmaddfp(a1, __vspltw(b0, 1), __vmulfp(a0, __vspltw(b0, 0))));
    __vector4 r1 = __vmaddfp(a2, __vspltw(b1, 2), __vmaddfp(a1, __vspltw(b1, 1), __vmulfp(a0, __vspltw(b1, 0))));
    __vector4 r2 = __vmaddfp(a2, __vspltw(b2, 2), __vmaddfp(a1, __vspltw(b2, 1), __vmulfp(a0, __vspltw(b2, 0))));
    __vector4 r3 = __vmaddfp(a2, __vspltw(b3, 2), __vmaddfp(a1, __vspltw(b3, 1), __vmaddfp(a0, __vspltw(b3, 0), a3)));
    STOREV(out->r0, r0);
    STOREV(out->r1, r1);
    STOREV(out->r2, r2);
    STOREV(out->r3, r3);
}

// rw::math Vector4 with a w setter: the object is homed in memory, w written as a float
static __forceinline __vector4 PoseWithW1(__vector4 v)
{
#if defined(RWP_GUEST)
    VLANE_SETW(v, 1.0f);
    return v;
#else
    union { __vector4 v; float f[4]; } t;
    t.v = v;
    t.f[3] = 1.0f;
    return t.v;
#endif
}

void PoseWithMatMult(int numBones, RWP_PTR(PoseParent) parentBones, int trajNode, RWP_PTR(PoseMatrix44) GM,
                     RWP_PTR(const AnimCmdSys::SQTDataInternal) sqt, RWP_PTR(PoseMatrix44) matMult,
                     RWP_PTR(PoseMatrix44) workBuffer)
{
    RWP_PTR(PoseMatrix44) L = workBuffer;
    if (!workBuffer)
        L = GM;

    // 1. local matrices, four bones per pass
    unsigned int passes = (unsigned int)numBones >> 2;
    if (((unsigned int)numBones & ~3u) < (unsigned int)numBones)
        passes++;
    // software-pipelined like the original: the next four bones are loaded one pass ahead
    // (the last pass reads one group past the end, as the original does)
    RWP_PTR(const AnimCmdSys::SQTDataInternal) s = sqt;
    __vector4 nS0 = LOADV(s[0].mScale), nQ0 = LOADV(s[0].mQuat), nT0 = LOADV(s[0].mTrans);
    __vector4 nS1 = LOADV(s[1].mScale), nQ1 = LOADV(s[1].mQuat), nT1 = LOADV(s[1].mTrans);
    __vector4 nS2 = LOADV(s[2].mScale), nQ2 = LOADV(s[2].mQuat), nT2 = LOADV(s[2].mTrans);
    __vector4 nS3 = LOADV(s[3].mScale), nQ3 = LOADV(s[3].mQuat), nT3 = LOADV(s[3].mTrans);
    if (passes)
    {
        const __vector4 one = __vcfux(__vspltisw(1), 0);
        const __vector4 two = __vcfux(__vspltisw(2), 0);
        const __vector4 zero = __vcfux(__vspltisw(0), 0);
        for (unsigned int p = 0; p < passes; p++)
        {
            __vector4 s0 = nS0, q0 = nQ0, t0 = nT0, s1 = nS1, q1 = nQ1, t1 = nT1;
            __vector4 s2 = nS2, q2 = nQ2, t2 = nT2, s3 = nS3, q3 = nQ3, t3 = nT3;
            s = s + 4;
            nS0 = LOADV(s[0].mScale); nQ0 = LOADV(s[0].mQuat); nT0 = LOADV(s[0].mTrans);
            nS1 = LOADV(s[1].mScale); nQ1 = LOADV(s[1].mQuat); nT1 = LOADV(s[1].mTrans);
            nS2 = LOADV(s[2].mScale); nQ2 = LOADV(s[2].mQuat); nT2 = LOADV(s[2].mTrans);
            nS3 = LOADV(s[3].mScale); nQ3 = LOADV(s[3].mQuat); nT3 = LOADV(s[3].mTrans);
            __dcbt(0x200, &*s);
            RWP_PTR(PoseMatrix44) m = L + p * 4;
            __dcbz128(0x100, &*m);
            // transpose the four bones: lane j = bone j
            __vector4 qa = __vmrghw(q0, q2), qb = __vmrghw(q1, q3), qc = __vmrglw(q0, q2), qd = __vmrglw(q1, q3);
            __vector4 X = __vmrghw(qa, qb), Y = __vmrglw(qa, qb), Z = __vmrghw(qc, qd), W = __vmrglw(qc, qd);
            __vector4 sa = __vmrghw(s0, s2), sb = __vmrghw(s1, s3);
            __vector4 Sx = __vmrghw(sa, sb), Sy = __vmrglw(sa, sb);
            __vector4 Sz = __vmrghw(__vmrglw(s0, s2), __vmrglw(s1, s3));

            __vector4 xx = __vmulfp(X, X), yy = __vmulfp(Y, Y), zz = __vmulfp(Z, Z);
            __vector4 xy = __vmulfp(X, Y), xz = __vmulfp(X, Z), yz = __vmulfp(Y, Z);
            __vector4 wx = __vmulfp(W, X), wy = __vmulfp(W, Y), wz = __vmulfp(W, Z);
            __vector4 m00 = __vmulfp(__vnmsubfp(__vaddfp(yy, zz), two, one), Sx);
            __vector4 m01 = __vmulfp(__vmulfp(__vaddfp(xy, wz), two), Sx);
            __vector4 m02 = __vmulfp(__vmulfp(__vsubfp(xz, wy), two), Sx);
            __vector4 m10 = __vmulfp(__vmulfp(__vsubfp(xy, wz), two), Sy);
            __vector4 m11 = __vmulfp(__vnmsubfp(__vaddfp(xx, zz), two, one), Sy);
            __vector4 m12 = __vmulfp(__vmulfp(__vaddfp(yz, wx), two), Sy);
            __vector4 m20 = __vmulfp(__vmulfp(__vaddfp(xz, wy), two), Sz);
            __vector4 m21 = __vmulfp(__vmulfp(__vsubfp(yz, wx), two), Sz);
            __vector4 m22 = __vmulfp(__vnmsubfp(__vaddfp(xx, yy), two, one), Sz);

            // back to one row per bone: (mk0, mk1, mk2, 0)
            __vector4 a, b;
            a = __vmrghw(m00, m02); b = __vmrghw(m01, zero);
            STOREV(m[0].r0, __vmrghw(a, b)); STOREV(m[1].r0, __vmrglw(a, b));
            a = __vmrglw(m00, m02); b = __vmrglw(m01, zero);
            STOREV(m[2].r0, __vmrghw(a, b)); STOREV(m[3].r0, __vmrglw(a, b));
            a = __vmrghw(m10, m12); b = __vmrghw(m11, zero);
            STOREV(m[0].r1, __vmrghw(a, b)); STOREV(m[1].r1, __vmrglw(a, b));
            a = __vmrglw(m10, m12); b = __vmrglw(m11, zero);
            STOREV(m[2].r1, __vmrghw(a, b)); STOREV(m[3].r1, __vmrglw(a, b));
            a = __vmrghw(m20, m22); b = __vmrghw(m21, zero);
            STOREV(m[0].r2, __vmrghw(a, b)); STOREV(m[1].r2, __vmrglw(a, b));
            a = __vmrglw(m20, m22); b = __vmrglw(m21, zero);
            STOREV(m[2].r2, __vmrghw(a, b)); STOREV(m[3].r2, __vmrglw(a, b));
            // row 3: the translation with w = 1 (a float store into the vector's w)
            STOREV(m[0].r3, PoseWithW1(t0));
            STOREV(m[1].r3, PoseWithW1(t1));
            STOREV(m[2].r3, PoseWithW1(t2));
            STOREV(m[3].r3, PoseWithW1(t3));
        }
    }

    // 2. hierarchy
    RWP_PTR(PoseMatrix44) G = workBuffer;
    if (!workBuffer)
        G = GM;
    if (!matMult)
        G = GM;
    STOREV(G[0].r0, LOADV(L[0].r0));
    STOREV(G[0].r1, LOADV(L[0].r1));
    STOREV(G[0].r2, LOADV(L[0].r2));
    STOREV(G[0].r3, LOADV(L[0].r3));
    for (int i = 0; i < numBones; i++)
    {
        __dcbt(0x100, &L[i]);
        __dcbt(0x100, &G[i]);
        int parent = parentBones[i].index;
        __vector4 l0 = LOADV(L[i].r0), l1 = LOADV(L[i].r1), l2 = LOADV(L[i].r2), l3 = LOADV(L[i].r3);
        if (parent == -1 || parent == trajNode)
        {
            STOREV(G[i].r0, l0);
            STOREV(G[i].r1, l1);
            STOREV(G[i].r2, l2);
            STOREV(G[i].r3, l3);
            continue;
        }
        RWP_PTR(PoseMatrix44) P = G + parent;
        PoseMul(G + i, LOADV(P->r0), LOADV(P->r1), LOADV(P->r2), LOADV(P->r3), l0, l1, l2, l3);
    }

    // 3. post-multiply
    if (matMult)
    {
        for (int i = 0; i < numBones; i++)
        {
            RWP_PTR(PoseMatrix44) g = G + i;
            RWP_PTR(PoseMatrix44) mm = matMult + i;
            PoseMul(GM + i, LOADV(g->r0), LOADV(g->r1), LOADV(g->r2), LOADV(g->r3),
                    LOADV(mm->r0), LOADV(mm->r1), LOADV(mm->r2), LOADV(mm->r3));
        }
    }
}

} }
