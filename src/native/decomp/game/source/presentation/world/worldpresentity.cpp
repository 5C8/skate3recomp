// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (2); comments keep the dev addresses.
// game/source/presentation/world/worldpresentity.cpp (sk83 assert path): world presentation
// entities.
//   Sk8::WorldPresEntityOptimesh::GetShadowCulledMeshList   sk83_na_m 0x8291c998 (0x130)
// Verified with sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int VectorDoInsertValue(...);
#endif

// The current LOD's (+0x10) shadow mesh list (+lod * 16 + 0x8c, rebuilt): every mesh (+lod * 16 +
// 0x2c) with at least one shadow-visible island. A mesh's island flags (+0x38, +0x20 of them) are
// compacted in place into the indices of the visible ones, counted in +0x28.
unsigned int WorldPresEntityOptimesh_GetShadowCulledMeshList(unsigned int self)
{
    RWP_GUEST_FRAME(frame, 0x80);
    {
        const unsigned int v = self + 16u * RWP_LD32(self + 0x10u) + 0x8cu;
        RWP_ST32(v + 4u, RWP_LD32(v));
    }
    const unsigned int l0 = self + 16u * RWP_LD32(self + 0x10u);
    const unsigned int n = (unsigned int)((int)(RWP_LD32(l0 + 0x30u) - RWP_LD32(l0 + 0x2cu)) >> 2);
    for (unsigned int k = 0; k < n; k++)
    {
        const unsigned int lst = self + 16u * RWP_LD32(self + 0x10u);
        const unsigned int mc = RWP_LD32(RWP_LD32(lst + 0x2cu) + 4u * k);
        const unsigned int cnt = RWP_LD32(mc + 0x20u);
        unsigned int src = RWP_LD32(mc + 0x38u);
        RWP_ST32(FRAME_AT(frame, 0x50), mc);
        RWP_ST32(mc + 0x28u, 0u);
        bool any = false;
        unsigned int dst = src - 4u;
        for (unsigned int i = 0; i < cnt; i++, src += 4u)
        {
            if ((int)RWP_LD32(src) != 0)
            {
                dst += 4u;
                RWP_ST32(dst, i);
                any = true;
                RWP_ST32(mc + 0x28u, RWP_LD32(mc + 0x28u) + 1u);
            }
        }
        if (any)
        {
            const unsigned int v = self + 16u * RWP_LD32(self + 0x10u) + 0x8cu;
            const unsigned int endp = RWP_LD32(v + 4u);
            if (endp < RWP_LD32(v + 8u))
            {
                RWP_ST32(v + 4u, endp + 4u);
                if (endp)
                    RWP_ST32(endp, mc);
            }
            else
                RWP_CALL(0x82ed5ca8, VectorDoInsertValue, v, endp, FRAME_AT(frame, 0x50));
        }
    }
    return self + 16u * RWP_LD32(self + 0x10u) + 0x8cu;
}

#if !defined(RWP_GUEST)
void SortKeyVector_DoInsertValue(unsigned int vec, unsigned int pos, unsigned int value);
#endif

// Sk8::WorldPresEntityOptimesh::FillMtlSrtKeyListWithCulledKeys(xvector<SORT_KEY_TYPE> &)
// sk83_na_m 0x8291a2a0 (0x110; vtable +0x18, called by SceneRenderView::Sort): every mesh of the
// current LOD (+lod * 16 + 0x2c) gets its island visibility flags (+0x34, +0x20 of them)
// compacted in place into the indices of the visible ones (count in +0x24); a mesh with at least
// one visible island adds its precomputed sort key (+lod * 16 + 0x5c, 8 bytes per mesh; the LOD
// read on entry) to the list. (Skate 2 waited for the entity's cull job first; sk83 does not.)
void WorldPresEntityOptimesh_FillMtlSrtKeyListWithCulledKeys(unsigned int self, unsigned int keys)
{
    RWP_SAVEGPRLR(25);
    RWP_GUEST_FRAME(frame, 0x90);
    const unsigned int lod = self + 16u * RWP_LD32(self + 0x10u);
    const unsigned int n = (unsigned int)((int)(RWP_LD32(lod + 0x30u) - RWP_LD32(lod + 0x2cu)) >> 2);
    if (n == 0)
        return;
    for (unsigned int k = 0; k < n; k++)
    {
        const unsigned int lst = self + 16u * RWP_LD32(self + 0x10u);
        const unsigned int mc = RWP_LD32(RWP_LD32(lst + 0x2cu) + 4u * k);
        const unsigned int cnt = RWP_LD32(mc + 0x20u);
        unsigned int src = RWP_LD32(mc + 0x34u);
        RWP_ST32(mc + 0x24u, 0u);
        bool any = false;
        unsigned int dst = src - 4u;
        for (unsigned int i = 0; i < cnt; i++, src += 4u)
        {
            if ((int)RWP_LD32(src) != 0)
            {
                dst += 4u;
                RWP_ST32(dst, i);
                any = true;
                RWP_ST32(mc + 0x24u, RWP_LD32(mc + 0x24u) + 1u);
            }
        }
        if (any)
        {
            const unsigned int key = RWP_LD32(lod + 0x5cu) + 8u * k;
            const unsigned int endp = RWP_LD32(keys + 4u);
            if (endp < RWP_LD32(keys + 8u))
            {
                RWP_ST32(keys + 4u, endp + 8u);
                if (endp)
                {
                    RWP_ST32(endp, RWP_LD32(key));
                    RWP_ST32(endp + 4u, RWP_LD32(key + 4u));
                }
            }
            else
            {
                RWP_NV(25, n - k);
                RWP_NV(26, 0u);
                RWP_NV(27, lod);
                RWP_NV(28, 8u * k);
                RWP_NV(29, keys);
                RWP_NV(30, 4u * k);
                RWP_NV(31, self);
                RWP_CALL(0x8296dd10, SortKeyVector_DoInsertValue, keys, endp, key);
            }
        }
    }
}

}
