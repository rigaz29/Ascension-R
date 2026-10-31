/* EDGE's matrix-palette skinning loop as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job), loop at LS 0xEC08. Each
 * iteration takes two vertices; for each, four bones, each a 3x4 matrix of
 * three rows in local store at r37/r35/r34 + index * 48. A row is blended as
 * w0*M0 + w1*M1 + w2*M2 + w3*M3 (fm, then fma three times). The blended rows go
 * out at r49 (lanes 0-2, row 0's lane 3 = 1.0), and the vertex at r50, rotated
 * twice, is transformed by them in place at r86 = r50 - 32. The loop is software
 * pipelined: an iteration loads the next pair's bone quadword (indices and
 * weights) and positions.
 *
 * Here the two vertices of an iteration share one 256-bit register, so each
 * blend or transform op is one AVX2 op for both. The SPU float ops are done
 * natively under the SPU code's MXCSR (round toward zero, FTZ, DAZ) with the +0
 * rule, as the speculative forms in spu_helpers.h: the result is the SPU's
 * whenever every result is below FLT_MAX in magnitude, which is checked once an
 * iteration before anything is stored. An iteration that fails the check, and
 * always the last one, runs in the lifted loop from the registers as the lifted
 * loop would have them, so the job ends with exactly the lifted registers.
 *
 * It runs only when the loop's sixteen invariant registers hold the values the
 * shuffles below are written for (the masks, 1.0, 1/255), else the lifted loop
 * runs alone. GOWA_EDGE_NATIVE=0 turns it off; GOWA_EDGE_NATIVE_STATS=1 counts
 * calls, iterations and fallbacks every 2^20 iterations. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

static __m128i ld(const spu_context* ctx, int r) { return _mm_loadu_si128((const __m128i*)ctx->gpr[r]._u32); }
static void st(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }

/* local store <-> register order (each word big-endian in LS) */
static __m128i bswap4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
static __m128i ls_rd(const uint8_t* ls, uint32_t a) { return bswap4(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))))); }
static void ls_wr(uint8_t* ls, uint32_t a, __m128i v) { _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))), bswap4(v)); }

static __m256i pair(__m128i lo, __m128i hi) { return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1); }
static __m256 pairf(__m128i lo, __m128i hi) { return _mm256_castsi256_ps(pair(lo, hi)); }

/* the SPU single-precision forms: native result, +0 rule, magnitude into *acc */
static __m256 sp_out(__m256 r, __m256i* acc)
{
    *acc = _mm256_max_epu32(*acc, _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF)));
    return _mm256_and_ps(r, _mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_NEQ_UQ));
}
static __m256 fm8(__m256 a, __m256 b, __m256i* acc) { return sp_out(_mm256_mul_ps(a, b), acc); }
static __m256 fma8(__m256 a, __m256 b, __m256 c, __m256i* acc) { return sp_out(_mm256_fmadd_ps(a, b, c), acc); }
static int in_range(__m256i acc)
{
    return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x7F7FFFFF), acc))) == 0xFF;
}

static __m128i rot16(__m128i v) { return _mm_or_si128(_mm_slli_epi32(v, 16), _mm_srli_epi32(v, 16)); }

/* The loop's invariant registers, as each word reads (the u128 layout). */
static const struct { int r; uint32_t w[4]; } s_inv[] = {
    { 36, { 0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu } },   /* (y, z, x, w) */
    { 82, { 0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu } },   /* (z, x, y, w) */
    { 69, { 0x1C1D1E1Fu, 0x04050607u, 0x08090A0Bu, 0x80808080u } },
    { 70, { 0x80808080u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x80808080u } },
    { 77, { 0x80808000u, 0x80808002u, 0x80808004u, 0x80808006u } },   /* weight bytes, vertex 0 */
    { 102, { 0x80808008u, 0x8080800Au, 0x8080800Cu, 0x8080800Eu } },  /* weight bytes, vertex 1 */
    { 81, { 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u } },  /* word 0 to all */
    { 95, { 0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u } },
    { 94, { 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu } },
    { 93, { 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu } },
    { 84, { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u } },
    { 97, { 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u } },  /* 1.0 */
    { 91, { 0x3B808080u, 0x3B808080u, 0x3B808080u, 0x3B808080u } },  /* 1/255 */
};

static volatile long long s_calls, s_iters, s_fallbacks, s_skipped;
static void stats(void)
{
    static int on = -1;
    if (on < 0) on = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0;
    if (on) fprintf(stderr, "[edge-native] skinning: %lld calls, %lld native iterations, %lld fallbacks, %lld calls lifted\n",
                    s_calls, s_iters, s_fallbacks, s_skipped);
}

/* Called at the loop function's entry; leaves the registers as the lifted loop
 * has them at the start of its next iteration, which the lifted code then runs. */
void gowa_edge_skin_ec08(spu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || atoi(e) != 0; }
    if (!on) return;
    const uint32_t n0 = ctx->gpr[48]._u32[0];
    int ok = n0 >= 4 && !(n0 & 1);
    for (unsigned i = 0; ok && i < sizeof s_inv / sizeof s_inv[0]; i++)
        ok = !memcmp(ctx->gpr[s_inv[i].r]._u32, s_inv[i].w, 16);
    if (!ok) { __atomic_add_fetch(&s_skipped, 1, __ATOMIC_RELAXED); return; }
    __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);

    uint8_t* ls = ctx->ls;
    const uint32_t b37 = ctx->gpr[37]._u32[0], b35 = ctx->gpr[35]._u32[0], b34 = ctx->gpr[34]._u32[0];
    const __m256 r97 = _mm256_set1_ps(1.0f);
    const __m256 inv255 = _mm256_castsi256_ps(_mm256_set1_epi32(0x3B808080));
    const __m256i lanes3 = _mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0);

    /* The iteration's inputs: bone offsets (word 0 of each), weights, positions,
     * pointers. From the entry registers first, then from the last iteration. */
    uint32_t o0[4] = { ctx->gpr[4]._u32[0], ctx->gpr[6]._u32[0], ctx->gpr[12]._u32[0], ctx->gpr[11]._u32[0] };
    uint32_t o1[4] = { ctx->gpr[3]._u32[0], ctx->gpr[7]._u32[0], ctx->gpr[9]._u32[0], ctx->gpr[8]._u32[0] };
    __m256 W[4] = { pairf(ld(ctx, 41), ld(ctx, 33)), pairf(ld(ctx, 42), ld(ctx, 40)),
                    pairf(ld(ctx, 43), ld(ctx, 38)), pairf(ld(ctx, 45), ld(ctx, 44)) };
    __m256 X = pairf(ld(ctx, 46), ld(ctx, 47)), Y = pairf(ld(ctx, 54), ld(ctx, 55)),
           Z = pairf(ld(ctx, 53), ld(ctx, 52));
    __m128i r48 = ld(ctx, 48), r49 = ld(ctx, 49), r50 = ld(ctx, 50), r51 = ld(ctx, 51), r86 = ld(ctx, 86);
    __m128i r6o = ld(ctx, 6);
    int done = 0;

    while (_mm_cvtsi128_si32(r48) != 2) {
        __m256i acc = _mm256_setzero_si256();
        /* the four bones' rows, vertex 0 low, vertex 1 high */
        __m256 R0, R1, R2;
        {
            const __m256 m0 = pairf(ls_rd(ls, b37 + o0[0]), ls_rd(ls, b37 + o1[0]));
            const __m256 m1 = pairf(ls_rd(ls, b35 + o0[0]), ls_rd(ls, b35 + o1[0]));
            const __m256 m2 = pairf(ls_rd(ls, b34 + o0[0]), ls_rd(ls, b34 + o1[0]));
            R0 = fm8(W[0], m0, &acc); R1 = fm8(W[0], m1, &acc); R2 = fm8(W[0], m2, &acc);
        }
        for (int b = 1; b < 4; b++) {
            const __m256 m0 = pairf(ls_rd(ls, b37 + o0[b]), ls_rd(ls, b37 + o1[b]));
            const __m256 m1 = pairf(ls_rd(ls, b35 + o0[b]), ls_rd(ls, b35 + o1[b]));
            const __m256 m2 = pairf(ls_rd(ls, b34 + o0[b]), ls_rd(ls, b34 + o1[b]));
            R0 = fma8(W[b], m0, R0, &acc); R1 = fma8(W[b], m1, R1, &acc); R2 = fma8(W[b], m2, R2, &acc);
        }
        /* the rows as stored: lanes 0-2, row 0's lane 3 = 1.0 (selb with r84/r97) */
        const __m256 O0 = _mm256_blendv_ps(r97, R0, _mm256_castsi256_ps(lanes3));
        const __m256 O1 = _mm256_and_ps(R1, _mm256_castsi256_ps(lanes3));
        const __m256 O2 = _mm256_and_ps(R2, _mm256_castsi256_ps(lanes3));
        /* T = (row0.w, row1.w, row2.w, 0) (shufb r70, then r69) */
        const __m256i a = _mm256_unpackhi_epi32(_mm256_castps_si256(R0), _mm256_castps_si256(R1));
        const __m256i bz = _mm256_unpackhi_epi32(_mm256_castps_si256(R2), _mm256_setzero_si256());
        const __m256 T = _mm256_castsi256_ps(_mm256_unpackhi_epi64(a, bz));
        __m256 P = fma8(X, O0, T, &acc);
        P = fma8(Y, O1, P, &acc);
        P = fma8(Z, O2, P, &acc);

        /* the next pair: bone quadword, weights, offsets, positions */
        const __m128i q = ls_rd(ls, (uint32_t)_mm_cvtsi128_si32(r51));
        const __m128i x0 = ls_rd(ls, (uint32_t)_mm_cvtsi128_si32(r50));
        const __m128i x1 = ls_rd(ls, (uint32_t)_mm_cvtsi128_si32(r50) + 0x10);
        /* weight bytes: SPU bytes 0,2,4,6 and 8,10,12,14 (byte p sits at p ^ 3) */
        const __m128i wb0 = _mm_shuffle_epi8(q, _mm_setr_epi8(3, -1, -1, -1, 1, -1, -1, -1, 7, -1, -1, -1, 5, -1, -1, -1));
        const __m128i wb1 = _mm_shuffle_epi8(q, _mm_setr_epi8(11, -1, -1, -1, 9, -1, -1, -1, 15, -1, -1, -1, 13, -1, -1, -1));
        const __m256 wf = fm8(_mm256_cvtepi32_ps(pair(wb0, wb1)), inv255, &acc);   /* cuflt, then * 1/255 */
        if (!in_range(acc)) { __atomic_add_fetch(&s_fallbacks, 1, __ATOMIC_RELAXED); break; }

        /* stores, in the lifted loop's order */
        const uint32_t o49 = (uint32_t)_mm_cvtsi128_si32(r49), o86 = (uint32_t)_mm_cvtsi128_si32(r86);
        ls_wr(ls, o49 + 0x00, _mm256_castsi256_si128(_mm256_castps_si256(O0)));
        ls_wr(ls, o49 + 0x10, _mm256_castsi256_si128(_mm256_castps_si256(O1)));
        ls_wr(ls, o49 + 0x30, _mm256_extracti128_si256(_mm256_castps_si256(O0), 1));
        ls_wr(ls, o49 + 0x40, _mm256_extracti128_si256(_mm256_castps_si256(O1), 1));
        ls_wr(ls, o49 + 0x20, _mm256_castsi256_si128(_mm256_castps_si256(O2)));
        ls_wr(ls, o49 + 0x50, _mm256_extracti128_si256(_mm256_castps_si256(O2), 1));
        ls_wr(ls, o86 + 0x00, _mm256_castsi256_si128(_mm256_castps_si256(P)));
        ls_wr(ls, o86 + 0x10, _mm256_extracti128_si256(_mm256_castps_si256(P), 1));

        /* advance: offsets = (index byte of each halfword) * 48 */
        const __m128i idx = _mm_and_si128(q, _mm_set1_epi32(0x00FF00FF));
        r6o = _mm_add_epi32(_mm_slli_epi32(idx, 5), _mm_slli_epi32(idx, 4));
        const __m128i o_hi = rot16(r6o);
        o0[0] = (uint32_t)_mm_cvtsi128_si32(o_hi);           o0[1] = (uint32_t)_mm_cvtsi128_si32(r6o);
        o0[2] = (uint32_t)_mm_extract_epi32(o_hi, 1);        o0[3] = (uint32_t)_mm_extract_epi32(r6o, 1);
        o1[0] = (uint32_t)_mm_extract_epi32(o_hi, 2);        o1[1] = (uint32_t)_mm_extract_epi32(r6o, 2);
        o1[2] = (uint32_t)_mm_extract_epi32(o_hi, 3);        o1[3] = (uint32_t)_mm_extract_epi32(r6o, 3);
        W[0] = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(wf), 0x00));
        W[1] = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(wf), 0x55));
        W[2] = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(wf), 0xAA));
        W[3] = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(wf), 0xFF));
        X = pairf(x0, x1);
        Y = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(X), _MM_SHUFFLE(3, 0, 2, 1)));
        Z = _mm256_castsi256_ps(_mm256_shuffle_epi32(_mm256_castps_si256(X), _MM_SHUFFLE(3, 1, 0, 2)));
        r48 = _mm_add_epi32(r48, _mm_set1_epi32(-2));
        r49 = _mm_add_epi32(r49, _mm_set1_epi32(96));
        r50 = _mm_add_epi32(r50, _mm_set1_epi32(32));
        r51 = _mm_add_epi32(r51, _mm_set1_epi32(16));
        r86 = _mm_add_epi32(r86, _mm_set1_epi32(32));
        done++;
    }
    if (!done) return;   /* the registers are still the entry's */
    const long long it = __atomic_add_fetch(&s_iters, done, __ATOMIC_RELAXED);
    if ((it >> 20) != ((it - done) >> 20)) stats();

    /* the registers at the start of the next iteration, as the lifted loop leaves them */
    st(ctx, 6, r6o);
    st(ctx, 4, rot16(r6o));
    const __m128i r11 = _mm_srli_si128(r6o, 4), r7 = _mm_srli_si128(r6o, 8), r8 = _mm_srli_si128(r6o, 12);
    st(ctx, 11, r11); st(ctx, 12, rot16(r11));
    st(ctx, 7, r7);   st(ctx, 3, rot16(r7));
    st(ctx, 8, r8);   st(ctx, 9, rot16(r8));
    st(ctx, 41, _mm256_castsi256_si128(_mm256_castps_si256(W[0]))); st(ctx, 33, _mm256_extracti128_si256(_mm256_castps_si256(W[0]), 1));
    st(ctx, 42, _mm256_castsi256_si128(_mm256_castps_si256(W[1]))); st(ctx, 40, _mm256_extracti128_si256(_mm256_castps_si256(W[1]), 1));
    st(ctx, 43, _mm256_castsi256_si128(_mm256_castps_si256(W[2]))); st(ctx, 38, _mm256_extracti128_si256(_mm256_castps_si256(W[2]), 1));
    st(ctx, 45, _mm256_castsi256_si128(_mm256_castps_si256(W[3]))); st(ctx, 44, _mm256_extracti128_si256(_mm256_castps_si256(W[3]), 1));
    st(ctx, 46, _mm256_castsi256_si128(_mm256_castps_si256(X))); st(ctx, 47, _mm256_extracti128_si256(_mm256_castps_si256(X), 1));
    st(ctx, 54, _mm256_castsi256_si128(_mm256_castps_si256(Y))); st(ctx, 55, _mm256_extracti128_si256(_mm256_castps_si256(Y), 1));
    st(ctx, 53, _mm256_castsi256_si128(_mm256_castps_si256(Z))); st(ctx, 52, _mm256_extracti128_si256(_mm256_castps_si256(Z), 1));
    st(ctx, 48, r48); st(ctx, 49, r49); st(ctx, 50, r50); st(ctx, 51, r51); st(ctx, 86, r86);
}

#else
void gowa_edge_skin_ec08(spu_context* ctx) { (void)ctx; }
#endif
