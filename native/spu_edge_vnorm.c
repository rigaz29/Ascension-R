/* EDGE's vector normalization (0xBBC8) as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job). 0xBBC8 (r3 an array of quadwords, r4 its
 * count, rounded up to 8) scales each quadword v in place by its length's reciprocal square
 * root estimate:
 *     L = fma(z, z, fma(y, y, fm(x, x)))       (the SPU's float forms)
 *     s = fi(L, frsqest(L))
 *     v = fm(v, (s, s, s, 1.0))                (w kept: fm by 1.0)
 * Every skinned lit job calls it twice, on the two arrays the transform (0xF538) has just written.
 *
 * The loop (0xBD54) takes a group of eight quadwords a pass, software pipelined three deep.
 * At pass p's head:
 *  - r24 r25 r27 r26 r28 r29 r30 r32 hold group p (quadwords 0-7), r48-r41 group p + 1 and
 *    r39-r36 r35 r34 r33 r40 group p + 2, as loaded;
 *  - r16 | r18 are group p's scales (quadwords 0-3 | 4-7), and r9 | r7 the z and r49 | r10 the
 *    partial sums (x*x + y*y) of group p + 1.
 * A pass stores group p, finishes group p + 1's lengths and scales, starts group p + 2's sums,
 * and loads group p + 3; the groups move r33-r40 -> r41-r48 -> r24-r32. The patterns it
 * shuffles with (r31, r22, r21, r50; the scale patterns r51, r68, r66, r56) transpose four
 * quadwords into x, y and z lanes. The routine runs only when they are the module's (LS 0xFE60,
 * 0xFE70, 0xFF30, 0xFF60), r20 is the select mask and r23 is 1.0.
 *
 * Here a group's quadwords share 256-bit registers (lanes 0-3 its first four quadwords, 4-7 the
 * others, as r16 | r18). The float ops are spu_edge_simd.h's forms: native under the SPU code's
 * MXCSR with the +0 rule, and the exact forms where a result leaves the single range (a zero
 * vector's estimate does). frsqest and fi are the runtime's bit-field rules. All passes but
 * the last run here; the lifted loop makes the last from the registers as it would have them,
 * so every register ends as the lifted code leaves it. A pending interrupt stops it before a
 * pass, for the loop's label to take.
 *
 * GOWA_EDGE_NATIVE bit 524288 (on by default); GOWA_EDGE_NATIVE_STATS=1 counts; GOWA_EDGE_EXACT=1
 * takes the exact float forms for every op (to test them). GOWA_EDGE_VERIFY=1 checks every call
 * of 0xBBC8, with the loop native against fully lifted (gowa_edge_vnorm_bbc8, a hook at its
 * entry that otherwise lets every call through).
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

static volatile long long s_vn_calls, s_vn_passes, s_vn_declined, s_vn_irq;
static int s_vn_stats;
static void vn_stats(void)
{
    fprintf(stderr, "[edge-native] normalize BD54: %lld calls, %lld passes, %lld declined, "
            "%lld stops for an interrupt, %lld exact half-ops\n",
            s_vn_calls, s_vn_passes, s_vn_declined, s_vn_irq, s_ec_exact);
}
static int vn_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 524288) != 0;
        s_vn_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        if (on && s_vn_stats) atexit(vn_stats);
    }
    return on;
}
#define VN_COUNT(v, n) do { if (s_vn_stats) __atomic_add_fetch(&(v), (long long)(n), __ATOMIC_RELAXED); } while (0)
/* the lifted code's check at the loop's label */
#define VN_IRQ(ctx) ((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask))

static SPU_THREAD_LOCAL spu_context* t_vn_ref;   /* GOWA_EDGE_VERIFY's reference run: off */

/* the patterns as the registers hold them (word 0 first) */
static const uint32_t k_a[4]   = { 0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u };   /* r31: x0 x1 y0 y1 */
static const uint32_t k_a8[4]  = { 0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u };   /* r22: r31 by 8 bytes */
static const uint32_t k_b[4]   = { 0x08090A0Bu, 0x18191A1Bu, 0x08090A0Bu, 0x18191A1Bu };   /* r21: z0 z1 z0 z1 */
static const uint32_t k_c[4]   = { 0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u };   /* r50: the y lanes */
static const uint32_t k_d[4]   = { 0x00010203u, 0x00010203u, 0x00010203u, 0x10111213u };   /* r51: s s s 1 */
static const uint32_t k_sel[4] = { 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu };                        /* r20: fsmbi 0xFF */
static const uint32_t k_one[4] = { 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u };   /* r23: 1.0 */

static int reg_is(const spu_context* ctx, int r, const uint32_t w[4], uint32_t orb)
{
    const uint32_t o = orb * 0x01010101u;   /* orbi: the byte into every byte */
    const uint32_t v[4] = { w[0] | o, w[1] | o, w[2] | o, w[3] | o };
    return memcmp(ctx->gpr[r]._u32, v, 16) == 0;
}

/* a group's eight quadwords, words in host order */
typedef struct { __m128i q[8]; } vn_grp;
/* the registers that hold a group at a pass's head, quadwords 0-7 */
static const int k_rg0[8] = { 24, 25, 27, 26, 28, 29, 30, 32 };   /* group p */
static const int k_rg1[8] = { 48, 47, 46, 45, 44, 43, 42, 41 };   /* group p + 1 */
static const int k_rg2[8] = { 39, 38, 37, 36, 35, 34, 33, 40 };   /* group p + 2 */

/* the x, y and z lanes of a group (quadwords 0-3 | 4-7): bit moves only */
EC_INL void vn_xyz(const vn_grp* g, __m256* x, __m256* y, __m256* z)
{
    const __m256 t0 = ec_pair(g->q[0], g->q[4]), t1 = ec_pair(g->q[1], g->q[5]);
    const __m256 t2 = ec_pair(g->q[2], g->q[6]), t3 = ec_pair(g->q[3], g->q[7]);
    const __m256 u0 = _mm256_unpacklo_ps(t0, t1), u1 = _mm256_unpacklo_ps(t2, t3);
    const __m256 u2 = _mm256_unpackhi_ps(t0, t1), u3 = _mm256_unpackhi_ps(t2, t3);
    *x = _mm256_shuffle_ps(u0, u1, 0x44);
    *y = _mm256_shuffle_ps(u0, u1, 0xEE);
    *z = _mm256_shuffle_ps(u2, u3, 0x44);
}
/* the float forms, or (GOWA_EDGE_EXACT=1) the exact ones for every op */
EC_INL __m256 vn_fm(int x, __m256 a, __m256 b) { return x ? ec_fm_x(a, b) : ec_fm(a, b); }
EC_INL __m256 vn_fma(int x, __m256 a, __m256 b, __m256 c) { return x ? ec_fma_x(a, b, c) : ec_fma(a, b, c); }

/* the loop's head, 0xBD54 */
void gowa_edge_vnorm_bd54(spu_context* ctx)
{
    if (!vn_on() || t_vn_ref == ctx) return;
    /* r19 counts up by 128 to 0: the passes left, this one included */
    const int32_t r19w = (int32_t)ctx->gpr[19]._u32[0];
    if (r19w > -256 || (r19w & 127) || r19w < -(int32_t)SPU_LS_SIZE) return;
    if (!reg_is(ctx, 31, k_a, 0) || !reg_is(ctx, 22, k_a8, 0) || !reg_is(ctx, 21, k_b, 0) ||
        !reg_is(ctx, 50, k_c, 0) || !reg_is(ctx, 51, k_d, 0) || !reg_is(ctx, 68, k_d, 4) ||
        !reg_is(ctx, 66, k_d, 8) || !reg_is(ctx, 56, k_d, 12) || !reg_is(ctx, 20, k_sel, 0) ||
        !reg_is(ctx, 23, k_one, 0) || (_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) {
        VN_COUNT(s_vn_declined, 1);
        return;
    }
    const uint32_t n = (uint32_t)(-r19w) / 128u;
    const int fx = ec_force_exact();
    uint8_t* ls = ctx->ls;
    /* the bases: stores of quadwords 0-7 (group p), loads of quadwords 0-7 (group p + 3) */
    const uint32_t st[8] = { ctx->gpr[17]._u32[0], ctx->gpr[69]._u32[0], ctx->gpr[67]._u32[0], ctx->gpr[65]._u32[0],
                             ctx->gpr[64]._u32[0], ctx->gpr[63]._u32[0], ctx->gpr[62]._u32[0], ctx->gpr[61]._u32[0] };
    const uint32_t ld[8] = { ctx->gpr[60]._u32[0], ctx->gpr[59]._u32[0], ctx->gpr[58]._u32[0], ctx->gpr[57]._u32[0],
                             ctx->gpr[55]._u32[0], ctx->gpr[54]._u32[0], ctx->gpr[53]._u32[0], ctx->gpr[52]._u32[0] };
    /* groups p .. p + 3 in a ring */
    vn_grp g[4];
    for (int k = 0; k < 8; k++) {
        g[0].q[k] = ec_m(ctx->gpr[k_rg0[k]]);
        g[1].q[k] = ec_m(ctx->gpr[k_rg1[k]]);
        g[2].q[k] = ec_m(ctx->gpr[k_rg2[k]]);
    }
    __m256 s = ec_join(ctx->gpr[16], ctx->gpr[18]);    /* group p's scales */
    __m256 c1 = ec_join(ctx->gpr[9], ctx->gpr[7]);     /* group p + 1: z */
    __m256 p1 = ec_join(ctx->gpr[49], ctx->gpr[10]);   /* group p + 1: x*x + y*y */
    __m128i r19 = ec_m(ctx->gpr[19]);
    const __m256 one = ec_set(0x3F800000u);

    uint32_t done = 0;
    for (; done + 1 < n; done++) {
        if (VN_IRQ(ctx)) { VN_COUNT(s_vn_irq, 1); break; }
        const vn_grp* g0 = &g[done & 3];
        const vn_grp* g2 = &g[(done + 2) & 3];
        vn_grp* g3 = &g[(done + 3) & 3];
        const uint32_t o = (uint32_t)_mm_cvtsi128_si32(r19);
        /* group p + 1's lengths */
        const __m256 len = vn_fma(fx, c1, c1, p1);
        /* group p, scaled and stored in order, two quadwords an op */
        for (int j = 0; j < 4; j++) {
            const __m256i at = _mm256_setr_epi32(2 * j, 2 * j, 2 * j, 0, 2 * j + 1, 2 * j + 1, 2 * j + 1, 0);
            const __m256 sc = _mm256_blend_ps(_mm256_permutevar8x32_ps(s, at), one, 0x88);
            const __m256 out = vn_fm(fx, ec_pair(g0->q[2 * j], g0->q[2 * j + 1]), sc);
            ec_stq(ls, st[2 * j] + o, ec_lo(out));
            ec_stq(ls, st[2 * j + 1] + o, ec_hi(out));
        }
        /* group p + 1's scales */
        const __m256i lb = _mm256_castps_si256(len);
        s = _mm256_castsi256_ps(ec_fi(lb, ec_frsqest(lb)));
        /* group p + 2's sums */
        __m256 x, y, z;
        vn_xyz(g2, &x, &y, &z);
        p1 = vn_fma(fx, y, y, vn_fm(fx, x, x));
        c1 = z;
        /* group p + 3 */
        for (int k = 0; k < 8; k++) g3->q[k] = ec_ldq(ls, ld[k] + o);
        r19 = _mm_add_epi32(r19, _mm_set1_epi32(128));
    }
    if (!done) return;
    const vn_grp* g0 = &g[done & 3];
    const vn_grp* g1 = &g[(done + 1) & 3];
    const vn_grp* g2 = &g[(done + 2) & 3];
    for (int k = 0; k < 8; k++) {
        ctx->gpr[k_rg0[k]] = ec_u(g0->q[k]);
        ctx->gpr[k_rg1[k]] = ec_u(g1->q[k]);
        ctx->gpr[k_rg2[k]] = ec_u(g2->q[k]);
    }
    ctx->gpr[16] = ec_u(ec_lo(s)); ctx->gpr[18] = ec_u(ec_hi(s));
    ctx->gpr[9] = ec_u(ec_lo(c1)); ctx->gpr[7] = ec_u(ec_hi(c1));
    ctx->gpr[49] = ec_u(ec_lo(p1)); ctx->gpr[10] = ec_u(ec_hi(p1));
    ctx->gpr[19] = ec_u(r19);
    if (s_vn_stats) {
        VN_COUNT(s_vn_passes, done);
        /* also every 65536 calls: a game run is ended by a kill, so atexit never comes */
        if (!(__atomic_add_fetch(&s_vn_calls, 1, __ATOMIC_RELAXED) & 0xFFFF)) vn_stats();
    }
}

/* GOWA_EDGE_VERIFY=1: the whole function (0xBBC8) on a copy with the loop native, against the
 * function on the real context without it (spu_edge_verify.h). It is a leaf without a frame,
 * and every register is compared. */
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000BBC8(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_vn_guard;
static edge_vstat s_vvn = { "normalize BBC8 (loop BD54 native)", 0, 0, 0, 0, 0, 0, 0, 1 };
static void vn_vreport(void) { edge_vreport(&s_vvn); }
static void vn_lifted(spu_context* ctx)
{
    t_vn_ref = ctx;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_0000BBC8(ctx);
    t_vn_ref = NULL;
}
static int vn_native(spu_context* c)
{
    const uint32_t link = c->gpr[0]._u32[0];
    t_vn_guard = c;
    c->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_0000BBC8(c);
    spu_drain_call(c, link);
    c->host_depth--;
    t_vn_guard = NULL;
    return 1;
}
int gowa_edge_vnorm_bbc8(spu_context* ctx)
{
    if (t_vn_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) { on = edge_verify_on() && vn_on(); if (on) atexit(vn_vreport); }
    if (!on) return 0;
    return edge_verify(ctx, &s_vvn, vn_native, vn_lifted, &t_vn_guard);
}

#else
void gowa_edge_vnorm_bd54(spu_context* ctx) { (void)ctx; }
int gowa_edge_vnorm_bbc8(spu_context* ctx) { (void)ctx; return 0; }
#endif
