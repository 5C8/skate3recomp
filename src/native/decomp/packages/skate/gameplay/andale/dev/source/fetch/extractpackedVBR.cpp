// Andale::Vector_ExtractPackedNVBR (sk83_na_m 0x82feb8b0, 0x8cc) and Andale::TrajectoryCalculation
// (0x82feaeb8, 0x17c): the animation fetch job (packages/skate/gameplay/andale/dev/source/fetch/extractpackedVBR.cpp;
// TrajectoryCalculation is in include/common/andale/fetch/extractpacked.h).
// Hand-decompiled from the sk83 code with Skate 2's PDB types/names and its Hex-Rays output as the
// guide.
//
// For each ExtractType record: fill the record's SQT range with identity bones (when it starts
// at bone 0), let the VBR codec decode the part's frame into place -- one frame, or two
// consecutive frames (the first into a stack scratch buffer) blended by currblend / 65535 (scale
// and translation lerp, quaternion nlerp) -- and set every bone's trans.w to the part's channel
// coefficient (channel animations) or 1. doTraj records also decode the previous frame pair of
// the trajectory bone and make bone 0 relative to it (TrajectoryCalculation). Every record's
// buffer is then copied to extraOut when set.
//
// The VBR codec (InitPerAnim, DecompressFrame, DecompressTwoConsecFrames) is not part of this
// file: XDK builds call it, the guest build (RWP_GUEST, the recomps) calls the recompiled guest
// functions through Andale::g_codec on the caller's PPCContext, with the same guest stack
// frame layout as the original (prev SQT at +0xd0, the scratch frame at +0x100). There is no
// host (HORUS_NATIVE) build: the test is guest-only (tools/equiv --hand extract).
//
// Floats copied through the FPU (lfs/stfs: channel coefficients, deltaT) are copied bit for bit,
// as the 360 does. The recomp runs its FPU with DAZ, so where clang does not fold an lfs/stfs pair
// it turns a denormal into a signed zero; tools/equiv counts those words as "DAZ-only".
#if defined(HORUS_NATIVE)
#error "extract_vbr.cpp is guest-only: the VBR codec it calls exists only as recompiled code"
#elif !defined(RWP_GUEST)
#include <rw/math/vpu.h>
#endif
#include "andale_types.h"
#include "../../include/common/andale/fetch/extractpacked.h"   // TrajectoryCalculation
#include <string.h>
#if !defined(RWP_GUEST)
extern "C" unsigned long __stdcall GetCurrentProcessorNumber(void);
extern "C" void *__stdcall XMemCpy(void *dst, const void *src, unsigned long size);
#endif

namespace Andale {
#ifdef RWP_GUEST
using namespace ::rwp;      // the recompiler's vector lowering

typedef void (*GuestFn)(PPCContext &, uint8_t *);
struct GuestCodec
{
    GuestFn initPerAnim;        // ANIM_CODEC::VBRDecompressor::InitPerAnim 0x831cddd8
    GuestFn decompressFrame;    // ::DecompressFrame 0x831ce3d8
    GuestFn twoConsecFrames;    // ::DecompressTwoConsecFrames 0x831ce528
    GuestFn processorNumber;    // GetCurrentProcessorNumber 0x82daf5d8
};
extern GuestCodec g_codec;
extern thread_local PPCContext *g_ctx;    // set by the caller (hook / test) before every call

static __forceinline u32 GuestCall(GuestFn fn, u32 r3, u32 r4 = 0, u32 r5 = 0, u32 r6 = 0, u32 r7 = 0,
                                   u32 r8 = 0, u32 r9 = 0)
{
    PPCContext &c = *g_ctx;
    c.r3.u64 = r3; c.r4.u64 = r4; c.r5.u64 = r5; c.r6.u64 = r6; c.r7.u64 = r7; c.r8.u64 = r8; c.r9.u64 = r9;
    fn(c, g_base);
    return c.r3.u32;
}
#endif

static const float kBlendScale = 1.0f / 65535.0f;   // 0x821e8490

// v / |v| (4D), two Newton steps on the reciprocal square root
static __forceinline __vector4 AnNormalize(__vector4 v, __vector4 half, __vector4 one)
{
    __vector4 d = __vmsum4fp(v, v);
    __vector4 e = __vrsqrtefp(d);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
    e = __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
    return __vmulfp(v, e);
}

// identity bones (scale 1, rotation (0,0,0,1), translation (0,0,0,w)) over [p, end), four bones
// per 192 bytes with the two cache lines ahead zeroed first -- like the original, the second
// line can reach up to 64 bytes past the end
static __forceinline void InitSQTs(SqtPtr p, SqtPtr end, __vector4 one, __vector4 quat, __vector4 trans)
{
    unsigned int z = 0x7f;
    for (; RWP_ADDR_OF(p) + 4 * sizeof(SQTData) <= RWP_ADDR_OF(end); p += 4)
    {
        __dcbz128(z, &p[0]);
        __dcbz128(z + 0x80, &p[0]);
        for (int k = 0; k < 4; k++)
        {
            STOREV(p[k].mScale, one);
            STOREV(p[k].mQuat, quat);
            STOREV(p[k].mTrans, trans);
        }
        z = 0x80;       // (the first block starts at +0x7f, the later ones at +0x80)
    }
    switch ((int)(RWP_ADDR_OF(end) - RWP_ADDR_OF(p)) / (int)sizeof(SQTData))
    {
    case 3:
        STOREV(p[2].mScale, one); STOREV(p[2].mQuat, quat); STOREV(p[2].mTrans, trans);
    case 2:
        STOREV(p[1].mScale, one); STOREV(p[1].mQuat, quat); STOREV(p[1].mTrans, trans);
    case 1:
        STOREV(p[0].mScale, one); STOREV(p[0].mQuat, quat); STOREV(p[0].mTrans, trans);
    }
}

// the frame block both frames are decoded from: f1's when the pair straddles a block boundary
static __forceinline unsigned int FrameBlock(unsigned int f0, unsigned int f1)
{
    unsigned int blk = f0 >> 3;
    if (blk != f1 >> 3 && ((f0 & 7) == 7 || (f1 & 7) == 0))
        blk = f1 >> 3;
    return blk;     // (otherwise the original asserts, extractpackedVBR line 76, and keeps f0's)
}

void Vector_ExtractPackedNVBR(RWP_PTR(ExtractType) ExtractBuffer, unsigned int ExtractBuffSize,
                              RWP_PTR(ANIM_CODEC::VBRDecompressor) decompressor)
{
    __dcbt(0, ExtractBuffer);
    const __vector4 zero = __vspltisw(0);
    const __vector4 one = __vcfux(__vspltisw(1), 0);
    const __vector4 quatIdentity = __vsldoi(zero, one, 4);
#if defined(RWP_GUEST)
    const unsigned char tid = (unsigned char)GuestCall(g_codec.processorNumber, 0);
    const u32 sp = g_ctx->r1.u32, frame = sp - 0x1ff0;     // the original's stack frame
    g_ctx->r1.u64 = frame;
    SqtPtr prevSQT = RWP_FROM_ADDR(SQTData, frame + 0xd0);
    SqtPtr scratch = RWP_FROM_ADDR(SQTData, frame + 0x100);
#else
    const unsigned char tid = (unsigned char)GetCurrentProcessorNumber();
    struct { SQTData prev; SQTData scratch[79]; } stackSQT;
    SqtPtr prevSQT = &stackSQT.prev;
    SqtPtr scratch = stackSQT.scratch;
#endif
    for (unsigned int i = 0; i < ExtractBuffSize; i++)
    {
        RWP_PTR(ExtractType) ex = ExtractBuffer + i;
        RWP_PTR(ExtractType) next = ExtractBuffer + (i + 1 < ExtractBuffSize ? i + 1 : ExtractBuffSize - 1);
        const unsigned int copySize = ex->sqtSize * (unsigned int)sizeof(SQTData);
        SqtPtr sqtBuff = ex->sqtBuff;
        // trans.w of the identity bones: 0 for channel animations (bit 0, as rw::math makes the
        // select mask from the u16), else 1
        const __vector4 transInit = __vsldoi(zero, (ex->channelAnim & 1) ? zero : one, 4);
        RWP_PTR(const AnimationPart) header = ex->header;
        SqtPtr out = sqtBuff + ex->sqtOffset;
        if (header)
        {
            __dcbt(0, header);
            __dcbt(0, next->header);
            if (ex->sqtOffset == 0)
                InitSQTs(sqtBuff, sqtBuff + ex->sqtSize, one, quatIdentity, transInit);

            const bool doTraj = ex->doTraj != 0;
            float blend = (float)ex->currblend * kBlendScale;
            unsigned int f0 = ex->data[0], f1 = ex->data[1];
            BytePtr part = RWP_CAST(const unsigned char, header);
#if defined(RWP_GUEST)
            const u32 initOff = GuestCall(g_codec.initPerAnim, RWP_ADDR_OF(decompressor),
                                          RWP_ADDR_OF(part) + header->mCompressedData,
                                          RWP_ADDR_OF(part) + header->mCompressionHeader, tid);
#else
            const unsigned int initOff = decompressor->InitPerAnim(part + header->mCompressedData,
                                                                   part + header->mCompressionHeader, tid);
#endif
            RWP_PTR(const RWP_F32) chanCoef = RWP_CAST(const RWP_F32, part + 0x10);   // header->mChannelCoef
            unsigned int numBones = ((header->mHeader >> 6) & 0x3f) + 1;
            SqtPtr dst = out;
            bool prevDone = false;
            for (;;)
            {
                // skip the frame blocks before the one to decode
                unsigned int blk = FrameBlock(f0, f1);
                unsigned short skip = 0;
                if (blk)
                {
                    RWP_FIELD_PTR(RWP_U16) sizes = decompressor->mWorkBuffers[tid].mhdr->mFrameBlockSizes;
                    for (unsigned int k = 0; k < blk; k++)
                        skip = (unsigned short)(skip + sizes[k]);
                }
                BytePtr data = part + (header->mCompressedData + initOff + skip);
                if (f0 == f1)
                {
#if defined(RWP_GUEST)
                    GuestCall(g_codec.decompressFrame, RWP_ADDR_OF(decompressor), RWP_ADDR_OF(data), f1,
                              RWP_ADDR_OF(dst), tid);
#else
                    decompressor->DecompressFrame(data, f1, RWP_ADDR_OF(dst), tid);
#endif
                    if (ex->channelAnim)
                        for (unsigned int b = 0; b < numBones; b++)
                        {
                            __vector4 t = LOADV(dst[b].mTrans);
                            VLANE_SETW(t, (float)chanCoef[b]);
                            STOREV(dst[b].mTrans, t);
                        }
                    else
                        for (unsigned int b = 0; b < numBones; b++)
                        {
                            __vector4 t = LOADV(dst[b].mTrans);
                            VLANE_SETW(t, 1.0f);
                            STOREV(dst[b].mTrans, t);
                        }
                }
                else
                {
#if defined(RWP_GUEST)
                    GuestCall(g_codec.twoConsecFrames, RWP_ADDR_OF(decompressor), RWP_ADDR_OF(data), f0, f1,
                              RWP_ADDR_OF(scratch), RWP_ADDR_OF(dst), tid);
#else
                    decompressor->DecompressTwoConsecFrames(data, f0, f1, RWP_ADDR_OF(scratch), RWP_ADDR_OF(dst), tid);
#endif
                    const bool channel = ex->channelAnim != 0;
                    const __vector4 w = VSPLAT_LX(blend);
                    const __vector4 half = __vcfsx(__vspltisw(1), 1), oneF = __vcfsx(__vspltisw(1), 0);
                    for (unsigned int b = 0; b < numBones; b++)
                    {
                        // frame 0 (scratch) towards frame 1 (in place)
                        __vector4 s0 = AnV3(LOADV(scratch[b].mScale));
                        STOREV(dst[b].mScale, __vmaddfp(__vsubfp(AnV3(LOADV(dst[b].mScale)), s0), w, s0));
                        __vector4 q1 = LOADV(scratch[b].mQuat), q2 = LOADV(dst[b].mQuat);
                        __vector4 shorter = __vcmpgtfp(__vmsum4fp(q1, q2), zero);
                        __vector4 toward = __vmaddfp(__vsubfp(q2, q1), w, q1);
                        __vector4 away = __vnmsubfp(__vaddfp(q2, q1), w, q1);
                        STOREV(dst[b].mQuat, AnNormalize(__vsel(away, toward, shorter), half, oneF));
                        __vector4 t0 = LOADV(scratch[b].mTrans);
                        __vector4 t = __vmaddfp(__vsubfp(LOADV(dst[b].mTrans), t0), w, t0);
                        VLANE_SETW(t, channel ? (float)chanCoef[b] : 1.0f);
                        STOREV(dst[b].mTrans, t);
                    }
                }
                if (!doTraj)
                    break;
                if (!prevDone)
                {
                    // again for the trajectory bone at the previous frame pair, into prevSQT
                    prevDone = true;
                    blend = (float)ex->prevblend * kBlendScale;
                    f0 = ex->prev[0];
                    f1 = ex->prev[1];
                    dst = prevSQT;
                    numBones = 1;
                    continue;
                }
                __vector4 deltaT = RWP_VEC3F(ex->deltaT[0], ex->deltaT[1], ex->deltaT[2]);
                TrajectoryCalculation(sqtBuff, ex->channelAnim != 0, (float)chanCoef[0], LOADV(prevSQT->mQuat),
                                      AnV3(LOADV(prevSQT->mTrans)), LOADV(ex->deltaQ), deltaT);
                break;
            }
        }
        SqtPtr extraOut = ex->extraOut;
        if (extraOut)
        {
#if defined(RWP_GUEST)
            memcpy(g_base + RWP_ADDR_OF(extraOut), g_base + RWP_ADDR_OF(sqtBuff), copySize);
#else
            XMemCpy(extraOut, sqtBuff, copySize);
#endif
        }
    }
#if defined(RWP_GUEST)
    g_ctx->r1.u64 = sp;
#endif
}

}
