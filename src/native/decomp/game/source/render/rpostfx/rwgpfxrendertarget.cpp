// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (3); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 822a40d0 8296eea8 82c73c80 82d94c18 82d9b100 82d9b450 82d9b7c0 82f63b00 833a5ea0 833a5eac
// game/source/render/rpostfx/rwgpfxrendertarget.cpp: renderengine::postfx render targets (file
// from the Skate 2 PDB, code/ng/render/rpostfx/rwgpfxrendertarget.cpp; no asserts in these).
//   renderengine::postfx::Target::Resolve()                          sk83_na_m 0x8296ebe0 (0x6c)
//   renderengine::postfx::RenderTarget::Begin(bool clear)            sk83_na_m 0x8296ed48 (0x160)
//   renderengine::postfx::RenderTarget::End(bool, bool, resolve_RECT *)  sk83_na_m 0x82976250 (0x94)
// RenderTarget (Skate 2 layout, same offsets in sk83): +4 m_width, +8 m_height, +0x10 m_colorMode,
// +0x1c m_MSAAMode, +0x20 mp_CurrRect (+0xc tile count, +0x10 tile rects), +0x1d4 m_CurrRTState.
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace renderengine { namespace postfx {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
void StateShadow_SetRenderTargetState(unsigned int state);
void StateShadow_Reset();
void D3DDevice_BeginTiling(unsigned int dev, unsigned int flags, unsigned int count, unsigned int rects,
                           unsigned int clearColor, float clearZ, unsigned int clearStencil);
void D3DDevice_EndTiling(unsigned int dev, unsigned int flags, unsigned int rects, unsigned int dest,
                         unsigned int clearColor, float clearZ, unsigned int clearStencil, unsigned int params);
void D3DDevice_Clear(unsigned int dev, unsigned int count, unsigned int rects, unsigned int flags,
                     unsigned int color, float z, unsigned int stencil, unsigned int edram);
void D3DDevice_SetViewportF(unsigned int dev, unsigned int vp);
void D3DDevice_SetScissorRect(unsigned int dev, unsigned int rect);
void RenderTarget_Resolve(unsigned int self, unsigned int resolveDepth, unsigned int rect, unsigned int extra);
unsigned int PixelBuffer_Xbox2ResolveTo(unsigned int pb, unsigned int tex, unsigned int a, unsigned int b,
                                        unsigned int c, unsigned int d, unsigned int e, unsigned int f, float g);
#endif

#define g_PfxDevice            0x5fff0000u   // renderengine::Device::m_device
#define g_PfxBackBufferRTState 0x5fff0000u   // the device target's RenderTargetState (colour mode 2)
#define kPfxZero               0x5fff0000u   // 0.0f
#define kPfxOne                0x8231a844u   // 1.0f
#define kPfxClearColor         0x5fff0000u   // (0, 0, 0, 0)

// Resolve the target's surface (+4, a PixelBuffer) into its texture (+0x64), when it has one,
// then reset the state shadow.
void Target_Resolve(unsigned int self)
{
    RWP_SAVELR();
    RWP_GUEST_FRAME(frame, 0x90);
    const unsigned int tex = RWP_LD32(self + 0x64u);
    if (!tex)
        return;
    RWP_FPU_MODE();
    const float one = RWP_GLOBAL(RWP_F32, kPfxOne);
    for (unsigned int off = 0x5cu; off <= 0x7cu; off += 8u)     // the stack arguments (0)
        RWP_ST32(FRAME_AT(frame, off), 0u);
    RWP_CALL(0x5fff0000, PixelBuffer_Xbox2ResolveTo, RWP_LD32(self + 4u), tex, 0u, 0u, 0u, 0u, 0u, 0u, RWP_FLT(one));
    RWP_CALL0(0x5fff0000, StateShadow_Reset);
}

// Bind the target: its render target state (the device target's for colour mode 2), then with
// MSAA tiling (+0x1c > 1) BeginTiling over the current rects with a zero clear colour and z 1,
// else an optional clear (target, z 1); then the full-target viewport (0..1 depth) and scissor.
void RenderTarget_Begin(unsigned int self, unsigned int clear)
{
    RWP_SAVEGPRLR(28);
    RWP_SAVEF31(0x30);
    RWP_GUEST_FRAME(frame, 0xd0);
    RWP_NV(31, self);
    RWP_NV(28, clear);
    const unsigned int rts = (int)RWP_LD32(self + 0x10u) == 2 ? RWP_LD32(g_PfxBackBufferRTState) : RWP_LD32(self + 0x1d4u);
    RWP_CALL(0x82cfc990, StateShadow_SetRenderTargetState, rts);
    RWP_FPU_MODE();
    const float zero = RWP_GLOBAL(RWP_F32, kPfxZero), one = RWP_GLOBAL(RWP_F32, kPfxOne);
    RWP_NV(29, 0u);
    RWP_NV(30, 0xffffffff833a0000ull);
    RWP_NVF31(zero);
    const unsigned int sc = FRAME_AT(frame, 0x60), vp = FRAME_AT(frame, 0x80);
    RWP_ST32(FRAME_AT(frame, 0x98), 0u);
    RWP_ST32(sc, 0u);
    RWP_ST32(sc + 4u, 0u);
    RWP_ST32(sc + 8u, 0u);
    RWP_ST32(sc + 0xcu, 0u);
    for (unsigned int k = 0; k < 0x14u; k += 4u)
        RWP_GLOBAL(RWP_F32, vp + k) = zero;
    RWP_GLOBAL(RWP_F32, vp + 0x14u) = one;
    if ((int)RWP_LD32(self + 0x1cu) > 1)
    {
        const unsigned int rects = RWP_LD32(self + 0x20u), color = FRAME_AT(frame, 0x70);
        RWP_ST64(color, RWP_LD64(kPfxClearColor));
        RWP_ST64(color + 8u, RWP_LD64(kPfxClearColor + 8u));
        RWP_CALL(0x5fff0000, D3DDevice_BeginTiling, RWP_LD32(g_PfxDevice), 0u, RWP_LD32(rects + 0xcu), rects + 0x10u,
                 color, RWP_FLT(one), 0u);
    }
    else if (clear & 0xffu)
        RWP_CALL(0x5fff0000, D3DDevice_Clear, RWP_LD32(g_PfxDevice), 0u, 0u, 0x10u, 0u, RWP_FLT(one), 0u, 0u);
    const unsigned int h = RWP_LD32(self + 8u), w = RWP_LD32(self + 4u);
    RWP_GLOBAL(RWP_F32, vp) = zero;
    RWP_GLOBAL(RWP_F32, vp + 4u) = zero;
    RWP_ST32(sc, 0u);
    RWP_ST32(sc + 4u, 0u);
#if defined(RWP_GUEST)
    GStore64(FRAME_AT(frame, 0x50), (uint64_t)w);                         // std (fcfid staging), last: the width
#endif
    RWP_ST32(sc + 8u, w);
    RWP_ST32(sc + 0xcu, h);
    RWP_GLOBAL(RWP_F32, vp + 8u) = (float)(double)w;
    RWP_GLOBAL(RWP_F32, vp + 0xcu) = (float)(double)h;
    RWP_CALL(0x5fff0000, D3DDevice_SetViewportF, RWP_LD32(g_PfxDevice), vp);
    RWP_CALL(0x82b78ef8, D3DDevice_SetScissorRect, RWP_LD32(g_PfxDevice), sc);
}

// Unbind: optional resolve (RenderTarget::Resolve with the depth flag, rect and the extra
// argument), and with MSAA tiling EndTiling (zero clear colour, z 1).
void RenderTarget_End(unsigned int self, unsigned int resolve, unsigned int resolveDepth, unsigned int rect,
                      unsigned int extra)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_NV(31, self);
    if (resolve & 0xffu)
        RWP_CALL(0x5fff0000, RenderTarget_Resolve, self, resolveDepth, rect, extra);
    if ((int)RWP_LD32(self + 0x1cu) > 1)
    {
        const unsigned int color = FRAME_AT(frame, 0x50);
        RWP_ST64(color, RWP_LD64(kPfxClearColor));
        RWP_ST64(color + 8u, RWP_LD64(kPfxClearColor + 8u));
        RWP_FPU_MODE();
        const float one = RWP_GLOBAL(RWP_F32, kPfxOne);
        RWP_CALL(0x5fff0000, D3DDevice_EndTiling, RWP_LD32(g_PfxDevice), 0u, 0u, 0u, color, RWP_FLT(one), 0u, 0u);
    }
}

} }
