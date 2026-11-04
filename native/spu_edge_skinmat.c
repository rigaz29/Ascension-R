/* EDGE's skinning driver (0xE550): the palette's matrix layout (0xE804) and the normal and tangent
 * transform (0xF104) as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job). 0xE550 skins the vertexes of a skinned job:
 *  - 0xE804, in every skinned job, lays the bones' matrixes out in place: each matrix's four
 *    quadwords q0-q3 become three,
 *        (q0.x q1.y q2.z q3.x)  (q1.x q2.y q0.z q3.y)  (q2.x q0.y q1.z q3.z),
 *    the "diagonals" a vector is multiplied with (the patterns at LS 0x10290-0x102B0, r28 r27
 *    r26, and the word masks r15 and r22);
 *  - 0xEC08 is the position loop (spu_edge_skin.c);
 *  - 0xF104, in the skinned lit jobs, transforms two attribute streams (r62, r65) by the
 *    per-vertex matrixes at r40 (three quadwords D0 D1 D2 a vertex, in that layout), four
 *    vertexes a pass, in place:
 *        out = fma(zxyw(v), D2, fma(yzxw(v), D1, fm(v, D0)))
 *    with yzxw and zxyw the patterns r36 (LS 0xFEF0) and r82 (r36 shuffled by itself).
 *
 * Both loops are software pipelined. 0xE804 loads matrix k + 1 before it stores matrix k from
 * the parts r12 r14 r16 r17 r13 r18. 0xF104 loads stream B's vertexes (r65) in the pass, but
 * stream A's (r62: r37 r35 r34 r38, with their yzxw r60 r59 r55 r54 and zxyw r56 r53 r57 r58)
 * and the matrixes (r52-r41) a pass ahead. A pass does all its loads, then its stores, in the
 * lifted order. All passes but the last run here; the lifted loop makes the last from the
 * registers as it would have them, so every register ends as the lifted code leaves it. A
 * pending interrupt stops a loop before a pass, for its label to take.
 *
 * 0xF104 keeps two vertexes in a 256-bit register, and its pipeline in eight of them. Its float
 * ops run native under the SPU code's MXCSR, with one range test of all twelve results of a
 * pass: a lane at or past FLT_MAX (where an overflow toward zero lands) or NaN (from an operand
 * of exponent 255) has the pass done again in spu_edge_simd.h's exact forms, op by op, from the
 * same inputs. The +0 rule is applied to what is stored: under round toward zero a zero's sign
 * never changes a non-zero result. The routines run only when the patterns and masks are the
 * module's.
 *
 * GOWA_EDGE_NATIVE bit 1048576 (on by default); GOWA_EDGE_NATIVE_STATS=1 counts; GOWA_EDGE_EXACT=1
 * takes the exact float forms for every op. GOWA_EDGE_VERIFY=1 checks every call of 0xE550 (a
 * hook at its entry): the whole driver with these loops native against it with them lifted;
 * it uses no channel, and every register and the whole local store are compared.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_simd.h"
#include "spu_edge_verify.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

static volatile long long s_sm_e804, s_sm_e804_passes, s_sm_f104, s_sm_f104_passes, s_sm_declined, s_sm_irq;
static volatile long long s_sm_redone;   /* 0xF104 passes redone in the exact forms */
static int s_sm_stats;
static void sm_stats(void)
{
    fprintf(stderr, "[edge-native] skinning layout E804: %lld calls, %lld passes; transform F104: %lld calls, "
            "%lld passes (%lld redone exact); %lld declined, %lld stops for an interrupt, %lld exact half-ops\n",
            s_sm_e804, s_sm_e804_passes, s_sm_f104, s_sm_f104_passes, s_sm_redone, s_sm_declined, s_sm_irq,
            s_ec_exact);
}
static int sm_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 1048576) != 0;
        s_sm_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        if (on && s_sm_stats) atexit(sm_stats);
    }
    return on;
}
#define SM_COUNT(v, n) do { if (s_sm_stats) __atomic_add_fetch(&(v), (long long)(n), __ATOMIC_RELAXED); } while (0)
/* the lifted code's check at a loop's label */
#define SM_IRQ(ctx) ((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask))
/* also every 65536 calls of the transform: a game run is ended by a kill, so atexit never comes */
static void sm_tick(volatile long long* calls)
{
    if (s_sm_stats && !(__atomic_add_fetch(calls, 1, __ATOMIC_RELAXED) & 0xFFFF)) sm_stats();
}

static SPU_THREAD_LOCAL spu_context* t_sm_ref;   /* GOWA_EDGE_VERIFY's reference run: off */

static int reg_is(const spu_context* ctx, int r, const uint32_t w[4])
{
    return memcmp(ctx->gpr[r]._u32, w, 16) == 0;
}
static void put(spu_context* ctx, int r, __m128i v) { ctx->gpr[r] = ec_u(v); }

/* ---- 0xE804: the matrix layout ---- */

static const uint32_t k_p0[4] = { 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x10111213u };   /* r28: a.z a.z a.z b.x */
static const uint32_t k_p1[4] = { 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x14151617u };   /* r27: a.z a.z a.z b.y */
static const uint32_t k_p2[4] = { 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x18191A1Bu };   /* r26: a.z a.z a.z b.z */
static const uint32_t k_w1[4] = { 0, 0xFFFFFFFFu, 0, 0 };                                  /* r15: fsmbi 0xF00 */
static const uint32_t k_w23[4] = { 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu };                       /* r22: fsmbi 0xFF */

/* shufb(a, b, (a.z a.z a.z b.w)) for the word w of b */
#define SM_ZZZ(a, b, w) _mm_blend_epi32(_mm_shuffle_epi32((a), 0xAA), _mm_shuffle_epi32((b), (w) * 0x55), 0x8)

void gowa_edge_skinmat_e804(spu_context* ctx)
{
    if (!sm_on() || t_sm_ref == ctx) return;
    /* r11 counts up by 64 to 0: the passes left, this one included */
    const int32_t r11w = (int32_t)ctx->gpr[11]._u32[0];
    if (r11w > -128 || (r11w & 63) || r11w < -(int32_t)SPU_LS_SIZE) return;
    if (!reg_is(ctx, 28, k_p0) || !reg_is(ctx, 27, k_p1) || !reg_is(ctx, 26, k_p2) ||
        !reg_is(ctx, 15, k_w1) || !reg_is(ctx, 22, k_w23)) {
        SM_COUNT(s_sm_declined, 1);
        return;
    }
    const uint32_t n = (uint32_t)(-r11w) / 64u;
    uint8_t* ls = ctx->ls;
    /* loads of the next matrix's q3 q0 q1 q2; stores of the three quadwords */
    const uint32_t l3 = ctx->gpr[19]._u32[0], l0 = ctx->gpr[25]._u32[0];
    const uint32_t l1 = ctx->gpr[21]._u32[0], l2 = ctx->gpr[20]._u32[0];
    const uint32_t s0 = ctx->gpr[10]._u32[0], s1 = ctx->gpr[24]._u32[0], s2 = ctx->gpr[23]._u32[0];
    __m128i r12 = ec_m(ctx->gpr[12]), r14 = ec_m(ctx->gpr[14]), r16 = ec_m(ctx->gpr[16]);
    __m128i r17 = ec_m(ctx->gpr[17]), r13 = ec_m(ctx->gpr[13]), r18 = ec_m(ctx->gpr[18]);
    __m128i r11 = ec_m(ctx->gpr[11]), r9 = ec_m(ctx->gpr[9]);
    uint32_t done = 0;
    for (; done + 1 < n; done++) {
        if (SM_IRQ(ctx)) { SM_COUNT(s_sm_irq, 1); break; }
        const uint32_t o = (uint32_t)_mm_cvtsi128_si32(r11), d = (uint32_t)_mm_cvtsi128_si32(r9);
        const __m128i q3 = ec_ldq(ls, l3 + o), q0 = ec_ldq(ls, l0 + o);
        const __m128i q1 = ec_ldq(ls, l1 + o), q2 = ec_ldq(ls, l2 + o);
        ec_stq(ls, s1 + d, _mm_blend_epi32(r14, r17, 0xC));   /* selb(r14, r17, r22) */
        ec_stq(ls, s2 + d, _mm_blend_epi32(r16, r13, 0xC));   /* selb(r16, r13, r22) */
        ec_stq(ls, s0 + d, _mm_blend_epi32(r12, r18, 0xC));   /* selb(r12, r18, r22) */
        r17 = SM_ZZZ(q0, q3, 1);
        r12 = _mm_blend_epi32(q0, q1, 0x2);                   /* selb(q0, q1, r15) */
        r13 = SM_ZZZ(q1, q3, 2);
        r16 = _mm_blend_epi32(q2, q0, 0x2);
        r18 = SM_ZZZ(q2, q3, 0);
        r14 = _mm_blend_epi32(q1, q2, 0x2);
        r11 = _mm_add_epi32(r11, _mm_set1_epi32(64));
        r9 = _mm_add_epi32(r9, _mm_set1_epi32(48));
    }
    if (!done) return;
    put(ctx, 12, r12); put(ctx, 14, r14); put(ctx, 16, r16);
    put(ctx, 17, r17); put(ctx, 13, r13); put(ctx, 18, r18);
    put(ctx, 11, r11); put(ctx, 9, r9);
    SM_COUNT(s_sm_e804_passes, done);
    SM_COUNT(s_sm_e804, 1);
}

/* ---- 0xF104: the normal and tangent transform ---- */

static const uint32_t k_yzx[4] = { 0x04050607u, 0x08090A0Bu, 0x00010203u, 0x0C0D0E0Fu };   /* r36 */
static const uint32_t k_zxy[4] = { 0x08090A0Bu, 0x00010203u, 0x04050607u, 0x0C0D0E0Fu };   /* r82 */

EC_INL __m256 sm_yzx(__m256 v) { return _mm256_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); }
EC_INL __m256 sm_zxy(__m256 v) { return _mm256_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); }

/* the registers at the loop's head */
/* Two quadwords of the local store as one 256-bit register, words in host order, and back. */
#define SM_BSWAP8 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12
EC_INL __m256 sm_ld2(const uint8_t* ls, uint32_t a0, uint32_t a1)
{
    const __m128i lo = _mm_loadu_si128((const __m128i*)(ls + (a0 & (SPU_LS_SIZE - 16))));
    const __m128i hi = _mm_loadu_si128((const __m128i*)(ls + (a1 & (SPU_LS_SIZE - 16))));
    return _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1),
                                                   _mm256_setr_epi8(SM_BSWAP8, SM_BSWAP8)));
}
EC_INL __m256i sm_be(__m256 v)
{
    return _mm256_shuffle_epi8(_mm256_castps_si256(v), _mm256_setr_epi8(SM_BSWAP8, SM_BSWAP8));
}
EC_INL void sm_st(uint8_t* ls, uint32_t a, __m128i be)
{
    _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_SIZE - 16))), be);
}
/* the lanes at or past FLT_MAX in magnitude, or NaN: where the native forms and the SPU's differ */
EC_INL __m256 sm_out(__m256 acc, __m256 r)
{
    const __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
    return _mm256_or_ps(acc, _mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ));
}

/* the registers at the loop's head: D0 D1 D2 of vertexes 0-3; stream A's vertexes, their yzxw
 * and zxyw */
static const int k_rm[12] = { 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41 };
static const int k_ra[4] = { 37, 35, 34, 38 };
static const int k_r36[4] = { 60, 59, 55, 54 };
static const int k_r82[4] = { 56, 53, 57, 58 };

void gowa_edge_skinmat_f104(spu_context* ctx)
{
    if (!sm_on() || t_sm_ref == ctx) return;
    /* r21 counts down by 4 to 0: the passes left, this one included */
    const uint32_t c = ctx->gpr[21]._u32[0];
    if (c < 8u || (c & 3u) || c > 4u * 0x4000u) return;
    if (!reg_is(ctx, 36, k_yzx) || !reg_is(ctx, 82, k_zxy) ||
        (_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) {
        SM_COUNT(s_sm_declined, 1);
        return;
    }
    const uint32_t n = c / 4u;
    const int fx = ec_force_exact();
    uint8_t* ls = ctx->ls;
    /* vertexes 0 1 and 2 3, two to a register: the matrixes' D0 D1 D2 and stream A's vertexes.
     * The registers' yzxw and zxyw of stream A are those shuffles of its vertexes. */
    __m256 d0[2], d1[2], d2[2], va[2];
    for (int h = 0; h < 2; h++) {
        d0[h] = ec_join(ctx->gpr[k_rm[6 * h]], ctx->gpr[k_rm[6 * h + 3]]);
        d1[h] = ec_join(ctx->gpr[k_rm[6 * h + 1]], ctx->gpr[k_rm[6 * h + 4]]);
        d2[h] = ec_join(ctx->gpr[k_rm[6 * h + 2]], ctx->gpr[k_rm[6 * h + 5]]);
        va[h] = ec_join(ctx->gpr[k_ra[2 * h]], ctx->gpr[k_ra[2 * h + 1]]);
    }
    uint32_t pa = ctx->gpr[62]._u32[0], pb = ctx->gpr[65]._u32[0], pm = ctx->gpr[40]._u32[0];
    uint32_t done = 0;
    for (; done + 1 < n; done++) {
        if (SM_IRQ(ctx)) { SM_COUNT(s_sm_irq, 1); break; }
        /* stream B's vertexes, then the outputs: the fast forms, and one range test for all */
        const __m256 vb[2] = { sm_ld2(ls, pb, pb + 0x10u), sm_ld2(ls, pb + 0x20u, pb + 0x30u) };
        __m256 oa[2], ob[2], out = _mm256_setzero_ps();
        for (int h = 0; h < 2; h++) {
            const __m256 ta = _mm256_mul_ps(va[h], d0[h]);
            const __m256 ua = _mm256_fmadd_ps(sm_yzx(va[h]), d1[h], ta);
            oa[h] = _mm256_fmadd_ps(sm_zxy(va[h]), d2[h], ua);
            const __m256 tb = _mm256_mul_ps(vb[h], d0[h]);
            const __m256 ub = _mm256_fmadd_ps(sm_yzx(vb[h]), d1[h], tb);
            ob[h] = _mm256_fmadd_ps(sm_zxy(vb[h]), d2[h], ub);
            out = sm_out(sm_out(sm_out(out, ta), ua), oa[h]);
            out = sm_out(sm_out(sm_out(out, tb), ub), ob[h]);
        }
        if (__builtin_expect(fx || _mm256_movemask_ps(out), 0)) {
            /* the exact forms, op by op, from the same inputs */
            for (int h = 0; h < 2; h++) {
                oa[h] = ec_fma_x(sm_zxy(va[h]), d2[h], ec_fma_x(sm_yzx(va[h]), d1[h], ec_fm_x(va[h], d0[h])));
                ob[h] = ec_fma_x(sm_zxy(vb[h]), d2[h], ec_fma_x(sm_yzx(vb[h]), d1[h], ec_fm_x(vb[h], d0[h])));
            }
            SM_COUNT(s_sm_redone, 1);
        } else {
            for (int h = 0; h < 2; h++) { oa[h] = ec_pz(oa[h]); ob[h] = ec_pz(ob[h]); }
        }
        /* the next pass's stream A and matrixes, loaded before this pass's stores */
        const __m256 na[2] = { sm_ld2(ls, pa + 0x40u, pa + 0x50u), sm_ld2(ls, pa + 0x60u, pa + 0x70u) };
        const __m256 n0[2] = { sm_ld2(ls, pm, pm + 0x30u), sm_ld2(ls, pm + 0x60u, pm + 0x90u) };
        const __m256 n1[2] = { sm_ld2(ls, pm + 0x10u, pm + 0x40u), sm_ld2(ls, pm + 0x70u, pm + 0xA0u) };
        const __m256 n2[2] = { sm_ld2(ls, pm + 0x20u, pm + 0x50u), sm_ld2(ls, pm + 0x80u, pm + 0xB0u) };
        /* the stores, in the lifted order: A 0 3 1 2, then B 0 3 1 2 */
        const __m256i a01 = sm_be(oa[0]), a23 = sm_be(oa[1]), b01 = sm_be(ob[0]), b23 = sm_be(ob[1]);
        sm_st(ls, pa + 0x00u, _mm256_castsi256_si128(a01));
        sm_st(ls, pa + 0x30u, _mm256_extracti128_si256(a23, 1));
        sm_st(ls, pa + 0x10u, _mm256_extracti128_si256(a01, 1));
        sm_st(ls, pa + 0x20u, _mm256_castsi256_si128(a23));
        sm_st(ls, pb + 0x00u, _mm256_castsi256_si128(b01));
        sm_st(ls, pb + 0x30u, _mm256_extracti128_si256(b23, 1));
        sm_st(ls, pb + 0x10u, _mm256_extracti128_si256(b01, 1));
        sm_st(ls, pb + 0x20u, _mm256_castsi256_si128(b23));
        for (int h = 0; h < 2; h++) { va[h] = na[h]; d0[h] = n0[h]; d1[h] = n1[h]; d2[h] = n2[h]; }
        pa += 64u; pb += 64u; pm += 192u;
    }
    if (!done) return;
    for (int h = 0; h < 2; h++) {
        put(ctx, k_rm[6 * h], ec_lo(d0[h])); put(ctx, k_rm[6 * h + 3], ec_hi(d0[h]));
        put(ctx, k_rm[6 * h + 1], ec_lo(d1[h])); put(ctx, k_rm[6 * h + 4], ec_hi(d1[h]));
        put(ctx, k_rm[6 * h + 2], ec_lo(d2[h])); put(ctx, k_rm[6 * h + 5], ec_hi(d2[h]));
        const __m128i v0 = ec_lo(va[h]), v1 = ec_hi(va[h]);
        put(ctx, k_ra[2 * h], v0); put(ctx, k_ra[2 * h + 1], v1);
        put(ctx, k_r36[2 * h], _mm_shuffle_epi32(v0, _MM_SHUFFLE(3, 0, 2, 1)));
        put(ctx, k_r36[2 * h + 1], _mm_shuffle_epi32(v1, _MM_SHUFFLE(3, 0, 2, 1)));
        put(ctx, k_r82[2 * h], _mm_shuffle_epi32(v0, _MM_SHUFFLE(3, 1, 0, 2)));
        put(ctx, k_r82[2 * h + 1], _mm_shuffle_epi32(v1, _MM_SHUFFLE(3, 1, 0, 2)));
    }
    /* r21, r40, r62, r65: every word moves with the first */
    put(ctx, 21, _mm_sub_epi32(ec_m(ctx->gpr[21]), _mm_set1_epi32((int)(4u * done))));
    put(ctx, 40, _mm_add_epi32(ec_m(ctx->gpr[40]), _mm_set1_epi32((int)(192u * done))));
    put(ctx, 62, _mm_add_epi32(ec_m(ctx->gpr[62]), _mm_set1_epi32((int)(64u * done))));
    put(ctx, 65, _mm_add_epi32(ec_m(ctx->gpr[65]), _mm_set1_epi32((int)(64u * done))));
    SM_COUNT(s_sm_f104_passes, done);
    sm_tick(&s_sm_f104);
}
/* GOWA_EDGE_VERIFY=1: the whole driver (0xE550) on a copy with these loops native, against the
 * driver on the real context with them lifted (spu_edge_verify.h). Neither it nor its callees
 * use a channel. */
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E550(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_sm_guard;
static edge_vstat s_vsm = { "skinning E550 (loops E804, F104 native)", 0, 0, 0, 0, 0, 0, 0, 1 };
static void sm_vreport(void) { edge_vreport(&s_vsm); }
static void sm_lifted(spu_context* ctx)
{
    t_sm_ref = ctx;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E550(ctx);
    t_sm_ref = NULL;
}
static int sm_native(spu_context* cp)
{
    const uint32_t link = cp->gpr[0]._u32[0];
    t_sm_guard = cp;
    cp->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E550(cp);
    spu_drain_call(cp, link);
    cp->host_depth--;
    t_sm_guard = NULL;
    return 1;
}
int gowa_edge_skinmat_e550(spu_context* ctx)
{
    if (t_sm_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) { on = edge_verify_on() && sm_on(); if (on) atexit(sm_vreport); }
    if (!on) return 0;
    return edge_verify(ctx, &s_vsm, sm_native, sm_lifted, &t_sm_guard);
}

#else
void gowa_edge_skinmat_e804(spu_context* ctx) { (void)ctx; }
void gowa_edge_skinmat_f104(spu_context* ctx) { (void)ctx; }
int gowa_edge_skinmat_e550(spu_context* ctx) { (void)ctx; return 0; }
#endif
