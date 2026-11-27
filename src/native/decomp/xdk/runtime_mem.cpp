// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// Memory copies of the XDK runtime / CRT / rw core that the render thread (and everything else)
// calls, as native copies on guest memory:
//   XMemCpy                        sk83_na_m 0x82dae9c0 (XDK: _blkmov for short copies, then
//                                  128-byte vector blocks with dcbz on whole destination lines)
//   memcpy                         0x83190ec0 (CRT)
//   rw::core::stdc::MemCopy        0x82ce6ab0 (bytes, then words, then bytes, forward)
// For non-overlapping ranges every one of them leaves exactly dst = src in the destination (the
// dcbz lines lie inside it -- the replay compare checks that). Below the caller's stack pointer
// they leave more, and game code reads it: an uninitialized local in the anim job setup
// (Anim::AnimJobScheduler, under DataSysMan::RestoreFrame) picks up the dst memcpy saved at
// r1-8. So the native versions make the same stack writes first (MemcpyEntry / XMemCpyEntry);
// rw MemCopy is a leaf that writes none. With those, a native copy is exact. Overlapping ranges (the result would depend on each routine's chunk order) and ranges
// that cross into the 0xe0000000 physical view keep the recompiled code (the hook falls back).
// Not original game sources: these live in Microsoft's / RenderWare's runtime libraries.
#include "rwp_guestcall.h"
#include <string.h>

namespace RwpRuntime {

#if defined(RWP_GUEST)
using namespace ::rwp;

// guest address -> host pointer (the 0xe0000000 view sits one 4 KB page further, see
// rex::memory::GuestPtr)
static inline uint8_t *Host(u32 a) { return g_base + a + (a >= 0xe0000000u ? 0x1000u : 0u); }

// memcpy's first instruction: std r3, -8(r1)
inline void MemcpyEntry(PPCContext &c)
{
    GStore64(c.r1.u32 - 8u, c.r3.u64);
}

// XMemCpy's prologue: mflr r12; bl __savegprlr_26 (r26..r31 at r1-56..r1-16, r12 at r1-8);
// stwu r1, -176(r1). Its leaf callees (_blkmov, CopyMisalignedCachelines) use no stack.
inline void XMemCpyEntry(PPCContext &c)
{
    const u32 sp = c.r1.u32;
    GStore64(sp - 56u, c.r26.u64);
    GStore64(sp - 48u, c.r27.u64);
    GStore64(sp - 40u, c.r28.u64);
    GStore64(sp - 32u, c.r29.u64);
    GStore64(sp - 24u, c.r30.u64);
    GStore64(sp - 16u, c.r31.u64);
    GStore32(sp - 8u, (u32)c.lr);
    GStore32(sp - 176u, sp);
}

// true when the copy was done natively
inline bool Copy(u32 dst, u32 src, u32 n)
{
    if (n == 0)
        return true;
    const unsigned long long d = dst, s = src, e = (unsigned long long)n;
    if (d < s + e && s < d + e)
        return false;                                   // overlapping
    if (d + e > 0x100000000ull || s + e > 0x100000000ull)
        return false;
    if ((dst < 0xe0000000u) != ((u32)(d + e - 1) < 0xe0000000u) || (src < 0xe0000000u) != ((u32)(s + e - 1) < 0xe0000000u))
        return false;                                   // crosses into the shifted view
    memcpy(Host(dst), Host(src), n);
    return true;
}
#endif

}
