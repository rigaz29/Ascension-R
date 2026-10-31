/* EDGE's normal loop as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job), loop at LS 0xA950. Each
 * iteration takes eight elements, each a pair of quadwords a (at r49) and b (at
 * r50), and stores at r48:
 *     x = b * a.yzxx - a * b.yzxx                 (fm, then fnms)
 *     s = b.w * (x.yzx, 0)                        (the cross product, scaled)
 *     L = s.x^2 + s.y^2 + s.z^2                   (fm, fa twice, splat)
 *     y = fi(L, frsqest(L)); y += (1 - L*y*y) * (y * 0.5)
 *     out = (L > 1.0e-30 ? s * y : s), w = 0
 * Element 0 is addressed through r51 + r81/r82/r53, the others at 16-byte steps.
 *
 * Here two elements share one 256-bit register, four pairs an iteration. The
 * SPU float ops are native under the SPU code's MXCSR with the +0 rule, and a
 * result at or beyond FLT_MAX in any lane has that half done again by the exact
 * forms (spu_fm & co.), as the lifted code's own forms do: near-zero vectors
 * make frsqest and the Newton step leave the single range in lanes the select
 * then drops. frsqest, fi and fcgt are the runtime's. The last iteration runs
 * in the lifted loop, from the pointers as the lifted loop has them, so the job
 * ends with exactly the lifted registers.
 *
 * It runs only when r47 and the two LS constants it reads (0xF840, 0xF850) are
 * the shuffles below. GOWA_EDGE_NATIVE=0 turns it off with the other EDGE
 * loops; GOWA_EDGE_NATIVE_STATS=1 counts. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

static __m128i ld(const spu_context* ctx, int r) { return _mm_loadu_si128((const __m128i*)ctx->gpr[r]._u32); }
static void st(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }
static __m128i bswap4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
static __m128i ls_rd(const uint8_t* ls, uint32_t a) { return bswap4(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))))); }
static void ls_wr(uint8_t* ls, uint32_t a, __m128i v) { _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))), bswap4(v)); }

static __m256 pairf(__m128i lo, __m128i hi)
{
    return _mm256_castsi256_ps(_mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1));
}
static u128 half(__m256 v, int h)
{
    u128 r;
    _mm_storeu_si128((__m128i*)r._u32, h ? _mm256_extracti128_si256(_mm256_castps_si256(v), 1)
                                         : _mm256_castsi256_si128(_mm256_castps_si256(v)));
    return r;
}
static __m256 join(u128 lo, u128 hi)
{
    return pairf(_mm_loadu_si128((const __m128i*)lo._u32), _mm_loadu_si128((const __m128i*)hi._u32));
}

/* the SPU forms: native where every lane is below FLT_MAX in magnitude (with the
 * +0 rule), else the exact forms on both halves */
static int in_range(__m256 r)
{
    const __m256i m = _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF));
    return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x7F7FFFFF), m))) == 0xFF;
}
static __m256 pz(__m256 r) { return _mm256_and_ps(r, _mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_NEQ_UQ)); }
static volatile long long s_exact;
static __m256 fm8(__m256 a, __m256 b)
{
    const __m256 r = _mm256_mul_ps(a, b);
    if (__builtin_expect(in_range(r), 1)) return pz(r);
    __atomic_add_fetch(&s_exact, 1, __ATOMIC_RELAXED);
    return join(spu_fm(half(a, 0), half(b, 0)), spu_fm(half(a, 1), half(b, 1)));
}
static __m256 fa8(__m256 a, __m256 b)
{
    const __m256 r = _mm256_add_ps(a, b);
    if (__builtin_expect(in_range(r), 1)) return pz(r);
    __atomic_add_fetch(&s_exact, 1, __ATOMIC_RELAXED);
    return join(spu_fa(half(a, 0), half(b, 0)), spu_fa(half(a, 1), half(b, 1)));
}
static __m256 fma8(__m256 a, __m256 b, __m256 c)      /* a*b + c */
{
    const __m256 r = _mm256_fmadd_ps(a, b, c);
    if (__builtin_expect(in_range(r), 1)) return pz(r);
    __atomic_add_fetch(&s_exact, 1, __ATOMIC_RELAXED);
    return join(spu_fma(half(a, 0), half(b, 0), half(c, 0)), spu_fma(half(a, 1), half(b, 1), half(c, 1)));
}
static __m256 fnms8(__m256 a, __m256 b, __m256 c)     /* c - a*b */
{
    const __m256 r = _mm256_fnmadd_ps(a, b, c);
    if (__builtin_expect(in_range(r), 1)) return pz(r);
    __atomic_add_fetch(&s_exact, 1, __ATOMIC_RELAXED);
    return join(spu_fnms(half(a, 0), half(b, 0), half(c, 0)), spu_fnms(half(a, 1), half(b, 1), half(c, 1)));
}
#define SHUF(v, imm) _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(v), (imm)))

static const uint32_t s_r47[4] = { 0x04050607u, 0x08090A0Bu, 0x00010203u, 0x00010203u };   /* (y, z, x, x) */
static const uint32_t s_f840[4] = { 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u };  /* one word round */
static const uint32_t s_f850[4] = { 0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u, 0x14151617u };  /* two words round */

static int ls_is(const uint8_t* ls, uint32_t a, const uint32_t w[4])
{
    const __m128i v = ls_rd(ls, a);
    return _mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_loadu_si128((const __m128i*)w))) == 0xFFFF;
}

static volatile long long s_calls, s_iters, s_skipped;
static void normals_stats(void)
{
    fprintf(stderr, "[edge-native] normals: %lld calls, %lld native iterations, %lld exact half-ops, %lld calls lifted\n",
            s_calls, s_iters, s_exact, s_skipped);
}

void gowa_edge_normals_a950(spu_context* ctx)
{
    static int on = -1, stats = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || (atoi(e) & 2) != 0; }
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(normals_stats); }
    if (!on) return;
    uint8_t* ls = ctx->ls;
    if (memcmp(ctx->gpr[47]._u32, s_r47, 16) || !ls_is(ls, 0xF840, s_f840) || !ls_is(ls, 0xF850, s_f850)) {
        __atomic_add_fetch(&s_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);

    const uint32_t o81 = ctx->gpr[81]._u32[0], o82 = ctx->gpr[82]._u32[0], o53 = ctx->gpr[53]._u32[0];
    const int32_t n83 = (int32_t)ctx->gpr[83]._u32[0];
    __m128i r48 = ld(ctx, 48), r49 = ld(ctx, 49), r50 = ld(ctx, 50), r51 = ld(ctx, 51), r52 = ld(ctx, 52);
    const __m256 mask3 = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    const __m256 one = _mm256_set1_ps(1.0f), halfc = _mm256_set1_ps(0.5f);
    const __m256 thr = _mm256_castsi256_ps(_mm256_set1_epi32(0x0DA2425F));
    int done = 0;

    while (n83 > (int32_t)((uint32_t)_mm_cvtsi128_si32(r52) + 8u)) {
        if (done && (!ls_is(ls, 0xF840, s_f840) || !ls_is(ls, 0xF850, s_f850))) break;
        const uint32_t p48 = (uint32_t)_mm_cvtsi128_si32(r48), p49 = (uint32_t)_mm_cvtsi128_si32(r49);
        const uint32_t p50 = (uint32_t)_mm_cvtsi128_si32(r50), p51 = (uint32_t)_mm_cvtsi128_si32(r51);
        /* the scaled cross products and their squared lengths, two elements a register */
        __m256 s[4], h[4], out[4];
        for (int p = 0; p < 4; p++) {
            const int e0 = 2 * p, e1 = e0 + 1;
            const __m128i a0 = e0 ? ls_rd(ls, p49 + 16u * e0) : ls_rd(ls, p51 + o81);
            const __m128i b0 = e0 ? ls_rd(ls, p50 + 16u * e0) : ls_rd(ls, p51 + o82);
            const __m256 A = pairf(a0, ls_rd(ls, p49 + 16u * e1));
            const __m256 B = pairf(b0, ls_rd(ls, p50 + 16u * e1));
            const __m256 t = fm8(B, SHUF(A, 0x09));
            const __m256 x = fnms8(A, SHUF(B, 0x09), t);
            const __m256 v = _mm256_and_ps(SHUF(x, 0x09), mask3);
            s[p] = fm8(SHUF(B, 0xFF), v);
            const __m256 q = fm8(s[p], s[p]);
            h[p] = fa8(fa8(q, SHUF(q, 0x39)), SHUF(q, 0x4E));
        }
        /* The length and everything after it up to the scale factor is one value an
         * element, which the SPU code keeps in all four lanes. Here the eight
         * elements' values share one register, lanes (0 2 4 6 | 1 3 5 7): the same
         * ops on the same inputs, so the same values. */
        const __m256i u01 = _mm256_unpacklo_epi32(_mm256_castps_si256(h[0]), _mm256_castps_si256(h[1]));
        const __m256i u23 = _mm256_unpacklo_epi32(_mm256_castps_si256(h[2]), _mm256_castps_si256(h[3]));
        const __m256 L = _mm256_castsi256_ps(_mm256_unpacklo_epi64(u01, u23));
        __m256 m;
        if (__builtin_expect(in_range(L), 1))          /* fcgt as the float compare (DAZ, no NaN) */
            m = _mm256_cmp_ps(L, thr, _CMP_GT_OQ);
        else
            m = join(spu_fcgt(half(L, 0), half(thr, 0)), spu_fcgt(half(L, 1), half(thr, 1)));
        const u128 Ll = half(L, 0), Lh = half(L, 1);
        const __m256 y = join(spu_fi(Ll, spu_frsqest(Ll)), spu_fi(Lh, spu_frsqest(Lh)));
        const __m256 e = fnms8(L, fm8(y, y), one);
        const __m256 yn = fma8(e, fm8(y, halfc), y);
        for (int p = 0; p < 4; p++) {
            const __m256i at = _mm256_setr_epi32(p, p, p, p, 4 + p, 4 + p, 4 + p, 4 + p);
            const __m256 nrm = fm8(s[p], _mm256_permutevar8x32_ps(yn, at));
            out[p] = _mm256_and_ps(_mm256_blendv_ps(s[p], nrm, _mm256_permutevar8x32_ps(m, at)), mask3);
        }
        /* stores, in the lifted loop's order: element 0, 7, then 1 to 6 */
        ls_wr(ls, p51 + o53, _mm256_castsi256_si128(_mm256_castps_si256(out[0])));
        ls_wr(ls, p48 + 0x70, _mm256_extracti128_si256(_mm256_castps_si256(out[3]), 1));
        for (int k = 1; k <= 6; k++)
            ls_wr(ls, p48 + 16u * k, (k & 1) ? _mm256_extracti128_si256(_mm256_castps_si256(out[k >> 1]), 1)
                                             : _mm256_castsi256_si128(_mm256_castps_si256(out[k >> 1])));
        const __m128i step = _mm_set1_epi32(128);
        r48 = _mm_add_epi32(r48, step); r49 = _mm_add_epi32(r49, step);
        r50 = _mm_add_epi32(r50, step); r51 = _mm_add_epi32(r51, step);
        r52 = _mm_add_epi32(r52, _mm_set1_epi32(8));
        done++;
    }
    if (!done) return;
    st(ctx, 48, r48); st(ctx, 49, r49); st(ctx, 50, r50); st(ctx, 51, r51); st(ctx, 52, r52);
    const long long it = __atomic_add_fetch(&s_iters, done, __ATOMIC_RELAXED);
    if (stats && (it >> 18) != ((it - done) >> 18))
        fprintf(stderr, "[edge-native] normals: %lld calls, %lld native iterations, %lld exact half-ops, %lld calls lifted\n",
                s_calls, s_iters, s_exact, s_skipped);
}

#else
void gowa_edge_normals_a950(spu_context* ctx) { (void)ctx; }
#endif
