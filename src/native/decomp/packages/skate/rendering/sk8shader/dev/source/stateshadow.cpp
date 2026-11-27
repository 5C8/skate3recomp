// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (11); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82c72f60 832fa424 833a5ea0 833a5f60 833c31b4 833c31b8 833c3cf0 833c3cf4 833c3cf8 833c3d10 833c3d50
// packages/skate/rendering/sk8shader/dev/source/stateshadow.cpp (sk83 assert path): StateShadow,
// the cache of the render states the shader system last set, so redundant device calls are skipped.
//   StateShadow::Reset                              sk83_na_m 0x82f63b00
//   StateShadow::SetState(RasterizerState*)         0x82f63b88
//   StateShadow::SetState(DepthStencilState*)       0x82f63bd0
//   StateShadow::SetState(BlendState*)              0x82f63c18
//   StateShadow::SetState(RenderTargetState*)       0x82f63c60
//   StateShadow::SetTexture(Texture*, stage)        0x82f63ca8
//   StateShadow::SetState(SamplerState*, stage)     0x82f63d70
//   StateShadow::SetVertexProgramState(state)       0x82f63dd8
//   StateShadow::SetPixelProgram(program, states)   0x82f63ee0 (map: "cStateGraph::RegisterState")
// (map names around here are off by one; named by the device call each makes.) Verified with
// sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace StateShadow {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

// the shadowed states (0x833c3cf0..)
#define s_VertexProgramState   RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define s_State3cf4            RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define s_State3cf8            RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define s_RasterizerState      RWP_GLOBAL(RWP_U32, 0x83085370)
#define s_DepthStencilState    RWP_GLOBAL(RWP_U32, 0x83085370)
#define s_BlendState           RWP_GLOBAL(RWP_U32, 0x83085370)
#define s_RenderTargetState    RWP_GLOBAL(RWP_U32, 0x83085370)
#define s_Textures(i)          RWP_GLOBAL(RWP_U32, 0x5fff0000 + 4 * (i))
#define s_Samplers(i)          RWP_GLOBAL(RWP_U32, 0x5fff0000 + 4 * (i))
#define g_pD3DDevice           RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define g_CurrentVPState       RWP_GLOBAL(RWP_U32, 0x5fff0000)    // renderengine's current vertex program state
#define g_ppDefaultTexture     RWP_GLOBAL(RWP_U32, 0x5fff0000)    // -> the texture bound for "no texture"

#if !defined(RWP_GUEST)
namespace Device {
void SetRasterizerState(unsigned int s);
void SetDepthStencilState(unsigned int s);
void SetBlendState(unsigned int s);
void SetRenderTargetState(unsigned int s);
void SetSamplerState(unsigned int s, unsigned int stage);
}
extern "C" void D3DDevice_SetTexture(unsigned int dev, unsigned int stage, unsigned int tex, unsigned int fetchMask);
extern "C" void D3DDevice_SetVertexShader(unsigned int dev, unsigned int shader);
extern "C" void D3DDevice_SetPixelShader(unsigned int dev, unsigned int shader);
typedef void (*RwpFn0)();
#endif

void Reset()
{
    s_RasterizerState = 0u;
    s_State3cf4 = 0u;
    s_VertexProgramState = 0u;
    s_DepthStencilState = 0u;
    s_State3cf8 = 0u;
    s_RenderTargetState = 0u;
    s_BlendState = 0u;
    for (int i = 0; i < 16; i++)
        s_Samplers(i) = 0u;
    for (int i = 0; i < 16; i++)
        s_Textures(i) = 0u;
}

void SetRasterizerState(unsigned int s)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (s != s_RasterizerState)
    {
        RWP_CALL(0x82a7b660, Device::SetRasterizerState, s);
        s_RasterizerState = s;
    }
}

void SetDepthStencilState(unsigned int s)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (s != s_DepthStencilState)
    {
        RWP_CALL(0x82a7b660, Device::SetDepthStencilState, s);
        s_DepthStencilState = s;
    }
}

void SetBlendState(unsigned int s)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (s != s_BlendState)
    {
        RWP_CALL(0x82a7b660, Device::SetBlendState, s);
        s_BlendState = s;
    }
}

void SetRenderTargetState(unsigned int s)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (s != s_RenderTargetState)
    {
        RWP_CALL(0x82a7b660, Device::SetRenderTargetState, s);
        s_RenderTargetState = s;
    }
}

// a texture on a fetch stage (null: the default texture); the fetch-constant mask is bit 31 - stage
void SetTexture(unsigned int tex, unsigned int stage)
{
    RWP_GUEST_FRAME(frame, 0x80);
    // (assert stage < 16, line 210)
    if (s_Textures(stage) == tex)
        return;
    const unsigned int mask = (unsigned int)(0x8000000000000000ull >> (stage + 32));
    if (tex)
        RWP_CALL(0x82b82478, D3DDevice_SetTexture, g_pD3DDevice, stage, tex, mask);
    else
        RWP_CALL(0x82b82478, D3DDevice_SetTexture, g_pD3DDevice, stage,
                 RWP_GLOBAL(RWP_U32, (unsigned int)g_ppDefaultTexture), mask);
    s_Textures(stage) = tex;
}

void SetSamplerState(unsigned int s, unsigned int stage)
{
    RWP_GUEST_FRAME(frame, 0x80);
    // (assert stage < 16, line 228)
    if (s_Samplers(stage) != s)
    {
        RWP_CALL(0x5fff0000, Device::SetSamplerState, s, stage);
        s_Samplers(stage) = s;
    }
}

// renderengine::VertexProgramState: the vertex shader object lives at +0x2c (0x82c71b50 returns it)
void SetVertexProgramState(unsigned int vps)
{
    RWP_GUEST_FRAME(frame, 0x70);
    if (s_VertexProgramState == vps)
        return;
    RWP_CALL(0x82b7f408, D3DDevice_SetVertexShader, g_pD3DDevice, vps + 0x2c);
    s_VertexProgramState = vps;
    g_CurrentVPState = vps;
}


#define s_PixelProgramBinds    RWP_GLOBAL(RWP_U32, 0x5fff0000)
#define s_PixelStateBinds      RWP_GLOBAL(RWP_U32, 0x5fff0000)

// A pixel program: its shader (+0x254, the D3D object at +0x14), and -- when asked -- its depth
// stencil / blend / rasterizer states (+0x58 / +0x5c / +0x60, through the shadow) and its own
// bind function (+0x54, called with whatever r3 holds: it takes no arguments).
void SetPixelProgram(unsigned int ps, unsigned int states)
{
    RWP_GUEST_FRAME(frame, 0x70);
    const unsigned int id = RWP_GLOBAL(RWP_U32, ps + 0x250u);
    if (id != s_State3cf4)
        s_State3cf4 = id;
    const unsigned int sh = RWP_GLOBAL(RWP_U32, ps + 0x254u);
    if (sh)
        RWP_CALL(0x82b7f150, D3DDevice_SetPixelShader, g_pD3DDevice, sh + 0x14u);
    s_PixelProgramBinds = s_PixelProgramBinds + 1u;
    if (!(states & 0xffu))
        return;
    s_PixelStateBinds = s_PixelStateBinds + 1u;
    const unsigned int ds = RWP_GLOBAL(RWP_U32, ps + 0x58u);
    if (ds != s_DepthStencilState)
    {
        RWP_CALL(0x82a7b660, Device::SetDepthStencilState, ds);
        s_DepthStencilState = ds;
    }
    const unsigned int bl = RWP_GLOBAL(RWP_U32, ps + 0x5cu);
    if (bl != s_BlendState)
    {
        RWP_CALL(0x82a7b660, Device::SetBlendState, bl);
        s_BlendState = bl;
    }
    const unsigned int rs = RWP_GLOBAL(RWP_U32, ps + 0x60u);
    if (rs != s_RasterizerState)
    {
        RWP_CALL(0x82a7b660, Device::SetRasterizerState, rs);
        s_RasterizerState = rs;
    }
#if defined(RWP_GUEST)
    rwp::GuestCall(rwp::ResolveGuest(RWP_GLOBAL(RWP_U32, ps + 0x54u)));
#else
    ((RwpFn0)(size_t)(unsigned int)RWP_GLOBAL(RWP_U32, ps + 0x54u))();
#endif
}

}
