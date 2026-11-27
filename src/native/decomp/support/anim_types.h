// AnimCmdSys data layouts (Skate 2 PDB names, ref/anim_types_sk82.h), in rwp_config.h types.
#ifndef RWP_ANIM_TYPES_H
#define RWP_ANIM_TYPES_H
#include "rwp_config.h"

namespace AnimCmdSys {

struct SQTDataInternal                  // size 0x30
{
    RWP_VEC mScale;                     // 0x00
    RWP_VEC mQuat;                      // 0x10
    RWP_VEC mTrans;                     // 0x20
};

struct BoneIndex { RWP_S32 v; };
struct BlendCoef { RWP_F32 v; };

namespace sqtBlendSQTs {
struct SQTOpData                        // size 0x40
{
    RWP_FIELD_PTR(SQTDataInternal) SQTInput1;       // 0x00
    RWP_FIELD_PTR(SQTDataInternal) SQTInput2;       // 0x04
    RWP_FIELD_PTR(SQTDataInternal) SQTOutput;       // 0x08
    RWP_FIELD_PTR(SQTDataInternal) extraOutput;     // 0x0c
    RWP_FIELD_PTR(SQTDataInternal) NBlendSQTPtrs[6];// 0x10
    RWP_FIELD_PTR(BlendCoef) NBlendCoefList;        // 0x28
    RWP_F32 alpha;                                  // 0x2c
#if defined(HORUS_NATIVE)
    RWP_U16 ArbitraryInt;
    RWP_U8 ArbitraryShort;
    RWP_U8 cmdtype;
#else
    RWP_U8 cmdtype;                                 // 0x30
    RWP_U8 ArbitraryShort;                          // 0x31
    RWP_U16 ArbitraryInt;                           // 0x32
#endif
    RWP_U32 pad[3];             // 0x34 (size 0x40)
};
struct SQTJobData                       // size 0x10
{
    RWP_U32 numSQTOps;                              // 0x00
#if defined(HORUS_NATIVE)   // host tests keep 32-bit words little-endian: halves swap places
    RWP_U16 TrajNode;
    RWP_U16 numBones;
#else
    RWP_U16 numBones;                               // 0x04
    RWP_U16 TrajNode;                               // 0x06
#endif
    RWP_FIELD_PTR(BoneIndex) BoneMirrorList;        // 0x08
    RWP_FIELD_PTR(SQTOpData) jobData;               // 0x0c
};
}

}

#if !defined(_M_PPC)   // cl 15 (XDK) has no static_assert
static_assert(sizeof(AnimCmdSys::sqtBlendSQTs::SQTOpData) == 0x40, "SQTOpData");
static_assert(sizeof(AnimCmdSys::SQTDataInternal) == 0x30, "SQTDataInternal");
#endif
#endif
