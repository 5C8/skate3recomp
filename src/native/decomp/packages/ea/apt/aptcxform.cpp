// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82123e80
// EA Apt: AptColorHelper (Skate 2 PDB: packages/ea/apt/2.07.01/source/apt/aptobject/aptcxform.cpp;
// the library has no asserts in sk83, so no sk83 path -- filed under the package name).
//   AptColorHelper::CopyToFloatArray4(float *) const   sk83_na_m 0x83154b28 (0x80, leaf)
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define kAptColorByteScale 0x5fff0000u   // 1/255

// The colour's four bytes (+4 .. +7) as floats in [0, 1]. Each byte goes through the stack for
// fcfid (std rX, -0x10(r1)); the last one written is the fourth byte, left there as it is.
void AptColorHelper_CopyToFloatArray4(unsigned int self, unsigned int out)
{
    RWP_FPU_MODE();
    const float scale = RWP_GLOBAL(RWP_F32, kAptColorByteScale);
    const unsigned int c0 = RWP_LD8(self + 4u), c1 = RWP_LD8(self + 5u), c2 = RWP_LD8(self + 6u);
    RWP_GLOBAL(RWP_F32, out) = (float)c0 * scale;
    RWP_GLOBAL(RWP_F32, out + 4u) = (float)c1 * scale;
    RWP_GLOBAL(RWP_F32, out + 8u) = (float)c2 * scale;
    const unsigned int c3 = RWP_LD32(self + 4u) & 0xffu;
    RWP_GLOBAL(RWP_F32, out + 0xcu) = (float)c3 * scale;
#if defined(RWP_GUEST)
    GStore64(g_ctx->r1.u32 - 0x10u, (uint64_t)c3);                       // std r10, -0x10(r1)
#endif
}
