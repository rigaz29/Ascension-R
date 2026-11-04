/* EDGE's context builder (0x88B0, its body at 0x8930): the two loops over the descriptor's tables
 * as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job). 0x88B0 runs once in every job and fills the
 * job's context from two tables of 8-byte entries. A[i] is the byte at r33 + i; 0xFF marks an
 * entry to leave out.
 *  - Loop A (0x8988, with 0x89F0 and 0x8B08), entry i in r28, its entry at r27: unless A[i] is
 *    0xFF, an entry with byte 1 == 8 and byte 3 == 1 takes the long path at 0x8EB8, and any
 *    other has its 8 bytes at r30 + r31 + 8 copied to r26 + 8, after which r26 moves on by 8 and
 *    r32 by 1. r27, r28 and r30 move on every pass; the count is r29's low byte.
 *  - Loop B (0x8B90, with 0x8D34), entry i in r29: unless A[i] is 0xFF, the 8 bytes at r27 + 8
 *    are copied to r28 + 8; then r32's low byte goes to r28 + 8 and A[i] to r28 + 13. Per word,
 *    unless the byte at r27 + 11 + 4w (in its quadword) is 27-30 while r36's word is 0, r28
 *    moves on by 8, r34 by 1 and r32 by the byte at r27 + 12 + 4w. r27 and r29 move on every
 *    pass. The count is the first byte of r35's quadword, loaded again after each copy (r4).
 * The SPU code moves a byte as a quadword load, a rotate that brings it to byte 3, a shuffle with
 * a cbd control and a quadword store. Here it is one byte, at the same address, in the same order.
 *
 * A pass sets only the scratch registers of the path it takes, so both loops run here up to the
 * last pass that copies (or the last pass, when none is left), and the lifted loop makes it and
 * any after it: every register ends as the lifted code leaves it. The passes left are worked out
 * first. The loops stop early before an entry for 0x8EB8 (loop A), and before a copy whose
 * stores would reach the bytes that work rests on (the A bytes still to be read, r35's
 * quadword). A pending interrupt stops either before a pass, for the loop's label to take.
 *
 * GOWA_EDGE_NATIVE bit 2097152 (on by default); GOWA_EDGE_NATIVE_STATS=1 counts. GOWA_EDGE_VERIFY=1
 * checks every call of 0x88B0 (a hook at its entry): the builder with these loops native against
 * it with them lifted. Neither it nor its callees (0x5CF8, 0xBFB8, 0xB600) use a channel, and
 * every register and the whole local store are compared.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_verify.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile long long s_cx_a, s_cx_a_passes, s_cx_b, s_cx_b_passes, s_cx_long, s_cx_reach, s_cx_irq;
static int s_cx_stats;
static void cx_stats(void)
{
    fprintf(stderr, "[edge-native] context loops: A 8988 %lld calls, %lld passes (%lld stops for 8EB8); "
            "B 8B90 %lld calls, %lld passes (%lld stops for the count's quadword); %lld stops for an interrupt\n",
            s_cx_a, s_cx_a_passes, s_cx_long, s_cx_b, s_cx_b_passes, s_cx_reach, s_cx_irq);
}
static int cx_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 2097152) != 0;
        s_cx_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        if (on && s_cx_stats) atexit(cx_stats);
    }
    return on;
}
#define CX_COUNT(v, n) do { if (s_cx_stats) __atomic_add_fetch(&(v), (long long)(n), __ATOMIC_RELAXED); } while (0)
/* the lifted code's check at a loop's label */
#define CX_IRQ(ctx) ((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask))
/* a byte of the local store, by SPU address */
#define LSB(ls, a) ((ls)[(uint32_t)(a) & (SPU_LS_SIZE - 1u)])

static SPU_THREAD_LOCAL spu_context* t_cx_ref;   /* GOWA_EDGE_VERIFY's reference run: off */

static __m128i cx_m(u128 v) { return _mm_loadu_si128((const __m128i*)v._u32); }
static void cx_put(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }
/* r + d in every word (ai, a) */
static void cx_add(spu_context* ctx, int r, uint32_t d)
{
    cx_put(ctx, r, _mm_add_epi32(cx_m(ctx->gpr[r]), _mm_set1_epi32((int)d)));
}

/* [a, a + n) and [b, b + m) overlap in the local store's address space */
static int cx_overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    return ((a - b) & (SPU_LS_SIZE - 1u)) < m || ((b - a) & (SPU_LS_SIZE - 1u)) < n;
}

/* ---- loop A, 0x8988 ----
 * A pass sets the scratch registers of the path it takes, and only a pass that copies (a normal
 * entry, or one for 0x8EB8, which ends in the same copy) sets the copy's. So the last pass that
 * copies, and every pass after it, is the lifted loop's. The count (r29) does not move, so the
 * passes left are known; the native passes' stores must not reach the A bytes still to be read. */
void gowa_edge_ctx_8988(spu_context* ctx)
{
    if (!cx_on() || t_cx_ref == ctx) return;
    uint8_t* ls = ctx->ls;
    const uint32_t a33 = ctx->gpr[33]._u32[0], b31 = ctx->gpr[31]._u32[0];
    const int32_t cnt = (int32_t)(ctx->gpr[29]._u32[0] & 255u);
    const uint32_t i0 = ctx->gpr[28]._u32[0];
    const uint32_t last = cnt > (int32_t)(i0 + 1u) ? (uint32_t)cnt - 1u : i0;
    if (last - i0 > 0x400u) return;
    uint32_t stop = last;   /* the native passes end before the last that copies, or the last */
    for (uint32_t i = last; ; i--) {
        if (LSB(ls, a33 + i) != 0xFF) { stop = i; break; }
        if (i == i0) break;
    }
    uint32_t i = i0, e = ctx->gpr[27]._u32[0], so = ctx->gpr[30]._u32[0], d = ctx->gpr[26]._u32[0];
    uint32_t copies = 0;
    for (; i < stop; i++, e += 8u, so += 8u) {
        if (CX_IRQ(ctx)) { CX_COUNT(s_cx_irq, 1); break; }
        if (LSB(ls, a33 + i) == 0xFF) continue;
        if (LSB(ls, e + 3u) == 1 && LSB(ls, e + 1u) == 8) { CX_COUNT(s_cx_long, 1); break; }
        if (cx_overlap(d + 8u, 8u, a33 + i, last - i + 1u)) { CX_COUNT(s_cx_reach, 1); break; }
        const uint32_t s = so + b31;
        for (uint32_t k = 8; k < 16; k++) LSB(ls, d + k) = LSB(ls, s + k);
        d += 8u;
        copies++;
    }
    const uint32_t done = i - i0;
    if (!done) return;
    cx_add(ctx, 28, done); cx_add(ctx, 27, 8u * done); cx_add(ctx, 30, 8u * done);
    cx_add(ctx, 26, 8u * copies); cx_add(ctx, 32, copies);
    CX_COUNT(s_cx_a, 1);
    CX_COUNT(s_cx_a_passes, done);
}

/* ---- loop B, 0x8B90 ----
 * As in loop A, the last pass that copies, and every pass after it, is the lifted loop's. The
 * count is r4's low byte until a copy, then the first byte of r35's quadword, loaded again after
 * each copy. The passes left are worked out from those first; that holds while the native
 * passes' stores reach neither that quadword nor the A bytes still to be read. */
void gowa_edge_ctx_8b90(spu_context* ctx)
{
    if (!cx_on() || t_cx_ref == ctx) return;
    uint8_t* ls = ctx->ls;
    const uint32_t a33 = ctx->gpr[33]._u32[0];
    const uint32_t q35 = ctx->gpr[35]._u32[0] & (SPU_LS_SIZE - 16u);
    const __m128i z36 = _mm_cmpeq_epi32(cx_m(ctx->gpr[36]), _mm_setzero_si128());   /* r36's words == 0 */
    __m128i r27 = cx_m(ctx->gpr[27]), r28 = cx_m(ctx->gpr[28]), r29 = cx_m(ctx->gpr[29]);
    __m128i r32 = cx_m(ctx->gpr[32]), r34 = cx_m(ctx->gpr[34]), r4 = cx_m(ctx->gpr[4]);
    const uint32_t i0 = (uint32_t)_mm_cvtsi128_si32(r29);
    uint32_t last = i0, stop = 0;
    int copy_left = 0;
    {
        int32_t c = (int32_t)((uint32_t)_mm_cvtsi128_si32(r4) & 255u);
        for (uint32_t i = i0; ; i++) {
            if (i - i0 > 0x400u) return;
            if (LSB(ls, a33 + i) != 0xFF) { c = ls[q35]; stop = i; copy_left = 1; }
            if (!(c > (int32_t)(i + 1u))) { last = i; break; }
        }
    }
    if (!copy_left) stop = last;
    uint32_t i = i0;
    for (; i < stop; i++) {
        if (CX_IRQ(ctx)) { CX_COUNT(s_cx_irq, 1); break; }
        const uint32_t e = (uint32_t)_mm_cvtsi128_si32(r27), d = (uint32_t)_mm_cvtsi128_si32(r28);
        if (LSB(ls, a33 + i) != 0xFF) {
            if (cx_overlap(d + 8u, 8u, q35, 16u) || cx_overlap(d + 8u, 8u, a33 + i, last - i + 1u)) {
                CX_COUNT(s_cx_reach, 1);
                break;
            }
            for (uint32_t k = 8; k < 11; k++) LSB(ls, d + k) = LSB(ls, e + k);
            /* the quadword of r27 + 11 as the SPU code loads it here: its bytes at 11 + 4w */
            const uint32_t q11 = (e + 11u) & (SPU_LS_SIZE - 16u);
            const __m128i v = _mm_setr_epi32(ls[q11 + ((e + 11u) & 15u)], ls[q11 + ((e + 15u) & 15u)],
                                             ls[q11 + ((e + 19u) & 15u)], ls[q11 + ((e + 23u) & 15u)]);
            for (uint32_t k = 11; k < 16; k++) LSB(ls, d + k) = LSB(ls, e + k);
            LSB(ls, d + 8u) = (uint8_t)_mm_cvtsi128_si32(r32);
            LSB(ls, d + 13u) = LSB(ls, a33 + i);
            /* r54: all ones in a word, unless its byte is 27-30 while r36's word is 0 */
            const __m128i in = _mm_cmpgt_epi32(_mm_set1_epi32(4), _mm_sub_epi32(v, _mm_set1_epi32(27)));
            const __m128i ge = _mm_cmpgt_epi32(v, _mm_set1_epi32(26));
            const __m128i m = _mm_andnot_si128(_mm_and_si128(_mm_and_si128(in, ge), z36), _mm_set1_epi32(-1));
            /* the bytes at r27 + 12 + 4w, read after the stores */
            const uint32_t q12 = (e + 12u) & (SPU_LS_SIZE - 16u);
            const __m128i b = _mm_setr_epi32(ls[q12 + ((e + 12u) & 15u)], ls[q12 + ((e + 16u) & 15u)],
                                             ls[q12 + ((e + 20u) & 15u)], ls[q12 + ((e + 24u) & 15u)]);
            r34 = _mm_sub_epi32(r34, m);
            r28 = _mm_add_epi32(r28, _mm_and_si128(m, _mm_set1_epi32(8)));
            r32 = _mm_add_epi32(r32, _mm_and_si128(m, b));
            r4 = cx_m(spu_rotqbyi(spu_ls_read128(ctx, q35), 13));
        }
        r29 = _mm_add_epi32(r29, _mm_set1_epi32(1));
        r27 = _mm_add_epi32(r27, _mm_set1_epi32(8));
    }
    const uint32_t done = i - i0;
    if (!done) return;
    cx_put(ctx, 27, r27); cx_put(ctx, 28, r28); cx_put(ctx, 29, r29);
    cx_put(ctx, 32, r32); cx_put(ctx, 34, r34); cx_put(ctx, 4, r4);
    CX_COUNT(s_cx_b, 1);
    CX_COUNT(s_cx_b_passes, done);
}

/* GOWA_EDGE_VERIFY=1: the whole builder (0x88B0) on a copy with the loops native, against the
 * builder on the real context with them lifted (spu_edge_verify.h). */
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_000088B0(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_cx_guard;
static edge_vstat s_vcx = { "context 88B0 (loops 8988, 8B90 native)", 0, 0, 0, 0, 0, 0, 0, 1 };
static void cx_vreport(void) { edge_vreport(&s_vcx); }
static void cx_lifted(spu_context* ctx)
{
    t_cx_ref = ctx;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_000088B0(ctx);
    t_cx_ref = NULL;
}
static int cx_native(spu_context* cp)
{
    const uint32_t link = cp->gpr[0]._u32[0];
    t_cx_guard = cp;
    cp->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_000088B0(cp);
    spu_drain_call(cp, link);
    cp->host_depth--;
    t_cx_guard = NULL;
    return 1;
}
int gowa_edge_ctx_88b0(spu_context* ctx)
{
    if (t_cx_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) { on = edge_verify_on() && cx_on(); if (on) atexit(cx_vreport); }
    if (!on) return 0;
    return edge_verify(ctx, &s_vcx, cx_native, cx_lifted, &t_cx_guard);
}
