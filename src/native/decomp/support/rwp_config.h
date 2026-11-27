// One decomp source, three builds. The rw::physics sources declare their structs with the
// field types below (Skate 2 PDB layouts and names) and access them as ordinary C++:
//
//   XDK (Xbox 360, matching)  plain types: the data is big-endian native memory.
//   HORUS_NATIVE (x86 tests)  plain types on host-order memory (tools/equiv, i686).
//   RWP_GUEST (recomp, x64)   the data stays big-endian in guest memory: every field is a
//                             byte-swapping wrapper and every pointer a 32-bit guest address
//                             resolved against rwp::g_base. The vector ops come from
//                             src/native/rwp_guest_rt.h (the recompiler's own lowering, in its
//                             reversed register layout), so results match the recomp exactly.
//
// Rules for the sources: read a field by assigning or converting it (`__vector4 v = rb->mVel;`,
// `float f = lim->mVtwist;`), write it by assigning; get scalar lanes with VLANE_F / VLANE_U;
// cast guest pointers with RWP_CAST; never take the host address of a field (use RWP_ADDR).
#pragma once

#if defined(RWP_GUEST)
#include "rwp_guest_rt.h"
#include <cstring>
namespace rwp {
extern uint8_t *g_base;

// big-endian scalar fields
struct BEU32
{
    u32 raw;
    operator u32() const { return __builtin_bswap32(raw); }
    BEU32 &operator=(u32 v) { raw = __builtin_bswap32(v); return *this; }
    BEU32 &operator++() { return *this = u32(*this) + 1; }
    u32 operator++(int) { u32 v = *this; *this = v + 1; return v; }
};
struct BEU16
{
    u16 raw;
    operator u16() const { return __builtin_bswap16(raw); }
    BEU16 &operator=(u16 v) { raw = __builtin_bswap16(v); return *this; }
};
struct BES32
{
    u32 raw;
    operator s32() const { return (s32)__builtin_bswap32(raw); }
    BES32 &operator=(s32 v) { raw = __builtin_bswap32((u32)v); return *this; }
};
struct BEF32
{
    u32 raw;
    operator float() const { return std::bit_cast<float>(__builtin_bswap32(raw)); }
    BEF32 &operator=(float v) { raw = __builtin_bswap32(std::bit_cast<u32>(v)); return *this; }
};
// a 16-byte VMX slot: loads/stores exactly like the recompiled lvx/stvx
struct alignas(16) BEVec
{
    u8 raw[16];
    operator __vector4() const
    { return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_load_si128((const simde__m128i *)raw), rwp_mask(0))); }
    BEVec &operator=(__vector4 v)
    { simde_mm_store_si128((simde__m128i *)raw, simde_mm_shuffle_epi8(simde_mm_castps_si128(v), rwp_mask(0))); return *this; }
};
// 32-bit guest pointer
template <class T> struct GPtr
{
    u32 addr;
    GPtr() = default;
    explicit GPtr(u32 a) : addr(a) {}
    T *operator->() const { return (T *)(g_base + addr); }
    T &operator*() const { return *(T *)(g_base + addr); }
    T &operator[](u32 i) const { return ((T *)(g_base + addr))[i]; }
    GPtr operator+(u32 i) const { return GPtr(addr + i * sizeof(T)); }
    int operator-(GPtr o) const { return (int)(addr - o.addr) / (int)sizeof(T); }   // C++ pointer difference
    GPtr &operator+=(u32 i) { addr += i * sizeof(T); return *this; }
    GPtr &operator++() { addr += sizeof(T); return *this; }
    GPtr operator++(int) { GPtr o = *this; addr += sizeof(T); return o; }
    bool operator==(GPtr o) const { return addr == o.addr; }
    bool operator!=(GPtr o) const { return addr != o.addr; }
    explicit operator bool() const { return addr != 0; }
};
// a pointer stored in guest memory (big-endian)
template <class T> struct BEPtr
{
    u32 raw;
    operator GPtr<T>() const { return GPtr<T>(__builtin_bswap32(raw)); }
    BEPtr &operator=(GPtr<T> p) { raw = __builtin_bswap32(p.addr); return *this; }
    T *operator->() const { return GPtr<T>(*this).operator->(); }
    T &operator[](int i) const { return ((T *)(g_base + __builtin_bswap32(raw)))[i]; }
};
template <class T> inline GPtr<T> rwp_addr(const T &field) { return GPtr<T>(u32((const u8 *)&field - g_base)); }
// fpu Vector3 slot (12 bytes; the 16-byte VMX load at it also takes the next member)
struct RwpV3 { BEF32 x, y, z; };
inline __vector4 ldv(const void *p)
{ return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_load_si128((const simde__m128i *)p), rwp_mask(0))); }
inline void stv(void *p, __vector4 v)
{ simde_mm_store_si128((simde__m128i *)p, simde_mm_shuffle_epi8(simde_mm_castps_si128(v), rwp_mask(0))); }
// lvlx at a guest address (the recompiler's lowering)
template <class T> inline __vector4 __lvlx(GPtr<T> p, int off)
{
    u32 a = p.addr + (u32)off;
    return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
        simde_mm_load_si128((const simde__m128i *)(g_base + (a & ~0xFu))), rwp_mask(a & 0xFu)));
}
// a static constant table (host-order words, guest lane order) in the reversed layout
inline __vector4 rwp_const(const unsigned int *w)
{ return simde_mm_castsi128_ps(simde_mm_set_epi32((int)w[0], (int)w[1], (int)w[2], (int)w[3])); }
}  // namespace rwp
namespace rw { namespace physics { using namespace ::rwp; } }
#define RWP_V3 rwp::RwpV3
#define LOADV(field) rwp::ldv(&(field))
#define STOREV(field, v) rwp::stv(&(field), (v))
#define VSPLAT_F(f) simde_mm_set1_ps(f)
// rw::math Vector3(x, y, z): w = 0 (reversed layout: set_ps takes lanes 3..0)
#define RWP_VEC3F(x, y, z) simde_mm_set_ps((float)(x), (float)(y), (float)(z), 0.0f)
#define VSPLAT_LX(f) simde_mm_set1_ps(f)
#define VSPLAT_REF(x) simde_mm_set1_ps((float)(x))
// an fpu Vector3 field as a vector with w = 0 (reversed layout: set_ps takes lanes 3..0)
#define RWP_VEC3(v3) simde_mm_set_ps((float)(v3).x, (float)(v3).y, (float)(v3).z, 0.0f)
#define RWP_CONST(tbl) rwp::rwp_const((const unsigned int *)(tbl))
#define RWP_ALIGN16 alignas(16)
#define __dcbt(off, p) ((void)0)   // cache hint (the recompiler drops dcbt as well)
// dcbzl: zero the 128-byte line holding guest address p + off (the recompiler's lowering)
#define __dcbz128(off, p) std::memset(rwp::g_base + (((u32)((const u8 *)(p) - rwp::g_base) + (u32)(off)) & ~127u), 0, 128)
#define RWP_U32 rwp::BEU32
#define RWP_U16 rwp::BEU16
#define RWP_U8 unsigned char
#define RWP_S32 rwp::BES32
#define RWP_F32 rwp::BEF32
#define RWP_VEC rwp::BEVec
#define RWP_PTR(T) rwp::GPtr<T>
#define RWP_FIELD_PTR(T) rwp::BEPtr<T>
#define RWP_CAST(T, p) rwp::GPtr<T>((p).addr)
#define RWP_ADDR(field) rwp::rwp_addr(field)
#define RWP_ADDR_OF(p) ((u32)(p).addr)
#define RWP_FROM_ADDR(T, a) rwp::GPtr<T>((u32)(a))
#define RWP_SELF_PTR(T, x) rwp::GPtr<T>(rwp::rwp_addr(x).addr)
// a runtime-initialised global vector at its guest address (e.g. MAX_VECFLOAT 0x833a8ce0)
#define RWP_GLOBAL_VEC(addr, sym) rwp::ldv(rwp::g_base + (addr))
#define RWP_LIST_GET(T, list, n) rwp::GPtr<T>(__builtin_bswap32(*(const u32 *)(rwp::g_base + (u32)(list) + 4 * (u32)(n))))
#define RWP_LOAD_PTR(T, hostp) rwp::GPtr<T>(__builtin_bswap32(*(const u32 *)(hostp)))
#define RWP_LIST_SET(list, n, value) (*(u32 *)(rwp::g_base + (u32)(list) + 4 * (u32)(n)) = __builtin_bswap32((u32)(value)))
// scalar lanes in the recomp's reversed layout (guest lane i = host lane 3 - i)
#define VLANE_F(v, i) simde_mm_cvtss_f32(simde_mm_shuffle_ps(v, v, (3 - (i)) * 0x55))
#define VLANE_U(v, i) ((u32)simde_mm_extract_epi32(simde_mm_castps_si128(v), 3 - (i)))
#define VCR6_ALL(m) (simde_mm_movemask_ps(m) == 0xF)           // record compare: every lane true
#define VALL_GE(a, b) VCR6_ALL(__vcmpgefp(a, b))
#define VALL_GT(a, b) VCR6_ALL(__vcmpgtfp(a, b))
#define VALL_EQ(a, b) VCR6_ALL(__vcmpeqfp(a, b))
#define VLANE_SET4(v, x, y, z, w) ((v) = simde_mm_set_ps((float)(x), (float)(y), (float)(z), (float)(w)))
// set lane 3 only (guest lane 3 = host lane 0 in the reversed layout)
#define VLANE_SETW(v, f) ((v) = simde_mm_move_ss((v), simde_mm_set_ss((float)(f))))

#else   // XDK and the host tests: plain memory
struct RwpV3 { float x, y, z; };          // rw::math::fpu::Vector3Template<float>
#define RWP_V3 RwpV3
#define LOADV(field) (*(const __vector4 *)&(field))
#define STOREV(field, v) (*(__vector4 *)&(field) = (v))
// rw::math VecFloat(float) as Skate 3 builds it: zero-filled stack vector, splat
#define VSPLAT_F(f) RwpSplatF(f)
#define RWP_VEC3(v3) RwpVec3(v3)
// rw::math Vector3(x, y, z) as Skate 3 builds it: w zero-filled by the initializer
static __forceinline __vector4 RwpVec3F(float x, float y, float z) { __vector4 b = { x, y, z }; return b; }
#define RWP_VEC3F(x, y, z) RwpVec3F(x, y, z)
static __forceinline __vector4 RwpVec3(const RwpV3 &v)
{
    __vector4 t;
    { __vector4 b = { v.x, v.y, v.z }; t = b; }   // rw::math CreateVectorV3: w zero-filled by the initializer
    return t;
}
#define RWP_CONST(tbl) (*(const __vector4 *)(tbl))
#define RWP_ALIGN16 __declspec(align(16))
static __forceinline __vector4 RwpSplatF(float f) { __vector4 t = { f }; return __vspltw(t, 0); }
// a float splat through its stack slot: stfs + lvlx + vspltw (the form most rw::physics code uses)
static __forceinline __vector4 RwpSplatLX(float f) { return __vspltw(__lvlx(&f, 0), 0); }
#define VSPLAT_LX(f) RwpSplatLX(f)
// rw::math VecFloat(const float &): lvlx straight from the float's own address (a field or a literal)
static __forceinline __vector4 RwpSplatRef(const float &f) { return __vspltw(__lvlx(&f, 0), 0); }
#define VSPLAT_REF(x) RwpSplatRef(x)
#define RWP_U32 unsigned int
#define RWP_U16 unsigned short
#define RWP_U8 unsigned char
#define RWP_S32 int
#define RWP_F32 float
#define RWP_VEC __vector4
#define RWP_PTR(T) T *
#define RWP_FIELD_PTR(T) T *
#define RWP_CAST(T, p) ((T *)(p))
#define RWP_ADDR(field) (&(field))
#define RWP_ADDR_OF(p) ((unsigned int)(size_t)(p))
#define RWP_FROM_ADDR(T, a) ((T *)(size_t)(a))
#define RWP_SELF_PTR(T, x) ((T *)&(x))
#if defined(HORUS_NATIVE)       // host tests map the xex image at its own address
#define RWP_GLOBAL_VEC(addr, sym) (*(const __vector4 *)(size_t)(addr))
#else
#define RWP_GLOBAL_VEC(addr, sym) (sym)
#endif
#define RWP_LIST_GET(T, list, n) (((T **)(size_t)(list))[n])
#define RWP_LOAD_PTR(T, hostp) (*(T * const *)(hostp))
#define RWP_LIST_SET(list, n, value) (((unsigned int *)(size_t)(list))[n] = (unsigned int)(value))
#define VLANE_F(v, i) ((v).vector4_f32[i])
#define VLANE_U(v, i) ((v).vector4_u32[i])
#define VCR6_ALL(m) (((m).vector4_u32[0] & (m).vector4_u32[1] & (m).vector4_u32[2] & (m).vector4_u32[3]) == 0xffffffffu)
#if defined(_M_PPC)
// every lane true, from the record-form compare's CR6 (vcmp*. + mfocrf, like XMComparisonAllTrue)
static __forceinline bool RwpAllGe(__vector4 a, __vector4 b) { unsigned int cr; __vcmpgefpR(a, b, &cr); return (cr & 0x80) == 0x80; }
static __forceinline bool RwpAllGt(__vector4 a, __vector4 b) { unsigned int cr; __vcmpgtfpR(a, b, &cr); return (cr & 0x80) == 0x80; }
static __forceinline bool RwpAllEq(__vector4 a, __vector4 b) { unsigned int cr; __vcmpeqfpR(a, b, &cr); return (cr & 0x80) == 0x80; }
#define VALL_GE(a, b) RwpAllGe(a, b)
#define VALL_GT(a, b) RwpAllGt(a, b)
#define VALL_EQ(a, b) RwpAllEq(a, b)
#else
#define VALL_GE(a, b) VCR6_ALL(__vcmpgefp(a, b))
#define VALL_GT(a, b) VCR6_ALL(__vcmpgtfp(a, b))
#define VALL_EQ(a, b) VCR6_ALL(__vcmpeqfp(a, b))
#endif
#define VLANE_SET4(v, x, y, z, w) ((v).vector4_f32[0] = (x), (v).vector4_f32[1] = (y), (v).vector4_f32[2] = (z), (v).vector4_f32[3] = (w))
#define VLANE_SETW(v, f) ((v).vector4_f32[3] = (f))
#endif
