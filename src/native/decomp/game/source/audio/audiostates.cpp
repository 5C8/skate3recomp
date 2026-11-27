// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// Sk8::Audio's state machines (CSTATEMGR_Base / CSTATE_Base). They have no asserts in sk83, so the
// source path is not in the game's strings; filed under game/source/audio by the namespace.
//   CSTATEMGR_Base::UpdateParams(float)   sk83_na_m 0x82a694b0 (0x7c)
//   CSTATEMGR_Base::ProcessUpdate()       sk83_na_m 0x82a69530 (0x5c)
//   CSTATE_Base::UpdateParams(float)      sk83_na_m 0x82a699c0 (0xe0)
// Verified with sk83_na_m --sk83_replay.
//   CSTATE_Base::ProcessUpdate()          sk83_na_m 0x82a69970 (0x50) -- written, not yet replay-verified
// (Skate 2 PDB: CSTATE_Base is packages/sk8/audiocore/1.01.00/source/state_base.cpp)
#include "rwp_guestcall.h"

namespace Sk8 { namespace Audio {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
#endif

// A manager (+0x18 active): its clock (+4 time, +8 last dt), then every state in its list (+0x10,
// linked by +4) gets UpdateParams(dt) (vtable +0x14).
void CSTATEMGR_Base_UpdateParams(unsigned int self, double dt)
{
    RWP_SAVEGPRLR(31);
    RWP_SAVEF31(0x18u);
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_NVF31(dt);
    if (!RWP_LD8(self + 0x18u))
        return;
    RWP_FPU_MODE();
    const double t = (double)(float)((double)(float)RWP_GLOBAL(RWP_F32, self + 4u) + dt);    // fadds
    RWP_GLOBAL(RWP_F32, self + 8u) = (float)dt;
    RWP_GLOBAL(RWP_F32, self + 4u) = (float)t;
    for (unsigned int st = RWP_LD32(self + 0x10u); st; st = RWP_LD32(st + 4u))
    {
        RWP_NV(31, st);
        RWP_VCALL(st, 0x14, RWP_FLT(dt));
    }
}

// a manager's states each get ProcessUpdate (vtable +0x18) while it is active
void CSTATEMGR_Base_ProcessUpdate(unsigned int self)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    if (!RWP_LD8(self + 0x18u))
        return;
    for (unsigned int st = RWP_LD32(self + 0x10u); st; st = RWP_LD32(st + 4u))
    {
        RWP_NV(31, st);
        RWP_VCALL(st, 0x18);
    }
}

// A state: its clock (+0x38 time, +0x3c last dt); while active (+0x34) both of its child lists
// (+0x20, +0x24; linked by +4) get UpdateParams(dt) (vtable +0x24) and then Apply (+0x30).
void CSTATE_Base_UpdateParams(unsigned int self, double dt)
{
    RWP_SAVEGPRLR(30);
    RWP_SAVEF31(0x20u);
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_FPU_MODE();
    const double t = (double)(float)((double)(float)RWP_GLOBAL(RWP_F32, self + 0x38u) + dt);
    RWP_GLOBAL(RWP_F32, self + 0x3cu) = (float)dt;
    RWP_GLOBAL(RWP_F32, self + 0x38u) = (float)t;
    RWP_NV(30, self);
    RWP_NVF31(dt);
    if (!RWP_LD8(self + 0x34u))
        return;
    for (unsigned int list = 0x20u; list <= 0x24u; list += 4u)
        for (unsigned int c = RWP_LD32(self + list); c; c = RWP_LD32(c + 4u))
        {
            RWP_NV(31, c);
            RWP_VCALL(c, 0x24, RWP_FLT(dt));
            RWP_VCALL(c, 0x30);
        }
}

// a state's second child list (+0x24, linked by +4): each child gets ProcessUpdate (vtable +0x28)
void CSTATE_Base_ProcessUpdate(unsigned int self)
{
    RWP_SAVEGPRLR(31);
    RWP_GUEST_FRAME(frame, 0x60);
    for (unsigned int c = RWP_LD32(self + 0x24u); c; c = RWP_LD32(c + 4u))
    {
        RWP_NV(31, c);
        RWP_VCALL(c, 0x28);
    }
}

} }
