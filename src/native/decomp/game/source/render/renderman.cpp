// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (7); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 820aa9ec 820aaa08 820aaa14 8220a898 82603c38 8261e530 8274c1b8 8289fff8 82906838 8290acf0 8290b010 82912b08 82913a60 82914300 8291bf78 82921f08 8293c120 8293f3a0 829404e0 82976250 82976fe0 8297a730 8297ab90 8297b1a8 8297c0c8 82980c20 82985338 82996d00 829972e0 82997488 82998488 82999b90 8299ce98 829a1a80 829a2e18 829a3508 829abc88 829ac2e8 829b7c20 82a812c8 82aa0c18 82d77498 82d94c18 82d9e330 82f63b00 832d12a8 832d12ac 832d3364 832d46c4 83365a94 83367290 833689e5 83368e68 83368e6c 83369364 83369b08 8336a0a8 8336a328 8336a594 8336a600 8336a764 8336cfa4 8336d05c 8337e110 8337e190 833a5ea0
// game/source/render/renderman.cpp (sk83 assert path): Sk8::Render::RenderMan.
//   scene view render + viewport copy      sk83_na_m 0x829715c8 (0xdc)
//   Sk8::Render::RenderMan::Render(dt)     sk83_na_m 0x829aca40 (0x614): the frame's render body
//   Sk8::VectorizedCullingJob              sk83_na_m 0x829addd8 (0xc8)
//   RenderView::DetermineVisibilityFromGameState  sk83_na_m 0x8297a228 (0x18c; assert line 604)
//   per-frame shadow caster culling        sk83_na_m 0x829a9f38 (0x3c4; no name in the map)
// RenderMan owns two scene views (the normal one at +0x1130 and an alternative at +0x68b0, picked
// by the flag at +0x2c1c2); after rendering the chosen one its four viewport words (+4..+0x10) are
// copied into RenderMan (+0x2c1a8, +0x2c1b4, +0x2c1ac, +0x2c1b8 -- in that order).
// Verified with sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"
#include <string.h>

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
// callees that are not decompiled (yet): sk83 addresses in the calls
unsigned int RenderMan_PreRender(...), Presentation_RenderBegin(...), RenderMan_UpdateCamera(...),
    RenderShadows_CSM(...), D3DDevice_SetRenderState_HiZEnable(...), D3DDevice_SetShaderGPRAllocation(...),
    Presentation_RenderSky(...), StateShadow_Reset(...), D3DDevice_SetScissorRect(...), D3DDevice_SetViewportF(...),
    PostFX_Begin(...), RenderMan_RenderBackground(...), ICEManager_IsEditorOn(...), ICELITEManager_IsEditorOn(...),
    FrontEndManager_Instance(...), FrontEndManager_Render3D(...), Presentation_RenderParticles(...),
    Presentation_RenderDecals(...), Presentation_RenderCrowd(...), Presentation_RenderSkaters(...),
    Net_RunOcclusionQueries(...), RenderMan_DebugRender(...), Presentation_RenderTrails(...),
    RenderTarget_End(...), PostFX_Resolve(...), RenderMan_DEBUG_DrawTextures(...), RenderMan_DebugDraw2(...),
    RenderMan_PostFrame(...), Presentation_RenderHUD(...), RenderMan_Render2DBegin(...), Render2D_Flush(...),
    FrontEndManager_Render2D(...), DrawFPSRect(...), RenderMan_RenderStats(...), RenderMan_FinalQuadFade(...);
static double RwpLoadF64(unsigned int a) { return *(const double *)(size_t)a; }
#else
static double RwpLoadF64(u32 a) { return std::bit_cast<double>(__builtin_bswap64(*(const uint64_t *)(g_base + a))); }
#endif
void RenderMan_RenderSceneView(unsigned int self);

// a render option (attrib-backed settings under root + 0x20: a pointer override or a byte)
static __forceinline bool RenderOptionAt(unsigned int root)
{
    const unsigned int set = RWP_GLOBAL(RWP_U32, root + 0x20u);
    const unsigned int ovr = RWP_GLOBAL(RWP_U32, set + 0xd0u);
    return (ovr ? (unsigned char)RWP_GLOBAL(RWP_U8, ovr) : (unsigned char)RWP_GLOBAL(RWP_U8, set + 0xcdu)) != 0;
}

#define g_pPresentation   RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define g_pGraphicsMan    RWP_GLOBAL(RWP_U32, 0x5fff0000)     // XBox2GraphicsMan
#define g_pD3DDevice      RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define ICE_MANAGER       0x5fff0000u
#define ICELITE_MANAGER   0x5fff0000u

void RenderMan_RenderSceneView(unsigned int self)
{
    RWP_GUEST_FRAME(frame, 0x60);
    const unsigned int view = RWP_GLOBAL(RWP_U8, self + 0x2c1c2u) ? self + 0x68b0u : self + 0x1130u;
    RWP_VCALL(view, 0x10);                              // view->Render()
    RWP_GLOBAL(RWP_U32, self + 0x2c1a8u) = (unsigned int)RWP_GLOBAL(RWP_U32, view + 0x4u);
    RWP_GLOBAL(RWP_U32, self + 0x2c1b4u) = (unsigned int)RWP_GLOBAL(RWP_U32, view + 0x8u);
    RWP_GLOBAL(RWP_U32, self + 0x2c1acu) = (unsigned int)RWP_GLOBAL(RWP_U32, view + 0xcu);
    RWP_GLOBAL(RWP_U32, self + 0x2c1b8u) = (unsigned int)RWP_GLOBAL(RWP_U32, view + 0x10u);
}


// the time step as the UI wants it: (int)(dt * scale), low word of the fctidz result
static __forceinline unsigned int UiTicks(float dt)
{
    const float k = RWP_GLOBAL(RWP_F32, 0x5fff0000);
    return (unsigned int)(unsigned long long)RwpFctidz((double)(float)(dt * k));
}

// The frame's render body: camera/shadow prep, the CSM shadows, the scene view and everything the
// presentation draws into it, the post resolve, then the 2D: debug textures, front end, the FPS
// bar, stats, the final fade and the debug overlay.
void RenderMan_Render(unsigned int self, float dt)
{
    RWP_GUEST_FRAME(frame, 0xe0);
    RWP_FPU_MODE();
    const bool full = RWP_GLOBAL(RWP_U8, self + 0x2c1c0u) && RWP_GLOBAL(RWP_U8, self + 0x2c1c1u);
    RWP_CALL(0x5fff0000, RenderMan_PreRender, self, RWP_FLT(dt));
    RWP_CALL(0x5fff0000, Presentation_RenderBegin, (unsigned int)g_pPresentation + 0x27d90u);
    RWP_CALL(0x5fff0000, RenderMan_UpdateCamera, self, RWP_FLT((float)RWP_GLOBAL(RWP_F32, 0x5fff0000)), RWP_FLT(dt));
    if (RenderOptionAt(0x5fff0000u) && full)
    {
        RWP_VCALL(self + 0xc030u, 0x0c);
        RWP_VCALL(self + 0x11380u, 0x0c);
        RWP_CALL(0x5fff0000, RenderShadows_CSM, self + 0x1070u);
    }
    RWP_CALL(0x82b78350, D3DDevice_SetRenderState_HiZEnable, (unsigned int)g_pD3DDevice, 2u);
    if (full)
    {
        RWP_CALL(0x5fff0000, D3DDevice_SetShaderGPRAllocation, (unsigned int)g_pD3DDevice, 0u, 0u, 0u);
        RWP_CALL(0x5fff0000, Presentation_RenderSky, (unsigned int)g_pPresentation + 0x7c74u);
    }
    RWP_CALL0(0x5fff0000, StateShadow_Reset);
    {
        const unsigned int rm = g_pGraphicsMan;
        RWP_CALL(0x82b78ef8, D3DDevice_SetScissorRect, (unsigned int)g_pD3DDevice, rm + 0x42a18u);
        RWP_CALL(0x5fff0000, D3DDevice_SetViewportF, (unsigned int)g_pD3DDevice, rm + 0x429fcu);
    }
    const unsigned int post = self + 0x410u;
    RWP_CALL(0x5fff0000, PostFX_Begin, post, RenderOptionAt(0x5fff0000u) ? 0u : 1u);
    RWP_CALL(0x5fff0000, RenderMan_RenderBackground, self);
    if (full)
    {
        RWP_VCALL(self + 0x1130u, 0x0c);
        if (RWP_GLOBAL(RWP_U8, self + 0x2c1c2u))
            RWP_VCALL(self + 0x68b0u, 0x0c);
        RenderMan_RenderSceneView(self);
        if (!(unsigned char)RWP_CALL(0x5fff0000, ICEManager_IsEditorOn, ICE_MANAGER))
        {
            const unsigned int ticks = UiTicks(dt);
            const unsigned int fe = RWP_CALL0(0x5fff0000, FrontEndManager_Instance);
            RWP_CALL(0x82f9af80, FrontEndManager_Render3D, fe, ticks);
        }
        RWP_CALL(0x5fff0000, Presentation_RenderParticles, (unsigned int)g_pPresentation + 0x7c74u);
        RWP_CALL(0x5fff0000, Presentation_RenderDecals, (unsigned int)g_pPresentation + 0x2ebb8u);
        if (!RWP_GLOBAL(RWP_U8, self + 0x2c1c2u))
            RWP_CALL(0x5fff0000, Presentation_RenderCrowd, (unsigned int)g_pPresentation + 0x44b0u);
        // (asserts: both singletons exist, line 130)
        RWP_CALL(0x5fff0000, Presentation_RenderSkaters, (unsigned int)RWP_GLOBAL(RWP_U32, 0x5fff0000));
        RWP_CALL(0x5fff0000, Net_RunOcclusionQueries, (unsigned int)RWP_GLOBAL(RWP_U32, 0x5fff0000));
    }
    if (RenderOptionAt(0x5fff0000u))
        RWP_CALL(0x5fff0000, RenderMan_DebugRender, self, RWP_FLT(dt), 1u);
    if (full)
        RWP_CALL(0x5fff0000, Presentation_RenderTrails, (unsigned int)g_pPresentation + 0x78d4u);
    RWP_CALL(0x5fff0000, RenderTarget_End, (unsigned int)RWP_GLOBAL(RWP_U32, post + 0xe8u), 1u, 3u, 3u, 0u);
    {
        const unsigned int rt = RWP_GLOBAL(RWP_U32, post + 0xe8u);
        const unsigned int v = RWP_GLOBAL(RWP_U32, rt + 0x1ecu);
        RWP_GLOBAL(RWP_U32, rt + 0x1cu) = 1u;
        RWP_GLOBAL(RWP_U32, rt + 0x20u) = rt + 0x24u;
        RWP_GLOBAL(RWP_U32, rt + 0x1d4u) = v;
    }
    RWP_CALL(0x82b78350, D3DDevice_SetRenderState_HiZEnable, (unsigned int)g_pD3DDevice, 0u);
    RWP_CALL(0x827f53b0, PostFX_Resolve, post);
    {
        const unsigned int rm = g_pGraphicsMan;
        RWP_CALL(0x82b78ef8, D3DDevice_SetScissorRect, (unsigned int)g_pD3DDevice, rm + 0x42a18u);
        RWP_CALL(0x5fff0000, D3DDevice_SetViewportF, (unsigned int)g_pD3DDevice, rm + 0x429fcu);
    }
    RWP_CALL(0x5fff0000, RenderMan_DEBUG_DrawTextures, self);
    if (RenderOptionAt(0x5fff0000u))
        RWP_CALL(0x5fff0000, RenderMan_DebugDraw2, self + 0x2b120u);
    RWP_CALL0(0x82758b38, RenderMan_PostFrame);
    if (RWP_GLOBAL(RWP_U8, 0x5fff0000u))
        return;
    if (g_pPresentation)
        RWP_CALL(0x5fff0000, Presentation_RenderHUD, (unsigned int)g_pPresentation + 0x20d84u);
    RWP_CALL0(0x5fff0000, RenderMan_Render2DBegin);
    RWP_CALL(0x5fff0000, Render2D_Flush, (unsigned int)RWP_GLOBAL(RWP_U32, 0x5fff0000));   // (assert: exists)
    if (!(unsigned char)RWP_CALL(0x5fff0000, ICEManager_IsEditorOn, ICE_MANAGER))
    {
        const unsigned int ticks = UiTicks(dt);
        const unsigned int fe = RWP_CALL0(0x5fff0000, FrontEndManager_Instance);
        RWP_CALL(0x5fff0000, FrontEndManager_Render2D, fe, ticks);
    }
    if (RenderOptionAt(0x5fff0000u))
    {
        // the frame-time bar: two counters of the render manager, scaled, coloured by a threshold
        const unsigned int rm = g_pGraphicsMan;
        const unsigned int a = RWP_GLOBAL(RWP_U32, rm + 0x107fcu), b = RWP_GLOBAL(RWP_U32, rm + 0x107f8u);
        const unsigned char interlaced = RWP_GLOBAL(RWP_U8, rm + 0x10850u);
        const float k0 = RWP_GLOBAL(RWP_F32, 0x5fff0000), k1 = RWP_GLOBAL(RWP_F32, 0x5fff0000);
        const float fb = (float)(double)(long long)(unsigned long long)b;
        const float fa = (float)(double)(long long)(unsigned long long)a;
        double x;
        if (interlaced)
            x = (float)((float)(fb - k0) * k1);
        else
            x = (double)(float)((float)(fb - k0) * k1) * RwpLoadF64(0x5fff0000u);
        const float x1 = (float)x;
        const unsigned int idx = RenderOptionAt(0x5fff0000u) ? 0x2c19cu : 0x2c198u;
        const float y = (float)(fa - k0);
        const int level = (int)RWP_GLOBAL(RWP_S32, self + idx);
        const float h = RWP_GLOBAL(RWP_F32, 0x5fff0000), k2 = RWP_GLOBAL(RWP_F32, 0x5fff0000);
        const float y2 = (float)(y * k2);
        const unsigned int colour = level > 0x37 ? 0x5fff0000u : 0x5fff0000u;
#if defined(RWP_GUEST)
        memcpy(g_base + frame.at(0x50), g_base + colour, 16);
#else
        memcpy((void *)FRAME_AT(frame, 0x50), (const void *)(size_t)colour, 16);
#endif
        RWP_CALL(0x5fff0000, DrawFPSRect, RWP_FLT(x1), RWP_FLT(y2), RWP_FLT(h), RWP_FLT(h), FRAME_AT(frame, 0x50));
    }
    if (RenderOptionAt(0x5fff0000u))
        RWP_CALL(0x5fff0000, RenderMan_RenderStats, self);
    RWP_CALL(0x5fff0000, RenderMan_FinalQuadFade, self);
    if (!RenderOptionAt(0x5fff0000u) && !(unsigned char)RWP_CALL(0x5fff0000, ICELITEManager_IsEditorOn, ICELITE_MANAGER))
        RWP_CALL(0x5fff0000, RenderMan_DebugRender, self, RWP_FLT(dt), 1u);
}


#if !defined(RWP_GUEST)
void DoFrustumCulling(unsigned int params, unsigned int cam, unsigned int occluders, unsigned int nOccluders, unsigned int counter);
#endif

// Sk8::VectorizedCullingJob(unsigned, unsigned, unsigned, unsigned), sk83_na_m 0x829addd8: the
// culling work item (params: +0x10 the camera's culling data). The occluders' five planes each
// (normal + abs normal, two vectors at camera +0x20 * k) are expanded into splatted x/y/z
// vectors on the stack (0x60 per plane, 0x1e0 per occluder), the culled-by-occlusion counter
// zeroed, then DoFrustumCulling runs (natively replaced in sk83: skate3_frustum_cull.cpp).
void VectorizedCullingJob(unsigned int, unsigned int params, unsigned int, unsigned int)
{
    RWP_SAVEGPRLR(29);
    RWP_GUEST_FRAME(frame, 0x3c80);
    const unsigned int cam = RWP_LD32(params + 0x10u);
    const unsigned int n = RWP_LD32(cam + 0x148cu);
    if (n)
    {
        unsigned int src = cam, occ = FRAME_AT(frame, 0x80);
        for (unsigned int k = 0; k < n; k++, occ += 0x1e0u)
        {
            unsigned int d = occ;
            for (int plane = 0; plane < 5; plane++, d += 0x60u, src += 0x20u)
            {
                const __vector4 a = RWP_GLOBAL(RWP_VEC, src), b = RWP_GLOBAL(RWP_VEC, src + 0x10u);
                RWP_GLOBAL(RWP_VEC, d - 0x20u) = __vspltw(a, 0);
                RWP_GLOBAL(RWP_VEC, d - 0x10u) = __vspltw(a, 1);
                RWP_GLOBAL(RWP_VEC, d) = __vspltw(a, 2);
                RWP_GLOBAL(RWP_VEC, d + 0x10u) = __vspltw(b, 0);
                RWP_GLOBAL(RWP_VEC, d + 0x20u) = __vspltw(b, 1);
                RWP_GLOBAL(RWP_VEC, d + 0x30u) = __vspltw(b, 2);
            }
        }
    }
    RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0x50)) = __vspltisw(0);
    RWP_CALL(0x82816a40, DoFrustumCulling, params, cam, FRAME_AT(frame, 0x60), n, FRAME_AT(frame, 0x50));
}


// ---- shadow casters and entity visibility (moved here by their asserts: RenderMan::Get, which is
// defined in this file, is inlined into them -- assert "renderman.cpp" line 604) ----
void ShadowView_GatherCasters(unsigned int self, unsigned int cascade, unsigned int lightMatrix, unsigned int onlyFlagged);
void ShadowView_GatherCasters2(unsigned int self, unsigned int slot, unsigned int lightMatrix);

#if !defined(RWP_GUEST)
bool SceneManager_IsInPhotoChallengeNIS(unsigned int sm);
unsigned long long AttribInstance_GetCollection(unsigned int inst);
void SkaterPresentation_GetPlayerRecipeID(unsigned int out, unsigned int sp);
unsigned int SceneManagerSingleton_Get(unsigned int);
#endif
#define g_pRenderMan      RWP_GLOBAL(RWP_U32, 0x5fff0000)     // RenderMan::Get (+0x354: the entity flags shown)
#define g_pSceneManager RWP_GLOBAL(RWP_U32, 0x5fff0000)

// Is the entity shown at all? Its flags (+8) must all be in the render manager's visible set --
// except during a photo challenge NIS: there entities with flags 0x222 show unless their attrib
// collection (+0x280) is one of two hidden ones, and skaters (0x122) other than the player's own
// recipe show only while the scene manager's flag (+4 -> +0x35c) is clear. Then the entity's own
// IsVisible (vtable +0x44).
bool RenderView_DetermineVisibilityFromGameState(unsigned int self, unsigned int ent)
{
    (void)self;
    RWP_GUEST_FRAME(frame, 0x80);
    const unsigned int flags = RWP_LD32(ent + 8u);
    // (assert g_pGraphicsMan, line 604)
    const unsigned int shown = RWP_LD32((unsigned int)g_pRenderMan + 0x354u);
    unsigned int vis = (flags - (shown & flags)) == 0;
    // (assert g_pSceneManager, line 130 of the scene manager header)
    if (RWP_CALL(0x5fff0000, SceneManager_IsInPhotoChallengeNIS, (unsigned int)g_pSceneManager) & 0xffu)
    {
        vis = 1;
        if ((flags & 0x222u) == 0x222u)
        {
#if defined(RWP_GUEST)
            rwp::Call(0x5fff0000, ent + 0x280u);
            const unsigned long long coll = RWP_REG64(r3);
#else
            const unsigned long long coll = AttribInstance_GetCollection(ent + 0x280u);
#endif
            vis = !(coll == 0x88d72df443d56751ull || coll == 0x4e6e5331d54e4f61ull);
        }
        else if ((flags & 0x122u) == 0x122u)
        {
            RWP_CALL(0x5fff0000, SkaterPresentation_GetPlayerRecipeID, FRAME_AT(frame, 0x50), (unsigned int)g_pPresentation + 0x27d90u);
            if (RWP_LD64(RWP_LD32(ent + 0xcu) + 0x38u) != RWP_LD64(FRAME_AT(frame, 0x50)))
            {
                const unsigned int sm = RWP_CALL(0x5fff0000, SceneManagerSingleton_Get, 0u);
                if (RWP_LD8(RWP_LD32(sm + 4u) + 0x35cu))
                    vis = 0;
            }
        }
    }
    if (!(vis & 0xffu))
        return false;
    return (RWP_VCALL(ent, 0x44) & 0xffu) != 0;
}

#if !defined(RWP_GUEST)
void EntitySet_DoFreeNodes(unsigned int set, unsigned int buckets, unsigned int count);
void EntitySet_InsertRange(unsigned int set, unsigned int first, unsigned int last);
void ShadowView_BuildLightMatrix(unsigned int out, unsigned int cascade, unsigned int bounds, float f, unsigned int a,
                                 unsigned int b, unsigned int c, unsigned int d);
void ShadowView_FinishCascade(unsigned int prev, unsigned int cascade, unsigned int next, float zmin, float zmax);
void ShadowView_SetDepthRange(unsigned int self, unsigned int second);
unsigned int SkaterPresentation_GetLocalIndex(unsigned int sp);
unsigned int SkaterPresentation_GetSkaterPresEntityFromIndex(unsigned int sp, unsigned int i);
#endif
#define g_Half          0x8209975cu     // 0.5f

static __forceinline void CopyMatrix(unsigned int dst, unsigned int src)
{
    for (unsigned int k = 0; k < 0x40u; k += 16u)
        RWP_GLOBAL(RWP_VEC, dst + k) = (__vector4)RWP_GLOBAL(RWP_VEC, src + k);
}

// Each frame: the caster set (+0x4f70) is refilled from the scene's shadow entities (+0x20, a
// hash set), every CSM cascade (+0x5050, 0x40 each) gets its light matrix (0x8297a730, from the
// render manager's shadow settings at +0x2c020) and its casters (0x829a7c48); in shadow mode 1
// (settings +0) the set is refilled again for the two cascades of the second shadow set
// (+0x5250). The light depth range (+0x4ed0 / +0x4ed4) follows the player skater's bounds when
// the settings ask for it (+0xd8), else 0x8297b1a8 sets it; then 0x8297ab90 finishes each cascade.
void ShadowView_CullShadowCasters(unsigned int self)
{
#if defined(RWP_GUEST)
    RWP_SAVEGPRLR(14);
    GStore64(g_ctx->r1.u32 - 0xa0u, g_ctx->f31.u64);                     // stfd f31, -0xa0(r1)
#endif
    RWP_GUEST_FRAME(frame, 0x1f0);
    // the scene's shadow entities: first node and end
    unsigned int bucket = RWP_LD32(self + 0x20u);
    unsigned int first = RWP_LD32(bucket);
    RWP_ST32(FRAME_AT(frame, 0x60), first);
    if (!first)
    {
        bucket += 4u;
        unsigned int n = RWP_LD32(bucket);
        while (!n)
        {
            bucket += 4u;
            n = RWP_LD32(bucket);
        }
        first = n;
        RWP_ST32(FRAME_AT(frame, 0x60), n);
    }
    const unsigned int endBucket = RWP_LD32(self + 0x20u) + 4u * RWP_LD32(self + 0x24u);
    const unsigned int endNode = RWP_LD32(endBucket);
    RWP_ST32(FRAME_AT(frame, 0x70), endNode);
    const unsigned int set = self + 0x4f70u;
    RWP_CALL(0x5fff0000, EntitySet_DoFreeNodes, set, RWP_LD32(self + 0x4f74u), RWP_LD32(self + 0x4f78u));
    RWP_ST32(FRAME_AT(frame, 0x80), endNode);
    RWP_ST32(FRAME_AT(frame, 0x84), endBucket);
    RWP_ST32(self + 0x4f7cu, 0u);
    RWP_ST32(FRAME_AT(frame, 0x68), first);
    RWP_ST32(FRAME_AT(frame, 0x6c), bucket);
    RWP_CALL(0x5fff0000, EntitySet_InsertRange, set, FRAME_AT(frame, 0x68), FRAME_AT(frame, 0x80));
    // (assert g_pGraphicsMan, line 604)
    const unsigned int cfg = (unsigned int)g_pRenderMan + 0x2c020u;
    const unsigned int second = RWP_LD32(cfg) == 1u;
    for (unsigned int i = 0; i < 3; i++)
    {
        RWP_FPU_MODE();
        const float f = RWP_GLOBAL(RWP_F32, cfg + 0x50u + 4u * i);
        RWP_ST32(FRAME_AT(frame, 0x54), cfg + 0xe0u);                        // stack arguments 9 and 10
        RWP_ST8(FRAME_AT(frame, 0x5f), 0u);
        const unsigned int c = self + 0x5050u + 0x40u * i;
        RWP_CALL(0x5fff0000, ShadowView_BuildLightMatrix, FRAME_AT(frame, 0xd0), c, c + 0xc0u, RWP_FLT(f), cfg + 0x40u,
                 cfg + 0x30u, cfg + 0x20u, cfg + 0x10u);
        CopyMatrix(FRAME_AT(frame, 0x90), FRAME_AT(frame, 0xd0));
        ShadowView_GatherCasters(self, i, FRAME_AT(frame, 0x90), second);
    }
    if (second)
    {
        RWP_CALL(0x5fff0000, EntitySet_DoFreeNodes, set, RWP_LD32(set + 4u), RWP_LD32(set + 8u));
        const unsigned int f0 = RWP_LD32(FRAME_AT(frame, 0x60)), e0 = RWP_LD32(FRAME_AT(frame, 0x70));
        RWP_ST32(set + 0xcu, 0u);
        RWP_ST32(FRAME_AT(frame, 0x64), endBucket);
        RWP_ST32(FRAME_AT(frame, 0x6c), bucket);
        RWP_ST32(FRAME_AT(frame, 0x68), f0);
        RWP_ST32(FRAME_AT(frame, 0x60), e0);
        RWP_CALL(0x5fff0000, EntitySet_InsertRange, set, FRAME_AT(frame, 0x68), FRAME_AT(frame, 0x60));
        for (unsigned int i = 0; i < 2; i++)
        {
            RWP_FPU_MODE();
            const float f = RWP_GLOBAL(RWP_F32, cfg + 0x5cu + 4u * i);
            RWP_ST32(FRAME_AT(frame, 0x54), cfg + 0xe0u);
            RWP_ST8(FRAME_AT(frame, 0x5f), 0u);
            const unsigned int c = self + 0x5250u + 0x40u * i;
            RWP_CALL(0x5fff0000, ShadowView_BuildLightMatrix, FRAME_AT(frame, 0x110), c, c + 0x80u, RWP_FLT(f), cfg + 0x40u,
                     cfg + 0x30u, cfg + 0x20u, cfg + 0x10u);
            CopyMatrix(FRAME_AT(frame, 0x90), FRAME_AT(frame, 0x110));
            ShadowView_GatherCasters2(self, i, FRAME_AT(frame, 0x90));
        }
    }
    // the light depth range
    const unsigned int sp = (unsigned int)g_pPresentation + 0x27d90u;
    const unsigned int idx = RWP_CALL(0x5fff0000, SkaterPresentation_GetLocalIndex, sp);
    const unsigned int skater = RWP_CALL(0x5fff0000, SkaterPresentation_GetSkaterPresEntityFromIndex, sp, idx);
    if (skater && RWP_LD8(cfg + 0xd8u))
    {
        RWP_FPU_MODE();
        const float extent = RWP_GLOBAL(RWP_F32, cfg + 0xd4u);
        const float half = RWP_GLOBAL(RWP_F32, g_Half);
        RWP_GLOBAL(RWP_F32, FRAME_AT(frame, 0x70)) = half;
        RWP_ST32(FRAME_AT(frame, 0x74), 0u);
        RWP_ST32(FRAME_AT(frame, 0x78), 0u);
        RWP_ST32(FRAME_AT(frame, 0x7c), 0u);
        RWP_VMX_MODE();
        const __vector4 lo = RWP_GLOBAL(RWP_VEC, skater + 0xc0u), hi = RWP_GLOBAL(RWP_VEC, skater + 0xd0u);
        const __vector4 sum = __vaddfp(__vspltw(hi, 1), __vspltw(lo, 1));
        RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0x70)) = __vmulfp(__vspltw(RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0x70)), 0), sum);
        RWP_FPU_MODE();
        const float mid = RWP_GLOBAL(RWP_F32, FRAME_AT(frame, 0x70));     // the skater's centre height
        RWP_GLOBAL(RWP_F32, self + 0x4ed4u) = RWP_FMADDS(extent, half, mid);
        RWP_GLOBAL(RWP_F32, self + 0x4ed0u) = RWP_FNMSUBS((float)RWP_GLOBAL(RWP_F32, cfg + 0xd4u), half, mid);
    }
    else
        RWP_CALL(0x5fff0000, ShadowView_SetDepthRange, self, 0u);
    for (unsigned int i = 0; i < 3; i++)
    {
        RWP_FPU_MODE();
        const unsigned int c = self + 0x5050u + 0x40u * i;
        const float zmax = RWP_GLOBAL(RWP_F32, self + 0x4ed4u), zmin = RWP_GLOBAL(RWP_F32, self + 0x4ed0u);
        RWP_CALL(0x5fff0000, ShadowView_FinishCascade, c - 0xc0u, c, c + 0xc0u, RWP_FLT(zmin), RWP_FLT(zmax));
    }
    if (second)
    {
        RWP_CALL(0x5fff0000, ShadowView_SetDepthRange, self, 1u);
        RWP_FPU_MODE();
        const float a = RWP_GLOBAL(RWP_F32, cfg + 0xb4u), b = RWP_GLOBAL(RWP_F32, self + 0x4ef4u);
        const float zmax = (double)(float)(a - b) >= 0.0 ? a : b;          // fsubs + fsel
        for (unsigned int i = 0; i < 2; i++)
        {
            const unsigned int c = self + 0x5250u + 0x40u * i;
            RWP_CALL(0x5fff0000, ShadowView_FinishCascade, c - 0x80u, c, c + 0x80u,
                     RWP_FLT((float)RWP_GLOBAL(RWP_F32, cfg + 0xb0u)), RWP_FLT(zmax));
        }
    }
}


} }
