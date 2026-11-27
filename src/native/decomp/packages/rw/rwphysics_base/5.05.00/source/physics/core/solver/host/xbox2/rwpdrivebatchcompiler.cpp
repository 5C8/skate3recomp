// rw::physics "Drive Compiler" stage: a jacobian for every active constraint with an active body.
// sk83_na_m Simulation::DriveBatchBuild 0x82ce10e8 (0x94) -> DriveJacobian::Build 0x82cde360 (rwpdrivejacobian.cpp)
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "simulation.h"

namespace rw { namespace physics {

void Simulation::DriveBatchBuild()
{
    m_DS_Count = 0;
    m_DR_Count = 0;
    if (m_ActiveDR_Count == 0)
        return;
    RWP_PTR(DriveJacobian) jacobians = m_DJ_Stack;
    RWP_PTR(Drive) d = m_ActiveDR_Anchor->m_right;
    do
    {
        if ((d->m_bodyB->mState | d->m_bodyA->mState) & ACTIVE_BODY)
            jacobians[m_DR_Count++].Build(*d, this);
        d = d->m_right;
    } while (d != m_ActiveDR_Anchor);
}

} }
