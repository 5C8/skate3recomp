// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (6); comments keep the dev addresses.
// EA Apt (the Flash-like UI player; the library has no asserts in sk83, so its source path is not
// in the game's strings -- filed under the package name).
//   AptRenderElement::Render(unsigned int) const   sk83_na_m 0x83164140 (0x1e0)
//   CallLegacyTransformCallbacks                    sk83_na_m 0x831610c8 (0xe0)
//   AptRenderBuffer::Render(unsigned int)           sk83_na_m 0x83164400 (0x60)
//   AptRenderListSet::Render(unsigned int)          sk83_na_m 0x83164460 (0xa4)
// Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"

#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
extern "C" void *memcpy(void *, const void *, unsigned int);
void CallLegacyTransformCallbacks(unsigned int cmd);
extern "C" void RtlEnterCriticalSection(unsigned int cs);
#endif

#define g_AptRenderCallbacks 0x830ce9c8u      // +0xd0: shape callback for kind 1, +0xd4: for the others
#define AptCmdVtable         0x8230c738u      // the command's two embedded objects' vtable

#define g_AptLegacyMatrix    0x830d00d0u      // the 2D transform handed to the legacy callback

// The legacy transform hooks: one (+0x70) gets a small two-object block with the command's +0x44 /
// +0x4c, the other (+0x6c) the command matrix's 2D part (rows 0, 1 and 3, x and y).
void CallLegacyTransformCallbacks(unsigned int cmd)
{
    RWP_SAVEGPRLR(30);
    RWP_GUEST_FRAME(frame, 0x80);
    const unsigned int t = g_AptRenderCallbacks;
    RWP_NV(31, cmd);
    RWP_NV(30, 0xffffffff833e2d28ull);
    if (RWP_LD32(t + 0x70u))
    {
        const unsigned int a = RWP_LD32(cmd + 0x44u), b = RWP_LD32(cmd + 0x4cu);
        const unsigned int fn = RWP_LD32(t + 0x70u);
        RWP_ST32(FRAME_AT(frame, 0x50), AptCmdVtable);
        RWP_ST32(FRAME_AT(frame, 0x54), a);
        RWP_ST32(FRAME_AT(frame, 0x5c), b);
        RWP_ST32(FRAME_AT(frame, 0x58), AptCmdVtable);
        RWP_FNCALL(fn, FRAME_AT(frame, 0x50));
        RWP_ST32(FRAME_AT(frame, 0x58), AptCmdVtable);
        RWP_ST32(FRAME_AT(frame, 0x50), AptCmdVtable);
    }
    if (RWP_LD32(t + 0x6cu))
    {
        RWP_FPU_MODE();
        const unsigned int fn = RWP_LD32(t + 0x6cu);
        const unsigned int m = g_AptLegacyMatrix;
        RWP_GLOBAL(RWP_F32, m) = (float)RWP_GLOBAL(RWP_F32, cmd);
        RWP_GLOBAL(RWP_F32, m + 4u) = (float)RWP_GLOBAL(RWP_F32, cmd + 4u);
        RWP_GLOBAL(RWP_F32, m + 8u) = (float)RWP_GLOBAL(RWP_F32, cmd + 0x10u);
        RWP_GLOBAL(RWP_F32, m + 0xcu) = (float)RWP_GLOBAL(RWP_F32, cmd + 0x14u);
        RWP_GLOBAL(RWP_F32, m + 0x10u) = (float)RWP_GLOBAL(RWP_F32, cmd + 0x30u);
        RWP_GLOBAL(RWP_F32, m + 0x14u) = (float)RWP_GLOBAL(RWP_F32, cmd + 0x34u);
        RWP_FNCALL(fn, m);
    }
}

// The render command (0x60 bytes on the stack): the element's 0x40-byte matrix block, then fields
// from the element (+0x44 .. +0x5c), with two embedded objects (+0x40, +0x48) whose vtables are
// set, wiped by the zero fill and restored after the callback -- reproduced store for store.
static __forceinline void BuildCmd(unsigned int cmd)
{
    for (unsigned int k = 0; k < 0x40u; k += 8u)
        RWP_ST64(cmd + k, 0ull);
}

// An element: a pre-render hook (+0x68, vtable +0x18); without a clip owner (+0x64) and a mask
// (+0x60), the command goes to the Apt render callback for its kind (+0x58); with a clip owner it
// goes -- if the element's layer (+0x50) is in the layer mask -- through the legacy transform
// callbacks to the owner (vtable +0, with the mask object).
void AptRenderElement_Render(unsigned int self, unsigned int layers)
{
    RWP_SAVEGPRLR(26);
    RWP_GUEST_FRAME(frame, 0x150);
    RWP_NV(30, self);
    RWP_NV(26, layers);
    const unsigned int hook = RWP_LD32(self + 0x68u);
    if (hook)
        RWP_VCALL(hook, 0x18, layers);
    RWP_NV(31, 0u);
    RWP_NV(29, 0xffffffff82289134ull);
    RWP_NV(28, 0xffffffff82289134ull);
    if (!RWP_LD32(self + 0x64u) && !RWP_LD32(self + 0x60u))
    {
        const unsigned int cmd = FRAME_AT(frame, 0x50);
        BuildCmd(cmd);
        RWP_ST32(FRAME_AT(frame, 0x90), AptCmdVtable);
        RWP_ST32(FRAME_AT(frame, 0x98), AptCmdVtable);
        RWP_ST32(FRAME_AT(frame, 0x94), 0u);
        RWP_ST32(FRAME_AT(frame, 0x9c), 0u);
        for (unsigned int k = 0; k < 0x60u; k += 8u)
            RWP_ST64(cmd + k, 0ull);
        RWP_CALL(0x82f52b30, memcpy, cmd, self, 0x40u);
        const unsigned int kind = RWP_LD32(self + 0x58u);
        const unsigned int a = RWP_LD32(self + 0x44u), b = RWP_LD32(self + 0x4cu), c = RWP_LD32(self + 0x50u);
        const unsigned int d = RWP_LD32(self + 0x54u), e = RWP_LD32(self + 0x5cu);
        RWP_ST32(FRAME_AT(frame, 0xa8), kind);
        RWP_ST32(FRAME_AT(frame, 0x94), a);
        RWP_ST32(FRAME_AT(frame, 0x9c), b);
        RWP_ST32(FRAME_AT(frame, 0xa0), c);
        RWP_ST32(FRAME_AT(frame, 0xa4), d);
        RWP_ST32(FRAME_AT(frame, 0xac), e);
        const unsigned int fn = RWP_LD32(g_AptRenderCallbacks + ((int)kind == 1 ? 0xd0u : 0xd4u));
        RWP_FNCALL(fn, cmd);
        RWP_ST32(FRAME_AT(frame, 0x98), AptCmdVtable);
        RWP_ST32(FRAME_AT(frame, 0x90), AptCmdVtable);
    }
    if (!RWP_LD32(self + 0x64u))
        return;
    const unsigned int layer = RWP_LD32(self + 0x50u);
    RWP_NV(27, layer);
    if (!(((layer & 0x20u) ? 0u : (1u << (layer & 0x1fu))) & layers))
        return;
    const unsigned int cmd = FRAME_AT(frame, 0xb0);
    BuildCmd(cmd);
    RWP_ST32(FRAME_AT(frame, 0xf4), 0u);
    RWP_ST32(FRAME_AT(frame, 0xfc), 0u);
    RWP_ST32(FRAME_AT(frame, 0xf0), AptCmdVtable);
    RWP_ST32(FRAME_AT(frame, 0xf8), AptCmdVtable);
    for (unsigned int k = 0; k < 0x60u; k += 8u)
        RWP_ST64(cmd + k, 0ull);
    RWP_CALL(0x82f52b30, memcpy, cmd, self, 0x40u);
    RWP_ST32(FRAME_AT(frame, 0xf4), RWP_LD32(self + 0x44u));
    RWP_ST32(FRAME_AT(frame, 0xfc), RWP_LD32(self + 0x4cu));
    RWP_ST32(FRAME_AT(frame, 0x100), layer);
    RWP_ST32(FRAME_AT(frame, 0x104), RWP_LD32(self + 0x54u));
    RWP_ST32(FRAME_AT(frame, 0x108), RWP_LD32(self + 0x58u));
    RWP_ST32(FRAME_AT(frame, 0x10c), RWP_LD32(self + 0x5cu));
    RWP_CALL(0x82e85038, CallLegacyTransformCallbacks, cmd);           // through the guest: sk83 hooks it (APT HUD spread)
    const unsigned int owner = RWP_LD32(self + 0x64u);
    RWP_VCALL(owner, 0, cmd, RWP_LD32(self + 0x60u));
}

// A render buffer: elements (+0, 0x6c bytes each, count +4), drawn while it is valid (+0xc).
void AptRenderBuffer_Render(unsigned int self, unsigned int layers)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(29, self);
    RWP_NV(28, layers);
    if (!RWP_LD8(self + 0xcu))
        return;
    unsigned int off = 0;
    for (unsigned int i = 0; i < RWP_LD32(self + 4u); i++, off += 0x6cu)
    {
        RWP_NV(30, i);
        RWP_NV(31, off);
        AptRenderElement_Render(RWP_LD32(self) + off, layers);
    }
}

// The list set triple-buffers its render lists (+0, 0x14 bytes each): the one being drawn
// (+0x44 = +0x40, mod 3) is locked (its critical section, +0x10; left locked here -- the producer
// side releases it) and flagged in use (+0xd), then drawn.
void AptRenderListSet_Render(unsigned int self, unsigned int layers)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(31, self);
    RWP_NV(30, 0xffffffffaaaaaaabull);
    RWP_NV(28, layers);
    RWP_ST32(self + 0x44u, RWP_LD32(self + 0x40u));
    const unsigned int buf = self + 0x14u * (RWP_LD32(self + 0x44u) % 3u);
    RWP_NV(29, buf);
    RWP_CALL(0x82f9cb44, RtlEnterCriticalSection, RWP_LD32(buf + 0x10u));
    RWP_ST8(buf + 0xdu, 1u);
    const unsigned int cur = self + 0x14u * (RWP_LD32(self + 0x44u) % 3u);
    if (cur && RWP_LD8(cur + 0xcu))
        AptRenderBuffer_Render(cur, layers);
}
