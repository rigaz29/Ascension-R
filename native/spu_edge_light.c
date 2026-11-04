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
#include "spu_edge_verify.h"
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

/* MXCSR around a fast pass, ordered against its ops by the values they carry (as
 * spu_edge_simd.h's ec_fl_*: the compiler takes the ops for pure and may move them across a
 * plain _mm_getcsr/_mm_setcsr) */
LT_INL void lt_fl_set(unsigned csr) { __asm__ volatile("ldmxcsr %0" : : "m"(csr) : "memory"); }
LT_INL unsigned lt_fl_read(void)
{
    unsigned v;
    __asm__ volatile("stmxcsr %0" : "=m"(v) : : "memory");
    return v;
}
#define LT_FL_KEEP(v) __asm__ volatile("" : "+x"(v))
#define LT_FL_USE(v) __asm__ volatile("" : : "x"(v))
/* GOWA_EDGE_VERIFY=1: each fast call checked against the exact pass from the same start */
static int lt_verify_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GOWA_EDGE_VERIFY"); v = e && e[0] == '1'; }
    return v;
}
static volatile long long s_lt_vcalls, s_lt_vbad, s_l2_vcalls, s_l2_vbad;
static void lt_verify_report(void)
{
    fprintf(stderr, "[edge-verify] light 7BB4: %lld fast calls checked, %lld different; light 7ED8: %lld checked, %lld different\n",
            s_lt_vcalls, s_lt_vbad, s_l2_vcalls, s_l2_vbad);
}
static int lt_same256(__m256 a, __m256 b)
{
    return _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_castps_si256(a), _mm256_castps_si256(b))) == -1;
}

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

/* finish light j: its direction and colour (stored for 0x7ED8), the sums */
LT_INL void lt7_finish(const int f, uint8_t* ls, const lt7_k* k, lt7_st* s)
{
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
    s->r46 = _mm_add_epi32(s->r46, _mm_set1_epi32(1));
    s->r23 = _mm_add_epi32(s->r23, _mm_set1_epi32(96));
    s->r29 = _mm_add_epi32(s->r29, _mm_set1_epi32(96));
    s->w23 += 96; s->w29 += 96;
}

/* begin light j+1: its position, colour and direction (bad: the quadwords' exponent-255 lanes) */
LT_INL void lt7_begin(const int f, const uint8_t* ls, const lt7_k* k, lt7_st* s, __m128i* bad)
{
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
    s->q99 += 16; s->q97 += 16; s->q98 += 16;
}

/* n iterations (fewer when the fast pass meets an interrupt); f: fast (bad collects the
 * light quadwords' exponent-255 lanes) or exact */
LT_INL int lt7_run(const int f, spu_context* ctx, uint8_t* ls, const lt7_k* k, lt7_st* s, int n, __m128i* bad)
{
    int done = 0;
    for (; done < n; done++) {
        if (f && ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        lt7_finish(f, ls, k, s);
        lt7_begin(f, ls, k, s, bad);
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
    lt_fl_set(csr & ~0x3Fu);
    LT_FL_KEEP(s.SQ); LT_FL_KEEP(s.RS); LT_FL_KEEP(s.DX); LT_FL_KEEP(s.DY); LT_FL_KEEP(s.DZ);
    LT_FL_KEEP(s.WS); LT_FL_KEEP(s.AX); LT_FL_KEEP(s.AY); LT_FL_KEEP(s.AZ);
    LT_FL_KEEP(s.K); LT_FL_KEEP(s.QX); LT_FL_KEEP(s.QY); LT_FL_KEEP(s.QZ);
    LT_FL_KEEP(s.CX); LT_FL_KEEP(s.CY); LT_FL_KEEP(s.CZ); LT_FL_KEEP(s.CW);
    const int done = lt7_run(1, ctx, ls, &k, &s, iters, &bad);
    LT_FL_USE(s.SQ); LT_FL_USE(s.RS); LT_FL_USE(s.DX); LT_FL_USE(s.DY); LT_FL_USE(s.DZ);
    LT_FL_USE(s.WS); LT_FL_USE(s.AX); LT_FL_USE(s.AY); LT_FL_USE(s.AZ);
    LT_FL_USE(s.CX); LT_FL_USE(s.CY); LT_FL_USE(s.CZ); LT_FL_USE(s.CW);
    const unsigned flags = lt_fl_read();
    lt_fl_set(csr);
    if (!done) return;
    static int force = -1;                                   /* GOWA_EDGE_EXACT=1: always (tests the exact pass) */
    if (force < 0) { const char* e = getenv("GOWA_EDGE_EXACT"); force = e && e[0] == '1'; }
    if (force || (flags & 0x09) || !_mm_testz_si128(bad, bad)) {     /* invalid or overflow, or exponent 255 */
        s = s0;
        lt7_run(0, ctx, ls, &k, &s, done, NULL);
        if (stats) __atomic_add_fetch(&s_lt_redone, 1, __ATOMIC_RELAXED);
    } else if (lt_verify_on()) {
        /* the exact pass from the same start; it stays (stores and registers) */
        static int once;
        if (!once) { once = 1; atexit(lt_verify_report); }
        static SPU_THREAD_LOCAL uint8_t* keep;
        const uint32_t nb = 96u * (uint32_t)done;
        if (!keep) keep = (uint8_t*)malloc(2 * 96 * 4096);
        if (keep) {
            memcpy(keep, ls + o23, nb);
            memcpy(keep + nb, ls + o29, nb);
            const lt7_st f = s;
            s = s0;
            lt7_run(0, ctx, ls, &k, &s, done, NULL);
            const __m256 fv[] = { f.SQ, f.RS, f.DX, f.DY, f.DZ, f.WS, f.AX, f.AY, f.AZ };
            const __m256 ev[] = { s.SQ, s.RS, s.DX, s.DY, s.DZ, s.WS, s.AX, s.AY, s.AZ };
            int same = !memcmp(keep, ls + o23, nb) && !memcmp(keep + nb, ls + o29, nb) &&
                       !memcmp(&f.K, &s.K, 16) && !memcmp(&f.CX, &s.CX, 16) && !memcmp(&f.CY, &s.CY, 16) &&
                       !memcmp(&f.CZ, &s.CZ, 16) && !memcmp(&f.CW, &s.CW, 16) && f.param == s.param;
            for (int i = 0; i < 9; i++) same = same && lt_same256(lt_pz(fv[i]), ev[i]);
            __atomic_add_fetch(&s_lt_vcalls, 1, __ATOMIC_RELAXED);
            if (!same && __atomic_add_fetch(&s_lt_vbad, 1, __ATOMIC_RELAXED) <= 4)
                fprintf(stderr, "[edge-verify] light 7BB4: the fast pass differs (%d iterations)\n", done);
        }
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

/* finish light j: the six sums */
LT_INL void lt8_finish(const int f, const lt8_k* k, lt8_st* s)
{
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
}

/* begin light j+1: its direction and colour from 0x7BB4's arrays, N.L */
LT_INL void lt8_begin(const int f, const uint8_t* ls, const lt8_k* k, lt8_st* s, __m256i* bad)
{
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

/* as lt7_run: n iterations, fast (bad collects the loaded quadwords' exponent-255 lanes) or exact */
LT_INL int lt8_run(const int f, spu_context* ctx, const uint8_t* ls, const lt8_k* k, lt8_st* s, int n, __m256i* bad)
{
    int done = 0;
    for (; done < n; done++) {
        if (f && ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        lt8_finish(f, k, s);
        lt8_begin(f, ls, k, s, bad);
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
    lt_fl_set(csr & ~0x3Fu);
    LT_FL_KEEP(s.NL); LT_FL_KEEP(s.CR); LT_FL_KEEP(s.CG); LT_FL_KEEP(s.CB);
    LT_FL_KEEP(s.PR); LT_FL_KEEP(s.NR); LT_FL_KEEP(s.PG); LT_FL_KEEP(s.NG); LT_FL_KEEP(s.PB); LT_FL_KEEP(s.NB);
    const int done = lt8_run(1, ctx, ls, &k, &s, iters, &bad);
    LT_FL_USE(s.NL); LT_FL_USE(s.PR); LT_FL_USE(s.NR); LT_FL_USE(s.PG); LT_FL_USE(s.NG);
    LT_FL_USE(s.PB); LT_FL_USE(s.NB);
    const unsigned flags = lt_fl_read();
    lt_fl_set(csr);
    if (!done) return;
    static int force = -1;                                   /* GOWA_EDGE_EXACT=1: always (tests the exact pass) */
    if (force < 0) { const char* e = getenv("GOWA_EDGE_EXACT"); force = e && e[0] == '1'; }
    if (force || (flags & 0x09) || !_mm256_testz_si256(bad, bad)) {
        s = s0;
        lt8_run(0, ctx, ls, &k, &s, done, NULL);
        if (stats) __atomic_add_fetch(&s_l2_redone, 1, __ATOMIC_RELAXED);
    } else if (lt_verify_on()) {
        static int once;
        if (!once) { once = 1; atexit(lt_verify_report); }
        const lt8_st f = s;
        s = s0;
        lt8_run(0, ctx, ls, &k, &s, done, NULL);
        const __m256 fv[] = { f.NL, f.CR, f.CG, f.CB, f.PR, f.NR, f.PG, f.NG, f.PB, f.NB };
        const __m256 ev[] = { s.NL, s.CR, s.CG, s.CB, s.PR, s.NR, s.PG, s.NG, s.PB, s.NB };
        int same = 1;
        for (int i = 0; i < 10; i++) same = same && lt_same256(i >= 1 && i <= 3 ? fv[i] : lt_pz(fv[i]), ev[i]);
        __atomic_add_fetch(&s_l2_vcalls, 1, __ATOMIC_RELAXED);
        if (!same && __atomic_add_fetch(&s_l2_vbad, 1, __ATOMIC_RELAXED) <= 4)
            fprintf(stderr, "[edge-verify] light 7ED8: the fast pass differs (%d iterations)\n", done);
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

/* ---------------------------------------------------------------------------
 * The outer loop, LS 0x79B0, around the two above (GOWA_EDGE_NATIVE bit 2048). A pass takes
 * eight vertices, those of the index entries at r103 (word 3: the offset):
 *  - positions at r77 + offset, normals at r76 + offset, vertex k in lane k (0-3 group A, the
 *    low half; 4-7 group B);
 *  - light 0 begun as 0x7BB4 begins a light (its parameter r100 > 0), the sum of weights at
 *    1e-4 and the summed directions at 0; then 0x7BB4's loop over the r83 lights;
 *  - r = fi(WS, frest(WS)), 1/WS = fma(fnms(r, WS, 1), r, r); the light position
 *    P = fma(A, 1/WS, V); the direction D = A * fi(d2, frsqest(d2)) with
 *    d2 = fma(Az, Az, fma(Ay, Ay, fma(Ax, Ax, eps)));
 *  - N.L of light 0 from 0x7BB4's arrays (r94, r95), the sums at 0; then 0x7ED8's loop;
 *  - per vertex, in this order: (NR, NG, NB, w) at r73 + offset, w being what the SPU's
 *    transpose leaves there (NR[3], NG[3], 0, 0 in each group), (Px, Py, Pz, Pz) at r75 +
 *    offset, (PR, PG, PB, PB) at r74 + offset.
 * (Mapped by simulating the lifted statements with every word a label.)
 *
 * The loop carries r103 (+128 a pass) and r114 (+8); everything else it reads is set before it.
 * This takes every pass but the last, which the lifted loop runs: the registers end as it
 * leaves them, and the arrays of 0x7BB4 too. Each pass runs fast first (one flag read a pass)
 * and again exact when the flags or an exponent 255 show, before anything of it is stored but
 * 0x7BB4's arrays, which must then lie apart from all a pass reads (else the pass runs exact).
 * GOWA_EDGE_VERIFY=1 checks the whole stage, 0x73E0, against the stage with this off. */

/* the two tables and the LS quadword the loop reads, as the job module has them */
static const uint32_t lo_tab[12][4] = {
    { 0x00010203u, 0x18191A1Bu, 0x04050607u, 0x10111213u },   /* r108 (F870) */
    { 0x18191A1Bu, 0x10111213u, 0x00010203u, 0x04050607u },   /* r109 (F880) */
    { 0x04050607u, 0x08090A0Bu, 0x18191A1Bu, 0x14151617u },   /* r110 (F890) */
    { 0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu },   /* r104 (F8A0) */
    { 0x0C0D0E0Fu, 0x00010203u, 0x10111213u, 0x1C1D1E1Fu },   /* r118 (F8B0) */
    { 0x04050607u, 0x08090A0Bu, 0x14151617u, 0x18191A1Bu },   /* r119 (F8C0) */
    { 0x04050607u, 0x18191A1Bu, 0x0C0D0E0Fu, 0x80808080u },   /* r120 (F8D0) */
    { 0x1C1D1E1Fu, 0x00010203u, 0x0C0D0E0Fu, 0x80808080u },   /* r121 (F8E0) */
    { 0x0C0D0E0Fu, 0x08090A0Bu, 0x14151617u, 0x10111213u },   /* r111 (F8F0) */
    { 0x08090A0Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x18191A1Bu },   /* r112 (F900) */
    { 0x04050607u, 0x00010203u, 0x10111213u, 0x14151617u },   /* r113 (F910) */
    { 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu },   /* LS F920, r3 each pass */
};
static const int lo_tab_reg[11] = { 108, 109, 110, 104, 118, 119, 120, 121, 111, 112, 113 };

LT_INL __m256i lt_frest(__m256i x)      /* the runtime's frest bit-field rule (spu_frest_ref) */
{
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x1F));
    const __m256i frac = _mm256_and_si256(lt_lut(spu_frest_fraction_lut, 4, idx), _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(253), exp);
    rexp = _mm256_andnot_si256(_mm256_cmpgt_epi32(exp, _mm256_set1_epi32(252)), rexp);
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_and_si256(x, _mm256_set1_epi32((int)0x80000000u)),
                           _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac));
}
/* eight quadwords: words 0-2 of each as three eight-lane vectors (lane j: quadword j) */
LT_INL void lt_soa3(const __m128i q[8], __m256* x, __m256* y, __m256* z)
{
    const __m256 r0 = lt_pair(q[0], q[4]), r1 = lt_pair(q[1], q[5]);
    const __m256 r2 = lt_pair(q[2], q[6]), r3 = lt_pair(q[3], q[7]);
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    *x = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
    *y = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
    *z = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
}
/* the reverse, four vectors: quadword j = lane j of each */
LT_INL void lt_aos4(const __m256 in[4], __m128i q[8])
{
    const __m256 u0 = _mm256_unpacklo_ps(in[0], in[1]), u1 = _mm256_unpackhi_ps(in[0], in[1]);
    const __m256 v0 = _mm256_unpacklo_ps(in[2], in[3]), v1 = _mm256_unpackhi_ps(in[2], in[3]);
    const __m256 w0 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 w1 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(3, 2, 3, 2));
    const __m256 w2 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 w3 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(3, 2, 3, 2));
    q[0] = lt_lo(w0); q[1] = lt_lo(w1); q[2] = lt_lo(w2); q[3] = lt_lo(w3);
    q[4] = lt_hi(w0); q[5] = lt_hi(w1); q[6] = lt_hi(w2); q[7] = lt_hi(w3);
}

typedef struct {
    lt7_k k7;                /* ONE EPS HALF THREE TWO SIGN r93 r100 (the vertices set a pass) */
    lt8_k k8;                /* ONE SGN (the direction set a pass) */
    uint32_t pos, nrm, lp99, lp97, lp98, s23, s29, s94, s95, o73, o75, o74;
    int32_t L;               /* the lights (r83; none when not above 0) */
} lo_k;

/* a pass up to its stores: P (x y z), the positive sums (r g b), the negative (r g b); fast
 * (bad7, bad8 collect exponent-255 lanes) or exact */
LT_INL void lo_pass(const int f, uint8_t* ls, const lo_k* K, const uint32_t off[8], __m256 out[9],
                    __m128i* bad7, __m256i* bad8)
{
    __m128i p[8], n[8];
    for (int j = 0; j < 8; j++) {
        p[j] = lt_ls_rd(ls, K->pos + off[j]);
        n[j] = lt_ls_rd(ls, K->nrm + off[j]);
    }
    lt7_k k = K->k7;
    lt_soa3(p, &k.VX, &k.VY, &k.VZ);
    lt_soa3(n, &k.NX, &k.NY, &k.NZ);
    if (f)
        *bad8 = _mm256_or_si256(*bad8, _mm256_or_si256(
            _mm256_or_si256(lt_e255(k.VX), _mm256_or_si256(lt_e255(k.VY), lt_e255(k.VZ))),
            _mm256_or_si256(lt_e255(k.NX), _mm256_or_si256(lt_e255(k.NY), lt_e255(k.NZ)))));
    lt7_st s;
    memset(&s, 0, sizeof s);
    s.WS = _mm256_castsi256_ps(_mm256_set1_epi32(0x38D1B717));     /* 1e-4 */
    s.w23 = K->s23; s.w29 = K->s29; s.q99 = K->lp99; s.q97 = K->lp97; s.q98 = K->lp98;
    if (K->L > 0) {
        lt7_begin(f, ls, &k, &s, bad7);                               /* light 0 */
        for (int32_t j = 0; j < K->L; j++) {
            lt7_finish(f, ls, &k, &s);
            if (j + 1 < K->L) lt7_begin(f, ls, &k, &s, bad7);
        }
    }
    const __m256 WS = o_pz(f, s.WS);                                  /* frest and fi read its bits */
    const __m256i wsi = _mm256_castps_si256(WS);
    const __m256 r = _mm256_castsi256_ps(lt_fi(wsi, lt_frest(wsi)));
    const __m256 inv = o_fma(f, o_fnms(f, r, WS, k.ONE), r, r);
    out[0] = o_fma(f, s.AX, inv, k.VX);
    out[1] = o_fma(f, s.AY, inv, k.VY);
    out[2] = o_fma(f, s.AZ, inv, k.VZ);
    for (int i = 3; i < 9; i++) out[i] = _mm256_setzero_ps();
    if (K->L <= 0) return;
    const __m256 d2 = o_pz(f, o_fma(f, s.AZ, s.AZ, o_fma(f, s.AY, s.AY, o_fma(f, s.AX, s.AX, k.EPS))));
    const __m256 rs = lt_rsqrt(d2);
    lt8_k k8 = K->k8;
    k8.NX = o_fm(f, s.AX, rs);
    k8.NY = o_fm(f, s.AY, rs);
    k8.NZ = o_fm(f, s.AZ, rs);
    const __m256 LX = lt_ld2(ls, K->s94 + 0x00, K->s94 + 0x30), LY = lt_ld2(ls, K->s94 + 0x10, K->s94 + 0x40);
    const __m256 LZ = lt_ld2(ls, K->s94 + 0x20, K->s94 + 0x50);
    lt8_st t;
    memset(&t, 0, sizeof t);
    t.CR = lt_ld2(ls, K->s95 + 0x00, K->s95 + 0x30);
    t.CG = lt_ld2(ls, K->s95 + 0x10, K->s95 + 0x40);
    t.CB = lt_ld2(ls, K->s95 + 0x20, K->s95 + 0x50);
    if (f)
        *bad8 = _mm256_or_si256(*bad8, _mm256_or_si256(
            _mm256_or_si256(lt_e255(LX), _mm256_or_si256(lt_e255(LY), lt_e255(LZ))),
            _mm256_or_si256(lt_e255(t.CR), _mm256_or_si256(lt_e255(t.CG), lt_e255(t.CB)))));
    t.NL = o_fma(f, k8.NZ, LZ, o_fma(f, k8.NY, LY, o_fm(f, k8.NX, LX)));
    t.q30 = K->s23 + 96; t.q31 = K->s29 + 96;
    for (int32_t j = 0; j < K->L; j++) {
        lt8_finish(f, &k8, &t);
        if (j + 1 < K->L) lt8_begin(f, ls, &k8, &t, bad8);
    }
    out[3] = t.PR; out[4] = t.PG; out[5] = t.PB;
    out[6] = t.NR; out[7] = t.NG; out[8] = t.NB;
}

/* a pass's stores, +0 applied, in the lifted order */
LT_INL void lo_store(uint8_t* ls, const lo_k* K, const uint32_t off[8], const __m256 o[9])
{
    const __m256 PX = lt_pz(o[0]), PY = lt_pz(o[1]), PZ = lt_pz(o[2]);
    const __m256 PR = lt_pz(o[3]), PG = lt_pz(o[4]), PB = lt_pz(o[5]);
    const __m256 NR = lt_pz(o[6]), NG = lt_pz(o[7]), NB = lt_pz(o[8]);
    /* r73's fourth words: NR[3], NG[3], 0, 0 in each group */
    const __m256 W = _mm256_blend_ps(_mm256_blend_ps(_mm256_permute_ps(NR, 0x03), _mm256_permute_ps(NG, 0x0C), 0x22),
                                     _mm256_setzero_ps(), 0xCC);
    const __m256 a73[4] = { NR, NG, NB, W }, a75[4] = { PX, PY, PZ, PZ }, a74[4] = { PR, PG, PB, PB };
    __m128i q[8];
    lt_aos4(a73, q);
    for (int j = 0; j < 8; j++) lt_ls_wr(ls, K->o73 + off[j], q[j]);
    lt_aos4(a75, q);
    for (int j = 0; j < 8; j++) lt_ls_wr(ls, K->o75 + off[j], q[j]);
    lt_aos4(a74, q);
    for (int j = 0; j < 8; j++) lt_ls_wr(ls, K->o74 + off[j], q[j]);
}

/* [a, a + n) inside the local store, apart from [b, b + m) (taken modulo the local store) */
static int lo_apart(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    b &= SPU_LS_SIZE - 1;
    return b + m <= SPU_LS_SIZE ? !lt_overlap(a, n, b, m) : (!lt_overlap(a, n, b, SPU_LS_SIZE - b) && !lt_overlap(a, n, 0, b + m - SPU_LS_SIZE));
}

static volatile long long s_lo_calls, s_lo_passes, s_lo_skipped, s_lo_redone, s_lo_exact;
static void lo_stats(void)
{
    fprintf(stderr, "[edge-native] light 79B0: %lld calls, %lld native passes, %lld calls lifted, %lld passes redone exact, %lld passes exact (apart test)\n",
            s_lo_calls, s_lo_passes, s_lo_skipped, s_lo_redone, s_lo_exact);
}
static SPU_THREAD_LOCAL spu_context* t_lo_ref;   /* GOWA_EDGE_VERIFY's reference run: off */
static int lo_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GOWA_EDGE_NATIVE"); on = !e || (atoi(e) & 2048) != 0; }
    return on;
}

void gowa_edge_light_79b0(spu_context* ctx)
{
    static int stats = -1;
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") ? 1 : 0; if (stats) atexit(lo_stats); }
    if (!lo_on() || t_lo_ref == ctx) return;
    /* the passes left: the loop is do-while, r114 += 8 a pass while r107 > r114 */
    const uint32_t r107 = ctx->gpr[107]._u32[0], r114 = ctx->gpr[114]._u32[0];
    if (r107 <= r114 || r107 - r114 > 0x40000u) return;
    const uint32_t n = (r107 - r114 + 7) >> 3;
    if (n < 2) return;
    const uint32_t m = n - 1;
    uint8_t* ls = ctx->ls;
    const int32_t L = (int32_t)ctx->gpr[83]._u32[0], n100 = (int32_t)ctx->gpr[100]._u32[0];
    int ok = (_mm_getcsr() & LT_MXCSR_RZFZ) == LT_MXCSR_RZFZ && L <= 4096 &&
             ctx->gpr[116]._u32[0] == (L > 0 ? 0xFFFFFFFFu : 0u) &&
             lt_all(ctx, 117, n100 > 0 ? ~0u : 0u, n100 > 0 ? ~0u : 0u, n100 > 0 ? ~0u : 0u, n100 > 0 ? ~0u : 0u) &&
             lt_all(ctx, 85, ~0u, ~0u, 0, 0) && lt_all(ctx, 80, 0, ~0u, ~0u, 0) &&
             lt_all(ctx, 96, 0, 0, ~0u, ~0u) && lt_all(ctx, 123, ~0u, 0, ~0u, 0) &&
             lt_all(ctx, 59, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u) &&
             lt_all(ctx, 82, 0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u) &&
             lt_all(ctx, 81, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu) &&
             lt_all(ctx, 78, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu);
    for (int i = 0; ok && i < 11; i++)
        ok = lt_all(ctx, lo_tab_reg[i], lo_tab[i][0], lo_tab[i][1], lo_tab[i][2], lo_tab[i][3]);
    if (ok) {
        const u128 f920 = lt_u(lt_ls_rd(ls, 0xF920));
        ok = !memcmp(f920._u32, lo_tab[11], 16);
    }
    if (!ok) {
        if (stats) __atomic_add_fetch(&s_lo_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (stats) __atomic_add_fetch(&s_lo_calls, 1, __ATOMIC_RELAXED);

    lo_k K;
    memset(&K, 0, sizeof K);
    K.k7.ONE = lt_dup(lt_ld(ctx, 33)); K.k7.EPS = lt_dup(lt_ld(ctx, 84)); K.k7.HALF = lt_dup(lt_ld(ctx, 72));
    K.k7.THREE = lt_dup(lt_ld(ctx, 101)); K.k7.TWO = lt_dup(lt_ld(ctx, 102)); K.k7.SIGN = lt_dup(lt_ld(ctx, 44));
    K.k7.r93 = ctx->gpr[93];
    K.k7.r93m = _mm_castsi128_ps(lt_m(K.k7.r93));
    K.k7.n100 = n100;
    K.k8.ONE = K.k7.ONE;
    K.k8.SGN = _mm256_castps_si256(K.k7.SIGN);
    K.pos = ctx->gpr[77]._u32[0]; K.nrm = ctx->gpr[76]._u32[0];
    K.lp99 = ctx->gpr[99]._u32[0]; K.lp97 = ctx->gpr[97]._u32[0]; K.lp98 = ctx->gpr[98]._u32[0];
    K.s23 = ctx->gpr[105]._u32[0]; K.s29 = ctx->gpr[106]._u32[0];
    K.s94 = ctx->gpr[94]._u32[0]; K.s95 = ctx->gpr[95]._u32[0];
    K.o73 = ctx->gpr[73]._u32[0]; K.o75 = ctx->gpr[75]._u32[0]; K.o74 = ctx->gpr[74]._u32[0];
    K.L = L > 0 ? L : 0;
    uint32_t idx = ctx->gpr[103]._u32[0];

    /* the fast pass's redo starts the pass over, so 0x7BB4's arrays (written in it) must lie
     * apart from what it reads: the constants (all in range), the lights, the index entries
     * (here), the vertices (each pass) */
    const uint32_t sn = 96u * (uint32_t)K.L, ln = 16u * (uint32_t)K.L;
    const __m256 kv[] = { K.k7.ONE, K.k7.EPS, K.k7.HALF, K.k7.THREE, K.k7.TWO };
    __m256i kb = _mm256_inserti128_si256(_mm256_castsi128_si256(lt_e255q(lt_m(K.k7.r93))), _mm_setzero_si128(), 1);
    for (unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++) kb = _mm256_or_si256(kb, lt_e255(kv[i]));
    static int force = -1;
    if (force < 0) { const char* e = getenv("GOWA_EDGE_EXACT"); force = e && e[0] == '1'; }
    const int fast_call = !force && _mm256_testz_si256(kb, kb) &&
        K.s23 + sn <= SPU_LS_SIZE && K.s29 + sn <= SPU_LS_SIZE && (K.s23 & 15) == 0 && (K.s29 & 15) == 0 &&
        lo_apart(K.s23, sn, K.lp99, ln) && lo_apart(K.s23, sn, K.lp97, ln) && lo_apart(K.s23, sn, K.lp98, ln) &&
        lo_apart(K.s29, sn, K.lp99, ln) && lo_apart(K.s29, sn, K.lp97, ln) && lo_apart(K.s29, sn, K.lp98, ln) &&
        lo_apart(K.s23, sn, idx, 128u * m) && lo_apart(K.s29, sn, idx, 128u * m);

    const unsigned csr = _mm_getcsr();
    uint32_t done = 0;
    for (; done < m; done++, idx += 128) {
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) break;
        uint32_t off[8];
        for (int j = 0; j < 8; j++) off[j] = (uint32_t)_mm_extract_epi32(lt_ls_rd(ls, idx + 16u * (uint32_t)j), 3);
        int fast = fast_call;
        for (int j = 0; fast && j < 8; j++)
            fast = lo_apart(K.s23, sn, K.pos + off[j], 16) && lo_apart(K.s29, sn, K.pos + off[j], 16) &&
                   lo_apart(K.s23, sn, K.nrm + off[j], 16) && lo_apart(K.s29, sn, K.nrm + off[j], 16);
        if (fast_call && !fast && stats) __atomic_add_fetch(&s_lo_exact, 1, __ATOMIC_RELAXED);
        __m256 out[9];
        if (fast) {
            __m128i b7 = _mm_setzero_si128();
            __m256i b8 = _mm256_setzero_si256();
            lt_fl_set(csr & ~0x3Fu);
            lo_pass(1, ls, &K, off, out, &b7, &b8);
            for (int i = 0; i < 9; i++) LT_FL_USE(out[i]);
            const unsigned fl = lt_fl_read();
            lt_fl_set(csr);
            if ((fl & 0x09) || !_mm_testz_si128(b7, b7) || !_mm256_testz_si256(b8, b8)) {
                fast = 0;
                if (stats) __atomic_add_fetch(&s_lo_redone, 1, __ATOMIC_RELAXED);
            }
        }
        if (!fast) lo_pass(0, ls, &K, off, out, NULL, NULL);
        lo_store(ls, &K, off, out);
    }
    if (!done) return;
    for (int i = 0; i < 4; i++) {
        ctx->gpr[103]._u32[i] += 128u * done;
        ctx->gpr[114]._u32[i] += 8u * done;
    }
    if (stats) __atomic_add_fetch(&s_lo_passes, (long long)done, __ATOMIC_RELAXED);
}

/* GOWA_EDGE_VERIFY=1 with the outer loop's routine on: the whole stage (0x73E0) on a copy with
 * it against the stage on the real context without it (spu_edge_verify.h); its callees use no
 * channel, so the copy runs it alone */
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_l73_guard;
static edge_vstat s_vl73 = { "lighting 73E0 (outer loop 79B0 native)", 0, 0, 0, 0, 0, 0, 0, 1 };
static void l73_report(void) { edge_vreport(&s_vl73); }
static void l73_lifted(spu_context* ctx)
{
    t_lo_ref = ctx;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0(ctx);
    t_lo_ref = NULL;
}
static int l73_native(spu_context* c)
{
    const uint32_t link = c->gpr[0]._u32[0];
    t_l73_guard = c;
    c->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0(c);
    spu_drain_call(c, link);
    c->host_depth--;
    t_l73_guard = NULL;
    return 1;
}
int gowa_edge_light_73e0(spu_context* ctx)
{
    if (t_l73_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) { on = edge_verify_on() && lo_on(); if (on) atexit(l73_report); }
    if (!on) return 0;
    return edge_verify(ctx, &s_vl73, l73_native, l73_lifted, &t_l73_guard);
}

/* The lighting stage's set-up (0x73E0, GOWA_EDGE_NATIVE bit 262144): the loop of the buffer
 * clear it calls (0x5D90's 0x5DA8), and its two loops before the outer loop, 0x75D8 and 0x7670.
 * Integer work only: 16-byte loads and stores at computed addresses, made here in the lifted
 * order with the same SPU helpers. A loop is run up to its last pass, which the lifted loop then
 * makes from the registers as it would have them, so the stage ends with exactly the lifted
 * registers. A pending interrupt stops a loop before its next pass: the lifted code takes it at
 * the loop's label, as it would have. GOWA_EDGE_VERIFY=1 checks all three with the stage
 * (gowa_edge_light_73e0): its reference run (t_lo_ref) has them off. */
static volatile long long s_ls_zero, s_ls_zero_passes, s_ls_scat, s_ls_scat_passes, s_ls_sum, s_ls_sum_passes;
static volatile long long s_ls_irq;
static int s_ls_stats;
static void ls_report(void)
{
    fprintf(stderr, "[edge-native] light set-up: clear 5DA8 %lld calls, %lld passes; scatter 75D8 %lld calls, "
            "%lld passes; prefix sum 7670 %lld calls, %lld passes; %lld stops for an interrupt\n",
            s_ls_zero, s_ls_zero_passes, s_ls_scat, s_ls_scat_passes, s_ls_sum, s_ls_sum_passes, s_ls_irq);
}
static int ls_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 262144) != 0;
        s_ls_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        if (on && s_ls_stats) atexit(ls_report);
    }
    return on;
}
#define LS_COUNT(v, n) do { if (s_ls_stats) __atomic_add_fetch(&(v), (long long)(n), __ATOMIC_RELAXED); } while (0)
/* the lifted code's check at a loop's label */
#define LS_IRQ(ctx) ((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask))

/* 0x5DA8, the loop of 0x5D90 (r3 the buffer, r4 its quadwords): stores r5 (zero) at 8
 * quadwords a pass, r6 counting them up by 8, while r4 > r6. */
void gowa_edge_lzero_5da8(spu_context* ctx)
{
    if (!ls_on() || t_lo_ref == ctx) return;
    const u128 r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    u128 r3 = ctx->gpr[3], r6 = ctx->gpr[6], r7 = ctx->gpr[7];
    uint32_t passes = 0;
    /* a pass that leaves r4 > r6 (signed) is not the last */
    while ((int32_t)r4._u32[0] > (int32_t)(r6._u32[0] + 8u) && passes < 0x10000u) {
        if (LS_IRQ(ctx)) { LS_COUNT(s_ls_irq, 1); break; }
        r6 = spu_ai(r6, 8);
        const uint32_t a = r3._u32[0];
        for (uint32_t o = 0; o < 0x80u; o += 0x10u) spu_ls_write128(ctx, a + o, r5);
        r7 = spu_cgt(r4, r6);
        r3 = spu_ai(r3, 128);
        passes++;
    }
    if (!passes) return;
    ctx->gpr[3] = r3; ctx->gpr[6] = r6; ctx->gpr[7] = r7;
    LS_COUNT(s_ls_zero, 1);
    LS_COUNT(s_ls_zero_passes, passes);
}

/* 0x75D8: for each 16-bit index of a list, the quadword r10 at r81 + index * 16. Software
 * pipelined: a pass loads the list's next quadword (r80 + r14) and stores at the eight offsets
 * the pass before left in r8, r9, r11, r12 (that quadword shifted left 4 bits, and rotated by
 * 4, 8, 12 bytes), as is and by roti 16. r13 counts the passes down to 0. */
void gowa_edge_lscat_75d8(spu_context* ctx)
{
    if (!ls_on() || t_lo_ref == ctx) return;
    const uint32_t n = ctx->gpr[13]._u32[0];
    if (n < 2u || n > 0x10000u) return;
    const uint32_t b80 = ctx->gpr[80]._u32[0], b81 = ctx->gpr[81]._u32[0];
    const u128 r10 = ctx->gpr[10];
    u128 r8 = ctx->gpr[8], r9 = ctx->gpr[9], r11 = ctx->gpr[11], r12 = ctx->gpr[12];
    u128 r13 = ctx->gpr[13], r14 = ctx->gpr[14];
    uint32_t k = 1;
    for (; k < n; k++) {
        if (LS_IRQ(ctx)) { LS_COUNT(s_ls_irq, 1); break; }
        const u128 r43 = spu_roti(r8, 16);
        const u128 r42 = spu_ls_read128(ctx, b80 + r14._u32[0]);
        const u128 r41 = spu_roti(r9, 16);
        spu_ls_write128(ctx, b81 + r8._u32[0], r10);
        const u128 r40 = spu_roti(r11, 16);
        spu_ls_write128(ctx, b81 + r9._u32[0], r10);
        const u128 r39 = spu_roti(r12, 16);
        spu_ls_write128(ctx, b81 + r11._u32[0], r10);
        r14 = spu_ai(r14, 16);
        spu_ls_write128(ctx, b81 + r12._u32[0], r10);
        r13 = spu_ai(r13, -1);
        spu_ls_write128(ctx, b81 + r43._u32[0], r10);
        r8 = spu_shlqbii(r42, 4);
        spu_ls_write128(ctx, b81 + r41._u32[0], r10);
        spu_ls_write128(ctx, b81 + r40._u32[0], r10);
        spu_ls_write128(ctx, b81 + r39._u32[0], r10);
        r9 = spu_rotqbyi(r8, 4);
        r11 = spu_rotqbyi(r8, 8);
        r12 = spu_rotqbyi(r8, 12);
    }
    if (k == 1) return;
    ctx->gpr[8] = r8; ctx->gpr[9] = r9; ctx->gpr[11] = r11; ctx->gpr[12] = r12;
    ctx->gpr[13] = r13; ctx->gpr[14] = r14;
    LS_COUNT(s_ls_scat, 1);
    LS_COUNT(s_ls_scat_passes, k - 1u);
}

/* 0x7670: a running sum r11 of four count tables read side by side (r16-r19 + r9, a quadword
 * each a pass), and before each addition the counter for that table (r15, r14, r13, r12, +64
 * a pass) stored at r81 + the sum. r9 = r10 counts up by 64 from below 0; the pass that takes
 * it to 0 or above is the last. */
void gowa_edge_lsum_7670(spu_context* ctx)
{
    if (!ls_on() || t_lo_ref == ctx) return;
    u128 r9 = ctx->gpr[9], r10 = ctx->gpr[10], r11 = ctx->gpr[11];
    u128 r12 = ctx->gpr[12], r13 = ctx->gpr[13], r14 = ctx->gpr[14], r15 = ctx->gpr[15];
    const uint32_t a16 = ctx->gpr[16]._u32[0], a17 = ctx->gpr[17]._u32[0];
    const uint32_t a18 = ctx->gpr[18]._u32[0], a19 = ctx->gpr[19]._u32[0];
    const uint32_t b81 = ctx->gpr[81]._u32[0];
    uint32_t passes = 0;
    while ((int32_t)(r10._u32[0] + 64u) <= -1 && passes < 0x10000u) {   /* another pass follows */
        if (LS_IRQ(ctx)) { LS_COUNT(s_ls_irq, 1); break; }
        const u128 r67 = spu_ai(r10, 64);
        const u128 r66 = spu_ls_read128(ctx, a19 + r9._u32[0]);
        const u128 r65 = spu_ai(r15, 64);
        const u128 r63 = spu_ls_read128(ctx, a18 + r9._u32[0]);
        const u128 r61 = spu_ai(r14, 64);
        const u128 r59 = spu_ls_read128(ctx, a17 + r9._u32[0]);
        const u128 r57 = spu_ai(r13, 64);
        const u128 r55 = spu_ls_read128(ctx, a16 + r9._u32[0]);
        const u128 r53 = spu_ai(r12, 64);
        spu_ls_write128(ctx, b81 + r11._u32[0], r15);
        const u128 r62 = spu_a(r11, r66);
        const u128 r58 = spu_a(r62, r63);
        spu_ls_write128(ctx, b81 + r62._u32[0], r14);
        const u128 r54 = spu_a(r58, r59);
        spu_ls_write128(ctx, b81 + r58._u32[0], r13);
        const u128 r2 = spu_a(r54, r55);
        spu_ls_write128(ctx, b81 + r54._u32[0], r12);
        r15 = r65; r10 = r67; r9 = r67; r14 = r61; r13 = r57; r12 = r53; r11 = r2;
        passes++;
    }
    if (!passes) return;
    ctx->gpr[9] = r9; ctx->gpr[10] = r10; ctx->gpr[11] = r11;
    ctx->gpr[12] = r12; ctx->gpr[13] = r13; ctx->gpr[14] = r14; ctx->gpr[15] = r15;
    if (s_ls_stats) {
        LS_COUNT(s_ls_sum_passes, passes);
        /* also every 65536 calls: a game run is ended by a kill, so atexit never comes */
        if (!(__atomic_add_fetch(&s_ls_sum, 1, __ATOMIC_RELAXED) & 0xFFFF)) ls_report();
    }
}

#else
void gowa_edge_lzero_5da8(spu_context* ctx) { (void)ctx; }
void gowa_edge_lscat_75d8(spu_context* ctx) { (void)ctx; }
void gowa_edge_lsum_7670(spu_context* ctx) { (void)ctx; }
void gowa_edge_light_7bb4(spu_context* ctx) { (void)ctx; }
void gowa_edge_light_7ed8(spu_context* ctx) { (void)ctx; }
void gowa_edge_light_79b0(spu_context* ctx) { (void)ctx; }
int gowa_edge_light_73e0(spu_context* ctx) { (void)ctx; return 0; }
#endif
