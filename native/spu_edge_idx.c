/*
 * EDGE's index decompression: 0xE4D0 of the job module (it calls 0xDCB0, which runs on into
 * 0xDCC0 and ends in 0xB600), GOWA_EDGE_NATIVE bit 65536, on unless GOWA_EDGE_NATIVE leaves it out.
 *
 * Arguments: r3 the EDGE context c, r4 the stream (in place: the indices end up there).
 *  - The index count is the halfword at c + 0x190; 0 does nothing but the frame.
 *  - The stream type is c[6] >> 4: only 2 and 3 are decompressed. Every nonzero count then sets
 *    c + 0x50 word 0 = r4.
 * The stream: an 8-byte header, then three sections.
 *  - Header: h0 (the reused indices), h1 (a delta bias), h2 (the bitmask's bytes), k (the bits
 *    of a reuse), and a byte.
 *  - Sections: h2 bytes of bitmask; ceil(2T / 8) bytes of 2-bit triangle codes (T = count / 3);
 *    the k-bit reuse values.
 * Five stages:
 *  1. n1 = 32 ceil(h0 / 32) k-bit values, MSB first, from the stream as it was, go as halfwords
 *     to S = r4 + align16(8 + h2 + codes). For k = 0 the piece instead zero-fills
 *     8 ceil(h0 / 8) of them (0xE0C8), and the rest up to n1 stay as the store held them.
 *  2. In place, over n1: s2[i] = s2[i - 8] + s1[i] - h1 (eight running sums, 16 bits).
 *  3. The vertex sequence, at Q = r4 + align16(6T): for each of n3 = 16 ceil(8 h2 / 16) bits
 *     from r4 + 8 (MSB first; past the bitmask, the bytes after it), a 1 takes the next s2 value
 *     (read from the local store at S) and a 0 a new vertex (counting from 0).
 *  4. align16(codes) bytes copied from r4 + 8 + h2, as the store is by then, to C = Q + 2 n3.
 *  5. At r4, 8 triangles a pass, ceil(T / 8) passes. Each triangle's code (2 bits from C, MSB
 *     first) against the triangle (a, b, c) before it:
 *     - 3: three sequence values;
 *     - 2: (b, a, next);
 *     - 1: (c, b, next);
 *     - 0: (a, c, next).
 *  Then c + 0x50 word 2 = align16(r4 + 48 ceil(T / 8)) (0xB600).
 * Also left:
 *  - E4D0's frame: r80 at r1 - 0x10, r81 at r1 - 0x20, r0 at r1 + 0x10, the back chain at
 *    r1 - 0x40;
 *  - r3 = cwd(r1, 0) for a nonzero count (c for a zero one);
 *  - the other volatile registers do not matter (strict replay with them scrambled).
 * A model of these stages matched the whole local store after the call in 16 probed calls.
 *
 * Declined (lifted instead):
 *  - c, r4 or r1 not 16-aligned;
 *  - a count not a multiple of 3, or T < 16;
 *  - k > 16, or h0 = 0 with k > 0 (not seen);
 *  - any of the regions it reads or writes outside the local store or over c, the frame, or
 *    each other where the order would matter.
 * GOWA_EDGE_VERIFY=1 checks every call (spu_edge_verify.h).
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_verify.h"
#include "spu_edge_memo.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile long long s_x11_calls, s_x11_tris, s_x11_declined, s_x11_why[13];
static int x11_no(int why) { __atomic_add_fetch(&s_x11_why[why], 1, __ATOMIC_RELAXED); return 0; }

static uint32_t x11_be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static void x11_put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void x11_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void x11_putq(uint8_t* ls, uint32_t a, const u128* r)
{
    for (int k = 0; k < 4; k++) x11_put32(ls + ((a + 4u * (uint32_t)k) & (SPU_LS_SIZE - 1)), r->_u32[k]);
}
static int x11_apart(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a1 <= b0 || b1 <= a0; }
/* n host-order halfwords to ls + a big-endian, and back */
static void x11_wr16(uint8_t* ls, uint32_t a, const uint16_t* v, uint32_t n)
{
    const __m128i sw = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8)
        _mm_storeu_si128((__m128i*)(ls + a + 2 * i), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(v + i)), sw));
    for (; i < n; i++) x11_put16(ls + a + 2 * i, v[i]);
}
static void x11_rd16(const uint8_t* ls, uint32_t a, uint16_t* v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) v[i] = (uint16_t)x11_be16(ls + ((a + 2 * i) & (SPU_LS_SIZE - 2)));
}
static uint64_t x11_be64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }

/* Stage 3, a bitmask byte m at a time: its eight outputs are the next reuses at its 1 bits and
 * the next new vertices at its 0 bits. x11_ea[m] picks the reuses (halfwords of A = s2[j..]),
 * x11_eb[m] the new ones (of B = nxt + 0..7), in order; x11_pc[m] = popcount(m). */
static __m128i x11_ea[256], x11_eb[256];
static uint8_t x11_pc[256];
/* Stage 5: from (a b c, x) the triangle before and (q0 q1 q2 q3) the sequence at the pointer,
 * the triangle for each code: 0 (a, c, q0), 1 (c, b, q0), 2 (b, a, q0), 3 (q0, q1, q2). */
static __m128i x11_tc[4];
__attribute__((constructor)) static void x11_init(void)
{
    for (int m = 0; m < 256; m++) {
        uint8_t a[16], b[16];
        int r1 = 0, r0 = 0;
        for (int q = 0; q < 8; q++) {
            if ((m >> (7 - q)) & 1) {
                a[2 * q] = (uint8_t)(2 * r1); a[2 * q + 1] = (uint8_t)(2 * r1 + 1); b[2 * q] = b[2 * q + 1] = 0x80; r1++;
            } else {
                b[2 * q] = (uint8_t)(2 * r0); b[2 * q + 1] = (uint8_t)(2 * r0 + 1); a[2 * q] = a[2 * q + 1] = 0x80; r0++;
            }
        }
        memcpy(&x11_ea[m], a, 16);
        memcpy(&x11_eb[m], b, 16);
        x11_pc[m] = (uint8_t)r1;
    }
    static const uint8_t hw[4][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 0, 4 }, { 4, 5, 6 } };
    for (int code = 0; code < 4; code++) {
        uint8_t t[16];
        memset(t, 0x80, 16);
        for (int i = 0; i < 3; i++) { t[2 * i] = (uint8_t)(2 * hw[code][i]); t[2 * i + 1] = (uint8_t)(2 * hw[code][i] + 1); }
        memcpy(&x11_tc[code], t, 16);
    }
}

static int x11_native(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], sp = ctx->gpr[1]._u32[0];
    if ((c & 15) || c + 0x1A0 > SPU_LS_SIZE || (sp & 15) || sp < 0x40 || sp + 0x20 > SPU_LS_SIZE || (src & 15)) return x11_no(0);
    const uint32_t count = x11_be16(ls + c + 0x190);
    const uint32_t typ = (uint32_t)(ls[c + 6] >> 4);
    const int decode = count != 0 && (typ == 2 || typ == 3);
    uint32_t T = 0, h0 = 0, h1 = 0, h2 = 0, k = 0, codes = 0, off_k = 0, S = 0, n1 = 0, Q = 0, n3 = 0, C = 0, ncp = 0, passes = 0;
    if (decode) {
        if (count % 3) return x11_no(1);
        T = count / 3;
        if (T < 16 || src + 16 > SPU_LS_SIZE) return x11_no(2);
        h0 = x11_be16(ls + src); h1 = x11_be16(ls + src + 2); h2 = x11_be16(ls + src + 4); k = ls[src + 6];
        if (k > 16 || (k > 0 && h0 == 0)) return x11_no(3);
        codes = (2 * T + 7) / 8;
        off_k = 8 + h2 + codes;
        S = src + ((off_k + 15) & ~15u);
        n1 = 32 * ((h0 + 31) / 32);
        Q = src + ((6 * T + 15) & ~15u);
        n3 = 16 * ((8 * h2 + 15) / 16);
        C = Q + 2 * n3;
        ncp = (codes + 15) & ~15u;
        passes = (T + 7) / 8;
        if (n1 > 4096 || passes > 1365 || n3 > 8192) return x11_no(4);
        /* the reuses the bitmask takes (its 1 bits, and up to 16 past it) within s2's room */
        if (src + 8 + h2 + codes + 16 > SPU_LS_SIZE) return x11_no(5);
        {
            uint32_t ones = 0, i = 0;
            for (; i + 8 <= h2 + 2; i += 8) ones += (uint32_t)__builtin_popcountll(x11_be64(ls + src + 8 + i));
            for (; i < h2 + 2; i++) ones += (uint32_t)__builtin_popcount(ls[src + 8 + i]);
            if (ones > n1 + 40) return x11_no(6);
        }
        /* the sequence the real triangles take within seq's room (the 7 extra ones take 21 more at most) */
        {
            /* the codes that are 3: a 1 in both bits of a pair */
            const uint8_t* cb = ls + src + 8 + h2;
            const uint64_t lo = 0x5555555555555555ull;
            uint32_t n3s = 0, i = 0;
            const uint32_t full = T / 4;
            for (; i + 8 <= full; i += 8) { const uint64_t x = x11_be64(cb + i); n3s += (uint32_t)__builtin_popcountll(x & (x >> 1) & lo); }
            for (; i < full; i++) { const uint32_t x = cb[i]; n3s += (uint32_t)__builtin_popcount(x & (x >> 1) & 0x55); }
            if (T & 3) { const uint32_t x = cb[full] & (0xFF00u >> (2 * (T & 3))); n3s += (uint32_t)__builtin_popcount(x & (x >> 1) & 0x55); }
            if (T + 2 * n3s > n3 + 20) return x11_no(7);
        }
        /* everything it touches inside the local store, with room for the reads past the ends */
        const uint32_t hi = C + ncp + 64;
        const uint32_t kend = src + off_k + (n1 * k + 7) / 8 + 16;
        if (hi > SPU_LS_SIZE || kend > SPU_LS_SIZE || S + 2 * n1 + 64 > SPU_LS_SIZE) return x11_no(8);
        /* the regions as the model has them: apart from c and the frame; the stages' outputs where
         * the order would matter: S before Q (stage 3 reads S, writes Q), the copy's source before C */
        const uint32_t lo_all = src, hi_all = hi > kend ? hi : kend;
        if (!x11_apart(lo_all, hi_all, c, c + 0x1A0) || !x11_apart(lo_all, hi_all, sp - 0x40, sp + 0x20)) return x11_no(9);
        if (!x11_apart(S, S + 2 * n1 + 32, Q, Q + 2 * n3)) return x11_no(10);
        if (!x11_apart(src + 8, src + 8 + h2 + ncp, C, C + ncp)) return x11_no(11);
        if (src + 48 * passes > Q + 2 * n3) return x11_no(12);
    }

    /* E4D0's frame */
    x11_putq(ls, sp - 0x10, &ctx->gpr[80]);
    x11_putq(ls, sp - 0x20, &ctx->gpr[81]);
    x11_putq(ls, sp + 0x10, &ctx->gpr[0]);
    x11_putq(ls, sp - 0x40, &ctx->gpr[1]);
    if (count == 0) return 1;                          /* r3 stays c */

    if (decode) {
        static SPU_THREAD_LOCAL uint16_t s2[8 + 4096 + 64], seq[8192 + 64], out[3 * 8 * 1366 + 8];
        /* 1 and 2: from the stream as it was (the piece expands it in place, from the end) */
        {
            const uint8_t* p = ls + src + off_k;
            uint64_t acc = 0;
            int nb = 0;
            const uint32_t mk = (1u << k) - 1;
            uint16_t* d = s2 + 8;
            memset(s2, 0, 16);
            if (k == 0) {
                /* the piece zero-fills 8 ceil(h0 / 8) of them (0xE0C8); stage 2 still takes n1, the
                 * rest as the store held them */
                const uint32_t z = 8 * ((h0 + 7) / 8);
                for (uint32_t i = 0; i < n1; i++) {
                    const uint32_t v = i < z ? 0 : x11_be16(ls + S + 2 * i);
                    d[i] = (uint16_t)(d[(int)i - 8] + v - h1);
                }
            } else {
                for (uint32_t i = 0; i < n1; i++) {
                    if (nb < (int)k) { acc = (acc << 32) | x11_be64(p) >> 32; p += 4; nb += 32; }
                    const uint32_t v = (uint32_t)(acc >> (nb - (int)k)) & mk;
                    nb -= (int)k;
                    d[i] = (uint16_t)(d[(int)i - 8] + v - h1);
                }
            }
        }
        x11_wr16(ls, S, s2 + 8, n1);
        /* the reuses past n1, as the store holds them: the bitmask's last bits may reach there */
        x11_rd16(ls, S + 2 * n1, s2 + 8 + n1, 48);
        /* 3: the bitmask (and the bytes after it) over the reuses and a counter, a byte at a time */
        {
            const uint8_t* m = ls + src + 8;
            const uint16_t* r = s2 + 8;
            const __m128i iota = _mm_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7);
            uint32_t j = 0, nxt = 0;
            for (uint32_t i = 0; i < n3; i += 8) {
                const uint32_t mb = m[i >> 3];
                const __m128i A = _mm_loadu_si128((const __m128i*)(r + j));
                const __m128i B = _mm_add_epi16(_mm_set1_epi16((short)nxt), iota);
                _mm_storeu_si128((__m128i*)(seq + i), _mm_or_si128(_mm_shuffle_epi8(A, x11_ea[mb]), _mm_shuffle_epi8(B, x11_eb[mb])));
                j += x11_pc[mb];
                nxt += 8u - x11_pc[mb];
            }
        }
        x11_wr16(ls, Q, seq, n3);
        /* 4 */
        memmove(ls + C, ls + src + 8 + h2, ncp);
        /* the sequence past n3 as the store holds it now: the last pass's extra triangles read there */
        x11_rd16(ls, Q + 2 * n3, seq + n3, 64);
        /* 5: all of it read before any of it is written (the passes never reach what they read) */
        {
            const uint8_t* cd = ls + C;
            const uint32_t nt = 8 * passes;
            __m128i tri = _mm_setzero_si128();
            uint32_t p = 0;
            for (uint32_t t = 0; t < nt; t++) {
                const uint32_t code = (uint32_t)(cd[t >> 2] >> (6 - 2 * (t & 3))) & 3;
                const __m128i q = _mm_loadl_epi64((const __m128i*)(seq + p));
                tri = _mm_shuffle_epi8(_mm_unpacklo_epi64(tri, q), x11_tc[code]);
                _mm_storel_epi64((__m128i*)(out + 3 * t), tri);
                p += 1u + ((code + 1) & 4) / 2;
            }
            x11_wr16(ls, src, out, 3 * nt);
        }
        x11_put32(ls + c + 0x58, (src + 48 * passes + 15) & ~15u);
        __atomic_add_fetch(&s_x11_tris, (long long)T, __ATOMIC_RELAXED);
    }
    x11_put32(ls + c + 0x50, src);
    /* r3: cwd(r1, 0), the word-insert control for slot 0 */
    ctx->gpr[3]._u32[0] = 0x00010203u; ctx->gpr[3]._u32[1] = 0x14151617u;
    ctx->gpr[3]._u32[2] = 0x18191A1Bu; ctx->gpr[3]._u32[3] = 0x1C1D1E1Fu;
    return 1;
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E4D0(spu_context* ctx);
static SPU_THREAD_LOCAL spu_context* t_x11_guard;

static void x11_stats(void)
{
    fprintf(stderr, "[edge-native] index decompression E4D0: %lld calls native, %lld triangles, %lld declined\n",
            s_x11_calls, s_x11_tris, s_x11_declined);
    fprintf(stderr, "[edge-native] index decompression E4D0 declined by check:");
    for (unsigned i = 0; i < sizeof s_x11_why / sizeof s_x11_why[0]; i++) if (s_x11_why[i]) fprintf(stderr, " #%u %lld", i, s_x11_why[i]);
    fprintf(stderr, "\n");
}
static edge_vstat s_vx11 = { "index decompression E4D0", 0, 1, 0, 0, 0, 0, 0, 0 };
static void x11_verify_report(void) { edge_vreport(&s_vx11); }

/* The hook at 0xE4D0's entry: 1 when the call is done (the function returns). */
int gowa_edge_idx_e4d0(spu_context* ctx)
{
    if (t_x11_guard == ctx) return 0;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        on = !e || (atoi(e) & 65536) != 0;
        if (on && getenv("GOWA_EDGE_NATIVE_STATS")) atexit(x11_stats);
        if (on && edge_verify_on()) atexit(x11_verify_report);
    }
    if (!on) return 0;
    if (edge_verify_on())
        return edge_verify(ctx, &s_vx11, x11_native, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E4D0, &t_x11_guard);
    /* a kept result (spu_edge_memo.c): the list in place, and what the stage leaves in c */
    uint8_t* const ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0];
    uint32_t bytes;
    if (edge_memo_idx(ctx, src, &bytes)) {
        x11_put32(ls + c + 0x58, src + bytes);
        x11_put32(ls + c + 0x50, src);
        ctx->gpr[3]._u32[0] = 0x00010203u; ctx->gpr[3]._u32[1] = 0x14151617u;
        ctx->gpr[3]._u32[2] = 0x18191A1Bu; ctx->gpr[3]._u32[3] = 0x1C1D1E1Fu;
        return 1;
    }
    if (x11_native(ctx)) {
        __atomic_add_fetch(&s_x11_calls, 1, __ATOMIC_RELAXED);
        if (x11_be16(ls + c + 0x190) && (ls[c + 6] >> 4 == 2 || ls[c + 6] >> 4 == 3))
            edge_memo_idx_note(ctx, src, ((uint32_t)ls[c + 0x58] << 24 | (uint32_t)ls[c + 0x59] << 16 |
                                          (uint32_t)ls[c + 0x5A] << 8 | ls[c + 0x5B]) - src);
        else edge_memo_cancel();
        return 1;
    }
    edge_memo_cancel();
    __atomic_add_fetch(&s_x11_declined, 1, __ATOMIC_RELAXED);
    return 0;
}
