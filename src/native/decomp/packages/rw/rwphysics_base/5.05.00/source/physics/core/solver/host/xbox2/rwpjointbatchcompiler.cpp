// rw::physics "Joint Compiler" stage: a jacobian for every active constraint with an active body.
// sk83_na_m Simulation::JointBatchBuild 0x82ce1180 (0x94) -> JointJacobian::Build 0x82cdf068 (rwpjointjacobian.cpp)
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

// Build a jacobian for every active joint with at least one active body.
void Simulation::JointBatchBuild()
{
    m_JS_Count = 0;
    m_JT_Count = 0;
    if (m_ActiveJT_Count == 0)
        return;
    RWP_PTR(JointJacobian) jacobians = m_JJ_Stack;
    RWP_PTR(Joint) j = m_ActiveJT_Anchor->m_right;
    do
    {
        if ((j->m_bodyB->mState | j->m_bodyA->mState) & ACTIVE_BODY)
            jacobians[m_JT_Count++].Build(*j, this);
        j = j->m_right;
    } while (j != m_ActiveJT_Anchor);
}

} }
