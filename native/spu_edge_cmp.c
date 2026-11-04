/*
 * EDGE's output compression for the lit jobs' vertices: the position-independent piece of EA
 * 0x755B80 (lifted as pic_00755B80; 0xD158 of the output stage calls it), GOWA_EDGE_NATIVE bit
 * 4096, on unless GOWA_EDGE_NATIVE leaves it out.
 *
 * The arguments:
 *  - r3: the EDGE context c;
 *  - r53: the vertex count n plus one (r4 holds n, which the piece does not read);
 *  - r5: the output (local store);
 *  - r43: the attribute pointer table (c + 0x10, a word an attribute);
 *  - r63: minus the frame's size, -656 (the piece adds it to r1).
 * It finds attributes 1, 3, 4, 27, 28, 29 and 30 in the list of ids at c + 0x160 (the first
 * match each) and takes their arrays: a quadword a vertex, x y z w. It writes 54 bytes a vertex,
 * big-endian:
 *  -  0-11: attribute 1 (the position), x y z copied;
 *  - 12-19: attribute 3 (the normal), x y z w as I16N: the high halfword of
 *    cflts(fma(v, 32767.5, -0.5), 157);
 *  - 20-23: attribute 4 (the tangent), X11Y11Z10 with x in the low bits:
 *    - i = cflts(fma(v, s, b), 142), with s, b = 1023.5/1024, -0.5/1024 for x and y, and
 *      511.5/512, -0.5/512 for z;
 *    - the fields are (i >> 21) & 0x7FF, (i >> 21) & 0x7FF and (i >> 22) & 0x3FF;
 *  - 24-35: attribute 27, x y z copied;
 *  - 36-53: attributes 28, 29 and 30, x y z as F16 by the piece's rule:
 *    - |high halfword| >= 0x4780 (|v| >= 65536, extended values included): 0x7BFF;
 *    - below 0x3800 (exponent < 112): +0, the sign dropped;
 *    - else ((bits - 0x38000000) >> 13) & 0x7FFF;
 *    - with the sign bit copied, but for +0.
 *
 * The piece does two vertices a pass, writing the 8 quadwords from the pass's output rounded
 * down. That leaves junk past the last vertex. For n a multiple of 8 (EDGE's counts, so far
 * every call), with the output 16-aligned, it is one quadword: 4 zero bytes, then the last 12
 * bytes of the output. The rest of what the piece leaves:
 *  - its frame: r84-r117 at r1 - 0x10 * (r - 79), r126 at r1 - 0x270, r1 at r1 - 0x290 (the
 *    entry values);
 *  - r80-r83 loaded from r1 - 0x10 .. r1 - 0x40 (which it does not save);
 *  - r1 + r63 + 656, and r3 = 0;
 *  - the other registers: they do not matter (the strict replay with r2-r79 scrambled after every
 *    call showed no difference).
 * (Mapped from the piece's code and eight probed calls' 3,344 vertices.)
 *
 * The other counts take the vertices up to a multiple of 8 that leaves 8-15, and run the lifted
 * piece on the rest, with its table entries moved past what was done (and put back after).
 *
 * Two vertices a 256-bit register, a fast pass a block of 8 vertices (spu_edge_simd.h), redone
 * exact when MXCSR's flags or an exponent 255 show. Declined (lifted instead):
 *  - another MXCSR;
 *  - fewer than 16 vertices;
 *  - r63 not -656;
 *  - an id missing;
 *  - the output not 16-aligned, or overlapping an array or the frame;
 *  - an array, the ids or the table in the frame.
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

static const uint8_t c7_ids[7] = { 1, 3, 4, 27, 28, 29, 30 };
static volatile long long s_c7_calls, s_c7_verts, s_c7_declined, s_c7_redone, s_c7_tails;

#define C7_FRAME 0x290u

/* the piece's F16 of eight words */
EC_INL __m256i c7_f16(__m256i b)
{
    const __m256i hi = _mm256_srli_epi32(b, 16);
    const __m256i ah = _mm256_and_si256(hi, _mm256_set1_epi32(0x7FFF));
    const __m256i conv = _mm256_and_si256(_mm256_srli_epi32(_mm256_sub_epi32(b, _mm256_set1_epi32(0x38000000)), 13),
                                          _mm256_set1_epi32(0x7FFF));
    const __m256i over = _mm256_cmpgt_epi32(ah, _mm256_set1_epi32(0x477F));
    const __m256i under = _mm256_cmpgt_epi32(_mm256_set1_epi32(0x3800), ah);
    __m256i r = _mm256_blendv_epi8(conv, _mm256_set1_epi32(0x7BFF), over);
    r = _mm256_or_si256(r, _mm256_and_si256(hi, _mm256_set1_epi32(0x8000)));
    return _mm256_andnot_si256(under, r);
}

/* a 128-bit pattern in both lanes */
#define C7_B2(...) _mm256_setr_epi8(__VA_ARGS__, __VA_ARGS__)

/* the bytes 12-23 of two vertices, a lane each: bytes 0-7 the normal's halfwords, 8-11 the
 * tangent's word, big-endian; from the normal's words ni and the tangent's ti */
EC_INL __m256i c7_nt(__m256i ni, __m256i ti)
{
    const __m256i f = _mm256_sllv_epi32(
        _mm256_and_si256(_mm256_srlv_epi32(ti, _mm256_setr_epi32(21, 21, 22, 0, 21, 21, 22, 0)),
                         _mm256_setr_epi32(0x7FF, 0x7FF, 0x3FF, 0, 0x7FF, 0x7FF, 0x3FF, 0)),
        _mm256_setr_epi32(0, 11, 22, 0, 0, 11, 22, 0));
    __m256i h = _mm256_or_si256(f, _mm256_shuffle_epi32(f, _MM_SHUFFLE(1, 0, 3, 2)));
    h = _mm256_or_si256(h, _mm256_shuffle_epi32(h, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm256_or_si256(
        _mm256_shuffle_epi8(ni, C7_B2(3, 2, 7, 6, 11, 10, 15, 14, -1, -1, -1, -1, -1, -1, -1, -1)),
        _mm256_shuffle_epi8(h, C7_B2(-1, -1, -1, -1, -1, -1, -1, -1, 3, 2, 1, 0, -1, -1, -1, -1)));
}

EC_INL __m256i c7_ld(const uint8_t* ls, uint32_t a) { return _mm256_loadu_si256((const __m256i*)(ls + a)); }

/* vertices v and v + 1: their 108 bytes at out + 54 v. f: fast (bad collects the exponent-255
 * lanes it reads), else the runtime's exact forms. */
EC_INL void c7_pair(const int f, uint8_t* ls, const uint32_t lo[7], uint32_t v, uint32_t out, __m256i* bad)
{
    const __m256i bs = C7_B2(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const uint32_t o = 16u * v;
    const __m256i P = c7_ld(ls, lo[0] + o), Q = c7_ld(ls, lo[3] + o);
    const __m256i N = _mm256_shuffle_epi8(c7_ld(ls, lo[1] + o), bs);
    /* the tangent's w is not used: 0 */
    const __m256i T = _mm256_blend_epi32(_mm256_shuffle_epi8(c7_ld(ls, lo[2] + o), bs), _mm256_setzero_si256(), 0x88);
    __m256i ni, ti;
    if (f) {
        *bad = _mm256_or_si256(*bad, _mm256_or_si256(ec_e255(_mm256_castsi256_ps(N)), ec_e255(_mm256_castsi256_ps(T))));
        const __m256 tn = _mm256_fmadd_ps(_mm256_castsi256_ps(N), ec_set(0x46FFFF00u), ec_set(0xBF000000u));
        const __m256 tt = _mm256_fmadd_ps(_mm256_castsi256_ps(T),
            _mm256_castsi256_ps(_mm256_setr_epi32(0x3F7FE000, 0x3F7FE000, 0x3F7FC000, 0, 0x3F7FE000, 0x3F7FE000, 0x3F7FC000, 0)),
            _mm256_castsi256_ps(_mm256_setr_epi32((int)0xBA000000u, (int)0xBA000000u, (int)0xBA800000u, 0,
                                                  (int)0xBA000000u, (int)0xBA000000u, (int)0xBA800000u, 0)));
        ni = _mm256_cvttps_epi32(_mm256_mul_ps(tn, _mm256_set1_ps(65536.0f)));
        ti = _mm256_cvttps_epi32(_mm256_mul_ps(tt, _mm256_set1_ps(2147483648.0f)));
        /* the results go into the flags' read (spu_edge_simd.h's ordering) */
        EC_FL_USE(ni); EC_FL_USE(ti);
    } else {
        u128 ns, nb, ts, tb;
        for (int i = 0; i < 4; i++) { ns._u32[i] = 0x46FFFF00u; nb._u32[i] = 0xBF000000u; }
        ts._u32[0] = ts._u32[1] = 0x3F7FE000u; ts._u32[2] = 0x3F7FC000u; ts._u32[3] = 0;
        tb._u32[0] = tb._u32[1] = 0xBA000000u; tb._u32[2] = 0xBA800000u; tb._u32[3] = 0;
        const u128 n0 = spu_cflts(spu_fma(ec_u(_mm256_castsi256_si128(N)), ns, nb), 157);
        const u128 n1 = spu_cflts(spu_fma(ec_u(_mm256_extracti128_si256(N, 1)), ns, nb), 157);
        const u128 t0 = spu_cflts(spu_fma(ec_u(_mm256_castsi256_si128(T)), ts, tb), 142);
        const u128 t1 = spu_cflts(spu_fma(ec_u(_mm256_extracti128_si256(T, 1)), ts, tb), 142);
        ni = _mm256_castps_si256(ec_join(n0, n1));
        ti = _mm256_castps_si256(ec_join(t0, t1));
    }
    const __m256i F28 = c7_f16(_mm256_shuffle_epi8(c7_ld(ls, lo[4] + o), bs));
    const __m256i F29 = c7_f16(_mm256_shuffle_epi8(c7_ld(ls, lo[5] + o), bs));
    const __m256i F30 = c7_f16(_mm256_shuffle_epi8(c7_ld(ls, lo[6] + o), bs));
    /* the 9 F16 halfwords, big-endian: Hl the first 8, Hh the last */
    const __m256i Hl = _mm256_or_si256(
        _mm256_or_si256(_mm256_shuffle_epi8(F28, C7_B2(1, 0, 5, 4, 9, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)),
                        _mm256_shuffle_epi8(F29, C7_B2(-1, -1, -1, -1, -1, -1, 1, 0, 5, 4, 9, 8, -1, -1, -1, -1))),
        _mm256_shuffle_epi8(F30, C7_B2(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 0, 5, 4)));
    const __m256i Hh = _mm256_shuffle_epi8(F30, C7_B2(9, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1));
    const __m256i NT = c7_nt(ni, ti);
    /* a vertex's bytes 0-15, 16-31, 32-47 and 38-53 */
    const __m256i R0 = _mm256_blend_epi32(P, _mm256_slli_si256(NT, 12), 0x88);
    const __m256i R1 = _mm256_blend_epi32(_mm256_srli_si256(NT, 4), _mm256_slli_si256(Q, 8), 0xCC);
    const __m256i R2 = _mm256_blend_epi32(_mm256_slli_si256(Hl, 4), _mm256_srli_si256(Q, 8), 0x11);
    const __m256i R3 = _mm256_alignr_epi8(Hh, Hl, 2);
    uint8_t* d = ls + out + 54u * v;
    _mm_storeu_si128((__m128i*)d, _mm256_castsi256_si128(R0));
    _mm_storeu_si128((__m128i*)(d + 16), _mm256_castsi256_si128(R1));
    _mm_storeu_si128((__m128i*)(d + 32), _mm256_castsi256_si128(R2));
    _mm_storeu_si128((__m128i*)(d + 38), _mm256_castsi256_si128(R3));
    _mm_storeu_si128((__m128i*)(d + 54), _mm256_extracti128_si256(R0, 1));
    _mm_storeu_si128((__m128i*)(d + 70), _mm256_extracti128_si256(R1, 1));
    _mm_storeu_si128((__m128i*)(d + 86), _mm256_extracti128_si256(R2, 1));
    _mm_storeu_si128((__m128i*)(d + 92), _mm256_extracti128_si256(R3, 1));
}

extern void pic_00755B80_spu_func_00030020(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_c7_guard;

static void c7_lifted(spu_context* ctx)
{
    const uint32_t link = ctx->gpr[0]._u32[0];
    t_c7_guard = ctx;
    ctx->host_depth++;
    pic_00755B80_spu_func_00030020(ctx);
    spu_drain_call(ctx, link);
    ctx->host_depth--;
    t_c7_guard = NULL;
}

static int c7_apart(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a1 <= b0 || b1 <= a0; }
static void c7_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int c7_native(spu_context* ctx)
{
    if ((_mm_getcsr() & EC_MXCSR_RZFZ) != EC_MXCSR_RZFZ) return 0;
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0] & (SPU_LS_SIZE - 16), r53 = ctx->gpr[53]._u32[0], n = r53 - 1;
    const uint32_t out = ctx->gpr[5]._u32[0], tab = ctx->gpr[43]._u32[0];
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if (n < 16 || n > 0x4000 || (out & 15) || out + 54u * n + 32 > SPU_LS_SIZE) return 0;
    if ((tab & 3) || tab + 64 > SPU_LS_SIZE || c + 0x170 > SPU_LS_SIZE) return 0;
    if (ctx->gpr[63]._u32[0] != (uint32_t)-656 || (sp & 15) || sp < C7_FRAME || sp > SPU_LS_SIZE) return 0;
    const uint32_t f0 = sp - C7_FRAME, oend = out + 54u * n + 32;
    if (!c7_apart(out, oend, f0, sp) || !c7_apart(c, c + 0x170, f0, sp) || !c7_apart(tab, tab + 64, f0, sp)) return 0;
    /* the attributes: the first match of each id, as the piece's ceqbi / gbb / clz */
    uint32_t idx[7], base[7], lo[7];
    for (int a = 0; a < 7; a++) {
        idx[a] = 16;
        for (uint32_t k = 0; k < 16; k++)
            if (ls[c + 0x160 + k] == c7_ids[a]) { idx[a] = k; break; }
        if (idx[a] == 16) return 0;
        base[a] = ec_be32(ls + tab + 4 * idx[a]);
        lo[a] = base[a] & (SPU_LS_SIZE - 16);
        /* the arrays (and the pipeline's reads past them) inside, apart from the output and the frame */
        const uint32_t len = 16u * (n + 4);
        if (lo[a] + len > SPU_LS_SIZE || !c7_apart(lo[a], lo[a] + len, out, oend) || !c7_apart(lo[a], lo[a] + len, f0, sp))
            return 0;
    }
    /* n a multiple of 8: all here; else up to a multiple of 8 that leaves 8-15 for the lifted piece */
    const int full = (n & 7) == 0;
    const uint32_t m = full ? n : (n - 8) & ~7u;

    const unsigned csr = _mm_getcsr();
    const int force = ec_force_exact();
    ec_fl_clear(csr);
    for (uint32_t v0 = 0; v0 < m; v0 += 8) {
        int fast = !force;
        if (fast) {
            /* a redo writes every byte of the block again; its reads are apart from them */
            __m256i bad = _mm256_setzero_si256();
            for (uint32_t p = 0; p < 8; p += 2) c7_pair(1, ls, lo, v0 + p, out, &bad);
            if ((ec_fl_read() & EC_FLAGS_BAD) || !_mm256_testz_si256(bad, bad)) {
                fast = 0;
                __atomic_add_fetch(&s_c7_redone, 1, __ATOMIC_RELAXED);
            }
        }
        if (!fast) {
            for (uint32_t p = 0; p < 8; p += 2) c7_pair(0, ls, lo, v0 + p, out, NULL);
            ec_fl_clear(csr);
        }
    }
    ec_fl_set(csr);
    __atomic_add_fetch(&s_c7_verts, (long long)m, __ATOMIC_RELAXED);

    if (full) {
        /* the junk quadword past the last vertex */
        uint8_t* e = ls + out + 54u * n;
        uint8_t q[16] = { 0 };
        memcpy(q + 4, e - 12, 12);
        memcpy(e, q, 16);
        /* the frame, the restores, r1 and the result */
        for (int r = 84; r <= 117; r++) ec_stq(ls, sp - 0x10u * (uint32_t)(r - 79), ec_m(ctx->gpr[r]));
        ec_stq(ls, sp - 0x270u, ec_m(ctx->gpr[126]));
        ec_stq(ls, sp - 0x290u, ec_m(ctx->gpr[1]));
        for (int k = 0; k < 4; k++) ctx->gpr[80 + k] = ec_u(ec_ldq(ls, sp - 0x10u * (uint32_t)(k + 1)));
        for (int k = 0; k < 4; k++) ctx->gpr[1]._u32[k] += ctx->gpr[63]._u32[k] + 656u;
        for (int k = 0; k < 4; k++) ctx->gpr[3]._u32[k] = 0;
        return 1;
    }

    /* the rest by the lifted piece, its table entries moved past what was done */
    for (int a = 0; a < 7; a++) c7_put32(ls + tab + 4 * idx[a], base[a] + 16u * m);
    ctx->gpr[4]._u32[0] -= m;
    ctx->gpr[53]._u32[0] = r53 - m;
    ctx->gpr[5]._u32[0] = out + 54u * m;
    c7_lifted(ctx);
    for (int a = 0; a < 7; a++) c7_put32(ls + tab + 4 * idx[a], base[a]);
    __atomic_add_fetch(&s_c7_tails, 1, __ATOMIC_RELAXED);
    return 1;
}

static void c7_stats(void)
{
    fprintf(stderr, "[edge-native] compression 755B80: %lld calls native (%lld with the lifted piece on the last vertices), "
            "%lld vertices, %lld declined, %lld blocks redone exact\n",
            s_c7_calls, s_c7_tails, s_c7_verts, s_c7_declined, s_c7_redone);
}
static edge_vstat s_vc7 = { "compression 755B80", 0, 1, 0, 0, 0, 0, 0, 0 };
static void c7_verify_report(void) { edge_vreport(&s_vc7); }

/* The hook at the piece's entry: 1 when the call is done (the function returns). */
int gowa_edge_cmp_755b80(spu_context* ctx)
{
    if (t_c7_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 4096) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(c7_stats);
        if (on && edge_verify_on()) atexit(c7_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vc7, c7_native, pic_00755B80_spu_func_00030020, &t_c7_guard);
    if (c7_native(ctx)) {
        __atomic_add_fetch(&s_c7_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_c7_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

/*
 * The position-only output: the piece of EA 0x765B70 (lifted as pic_00765B70, entry 0x30000;
 * called from 0xD1E0), GOWA_EDGE_NATIVE bit 8192.
 *  - r3: the EDGE context c; r4: the vertex count n; r5: the output.
 *  - It takes attribute 1 (the first match of id 1 in the list at c + 0x160, its array from the
 *    table at c + 0x10) and copies x y z of (n + 1) & ~1 vertices, 12 bytes each.
 *  - A pass does two vertices in three stores from its start rounded down: the bytes before
 *    the start are kept (the previous pass's), and past the last vertex the last pass leaves
 *    24 - ph bytes (ph = its start & 15) of: 8 zero bytes, the last vertex's y z, 8 zero bytes.
 *    For n a multiple of 4 with the output 16-aligned, ph is 8: one quadword, 8 zero bytes and
 *    the output's last 8.
 *  - r3 = 0 after. No frame, no register above r44 touched; the others do not matter (strict
 *    replay with r2-r79 scrambled after every call: no difference).
 * (Mapped from the piece's code, its one constant the identity pattern; the whole local store
 * after the call as predicted in 12 of 12 probed calls.) Declined, lifted instead:
 *  - n not a multiple of 4 (none seen: every count a multiple of 8), fewer than 4, or the output
 *    not 16-aligned;
 *  - c not 16-aligned, or no id 1;
 *  - the array or the context overlapping the output.
 */
extern void pic_00765B70_spu_func_00030000(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_c8_guard;
static volatile long long s_c8_calls, s_c8_verts, s_c8_declined;

static int c8_native(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], n = ctx->gpr[4]._u32[0], out = ctx->gpr[5]._u32[0];
    if ((c & 15) || c + 0x170 > SPU_LS_SIZE) return 0;
    if (n < 4 || n > 0x4000 || (n & 3) || (out & 15)) return 0;
    const uint32_t end = out + 12u * n;
    if (end + 16 > SPU_LS_SIZE) return 0;
    uint32_t idx = 16;
    for (uint32_t k = 0; k < 16; k++)
        if (ls[c + 0x160 + k] == 1) { idx = k; break; }
    if (idx == 16) return 0;
    /* the array and the piece's reads past it (6 vertices ahead) */
    const uint32_t lo = ec_be32(ls + c + 0x10 + 4 * idx) & (SPU_LS_SIZE - 16), len = 16u * (n + 8);
    if (lo + len > SPU_LS_SIZE || !c7_apart(lo, lo + len, out, end + 16) || !c7_apart(c, c + 0x170, out, end + 16))
        return 0;
    /* each store's last 4 bytes are the next vertex's first, written after */
    for (uint32_t v = 0; v < n; v++)
        _mm_storeu_si128((__m128i*)(ls + out + 12u * v), _mm_loadu_si128((const __m128i*)(ls + lo + 16u * v)));
    uint8_t q[16] = { 0 };
    memcpy(q + 8, ls + end - 8, 8);
    memcpy(ls + end, q, 16);
    for (int k = 0; k < 4; k++) ctx->gpr[3]._u32[k] = 0;
    __atomic_add_fetch(&s_c8_verts, (long long)n, __ATOMIC_RELAXED);
    return 1;
}

static void c8_stats(void)
{
    fprintf(stderr, "[edge-native] position output 765B70: %lld calls native, %lld vertices, %lld declined\n",
            s_c8_calls, s_c8_verts, s_c8_declined);
}
static edge_vstat s_vc8 = { "position output 765B70", 0, 1, 0, 0, 0, 0, 0, 0 };
static void c8_verify_report(void) { edge_vreport(&s_vc8); }

int gowa_edge_pos_765b70(spu_context* ctx)
{
    if (t_c8_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 8192) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(c8_stats);
        if (on && edge_verify_on()) atexit(c8_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vc8, c8_native, pic_00765B70_spu_func_00030000, &t_c8_guard);
    if (c8_native(ctx)) {
        __atomic_add_fetch(&s_c8_calls, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_add_fetch(&s_c8_declined, 1, __ATOMIC_RELAXED);
    return 0;
}

#else
int gowa_edge_cmp_755b80(spu_context* ctx) { (void)ctx; return 0; }
int gowa_edge_pos_765b70(spu_context* ctx) { (void)ctx; return 0; }
#endif
