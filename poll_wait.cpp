/*
 * GowARecomp -- the main thread's polling loops wait instead of spinning.
 *
 * The title's main thread waits for its SPU jobs by polling: it scans its job
 * graph for entries that became ready (func_0044CD20 / func_0044D120 around
 * func_0044CA48) and spins until per-SPU progress counters reach a target
 * (func_0049D688, func_0049B35C). In the first level that was 61% of its time,
 * on a core it shares with an SPU worker. scripts/ppu_hooks.json has the lift
 * call gowa_poll_wait at the head of each of those loops (ppu_lifter.py
 * --insn-hooks); after GOWA_POLL_WAIT_N passes in a row at one head it waits
 * GOWA_POLL_WAIT_US microseconds before the next.
 *
 * The wait is MONITORX/MWAITX with its timer where the CPU has it (AMD's,
 * since Excavator): the core stops issuing for this thread until the time is
 * up, without the OS, so the SMT sibling -- an SPU worker -- gets the core, and
 * the loop is back a few microseconds after the deadline. A waitable timer
 * sleeps no less than ~0.5 ms here: that freed the core as well, but the frame
 * rate did not move (12.70 -> 12.81). Without MONITORX it spins on PAUSE to the
 * deadline. The defaults (8 passes, 20 us) gave 12.60 -> 12.94 flips/s in the
 * first level (ABBA, two runs each).
 *
 * GOWA_POLL_WAIT_US=0 turns it off.
 *
 * Under PS3_FTRACE the passes at one loop head, first to last, are one span of
 * the thread's, kind usleep with the loop head for its call site: `gowa.py
 * frames` lists them with the usleeps ("usleep@49D708") and draws them as 'u'.
 */
#include "ppu_recomp.h"
#include "ps3emu/ps3_ftrace.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#include <x86intrin.h>
#define POLL_X86 1
#endif

#if POLL_X86
__attribute__((target("mwaitx"))) static void poll_mwaitx(unsigned cycles)
{
    alignas(64) static thread_local volatile uint64_t line[8];
    _mm_monitorx((void*)line, 0, 0);
    _mm_mwaitx(2, 0, cycles);                   /* 2: wake at the timer, too */
}
#endif

namespace {
struct PollWait {
    int us = -1, passes = 8, mwaitx = 0;
    unsigned long long cycles = 0;              /* the wait, in TSC cycles */
    double tsc_per_us = 0;

    void init()
    {
        const char* e = getenv("GOWA_POLL_WAIT_US");
        const char* n = getenv("GOWA_POLL_WAIT_N");
        us = e ? atoi(e) : 20;
        if (n && atoi(n) > 0) passes = atoi(n);
#if POLL_X86 && defined(_WIN32)
        if (us <= 0) return;
        int r[4];
        __cpuid(r, (int)0x80000001);
        mwaitx = (r[2] >> 29) & 1;              /* CPUID 8000_0001 ECX[29]: MONITORX */
        /* The TSC rate, against the performance counter, once. */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const unsigned long long t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 100 < f.QuadPart);
        tsc_per_us = (double)(__rdtsc() - t0) * (double)f.QuadPart / ((double)(b.QuadPart - a.QuadPart) * 1e6);
        cycles = (unsigned long long)(tsc_per_us * us);
        fprintf(stderr, "[poll-wait] %d us after %d passes, %s (TSC %.0f MHz)\n", us, passes,
                mwaitx ? "MWAITX" : "PAUSE", tsc_per_us);
#else
        us = 0;                                 /* no TSC wait off x86 / Windows */
#endif
    }
} s_poll;
}

/* A pass more than 200 us after the last one, or at another head, starts a new
 * wait; a single pass did not wait. */
static void poll_trace(ppu_context* ctx, uint32_t addr)
{
    static thread_local uint32_t head, passes;
    static thread_local uint64_t t0, t1;
    const uint64_t t = ps3_ft_now();
    if (addr != head || t - t1 > 200000) {
        if (passes > 1 && t1 - t0 >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_USLEEP, (uint32_t)ctx->thread_id, passes, head, t0, t1);
        head = addr;
        t0 = t;
        passes = 0;
    }
    passes++;
    t1 = t;
}

extern "C" void gowa_poll_wait(ppu_context* ctx, uint32_t addr)
{
    if (s_poll.us < 0) s_poll.init();
    if (PS3_FT_ON()) poll_trace(ctx, addr);
    if (s_poll.us <= 0) return;
#if POLL_X86
    /* Passes in a row at one loop head: the previous pass ended at most 200 us
     * ago (a scan of the job graph takes a few). */
    static thread_local uint32_t last_addr;
    static thread_local unsigned long long last_tsc;
    static thread_local int run;
    const unsigned long long now = __rdtsc();
    run = (addr == last_addr && now - last_tsc < (unsigned long long)(s_poll.tsc_per_us * 200)) ? run + 1 : 0;
    last_addr = addr;
    if (run >= s_poll.passes) {
        run = s_poll.passes / 2;
        if (s_poll.mwaitx)
            poll_mwaitx((unsigned)s_poll.cycles);
        else
            while (__rdtsc() - now < s_poll.cycles) _mm_pause();
    }
    last_tsc = __rdtsc();
#else
    (void)addr;
#endif
}
