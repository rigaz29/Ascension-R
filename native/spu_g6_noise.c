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

static void g6_report(void)
{
    fprintf(stderr, "[g6-noise] %lld calls with zero arguments, %lld with others; %lld of the zero ones not 0\n",
            s_g6_zero, s_g6_other, s_g6_bad);
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
