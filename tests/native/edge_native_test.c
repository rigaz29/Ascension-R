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
#include "spu_edge_light.c"
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
    if (rnd() % 2) c->gpr[18] = c->gpr[19];   /* as the piece sets it: the paired form's case */
    if (rnd() % 3 == 0) {
        const uint32_t a27 = c->gpr[27]._u32[0], a18 = c->gpr[18]._u32[0];
        c->gpr[36]._u32[0] = a27 - a18 + rnd() % 96 - 48;
        c->gpr[29]._u32[0] = a27 - a18 + rnd() % 96 - 48;
    }
}
static long long iters_30310(void) { return s_d1_iters; }
static long long paired_30310(void) { return 2 * s_d1_pairs; }

/* 304A0: r35 = -32 * the iterations (1-48). One case in four puts the two
 * attribute outputs on the records, one in four a merged output on the first
 * table entry the loop reads and another on the records. */
static void setup_304a0(spu_context* c)
{
    const uint32_t n = 1 + rnd() % 48;
    c->gpr[35]._u32[0] = 0u - 32u * n;
    const uint32_t a44 = c->gpr[44]._u32[0], o35 = c->gpr[35]._u32[0], o6 = c->gpr[6]._u32[0];
    switch (rnd() % 4) {
    case 0:
        c->gpr[78]._u32[0] = a44 - o35 + rnd() % 96 - 48;
        c->gpr[74]._u32[0] = a44 - o35 + rnd() % 96 - 48;
        break;
    case 1:
        c->gpr[65]._u32[0] = c->gpr[49]._u32[0] + c->gpr[47]._u32[0] - o6 + rnd() % 64 - 32;
        c->gpr[75]._u32[0] = a44 - o6 + rnd() % 96 - 48;
        break;
    }
}
static long long iters_304a0(void) { return s_d2_iters; }
static long long none(void) { return 0; }

static void set4(spu_context* c, int r, uint32_t v) { for (int w = 0; w < 4; w++) c->gpr[r]._u32[w] = v; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void setf(spu_context* c, int r, float lo, float hi, int splat)
{
    const float s = rndf(lo, hi);
    for (int w = 0; w < 4; w++) c->gpr[r]._u32[w] = fbits(splat ? s : rndf(lo, hi));
}
static void ls_f(spu_context* c, uint32_t a, float x, float y, float z, float w)
{
    const float v[4] = { x, y, z, w };
    for (int k = 0; k < 4; k++) {
        const uint32_t b = fbits(v[k]);
        c->ls[a + 4 * k] = (uint8_t)(b >> 24); c->ls[a + 4 * k + 1] = (uint8_t)(b >> 16);
        c->ls[a + 4 * k + 2] = (uint8_t)(b >> 8); c->ls[a + 4 * k + 3] = (uint8_t)b;
    }
}

/* 7BB4: r83 - r46 = the lights left (1-24), r43 = 16 * (r46 + 1); the light arrays
 * (r99, r97, r98) and the two outputs (r23, r29) each in its own 8 KB; the controls the
 * native routine wants. One case in sixteen breaks a control, one in sixteen misaligns
 * an output, one in eight puts an output on a light array, one in sixteen has an
 * interrupt pending: the native routine must leave those to the lifted loop. Half the
 * cases have plausible floats (vertices, normals, lights, the loop's constants and
 * state), the other half the random bits, which take the exact forms. */
static void setup_7bb4(spu_context* c)
{
    const uint32_t j0 = rnd() % 4, left = 1 + rnd() % 24;
    set4(c, 46, j0);
    set4(c, 83, j0 + left);
    set4(c, 43, 16 * (j0 + 1));
    set4(c, 59, 0x00010203u); set4(c, 82, 0x04050607u); set4(c, 81, 0x08090A0Bu); set4(c, 78, 0x0C0D0E0Fu);
    const uint32_t i64[4] = { 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu };
    const uint32_t i104[4] = { 0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu };
    memcpy(c->gpr[64]._u32, i64, 16);
    memcpy(c->gpr[104]._u32, i104, 16);
    c->gpr[9] = (rnd() & 1) ? c->gpr[104] : c->gpr[64];
    set4(c, 100, 16 * (rnd() % 32));
    uint32_t base[5];
    for (int k = 0; k < 5; k++) base[k] = 0x8000u + 0x2000u * (uint32_t)k + 16u * (rnd() % 64);
    set4(c, 99, base[0]); set4(c, 97, base[1]); set4(c, 98, base[2]);
    set4(c, 23, base[3]); set4(c, 29, base[4]);
    c->int_enable = 0; c->event_status = 0; c->event_mask = 0;
    switch (rnd() % 16) {
    case 0: c->gpr[82]._u32[2] ^= 0x01000000u; break;                       /* a control that is not a splat */
    case 1: c->gpr[23]._u32[0] += 4; break;                                  /* not a whole quadword */
    case 2: case 3: c->gpr[29]._u32[0] = base[1] + 16 * (j0 + 1) + 16 * (rnd() % 4); break;   /* on the colours */
    case 4: c->int_enable = 1; c->event_status = 1; c->event_mask = 1; break;
    }
    if (rnd() & 1) return;   /* random bits */
    static const int vtx[] = { 66, 62, 60, 65, 63, 61 }, nrm[] = { 52, 54, 56, 50, 53, 55 };
    for (int k = 0; k < 6; k++) { setf(c, vtx[k], -20, 20, 0); setf(c, nrm[k], -1, 1, 0); }
    set4(c, 33, fbits(1.0f)); set4(c, 72, fbits(0.5f)); set4(c, 101, fbits(3.0f)); set4(c, 102, fbits(2.0f));
    set4(c, 84, 0x38D1B717u); set4(c, 44, 0x80000000u);
    setf(c, 93, 0.5f, 2.0f, 0);
    for (uint32_t i = 0; i < 32; i++) {
        ls_f(c, base[0] + 16 * i, rndf(-20, 20), rndf(-20, 20), rndf(-20, 20), rndf(0.01f, 0.3f));
        ls_f(c, base[1] + 16 * i, rndf(0, 2), rndf(0, 2), rndf(0, 2), rndf(0, 2));
        ls_f(c, base[2] + 16 * i, rndf(-1, 1), rndf(-1, 1), rndf(-1, 1), rndf(-1, 1));
    }
    static const int sq[] = { 25, 24 }, rs[] = { 10, 12 }, dv[] = { 31, 32, 28, 30, 26, 27 };
    static const int acc[] = { 42, 45, 57, 48, 49, 58, 51, 47 };
    for (int k = 0; k < 2; k++) { setf(c, sq[k], 0.01f, 400, 0); setf(c, rs[k], 0.05f, 10, 0); }
    for (int k = 0; k < 6; k++) setf(c, dv[k], -20, 20, 0);
    for (int k = 0; k < 8; k++) setf(c, acc[k], -5, 5, 0);
    setf(c, 39, 0.01f, 0.3f, 1);
    setf(c, 40, -1, 1, 1); setf(c, 38, -1, 1, 1); setf(c, 41, -1, 1, 1);
    setf(c, 37, 0, 2, 1); setf(c, 36, 0, 2, 1); setf(c, 35, 0, 2, 1); setf(c, 34, 0, 2, 1);
}
static long long iters_7bb4(void) { return s_lt_iters; }

/* 7ED8: r83 - r32 = the lights left (1-24); the arrays it reads (r30: directions, r31:
 * colours) in their own 8 KB each. One case in sixteen misaligns one, one in sixteen has an
 * interrupt pending. Half the cases have plausible floats -- colours down to -0.5 for the
 * negative sums, N.L from -1.5 to 1.5 for both ends of the clamp -- the other half the
 * random bits. */
static void setup_7ed8(spu_context* c)
{
    const uint32_t j0 = rnd() % 4, left = 1 + rnd() % 24;
    set4(c, 32, j0);
    set4(c, 83, j0 + left);
    const uint32_t b30 = 0x8000u + 16u * (rnd() % 64), b31 = 0xA000u + 16u * (rnd() % 64);
    set4(c, 30, b30);
    set4(c, 31, b31);
    c->int_enable = 0; c->event_status = 0; c->event_mask = 0;
    switch (rnd() % 16) {
    case 0: c->gpr[31]._u32[0] += 8; break;
    case 1: c->int_enable = 1; c->event_status = 1; c->event_mask = 1; break;
    }
    if (rnd() & 1) return;
    set4(c, 33, fbits(1.0f)); set4(c, 44, 0x80000000u);
    static const int nrm[] = { 71, 69, 67, 70, 68, 64 }, col[] = { 56, 55, 54, 53, 52, 50 };
    static const int acc[] = { 46, 35, 39, 41, 42, 45, 38, 40, 43, 34, 36, 37 };
    for (int k = 0; k < 6; k++) { setf(c, nrm[k], -1, 1, 0); setf(c, col[k], -0.5f, 2, 0); }
    for (int k = 0; k < 12; k++) setf(c, acc[k], 0, 5, 0);
    setf(c, 12, -1.5f, 1.5f, 0); setf(c, 5, -1.5f, 1.5f, 0);
    for (uint32_t i = 0; i < 6 * 30; i++) {
        ls_f(c, b30 + 16 * i, rndf(-1, 1), rndf(-1, 1), rndf(-1, 1), rndf(-1, 1));
        ls_f(c, b31 + 16 * i, rndf(-0.5f, 2), rndf(-0.5f, 2), rndf(-0.5f, 2), rndf(-0.5f, 2));
    }
}
static long long iters_7ed8(void) { return s_l2_iters; }

typedef struct {
    const char* name;
    void (*ref)(spu_context*);
    void (*native)(spu_context*);
    void (*setup)(spu_context*);
    long long (*iters)(void);
    long long (*paired)(void);
} edge_test;
static const edge_test s_tests[] = {
    { "decode 30310", ref_30310, gowa_edge_decode_30310, setup_30310, iters_30310, paired_30310 },
    { "decode 304A0", ref_304A0, gowa_edge_decode_304a0, setup_304a0, iters_304a0, none },
    { "light 7BB4", ref_7BB4, gowa_edge_light_7bb4, setup_7bb4, iters_7bb4, none },
    { "light 7ED8", ref_7ED8, gowa_edge_light_7ed8, setup_7ed8, iters_7ed8, none },
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
    setenv("GOWA_EDGE_NATIVE_STATS", "1", 1);   /* the light loops count their iterations only then */
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
        const long long it0 = T->iters(), pr0 = T->paired();
        for (int k = 0; k < cases && bad < 4; k++) {
            a->ls = la;
            fill(a);
            T->setup(a);
            memcpy(b->gpr, a->gpr, sizeof a->gpr);
            b->pic_base = a->pic_base;
            b->int_enable = a->int_enable;
            b->event_status = a->event_status;
            b->event_mask = a->event_mask;
            b->ls = lb;
            memcpy(lb, la, SPU_LS_SIZE);
            T->ref(a);
            T->native(b);
            T->ref(b);
            bad += compare(a, b, T->name, k);
        }
        const long long it = T->iters() - it0, pr = T->paired() - pr0;
        printf("[%s] %d cases, %lld native iterations (%lld in pairs): %s\n", T->name, cases, it, pr,
               bad ? "FAIL" : it ? "PASS" : "FAIL (the native routine did nothing)");
        fails += bad || !it;
    }
    return fails ? 1 : 0;
}
