// Buffer touch events for the persistent static world items (skate3_native_render_scene_static_persist).
// RETAIL PORT of sk83_na_m src/native/sk83_buffer_events.cpp.
//
// A persistent item is rebuilt when one of its objects (vertex / index buffer, mesh) was touched
// after it was built; these hooks report every game path that can (re)point, fill or free a
// buffer. Object identity: a renderengine::VertexBuffer / IndexBuffer starts with the D3D buffer
// header, so the D3D resource argument and the renderengine object are the same address.
//
// Retail is an LTCG build: the XDK D3D*Buffer::Lock / Unlock wrappers (dev 82D96BB0 / 82D96C00 /
// 82D95C70 / 82D95C80) and the renderengine Lock wrappers are inlined into their callers, so this
// hooks what they all end in -- D3D::LockResource (retail 82B7B3E0, dev 82D967E8) and
// D3D::UnlockResource (retail 82B7B6E0, dev 82D95990), r3 = the resource. That also reports
// texture locks: harmless extra events (they only force early rebuilds of nothing).
// renderengine Release / Destruct / Initialize keep their retail symbols. VertexBuffer::Fixup:
// retail 82A772B8 (endian-converts the fetch words at +24, then the object helper 82A779B8; dev
// 82C76B68 calls the generic serializer instead), r3 = the object in both.
#include "generated/skate3_init.h"

#include <cstdint>

namespace skate3::native_scene {
void NoteBufferDirty(uint32_t obj);
}

namespace {
inline void Touch(uint32_t obj) { skate3::native_scene::NoteBufferDirty(obj); }
}  // namespace

// Reported both before (pointer known) and after (writes done) the original.
#define BUFFER_EVENT_HOOK(sym)                \
  extern "C" REX_FUNC(sym) {                  \
    const uint32_t obj = ctx.r3.u32;          \
    Touch(obj);                               \
    __imp__##sym(ctx, base);                  \
    Touch(obj);                               \
  }

// XDK D3D runtime: CPU access to resource contents (all buffer Lock / Unlock paths).
BUFFER_EVENT_HOOK(rex_D3D_LockResource_YAPAXPAUD3DResource_KW4_D3DBLOCKTYPE_KKKKKKK_Z_82B7B3E0)
BUFFER_EVENT_HOOK(sub_82B7B6E0)  // D3D::UnlockResource

// renderengine (this = r3).
BUFFER_EVENT_HOOK(rex_renderengine_VertexBuffer_Release_QAAXXZ_82A79958)
BUFFER_EVENT_HOOK(rex_renderengine_IndexBuffer_Release_QAAXXZ_82A778E0)
BUFFER_EVENT_HOOK(rex_renderengine_VertexBuffer_Destruct_SAXPAV12_Z_82A798D8)
BUFFER_EVENT_HOOK(rex_renderengine_IndexBuffer_Destruct_SAXPAV12_Z_82A77978)
BUFFER_EVENT_HOOK(sub_82A772B8)  // renderengine::VertexBuffer::Fixup (arena load)

// Initialize(Resource const& r3, Parameters const& r4): the object is the resource's first word;
// returns it in r3.
#define BUFFER_INIT_HOOK(sym)                                        \
  extern "C" REX_FUNC(sym) {                                         \
    const uint32_t obj = ctx.r3.u32 ? REX_LOAD_U32(ctx.r3.u32) : 0u; \
    Touch(obj);                                                      \
    __imp__##sym(ctx, base);                                         \
    Touch(obj);                                                      \
    Touch(ctx.r3.u32);                                               \
  }
BUFFER_INIT_HOOK(rex_renderengine_VertexBuffer_Initialize_SAPAV12_ABVResource_rw_ABUParameters_12_Z_82A79830)
BUFFER_INIT_HOOK(rex_renderengine_IndexBuffer_Initialize_SAPAV12_ABVResource_rw_ABUParameters_12_Z_82A777D8)
