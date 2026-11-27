// RETAIL-only render-view functions (skate3recomp): written from retail's own listings where LTCG changed
// the function against sk83's decomp (game/source/render/renderview.cpp keeps the sk83 versions).
// Built for guest memory (RWP_GUEST); calls into not-decompiled code go through RWP_CALL on the caller's
// context. Proven per function with --sk83_replay (tools/sk83_port/PERF_PORT_2026-11.md).
#include "rwp_guestcall.h"

namespace Sk8 { namespace Render { namespace Retail {

static const unsigned int kD3DDevice = 0x8308446Cu;   // g_pD3DDevice (lis -31992 / 17516)

// Sk8::Render::RenderMesh, retail 0x827FAEA8 (sk83 0x8297A5F8). Retail differs from sk83: no wireframe debug
// option, no PIX markers, no draw stats; MeshContext island count is a u16 at +58, the visible-island
// index list holds u16s (+64), the island draw list (+72) has 16-byte {primitive, baseVertex, startIndex,
// indexCount} entries; SetStreamSource takes (device, stream, vb, offset, 1) (sk83 had one more argument).
void RenderMesh(unsigned int mc, unsigned int vpState)
{
    RWP_GUEST_FRAME(frame, 112);
    RWP_CALL(0x82cfcac8u, StateShadow_SetVertexProgramState, vpState);
    const unsigned int mesh = RWP_GLOBAL(RWP_U32, RWP_GLOBAL(RWP_U32, mc + 0u) + 0u);
    RWP_CALL(0x82b79190u, D3DDevice_SetIndices, (unsigned int)RWP_GLOBAL(RWP_U32, kD3DDevice),
             (unsigned int)RWP_GLOBAL(RWP_U32, mesh + 48u));
    RWP_CALL(0x82b78ff0u, D3DDevice_SetStreamSource, (unsigned int)RWP_GLOBAL(RWP_U32, kD3DDevice), 0u,
             (unsigned int)RWP_GLOBAL(RWP_U32, mesh + 52u), 0u, 1u);
    for (unsigned int i = 0; i < (unsigned int)RWP_GLOBAL(RWP_U16, mc + 58u); i = (i + 1u) & 0xffffu)
    {
        const unsigned int idx = RWP_GLOBAL(RWP_U16, RWP_GLOBAL(RWP_U32, mc + 64u) + 2u * i);
        const unsigned int p = RWP_GLOBAL(RWP_U32, mc + 72u) + (idx << 4);
        RWP_CALL(0x82b7ad68u, D3DDevice_DrawIndexedVertices, (unsigned int)RWP_GLOBAL(RWP_U32, p + 0u),
                 (unsigned int)RWP_GLOBAL(RWP_U32, p + 4u), (unsigned int)RWP_GLOBAL(RWP_U32, p + 8u),
                 (unsigned int)RWP_GLOBAL(RWP_U32, p + 12u));
    }
}

// ShaderSystem::ResetCurrentEffect, inlined in retail: the four current-effect words cleared
static __forceinline void ResetCurrentEffect()
{
    RWP_GLOBAL(RWP_U32, 0x830846F4u) = 0u;
    RWP_GLOBAL(RWP_U32, 0x830846F8u) = 0u;
    RWP_GLOBAL(RWP_U32, 0x830846FCu) = 0u;
    RWP_GLOBAL(RWP_U32, 0x83084700u) = 0u;
}

// SceneRenderView draw-list dispatch (RenderMeshes), retail 0x827FAF50 (sk83 0x829A77B8): keys [first, end)
// of the 8-byte {sort key, MeshContext*} vector; single-pass effects draw right away, multipass meshes are
// collected (view+20176 eastl vector) and then drawn pass by pass. Retail differs from sk83: no asserts,
// no mesh stats, ResetCurrentEffect inlined (and not called at entry/exit), pass counter u16 at mc+62,
// effect at mc+4, render test at mc+76, effect pass count +68, vertex-program state +100, DispatchEffect
// 0x82CA5DF0, RenderMesh 0x827FAEA8 (called through the guest so its own hook decides).
void RenderMeshes(unsigned int self, unsigned int keys, unsigned int first, unsigned int end)
{
    if (end == 0)
        return;
    RWP_GUEST_FRAME(frame, 208);
    const unsigned int multi = self + 20176u;   // {begin, end, capacity}
    RWP_GLOBAL(RWP_U32, multi + 4u) = (unsigned int)RWP_GLOBAL(RWP_U32, multi + 0u);
    int maxPasses = 0;
    unsigned int key = RWP_GLOBAL(RWP_U32, keys + 0u) + (first << 3) + 4u;   // -> MeshContext*
    for (unsigned int i = first; i < end; ++i, key += 8u)
    {
        const unsigned int mc = RWP_GLOBAL(RWP_U32, key);
        const unsigned int effect = RWP_GLOBAL(RWP_U32, mc + 4u);
        const unsigned int test = RWP_GLOBAL(RWP_U32, mc + 76u);
        RWP_ST32(frame.at(80), mc);   // the value DoInsertValue takes by address
        const int passes = (int)RWP_GLOBAL(RWP_U32, effect + 68u);
        if (test)
        {
            RWP_VCALL(test, 0, mc);   // test->Test(mc)
            ResetCurrentEffect();
        }
        if (passes > 1)
        {
            RWP_GLOBAL(RWP_U16, mc + 62u) = (unsigned short)(passes & 0xff);
            const unsigned int endp = RWP_GLOBAL(RWP_U32, multi + 4u);
            if (endp < (unsigned int)RWP_GLOBAL(RWP_U32, multi + 8u))
            {
                RWP_GLOBAL(RWP_U32, multi + 4u) = endp + 4u;
                if (endp)
                    RWP_GLOBAL(RWP_U32, endp) = mc;
            }
            else
                RWP_CALL(0x82813f20u, MeshContextVector_DoInsertValue, multi, endp, frame.at(80));
            maxPasses = passes < maxPasses ? maxPasses : passes;
        }
        else
        {
            RWP_VCALL(effect, 8, 0u);                          // effect->SetPass(0)
            RWP_CALL(0x82ca5df0u, ShaderSystem_DispatchEffect, effect);
            RWP_CALL(0x827faea8u, RenderMesh, mc, (unsigned int)RWP_GLOBAL(RWP_U32, effect + 100u));
        }
    }
    const unsigned int n = (unsigned int)((int)(RWP_GLOBAL(RWP_U32, multi + 4u) - RWP_GLOBAL(RWP_U32, multi + 0u)) >> 2);
    for (int pass = 0; pass < maxPasses; ++pass)
    {
        for (unsigned int k = 0; k < n; ++k)
        {
            const unsigned int mc = RWP_GLOBAL(RWP_U32, RWP_GLOBAL(RWP_U32, multi + 0u) + 4u * k);
            const unsigned int left = RWP_GLOBAL(RWP_U16, mc + 62u);
            if (left)
            {
                RWP_GLOBAL(RWP_U16, mc + 62u) = (unsigned short)(left + 0xffffu);
                const unsigned int effect = RWP_GLOBAL(RWP_U32, mc + 4u);
                RWP_VCALL(effect, 8, (unsigned int)pass);      // effect->SetPass(pass)
                RWP_CALL(0x82ca5df0u, ShaderSystem_DispatchEffect, effect);
                RWP_CALL(0x827faea8u, RenderMesh, mc,
                         (unsigned int)RWP_GLOBAL(RWP_U32, RWP_GLOBAL(RWP_U32, mc + 4u) + 100u));
            }
        }
        ResetCurrentEffect();
    }
}

} } }
