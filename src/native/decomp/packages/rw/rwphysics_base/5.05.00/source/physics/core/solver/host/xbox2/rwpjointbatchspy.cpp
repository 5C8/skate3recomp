// rw::physics "Joint Spies" stage: report the solved impulses of flagged constraints, compacting
// the records in place over the jacobian array (every input is read before its record is
// written). sk83_na_m SpyJointJacobians 0x82cdd380 (0x15c).
#ifdef HORUS_NATIVE
#ifdef HORUS_NATIVE_FAST
#include "fast_shim.h"
#else
#include "horus_native_shim.h"   // x86 equivalence build (tools/equiv)
#endif
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "rwpspy_types.h"
#include <math.h>

namespace rw { namespace physics {

// Joints with a spy (m_nb): force and torque in world space.
void Simulation::SpyJointJacobians()
{
    __vector4 freq = __vspltw(__lvlx(RWP_ADDR(m_Frequency), 0), 0);
    __vector4 freq2 = __vmulfp(freq, freq);
    RWP_PTR(JointJacobian) j = m_JJ_Stack;
    RWP_PTR(JointSpy) out = RWP_CAST(JointSpy, (RWP_PTR(JointJacobian))m_JJ_Stack);
    for (unsigned int i = 0; i < m_JT_Count; i++, j += 1)
    {
        if (j->m_nb == 0)
            continue;
        // PERM_BEGIN
        RWP_PTR(Joint) joint = j->m_joint;
        __vector4 force = SpyRows(LOADV(j->m_L_ri), LOADV(j->m_L_up), LOADV(j->m_L_at), LOADV(j->m_Pos_Imp));
        __vector4 torque = SpyRows(RWP_VEC3(j->m_T_ri), RWP_VEC3(j->m_T_up), RWP_VEC3(j->m_T_at), LOADV(j->m_Ang_Imp));
        out->m_force = __vmulfp(force, freq2);        out->m_torque = __vmulfp(torque, freq2);
        out->m_joint = joint;
        out->m_tag = joint->m_tag;
        m_JS_Count++;
        // PERM_END
        out += 1;
    }
}

} }
