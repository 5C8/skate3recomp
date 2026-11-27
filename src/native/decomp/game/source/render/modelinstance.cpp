// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// game/source/render/modelinstance.cpp (sk83 assert path): model instances.
//   technique + pass 0 on every material of an instance     sk83_na_m 0x82975a78 (0x8c)
// (the function has no name in the map or the PDB; named after what it does). Verified with
// sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
#endif

// An instance: +0 its model (+0x32: material count, u16), +4 its materials (0x4c bytes each, the
// effect at +4). Every effect gets the technique (vtable +4) and pass 0 (vtable +8).
void ModelInstance_SetTechnique(unsigned int inst, unsigned int technique)
{
    RWP_GUEST_FRAME(frame, 0x80);
    if (!(RWP_LD32(RWP_GLOBAL(RWP_U32, inst) + 0x30u) & 0xffffu))
        return;
    int i = 0;
    unsigned int off = 0;
    do
    {
        RWP_VCALL(RWP_GLOBAL(RWP_U32, (unsigned int)RWP_GLOBAL(RWP_U32, inst + 4u) + off + 4u), 4, technique);
        RWP_VCALL(RWP_GLOBAL(RWP_U32, (unsigned int)RWP_GLOBAL(RWP_U32, inst + 4u) + off + 4u), 8, 0u);
        i++;
        off += 0x4cu;
    } while (i < (int)(RWP_LD32(RWP_GLOBAL(RWP_U32, inst) + 0x30u) & 0xffffu));
}

} }
