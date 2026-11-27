// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (11); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 829295f0 82929c40 8296de58 8296def8 8296df78 82973ae8 82974c38 8297a3b8 8297a478 8298b640 829a1b78 829a77b8 82ab8c68 82d77458 82d9e330 82f46de0 82f63b00 83367290 833689c8 833689e8 83368e2c 83368e30 83368e34 83368e38 83368e4c 83368e50 83368e6c 83369c7c 83369ca4 8336a030 8336a404 833a5ea0
// game/source/render/renderview.cpp (sk83 assert paths): the scene views' mesh drawing.
//   Sk8::Render::SceneRenderView::RenderMeshes   sk83_na_m 0x829a77b8 (0x238)
//   RenderMesh                                   sk83_na_m 0x8297a5f8 (0x134)
//   Sk8::Render::SceneRenderView::RenderZPrePass sk83_na_m 0x8297a478 (0x180)
//   RenderMeshZ (the z-prepass mesh draw)        sk83_na_m 0x829712a8 (0xb8)
//   PerfMarker (compiled out: blr)               sk83_na_m 0x831f4b98
//   Sk8::Render::SceneRenderView::Render         sk83_na_m 0x829a9e00 (0x138)
//   Sk8::Render::SceneRenderView::PrepZPrePass   sk83_na_m 0x829a6238 (0x170)
//   RenderShadowMesh                             sk83_na_m 0x82971360 (0xb8)
//   Sk8::Render::ShadowView::Render              sk83_na_m 0x8297b3a0 (0x210)
//   Sk8::Render::SceneRenderView::Sort           sk83_na_m 0x8298e6f8 (0xf0)
//   the sort keys' radix sort                    sk83_na_m 0x82980cf8 (0x164)
//   IsInLightView(Matrix44 &, const tAABB &)     sk83_na_m 0x8297af48 (0x25c)
//   shadow caster gather, CSM cascades           sk83_na_m 0x829a7c48 (0x1e4; assert line 1439)
//   shadow caster gather, second shadow set      sk83_na_m 0x829a7e30 (0x1d4; assert line 1481)
//   scene view culling + visible entity list     sk83_na_m 0x829a7608 (0x1ac; no name in the map)
// Hand-decompiled from the sk83 code; names from the Skate 2 PDB (same file there; sk83 reworked
// the effect dispatch, so the Skate 2 bodies only guide the naming). Verified on real game state
// with sk83_na_m --sk83_replay (snapshots of RenderMeshes calls in the flythrough).
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "sk8_render_types.h"
#include "../job/bufferediterativejob.h"
#include <string.h>

namespace Sk8 { namespace ShaderSystem { void ResetCurrentEffect(); } }   // materialman.cpp

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

// the debug-render settings RenderMesh consults (attrib-backed: a pointer override or a byte)
struct RenderDebugSettings
{
    RWP_U8 pad[0xcd];
    RWP_U8 m_bFlagDefault;              // 0xcd
    RWP_U8 pad2[2];
    RWP_FIELD_PTR(RWP_U8) m_pFlag;      // 0xd0
};
struct RenderDebugRoot { RWP_U32 pad[8]; RWP_FIELD_PTR(RenderDebugSettings) m_pSettings; };   // +0x20

#define g_pD3DDevice        RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define g_RenderDebugArg    RWP_GLOBAL(RWP_U32, 0x5fff0000)    // the rasterizer state RenderMesh forces when the option is on
#define g_ViewRasterizer    RWP_GLOBAL(RWP_U32, 0x5fff0000)    // the one Render restores afterwards
#define g_StatSortKeys      RWP_GLOBAL(RWP_U32, 0x5fff0000)    // = meshes in the view's sort list
#define g_pGPRSplit         RWP_GLOBAL(RWP_U32, 0x5fff0000)    // -> +0xd8: pixel shader GPRs (vertex shaders get 0x80 - that)
#define g_ZPrePassMaxDist   RWP_GLOBAL(RWP_F32, 0x5fff0000)    // default z-prepass distance (attribute overrides it)
#define g_pPresentation     RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define g_pGraphicsMan        RWP_GLOBAL(RWP_U32, 0x5fff0000)    // XBox2GraphicsMan (DebugRenderMesh's this; +0x42d84 a marker argument, +0x42a28 the z-prepass effect)
#define g_PerfMarkerTable   g_pGraphicsMan
#define g_StatZMeshes       RWP_GLOBAL(RWP_U32, 0x5fff0000)    // = meshes in the z prepass
#define g_StatZDraws        RWP_GLOBAL(RWP_U32, 0x5fff0000)    // += MeshContext +0x24 per z-prepass mesh
#define g_StatMeshes        RWP_GLOBAL(RWP_U32, 0x5fff0000)    // += MeshContext +0x24 per mesh
#define g_StatA             RWP_GLOBAL(RWP_U32, 0x5fff0000)    // += MeshContext +0x48 per mesh
#define g_StatB             RWP_GLOBAL(RWP_U32, 0x5fff0000)    // += MeshContext +0x44 per mesh

#if !defined(RWP_GUEST)
extern "C" {
void D3DDevice_SetIndices(unsigned int dev, unsigned int ib);
void D3DDevice_SetStreamSource(unsigned int dev, unsigned int stream, unsigned int vb, unsigned int offset,
                               unsigned int stride, unsigned int flags);
void D3DDevice_DrawIndexedVertices(unsigned int dev, unsigned int prim, unsigned int base, unsigned int start,
                                   unsigned int count);
}
void StateShadow_SetVertexProgramState(unsigned int vpState);
void StateShadow_SetRasterizerState(unsigned int s);
unsigned int Attrib_GetAttributePointer(unsigned int inst, unsigned long long key, unsigned int idx);
void CameraPresentation_GetMatrix(unsigned int out, unsigned int cam, unsigned int b);
void SceneRenderView_UpdateLODs(unsigned int self);
void PresentationEntity_UpdateLOD(unsigned int ent, unsigned int lod);
void PresentationEntity_Tag(unsigned int ent, unsigned int tag);
void GraphicsMan_DebugRenderMesh(unsigned int man, unsigned int mc, unsigned int vps);
void ShadowView_Finish(unsigned int self, unsigned int meshes, unsigned int count);
void EffectDispatch(unsigned int effect);
void StateShadow_Reset();
void BufferedIterativeJob_Flush(unsigned int job);
void Job_WaitOn(unsigned int job, unsigned int fn, unsigned int arg, int timeout);
void SortKeys(unsigned int begin, unsigned int end, unsigned int ctx, unsigned int b, unsigned int c);
void SceneRenderView_RenderMeshes_Hooked(unsigned int self, unsigned int keys, unsigned int first, unsigned int end);
void SceneRenderView_RenderZPrePass_Hooked(unsigned int self);
void EffectDispatch(unsigned int effect);
void StateShadow_Reset();
void BufferedIterativeJob_Flush(unsigned int job);
void Job_WaitOn(unsigned int job, unsigned int fn, unsigned int arg, int timeout);
void SortKeys(unsigned int begin, unsigned int end, unsigned int ctx, unsigned int b, unsigned int c);
void EffectBegin(unsigned int fx, unsigned int arg);
extern "C" void D3DDevice_SetShaderGPRAllocation(unsigned int dev, unsigned int flags, unsigned int vs, unsigned int ps);
extern "C" void D3DDevice_SetPixelShader(unsigned int dev, unsigned int ps);
void DoInsertValue(MeshContextVector *v, RWP_FIELD_PTR(MeshContext) *pos, RWP_FIELD_PTR(MeshContext) *value);
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
#endif

// sub_831f4b98: a profiling marker, compiled out in this build (just blr)
void PerfMarker(unsigned int) {}

// a render option (attrib-backed settings under root + 0x20: a pointer override or a byte)
static __forceinline bool RenderOption(unsigned int root)
{
    RWP_PTR(RenderDebugSettings) dbg = RWP_GLOBAL(RenderDebugRoot, root).m_pSettings;
    RWP_PTR(RWP_U8) flag = dbg->m_pFlag;
    const unsigned char on = flag ? (unsigned char)*flag : (unsigned char)dbg->m_bFlagDefault;
    return on != 0;
}
#define OPTION_WIREFRAME  0x5fff0000u
#define OPTION_ZPREPASS   0x5fff0000u

void RenderMesh(RWP_PTR(MeshContext) mc, unsigned int vpState)
{
    RWP_GUEST_FRAME(frame, 0x90);
    if (RenderOption(OPTION_WIREFRAME))
        RWP_CALL(0x82cfc990, StateShadow_SetRasterizerState, g_RenderDebugArg);
    RWP_CALL(0x82cfcac8, StateShadow_SetVertexProgramState, vpState);
    PerfMarker(RWP_GLOBAL(RWP_U32, (unsigned int)g_PerfMarkerTable + 0x42d84));
    RWP_PTR(RWP_FIELD_PTR(pegasus::tRModelData::tMeshData)) meshRef = mc->m_pMesh;
    RWP_PTR(pegasus::tRModelData::tMeshData) mesh = *meshRef;
    RWP_CALL(0x82b79190, D3DDevice_SetIndices, g_pD3DDevice, mesh->m_pIndexBuffer);
    RWP_CALL(0x82b78ff0, D3DDevice_SetStreamSource, g_pD3DDevice, 0u, mesh->m_pVertexBuffer, 0u, 0u, 1u);
    for (unsigned int i = 0; i < mc->m_uiNumVisibleIslands; i++)
    {
        RWP_PTR(RWP_U32) indices = mc->m_uiVisibleIslandIndices;
        RWP_PTR(renderengine::DrawIndexedParameters) list = mc->m_islandDrawList;
        RWP_PTR(renderengine::DrawIndexedParameters) p = list + (unsigned int)indices[i];
        RWP_CALL(0x82b7ad68, D3DDevice_DrawIndexedVertices, g_pD3DDevice, p->primitiveType, p->baseVertexIndex,
                 p->startIndex, p->indexCount);
    }
    PerfMarker(RWP_GLOBAL(RWP_U32, (unsigned int)g_PerfMarkerTable + 0x42d84));
    const unsigned int a = g_StatA, b = g_StatB;
    g_StatA = mc->m_uiStatB + a;
    g_StatB = mc->m_uiStatA + b;
}

// a z-prepass mesh: no debug hook, no stats
void RenderMeshZ(RWP_PTR(MeshContext) mc, unsigned int vpState)
{
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_CALL(0x82cfcac8, StateShadow_SetVertexProgramState, vpState);
    PerfMarker(1u);
    RWP_PTR(RWP_FIELD_PTR(pegasus::tRModelData::tMeshData)) meshRef = mc->m_pMesh;
    RWP_PTR(pegasus::tRModelData::tMeshData) mesh = *meshRef;
    RWP_CALL(0x82b79190, D3DDevice_SetIndices, g_pD3DDevice, mesh->m_pIndexBuffer);
    RWP_CALL(0x82b78ff0, D3DDevice_SetStreamSource, g_pD3DDevice, 0u, mesh->m_pVertexBuffer, 0u, 0u, 1u);
    for (unsigned int i = 0; i < mc->m_uiNumVisibleIslands; i++)
    {
        RWP_PTR(RWP_U32) indices = mc->m_uiVisibleIslandIndices;
        RWP_PTR(renderengine::DrawIndexedParameters) list = mc->m_islandDrawList;
        RWP_PTR(renderengine::DrawIndexedParameters) p = list + (unsigned int)indices[i];
        RWP_CALL(0x82b7ad68, D3DDevice_DrawIndexedVertices, g_pD3DDevice, p->primitiveType, p->baseVertexIndex,
                 p->startIndex, p->indexCount);
    }
    PerfMarker(1u);
}

// Depth only: the z-prepass effect with no pixel shader, every listed mesh with its own world
// matrix (the effect's matrix bind gets the four rows by address) and its simple vertex program.
void SceneRenderView_RenderZPrePass(RWP_PTR(SceneRenderView) self)
{
    MeshContextVector &list = self->m_ZPrePassMeshes;
    unsigned int n = (unsigned int)((int)(RWP_ADDR_OF((RWP_PTR(RWP_FIELD_PTR(MeshContext)))list.mpEnd) -
                                          RWP_ADDR_OF((RWP_PTR(RWP_FIELD_PTR(MeshContext)))list.mpBegin)) >> 2);
    if (n == 0)
        return;
    RWP_GUEST_FRAME(frame, 0xd0);
    const unsigned int fx = RWP_GLOBAL(RWP_U32, (unsigned int)g_pGraphicsMan + 0x42a28);   // the z-prepass effect
    RWP_VCALL(fx, 4, 0x2ea8fb98u);                      // technique by name hash
    RWP_VCALL(fx, 8, 0u);                               // pass 0
    RWP_VCALL(RWP_GLOBAL(RWP_U32, fx + 0x14), 8, 0x7ddf552bu, FRAME_AT(frame, 0x50), 4u);   // parameter by name hash, into the frame
    RWP_CALL(0x5fff0000, D3DDevice_SetShaderGPRAllocation, g_pD3DDevice, 0u, 0x70u, 0x10u);
    RWP_CALL(0x82ca5d10, EffectBegin, fx, 3u);
    RWP_CALL(0x82b7f150, D3DDevice_SetPixelShader, g_pD3DDevice, 0u);
    {
        RWP_PTR(MeshContext) first = list.mpBegin[0];
        RWP_CALL(0x82cfcac8, StateShadow_SetVertexProgramState, (unsigned int)first->m_pSimpleVPState);
    }
    g_StatZMeshes = n;
    unsigned int k = 0;
    do
    {
        RWP_PTR(MeshContext) mc = list.mpBegin[k];
        // the world matrix's four rows into the frame (lvx/stvx), then the effect's matrix bind
#if defined(RWP_GUEST)
        std::memcpy(g_base + frame.at(0x50), g_base + (u32)mc->m_pWorldMatrix, 64);
#else
        std::memcpy((void *)FRAME_AT(frame, 0x50), (const void *)(size_t)(unsigned int)mc->m_pWorldMatrix, 64);
#endif
        RWP_FNCALL(RWP_GLOBAL(RWP_U32, fx + 0x50), RWP_GLOBAL(RWP_U32, fx + 0x14), 0u, 0u, FRAME_AT(frame, 0x70),
                   FRAME_AT(frame, 0x60), FRAME_AT(frame, 0x50));
        g_StatZDraws = mc->m_uiNumVisibleIslands + g_StatZDraws;
        RenderMeshZ(mc, mc->m_pSimpleVPState);
        k++;
    } while (--n);
}

// one mesh with the current pass of its effect (multipass meshes re-read the effect for the
// vertex program state, single-pass ones use the one they started with)
static __forceinline void DrawPass(RWP_PTR(MeshContext) mc, unsigned int effect, unsigned int pass, bool reread)
{
    RWP_VCALL(effect, 8, pass);                         // effect->SetPass(pass)
    RWP_CALL(0x5fff0000, EffectDispatch, effect);
    g_StatMeshes = mc->m_uiNumVisibleIslands + g_StatMeshes;
    const unsigned int e = reread ? (unsigned int)mc->m_pEffectList : effect;
    RenderMesh(mc, RWP_GLOBAL(RWP_U32, e + 0x64));
}

// Draws the meshes of keys [first, end): single-pass effects right away, multipass ones collected
// and then drawn pass by pass (pass p for every mesh that still has passes left).
void SceneRenderView_RenderMeshes(RWP_PTR(SceneRenderView) self, RWP_PTR(SortKeyVector) keys, unsigned int first,
                                  unsigned int end)
{
    if (end == 0)
        return;
    RWP_GUEST_FRAME(frame, 0xb0);
    MeshContextVector &multi = self->m_MultipassMeshes;
    multi.mpEnd = multi.mpBegin;
    int maxPasses = 0;
    RWP_PTR(SortKey) key = (RWP_PTR(SortKey))keys->mpBegin + first;
    unsigned int i = first;
    do
    {
        RWP_PTR(MeshContext) mc = key->mpMshCntxt;
        // (asserts: mc != 0 line 899, mc->m_pEffectList != 0 line 920)
        const unsigned int effect = mc->m_pEffectList;
        const unsigned int test = mc->m_pRenderTest;
        const int passes = (int)RWP_GLOBAL(RWP_S32, effect + 0x44);
        if (test)
        {
            RWP_VCALL(test, 0, RWP_ADDR_OF(mc));
            ShaderSystem::ResetCurrentEffect();
        }
        if (passes > 1)
        {
            // (assert passes < 256, line 934)
            mc->m_PassCount = (unsigned char)passes;
#if defined(RWP_GUEST)
            const u32 endp = multi.mpEnd.raw ? (u32)__builtin_bswap32(multi.mpEnd.raw) : 0u;
            const u32 cap = (u32)__builtin_bswap32(multi.mpCapacity.raw);
            if (endp < cap)
            {
                multi.mpEnd = RWP_FROM_ADDR(RWP_FIELD_PTR(MeshContext), endp + 4);
                if (endp)
                    GStore32(endp, RWP_ADDR_OF(mc));
            }
            else
            {
                GStore32(frame.at(0x50), RWP_ADDR_OF(mc));     // the value is passed by address
                rwp::Call(0x82ed5ca8, RWP_ADDR_OF(self) + 0x4fec, endp, frame.at(0x50));
            }
#else
            RWP_FIELD_PTR(MeshContext) *endp = multi.mpEnd;
            if (endp < multi.mpCapacity)
            {
                multi.mpEnd = endp + 1;
                if (endp)
                    *endp = mc;
            }
            else
            {
                RWP_FIELD_PTR(MeshContext) value = mc;
                DoInsertValue(&multi, endp, &value);
            }
#endif
            maxPasses = passes < maxPasses ? maxPasses : passes;
        }
        else
            DrawPass(mc, effect, 0u, false);
        key += 1;
    } while (++i < end);
    const unsigned int n = (unsigned int)((int)(RWP_ADDR_OF((RWP_PTR(RWP_FIELD_PTR(MeshContext)))multi.mpEnd) -
                                                RWP_ADDR_OF((RWP_PTR(RWP_FIELD_PTR(MeshContext)))multi.mpBegin)) >> 2);
    for (int pass = 0; pass < maxPasses; pass++)
    {
        for (unsigned int k = 0; k < n; k++)
        {
            RWP_PTR(MeshContext) mc = multi.mpBegin[k];
            if (mc->m_PassCount)
            {
                mc->m_PassCount = (unsigned char)(mc->m_PassCount + 0xff);
                DrawPass(mc, mc->m_pEffectList, (unsigned int)pass, true);
            }
        }
        ShaderSystem::ResetCurrentEffect();
    }
}

// The meshes nearer than the z-prepass distance (attribute, else a default) whose effect is in the
// view's z-prepass group go into the z-prepass list; every mesh gets its distance in front of the
// camera's near plane (the AABB corner nearest along the plane normal).
void SceneRenderView_PrepZPrePass(RWP_PTR(SceneRenderView) self)
{
    RWP_GUEST_FRAME(frame, 0x100);
    MeshContextVector &zl = self->m_ZPrePassMeshes;
    RWP_PTR(SortKey) key = (RWP_PTR(SortKey))self->m_SortKeys.mpBegin;
    unsigned int n = (unsigned int)((int)(RWP_ADDR_OF((RWP_PTR(SortKey))self->m_SortKeys.mpEnd) - RWP_ADDR_OF(key)) >> 3);
    zl.mpEnd = zl.mpBegin;
    float maxDist = g_ZPrePassMaxDist;
    const unsigned int attr = RWP_CALL(0x5fff0000, Attrib_GetAttributePointer, RWP_ADDR_OF(self) + 0x5718u,
                                       0x93ba4491d0a633fbull, 0u);
    if (attr)
        maxDist = RWP_GLOBAL(RWP_F32, attr);
    RWP_CALL(0x5fff0000, CameraPresentation_GetMatrix, FRAME_AT(frame, 0x70), (unsigned int)g_pPresentation + 0x29a90u, 1u);
    if (n == 0)
        return;
    do
    {
        RWP_VMX_MODE();
        const __vector4 zero = __vspltisw(0);
        const __vector4 plane = RWP_GLOBAL(RWP_VEC, (unsigned int)self->m_pCamera + 0x260u);
        RWP_PTR(MeshContext) mc = key->mpMshCntxt;
        RWP_GLOBAL(RWP_U32, FRAME_AT(frame, 0x50)) = RWP_ADDR_OF(mc);
        const unsigned int aabb = mc->m_aabb;
        const __vector4 vmin = RWP_GLOBAL(RWP_VEC, aabb), vmax = RWP_GLOBAL(RWP_VEC, aabb + 0x10);
        const __vector4 corner = __vsel(vmax, vmin, __vcmpgefp(plane, zero));
        const __vector4 d = __vaddfp(__vmsum3fp(corner, plane), __vspltw(plane, 3));
        const float dist = VLANE_F(d, 0);
        RWP_FPU_MODE();
        if (dist < maxDist && (int)RWP_GLOBAL(RWP_S32, (unsigned int)mc->m_pEffectList + 0x38u) == (int)self->m_iZPrePassGroup)
        {
#if defined(RWP_GUEST)
            const u32 endp = (u32)__builtin_bswap32(zl.mpEnd.raw);
            if (endp < (u32)__builtin_bswap32(zl.mpCapacity.raw))
            {
                zl.mpEnd = RWP_FROM_ADDR(RWP_FIELD_PTR(MeshContext), endp + 4);
                if (endp)
                    GStore32(endp, RWP_ADDR_OF(mc));
            }
            else
                rwp::Call(0x5fff0000, RWP_ADDR_OF(self) + 0x4ee0u, endp, frame.at(0x50));   // fixed_vector DoInsertValue
#else
            RWP_FIELD_PTR(MeshContext) *endp = zl.mpEnd;
            if (endp < zl.mpCapacity)
            {
                zl.mpEnd = endp + 1;
                if (endp)
                    *endp = mc;
            }
            else
                DoInsertValue(&zl, endp, (RWP_FIELD_PTR(MeshContext) *)FRAME_AT(frame, 0x50));
#endif
        }
        mc->m_DistFromCam = dist;
        key += 1;
    } while (--n);
}

// The scene view's render: z prepass (when enabled), the GPR split, the sorted meshes (through
// the hooked RenderMeshes), the post-render entities, the wireframe option's rasterizer restore,
// then the LOD update. Nothing at all for an empty view.
void SceneRenderView_Render(RWP_PTR(SceneRenderView) self)
{
    RWP_GUEST_FRAME(frame, 0x80);
    const unsigned int n = (unsigned int)((int)(RWP_ADDR_OF((RWP_PTR(SortKey))self->m_SortKeys.mpEnd) -
                                                RWP_ADDR_OF((RWP_PTR(SortKey))self->m_SortKeys.mpBegin)) >> 3);
    if (n == 0)
        return;
    g_StatSortKeys = n;
    if (RenderOption(OPTION_ZPREPASS))
    {
        SceneRenderView_PrepZPrePass(self);
        // through the recompiled address like RenderMeshes: the game's hook there may skip the pass
        RWP_CALL(0x5fff0000, SceneRenderView_RenderZPrePass_Hooked, RWP_ADDR_OF(self));
    }
    const unsigned int gpr = RWP_GLOBAL(RWP_U32, (unsigned int)g_pGPRSplit + 0xd8u);
    RWP_CALL(0x5fff0000, D3DDevice_SetShaderGPRAllocation, g_pD3DDevice, 0u, 0x80u - gpr, gpr);
    RWP_CALL(0x5fff0000, SceneRenderView_RenderMeshes_Hooked, RWP_ADDR_OF(self), RWP_ADDR_OF(self) + 0x4ed0u, 0u, n);
    PtrVector &post = self->m_PostRenderEntities;
    unsigned int m = (unsigned int)((int)((unsigned int)post.mpEnd - (unsigned int)post.mpBegin) >> 2);
    for (unsigned int k = 0; k < m; k++)
        RWP_VCALL(RWP_GLOBAL(RWP_U32, (unsigned int)post.mpBegin + 4 * k), 0x4c);
    if (RenderOption(OPTION_WIREFRAME))
        RWP_CALL(0x82cfc990, StateShadow_SetRasterizerState, g_ViewRasterizer);
    RWP_CALL(0x5fff0000, SceneRenderView_UpdateLODs, RWP_ADDR_OF(self));
}

// a mesh into a shadow map: the shadow island list (+0x28 / +0x38)
void RenderShadowMesh(RWP_PTR(MeshContext) mc, unsigned int vpState)
{
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_CALL(0x82cfcac8, StateShadow_SetVertexProgramState, vpState);
    PerfMarker(1u);
    RWP_PTR(RWP_FIELD_PTR(pegasus::tRModelData::tMeshData)) meshRef = mc->m_pMesh;
    RWP_PTR(pegasus::tRModelData::tMeshData) mesh = *meshRef;
    RWP_CALL(0x82b79190, D3DDevice_SetIndices, g_pD3DDevice, mesh->m_pIndexBuffer);
    RWP_CALL(0x82b78ff0, D3DDevice_SetStreamSource, g_pD3DDevice, 0u, mesh->m_pVertexBuffer, 0u, 0u, 1u);
    for (unsigned int i = 0; i < mc->m_uiShadowNumVisibleIslands; i++)
    {
        RWP_PTR(RWP_U32) indices = mc->m_uiShadowVisibleIslandIndices;
        RWP_PTR(renderengine::DrawIndexedParameters) list = mc->m_islandDrawList;
        RWP_PTR(renderengine::DrawIndexedParameters) p = list + (unsigned int)indices[i];
        RWP_CALL(0x82b7ad68, D3DDevice_DrawIndexedVertices, g_pD3DDevice, p->primitiveType, p->baseVertexIndex,
                 p->startIndex, p->indexCount);
    }
    PerfMarker(1u);
}

#define OPTION_SHADOW_LOD 0x5fff0000u   // draw shadow casters at LOD 2

// One cascade of a shadow view: every caster entity of the cascade's list (the world view keeps
// separate lists, +0x4f5 * 16, the others +0x4f2 * 16) with its shadow meshes, each mesh's effect
// dispatched and drawn by the graphics manager; optionally at LOD 2 (restored afterwards).
void ShadowView_Render(unsigned int self, unsigned int cascade, unsigned char world)
{
    RWP_GUEST_FRAME(frame, 0xc0);
    // (asserts: cascade < 2 for the world view, line 1586; else cascade < 3, line 1590)
    RWP_GLOBAL(RWP_U32, self + 8) = 0u;
    RWP_GLOBAL(RWP_U32, self + 4) = 0u;
    RWP_GLOBAL(RWP_U32, self + 0x10) = 0u;
    const unsigned int list = self + 16 * (world ? cascade + 0x4f5 : cascade + 0x4f2);
    unsigned int n = (unsigned int)((int)((unsigned int)RWP_GLOBAL(RWP_U32, list + 4) - (unsigned int)RWP_GLOBAL(RWP_U32, list)) >> 2);
    if (n == 0)
        return;
    unsigned int ents = RWP_GLOBAL(RWP_U32, self + 16 * (world ? cascade + 0x4f5 : cascade + 0x4f2));
    RWP_CALL0(0x5fff0000, StateShadow_Reset);
    do
    {
        const unsigned int ent = RWP_GLOBAL(RWP_U32, ents);
        const unsigned int lod = RWP_GLOBAL(RWP_U32, ent + 0x10);
        if (RenderOption(OPTION_SHADOW_LOD))
            RWP_CALL(0x5fff0000, PresentationEntity_UpdateLOD, ent, 2u);
        unsigned int meshes = RWP_VCALL(ent, 0x24);         // the entity's shadow mesh list (a vector)
        const unsigned int m = (unsigned int)((int)((unsigned int)RWP_GLOBAL(RWP_U32, meshes + 4) - (unsigned int)RWP_GLOBAL(RWP_U32, meshes)) >> 2);
        if (m)
        {
            const unsigned int saved = RWP_GLOBAL(RWP_U32, ent + 4 * ((unsigned int)RWP_GLOBAL(RWP_U32, ent + 0x10) + 0x52));
            RWP_CALL(0x5fff0000, PresentationEntity_Tag, ent, 0xbb34865du);
            meshes = RWP_VCALL(ent, 0x24);
            const unsigned int first = RWP_GLOBAL(RWP_U32, meshes);
            for (unsigned int k = 0; k < m; k++)
            {
                const unsigned int mc = RWP_GLOBAL(RWP_U32, first + 4 * k);
                RWP_CALL(0x5fff0000, EffectDispatch, (unsigned int)RWP_GLOBAL(RWP_U32, mc + 4));
                const unsigned int mc2 = RWP_GLOBAL(RWP_U32, first + 4 * k);
                RWP_CALL(0x5fff0000, GraphicsMan_DebugRenderMesh, (unsigned int)g_pGraphicsMan, mc2,
                         (unsigned int)RWP_GLOBAL(RWP_U32, (unsigned int)RWP_GLOBAL(RWP_U32, mc2 + 4) + 0x64));
            }
            RWP_CALL(0x5fff0000, ShadowView_Finish, self, first, m);
            RWP_CALL(0x5fff0000, PresentationEntity_Tag, ent, saved);
        }
        if (RenderOption(OPTION_SHADOW_LOD))
            RWP_CALL(0x5fff0000, PresentationEntity_UpdateLOD, ent, lod);
        ents += 4;
    } while (--n);
}


// The view's sort list for this frame: clear it, finish the view's culling job (flush it when it
// still has queued work and the view runs it buffered, then wait), let every visible entity add
// its meshes (vtable +0x18), and sort when there is at least one key.
void SceneRenderView_Sort(RWP_PTR(SceneRenderView) self)
{
    RWP_GUEST_FRAME(frame, 0x90);
    const unsigned int s = RWP_ADDR_OF(self);
    self->m_SortKeys.mpEnd = self->m_SortKeys.mpBegin;
    const unsigned int job = s + 0x5390u;
    if (!RWP_GLOBAL(RWP_U8, s + 0x56e0u) && (unsigned int)RWP_GLOBAL(RWP_U32, job + 0x358u) != 0)
        RWP_CALL(0x5fff0000, BufferedIterativeJob_Flush, job);
    if (RWP_GLOBAL(RWP_U8, job + 0x350u))
    {
        RWP_CALL(0x5fff0000, Job_WaitOn, job, 0u, 0u, 0xffffffffu);
        RWP_GLOBAL(RWP_U8, job + 0x350u) = 0;
    }
    PtrVector &ents = self->m_PostRenderEntities;
    unsigned int n = (unsigned int)((int)((unsigned int)ents.mpEnd - (unsigned int)ents.mpBegin) >> 2);
    if (n == 0)
        return;
    const unsigned int first = ents.mpBegin;
    for (unsigned int k = 0; k < n; k++)
        RWP_VCALL(RWP_GLOBAL(RWP_U32, first + 4 * k), 0x18, s + 0x4ed0u);   // entity->AddToSortList(keys)
    const unsigned int b = RWP_ADDR_OF((RWP_PTR(SortKey))self->m_SortKeys.mpBegin);
    const unsigned int e = RWP_ADDR_OF((RWP_PTR(SortKey))self->m_SortKeys.mpEnd);
    if ((int)((e - b) & 0xfffffff8u) != 0)
    {
        RWP_GLOBAL(RWP_U8, FRAME_AT(frame, 0x50)) = 0;   // a bool passed by value from the frame
        RWP_CALL(0x828150e0, SortKeys, b, e, (unsigned int)RWP_GLOBAL(RWP_U32, s + 0x4ffcu), 0u, 0u);
    }
}


// LSD radix sort of the {key, MeshContext*} items in [begin, end) on the 32-bit key, 8 bits a
// pass, ping-ponging with tmp (four passes: the result ends up back in [begin, end)). The
// original keeps the histogram and offsets in its 0x8a0-byte frame; nothing reads them later.
void SortKeys(unsigned int begin, unsigned int end, unsigned int tmp)
{
    RWP_GUEST_FRAME(frame, 0x8a0);
    unsigned int src = begin, e = end, dst = tmp;
    for (unsigned int shift = 0; shift < 32; shift += 8)
    {
        unsigned int hist[256], off[256];
        for (int i = 0; i < 256; i++)
            hist[i] = 0;
        for (unsigned int q = src; q != e; q += 8)
            hist[(RWP_LD32(q) >> shift) & 0xffu]++;
        off[0] = 0;
        for (int i = 0; i < 255; i++)
            off[i + 1] = off[i] + hist[i];
        for (unsigned int q = src; q != e; q += 8)
        {
            const unsigned int k = RWP_LD32(q);
            const unsigned int b = (k >> shift) & 0xffu;
            const unsigned int d = dst + 8u * off[b];
            off[b]++;
            RWP_ST32(d, k);
            RWP_ST32(d + 4u, RWP_LD32(q + 4u));
        }
        const unsigned int n = (unsigned int)((int)(e - src) >> 3);
        const unsigned int old = src;
        src = dst;
        e = dst + 8u * n;
        dst = old;
    }
}


// IsInLightView: the box's eight corners through the light's view-projection; per corner, x and
// y against [-1, 1]. Returns eShadowCullType:
//   outside keeps a bit only while every corner is beyond that side (1: x < -1, 2: x > 1,
//   8: y < -1, 4: y > 1); inside loses a bit when any corner crosses a side (1: x < -1,
//   2: x > 1, 8: y < -1, 4: y > 1). outside != 0 -> 0 (all corners off one side), inside == 0xf -> 1 (no
//   corner outside), else 2 (straddles).
// The corner is a stack Vector4 that gets x, y, z from min/max and its w from the 1.0f the
// compiler spilled into that lane (stfs at r1-0x174 lands in the corner copy at r1-0x180). The
// function has no frame (red zone only); its stack traffic is reproduced below r1 as is.
#if defined(RWP_GUEST)
#define LV_RED(off) (sp + (unsigned int)(off))
#else
#define LV_RED(off) ((unsigned int)(size_t)(red + 0x1a0 + (off)))
#endif
#define LV_V(off) RWP_GLOBAL(RWP_VEC, LV_RED(off))
#define LV_F(off) RWP_GLOBAL(RWP_F32, LV_RED(off))
#define LV_U(off) RWP_GLOBAL(RWP_U32, LV_RED(off))
#define g_LightClipMin  0x8216dee0u     // -1.0f
#define g_LightClipMax  0x8231a844u     //  1.0f

unsigned int IsInLightView(unsigned int m, unsigned int box)
{
#if defined(RWP_GUEST)
    const unsigned int sp = rwp::g_ctx->r1.u32;
    RWP_SAVEGPRLR(28);
#else
    __declspec(align(16)) unsigned char red[0x1a0];
#endif
    const __vector4 vmin = RWP_GLOBAL(RWP_VEC, box), vmax = RWP_GLOBAL(RWP_VEC, box + 0x10u);
    const __vector4 r0 = RWP_GLOBAL(RWP_VEC, m), r1 = RWP_GLOBAL(RWP_VEC, m + 0x10u);
    const __vector4 r2 = RWP_GLOBAL(RWP_VEC, m + 0x20u), r3 = RWP_GLOBAL(RWP_VEC, m + 0x30u);
    LV_V(-0x150) = vmin;
    LV_V(-0x140) = vmax;
    RWP_FPU_MODE();
    const float lo = RWP_GLOBAL(RWP_F32, g_LightClipMin), hi = RWP_GLOBAL(RWP_F32, g_LightClipMax);
    unsigned int outside = 0xf, inside = 0xf;
    for (int i = 0; i < 8; i++)
    {
        RWP_FPU_MODE();
        LV_F(-0x170) = lo;
        // the corner: x, y, z from min / max by bits 0, 1, 2 of i
        const int slot = -0x130 + 16 * i;
        RWP_VMX_MODE();
        __vector4 c = LV_V(slot);
        c = __vrlimi(c, LV_V(-0x150 + 16 * (i & 1)), 8, 0);
        LV_V(slot) = c;
        c = __vrlimi(c, LV_V(-0x150 + 16 * ((i >> 1) & 1)), 4, 0);
        LV_U(-0x16c) = 0u;
        LV_U(-0x168) = 0u;
        LV_U(-0x164) = 0u;
        LV_V(slot) = c;
        c = __vrlimi(c, LV_V(-0x150 + 16 * ((i >> 2) & 1)), 2, 0);
        LV_V(slot) = c;
        LV_V(-0x180) = c;
        RWP_FPU_MODE();
        LV_F(-0x174) = hi;
        RWP_VMX_MODE();
        c = LV_V(-0x180);
        const __vector4 lov = __vspltw(LV_V(-0x170), 0);
        LV_V(slot) = c;
        __vector4 t = __vmulfp(r0, __vspltw(c, 0));
        t = __vmaddfp(r1, __vspltw(c, 1), t);
        t = __vmaddfp(r2, __vspltw(c, 2), t);
        t = __vmaddfp(r3, __vspltw(c, 3), t);
        LV_V(-0xb0 + 16 * i) = t;
        // x
        unsigned int o;
        const __vector4 x = __vspltw(t, 0);
        if (VALL_GT(lov, x))
        {
            o = outside & ~2u;
            inside &= ~1u;
        }
        else
        {
            RWP_FPU_MODE();
            LV_F(-0x160) = hi;
            LV_U(-0x15c) = 0u;
            LV_U(-0x158) = 0u;
            LV_U(-0x154) = 0u;
            RWP_VMX_MODE();
            if (VALL_GT(x, __vspltw(LV_V(-0x160), 0)))
            {
                o = outside & ~1u;
                inside &= ~2u;
            }
            else
                o = outside & ~3u;
        }
        // y
        RWP_FPU_MODE();
        LV_F(-0x1a0) = lo;
        LV_U(-0x19c) = 0u;
        LV_U(-0x198) = 0u;
        LV_U(-0x194) = 0u;
        RWP_VMX_MODE();
        const __vector4 y = __vspltw(t, 1);
        if (VALL_GT(__vspltw(LV_V(-0x1a0), 0), y))
        {
            outside = o & ~4u;
            inside &= ~8u;
        }
        else
        {
            RWP_FPU_MODE();
            LV_F(-0x190) = hi;
            LV_U(-0x18c) = 0u;
            LV_U(-0x188) = 0u;
            LV_U(-0x184) = 0u;
            RWP_VMX_MODE();
            if (VALL_GT(y, __vspltw(LV_V(-0x190), 0)))
            {
                outside = o & ~8u;
                inside &= ~4u;
            }
            else
                outside = o & ~0xcu;
        }
    }
    if (outside != 0)
        return 0;
    return inside == 0xf ? 1u : 2u;
}


#if !defined(RWP_GUEST)
unsigned int ShadowCasterSet_Erase(unsigned int result, unsigned int set, unsigned int it);
unsigned int EntityVector_DoInsertValue(unsigned int vec, unsigned int pos, unsigned int value);
void CameraPresentation_GetMatrix(unsigned int out, unsigned int cam, unsigned int b);
#endif
#define g_pPresentation_ RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define g_One           0x8231a844u     // 1.0f

bool RenderView_DetermineVisibilityFromGameState(unsigned int self, unsigned int ent);   // renderman.cpp

// The scene's shadow caster set (+0x4f70: an EASTL hash set of PresentationEntity*, buckets
// +0x4f74, count +0x4f78, end sentinel at buckets[count]) is sorted into per-slot caster vectors
// (0x10 bytes each: begin, end, capacity) with IsInLightView against the slot's light matrix.
// An entity fully inside one slot's view (result 1) is taken out of the set, so later slots
// skip it. Also stores the camera position (w = 1) at +0x4f10.
//   kSecond = 0: CSM cascades 0..2, vectors at +0x4f20; with onlyFlagged, entities whose
//                flags (+8) have none of 0x2f80 are skipped.
//   kSecond = 1: slots 0..1, vectors at +0x4f50; entities with any of 0x2f80 are skipped.
template <int kSecond>
static __forceinline void GatherShadowCasters(unsigned int self, unsigned int slot, unsigned int lightMatrix, unsigned int onlyFlagged)
{
    RWP_GUEST_FRAME(frame, kSecond ? 0x100 : 0x110);
    // (assert slot < 3 / 2)
    const unsigned int vec = self + 16u * (slot + (kSecond ? 0x4f5u : 0x4f2u));
    RWP_ST32(vec + 4u, RWP_LD32(vec));                                     // clear
    unsigned int bucket = RWP_LD32(self + 0x4f74u);
    unsigned int node = RWP_LD32(bucket);
    while (!node)
    {
        bucket += 4u;
        node = RWP_LD32(bucket);
    }
    const unsigned int end = RWP_LD32(RWP_LD32(self + 0x4f74u) + 4u * RWP_LD32(self + 0x4f78u));
    RWP_CALL(0x5fff0000, CameraPresentation_GetMatrix, FRAME_AT(frame, 0x70), (unsigned int)g_pPresentation_ + 0x29a90u, 1u);
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, FRAME_AT(frame, 0x60)) = RWP_GLOBAL(RWP_F32, g_One);
    RWP_ST32(FRAME_AT(frame, 0x64), 0u);
    RWP_ST32(FRAME_AT(frame, 0x68), 0u);
    RWP_ST32(FRAME_AT(frame, 0x6c), 0u);
    RWP_VMX_MODE();
    const __vector4 pos = RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0xa0));       // the matrix's translation row
    RWP_GLOBAL(RWP_VEC, self + 0x4f10u) = pos;
    RWP_GLOBAL(RWP_VEC, self + 0x4f10u) = __vrlimi(pos, __vspltw(RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0x60)), 0), 1, 1);
    while (node != end)
    {
        const unsigned int ent = RWP_LD32(node + 4u);
        RWP_ST32(FRAME_AT(frame, 0x50), ent);
        const unsigned int f = RWP_LD32(ent + 8u) & 0x2f80u;
        const bool skip = kSecond ? f != 0 : ((onlyFlagged & 0xffu) && f == 0);
        if (!skip && RenderView_DetermineVisibilityFromGameState(self, ent))
        {
            const unsigned int t = IsInLightView(lightMatrix, ent + 0xc0u);
            if (t != 0)
            {
                const unsigned int endp = RWP_LD32(vec + 4u);
                if (endp < RWP_LD32(vec + 8u))
                {
                    RWP_ST32(vec + 4u, endp + 4u);
                    if (endp)
                        RWP_ST32(endp, ent);
                }
                else
                    RWP_CALL(0x82ed5ca8, EntityVector_DoInsertValue, vec, endp, FRAME_AT(frame, 0x50));
            }
            if (t == 1)
            {
                RWP_ST32(FRAME_AT(frame, 0x58), node);
                RWP_ST32(FRAME_AT(frame, 0x5c), bucket);
                const unsigned int r = RWP_CALL(0x5fff0000, ShadowCasterSet_Erase, FRAME_AT(frame, 0x60), self + 0x4f70u,
                                                FRAME_AT(frame, 0x58));
                RWP_ST64(FRAME_AT(frame, 0x50), RWP_LD64(r));
                bucket = RWP_LD32(FRAME_AT(frame, 0x54));
                node = RWP_LD32(FRAME_AT(frame, 0x50));
                continue;
            }
        }
        node = RWP_LD32(node + 8u);
        while (!node)
        {
            bucket += 4u;
            node = RWP_LD32(bucket);
        }
    }
}

void ShadowView_GatherCasters(unsigned int self, unsigned int cascade, unsigned int lightMatrix, unsigned int onlyFlagged)
{
    GatherShadowCasters<0>(self, cascade, lightMatrix, onlyFlagged);
}
void ShadowView_GatherCasters2(unsigned int self, unsigned int slot, unsigned int lightMatrix)
{
    GatherShadowCasters<1>(self, slot, lightMatrix, 0u);
}


#if !defined(RWP_GUEST)
void SceneRenderView_GetLODDistancesFromAttrib(unsigned int self);
#endif
void VectorizedCullingJob(unsigned int, unsigned int params, unsigned int, unsigned int);

// The scene view's culling: the previous frame's update job (+0x5030) is flushed and waited for,
// every entity box (+0x4eb0, 0x20 each) is frustum/occlusion culled into the visibility words
// (+0x4ec0) by VectorizedCullingJob run right here, then each entity (+0x3efc) that is visible
// gets its LOD update queued on the update job (+0x5390: vtable +0x30, then +0x10) and joins the
// visible list (+0x3eec); a culled entity with flags 0x22 still gets its +0x10 call.
void SceneRenderView_Cull(unsigned int self)
{
    RWP_SAVEGPRLR(27);
    RWP_GUEST_FRAME(frame, 0x90);
    RWP_CALL(0x5fff0000, SceneRenderView_GetLODDistancesFromAttrib, self);
    const unsigned int job = self + 0x5030u;
    if (!RWP_LD8(self + 0x5380u) && RWP_LD32(job + 0x358u) != 0)
        Sk8::BufferedIterativeJob_Flush(job);
    if (RWP_LD8(job + 0x350u))
    {
        RWP_CALL(0x5fff0000, Job_WaitOn, job, 0u, 0u, -1);
        RWP_ST8(job + 0x350u, 0u);
    }
    const unsigned int n = (unsigned int)((int)(RWP_LD32(self + 0x4eb4u) - RWP_LD32(self + 0x4eb0u)) >> 5);
    if (!n)
        return;
    {
        const unsigned int begin = RWP_LD32(self + 0x4eb0u), end = RWP_LD32(self + 0x4eb4u);
        const unsigned int p = RWP_LD32(self + 0x18u) + 0x1a0u;
        RWP_ST32(p + 0x100u, begin);
        RWP_ST32(p + 0x108u, RWP_LD32(self + 0x4ec0u));
        RWP_ST32(p + 0x104u, 0u);
        RWP_ST32(p + 0x10cu, (unsigned int)((int)(end - begin) >> 5));
        RWP_ST32(p + 0x110u, p + 0x180u);
        VectorizedCullingJob(0u, p + 0x100u, 1u, 1u);
    }
    const unsigned int upd = self + 0x5390u;
    Sk8::BufferedIterativeJob_Wait(upd);
    for (unsigned int k = 0, off = 0; k < n; k++, off += 4u)
    {
        const unsigned int ent = RWP_LD32(RWP_LD32(self + 0x3efcu) + off);
        const int vis = (int)RWP_LD32(RWP_LD32(self + 0x4ec0u) + off);
        RWP_ST32(FRAME_AT(frame, 0x50), ent);
        if (vis != 0)
        {
            RWP_VCALL(ent, 0x30, RWP_LD32(self + 0x18u) + 0x1a0u, 8u, upd);
            RWP_VCALL(ent, 0x10);
            const unsigned int endp = RWP_LD32(self + 0x3ef0u);
            if (endp < RWP_LD32(self + 0x3ef4u))
            {
                RWP_ST32(self + 0x3ef0u, endp + 4u);
                if (endp)
                    RWP_ST32(endp, ent);
            }
            else
                RWP_CALL(0x82ed5ca8, EntityVector_DoInsertValue, self + 0x3eecu, endp, FRAME_AT(frame, 0x50));
        }
        else if ((RWP_LD32(ent + 8u) & 0x22u) == 0x22u)
            RWP_VCALL(ent, 0x10);
    }
    Sk8::BufferedIterativeJob_Flush(upd);
}

} }
