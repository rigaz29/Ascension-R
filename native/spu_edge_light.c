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
 * DAZ). A call runs fast first, nothing tested per op: an exact result beyond
 * FLT_MAX sets MXCSR's overflow flag, an operand with exponent 255 is looked
 * for in what the ops read; either redoes the call with the exact forms (an op
 * whose result is at or beyond FLT_MAX in a lane done again by spu_fm & co. on
 * both halves, +0 applied), from the same start. The fast pass applies +0 where
 * a value leaves (stores, registers): a zero's sign never changes another
 * result of these ops. frsqest and fi are spu_edge_decode.c's 256-bit ports of the
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

/* any lane with the SPU's exponent 255 (extended range): Inf or NaN to the native ops */
LT_INL __m256i lt_e255(__m256 x)
{
    const __m256i e = _mm256_set1_epi32(0x7F800000);
    return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(x), e), e);
}
LT_INL __m128i lt_e255q(__m128i x)
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_cmpeq_epi32(_mm_and_si128(x, e), e);
}
LT_INL __m128i lt_pzq(__m128 r) { return _mm_castps_si128(_mm_and_ps(r, _mm_cmpneq_ps(r, _mm_setzero_ps()))); }

/* one op, fast (native, nothing tested: the caller looks at MXCSR's flags) or exact (the
 * forms above, +0 applied); f is a constant where these are inlined */
LT_INL __m256 o_fm(int f, __m256 a, __m256 b) { return f ? _mm256_mul_ps(a, b) : lt_fm(a, b); }
LT_INL __m256 o_fa(int f, __m256 a, __m256 b) { return f ? _mm256_add_ps(a, b) : lt_fa(a, b); }
LT_INL __m256 o_fs(int f, __m256 a, __m256 b) { return f ? _mm256_sub_ps(a, b) : lt_fs(a, b); }
LT_INL __m256 o_fma(int f, __m256 a, __m256 b, __m256 c) { return f ? _mm256_fmadd_ps(a, b, c) : lt_fma(a, b, c); }
LT_INL __m256 o_fnms(int f, __m256 a, __m256 b, __m256 c) { return f ? _mm256_fnmadd_ps(a, b, c) : lt_fnms(a, b, c); }
LT_INL __m256 o_fcgt(int f, __m256 a, __m256 b) { return f ? _mm256_cmp_ps(a, b, _CMP_GT_OQ) : lt_fcgt(a, b); }
LT_INL __m256 o_pz(int f, __m256 r) { return f ? lt_pz(r) : r; }

/* 0x7BB4's constants and what it carries */
typedef struct {
    __m256 VX, VY, VZ, NX, NY, NZ, ONE, EPS, HALF, THREE, TWO, SIGN;
    u128 r93;
    __m128 r93m;
    int32_t n100;
} lt7_k;
typedef struct {
    __m256 SQ, RS, DX, DY, DZ, WS, AX, AY, AZ;
    __m128i K, QX, QY, QZ, CX, CY, CZ, CW, r43, r46, r23, r29;
    uint32_t w23, w29, q99, q97, q98;
    int param;
} lt7_st;

/* n iterations (fewer when the fast pass meets an interrupt); f: fast (bad collects the
 * light quadwords' exponent-255 lanes) or exact */
LT_INL int lt7_run(const int f, spu_context* ctx, uint8_t* ls, const lt7_k* k, lt7_st* s, int n, __m128i* bad)
{
    int done = 0;
    for (; done < n; done++) {
        if (f && ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        /* ---- finish light j ---- */
        const __m256 D = o_fm(f, s->SQ, s->RS);
        const __m256 nz = o_fm(f, s->DZ, s->RS), nx = o_fm(f, s->DX, s->RS), ny = o_fm(f, s->DY, s->RS);
        const __m256 DK = o_fm(f, D, lt_dup(s->K));
        const __m256 LX = s->param ? lt_dup(s->QX) : o_pz(f, nx);
        const __m256 LY = s->param ? lt_dup(s->QY) : o_pz(f, ny);
        const __m256 LZ = s->param ? lt_dup(s->QZ) : o_pz(f, nz);
        lt_ls_wr(ls, s->w23 + 0x20, lt_lo(LZ));
        lt_ls_wr(ls, s->w23 + 0x00, lt_lo(LX));
        lt_ls_wr(ls, s->w23 + 0x10, lt_lo(LY));
        lt_ls_wr(ls, s->w23 + 0x30, lt_hi(LX));
        lt_ls_wr(ls, s->w23 + 0x40, lt_hi(LY));
        lt_ls_wr(ls, s->w23 + 0x50, lt_hi(LZ));
        __m256 NL = o_fm(f, k->NX, LX);
        NL = o_fma(f, k->NY, LY, NL);
        NL = o_fma(f, k->NZ, LZ, NL);
        const __m256 M = _mm256_blendv_ps(DK, k->ONE, o_fcgt(f, DK, k->ONE));
        const __m256 T = o_fs(f, k->ONE, M);
        const __m256 S = o_fnms(f, k->TWO, T, k->THREE);
        const __m256 T2 = o_fm(f, T, T);
        const __m256 SM = o_fm(f, T2, S);
        const __m256 SM2 = o_fm(f, SM, SM);
        const __m256 ATT = o_fm(f, SM2, lt_dup(s->CW));
        __m256 H = _mm256_andnot_ps(k->SIGN, o_fma(f, NL, k->HALF, k->HALF));
        H = _mm256_blendv_ps(H, k->ONE, o_fcgt(f, H, k->ONE));
        const __m256 AA = _mm256_andnot_ps(k->SIGN, ATT);
        const __m256 CR = o_pz(f, o_fm(f, lt_dup(s->CX), ATT));
        const __m256 CG = o_pz(f, o_fm(f, lt_dup(s->CY), ATT));
        const __m256 CB = o_pz(f, o_fm(f, lt_dup(s->CZ), ATT));
        const __m256 W = o_fm(f, AA, H);
        lt_ls_wr(ls, s->w29 + 0x20, lt_lo(CB));
        lt_ls_wr(ls, s->w29 + 0x50, lt_hi(CB));
        lt_ls_wr(ls, s->w29 + 0x30, lt_hi(CR));
        lt_ls_wr(ls, s->w29 + 0x00, lt_lo(CR));
        lt_ls_wr(ls, s->w29 + 0x40, lt_hi(CG));
        lt_ls_wr(ls, s->w29 + 0x10, lt_lo(CG));
        s->AZ = o_fma(f, LZ, W, s->AZ);
        s->WS = o_fa(f, s->WS, W);
        s->AX = o_fma(f, LX, W, s->AX);
        s->AY = o_fma(f, LY, W, s->AY);
        /* ---- begin light j+1 ---- */
        const __m128i P = lt_ls_rd(ls, s->q99), C = lt_ls_rd(ls, s->q97), Q = lt_ls_rd(ls, s->q98);
        if (f) *bad = _mm_or_si128(*bad, _mm_or_si128(lt_e255q(P), _mm_or_si128(lt_e255q(C), lt_e255q(Q))));
        s->DX = o_fs(f, lt_dup(_mm_shuffle_epi32(P, 0x00)), k->VX);
        s->DY = o_fs(f, lt_dup(_mm_shuffle_epi32(P, 0x55)), k->VY);
        s->DZ = o_fs(f, lt_dup(_mm_shuffle_epi32(P, 0xAA)), k->VZ);
        __m256 SQ = o_fma(f, s->DX, s->DX, k->EPS);
        SQ = o_fma(f, s->DY, s->DY, SQ);
        SQ = o_fma(f, s->DZ, s->DZ, SQ);
        s->SQ = o_pz(f, SQ);                          /* frsqest and fi read its bits */
        s->RS = lt_rsqrt(s->SQ);
        s->K = _mm_shuffle_epi32(P, 0xFF);
        s->QX = _mm_shuffle_epi32(Q, 0x00);
        s->QY = _mm_shuffle_epi32(Q, 0x55);
        s->QZ = _mm_shuffle_epi32(Q, 0xAA);
        const __m128i C2 = f ? lt_pzq(_mm_mul_ps(k->r93m, _mm_castsi128_ps(C))) : lt_m(spu_fm(k->r93, lt_u(C)));
        s->CX = _mm_shuffle_epi32(C2, 0x00);
        s->CY = _mm_shuffle_epi32(C2, 0x55);
        s->CZ = _mm_shuffle_epi32(C2, 0xAA);
        s->CW = _mm_shuffle_epi32(C2, 0xFF);
        s->param = k->n100 > (int32_t)_mm_cvtsi128_si32(s->r43);
        s->r43 = _mm_add_epi32(s->r43, _mm_set1_epi32(16));
        s->r46 = _mm_add_epi32(s->r46, _mm_set1_epi32(1));
        s->r23 = _mm_add_epi32(s->r23, _mm_set1_epi32(96));
        s->r29 = _mm_add_epi32(s->r29, _mm_set1_epi32(96));
        s->w23 += 96; s->w29 += 96; s->q99 += 16; s->q97 += 16; s->q98 += 16;
    }
    return done;
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

static volatile long long s_lt_calls, s_lt_iters, s_lt_skipped, s_lt_redone;
static void light_stats(void)
{
    fprintf(stderr, "[edge-native] light 7BB4: %lld calls, %lld native iterations, %lld exact ops, %lld calls lifted, %lld redone exact\n",
            s_lt_calls, s_lt_iters, s_lt_exact, s_lt_skipped, s_lt_redone);
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
    lt7_k k;
    k.VX = lt_pair(lt_ld(ctx, 66), lt_ld(ctx, 65)); k.VY = lt_pair(lt_ld(ctx, 62), lt_ld(ctx, 63));
    k.VZ = lt_pair(lt_ld(ctx, 60), lt_ld(ctx, 61));
    k.NX = lt_pair(lt_ld(ctx, 52), lt_ld(ctx, 50)); k.NY = lt_pair(lt_ld(ctx, 54), lt_ld(ctx, 53));
    k.NZ = lt_pair(lt_ld(ctx, 56), lt_ld(ctx, 55));
    k.ONE = lt_dup(lt_ld(ctx, 33)); k.EPS = lt_dup(lt_ld(ctx, 84)); k.HALF = lt_dup(lt_ld(ctx, 72));
    k.THREE = lt_dup(lt_ld(ctx, 101)); k.TWO = lt_dup(lt_ld(ctx, 102)); k.SIGN = lt_dup(lt_ld(ctx, 44));
    k.r93 = ctx->gpr[93];
    k.r93m = _mm_castsi128_ps(lt_m(k.r93));
    k.n100 = (int32_t)ctx->gpr[100]._u32[0];

    /* what the loop carries */
    lt7_st s0;
    s0.SQ = lt_pair(lt_ld(ctx, 25), lt_ld(ctx, 24)); s0.RS = lt_pair(lt_ld(ctx, 10), lt_ld(ctx, 12));
    s0.DX = lt_pair(lt_ld(ctx, 31), lt_ld(ctx, 32)); s0.DY = lt_pair(lt_ld(ctx, 28), lt_ld(ctx, 30));
    s0.DZ = lt_pair(lt_ld(ctx, 26), lt_ld(ctx, 27));
    s0.WS = lt_pair(lt_ld(ctx, 42), lt_ld(ctx, 45)); s0.AX = lt_pair(lt_ld(ctx, 57), lt_ld(ctx, 48));
    s0.AY = lt_pair(lt_ld(ctx, 49), lt_ld(ctx, 58)); s0.AZ = lt_pair(lt_ld(ctx, 51), lt_ld(ctx, 47));
    s0.K = lt_ld(ctx, 39); s0.QX = lt_ld(ctx, 40); s0.QY = lt_ld(ctx, 38); s0.QZ = lt_ld(ctx, 41);
    s0.CX = lt_ld(ctx, 37); s0.CY = lt_ld(ctx, 36); s0.CZ = lt_ld(ctx, 35); s0.CW = lt_ld(ctx, 34);
    s0.r43 = lt_ld(ctx, 43); s0.r46 = lt_ld(ctx, 46); s0.r23 = lt_ld(ctx, 23); s0.r29 = lt_ld(ctx, 29);
    s0.w23 = o23; s0.w29 = o29; s0.q99 = p99; s0.q97 = p97; s0.q98 = p98;
    s0.param = param;

    /* The fast pass: every op native, no range test per op. An op whose exact result is
     * beyond FLT_MAX leaves FLT_MAX under round-toward-zero and sets MXCSR's overflow flag;
     * an operand with the SPU's exponent 255 (extended range, Inf or NaN here) is looked for
     * in every value the ops read (the constants and the carried values here, the three
     * light quadwords each iteration). Either way the whole call runs again with the exact
     * forms, from the same start: the stores of the fast pass go to the two output arrays
     * only, apart from everything read (checked above), and are all written again. A zero's
     * sign never changes a non-zero result of these ops, so +0 is applied where a value
     * leaves: what is stored and what is written back. */
    const unsigned csr = _mm_getcsr();
    __m128i bad = _mm_setzero_si128();
    {
        __m256i b = _mm256_setzero_si256();
        const __m256 vk[] = { k.VX, k.VY, k.VZ, k.NX, k.NY, k.NZ, k.ONE, k.EPS, k.HALF, k.THREE, k.TWO,
                              s0.SQ, s0.DX, s0.DY, s0.DZ, s0.WS, s0.AX, s0.AY, s0.AZ };
        for (unsigned i = 0; i < sizeof vk / sizeof vk[0]; i++) b = _mm256_or_si256(b, lt_e255(vk[i]));
        bad = _mm_or_si128(_mm256_castsi256_si128(b), _mm256_extracti128_si256(b, 1));
        const __m128i vq[] = { s0.K, s0.QX, s0.QY, s0.QZ, s0.CX, s0.CY, s0.CZ, s0.CW, lt_m(k.r93) };
        for (unsigned i = 0; i < sizeof vq / sizeof vq[0]; i++) bad = _mm_or_si128(bad, lt_e255q(vq[i]));
    }
    lt7_st s = s0;
    _mm_setcsr(csr & ~0x3Fu);
    const int done = lt7_run(1, ctx, ls, &k, &s, iters, &bad);
    const unsigned flags = _mm_getcsr();
    _mm_setcsr(csr);
    if (!done) return;
    static int force = -1;                                   /* GOWA_EDGE_EXACT=1: always (tests the exact pass) */
    if (force < 0) { const char* e = getenv("GOWA_EDGE_EXACT"); force = e && e[0] == '1'; }
    if (force || (flags & 0x09) || !_mm_testz_si128(bad, bad)) {     /* invalid or overflow, or exponent 255 */
        s = s0;
        lt7_run(0, ctx, ls, &k, &s, done, NULL);
        if (stats) __atomic_add_fetch(&s_lt_redone, 1, __ATOMIC_RELAXED);
    }
    lt_st(ctx, 25, lt_lo(lt_pz(s.SQ))); lt_st(ctx, 24, lt_hi(lt_pz(s.SQ)));
    lt_st(ctx, 10, lt_lo(s.RS)); lt_st(ctx, 12, lt_hi(s.RS));
    lt_st(ctx, 31, lt_lo(lt_pz(s.DX))); lt_st(ctx, 32, lt_hi(lt_pz(s.DX)));
    lt_st(ctx, 28, lt_lo(lt_pz(s.DY))); lt_st(ctx, 30, lt_hi(lt_pz(s.DY)));
    lt_st(ctx, 26, lt_lo(lt_pz(s.DZ))); lt_st(ctx, 27, lt_hi(lt_pz(s.DZ)));
    lt_st(ctx, 42, lt_lo(lt_pz(s.WS))); lt_st(ctx, 45, lt_hi(lt_pz(s.WS)));
    lt_st(ctx, 57, lt_lo(lt_pz(s.AX))); lt_st(ctx, 48, lt_hi(lt_pz(s.AX)));
    lt_st(ctx, 49, lt_lo(lt_pz(s.AY))); lt_st(ctx, 58, lt_hi(lt_pz(s.AY)));
    lt_st(ctx, 51, lt_lo(lt_pz(s.AZ))); lt_st(ctx, 47, lt_hi(lt_pz(s.AZ)));
    lt_st(ctx, 39, s.K); lt_st(ctx, 40, s.QX); lt_st(ctx, 38, s.QY); lt_st(ctx, 41, s.QZ);
    lt_st(ctx, 37, s.CX); lt_st(ctx, 36, s.CY); lt_st(ctx, 35, s.CZ); lt_st(ctx, 34, s.CW);
    lt_st(ctx, 9, lt_ld(ctx, s.param ? 104 : 64));
    lt_st(ctx, 43, s.r43); lt_st(ctx, 46, s.r46); lt_st(ctx, 23, s.r23); lt_st(ctx, 29, s.r29);
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
 * last iteration runs in the lifted loop, from the 23 registers it carries. Fast
 * first, then exact if needed, as the first loop.
 * GOWA_EDGE_NATIVE bit 64. */

LT_INL __m256 lt_ld2(const uint8_t* ls, uint32_t a, uint32_t b) { return lt_pair(lt_ls_rd(ls, a), lt_ls_rd(ls, b)); }
/* x where its word is above 0 as a signed integer, else 0 (and with cgti) */
LT_INL __m256 lt_pos(__m256 x)
{
    const __m256i v = _mm256_castps_si256(x);
    return _mm256_castsi256_ps(_mm256_and_si256(v, _mm256_cmpgt_epi32(v, _mm256_setzero_si256())));
}

/* 0x7ED8's constants and what it carries */
typedef struct { __m256 NX, NY, NZ, ONE; __m256i SGN; } lt8_k;
typedef struct {
    __m256 NL, CR, CG, CB, PR, NR, PG, NG, PB, NB;
    __m128i r30, r31, r32;
    uint32_t q30, q31;
} lt8_st;

/* as lt7_run: n iterations, fast (bad collects the loaded quadwords' exponent-255 lanes) or exact */
LT_INL int lt8_run(const int f, spu_context* ctx, const uint8_t* ls, const lt8_k* k, lt8_st* s, int n, __m256i* bad)
{
    int done = 0;
    for (; done < n; done++) {
        if (f && ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        /* ---- finish light j ---- */
        const __m256 MN = _mm256_blendv_ps(s->NL, k->ONE, o_fcgt(f, s->NL, k->ONE));
        const __m256 L = _mm256_and_ps(MN, _mm256_castsi256_ps(
            _mm256_cmpgt_epi32(_mm256_castps_si256(s->NL), _mm256_setzero_si256())));
        const __m256 VB = o_fm(f, s->CB, L), VR = o_fm(f, s->CR, L), VG = o_fm(f, s->CG, L);
        const __m256 nVR = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VR), k->SGN));
        const __m256 nVG = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VG), k->SGN));
        const __m256 nVB = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(VB), k->SGN));
        s->PR = o_fa(f, s->PR, lt_pos(VR));
        s->PG = o_fa(f, s->PG, lt_pos(VG));
        s->PB = o_fa(f, s->PB, lt_pos(VB));
        s->NR = o_fa(f, s->NR, lt_pos(nVR));
        s->NG = o_fa(f, s->NG, lt_pos(nVG));
        s->NB = o_fa(f, s->NB, lt_pos(nVB));
        /* ---- begin light j+1 ---- */
        const __m256 LX = lt_ld2(ls, s->q30 + 0x00, s->q30 + 0x30), LY = lt_ld2(ls, s->q30 + 0x10, s->q30 + 0x40);
        const __m256 LZ = lt_ld2(ls, s->q30 + 0x20, s->q30 + 0x50);
        s->CR = lt_ld2(ls, s->q31 + 0x00, s->q31 + 0x30);
        s->CG = lt_ld2(ls, s->q31 + 0x10, s->q31 + 0x40);
        s->CB = lt_ld2(ls, s->q31 + 0x20, s->q31 + 0x50);
        if (f)
            *bad = _mm256_or_si256(*bad, _mm256_or_si256(
                _mm256_or_si256(lt_e255(LX), _mm256_or_si256(lt_e255(LY), lt_e255(LZ))),
                _mm256_or_si256(lt_e255(s->CR), _mm256_or_si256(lt_e255(s->CG), lt_e255(s->CB)))));
        __m256 NL = o_fm(f, k->NX, LX);
        NL = o_fma(f, k->NY, LY, NL);
        s->NL = o_fma(f, k->NZ, LZ, NL);
        s->r30 = _mm_add_epi32(s->r30, _mm_set1_epi32(96));
        s->r31 = _mm_add_epi32(s->r31, _mm_set1_epi32(96));
        s->r32 = _mm_add_epi32(s->r32, _mm_set1_epi32(1));
        s->q30 += 96; s->q31 += 96;
    }
    return done;
}

static volatile long long s_l2_calls, s_l2_iters, s_l2_skipped, s_l2_redone;
static void light2_stats(void)
{
    fprintf(stderr, "[edge-native] light 7ED8: %lld calls, %lld native iterations, %lld calls lifted, %lld redone exact\n",
            s_l2_calls, s_l2_iters, s_l2_skipped, s_l2_redone);
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

    lt8_k k;
    k.NX = lt_pair(lt_ld(ctx, 71), lt_ld(ctx, 70)); k.NY = lt_pair(lt_ld(ctx, 69), lt_ld(ctx, 68));
    k.NZ = lt_pair(lt_ld(ctx, 67), lt_ld(ctx, 64));
    k.ONE = lt_dup(lt_ld(ctx, 33));
    k.SGN = _mm256_castps_si256(lt_dup(lt_ld(ctx, 44)));

    lt8_st s0;
    s0.NL = lt_pair(lt_ld(ctx, 12), lt_ld(ctx, 5));
    s0.CR = lt_pair(lt_ld(ctx, 56), lt_ld(ctx, 53)); s0.CG = lt_pair(lt_ld(ctx, 55), lt_ld(ctx, 52));
    s0.CB = lt_pair(lt_ld(ctx, 54), lt_ld(ctx, 50));
    s0.PR = lt_pair(lt_ld(ctx, 46), lt_ld(ctx, 35)); s0.NR = lt_pair(lt_ld(ctx, 39), lt_ld(ctx, 41));
    s0.PG = lt_pair(lt_ld(ctx, 42), lt_ld(ctx, 45)); s0.NG = lt_pair(lt_ld(ctx, 38), lt_ld(ctx, 40));
    s0.PB = lt_pair(lt_ld(ctx, 43), lt_ld(ctx, 34)); s0.NB = lt_pair(lt_ld(ctx, 36), lt_ld(ctx, 37));
    s0.r30 = lt_ld(ctx, 30); s0.r31 = lt_ld(ctx, 31); s0.r32 = lt_ld(ctx, 32);
    s0.q30 = a30; s0.q31 = a31;

    /* fast first, as 0x7BB4's (it only reads the local store, so a redo just starts over) */
    const unsigned csr = _mm_getcsr();
    __m256i bad = _mm256_setzero_si256();
    {
        const __m256 vk[] = { k.NX, k.NY, k.NZ, k.ONE, s0.NL, s0.CR, s0.CG, s0.CB,
                              s0.PR, s0.NR, s0.PG, s0.NG, s0.PB, s0.NB };
        for (unsigned i = 0; i < sizeof vk / sizeof vk[0]; i++) bad = _mm256_or_si256(bad, lt_e255(vk[i]));
    }
    lt8_st s = s0;
    _mm_setcsr(csr & ~0x3Fu);
    const int done = lt8_run(1, ctx, ls, &k, &s, iters, &bad);
    const unsigned flags = _mm_getcsr();
    _mm_setcsr(csr);
    if (!done) return;
    static int force = -1;                                   /* GOWA_EDGE_EXACT=1: always (tests the exact pass) */
    if (force < 0) { const char* e = getenv("GOWA_EDGE_EXACT"); force = e && e[0] == '1'; }
    if (force || (flags & 0x09) || !_mm256_testz_si256(bad, bad)) {
        s = s0;
        lt8_run(0, ctx, ls, &k, &s, done, NULL);
        if (stats) __atomic_add_fetch(&s_l2_redone, 1, __ATOMIC_RELAXED);
    }
    lt_st(ctx, 12, lt_lo(lt_pz(s.NL))); lt_st(ctx, 5, lt_hi(lt_pz(s.NL)));
    lt_st(ctx, 56, lt_lo(s.CR)); lt_st(ctx, 53, lt_hi(s.CR));
    lt_st(ctx, 55, lt_lo(s.CG)); lt_st(ctx, 52, lt_hi(s.CG));
    lt_st(ctx, 54, lt_lo(s.CB)); lt_st(ctx, 50, lt_hi(s.CB));
    lt_st(ctx, 46, lt_lo(lt_pz(s.PR))); lt_st(ctx, 35, lt_hi(lt_pz(s.PR)));
    lt_st(ctx, 39, lt_lo(lt_pz(s.NR))); lt_st(ctx, 41, lt_hi(lt_pz(s.NR)));
    lt_st(ctx, 42, lt_lo(lt_pz(s.PG))); lt_st(ctx, 45, lt_hi(lt_pz(s.PG)));
    lt_st(ctx, 38, lt_lo(lt_pz(s.NG))); lt_st(ctx, 40, lt_hi(lt_pz(s.NG)));
    lt_st(ctx, 43, lt_lo(lt_pz(s.PB))); lt_st(ctx, 34, lt_hi(lt_pz(s.PB)));
    lt_st(ctx, 36, lt_lo(lt_pz(s.NB))); lt_st(ctx, 37, lt_hi(lt_pz(s.NB)));
    lt_st(ctx, 30, s.r30); lt_st(ctx, 31, s.r31); lt_st(ctx, 32, s.r32);
    if (stats) {
        const long long it = __atomic_add_fetch(&s_l2_iters, done, __ATOMIC_RELAXED);
        if ((it >> 20) != ((it - done) >> 20)) light2_stats();
    }
}

#else
void gowa_edge_light_7bb4(spu_context* ctx) { (void)ctx; }
void gowa_edge_light_7ed8(spu_context* ctx) { (void)ctx; }
#endif
