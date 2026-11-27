// Sk8::Render layouts as sk83 uses them (decoded from the sk83 code; names from the Skate 2 PDB
// where the field has a Skate 2 counterpart -- sk83 grew several of these structs).
#ifndef SK8_RENDER_TYPES_H
#define SK8_RENDER_TYPES_H
#include "rwp_guestcall.h"

namespace pegasus { namespace tRModelData {
struct tMeshData                        // only the fields the draw code reads
{
    RWP_U32 pad0[12];
    RWP_U32 m_pIndexBuffer;             // 0x30 D3DIndexBuffer*
    RWP_U32 m_pVertexBuffer;            // 0x34 D3DVertexBuffer*
};
} }

namespace renderengine {
struct DrawIndexedParameters            // size 0x10: D3DDevice_DrawIndexedVertices r4..r7
{
    RWP_U32 primitiveType;
    RWP_U32 baseVertexIndex;
    RWP_U32 startIndex;
    RWP_U32 indexCount;
};
}

namespace Sk8 { namespace Render {

struct MeshContext                      // sk83 (Skate 2: 0x34)
{
    RWP_FIELD_PTR(RWP_FIELD_PTR(pegasus::tRModelData::tMeshData)) m_pMesh;   // 0x00 (one more indirection than Skate 2)
    RWP_U32 m_pEffectList;              // 0x04 the effect instance (vtable; +0x44 pass count, +0x64 VertexProgramState*)
    RWP_U32 m_pSimpleVPState;           // 0x08 VertexProgramState* (z prepass)
    RWP_U32 pad0c;                      // 0x0c
    RWP_U32 m_pWorldMatrix;             // 0x10 Matrix44* (16-byte aligned)
    RWP_U32 pad14;                      // 0x14
    RWP_F32 m_DistFromCam;              // 0x18 (PrepZPrePass writes it)
#if defined(HORUS_NATIVE)
    RWP_U8 pad1f[3];
    RWP_U8 m_PassCount;
#else
    RWP_U8 m_PassCount;                 // 0x1c passes still to draw (multipass meshes)
    RWP_U8 pad1d[3];
#endif
    RWP_U32 pad20;                      // 0x20
    RWP_U32 m_uiNumVisibleIslands;      // 0x24 draws (DrawIndexedParameters) this frame
    RWP_U32 m_uiShadowNumVisibleIslands;    // 0x28 shadow draws this frame
    RWP_U32 pad2c;                      // 0x2c
    RWP_U32 m_aabb;                     // 0x30 pegasus::tAABB* (min, max as two vectors)
    RWP_FIELD_PTR(RWP_U32) m_uiVisibleIslandIndices;               // 0x34
    RWP_FIELD_PTR(RWP_U32) m_uiShadowVisibleIslandIndices;         // 0x38
    RWP_FIELD_PTR(renderengine::DrawIndexedParameters) m_islandDrawList;   // 0x3c
    RWP_U32 m_pRenderTest;              // 0x40 optional object (vtable slot 0: called before the draw)
    RWP_U32 m_uiStatA;                  // 0x44 added to a render stat per draw
    RWP_U32 m_uiStatB;                  // 0x48 added to a render stat per draw
};

struct SortKey                          // size 8: {sort key, MeshContext*}
{
    RWP_U32 key;
    RWP_FIELD_PTR(MeshContext) mpMshCntxt;
};

// eastl::vector (one declarator per field: RWP_FIELD_PTR is a plain `T *` in the XDK build)
struct SortKeyVector
{
    RWP_FIELD_PTR(SortKey) mpBegin;
    RWP_FIELD_PTR(SortKey) mpEnd;
    RWP_FIELD_PTR(SortKey) mpCapacity;
};
struct MeshContextVector
{
    RWP_FIELD_PTR(RWP_FIELD_PTR(MeshContext)) mpBegin;
    RWP_FIELD_PTR(RWP_FIELD_PTR(MeshContext)) mpEnd;
    RWP_FIELD_PTR(RWP_FIELD_PTR(MeshContext)) mpCapacity;
};

struct PtrVector                        // eastl::vector<T*>
{
    RWP_U32 mpBegin;
    RWP_U32 mpEnd;
    RWP_U32 mpCapacity;
};

struct SceneRenderView                  // the fields the draw code uses
{
    RWP_U32 pad0[0x18 / 4];
    RWP_U32 m_pCamera;                      // 0x18 (+0x260: the near plane PrepZPrePass measures against)
    RWP_U32 pad1c[(0x3eec - 0x1c) / 4];
    PtrVector m_PostRenderEntities;         // 0x3eec (vtable +0x4c called after the meshes)
    RWP_U32 pad3ef8[(0x4ed0 - 0x3ef8) / 4];
    SortKeyVector m_SortKeys;               // 0x4ed0
    RWP_U32 pad4edc;
    MeshContextVector m_ZPrePassMeshes;     // 0x4ee0 (PrepZPrePass fills it)
    RWP_U32 pad4eec[(0x4fec - 0x4eec) / 4];
    MeshContextVector m_MultipassMeshes;    // 0x4fec
    RWP_U32 pad4ff8[(0x5718 - 0x4ff8) / 4];
    RWP_U32 m_Attribs;                      // 0x5718 Attrib::Instance (z-prepass distance attribute)
    RWP_U32 pad571c[(0x5770 - 0x571c) / 4];
    RWP_S32 m_iZPrePassGroup;               // 0x5770 effects with this +0x38 group go into the z prepass
};

} }

#if !defined(_M_PPC)
#include <cstddef>
static_assert(sizeof(Sk8::Render::MeshContext) == 0x4c, "MeshContext");
static_assert(sizeof(Sk8::Render::SortKey) == 8, "SortKey");
static_assert(offsetof(Sk8::Render::SceneRenderView, m_iZPrePassGroup) == 0x5770, "SceneRenderView");
#endif
#endif
