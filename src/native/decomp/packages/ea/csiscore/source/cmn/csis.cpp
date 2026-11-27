// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// Csis (EA's audio class/instance system; Skate 2 PDB: packages/ea/csiscore/2.11.00/source/cmn/csis.cpp).
//   Csis::ClassData::SendParameters(Csis::Parameter *)   sk83_na_m 0x82a6f238 (0x5c)
// Its callers (Class::SetMemberData 0x82a6fe10 ...) hold the Csis mutex, which sk83_na_m maps to a
// host mutex (sk83_native_sync.cpp); they stay recompiled.
// Written from the sk83 recompiled code; not yet replay-verified.
#include "rwp_guestcall.h"

namespace Csis {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

// every update client (mUpdateClients, +8; nodes linked by +0) gets the new parameters:
// callback (node +8)(pParameters, its context (node +0xc))
void ClassData_SendParameters(unsigned int self, unsigned int pParameters)
{
    RWP_SAVEGPRLR(30);
    RWP_GUEST_FRAME(frame, 0x70);
    RWP_NV(30, pParameters);
    for (unsigned int n = RWP_LD32(self + 8u); n; n = RWP_LD32(n))
    {
        RWP_NV(31, n);
        RWP_FNCALL(RWP_LD32(n + 8u), pParameters, RWP_LD32(n + 12u));
    }
}

}
