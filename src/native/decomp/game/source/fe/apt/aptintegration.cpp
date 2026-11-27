// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (1); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 82137170
// game/source/fe/apt/aptintegration.cpp (sk83 assert path, line 398): Sk8::FE::AptIntegration.
//   Sk8::FE::AptIntegration::SetAptMatrix(Matrix44 &, AptMatrix *)   sk83_na_m 0x8273b450 (0x9c)
// Skate 2: same function (sk82 0x826018xx area, aptintegration.cpp). Written from the sk83
// recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace FE {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define kAptMatZero    0x5fff0000u   // 0.0f
#define kAptMatOne     0x8231a844u   // 1.0f
#define kAptMatRow2    0x5fff0000u   // (0, 0, 1, 0)

// One 16-byte row: lvx from a stack temporary / constant, stvx into the matrix (bytes as they are).
static __forceinline void AptMatCopy16(unsigned int dst, unsigned int src)
{
    RWP_ST64(dst, RWP_LD64(src));
    RWP_ST64(dst + 8u, RWP_LD64(src + 8u));
}

// Apt's 2D affine (a b c d tx ty) -> a 4x4 row matrix:
//   (a, b, 0, 0) (c, d, 0, 0) (0, 0, 1, 0) (tx, ty, 0, 1).
// The rows are built in two stack temporaries (+0x60 twice, +0x50) with lfs/stfs, then stored
// with stvx -- the temporaries are reproduced (later code can read stale stack).
void AptIntegration_SetAptMatrix(unsigned int matrix, unsigned int pMatrix)
{
    RWP_SAVEGPRLR(30);
    RWP_GUEST_FRAME(frame, 0x90);
    RWP_NV(30, matrix);
    RWP_NV(31, pMatrix);
    // if (!pMatrix) FailAssert (line 398)
    RWP_FPU_MODE();
    const float zero = RWP_GLOBAL(RWP_F32, kAptMatZero);
    const unsigned int t60 = FRAME_AT(frame, 0x60), t50 = FRAME_AT(frame, 0x50);
    RWP_GLOBAL(RWP_F32, t60) = (float)RWP_GLOBAL(RWP_F32, pMatrix);
    RWP_GLOBAL(RWP_F32, t60 + 4u) = (float)RWP_GLOBAL(RWP_F32, pMatrix + 4u);
    RWP_GLOBAL(RWP_F32, t60 + 8u) = zero;
    RWP_GLOBAL(RWP_F32, t60 + 0xcu) = zero;
    AptMatCopy16(matrix, t60);                                              // row 0 (v62)
    RWP_GLOBAL(RWP_F32, t60) = (float)RWP_GLOBAL(RWP_F32, pMatrix + 8u);
    RWP_GLOBAL(RWP_F32, t60 + 4u) = (float)RWP_GLOBAL(RWP_F32, pMatrix + 0xcu);
    RWP_GLOBAL(RWP_F32, t60 + 8u) = zero;
    RWP_GLOBAL(RWP_F32, t60 + 0xcu) = zero;
    RWP_GLOBAL(RWP_F32, t50) = (float)RWP_GLOBAL(RWP_F32, pMatrix + 0x10u);
    RWP_GLOBAL(RWP_F32, t50 + 4u) = (float)RWP_GLOBAL(RWP_F32, pMatrix + 0x14u);
    RWP_GLOBAL(RWP_F32, t50 + 8u) = zero;
    RWP_GLOBAL(RWP_F32, t50 + 0xcu) = (float)RWP_GLOBAL(RWP_F32, kAptMatOne);
    AptMatCopy16(matrix + 0x20u, kAptMatRow2);
    AptMatCopy16(matrix + 0x30u, t50);
    AptMatCopy16(matrix + 0x10u, t60);
}

} }
