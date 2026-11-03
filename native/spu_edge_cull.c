/*
 * EDGE's triangle culling as native code (job module e8f44fbc6dbf; docs/notes/2026-11-03-edge-stages.md).
 *
 * 0xD250, the vertex pass (GOWA_EDGE_NATIVE bit 128, on unless GOWA_EDGE_NATIVE leaves it out):
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

/* the vertex pass's constants */
typedef struct {
    __m256 c[4][4];                 /* C's rows, x y z w */
    __m256 sxs, sys, szs, oxs, oys, ozs, xmin, ymin, xmax, ymax, zmin, zmax, one, sign;
} d250_k;

/* eight vertices: the screen x and y (+0 applied) and the flags, fast or exact */
EC_INL void d250_pass(const int f, const d250_k* k, __m256 X, __m256 Y, __m256 Z, __m256* osx, __m256* osy, __m256i* ofl)
{
    const __m256 xp = ecf_fma(f, Z, k->c[0][2], ecf_fma(f, Y, k->c[0][1], ecf_fma(f, X, k->c[0][0], k->c[0][3])));
    const __m256 yp = ecf_fma(f, Z, k->c[1][2], ecf_fma(f, Y, k->c[1][1], ecf_fma(f, X, k->c[1][0], k->c[1][3])));
    const __m256 zp = ecf_fma(f, Z, k->c[2][2], ecf_fma(f, Y, k->c[2][1], ecf_fma(f, X, k->c[2][0], k->c[2][3])));
    const __m256 W = ecf_pz(f, ecf_fma(f, Z, k->c[3][2], ecf_fma(f, Y, k->c[3][1], ecf_fma(f, X, k->c[3][0], k->c[3][3]))));
    const __m256i wi = _mm256_castps_si256(W);                /* frest, fi and the sign read its bits */
    const __m256 r = _mm256_castsi256_ps(ec_fi(wi, ec_frest(wi)));
    const __m256 rcp = ecf_fma(f, ecf_fnms(f, r, W, k->one), r, r);
    const __m256 ws = _mm256_and_ps(W, k->sign);
    const __m256 px = _mm256_xor_ps(ecf_fm(f, xp, rcp), ws);
    const __m256 py = _mm256_xor_ps(ecf_fm(f, yp, rcp), ws);
    const __m256 pz = _mm256_xor_ps(ecf_fm(f, zp, rcp), ws);
    const __m256 sx = ecf_fma(f, px, k->sxs, k->oxs), sy = ecf_fma(f, py, k->sys, k->oys), sz = ecf_fma(f, pz, k->szs, k->ozs);
    __m256i fl = _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, sx, k->xmax)), _mm256_set1_epi32(0x80));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, k->xmin, sx)), _mm256_set1_epi32(0x40)));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, sy, k->ymax)), _mm256_set1_epi32(0x20)));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, k->ymin, sy)), _mm256_set1_epi32(0x10)));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, sz, k->zmax)), _mm256_set1_epi32(0x08)));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, k->zmin, sz)), _mm256_set1_epi32(0x04)));
    fl = _mm256_or_si256(fl, _mm256_and_si256(_mm256_castps_si256(ecf_fcgt(f, W, _mm256_setzero_ps())), _mm256_set1_epi32(0x02)));
    *osx = ecf_pz(f, sx);
    *osy = ecf_pz(f, sy);
    *ofl = fl;
}

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

    /* the lifted prologue's saves (r80, r126, r1 below the caller's r1): the local store ends
     * the same, the frame included */
    const uint32_t sp = ctx->gpr[1]._u32[0];
    ec_stq(ls, sp - 0x10, ec_m(ctx->gpr[80]));
    ec_stq(ls, sp - 0x20, ec_m(ctx->gpr[126]));
    ec_stq(ls, sp - 0x40, ec_m(ctx->gpr[1]));

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

    d250_k kk;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) kk.c[i][j] = ec_set(C[i]._u32[j]);
    kk.sxs = ec_set(S._u32[0]); kk.sys = ec_set(S._u32[1]); kk.szs = ec_set(S._u32[2]);
    kk.oxs = ec_set(O._u32[0]); kk.oys = ec_set(O._u32[1]); kk.ozs = ec_set(O._u32[2]);
    kk.xmin = _mm256_set1_ps((float)h0); kk.ymin = _mm256_set1_ps((float)h1);
    kk.xmax = _mm256_set1_ps((float)((h0 + h2) & 0xFFFF)); kk.ymax = _mm256_set1_ps((float)((h1 + h3) & 0xFFFF));
    kk.zmin = ec_set(zmin_b); kk.zmax = ec_set(zmax_b);
    kk.one = ec_set(0x3F800000u); kk.sign = ec_set(0x80000000u);
    const __m256 zero = _mm256_setzero_ps();

    /* each pass fast (spu_edge_simd.h), redone exact from its inputs, still in registers, when
     * MXCSR's flags or an exponent 255 show; constants with exponent 255: every pass exact */
    __m256i kbad = _mm256_setzero_si256();
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) kbad = _mm256_or_si256(kbad, ec_e255(kk.c[i][j]));
    const __m256 kv[] = { kk.sxs, kk.sys, kk.szs, kk.oxs, kk.oys, kk.ozs, kk.zmin, kk.zmax };
    for (unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++) kbad = _mm256_or_si256(kbad, ec_e255(kv[i]));
    const int all_exact = ec_force_exact() || !_mm256_testz_si256(kbad, kbad);
    /* the records apart from the positions (the set-up refuses a partial overlap): one flag
     * read a call, the whole pass redone exact when it shows; in place, a read each pass */
    const int whole = !all_exact && out_a != in_a;
    int redo_all = 0;
    __m256i wbad = _mm256_setzero_si256();
    const unsigned csr = _mm_getcsr();
    ec_fl_clear(csr);
again:
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

        __m256 sx, sy;
        __m256i f;
        int fast = !all_exact && !redo_all;
        if (fast && whole) {
            d250_pass(1, &kk, X, Y, Z, &sx, &sy, &f);
            wbad = _mm256_or_si256(wbad, _mm256_or_si256(_mm256_or_si256(ec_e255(X), ec_e255(Y)), ec_e255(Z)));
        } else if (fast) {
            d250_pass(1, &kk, X, Y, Z, &sx, &sy, &f);
            const __m256i bad = _mm256_or_si256(_mm256_or_si256(ec_e255(X), ec_e255(Y)), ec_e255(Z));
            EC_FL_USE(sx); EC_FL_USE(sy); EC_FL_USE(f);
            if ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(bad, bad)) {
                fast = 0;
                __atomic_add_fetch(&s_ec_redone, 1, __ATOMIC_RELAXED);
            }
        }
        if (!fast) {
            d250_pass(0, &kk, X, Y, Z, &sx, &sy, &f);
            EC_FL_USE(sx); EC_FL_USE(sy); EC_FL_USE(f);
            ec_fl_clear(csr);
        }

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
    /* every result went to a store, which the read's memory clobber keeps above it */
    if (whole && !redo_all && ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(wbad, wbad))) {
        redo_all = 1;
        __atomic_add_fetch(&s_ec_redone, 1, __ATOMIC_RELAXED);
        goto again;
    }
    ec_fl_set(csr);
    __atomic_add_fetch(&s_d250_verts, (long long)n, __ATOMIC_RELAXED);
    return 1;
}

static void cull_stats(void)
{
    fprintf(stderr, "[edge-native] cull D250: %lld calls native, %lld vertices, %lld declined, %lld exact ops, %lld passes redone (both routines)\n",
            s_d250_calls, s_d250_verts, s_d250_declined, s_ec_exact, s_ec_redone);
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D250(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_d250_guard;
static edge_vstat s_v250 = { "cull D250", 0, 0, 0, 0, 0, 0, 0 };
static void cull_verify_report(void) { edge_vreport(&s_v250); }

/* The hook at 0xD250's entry: 1 when the call is done (the function returns). */
int gowa_edge_cull_d250(spu_context* ctx)
{
    if (t_d250_guard == ctx) return 0;     /* the lifted function, run by the check */
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 128) != 0;
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

/* ------------------------------------------------------------------ 0xD618 */
/*
 * 0xD618 / 0xD640, the triangle culling (GOWA_EDGE_NATIVE bit 512, on unless the variable leaves it out;
 * docs/notes/2026-11-03-edge-stages.md):
 *  - its frame, then the vertex pass (0xD250) as the lifted code calls it;
 *  - each group of four triangles' cull mask, here eight triangles a pass in the SPU's exact forms:
 *    - degenerate after snapping (cflts(x * s + 0.5)), all x or all y equal, kept by Mesh Trimming;
 *    - outside one plane (all three flags), or facing when the mode culls it;
 *    - the last group's lanes past the count;
 *  - the in-place writer, statement for statement as the lifted loop runs it: its three
 *    16-entry shuffle tables, the stores past the end into the frame, and the two passes that
 *    fill the pipeline. So the index buffer, the frame and the scratch end the same, byte for byte;
 *  - the count (3 * kept) into ctx + 0x190, the buffer's end into ctx + 0x58 (0xB600), r3.
 * Declined:
 *  - the paths that do not cull: no mode, the segment's culling off, everything culled;
 *  - a custom vertex pass;
 *  - another mode;
 *  - a misaligned context or record base;
 *  - a kept triangle's record inside the index buffer.
 */
extern int g_gowa_patch_disable_mesh_trimming;
static volatile long long s_d618_calls, s_d618_tris, s_d618_declined;
static volatile long long s_d618_why[32];

EC_INL uint32_t ec_ls8(const uint8_t* ls, uint32_t a) { return ls[a & (SPU_LS_SIZE - 1)]; }
EC_INL uint32_t ec_ls16(const uint8_t* ls, uint32_t a) { return (ec_ls8(ls, a) << 8) | ec_ls8(ls, a + 1); }
EC_INL u128 ec_splat32(uint32_t v) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = v; return r; }

/* the cull masks of groups g and g + 1 (lanes 0-3 and 4-7) */
/* f: fast (bad collects the records' exponent-255 lanes; the caller reads MXCSR's flags) or exact */
EC_INL void d618_masks2(const int f, const uint8_t* ls, uint32_t ibase, uint32_t r29, uint32_t g,
                        const __m256 s39, const __m256i r63, const __m256i r55, uint32_t* m0, uint32_t* m1,
                        __m256i* bad)
{
    /* the two groups' 24 indexes (48 bytes; the caller keeps the buffer inside the local store) */
    const __m128i sw16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    const uint8_t* ip = ls + ((ibase + 24 * g) & (SPU_LS_SIZE - 1));
    __attribute__((aligned(16))) uint16_t id[24];
    _mm_store_si128((__m128i*)id, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)ip), sw16));
    _mm_store_si128((__m128i*)(id + 8), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ip + 16)), sw16));
    _mm_store_si128((__m128i*)(id + 16), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ip + 32)), sw16));
    /* the records: x, y, flags of each vertex (a, b, c) of the eight triangles */
    const __m256i base = _mm256_set1_epi32((int)r29), amask = _mm256_set1_epi32((int)(SPU_LS_SIZE - 16));
    const __m256i sw32 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                          3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    __m256i gx[3], gy[3], gf[3];
    for (int v = 0; v < 3; v++) {
        const __m256i idx = _mm256_setr_epi32(id[v], id[3 + v], id[6 + v], id[9 + v],
                                              id[12 + v], id[15 + v], id[18 + v], id[21 + v]);
        const __m256i off = _mm256_and_si256(_mm256_add_epi32(base, _mm256_slli_epi32(idx, 4)), amask);
        gx[v] = _mm256_shuffle_epi8(_mm256_i32gather_epi32((const int*)ls, off, 1), sw32);
        gy[v] = _mm256_shuffle_epi8(_mm256_i32gather_epi32((const int*)(ls + 4), off, 1), sw32);
        gf[v] = _mm256_shuffle_epi8(_mm256_i32gather_epi32((const int*)(ls + 8), off, 1), sw32);
    }
    const __m256 Xa = _mm256_castsi256_ps(gx[0]), Xb = _mm256_castsi256_ps(gx[1]), Xc = _mm256_castsi256_ps(gx[2]);
    const __m256 Ya = _mm256_castsi256_ps(gy[0]), Yb = _mm256_castsi256_ps(gy[1]), Yc = _mm256_castsi256_ps(gy[2]);
    const __m256i Fa = gf[0], Fb = gf[1], Fc = gf[2];
    if (f)
        *bad = _mm256_or_si256(*bad, _mm256_or_si256(
            _mm256_or_si256(ec_e255(Xa), _mm256_or_si256(ec_e255(Xb), ec_e255(Xc))),
            _mm256_or_si256(ec_e255(Ya), _mm256_or_si256(ec_e255(Yb), ec_e255(Yc)))));
    /* the facing: fms(Xb - Xa, Yc - Ya, (Yb - Ya) * (Xc - Xa)), the mode's sign, > 0 (a zero's
     * sign does not change the float compare) */
    const __m256 cross = ecf_fms(f, ecf_fs(f, Xb, Xa), ecf_fs(f, Yc, Ya), ecf_fm(f, ecf_fs(f, Yb, Ya), ecf_fs(f, Xc, Xa)));
    const __m256i facing = _mm256_castps_si256(ecf_fcgt(f, _mm256_xor_ps(cross, _mm256_castsi256_ps(r63)),
                                                        _mm256_setzero_ps()));
    /* degenerate after snapping (cvttps of a zero is 0 whatever its sign) */
    const __m256 half = ec_set(0x3F000000u);
    const __m256i qxa = ecf_cflts0(f, ecf_fma(f, Xa, s39, half)), qxb = ecf_cflts0(f, ecf_fma(f, Xb, s39, half));
    const __m256i qxc = ecf_cflts0(f, ecf_fma(f, Xc, s39, half)), qya = ecf_cflts0(f, ecf_fma(f, Ya, s39, half));
    const __m256i qyb = ecf_cflts0(f, ecf_fma(f, Yb, s39, half)), qyc = ecf_cflts0(f, ecf_fma(f, Yc, s39, half));
    const __m256i allx = _mm256_and_si256(_mm256_and_si256(_mm256_cmpeq_epi32(qxc, qxa), _mm256_cmpeq_epi32(qxb, qxc)),
                                          _mm256_cmpeq_epi32(qxa, qxb));
    const __m256i ally = _mm256_and_si256(_mm256_and_si256(_mm256_cmpeq_epi32(qyc, qya), _mm256_cmpeq_epi32(qyb, qyc)),
                                          _mm256_cmpeq_epi32(qya, qyb));
    const __m256i degen = _mm256_or_si256(allx, ally);
    /* the flags */
    const __m256i fand = _mm256_and_si256(_mm256_and_si256(Fa, Fb), Fc), forr = _mm256_or_si256(_mm256_or_si256(Fa, Fb), Fc);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i nonear = _mm256_cmpeq_epi32(_mm256_and_si256(forr, _mm256_set1_epi32(4)), zero);
    const __m256i allw = _mm256_cmpgt_epi32(_mm256_and_si256(fand, _mm256_set1_epi32(2)), zero);
    const __m256i outside = _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_and_si256(fand, _mm256_set1_epi32(252)), zero),
                                             _mm256_set1_epi32(-1));
    const __m256i r25 = _mm256_and_si256(nonear, allw);
    const __m256i trim = g_gowa_patch_disable_mesh_trimming ? zero : _mm256_and_si256(degen, r25);
    const __m256i r19 = _mm256_or_si256(outside, _mm256_and_si256(_mm256_and_si256(r25, facing), r55));
    const int bits = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_or_si256(trim, r19)));
    /* gb: triangle 0 is bit 3 */
    *m0 = (uint32_t)(((bits & 1) << 3) | ((bits & 2) << 1) | ((bits & 4) >> 1) | ((bits & 8) >> 3));
    *m1 = (uint32_t)(((bits & 16) >> 1) | ((bits & 32) >> 3) | ((bits & 64) >> 5) | ((bits & 128) >> 7));
}

static int d618_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) { s_d618_why[1]++; return 0; }
    uint8_t* const ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0];
    if ((c & 15) || c > SPU_LS_SIZE - 0x1A0) { s_d618_why[2]++; return 0; }
    const uint32_t mode = ctx->gpr[4]._u32[0];
    if (mode != 1 && mode != 2 && mode != 3) { s_d618_why[3]++; return 0; }              /* 0: no culling; others: lifted */
    if (!(ec_ls8(ls, c + 14) & 0x80)) { s_d618_why[4]++; return 0; }                     /* the segment's culling is off */
    if (ec_be32(ls + c + 0x184)) { s_d618_why[5]++; return 0; }                           /* a custom vertex pass */
    if (ec_ls8(ls, c + 0x140) == 5) { s_d618_why[6]++; return 0; }                        /* everything culled */
    const u128 e0 = ctx->gpr[0], e1 = ctx->gpr[1], e80 = ctx->gpr[80], e81 = ctx->gpr[81];
    const u128 e82 = ctx->gpr[82], e126 = ctx->gpr[126];
    if ((int32_t)(e1._u32[1] - 144) < 0) { s_d618_why[7]++; return 0; }                   /* the stack check halts */
    const uint32_t sp = e1._u32[0], sp2 = sp - 144;
    const uint32_t k = ec_ls8(ls, c) & 15;
    const uint32_t r29 = ec_be32(ls + c + 0x10 + 4 * k) + 16 * ctx->gpr[5]._u32[0];
    if (r29 & 15) { s_d618_why[8]++; return 0; }
    const uint32_t numtri = ec_ls16(ls, c + 0x190) / 3, groups = (numtri + 3) >> 2;
    const uint32_t lastmask = (numtri & 3) ? (15u >> (numtri & 3)) : 0;
    const uint32_t ibase = ec_be32(ls + c + 0x50);
    const uint32_t b14e = ec_ls8(ls, c + 0x14E), nib = ec_ls8(ls, c + 6) >> 4;
    const __m256 s39 = ec_set((b14e == 3 || b14e == 4) ? 0x40000000u : 0x3F800000u);
    const __m256i r63 = _mm256_set1_epi32((int)((mode == 2 ? 0u : 0x80000000u) ^ ((nib == 0 || nib == 2) ? 0x80000000u : 0u)));
    const __m256i r55 = _mm256_set1_epi32(mode == 1 ? 0 : -1);
    const uint32_t ilo = ibase & (SPU_LS_SIZE - 16), ihi = ilo + 24 * groups + 64;
    /* checked before anything is written: the vertex pass's records apart from the index buffer,
     * and no kept triangle's record read from it (the lifted loop would read it after the writer) */
    const uint32_t recbase = ec_be32(ls + c + 0x10 + 4 * k) & (SPU_LS_SIZE - 16);
    const uint32_t nvert = ec_ls16(ls, c + 8), rechi = recbase + 64 * ((nvert + 3) >> 2) + 128;
    if (recbase < ihi && ilo < rechi) { s_d618_why[9]++; return 0; }
    if ((ibase & (SPU_LS_SIZE - 1)) + 24 * groups + 64 > SPU_LS_SIZE) { s_d618_why[11]++; return 0; }
    {
        /* every kept triangle's three records, sixteen indexes at a time */
        const uint8_t* ip = ls + (ibase & (SPU_LS_SIZE - 1));
        const uint32_t n = 3 * numtri;
        const __m256i sw16 = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
                                              1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        const __m256i base = _mm256_set1_epi32((int)r29), amask = _mm256_set1_epi32((int)(SPU_LS_SIZE - 16));
        const __m256i lo = _mm256_set1_epi32((int)(ilo ^ 0x80000000u)), span = _mm256_set1_epi32((int)((ihi - ilo) ^ 0x80000000u));
        uint32_t i = 0;
        for (; i + 16 <= n; i += 16) {
            const __m256i w = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(ip + 2 * i)), sw16);
            for (int h = 0; h < 2; h++) {
                const __m256i idx = _mm256_cvtepu16_epi32(h ? _mm256_extracti128_si256(w, 1) : _mm256_castsi256_si128(w));
                const __m256i a = _mm256_and_si256(_mm256_add_epi32(base, _mm256_slli_epi32(idx, 4)), amask);
                /* a - ilo < ihi - ilo, unsigned */
                const __m256i d = _mm256_xor_si256(_mm256_sub_epi32(a, _mm256_xor_si256(lo, _mm256_set1_epi32((int)0x80000000u))),
                                                   _mm256_set1_epi32((int)0x80000000u));
                if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(span, d)))) { s_d618_why[10]++; return 0; }
            }
        }
        for (; i < n; i++) {
            const uint32_t a = (r29 + 16 * ec_ls16(ls, ibase + 2 * i)) & (SPU_LS_SIZE - 16);
            if (a >= ilo && a < ihi) { s_d618_why[10]++; return 0; }
        }
    }

    /* the frame: 0xD618's saves, its registers, then 0xD640's */
    spu_ls_write128(ctx, sp - 0x10, e80);
    spu_ls_write128(ctx, sp - 0x20, e81);
    spu_ls_write128(ctx, sp - 0x30, e82);
    spu_ls_write128(ctx, sp - 0x40, e126);
    ctx->gpr[80] = ctx->gpr[4];
    ctx->gpr[81] = ctx->gpr[3];
    ctx->gpr[82] = ctx->gpr[5];
    spu_ls_write128(ctx, sp + 0x10, e0);
    spu_ls_write128(ctx, sp - 0x90, e1);
    ctx->gpr[1] = spu_ai(e1, -144);
    ctx->gpr[126] = spu_sf(ec_splat32(0x9640), spu_link(0xD640));
    /* the vertex pass, as 0xD640 calls it */
    ctx->gpr[0] = spu_link(0xD6C0);
    if (!d250_native(ctx)) {
        const int32_t si = (int32_t)ctx->image_id;
        t_d250_guard = ctx;
        ctx->host_depth++;
        jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D250(ctx);
        spu_drain_call(ctx, 0xD6C0);
        ctx->host_depth--;
        spu_img_restore(ctx, si);
        t_d250_guard = NULL;
    }

    /* the cull masks, every group's (the writer changes nothing they read first) */
    uint32_t mstack[1024], *masks = mstack;
    if (groups + 2 > 1024) masks = (uint32_t*)malloc(sizeof(uint32_t) * (groups + 2));
    if (!masks) abort();
    {
        /* every group fast (spu_edge_simd.h), all redone exact when MXCSR's flags or an exponent
         * 255 show (the masks go to this array only) */
        int fast = !ec_force_exact();
        if (fast) {
            const unsigned csr = _mm_getcsr();
            __m256i bad = _mm256_setzero_si256();
            uint32_t all = 0;                               /* every mask, for the flags' order */
            ec_fl_clear(csr);
            for (uint32_t g = 0; g < groups; g += 2) {
                d618_masks2(1, ls, ibase, r29, g, s39, r63, r55, &masks[g], &masks[g + 1], &bad);
                all |= masks[g] | masks[g + 1];
            }
            EC_FL_USEI(all);
            if ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(bad, bad)) {
                fast = 0;
                __atomic_add_fetch(&s_ec_redone, 1, __ATOMIC_RELAXED);
            }
            ec_fl_set(csr);
        }
        if (!fast)
            for (uint32_t g = 0; g < groups; g += 2)
                d618_masks2(0, ls, ibase, r29, g, s39, r63, r55, &masks[g], &masks[g + 1], NULL);
    }
    if (groups) masks[groups - 1] |= lastmask;

    /* the writer, statement for statement (0xD92C), with the pipeline's two passes ahead */
    const u128 zero = ec_splat32(0), ctl_w0 = ec_splat32(0x00010203u);
    const u128 r69 = spu_ls_read128(ctx, 0xFEB0), r68 = spu_ls_read128(ctx, 0xF9F0);
    const u128 r61 = ec_splat32(0x10190), r62 = ec_splat32(0xFF90), r60 = ec_splat32(0x10090);
    const u128 r11v = spu_ls_read128(ctx, c + 0x50);
    const u128 r58 = r11v, r59 = spu_ai(r11v, 16);
    const u128 r71 = spu_fsmbi(0xFFFF), r67 = ec_splat32(sp2 + 32);
    const uint32_t endb = ibase + ((6 * numtri + 15) & ~15u);
    u128 r65;
    r65._u32[0] = endb - 17; r65._u32[1] = endb - 33; r65._u32[2] = endb; r65._u32[3] = endb;
    u128 r31 = spu_andbi(spu_fsmbi(0x1100), 15), r5 = zero, r8 = zero, r33 = ec_splat32(ibase), r9 = zero;
    u128 r3 = zero, r48 = r69, r54 = spu_ls_read128(ctx, ibase), r32 = spu_ls_read128(ctx, ibase + 16);
    u128 r47 = zero, r10 = zero, r12 = zero, r64 = zero, r30 = zero, r41 = zero, r46 = zero;
    u128 r34 = ec_splat32(groups + 2);
    for (uint32_t p = 1;; p++) {
        u128 r51 = spu_or(r31, r5);
        r8 = spu_ls_read128(ctx, r61._u32[0] + r8._u32[0]);
        const u128 r75 = spu_shli(r51, 4);
        r51 = spu_shufb(r32, r32, r48);
        const u128 r82 = spu_a(r33, r9);
        const u128 r73 = spu_rotqmby(r71, r3);
        const u128 r52 = spu_shufb(r54, r32, r48);
        const u128 r77 = spu_ls_read128(ctx, r82._u32[0]);
        const u128 r28 = spu_cgt(r82, r65);
        const u128 r14 = spu_ls_read128(ctx, r62._u32[0] + r75._u32[0]);
        r5 = spu_selb(r69, r8, r73);
        const u128 r20 = spu_ls_read128(ctx, r60._u32[0] + r75._u32[0]);
        const u128 r33t = spu_shlqbyi(r31, 4);
        const u128 r76 = spu_selb(r82, r67, r28);
        r32 = spu_ai(r47, 24);
        const u128 r4 = r76;
        r47 = r32;
        const u128 r78 = spu_shufb(r10, r12, r14);
        const u128 r19 = spu_shufb(r12, r12, r20);
        r31 = r33t;
        r33 = r82;
        const u128 r18 = spu_shufb(r77, r78, r5);
        const u128 r25 = spu_shufb(r78, r19, r8);
        const u128 r12s = spu_shufb(r19, r19, r8);
        const u128 r23 = spu_rotqbyi(r76, 4);
        r32 = spu_ls_read128(ctx, r59._u32[0] + r32._u32[0]);
        r54 = spu_ls_read128(ctx, r58._u32[0] + r47._u32[0]);
        spu_ls_write128(ctx, r33._u32[0], r18);
        spu_ls_write128(ctx, r4._u32[0] + 0x10, r25);
        spu_ls_write128(ctx, r23._u32[0] + 0x20, r12s);
        u128 r11 = spu_cntb(r75);
        r48 = spu_xor(r48, r68);
        r11 = spu_shufb(r11, ctl_w0, ctl_w0);
        const u128 r73k = spu_sfi(r11, 4);
        r9 = spu_mpyui(r73k, 6);
        const u128 r2 = spu_ai(r34, -1);
        r34 = r2;
        const u128 r78p = spu_a(r41, r9);
        r41 = spu_andi(r78p, 15);
        r46 = spu_a(r46, r73k);
        r8 = spu_shli(r41, 4);
        /* the mask this pass computes: group p - 2's (the last group's with its lanes past the count) */
        r5 = zero;
        if (p >= 2 && p - 2 < groups) {
            r5._u32[0] = masks[p - 2];
            if (p - 2 == groups - 1) r5._u32[1] = r5._u32[2] = r5._u32[3] = lastmask;
        }
        r12 = r30;
        r10 = r64;
        r3 = spu_sfi(r41, 0);
        if (r2._u32[0] == 0) break;
        r64 = r52;
        r30 = r51;
    }
    if (masks != mstack) free(masks);

    /* the count, the buffer's end (0xB600), the result */
    const uint32_t count = 3 * r46._u32[0];
    ls[(c + 0x190) & (SPU_LS_SIZE - 1)] = (uint8_t)(count >> 8);
    ls[(c + 0x191) & (SPU_LS_SIZE - 1)] = (uint8_t)count;
    const uint32_t bend = (ibase + 2 * count + 15) & ~15u;
    ls[(c + 0x58) & (SPU_LS_SIZE - 1)] = (uint8_t)(bend >> 24);
    ls[(c + 0x59) & (SPU_LS_SIZE - 1)] = (uint8_t)(bend >> 16);
    ls[(c + 0x5A) & (SPU_LS_SIZE - 1)] = (uint8_t)(bend >> 8);
    ls[(c + 0x5B) & (SPU_LS_SIZE - 1)] = (uint8_t)bend;
    ctx->gpr[3] = spu_a(spu_a(r46, r46), r46);
    ctx->gpr[0] = e0; ctx->gpr[1] = e1;
    ctx->gpr[80] = e80; ctx->gpr[81] = e81; ctx->gpr[82] = e82; ctx->gpr[126] = e126;
    __atomic_add_fetch(&s_d618_tris, (long long)numtri, __ATOMIC_RELAXED);
    return 1;
}

static void d618_stats(void)
{
    fprintf(stderr, "[edge-native] cull D618: %lld calls native, %lld triangles, %lld declined\n",
            s_d618_calls, s_d618_tris, s_d618_declined);
    for (int i = 0; i < 32; i++)
        if (s_d618_why[i]) fprintf(stderr, "[edge-native] cull D618 declined at check %d: %lld\n", i, s_d618_why[i]);
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D618(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_d618_guard;
static edge_vstat s_v618 = { "cull D618", 0, 1, 0, 0, 0, 0, 0 };
static void d618_verify_report(void) { edge_vreport(&s_v618); }

/* The hook at 0xD618's entry: 1 when the call is done (the function returns). */
int gowa_edge_cull_d618(spu_context* ctx)
{
    if (t_d618_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 512) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(d618_stats);
        if (on && edge_verify_on()) atexit(d618_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_v618, d618_native, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000D618, &t_d618_guard);
    if (d618_native(ctx)) {
        __atomic_add_fetch(&s_d618_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_d618_declined, 1, __ATOMIC_RELAXED);
    return 0;
}


#else
int gowa_edge_cull_d250(spu_context* ctx) { (void)ctx; return 0; }
int gowa_edge_cull_d618(spu_context* ctx) { (void)ctx; return 0; }
#endif
