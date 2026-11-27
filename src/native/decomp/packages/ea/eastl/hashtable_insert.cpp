// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (4); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82a3f518 82a3f690
// eastl::hashtable node insertion / release the render thread runs every frame (EASTL's
// include/eastl/internal/hashtable.h, compiled into the game's TUs; sk83 has no path for them).
// The shadow caster culling (0x829a9f38) empties its caster set (RenderView +0x4f70, a
// hash_map<PresentationEntity*, PresentationEntity*>) and refills it from the scene's shadow
// entities with insert(first, last):
//   hashtable<unsigned short, pair<const unsigned short, int>, ...>::DoFreeNodes   sk83_na_m 0x82980c20 (0x5c)
//       (one body for every hashtable with 12-byte nodes and an eastl_allocator<50,1>: the
//       linker folded them; the caster set's DoFreeNodes is this one)
//   hashtable<PresentationEntity*, pair<PresentationEntity* const, PresentationEntity*>, ...>
//       ::insert<hashtable_iterator<...>>(first, last)                             sk83_na_m 0x829a1a80 (0xf4)
//       ::DoInsertValue(const value_type &, true_type)  (unique keys)             sk83_na_m 0x829910c8 (0x140)
//   eastl::distance_impl<hashtable_iterator<...>>                                 sk83_na_m 0x82981130 (0x64)
//   eastl::prime_rehash_policy::GetRehashRequired: its no-rehash path inline; a rehash goes to
//       the recompiled function (0x82d17e60), as do DoRehash (0x8298bc08) and the allocator.
// The table: +4 buckets, +8 bucket count, +0xc element count, +0x10 the rehash policy
// (mfMaxLoadFactor, mfGrowthFactor, mnNextResize at +8). A node (12 bytes, category 62): key +0,
// value +4, next +8. An iterator: {node, bucket}; the hash is the key (a pointer) mod the bucket
// count. Iterators are advanced in memory, as the original does (the caller's `first` too).
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace eastl {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#if !defined(RWP_GUEST)
void prime_rehash_policy_GetRehashRequired(unsigned int result, unsigned int policy, unsigned int buckets,
                                           unsigned int elements, unsigned int add);
void EntityMap_DoRehash(unsigned int table, unsigned int buckets);
unsigned int XCore_EASTL_Category_Alloc(unsigned int size, int category, unsigned int name, int flags);
void XCore_EASTL_Category_Free(unsigned int p, unsigned int size, int category);
#endif

#define HT_NODE_SIZE     12u
#define HT_CATEGORY      62u
#define HT_ALLOC_NAME    0x822fb850u     // the allocator's name string (lis 0x820b, addi -0x6020)

// prime_rehash_policy::GetRehashRequired(result, policy, buckets, elements, add): {false, 0}
// when elements + add stays within mnNextResize, done here with the callee's stack writes
// (__savegprlr_29, stwu -0x80); anything else (a rehash or a new mnNextResize) goes to the
// recompiled function. result: a pair<bool, unsigned> (bool +0, count +4).
static __forceinline void GetRehashRequired(unsigned int result, unsigned int policy, unsigned int buckets,
                                            unsigned int elements, unsigned int add)
{
    if (elements + add > RWP_LD32(policy + 8u))
    {
        RWP_CALL(0x82b0eab0, prime_rehash_policy_GetRehashRequired, result, policy, buckets, elements, add);
        return;
    }
#if defined(RWP_GUEST)
    rwp::SaveGprLr(29);
    GStore32(g_ctx->r1.u32 - 0x80u, g_ctx->r1.u32);
#endif
    RWP_ST32(result + 4u, 0u);
    RWP_ST8(result, 0u);
}

// ++it on an iterator in memory: the next node of the chain, else the next non-empty bucket
// (the end sentinel stops the scan)
static __forceinline void IncrementIterator(unsigned int it)
{
    const unsigned int next = RWP_LD32(RWP_LD32(it) + 8u);
    RWP_ST32(it, next);
    if (next)
        return;
    unsigned int b = RWP_LD32(it + 4u);
    unsigned int n;
    do
    {
        b += 4u;
        RWP_ST32(it + 4u, b);
        n = RWP_LD32(b);
        RWP_ST32(it, n);
    } while (!n);
}

// distance_impl(first, last): counts by advancing `first` (the caller's copy) to last
static __forceinline unsigned int Distance(unsigned int first, unsigned int last)
{
    unsigned int n = 0;
    while (RWP_LD32(first) != RWP_LD32(last))
    {
        IncrementIterator(first);
        n++;
    }
    return n;
}

// DoFreeNodes(buckets, count): every node of every bucket back to the allocator, the bucket
// emptied after its chain.
void Hashtable_DoFreeNodes(unsigned int self, unsigned int buckets, unsigned int count)
{
    (void)self;
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    if (count == 0)
        return;
    unsigned int prev = buckets - 4u;
    unsigned int left = count;
    RWP_NV(28, 0u);
    do
    {
        unsigned int n = RWP_LD32(prev + 4u);
        while (n)
        {
            const unsigned int node = n;
            n = RWP_LD32(node + 8u);
            RWP_NV(29, left);
            RWP_NV(30, prev);
            RWP_NV(31, n);
            RWP_CALL(0x5fff0000, XCore_EASTL_Category_Free, node, HT_NODE_SIZE, HT_CATEGORY);
        }
        prev += 4u;
        RWP_ST32(prev, 0u);
    } while (--left);
}

// DoInsertValue(value, true_type): the existing node when the key is there ({node, bucket,
// false}), else a new node at the head of its bucket ({node, bucket, true}), rehashing first
// when the policy asks for it. result: pair<iterator, bool> (node +0, bucket +4, bool +8).
unsigned int EntityMap_DoInsertValue(unsigned int result, unsigned int self, unsigned int value)
{
    RWP_SAVEGPRLR(26);
    RWP_GUEST_FRAME(frame, 0x90);
    const unsigned int key = RWP_LD32(value);
    const unsigned int count = RWP_LD32(self + 8u);
    // (twllei: a zero bucket count traps; the hook leaves that to the recompiled code)
    unsigned int idx = key % count;
    {
        const unsigned int slot = RWP_LD32(self + 4u) + 4u * idx;
        for (unsigned int n = RWP_LD32(slot); n; n = RWP_LD32(n + 8u))
        {
            if (RWP_LD32(n) == key)
            {
                RWP_ST32(result, n);
                RWP_ST32(result + 4u, slot);
                RWP_ST8(result + 8u, 0u);
                return result;
            }
        }
    }
    RWP_NV(26, value);
    RWP_NV(27, idx);
    RWP_NV(29, key);
    RWP_NV(30, result);
    RWP_NV(31, self);
    GetRehashRequired(FRAME_AT(frame, 0x50), self + 0x10u, count, RWP_LD32(self + 0xcu), 1u);
    const unsigned int node = RWP_CALL(0x5fff0000, XCore_EASTL_Category_Alloc, HT_NODE_SIZE, HT_CATEGORY, HT_ALLOC_NAME, 0u);
    RWP_NV(28, node);
    if (node)
    {
        RWP_ST32(node, RWP_LD32(value));
        RWP_ST32(node + 4u, RWP_LD32(value + 4u));
    }
    RWP_ST32(node + 8u, 0u);
    if (RWP_LD8(FRAME_AT(frame, 0x50)))
    {
        const unsigned int nb = RWP_LD32(FRAME_AT(frame, 0x54));
        idx = key % nb;
        RWP_NV(27, idx);
        RWP_CALL(0x8296e078, EntityMap_DoRehash, self, nb);
    }
    const unsigned int off = 4u * idx;
    RWP_ST32(result, node);
    RWP_ST8(result + 8u, 1u);
    RWP_ST32(node + 8u, RWP_LD32(RWP_LD32(self + 4u) + off));
    RWP_ST32(RWP_LD32(self + 4u) + off, node);
    const unsigned int buckets = RWP_LD32(self + 4u);
    RWP_ST32(self + 0xcu, RWP_LD32(self + 0xcu) + 1u);
    RWP_ST32(result + 4u, buckets + off);
    return result;
}

// insert(first, last): one rehash check for the whole range (its length by distance over a
// copy), then every element by DoInsertValue, advancing the caller's `first` in place. The
// tag bytes (stale r1+0x50 for distance, 0 there for DoInsertValue) are passed as in the
// original.
void EntityMap_InsertRange(unsigned int self, unsigned int first, unsigned int last)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0xa0);
    RWP_NV(29, last);
    RWP_NV(30, self);
    RWP_NV(31, first);
    {
        const unsigned int ln = RWP_LD32(last), lb = RWP_LD32(last + 4u);
        const unsigned int fn = RWP_LD32(first), fb = RWP_LD32(first + 4u);
        RWP_ST32(FRAME_AT(frame, 0x58), ln);
        RWP_ST32(FRAME_AT(frame, 0x5c), lb);
        RWP_ST32(FRAME_AT(frame, 0x60), fn);
        RWP_ST32(FRAME_AT(frame, 0x64), fb);
    }
    const unsigned int n = Distance(FRAME_AT(frame, 0x60), FRAME_AT(frame, 0x58));
    GetRehashRequired(FRAME_AT(frame, 0x70), self + 0x10u, RWP_LD32(self + 8u), RWP_LD32(self + 0xcu), n);
    if (RWP_LD8(FRAME_AT(frame, 0x70)))
        RWP_CALL(0x8296e078, EntityMap_DoRehash, self, (unsigned int)RWP_LD32(FRAME_AT(frame, 0x74)));
    if (RWP_LD32(first) == RWP_LD32(last))
        return;
    RWP_NV(28, 0u);
    do
    {
        RWP_ST8(FRAME_AT(frame, 0x50), 0u);
        if (RWP_LD32(self + 8u) != 0)
            EntityMap_DoInsertValue(FRAME_AT(frame, 0x60), self, RWP_LD32(first));
        else   // a zero bucket count traps (twllei) in the original: the recompiled code's business
            RWP_CALL(0x8296df38, EntityMap_DoInsertValue, FRAME_AT(frame, 0x60), self, (unsigned int)RWP_LD32(first));
        IncrementIterator(first);
    } while (RWP_LD32(first) != RWP_LD32(last));
}

}
