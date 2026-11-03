/*
 * GOWA_EDGE_VERIFY=1 (measurement only): a native routine that takes a whole lifted function
 * (a "?name" hook in scripts/spu_native_hooks.json) is checked against the lifted function on
 * every call, on the game's own data.
 *
 * At the function's entry the hook copies the context (registers and local store), runs the
 * lifted function on the real one (its own hook lets it through, by the thread's guard), runs
 * the native routine on the copy, and compares them:
 *  - the local store, but for the stack below the caller's r1 (the lifted function's frame);
 *  - r0, r1 and the callee-saved r80-r127, and r3 for a function with a result (all 128 for a
 *    routine that leaves every register as the lifted function does: all_regs).
 * The game goes on with the lifted result. The counts and the first differences are printed
 * about every 4096 calls and at exit.
 */
#ifndef GOWA_SPU_EDGE_VERIFY_H
#define GOWA_SPU_EDGE_VERIFY_H

#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

typedef struct {
    const char* name;
    uint32_t stack_bytes;     /* the lifted function's frame and spills below the caller's r1 */
    int has_result;           /* r3 is the function's result */
    volatile long long calls, declined, ls_differ, reg_differ;
    volatile long printed;
    int all_regs;             /* compare all 128 registers (a leaf whose callers spill what it leaves) */
} edge_vstat;

static int edge_verify_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GOWA_EDGE_VERIFY"); v = e && e[0] == '1'; }
    return v;
}

static void edge_vreport(const edge_vstat* st)
{
    fprintf(stderr, "[edge-verify] %s: %lld calls checked, %lld declined by the native routine, "
                    "%lld with the local store different, %lld with registers different\n",
            st->name, st->calls, st->declined, st->ls_differ, st->reg_differ);
}

static void edge_vdiff(edge_vstat* st, const char* what, uint32_t at, const uint8_t* want, const uint8_t* got)
{
    if (__atomic_add_fetch(&st->printed, 1, __ATOMIC_RELAXED) > 12) return;
    fprintf(stderr, "[edge-verify] %s: %s %05X lifted", st->name, what, at);
    for (int i = 0; i < 16; i++) fprintf(stderr, "%s%02X", (i & 3) ? "" : " ", want[i]);
    fprintf(stderr, " native");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%s%02X", (i & 3) ? "" : " ", got[i]);
    fprintf(stderr, "\n");
}

/* The whole check; 1 when the call is done (the lifted function ran on ctx). */
static int edge_verify(spu_context* ctx, edge_vstat* st, int (*native)(spu_context*),
                       void (*lifted)(spu_context*), spu_context** guard)
{
    /* one copy per level: a checked function whose lifted run calls another checked one (the
     * culling calls the vertex pass) must not lose its copy to the inner check */
    static SPU_THREAD_LOCAL spu_context* copies[4];
    static SPU_THREAD_LOCAL int depth;
    if (depth >= 4) return 0;
    spu_context* copy = copies[depth];
    if (!copy) {
#ifdef _WIN32
        copy = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
#else
        copy = (spu_context*)aligned_alloc(64, (sizeof(spu_context) + 63) & ~(size_t)63);
#endif
        if (!copy) return 0;
        copies[depth] = copy;
    }
    depth++;
    memcpy(copy, ctx, sizeof *copy);
    memcpy(copy->ls_store, ctx->ls, SPU_LS_SIZE);
    copy->ls = copy->ls_store;

    const uint32_t link = ctx->gpr[0]._u32[0];
    *guard = ctx;
    ctx->host_depth++;
    lifted(ctx);
    spu_drain_call(ctx, link);
    ctx->host_depth--;
    *guard = NULL;

    const long long n = __atomic_add_fetch(&st->calls, 1, __ATOMIC_RELAXED);
    if (!native(copy)) {
        __atomic_add_fetch(&st->declined, 1, __ATOMIC_RELAXED);
    } else {
        const uint32_t top = copy->gpr[1]._u32[0] & (SPU_LS_SIZE - 16);
        const uint32_t low = top > st->stack_bytes ? top - st->stack_bytes : 0;
        int ls_bad = 0, reg_bad = 0;
        for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16) {
            if (a >= low && a < top) continue;
            if (memcmp(ctx->ls + a, copy->ls + a, 16)) {
                if (!ls_bad) edge_vdiff(st, "LS", a, ctx->ls + a, copy->ls + a);
                ls_bad = 1;
            }
        }
        for (int r = 0; r < 128; r++) {
            if (!st->all_regs && !(r == 0 || r == 1 || r >= 80 || (r == 3 && st->has_result))) continue;
            if (memcmp(ctx->gpr[r]._u32, copy->gpr[r]._u32, 16)) {
                if (!reg_bad) {
                    uint8_t w[16], g[16];
                    memcpy(w, ctx->gpr[r]._u32, 16);
                    memcpy(g, copy->gpr[r]._u32, 16);
                    edge_vdiff(st, "register", (uint32_t)r, w, g);
                }
                reg_bad = 1;
            }
        }
        if (ls_bad) __atomic_add_fetch(&st->ls_differ, 1, __ATOMIC_RELAXED);
        if (reg_bad) __atomic_add_fetch(&st->reg_differ, 1, __ATOMIC_RELAXED);
    }
    if ((n & 4095) == 0) edge_vreport(st);
    depth--;
    return 1;
}

#endif
