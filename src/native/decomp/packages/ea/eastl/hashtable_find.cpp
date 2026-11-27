// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// eastl::hashtable<...>::find instantiations the render thread calls (EASTL's
// include/eastl/internal/hashtable.h, compiled into the game's TUs; sk83 has no path for them):
//   hashtable<unsigned short, pair<const unsigned short, int>, ...>::find   sk83_na_m 0x82b6dcb0
//   hashtable<eSk8FEScreenState, pair<...>, ...>::find (an int key)         sk83_na_m 0x82990780
//   hashtable<unsigned int, pair<const unsigned int, HUDComponentInfo*>, ...>::find  sk83_na_m 0x82ff1910
// The table: +4 buckets, +8 bucket count; a node: key at +0, next at +8. find(key) returns an
// iterator {node, bucket} through the hidden result pointer, the end iterator {*buckets[count],
// &buckets[count]} when the key is not there. The iterator is built in a stack temporary first
// (r1-0x10 node, r1-0xc bucket) and copied out -- reproduced, as all stack writes are. A zero bucket count traps (twi) in the original;
// the hooks leave that case to the recompiled code. Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"

namespace eastl {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

static __forceinline void HashFind(unsigned int result, unsigned int table, unsigned int key, bool halfword)
{
    const unsigned int count = RWP_LD32(table + 8u), buckets = RWP_LD32(table + 4u);
    const unsigned int slot = buckets + 4u * (key % count);
    for (unsigned int n = RWP_LD32(slot); n; n = RWP_LD32(n + 8u))
    {
        const unsigned int k = halfword ? (RWP_LD32(n) >> 16) : RWP_LD32(n);
        if (k == key)
        {
#if defined(RWP_GUEST)
            GStore32(g_ctx->r1.u32 - 0xcu, slot);
            GStore32(g_ctx->r1.u32 - 0x10u, n);
#endif
            RWP_ST32(result, n);
            RWP_ST32(result + 4u, slot);
            return;
        }
    }
    const unsigned int last = buckets + 4u * count;
#if defined(RWP_GUEST)
    GStore32(g_ctx->r1.u32 - 0xcu, last);
    GStore32(g_ctx->r1.u32 - 0x10u, RWP_LD32(last));
#endif
    RWP_ST32(result, RWP_LD32(last));
    RWP_ST32(result + 4u, last);
}

void HashFindU16(unsigned int result, unsigned int table, unsigned int keyp)
{
    HashFind(result, table, ((unsigned int)RWP_LD8(keyp) << 8) | RWP_LD8(keyp + 1u), true);
}
void HashFindS32(unsigned int result, unsigned int table, unsigned int keyp)
{
    HashFind(result, table, RWP_LD32(keyp), false);
}

}
