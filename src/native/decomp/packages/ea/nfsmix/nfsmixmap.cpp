// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (9); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 8209660c 820aa9ec 822605f8 82b0eef8 832d6828 832d7990 832d8360 832d8364
// NFSMixMap, the audio mix-map runtime the game runs on its render thread (from EA's NFS audio
// tech; it has no asserts in sk83, so its source path is not in the game's strings -- filed under
// the class name).
//   NFSMixMap::MixMasterChannels()   sk83_na_m 0x82b0dff0 (0x5cc)
//   NFSMixMap::UpdateMasterChannels() sk83_na_m 0x82b0df30 (0xbc)
//   NFSMixMap::ProcessMixMap(float, int) sk83_na_m 0x82b123a0 (0x534)
//   NFSMixMap::UpdateSubChannels()  sk83_na_m 0x82b0de70 (0xbc)
//   NFSMixShape::GetPitchMultFromCents(int)          sk83_na_m 0x82b0b998 (0x120)
//   NFSMixShape::GetCurveOutput(eMIXTABLEID, int, bool) sk83_na_m 0x82b0bab8 (0x19c)
// Verified with sk83_na_m --sk83_replay.
//   NFSMixMap::Update3DMixCtls()    sk83_na_m 0x82b0eef8 (0x6e4) -- written, not yet replay-verified
// (Skate 2 PDB: packages/versioned/nicotine/1.02.00/source/nfsmixmap.cpp)
#include "rwp_guestcall.h"
#include "rwp_ppc_ops.h"

namespace NFSMixShape { double GetPitchMultFromCents_(int cents); unsigned int GetCurveOutput_(unsigned int table, unsigned int x); }
// nfsmixshape.cpp
namespace NFSMixShape {
unsigned int GetAzimShapeOutput_(unsigned int table1, unsigned int table2, unsigned int pDistances, unsigned int mixRatio);
unsigned int GetCentsFromPitchMult_(double ratio);
}
void NFSMixMap_Update3DMixCtls(unsigned int self);
unsigned int NFSMixMap_UpdateEvtMixCtls(unsigned int self);
unsigned int NFSMixMap_UpdateSubChannels(unsigned int self);
static void NFSMixMap_UpdateSubChannels_(unsigned int self);

#ifdef RWP_GUEST
using namespace ::rwp;
#endif

namespace NFSMixShape {

#define g_PitchSemitone 0x82fdcfe8u     // float[100]: 2^(c/1200) for the cents within a semitone step
#define g_PitchStep     0x82fdcfb4u     // float[12]:  2^(s/12) per 100 cents
#define g_CurveTable    0x5fff0000u     // Q15 curve, 0x200 entries (64 inputs per entry)
#define g_Two           0x82060c50u     // 2.0f (an octave)
#define g_OneF          0x8231a844u     // 1.0f

// cents -> frequency multiplier: whole octaves (1200 cents) as powers of two, the rest from the
// two tables; negative cents divide (the octave factor too: the loop multiplies, then divides).
double GetPitchMultFromCents_(int cents)
{
    RWP_FPU_MODE();
    const bool neg = (unsigned int)cents >> 31;
    const double one = (double)(float)RWP_GLOBAL(RWP_F32, g_OneF), two = (double)(float)RWP_GLOBAL(RWP_F32, g_Two);
    double oct = one;
    if (cents >= 1200)
    {
        const unsigned int n = (unsigned int)cents / 1200u;
        cents -= (int)(n * 1200u);
        for (unsigned int k = 0; k < n; k++)
            oct = (double)(float)(oct * two);                              // fmuls
    }
    if (!(cents > -1200))
    {
        const unsigned int n = (unsigned int)(-1200 - cents) / 1200u + 1u;
        cents += (int)(n * 1200u);
        for (unsigned int k = 0; k < n; k++)
            oct = (double)(float)(oct * two);
    }
    if (!neg)
    {
        int q = (int)(((long long)cents * 0x51eb851fll) >> 32) >> 5;      // cents / 100 (mulhw)
        q += (int)((unsigned int)q >> 31);
        const int s = cents / 100;                                          // divw
        const int r = cents - q * 100;
        const double a = (double)(float)RWP_GLOBAL(RWP_F32, g_PitchSemitone + 4u * (unsigned int)r);
        const double b = (double)(float)RWP_GLOBAL(RWP_F32, g_PitchStep + 4u * (unsigned int)s);
        return (double)(float)((double)(float)(a * b) * oct);
    }
    const int m = -cents;
    int q = (int)(((long long)m * 0x51eb851fll) >> 32) >> 5;
    q += (int)((unsigned int)q >> 31);
    const int s = m / 100;
    const double b = (double)(float)RWP_GLOBAL(RWP_F32, g_PitchStep + 4u * (unsigned int)s);
    const double inv = (double)(float)(one / b);                            // fdivs
    const int r = m - q * 100;
    const double t = (double)(float)(inv / oct);
    const double a = (double)(float)RWP_GLOBAL(RWP_F32, g_PitchSemitone + 4u * (unsigned int)r);
    const double inv2 = (double)(float)(one / a);
    return (double)(float)(t * inv2);
}

// A mix curve (Q15 in, Q15 out). Even ids are shapes on the table: 0 rising (interpolated), 2 its
// square, 4 the mirrored falling one, 6 its square, 8 linear falling; odd ids are the even one
// below them on the reversed input (0x7fff - x).
unsigned int GetCurveOutput_(unsigned int table, unsigned int x)
{
    RWP_SAVELR();
    RWP_GUEST_FRAME(frame, 0x60);
    for (;;)
    {
        if (table > 9u)
            return 0u;
        switch (table)
        {
        case 0:
        {
            const int i = (int)x >> 6;
            if (i < 0 || i >= 0x1ff)
                return 0u;
            const unsigned int a = RWP_LD32(g_CurveTable + 4u * (unsigned int)i);
            if ((int)a == 0)
                return 0u;
            const unsigned int b = RWP_LD32(g_CurveTable + 4u + 4u * (unsigned int)i);
            const unsigned int frac = RwpRlwimi(0x3ffu, x, 9, 0x12, 0x15);
            return (unsigned int)((int)((b - a) * frac) >> 15) + a;
        }
        case 2:
        {
            const unsigned int r = GetCurveOutput_(0u, x);
            return (unsigned int)((int)(r * r) >> 15);
        }
        case 4:
        {
            const int i = (int)x >> 6;
            const unsigned int j = (unsigned int)(0x1ff - i);
            const unsigned int frac = RwpRlwimi(0x3ffu, x, 9, 0x12, 0x15);
            const unsigned int a = 0x7fffu - RWP_LD32(g_CurveTable + 4u * j);
            const unsigned int b = 0x7fffu - RWP_LD32(g_CurveTable + 4u + 4u * j);
            return (unsigned int)((int)((b - a) * frac) >> 15) + a;
        }
        case 6:
        {
            const unsigned int r = GetCurveOutput_(4u, x);
            return (unsigned int)((int)(r * r) >> 15);
        }
        case 8:
            return 0x7fffu - x;
        default:
            table -= 1u;
            x = 0x7fffu - x;
            break;
        }
    }
}

}

#define g_PitchScale     0x5fff0000u     // float: the pitch multiplier's fixed-point scale
#define g_GainTable      0x5fff0000u     // millibel -> linear gain, 602 entries per 6.02 dB halving

// [-10000, 0]: below -10000 clamps, above 0 goes to 0 (branch-free in the original)
static __forceinline int ClampGain(int v)
{
    if (v < -10000)
        return -10000;
    return v > 0 ? 0 : v;
}

// millibels -> linear: each 602 mB (6.02 dB) halves the gain, the remainder from the table
static __forceinline unsigned int GainFromMb(int v)
{
    v = ClampGain(v);
    const int hi = (int)(((long long)v * 0x1b37484bll) >> 32);     // mulhw
    int q = hi >> 6;
    q += (int)((unsigned int)q >> 31);
    const int steps = v / -602;                                         // divw
    const int rem = -(v - q * 602);
    if ((unsigned int)steps > 15u)
        return 0u;
    return (unsigned int)((int)RWP_LD32(g_GainTable + 0x964u - 4u * (unsigned int)rem) >> steps);
}

// Every voice (+0x1c0, 8 bytes: definition, state; count +0x1dc) runs its mix program
// (definition +0xc: a header word -- op count in bits 0..4, parameter type in bits 24..27 --
// then one word per op: output slot in bits 26..30, source in bits 21..25, bit 31 "take the
// source's raw value", a signed 16-bit offset). Each op makes one 16-bit value, packed into the
// state's output words (+0x10, two slots per word, slot 1 in the low half... see the shifts).
// Parameter types: 1 pitch cents [-4800, 2400], 2 gain mB [-10000, 0], 3 raw, 0/4 gain mB through
// the table. A voice whose state is inactive (+0x10 -> +0x3c bit 0) only writes its first op's
// slot with the type's silent value.
void NFSMixMap_MixMasterChannels(unsigned int self)
{
#if defined(RWP_GUEST)
    RWP_SAVEGPRLR(18);
    GStore64(g_ctx->r1.u32 - 0x80u, g_ctx->f31.u64);                     // stfd f31, -0x80(r1)
#endif
    RWP_GUEST_FRAME(frame, 0xe0);
    unsigned int voice = RWP_LD32(self + 0x1c0u);
    if ((int)RWP_LD32(self + 0x1dcu) <= 0)
        return;
    RWP_FPU_MODE();
    const float pitchScale = RWP_GLOBAL(RWP_F32, g_PitchScale);
    for (int i = 0; i < (int)RWP_LD32(self + 0x1dcu); i++, voice += 8u)
    {
        const unsigned int prog = RWP_LD32(RWP_LD32(voice) + 0xcu);
        unsigned int op = prog + 4u;
        const unsigned int out = RWP_LD32(RWP_LD32(voice + 4u) + 0x10u);
        const unsigned int head = RWP_LD32(prog);
        const unsigned int type = (unsigned int)((int)head >> 24) & 0xfu;
        const int nOps = (int)(head & 0x1fu);
        if (!(RWP_LD32(out + 0x3cu) & 1u))
        {
            int v;
            switch (type)
            {
            case 1: v = 0; break;
            case 2: v = 25000; break;
            default: v = -10000; break;
            }
            const unsigned int w = RWP_LD32(op);
            const unsigned int slot = (unsigned int)((int)w >> 26) & 0x1fu;
            const unsigned int word = out + 4u * (unsigned int)((int)slot >> 1);
            const unsigned int keep = 0xffffu << (((slot - 1u) << 4) & 0x10u);
            RWP_ST32(word, (keep & RWP_LD32(word)) | (((unsigned int)v & 0xffffu) << ((slot << 4) & 0x10u)));
            continue;
        }
        for (int k = nOps; k > 0; k--, op += 4u)
        {
            const unsigned int w = RWP_LD32(op);
            const unsigned int def = RWP_LD32(voice), state = RWP_LD32(voice + 4u);
            const unsigned int slot = (unsigned int)((int)w >> 26) & 0x1fu;
            const int src = (int)(((int)w >> 21) & 0x1f);
            const int nSrc = (int)(RWP_LD32(def + 8u) >> 16 & 0x1fu);       // lhz +8
            const unsigned int dst = RWP_LD32(state + 0x10u) + 4u * (unsigned int)((int)slot >> 1);
            const int off = (short)(w & 0xffffu);
            unsigned int v;
            if (nSrc > 0 && src < nSrc)
            {
                const unsigned int s = RWP_LD32(RWP_LD32(state + 0xcu) + 4u * (unsigned int)src);
                if (w & 0x80000000u)
                    v = RWP_LD32(RWP_LD32(s + 4u) + 8u) & 0xffffu;
                else if (type > 4u || type == 3u)
                    v = RWP_LD32(state + 8u);
                else if (type == 1u)
                {
                    const int p = (int)RWP_LD32(RWP_LD32(s + 4u) + 0x14u) + (int)RWP_LD32(state + 8u) + off;
                    v = p > 2400 ? 2400u : (p < -4800 ? 0u : (unsigned int)p);
                }
                else if (type == 2u)
                    v = (unsigned int)ClampGain((int)RWP_LD32(state + 8u) + off);
                else
                    v = GainFromMb((int)RWP_LD32(RWP_LD32(s + 4u) + 0xcu) + (int)RWP_LD32(state + 8u) + off);
            }
            else
            {
                const int p = (int)RWP_LD32(state + 8u) + off;
                if (type > 4u || type == 3u)
                    v = p < 0 ? 0u : (p > 25000 ? 25000u : (unsigned int)p);
                else if (type == 1u)
                    v = p > 2400 ? 2400u : (p < -4800 ? (unsigned int)-4800 : (unsigned int)p);
                else if (type == 2u)
                {
                    const double mult = NFSMixShape::GetPitchMultFromCents_(ClampGain(p));
                    RWP_FPU_MODE();
                    const double scaled = (double)(float)(mult * (double)pitchScale);         // fmuls
                    long long t;                                                                  // fctiwz
                    if (scaled != scaled)
                        t = 0x80000000ll;
                    else if (scaled > 2147483647.0)
                        t = 2147483647ll;
                    else if (scaled < -2147483648.0)
                        t = (long long)(int)0x80000000;
                    else
                        t = (long long)(int)scaled;
                    RWP_ST64(FRAME_AT(frame, 0x50), (unsigned long long)t);
                    v = RWP_LD32(FRAME_AT(frame, 0x54));
                }
                else
                    v = GainFromMb(p);
            }
            const unsigned int keep = 0xffffu << (((slot - 1u) << 4) & 0x10u);
            RWP_ST32(dst, (RWP_LD32(dst) & keep) | ((v & 0xffffu) << ((slot << 4) & 0x10u)));
        }
    }
}

// Each voice's level (state +8): an active voice starts from its definition's base (a signed
// 16-bit value at definition -> +4) and, when it has modulators (state +4, count in definition
// +8 low byte), adds each one's current value; an inactive voice is at -10000 (silent).
void NFSMixMap_UpdateMasterChannels(unsigned int self)
{
    unsigned int voice = RWP_LD32(self + 0x1c0u);
    for (int i = 0; i < (int)RWP_LD32(self + 0x1dcu); i++, voice += 8u)
    {
        const unsigned int state = RWP_LD32(voice + 4u), def = RWP_LD32(voice);
        if (!(RWP_LD32(RWP_LD32(state + 0x10u) + 0x3cu) & 1u))
        {
            RWP_ST32(state + 8u, (unsigned int)-10000);
            continue;
        }
        const unsigned int base = RWP_LD32(RWP_LD32(def) + 4u) >> 16;          // lhz
        RWP_ST32(state + 8u, base);
        if (base & 0x8000u)
            RWP_ST32(state + 8u, base | 0xffff0000u);
        if (!RWP_LD32(RWP_LD32(voice + 4u) + 4u))
            continue;
        const int n = (int)(RWP_LD32(def + 8u) & 0xffu);
        if (n <= 0)
            continue;
        unsigned int p = RWP_LD32(state + 4u);
        for (int k = 0; k < n; k++, p += 4u)
            RWP_ST32(state + 8u, RWP_LD32(RWP_LD32(p)) + RWP_LD32(state + 8u));
    }
}

// Each sub channel (+0x1b4, 8 bytes: definition, state; count +0x1d8) with modulators (state +0)
// sums their values (definition +8 low byte of them) into state +4, clamped to the definition's
// range (definition -> +4: high half & 0x7fff the maximum, low half | 0xffff0000 the minimum).
static void NFSMixMap_UpdateSubChannels_(unsigned int self)
{
#if defined(RWP_GUEST)
    GStore64(g_ctx->r1.u32 - 8u, g_ctx->r31.u64);                         // std r31, -8(r1)
#endif
    unsigned int e = RWP_LD32(self + 0x1b4u);
    for (int i = 0; i < (int)RWP_LD32(self + 0x1d8u); i++, e += 8u)
    {
        const unsigned int state = RWP_LD32(e + 4u), def = RWP_LD32(e);
        if (!RWP_LD32(state))
            continue;
        RWP_ST32(state + 4u, 0u);
        const int n = (int)(RWP_LD32(def + 8u) & 0xffu);
        if (n > 0)
        {
            const unsigned int arr = RWP_LD32(state);
            for (int k = 0; k < n; k++)
                RWP_ST32(state + 4u, RWP_LD32(state + 4u) + RWP_LD32(RWP_LD32(arr + 4u * (unsigned int)k)));
        }
        const unsigned int range = RWP_LD32(RWP_LD32(def) + 4u);
        const int hi = (int)(((int)range >> 16) & 0x7fff), lo = (int)(range | 0xffff0000u);
        if ((int)RWP_LD32(state + 4u) > hi)
            RWP_ST32(state + 4u, (unsigned int)hi);
        if ((int)RWP_LD32(state + 4u) < lo)
            RWP_ST32(state + 4u, (unsigned int)lo);
    }
}

#define g_MbFromLinear   0x82fdbfb0u     // the log table: 0..0x7fff linear (Q15) -> millibels by octave
#define g_MixRateScale   0x5fff0000u     // float
#define g_MixRateDiv     0x5fff0000u     // float

// Q15 linear gain -> millibels: the octave from the leading zeros (cntlzw - 17: 0 for
// 0x4000..0x7fff, 14 for 1), 602 mB per octave, the mantissa through the table (coarser index for
// the high octaves, finer -- an rlwimi-built byte offset -- for the low ones). Zero, or anything
// outside 1..0x7fff, is -10000.
static unsigned int MbFromLinear(unsigned int x)
{
    if (x == 0)
        return (unsigned int)-10000;                                    // cntlzw 32 -> 15
    const unsigned int c = (unsigned int)RwpCntlzw(x) - 17u;
    const unsigned int t = g_MbFromLinear;
    if (c > 14u)
        return (unsigned int)-10000;
    switch (c)
    {
    case 0: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x4000u) >> 5)) - 602u;
    case 1: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x2000u) >> 4)) - 1204u;
    case 2: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x1000u) >> 3)) - 1806u;
    case 3: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x800u) >> 2)) - 2408u;
    case 4: return RWP_LD32(t + 4u * (unsigned int)((int)(x - 0x400u) >> 1)) - 3010u;
    case 5: return RWP_LD32(t + 4u * (x - 0x200u)) - 3612u;
    case 6: return RWP_LD32(t + RwpRlwimi(4u, (x << 1) - 0x1ffu, 2, 0, 0x1c)) - 4214u;
    case 7: return RWP_LD32(t + RwpRlwimi(0xcu, (x << 2) - 0x1fdu, 2, 0, 0x1b)) - 4816u;
    case 8: return RWP_LD32(t + RwpRlwimi(0x1cu, (x << 3) - 0x1f9u, 2, 0, 0x1a)) - 5418u;
    case 9: return RWP_LD32(t + RwpRlwimi(0x3cu, (x << 4) - 0x1f1u, 2, 0, 0x19)) - 6020u;
    case 10: return RWP_LD32(t + RwpRlwimi(0x7cu, (x << 5) - 0x1e1u, 2, 0, 0x18)) - 6622u;
    case 11: return RWP_LD32(t + RwpRlwimi(0xfcu, (x << 6) - 0x1c1u, 2, 0, 0x17)) - 7224u;
    case 12: return RWP_LD32(t + RwpRlwimi(0x1fcu, (x << 7) - 0x181u, 2, 0, 0x16)) - 7826u;
    case 13: return RWP_LD32(t + RwpRlwimi(0x3fcu, (x << 8) - 0x101u, 2, 0, 0x15)) - 8428u;
    default: return RWP_LD32(t + RwpRlwimi(0x7fcu, (x << 9) - 1u, 2, 0, 0x14)) - 9030u;
    }
}

// The mix map's frame: timing (+0x84 dt, +0x88 dt * scale, +0x22c dt / div with the previous one
// kept in +0x230, +0x7c/+0x80 the previous/current tick), every curve (+0x184, 16 bytes: table
// id in the low nibble of +0, input pointer +4) evaluated and turned into millibels (+8, the raw
// output in +0xc), every sub channel (+0x190, 8 bytes: definition, state) given its gain --
// (0x7fff - (0x7fff - level) * depth >> 15) in millibels plus the definition's offset, scaled by
// the Q15 product of its modulators -- then the dependent updates.
void NFSMixMap_ProcessMixMap(unsigned int self, double dt, int tick)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_NV(28, self);
    RWP_FPU_MODE();
    const unsigned int prevTick = RWP_LD32(self + 0x80u);
    RWP_GLOBAL(RWP_F32, self + 0x230u) = (float)RWP_GLOBAL(RWP_F32, self + 0x22cu);
    const int nCurves = (int)RWP_LD32(self + 0xd0u);
    const unsigned int curves = RWP_LD32(self + 0x184u);
    const double scale = (double)(float)RWP_GLOBAL(RWP_F32, g_MixRateScale);
    const double div = (double)(float)RWP_GLOBAL(RWP_F32, g_MixRateDiv);
    const float scaled = (float)(dt * scale);                            // fmuls
    RWP_GLOBAL(RWP_F32, self + 0x22cu) = (float)(dt / div);              // fdivs
    RWP_GLOBAL(RWP_F32, self + 0x84u) = (float)dt;
    RWP_NV(31, 0xffffffff832d7190ull);
    RWP_GLOBAL(RWP_F32, self + 0x88u) = scaled;
    RWP_ST32(self + 0x7cu, prevTick);
    RWP_ST32(self + 0x80u, (unsigned int)tick);
    if (nCurves > 0)
    {
        unsigned int c = curves - 8u;
        for (int j = 0; j < (int)RWP_LD32(self + 0xd0u); j++)
        {
            RWP_NV(29, (unsigned int)j);
            RWP_NV(30, c);
            const unsigned int out = NFSMixShape::GetCurveOutput_(RWP_LD8(c + 8u) & 0xfu, RWP_LD32(RWP_LD32(c + 0xcu)));
            RWP_ST32(c + 0x14u, out);
            c += 0x10u;
            RWP_ST32(c, MbFromLinear(out));
        }
    }
    unsigned int e = RWP_LD32(self + 0x190u);
    for (int k = 0; k < (int)RWP_LD32(self + 0x1d0u); k++, e += 8u)
    {
        const unsigned int state = RWP_LD32(e + 4u), def = RWP_LD32(e);
        const unsigned int level = RWP_LD32(RWP_LD32(state) + 0xcu);
        const unsigned int x = 0x7fffu - (unsigned int)((int)((0x7fffu - level) * RWP_LD32(def + 0xcu)) >> 15);
        const unsigned int mb = MbFromLinear(x);
        const unsigned int total = RWP_LD32(def + 8u) + mb;
        unsigned int g = 0x7fffu;
        const unsigned int mods = RWP_LD32(state + 4u);
        if (mods)
        {
            const int n = (int)RWP_LD8(RWP_LD32(def) + 4u);
            for (int m = 0; m < n; m++)
                g = (unsigned int)((int)(RWP_LD32(RWP_LD32(mods + 4u * (unsigned int)m)) * g) >> 15);
        }
        RWP_ST32(state + 8u, (unsigned int)((int)(g * total) >> 15));
    }
    RWP_CALL(0x5fff0000, NFSMixMap_Update3DMixCtls, self);
    RWP_CALL(0x829502a4, NFSMixMap_UpdateEvtMixCtls, self);
    // UpdateMasterChannels and MixMasterChannels are called on whatever r3 the previous call
    // returned (the compiler knew it is still this)
    NFSMixMap_UpdateSubChannels_(self);                                   // leaves r3 = this
    const unsigned int r = self;
    NFSMixMap_UpdateMasterChannels(r);
    NFSMixMap_MixMasterChannels(r);
}

unsigned int NFSMixMap_UpdateSubChannels(unsigned int self)
{
    NFSMixMap_UpdateSubChannels_(self);
    return self;
}

#define g_ZeroF          0x5fff0000u     // 0.0f
#define g_MinusOneF      0x8216dee0u     // -1.0f
#define g_Q15F           0x821747fcu     // 32767.0f
#define g_DopplerSmooth  0x5fff0000u     // DOPPLER_SMOOTHING_FACTOR (0.2f)

// The 3D mix controls. On a camera state change every shared control (+0x1a4, 20 bytes; count
// +0x164) picks its state parameters for the new state (map params: count in the low nibble of
// byte 0, then 24-byte entries keyed by the low nibble of their first byte; no match retries with
// state 0 -- forever, as in the original, if that is missing too) and records the change. Then
// every control (+0x1a0, 8 bytes: shared, unique; count +0x1d4) whose inputs are live (input word
// 15 bit 0): the distance input the state selects (word 0 / 1, else -1), its azimuth (word 2 / 3)
// picking a quadrant of four min/max distance pairs and two rolloff curves, the Q15 rolloff
// crossfaded between the quadrants (+0x10) and in millibels (+0xc); with a doppler speed (low
// half of state word 1) the pitch from the closing speed, smoothed into the doppler cents (+0x14).
void NFSMixMap_Update3DMixCtls(unsigned int self)
{
    RWP_SAVEGPRLR(20);
#if defined(RWP_GUEST)
    {   // __savefpr_27 at r1-104: f27..f31 below it
        const u32 r12 = g_ctx->r1.u32 - 104u;
        GStore64(r12 - 40u, g_ctx->f27.u64);
        GStore64(r12 - 32u, g_ctx->f28.u64);
        GStore64(r12 - 24u, g_ctx->f29.u64);
        GStore64(r12 - 16u, g_ctx->f30.u64);
        GStore64(r12 - 8u, g_ctx->f31.u64);
    }
#endif
    RWP_GUEST_FRAME(frame, 0x110);
    RWP_NV(22, self);
    RWP_NV(23, 0u);
    // camera state change: re-select every shared control's state parameters
    if (RWP_LD32(self + 0x80u) != RWP_LD32(self + 0x7cu) && (int)RWP_LD32(self + 0x164u) > 0)
    {
        unsigned int sh = RWP_LD32(self + 0x1a4u);
        for (int i = 0; i < (int)RWP_LD32(self + 0x164u); i++, sh += 20u)
        {
            const unsigned int map = RWP_LD32(sh);
            const int n = (int)(RWP_LD8(map) & 0xfu);
            unsigned int key = RWP_LD32(self + 0x80u);
            bool found = false;
            for (;;)
            {
                unsigned int e = map + 4u;
                for (int k = 0; k < n; k++, e += 24u)
                    if ((RWP_LD8(e) & 0xfu) == key)
                    {
                        RWP_ST32(sh + 4u, e);
                        found = true;
                        RWP_ST32(sh + 12u, RWP_LD32(self + 0x7cu));
                        RWP_ST32(sh + 8u, RWP_LD32(self + 0x80u));
                        RWP_ST32(sh + 16u, 0u);
                        break;
                    }
                if (found)
                    break;
                key = 0;
            }
        }
    }
    RWP_NV(21, 0u);
    if ((int)RWP_LD32(self + 0x1d4u) <= 0)
        return;
    RWP_FPU_MODE();
    const float zero = RWP_GLOBAL(RWP_F32, g_ZeroF), minusOne = RWP_GLOBAL(RWP_F32, g_MinusOneF);
    const float one = RWP_GLOBAL(RWP_F32, g_OneF), q15 = RWP_GLOBAL(RWP_F32, g_Q15F);
    RWP_NV(24, 0xffffffff832e0000ull);
    RWP_NV(25, 0x8000u);
    RWP_NV(26, 0xc000u);
    RWP_NV(27, (unsigned int)-10000);
    RWP_NV(30, 0xffffffff832d7190ull);
    unsigned int proc = RWP_LD32(self + 0x1a0u);
    for (int i = 0; i < (int)RWP_LD32(self + 0x1d4u); i++, proc += 8u)
    {
        RWP_NV(21, (unsigned int)i);
        RWP_NV(31, proc - 4u);
        const unsigned int u = RWP_LD32(proc + 4u);
        if (!(RWP_LD32(RWP_LD32(u + 4u) + 60u) & 1u))
        {   // inputs not live: silent
            RWP_ST32(u + 12u, (unsigned int)-10000);
            RWP_ST32(RWP_LD32(proc + 4u) + 16u, 0u);
            RWP_ST32(RWP_LD32(proc + 4u) + 8u, 0u);
            RWP_ST32(RWP_LD32(proc + 4u) + 20u, 0u);
            continue;
        }
        const unsigned int sp = RWP_LD32(RWP_LD32(proc) + 4u);          // pCurStateParams
        const unsigned int id = RWP_LD32(sp), shapes = RWP_LD32(sp + 4u);
        const unsigned int distSel = (id >> 12) & 0xfu, azimSel = (id >> 8) & 0xfu;
        RWP_NV(28, distSel);
        RWP_NV(29, sp);
        float dist = distSel == 0 ? (float)RWP_GLOBAL(RWP_F32, RWP_LD32(u + 4u) + 4u)
                   : distSel == 1 ? (float)RWP_GLOBAL(RWP_F32, RWP_LD32(u + 4u)) : minusOne;
        float dist2 = dist;
        const unsigned int azim = azimSel == 0 ? RWP_LD32(RWP_LD32(u + 4u) + 12u)
                                : azimSel == 1 ? RWP_LD32(RWP_LD32(u + 4u) + 8u) : 0u;
        RWP_ST32(u + 8u, azim);
        // the quadrant: its two rolloff tables, the mix ratio within it, and its min/max pairs
        unsigned int t1, t2, mix;
        unsigned int mm0, mm1;                                            // fMinMaxDist[q], [q + 1]
        switch ((azim >> 14) & 3u)
        {
        case 0: t1 = shapes >> 28; t2 = shapes >> 16; mix = azim; mm0 = 32u; mm1 = 40u; break;
        case 1: t1 = shapes >> 16; t2 = shapes >> 24; mix = azim - 0x4000u; mm0 = 40u; mm1 = 48u; break;
        case 2: t1 = shapes >> 24; t2 = shapes >> 20; mix = azim - 0x8000u; mm0 = 48u; mm1 = 56u; break;
        default: t1 = shapes >> 20; t2 = shapes >> 28; mix = azim - 0xc000u; mm0 = 56u; mm1 = 32u; break;
        }
        const unsigned int un = RWP_LD32(proc + 4u);
        const float min0 = RWP_GLOBAL(RWP_F32, un + mm0), max0 = RWP_GLOBAL(RWP_F32, un + mm0 + 4u);
        const float min1 = RWP_GLOBAL(RWP_F32, un + mm1), max1 = RWP_GLOBAL(RWP_F32, un + mm1 + 4u);
        if (dist > max0 && dist > max1)
        {   // beyond both quadrants' ranges: silent (the azimuth stays)
            RWP_ST32(RWP_LD32(proc + 4u) + 12u, (unsigned int)-10000);
            RWP_ST32(RWP_LD32(proc + 4u) + 16u, 0u);
            RWP_ST32(RWP_LD32(proc + 4u) + 20u, 0u);
            continue;
        }
        if (dist < min0)
            dist = min0;
        if (dist2 < min1)
            dist2 = min1;
        if (dist > max0)
            dist = max0;
        if (dist2 > max1)
            dist2 = max1;
        const float f0 = (dist - min0) / (max0 - min0) * q15;                 // fsubs, fdivs, fmuls
        const float f1 = (dist2 - min1) / (max1 - min1) * q15;
        RWP_GLOBAL(RWP_U32, FRAME_AT(frame, 0x50)) = (unsigned int)RwpFctiwz((double)f0);
        RWP_GLOBAL(RWP_U32, FRAME_AT(frame, 0x54)) = (unsigned int)RwpFctiwz((double)f1);
        RWP_NV(20, un);
        const unsigned int rolloff = NFSMixShape::GetAzimShapeOutput_(t1 & 0xfu, t2 & 0xfu, FRAME_AT(frame, 0x50), mix);
        RWP_ST32(un + 16u, rolloff);
        RWP_ST32(RWP_LD32(proc + 4u) + 12u, MbFromLinear(RWP_LD32(RWP_LD32(proc + 4u) + 16u)));
        const unsigned int speed = RWP_LD32(sp + 4u) & 0xffffu;
        if (!speed)
            continue;
        // doppler: the pitch ratio speed / (speed + closing speed), as cents
        RWP_FPU_MODE();
        const float fs = (float)(int)speed;
        const unsigned int in = RWP_LD32(RWP_LD32(proc + 4u) + 4u);
        float cents, curDist;
        if (distSel == 1)
        {
            curDist = RWP_GLOBAL(RWP_F32, in);
            if (!(RWP_LD32(in + 60u) & 0x80000000u))
            {
                float d = (float)RWP_GLOBAL(RWP_F32, in + 52u) + fs;
                if (!(d > zero))
                    d = fs;
                cents = (float)(int)NFSMixShape::GetCentsFromPitchMult_((double)(fs / d));
                RWP_FPU_MODE();
            }
            else
            {   // reset request (bit 31): clear it, no pitch change this frame
                const unsigned int in2 = RWP_LD32(RWP_LD32(proc + 4u) + 4u);
                RWP_ST32(in2 + 60u, RWP_LD32(in2 + 60u) & 0x7fffffffu);
                cents = zero;
            }
        }
        else
        {
            curDist = RWP_GLOBAL(RWP_F32, in + 4u);
            if (!(RWP_LD32(in + 60u) & 0x40000000u))
            {
                float d = (float)RWP_GLOBAL(RWP_F32, in + 56u) + fs;
                if (!(d > zero))
                    d = fs;
                cents = (float)(int)NFSMixShape::GetCentsFromPitchMult_((double)(fs / d));
                RWP_FPU_MODE();
            }
            else
            {   // reset request (bit 30)
                const unsigned int in2 = RWP_LD32(RWP_LD32(proc + 4u) + 4u);
                RWP_ST32(in2 + 60u, RWP_LD32(in2 + 60u) & 0xbfffffffu);
                cents = zero;
            }
        }
        if ((float)RWP_GLOBAL(RWP_F32, RWP_LD32(proc + 4u) + 28u) == zero)
            RWP_GLOBAL(RWP_F32, RWP_LD32(proc + 4u) + 28u) = one;
        {
            const float prev = RWP_GLOBAL(RWP_F32, RWP_LD32(proc + 4u) + 24u);
            RWP_GLOBAL(RWP_F32, RWP_LD32(proc + 4u) + 28u) = curDist > prev ? curDist - prev : prev - curDist;
            RWP_GLOBAL(RWP_F32, RWP_LD32(proc + 4u) + 24u) = curDist;
        }
        const unsigned int un3 = RWP_LD32(proc + 4u);
        const float smooth = RWP_GLOBAL(RWP_F32, g_DopplerSmooth);
        const unsigned int dc = RWP_LD32(un3 + 20u);
        const float step = (cents - (float)(int)dc) * smooth;                  // fsubs, fmuls
        RWP_ST32(un3 + 20u, (unsigned int)RwpFctiwz((double)step) + dc);
    }
}
