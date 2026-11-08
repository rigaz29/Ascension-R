/* The SPU's fused multiply-add (round toward zero, a denormal in or out as zero, a zero result +0) in
 * 32-bit integer operations only, the same code as the vertex pass's HLSL (native/spu_edge_gpulight.c,
 * spu_fma and its helpers; keep the two alike), against the x86 FMA under the SPU's MXCSR. The pass
 * skins EDGE's vertexes with it so the game's depth pass and lit pass land on the same depth wherever
 * either was skinned (docs/notes/2026-11-08-prison-trails.md). 2026-11-08: 40 million operations with
 * the default seed and 120 million with 0x2545F4914F6CDD1D, no difference.
 *
 *     gcc -O2 -mfma tests/native/skin_fma_test.c -o skin_fma_test && ./skin_fma_test [cases] [seed]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

typedef uint32_t u32;
typedef struct { u32 hi, lo; } w64;

static int msb32(u32 x) { return x ? 31 - __builtin_clz(x) : -1; }          /* HLSL firstbithigh */
static int msb64(w64 v) { return v.hi ? 32 + msb32(v.hi) : msb32(v.lo); }
static w64 mk(u32 hi, u32 lo) { w64 r; r.hi = hi; r.lo = lo; return r; }
static w64 shl64(w64 v, u32 n)                                               /* n 0..63 */
{
    if (n == 0u) return v;
    if (n >= 32u) return mk(v.lo << (n - 32u), 0u);
    return mk((v.hi << n) | (v.lo >> (32u - n)), v.lo << n);
}
static w64 shr64s(w64 v, u32 n, u32* sticky)                                 /* any n; sticky: a 1 shifted out */
{
    if (n == 0u) { *sticky = 0u; return v; }
    if (n >= 64u) { *sticky = (v.hi | v.lo) != 0u; return mk(0u, 0u); }
    if (n >= 32u) {
        const u32 m = n - 32u;
        *sticky = (v.lo != 0u || (m != 0u && (v.hi << (32u - m)) != 0u)) ? 1u : 0u;
        return mk(0u, m != 0u ? v.hi >> m : v.hi);
    }
    *sticky = (v.lo << (32u - n)) != 0u ? 1u : 0u;
    return mk(v.hi >> n, (v.lo >> n) | (v.hi << (32u - n)));
}
static w64 add64(w64 a, w64 b) { w64 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u); return r; }
static w64 sub64(w64 a, w64 b) { w64 r; r.lo = a.lo - b.lo; r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u); return r; }
static int ge64(w64 a, w64 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }
static w64 mul24(u32 a, u32 b)                                               /* two 24-bit mantissas */
{
    const u32 ah = a >> 12, al = a & 0xFFFu, bh = b >> 12, bl = b & 0xFFFu;
    w64 r = mk(0u, al * bl);
    r = add64(r, shl64(mk(0u, ah * bl + al * bh), 12u));
    return add64(r, shl64(mk(0u, ah * bh), 24u));
}
static u32 spu_fma(u32 a, u32 b, u32 c)
{
    const u32 ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu, ec = (c >> 23) & 0xFFu;
    const u32 sp = (a ^ b) & 0x80000000u, sc = c & 0x80000000u;
    if (ea == 0u || eb == 0u) return ec == 0u ? 0u : c;                     /* a * b is zero */
    const w64 P = mul24((a & 0x7FFFFFu) | 0x800000u, (b & 0x7FFFFFu) | 0x800000u);
    const int mp = msb64(P);                                                 /* 46 or 47 */
    const int Ep = (int)ea + (int)eb - 300 + mp;                             /* P's top bit is 2^Ep */
    const w64 Pw = shl64(P, (u32)(62 - mp));                                 /* the window: top bit at 62 */
    w64 L, S; int EL; u32 sL, sS, sticky = 0u;
    if (ec == 0u) { L = Pw; EL = Ep; sL = sp; S = mk(0u, 0u); sS = sp; }
    else {
        const int Ec = (int)ec - 127;
        const w64 Cw = shl64(mk(0u, (c & 0x7FFFFFu) | 0x800000u), 39u);
        if (Ep >= Ec) { L = Pw; EL = Ep; sL = sp; S = shr64s(Cw, (u32)(Ep - Ec), &sticky); sS = sc; }
        else          { L = Cw; EL = Ec; sL = sc; S = shr64s(Pw, (u32)(Ec - Ep), &sticky); sS = sp; }
    }
    w64 W; u32 s;
    if (sL == sS) { W = add64(L, S); s = sL; }
    else {
        const w64 Sb = sticky != 0u ? add64(S, mk(0u, 1u)) : S;             /* what was shifted out, taken too */
        if (ge64(L, Sb)) { W = sub64(L, Sb); s = sL; }
        else { W = sub64(Sb, L); s = sS; }
    }
    if ((W.hi | W.lo) == 0u) return 0u;
    const int m = msb64(W);
    const int er = EL - 62 + m + 127;
    if (er <= 0) return 0u;
    if (er >= 255) return s | 0x7F7FFFFFu;
    u32 mant, st;
    if (m >= 23) mant = shr64s(W, (u32)(m - 23), &st).lo;
    else mant = W.lo << (u32)(23 - m);
    return s | ((u32)er << 23) | (mant & 0x7FFFFFu);
}
static u32 spu_mul(u32 a, u32 b) { return spu_fma(a, b, 0u); }

/* the reference: x86 under RZ | FTZ | DAZ, exceptions masked, a zero +0 */
static u32 ref_fma(u32 a, u32 b, u32 c)
{
    float fa, fb, fc; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); memcpy(&fc, &c, 4);
    __m128 r = _mm_fmadd_ss(_mm_set_ss(fa), _mm_set_ss(fb), _mm_set_ss(fc));
    u32 u = (u32)_mm_cvtsi128_si32(_mm_castps_si128(r));
    return (u & 0x7FFFFFFFu) == 0u ? 0u : u;
}
static u32 ref_mul(u32 a, u32 b)
{
    float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    __m128 r = _mm_mul_ss(_mm_set_ss(fa), _mm_set_ss(fb));
    u32 u = (u32)_mm_cvtsi128_si32(_mm_castps_si128(r));
    return (u & 0x7FFFFFFFu) == 0u ? 0u : u;
}

static uint64_t s_x = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { s_x ^= s_x << 13; s_x ^= s_x >> 7; s_x ^= s_x << 17; return (u32)(s_x >> 16); }
static u32 rfloat(int lo, int hi)                                            /* biased exponent in [lo, hi] */
{
    const u32 e = (u32)(lo + (int)(rnd() % (u32)(hi - lo + 1)));
    return (rnd() & 0x80000000u) | (e << 23) | (rnd() & 0x7FFFFFu);
}
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }

int main(int argc, char** argv)
{
    _mm_setcsr(0xFFC0u);   /* RZ, FTZ, DAZ, all exceptions masked */
    long long n = 0, bad = 0;
    const long long N = argc > 1 ? atoll(argv[1]) : 20000000;
    if (argc > 2) s_x = strtoull(argv[2], NULL, 0);
    for (long long i = 0; i < N; i++) {
        u32 a, b, c;
        switch (i % 6) {
        case 0: a = rfloat(1, 254); b = rfloat(1, 254); c = rfloat(1, 254); break;           /* anything */
        case 1: a = rfloat(100, 150); b = rfloat(100, 150); c = rfloat(100, 150); break;     /* near one */
        case 2: {                                                                             /* cancelling */
            a = rfloat(110, 140); b = rfloat(110, 140);
            u32 p = f2u(u2f(a) * u2f(b)) ^ 0x80000000u;
            c = p + (rnd() % 7u) - 3u; break; }
        case 3: a = rfloat(110, 140); b = rfloat(110, 140); c = rfloat(60, 200); break;      /* far apart */
        case 4: {                                                                             /* skinning */
            a = f2u((float)(rnd() & 0xFFu) * u2f(0x3B808080u)); b = rfloat(115, 129); c = rfloat(100, 135);
            break; }
        default: a = (rnd() & 3u) == 0u ? (rnd() & 0x807FFFFFu) : rfloat(1, 254);           /* zeros, denormals */
                 b = (rnd() & 3u) == 0u ? (rnd() & 0x807FFFFFu) : rfloat(1, 254);
                 c = (rnd() & 3u) == 0u ? (rnd() & 0x807FFFFFu) : rfloat(1, 254); break;
        }
        const u32 g = spu_fma(a, b, c), r = ref_fma(a, b, c);
        n++;
        if (g != r) {
            if (bad < 12) printf("fma a=%08X b=%08X c=%08X: int %08X, x86 %08X\n", a, b, c, g, r);
            bad++;
        }
        const u32 gm = spu_mul(a, b), rm = ref_mul(a, b);
        n++;
        if (gm != rm) {
            if (bad < 12) printf("mul a=%08X b=%08X: int %08X, x86 %08X\n", a, b, gm, rm);
            bad++;
        }
    }
    printf("%lld operations, %lld differ\n", n, bad);
    return bad != 0;
}
