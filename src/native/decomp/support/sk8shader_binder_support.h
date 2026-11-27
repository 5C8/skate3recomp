// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (1); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82b58838 82c72f60 82dad568
// Declarations the generated sk8shader binders (gen/source/*.cpp, tools/gen_shader_binders.py) use.
#ifndef SK8SHADER_BINDER_SUPPORT_H
#define SK8SHADER_BINDER_SUPPORT_H
#include "rwp_guestcall.h"

namespace StateShadow { void SetTexture(unsigned int tex, unsigned int stage); }
#if !defined(RWP_GUEST)
unsigned int RwpVCall(unsigned int obj, unsigned int slot, ...);
extern "C" void XMemCpy(unsigned int dst, unsigned int src, unsigned int n);
namespace Device { void SetSamplerState(unsigned int s, unsigned int stage); }
#endif

// guest memory as the binders use it (64-bit register values; addresses are their low 32 bits)
#define BLD32(a) ((unsigned long long)RWP_LD32((unsigned int)(a)))
#define BLD64(a) ((unsigned long long)RWP_LD64((unsigned int)(a)))
#define BST32(a, v) RWP_ST32((unsigned int)(a), (unsigned int)(v))
#define BST64(a, v) RWP_ST64((unsigned int)(a), (unsigned long long)(v))
#define BINDER_XMEMCPY(d, s, n) RWP_CALL(0x82edf460, XMemCpy, (unsigned int)(d), (unsigned int)(s), (unsigned int)(n))
#define BINDER_SETTEXTURE(t, stage) StateShadow::SetTexture((unsigned int)(t), (unsigned int)(stage))
#define BINDER_SETSAMPLER(s, stage) RWP_CALL(0x5fff0000, Device::SetSamplerState, (unsigned int)(s), (unsigned int)(stage))

// the effect-type check's failure: xcore::FailAssert(0, file, line) + DebugBreak
#if defined(RWP_GUEST)
#define BINDER_ASSERT(file, line) (RWP_CALL(0x5fff0000, FailAssert, 0u, file, (unsigned int)(line)), RWP_CALL0(0x5fff0000, DebugBreak))
#else
#define BINDER_ASSERT(file, line) __debugbreak()
#endif
#endif
