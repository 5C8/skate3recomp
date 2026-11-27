// Runtime for functions lifted by tools/vlift.py (one C statement per PPC instruction).
// Both targets are 32-bit: guest addresses are plain pointers held in u32 registers.
#pragma once
#include <stddef.h>      // uintptr_t (the XDK's VS2008-era CRT has no stdint.h)
#include <math.h>
#include <string.h>

typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef signed char s8;

#define FLUSH_V() ((void)0)    // the native equivalence build sets FTZ/DAZ once
#define FLUSH_F() ((void)0)
#define MEM(T, a) (*(T *)(uintptr_t)(u32)(a))

// condition register field bits
enum { CR_LT = 8, CR_GT = 4, CR_EQ = 2, CR_SO = 1 };
static __forceinline u32 CMP_S(s32 a, s32 b) { return a < b ? CR_LT : a > b ? CR_GT : CR_EQ; }
static __forceinline u32 CMP_U(u32 a, u32 b) { return a < b ? CR_LT : a > b ? CR_GT : CR_EQ; }
static __forceinline u32 CMP_F(double a, double b) { return a < b ? CR_LT : a > b ? CR_GT : a == b ? CR_EQ : CR_SO; }
// record-form vector compare: LT = every lane true, EQ = no lane true
static __forceinline u32 VCR6(__vector4 m)
{
    u32 all = m.vector4_u32[0] & m.vector4_u32[1] & m.vector4_u32[2] & m.vector4_u32[3];
    u32 any = m.vector4_u32[0] | m.vector4_u32[1] | m.vector4_u32[2] | m.vector4_u32[3];
    return (all == 0xffffffffu ? CR_LT : 0) | (any == 0 ? CR_EQ : 0);
}
#define CRWORD6(cr) ((u32)(cr) << 4)            // mfocrf of field 6
static __forceinline u32 RLWINM(u32 x, int sh, int mb, int me)
{
    u32 r = sh ? (x << sh) | (x >> (32 - sh)) : x;
    u32 m = mb <= me ? (0xffffffffu >> mb) & (0xffffffffu << (31 - me))
                     : (0xffffffffu >> mb) | (0xffffffffu << (31 - me));
    return r & m;
}
#ifndef FMA_D
#define FMA_D(a, b, c) ((a) * (b) + (c))        // native equivalence build: fma (recomp lowering)
#endif
static __forceinline double FABS_D(double x) { return fabs(x); }

#define VZERO __vspltisw(0)
#define DCBZL(a) memset((void *)(uintptr_t)((u32)(a) & ~127u), 0, 128)
#define LVX(a) (*(const __vector4 *)(uintptr_t)((u32)(a) & ~15u))
#define STVX(a, v) (*(__vector4 *)(uintptr_t)((u32)(a) & ~15u) = (v))
#define LVLX(a) __lvlx((const void *)(uintptr_t)(u32)(a), 0)

// the lifted function's stack: r1 starts in the middle so both the callee frame (stwu) and
// the caller-area offsets above the entry r1 stay inside the buffer
#define VLIFT_FRAME(n) \
    __declspec(align(16)) char vlift_stack[2 * (n)]; \
    u32 r1 = (u32)(uintptr_t)(vlift_stack + (n))
