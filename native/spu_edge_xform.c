/*
 * EDGE's 4x4 transform of a vertex stream (job module e8f44fbc6dbf, 0xF538; the stage driver
 * 0x51D0 calls it three times a job): GOWA_EDGE_NATIVE bit 1024, on unless GOWA_EDGE_NATIVE
 * leaves it out.
 *
 * The arguments: r3 the count, r4 the input, r5 the output, r6 the matrix (four quadwords).
 * The function is a leaf with no frame.
 *
 * Its set-up turns the matrix into four constant vectors (c0..c3, through the shuffle tables at
 * 0xFE70, 0xFF80 and 0xFF60). Then, per vertex v, lane by lane:
 *   out = fma(rot12(v), c3, fma(rot8(v), c2, fma(rot4(v), c1, v * c0)))
 * where rotN is the quadword rotated left by N bytes.
 *
 * The loop is two passes deep, eight vertices a pass: pass p loads vertices 8p..8p+7, then
 * stores pass p - 1's results (pass 0 stores its initial registers: two zero quadwords and six
 * fma(0, c3, 0)), always at the output plus 128 * max(p - 1, 0). There are
 * ((count + 7) >> 3) + 1 passes. All eight loads come before the eight stores, so this routine
 * keeps that order too, which keeps any overlap of input and output as the lifted loop has it.
 *
 * Exact: the set-up runs through the runtime's own SPU forms, the loop through the fast pass of
 * spu_edge_simd.h (a pass redone with the exact forms when MXCSR's flags or an exponent 255
 * show). The last
 * pass runs with the runtime's forms and leaves every register as the lifted loop does: the
 * job's later code spills some of them to the local store.
 * Declined (lifted instead): another MXCSR; a count above 0x10000.
 * GOWA_EDGE_VERIFY=1 checks every call (spu_edge_verify.h).
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

static volatile long long s_xf_calls, s_xf_verts, s_xf_declined;

/* two vertices (one per half): the three-term sum and the result, fast or exact */
EC_INL void xf_vertex(const int f, __m256 v, __m256 c0, __m256 c1, __m256 c2, __m256 c3, __m256* part, __m256* res)
{
    const __m256 a = ecf_fm(f, v, c0);
    const __m256 b = ecf_fma(f, _mm256_permute_ps(v, _MM_SHUFFLE(0, 3, 2, 1)), c1, a);
    *part = ecf_fma(f, _mm256_permute_ps(v, _MM_SHUFFLE(1, 0, 3, 2)), c2, b);
    *res = ecf_fma(f, _mm256_permute_ps(v, _MM_SHUFFLE(2, 1, 0, 3)), c3, *part);
}

static int xf_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return 0;
    const uint32_t count = ctx->gpr[3]._u32[0];
    if (count == 0) return 1;                      /* the lifted function returns at once */
    if (count > 0x10000) return 0;
    uint8_t* const ls = ctx->ls;
    u128* const g = ctx->gpr;
    const u128 a3 = g[3], a4 = g[4], a5 = g[5];
    const uint32_t src = a4._u32[0], dst = a5._u32[0], mat = g[6]._u32[0];

    /* the set-up, statement for statement; of it, the four constants and the output
     * pointers stay to the end (every other register it writes the loop writes again) */
    const u128 m0 = ec_u(ec_ldq(ls, mat)), m1 = ec_u(ec_ldq(ls, mat + 0x10));
    const u128 m2 = ec_u(ec_ldq(ls, mat + 0x20)), m3 = ec_u(ec_ldq(ls, mat + 0x30));
    const u128 k18 = ec_u(ec_ldq(ls, 0xFE70)), k16 = ec_u(ec_ldq(ls, 0xFF80)), k23 = ec_u(ec_ldq(ls, 0xFF60));
    const u128 sel9 = spu_fsmbi(0xFF0), sel13 = spu_fsmbi(0xFF);
    const u128 r21 = spu_rotqbyi(m1, 4), r6 = spu_rotqbyi(m3, 12), r3 = spu_rotqbyi(m2, 8);
    const u128 r20 = spu_rotqbyi(k18, 8), r15 = spu_rotqbyi(k16, 8), r14 = spu_rotqbyi(k23, 8);
    const u128 r8 = spu_selb(r6, r21, sel9), r4 = spu_selb(r21, r6, sel9);
    const u128 r19 = spu_selb(r3, m0, sel9), r17 = spu_selb(m0, r3, sel9);
    const u128 r10 = spu_shufb(r19, r8, r20), r2 = spu_shufb(r19, r8, r15);
    const u128 r12 = spu_shufb(r17, r4, k18), r5 = spu_shufb(r17, r4, k16);
    const u128 c1u = spu_selb(r10, r12, sel13);   /* r36 */
    const u128 c3u = spu_selb(r5, r2, sel13);     /* r34 */
    const u128 c2u = spu_selb(r2, r5, sel13);     /* r33 */
    const u128 c0u = spu_shufb(r12, r10, r14);    /* r35 */
    const u128 r37 = spu_ai(spu_rotmi(spu_ai(a3, 7), -3), 1);
    const uint32_t passes = r37._u32[0];           /* ((count + 7) >> 3) + 1, two at least */
    const __m256 c0 = ec_join(c0u, c0u), c1 = ec_join(c1u, c1u), c2 = ec_join(c2u, c2u), c3 = ec_join(c3u, c3u);

    /* passes 0 .. passes - 2, eight lanes at a time; pass 0 stores the loop's initial
     * registers (r20 and r21 zero, the other six fma(0, c3, 0)) */
    const __m256 z = _mm256_setzero_ps(), f0 = ec_fma(z, c3, z);
    __m256 prev[4] = { z, f0, f0, f0 }, v[4], part[4];
    /* each pass fast (spu_edge_simd.h), redone exact from its inputs, still in registers, when
     * MXCSR's flags or an exponent 255 show; constants with exponent 255: every pass exact */
    const unsigned csr = _mm_getcsr();
    const int all_exact = ec_force_exact() ||
        !_mm256_testz_si256(_mm256_or_si256(_mm256_or_si256(ec_e255(c0), ec_e255(c1)), _mm256_or_si256(ec_e255(c2), ec_e255(c3))),
                            _mm256_set1_epi32(-1));
    /* the output apart from the input (both whole inside the local store): one flag read a
     * call, every pass redone exact when it shows; else a read each pass */
    const uint32_t s0a = src & (SPU_LS_SIZE - 16), d0a = dst & (SPU_LS_SIZE - 16);
    const uint32_t nin = 128u * passes, nout = 128u * (passes - 1);
    const int whole = !all_exact && s0a + nin <= SPU_LS_SIZE && d0a + nout <= SPU_LS_SIZE &&
                      (d0a + nout <= s0a || s0a + nin <= d0a);
    int redo_all = 0;
    __m256i wbad = _mm256_setzero_si256();
    ec_fl_clear(csr);
again:
    for (uint32_t p = 0; p + 1 < passes; p++) {
        const uint32_t in = src + 128u * p, out = dst + 128u * (p ? p - 1 : 0);
        for (int j = 0; j < 4; j++)
            v[j] = ec_pair(ec_ldq(ls, in + 32u * (uint32_t)j), ec_ldq(ls, in + 32u * (uint32_t)j + 16));
        for (int j = 0; j < 4; j++) {
            ec_stq(ls, out + 32u * (uint32_t)j, ec_lo(prev[j]));
            ec_stq(ls, out + 32u * (uint32_t)j + 16, ec_hi(prev[j]));
        }
        int fast = !all_exact && !redo_all;
        if (fast && whole) {
            for (int j = 0; j < 4; j++) {
                wbad = _mm256_or_si256(wbad, ec_e255(v[j]));
                xf_vertex(1, v[j], c0, c1, c2, c3, &part[j], &prev[j]);
                prev[j] = ec_pz(prev[j]);
            }
        } else if (fast) {
            __m256i bad = _mm256_setzero_si256();
            for (int j = 0; j < 4; j++) {
                bad = _mm256_or_si256(bad, ec_e255(v[j]));
                xf_vertex(1, v[j], c0, c1, c2, c3, &part[j], &prev[j]);
            }
            for (int j = 0; j < 4; j++) EC_FL_USE(prev[j]);
            if ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(bad, bad)) {
                fast = 0;
                __atomic_add_fetch(&s_ec_redone, 1, __ATOMIC_RELAXED);
            } else
                for (int j = 0; j < 4; j++) prev[j] = ec_pz(prev[j]);
        }
        if (!fast) {
            for (int j = 0; j < 4; j++) xf_vertex(0, v[j], c0, c1, c2, c3, &part[j], &prev[j]);
            for (int j = 0; j < 4; j++) EC_FL_USE(prev[j]);
            ec_fl_clear(csr);
        }
    }
    if (whole && !redo_all) {
        /* the last pass's results are not stored yet: they go through the barrier */
        for (int j = 0; j < 4; j++) { EC_FL_USE(prev[j]); EC_FL_USE(part[j]); }
        if ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(wbad, wbad)) {
            redo_all = 1;
            prev[0] = z; prev[1] = prev[2] = prev[3] = f0;
            __atomic_add_fetch(&s_ec_redone, 1, __ATOMIC_RELAXED);
            goto again;
        }
    }
    ec_fl_set(csr);

    /* the last pass with the runtime's SPU forms, which leaves every register as the lifted
     * loop does (callers spill some of them): its loads, pass passes - 2's stores, then its
     * own work, which nothing stores */
    u128 pv[8], pp[8];                             /* pass passes - 2: the inputs, the three-term sums */
    for (int j = 0; j < 4; j++) {
        pv[2 * j] = ec_u(ec_lo(v[j]));    pv[2 * j + 1] = ec_u(ec_hi(v[j]));
        const __m256 pz = ec_pz(part[j]);           /* the fast pass leaves a zero's sign */
        pp[2 * j] = ec_u(ec_lo(pz)); pp[2 * j + 1] = ec_u(ec_hi(pz));
    }
    u128 fo[8];                                    /* its results: r20, r21, r7, r2, r5, r76, r73, r72 */
    fo[0] = ec_u(ec_lo(prev[0]));
    fo[1] = ec_u(ec_hi(prev[0]));
    for (int k = 2; k < 8; k++) fo[k] = spu_fma(spu_rotqbyi(pv[k], 12), c3u, pp[k]);
    const uint32_t in = src + 128u * (passes - 1), out = dst + 128u * (passes - 2);
    u128 w[8], m[8], r4k[8], p2[8], r8k[8], p3[8], r12k[8];
    for (int k = 0; k < 8; k++) w[k] = ec_u(ec_ldq(ls, in + 16u * (uint32_t)k));
    for (int k = 0; k < 8; k++) ec_stq(ls, out + 16u * (uint32_t)k, ec_m(fo[k]));
    for (int k = 0; k < 8; k++) {
        m[k] = spu_fm(w[k], c0u);
        r4k[k] = spu_rotqbyi(w[k], 4);
        p2[k] = spu_fma(r4k[k], c1u, m[k]);
        r8k[k] = spu_rotqbyi(w[k], 8);
        p3[k] = spu_fma(r8k[k], c2u, p2[k]);
        r12k[k] = spu_rotqbyi(w[k], 12);
    }
    /* each register's last value in the lifted loop body */
    g[7] = fo[2]; g[2] = fo[3]; g[5] = fo[4]; g[76] = fo[5]; g[73] = fo[6]; g[72] = fo[7];
    g[4] = w[0]; g[6] = w[2]; g[13] = w[3]; g[14] = w[4]; g[15] = w[5]; g[16] = w[6]; g[19] = w[7];
    g[17] = p2[0]; g[18] = p2[1]; g[31] = p3[0]; g[30] = p3[1];
    g[20] = spu_fma(r12k[0], c3u, p3[0]);
    g[21] = spu_fma(r12k[1], c3u, p3[1]);
    g[60] = r12k[0]; g[59] = r12k[1];
    g[10] = m[2]; g[11] = m[3]; g[12] = m[4]; g[8] = m[6]; g[27] = m[7];
    g[22] = r4k[2]; g[23] = r4k[3]; g[24] = r4k[4]; g[25] = r4k[5]; g[26] = r4k[6]; g[3] = r4k[7];
    g[75] = p2[2]; g[70] = p2[3]; g[68] = p2[4]; g[66] = p2[5]; g[64] = p2[6]; g[62] = p2[7];
    g[74] = r8k[2]; g[69] = r8k[3]; g[67] = r8k[4]; g[65] = r8k[5]; g[63] = r8k[6]; g[61] = r8k[7];
    g[9] = p3[2]; g[43] = p3[3]; g[41] = p3[4]; g[50] = p3[5]; g[49] = p3[6]; g[48] = p3[7];
    g[53] = r12k[2]; g[52] = r12k[3]; g[51] = r12k[4]; g[57] = g[79] = r12k[5]; g[56] = g[78] = r12k[6];
    g[55] = g[77] = r12k[7];
    for (int i = 0; i < 4; i++) {
        g[37]._u32[i] = r37._u32[i] - passes;
        g[38]._u32[i] = g[29]._u32[i] = a4._u32[i] + 128u * passes;
        g[39]._u32[i] = g[28]._u32[i] = 128u * (passes - 1);
        g[32]._u32[i] = 128;
    }
    /* the set-up's that stay */
    g[35] = c0u; g[36] = c1u; g[33] = c2u; g[34] = c3u;
    g[58] = a5; g[54] = spu_ai(a5, 16); g[47] = spu_ai(a5, 32); g[46] = spu_ai(a5, 48);
    g[45] = spu_ai(a5, 64); g[44] = spu_ai(a5, 80); g[42] = spu_ai(a5, 96); g[40] = spu_ai(a5, 112);
    __atomic_add_fetch(&s_xf_verts, (long long)count, __ATOMIC_RELAXED);
    return 1;
}

static void xf_stats(void)
{
    fprintf(stderr, "[edge-native] transform F538: %lld calls native, %lld vertices, %lld declined, %lld exact ops, %lld passes redone\n",
            s_xf_calls, s_xf_verts, s_xf_declined, s_ec_exact, s_ec_redone);
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000F538(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_xf_guard;
static edge_vstat s_vxf = { "transform F538", 0, 0, 0, 0, 0, 0, 0, 1 };
static void xf_verify_report(void) { edge_vreport(&s_vxf); }

/* The hook at 0xF538's entry: 1 when the call is done (the function returns). */
int gowa_edge_xform_f538(spu_context* ctx)
{
    if (t_xf_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 1024) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(xf_stats);
        if (on && edge_verify_on()) atexit(xf_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vxf, xf_native, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000F538, &t_xf_guard);
    if (xf_native(ctx)) {
        __atomic_add_fetch(&s_xf_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_xf_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

#else
int gowa_edge_xform_f538(spu_context* ctx) { (void)ctx; return 0; }
#endif
