/*
 * General6's noise, 0x4930 of the job module a67e549fa0ab: four arguments r3-r6, a pixel a lane, the
 * result in r3. Its jobs (entry 0x81E8) call it ~2,100 times each, 16 jobs a frame, ~0.9 us a call:
 * about half of General6's time (docs/notes/2026-11-05-general6-noise-review.md).
 *
 * In the levels the job's record has the noise's scale (words 28-30) at zero, so every call has all
 * four arguments zero and returns 0. GOWA_G6_NOISE:
 *  - 1, the default in the game: all four zero returns 0 at the routine's entry. It has a frame of its
 *    own and restores r80 and up, so only r3 changes, and its frame stays unwritten: dead stack;
 *  - 2, the check: the routine runs, and what it returns for zero arguments is compared with 0
 *    (GOWA_EDGE_NATIVE_STATS=1 prints the counts);
 *  - 0: off. The default in a replay and a capture, whose strict compare sees the dead stack.
 * Any other arguments run the routine, as in the main menu, whose scale is not zero.
 *
 * The pixel loop around it, 0x63B4: a pass makes 4 pixels, 16 passes a cell of 64, 2 cells a row, rows
 * 1-15 of the job's tile. A pass adds four noise calls to each pixel's running sum, in the array at r125,
 * and makes the pixels at r100: three channels the cell's, in r127, and the fourth from the sum. The
 * noise's arguments are the pass's position times the 16 constants in r109-r124. With those zero and the
 * cell's sums zero, the sums stay zero and all of a cell's pixels are the same: the job makes 2,048 pixels
 * of which 32 differ. So the loop is left at its last pass, and at its exit, 0x6A90, that pass's 32 bytes
 * go to the passes left out. The position, in r82, r83, r85 and r88, stays where it was for that pass: it
 * only meets the zeros, and the loop's next entry reloads it. GOWA_G6_CELL:
 *  - 1, the default in the game;
 *  - 2, the check: every pass runs, and at the exit the cell is compared with its last pass and its sums
 *    with zero (GOWA_EDGE_NATIVE_STATS=1 prints the counts);
 *  - 0: off. The default in a replay and a capture, whose strict compare sees the registers and the dead
 *    stack.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void jobmod_a67e549fa0ab_e41E8_spu_func_00004930(spu_context* ctx);
static SPU_THREAD_LOCAL int t_g6_guard;
static volatile long long s_g6_zero, s_g6_other, s_g6_bad;
static int s_g6_stats;

static volatile long long s_cell_on, s_cell_off, s_cell_bad;

static void g6_report(void)
{
    fprintf(stderr, "[g6-noise] %lld calls with zero arguments, %lld with others; %lld of the zero ones not 0\n",
            s_g6_zero, s_g6_other, s_g6_bad);
    fprintf(stderr, "[g6-noise] %lld cells the same, %lld not looked at; %lld of the same ones wrong\n",
            s_cell_on, s_cell_off, s_cell_bad);
}

static int g6_mode(void)
{
    static volatile int m = -1;
    int v = m;
    if (v >= 0) return v;
    const char* e = getenv("GOWA_G6_NOISE");
    v = e ? atoi(e) : !(getenv("SPU_JOB_REPLAY") || getenv("SPU_JOB_CAPTURE"));
    s_g6_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
    if (s_g6_stats) atexit(g6_report);
    m = v;
    return v;
}

int gowa_g6_noise_4930(spu_context* ctx)
{
    if (t_g6_guard) return 0;
    const int mode = g6_mode();
    if (!mode) return 0;
    const uint64_t* a = (const uint64_t*)&ctx->gpr[3];
    const uint64_t* b = (const uint64_t*)&ctx->gpr[4];
    const uint64_t* c = (const uint64_t*)&ctx->gpr[5];
    const uint64_t* d = (const uint64_t*)&ctx->gpr[6];
    if (a[0] | a[1] | b[0] | b[1] | c[0] | c[1] | d[0] | d[1]) {
        if (s_g6_stats) __atomic_add_fetch(&s_g6_other, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (s_g6_stats) __atomic_add_fetch(&s_g6_zero, 1, __ATOMIC_RELAXED);
    if (mode == 1) {
        memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);
        return 1;
    }
    const uint32_t link = ctx->gpr[0]._u32[0];
    t_g6_guard = 1;
    ctx->host_depth++;
    jobmod_a67e549fa0ab_e41E8_spu_func_00004930(ctx);
    spu_drain_call(ctx, link);
    ctx->host_depth--;
    t_g6_guard = 0;
    const uint64_t* r = (const uint64_t*)&ctx->gpr[3];
    if (r[0] | r[1]) __atomic_add_fetch(&s_g6_bad, 1, __ATOMIC_RELAXED);
    return 1;
}

#define CELL_PASSES 64u
static SPU_THREAD_LOCAL uint32_t t_cell_left, t_cell_out, t_cell_sum;

static int cell_mode(void)
{
    static volatile int m = -1;
    int v = m;
    if (v >= 0) return v;
    const char* e = getenv("GOWA_G6_CELL");
    v = e ? atoi(e) : !(getenv("SPU_JOB_REPLAY") || getenv("SPU_JOB_CAPTURE"));
    g6_mode();
    m = v;
    return v;
}

static void cell_count(volatile long long* c)
{
    if (s_g6_stats && !(__atomic_add_fetch(c, 1, __ATOMIC_RELAXED) & 0x3FFFF)) g6_report();
}

static void cell_step(u128* r, uint32_t by)
{
    for (int i = 0; i < 4; i++) r->_u32[i] += by;
}

void gowa_g6_cell_63b4(spu_context* ctx)
{
    const int mode = cell_mode();
    if (!mode) return;
    const uint32_t r1 = ctx->gpr[1]._u32[0];
    const uint32_t n = spu_ls_read32(ctx, r1 + 0x870u), sum = ctx->gpr[125]._u32[0], out = ctx->gpr[100]._u32[0];
    if (spu_ls_read32(ctx, r1 + 0x9B0u) || n < 2u || n > CELL_PASSES || ((sum | out) & 15u) ||
        sum + n * 16u > SPU_LS_SIZE || out + n * 32u > SPU_LS_SIZE)
        return;
    uint64_t any = 0;
    for (int r = 109; r <= 124; r++) {
        const uint64_t* q = (const uint64_t*)&ctx->gpr[r];
        any |= q[0] | q[1];
    }
    const uint64_t* s = (const uint64_t*)(ctx->ls + sum);
    for (uint32_t i = 0; i < n * 2u; i++) any |= s[i];
    if (any) {
        cell_count(&s_cell_off);
        return;
    }
    cell_count(&s_cell_on);
    t_cell_left = n - 1u;
    t_cell_out = out;
    t_cell_sum = sum;
    if (mode != 1) return;
    cell_step(&ctx->gpr[125], (n - 1u) * 16u);
    cell_step(&ctx->gpr[100], (n - 1u) * 32u);
    for (uint32_t i = 0; i < 16u; i += 4u) spu_ls_write32(ctx, r1 + 0x9B0u + i, n - 1u);
}

void gowa_g6_cell_6a90(spu_context* ctx)
{
    const uint32_t left = t_cell_left;
    if (!left) return;
    t_cell_left = 0;
    uint8_t* const o = ctx->ls + t_cell_out;
    const uint8_t* const last = o + left * 32u;
    if (cell_mode() == 1) {
        for (uint32_t i = 0; i < left; i++) memcpy(o + i * 32u, last, 32u);
        return;
    }
    int bad = 0;
    for (uint32_t i = 0; i < left; i++) bad |= memcmp(o + i * 32u, last, 32u);
    const uint64_t* s = (const uint64_t*)(ctx->ls + t_cell_sum);
    for (uint32_t i = 0; i < (left + 1u) * 2u; i++) bad |= s[i] != 0;
    if (bad) cell_count(&s_cell_bad);
}
