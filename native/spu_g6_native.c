/*
 * Small loops of the job code and the job manager as native code. GOWA_G6_NATIVE=0 leaves them to the
 * lifted code.
 *
 * Each routine runs at its function's entry, does every pass but the last, and leaves the registers as
 * the lifted loop would have them there. The lifted loop makes the last pass, so every register ends as
 * it does in the lifted code.
 *
 * 0x9FB8, General6's module 1bdd67b54c3b: memset's loop. r2 bytes left, a multiple of 16, r5 the
 * address, r8 the value.
 *
 * 0xA60 and 0xAAC, the job manager (wwspm), on the way into and out of its interrupt handler: they
 * write the register save and restore stubs at the top of the local store. r76 the base, r78 the bytes,
 * counted down by 16, r77 the first quadword, less r79 each pass.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_simd.h"
#include <stdlib.h>

#if defined(__AVX2__) && defined(__FMA__)

static int g6_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GOWA_G6_NATIVE"); on = !e || atoi(e) != 0; }
    return on;
}

void gowa_g6_memset_9fb8(spu_context* ctx)
{
    const uint32_t n = ctx->gpr[2]._u32[0], a = ctx->gpr[5]._u32[0];
    if (n <= 16u || (n & 15u) || n > SPU_LS_SIZE || !g6_on()) return;
    const uint32_t done = n - 16u;
    const __m128i v = ec_bswap4(ec_m(ctx->gpr[8]));
    uint8_t* const ls = ctx->ls;
    for (uint32_t o = 0; o < done; o += 16u)
        _mm_storeu_si128((__m128i*)(ls + ((a + o) & (SPU_LS_SIZE - 16))), v);
    ctx->gpr[2] = ec_u(_mm_sub_epi32(ec_m(ctx->gpr[2]), _mm_set1_epi32((int)done)));
    ctx->gpr[5] = ec_u(_mm_add_epi32(ec_m(ctx->gpr[5]), _mm_set1_epi32((int)done)));
}

static void pm_stub(spu_context* ctx)
{
    const uint32_t n = ctx->gpr[78]._u32[0], base = ctx->gpr[76]._u32[0];
    if (n <= 16u || (n & 15u) || n > 0x1000u || !g6_on()) return;
    __m128i v = ec_m(ctx->gpr[77]);
    const __m128i d = ec_m(ctx->gpr[79]);
    uint8_t* const ls = ctx->ls;
    uint32_t o = n;
    while (o > 16u) {
        v = _mm_sub_epi32(v, d);
        o -= 16u;
        ec_stq(ls, base + o, v);
    }
    ctx->gpr[77] = ec_u(v);
    ctx->gpr[78] = ec_u(_mm_sub_epi32(ec_m(ctx->gpr[78]), _mm_set1_epi32((int)(n - 16u))));
}
void gowa_pm_stub_a60(spu_context* ctx) { pm_stub(ctx); }
void gowa_pm_stub_aac(spu_context* ctx) { pm_stub(ctx); }

#else
void gowa_g6_memset_9fb8(spu_context* ctx) { (void)ctx; }
void gowa_pm_stub_a60(spu_context* ctx) { (void)ctx; }
void gowa_pm_stub_aac(spu_context* ctx) { (void)ctx; }
#endif
