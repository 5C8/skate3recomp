// PowerPC rotate-and-mask operations, exactly as the instructions define them (big-endian bit
// numbering: bit 0 is the MSB). For code whose bitfield packing only reads clearly this way.
#ifndef RWP_PPC_OPS_H
#define RWP_PPC_OPS_H

static __forceinline unsigned int RwpRotl32(unsigned int x, int sh)
{
    sh &= 31;
    return sh ? (x << sh) | (x >> (32 - sh)) : x;
}
// MASK(mb, me): ones from bit mb to bit me (wrapping when mb > me)
static __forceinline unsigned int RwpMask32(int mb, int me)
{
    const unsigned int a = 0xffffffffu >> mb, b = 0xffffffffu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
// rlwinm rA, rS, sh, mb, me
static __forceinline unsigned int RwpRlwinm(unsigned int s, int sh, int mb, int me)
{
    return RwpRotl32(s, sh) & RwpMask32(mb, me);
}
// rlwimi rA, rS, sh, mb, me (rA is the inserted-into value)
static __forceinline unsigned int RwpRlwimi(unsigned int a, unsigned int s, int sh, int mb, int me)
{
    const unsigned int m = RwpMask32(mb, me);
    return (RwpRotl32(s, sh) & m) | (a & ~m);
}

static __forceinline unsigned long long RwpRotl64(unsigned long long x, int n)
{
    n &= 63;
    return n ? (x << n) | (x >> (64 - n)) : x;
}
// srad: 7-bit shift count, 64 and up fill with the sign
static __forceinline unsigned long long RwpSrad(unsigned long long s, unsigned long long b)
{
    const unsigned int sh = (unsigned int)b & 0x7fu;
    if (sh & 0x40u)
        return (s >> 63) ? ~0ull : 0ull;
    return (unsigned long long)((long long)s >> sh);
}
// cntlzw (32 for zero)
static __forceinline unsigned int RwpCntlzw(unsigned int x)
{
    unsigned int n = 0;
    if (!x)
        return 32;
    while (!(x & 0x80000000u))
    {
        x <<= 1;
        n++;
    }
    return n;
}

#endif
