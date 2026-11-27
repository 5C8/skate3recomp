// rw::physics "Create Simulation Islands" stage: Simulation::CreateSimulationIslandTables.
// sk83_na_m 0x82cdd4e0 (0x5f0, leaf). Job: SimulationManager entry 0x830ac400.
//
// Sorts the active bodies, contacts, joints and drives into the per-island lists that the
// island solver (HorusPipelineIslandUsingIndirectJacobians) walks, and lays out each island's
// jacobian stack. A constraint goes to the island of its first body, or of the second when the
// first has none (-1). Without an island map every body is in island 0.
// The per-island lists (m_islandXxx[k]) are arrays of 32-bit pointers.
#include "simulation.h"

namespace rw { namespace physics {

static __forceinline signed char IslandOf(RWP_PTR(RigidBodySimulationIslandMap) map, RWP_PTR(RigidBody) body)
{
    if (map)
        return map->m_islands[body - (RWP_PTR(RigidBody))map->m_simulation->m_RB_Stack];
    return 0;
}

// append item (and its jacobian) to island isl's lists (the member arrays, indexed off this)
#define ISLAND_PUSH(counts, lists, jacLists, isl, item, jacobian) \
    do { \
        unsigned int n_ = counts[isl]; \
        counts[isl] = n_ + 1; \
        RWP_LIST_SET(lists[isl], n_, item); \
        RWP_LIST_SET(jacLists[isl], n_, jacobian); \
    } while (0)

void Simulation::CreateSimulationIslandTables(unsigned int swapContactStacks)
{
    // bodies
    RWP_PTR(RigidBody) b = m_ActiveRB_Anchor->mRight;
    if (b != m_ActiveRB_Anchor)
    {
        do
        {
            int isl = IslandOf(m_RB_Islands, b);
            if (isl >= 0)
            {
                RWP_LIST_SET(m_islandRigidBodies[isl], m_islandRigidBodiesCount[isl], RWP_ADDR_OF(b));
                m_islandRigidBodiesCount[isl]++;
            }
            b = b->mRight;
        } while (b != m_ActiveRB_Anchor);
    }

    // contacts: their jacobians go to the second stack when the stacks will be swapped
    unsigned int jacBase = RWP_ADDR_OF((RWP_PTR(ContactJacobian))m_CJ_Stack_Islands);
    if (!swapContactStacks)
        jacBase = RWP_ADDR_OF((RWP_PTR(ContactJacobian))m_CJ_Stack);
    for (unsigned int i = 0; i < m_CT_Count; i++)
    {
        // PERM_BEGIN
        RWP_PTR(ContactJacobian) c = (RWP_PTR(ContactJacobian))m_CJ_Stack + i;
        RWP_PTR(RigidBody) rbB = RWP_FROM_ADDR(RigidBody, RWP_ADDR_OF((RWP_PTR(ReactionForces))c->m_ib));
        signed char isA = IslandOf(m_RB_Islands, RWP_FROM_ADDR(RigidBody, RWP_ADDR_OF((RWP_PTR(ReactionForces))c->m_ia)));
        signed char isB = IslandOf(m_RB_Islands, rbB);
        if (isA != -1)
            isB = isA;
        ISLAND_PUSH(m_islandContactsCount, m_islandContacts, m_islandContactJacobians, isB,
                   RWP_ADDR_OF((RWP_PTR(ContactJacobian))m_CJ_Stack + i), jacBase + i * sizeof(ContactJacobian));
        // PERM_END
    }

    // joints and drives with an active body (their island: body B's, else body A's)
    unsigned int nJoints = 0;
    for (RWP_PTR(Joint) j = m_ActiveJT_Anchor->m_right; j != m_ActiveJT_Anchor; j = j->m_right)
    {
        RWP_PTR(RigidBody) bodyB = j->m_bodyB;
        RWP_PTR(RigidBody) bodyA = j->m_bodyA;
        if (!((bodyA->mState | bodyB->mState) & ACTIVE_BODY))
            continue;
        signed char is1 = IslandOf(m_RB_Islands, bodyB);
        signed char is2 = IslandOf(m_RB_Islands, bodyA);
        if (is1 != -1)
            is2 = is1;
        ISLAND_PUSH(m_islandJointsCount, m_islandJoints, m_islandJointJacobians, is2,
                   RWP_ADDR_OF(j), RWP_ADDR_OF((RWP_PTR(JointJacobian))m_JJ_Stack + nJoints));
        nJoints++;
    }
    m_JT_Count = nJoints;

    unsigned int nDrives = 0;
    for (RWP_PTR(Drive) d = m_ActiveDR_Anchor->m_right; d != m_ActiveDR_Anchor; d = d->m_right)
    {
        RWP_PTR(RigidBody) bodyB = d->m_bodyB;
        RWP_PTR(RigidBody) bodyA = d->m_bodyA;
        if (!((bodyA->mState | bodyB->mState) & ACTIVE_BODY))
            continue;
        signed char is1 = IslandOf(m_RB_Islands, bodyB);
        signed char is2 = IslandOf(m_RB_Islands, bodyA);
        if (is1 != -1)
            is2 = is1;
        ISLAND_PUSH(m_islandDrivesCount, m_islandDrives, m_islandDriveJacobians, is2,
                   RWP_ADDR_OF(d), RWP_ADDR_OF((RWP_PTR(DriveJacobian))m_DJ_Stack + nDrives));
        nDrives++;
    }
    m_DR_Count = nDrives;

    // per-island jacobian stacks: prefix sums of the counts
    RWP_PTR(DriveJacobian) dj = m_DJ_Stack;
    RWP_PTR(ContactJacobian) cj = m_CJ_Stack_Islands;
    RWP_PTR(JointJacobian) jj = m_JJ_Stack;
    for (int k = 0; k < 10; k++)
    {
        m_islandCJStack[k] = RWP_ADDR_OF(cj);
        m_islandJJStack[k] = RWP_ADDR_OF(jj);
        m_islandDJStack[k] = RWP_ADDR_OF(dj);
        cj += m_islandContactsCount[k];
        jj += m_islandJointsCount[k];
        dj += m_islandDrivesCount[k];
    }

    if (swapContactStacks)
    {
        RWP_PTR(ContactJacobian) t = m_CJ_Stack;
        m_CJ_Stack = m_CJ_Stack_Islands;
        m_CJ_Stack_Islands = t;
    }
}

} }
