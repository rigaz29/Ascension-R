/*
 * The SPU's float forms on eight lanes for the native EDGE routines (spu_edge_cull.c,
 * spu_edge_lightpt.c), as spu_edge_light.c has its own: native where every lane is below FLT_MAX in
 * magnitude, with the +0 rule (an SPU zero is +0); else the runtime's exact forms on both halves. The
 * estimates (frest, frsqest, fi) are the runtime's bit-field rules, exact in every lane. Callers run
 * under the SPU code's MXCSR (toward zero, FTZ, DAZ) and check it first (EC_MXCSR_RZFZ).
 */
#ifndef GOWA_SPU_EDGE_SIMD_H
#define GOWA_SPU_EDGE_SIMD_H

#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdint.h>

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
static volatile long long s_ec_exact;   /* per including file */
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

EC_COLD __m256 ec_fs_x(__m256 a, __m256 b)
{
    EC_EXACT();
    return ec_join(spu_fs(ec_u(ec_lo(a)), ec_u(ec_lo(b))), spu_fs(ec_u(ec_hi(a)), ec_u(ec_hi(b))));
}
EC_INL __m256 ec_fs(__m256 a, __m256 b)              /* a - b */
{
    const __m256 r = _mm256_sub_ps(a, b);
    return __builtin_expect(ec_ok(r), 1) ? ec_pz(r) : ec_fs_x(a, b);
}
EC_COLD __m256 ec_fms_x(__m256 a, __m256 b, __m256 c)
{
    EC_EXACT();
    return ec_join(spu_fms(ec_u(ec_lo(a)), ec_u(ec_lo(b)), ec_u(ec_lo(c))),
                   spu_fms(ec_u(ec_hi(a)), ec_u(ec_hi(b)), ec_u(ec_hi(c))));
}
EC_INL __m256 ec_fms(__m256 a, __m256 b, __m256 c)   /* a*b - c */
{
    const __m256 r = _mm256_fmsub_ps(a, b, c);
    return __builtin_expect(ec_ok(r), 1) ? ec_pz(r) : ec_fms_x(a, b, c);
}
/* cflts with scale 0 (i8 173): truncation toward zero, the SPU's saturation. Native where every
 * lane's biased exponent is below 158 (|x| < 2^31; exponent 0 reads as zero under DAZ). */
EC_COLD __m256i ec_cflts0_x(__m256 x)
{
    EC_EXACT();
    return _mm256_castps_si256(ec_join(spu_cflts(ec_u(ec_lo(x)), 173), spu_cflts(ec_u(ec_hi(x)), 173)));
}
EC_INL __m256i ec_cflts0(__m256 x)
{
    const __m256i e = _mm256_and_si256(_mm256_srli_epi32(_mm256_castps_si256(x), 23), _mm256_set1_epi32(0xFF));
    if (__builtin_expect(!_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(e, _mm256_set1_epi32(157)))), 1))
        return _mm256_cvttps_epi32(x);
    return ec_cflts0_x(x);
}
EC_INL __m256i ec_frsqest(__m256i x)
{
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x3F));
    const __m256i frac = _mm256_and_si256(ec_lut(spu_frsqest_fraction_lut, 8, idx), _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(190), _mm256_srli_epi32(_mm256_add_epi32(exp, _mm256_set1_epi32(1)), 1));
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac);
}

#endif
#endif
