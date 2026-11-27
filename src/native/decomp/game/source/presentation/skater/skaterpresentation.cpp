// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// game/source/presentation/skater/skaterpresentation.cpp (sk83 assert path of the caller): the
// skater presentation's replay / restore posing.
//   SkaterPresentation: relative SQTs onto a part's bones   sk83_na_m 0x829068d0 (0xc8)
// The helper has no assert of its own; its only caller is the skaterpresentation.cpp function at
// 0x82914980 (asserts at lines 1087 / 1100 / 1111), and it is not in Skate 2 (new in sk83).
// Not yet replay-verified (snapshot set: restoreframe / restoreframe1).
#include "rwp_guestcall.h"

namespace Sk8 {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

// AnimCmdSys::SQTExt (0x30): mScale +0x00, mQuat +0x10, mTrans +0x20

// For n bones: dst.mTrans += src.mQuat rotating t[i]; dst.mQuat = src.mQuat * q[i]. src / dst are
// SQTExt arrays (already offset to the part's first bone), q / t packed 16-byte arrays. Returns n
// (the caller keeps using r3).
unsigned int SkaterPresentation_ApplyRelativeSQTs(unsigned int n, unsigned int src, unsigned int dst,
                                                  unsigned int q, unsigned int t)
{
    if (n == 0)
        return n;
    RWP_VMX_MODE();
    const __vector4 two = __vcfsx(__vspltisw(2), 0);
    unsigned int sq = src + 0x10u, dt = dst + 0x20u;
    for (unsigned int i = n; i; i--)
    {
        // v' = v + 2 * (r x (w v + r x v)), r = the rotation's xyz, w its w
        const __vector4 v = RWP_GLOBAL(RWP_VEC, t);
        t += 16u;
        __vector4 r = RWP_GLOBAL(RWP_VEC, sq);
        const __vector4 rp = __vpermwi(r, 0x63);
        const __vector4 rw = __vspltw(r, 3);
        const __vector4 trans = RWP_GLOBAL(RWP_VEC, dt);
        const __vector4 c1 = __vmaddfp(rw, v, __vpermwi(__vnmsubfp(rp, v, __vmulfp(r, __vpermwi(v, 0x63))), 0x63));
        const __vector4 c2 = __vpermwi(__vnmsubfp(rp, c1, __vmulfp(r, __vpermwi(c1, 0x63))), 0x63);
        RWP_GLOBAL(RWP_VEC, dt) = __vaddfp(trans, __vmaddfp(two, c2, v));

        // quaternion product r * b (reloaded: the store above may alias it)
        r = RWP_GLOBAL(RWP_VEC, sq);
        sq += 0x30u;
        const __vector4 b = RWP_GLOBAL(RWP_VEC, q);
        const __vector4 bw = __vspltw(b, 3);
        const __vector4 dot = __vmsum3fp(r, b);
        const __vector4 aw = __vspltw(r, 3);
        q += 16u;
        const __vector4 w = __vsubfp(__vmulfp(aw, bw), dot);
        const __vector4 x = __vpermwi(__vnmsubfp(__vpermwi(r, 0x63), b, __vmulfp(r, __vpermwi(b, 0x63))), 0x63);
        RWP_GLOBAL(RWP_VEC, dt - 0x10u) = __vrlimi(__vmaddfp(r, bw, __vmaddfp(b, aw, x)), w, 1, 1);
        dt += 0x30u;
    }
    return n;
}

}  // namespace Sk8
