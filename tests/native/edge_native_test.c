/* The native EDGE loops against the lifted loops they stand in for.
 *
 * tests/native/test_edge_native.py writes edge_refs.h -- each lifted loop as
 * ref_<name>(ctx), taken from the lifted source -- and builds this with the
 * native routines included. A case fills all 128 registers and the local
 * store with random bits, shapes the few registers a loop needs shaped (its
 * trip count, sometimes outputs over its input), then runs
 *     a: the lifted loop alone
 *     b: the native routine, then the lifted loop (as the hook runs them)
 * and wants the same registers and local store from both. The native routine
 * must also have done the work (its iteration count), else the case proves
 * nothing. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_edge_decode.c"
#include "edge_refs.h"

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static void fill(spu_context* c)
{
    for (int r = 0; r < 128; r++)
        for (int w = 0; w < 4; w++) c->gpr[r]._u32[w] = rnd();
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 4) {
        const uint32_t v = rnd();
        memcpy(c->ls + i, &v, 4);
    }
}

/* 30310: r19 = -32 * the iterations (1-64). One case in three puts the two
 * outputs on the records the loop reads, within a few quadwords. */
static void setup_30310(spu_context* c)
{
    const uint32_t n = 1 + rnd() % 64;
    c->gpr[19]._u32[0] = 0u - 32u * n;
    if (rnd() % 3 == 0) {
        const uint32_t a27 = c->gpr[27]._u32[0], a18 = c->gpr[18]._u32[0];
        c->gpr[36]._u32[0] = a27 - a18 + rnd() % 96 - 48;
        c->gpr[29]._u32[0] = a27 - a18 + rnd() % 96 - 48;
    }
}
static long long iters_30310(void) { return s_dec_iters; }

typedef struct {
    const char* name;
    void (*ref)(spu_context*);
    void (*native)(spu_context*);
    void (*setup)(spu_context*);
    long long (*iters)(void);
} edge_test;
static const edge_test s_tests[] = {
    { "decode 30310", ref_30310, gowa_edge_decode_30310, setup_30310, iters_30310 },
};

static int compare(const spu_context* a, const spu_context* b, const char* name, int k)
{
    for (int r = 0; r < 128; r++)
        if (memcmp(a->gpr[r]._u32, b->gpr[r]._u32, 16)) {
            printf("[%s] case %d: r%d lifted %08X %08X %08X %08X, native %08X %08X %08X %08X\n", name, k, r,
                   a->gpr[r]._u32[0], a->gpr[r]._u32[1], a->gpr[r]._u32[2], a->gpr[r]._u32[3],
                   b->gpr[r]._u32[0], b->gpr[r]._u32[1], b->gpr[r]._u32[2], b->gpr[r]._u32[3]);
            return 1;
        }
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 16)
        if (memcmp(a->ls + i, b->ls + i, 16)) {
            printf("[%s] case %d: local store differs at 0x%05X\n", name, k, i);
            return 1;
        }
    return 0;
}

int main(int argc, char** argv)
{
    const int cases = argc > 1 ? atoi(argv[1]) : 3000;
    g_spu_float_rz = 1;
    _mm_setcsr(_mm_getcsr() | SPU_MXCSR_RZFZ);
    spu_context* a = calloc(1, sizeof *a);
    spu_context* b = calloc(1, sizeof *b);
    uint8_t* la = malloc(SPU_LS_SIZE);
    uint8_t* lb = malloc(SPU_LS_SIZE);
    if (!a || !b || !la || !lb) return 2;
    int fails = 0;
    for (size_t t = 0; t < sizeof s_tests / sizeof s_tests[0]; t++) {
        const edge_test* T = &s_tests[t];
        int bad = 0;
        const long long it0 = T->iters();
        for (int k = 0; k < cases && bad < 4; k++) {
            a->ls = la;
            fill(a);
            T->setup(a);
            memcpy(b->gpr, a->gpr, sizeof a->gpr);
            b->pic_base = a->pic_base;
            b->ls = lb;
            memcpy(lb, la, SPU_LS_SIZE);
            T->ref(a);
            T->native(b);
            T->ref(b);
            bad += compare(a, b, T->name, k);
        }
        const long long it = T->iters() - it0;
        printf("[%s] %d cases, %lld native iterations: %s\n", T->name, cases, it,
               bad ? "FAIL" : it ? "PASS" : "FAIL (the native routine did nothing)");
        fails += bad || !it;
    }
    return fails ? 1 : 0;
}
