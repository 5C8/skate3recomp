// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (4); comments keep the dev addresses.
// packages/skate/junktofixup/sk8util/dev/include/util/kdtree.h (sk83 assert path; a template
// header, instantiated in the render library -- the Skate 2 PDB puts these instances in
// render_lib.obj): the anti-frustum quad k-d tree query that
// Sk8::Render::AntiFrustumManager::Select_Antifrustums (0x829aad18, under Frustum::UpdateCullParams)
// runs per tree each frame on the render thread, and the result set it fills.
//   KDNode<AntiFrustumQuad *, Vector3, 3>::FindNear   sk83_na_m 0x829a94b8 (0x338)
//   KDTree<AntiFrustumQuad *, Vector3, 3>::FindNear   sk83_na_m 0x829aaa28 (0x1bc)
//   result set eastl::fixed_multiset<KDObjDist<...>, 256> (float key mDistance at node +0x10,
//   mObject +0x14; identical-code-folded with the other KDObjDist instances, so the map names
//   them after tRAINItemData / FE::MapIcon):
//     rbtree::insert (upper-bound walk + DoInsertValueImpl)     sk83_na_m 0x82ef15e8 (0x84)
//     rbtree::DoInsertValueImpl(parent, value, bForceToLeft)    sk83_na_m 0x82ef1240 (0xd4)
//     rbtree::DoNukeSubtree (nodes back to the fixed node pool)  sk83_na_m 0x830b1e48 (0x5c)
// KDNode: +0x10 left child, +0x14 right child, +0x18 parent, +0x30 the object, +0x40 the
// position (vpu Vector3). KDTree: +0x1c the root. rbtree: +4 anchor (right, left, parent,
// color), +0x14 size, +0x18 fixed_node_pool (head, current size, peak size, nodes at +0x24).
// Hand-decompiled from the sk83 code (Skate 2 Hex-Rays for the names); stack temporaries of the
// originals are reproduced at their frame offsets, non-volatile registers mirrored around calls.
// Written for sk83_na_m --sk83_replay (frameparts: Presentation::Update 0x8292ed90).
#pragma once
#include "rwp_guestcall.h"
#include <string.h>

namespace eastl {   // packages/ea/eastl/red_black_tree.cpp
unsigned int RBTreeIncrement(unsigned int pNode);
void RBTreeInsert(unsigned int pNode, unsigned int pNodeParent, unsigned int pNodeAnchor, int insertionSide);
}

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
void VectorFloat_DoInsertValue(unsigned int v, unsigned int pos, unsigned int pValue);
void VectorKDNode_DoInsertValue(unsigned int v, unsigned int pos, unsigned int pValue);
void Vector_Reserve(unsigned int v, unsigned int n);
unsigned int KdResultSet_Construct(unsigned int set);
#endif

// a float through lfs + stfs: the value is unchanged except that a signalling NaN comes out quiet
// (the recompiler converts through double)
static inline unsigned int KdF32Pass(unsigned int b)
{
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu))
        b |= 0x00400000u;
    return b;
}
static inline float KdF32(unsigned int b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static inline unsigned int KdBits(float f)
{
    unsigned int b;
    memcpy(&b, &f, 4);
    return b;
}
// the recompiler's float modes, set unconditionally as the originals' recompiled code does
// (ldmxcsr only: the cached RWP_VMX_MODE / RWP_FPU_MODE read the csr back with stmxcsr first,
// which made this walk slower than the recompiled one)
#if defined(RWP_GUEST)
#define KD_VMX_MODE() rwp::g_ctx->fpscr.enableFlushModeUnconditional()
#define KD_FPU_MODE() rwp::g_ctx->fpscr.disableFlushModeUnconditional()
#else
#define KD_VMX_MODE() ((void)0)
#define KD_FPU_MODE() ((void)0)
#endif
// lvx / stvx at a guest address (low four bits dropped) and a word splatted to all lanes
#if defined(RWP_GUEST)
static inline __vector4 KdLvx(unsigned int a) { return rwp::ldv(rwp::g_base + (a & ~15u)); }
static inline void KdStvx(unsigned int a, __vector4 v) { rwp::stv(rwp::g_base + (a & ~15u), v); }
static inline __vector4 KdSplatWord(unsigned int a)
{
    return simde_mm_castsi128_ps(simde_mm_set1_epi32((int)rwp::GLoad32(a)));
}
#else
static __forceinline __vector4 KdLvx(unsigned int a) { return *(const __vector4 *)(size_t)(a & ~15u); }
static __forceinline void KdStvx(unsigned int a, __vector4 v) { *(__vector4 *)(size_t)(a & ~15u) = v; }
static __forceinline __vector4 KdSplatWord(unsigned int a) { return __vspltw(__lvlx((void *)(size_t)a, 0), 0); }
#endif

// ---- result set: eastl::rbtree<KDObjDist> over a fixed_node_pool -------------------------

unsigned int KdResultSet_DoInsertValueImpl(unsigned int result, unsigned int tree, unsigned int parent,
                                           unsigned int value, unsigned int bForceToLeft)
{
    RWP_SAVEGPRLR(29);
    RWP_GUEST_FRAME(frame, 0x70);
    int side;
    if ((bForceToLeft & 0xffu) || parent == tree + 4u)
        side = 0;   // kRBTreeSideLeft
    else
    {
        KD_FPU_MODE();
        side = KdF32(RWP_LD32(value)) < KdF32(RWP_LD32(parent + 16u)) ? 0 : 1;
    }
    // fixed_node_pool::allocate (no overflow allocator: null when the pool is used up)
    const unsigned int node = RWP_LD32(tree + 24u);
    if (node)
    {
        const unsigned int n = RWP_LD32(tree + 28u) + 1u;
        RWP_ST32(tree + 28u, n);
        if (n > RWP_LD32(tree + 32u))
            RWP_ST32(tree + 32u, n);
        RWP_ST32(tree + 24u, RWP_LD32(node));
    }
    if (node + 16u)
    {
        KD_FPU_MODE();
        RWP_ST32(node + 16u, KdF32Pass(RWP_LD32(value)));
        RWP_ST32(node + 20u, RWP_LD32(value + 4u));
    }
    RWP_NV(29, result);
    RWP_NV(30, tree);
    RWP_NV(31, node);
    eastl::RBTreeInsert(node, parent, tree + 4u, side);
    RWP_ST32(tree + 20u, RWP_LD32(tree + 20u) + 1u);
    RWP_ST32(result, node);
    return result;
}

// multiset insert: the last position the key's upper bound walk reaches
unsigned int KdResultSet_Insert(unsigned int result, unsigned int tree, unsigned int value)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    unsigned int parent = tree + 4u;
    unsigned int n = RWP_LD32(tree + 12u);
    if (n)
    {
        KD_FPU_MODE();
        const float key = KdF32(RWP_LD32(value));
        do
        {
            parent = n;
            n = key < KdF32(RWP_LD32(n + 16u)) ? RWP_LD32(n + 4u) : RWP_LD32(n);
        } while (n);
    }
    RWP_NV(31, result);
    KdResultSet_DoInsertValueImpl(result, tree, parent, value, 0u);
    return result;
}

// every node of the subtree back on the pool's free list (right subtrees recursively)
void KdResultSet_DoNukeSubtree(unsigned int tree, unsigned int node)
{
    RWP_SAVEGPRLR(29);
    RWP_GUEST_FRAME(frame, 0x70);
    if (!node)
        return;
    const unsigned int pool = tree + 24u;
    RWP_NV(29, tree);
    RWP_NV(30, pool);
    do
    {
        RWP_NV(31, node);
        KdResultSet_DoNukeSubtree(tree, RWP_LD32(node));
        const unsigned int n = RWP_LD32(pool + 4u) - 1u;
        const unsigned int head = RWP_LD32(pool);
        const unsigned int left = RWP_LD32(node + 4u);
        RWP_ST32(pool + 4u, n);
        RWP_ST32(node, head);
        RWP_ST32(pool, node);
        node = left;
    } while (node);
}

// ---- KDNode<AntiFrustumQuad *, Vector3, 3>::FindNear ---------------------------------------
// Iterative walk from this node: every node within radius of point (distance = |pos - point|
// by rsqrte + two Newton steps, 0 for a zero length) goes into nodes / distances, until nodes
// holds min(neighbours, its capacity). A child is entered when the splitting plane (dimension
// dim of the node's position) is within radius of the point on its side; climbing back up (to
// the parent, dim + 2) the left child's return also tries the right side. Returns true.
// maxnear is not used. dim is (dimstart + depth) % 3; the hook takes dimstart 0..2 only (the
// plane value is read back from a stack copy of the position at 4 * dim).
unsigned int KDNode_FindNear(unsigned int self, int dimstart, unsigned int point, double radius, int /*maxnear*/,
                             int neighbours, unsigned int nodes, unsigned int dists)
{
    RWP_SAVEGPRLR(20);
    RWP_SAVEF31(112);
    RWP_GUEST_FRAME(frame, 0x140);
    const unsigned int sp = FRAME_AT(frame, 0);   // the new r1 (frame offsets below are from it)
    const int capacity = (int)(RWP_LD32(nodes + 8u) - RWP_LD32(nodes)) >> 2;
    RWP_ST32((sp + 0x50u), self);
    const int maxFound = neighbours < capacity ? neighbours : capacity;
    unsigned int cur = self, prev = self;
    int dim = dimstart;
    bool up = false;
#define KDN_MIRROR_NV()                         \
    do                                          \
    {                                           \
        RWP_NV(20, (long long)maxFound);        \
        RWP_NV(21, 64u);                        \
        RWP_NV(22, dists);                      \
        RWP_NV(23, self);                       \
        RWP_NV(24, point);                      \
        RWP_NV(25, nodes);                      \
        RWP_NV(26, wasUp ? 1u : 0u);            \
        RWP_NV(27, prev);                       \
        RWP_NV(28, 0u);                         \
        RWP_NV(29, 0x55555556u);                \
        RWP_NV(30, (long long)dim);             \
        RWP_NV(31, cur);                        \
        RWP_NVF31(radius);                      \
    } while (0)
    if (self == RWP_LD32(self + 24u))
        return 1u;
    do
    {
        if ((int)(RWP_LD32(nodes + 4u) - RWP_LD32(nodes)) >> 2 >= maxFound)
            break;
        const bool wasUp = up;
        if (!wasUp)
        {
            // |pos - point|
            KD_VMX_MODE();
            const __vector4 vPoint = KdLvx(point);
            const __vector4 one = __vcfsx(__vspltisw(1), 0);
            const __vector4 vPos = KdLvx(cur + 64u);
            const __vector4 zero = __vspltisw(0);
            const __vector4 d = __vsubfp(vPos, vPoint);
            const __vector4 half = __vcfsx(__vspltisw(1), 1);
            const __vector4 len2 = __vmsum3fp(d, d);
            __vector4 e = __vrsqrtefp(len2);
            const __vector4 isZero = __vcmpeqfp(zero, len2);
            e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), one), e);
            e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(len2, __vmulfp(e, e), one), e);
            KdStvx((sp + 0xa0u), __vsel(__vmulfp(len2, e), zero, isZero));
            KD_FPU_MODE();
            const unsigned int distBits = RWP_LD32((sp + 0xa0u));
            RWP_ST32((sp + 0x54u), KdF32Pass(distBits));
            if (!((double)KdF32(distBits) > radius))
            {
                // distances.push_back(dist)
                const unsigned int fe = RWP_LD32(dists + 4u);
                if (fe < RWP_LD32(dists + 8u))
                {
                    RWP_ST32(dists + 4u, fe + 4u);
                    if (fe)
                        RWP_ST32(fe, KdF32Pass(distBits));
                }
                else
                {
                    KDN_MIRROR_NV();
                    RWP_CALL(0x826d1e70, VectorFloat_DoInsertValue, dists, fe, (sp + 0x54u));
                }
                // nodes.push_back(this node)
                const unsigned int ne = RWP_LD32(nodes + 4u);
                if (ne < RWP_LD32(nodes + 8u))
                {
                    RWP_ST32(nodes + 4u, ne + 4u);
                    if (ne)
                        RWP_ST32(ne, cur);
                }
                else
                {
                    KDN_MIRROR_NV();
                    RWP_CALL(0x8268e038, VectorKDNode_DoInsertValue, nodes, ne, (sp + 0x50u));
                    cur = RWP_LD32((sp + 0x50u));
                }
            }
        }
        const unsigned int left = RWP_LD32(cur + 16u);
        bool goLeft = false;
        if (left && !wasUp)
        {
            // pos[dim] >= point[dim] - radius: the left side can hold points within radius
            KD_FPU_MODE();
            RWP_ST32((sp + 0x60u), KdBits((float)radius));
            KdStvx((sp + 0xb0u), KdLvx(cur + 64u));
            const unsigned int planeBits = RWP_LD32((sp + 0xb0u) + 4u * (unsigned int)dim);
            RWP_ST32((sp + 0x64u), 0u);
            RWP_ST32((sp + 0x68u), 0u);
            RWP_ST32((sp + 0x6cu), 0u);
            RWP_ST32((sp + 0x70u), KdF32Pass(planeBits));
            RWP_ST32((sp + 0x74u), 0u);
            RWP_ST32((sp + 0x78u), 0u);
            RWP_ST32((sp + 0x7cu), 0u);
            KD_VMX_MODE();
            const __vector4 p = KdSplatWord((point & ~15u) + ((4u * (unsigned int)dim) & 15u));
            const __vector4 r = KdSplatWord((sp + 0x60u));
            const __vector4 plane = KdSplatWord((sp + 0x70u));
            goLeft = VALL_GE(plane, __vsubfp(p, r));
        }
        int next;
        if (goLeft)
        {
            prev = cur;
            cur = left;
            next = dim + 1;
            up = false;
        }
        else
        {
            const unsigned int right = RWP_LD32(cur + 20u);
            bool goRight = false;
            if (right && (!wasUp || prev == left))
            {
                // point[dim] + radius > pos[dim]: the right side can hold points within radius
                KD_FPU_MODE();
                RWP_ST32((sp + 0x80u), KdBits((float)radius));
                KdStvx((sp + 0xc0u), KdLvx(cur + 64u));
                const unsigned int planeBits = RWP_LD32((sp + 0xc0u) + 4u * (unsigned int)dim);
                RWP_ST32((sp + 0x84u), 0u);
                RWP_ST32((sp + 0x88u), 0u);
                RWP_ST32((sp + 0x8cu), 0u);
                RWP_ST32((sp + 0x90u), KdF32Pass(planeBits));
                RWP_ST32((sp + 0x94u), 0u);
                RWP_ST32((sp + 0x98u), 0u);
                RWP_ST32((sp + 0x9cu), 0u);
                KD_VMX_MODE();
                const __vector4 p = KdSplatWord((point & ~15u) + ((4u * (unsigned int)dim) & 15u));
                const __vector4 r = KdSplatWord((sp + 0x80u));
                const __vector4 plane = KdSplatWord((sp + 0x90u));
                goRight = VALL_GT(__vaddfp(p, r), plane);
            }
            if (goRight)
            {
                prev = cur;
                cur = right;
                next = dim + 1;
                up = false;
            }
            else
            {
                prev = cur;
                cur = RWP_LD32(cur + 24u);
                next = dim + 2;
                up = true;
            }
        }
        dim = (short)(next % 3);
        RWP_ST32((sp + 0x50u), cur);
    } while (cur != RWP_LD32(self + 24u));
#undef KDN_MIRROR_NV
    return 1u;
}

// ---- KDTree<AntiFrustumQuad *, Vector3, 3>::FindNear ---------------------------------------
// Up to numItems objects within radius of point, nearest first: the root's FindNear into two
// stack fixed_vectors (nodes, distances; 256 each), sorted through a stack fixed_multiset of
// KDObjDist, then copied out (distances[i], objects[i]) and the count stored. numItems > 256
// asserts (kdtree.h line 723): the hook leaves that case to the recompiled code.
void KDTree_FindNear(unsigned int self, unsigned int point, double radius, unsigned int numItems,
                     unsigned int pNumFound, unsigned int pObjects, unsigned int pDistances)
{
    RWP_SAVEGPRLR(25);
    RWP_SAVEF31(72);
    RWP_GUEST_FRAME(frame, 0x2120);
    const unsigned int sp = FRAME_AT(frame, 0);   // the new r1 (frame offsets below are from it)
    RWP_ST32(pNumFound, 0u);
    const unsigned int root = RWP_LD32(self + 28u);
    if (!root)
        return;
    // the original's non-volatile registers while calling out
    unsigned long long nv29 = 0u;   // subf over the zero-extended pointers: 64-bit
    unsigned int nv30 = point, nv31 = self;
#define KDT_MIRROR_NV()             \
    do                              \
    {                               \
        RWP_NV(25, pNumFound);      \
        RWP_NV(26, pObjects);       \
        RWP_NV(27, pDistances);     \
        RWP_NV(28, numItems);       \
        RWP_NV(29, nv29);           \
        RWP_NV(30, nv30);           \
        RWP_NV(31, nv31);           \
        RWP_NVF31(radius);          \
    } while (0)
    // eastl::fixed_vector<KDNode *, 256> nodes (+0x70, buffer +0x80) and
    // eastl::fixed_vector<float, 256> distances (+0x480, buffer +0x490)
    const unsigned int vNodes = (sp + 0x70u), vDists = (sp + 0x480u), set = (sp + 0x890u);
    RWP_ST32(vNodes + 4u, (sp + 0x80u));
    RWP_ST32(vNodes, (sp + 0x80u));
    RWP_ST32(vNodes + 8u, (sp + 0x480u));
    RWP_ST32(vDists + 4u, (sp + 0x490u));
    RWP_ST32(vDists, (sp + 0x490u));
    RWP_ST32(vDists + 8u, (sp + 0x890u));
    KDT_MIRROR_NV();
    RWP_CALL(0x826c3c90, Vector_Reserve, vNodes, numItems);
    RWP_CALL(0x826c3c90, Vector_Reserve, vDists, numItems);
    RWP_CALL(0x00000000, KdResultSet_Construct, set);
    KDT_MIRROR_NV();
    if (KDNode_FindNear(root, 0, point, radius, 0, (int)numItems, vNodes, vDists) & 0xffu)
    {
        nv30 = 0u;
        unsigned int b = RWP_LD32(vNodes);
        if ((int)(RWP_LD32(vNodes + 4u) - b) >> 2)
        {
            nv31 = 0u;
            do
            {
                const unsigned int node = RWP_LD32(b + nv31);
                RWP_ST8((sp + 0x50u), 0u);
                const unsigned int distBits = RWP_LD32(RWP_LD32(vDists) + nv31);
                KD_FPU_MODE();
                RWP_ST32((sp + 0x58u), KdF32Pass(distBits));   // KDObjDist { mDistance, mObject }
                RWP_ST32((sp + 0x5cu), RWP_LD32(node + 48u));
                KDT_MIRROR_NV();
                KdResultSet_Insert((sp + 0x60u), set, (sp + 0x58u));
                b = RWP_LD32(vNodes);
                nv30++;
                nv31 += 4u;
            } while (nv30 < (unsigned int)((int)(RWP_LD32(vNodes + 4u) - b) >> 2));
        }
        // in key order: begin() is the anchor's left (set +8), end() the anchor (set +4)
        unsigned int it = RWP_LD32(set + 8u);
        nv30 = 0u;
        if (it != set + 4u)
        {
            nv31 = pDistances;
            nv29 = (unsigned long long)pObjects - (unsigned long long)pDistances;
            while (nv30 < numItems)
            {
                const unsigned int obj = RWP_LD32(it + 20u);
                KD_FPU_MODE();
                RWP_ST32(nv31, KdF32Pass(RWP_LD32(it + 16u)));
                nv30++;
                RWP_ST32((unsigned int)nv29 + nv31, obj);
                nv31 += 4u;
                it = eastl::RBTreeIncrement(it);
                if (it == set + 4u)
                    break;
            }
        }
        RWP_ST32(pNumFound, nv30);
    }
    // ~fixed_multiset
    KDT_MIRROR_NV();
    KdResultSet_DoNukeSubtree(set, RWP_LD32(set + 12u));
#undef KDT_MIRROR_NV
}

}}   // namespace Sk8::Render
