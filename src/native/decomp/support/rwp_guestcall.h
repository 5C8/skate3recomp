// Calling recompiled guest functions from hand-decompiled code.
//
// Most of the render thread's functions call code that is not decompiled yet (the D3D runtime,
// the shader system, virtual methods). In the guest build (RWP_GUEST, inside a recomp) such a call
// goes to the recompiled function on the caller's PPCContext: arguments in r3..r10 / f1..,
// the result in r3 / f1, and a guest stack frame (GuestFrame) for locals whose address is passed.
// Direct calls name the guest address; virtual calls read the vtable from guest memory and go
// through the recompiler's own dispatch (REX_LOOKUP_FUNC), so hooked functions stay hooked.
// In the XDK build the same source makes ordinary C++ calls (RWP_CALL / RWP_VCALL below).
#ifndef RWP_GUESTCALL_H
#define RWP_GUESTCALL_H
#include "rwp_config.h"
#include <cmath>

#if defined(RWP_GUEST)
namespace rwp {

using u64 = uint64_t;
typedef void (*GuestFn)(PPCContext &, uint8_t *);
extern thread_local PPCContext *g_ctx;       // set by the hook that entered hand-written code
GuestFn ResolveGuest(u32 addr);              // the recompiled function at addr (host provides it)

// guest memory through g_base (LD_U32/ST_U32 need a local `base`)
inline u32 GLoad32(u32 a) { return __builtin_bswap32(*(const u32 *)(g_base + a)); }
inline void GStore32(u32 a, u32 v) { *(u32 *)(g_base + a) = __builtin_bswap32(v); }
inline u32 GLoad8(u32 a) { return *(const u8 *)(g_base + a); }
inline void GStore8(u32 a, u32 v) { *(u8 *)(g_base + a) = (u8)v; }
inline uint64_t GLoad64(u32 a) { return __builtin_bswap64(*(const uint64_t *)(g_base + a)); }
inline void GStore64(u32 a, uint64_t v) { *(uint64_t *)(g_base + a) = __builtin_bswap64(v); }

// argument -> register value: integers, big-endian fields (they convert), guest pointers, and
// 64-bit values whole (attribute keys and the like go in one 64-bit register)
template <class T> inline u64 ArgReg(const T &v) { return (u64)(u32)v; }
inline u64 ArgReg(unsigned long long v) { return v; }
inline u64 ArgReg(bool v) { return v ? 1u : 0u; }
template <class T> inline u64 ArgReg(const GPtr<T> &p) { return p.addr; }
template <class T> inline u64 ArgReg(const BEPtr<T> &p) { return __builtin_bswap32(p.raw); }
template <class T> inline u32 ArgU32(const T &v) { return (u32)ArgReg(v); }

// a float argument: goes in the next f register and, as in the 360 ABI, uses up a GPR slot too
// (that GPR is left as it is)
struct FltArg
{
    double v;
    explicit FltArg(double x) : v(x) {}
};

struct ArgState
{
    PPCRegister *r[8];
    PPCRegister *f[8];
    unsigned gi, fi;
};
template <class T> inline void SetArg(ArgState &s, const T &v) { s.r[s.gi++]->u64 = ArgReg(v); }
inline void SetArg(ArgState &s, const FltArg &a)
{
    s.f[s.fi++]->f64 = a.v;
    s.gi++;
}

template <class... A> inline u32 GuestCall(GuestFn fn, A... a)
{
    PPCContext &c = *g_ctx;
    ArgState s = { { &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10 },
                   { &c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8 }, 0, 0 };
    (SetArg(s, a), ...);
    fn(c, g_base);
    return c.r3.u32;
}
inline double RetF() { return g_ctx->f1.f64; }   // a float result (f1)
template <class... A> inline u32 Call(u32 addr, A... a) { return GuestCall(ResolveGuest(addr), a...); }
// virtual call: slot = byte offset in the vtable; the object is the first argument (r3)
template <class... A> inline u32 VCall(u32 obj, u32 slot, A... a)
{
    const u32 vt = GLoad32(obj);
    return GuestCall(ResolveGuest(GLoad32(vt + slot)), obj, a...);
}

// a guest stack frame for the duration of a scope (locals passed to guest code by address)
struct GuestFrame
{
    u32 saved;
    // stwu r1, -size(r1): the back chain word too (later code can read stale stack)
    explicit GuestFrame(u32 size) : saved(g_ctx->r1.u32)
    {
        g_ctx->r1.u64 = (saved - size) & ~15u;
        GStore32(g_ctx->r1.u32, saved);
    }
    ~GuestFrame() { g_ctx->r1.u64 = saved; }
    u32 at(u32 off) const { return g_ctx->r1.u32 + off; }
};

// __savegprlr_<first>: r<first>..r31 at r1-8*(32-n)-8, LR at r1-8 (made before the stwu; later
// code can read that stack). r14..r31 are contiguous in PPCContext.
inline void SaveGprLr(int first)
{
    PPCContext &c = *g_ctx;
    const u32 sp = c.r1.u32;
    for (int n = first; n < 32; n++)
        GStore64(sp - 8u * (u32)(32 - n) - 8u, (&c.r14)[n - 14].u64);
    GStore32(sp - 8u, (u32)c.lr);
}

// The original's non-volatile registers r<first>..r31, mirrored into the guest context for the
// duration of a native function: callees save them on the stack (savegprlr) and game code can
// read such stale stack later, so what is there must be what the original had. The epilogue's
// restore is the destructor.
struct NvScope
{
    u64 saved[18];
    int first;
    explicit NvScope(int f) : first(f)
    {
        for (int n = first; n < 32; n++)
            saved[n - 14] = (&g_ctx->r14)[n - 14].u64;
    }
    ~NvScope()
    {
        for (int n = first; n < 32; n++)
            (&g_ctx->r14)[n - 14].u64 = saved[n - 14];
    }
};
inline void SetNv(int n, u64 v) { (&g_ctx->r14)[n - 14].u64 = v; }
// f31 the same way (stfd f31, -off(r1) in the prologue; lfd in the epilogue)
struct NvF31Scope
{
    u64 saved;
    explicit NvF31Scope(u32 off) : saved(g_ctx->f31.u64) { GStore64(g_ctx->r1.u32 - off, saved); }
    ~NvF31Scope() { g_ctx->f31.u64 = saved; }
};

// the recompiler's float modes: vector code runs with flush-to-zero, scalar code without
inline void VmxMode() { g_ctx->fpscr.enableFlushMode(); }
inline void FpuMode() { g_ctx->fpscr.disableFlushMode(); }

}   // namespace rwp

// the original function's stack frame (size from its stwu): callees then see the same r1 as in
// the recompiled code -- some of them record it (found by the replay compare)
#define RWP_GUEST_FRAME(name, size) rwp::GuestFrame name(size)
#define FRAME_AT(name, off) (name).at(off)
#define RWP_VMX_MODE() rwp::VmxMode()
#define RWP_FPU_MODE() rwp::FpuMode()
// call through a guest function pointer
#define RWP_FNCALL(fn, ...) rwp::Call((u32)(fn), ##__VA_ARGS__)
// a global at a fixed guest address, as a guest-memory field
#define RWP_GLOBAL(T, addr) (*(T *)(rwp::g_base + (addr)))
#define RWP_CALL(addr, name, ...) rwp::Call(addr, ##__VA_ARGS__)
#define RWP_CALL0(addr, name) rwp::Call(addr)
#define RWP_VCALL(obj, slot, ...) rwp::VCall(rwp::ArgU32(obj), slot, ##__VA_ARGS__)
#define RWP_FLT(x) rwp::FltArg(x)
// raw guest memory by address (big-endian)
#define RWP_LD8(a) rwp::GLoad8(a)
#define RWP_LD32(a) rwp::GLoad32(a)
#define RWP_LD64(a) rwp::GLoad64(a)
#define RWP_ST8(a, v) rwp::GStore8(a, v)
#define RWP_ST32(a, v) rwp::GStore32(a, v)
#define RWP_ST64(a, v) rwp::GStore64(a, v)
#define RWP_REG64(name) (rwp::g_ctx->name.u64)     // a full 64-bit argument register
#define RWP_SAVEGPRLR(first) rwp::SaveGprLr(first); rwp::NvScope rwp_nv_scope_(first)
#define RWP_NV(n, v) rwp::SetNv(n, (u64)(v))      // the original keeps v in r<n> from here on
#define RWP_SAVELR() rwp::GStore32(rwp::g_ctx->r1.u32 - 8u, (u32)rwp::g_ctx->lr)   // mflr r12; stw r12, -8(r1)
#define RWP_SAVEF31(off) rwp::NvF31Scope rwp_nvf31_scope_(off)
#define RWP_NVF31(v) (rwp::g_ctx->f31.f64 = (double)(v))
// single-precision fused ops as the recompiler evaluates them (double fma, then round to float)
#define RWP_FMADDS(a, b, c) ((float)std::fma((double)(a), (double)(b), (double)(c)))
#define RWP_FNMSUBS(a, b, c) ((float)-std::fma((double)(a), (double)(b), -(double)(c)))

#else   // XDK / host: plain C++ (the callees are declared by the including source)
#define RWP_VMX_MODE() ((void)0)
#define RWP_FPU_MODE() ((void)0)
#define RWP_SAVEGPRLR(first) ((void)0)
#define RWP_NV(n, v) ((void)0)
#define RWP_SAVELR() ((void)0)
#define RWP_SAVEF31(off) ((void)0)
#define RWP_NVF31(v) ((void)0)
#define RWP_FMADDS(a, b, c) ((a) * (b) + (c))
#define RWP_FNMSUBS(a, b, c) ((c) - (a) * (b))
#define RWP_GUEST_FRAME(name, size) __declspec(align(16)) unsigned char name##_buf[size]
#define FRAME_AT(name, off) ((unsigned int)(size_t)(name##_buf + (off)))
typedef void (*RwpFn)(...);
#define RWP_FNCALL(fn, ...) ((RwpFn)(size_t)(unsigned int)(fn))(__VA_ARGS__)
#define RWP_GLOBAL(T, addr) (*(T *)(size_t)(addr))
#define RWP_CALL(addr, name, ...) name(__VA_ARGS__)
#define RWP_CALL0(addr, name) name()
#define RWP_VCALL(obj, slot, ...) RwpVCall(obj, slot, __VA_ARGS__)
#define RWP_FLT(x) (x)
#define RWP_LD8(a) (*(volatile unsigned char *)(size_t)(a))
#define RWP_LD32(a) (*(volatile unsigned int *)(size_t)(a))
#define RWP_LD64(a) (*(volatile unsigned long long *)(size_t)(a))
#define RWP_ST8(a, v) (*(volatile unsigned char *)(size_t)(a) = (unsigned char)(v))
#define RWP_ST32(a, v) (*(volatile unsigned int *)(size_t)(a) = (v))
#define RWP_ST64(a, v) (*(volatile unsigned long long *)(size_t)(a) = (v))
#endif

// fctidz: float -> int64, truncating, saturating (NaN -> INT64_MIN), as the PPC instruction
// fctiwz as the recompiler evaluates it: NaN -> 0x80000000 (zero-extended), above INT_MAX ->
// INT_MAX, else the truncation sign-extended (below INT_MIN: INT_MIN) -- the full 64 bits, since
// the game stores them with stfd
static inline unsigned long long RwpFctiwz(double x)
{
    if (x != x) return 0x80000000ull;
    if (x > 2147483647.0) return 0x7fffffffull;
    if (x < -2147483648.0) return (unsigned long long)(long long)(int)0x80000000;
    return (unsigned long long)(long long)(int)x;
}
static inline long long RwpFctidz(double x)
{
    if (x != x) return (long long)0x8000000000000000ull;
    if (x >= 9223372036854775808.0) return 0x7fffffffffffffffll;
    if (x < -9223372036854775808.0) return (long long)0x8000000000000000ull;
    return (long long)x;
}

#endif
