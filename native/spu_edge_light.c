/* EDGE's per-vertex lighting loop as native code.
 *
 * Job module e8f44fbc6dbf (the EDGE geometry job), loop at LS 0x7BB4, inside
 * the lighting stage at 0x73E0. At the gate after the first fight that stage
 * is 23% of EDGE's time and this loop 57% of the stage (performance plan
 * 2.19). An iteration takes one light for eight vertices, two groups of four
 * in SoA form: A, positions r66/r62/r60 and normals r52/r54/r56, and B,
 * positions r65/r63/r61 and normals r50/r53/r55. The lights are three arrays
 * of quadwords, position (r99), colour (r97) and a direction (r98).
 *
 * It is software-pipelined: iteration j finishes light j with what iteration
 * j-1 began for it, and begins light j+1.
 *   finish j:  d = sq * rs, n = v * rs                (v: light - vertex)
 *              L = j's direction ? (r40, r38, r41) : n   (shufb by r9)
 *              store L (A x y z, B x y z) at r23
 *              m = min(d * k, 1), t = 1 - m, att = (t*t * (3 - 2t))^2 * c'.w
 *              h = min(|(N.L) * 0.5 + 0.5|, 1), w = |att| * h
 *              store att * c'.xyz (A r g b, B r g b) at r29
 *              sum += w, sum_x += L.x * w, sum_y ..., sum_z ... (fa, fma)
 *   begin j+1: P, C, Q at r99/r97/r98 + r43; v = P.xyz - vertex,
 *              sq = v.x^2 + 1e-4, + v.y^2, + v.z^2 (fma), rs = fi(sq, frsqest(sq)),
 *              k = P.w, c' = r93 * C, the direction choice from r100 > r43
 * A and B take the same operations on different registers, so here they share
 * one 256-bit register (A low, B high): one native op for two lifted ones, in
 * the lifted code's order, with its operands.
 *
 * The SPU float ops are native under the SPU code's MXCSR (toward zero, FTZ,
 * DAZ) with the +0 rule; a result at or beyond FLT_MAX in any lane has that op
 * done again by the exact forms on both halves (spu_fm & co.), as the lifted
 * code's forms do. frsqest and fi are spu_edge_decode.c's 256-bit ports of the
 * runtime's forms. The stores keep the lifted order within each array; the
 * arrays must not overlap one another or the light arrays read on the way,
 * else the lifted loop runs. The last iteration runs in the lifted loop, from
 * the 31 registers the loop carries, written back as it has them, so the job
 * ends with exactly the lifted registers.
 *
 * It runs only when the shuffle controls are the splats (r59 r82 r81 r78) and
 * the two choices (r64: the first operand, r104: the second), r9 one of them,
 * and the SPU code's MXCSR is set; an interrupt the lifted loop would take
 * stops it at that iteration. GOWA_EDGE_NATIVE bit 32 (32 = on with the other
 * bits off), GOWA_EDGE_NATIVE_STATS=1 counts (only then: six SPU threads adding
 * to one counter a call cost ~4% of the loop). */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) && defined(__FMA__)

#define LT_MXCSR_RZFZ 0xE040u   /* round toward zero, FTZ, DAZ (SPU_MXCSR_RZFZ) */
/* Every helper inline: on Windows x64 a __m256 argument is passed through memory, so a
 * helper left as a call (clang-cl left lt_fm & co. so, with their fallbacks in them) costs
 * more than the op. The exact fallbacks are cold calls of their own. */
#define LT_INL static inline __attribute__((always_inline))
#define LT_COLD static __attribute__((noinline, cold))

LT_INL __m128i lt_ld(const spu_context* ctx, int r) { return _mm_loadu_si128((const __m128i*)ctx->gpr[r]._u32); }
LT_INL void lt_st(spu_context* ctx, int r, __m128i v) { _mm_storeu_si128((__m128i*)ctx->gpr[r]._u32, v); }
LT_INL __m128i lt_bswap4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
LT_INL __m128i lt_ls_rd(const uint8_t* ls, uint32_t a)
{
    return lt_bswap4(_mm_loadu_si128((const __m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu)))));
}
LT_INL void lt_ls_wr(uint8_t* ls, uint32_t a, __m128i v)
{
    _mm_storeu_si128((__m128i*)(ls + (a & (SPU_LS_MASK & ~0xFu))), lt_bswap4(v));
}

LT_INL __m256 lt_pair(__m128i lo, __m128i hi)
{
    return _mm256_castsi256_ps(_mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1));
}
LT_INL __m256 lt_dup(__m128i v) { return lt_pair(v, v); }
LT_INL __m128i lt_lo(__m256 v) { return _mm256_castsi256_si128(_mm256_castps_si256(v)); }
LT_INL __m128i lt_hi(__m256 v) { return _mm256_extracti128_si256(_mm256_castps_si256(v), 1); }
LT_INL u128 lt_u(__m128i v) { u128 r; _mm_storeu_si128((__m128i*)r._u32, v); return r; }
LT_INL __m128i lt_m(u128 v) { return _mm_loadu_si128((const __m128i*)v._u32); }
LT_INL __m256 lt_join(u128 lo, u128 hi) { return lt_pair(lt_m(lo), lt_m(hi)); }

/* the SPU forms: native where every lane is below FLT_MAX in magnitude (with
 * the +0 rule), else the exact forms on both halves */
LT_INL int lt_ok(__m256 r)
{
    const __m256i m = _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF));
    return _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x7F7FFFFF), m))) == 0xFF;
}
LT_INL __m256 lt_pz(__m256 r) { return _mm256_and_ps(r, _mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_NEQ_UQ)); }
static volatile long long s_lt_exact;
#define LT_EXACT() __atomic_add_fetch(&s_lt_exact, 1, __ATOMIC_RELAXED)
LT_COLD __m256 lt_fm_x(__m256 a, __m256 b)
{
    LT_EXACT();
    return lt_join(spu_fm(lt_u(lt_lo(a)), lt_u(lt_lo(b))), spu_fm(lt_u(lt_hi(a)), lt_u(lt_hi(b))));
}
LT_COLD __m256 lt_fa_x(__m256 a, __m256 b)
{
    LT_EXACT();
    return lt_join(spu_fa(lt_u(lt_lo(a)), lt_u(lt_lo(b))), spu_fa(lt_u(lt_hi(a)), lt_u(lt_hi(b))));
}
LT_COLD __m256 lt_fs_x(__m256 a, __m256 b)
{
    LT_EXACT();
    return lt_join(spu_fs(lt_u(lt_lo(a)), lt_u(lt_lo(b))), spu_fs(lt_u(lt_hi(a)), lt_u(lt_hi(b))));
}
LT_COLD __m256 lt_fma_x(__m256 a, __m256 b, __m256 c)
{
    LT_EXACT();
    return lt_join(spu_fma(lt_u(lt_lo(a)), lt_u(lt_lo(b)), lt_u(lt_lo(c))),
                   spu_fma(lt_u(lt_hi(a)), lt_u(lt_hi(b)), lt_u(lt_hi(c))));
}
LT_COLD __m256 lt_fnms_x(__m256 a, __m256 b, __m256 c)
{
    LT_EXACT();
    return lt_join(spu_fnms(lt_u(lt_lo(a)), lt_u(lt_lo(b)), lt_u(lt_lo(c))),
                   spu_fnms(lt_u(lt_hi(a)), lt_u(lt_hi(b)), lt_u(lt_hi(c))));
}
LT_COLD __m256 lt_fcgt_x(__m256 a, __m256 b)
{
    LT_EXACT();
    return lt_join(spu_fcgt(lt_u(lt_lo(a)), lt_u(lt_lo(b))), spu_fcgt(lt_u(lt_hi(a)), lt_u(lt_hi(b))));
}
LT_INL __m256 lt_fm(__m256 a, __m256 b)
{
    const __m256 r = _mm256_mul_ps(a, b);
    return __builtin_expect(lt_ok(r), 1) ? lt_pz(r) : lt_fm_x(a, b);
}
LT_INL __m256 lt_fa(__m256 a, __m256 b)
{
    const __m256 r = _mm256_add_ps(a, b);
    return __builtin_expect(lt_ok(r), 1) ? lt_pz(r) : lt_fa_x(a, b);
}
LT_INL __m256 lt_fs(__m256 a, __m256 b)            /* a - b */
{
    const __m256 r = _mm256_sub_ps(a, b);
    return __builtin_expect(lt_ok(r), 1) ? lt_pz(r) : lt_fs_x(a, b);
}
LT_INL __m256 lt_fma(__m256 a, __m256 b, __m256 c) /* a*b + c */
{
    const __m256 r = _mm256_fmadd_ps(a, b, c);
    return __builtin_expect(lt_ok(r), 1) ? lt_pz(r) : lt_fma_x(a, b, c);
}
LT_INL __m256 lt_fnms(__m256 a, __m256 b, __m256 c) /* c - a*b */
{
    const __m256 r = _mm256_fnmadd_ps(a, b, c);
    return __builtin_expect(lt_ok(r), 1) ? lt_pz(r) : lt_fnms_x(a, b, c);
}
/* fcgt: the float compare where both sides are in range (DAZ, no NaN), else the
 * runtime's on both halves */
LT_INL __m256 lt_fcgt(__m256 a, __m256 b)
{
    return __builtin_expect(lt_ok(a) && lt_ok(b), 1) ? _mm256_cmp_ps(a, b, _CMP_GT_OQ) : lt_fcgt_x(a, b);
}

/* frsqest and fi on two quadwords: spu_edge_decode.c's ports of the runtime's
 * x86 forms (spu_frsqest, spu_fi, spu__xe_lut), lane for lane */
LT_INL __m256i lt_lut(const uint32_t* tbl, int n, __m256i idx)
{
    const __m256i row = _mm256_srli_epi32(idx, 3);
    __m256i r = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)tbl), idx);
    for (int k = 1; k < n; k++) {
        const __m256i v = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(tbl + 8 * k)), idx);
        r = _mm256_blendv_epi8(r, v, _mm256_cmpeq_epi32(row, _mm256_set1_epi32(k)));
    }
    return r;
}
LT_INL __m256i lt_frsqest(__m256i x)
{
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x3F));
    const __m256i frac = _mm256_and_si256(lt_lut(spu_frsqest_fraction_lut, 8, idx), _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(190), _mm256_srli_epi32(_mm256_add_epi32(exp, _mm256_set1_epi32(1)), 1));
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac);
}
LT_INL __m256i lt_fi(__m256i a, __m256i est)
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
LT_INL __m256 lt_rsqrt(__m256 sq)       /* fi(sq, frsqest(sq)) */
{
    const __m256i x = _mm256_castps_si256(sq);
    return _mm256_castsi256_ps(lt_fi(x, lt_frsqest(x)));
}

static int lt_all(const spu_context* ctx, int r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    const uint32_t* g = ctx->gpr[r]._u32;
    return g[0] == w0 && g[1] == w1 && g[2] == w2 && g[3] == w3;
}
static int lt_same(const spu_context* ctx, int a, int b) { return !memcmp(ctx->gpr[a]._u32, ctx->gpr[b]._u32, 16); }

/* [a, a + n) and [b, b + m) share a byte; [a, a + n) lies in the local store */
static int lt_overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m) { return a < b + m && b < a + n; }
static int lt_in_ls(uint32_t a, uint32_t n) { return n <= (uint32_t)SPU_LS_SIZE && a <= (uint32_t)SPU_LS_SIZE - n; }

static volatile long long s_lt_calls, s_lt_iters, s_lt_skipped;
static void light_stats(void)
{
    fprintf(stderr, "[edge-native] light 7BB4: %lld calls, %lld native iterations, %lld exact ops, %lld calls lifted\n",
            s_lt_calls, s_lt_iters, s_lt_exact, s_lt_skipped);
}

void gowa_edge_light_7bb4(spu_context* ctx)
{
    static int on = -1, stats = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || (atoi(e) & 32) != 0; }
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(light_stats); }
    if (!on) return;
    /* the lights left: the loop ends when r46 reaches r83; the last one stays lifted */
    const int32_t left = (int32_t)(ctx->gpr[83]._u32[0] - ctx->gpr[46]._u32[0]);
    if (left < 2) return;
    const int iters = left - 1 > 4096 ? 4096 : left - 1;
    int param;
    if (lt_same(ctx, 9, 104)) param = 1;
    else if (lt_same(ctx, 9, 64)) param = 0;
    else param = -1;
    const uint32_t a43 = ctx->gpr[43]._u32[0], o23 = ctx->gpr[23]._u32[0], o29 = ctx->gpr[29]._u32[0];
    const uint32_t p99 = ctx->gpr[99]._u32[0] + a43, p97 = ctx->gpr[97]._u32[0] + a43, p98 = ctx->gpr[98]._u32[0] + a43;
    const uint32_t nw = 96u * (uint32_t)iters, nr = 16u * (uint32_t)iters;
    if (param < 0 || (_mm_getcsr() & LT_MXCSR_RZFZ) != LT_MXCSR_RZFZ ||
        !lt_all(ctx, 59, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u) ||
        !lt_all(ctx, 82, 0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u) ||
        !lt_all(ctx, 81, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu) ||
        !lt_all(ctx, 78, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu) ||
        !lt_all(ctx, 64, 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu) ||
        !lt_all(ctx, 104, 0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu) ||
        /* whole quadwords, inside the local store, the two outputs apart from each
         * other and from the lights read on the way */
        ((o23 | o29 | p99 | p97 | p98) & 15) ||
        !lt_in_ls(o23, nw) || !lt_in_ls(o29, nw) || !lt_in_ls(p99, nr) || !lt_in_ls(p97, nr) || !lt_in_ls(p98, nr) ||
        lt_overlap(o23, nw, o29, nw) ||
        lt_overlap(o23, nw, p99, nr) || lt_overlap(o23, nw, p97, nr) || lt_overlap(o23, nw, p98, nr) ||
        lt_overlap(o29, nw, p99, nr) || lt_overlap(o29, nw, p97, nr) || lt_overlap(o29, nw, p98, nr)) {
        if (stats) __atomic_add_fetch(&s_lt_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (stats) __atomic_add_fetch(&s_lt_calls, 1, __ATOMIC_RELAXED);   /* six threads on one line: only when asked */
    uint8_t* ls = ctx->ls;

    /* the vertices (A low, B high) and the constants */
    const __m256 VX = lt_pair(lt_ld(ctx, 66), lt_ld(ctx, 65)), VY = lt_pair(lt_ld(ctx, 62), lt_ld(ctx, 63));
    const __m256 VZ = lt_pair(lt_ld(ctx, 60), lt_ld(ctx, 61));
    const __m256 NX = lt_pair(lt_ld(ctx, 52), lt_ld(ctx, 50)), NY = lt_pair(lt_ld(ctx, 54), lt_ld(ctx, 53));
    const __m256 NZ = lt_pair(lt_ld(ctx, 56), lt_ld(ctx, 55));
    const __m256 ONE = lt_dup(lt_ld(ctx, 33)), EPS = lt_dup(lt_ld(ctx, 84)), HALF = lt_dup(lt_ld(ctx, 72));
    const __m256 THREE = lt_dup(lt_ld(ctx, 101)), TWO = lt_dup(lt_ld(ctx, 102)), SIGN = lt_dup(lt_ld(ctx, 44));
    const u128 r93 = ctx->gpr[93];
    const int32_t n100 = (int32_t)ctx->gpr[100]._u32[0];

    /* what the loop carries */
    __m256 SQ = lt_pair(lt_ld(ctx, 25), lt_ld(ctx, 24)), RS = lt_pair(lt_ld(ctx, 10), lt_ld(ctx, 12));
    __m256 DX = lt_pair(lt_ld(ctx, 31), lt_ld(ctx, 32)), DY = lt_pair(lt_ld(ctx, 28), lt_ld(ctx, 30));
    __m256 DZ = lt_pair(lt_ld(ctx, 26), lt_ld(ctx, 27));
    __m256 WS = lt_pair(lt_ld(ctx, 42), lt_ld(ctx, 45)), AX = lt_pair(lt_ld(ctx, 57), lt_ld(ctx, 48));
    __m256 AY = lt_pair(lt_ld(ctx, 49), lt_ld(ctx, 58)), AZ = lt_pair(lt_ld(ctx, 51), lt_ld(ctx, 47));
    __m128i K = lt_ld(ctx, 39), QX = lt_ld(ctx, 40), QY = lt_ld(ctx, 38), QZ = lt_ld(ctx, 41);
    __m128i CX = lt_ld(ctx, 37), CY = lt_ld(ctx, 36), CZ = lt_ld(ctx, 35), CW = lt_ld(ctx, 34);
    __m128i r43 = lt_ld(ctx, 43), r46 = lt_ld(ctx, 46), r23 = lt_ld(ctx, 23), r29 = lt_ld(ctx, 29);
    uint32_t w23 = o23, w29 = o29, q99 = p99, q97 = p97, q98 = p98;
    int done = 0;

    for (; done < iters; done++) {
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        /* ---- finish light j ---- */
        const __m256 D = lt_fm(SQ, RS);
        const __m256 nz = lt_fm(DZ, RS), nx = lt_fm(DX, RS), ny = lt_fm(DY, RS);
        const __m256 DK = lt_fm(D, lt_dup(K));
        const __m256 LX = param ? lt_dup(QX) : nx, LY = param ? lt_dup(QY) : ny, LZ = param ? lt_dup(QZ) : nz;
        lt_ls_wr(ls, w23 + 0x20, lt_lo(LZ));
        lt_ls_wr(ls, w23 + 0x00, lt_lo(LX));
        lt_ls_wr(ls, w23 + 0x10, lt_lo(LY));
        lt_ls_wr(ls, w23 + 0x30, lt_hi(LX));
        lt_ls_wr(ls, w23 + 0x40, lt_hi(LY));
        lt_ls_wr(ls, w23 + 0x50, lt_hi(LZ));
        __m256 NL = lt_fm(NX, LX);
        NL = lt_fma(NY, LY, NL);
        NL = lt_fma(NZ, LZ, NL);
        const __m256 M = _mm256_blendv_ps(DK, ONE, lt_fcgt(DK, ONE));
        const __m256 T = lt_fs(ONE, M);
        const __m256 S = lt_fnms(TWO, T, THREE);
        const __m256 T2 = lt_fm(T, T);
        const __m256 SM = lt_fm(T2, S);
        const __m256 SM2 = lt_fm(SM, SM);
        const __m256 ATT = lt_fm(SM2, lt_dup(CW));
        __m256 H = _mm256_andnot_ps(SIGN, lt_fma(NL, HALF, HALF));
        H = _mm256_blendv_ps(H, ONE, lt_fcgt(H, ONE));
        const __m256 AA = _mm256_andnot_ps(SIGN, ATT);
        const __m256 CR = lt_fm(lt_dup(CX), ATT), CG = lt_fm(lt_dup(CY), ATT), CB = lt_fm(lt_dup(CZ), ATT);
        const __m256 W = lt_fm(AA, H);
        lt_ls_wr(ls, w29 + 0x20, lt_lo(CB));
        lt_ls_wr(ls, w29 + 0x50, lt_hi(CB));
        lt_ls_wr(ls, w29 + 0x30, lt_hi(CR));
        lt_ls_wr(ls, w29 + 0x00, lt_lo(CR));
        lt_ls_wr(ls, w29 + 0x40, lt_hi(CG));
        lt_ls_wr(ls, w29 + 0x10, lt_lo(CG));
        AZ = lt_fma(LZ, W, AZ);
        WS = lt_fa(WS, W);
        AX = lt_fma(LX, W, AX);
        AY = lt_fma(LY, W, AY);
        /* ---- begin light j+1 ---- */
        const __m128i P = lt_ls_rd(ls, q99), C = lt_ls_rd(ls, q97), Q = lt_ls_rd(ls, q98);
        DX = lt_fs(lt_dup(_mm_shuffle_epi32(P, 0x00)), VX);
        DY = lt_fs(lt_dup(_mm_shuffle_epi32(P, 0x55)), VY);
        DZ = lt_fs(lt_dup(_mm_shuffle_epi32(P, 0xAA)), VZ);
        SQ = lt_fma(DX, DX, EPS);
        SQ = lt_fma(DY, DY, SQ);
        SQ = lt_fma(DZ, DZ, SQ);
        RS = lt_rsqrt(SQ);
        K = _mm_shuffle_epi32(P, 0xFF);
        QX = _mm_shuffle_epi32(Q, 0x00);
        QY = _mm_shuffle_epi32(Q, 0x55);
        QZ = _mm_shuffle_epi32(Q, 0xAA);
        const __m128i C2 = lt_m(spu_fm(r93, lt_u(C)));
        CX = _mm_shuffle_epi32(C2, 0x00);
        CY = _mm_shuffle_epi32(C2, 0x55);
        CZ = _mm_shuffle_epi32(C2, 0xAA);
        CW = _mm_shuffle_epi32(C2, 0xFF);
        param = n100 > (int32_t)_mm_cvtsi128_si32(r43);
        r43 = _mm_add_epi32(r43, _mm_set1_epi32(16));
        r46 = _mm_add_epi32(r46, _mm_set1_epi32(1));
        r23 = _mm_add_epi32(r23, _mm_set1_epi32(96));
        r29 = _mm_add_epi32(r29, _mm_set1_epi32(96));
        w23 += 96; w29 += 96; q99 += 16; q97 += 16; q98 += 16;
    }
    if (!done) return;
    lt_st(ctx, 25, lt_lo(SQ)); lt_st(ctx, 24, lt_hi(SQ));
    lt_st(ctx, 10, lt_lo(RS)); lt_st(ctx, 12, lt_hi(RS));
    lt_st(ctx, 31, lt_lo(DX)); lt_st(ctx, 32, lt_hi(DX));
    lt_st(ctx, 28, lt_lo(DY)); lt_st(ctx, 30, lt_hi(DY));
    lt_st(ctx, 26, lt_lo(DZ)); lt_st(ctx, 27, lt_hi(DZ));
    lt_st(ctx, 42, lt_lo(WS)); lt_st(ctx, 45, lt_hi(WS));
    lt_st(ctx, 57, lt_lo(AX)); lt_st(ctx, 48, lt_hi(AX));
    lt_st(ctx, 49, lt_lo(AY)); lt_st(ctx, 58, lt_hi(AY));
    lt_st(ctx, 51, lt_lo(AZ)); lt_st(ctx, 47, lt_hi(AZ));
    lt_st(ctx, 39, K); lt_st(ctx, 40, QX); lt_st(ctx, 38, QY); lt_st(ctx, 41, QZ);
    lt_st(ctx, 37, CX); lt_st(ctx, 36, CY); lt_st(ctx, 35, CZ); lt_st(ctx, 34, CW);
    lt_st(ctx, 9, lt_ld(ctx, param ? 104 : 64));
    lt_st(ctx, 43, r43); lt_st(ctx, 46, r46); lt_st(ctx, 23, r23); lt_st(ctx, 29, r29);
    if (stats) {
        const long long it = __atomic_add_fetch(&s_lt_iters, done, __ATOMIC_RELAXED);
        if ((it >> 20) != ((it - done) >> 20)) light_stats();
    }
}

/* ---------------------------------------------------------------------------
 * The second loop, LS 0x7ED8: after the first, with a normal made from what it
 * summed (r71 r69 r67 for A, r70 r68 r64 for B), each light again from the two
 * arrays the first wrote (directions at r30, colours at r31, 96 bytes a light):
 *   finish j:  l = (N.L > 0 as a word) ? min(N.L, 1) : 0
 *              v = colour * l (r, g, b); sum_pos += v > 0 ? v : 0,
 *              sum_neg += -v > 0 ? -v : 0 (as words, -v by r44), six sums a group
 *   begin j+1: L and the colour from the arrays, N.L = N.x*L.x, + N.y*L.y, + N.z*L.z
 * It only reads the local store. A and B share 256-bit registers as above; the
 * last iteration runs in the lifted loop, from the 23 registers it carries.
 * GOWA_EDGE_NATIVE bit 64. */

LT_INL __m256 lt_ld2(const uint8_t* ls, uint32_t a, uint32_t b) { return lt_pair(lt_ls_rd(ls, a), lt_ls_rd(ls, b)); }
/* x where its word is above 0 as a signed integer, else 0 (and with cgti) */
LT_INL __m256 lt_pos(__m256 x)
{
    const __m256i v = _mm256_castps_si256(x);
    return _mm256_castsi256_ps(_mm256_and_si256(v, _mm256_cmpgt_epi32(v, _mm256_setzero_si256())));
}

static volatile long long s_l2_calls, s_l2_iters, s_l2_skipped;
static void light2_stats(void)
{
    fprintf(stderr, "[edge-native] light 7ED8: %lld calls, %lld native iterations, %lld calls lifted\n",
            s_l2_calls, s_l2_iters, s_l2_skipped);
}

void gowa_edge_light_7ed8(spu_context* ctx)
{
    static int on = -1, stats = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || (atoi(e) & 64) != 0; }
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(light2_stats); }
    if (!on) return;
    const int32_t left = (int32_t)(ctx->gpr[83]._u32[0] - ctx->gpr[32]._u32[0]);
    if (left < 2) return;
    const int iters = left - 1 > 2048 ? 2048 : left - 1;
    const uint32_t a30 = ctx->gpr[30]._u32[0], a31 = ctx->gpr[31]._u32[0], nb = 96u * (uint32_t)iters;
    if ((_mm_getcsr() & LT_MXCSR_RZFZ) != LT_MXCSR_RZFZ || ((a30 | a31) & 15) || !lt_in_ls(a30, nb) || !lt_in_ls(a31, nb)) {
        if (stats) __atomic_add_fetch(&s_l2_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (stats) __atomic_add_fetch(&s_l2_calls, 1, __ATOMIC_RELAXED);
    const uint8_t* ls = ctx->ls;

    const __m256 NXp = lt_pair(lt_ld(ctx, 71), lt_ld(ctx, 70)), NYp = lt_pair(lt_ld(ctx, 69), lt_ld(ctx, 68));
    const __m256 NZp = lt_pair(lt_ld(ctx, 67), lt_ld(ctx, 64));
    const __m256 ONE = lt_dup(lt_ld(ctx, 33));
    const __m256i SGN = _mm256_castps_si256(lt_dup(lt_ld(ctx, 44)));

    __m256 NL = lt_pair(lt_ld(ctx, 12), lt_ld(ctx, 5));
    __m256 CR = lt_pair(lt_ld(ctx, 56), lt_ld(ctx, 53)), CG = lt_pair(lt_ld(ctx, 55), lt_ld(ctx, 52));
    __m256 CB = lt_pair(lt_ld(ctx, 54), lt_ld(ctx, 50));
    __m256 PR = lt_pair(lt_ld(ctx, 46), lt_ld(ctx, 35)), NR = lt_pair(lt_ld(ctx, 39), lt_ld(ctx, 41));
    __m256 PG = lt_pair(lt_ld(ctx, 42), lt_ld(ctx, 45)), NG = lt_pair(lt_ld(ctx, 38), lt_ld(ctx, 40));
    __m256 PB = lt_pair(lt_ld(ctx, 43), lt_ld(ctx, 34)), NB = lt_pair(lt_ld(ctx, 36), lt_ld(ctx, 37));
    __m128i r30 = lt_ld(ctx, 30), r31 = lt_ld(ctx, 31), r32 = lt_ld(ctx, 32);
    uint32_t q30 = a30, q31 = a31;
    int done = 0;

    for (; done < iters; done++) {
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        /* ---- finish light j ---- */
        const __m256 MN = _mm256_blendv_ps(NL, ONE, lt_fcgt(NL, ONE));
        const __m256 L = _mm256_and_ps(MN, _mm256_castsi256_ps(
            _mm256_cmpgt_epi32(_mm256_castps_si256(NL), _mm256_setzero_si256())));
        const __m256 VB = lt_fm(CB, L), VR = lt_fm(CR, L), VG = lt_fm(CG, L);
        const __m256 nVR = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VR), SGN));
        const __m256 nVG = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VG), SGN));
        const __m256 nVB = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VB), SGN));
        PR = lt_fa(PR, lt_pos(VR));
        PG = lt_fa(PG, lt_pos(VG));
        PB = lt_fa(PB, lt_pos(VB));
        NR = lt_fa(NR, lt_pos(nVR));
        NG = lt_fa(NG, lt_pos(nVG));
        NB = lt_fa(NB, lt_pos(nVB));
        /* ---- begin light j+1 ---- */
        const __m256 LX = lt_ld2(ls, q30 + 0x00, q30 + 0x30), LY = lt_ld2(ls, q30 + 0x10, q30 + 0x40);
        const __m256 LZ = lt_ld2(ls, q30 + 0x20, q30 + 0x50);
        CR = lt_ld2(ls, q31 + 0x00, q31 + 0x30);
        CG = lt_ld2(ls, q31 + 0x10, q31 + 0x40);
        CB = lt_ld2(ls, q31 + 0x20, q31 + 0x50);
        NL = lt_fm(NXp, LX);
        NL = lt_fma(NYp, LY, NL);
        NL = lt_fma(NZp, LZ, NL);
        r30 = _mm_add_epi32(r30, _mm_set1_epi32(96));
        r31 = _mm_add_epi32(r31, _mm_set1_epi32(96));
        r32 = _mm_add_epi32(r32, _mm_set1_epi32(1));
        q30 += 96; q31 += 96;
    }
    if (!done) return;
    lt_st(ctx, 12, lt_lo(NL)); lt_st(ctx, 5, lt_hi(NL));
    lt_st(ctx, 56, lt_lo(CR)); lt_st(ctx, 53, lt_hi(CR));
    lt_st(ctx, 55, lt_lo(CG)); lt_st(ctx, 52, lt_hi(CG));
    lt_st(ctx, 54, lt_lo(CB)); lt_st(ctx, 50, lt_hi(CB));
    lt_st(ctx, 46, lt_lo(PR)); lt_st(ctx, 35, lt_hi(PR));
    lt_st(ctx, 39, lt_lo(NR)); lt_st(ctx, 41, lt_hi(NR));
    lt_st(ctx, 42, lt_lo(PG)); lt_st(ctx, 45, lt_hi(PG));
    lt_st(ctx, 38, lt_lo(NG)); lt_st(ctx, 40, lt_hi(NG));
    lt_st(ctx, 43, lt_lo(PB)); lt_st(ctx, 34, lt_hi(PB));
    lt_st(ctx, 36, lt_lo(NB)); lt_st(ctx, 37, lt_hi(NB));
    lt_st(ctx, 30, r30); lt_st(ctx, 31, r31); lt_st(ctx, 32, r32);
    if (stats) {
        const long long it = __atomic_add_fetch(&s_l2_iters, done, __ATOMIC_RELAXED);
        if ((it >> 20) != ((it - done) >> 20)) light2_stats();
    }
}

#else
void gowa_edge_light_7bb4(spu_context* ctx) { (void)ctx; }
void gowa_edge_light_7ed8(spu_context* ctx) { (void)ctx; }
#endif
