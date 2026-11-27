// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// EASTL 1.10.01 source/red_black_tree.cpp (the Skate 2 PDB puts it in the EASTL package's
// runtime object; sk83 has no path string for it):
//   eastl::RBTreeIncrement(const rbtree_node_base *)                       sk83_na_m 0x82d17430 (0x6c)
//   eastl::RBTreeInsert(rbtree_node_base *pNode, rbtree_node_base *pNodeParent,
//                       rbtree_node_base *pNodeAnchor, RBTreeSide insertionSide)  sk83_na_m 0x82d17620 (0x2b8)
// rbtree_node_base: mpNodeRight +0, mpNodeLeft +4, mpNodeParent +8, mColor +0xc (u8, red 0 /
// black 1). The anchor: right = rightmost node, left = leftmost node, parent = root.
// RBTreeInsert has its rotations inlined; the stores below follow the original's order.
// It is a leaf without a frame, but saves r31 at r1-8 (std r31,-8(r1)) -- reproduced.
#include "rwp_guestcall.h"

namespace eastl {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#define RB_RIGHT(n) RWP_LD32((n) + 0u)
#define RB_LEFT(n) RWP_LD32((n) + 4u)
#define RB_PARENT(n) RWP_LD32((n) + 8u)
#define RB_COLOR(n) RWP_LD8((n) + 12u)
#define RB_SET_RIGHT(n, v) RWP_ST32((n) + 0u, (v))
#define RB_SET_LEFT(n, v) RWP_ST32((n) + 4u, (v))
#define RB_SET_PARENT(n, v) RWP_ST32((n) + 8u, (v))
#define RB_SET_COLOR(n, v) RWP_ST8((n) + 12u, (v))
enum { kRBTreeColorRed = 0, kRBTreeColorBlack = 1 };

unsigned int RBTreeIncrement(unsigned int pNode)
{
    unsigned int r = RB_RIGHT(pNode);
    if (r)
    {
        // the leftmost node of the right subtree
        for (unsigned int l = RB_LEFT(r); l; l = RB_LEFT(r))
            r = l;
        return r;
    }
    unsigned int pTemp = RB_PARENT(pNode);
    while (pNode == RB_RIGHT(pTemp))
    {
        pNode = pTemp;
        pTemp = RB_PARENT(pTemp);
    }
    // (the anchor's right child is the rightmost node: incrementing it reaches the anchor)
    if (RB_RIGHT(pNode) != pTemp)
        pNode = pTemp;
    return pNode;
}

void RBTreeInsert(unsigned int pNode, unsigned int pNodeParent, unsigned int pNodeAnchor, int insertionSide)
{
#if defined(RWP_GUEST)
    GStore64(g_ctx->r1.u32 - 8u, g_ctx->r31.u64);   // std r31,-8(r1)
#endif
    RB_SET_PARENT(pNode, pNodeParent);
    RB_SET_RIGHT(pNode, 0u);
    RB_SET_LEFT(pNode, 0u);
    RB_SET_COLOR(pNode, kRBTreeColorRed);

    if (insertionSide == 0)   // kRBTreeSideLeft
    {
        RB_SET_LEFT(pNodeParent, pNode);
        if (pNodeParent == pNodeAnchor)
        {
            RB_SET_PARENT(pNodeAnchor, pNode);
            RB_SET_RIGHT(pNodeAnchor, pNode);
        }
        else if (pNodeParent == RB_LEFT(pNodeAnchor))
            RB_SET_LEFT(pNodeAnchor, pNode);
    }
    else
    {
        RB_SET_RIGHT(pNodeParent, pNode);
        if (pNodeParent == RB_RIGHT(pNodeAnchor))
            RB_SET_RIGHT(pNodeAnchor, pNode);
    }

    // rebalance
    while (pNode != RB_PARENT(pNodeAnchor))
    {
        unsigned int p = RB_PARENT(pNode);
        if (RB_COLOR(p) != kRBTreeColorRed)
            break;
        const unsigned int gp = RB_PARENT(p);
        if (p == RB_LEFT(gp))
        {
            const unsigned int uncle = RB_RIGHT(gp);
            if (uncle && RB_COLOR(uncle) == kRBTreeColorRed)
            {
                RB_SET_COLOR(p, kRBTreeColorBlack);
                pNode = gp;
                RB_SET_COLOR(uncle, kRBTreeColorBlack);
                RB_SET_COLOR(gp, kRBTreeColorRed);
                continue;
            }
            if (pNode == RB_RIGHT(p))
            {
                // RBTreeRotateLeft(p, root)
                const unsigned int pivot = pNode;
                unsigned int root = RB_PARENT(pNodeAnchor);
                pNode = p;
                RB_SET_RIGHT(pNode, RB_LEFT(pivot));
                if (const unsigned int t = RB_LEFT(pivot))
                    RB_SET_PARENT(t, pNode);
                RB_SET_PARENT(pivot, RB_PARENT(pNode));
                if (pNode == root)
                    root = pivot;
                else
                {
                    const unsigned int pp = RB_PARENT(pNode);
                    if (pNode == RB_LEFT(pp))
                        RB_SET_LEFT(pp, pivot);
                    else
                        RB_SET_RIGHT(pp, pivot);
                }
                RB_SET_LEFT(pivot, pNode);
                RB_SET_PARENT(pNode, pivot);
                RB_SET_PARENT(pNodeAnchor, root);
            }
            RB_SET_COLOR(RB_PARENT(pNode), kRBTreeColorBlack);
            RB_SET_COLOR(gp, kRBTreeColorRed);
            {
                // RBTreeRotateRight(gp, root)
                const unsigned int pivot = RB_LEFT(gp);
                unsigned int root = RB_PARENT(pNodeAnchor);
                RB_SET_LEFT(gp, RB_RIGHT(pivot));
                if (const unsigned int t = RB_RIGHT(pivot))
                    RB_SET_PARENT(t, gp);
                RB_SET_PARENT(pivot, RB_PARENT(gp));
                if (gp == root)
                {
                    root = pivot;
                    RB_SET_RIGHT(pivot, gp);
                }
                else
                {
                    const unsigned int pp = RB_PARENT(gp);
                    if (gp == RB_RIGHT(pp))
                        RB_SET_RIGHT(pp, pivot);
                    else
                        RB_SET_LEFT(pp, pivot);
                    RB_SET_RIGHT(pivot, gp);
                }
                RB_SET_PARENT(gp, pivot);
                RB_SET_PARENT(pNodeAnchor, root);
            }
        }
        else
        {
            const unsigned int uncle = RB_LEFT(gp);
            if (uncle && RB_COLOR(uncle) == kRBTreeColorRed)
            {
                RB_SET_COLOR(p, kRBTreeColorBlack);
                pNode = gp;
                RB_SET_COLOR(uncle, kRBTreeColorBlack);
                RB_SET_COLOR(gp, kRBTreeColorRed);
                continue;
            }
            if (pNode == RB_LEFT(p))
            {
                // RBTreeRotateRight(p, root)
                const unsigned int pivot = pNode;
                unsigned int root = RB_PARENT(pNodeAnchor);
                pNode = p;
                RB_SET_LEFT(pNode, RB_RIGHT(pivot));
                if (const unsigned int t = RB_RIGHT(pivot))
                    RB_SET_PARENT(t, pNode);
                RB_SET_PARENT(pivot, RB_PARENT(pNode));
                if (pNode == root)
                    root = pivot;
                else
                {
                    const unsigned int pp = RB_PARENT(pNode);
                    if (pNode == RB_RIGHT(pp))
                        RB_SET_RIGHT(pp, pivot);
                    else
                        RB_SET_LEFT(pp, pivot);
                }
                RB_SET_RIGHT(pivot, pNode);
                RB_SET_PARENT(pNode, pivot);
                RB_SET_PARENT(pNodeAnchor, root);
            }
            RB_SET_COLOR(RB_PARENT(pNode), kRBTreeColorBlack);
            RB_SET_COLOR(gp, kRBTreeColorRed);
            {
                // RBTreeRotateLeft(gp, root)
                const unsigned int pivot = RB_RIGHT(gp);
                unsigned int root = RB_PARENT(pNodeAnchor);
                RB_SET_RIGHT(gp, RB_LEFT(pivot));
                if (const unsigned int t = RB_LEFT(pivot))
                    RB_SET_PARENT(t, gp);
                RB_SET_PARENT(pivot, RB_PARENT(gp));
                if (gp == root)
                    root = pivot;
                else
                {
                    const unsigned int pp = RB_PARENT(gp);
                    if (gp == RB_LEFT(pp))
                        RB_SET_LEFT(pp, pivot);
                    else
                        RB_SET_RIGHT(pp, pivot);
                }
                RB_SET_LEFT(pivot, gp);
                RB_SET_PARENT(gp, pivot);
                RB_SET_PARENT(pNodeAnchor, root);
            }
        }
    }
    RB_SET_COLOR(RB_PARENT(pNodeAnchor), kRBTreeColorBlack);
}

#undef RB_RIGHT
#undef RB_LEFT
#undef RB_PARENT
#undef RB_COLOR
#undef RB_SET_RIGHT
#undef RB_SET_LEFT
#undef RB_SET_PARENT
#undef RB_SET_COLOR

}   // namespace eastl
