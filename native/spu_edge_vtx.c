/*
 * EDGE's vertex unpacking for the lit jobs: the position-independent piece of EA 0x75BD10
 * (lifted as pic_0075BD10; the job calls it at its start, returning to 0xDC5C), GOWA_EDGE_NATIVE
 * bit 16384, on unless GOWA_EDGE_NATIVE leaves it out.
 *
 * The hook is at 0x30020, after the piece's first eight instructions, which run before it:
 *  - they store the back chain at r1 - 224 and take 224 from r1;
 *  - they load the attribute list (c + 0x160) into r23;
 *  - they set r8 = 1, r69 = 3 and r44 = c + 16;
 *  - they load two of the patterns below into r25 and r21.
 * The arguments: r3 the EDGE context c, r4 the stream, r5 the vertex count n.
 *
 * It unpacks cnt = (n + 1) & ~1 records of 18 bytes into three attributes it adds to the list.
 * A record is x y z (F32), then a 48-bit code.
 *  - The list: ids 1, 3 and 4 go to its first three free (zero) slots s. Their arrays are the
 *    pool's words at c + 0x10 + 4 s. The same words also go to c + 0x5C, c + 0x64, c + 0x68.
 *  - Attribute 1: x y z copied, w = 1.0.
 *  - Attributes 3 and 4 come from the code's high and low 24 bits G:
 *    - m1 = G >> 14, m2 = (G >> 4) & 0x3FF, idx = G & 15;
 *    - c1 = fma(A, f(0x3F800000 | m1), B), and c2 the same from m2, with A, B = 0x46352E7C,
 *      0xC6353150 (so -+0.7071 for m 0 and 1023);
 *    - t = 1 - (c1 * c1 + c2 * c2), and c3 = t * fi(t, frsqest(t)), its square root;
 *    - pattern idx is one of the 12 the piece stores at r1 + 0x20 + 16 idx. c3 goes to x, y or
 *      z for idx / 4, with c1 and c2 in order in the other two. c3 is negative for an even idx.
 *      w is -1.0 for idx % 4 < 2, else 1.0.
 *    The float ops are the SPU's: toward zero, no denormals.
 *  - An idx past 11 reads past the table, into the caller's stack: such a vector is what the
 *    piece's shuffle makes from those bytes (v9_general does it with the piece's own ops; 16 pairs
 *    in the replay's 68 calls).
 * What else it leaves:
 *  - the 12 patterns in the frame, r1 + 0x20 .. r1 + 0xDF;
 *  - two zero quadwords at the end of the arrays of 3 and 4, written first: its first pass
 *    stores what the pass before it would have made;
 *  - r1 + 224, and r3 = 0. The other registers do not matter: the strict replay with r2-r79
 *    scrambled after every call gave no difference.
 * Mapped from the probed calls with a bit-exact model (10,224 vectors in 12 calls) and from the
 * piece's code.
 *
 * Eight vertices a pass, fast first (spu_edge_simd.h), redone exact on a flag. Declined (lifted
 * instead):
 *  - another MXCSR;
 *  - n < 2;
 *  - c or r1 not 16-aligned;
 *  - r8, r44, r69 not as the first instructions leave them;
 *  - the piece's constants (its pool, r21, r25) not as in the executable;
 *  - fewer than three free slots, or an array not 16-aligned;
 *  - overlaps between the arrays, the junk past them and the position array, the stream, the
 *    context's fields, the frame (and the 64 bytes above it that idx 12-15 read) and the pool.
 * GOWA_EDGE_VERIFY=1 checks every call (spu_edge_verify.h).
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_verify.h"
#include "spu_edge_simd.h"
#include "spu_edge_memo.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

#define V9_A 0x46352E7Cu
#define V9_B 0xC6353150u
#define V9_ONE 0x3F800000u
#define V9_POOL 0x304B0u          /* the piece's constants, from the lifted base 0x30000 */
#define V9_POOL_Q 22

/* the piece's constant pool as the executable holds it (big-endian words) */
static const uint32_t v9_pool[V9_POOL_Q][4] = {
    { 0x90111213u, 0x00010203u, 0x94151617u, 0x3F800000u },   /* 0x304B0, r25 */
    { 0x10111213u, 0x00010203u, 0x14151617u, 0x3F800000u },   /* 0x304C0, r21 */
    { 0x90111213u, 0x14151617u, 0x80010203u, 0xBF800000u },   /* 0x304D0 */
    { 0x90111213u, 0x14151617u, 0x80010203u, 0x3F800000u },
    { 0x10111213u, 0x14151617u, 0x00010203u, 0x3F800000u },
    { 0x80010203u, 0x10111213u, 0x94151617u, 0xBF800000u },   /* 0x30500 */
    { 0x00010203u, 0x10111213u, 0x14151617u, 0xBF800000u },
    { 0x80010203u, 0x10111213u, 0x94151617u, 0x3F800000u },
    { 0x00010203u, 0x10111213u, 0x14151617u, 0x3F800000u },
    { 0x90111213u, 0x00010203u, 0x94151617u, 0xBF800000u },   /* 0x30540 */
    { 0x10111213u, 0x00010203u, 0x14151617u, 0xBF800000u },
    { 0x10111213u, 0x14151617u, 0x00010203u, 0xBF800000u },   /* 0x30560 */
    { 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu },
    { 0x80800280u, 0x80800001u, 0x80800102u, 0x80808080u },
    { 0xFFFFFFFCu, 0xFFFFFFFAu, 0xFFFFFFFCu, 0x00000000u },   /* 0x30590 */
    { 0x00000000u, 0x3F800000u, 0x3F800000u, 0x00000000u },
    { 0x000000F0u, 0x000003FFu, 0x000003FFu, 0x00000000u },
    { 0x04050607u, 0x08090A0Bu, 0x14151617u, 0x18191A1Bu },
    { 0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u },   /* 0x305D0 */
    { 0x00000000u, 0x00000000u, 0x00000000u, 0x0C0D0E0Fu },
    { 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x00000000u },
    { 0x80000000u, 0x80000000u, 0x80000000u, 0xFFFFFFFFu },   /* 0x30600 */
};
/* the frame's 12 patterns, r1 + 0x20 + 16 k: the pool quadword each is */
static const uint8_t v9_frame_q[12] = { 5, 6, 7, 8, 9, 10, 0, 1, 2, 11, 3, 4 };

static volatile long long s_v9_calls, s_v9_verts, s_v9_declined, s_v9_redone, s_v9_general, s_v9_why[14];
static int v9_no(int why) { __atomic_add_fetch(&s_v9_why[why], 1, __ATOMIC_RELAXED); return 0; }

static int v9_apart(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a1 <= b0 || b1 <= a0; }

/* x y z w of eight 24-bit codes G, as words: fast (the native float ops) or with the runtime's
 * exact forms */
EC_INL void v9_vec8(const int f, __m256i G, __m256i out[4])
{
    const __m256i m1 = _mm256_or_si256(_mm256_srli_epi32(G, 14), _mm256_set1_epi32((int)V9_ONE));
    const __m256i m2 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(G, 4), _mm256_set1_epi32(0x3FF)),
                                       _mm256_set1_epi32((int)V9_ONE));
    __m256i c1, c2, c3;
    if (f) {
        const __m256 A = ec_set(V9_A), B = ec_set(V9_B);
        const __m256 x1 = _mm256_fmadd_ps(A, _mm256_castsi256_ps(m1), B);
        const __m256 x2 = _mm256_fmadd_ps(A, _mm256_castsi256_ps(m2), B);
        const __m256 t = _mm256_sub_ps(ec_set(V9_ONE), _mm256_add_ps(_mm256_mul_ps(x1, x1), _mm256_mul_ps(x2, x2)));
        const __m256i ti = _mm256_castps_si256(t);
        const __m256 x3 = _mm256_mul_ps(t, _mm256_castsi256_ps(ec_fi(ti, ec_frsqest(ti))));
        /* the results go into the flags' read (spu_edge_simd.h's ordering) */
        EC_FL_USE(x1); EC_FL_USE(x2); EC_FL_USE(x3);
        c1 = _mm256_castps_si256(x1); c2 = _mm256_castps_si256(x2); c3 = _mm256_castps_si256(x3);
    } else {
        u128 a, b, one, r1[2], r2[2], r3[2];
        for (int i = 0; i < 4; i++) { a._u32[i] = V9_A; b._u32[i] = V9_B; one._u32[i] = V9_ONE; }
        for (int h = 0; h < 2; h++) {
            const u128 v1 = ec_u(h ? _mm256_extracti128_si256(m1, 1) : _mm256_castsi256_si128(m1));
            const u128 v2 = ec_u(h ? _mm256_extracti128_si256(m2, 1) : _mm256_castsi256_si128(m2));
            r1[h] = spu_fma(a, v1, b);
            r2[h] = spu_fma(a, v2, b);
            const u128 t = spu_fs(one, spu_fa(spu_fm(r1[h], r1[h]), spu_fm(r2[h], r2[h])));
            r3[h] = spu_fm(t, spu_fi(t, spu_frsqest(t)));
        }
        c1 = _mm256_castps_si256(ec_join(r1[0], r1[1]));
        c2 = _mm256_castps_si256(ec_join(r2[0], r2[1]));
        c3 = _mm256_castps_si256(ec_join(r3[0], r3[1]));
    }
    const __m256i idx = _mm256_and_si256(G, _mm256_set1_epi32(15));
    const __m256i s3 = _mm256_or_si256(c3, _mm256_slli_epi32(_mm256_andnot_si256(idx, _mm256_set1_epi32(1)), 31));
    const __m256i pos = _mm256_srli_epi32(idx, 2);
    const __m256i p0 = _mm256_cmpeq_epi32(pos, _mm256_setzero_si256());
    const __m256i p1 = _mm256_cmpeq_epi32(pos, _mm256_set1_epi32(1));
    const __m256i p2 = _mm256_cmpeq_epi32(pos, _mm256_set1_epi32(2));
    out[0] = _mm256_blendv_epi8(c1, s3, p0);
    out[1] = _mm256_blendv_epi8(_mm256_blendv_epi8(c2, s3, p1), c1, p0);
    out[2] = _mm256_blendv_epi8(c2, s3, p2);
    out[3] = _mm256_blendv_epi8(_mm256_set1_epi32((int)0xBF800000u), _mm256_set1_epi32((int)V9_ONE),
                                _mm256_cmpeq_epi32(_mm256_and_si256(idx, _mm256_set1_epi32(2)), _mm256_set1_epi32(2)));
}

/* x y z w of eight vertices as eight big-endian quadwords at lo + 16 v; nv of them */
EC_INL void v9_store8(uint8_t* ls, uint32_t lo, uint32_t v, const __m256i o[4], int nv)
{
    const __m256i t0 = _mm256_unpacklo_epi32(o[0], o[1]), t1 = _mm256_unpackhi_epi32(o[0], o[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(o[2], o[3]), t3 = _mm256_unpackhi_epi32(o[2], o[3]);
    const __m256i bs = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    __m256i q[4] = { _mm256_unpacklo_epi64(t0, t2), _mm256_unpackhi_epi64(t0, t2),
                     _mm256_unpacklo_epi64(t1, t3), _mm256_unpackhi_epi64(t1, t3) };
    for (int k = 0; k < 4; k++) q[k] = _mm256_shuffle_epi8(q[k], bs);
    for (int j = 0; j < nv; j++) {
        const __m128i x = j < 4 ? _mm256_castsi256_si128(q[j]) : _mm256_extracti128_si256(q[j - 4], 1);
        _mm_storeu_si128((__m128i*)(ls + lo + 16u * (v + (uint32_t)j)), x);
    }
}

static void v9_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* A pair's vectors as the piece makes them, for any pattern: the even and the odd vertex's
 * codes Ge, Go share a register, (c1e, c2e, c1o, c2o), and their square-root lanes; the
 * pattern of idx is the quadword at r1 + 0x20 + 16 idx, which past 11 is the caller's stack.
 *   out = shufb(selb(r15, P, sign bits), components, selb((0, 0, 0, 0x0C0D0E0F), P, 0x7FFFFFFF x 3)),
 * the odd vertex's control plus 0x08080808 in words 0-2. which: 1 the even, 2 the odd, 3 both. */
static void v9_general(uint8_t* ls, uint32_t sp, uint32_t Ge, uint32_t Go, int which, uint32_t lo, uint32_t v)
{
    u128 a, b, one, x, sm, m7, r31, add8;
    for (int i = 0; i < 4; i++) {
        a._u32[i] = V9_A; b._u32[i] = V9_B; one._u32[i] = V9_ONE;
        sm._u32[i] = i < 3 ? 0x80000000u : 0xFFFFFFFFu;
        m7._u32[i] = i < 3 ? 0x7FFFFFFFu : 0;
        r31._u32[i] = i < 3 ? 0 : 0x0C0D0E0Fu;
        add8._u32[i] = i < 3 ? 0x08080808u : 0;
    }
    x._u32[0] = V9_ONE | (Ge >> 14); x._u32[1] = V9_ONE | ((Ge >> 4) & 0x3FF);
    x._u32[2] = V9_ONE | (Go >> 14); x._u32[3] = V9_ONE | ((Go >> 4) & 0x3FF);
    const u128 comp = spu_fma(a, x, b);
    const u128 sq = spu_fm(comp, comp);
    u128 sh;
    sh._u32[0] = sq._u32[1]; sh._u32[1] = sq._u32[2]; sh._u32[2] = sq._u32[3]; sh._u32[3] = 0;
    const u128 t = spu_fs(one, spu_fa(sq, sh));
    const u128 r15 = spu_fm(t, spu_fi(t, spu_frsqest(t)));
    for (int k = 0; k < 2; k++) {
        if (!(which & (1 << k))) continue;
        const uint32_t idx = (k ? Go : Ge) & 15;
        const u128 P = ec_u(ec_ldq(ls, sp + 0x20u + 16u * idx));
        u128 ctrl = spu_selb(r31, P, m7);
        if (k) ctrl = spu_a(ctrl, add8);
        const u128 out = spu_shufb(spu_selb(r15, P, sm), comp, ctrl);
        ec_stq(ls, lo + 16u * (v + (uint32_t)k), ec_m(out));
    }
}

/* the arrays: cnt records from src; sp the frame with the patterns */
static void v9_decode(uint8_t* ls, uint32_t sp, const uint8_t* src, uint32_t cnt, uint32_t P0, uint32_t N0, uint32_t T0)
{
    const unsigned csr = _mm_getcsr();
    const int force = ec_force_exact();
    ec_fl_clear(csr);
    for (uint32_t v = 0; v < cnt; v += 8) {
        const int nv = (int)(cnt - v < 8 ? cnt - v : 8);
        uint32_t gn[8] = { 0 }, gt[8] = { 0 };
        for (int j = 0; j < nv; j++) {
            const uint8_t* r = src + 18u * (v + (uint32_t)j);
            gn[j] = ((uint32_t)r[12] << 16) | ((uint32_t)r[13] << 8) | r[14];
            gt[j] = ((uint32_t)r[15] << 16) | ((uint32_t)r[16] << 8) | r[17];
            /* the position: x y z, w = 1.0 (big-endian 3F800000) */
            const __m128i q = _mm_blend_epi32(_mm_loadu_si128((const __m128i*)r), _mm_set1_epi32(0x0000803F), 8);
            _mm_storeu_si128((__m128i*)(ls + P0 + 16u * (v + (uint32_t)j)), q);
        }
        const __m256i GN = _mm256_loadu_si256((const __m256i*)gn), GT = _mm256_loadu_si256((const __m256i*)gt);
        __m256i on[4], ot[4];
        int fast = !force;
        if (fast) {
            v9_vec8(1, GN, on);
            v9_vec8(1, GT, ot);
            if (ec_fl_read() & EC_FLAGS_BAD) {
                fast = 0;
                __atomic_add_fetch(&s_v9_redone, 1, __ATOMIC_RELAXED);
            }
        }
        if (!fast) {
            v9_vec8(0, GN, on);
            v9_vec8(0, GT, ot);
            ec_fl_clear(csr);
        }
        v9_store8(ls, N0, v, on, nv);
        v9_store8(ls, T0, v, ot, nv);
        /* a pattern past the 12 (the caller's stack): the pair as the piece makes it */
        for (int j = 0; j < nv; j += 2) {
            const int wn = ((gn[j] & 15) >= 12) | (((gn[j + 1] & 15) >= 12) << 1);
            const int wt = ((gt[j] & 15) >= 12) | (((gt[j + 1] & 15) >= 12) << 1);
            if (wn) v9_general(ls, sp, gn[j], gn[j + 1], wn, N0, v + (uint32_t)j);
            if (wt) v9_general(ls, sp, gt[j], gt[j + 1], wt, T0, v + (uint32_t)j);
            if (wn | wt) __atomic_add_fetch(&s_v9_general, 1, __ATOMIC_RELAXED);
        }
    }
    ec_fl_set(csr);
}

static int v9_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return v9_no(0);
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], n = ctx->gpr[5]._u32[0];
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if ((c & 15) || c + 0x170 > SPU_LS_SIZE || (sp & 15) || sp + 0x120 > SPU_LS_SIZE) return v9_no(1);
    if (n < 2 || n > 0x4000) return v9_no(2);
    if (ctx->gpr[44]._u32[0] != c + 16 || (ctx->gpr[8]._u32[0] & 0xFF) != 1 || (ctx->gpr[69]._u32[0] & 0xFF) != 3)
        return v9_no(3);
    /* the piece's constants, and the two patterns its first instructions loaded */
    const uint32_t pool = (ctx->pic_base + V9_POOL) & (SPU_LS_SIZE - 1);
    if ((pool & 15) || pool + 16u * V9_POOL_Q > SPU_LS_SIZE) return v9_no(4);
    for (int q = 0; q < V9_POOL_Q; q++)
        for (int k = 0; k < 4; k++)
            if (ec_be32(ls + pool + 16 * q + 4 * k) != v9_pool[q][k]) return v9_no(5);
    for (int k = 0; k < 4; k++)
        if (ctx->gpr[25]._u32[k] != v9_pool[0][k] || ctx->gpr[21]._u32[k] != v9_pool[1][k]) return v9_no(6);
    /* the list (r23, as the first instructions loaded it): 1, 3, 4 into its first free slots */
    uint8_t ids[16];
    for (int k = 0; k < 4; k++) v9_put32(ids + 4 * k, ctx->gpr[23]._u32[k]);
    static const uint8_t idv[3] = { 1, 3, 4 };
    uint32_t base[3];
    for (int a = 0; a < 3; a++) {
        int s = 0;
        while (s < 16 && ids[s]) s++;
        if (s == 16) return v9_no(7);
        ids[s] = idv[a];
        base[a] = ec_be32(ls + c + 0x10 + 4 * s);
        if (base[a] & 15) return v9_no(8);
        base[a] &= SPU_LS_SIZE - 16;
    }
    const uint32_t cnt = (n + 1) & ~1u, len = 16u * cnt;
    const uint32_t P0 = base[0], N0 = base[1], T0 = base[2];
    if (P0 + len > SPU_LS_SIZE || N0 + len + 32 > SPU_LS_SIZE || T0 + len + 32 > SPU_LS_SIZE) return v9_no(9);
    const uint32_t S0 = src & ~15u, S1 = (src + 18u * cnt + 96u + 15u) & ~15u;
    if (src >= SPU_LS_SIZE || S1 > SPU_LS_SIZE) return v9_no(10);
    /* the arrays apart; the junk past those of 3 and 4 apart from the positions' (it would land
     * between their stores) */
    if (!v9_apart(P0, P0 + len, N0, N0 + len) || !v9_apart(P0, P0 + len, T0, T0 + len) ||
        !v9_apart(N0, N0 + len, T0, T0 + len) || !v9_apart(N0 + len, N0 + len + 32, P0, P0 + len) ||
        !v9_apart(T0 + len, T0 + len + 32, P0, P0 + len))
        return v9_no(11);
    /* what it writes apart from what it reads: the stream, the context's fields (pool to list),
     * the frame, the piece's constants */
    {
        const uint32_t wr[3][2] = { { P0, P0 + len }, { N0, N0 + len + 32 }, { T0, T0 + len + 32 } };
        const uint32_t other[4][2] = { { S0, S1 }, { c + 0x10, c + 0x170 }, { sp, sp + 0x120 }, { pool, pool + 16u * V9_POOL_Q } };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                if (!v9_apart(wr[i][0], wr[i][1], other[j][0], other[j][1])) return v9_no(12);
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                if (!v9_apart(other[i][0], other[i][1], other[j][0], other[j][1])) return v9_no(13);
    }
    /* the list, the table, the frame's patterns, the junk */
    memcpy(ls + c + 0x160, ids, 16);
    v9_put32(ls + c + 0x5C, base[0]);
    v9_put32(ls + c + 0x64, base[1]);
    v9_put32(ls + c + 0x68, base[2]);
    for (int k = 0; k < 12; k++) memcpy(ls + sp + 0x20 + 16 * k, ls + pool + 16 * v9_frame_q[k], 16);
    memset(ls + N0 + len, 0, 32);
    memset(ls + T0 + len, 0, 32);

    /* a kept result (spu_edge_memo.c): the list is filled, the arrays can wait */
    if (!edge_memo_unpack(ctx, 1, src, cnt, P0, N0, T0)) v9_decode(ls, sp, ls + src, cnt, P0, N0, T0);

    for (int k = 0; k < 4; k++) { ctx->gpr[1]._u32[k] += 224u; ctx->gpr[3]._u32[k] = 0; }
    __atomic_add_fetch(&s_v9_verts, (long long)cnt, __ATOMIC_RELAXED);
    return 1;
}

extern void pic_0075BD10_spu_func_00030020(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_v9_guard;

static void v9_stats(void)
{
    fprintf(stderr, "[edge-native] vertex unpack 75BD10: %lld calls native, %lld vertices, %lld declined, %lld blocks redone exact, "
            "%lld pairs with a pattern past 11\n", s_v9_calls, s_v9_verts, s_v9_declined, s_v9_redone, s_v9_general);
    fprintf(stderr, "[edge-native] vertex unpack 75BD10 declined by check:");
    for (unsigned i = 0; i < sizeof s_v9_why / sizeof s_v9_why[0]; i++) if (s_v9_why[i]) fprintf(stderr, " #%u %lld", i, s_v9_why[i]);
    fprintf(stderr, "\n");
}
static edge_vstat s_vv9 = { "vertex unpack 75BD10", 0, 1, 0, 0, 0, 0, 0, 0 };
static void v9_verify_report(void) { edge_vreport(&s_vv9); }

/* The hook at 0x30020: 1 when the call is done (the function returns). */
int gowa_edge_vtx_75bd10(spu_context* ctx)
{
    if (t_v9_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 16384) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(v9_stats);
        if (on && edge_verify_on()) atexit(v9_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vv9, v9_native, pic_0075BD10_spu_func_00030020, &t_v9_guard);
    if (v9_native(ctx)) {
        __atomic_add_fetch(&s_v9_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_v9_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

/*
 * The position-only unpacking: the piece of EA 0x764D10 (lifted as pic_00764D10),
 * GOWA_EDGE_NATIVE bit 32768.
 *
 * The hook is at 0x30018, after its first six instructions:
 *  - they store the back chain and take 224 from r1;
 *  - they load the list into r45;
 *  - they set r44 = 1 and r25 = r4 + 36;
 *  - they load the first pattern into r57.
 *
 * The records are those of 0x75BD10 (above), but it unpacks only attribute 1: x y z, w = 1.0.
 *  - The array is the first free slot's in the pool, and its word also goes to c + 0x5C.
 *  - It writes the same 12 patterns to its frame, from its own pool, which holds them in the
 *    frame's order.
 *  - It leaves r1 + 224 and r3 = 0, and no junk.
 * Declined as above.
 */
#define P10_POOL 0x301C0u
#define P10_POOL_Q 14
extern void pic_00764D10_spu_func_00030018(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_p10_guard;
static volatile long long s_p10_calls, s_p10_verts, s_p10_declined;

/* its pool's quadword q (big-endian words): the 12 patterns in the frame's order, the identity
 * pattern, then (0, 0, 0, 1.0) */
static const uint32_t* p10_pool(int q)
{
    return q < 12 ? v9_pool[v9_frame_q[q]] : v9_pool[q == 12 ? 12 : 18];
}

static void p10_decode(uint8_t* ls, uint32_t src, uint32_t cnt, uint32_t P0)
{
    for (uint32_t v = 0; v < cnt; v++) {
        const __m128i q = _mm_blend_epi32(_mm_loadu_si128((const __m128i*)(ls + src + 18u * v)), _mm_set1_epi32(0x0000803F), 8);
        _mm_storeu_si128((__m128i*)(ls + P0 + 16u * v), q);
    }
}

/* The arrays of a lit job that left them out (spu_edge_memo.c), late, its frame below the stack
 * pointer: the stream as it came, a[] its place, the records and the arrays. */
void edge_vtx_unpack_late(spu_context* ctx, const uint8_t* stream, const uint32_t a[5])
{
    uint8_t* ls = ctx->ls;
    const uint32_t sp = ctx->gpr[1]._u32[0] - 224u;
    for (int k = 0; k < 12; k++)
        for (int w = 0; w < 4; w++) v9_put32(ls + sp + 0x20 + 16 * k + 4 * w, v9_pool[v9_frame_q[k]][w]);
    v9_decode(ls, sp, stream, a[1], a[2], a[3], a[4]);
}

static int p10_native(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], n = ctx->gpr[5]._u32[0];
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if ((c & 15) || c + 0x170 > SPU_LS_SIZE || (sp & 15) || sp + 0xE0 > SPU_LS_SIZE) return 0;
    if (n < 2 || n > 0x4000) return 0;
    if ((ctx->gpr[44]._u32[0] & 0xFF) != 1 || ctx->gpr[25]._u32[0] != src + 36) return 0;
    const uint32_t pool = (ctx->pic_base + P10_POOL) & (SPU_LS_SIZE - 1);
    if ((pool & 15) || pool + 16u * P10_POOL_Q > SPU_LS_SIZE) return 0;
    for (int q = 0; q < P10_POOL_Q; q++)
        for (int k = 0; k < 4; k++)
            if (ec_be32(ls + pool + 16 * q + 4 * k) != p10_pool(q)[k]) return 0;
    for (int k = 0; k < 4; k++)
        if (ctx->gpr[57]._u32[k] != p10_pool(0)[k]) return 0;
    /* the list (r45): 1 into its first free slot */
    uint8_t ids[16];
    for (int k = 0; k < 4; k++) v9_put32(ids + 4 * k, ctx->gpr[45]._u32[k]);
    int s = 0;
    while (s < 16 && ids[s]) s++;
    if (s == 16) return 0;
    ids[s] = 1;
    const uint32_t base = ec_be32(ls + c + 0x10 + 4 * s);
    if (base & 15) return 0;
    const uint32_t P0 = base & (SPU_LS_SIZE - 16), cnt = (n + 1) & ~1u, len = 16u * cnt;
    if (P0 + len > SPU_LS_SIZE) return 0;
    const uint32_t S0 = src & ~15u, S1 = (src + 18u * cnt + 96u + 15u) & ~15u;
    if (src >= SPU_LS_SIZE || S1 > SPU_LS_SIZE) return 0;
    {
        const uint32_t other[4][2] = { { S0, S1 }, { c + 0x10, c + 0x170 }, { sp, sp + 0xE0 }, { pool, pool + 16u * P10_POOL_Q } };
        for (int j = 0; j < 4; j++)
            if (!v9_apart(P0, P0 + len, other[j][0], other[j][1])) return 0;
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                if (!v9_apart(other[i][0], other[i][1], other[j][0], other[j][1])) return 0;
    }
    memcpy(ls + c + 0x160, ids, 16);
    v9_put32(ls + c + 0x5C, base);
    for (int k = 0; k < 12; k++) memcpy(ls + sp + 0x20 + 16 * k, ls + pool + 16 * k, 16);
    /* a kept result (spu_edge_memo.c): the list is filled, the array can wait */
    if (!edge_memo_unpack(ctx, 0, src, cnt, P0, 0, 0)) p10_decode(ls, src, cnt, P0);
    for (int k = 0; k < 4; k++) { ctx->gpr[1]._u32[k] += 224u; ctx->gpr[3]._u32[k] = 0; }
    __atomic_add_fetch(&s_p10_verts, (long long)cnt, __ATOMIC_RELAXED);
    return 1;
}

static void p10_stats(void)
{
    fprintf(stderr, "[edge-native] position unpack 764D10: %lld calls native, %lld vertices, %lld declined\n",
            s_p10_calls, s_p10_verts, s_p10_declined);
}
static edge_vstat s_vp10 = { "position unpack 764D10", 0, 1, 0, 0, 0, 0, 0, 0 };
static void p10_verify_report(void) { edge_vreport(&s_vp10); }

/* The hook at 0x30018: 1 when the call is done (the function returns). */
int gowa_edge_pos_764d10(spu_context* ctx)
{
    if (t_p10_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 32768) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(p10_stats);
        if (on && edge_verify_on()) atexit(p10_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vp10, p10_native, pic_00764D10_spu_func_00030018, &t_p10_guard);
    if (p10_native(ctx)) {
        __atomic_add_fetch(&s_p10_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_p10_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

#else
int gowa_edge_vtx_75bd10(spu_context* ctx) { (void)ctx; return 0; }
void edge_vtx_unpack_late(spu_context* ctx, const uint8_t* stream, const uint32_t a[5]) { (void)ctx; (void)stream; (void)a; }
int gowa_edge_pos_764d10(spu_context* ctx) { (void)ctx; return 0; }
#endif
