// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (1); comments keep the dev addresses.
// game/source/scene/sys/scenemanagerimpl.cpp (sk83 assert path): the scene manager's frame update.
//   scene asset loaders' per-frame update   sk83_na_m 0x829c6fe8 (0xe0; no name in the map; called
//                                           by SceneManagerImpl::UpdateDeltaFrameBegin)
// Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"

namespace Sk8 { namespace Scene {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
void AnimPoolLoader_Update(unsigned int self);
#endif

// The anim pool loader (+0x24) first; then every loader in the hash map at +8 (buckets +8, count
// +0xc; a node's value at +4 is a vector of loaders, begin/end): a loader whose state (vtable +4)
// is 1 gets its update (vtable +0x14).
void SceneAssetLoaders_Update(unsigned int self)
{
    RWP_SAVEGPRLR(26);
    RWP_GUEST_FRAME(frame, 0x90);
    RWP_NV(31, self);
    RWP_CALL(0x828269f0, AnimPoolLoader_Update, self + 0x24u);
    unsigned int bucket = RWP_LD32(self + 8u);
    unsigned int node = RWP_LD32(bucket);
    while (!node)
    {
        bucket += 4u;
        node = RWP_LD32(bucket);
    }
    const unsigned int end = RWP_LD32(RWP_LD32(self + 8u) + 4u * RWP_LD32(self + 0xcu));
    RWP_NV(27, bucket);
    RWP_NV(26, end);
    while (node != end)
    {
        const unsigned int vec = RWP_LD32(node + 4u);
        RWP_NV(29, node);
        RWP_NV(28, vec);
        for (unsigned int it = RWP_LD32(vec); it != RWP_LD32(vec + 4u); it += 4u)
        {
            const unsigned int loader = RWP_LD32(it);
            RWP_NV(31, it);
            RWP_NV(30, loader);
            if ((int)RWP_VCALL(loader, 4) == 1)
                RWP_VCALL(loader, 0x14);
        }
        node = RWP_LD32(node + 8u);
        while (!node)
        {
            bucket += 4u;
            node = RWP_LD32(bucket);
        }
        RWP_NV(27, bucket);
    }
}

} }
