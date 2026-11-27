// Andale fetch + ANIM_CODEC VBR layouts (Skate 2 PDB names, ref/anim_types_sk82.h), in rwp_config.h types.
#ifndef RWP_ANDALE_TYPES_H
#define RWP_ANDALE_TYPES_H
#include "rwp_config.h"

namespace ANIM_CODEC {

struct VBRDataHeader                    // size 0x14
{
    RWP_U32 mData;                                  // 0x00 VBRDataHeaderValue*
    RWP_U32 mConstantPaletteData;                   // 0x04
    RWP_U32 mChannelTypeInfo;                       // 0x08
    RWP_FIELD_PTR(RWP_U16) mFrameBlockSizes;        // 0x0c
    RWP_U32 mLoadedFromMemory;                      // 0x10
};

struct DataPerDecompress                // size 0x40 (only the header pointer is read here)
{
    RWP_FIELD_PTR(VBRDataHeader) mhdr;              // 0x00
    RWP_U32 rest[15];
};

#if defined(RWP_GUEST)
struct VBRDecompressor                  // size 0x55d0; the codec itself stays recompiled guest code
{
    u8 pad[0x2430];
    DataPerDecompress mWorkBuffers[6];              // 0x2430
};
#else
class VBRDecompressor
{
public:
    unsigned int InitPerAnim(const unsigned char *data, const unsigned char *header, unsigned char threadId);
    void DecompressFrame(const unsigned char *data, unsigned int frame, unsigned int putLoc, unsigned char threadId);
    void DecompressTwoConsecFrames(const unsigned char *data, unsigned int frame0, unsigned int frame1,
                                   unsigned int putLoc0, unsigned int putLoc1, unsigned char threadId);
    unsigned char pad[0x2430];
    DataPerDecompress mWorkBuffers[6];              // 0x2430
};
#endif

}

namespace Andale {

struct SQTData                          // size 0x30
{
    RWP_VEC mScale;                                 // 0x00
    RWP_VEC mQuat;                                  // 0x10
    RWP_VEC mTrans;                                 // 0x20
};

// AnimationPart: offsets relative to the part itself; the VBR header (+0x10) starts with the
// per-joint channel coefficients the fetch copies into trans.w
struct AnimationPart
{
    RWP_U32 mHeader;                                // 0x00 bits 6..11: bones - 1
    RWP_U32 mDataSize;                              // 0x04
    RWP_U32 mCompressionHeader;                     // 0x08
    RWP_U32 mCompressedData;                        // 0x0c
    RWP_F32 mChannelCoef[1];                        // 0x10
};

struct ExtractType                      // size 0x40
{
    RWP_VEC deltaQ;                                 // 0x00
    RWP_F32 deltaT[3];                              // 0x10
    RWP_FIELD_PTR(const AnimationPart) header;      // 0x1c
    RWP_FIELD_PTR(SQTData) sqtBuff;                 // 0x20
    RWP_FIELD_PTR(SQTData) extraOut;                // 0x24
    RWP_U8 codecType;                               // 0x28
    RWP_U8 doTraj;                                  // 0x29
    RWP_U16 channelAnim;                            // 0x2a
    RWP_U16 headersize;                             // 0x2c
    RWP_U16 currblend;                              // 0x2e
    RWP_U16 prevblend;                              // 0x30
    RWP_U16 totalBones;                             // 0x32
    RWP_U16 data[2];                                // 0x34
    RWP_U16 prev[2];                                // 0x38
    RWP_U16 sqtSize;                                // 0x3c
    RWP_U16 sqtOffset;                              // 0x3e
};

}

#if !defined(_M_PPC)
static_assert(sizeof(Andale::ExtractType) == 0x40, "ExtractType");
static_assert(sizeof(Andale::SQTData) == 0x30, "SQTData");
#endif
#endif
