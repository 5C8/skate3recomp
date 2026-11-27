// rw::physics "Dynamic Update" stage: integrate every active rigid body
// (sk83_na_m Simulation::BatchIntegrator 0x82cde1a8; RigidBody::DynamicUpdate is in
// core/rigidbody/host/xbox2/rwprigidbody.cpp).
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

void Simulation::BatchIntegrator()
{
    // the anchor is a sentinel in the circular mRight list; the list is never empty here
    RWP_PTR(RigidBody) b = m_ActiveRB_Anchor->mRight;
    do
    {
        b->DynamicUpdate();
        b = b->mRight;
    } while (b != m_ActiveRB_Anchor);
}

} }
