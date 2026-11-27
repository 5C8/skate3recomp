// rw::physics (rwphysics_base 5.05, rwpcore) data layouts, from the Skate 2 PDB
// (ref/rwphysics_types_sk82.h) and checked against sk83_na_m code. Field types come from
// rwp_config.h so one source builds for the XDK, the host tests and guest memory.
// Where sk83 differs from the Skate 2 PDB it is noted.
#pragma once
#include "rwp_config.h"

#if defined(RWP_GUEST)
#define RWP_VOLATILE
#else
#define RWP_VOLATILE volatile
#endif

namespace rw { namespace physics {

class Simulation;
class RigidBody;
class Joint;
class Drive;
class ContactJacobian;
class JointJacobian;
class DriveJacobian;

enum BodyState { FREE_BODY = 0, STATIC_BODY = 1, FROZEN_BODY = 2, ACTIVE_BODY = 4, STATE_FILTER = 7, SPY_BODY = 8 };
enum SwingType { SWING_LOCKED = 0, SWING_CONE = 1, SWING_HINGE = 2, SWING_AXLE = 3, SWING_FREE = 4 };
enum TwistType { TWIST_LOCKED = 0, TWIST_ARC = 1, TWIST_FREE = 2 };

// The solver's per-body velocity block (sk83: RigidBody +0x1c points here; the Skate 2 PDB
// calls that slot mId). The Horus iterations accumulate into it, DynamicUpdate consumes it.
struct ReactionForces                   // size 0x40
{
    RWP_VEC m_plin;                     // 0x00 linear velocity correction
    RWP_VEC m_vlin;                     // 0x10 linear position correction
    RWP_VEC m_pang;                     // 0x20 angular velocity correction
    RWP_VEC m_vang;                     // 0x30 angular position correction
};

class Inertia                           // size 0x30
{
public:
    RWP_VEC mInvTens;                   // 0x00
    RWP_F32 mInvMass;                   // 0x10
    RWP_F32 mSpherical;                 // 0x14
    RWP_F32 mMaxVelocity;               // 0x18
    RWP_F32 mMaxOmega;                  // 0x1c
    RWP_F32 mLinearDrag;                // 0x20
    RWP_F32 mAngularDrag;               // 0x24
    RWP_U32 m_pad28[2];
};

class RigidBody                         // size 0xb0
{
public:
    void DynamicUpdate();

    RWP_VEC mQuat;                      // 0x00
    RWP_V3 mCom;                        // 0x10
    RWP_FIELD_PTR(ReactionForces) mRF;  // 0x1c (PDB: mId)
    RWP_V3 mVel;                        // 0x20
    RWP_FIELD_PTR(RigidBody) mRight;    // 0x2c next in the state list
    RWP_V3 mOmega;                      // 0x30
    RWP_FIELD_PTR(RigidBody) mLeft;     // 0x3c
    RWP_V3 mRi;                         // 0x40 orientation rows
    RWP_FIELD_PTR(Simulation) mStasis;  // 0x4c
    RWP_V3 mUp;                         // 0x50
    RWP_FIELD_PTR(Inertia) mInertia;    // 0x5c
    RWP_V3 mAt;                         // 0x60
    RWP_U32 mTag;                       // 0x6c
    RWP_V3 mIfull;                      // 0x70 world inverse inertia, diagonal
    RWP_F32 mInvm;                      // 0x7c
    RWP_V3 mIsplt;                      // 0x80 world inverse inertia, off-diagonal
    RWP_S32 mState;                     // 0x8c BodyState
    RWP_V3 mForce;                      // 0x90
    RWP_F32 mKine;                      // 0x9c
    RWP_V3 mTorque;                     // 0xa0
    RWP_U32 mCool;                      // 0xac
};

struct RigidBodySimulationIslandMap     // size 0x8
{
    RWP_FIELD_PTR(Simulation) m_simulation;
    RWP_FIELD_PTR(signed char) m_islands;
};

class JointFrames                       // size 0x50
{
public:
    RWP_VEC mQuatA;                     // 0x00
    RWP_VEC mPosA;                      // 0x10
    RWP_VEC mQuatB;                     // 0x20
    RWP_VEC mPosB;                      // 0x30
    RWP_VEC mQuatL;                     // 0x40
};

class JointLimits                       // size 0x40
{
public:
    RWP_VEC mPprism;                    // 0x00
    RWP_VEC mVprism;                    // 0x10
    RWP_F32 mVtwist;                    // 0x20
    RWP_F32 mVswing;                    // 0x24
    RWP_F32 mSwinga;                    // 0x28
    RWP_F32 mTwista;                    // 0x2c
    RWP_F32 mSwingc;                    // 0x30
    RWP_F32 mTwistc;                    // 0x34
    RWP_S32 mSwingf;                    // 0x38 SwingType
    RWP_S32 mTwistf;                    // 0x3c TwistType
};

class Joint                             // size 0x20
{
public:
    RWP_FIELD_PTR(JointFrames) m_skel;  // 0x00
    RWP_FIELD_PTR(JointLimits) m_limit; // 0x04
    RWP_FIELD_PTR(Joint) m_right;       // 0x08 next in the active list
    RWP_FIELD_PTR(Joint) m_left;        // 0x0c
    RWP_FIELD_PTR(RigidBody) m_bodyA;   // 0x10
    RWP_FIELD_PTR(RigidBody) m_bodyB;   // 0x14
    RWP_U32 m_tag;                      // 0x18
    RWP_U32 m_spy;                      // 0x1c
};

class DriveFrames                       // size 0x40
{
public:
    RWP_VEC mQuatA;                     // 0x00
    RWP_VEC mPosA;                      // 0x10
    RWP_VEC mQuatB;                     // 0x20
    RWP_VEC mPosB;                      // 0x30
};

enum DriveType { DRIVE_NONE = 0, DRIVE_SPRING = 1, DRIVE_STRENGTH = 2 };   // values as sk83 tests them

class DriveDynamics                     // size 0x20
{
public:
    struct Params                       // size 0x10
    {
        RWP_F32 mSpring;
        RWP_F32 mDamping;
        RWP_F32 mStrength;
        RWP_S32 mType;                  // DriveType
    };
    Params mLinear;                     // 0x00
    Params mAngular;                    // 0x10
};

class Drive                             // size 0x20
{
public:
    RWP_FIELD_PTR(DriveFrames) m_skel;  // 0x00
    RWP_FIELD_PTR(DriveDynamics) m_crtl;// 0x04
    RWP_FIELD_PTR(Drive) m_right;       // 0x08
    RWP_FIELD_PTR(Drive) m_left;        // 0x0c
    RWP_FIELD_PTR(RigidBody) m_bodyA;   // 0x10
    RWP_FIELD_PTR(RigidBody) m_bodyB;   // 0x14
    RWP_U32 m_tag;                      // 0x18
    RWP_U32 m_spy;                      // 0x1c
};

// d(q1*q2)/dq rows (PDB names)
class Jacobian_RQD                      // size 0x40
{
public:
    void Create(const __vector4 &q1, const __vector4 &q2);
    RWP_VEC Jwi;                        // 0x00
    RWP_VEC Jwj;                        // 0x10
    RWP_VEC Jwk;                        // 0x20
    RWP_VEC Jwr;                        // 0x30
};

class ContactJacobian                   // size 0x100
{
public:
    RWP_V3 m_ra;              RWP_FIELD_PTR(ReactionForces) m_ia;   // 0x00
    RWP_V3 m_rb;              RWP_FIELD_PTR(ReactionForces) m_ib;   // 0x10
    RWP_V3 m_CT_ri;           RWP_U32 m_nb;                         // 0x20 (m_nb: flags)
    RWP_V3 m_CT_up;           RWP_F32 m_mus;                        // 0x30
    RWP_V3 m_CT_at;           RWP_F32 m_mud;                        // 0x40
    RWP_V3 m_Pos_Imp;         RWP_F32 m_Pst_Imp;                    // 0x50
    RWP_V3 m_Pos_Lim;         RWP_F32 m_dist;                       // 0x60
    RWP_V3 m_C_ri;            RWP_FIELD_PTR(RigidBody) m_bodya;     // 0x70
    RWP_V3 m_IRAC_ri;         RWP_F32 m_ima;                        // 0x80
    RWP_V3 m_IRBC_ri;         RWP_F32 m_imb;                        // 0x90
    RWP_V3 m_C_up;            RWP_FIELD_PTR(RigidBody) m_bodyb;     // 0xa0
    RWP_V3 m_IRAC_up;         RWP_F32 m_pad0;                       // 0xb0
    RWP_V3 m_IRBC_up;         RWP_F32 m_pad1;                       // 0xc0
    RWP_V3 m_C_at;            RWP_U32 m_tag;                        // 0xd0
    RWP_V3 m_IRAC_at;         RWP_F32 m_pad2;                       // 0xe0
    RWP_V3 m_IRBC_at;         RWP_F32 m_pad3;                       // 0xf0
    // The 16-byte rows are read and written whole (LOADV / STOREV), w lane included.
};

// Joint and drive jacobians share the 0x180 frame (PDB names; drive names in comments).
class JointJacobian                     // size 0x180
{
public:
    void Build(const Joint &jtx, Simulation *stasis);
    RWP_V3 m_ra;      RWP_FIELD_PTR(ReactionForces) m_ia;           // 0x000
    RWP_V3 m_rb;      RWP_FIELD_PTR(ReactionForces) m_ib;           // 0x010
    RWP_V3 m_Pos_Imp; RWP_F32 m_pad0;     // 0x020 (drive: m_lindyn)
    RWP_V3 m_Ang_Imp; RWP_F32 m_pad1;     // 0x030 (drive: m_angdyn)
    RWP_V3 m_LT_ri;   RWP_U32 m_nb;       // 0x040
    RWP_V3 m_TT_ri;   RWP_F32 m_pad2;     // 0x050
    RWP_V3 m_LT_up;   RWP_F32 m_Ang_X_Max;// 0x060 (drive: m_Pos_Lim_X)
    RWP_V3 m_TT_up;   RWP_F32 m_Ang_Y_Max;// 0x070 (drive: m_Pos_Lim_Y)
    RWP_V3 m_LT_at;   RWP_F32 m_Ang_X_Min;// 0x080 (drive: m_Ang_Lim_X)
    RWP_V3 m_TT_at;   RWP_F32 m_Ang_Y_Min;// 0x090 (drive: m_Ang_Lim_Y)
    RWP_V3 m_Pos_Max; RWP_F32 m_Ang_Z_Max;// 0x0a0 (drive: m_Pos_Off, m_Pos_Lim_Z)
    RWP_V3 m_Pos_Min; RWP_F32 m_Ang_Z_Min;// 0x0b0 (drive: m_Ang_Off, m_Ang_Lim_Z)
    RWP_V3 m_L_ri;    RWP_FIELD_PTR(Joint) m_joint;                 // 0x0c0
    RWP_V3 m_L_up;    RWP_F32 m_pad3;     // 0x0d0
    RWP_V3 m_L_at;    RWP_F32 m_pad4;     // 0x0e0
    RWP_V3 m_T_ri;    RWP_F32 m_pad5;     // 0x0f0
    RWP_V3 m_T_up;    RWP_F32 m_pad6;     // 0x100
    RWP_V3 m_T_at;    RWP_F32 m_pad7;     // 0x110
    RWP_V3 m_InvInA_ri; RWP_F32 m_ima;    // 0x120
    RWP_V3 m_InvInA_up; RWP_F32 m_pad8;   // 0x130
    RWP_V3 m_InvInA_at; RWP_F32 m_pad9;   // 0x140
    RWP_V3 m_InvInB_ri; RWP_F32 m_imb;    // 0x150
    RWP_V3 m_InvInB_up; RWP_F32 m_pad10;  // 0x160
    RWP_V3 m_InvInB_at; RWP_F32 m_pad11;  // 0x170
};

class DriveJacobian                     // size 0x180 (same frame as JointJacobian; PDB names)
{
public:
    void Build(const Drive &drv, Simulation *stasis);
    RWP_V3 m_ra;      RWP_FIELD_PTR(ReactionForces) m_ia;           // 0x000
    RWP_V3 m_rb;      RWP_FIELD_PTR(ReactionForces) m_ib;           // 0x010
    RWP_V3 m_Pos_Imp; RWP_F32 m_lindyn;   // 0x020
    RWP_V3 m_Ang_Imp; RWP_F32 m_angdyn;   // 0x030
    RWP_V3 m_LT_ri;   RWP_U32 m_nb;       // 0x040
    RWP_V3 m_TT_ri;   RWP_F32 m_pad0;     // 0x050
    RWP_V3 m_LT_up;   RWP_F32 m_Pos_Lim_X;// 0x060
    RWP_V3 m_TT_up;   RWP_F32 m_Pos_Lim_Y;// 0x070
    RWP_V3 m_LT_at;   RWP_F32 m_Ang_Lim_X;// 0x080
    RWP_V3 m_TT_at;   RWP_F32 m_Ang_Lim_Y;// 0x090
    RWP_V3 m_Pos_Off; RWP_F32 m_Pos_Lim_Z;// 0x0a0
    RWP_V3 m_Ang_Off; RWP_F32 m_Ang_Lim_Z;// 0x0b0
    RWP_V3 m_L_ri;    RWP_FIELD_PTR(Drive) m_drive;                 // 0x0c0
    RWP_V3 m_L_up;    RWP_F32 m_pad1;     // 0x0d0
    RWP_V3 m_L_at;    RWP_F32 m_pad2;     // 0x0e0
    RWP_V3 m_T_ri;    RWP_F32 m_pad3;     // 0x0f0
    RWP_V3 m_T_up;    RWP_F32 m_pad4;     // 0x100
    RWP_V3 m_T_at;    RWP_F32 m_pad5;     // 0x110
    RWP_V3 m_InvInA_ri; RWP_F32 m_ima;    // 0x120
    RWP_V3 m_InvInA_up; RWP_F32 m_pad6;   // 0x130
    RWP_V3 m_InvInA_at; RWP_F32 m_pad7;   // 0x140
    RWP_V3 m_InvInB_ri; RWP_F32 m_imb;    // 0x150
    RWP_V3 m_InvInB_up; RWP_F32 m_pad8;   // 0x160
    RWP_V3 m_InvInB_at; RWP_F32 m_pad9;   // 0x170
};

class Simulation                        // size 0x300
{
public:
    void Horus_Pipeline();
    void HorusPipelineIslandUsingIndirectJacobians(unsigned int island);
    void BatchIntegrator();
    void ContactBatchBuild();
    void JointBatchBuild();
    void DriveBatchBuild();
    void SpyContactJacobians();
    void SpyJointJacobians();
    void SpyDriveJacobians();
    void CreateSimulationIslandTables(unsigned int swapContactStacks);

    RWP_FIELD_PTR(RigidBody) m_RB_Stack;        // 0x000
    RWP_FIELD_PTR(Joint) m_JT_Stack;            // 0x004
    RWP_FIELD_PTR(Drive) m_DR_Stack;            // 0x008
    RWP_U32 m_RF_Stack;                         // 0x00c
    RWP_FIELD_PTR(ContactJacobian) m_CJ_Stack;  // 0x010
    RWP_FIELD_PTR(JointJacobian) m_JJ_Stack;    // 0x014
    RWP_FIELD_PTR(DriveJacobian) m_DJ_Stack;    // 0x018
    RWP_FIELD_PTR(RigidBody) m_FreeRB_Anchor;   // 0x01c
    RWP_FIELD_PTR(RigidBody) m_StaticRB_Anchor; // 0x020
    RWP_FIELD_PTR(RigidBody) m_FrozenRB_Anchor; // 0x024
    RWP_FIELD_PTR(RigidBody) m_ActiveRB_Anchor; // 0x028
    RWP_FIELD_PTR(Joint) m_FreeJT_Anchor;       // 0x02c
    RWP_FIELD_PTR(Joint) m_ActiveJT_Anchor;     // 0x030
    RWP_FIELD_PTR(Drive) m_FreeDR_Anchor;       // 0x034
    RWP_FIELD_PTR(Drive) m_ActiveDR_Anchor;     // 0x038
    RWP_U32 m_FreeRB_Count;                     // 0x03c
    RWP_U32 m_StaticRB_Count;                   // 0x040
    RWP_U32 m_FrozenRB_Count;                   // 0x044
    RWP_U32 m_ActiveRB_Count;                   // 0x048
    RWP_U32 m_FreeJT_Count;                     // 0x04c
    RWP_U32 m_ActiveJT_Count;                   // 0x050
    RWP_U32 m_FreeDR_Count;                     // 0x054
    RWP_U32 m_ActiveDR_Count;                   // 0x058
    RWP_U32 m_JT_Stride;                        // 0x05c
    RWP_U32 m_DR_Stride;                        // 0x060
    RWP_VOLATILE RWP_U32 m_CT_Count;            // 0x064
    RWP_U32 m_CS_Count;                         // 0x068
    RWP_U32 m_JT_Count;                         // 0x06c
    RWP_U32 m_JS_Count;                         // 0x070
    RWP_U32 m_DR_Count;                         // 0x074
    RWP_U32 m_DS_Count;                         // 0x078
    RWP_U32 m_CT_Max;                           // 0x07c
    RWP_U32 m_JT_Max;                           // 0x080
    RWP_U32 m_DR_Max;                           // 0x084
    RWP_U32 m_RF_Max;                           // 0x088
    RWP_U32 m_pad8c;
    RWP_VEC m_Gravity;                          // 0x090
    RWP_F32 m_TimeStep;                         // 0x0a0
    RWP_F32 m_Frequency;                        // 0x0a4
    RWP_U32 m_CoolDown;                         // 0x0a8
    RWP_F32 m_MinEnergy;                        // 0x0ac
    RWP_U32 m_MaxIteration;                     // 0x0b0
    RWP_U32 m_SpyFlag;                          // 0x0b4
    RWP_U32 m_simulationIslandsEnabled;         // 0x0b8 (bool + padding)
    RWP_FIELD_PTR(RigidBodySimulationIslandMap) m_RB_Islands;   // 0x0bc
    RWP_U32 m_islandRigidBodiesCount[10];       // 0x0c0
    RWP_U32 m_islandContactsCount[10];          // 0x0e8
    RWP_U32 m_islandJointsCount[10];            // 0x110
    RWP_U32 m_islandDrivesCount[10];            // 0x138
    RWP_U32 m_islandRigidBodies[10];            // 0x160 (each: a list of RigidBody*)
    RWP_U32 m_islandContacts[10];               // 0x188 (ContactJacobian*)
    RWP_U32 m_islandJoints[10];                 // 0x1b0 (Joint*)
    RWP_U32 m_islandDrives[10];                 // 0x1d8 (Drive*)
    RWP_U32 m_islandContactJacobians[10];       // 0x200 (ContactJacobian*)
    RWP_U32 m_islandJointJacobians[10];         // 0x228 (JointJacobian*)
    RWP_U32 m_islandDriveJacobians[10];         // 0x250 (DriveJacobian*)
    RWP_U32 m_islandCJStack[10];                // 0x278
    RWP_U32 m_islandJJStack[10];                // 0x2a0
    RWP_U32 m_islandDJStack[10];                // 0x2c8
    RWP_FIELD_PTR(ContactJacobian) m_CJ_Stack_Islands;  // 0x2f0
    RWP_U32 m_pad2f4[3];
};

void HorusPipeline(unsigned int numContacts, RWP_PTR(ContactJacobian) contacts,
                   unsigned int numJoints, RWP_PTR(JointJacobian) joints,
                   unsigned int numDrives, RWP_PTR(DriveJacobian) drives,
                   unsigned int iterations);

} }
