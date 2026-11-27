// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// game/source/presentation/presentationentity.cpp (next to the sk83 assert path Presentation/
// PresentationEntity.h; the function itself has no name in the map or the PDB):
//   entity effect technique + per-mesh state bytes     sk83_na_m 0x82929c40 (0x110)
// Verified with sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
namespace Render { void ModelInstance_SetTechnique(unsigned int inst, unsigned int technique); }

// A presentation entity keeps per LOD (+0x10): its technique (+(lod + 0x52) * 4), instance count
// (+(lod + 5) * 4) and instances (+(lod + 8) * 4, 0x28 bytes each), and a mesh list (+lod * 16 +
// 0x2c) with a parallel state table (+lod * 16 + 0x5c, 8 bytes per mesh). A new technique goes to
// every instance's materials; every mesh's state word becomes its effect's four state bytes
// (+0x38, +0x3c, +0x44, +0x40 -> bytes 0..3).
void PresentationEntity_SetTechnique(unsigned int ent, unsigned int technique)
{
    RWP_GUEST_FRAME(frame, 0x80);
    unsigned int lod = RWP_GLOBAL(RWP_U32, ent + 0x10u);
    if (technique == RWP_LD32(ent + 4u * (lod + 0x52u)))
        return;
    if (RWP_LD32(ent + 4u * (lod + 5u)) != 0)
    {
        unsigned int i = 0, off = 0;
        do
        {
            Render::ModelInstance_SetTechnique(RWP_LD32(ent + 4u * (lod + 8u)) + off, technique);
            const unsigned int list = ent + 16u * RWP_LD32(ent + 0x10u);
            const unsigned int n = (unsigned int)((int)(RWP_LD32(list + 0x30u) - RWP_LD32(list + 0x2cu)) >> 2);
            for (unsigned int k = 0, t = 0; k < n; k++, t += 8u)
            {
                const unsigned int mc = RWP_LD32(RWP_LD32(list + 0x2cu) + 4u * k);
                const unsigned int table = RWP_LD32(ent + 16u * RWP_LD32(ent + 0x10u) + 0x5cu);
                const unsigned int fx = RWP_LD32(mc + 4u);
                RWP_ST8(FRAME_AT(frame, 0x50), RWP_LD32(fx + 0x38u));
                RWP_ST8(FRAME_AT(frame, 0x52), RWP_LD32(fx + 0x44u));
                RWP_ST8(FRAME_AT(frame, 0x51), RWP_LD32(fx + 0x3cu));
                RWP_ST8(FRAME_AT(frame, 0x53), RWP_LD32(fx + 0x40u));
                RWP_ST32(table + t, RWP_LD32(FRAME_AT(frame, 0x50)));
            }
            lod = RWP_GLOBAL(RWP_U32, ent + 0x10u);
            i++;
            off += 0x28u;
        } while (i < RWP_LD32(ent + 4u * (lod + 5u)));
    }
    RWP_ST32(ent + 4u * (lod + 0x52u), technique);
}

}
