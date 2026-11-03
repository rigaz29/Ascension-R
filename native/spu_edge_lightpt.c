/*
 * EDGE's per-vertex lighting, point lights (job module e8f44fbc6dbf, 0x6AC8; lighting's
 * 0x73E0 calls it once a lit job): GOWA_EDGE_NATIVE bit 256, off by default while it is new.
 *
 * The arguments:
 *  - r3: the colours;  r4: the positions;  r6: the index list;  r7: the count;
 *  - r8: the light table (its first quadword: the offsets of the lights' positions and parameters);
 *  - r9 / r10: the first light and the end;
 *  - r11: bit 8 starts the sums at 0.5 (else 0);
 *  - r12: its low byte keeps the old colour (blend 1.0, else 0.0);
 *  - r13: the intensity.
 * r3's result is not used.
 *
 * Eight vertices a pass, gathered through the index list (each entry's word 3 is the offset of
 * the vertex's position and colour). Each pass reads all eight positions and old colours, then:
 *  - for each light (position x, y, z, 1/radius; C = intensity * parameters):
 *    - d2 = fma(dz, dz, fma(dy, dy, dx * dx)) with d = light - vertex;
 *    - y = fi(d2, frsqest(d2)); y' = fma(1 - (y d2) y, y / 2, y); distance = y' d2;
 *    - t = distance / radius, clamped to 1; a = 1 - t;
 *    - s = (a^2 (3 - 2a))^2;
 *    - sum = fma(s C.w, C, sum);
 *  - colour = fma(old colour, blend, sum), stored vertex 0 to 7.
 * A last pass past the count is done whole, as the lifted loop does.
 *
 * This takes the eight vertices as eight lanes, each channel's sum as its own vector, with the SPU's
 * exact float forms: per element the same operations in the same order as the lifted code.
 * Declined (lifted instead):
 *  - another MXCSR;
 *  - no lights (first == end: another path, a tail call).
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

static volatile long long s_pt_calls, s_pt_verts, s_pt_declined;

/* eight quadwords (host-order words) as four eight-lane vectors: lane j = quadword j's word k */
EC_INL void lp_soa(const __m128i q[8], __m256 out[4])
{
    const __m256 r0 = ec_pair(q[0], q[4]), r1 = ec_pair(q[1], q[5]);
    const __m256 r2 = ec_pair(q[2], q[6]), r3 = ec_pair(q[3], q[7]);
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    out[0] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
    out[1] = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
    out[2] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
    out[3] = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(3, 2, 3, 2));
}
/* the reverse: lane j of the four vectors back into quadword j */
EC_INL void lp_aos(const __m256 in[4], __m128i q[8])
{
    const __m256 u0 = _mm256_unpacklo_ps(in[0], in[1]), u1 = _mm256_unpackhi_ps(in[0], in[1]);
    const __m256 v0 = _mm256_unpacklo_ps(in[2], in[3]), v1 = _mm256_unpackhi_ps(in[2], in[3]);
    const __m256 w0 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 w1 = _mm256_shuffle_ps(u0, v0, _MM_SHUFFLE(3, 2, 3, 2));
    const __m256 w2 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 w3 = _mm256_shuffle_ps(u1, v1, _MM_SHUFFLE(3, 2, 3, 2));
    q[0] = ec_lo(w0); q[1] = ec_lo(w1); q[2] = ec_lo(w2); q[3] = ec_lo(w3);
    q[4] = ec_hi(w0); q[5] = ec_hi(w1); q[6] = ec_hi(w2); q[7] = ec_hi(w3);
}

static int pt_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return 0;
    const int32_t first = (int32_t)ctx->gpr[9]._u32[0], end = (int32_t)ctx->gpr[10]._u32[0];
    if (first == end) return 0;
    const int32_t count = (int32_t)ctx->gpr[7]._u32[0];
    if (count > 0x10000) return 0;
    uint8_t* const ls = ctx->ls;
    /* the lifted prologue's saves (r80-r85, r126, r1 below the caller's r1), before any test:
     * the local store ends the same, the frame included */
    const uint32_t sp = ctx->gpr[1]._u32[0];
    for (int r = 80; r <= 85; r++) ec_stq(ls, sp - 0x10u * (uint32_t)(r - 79), ec_m(ctx->gpr[r]));
    ec_stq(ls, sp - 0x70, ec_m(ctx->gpr[126]));
    ec_stq(ls, sp - 0x90, ec_m(ctx->gpr[1]));
    if (count <= 0) return 1;                      /* the lifted function returns here */
    const uint32_t colors = ctx->gpr[3]._u32[0], pos = ctx->gpr[4]._u32[0], idx = ctx->gpr[6]._u32[0];
    const uint32_t lt = ctx->gpr[8]._u32[0];
    const u128 inten = ctx->gpr[13];
    const __m128i lt0 = ec_ldq(ls, lt);
    const uint32_t lpos = lt + (uint32_t)_mm_extract_epi32(lt0, 0), lpar = lt + (uint32_t)_mm_extract_epi32(lt0, 1);
    const __m256 init = (ctx->gpr[11]._u32[0] & 256) ? _mm256_set1_ps(0.5f) : _mm256_setzero_ps();
    const __m256 blend = (ctx->gpr[12]._u32[0] & 0xFF) ? _mm256_set1_ps(1.0f) : _mm256_setzero_ps();
    const __m256 one = ec_set(0x3F800000u), half = ec_set(0x3F000000u), two = ec_set(0x40000000u), three = ec_set(0x40400000u);
    const int32_t lights = end > first ? end - first : 0;

    for (int32_t done = 0; done < count; done += 8) {
        uint32_t off[8];
        __m128i p[8], c[8];
        for (int j = 0; j < 8; j++)
            off[j] = (uint32_t)_mm_extract_epi32(ec_ldq(ls, idx + 128u * (uint32_t)(done >> 3) + 16u * (uint32_t)j), 3);
        for (int j = 0; j < 8; j++) {
            p[j] = ec_ldq(ls, pos + off[j]);
            c[j] = ec_ldq(ls, colors + off[j]);
        }
        __m256 P[4], acc[4];
        lp_soa(p, P);
        acc[0] = acc[1] = acc[2] = acc[3] = init;
        for (int32_t i = 0; i < lights; i++) {
            const uint32_t li = 16u * (uint32_t)(first + i);
            const u128 L = ec_u(ec_ldq(ls, lpos + li));
            const u128 C = spu_fm(inten, ec_u(ec_ldq(ls, lpar + li)));
            const __m256 dx = ec_fs(ec_set(L._u32[0]), P[0]);
            const __m256 dy = ec_fs(ec_set(L._u32[1]), P[1]);
            const __m256 dz = ec_fs(ec_set(L._u32[2]), P[2]);
            const __m256 d2 = ec_fma(dz, dz, ec_fma(dy, dy, ec_fm(dx, dx)));
            const __m256i d2i = _mm256_castps_si256(d2);
            const __m256 y = _mm256_castsi256_ps(ec_fi(d2i, ec_frsqest(d2i)));
            const __m256 y2 = ec_fma(ec_fnms(ec_fm(y, d2), y, one), ec_fm(y, half), y);
            const __m256 t = ec_fm(ec_fm(y2, d2), ec_set(L._u32[3]));
            const __m256 tc = _mm256_blendv_ps(t, one, ec_fcgt(t, one));
            const __m256 a = ec_fs(one, tc);
            const __m256 s = ec_fm(ec_fm(a, a), ec_fnms(two, a, three));
            const __m256 att = ec_fm(ec_fm(s, s), ec_set(C._u32[3]));
            for (int k = 0; k < 4; k++) acc[k] = ec_fma(att, ec_set(C._u32[k]), acc[k]);
        }
        __m256 old[4], out[4];
        lp_soa(c, old);
        for (int k = 0; k < 4; k++) out[k] = ec_fma(old[k], blend, acc[k]);
        __m128i q[8];
        lp_aos(out, q);
        for (int j = 0; j < 8; j++) ec_stq(ls, colors + off[j], q[j]);
    }
    __atomic_add_fetch(&s_pt_verts, (long long)count, __ATOMIC_RELAXED);
    return 1;
}

static void pt_stats(void)
{
    fprintf(stderr, "[edge-native] point lights 6AC8: %lld calls native, %lld vertices, %lld declined, %lld exact ops\n",
            s_pt_calls, s_pt_verts, s_pt_declined, s_ec_exact);
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_00006AC8(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_pt_guard;
static edge_vstat s_vpt = { "point lights 6AC8", 0, 0, 0, 0, 0, 0, 0 };
static void pt_verify_report(void) { edge_vreport(&s_vpt); }

/* The hook at 0x6AC8's entry: 1 when the call is done (the function returns). */
int gowa_edge_lightpt_6ac8(spu_context* ctx)
{
    if (t_pt_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = e && (atoi(e) & 256) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(pt_stats);
        if (on && edge_verify_on()) atexit(pt_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vpt, pt_native, jobmod_e8f44fbc6dbf_e6D98_spu_func_00006AC8, &t_pt_guard);
    if (pt_native(ctx)) {
        __atomic_add_fetch(&s_pt_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_pt_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

#else
int gowa_edge_lightpt_6ac8(spu_context* ctx) { (void)ctx; return 0; }
#endif
