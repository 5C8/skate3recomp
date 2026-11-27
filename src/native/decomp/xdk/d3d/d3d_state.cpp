// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (6); comments keep the dev addresses.
// The XDK D3D runtime's state setters the render thread calls per draw (Microsoft's library code
// linked into the game; not a game source -- the original path is unknown):
//   D3DDevice_SetTexture        sk83_na_m 0x82d99c48 (0x18c)
//   D3DDevice_SetStreamSource   sk83_na_m 0x82d94340 (0x120)
//   D3DDevice_SetIndices        sk83_na_m 0x82d944e8 (0x90)
//   D3D::SetPending_RenderStates     sk83_na_m 0x82d9f618 (0xc0)
//   D3D::SetPending_FetchConstants   sk83_na_m 0x82d9f858 (0x158)
//   D3DDevice_SetVertexShader   sk83_na_m 0x82d9dfd8 (0x1cc)
//   D3DDevice_SetPixelShader    sk83_na_m 0x82d9de18 (0x1bc)
//   D3DDevice_SetScissorRect    sk83_na_m 0x82d94248 (0xf8)
//   D3D::SetSurfaceClip         sk83_na_m 0x82d91c90 (0x2f0)
//   D3D::SetViewport            sk83_na_m 0x82d94930 (0x258)
// They write the stage's fetch constant into the device's shadow copy, OR the caller's 64-bit
// dirty mask into the device, and hand the resource they replace to the device's fence /
// reference list (D3D::MakeBufferResourceSpace when that list is full). Field names follow
// what the code does with them (the XDK headers do not describe CDevice internals).
// Verified with sk83_na_m --sk83_replay.
#include "rwp_guestcall.h"
#include "rwp_ppc_ops.h"

namespace D3D {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif

#if !defined(RWP_GUEST)
unsigned int MakeBufferResourceSpace(...), SetPending_Split(...), CDevice_KickOff(...), CrtMemcpy(...);
#endif

// The resource a slot held before: with a pending fence (+0x2a9c) it gets that fence; else, when
// it is one the device tracks (+0x2aa0 & its first word), an 8-byte reference record goes into
// the buffer-resource list (+0x35a4, limit +0x35a8). The record's top bit comes from the caller's
// frame word at +0x50 (the original builds it in place there: rlwimi into the uninitialised word).
static __forceinline void ReleaseResource(unsigned int dev, unsigned int old, unsigned int frame50)
{
    if (!old)
        return;
    const unsigned int fence = RWP_LD32(dev + 0x2a9cu);
    if (fence)
    {
        RWP_ST32(old + 8u, fence);
        return;
    }
    if (!(RWP_LD32(dev + 0x2aa0u) & RWP_LD32(old)))
        return;
    unsigned int rec = RWP_LD32(dev + 0x35a4u);
    if (rec >= RWP_LD32(dev + 0x35a8u))
        rec = RWP_CALL(0x82b85888, MakeBufferResourceSpace, dev);
    unsigned int w = RWP_LD32(frame50);
    w = RwpRlwimi(w, old, 30, 2, 31);
    w = RwpRlwinm(w, 0, 2, 0);
    RWP_ST32(frame50 + 4u, 0xffffffffu);
    RWP_ST32(frame50, w);
    RWP_ST64(rec, ((unsigned long long)w << 32) | 0xffffffffull);
    RWP_ST32(dev + 0x35a4u, rec + 8u);
}

void SetTexture(unsigned int dev, unsigned int stage, unsigned int tex, unsigned long long dirty)
{
    RWP_GUEST_FRAME(frame, 0xc0);
    const unsigned int slot = dev + 4u * (stage + 0xc6cu);
    const unsigned int old = RWP_LD32(slot);
    const unsigned int f = dev + (stage + 0x30u) * 0x18u;       // the stage's fetch constant (6 words)
    if (tex)
    {
        const unsigned int t30 = RWP_LD32(tex + 0x30u), t20 = RWP_LD32(tex + 0x20u), t2c = RWP_LD32(tex + 0x2cu);
        const unsigned int t1c = RWP_LD32(tex + 0x1cu), t24 = RWP_LD32(tex + 0x24u), t28 = RWP_LD32(tex + 0x28u);
        const unsigned int f0 = RWP_LD32(f), fc = RWP_LD32(f + 0xcu), f14 = RWP_LD32(f + 0x14u);
        const unsigned int f10 = RWP_LD32(f + 0x10u), f4 = RWP_LD32(f + 4u);
        const unsigned int ds = dev + stage;
        RWP_ST32(f + 8u, t24);
        unsigned int w4 = (((t20 >> 20) + 0x200u) & 0x1000u) + (t20 & 0x1fffffffu);
        unsigned int w14 = (((t30 >> 20) + 0x200u) & 0x1000u) + RwpRlwinm(t30, 0, 3, 22);
        unsigned int w10 = RwpRlwimi(t2c, f10, 0, 30, 21);
        unsigned int maxMip = RWP_LD8(ds + 0x2f5eu);
        const unsigned int w0 = RwpRlwimi(t1c, f0, 0, 10, 21);
        w4 = RwpRlwimi(w4, f4, 0, 20, 20);
        RWP_ST32(f + 0x10u, w10);
        const unsigned int wc = RwpRlwimi(t28, fc, 0, 1, 12);
        RWP_ST32(f, w0);
        w14 = RwpRlwimi(w14, f14, 0, 23, 31);
        RWP_ST32(f + 4u, w4);
        const unsigned int lod = RwpRlwinm(t2c, 30, 28, 31);
        RWP_ST32(f + 0xcu, wc);
        RWP_ST32(f + 0x14u, w14);
        if (lod > maxMip)
            maxMip = lod;
        w10 = RwpRlwimi(w10, maxMip, 2, 26, 29);
        unsigned int minMip = RWP_LD8(ds + 0x2f78u);
        const unsigned int lod2 = RwpRlwinm(RWP_LD32(tex + 0x2cu), 26, 28, 31);
        RWP_ST32(f + 0x10u, w10);
        if (lod2 < minMip)
            minMip = lod2;
        const unsigned long long mask = RWP_LD64(dev + 0x18u);
        w10 = RwpRlwimi(w10, minMip, 6, 22, 25);
        RWP_ST32(f + 0x10u, w10);
        RWP_ST64(dev + 0x18u, mask | dirty);
    }
    else
        RWP_ST32(f, RWP_LD32(f) & 0xfffffffcu);
    RWP_ST32(slot, tex);
    ReleaseResource(dev, old, FRAME_AT(frame, 0x50));
}

void SetStreamSource(unsigned int dev, unsigned int stream, unsigned int vb, unsigned int offset, unsigned int stride,
                     unsigned long long dirty)
{
    RWP_GUEST_FRAME(frame, 0x90);
    if (vb)
    {
        const unsigned int a = RWP_LD32(vb + 0x18u) + offset;
        const unsigned int size = RWP_LD32(vb + 0x1cu);
        const unsigned int k = 0x11u - stream;
        RWP_ST32(dev + 8u * (k + 0xdeu), (((a >> 20) + 0x200u) & 0x1000u) + (a & 0x1fffffffu));
        RWP_ST32(dev + 8u * k + 0x6f4u, size - offset);
        RWP_ST64(dev + 0x18u, RWP_LD64(dev + 0x18u) | dirty);
    }
    const unsigned int slot = dev + 4u * (stream + 0xc57u);
    const unsigned int old = RWP_LD32(slot);
    ReleaseResource(dev, old, FRAME_AT(frame, 0x50));
    RWP_ST32(slot, vb);
    const unsigned int ds = dev + stream;
    const unsigned int words = stride >> 2;
    RWP_ST8(ds + 0x31a0u, words & 0xffu);
    if (words != 0 && words != RWP_LD8(ds + 0x2ee8u))
        RWP_ST64(dev + 0x10u, RWP_LD64(dev + 0x10u) | 0x80000ull);
}


void SetIndices(unsigned int dev, unsigned int ib)
{
    RWP_GUEST_FRAME(frame, 0x80);
    ReleaseResource(dev, RWP_LD32(dev + 0x3144u), FRAME_AT(frame, 0x50));
    RWP_ST32(dev + 0x3144u, ib);
}

static __forceinline unsigned int Clz64(unsigned long long x)
{
    unsigned int n = 0;
    if (!x)
        return 64;
    while (!(x & 0x8000000000000000ull))
    {
        x <<= 1;
        n++;
    }
    return n;
}
static __forceinline unsigned long long Shl64(unsigned long long x, unsigned int n) { return n >= 64 ? 0ull : x << n; }

// The device's command buffer write pointer (+0x30, pre-increment style: it points at the last
// word written) and segment end (+0x34). Every run of set bits in the dirty mask becomes one
// register-write packet: header ((count - 1) << 16 | first register), then the values.
void SetPending_RenderStates(unsigned int dev, unsigned long long mask, unsigned int reg, unsigned int src)
{
    RWP_GUEST_FRAME(frame, 0x90);
    unsigned int cur = RWP_LD32(dev + 0x30u);
    unsigned int s = src - 4u;
    do
    {
        const unsigned int lz = Clz64(mask);
        mask = Shl64(mask, lz);
        s += 4u * lz;
        const unsigned int first = lz + reg;
        const unsigned int run = Clz64(~mask);
        if (cur + 4u * run >= RWP_LD32(dev + 0x34u))
        {
            cur = RWP_CALL(0x82b83600, SetPending_Split, dev, cur, first, s, run, 1u);
            s += 4u * run;
            reg = run + first;
            mask = Shl64(mask, run);
        }
        else
        {
            reg = run + first;
            cur += 4u;
            RWP_ST32(cur, ((run - 1u) << 16) | first);
            for (unsigned int k = 0; k < run; k++)
            {
                s += 4u;
                cur += 4u;
                RWP_ST32(cur, RWP_LD32(s));
                mask <<= 1;
            }
        }
    } while (mask);
    RWP_ST32(dev + 0x30u, cur);
}

// The same for the fetch constants (6 words each, from the device's shadow copy at +0x480;
// register 0x4800 + 6 * index). Packets are 8-byte aligned (a 0x80000000 pad word otherwise),
// and the set ends with a 0x25 sync packet.
void SetPending_FetchConstants(unsigned int dev, unsigned long long mask)
{
    RWP_GUEST_FRAME(frame, 0x90);
    unsigned int cur = RWP_LD32(dev + 0x30u);
    unsigned int s = dev + 0x480u - 4u;
    unsigned int reg = 0x4800u;
    for (;;)
    {
        const unsigned int lz = Clz64(mask);
        const unsigned int end = RWP_LD32(dev + 0x34u);
        mask = Shl64(mask, lz);
        const unsigned int first = lz * 6u + reg;
        const unsigned int run = Clz64(~mask);
        const unsigned int words = run * 6u;
        s += lz * 0x18u;
        if ((words + 5u) * 4u + cur >= end)
        {
            cur = RWP_CALL(0x82b83600, SetPending_Split, dev, cur, first, s, words, 6u);
            mask = Shl64(mask, run);
            s += words * 4u;
            reg = words + first;
            if (mask)
                continue;
            const unsigned int lim = RWP_LD32(dev + 0x38u);
            RWP_ST32(dev + 0x30u, cur);
            if (cur > lim)
                cur = RWP_CALL(0x82b754c0, CDevice_KickOff, dev);
            cur += 4u; RWP_ST32(cur, 0x25000u);
            cur += 4u; RWP_ST32(cur, 0u);
            cur += 4u; RWP_ST32(cur, 0u);
            cur += 4u; RWP_ST32(cur, 0u);
            RWP_ST32(dev + 0x30u, cur);
            return;
        }
        RWP_ST32(cur + 4u, 0x80000000u);
        cur += cur & 7u;
        cur += 4u;
        RWP_ST32(cur, ((words - 1u) << 16) | first);
        reg = words + first;
        for (unsigned int k = 0; k < run; k++)
        {
            const unsigned long long a = RWP_LD64(s + 4u), b = RWP_LD64(s + 0xcu), c = RWP_LD64(s + 0x14u);
            mask <<= 1;
            s += 0x18u;
            RWP_ST64(cur + 4u, a);
            RWP_ST64(cur + 0xcu, b);
            RWP_ST64(cur + 0x14u, c);
            cur += 0x18u;
        }
        if (mask)
            continue;
        RWP_ST32(dev + 0x30u, cur + 0x10u);
        RWP_ST64(cur + 4u, 0x25ull << 44);
        RWP_ST64(cur + 0xcu, 0x25ull << 44);
        return;
    }
}


// The shader's constant patches (+0x368, a block at +0x14 from there): dirty bits it clears
// (+0) / a flag it raises (+8, device +0x20 bit 56), then three sections of {u16 register
// offset, u16 count} records, each ending at a zero count: the first is skipped (8 bytes per
// record), the second is copied into the device's constant shadow (+0x480), the third is
// applied as (old & mask) | value word pairs.
void SetVertexShader(unsigned int dev, unsigned int vs)
{
    RWP_GUEST_FRAME(frame, 0x80);
    if (vs)
        RWP_ST64(dev + 0x10u, RWP_LD64(dev + 0x10u) | 0x80000ull);
    ReleaseResource(dev, RWP_LD32(dev + 0x3248u), FRAME_AT(frame, 0x50));
    const unsigned int flags = RWP_LD8(dev + 0x2abeu);
    RWP_ST32(dev + 0x3248u, vs);
    RWP_ST8(dev + 0x2abeu, flags & 0x7fu);
    if (!vs)
        return;
    const unsigned int p = vs + 0x368u;
    if (!p)
        return;
    const unsigned int off = RWP_LD32(p + 0x14u);
    if (!off)
        return;
    const unsigned int q = off + p;
    RWP_ST64(dev, RWP_LD64(dev) & ~RWP_LD64(q));
    if (RWP_LD64(q + 8u))
        RWP_ST64(dev + 0x20u, RWP_LD64(dev + 0x20u) | (1ull << 56));
    unsigned int r = q + 0x14u;
    const unsigned int end = RWP_LD32(q + 0x10u) + r;
    const unsigned int shadow = dev + 0x480u;
    if (r >= end)
        return;
    for (;;)                                            // section 1: skipped
    {
        const unsigned int count = RWP_LD32(r) & 0xffffu;
        r += 4u;
        if (count == 0)
            break;
        r += 4u;
        if (r >= end)
            break;
    }
    while (r < end)                                     // section 2: copies
    {
        const unsigned int w = RWP_LD32(r);
        const unsigned int count = w & 0xffffu, reg = w >> 16;
        r += 4u;
        if (count == 0)
            break;
        RWP_CALL(0x82f52b30, CrtMemcpy, reg + shadow, r, count * 4u);
        r += count * 4u;
    }
    while (r < end)                                     // section 3: masked writes
    {
        const unsigned int w = RWP_LD32(r);
        unsigned int count = w & 0xffffu;
        const unsigned int reg = w >> 16;
        r += 4u;
        if (count == 0)
            return;
        unsigned int d = reg + shadow - 4u;
        do
        {
            const unsigned int mask = RWP_LD32(r), value = RWP_LD32(r + 4u);
            const unsigned int old = RWP_LD32(d + 4u);
            count = (count + 0x10000u - 2u) & 0xffffu;
            r += 8u;
            d += 4u;
            RWP_ST32(d, (old & mask) | value);
        } while (count);
    }
}


// A shader's constant patch block (q): dirty bits it clears (q+0, from the device word at
// +dirtyOff) / a flag it raises (q+8, device +0x20 bit 56), then the three record sections (see
// SetVertexShader).
static void ApplyShaderPatches(unsigned int dev, unsigned int q, unsigned int dirtyOff)
{
    RWP_ST64(dev + dirtyOff, RWP_LD64(dev + dirtyOff) & ~RWP_LD64(q));
    if (RWP_LD64(q + 8u))
        RWP_ST64(dev + 0x20u, RWP_LD64(dev + 0x20u) | (1ull << 56));
    unsigned int r = q + 0x14u;
    const unsigned int end = RWP_LD32(q + 0x10u) + r;
    const unsigned int shadow = dev + 0x480u;
    if (r >= end)
        return;
    for (;;)
    {
        const unsigned int count = RWP_LD32(r) & 0xffffu;
        r += 4u;
        if (count == 0)
            break;
        r += 4u;
        if (r >= end)
            break;
    }
    while (r < end)
    {
        const unsigned int w = RWP_LD32(r);
        const unsigned int count = w & 0xffffu, reg = w >> 16;
        r += 4u;
        if (count == 0)
            break;
        RWP_CALL(0x82f52b30, CrtMemcpy, reg + shadow, r, count * 4u);
        r += count * 4u;
    }
    while (r < end)
    {
        const unsigned int w = RWP_LD32(r);
        unsigned int count = w & 0xffffu;
        const unsigned int reg = w >> 16;
        r += 4u;
        if (count == 0)
            return;
        unsigned int d = reg + shadow - 4u;
        do
        {
            const unsigned int mask = RWP_LD32(r), value = RWP_LD32(r + 4u);
            const unsigned int old = RWP_LD32(d + 4u);
            count = (count + 0x10000u - 2u) & 0xffffu;
            r += 8u;
            d += 4u;
            RWP_ST32(d, (old & mask) | value);
        } while (count);
    }
}

// The pixel shader: the old one released, the shader dirty flags raised (device +0x10: 0x100000,
// then 0x20000), and its constant patches (+0x28 block, offset at +0x3c) applied against the
// pixel constants' dirty word (+8).
void SetPixelShader(unsigned int dev, unsigned int ps)
{
    RWP_SAVEGPRLR(28);
    RWP_GUEST_FRAME(frame, 0x80);
    ReleaseResource(dev, RWP_LD32(dev + 0x3244u), FRAME_AT(frame, 0x50));
    RWP_ST32(dev + 0x3244u, ps);
    unsigned long long f = RWP_LD64(dev + 0x10u) | 0x100000ull;
    RWP_ST64(dev + 0x10u, f);
    f |= 0x20000ull;
    RWP_ST64(dev + 0x10u, f);
    if (!ps)
        return;
    const unsigned int off = RWP_LD32(ps + 0x3cu);
    if (!off)
        return;
    ApplyShaderPatches(dev, off + ps + 0x28u, 8u);
}


// The command buffer: write pointer +0x30 (at the last word written), limit +0x38.
static __forceinline unsigned int Put(unsigned int p, unsigned int w)
{
    RWP_ST32(p + 4u, w);
    return p + 4u;
}

static __forceinline unsigned int ClipXY(int x, int y)
{
    return (((unsigned int)y << 16) & 0x3fff0000u) | ((unsigned int)x & 0x3fffu);
}

// The scissor / window clip in the command stream. Normally one packet (0x22080: the screen
// scissor, top-left / bottom-right); when the device is predicated per screen extent (+0x2abc
// bit 0x10, or bit 0x20 with the extent list +0x3268 matching the bound targets +0x3148) it
// writes, per extent (+0x327c of them: rects +0x3280, offsets +0x3370), a predicated scissor
// clipped to that extent and the window offset, then (with +0x2abf 0x20 and +0x2abc 0x40) the
// unclipped scissor for the second set, and restores the predication words (+0x3254, +0x3258).
void SetSurfaceClip(unsigned int dev, int x0, int y0, int x1, int y1)
{
    RWP_SAVEGPRLR(21);
    RWP_GUEST_FRAME(frame, 0xb0);
    RWP_NV(31, dev);
    RWP_NV(25, (long long)x0);
    RWP_NV(24, (long long)y0);
    RWP_NV(23, (long long)x1);
    RWP_NV(22, (long long)y1);
    unsigned int p = RWP_LD32(dev + 0x30u);
    if (p > RWP_LD32(dev + 0x38u))
        p = RWP_CALL(0x82b754c0, CDevice_KickOff, dev);
    const unsigned int flags = RWP_LD8(dev + 0x2abcu);
    bool perExtent;
    if (flags & 0x10u)
        perExtent = true;
    else if (!(flags & 0x20u))
        perExtent = false;
    else
    {
        perExtent = true;
        for (unsigned int k = 0; k < 5u && perExtent; k++)
        {
            const unsigned int a = RWP_LD32(dev + 0x3148u + 4u * k), b = RWP_LD32(dev + 0x3268u + 4u * k);
            if (b != a && a != 0)
                perExtent = false;
        }
    }
    if (!perExtent)
    {
        p = Put(p, 0x22080u);
        p = Put(p, 0u);
        p = Put(p, ClipXY(x0, y0));
        p = Put(p, ClipXY(x1, y1));
        RWP_ST32(dev + 0x30u, p);
        return;
    }
    RWP_NV(27, 0);
    p = Put(p, 0xc0006100u);
    RWP_NV(26, 0xffffffffc0006000ull);
    p = Put(p, 0u);
    if (RWP_LD32(dev + 0x327cu) > 0u)
    {
        unsigned int rect = dev + 0x3284u, offs = dev + 0x3374u;
        for (unsigned int k = 0, shift = 0; k < RWP_LD32(dev + 0x327cu); k++, rect += 0x10u, offs += 8u, shift += 2u)
        {
            const int a = (int)RWP_LD32(rect - 4u), b = (int)RWP_LD32(rect), c = (int)RWP_LD32(rect + 4u), d = (int)RWP_LD32(rect + 8u);
            const int ox = (int)RWP_LD32(offs - 4u), oy = (int)RWP_LD32(offs);
            int l = x0 > a ? x0 : a, t = y0 > b ? y0 : b;
            int r = x1 < c ? x1 : c, bt = y1 < d ? y1 : d;
            if (!(l < r) || !(t < bt))
                l = t = r = bt = 0;
            p = Put(p, 0xc0006000u);
            p = Put(p, (shift & 0x20u) ? 0u : (3u << (shift & 0x1fu)));
            p = Put(p, 0xc0032d01u);
            p = Put(p, 0x40080u);
            p = Put(p, ((0u - (unsigned int)oy) << 16 & 0x7fff0000u) | ((0u - (unsigned int)ox) & 0x7fffu));
            p = Put(p, ClipXY(l, t));
            p = Put(p, ClipXY(r, bt));
            RWP_ST32(dev + 0x30u, p);
            if (p > RWP_LD32(dev + 0x38u))
            {
                RWP_NV(30, rect);
                RWP_NV(29, shift);
                RWP_NV(28, offs);
                RWP_NV(27, k);
                RWP_NV(21, 0x40080u);
                p = RWP_CALL(0x82b754c0, CDevice_KickOff, dev);
            }
        }
    }
    if ((RWP_LD8(dev + 0x2abfu) & 0x20u) && (RWP_LD8(dev + 0x2abcu) & 0x40u))
    {
        p = Put(p, 0xc0006000u);
        p = Put(p, 1u);
        p = Put(p, 0xc0022d01u);
        p = Put(p, 0x40081u);
        p = Put(p, ClipXY(x0, y0));
        p = Put(p, ClipXY(x1, y1));
    }
    p = Put(p, 0xc0006000u);
    p = Put(p, RWP_LD32(dev + 0x3254u));
    p = Put(p, 0xc0006100u);
    p = Put(p, RWP_LD32(dev + 0x3258u));
    RWP_ST32(dev + 0x30u, p);
}

// The scissor rect (left, top, right, bottom) is kept (+0x3234..+0x3240); with scissoring on
// (+0x2f00) it is intersected with the viewport (+0x3218 x, +0x321c y, +0x3220 width, +0x3224
// height, converted with fctiwz through the stack as the original does), else the viewport is
// the clip. The result goes into the device's scissor registers (+0x28c4 top-left, +0x28c8
// bottom-right) and the command stream.
void SetScissorRect(unsigned int dev, unsigned int rect)
{
    RWP_SAVEGPRLR(30);
    RWP_GUEST_FRAME(frame, 0x80);
    RWP_FPU_MODE();
    RWP_ST64(FRAME_AT(frame, 0x50), RwpFctiwz((double)(float)RWP_GLOBAL(RWP_F32, dev + 0x3218u)));
    RWP_ST64(FRAME_AT(frame, 0x58), RwpFctiwz((double)(float)RWP_GLOBAL(RWP_F32, dev + 0x3220u)));
    const unsigned int r0 = RWP_LD32(rect);
    int x = (int)RWP_LD32(FRAME_AT(frame, 0x54));
    const int w = (int)RWP_LD32(FRAME_AT(frame, 0x5c));
    const unsigned int r1 = RWP_LD32(rect + 4u), r2 = RWP_LD32(rect + 8u), r3 = RWP_LD32(rect + 0xcu);
    RWP_NV(31, r2);
    RWP_NV(30, r3);
    RWP_ST64(FRAME_AT(frame, 0x58), RwpFctiwz((double)(float)RWP_GLOBAL(RWP_F32, dev + 0x3224u)));
    RWP_ST64(FRAME_AT(frame, 0x50), RwpFctiwz((double)(float)RWP_GLOBAL(RWP_F32, dev + 0x321cu)));
    const int h = (int)RWP_LD32(FRAME_AT(frame, 0x5c));
    int y = (int)RWP_LD32(FRAME_AT(frame, 0x54));
    RWP_ST32(dev + 0x3234u, r0);
    RWP_ST32(dev + 0x3238u, r1);
    RWP_ST32(dev + 0x323cu, r2);
    RWP_ST32(dev + 0x3240u, r3);
    int r = w + x, b = h + y;
    if (RWP_LD32(dev + 0x2f00u))
    {
        if (!(x > (int)r0))
            x = (int)r0;
        if (!(y > (int)r1))
            y = (int)r1;
        if (!(r < (int)r2))
            r = (int)r2;
        if (!(b < (int)r3))
            b = (int)r3;
    }
    RWP_ST32(dev + 0x28c4u, RwpRlwimi(RwpRlwimi(RWP_LD32(dev + 0x28c4u), (unsigned int)y, 16, 1, 15), (unsigned int)x, 0, 17, 31));
    RWP_ST32(dev + 0x28c8u, RwpRlwimi(RwpRlwimi(RWP_LD32(dev + 0x28c8u), (unsigned int)b, 16, 1, 15), (unsigned int)r, 0, 17, 31));
    SetSurfaceClip(dev, x, y, r, b);
}


// the device is drawing per screen extent (see SetSurfaceClip)
static __forceinline bool PerExtent(unsigned int dev)
{
    const unsigned int flags = RWP_LD8(dev + 0x2abcu);
    if (flags & 0x10u)
        return true;
    if (!(flags & 0x20u))
        return false;
    for (unsigned int k = 0; k < 5u; k++)
    {
        const unsigned int a = RWP_LD32(dev + 0x3148u + 4u * k), b = RWP_LD32(dev + 0x3268u + 4u * k);
        if (b != a && a != 0)
            return false;
    }
    return true;
}

// The viewport (x, y, width, height, min z, max z as doubles in f1..f6): clipped to the bound
// surface (colour target +0x3148, else depth +0x3158; size from its +0x24 dimension word, or the
// extent size +0x342c/+0x3430 per extent), stored (+0x3218..+0x3230), the scissor re-applied,
// and the viewport transform written (+0x2908 half width, +0x290c x centre, +0x2910 -half height,
// +0x2914 y centre, +0x2918 z range, +0x291c min z) with its six dirty bits.
void SetViewport(unsigned int dev, double x, double y, double w, double h, double zmin, double zmax)
{
    RWP_SAVEGPRLR(31);                                                     // stw r12, -8 / std r31, -0x10
#if defined(RWP_GUEST)
    {
        PPCContext &c = *g_ctx;
        const unsigned int r12 = c.r1.u32 - 0x10u;                        // __savefpr_26
        GStore64(r12 - 0x30u, c.f26.u64);
        GStore64(r12 - 0x28u, c.f27.u64);
        GStore64(r12 - 0x20u, c.f28.u64);
        GStore64(r12 - 0x18u, c.f29.u64);
        GStore64(r12 - 0x10u, c.f30.u64);
        GStore64(r12 - 0x08u, c.f31.u64);
    }
#endif
    RWP_GUEST_FRAME(frame, 0xa0);
    RWP_NV(31, dev);
    unsigned int surf = RWP_LD32(dev + 0x3148u);
    if (!surf)
    {
        surf = RWP_LD32(dev + 0x3158u);
        if (!surf)
            return;
    }
    int maxW, maxH;
    if (PerExtent(dev))
    {
        maxW = (int)RWP_LD32(dev + 0x342cu);
        maxH = (int)RWP_LD32(dev + 0x3430u);
    }
    else
    {
        const unsigned int d = RWP_LD32(surf + 0x24u);
        maxW = (int)(d >> 18) + 1;
        maxH = (int)((d >> 3) & 0x7fffu) + 1;
    }
    RWP_FPU_MODE();
    RWP_ST64(FRAME_AT(frame, 0x50), RwpFctiwz(x));
    RWP_ST64(FRAME_AT(frame, 0x58), RwpFctiwz(w));
    int right = (int)RWP_LD32(FRAME_AT(frame, 0x5c));
    const int ix = (int)RWP_LD32(FRAME_AT(frame, 0x54));
    right += ix;
    RWP_ST64(FRAME_AT(frame, 0x58), RwpFctiwz(y));
    const int iy = (int)RWP_LD32(FRAME_AT(frame, 0x5c));
    if (right < maxW)
        maxW = right;
    RWP_ST64(FRAME_AT(frame, 0x58), RwpFctiwz(h));
    int bottom = (int)RWP_LD32(FRAME_AT(frame, 0x5c)) + iy;
    if (!(bottom < maxH))
        bottom = maxH;
    long long cw = maxW - ix, ch = bottom - iy;
    if (cw < 0 || ch < 0)
        cw = ch = 0;
    RWP_GLOBAL(RWP_F32, dev + 0x3218u) = (float)x;
    RWP_GLOBAL(RWP_F32, dev + 0x321cu) = (float)y;
    RWP_ST64(FRAME_AT(frame, 0x50), (unsigned long long)cw);
    RWP_ST64(FRAME_AT(frame, 0x58), (unsigned long long)ch);
    const double fh = (double)(float)(double)(long long)RWP_LD64(FRAME_AT(frame, 0x58));   // fcfid + frsp
    RWP_GLOBAL(RWP_F32, dev + 0x3228u) = (float)zmin;
    const double fw = (double)(float)(double)(long long)RWP_LD64(FRAME_AT(frame, 0x50));
    RWP_GLOBAL(RWP_F32, dev + 0x322cu) = (float)zmax;
    RWP_GLOBAL(RWP_F32, dev + 0x3224u) = (float)fh;
    RWP_ST32(dev + 0x3230u, 0u);
    RWP_GLOBAL(RWP_F32, dev + 0x3220u) = (float)fw;
    SetScissorRect(dev, dev + 0x3234u);
    RWP_FPU_MODE();
    RWP_GLOBAL(RWP_F32, dev + 0x2918u) = (float)(zmax - zmin);                    // fsubs
    RWP_GLOBAL(RWP_F32, dev + 0x291cu) = (float)zmin;
    const double half = (double)(float)RWP_GLOBAL(RWP_F32, 0x8209975cu);           // 0.5f
    const double hw = (double)(float)(fw * half);                                   // fmuls
    RWP_GLOBAL(RWP_F32, dev + 0x2908u) = (float)hw;
    const double hh = (double)(float)(fh * half);
    RWP_GLOBAL(RWP_F32, dev + 0x290cu) = (float)(double)(float)(hw + x);          // fadds
    RWP_GLOBAL(RWP_F32, dev + 0x2910u) = (float)-hh;                                // fneg
    RWP_GLOBAL(RWP_F32, dev + 0x2914u) = (float)(double)(float)(hh + y);
    unsigned long long f = RWP_LD64(dev + 0x10u);
    for (unsigned long long bit = 0x4000000ull; bit >= 0x200000ull; bit >>= 1)
    {
        f |= bit;
        RWP_ST64(dev + 0x10u, f);
    }
}

}
