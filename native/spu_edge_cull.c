/*
 * EDGE's triangle culling as native code (job module e8f44fbc6dbf; docs/notes/2026-11-03-edge-stages.md).
 *
 * 0xD250, the vertex pass (GOWA_EDGE_NATIVE bit 128, off by default while it is new):
 *  - D640 (the culling) calls it once a call, before its triangle loop.
 *  - For each vertex it writes a 16-byte record (sx, sy, flags, 0).
 *  - flags:
 *    bit 7 sx > xmax    bit 6 xmin > sx
 *    bit 5 sy > ymax    bit 4 ymin > sy
 *    bit 3 sz > zmax    bit 2 zmin > sz
 *    bit 1 W > 0
 *  - The math, per vertex (X, Y, Z) read from a 16-byte record:
 *    - C = A * B, the matrices at ctx + 0xE0 and ctx + 0xA0, B's last row from LS 0xF990;
 *    - x' = fma(Z, C0.z, fma(Y, C0.y, fma(X, C0.x, C0.w))), and y', z', W the same with C1-C3;
 *    - 1/W = fma(1 - r W, r, r) with r = fi(W, frest(W)) (the 1 - r W fused);
 *    - p = x' * (1/W) with W's sign xor'ed in, s = fma(p, scale, offset) (ctx + 0x120, + 0x130);
 *    - the bounds:
 *      - xy from the halfwords at ctx + 0xD0 (x, y, width, height; the sums wrap at 16 bits);
 *      - z from its two floats, the lesser first.
 *  - The context:
 *    - the vertex count is the halfword at ctx + 8;
 *    - the input is at the word at ctx + 0x5C;
 *    - the output at the word at ctx + 0x10 + 4 * (ctx[0] & 15).
 *  - The lifted loop takes four vertices a pass, pipelined three deep. It runs two passes more
 *    than it has groups of four; their stores land on the first group's slots, which the first
 *    real group overwrites.
 *  - This takes eight a pass, in the same operations per element, with the exact SPU float
 *    forms. A group past the vertex count is computed and stored as the lifted loop does.
 *
 * Declined (the lifted function runs instead):
 *  - another MXCSR than the SPU code's;
 *  - no vertices;
 *  - a context not 16-byte aligned;
 *  - input or output past the local store;
 *  - an output that overlaps the input other than exactly (the lifted loop's order of loads and
 *    stores would show).
 *
 * GOWA_EDGE_VERIFY=1 checks every call against the lifted function (spu_edge_verify.h).
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_verify.h"
#include "spu_edge_simd.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

/* ------------------------------------------------------------------ 0xD250 */

static volatile long long s_d250_calls, s_d250_verts, s_d250_declined;

static int d250_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return 0;
    uint8_t* const ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0];
    if ((c & 15) || c > SPU_LS_SIZE - 0x140) return 0;
    const __m128i hdr = ec_ldq(ls, c);
    const uint32_t n = (uint32_t)_mm_extract_epi32(hdr, 2) >> 16;
    if (!n) return 0;
    const uint32_t k = ((uint32_t)_mm_extract_epi32(hdr, 0) >> 24) & 15;
    const uint32_t out_a = ec_be32(ls + c + 0x10 + 4 * k) & (SPU_LS_SIZE - 16);
    const uint32_t in_a = ec_be32(ls + c + 0x5C) & (SPU_LS_SIZE - 16);
    const uint32_t groups = (n + 3) >> 2;
    if (in_a + 64 * (groups + 2) > SPU_LS_SIZE || out_a + 64 * groups > SPU_LS_SIZE) return 0;
    if (out_a != in_a && out_a < in_a + 64 * (groups + 2) && in_a < out_a + 64 * groups) return 0;

    /* C = A * B as the prologue makes it, four lanes at a time with the runtime's forms */
    u128 A[4], B[4], C[4];
    for (int i = 0; i < 4; i++) A[i] = ec_u(ec_ldq(ls, c + 0xE0 + 16 * i));
    for (int i = 0; i < 3; i++) B[i] = ec_u(ec_ldq(ls, c + 0xA0 + 16 * i));
    B[3] = ec_u(ec_ldq(ls, 0xF990));
    for (int i = 0; i < 4; i++)
        C[i] = spu_fma(ec_splat(A[i], 3), B[3],
                       spu_fma(ec_splat(A[i], 2), B[2],
                               spu_fma(ec_splat(A[i], 1), B[1], spu_fm(ec_splat(A[i], 0), B[0]))));
    const u128 S = ec_u(ec_ldq(ls, c + 0x120)), O = ec_u(ec_ldq(ls, c + 0x130)), D = ec_u(ec_ldq(ls, c + 0xD0));
    const uint32_t h0 = D._u32[0] >> 16, h1 = D._u32[0] & 0xFFFF, h2 = D._u32[1] >> 16, h3 = D._u32[1] & 0xFFFF;
    const u128 zgt = spu_fcgt(ec_splat(D, 2), ec_splat(D, 3));
    const uint32_t zmin_b = zgt._u32[0] ? D._u32[3] : D._u32[2], zmax_b = zgt._u32[0] ? D._u32[2] : D._u32[3];

    const __m256 c0x = ec_set(C[0]._u32[0]), c0y = ec_set(C[0]._u32[1]), c0z = ec_set(C[0]._u32[2]), c0w = ec_set(C[0]._u32[3]);
    const __m256 c1x = ec_set(C[1]._u32[0]), c1y = ec_set(C[1]._u32[1]), c1z = ec_set(C[1]._u32[2]), c1w = ec_set(C[1]._u32[3]);
    const __m256 c2x = ec_set(C[2]._u32[0]), c2y = ec_set(C[2]._u32[1]), c2z = ec_set(C[2]._u32[2]), c2w = ec_set(C[2]._u32[3]);
    const __m256 c3x = ec_set(C[3]._u32[0]), c3y = ec_set(C[3]._u32[1]), c3z = ec_set(C[3]._u32[2]), c3w = ec_set(C[3]._u32[3]);
    const __m256 sxs = ec_set(S._u32[0]), sys = ec_set(S._u32[1]), szs = ec_set(S._u32[2]);
    const __m256 oxs = ec_set(O._u32[0]), oys = ec_set(O._u32[1]), ozs = ec_set(O._u32[2]);
    const __m256 xmin = _mm256_set1_ps((float)h0), ymin = _mm256_set1_ps((float)h1);
    const __m256 xmax = _mm256_set1_ps((float)((h0 + h2) & 0xFFFF)), ymax = _mm256_set1_ps((float)((h1 + h3) & 0xFFFF));
    const __m256 zmin = ec_set(zmin_b), zmax = ec_set(zmax_b);
    const __m256 one = ec_set(0x3F800000u), sign = ec_set(0x80000000u), zero = _mm256_setzero_ps();

    for (uint32_t g = 0; g < groups; g += 2) {
        const uint32_t a = in_a + 64 * g;
        const __m256 r0 = ec_pair(ec_ldq(ls, a), ec_ldq(ls, a + 64));
        const __m256 r1 = ec_pair(ec_ldq(ls, a + 16), ec_ldq(ls, a + 80));
        const __m256 r2 = ec_pair(ec_ldq(ls, a + 32), ec_ldq(ls, a + 96));
        const __m256 r3 = ec_pair(ec_ldq(ls, a + 48), ec_ldq(ls, a + 112));
        const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
        const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
        const __m256 X = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 Y = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 Z = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));

        const __m256 xp = ec_fma(Z, c0z, ec_fma(Y, c0y, ec_fma(X, c0x, c0w)));
        const __m256 yp = ec_fma(Z, c1z, ec_fma(Y, c1y, ec_fma(X, c1x, c1w)));
        const __m256 zp = ec_fma(Z, c2z, ec_fma(Y, c2y, ec_fma(X, c2x, c2w)));
        const __m256 W = ec_fma(Z, c3z, ec_fma(Y, c3y, ec_fma(X, c3x, c3w)));
        const __m256i wi = _mm256_castps_si256(W);
        const __m256 r = _mm256_castsi256_ps(ec_fi(wi, ec_frest(wi)));
        const __m256 rcp = ec_fma(ec_fnms(r, W, one), r, r);
        const __m256 ws = _mm256_and_ps(W, sign);
        const __m256 px = _mm256_xor_ps(ec_fm(xp, rcp), ws);
        const __m256 py = _mm256_xor_ps(ec_fm(yp, rcp), ws);
        const __m256 pz = _mm256_xor_ps(ec_fm(zp, rcp), ws);
        const __m256 sx = ec_fma(px, sxs, oxs), sy = ec_fma(py, sys, oys), sz = ec_fma(pz, szs, ozs);

        __m256i f = _mm256_and_si256(_mm256_castps_si256(ec_fcgt(sx, xmax)), _mm256_set1_epi32(0x80));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(xmin, sx)), _mm256_set1_epi32(0x40)));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(sy, ymax)), _mm256_set1_epi32(0x20)));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(ymin, sy)), _mm256_set1_epi32(0x10)));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(sz, zmax)), _mm256_set1_epi32(0x08)));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(zmin, sz)), _mm256_set1_epi32(0x04)));
        f = _mm256_or_si256(f, _mm256_and_si256(_mm256_castps_si256(ec_fcgt(W, zero)), _mm256_set1_epi32(0x02)));

        /* records (sx, sy, flags, 0): vertices 0-3 in the low lanes' order, 4-7 in the high */
        const __m256 fl = _mm256_castsi256_ps(f);
        const __m256 u0 = _mm256_unpacklo_ps(sx, sy), u1 = _mm256_unpackhi_ps(sx, sy);
        const __m256 v0 = _mm256_unpacklo_ps(fl, zero), v1 = _mm256_unpackhi_ps(fl, zero);
        const __m256 q0 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 q1 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 q2 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 q3 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(3, 2, 3, 2));
        const uint32_t o = out_a + 64 * g;
        ec_stq(ls, o, ec_lo(q0));
        ec_stq(ls, o + 16, ec_lo(q1));
        ec_stq(ls, o + 32, ec_lo(q2));
        ec_stq(ls, o + 48, ec_lo(q3));
        if (g + 1 < groups) {
            ec_stq(ls, o + 64, ec_hi(q0));
            ec_stq(ls, o + 80, ec_hi(q1));
            ec_stq(ls, o + 96, ec_hi(q2));
            ec_stq(ls, o + 112, ec_hi(q3));
        }
    }
    __atomic_add_fetch(&s_d250_verts, (long long)n, __ATOMIC_RELAXED);
    return 1;
}

static void cull_stats(void)
{
    fprintf(stderr, "[edge-native] cull D250: %lld calls native, %lld vertices, %lld declined, %lld exact ops\n",
            s_d250_calls, s_d250_verts, s_d250_declined, s_ec_exact);
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D250(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_d250_guard;
static edge_vstat s_v250 = { "cull D250", 0x200, 0, 0, 0, 0, 0, 0 };
static void cull_verify_report(void) { edge_vreport(&s_v250); }

/* The hook at 0xD250's entry: 1 when the call is done (the function returns). */
int gowa_edge_cull_d250(spu_context* ctx)
{
    if (t_d250_guard == ctx) return 0;     /* the lifted function, run by the check */
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = e && (atoi(e) & 128) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(cull_stats);
        if (on && edge_verify_on()) atexit(cull_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_v250, d250_native, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D250, &t_d250_guard);
    if (d250_native(ctx)) {
        __atomic_add_fetch(&s_d250_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_d250_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

#else
int gowa_edge_cull_d250(spu_context* ctx) { (void)ctx; return 0; }
#endif
