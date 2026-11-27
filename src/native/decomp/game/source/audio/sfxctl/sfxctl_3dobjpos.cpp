// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (2); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82096500 8209660c 82b58838 82dad568 8336cfec
// Sk8::Audio::SFXCTL_3DObjPos, the 3D position controller of an audio object (Skate 2 PDB:
// code/ng/audio/sfxctl/sfxctl_3dobjpos.cpp -> game/source/audio/sfxctl in sk83's layout; its only
// assert is the inlined singletonholder.h one, which says nothing about the file).
//   SFXCTL_3DObjPos::UpdateParams(float)       sk83_na_m 0x82645290 (0x1f8)
//   SFXCTL_3DObjPos::UpdateDoppler(int, float) sk83_na_m 0x8261f0a0 (0x404)
// Written from the sk83 recompiled code (Skate 2 Hex-Rays as the guide); not yet replay-verified.
#include "rwp_guestcall.h"

namespace Sk8 { namespace Audio {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
void xcore_FailAssert(unsigned int, unsigned int, unsigned int);
void DebugBreak_();
#endif

void SFXCTL_3DObjPos_UpdateDoppler(unsigned int self, unsigned int playerNum, double dt);

#define g_MicrophoneMan     0x5fff0000u     // SingletonHolder<Sk8::Audio::MicrophoneMan>::sInstance
#define g_SingletonHolderH  0x5fff0000u     // "...singleton/singletonholder.h"
#define g_3dZero            0x5fff0000u     // 0.0f
#define g_3dMinusOne        0x8216dee0u     // -1.0f

// Nicotine::DMixIO inputs the controller drives (vtable +8 SetDMixInput(idx, value), +0xc Get)
enum { kDMixIn_Flags = 15, kDMixIn_Player = 11, kDMixIn_Speed0 = 13, kDMixIn_Speed1 = 14 };

// SingletonHolder<MicrophoneMan>::Instance(): asserts (line 130) on a null instance
static __forceinline unsigned int Microphones()
{
    if (!RWP_LD32(g_MicrophoneMan))
    {
        RWP_CALL(0x5fff0000, xcore_FailAssert, 0u, g_SingletonHolderH, 130u);
        RWP_CALL0(0x5fff0000, DebugBreak_);
    }
    return RWP_LD32(g_MicrophoneMan);
}

// |d.xyz| as rw::math computes it: rsqrte refined by two Newton steps, times the square; 0 for 0
static __forceinline float Length3(__vector4 d)
{
    const __vector4 zero = __vspltisw(0);
    const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
    const __vector4 sq = __vmsum3fp(d, d);
    __vector4 r = __vrsqrtefp(sq);
    r = __vmaddfp(__vmulfp(r, half), __vnmsubfp(sq, __vmulfp(r, r), one), r);
    r = __vmaddfp(__vmulfp(r, half), __vnmsubfp(sq, __vmulfp(r, r), one), r);
    return VLANE_F(__vsel(__vmulfp(sq, r), zero, __vcmpeqfp(zero, sq)), 0);
}

static __forceinline unsigned int Vec(unsigned int a)   // lvx: 16-byte aligned
{
    return a & ~15u;
}

// The object's distances to the two microphones (+0x28 / +0x30, previous ones in +0x2c / +0x34)
// and its velocity relative to each (+0x60 / +0x68, previous +0x64 / +0x6c), signed negative while
// it approaches that microphone. A speed that changes sign flags a doppler reset on the mix input
// (bit 31 / bit 30 of input 15); both speeds go to inputs 13 / 14 as raw floats.
void SFXCTL_3DObjPos_UpdateDoppler(unsigned int self, unsigned int playerNum, double dt)
{
    (void)playerNum;
    (void)dt;
    RWP_SAVEGPRLR(29);
    RWP_SAVEF31(0x28u);
    RWP_GUEST_FRAME(frame, 0xe0);
    if (!RWP_LD32(self + 36u))                                    // m_v3Obj.vel
        return;
    RWP_NV(31, self);
    RWP_NV(30, 0xffffffff83370000ull);
    RWP_NV(29, 0xffffffff82096500ull);
    // velocity relative to both microphones (+0x70, +0x30), keep the old distance to the first
    unsigned int mic = Microphones();
    RWP_VMX_MODE();
    const __vector4 vel = RWP_GLOBAL(RWP_VEC, Vec(RWP_LD32(self + 36u)));
    const __vector4 relVel0 = __vsubfp(vel, RWP_GLOBAL(RWP_VEC, Vec(mic + 112u)));
    const __vector4 relVel1 = __vsubfp(vel, RWP_GLOBAL(RWP_VEC, Vec(mic + 48u)));
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, self + 44u) = (float)RWP_GLOBAL(RWP_F32, self + 40u);
    // distance to the listener position (+0x40), keep the old distance to the second
    mic = Microphones();
    RWP_FPU_MODE();
    const float prevDist1 = RWP_GLOBAL(RWP_F32, self + 48u);
    RWP_VMX_MODE();
    const __vector4 d0 = __vsubfp(RWP_GLOBAL(RWP_VEC, Vec(mic + 64u)), RWP_GLOBAL(RWP_VEC, Vec(RWP_LD32(self + 32u))));
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, self + 52u) = prevDist1;
    RWP_VMX_MODE();
    const float dist0 = Length3(d0);
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, self + 40u) = dist0;
    mic = Microphones();
    RWP_VMX_MODE();
    const __vector4 d1 = __vsubfp(RWP_GLOBAL(RWP_VEC, Vec(mic)), RWP_GLOBAL(RWP_VEC, Vec(RWP_LD32(self + 32u))));
    RWP_FPU_MODE();
    const bool closer0 = (float)RWP_GLOBAL(RWP_F32, self + 44u) > (float)RWP_GLOBAL(RWP_F32, self + 40u);
    RWP_GLOBAL(RWP_F32, self + 100u) = (float)RWP_GLOBAL(RWP_F32, self + 96u);
    RWP_GLOBAL(RWP_F32, self + 108u) = (float)RWP_GLOBAL(RWP_F32, self + 104u);
    const float minusOne = RWP_GLOBAL(RWP_F32, g_3dMinusOne);
    RWP_VMX_MODE();
    const float speed0 = Length3(relVel0), speed1 = Length3(relVel1), dist1 = Length3(d1);
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, self + 96u) = speed0;
    RWP_GLOBAL(RWP_F32, self + 104u) = speed1;
    RWP_GLOBAL(RWP_F32, self + 48u) = dist1;
    if (closer0)
        RWP_GLOBAL(RWP_F32, self + 96u) = speed0 * minusOne;            // fmuls
    if ((float)RWP_GLOBAL(RWP_F32, self + 52u) > dist1)
        RWP_GLOBAL(RWP_F32, self + 104u) = speed1 * minusOne;
    const float zero = RWP_GLOBAL(RWP_F32, g_3dZero);
    RWP_NVF31(zero);
    for (int k = 0; k < 2; k++)
    {
        const unsigned int cur = k ? 104u : 96u;
        const float prev = RWP_GLOBAL(RWP_F32, self + cur + 4u);
        const bool flip = (prev < zero && (float)RWP_GLOBAL(RWP_F32, self + cur) > zero) ||
                          (prev > zero && (float)RWP_GLOBAL(RWP_F32, self + cur) < zero);
        if (!flip)
            continue;
        const unsigned int dm = RWP_LD32(self + 12u);
        const unsigned int flags = dm ? RWP_VCALL(dm, 0xc, (unsigned int)kDMixIn_Flags) : 0u;
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Flags, flags | (k ? 0x40000000u : 0x80000000u));
        RWP_FPU_MODE();
    }
    if (RWP_LD32(self + 12u))
        RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Speed0, RWP_LD32(self + 96u));
    if (RWP_LD32(self + 12u))
        RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Speed1, RWP_LD32(self + 104u));
}

// Without a position (+0x20) the object is unplaced: the mix inputs 0..3 are reset (distances
// -1.0f as raw bits) and input 15 loses its live bit. Placed: input 15 gets the live bit, input 11
// the player, then Generate3DParams(0) (vtable +0x34) and the doppler update.
void SFXCTL_3DObjPos_UpdateParams(unsigned int self, double dt)
{
    RWP_SAVEGPRLR(30);
    RWP_SAVEF31(0x20u);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(31, self);
    RWP_NVF31(dt);
    if (!RWP_LD32(self + 32u))
    {
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, 3u, 0u);
        RWP_FPU_MODE();
        const unsigned int minusOne = RWP_LD32(g_3dMinusOne);            // lfs; stfs 0x50(r1); lwz
        RWP_GLOBAL(RWP_U32, FRAME_AT(frame, 0x50)) = minusOne;
        RWP_NV(30, minusOne);
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, 1u, minusOne);
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, 2u, 0u);
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, 0u, minusOne);
        const unsigned int flags = RWP_LD32(self + 12u) ? RWP_VCALL(RWP_LD32(self + 12u), 0xc, (unsigned int)kDMixIn_Flags) : 0u;
        if (RWP_LD32(self + 12u))
            RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Flags, flags & ~1u);
        return;
    }
    const unsigned int flags = RWP_LD32(self + 12u) ? RWP_VCALL(RWP_LD32(self + 12u), 0xc, (unsigned int)kDMixIn_Flags) : 0u;
    if (RWP_LD32(self + 12u))
        RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Flags, flags | 1u);
    if (RWP_LD32(self + 12u))
        RWP_VCALL(RWP_LD32(self + 12u), 8, (unsigned int)kDMixIn_Player, RWP_LD32(self + 56u));
    RWP_VCALL(self, 0x34, 0u);                                          // Generate3DParams(0)
    SFXCTL_3DObjPos_UpdateDoppler(self, RWP_LD32(self + 56u), dt);
}

} }
