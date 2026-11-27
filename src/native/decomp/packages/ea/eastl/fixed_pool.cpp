// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// EASTL 1.10.01 source/fixed_pool.cpp (the Skate 2 PDB puts it in the EASTL package's runtime
// object; sk83 has no path string for it):
//   eastl::fixed_pool_base::init(void *pMemory, size_t memorySize, size_t nodeSize,
//                                size_t alignment, size_t alignmentOffset)   sk83_na_m 0x82d173b8 (0x78)
// Links the pool's nodes into the free list: mpHead (+0) = the first aligned node, each node's
// first word -> the next one, the last one -> 0. alignmentOffset is not used. The original traps
// (twllei) on a zero node size, which only an alignment above 0xfffffffc can produce: the hook
// leaves that case to the recompiled code. Every fixed_* EASTL container constructor runs it
// (KDTree::FindNear's 256-node result set builds one per call).
#include "rwp_guestcall.h"

namespace eastl {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

void fixed_pool_base_init(unsigned int self, unsigned int pMemory, unsigned int memorySize, unsigned int nodeSize,
                          unsigned int alignment, unsigned int /*alignmentOffset*/)
{
    if (!pMemory)
        return;
    if (alignment < 1u)
        alignment = 1u;
    if (nodeSize < 4u)
        nodeSize = (alignment + 3u) & ~(alignment - 1u);
    // the last node starts at pMemory (unaligned) + (count - 1) * nodeSize
    const unsigned int pLast = pMemory + (memorySize / nodeSize - 1u) * nodeSize;
    unsigned int p = (pMemory + alignment - 1u) & ~(alignment - 1u);
    RWP_ST32(self, p);
    while (p < pLast)
    {
        const unsigned int next = p + nodeSize;
        RWP_ST32(p, next);
        p = next;
    }
    RWP_ST32(p, 0u);
}

}   // namespace eastl
