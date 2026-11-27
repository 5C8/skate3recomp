// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (10); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 822a40d0 822c1ac0 824e0088 82977518 829778b8 82978d00 82979c78 82983940 82983cb0 82984c30 829941e8 829943e8 82d77458 83368e64 833a5ea0
// game/source/render/PostProcessor.cpp (sk83 assert path): Sk8::Render::PostFX, the frame's
// post-processing chain.
//   PostFX frame entry                     sk83_na_m 0x829a0120 (0xa8; no name in the map)
//   PostFX::Render (the pass dispatcher)   sk83_na_m 0x82995040 (0x1c8; assert line 3305)
// The passes themselves (0x82983cb0 ...) are still the recompiled code. Verified with
// sk83_na_m --sk83_replay.
#if !defined(RWP_GUEST) && !defined(HORUS_NATIVE)
#include <rw/math/vpu.h>
#endif
#include "rwp_guestcall.h"

namespace StateShadow { void Reset(); }

namespace Sk8 { namespace Render {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
void PostFX_Pass_82983cb0(unsigned int self);
void PostFX_Pass_829941e8(unsigned int self);
void PostFX_Pass_82983940(unsigned int self);
void PostFX_Pass_82979c78(unsigned int self);
void PostFX_Composite_829943e8(unsigned int self, unsigned int flags);
void PostFX_Composite_82978d00(unsigned int self, unsigned int flags);
void PostFX_ReleaseTarget(unsigned int self, unsigned int target);
void PostFX_Pass_82984c30(unsigned int self, float a, float b, unsigned int v);
unsigned int Attrib_GetAttributePointer(unsigned int inst, unsigned long long key, unsigned int idx);
unsigned int Attrib_DefaultDataArea(unsigned int size);
void PostFX_Pass_829778b8(unsigned int self);
void PostFX_Blit_ToDeviceRT(unsigned int self, unsigned int arg);
void PostFX_Begin_8299fe10(unsigned int self);
void PostFX_End_82970ee0(unsigned int self);
void PostFX_Skip_82978910(unsigned int self);
#endif

#define g_PostFXNoDeviceBlit RWP_GLOBAL(RWP_U32, 0x5fff0000)   // non-zero: the chain does not end on the device target

// The passes run by the flag word at +0xc54 (bit 2, 0x100, 8, 0x200, then the composite: bit 0
// or else bit 1 -- one of them must be set). Unless something else owns the device target, the
// two intermediate targets (+0x210, +0xec) are released and the hook at +0xc0c is told
// (vtable +4). Bits 0x400 / 0x40 run the 0x82984c30 pass with +0xc14, +0xc18 and the vector at
// +0xc20; an attrib switch (+0x8d0, key 0xde28dc51848e90d8) runs 0x829778b8; then the blit.
void PostFX_Render(unsigned int self)
{
    RWP_SAVEGPRLR(30);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(31, self);
    StateShadow::Reset();
    if (RWP_LD32(self + 0xc54u) & 4u)
        RWP_CALL(0x5fff0000, PostFX_Pass_82983cb0, self);
    if (RWP_LD32(self + 0xc54u) & 0x100u)
        RWP_CALL(0x5fff0000, PostFX_Pass_829941e8, self);
    if (RWP_LD32(self + 0xc54u) & 8u)
        RWP_CALL(0x5fff0000, PostFX_Pass_82983940, self);
    if (RWP_LD32(self + 0xc54u) & 0x200u)
        RWP_CALL(0x5fff0000, PostFX_Pass_82979c78, self);
    {
        const unsigned int f = RWP_LD32(self + 0xc54u);
        if (f & 1u)                                                  // (the flag word goes along in r4)
            RWP_CALL(0x5fff0000, PostFX_Composite_829943e8, self, f);
        else if (f & 2u)
            RWP_CALL(0x5fff0000, PostFX_Composite_82978d00, self, f);
        // else: assert (line 3305)
    }
    RWP_NV(30, 0xffffffff83370000ull);                                   // lis r30 (&g_PostFXNoDeviceBlit; sign-extended)
    if (!g_PostFXNoDeviceBlit)
    {
        RWP_CALL(0x827eec28, PostFX_ReleaseTarget, self, RWP_LD32(self + 0x210u));
        RWP_CALL(0x827eec28, PostFX_ReleaseTarget, self, RWP_LD32(self + 0xecu));
        const unsigned int hook = RWP_LD32(self + 0xc0cu);
        if (hook)
            RWP_VCALL(hook, 4);
    }
    {
        const unsigned int f = RWP_LD32(self + 0xc54u);
        if ((f & 0x400u) || (f & 0x40u))
        {
            RWP_FPU_MODE();
            const float b = RWP_GLOBAL(RWP_F32, self + 0xc18u), a = RWP_GLOBAL(RWP_F32, self + 0xc14u);
            RWP_GLOBAL(RWP_VEC, FRAME_AT(frame, 0x50)) = (__vector4)RWP_GLOBAL(RWP_VEC, self + 0xc20u);
            RWP_CALL(0x5fff0000, PostFX_Pass_82984c30, self, RWP_FLT(a), RWP_FLT(b), FRAME_AT(frame, 0x50));
        }
    }
    unsigned int attr = RWP_CALL(0x5fff0000, Attrib_GetAttributePointer, self + 0x8d0u, 0xde28dc51848e90d8ull, 0u);
    if (!attr)
        attr = RWP_CALL(0x5fff0000, Attrib_DefaultDataArea, 1u);
    if (RWP_LD8(attr))
        RWP_CALL(0x5fff0000, PostFX_Pass_829778b8, self);
    if (!g_PostFXNoDeviceBlit)
        RWP_CALL(0x5fff0000, PostFX_Blit_ToDeviceRT, self, 0u);
    StateShadow::Reset();
}

// The frame's post-processing: when enabled (+0x50) begin, run the chain, end; otherwise the
// skip path, which still releases the targets when +0xc08 says so and tells the +0xc0c hook.
void PostFX_Frame(unsigned int self)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    RWP_NV(31, self);
    if (RWP_LD8(self + 0x50u))
    {
        RWP_CALL(0x827f4d28, PostFX_Begin_8299fe10, self);
        PostFX_Render(self);
        RWP_CALL(0x827f52e8, PostFX_End_82970ee0, self);
        return;
    }
    RWP_CALL(0x827f0ef0, PostFX_Skip_82978910, self);
    if (!RWP_LD8(self + 0xc08u))
        return;
    RWP_CALL(0x827eec28, PostFX_ReleaseTarget, self, RWP_LD32(self + 0x210u));
    RWP_CALL(0x827eec28, PostFX_ReleaseTarget, self, RWP_LD32(self + 0xecu));
    const unsigned int hook = RWP_LD32(self + 0xc0cu);
    if (hook)
        RWP_VCALL(hook, 4);
}

// ---- PostFX quad helpers (sk83 has no path for them: no asserts; filed with their PostFX callers)
//   PostFX: draw a quad with an effect and eight vector parameters   sk83_na_m 0x8296f158 (0x220)
//   PostFX: 2x2 box tap offsets for a w x h texture                   sk83_na_m 0x8296f490 (0x90, leaf)
// Written from the sk83 recompiled code; not yet replay-verified.
#if !defined(RWP_GUEST)
void PostFX_SetVertexProgramState(unsigned int state);
unsigned int PostFX_BeginEffect(unsigned int binding, unsigned int pass);
void PostFX_SetStreamSource(unsigned int dev, unsigned int stream, unsigned int vb, unsigned int offset,
                            unsigned int stride, unsigned int flags);
void PostFX_DrawVertices(unsigned int dev, unsigned int prim, unsigned int start, unsigned int count);
#endif

#define g_PostFXDevice      0x5fff0000u   // renderengine::Device::m_device
#define kPostFXQuadZeroVec  0x5fff0000u   // (0, 0, 0, 0)
#define kPostFXQuadOneVec   0x5fff0000u   // (1, 1, 1, 1)
#define kPostFXTapOne       0x8231a844u   // 1.0f
#define kPostFXTapQuarter   0x820c6d98u   // 0.25f

// The effect binding's vertex program state (+0x64), then eight vector parameters on its constant
// buffer (+0xc, vtable +8, count 1): missing ones (null) get zero or one vectors (stack +0x50 /
// +0x60); the last two come on the stack (caller's +0x54 / +0x5c). Then the effect pass and the
// PostFX's quad: stream 0 = +0x34, DrawVertices(+0x24, +0x28, +0x2c).
void PostFX_DrawQuad(unsigned int self, unsigned int binding, unsigned int p5, unsigned int p6, unsigned int p7,
                     unsigned int p8, unsigned int p9, unsigned int p10, unsigned int pStack54, unsigned int pStack5c)
{
    RWP_SAVEGPRLR(24);
    RWP_GUEST_FRAME(frame, 0xc0);
    RWP_NV(24, p10);
    RWP_NV(28, p8);
    RWP_NV(29, p9);
    RWP_NV(26, p6);
    RWP_NV(27, p7);
    RWP_NV(25, self);
    RWP_NV(31, binding);
    RWP_NV(30, p5);
    const unsigned int t50 = FRAME_AT(frame, 0x50), t60 = FRAME_AT(frame, 0x60);
    RWP_ST64(t50, RWP_LD64(kPostFXQuadZeroVec));
    RWP_ST64(t50 + 8u, RWP_LD64(kPostFXQuadZeroVec + 8u));
    RWP_ST64(t60, RWP_LD64(kPostFXQuadOneVec));
    RWP_ST64(t60 + 8u, RWP_LD64(kPostFXQuadOneVec + 8u));
    RWP_CALL(0x82cfcac8, PostFX_SetVertexProgramState, RWP_LD32(binding + 0x64u));
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0xffffffffa88ed8c4ull, p5 ? p5 : t50, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0xffffffffbf99859cull, p9 ? p9 : t60, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0x02d6c9b8u, p8 ? p8 : t50, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0x11978c0du, p7 ? p7 : t60, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0x59a14d69u, p6 ? p6 : t50, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0xffffffffb0f346c4ull, p10 ? p10 : t50, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0xffffffffb5f58d4bull, pStack54 ? pStack54 : t50, 1u);
    RWP_VCALL(RWP_LD32(binding + 0xcu), 8, 0xffffffffa80481c5ull, pStack5c ? pStack5c : t50, 1u);
    RWP_CALL(0x82ca5d10, PostFX_BeginEffect, binding, 1u);
    RWP_NV(31, 0xffffffff833a0000ull);
    RWP_CALL(0x82b78ff0, PostFX_SetStreamSource, RWP_LD32(g_PostFXDevice), 0u, RWP_LD32(self + 0x34u), 0u, 0u, 1u);
    RWP_CALL(0x82b7a970, PostFX_DrawVertices, RWP_LD32(g_PostFXDevice), RWP_LD32(self + 0x24u), RWP_LD32(self + 0x28u),
             RWP_LD32(self + 0x2cu));
}

// Four taps (x, y, 0.25, 1) at (-1, -1) (1, -1) (-1, 1) (1, 1) texels of a w x h texture, y major.
// The int -> float conversions go through the stack (std, r1-0x10 / r1-8): left as the last
// iteration wrote them.
void PostFX_BoxTapOffsets(unsigned int self, unsigned int out, int w, int h)
{
    (void)self;
    RWP_FPU_MODE();
    const float fh = (float)(double)h, fw = (float)(double)w;
    const float one = RWP_GLOBAL(RWP_F32, kPostFXTapOne), quarter = RWP_GLOBAL(RWP_F32, kPostFXTapQuarter);
    const float iw = one / fw, ih = one / fh;
    unsigned int o = out;
    for (int j = 0; j < 4; j += 2)
    {
        const float y = (float)(j - 1) * ih;
        for (int i = 0; i < 4; i += 2, o += 0x10u)
        {
            RWP_GLOBAL(RWP_F32, o) = (float)(i - 1) * iw;
            RWP_GLOBAL(RWP_F32, o + 4u) = y;
            RWP_GLOBAL(RWP_F32, o + 8u) = quarter;
            RWP_GLOBAL(RWP_F32, o + 0xcu) = one;
        }
    }
#if defined(RWP_GUEST)
    GStore64(g_ctx->r1.u32 - 0x10u, 1ull);                               // std r10, -0x10(r1): j - 1
    GStore64(g_ctx->r1.u32 - 8u, 1ull);                                  // std r6, -8(r1): i - 1
#endif
}

} }
