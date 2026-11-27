// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 833c31a8 833c31ac
// packages/skate/rendering/sk8shader/dev/source/materialman.cpp (the sk8shader package; Skate 2's
// cEffectScope::DispatchEffect lives in materialman.cpp -- sk83 has no assert string here, so the
// file is taken from Skate 2): binding an effect's programs and parameters.
//   effect dispatch                         sk83_na_m 0x82f46de0
//   reset of the "current" effect slots     0x82f46cf8
//   effect begin (full bind)                0x82f46d18
// sk83's effect objects (layout from use): +0x0c/+0x10/+0x14 the three bind arguments,
// +0x48/+0x4c/+0x50 the three bind functions, vtable +0x0c Begin(bool), +0x10 the parameter
// dispatch. The first two binds are skipped when the same program is already current.
// Verified with sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace ShaderSystem {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define s_CurrentProgram0   RWP_GLOBAL(RWP_U32, 0x5fff0000)   // the +0x48 bind function last called
#define s_CurrentProgram1   RWP_GLOBAL(RWP_U32, 0x5fff0000)   // the +0x10 argument last bound

struct EffectBinding
{
    RWP_U32 vtable;                     // 0x00
    RWP_U32 pad04[2];
    RWP_U32 m_Arg0;                     // 0x0c
    RWP_U32 m_Arg1;                     // 0x10
    RWP_U32 m_Arg2;                     // 0x14
    RWP_U32 pad18[12];
    RWP_U32 m_pfnBind0;                 // 0x48
    RWP_U32 m_pfnBind1;                 // 0x4c
    RWP_U32 m_pfnBind2;                 // 0x50
};

#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
#endif

// sub_82f46cf8
void ResetCurrentEffect()
{
    s_CurrentProgram0 = 0u;
    s_CurrentProgram1 = 0u;
}

// sub_82f46de0
void DispatchEffect(RWP_PTR(EffectBinding) e)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (e->m_pfnBind0 != s_CurrentProgram0)
    {
        RWP_VCALL(RWP_ADDR_OF(e), 0x0c, 1u);
        RWP_FNCALL((unsigned int)e->m_pfnBind0, (unsigned int)e->m_Arg0, 0u, 0u);
        s_CurrentProgram0 = e->m_pfnBind0;
    }
    if (e->m_Arg1 != s_CurrentProgram1)
    {
        RWP_FNCALL((unsigned int)e->m_pfnBind1, (unsigned int)e->m_Arg1, 0u, 0u);
        s_CurrentProgram1 = e->m_Arg1;
    }
    RWP_FNCALL((unsigned int)e->m_pfnBind2, (unsigned int)e->m_Arg2, 0u, 0u);
    RWP_VCALL(RWP_ADDR_OF(e), 0x10);
}


// sub_82f46d18: Begin(bit 1 of flags), then -- with bit 0 -- all three binds unconditionally, the
// parameter dispatch, and no "current" program any more (the next DispatchEffect binds again)
void BeginEffect(RWP_PTR(EffectBinding) e, unsigned int flags)
{
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_VCALL(RWP_ADDR_OF(e), 0x0c, (flags >> 1) & 1u);
    if (flags & 1u)
    {
        RWP_FNCALL((unsigned int)e->m_pfnBind0, (unsigned int)e->m_Arg0, 0u, 0u);
        RWP_FNCALL((unsigned int)e->m_pfnBind1, (unsigned int)e->m_Arg1, 0u, 0u);
        RWP_FNCALL((unsigned int)e->m_pfnBind2, (unsigned int)e->m_Arg2, 0u, 0u);
    }
    RWP_VCALL(RWP_ADDR_OF(e), 0x10);
    s_CurrentProgram0 = 0u;
    s_CurrentProgram1 = 0u;
}

} }
