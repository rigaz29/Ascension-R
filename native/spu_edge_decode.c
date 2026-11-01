/* EDGE's fixed-point vertex attribute decoder as native code.
 *
 * Position-independent piece EA 0x75EB40 (an EDGE geometry job streams it to
 * wherever it has room in local store), loop at piece offset 0x310. A vertex
 * is a 24-byte record of packed bit fields; the loop turns two attributes of
 * it into floats, each field an integer part plus a fraction:
 *   fetch    the 32 bytes at r27: two shufb of the three quadwords around
 *            them, by r41 plus the record's offset in its quadword (via r40)
 *   decode   two gathers (r39, r25), the fields shifted into place by bits
 *            (rotqmbi by r23, r24, r26) and then bytes (rotqmbybi by r23,
 *            r24, r26, r28), merged by r32 and r37: one quadword an attribute
 *   convert  x = (field & r33) + r31; csflt(x rotma r17, 155) plus
 *            cuflt(x shl r34, 123) (fa), lane 3 from r35 by r38, stored at
 *            r36 + r18 and r29 + r18
 * The loop is pipelined three deep: an iteration converts and stores what the
 * one before decoded, decodes what the one before fetched, and fetches the
 * next record. r19 counts up by 32 to 0.
 *
 * Here an iteration is the same three stages in the same order -- the stores
 * before the next record is read, so an output that overlaps the input reads
 * the same bytes as on the SPU -- with what is fixed for the call (the gather
 * controls, the shift counts) prepared once. The format comes from the job,
 * so nothing is assumed about it: any controls and counts work. The integer
 * ops are the SPU's. csflt is cvtdq2ps and cuflt an unsigned conversion with
 * the same rounding, both under the SPU code's MXCSR (round toward zero, FTZ,
 * DAZ), which is checked at entry; fa is native with the range check and the
 * +0 rule, else the exact form. The last iteration runs in the lifted loop,
 * so the job ends with exactly the lifted registers.
 *
 * GOWA_EDGE_NATIVE bit 4 (unset: all on); GOWA_EDGE_NATIVE_STATS=1 counts.
 * tests/native/test_edge_native.py checks it against the lifted loop. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

#define DEC_MXCSR_RZFZ 0xE040u   /* round toward zero, FTZ, DAZ (SPU_MXCSR_RZFZ) */

static __m128i dec_ld(const spu_context* ctx, int r) { return _mm_loadu_si128((const __m128i*)ctx->gpr[r]._u32); }
static void dec_st(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }
static __m128i dec_bswap4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
static __m128i dec_ls_rd(const uint8_t* ls, uint32_t a)
{
    return dec_bswap4(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu)))));
}
static void dec_ls_wr(uint8_t* ls, uint32_t a, __m128i v)
{
    _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))), dec_bswap4(v));
}
static __m128i dec_selb(__m128i a, __m128i b, __m128i m) { return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(m, b)); }

/* shufb, as spu_shufb: the source byte of each result byte from the control's
 * low four bits (each word is byte-reversed on the host), a or b by bit 4, and
 * the special selectors (bit 7: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80). For a
 * control fixed for the call the parts are prepared once. */
typedef struct { __m128i idx, sel, spec, specm; int any_spec; } dec_shuf;
static dec_shuf dec_shuf_prep(__m128i c)
{
    dec_shuf s;
    s.idx = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x03));
    s.sel = _mm_slli_epi16(c, 3);
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, (char)0xFF, (char)0x80, 0, 0, 0, 0, 0, 0, 0, 0);
    s.spec = _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(c, 5), _mm_set1_epi8(0x07)));
    s.specm = c;
    s.any_spec = _mm_movemask_epi8(c) != 0;
    return s;
}
static __m128i dec_shuf_do(const dec_shuf* s, __m128i a, __m128i b)
{
    const __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(a, s->idx), _mm_shuffle_epi8(b, s->idx), s->sel);
    return s->any_spec ? _mm_blendv_epi8(r, s->spec, s->specm) : r;
}
static __m128i dec_shufb(__m128i a, __m128i b, __m128i c)
{
    const __m128i idx = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x03));
    const __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(a, idx), _mm_shuffle_epi8(b, idx), _mm_slli_epi16(c, 3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, (char)0xFF, (char)0x80, 0, 0, 0, 0, 0, 0, 0, 0);
    return _mm_blendv_epi8(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(c, 5), _mm_set1_epi8(0x07))), c);
}

/* rotqmbi: the quadword right by (0 - count) & 7 bits (spu__xshrqb).
 * rotqmbybi: right by (0 - (count >> 3)) & 31 bytes, zero past 15: one pshufb
 * by the control spu__xperm builds for it. Both counts from word 0. */
static int dec_bits(__m128i c) { return (0 - (int)_mm_cvtsi128_si32(c)) & 7; }
static __m128i dec_bytes(__m128i c)
{
    const __m128i ir = spu__xshr((0 - ((int)_mm_cvtsi128_si32(c) >> 3)) & 0x1F);
    const __m128i oob = _mm_cmpeq_epi8(_mm_max_epu8(ir, _mm_set1_epi8(16)), ir);
    return _mm_or_si128(_mm_xor_si128(ir, _mm_set1_epi8(3)), oob);
}

/* csflt(x, 155): x as a float, rounded toward zero. cuflt(x, 123): x unsigned,
 * rounded toward zero, times 2^-32. Below 2^31 a signed conversion is the
 * unsigned one; from 2^31 x has 32 significant bits of which rounding keeps the
 * top 24, which x >> 1 has too, so converting x >> 1 and doubling is exact. */
static __m128 dec_csflt155(__m128i x) { return _mm_cvtepi32_ps(x); }
static __m128 dec_cuflt123(__m128i x)
{
    const __m128 lo = _mm_cvtepi32_ps(x);
    const __m128 hi = _mm_add_ps(_mm_cvtepi32_ps(_mm_srli_epi32(x, 1)), _mm_cvtepi32_ps(_mm_srli_epi32(x, 1)));
    return _mm_mul_ps(_mm_blendv_ps(lo, hi, _mm_castsi128_ps(x)), _mm_set1_ps(0x1p-32f));
}

static volatile long long s_dec_exact;
/* fa: native where every lane is below FLT_MAX in magnitude (with the +0
 * rule), else spu_fa */
static __m128 dec_fa(__m128 a, __m128 b)
{
    const __m128 r = _mm_add_ps(a, b);
    const __m128i m = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF), m))) == 0xF, 1))
        return _mm_and_ps(r, _mm_cmp_ps(r, _mm_setzero_ps(), _CMP_NEQ_UQ));
    __atomic_add_fetch(&s_dec_exact, 1, __ATOMIC_RELAXED);
    u128 ua, ub;
    _mm_storeu_ps(ua._f32, a);
    _mm_storeu_ps(ub._f32, b);
    const u128 ur = spu_fa(ua, ub);
    return _mm_loadu_ps(ur._f32);
}

static volatile long long s_dec_calls, s_dec_iters, s_dec_skipped;
static void dec_stats(void)
{
    fprintf(stderr, "[edge-native] decode 30310: %lld calls, %lld native iterations, %lld exact fa, %lld calls lifted\n",
            s_dec_calls, s_dec_iters, s_dec_exact, s_dec_skipped);
}

void gowa_edge_decode_30310(spu_context* ctx)
{
    static int on = -1, stats = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || (atoi(e) & 4) != 0; }
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(dec_stats); }
    if (!on) return;
    /* r19 runs up by 32 to 0: -32 * the iterations left. The lifted loop does
     * the last one. */
    const int32_t c19 = (int32_t)ctx->gpr[19]._u32[0];
    if (c19 >= 0 || (c19 & 31) || (_mm_getcsr() & DEC_MXCSR_RZFZ) != DEC_MXCSR_RZFZ) {
        __atomic_add_fetch(&s_dec_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    const int n = (int)((0u - (uint32_t)c19) >> 5) - 1;
    if (n <= 0) return;
    __atomic_add_fetch(&s_dec_calls, 1, __ATOMIC_RELAXED);

    const __m128i r17 = dec_ld(ctx, 17), r31 = dec_ld(ctx, 31), r32 = dec_ld(ctx, 32), r33 = dec_ld(ctx, 33);
    const __m128i r34 = dec_ld(ctx, 34), r35 = dec_ld(ctx, 35), r37 = dec_ld(ctx, 37), r38 = dec_ld(ctx, 38);
    const __m128i r41 = dec_ld(ctx, 41);
    const dec_shuf s39 = dec_shuf_prep(dec_ld(ctx, 39)), s25 = dec_shuf_prep(dec_ld(ctx, 25));
    const dec_shuf s40 = dec_shuf_prep(dec_ld(ctx, 40));
    const int b23 = dec_bits(dec_ld(ctx, 23)), b24 = dec_bits(dec_ld(ctx, 24)), b26 = dec_bits(dec_ld(ctx, 26));
    const __m128i y23 = dec_bytes(dec_ld(ctx, 23)), y24 = dec_bytes(dec_ld(ctx, 24));
    const __m128i y26 = dec_bytes(dec_ld(ctx, 26)), y28 = dec_bytes(dec_ld(ctx, 28));
    const __m128i shl34 = _mm_and_si128(r34, _mm_set1_epi32(0x3F));                               /* shl */
    const __m128i rma17 = _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), r17), _mm_set1_epi32(0x3F)); /* rotma */
    const uint32_t a36 = ctx->gpr[36]._u32[0], a29 = ctx->gpr[29]._u32[0];

    __m128i r2 = dec_ld(ctx, 2), r5 = dec_ld(ctx, 5), r9 = dec_ld(ctx, 9), r13 = dec_ld(ctx, 13);
    __m128i r18 = dec_ld(ctx, 18), r19 = dec_ld(ctx, 19), r27 = dec_ld(ctx, 27);
    uint8_t* ls = ctx->ls;
    for (int k = 0; k < n; k++) {
        /* convert and store the vertex decoded last time */
        const __m128i x9 = _mm_add_epi32(_mm_and_si128(r9, r33), r31);
        const __m128i x5 = _mm_add_epi32(_mm_and_si128(r5, r33), r31);
        const __m128 v9 = dec_fa(dec_csflt155(_mm_srav_epi32(x9, rma17)), dec_cuflt123(_mm_sllv_epi32(x9, shl34)));
        const __m128 v5 = dec_fa(dec_csflt155(_mm_srav_epi32(x5, rma17)), dec_cuflt123(_mm_sllv_epi32(x5, shl34)));
        const uint32_t o18 = (uint32_t)_mm_cvtsi128_si32(r18);
        dec_ls_wr(ls, a36 + o18, dec_selb(_mm_castps_si128(v9), r35, r38));
        dec_ls_wr(ls, a29 + o18, dec_selb(_mm_castps_si128(v5), r35, r38));
        /* decode the record fetched last time */
        const __m128i g71 = dec_shuf_do(&s39, r13, r2), g72 = dec_shuf_do(&s25, r13, r2);
        const __m128i r54 = spu__xshrqb(g71, b23), r51 = spu__xshrqb(g72, b23);
        const __m128i r62 = spu__xshrqb(g71, b24), r70 = spu__xshrqb(g72, b24);
        const __m128i r68 = spu__xshrqb(g72, b26), r60 = spu__xshrqb(g71, b26);
        const __m128i r44 = dec_selb(_mm_shuffle_epi8(r70, y24), _mm_shuffle_epi8(r68, y26), r32);
        const __m128i r79 = dec_selb(_mm_shuffle_epi8(r62, y24), _mm_shuffle_epi8(r60, y26), r32);
        const __m128i r42 = dec_selb(_mm_shuffle_epi8(r51, y23), _mm_shuffle_epi8(r51, y28), r32);
        const __m128i r6 = dec_selb(_mm_shuffle_epi8(r54, y23), _mm_shuffle_epi8(r54, y28), r32);
        r9 = dec_selb(r44, r42, r37);
        r5 = dec_selb(r79, r6, r37);
        /* fetch the next record */
        const __m128i nib = _mm_and_si128(r27, _mm_set1_epi8(15));
        const __m128i r67 = _mm_add_epi32(r41, dec_shuf_do(&s40, nib, nib));
        const uint32_t a27 = (uint32_t)_mm_cvtsi128_si32(r27);
        const __m128i q63 = dec_ls_rd(ls, a27 + 0x10), q4 = dec_ls_rd(ls, a27), q3 = dec_ls_rd(ls, a27 + 0x20);
        r27 = _mm_add_epi32(r27, _mm_set1_epi32(24));
        r13 = dec_shufb(q4, q63, r67);
        r2 = dec_shufb(q63, q3, r67);
        r19 = _mm_add_epi32(r19, _mm_set1_epi32(32));
        r18 = r19;
    }
    dec_st(ctx, 2, r2); dec_st(ctx, 5, r5); dec_st(ctx, 9, r9); dec_st(ctx, 13, r13);
    dec_st(ctx, 18, r18); dec_st(ctx, 19, r19); dec_st(ctx, 27, r27);
    const long long it = __atomic_add_fetch(&s_dec_iters, n, __ATOMIC_RELAXED);
    if (stats && (it >> 20) != ((it - n) >> 20)) dec_stats();
}

#else
void gowa_edge_decode_30310(spu_context* ctx) { (void)ctx; }
#endif
