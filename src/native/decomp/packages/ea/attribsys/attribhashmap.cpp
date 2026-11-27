// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (2); comments keep the dev addresses.
// EA AttribSys: Attrib::HashMap (the library is in sk83 without asserts, so its source path is not
// in the game's strings; filed under the package name).
//   Attrib::HashMap::FindIndex(unsigned __int64) const   sk83_na_m 0x82d7cb18 (0xe8)
//   Attrib::VecHashMap<unsigned __int64, Collection, ...>::FindIndex(unsigned __int64) const  0x82d7b830 (0xc4)
//   Attrib::HashMap::Find(unsigned __int64) const               sk83_na_m 0x82d7cc00 (0x8c)
//   Attrib::Collection::GetNode(unsigned __int64, const Collection *&) const  0x82d81fe0 (0xe0)
//   Attrib::Collection::GetData(unsigned __int64, unsigned int) const (map: unnamed)  0x82d82178 (0xe0)
//   Attrib::Node::GetPointer<Collection>(void *, const Collection *) const   0x82d7aca0 (0xac)
//   Attrib::Node::GetArray<Collection>(void *, const Collection *) const     0x82d7ad50 (0x88)
// Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"
#include "rwp_ppc_ops.h"

namespace Attrib {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

// The map: +0 entries (16 bytes: u64 key, ..., +0xe u8 max probe length from this home slot,
// +0xf u8 flags, 0x80 = used), +8 u16 table size, +0xa u16 entry count, +0xe u16 key rotation.
// The home slot is the key rotated left by the rotation, folded to 32 bits, mod the size; linear
// probing from there for at most the home slot's probe length. Not found: the table size.
// (A zero table size traps in the original -- twi -- and is left to the recompiled code.)
static __forceinline unsigned long long SlotKey(unsigned int e)
{
    return (RWP_LD8(e + 0xfu) & 0x80u) ? RWP_LD64(e) : 0ull;
}

unsigned int HashMap_FindIndex(unsigned int self, unsigned long long key)
{
    const unsigned int size = RWP_LD8(self + 8u) << 8 | RWP_LD8(self + 9u);
    if ((RWP_LD8(self + 0xau) << 8 | RWP_LD8(self + 0xbu)) == 0)
        return size;
    const unsigned int rot = RWP_LD8(self + 0xeu) << 8 | RWP_LD8(self + 0xfu);
    const unsigned int entries = RWP_LD32(self);
    // slw / srd: shift counts past the width give 0 (6 and 7 bit shift fields)
    const unsigned int lo = (rot & 0x20u) ? 0u : ((unsigned int)key << (rot & 0x1fu));
    const unsigned int sr = (0x40u - rot) & 0x7fu;
    const unsigned int hi = (sr & 0x40u) ? 0u : (unsigned int)(key >> sr);
    unsigned int idx = (hi | lo) % size;
    const unsigned int probes = RWP_LD8(entries + 16u * idx + 0xeu);
    for (unsigned int n = 0; probes != 0; )
    {
        if (SlotKey(entries + 16u * idx) == key)
            break;
        idx++;
        n++;
        idx -= (idx / size) * size;
        if (!(n < probes))
            break;
    }
    if (SlotKey(entries + 16u * idx) != key)
        idx = size;
    return idx;
}


// The vector hash map: +0 entries (16 bytes: u64 key, +8 a word that points at the entry itself
// when it is empty, +0xc the probe length from this home slot), +4 u16 size, +6 u16 count. Home
// slot = the key's low word mod the size, then linear probing; not found: the size.
static __forceinline unsigned long long VecSlotKey(unsigned int e)
{
    return RWP_LD32(e + 8u) == e ? 0ull : RWP_LD64(e);
}

unsigned int VecHashMap_FindIndex(unsigned int self, unsigned long long key)
{
    const unsigned int size = RWP_LD8(self + 4u) << 8 | RWP_LD8(self + 5u);
    if ((RWP_LD8(self + 6u) << 8 | RWP_LD8(self + 7u)) == 0)
        return size;
    const unsigned int entries = RWP_LD32(self);
    unsigned int idx = (unsigned int)key % size;
    const unsigned int probes = RWP_LD32(entries + 16u * idx + 0xcu);
    for (unsigned int n = 0; probes != 0; )
    {
        if (VecSlotKey(entries + 16u * idx) == key)
            break;
        idx++;
        n++;
        idx -= (idx / size) * size;
        if (!(n < probes))
            break;
    }
    if (VecSlotKey(entries + 16u * idx) != key)
        idx = size;
    return idx;
}


#if !defined(RWP_GUEST)
extern "C" void RtlEnterCriticalSection(unsigned int cs), RtlLeaveCriticalSection(unsigned int cs);
#endif

// A node's data (+8): inline in the node (flags 0x40, GetPointer only), at an offset into the
// collection's layout (0x10), into the class's static layout (0x20: owner +0x20 -> +8 -> +0x38),
// or the stored pointer itself.
static __forceinline unsigned int NodeData(unsigned int node, unsigned int layout, unsigned int owner, bool inl)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    RWP_NV(31, node);
    const unsigned int f = RWP_LD8(node + 0xfu);
    if (inl && (f & 0x40u))
        return node + 8u;
    if (f & 0x10u)
        return RWP_LD32(node + 8u) + layout;
    if (f & 0x20u)
        return RWP_LD32(node + 8u) + RWP_LD32(RWP_LD32(RWP_LD32(owner + 0x20u) + 8u) + 0x38u);
    return RWP_LD32(node + 8u);
}
unsigned int Node_GetPointer(unsigned int node, unsigned int layout, unsigned int owner) { return NodeData(node, layout, owner, true); }
unsigned int Node_GetArray(unsigned int node, unsigned int layout, unsigned int owner) { return NodeData(node, layout, owner, false); }

// the used entry (flags +0xf bit 0x80) for the key, or null
unsigned int HashMap_Find(unsigned int self, unsigned long long key)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    RWP_NV(31, self);
    const unsigned int i = HashMap_FindIndex(self, key);
    if (i < (RWP_LD8(self + 8u) << 8 | RWP_LD8(self + 9u)) && (RWP_LD8(RWP_LD32(self) + 16u * i + 0xfu) & 0x80u))
        return RWP_LD32(self) + 16u * i;
    return 0u;
}

// A collection's node for the key: its own table, then its parents' (+0x10), then -- when the
// class has a layout (+0x24) -- the class's table (+0x20 -> +8, +0x10); the owning collection
// goes to *owner (null when not found).
unsigned int Collection_GetNode(unsigned int coll, unsigned long long key, unsigned int owner)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(30, coll);
    RWP_NV(29, key);
    RWP_NV(28, owner);
    for (unsigned int cur = coll; cur; cur = RWP_LD32(cur + 0x10u))
    {
        RWP_NV(31, cur);
        const unsigned int i = HashMap_FindIndex(cur, key);
        if (i < (RWP_LD8(cur + 8u) << 8 | RWP_LD8(cur + 9u)) && (RWP_LD8(RWP_LD32(cur) + 16u * i + 0xfu) & 0x80u))
        {
            const unsigned int node = RWP_LD32(cur) + 16u * i;
            if (node)
            {
                RWP_ST32(owner, cur);
                return node;
            }
        }
    }
    if (RWP_LD32(coll + 0x24u))
    {
        const unsigned int node = HashMap_Find(RWP_LD32(RWP_LD32(coll + 0x20u) + 8u) + 0x10u, key);
        if (node)
        {
            RWP_ST32(owner, coll);
            return node;
        }
    }
    RWP_ST32(owner, 0u);
    return 0u;
}

// The attribute's data for the key (element idx of an array attribute), under the class lock
// (+0x20 -> +8 -> +0x3c): null when the node is missing or the index is past the array.
unsigned int Collection_GetData(unsigned int coll, unsigned long long key, unsigned int idx)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(29, coll);
    RWP_NV(28, key);
    RWP_NV(31, idx);
    const unsigned int lock = RWP_LD32(RWP_LD32(RWP_LD32(coll + 0x20u) + 8u) + 0x3cu);
    RWP_NV(30, lock);
    RWP_CALL(0x82f9cb44, RtlEnterCriticalSection, lock);
    const unsigned int node = Collection_GetNode(coll, key, FRAME_AT(frame, 0x50));
    if (!node)
    {
        RWP_CALL(0x82f9cb54, RtlLeaveCriticalSection, lock);
        return 0u;
    }
    const unsigned int flags = RWP_LD8(node + 0xfu);
    const unsigned int owner = RWP_LD32(FRAME_AT(frame, 0x50));
    const unsigned int layout = RWP_LD32(owner + 0x24u);
    unsigned int r;
    if (flags & 2u)
    {
        const unsigned int arr = Node_GetArray(node, layout, owner);
        const unsigned int n = RWP_LD32(arr) & 0xffffu;                  // lhz +2
        if (idx < n)
        {
            const unsigned int h6 = RWP_LD32(arr + 4u) & 0xffffu, h4 = RWP_LD32(arr + 4u) >> 16;
            r = RwpRlwinm(h6, 0x14, 0xc, 0x1c) + h4 * idx + arr + 8u;
        }
        else
            r = 0u;
        RWP_NV(31, r);
    }
    else
    {
        r = Node_GetPointer(node, layout, owner);
        RWP_NV(31, r);
    }
    RWP_CALL(0x82f9cb54, RtlLeaveCriticalSection, lock);
    return r;
}

}
