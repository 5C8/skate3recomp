// rw::physics "Contact Spies" stage: report the solved impulses of flagged constraints, compacting
// the records in place over the jacobian array (every input is read before its record is
// written). sk83_na_m SpyContactJacobians 0x82cde1f0 (0x170).
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

// Contacts with the spy flag (m_nb bit 3) and a positive normal impulse.
void Simulation::SpyContactJacobians()
{
    __vector4 freq = __vspltw(__lvlx(RWP_ADDR(m_Frequency), 0), 0);
    freq = __vmulfp(freq, freq);
    RWP_PTR(ContactJacobian) c = m_CJ_Stack;
    RWP_PTR(ContactSpy) out = RWP_CAST(ContactSpy, (RWP_PTR(ContactJacobian))m_CJ_Stack);   // a second read of the stack
    for (unsigned int i = 0; i < m_CT_Count; i++, c += 1)
    {
        if (!(c->m_nb & 8))
            continue;
        __vector4 imp = LOADV(c->m_Pos_Imp);
        if (!(VLANE_F(imp, 0) > 0.0f))
            continue;
        // PERM_BEGIN
        float ima = c->m_ima;
        __vector4 ri = LOADV(c->m_C_ri);
        __vector4 at = LOADV(c->m_C_at);
        float imb = c->m_imb;
        RWP_PTR(RigidBody) bodyB = c->m_bodyb;
        RWP_PTR(RigidBody) bodyA = c->m_bodya;
        __vector4 up = LOADV(c->m_C_up);
        __vector4 fric = __vmulfp(at, __vspltw(imp, 2));
        __vector4 posB = __vaddfp(LOADV(bodyB->mCom), LOADV(c->m_rb));
        fric = __vmaddfp(up, __vspltw(imp, 1), fric);
        __vector4 vima = VSPLAT_LX(ima);
        posB = __vmulfp(posB, VSPLAT_LX(imb));
        fric = __vmulfp(fric, freq);
        unsigned int tag = c->m_tag;
        __vector4 posA = __vaddfp(LOADV(bodyA->mCom), LOADV(c->m_ra));
        __vector4 nimp = __vmulfp(ri, __vspltw(imp, 0));
        float sum = imb + ima;
        __vector4 pos = __vmaddfp(posA, vima, posB);
        out->mBodyA = bodyA;
        out->mBodyB = bodyB;
        out->mTag = tag;
        float inv = 1.0f / sum;
        out->mNimp = __vmulfp(nimp, freq);
        out->mFric = fric;
        m_CS_Count++;
        out->mFrame.xAxis = ri;
        out->mFrame.yAxis = up;
        pos = __vmulfp(pos, VSPLAT_LX(inv));
        out->mFrame.zAxis = at;
        out->mFrame.wAxis = pos;
        // PERM_END
        out += 1;
    }
}

} }
