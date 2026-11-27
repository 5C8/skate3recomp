// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (3); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82974b70 82d8e540 82d96d48 82f63ab0 8336b260 8336b26c 833a5ea0
// game/source/render/simpledraw.cpp (sk83 assert path, lines 577-579): Sk8::Render::SimpleDraw.
//   Sk8::Render::SimpleDraw::Draw_TriListTextured_Fasttrack(numVerts, verts, uvs, tex, colour, m)
//                                                    sk83_na_m 0x82987d38 (0x2d0)
// The Apt texture units' draw (AptTextureUnit::Draw). Written from the sk83 recompiled code; not
// yet replay-verified.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
unsigned int SimpleDraw_BeginEffect(unsigned int binding, unsigned int pass);
unsigned int StateShadow_GetStateFactoryInstance();
unsigned int StateShadow_GetVertexProgramState(unsigned int factory, unsigned int desc, unsigned int program);
void StateShadow_SetVertexProgramState(unsigned int state);
void D3DDevice_InsertFence(unsigned int dev);
unsigned int D3DDevice_BeginVertices(unsigned int dev, unsigned int prim, unsigned int count, unsigned int stride);
void D3DDevice_EndVertices(unsigned int dev);
#endif

#define g_SimpleDrawDevice                   0x5fff0000u   // renderengine::Device::m_device
#define g_FasttrackTexParam           0x5fff0000u   // function static {texture, 0, 0} (the sampler parameter)
#define g_FasttrackTexParamGuard      0x5fff0000u
#define kFasttrackMaxInlineBytes      0x80000u      // above this the device inserts a fence first

// The draw: the effect binding (+0, its constant buffer +0x14) gets the transposed matrix
// (4 rows, key 0x7ddf552b), the colour (0x55294431) and the texture parameter (0xa02d9166); then
// the effect pass, the vertex program state for (+0x40, effect +0x250), and the triangles go
// inline through BeginVertices: per vertex the position (x y z w) and the uv's first two words
// (both arrays stride 16), 24 bytes.
void SimpleDraw_Draw_TriListTextured_Fasttrack(unsigned int self, unsigned int numVerts, unsigned int verts,
                                               unsigned int uvs, unsigned int tex, unsigned int colour,
                                               unsigned int m)
{
    RWP_SAVEGPRLR(24);
    RWP_GUEST_FRAME(frame, 0xf0);
    RWP_NV(31, self);
    RWP_NV(28, numVerts);
    RWP_NV(25, verts);
    RWP_NV(24, uvs);
    RWP_NV(27, tex);
    RWP_NV(26, colour);
    RWP_NV(29, m);
    RWP_NV(30, 0xffffffff82202c3cull);                                   // the assert path (lines 577-579: verts, uvs, tex)
    RWP_NV(30, 0xffffffff8336b260ull);
    {
        const unsigned int guard = RWP_LD32(g_FasttrackTexParamGuard);
        if (!(guard & 1u))
        {
            RWP_ST32(g_FasttrackTexParam + 4u, 0u);
            RWP_ST32(g_FasttrackTexParamGuard, guard | 1u);
            RWP_ST32(g_FasttrackTexParam + 8u, 0u);
        }
    }
    RWP_ST32(g_FasttrackTexParam, tex);
    RWP_VCALL(RWP_LD32(self), 4, RWP_LD32(self + 8u));
    RWP_VCALL(RWP_LD32(self), 8, 0u);
    // the matrix, transposed (vmrghw / vmrglw), into +0x60 .. +0x9c
    const unsigned int t60 = FRAME_AT(frame, 0x60);
    {
        const unsigned int mm = m & ~15u;
        for (unsigned int r = 0; r < 4u; r++)
            for (unsigned int c = 0; c < 4u; c++)
                RWP_ST32(t60 + 0x10u * c + 4u * r, RWP_LD32(mm + 0x10u * r + 4u * c));
    }
    RWP_NV(27, FRAME_AT(frame, 0x90));
    const unsigned int cb = RWP_LD32(RWP_LD32(self) + 0x14u);
    RWP_NV(29, cb);
    RWP_VCALL(cb, 8, 0x7ddf552bu, t60, 4u);
    RWP_VCALL(cb, 8, 0x55294431u, colour, 1u);
    RWP_VCALL(cb, 0xc, 0xffffffffa02d9166ull, 0xffffffff8336b260ull);
    RWP_CALL(0x82ca5d10, SimpleDraw_BeginEffect, RWP_LD32(self), 1u);
    const unsigned int desc = RWP_LD32(self + 0x40u), program = RWP_LD32(RWP_LD32(self) + 0x250u);
    RWP_NV(31, desc);
    RWP_NV(30, program);
    const unsigned int factory = RWP_CALL0(0x5fff0000, StateShadow_GetStateFactoryInstance);
    const unsigned int vps = RWP_CALL(0x5fff0000, StateShadow_GetVertexProgramState, RWP_LD32(factory), desc, program);
    RWP_CALL(0x82cfcac8, StateShadow_SetVertexProgramState, vps);
    const unsigned int dev = RWP_LD32(g_SimpleDrawDevice);
    RWP_NV(31, dev);
    if (numVerts * 24u > kFasttrackMaxInlineBytes)
        RWP_CALL(0x5fff0000, D3DDevice_InsertFence, dev);
    const unsigned int p = RWP_CALL(0x82b79fc0, D3DDevice_BeginVertices, dev, 4u, numVerts, 24u);
    if (!p)
        return;
    unsigned int v = verts, o = p;
    for (unsigned int i = 0; i < numVerts; i++, v += 0x10u, o += 24u)
    {
        const unsigned int pos = v & ~15u, uv = (uvs - verts + v) & ~15u;
        RWP_ST32(o, RWP_LD32(pos));
        RWP_ST32(o + 4u, RWP_LD32(pos + 4u));
        RWP_ST32(o + 8u, RWP_LD32(pos + 8u));
        RWP_ST32(o + 0xcu, RWP_LD32(pos + 0xcu));
        RWP_ST32(o + 0x10u, RWP_LD32(uv));
        RWP_ST32(o + 0x14u, RWP_LD32(uv + 4u));
    }
    RWP_CALL(0x5fff0000, D3DDevice_EndVertices, dev);
}

} }
