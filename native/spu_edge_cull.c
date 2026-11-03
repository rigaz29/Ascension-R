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
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

#define EC_INL static inline __attribute__((always_inline))
#define EC_COLD static __attribute__((noinline, cold))
#define EC_MXCSR_RZFZ 0xE040u   /* round toward zero, FTZ, DAZ (SPU_MXCSR_RZFZ) */

static uint32_t ec_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
EC_INL __m128i ec_bswap4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
/* a quadword of the local store, its words in host order */
EC_INL __m128i ec_ldq(const uint8_t* ls, uint32_t a)
{
    return ec_bswap4(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_SIZE - 16)))));
}
EC_INL void ec_stq(uint8_t* ls, uint32_t a, __m128i v)
{
    _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_SIZE - 16))), ec_bswap4(v));
}
EC_INL u128 ec_u(__m128i v) { u128 r; _mm_storeu_si128((__m128i*)r._u32, v); return r; }
EC_INL __m128i ec_m(u128 v) { return _mm_loadu_si128((const __m128i*)v._u32); }
EC_INL u128 ec_splat(u128 v, int i) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = v._u32[i]; return r; }
EC_INL __m256 ec_set(uint32_t bits) { return _mm256_castsi256_ps(_mm256_set1_epi32((int)bits)); }
EC_INL __m256 ec_pair(__m128i lo, __m128i hi)
{
    return _mm256_castsi256_ps(_mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1));
}
EC_INL __m128i ec_lo(__m256 v) { return _mm256_castsi256_si128(_mm256_castps_si256(v)); }
EC_INL __m128i ec_hi(__m256 v) { return _mm256_extracti128_si256(_mm256_castps_si256(v), 1); }
EC_INL __m256 ec_join(u128 lo, u128 hi) { return ec_pair(ec_m(lo), ec_m(hi)); }

/* The SPU float forms on eight lanes (as spu_edge_light.c's): native where every lane is
 * below FLT_MAX in magnitude, with the +0 rule; else the runtime's exact forms on both halves. */
EC_INL int ec_ok(__m256 r)
{
    const __m256i m = _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF));
    return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x7F7FFFFF), m))) == 0xFF;
}
EC_INL __m256 ec_pz(__m256 r) { return _mm256_and_ps(r, _mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_NEQ_UQ)); }
static volatile long long s_ec_exact;
#define EC_EXACT() __atomic_add_fetch(&s_ec_exact, 1, __ATOMIC_RELAXED)
EC_COLD __m256 ec_fm_x(__m256 a, __m256 b)
{
    EC_EXACT();
    return ec_join(spu_fm(ec_u(ec_lo(a)), ec_u(ec_lo(b))), spu_fm(ec_u(ec_hi(a)), ec_u(ec_hi(b))));
}
EC_COLD __m256 ec_fma_x(__m256 a, __m256 b, __m256 c)
{
    EC_EXACT();
    return ec_join(spu_fma(ec_u(ec_lo(a)), ec_u(ec_lo(b)), ec_u(ec_lo(c))),
                   spu_fma(ec_u(ec_hi(a)), ec_u(ec_hi(b)), ec_u(ec_hi(c))));
}
EC_COLD __m256 ec_fnms_x(__m256 a, __m256 b, __m256 c)
{
    EC_EXACT();
    return ec_join(spu_fnms(ec_u(ec_lo(a)), ec_u(ec_lo(b)), ec_u(ec_lo(c))),
                   spu_fnms(ec_u(ec_hi(a)), ec_u(ec_hi(b)), ec_u(ec_hi(c))));
}
EC_COLD __m256 ec_fcgt_x(__m256 a, __m256 b)
{
    EC_EXACT();
    return ec_join(spu_fcgt(ec_u(ec_lo(a)), ec_u(ec_lo(b))), spu_fcgt(ec_u(ec_hi(a)), ec_u(ec_hi(b))));
}
EC_INL __m256 ec_fm(__m256 a, __m256 b)
{
    const __m256 r = _mm256_mul_ps(a, b);
    return __builtin_expect(ec_ok(r), 1) ? ec_pz(r) : ec_fm_x(a, b);
}
EC_INL __m256 ec_fma(__m256 a, __m256 b, __m256 c)   /* a*b + c */
{
    const __m256 r = _mm256_fmadd_ps(a, b, c);
    return __builtin_expect(ec_ok(r), 1) ? ec_pz(r) : ec_fma_x(a, b, c);
}
EC_INL __m256 ec_fnms(__m256 a, __m256 b, __m256 c)  /* c - a*b */
{
    const __m256 r = _mm256_fnmadd_ps(a, b, c);
    return __builtin_expect(ec_ok(r), 1) ? ec_pz(r) : ec_fnms_x(a, b, c);
}
EC_INL __m256 ec_fcgt(__m256 a, __m256 b)
{
    return __builtin_expect(ec_ok(a) && ec_ok(b), 1) ? _mm256_cmp_ps(a, b, _CMP_GT_OQ) : ec_fcgt_x(a, b);
}

/* frest and fi on eight lanes: the runtime's bit-field rules (spu_frest_ref, spu_fi), exact */
EC_INL __m256i ec_lut(const uint32_t* tbl, int n, __m256i idx)
{
    const __m256i row = _mm256_srli_epi32(idx, 3);
    __m256i r = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)tbl), idx);
    for (int k = 1; k < n; k++) {
        const __m256i v = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(tbl + 8 * k)), idx);
        r = _mm256_blendv_epi8(r, v, _mm256_cmpeq_epi32(row, _mm256_set1_epi32(k)));
    }
    return r;
}
EC_INL __m256i ec_frest(__m256i x)
{
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x1F));
    const __m256i frac = _mm256_and_si256(ec_lut(spu_frest_fraction_lut, 4, idx), _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(253), exp);
    rexp = _mm256_andnot_si256(_mm256_cmpgt_epi32(exp, _mm256_set1_epi32(252)), rexp);          /* exp >= 253: 0 */
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_and_si256(x, _mm256_set1_epi32((int)0x80000000u)),
                           _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac));
}
EC_INL __m256i ec_fi(__m256i a, __m256i est)
{
    const __m256i y = _mm256_and_si256(a, _mm256_set1_epi32(0x7FFFF));
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(est, 23), _mm256_set1_epi32(0xFF));
    const __m256i b19 = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(est, 10), _mm256_set1_epi32(0x1FFF)), 19);
    const __m256i sy = _mm256_mullo_epi32(_mm256_and_si256(est, _mm256_set1_epi32(0x3FF)), y);
    const __m256i big = _mm256_cmpeq_epi32(_mm256_max_epu32(b19, sy), b19);
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
