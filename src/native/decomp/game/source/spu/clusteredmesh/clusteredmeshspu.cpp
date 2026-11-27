// UnitGetOverlappingGPInstances (sk83_na_m 0x828fdb48, 0xed4; clusteredmeshspu.cpp) and its quad
// helper (0x828fcc00): decode one ClusteredMesh unit (a triangle or a quad) into GPTriangle
// instances in the caller's space. The hottest collision leaf in gameplay (the RipTriangles
// functors of the Sk8 collision jobs call it per unit). Hand-decompiled from the sk83 code with
// Skate 2's PDB types (ClusteredMeshCluster, GPInstance) as the guide.
//
// Built for guest memory only (RWP_GUEST): units are byte streams and compressed vertices are
// unaligned big-endian halfwords, read straight from guest memory. Returns false (and touches
// nothing) where the original would assert (offset past the unit data, unit type > 3, or a
// unit that is neither a triangle nor a quad); the caller then runs the recompiled function.
//
// Unit layout: byte 0 = type (low nibble: 1 triangle, 2 quad, 3 list) | 0x20 edge cosines |
// 0x40 group id | 0x80 surface id; [list count]; vertex indices (prims + 2); edge cosine bytes
// (prims + 2, when 0x20); group id (1 or 2 bytes, little-endian); surface id (likewise).
// Vertices per cluster compressionMode: 1 = uint16 xyz + the int offset in vertexArray[0],
// 2 = int32 xyz, else Vector3; modes 1/2 are scaled by vertexCompressionGranularity.
#ifndef RWP_GUEST
#error clusteredmesh_unit.cpp is built for guest memory only (RWP_GUEST)
#endif
#include "rwp_config.h"

namespace rw { namespace collision {
using namespace ::rwp;

RWP_ALIGN16 static const unsigned int sUnitConst[][4] =
{
    { 0x00010203, 0x14151617, 0x00010203, 0x00010203 },   // (a.x, b.y, .., ..)
};

// RETAIL (skate3recomp): retail's table is at 0x82fd58a0 (its UnitGetOverlappingGPInstances,
// sub_82778238, builds lis 0x82fd + 22640, + 48); the dev build's is 0x832e2d20.
static const u32 kTriangleVolumeMethods = 0x82fd58a0;     // GPInstance::VolumeMethods for TRIANGLE
static const float kPiSquared = 9.86960506439209f;        // edge cosine scale (0x821d0a40)
static const float kDegenerateNormal = 1.17549435e-38f;   // FLT_MIN (0x821ce318)

// a x b in the permuted form rw::math vpu Cross uses
static __forceinline __vector4 UCross(__vector4 a, __vector4 b)
{
    return __vpermwi(__vnmsubfp(__vpermwi(a, 0x63), b, __vmulfp(a, __vpermwi(b, 0x63))), 0x63);
}
// 1/sqrt(d), one or two Newton steps
static __forceinline __vector4 UInvSqrt1(__vector4 d, __vector4 half, __vector4 one)
{
    __vector4 e = __vrsqrtefp(d);
    return __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
}
static __forceinline __vector4 UInvSqrt2(__vector4 d, __vector4 half, __vector4 one)
{
    __vector4 e = UInvSqrt1(d, half, one);
    return __vmaddfp(__vmulfp(e, half), __vnmsubfp(d, __vmulfp(e, e), one), e);
}
// 1/x, two Newton steps, times one
static __forceinline __vector4 URecip2(__vector4 x, __vector4 one)
{
    __vector4 r = __vrefp(x);
    r = __vmaddfp(r, __vnmsubfp(r, x, one), r);
    r = __vmaddfp(r, __vnmsubfp(r, x, one), r);
    return __vmulfp(r, one);
}
// p transformed by the affine rows of m
static __forceinline __vector4 UTransform(const __vector4 m[4], __vector4 p)
{
    return __vmaddfp(m[2], __vspltw(p, 2), __vmaddfp(m[1], __vspltw(p, 1), __vmaddfp(m[0], __vspltw(p, 0), m[3])));
}
// one cluster vertex in cluster space
static __forceinline __vector4 UVertex(const u8 *base, u32 cluster, u32 mode, u32 index, __vector4 offset,
                                       __vector4 granularity)
{
    if (mode == 1)
    {
        __vector4 h = __vmrghh(__vspltisb(0), LVU(base, cluster + 0x1c + 6 * index));
        return __vmulfp(__vcfsx(__vaddsws(h, offset), 0), granularity);
    }
    if (mode == 2)
        return __vmulfp(__vcfsx(LVU(base, cluster + 0x10 + 12 * index), 0), granularity);
    return LVX(cluster + (index + 1) * 16);
}
// 1 - pi^2 / (8 << bits): an edge's cosine limit from its packed byte
static __forceinline float UEdgeCos(u32 byte)
{
    const int k = (int)(8u << (byte & 0x1f));
    return 1.0f - kPiSquared / (float)(double)k;
}

// quad helper (0x828fcc00): cosine between the normals of (p0, p1, p2) and (p3, p2, p1), 1 when
// either is degenerate; *convex = 0x20 when the quad folds the convex way across p1-p2
static float UQuadDiagonal(u8 *convex, __vector4 p0, __vector4 p1, __vector4 p2, __vector4 p3)
{
    __vector4 n2 = UCross(__vsubfp(p2, p3), __vsubfp(p1, p3));
    __vector4 n1 = UCross(__vsubfp(p1, p0), __vsubfp(p2, p0));
    __vector4 fold = __vmsum3fp(__vsubfp(p2, p1), UCross(n1, n2));
    *convex = VCR6_ALL(__vcmpgtfp(fold, VSPLAT_F(0.0f))) ? 0x20 : 0;
    const float l1 = VLANE_F(__vmsum3fp(n1, n1), 0), l2 = VLANE_F(__vmsum3fp(n2, n2), 0);
    if (l1 <= kDegenerateNormal || l2 <= kDegenerateNormal)     // ordered compares: NaN goes on
        return 1.0f;
    const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
    __vector4 e1 = UInvSqrt2(VSPLAT_F(l1), half, one);
    __vector4 e2 = UInvSqrt2(VSPLAT_F(l2), half, one);
    __vector4 d = VSPLAT_F(VLANE_F(__vmsum3fp(n1, n2), 0));
    return VLANE_F(__vmulfp(__vmulfp(e1, d), e2), 0);
}

// one GPTriangle (GPInstance, 0xc0 bytes): vertices a, b, c, unit normal, edge directions and
// lengths, edge data, flags, tags, the triangle volume methods
static void UStoreTriangle(u8 *base, u32 inst, __vector4 a, __vector4 b, __vector4 c, __vector4 normal,
                           float edge0, float edge1, float edge2, u32 flags, u32 userTag, u32 volumeTag)
{
    const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
    const __vector4 pack = RWP_CONST(sUnitConst[0]);
    STVX(inst + 0x00, a);                       // mPos (vertex 0)
    ST_U32(inst + 0x90, 3);                     // mVolumeType = TRIANGLE
    ST_U32(inst + 0x84, volumeTag);             // mVolumeTag
    ST_U8(inst + 0x8c, 1);                      // mNumFaceNormals
    ST_U8(inst + 0x8d, 3);                      // mNumEdgeDirections
    ST_U32(inst + 0x88, userTag);               // mUserTag (surface << 16 | group)
    STVX(inst + 0x20, b);                       // vertices 1, 2 (in mFaceNormals[1..2])
    STVX(inst + 0x30, c);
    ST_F32(inst + 0x80, 0.0f);                  // mFatness
    STVX(inst + 0x10, normal);                  // mFaceNormals[0]
    ST_F32(inst + 0x98, edge0);                 // mEdgeData
    ST_F32(inst + 0x9c, edge1);
    ST_F32(inst + 0xa0, edge2);
    ST_U32(inst + 0x94, flags);                 // mFlags
    __vector4 ca = __vsubfp(c, a), bc = __vsubfp(b, c), ab = __vsubfp(a, b);
    __vector4 inv = __vrlimi(__vperm(UInvSqrt1(__vmsum3fp(ca, ca), half, one), UInvSqrt1(__vmsum3fp(bc, bc), half, one), pack),
                             UInvSqrt1(__vmsum3fp(ab, ab), half, one), 2, 0);
    __vector4 i0 = __vspltw(inv, 0), i1 = __vspltw(inv, 1), i2 = __vspltw(inv, 2);
    STVX(inst + 0x40, __vmulfp(ca, i0));        // mEdgeDirections
    STVX(inst + 0x50, __vmulfp(bc, i1));
    STVX(inst + 0x60, __vmulfp(ab, i2));
    STVX(inst + 0x70, __vrlimi(__vperm(URecip2(i0, one), URecip2(i1, one), pack), URecip2(i2, one), 2, 0));  // mDimensions
    for (u32 k = 0; k < 16; k += 4)             // mVolumeMethods
        ST_U32(inst + 0xa4 + k, LD_U32(kTriangleVolumeMethods + k));
}

static __forceinline __vector4 UNormal(__vector4 a, __vector4 b, __vector4 c)
{
    const __vector4 one = __vcfsx(__vspltisw(1), 0), half = __vcfsx(__vspltisw(1), 1);
    __vector4 n = UCross(__vsubfp(b, a), __vsubfp(c, a));
    return __vmulfp(n, UInvSqrt2(__vmsum3fp(n, n), half, one));
}

// The decode. numPrimitives receives 1 or 2; returns the unit's size in bytes.
bool UnitGetOverlappingGPInstances(u32 *result, u32 cluster, u32 offset, u32 transform, u32 instances,
                                   u32 numPrimitivesAddr, u32 groupIdSize, u32 surfaceIdSize, float granularity,
                                   u32 flagsArg, u32 volumeTag)
{
    u8 *const base = rwp::g_base;
    if (offset >= LD_U16(cluster + 2))          // unitDataSize
        return false;
    const u32 unit = cluster + (LD_U16(cluster + 4) + 1) * 16 + offset;   // vertexArray[unitDataStart]
    const u32 b0 = LD_U8(unit);
    const u32 type = b0 & 0xf;
    if (type > 3)
        return false;
    const u32 verts = unit + 1 + (type == 3);
    const u32 numPrims = (type == 2 ? 2 : 0) + (type == 1 ? 1 : 0) + (type == 3 ? LD_U8(unit + 1) : 0);
    if (numPrims != 1 && numPrims != 2)
        return false;
    const u32 edgeCos = verts + numPrims + 2;
    u32 p = edgeCos + ((b0 & 0x20) ? numPrims + 2 : 0);
    u32 groupId = 0, surfaceId = 0;
    if (b0 & 0x40)
    {
        u32 lo = LD_U8(p++), hi = 0;
        if (groupIdSize == 2) hi = LD_U8(p++) << 8;
        groupId = hi + lo;
    }
    if (b0 & 0x80)
    {
        u32 lo = LD_U8(p++), hi = 0;
        if (surfaceIdSize == 2) hi = LD_U8(p++) << 8;
        surfaceId = hi + lo;
    }
    const u32 size = p - unit;
    const u32 userTag = (surfaceId << 16) | groupId;
    const u32 mode = LD_U8(cluster + 0xc);      // compressionMode
    const __vector4 vofs = LVX(cluster + 0x10);
    const __vector4 gran = VSPLAT_F(granularity);
    const __vector4 m[4] = { LVX(transform), LVX(transform + 0x10), LVX(transform + 0x20), LVX(transform + 0x30) };
    const u32 fa = flagsArg & 0x10;             // GPInstance::FLAG_TRIANGLEONESIDED from the caller

    if (numPrims == 1)
    {
        __vector4 a = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 0), vofs, gran));
        __vector4 b = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 1), vofs, gran));
        __vector4 c = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 2), vofs, gran));
        __vector4 normal = UNormal(a, b, c);
        float e0 = 0.0f, e1 = 0.0f, e2 = 0.0f;
        u32 flags = 0x1e0;                      // FLAG_TRIANGLEDEFAULT
        if (type == 0)
            flags = b0 & 0xf0;
        else if (b0 & 0x20)
        {
            const u32 c0 = LD_U8(edgeCos), c1 = LD_U8(edgeCos + 1), c2 = LD_U8(edgeCos + 2);
            e0 = UEdgeCos(c0);
            e1 = UEdgeCos(c1);
            e2 = UEdgeCos(c2);
            u32 t = (c1 & ~0x80u) | ((c2 << 1) & 0x80);
            t = (c0 & ~0x180u) | ((t << 1) & 0x180);
            t = ((t << 1) & 0x380) | (c2 & 0x20);
            t = (t << 1) | (c1 & 0x20);
            t = (t << 1) | (c0 & 0x20);
            flags = t | fa | 0x101;
        }
        UStoreTriangle(base, instances, a, b, c, normal, e0, e1, e2, flags, userTag, volumeTag);
    }
    else
    {
        __vector4 p0 = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 0), vofs, gran));
        __vector4 p1 = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 1), vofs, gran));
        __vector4 p2 = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 2), vofs, gran));
        __vector4 p3 = UTransform(m, UVertex(base, cluster, mode, LD_U8(verts + 3), vofs, gran));
        __vector4 n1 = UNormal(p0, p1, p2), n2 = UNormal(p3, p2, p1);
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, b0e = 0.0f, b1 = 0.0f, b2 = 0.0f;
        u32 flags1 = 0x1e0, flags2 = 0x1e0;
        if (b0 & 0x20)
        {
            u8 convex = 0;
            const float diag = UQuadDiagonal(&convex, p0, p1, p2, p3);
            const u32 c0 = LD_U8(edgeCos), c1 = LD_U8(edgeCos + 1), c2 = LD_U8(edgeCos + 2), c3 = LD_U8(edgeCos + 3);
            a0 = UEdgeCos(c0); a1 = diag; a2 = UEdgeCos(c2);
            b0e = UEdgeCos(c3); b1 = diag; b2 = UEdgeCos(c1);
            const u32 s1 = (c1 & 0x40) | convex, s2 = (c2 & 0x40) | convex;
            u32 t = ((s1 & 0xff) & ~0x80u) | ((c2 << 1) & 0x80);
            t = (c0 & ~0x180u) | ((t << 1) & 0x180);
            t = (((t << 1) & 0x380) | (c2 & 0x20)) << 1;
            t = ((t | (s1 & 0x20)) << 1) | (c0 & 0x20);
            flags1 = t | fa | 0x101;
            u32 u = ((s2 & 0xff) & ~0x80u) | ((c1 << 1) & 0x80);
            u = (c3 & ~0x180u) | ((u << 1) & 0x180);
            u = ((((u << 1) & 0x380) | (c1 & 0x20)) << 1) | (s2 & 0x20);
            u = (u << 1) | (c3 & 0x20);
            flags2 = u | fa | 0x101;
        }
        UStoreTriangle(base, instances, p0, p1, p2, n1, a0, a1, a2, flags1, userTag, volumeTag);
        UStoreTriangle(base, instances + 0xc0, p3, p2, p1, n2, b0e, b1, b2, flags2, userTag, volumeTag);
    }
    ST_U32(numPrimitivesAddr, numPrims);
    *result = size;
    return true;
}

} }
