/* EDGE's fixed-point vertex attribute decoders as native code.
 *
 * Two position-independent pieces an EDGE geometry job streams to wherever it
 * has room in local store decode its compressed vertex streams. A vertex is a
 * 24-byte record of packed bit fields; a field is an integer part plus a
 * fraction. Both loops are software pipelined: an iteration works on several
 * vertices at once, each at another stage.
 *
 * 30310, piece EA 0x75EB40 (GOWA_EDGE_NATIVE bit 4): two attributes a vertex.
 *   fetch    the 32 bytes at r27: two shufb of the three quadwords around
 *            them, by r41 plus the record's offset in its quadword (via r40)
 *   decode   two gathers (r39, r25), the fields shifted into place by bits
 *            (rotqmbi by r23, r24, r26) and then bytes (rotqmbybi by r23,
 *            r24, r26, r28), merged by r32 and r37: one quadword an attribute
 *   convert  x = (field & r33) + r31; csflt(x rotma r17, 155) plus
 *            cuflt(x shl r34, 123) (fa), lane 3 from r35 by r38, stored at
 *            r36 + r18 and r29 + r18
 *   An iteration converts and stores what the one before decoded, decodes
 *   what the one before fetched, and fetches the next record; r19 counts up
 *   by 32 to 0.
 *
 * 304A0, piece EA 0x758590 (bit 8): the same decoder with more after it, four
 * deep. An iteration fetches record t+1 and decodes record t: two attributes,
 * and four values (rotm, merged with r54) that are table indices and, scaled
 * (fma), two coordinate pairs whose squares it keeps. For vertex t-1 it
 * converts and stores the two attributes, reads four table quadwords at r49
 * plus the indices, and rebuilds two third coordinates as sqrt(1 - x^2 - y^2)
 * (frsqest, fi, fm). For vertex t-2 it merges those into four quadwords by
 * shufb controls taken from the table entries, and stores them. r35 counts up
 * by 32 to 0.
 *
 * Native, each loop keeps the lifted loop's order of local store accesses,
 * stores and loads interleaved as they are, so data that overlaps (an output
 * on the records, a table under an output) reads the same bytes as on the
 * SPU. What is fixed for a call -- the shufb controls, shift counts, masks --
 * is prepared once; the format comes from the job, so any values work. The
 * integer ops are the SPU's. csflt is cvtdq2ps and cuflt an unsigned
 * conversion with the same rounding, under the SPU code's MXCSR (round toward
 * zero, FTZ, DAZ), which is checked at entry. fa, fs, fm and fma are native
 * with the range check and the +0 rule, else the exact forms; frsqest and fi
 * are the runtime's forms, lane for lane. The last iteration runs in the
 * lifted loop, so the job ends with exactly the lifted registers.
 *
 * 304A0's work comes in pairs within an iteration (two attributes, gathers
 * that share sources and shift counts, two coordinate pairs, two merges), and
 * each pair is one 256-bit op. 30310 also has a form that does two iterations
 * with each 256-bit op (bit 16): the next two records are read before the two
 * vertices are stored, so it runs only when the records and the outputs are
 * apart in local store.
 *
 * GOWA_EDGE_NATIVE: unset, all on; GOWA_EDGE_NATIVE_STATS=1 counts.
 * tests/native/test_edge_native.py checks both against their lifted loops. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int edge_memo_skip_decode(void);

#if defined(__AVX2__) && defined(__FMA__)

#define DEC_MXCSR_RZFZ 0xE040u   /* round toward zero, FTZ, DAZ (SPU_MXCSR_RZFZ) */

static int dec_mask(void)
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); m = e ? atoi(e) : -1; if (m < 0) m = 0x7FFFFFFF; }
    return m;
}
static int dec_stats_on(void);

/* ---- 128-bit forms ---- */

static __m128i dec_ld(const spu_context* ctx, int r) { return _mm_loadu_si128((const __m128i*)ctx->gpr[r]._u32); }
static void dec_st(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }
static uint32_t dec_w0(__m128i v) { return (uint32_t)_mm_cvtsi128_si32(v); }
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
static __m128i dec_nibbles(__m128i v) { return _mm_and_si128(v, _mm_set1_epi8(15)); }   /* andbi 15 */

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
/* the per-word shift counts: shl takes count & 63, rotma and rotm (0 - count) & 63 */
static __m128i dec_cnt_shl(__m128i c) { return _mm_and_si128(c, _mm_set1_epi32(0x3F)); }
static __m128i dec_cnt_neg(__m128i c) { return _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), c), _mm_set1_epi32(0x3F)); }

/* csflt(x, 155): x as a float, rounded toward zero. cuflt(x, 123): x unsigned,
 * rounded toward zero, times 2^-32. Below 2^31 a signed conversion is the
 * unsigned one; from 2^31 x has 32 significant bits of which rounding keeps the
 * top 24, which x >> 1 has too, so converting x >> 1 and doubling is exact. */
static __m128 dec_csflt155(__m128i x) { return _mm_cvtepi32_ps(x); }
static __m128 dec_cuflt123(__m128i x)
{
    const __m128 lo = _mm_cvtepi32_ps(x);
    const __m128 h = _mm_cvtepi32_ps(_mm_srli_epi32(x, 1));
    return _mm_mul_ps(_mm_blendv_ps(lo, _mm_add_ps(h, h), _mm_castsi128_ps(x)), _mm_set1_ps(0x1p-32f));
}

/* fa, fs, fm, fma: native where every lane is below FLT_MAX in magnitude (with
 * the +0 rule), else the exact form. An input the SPU treats as a large number
 * (exponent 255) is an infinity or a NaN here, and so is any result made from
 * it, so the result check covers the inputs too. */
static volatile long long s_dec_exact;
static int dec_ok(__m128 r)
{
    const __m128i m = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF), m))) == 0xF;
}
static __m128 dec_pz(__m128 r) { return _mm_and_ps(r, _mm_cmp_ps(r, _mm_setzero_ps(), _CMP_NEQ_UQ)); }
static u128 dec_u(__m128 v) { u128 u; _mm_storeu_ps(u._f32, v); return u; }
static __m128 dec_f(u128 u) { return _mm_loadu_ps(u._f32); }
static __m128 dec_fa(__m128 a, __m128 b)
{
    const __m128 r = _mm_add_ps(a, b);
    if (__builtin_expect(dec_ok(r), 1)) return dec_pz(r);
    __atomic_add_fetch(&s_dec_exact, 1, __ATOMIC_RELAXED);
    return dec_f(spu_fa(dec_u(a), dec_u(b)));
}
static __m128 dec_fs(__m128 a, __m128 b)
{
    const __m128 r = _mm_sub_ps(a, b);
    if (__builtin_expect(dec_ok(r), 1)) return dec_pz(r);
    __atomic_add_fetch(&s_dec_exact, 1, __ATOMIC_RELAXED);
    return dec_f(spu_fs(dec_u(a), dec_u(b)));
}
static __m128 dec_fm(__m128 a, __m128 b)
{
    const __m128 r = _mm_mul_ps(a, b);
    if (__builtin_expect(dec_ok(r), 1)) return dec_pz(r);
    __atomic_add_fetch(&s_dec_exact, 1, __ATOMIC_RELAXED);
    return dec_f(spu_fm(dec_u(a), dec_u(b)));
}
static __m128 dec_fma(__m128 a, __m128 b, __m128 c)   /* a*b + c */
{
    const __m128 r = _mm_fmadd_ps(a, b, c);
    if (__builtin_expect(dec_ok(r), 1)) return dec_pz(r);
    __atomic_add_fetch(&s_dec_exact, 1, __ATOMIC_RELAXED);
    return dec_f(spu_fma(dec_u(a), dec_u(b), dec_u(c)));
}
#define DEC_F(v) _mm_castsi128_ps(v)
#define DEC_I(v) _mm_castps_si128(v)

/* ---- 256-bit forms: two quadwords, one per 128-bit lane ---- */

static __m256i dec_pair(__m128i lo, __m128i hi) { return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1); }
static __m128i dec_lo(__m256i v) { return _mm256_castsi256_si128(v); }
static __m128i dec_hi(__m256i v) { return _mm256_extracti128_si256(v, 1); }
static __m256i dec_both(__m128i v) { return _mm256_broadcastsi128_si256(v); }
static __m256i dec_selb2(__m256i a, __m256i b, __m256i m)
{
    return _mm256_or_si256(_mm256_andnot_si256(m, a), _mm256_and_si256(m, b));
}
typedef struct { __m256i idx, sel, spec, specm; int any_spec; } dec_shuf2;
static dec_shuf2 dec_shuf2_of(const dec_shuf* s)
{
    dec_shuf2 d;
    d.idx = dec_both(s->idx); d.sel = dec_both(s->sel); d.spec = dec_both(s->spec); d.specm = dec_both(s->specm);
    d.any_spec = s->any_spec;
    return d;
}
static __m256i dec_shuf_do2(const dec_shuf2* s, __m256i a, __m256i b)
{
    const __m256i r = _mm256_blendv_epi8(_mm256_shuffle_epi8(a, s->idx), _mm256_shuffle_epi8(b, s->idx), s->sel);
    return s->any_spec ? _mm256_blendv_epi8(r, s->spec, s->specm) : r;
}
static __m256i dec_shufb2(__m256i a, __m256i b, __m256i c)
{
    const __m256i idx = _mm256_xor_si256(_mm256_and_si256(c, _mm256_set1_epi8(0x0F)), _mm256_set1_epi8(0x03));
    const __m256i r = _mm256_blendv_epi8(_mm256_shuffle_epi8(a, idx), _mm256_shuffle_epi8(b, idx), _mm256_slli_epi16(c, 3));
    const __m256i lut = dec_both(_mm_setr_epi8(0, 0, 0, 0, 0, 0, (char)0xFF, (char)0x80, 0, 0, 0, 0, 0, 0, 0, 0));
    return _mm256_blendv_epi8(r, _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(c, 5), _mm256_set1_epi8(0x07))), c);
}
static __m256i dec_shrqb2(__m256i v, int sh)   /* spu__xshrqb on each lane */
{
    return _mm256_or_si256(_mm256_srl_epi32(v, _mm_cvtsi32_si128(sh)),
                           _mm256_sll_epi32(_mm256_slli_si256(v, 4), _mm_cvtsi32_si128(32 - sh)));
}
static __m256 dec_cuflt123x2(__m256i x)
{
    const __m256 lo = _mm256_cvtepi32_ps(x);
    const __m256 h = _mm256_cvtepi32_ps(_mm256_srli_epi32(x, 1));
    return _mm256_mul_ps(_mm256_blendv_ps(lo, _mm256_add_ps(h, h), _mm256_castsi256_ps(x)), _mm256_set1_ps(0x1p-32f));
}
/* fa, fs, fm, fma on two quadwords: native when all eight lanes are in range,
 * else each half by its 128-bit form (native or exact by its own check) */
static int dec_ok2(__m256 r)
{
    const __m256i m = _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF));
    return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x7F7FFFFF), m))) == 0xFF;
}
static __m256 dec_pz2(__m256 r) { return _mm256_and_ps(r, _mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_NEQ_UQ)); }
static __m128 dec_lof(__m256 v) { return _mm256_castps256_ps128(v); }
static __m128 dec_hif(__m256 v) { return _mm256_extractf128_ps(v, 1); }
static __m256 dec_pairf(__m128 lo, __m128 hi) { return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1); }
static __m256 dec_fa2(__m256 a, __m256 b)
{
    const __m256 r = _mm256_add_ps(a, b);
    if (__builtin_expect(dec_ok2(r), 1)) return dec_pz2(r);
    return dec_pairf(dec_fa(dec_lof(a), dec_lof(b)), dec_fa(dec_hif(a), dec_hif(b)));
}
static __m256 dec_fs2(__m256 a, __m256 b)
{
    const __m256 r = _mm256_sub_ps(a, b);
    if (__builtin_expect(dec_ok2(r), 1)) return dec_pz2(r);
    return dec_pairf(dec_fs(dec_lof(a), dec_lof(b)), dec_fs(dec_hif(a), dec_hif(b)));
}
static __m256 dec_fm2(__m256 a, __m256 b)
{
    const __m256 r = _mm256_mul_ps(a, b);
    if (__builtin_expect(dec_ok2(r), 1)) return dec_pz2(r);
    return dec_pairf(dec_fm(dec_lof(a), dec_lof(b)), dec_fm(dec_hif(a), dec_hif(b)));
}
static __m256 dec_fma2(__m256 a, __m256 b, __m256 c)   /* a*b + c */
{
    const __m256 r = _mm256_fmadd_ps(a, b, c);
    if (__builtin_expect(dec_ok2(r), 1)) return dec_pz2(r);
    return dec_pairf(dec_fma(dec_lof(a), dec_lof(b), dec_lof(c)), dec_fma(dec_hif(a), dec_hif(b), dec_hif(c)));
}
/* frsqest and fi on two quadwords: the runtime's x86 forms (spu_frsqest, spu_fi
 * and spu__xe_lut in spu_helpers.h) lane for lane, so the table lookup's
 * permutes use all eight lanes */
static __m256i dec_lut2(const uint32_t* tbl, int n, __m256i idx)
{
    const __m256i row = _mm256_srli_epi32(idx, 3);
    __m256i r = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)tbl), idx);
    for (int k = 1; k < n; k++) {
        const __m256i v = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(tbl + 8 * k)), idx);
        r = _mm256_blendv_epi8(r, v, _mm256_cmpeq_epi32(row, _mm256_set1_epi32(k)));
    }
    return r;
}
static __m256i dec_frsqest2(__m256i x)
{
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x3F));
    const __m256i frac = _mm256_and_si256(dec_lut2(spu_frsqest_fraction_lut, 8, idx), _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(190), _mm256_srli_epi32(_mm256_add_epi32(exp, _mm256_set1_epi32(1)), 1));
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac);
}
static __m256i dec_fi2(__m256i a, __m256i est)
{
    const __m256i y = _mm256_and_si256(a, _mm256_set1_epi32(0x7FFFF));
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(est, 23), _mm256_set1_epi32(0xFF));
    const __m256i b19 = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(est, 10), _mm256_set1_epi32(0x1FFF)), 19);
    const __m256i sy = _mm256_mullo_epi32(_mm256_and_si256(est, _mm256_set1_epi32(0x3FF)), y);
    const __m256i big = _mm256_cmpeq_epi32(_mm256_max_epu32(b19, sy), b19);           /* magnitude >= 1.0 */
    const __m256i d = _mm256_sub_epi32(b19, sy);
    const __m256i frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(d, 8), _mm256_srli_epi32(d, 9), big),
                                          _mm256_set1_epi32(0x7FFFFF));
    const __m256i e2 = _mm256_sub_epi32(exp, _mm256_andnot_si256(big, _mm256_set1_epi32(1)));
    const __m256i r = _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(est, _mm256_set1_epi32((int)0x80000000u)),
                                                      _mm256_slli_epi32(e2, 23)), frac);
    const __m256i zero = _mm256_or_si256(_mm256_cmpeq_epi32(exp, _mm256_setzero_si256()),
                                         _mm256_cmpeq_epi32(e2, _mm256_setzero_si256()));
    return _mm256_andnot_si256(zero, r);
}

/* ---- 30310 ---- */

typedef struct {
    dec_shuf s39, s25, s40;
    int b23, b24, b26;
    __m128i y23, y24, y26, y28, shl34, rma17;
    __m128i r31, r32, r33, r35, r37, r38, r41;
    uint32_t a36, a29;
} d30310_fix;
typedef struct { __m128i r2, r5, r9, r13, r18, r19, r27; } d30310_state;

static void d30310_step(const d30310_fix* F, d30310_state* S, uint8_t* ls)
{
    /* convert and store the vertex decoded last time */
    const __m128i x9 = _mm_add_epi32(_mm_and_si128(S->r9, F->r33), F->r31);
    const __m128i x5 = _mm_add_epi32(_mm_and_si128(S->r5, F->r33), F->r31);
    const __m128 v9 = dec_fa(dec_csflt155(_mm_srav_epi32(x9, F->rma17)), dec_cuflt123(_mm_sllv_epi32(x9, F->shl34)));
    const __m128 v5 = dec_fa(dec_csflt155(_mm_srav_epi32(x5, F->rma17)), dec_cuflt123(_mm_sllv_epi32(x5, F->shl34)));
    const uint32_t o18 = dec_w0(S->r18);
    dec_ls_wr(ls, F->a36 + o18, dec_selb(DEC_I(v9), F->r35, F->r38));
    dec_ls_wr(ls, F->a29 + o18, dec_selb(DEC_I(v5), F->r35, F->r38));
    /* decode the record fetched last time */
    const __m128i g71 = dec_shuf_do(&F->s39, S->r13, S->r2), g72 = dec_shuf_do(&F->s25, S->r13, S->r2);
    const __m128i r54 = spu__xshrqb(g71, F->b23), r51 = spu__xshrqb(g72, F->b23);
    const __m128i r62 = spu__xshrqb(g71, F->b24), r70 = spu__xshrqb(g72, F->b24);
    const __m128i r68 = spu__xshrqb(g72, F->b26), r60 = spu__xshrqb(g71, F->b26);
    const __m128i r44 = dec_selb(_mm_shuffle_epi8(r70, F->y24), _mm_shuffle_epi8(r68, F->y26), F->r32);
    const __m128i r79 = dec_selb(_mm_shuffle_epi8(r62, F->y24), _mm_shuffle_epi8(r60, F->y26), F->r32);
    const __m128i r42 = dec_selb(_mm_shuffle_epi8(r51, F->y23), _mm_shuffle_epi8(r51, F->y28), F->r32);
    const __m128i r6 = dec_selb(_mm_shuffle_epi8(r54, F->y23), _mm_shuffle_epi8(r54, F->y28), F->r32);
    S->r9 = dec_selb(r44, r42, F->r37);
    S->r5 = dec_selb(r79, r6, F->r37);
    /* fetch the next record */
    const __m128i nib = dec_nibbles(S->r27);
    const __m128i r67 = _mm_add_epi32(F->r41, dec_shuf_do(&F->s40, nib, nib));
    const uint32_t a27 = dec_w0(S->r27);
    const __m128i q63 = dec_ls_rd(ls, a27 + 0x10), q4 = dec_ls_rd(ls, a27), q3 = dec_ls_rd(ls, a27 + 0x20);
    S->r27 = _mm_add_epi32(S->r27, _mm_set1_epi32(24));
    S->r13 = dec_shufb(q4, q63, r67);
    S->r2 = dec_shufb(q63, q3, r67);
    S->r19 = _mm_add_epi32(S->r19, _mm_set1_epi32(32));
    S->r18 = S->r19;
}

/* Two iterations, t and t+1, each op on both. Here the records t+1 and t+2 are
 * read before the vertices t-1 and t are stored (the lifted loop stores t-1,
 * reads t+1, stores t, reads t+2), so the caller has checked that no store of
 * the call lands on a record it reads. The two stores of a vertex and the
 * vertices keep their order. */
typedef struct {
    dec_shuf2 s39, s25, s40;
    __m256i y23, y24, y26, y28, shl34, rma17, r31, r32, r33, r35, r37, r38, r41;
} d30310_fix2;
static void d30310_pair(const d30310_fix* F, const d30310_fix2* G, d30310_state* S, uint8_t* ls)
{
    /* fetch the records t+1 (at r27) and t+2 (at r27 + 24) */
    const __m128i ra = S->r27, rb = _mm_add_epi32(S->r27, _mm_set1_epi32(24));
    const __m256i R = dec_pair(ra, rb), nib = _mm256_and_si256(R, _mm256_set1_epi8(15));
    const __m256i c67 = _mm256_add_epi32(G->r41, dec_shuf_do2(&G->s40, nib, nib));
    const uint32_t aa = dec_w0(ra), ab = dec_w0(rb);
    const __m256i q4 = dec_pair(dec_ls_rd(ls, aa), dec_ls_rd(ls, ab));
    const __m256i q63 = dec_pair(dec_ls_rd(ls, aa + 0x10), dec_ls_rd(ls, ab + 0x10));
    const __m256i q3 = dec_pair(dec_ls_rd(ls, aa + 0x20), dec_ls_rd(ls, ab + 0x20));
    const __m256i W13 = dec_shufb2(q4, q63, c67), W2 = dec_shufb2(q63, q3, c67);
    /* decode the records t (fetched before) and t+1 */
    const __m256i r13 = dec_pair(S->r13, dec_lo(W13)), r2 = dec_pair(S->r2, dec_lo(W2));
    const __m256i g71 = dec_shuf_do2(&G->s39, r13, r2), g72 = dec_shuf_do2(&G->s25, r13, r2);
    const __m256i r54 = dec_shrqb2(g71, F->b23), r51 = dec_shrqb2(g72, F->b23);
    const __m256i r62 = dec_shrqb2(g71, F->b24), r70 = dec_shrqb2(g72, F->b24);
    const __m256i r68 = dec_shrqb2(g72, F->b26), r60 = dec_shrqb2(g71, F->b26);
    const __m256i r44 = dec_selb2(_mm256_shuffle_epi8(r70, G->y24), _mm256_shuffle_epi8(r68, G->y26), G->r32);
    const __m256i r79 = dec_selb2(_mm256_shuffle_epi8(r62, G->y24), _mm256_shuffle_epi8(r60, G->y26), G->r32);
    const __m256i r42 = dec_selb2(_mm256_shuffle_epi8(r51, G->y23), _mm256_shuffle_epi8(r51, G->y28), G->r32);
    const __m256i r6 = dec_selb2(_mm256_shuffle_epi8(r54, G->y23), _mm256_shuffle_epi8(r54, G->y28), G->r32);
    const __m256i D9 = dec_selb2(r44, r42, G->r37), D5 = dec_selb2(r79, r6, G->r37);
    /* convert and store the vertices t-1 (decoded before) and t */
    const __m256i r9 = dec_pair(S->r9, dec_lo(D9)), r5 = dec_pair(S->r5, dec_lo(D5));
    const __m256i x9 = _mm256_add_epi32(_mm256_and_si256(r9, G->r33), G->r31);
    const __m256i x5 = _mm256_add_epi32(_mm256_and_si256(r5, G->r33), G->r31);
    const __m256 v9 = dec_fa2(_mm256_cvtepi32_ps(_mm256_srav_epi32(x9, G->rma17)), dec_cuflt123x2(_mm256_sllv_epi32(x9, G->shl34)));
    const __m256 v5 = dec_fa2(_mm256_cvtepi32_ps(_mm256_srav_epi32(x5, G->rma17)), dec_cuflt123x2(_mm256_sllv_epi32(x5, G->shl34)));
    const __m256i o9 = dec_selb2(_mm256_castps_si256(v9), G->r35, G->r38);
    const __m256i o5 = dec_selb2(_mm256_castps_si256(v5), G->r35, G->r38);
    const uint32_t oa = dec_w0(S->r18), ob = dec_w0(S->r19) + 32u;
    dec_ls_wr(ls, F->a36 + oa, dec_lo(o9));
    dec_ls_wr(ls, F->a29 + oa, dec_lo(o5));
    dec_ls_wr(ls, F->a36 + ob, dec_hi(o9));
    dec_ls_wr(ls, F->a29 + ob, dec_hi(o5));
    S->r9 = dec_hi(D9); S->r5 = dec_hi(D5);
    S->r13 = dec_hi(W13); S->r2 = dec_hi(W2);
    S->r27 = _mm_add_epi32(S->r27, _mm_set1_epi32(48));
    S->r19 = _mm_add_epi32(S->r19, _mm_set1_epi32(64));
    S->r18 = S->r19;
}

/* Whether m iterations' stores (at base + r18, then base + r19 + 32k) miss
 * every quadword their fetches read (around r27 + 24k), none of it wrapping
 * the end of local store. Spans are conservative supersets. */
static int d30310_apart(const spu_context* ctx, uint32_t m)
{
    const uint32_t a27 = ctx->gpr[27]._u32[0] & SPU_LS_MASK;
    const uint32_t rlo = a27 & ~0xFu, rhi = ((a27 + 24u * (m - 1u)) & ~0xFu) + 0x30u;
    if (a27 + 24u * (m - 1u) + 0x30u > SPU_LS_SIZE) return 0;
    const uint32_t o18 = ctx->gpr[18]._u32[0], o19 = ctx->gpr[19]._u32[0];
    const uint32_t base[2] = { ctx->gpr[36]._u32[0], ctx->gpr[29]._u32[0] };
    for (int b = 0; b < 2; b++) {
        const uint32_t u = (base[b] + o18) & SPU_LS_MASK, v = (base[b] + o19 + 32u) & SPU_LS_MASK;
        if (v + 32u * (m - 1u) + 16u > SPU_LS_SIZE) return 0;
        const uint32_t lo = (u < v ? u : v) & ~0xFu;
        const uint32_t w = v + 32u * (m - 1u);
        const uint32_t hi = ((u > w ? u : w) & ~0xFu) + 16u;
        if (lo < rhi && rlo < hi) return 0;
    }
    return 1;
}

static volatile long long s_d1_calls, s_d1_iters, s_d1_pairs, s_d1_skipped;
static volatile long long s_d2_calls, s_d2_iters, s_d2_skipped;
static void dec_stats(void)
{
    fprintf(stderr, "[edge-native] decode 30310: %lld calls, %lld native iterations (%lld in pairs), %lld calls lifted; "
                    "decode 304A0: %lld calls, %lld native iterations, %lld calls lifted; %lld exact float ops\n",
            s_d1_calls, s_d1_iters, 2 * s_d1_pairs, s_d1_skipped, s_d2_calls, s_d2_iters, s_d2_skipped, s_dec_exact);
}
static int dec_stats_on(void)
{
    static int stats = -1;
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(dec_stats); }
    return stats;
}
static void dec_count(volatile long long* it, int n)
{
    const long long v = __atomic_add_fetch(it, n, __ATOMIC_RELAXED);
    if (dec_stats_on() && (v >> 22) != ((v - n) >> 22)) dec_stats();
}

void gowa_edge_decode_30310(spu_context* ctx)
{
    if (!(dec_mask() & 4)) return;
    /* r19 runs up by 32 to 0: -32 * the iterations left. The lifted loop does
     * the last one. */
    const int32_t c19 = (int32_t)ctx->gpr[19]._u32[0];
    if (c19 >= 0 || (c19 & 31) || (_mm_getcsr() & DEC_MXCSR_RZFZ) != DEC_MXCSR_RZFZ) {
        __atomic_add_fetch(&s_d1_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    const int n = (int)((0u - (uint32_t)c19) >> 5) - 1;
    if (n <= 0) return;
    if (edge_memo_skip_decode()) {      /* the lifted loop's one pass, at the array's start */
        ctx->gpr[19]._u32[0] = (uint32_t)-32;
        return;
    }
    __atomic_add_fetch(&s_d1_calls, 1, __ATOMIC_RELAXED);

    d30310_fix F;
    F.s39 = dec_shuf_prep(dec_ld(ctx, 39)); F.s25 = dec_shuf_prep(dec_ld(ctx, 25)); F.s40 = dec_shuf_prep(dec_ld(ctx, 40));
    F.b23 = dec_bits(dec_ld(ctx, 23)); F.b24 = dec_bits(dec_ld(ctx, 24)); F.b26 = dec_bits(dec_ld(ctx, 26));
    F.y23 = dec_bytes(dec_ld(ctx, 23)); F.y24 = dec_bytes(dec_ld(ctx, 24));
    F.y26 = dec_bytes(dec_ld(ctx, 26)); F.y28 = dec_bytes(dec_ld(ctx, 28));
    F.shl34 = dec_cnt_shl(dec_ld(ctx, 34)); F.rma17 = dec_cnt_neg(dec_ld(ctx, 17));
    F.r31 = dec_ld(ctx, 31); F.r32 = dec_ld(ctx, 32); F.r33 = dec_ld(ctx, 33); F.r35 = dec_ld(ctx, 35);
    F.r37 = dec_ld(ctx, 37); F.r38 = dec_ld(ctx, 38); F.r41 = dec_ld(ctx, 41);
    F.a36 = ctx->gpr[36]._u32[0]; F.a29 = ctx->gpr[29]._u32[0];
    d30310_state S = { dec_ld(ctx, 2), dec_ld(ctx, 5), dec_ld(ctx, 9), dec_ld(ctx, 13),
                       dec_ld(ctx, 18), dec_ld(ctx, 19), dec_ld(ctx, 27) };
    uint8_t* ls = ctx->ls;
    int k = 0;
    if (n >= 2 && (dec_mask() & 16) && d30310_apart(ctx, (uint32_t)n)) {
        d30310_fix2 G;
        G.s39 = dec_shuf2_of(&F.s39); G.s25 = dec_shuf2_of(&F.s25); G.s40 = dec_shuf2_of(&F.s40);
        G.y23 = dec_both(F.y23); G.y24 = dec_both(F.y24); G.y26 = dec_both(F.y26); G.y28 = dec_both(F.y28);
        G.shl34 = dec_both(F.shl34); G.rma17 = dec_both(F.rma17);
        G.r31 = dec_both(F.r31); G.r32 = dec_both(F.r32); G.r33 = dec_both(F.r33); G.r35 = dec_both(F.r35);
        G.r37 = dec_both(F.r37); G.r38 = dec_both(F.r38); G.r41 = dec_both(F.r41);
        for (; k + 2 <= n; k += 2) d30310_pair(&F, &G, &S, ls);
        __atomic_add_fetch(&s_d1_pairs, k / 2, __ATOMIC_RELAXED);
    }
    for (; k < n; k++) d30310_step(&F, &S, ls);
    dec_st(ctx, 2, S.r2); dec_st(ctx, 5, S.r5); dec_st(ctx, 9, S.r9); dec_st(ctx, 13, S.r13);
    dec_st(ctx, 18, S.r18); dec_st(ctx, 19, S.r19); dec_st(ctx, 27, S.r27);
    dec_count(&s_d1_iters, n);
}

/* ---- 304A0 ----
 * The numbers are the lifted loop's statements, in its order. Nearly all of
 * the work comes in pairs -- the two attributes, the gathers that share their
 * sources and shift counts, the two coordinate pairs and their third
 * coordinates, the two merges of a table entry -- and each pair is one 256-bit
 * op, (low lane | high lane). The state keeps them paired from one iteration to
 * the next. A selb with one operand fixed for the call keeps that operand's
 * part prepared. */

typedef struct {
    dec_shuf2 g1, g2, g3, s82;             /* (r83 | r34), (r85 | r87), (r84 | r86), r82 */
    dec_shuf s88;
    int b33, b36, b37;
    __m256i y33, y36, y37, y60, rm55, shl67, rma66, r63, r68;
    __m256i r43, r64, r80, r41, k42, r53, k54, r81, k77, r70h;   /* r70h: (0 | r70) */
    __m128i r79;
    __m256 r71, r72, r73;
    uint32_t a49, a65, a69, a74, a75, a76, a78;
} d304_fix;
typedef struct {
    __m256i r3_10, w16_11, s38_40, i46_48, i45_47, z59_50, l51_52, l58_57;
    __m128i r6, r35, r39, r44, r56, r61, r62;
} d304_state;

static __m256i dec_swap(__m256i v) { return _mm256_permute2x128_si256(v, v, 0x01); }
static __m256i dec_lo2(__m256i v) { return _mm256_permute2x128_si256(v, v, 0x00); }
static __m256i dec_hi2(__m256i v) { return _mm256_permute2x128_si256(v, v, 0x11); }

static void d304_step(const d304_fix* F, d304_state* S, uint8_t* ls)
{
    /* vertex t-1: its two attributes, integer part plus fraction (1-25, 34, 38) */
    const __m256i T = _mm256_add_epi32(_mm256_and_si256(S->r3_10, F->r68), F->r63);       /* 1, 3, 5, 7 */
    const __m256 A = dec_fa2(_mm256_cvtepi32_ps(_mm256_srav_epi32(T, F->rma66)),          /* 11, 13, 21, 25 */
                             dec_cuflt123x2(_mm256_sllv_epi32(T, F->shl67)));             /* 9, 15, 19, 23; 34, 38 */
    /* record t: the gathers (14-24), the fields into place (26-57), the four values (36-48) */
    const __m256i r16 = dec_lo2(S->w16_11), r11 = dec_hi2(S->w16_11);
    const __m256i G1 = dec_shuf_do2(&F->g1, r16, r11);                                    /* g21 | g45: 14, 20 */
    const __m256i G2 = dec_shuf_do2(&F->g2, r16, r11);                                    /* g17 | g12: 16, 18 */
    const __m256i G3 = dec_shuf_do2(&F->g3, r11, r11);                                    /* g19 | g27: 22, 24 */
    const __m256i H33 = dec_shrqb2(G1, F->b33), H36 = dec_shrqb2(G1, F->b36), H37 = dec_shrqb2(G1, F->b37);
    /* h6 | h7 (26, 27), h18 | h20 (33, 29), h23 | h24 (35, 31) */
    const __m256i X33 = _mm256_shuffle_epi8(H33, F->y33), X60 = _mm256_shuffle_epi8(H33, F->y60);
    const __m256i X36 = _mm256_shuffle_epi8(H36, F->y36), X37 = _mm256_shuffle_epi8(H37, F->y37);
    /* h6b | h7b (49, 57), h27 | h28 (47, 41), h18b | h20b (43, 39), h23b | h24b (45, 51) */
    const __m256i I1 = _mm256_or_si256(F->k54, _mm256_and_si256(_mm256_srlv_epi32(G2, F->rm55), F->r53)); /* i46 | i48: 46, 36, 67, 64 */
    const __m256i I2 = _mm256_or_si256(F->k54, _mm256_and_si256(_mm256_srlv_epi32(G3, F->rm55), F->r53)); /* i45 | i47: 48, 44, 68, 65 */
    /* vertex t-1: the squares of last time summed (8, 12, 28, 30), one minus them (40, 42);
     * frsqest (53, 55) and fi (75, 77); the third coordinates (89, 91) */
    const __m256 L = dec_fs2(F->r73, dec_fa2(_mm256_castsi256_ps(S->s38_40),
                                             _mm256_castsi256_ps(_mm256_srli_si256(S->s38_40, 4))));  /* f13b | f38 */
    const __m256i E = dec_fi2(_mm256_castps_si256(L), dec_frsqest2(_mm256_castps_si256(L)));
    const __m256 Z = dec_fm2(L, _mm256_castsi256_ps(E));                                   /* z59 | z50 */
    /* vertex t-2: the merges (32, 50-78) */
    const __m256i TT1 = dec_selb2(dec_hi2(S->z59_50), S->l51_52, F->r43);                 /* t11 | t9: 50, 52 */
    const __m256i TT2 = dec_selb2(dec_lo2(S->z59_50), S->l58_57, F->r43);                 /* t5 | t50: 60, 70 */
    const __m256i CC1 = _mm256_add_epi32(_mm256_or_si256(F->k42, _mm256_and_si256(S->l51_52, F->r41)), F->r70h); /* c10 | c2: 56, 32, 54 */
    const __m256i CC2 = _mm256_add_epi32(_mm256_or_si256(F->k42, _mm256_and_si256(S->l58_57, F->r41)), F->r70h); /* c21 | c10b: 62, 58, 74 */
    const __m256i N1 = dec_shufb2(TT1, dec_both(S->r39), CC1);                            /* n51 | n52: 61, 59 */
    const __m256i N2 = dec_shufb2(TT2, dec_both(S->r56), CC2);                            /* n57 | n19: 71, 78 */
    const __m256i O = _mm256_or_si256(_mm256_andnot_si256(F->r81, _mm256_castps_si256(A)), F->k77);  /* o39 | o58: 63, 72 */
    /* record t: the coordinate pairs (69, 73) scaled (81, 83) and squared (93, 100) */
    const __m256 D = dec_fma2(F->r72, _mm256_castsi256_ps(dec_shuf_do2(&F->s82, I1, I2)), F->r71);  /* d39 | d21 */
    const __m256 SQ = dec_fm2(D, D);                                                       /* s38 | s40 */
    /* record t: the fields merged for next time (85-96, 110, 112) */
    const __m256i QA = dec_selb2(X33, X60, F->r64);                                       /* q4 | q3: 85, 95 */
    const __m256i QB = dec_selb2(X36, X37, F->r64);                                       /* q5 | q25: 96, 87 */
    const __m256i R = dec_selb2(dec_swap(QB), dec_swap(QA), F->r80);                      /* r3 | r10: 110, 112 */
    /* the next record's window (37, 79, 99, 106, 111, 113) */
    const __m128i nib = dec_nibbles(S->r44);
    const __m128i c8 = _mm_add_epi32(F->r79, dec_shuf_do(&F->s88, nib, nib));
    /* the local store accesses, in the lifted loop's order */
    const uint32_t o35 = dec_w0(S->r35), o22 = dec_w0(S->r6), a44 = dec_w0(S->r44);        /* 2: r22 = r6 */
    dec_ls_wr(ls, F->a78 + o35, dec_lo(O));                                                /* 66 */
    dec_ls_wr(ls, F->a74 + o35, dec_hi(O));                                                /* 76 */
    dec_ls_wr(ls, F->a65 + o22, dec_lo(N1));                                               /* 80 */
    dec_ls_wr(ls, F->a75 + o22, dec_hi(N1));                                               /* 82 */
    const __m128i L52 = dec_ls_rd(ls, F->a49 + _mm256_extract_epi32(S->i45_47, 4));        /* 6, 84: r47 */
    const __m128i L51 = dec_ls_rd(ls, F->a49 + _mm256_extract_epi32(S->i46_48, 4));        /* 4, 86: r48 */
    dec_ls_wr(ls, F->a69 + o22, dec_lo(N2));                                               /* 88 */
    dec_ls_wr(ls, F->a76 + o22, dec_hi(N2));                                               /* 90 */
    const __m128i w22 = dec_ls_rd(ls, a44 + 0x10);                                         /* 92 */
    const __m128i w9 = dec_ls_rd(ls, a44);                                                 /* 94 */
    const __m128i w2 = dec_ls_rd(ls, a44 + 0x20);                                          /* 97 */
    const __m128i L57 = dec_ls_rd(ls, F->a49 + _mm256_extract_epi32(S->i45_47, 0));        /* 17, 101: r45 */
    const __m128i L58 = dec_ls_rd(ls, F->a49 + _mm256_extract_epi32(S->i46_48, 0));        /* 10, 103: r46 */
    /* the state for the next iteration */
    S->r6 = S->r35;                                                                        /* 98 */
    S->r35 = _mm_add_epi32(S->r35, _mm_set1_epi32(32));                                   /* 102, 108 */
    S->r56 = S->r61; S->r61 = _mm256_castsi256_si128(_mm256_castps_si256(D));              /* 104, 105 */
    S->r39 = S->r62; S->r62 = _mm256_extracti128_si256(_mm256_castps_si256(D), 1);         /* 107, 109 */
    S->w16_11 = dec_shufb2(dec_pair(w9, w22), dec_pair(w22, w2), dec_both(c8));           /* 111, 113 */
    S->r44 = _mm_add_epi32(S->r44, _mm_set1_epi32(24));                                   /* 114 */
    S->r3_10 = R;
    S->s38_40 = _mm256_castps_si256(SQ);
    S->i46_48 = I1; S->i45_47 = I2;
    S->z59_50 = _mm256_castps_si256(Z);
    S->l51_52 = dec_pair(L51, L52); S->l58_57 = dec_pair(L58, L57);
}

static dec_shuf2 dec_shuf2_two(__m128i lo, __m128i hi)   /* a different control in each lane */
{
    const dec_shuf a = dec_shuf_prep(lo), b = dec_shuf_prep(hi);
    dec_shuf2 d;
    d.idx = dec_pair(a.idx, b.idx); d.sel = dec_pair(a.sel, b.sel);
    d.spec = dec_pair(a.spec, b.spec); d.specm = dec_pair(a.specm, b.specm);
    d.any_spec = a.any_spec || b.any_spec;
    return d;
}

void gowa_edge_decode_304a0(spu_context* ctx)
{
    if (!(dec_mask() & 8)) return;
    /* r35 runs up by 32 to 0 (the exit test is r35 + 32 != 0) */
    const int32_t c35 = (int32_t)ctx->gpr[35]._u32[0];
    if (c35 >= 0 || (c35 & 31) || (_mm_getcsr() & DEC_MXCSR_RZFZ) != DEC_MXCSR_RZFZ) {
        __atomic_add_fetch(&s_d2_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    const int n = (int)((0u - (uint32_t)c35) >> 5) - 1;
    if (n <= 0) return;
    if (edge_memo_skip_decode()) {
        ctx->gpr[35]._u32[0] = (uint32_t)-32;
        return;
    }
    __atomic_add_fetch(&s_d2_calls, 1, __ATOMIC_RELAXED);

    d304_fix F;
    F.g1 = dec_shuf2_two(dec_ld(ctx, 83), dec_ld(ctx, 34));
    F.g2 = dec_shuf2_two(dec_ld(ctx, 85), dec_ld(ctx, 87));
    F.g3 = dec_shuf2_two(dec_ld(ctx, 84), dec_ld(ctx, 86));
    F.s82 = dec_shuf2_two(dec_ld(ctx, 82), dec_ld(ctx, 82));
    F.s88 = dec_shuf_prep(dec_ld(ctx, 88));
    F.b33 = dec_bits(dec_ld(ctx, 33)); F.b36 = dec_bits(dec_ld(ctx, 36)); F.b37 = dec_bits(dec_ld(ctx, 37));
    F.y33 = dec_both(dec_bytes(dec_ld(ctx, 33))); F.y36 = dec_both(dec_bytes(dec_ld(ctx, 36)));
    F.y37 = dec_both(dec_bytes(dec_ld(ctx, 37))); F.y60 = dec_both(dec_bytes(dec_ld(ctx, 60)));
    F.rm55 = dec_both(dec_cnt_neg(dec_ld(ctx, 55)));
    F.shl67 = dec_both(dec_cnt_shl(dec_ld(ctx, 67))); F.rma66 = dec_both(dec_cnt_neg(dec_ld(ctx, 66)));
    F.r63 = dec_both(dec_ld(ctx, 63)); F.r68 = dec_both(dec_ld(ctx, 68));
    F.r43 = dec_both(dec_ld(ctx, 43)); F.r64 = dec_both(dec_ld(ctx, 64)); F.r80 = dec_both(dec_ld(ctx, 80));
    F.r41 = dec_both(dec_ld(ctx, 41)); F.r53 = dec_both(dec_ld(ctx, 53)); F.r81 = dec_both(dec_ld(ctx, 81));
    F.k42 = dec_both(_mm_andnot_si128(dec_ld(ctx, 41), dec_ld(ctx, 42)));
    F.k54 = dec_both(_mm_andnot_si128(dec_ld(ctx, 53), dec_ld(ctx, 54)));
    F.k77 = dec_both(_mm_and_si128(dec_ld(ctx, 77), dec_ld(ctx, 81)));
    F.r70h = dec_pair(_mm_setzero_si128(), dec_ld(ctx, 70));
    F.r79 = dec_ld(ctx, 79);
    F.r71 = _mm256_castsi256_ps(dec_both(dec_ld(ctx, 71)));
    F.r72 = _mm256_castsi256_ps(dec_both(dec_ld(ctx, 72)));
    F.r73 = _mm256_castsi256_ps(dec_both(dec_ld(ctx, 73)));
    F.a49 = ctx->gpr[49]._u32[0]; F.a65 = ctx->gpr[65]._u32[0]; F.a69 = ctx->gpr[69]._u32[0];
    F.a74 = ctx->gpr[74]._u32[0]; F.a75 = ctx->gpr[75]._u32[0]; F.a76 = ctx->gpr[76]._u32[0];
    F.a78 = ctx->gpr[78]._u32[0];
    d304_state S;
    S.r3_10 = dec_pair(dec_ld(ctx, 3), dec_ld(ctx, 10));
    S.w16_11 = dec_pair(dec_ld(ctx, 16), dec_ld(ctx, 11));
    S.s38_40 = dec_pair(dec_ld(ctx, 38), dec_ld(ctx, 40));
    S.i46_48 = dec_pair(dec_ld(ctx, 46), dec_ld(ctx, 48));
    S.i45_47 = dec_pair(dec_ld(ctx, 45), dec_ld(ctx, 47));
    S.z59_50 = dec_pair(dec_ld(ctx, 59), dec_ld(ctx, 50));
    S.l51_52 = dec_pair(dec_ld(ctx, 51), dec_ld(ctx, 52));
    S.l58_57 = dec_pair(dec_ld(ctx, 58), dec_ld(ctx, 57));
    S.r6 = dec_ld(ctx, 6); S.r35 = dec_ld(ctx, 35); S.r39 = dec_ld(ctx, 39); S.r44 = dec_ld(ctx, 44);
    S.r56 = dec_ld(ctx, 56); S.r61 = dec_ld(ctx, 61); S.r62 = dec_ld(ctx, 62);
    uint8_t* ls = ctx->ls;
    for (int k = 0; k < n; k++) d304_step(&F, &S, ls);
    dec_st(ctx, 3, dec_lo(S.r3_10)); dec_st(ctx, 10, dec_hi(S.r3_10));
    dec_st(ctx, 16, dec_lo(S.w16_11)); dec_st(ctx, 11, dec_hi(S.w16_11));
    dec_st(ctx, 38, dec_lo(S.s38_40)); dec_st(ctx, 40, dec_hi(S.s38_40));
    dec_st(ctx, 46, dec_lo(S.i46_48)); dec_st(ctx, 48, dec_hi(S.i46_48));
    dec_st(ctx, 45, dec_lo(S.i45_47)); dec_st(ctx, 47, dec_hi(S.i45_47));
    dec_st(ctx, 59, dec_lo(S.z59_50)); dec_st(ctx, 50, dec_hi(S.z59_50));
    dec_st(ctx, 51, dec_lo(S.l51_52)); dec_st(ctx, 52, dec_hi(S.l51_52));
    dec_st(ctx, 58, dec_lo(S.l58_57)); dec_st(ctx, 57, dec_hi(S.l58_57));
    dec_st(ctx, 6, S.r6); dec_st(ctx, 35, S.r35); dec_st(ctx, 39, S.r39); dec_st(ctx, 44, S.r44);
    dec_st(ctx, 56, S.r56); dec_st(ctx, 61, S.r61); dec_st(ctx, 62, S.r62);
    dec_count(&s_d2_iters, n);
}

#else
void gowa_edge_decode_30310(spu_context* ctx) { (void)ctx; }
void gowa_edge_decode_304a0(spu_context* ctx) { (void)ctx; }
#endif
