/*
 * The EDGE geometry job as a whole (job module e8f44fbc6dbf, entry 0xAD98): the C job of
 * docs/notes/2026-11-04-edge-host-reference.md. GOWA_EDGE_JOB=1 turns the hook on (off by
 * default while it is being built).
 *
 * For now the hook only reads the job: it takes the job's input set apart, counts the kinds it
 * sees, and declines every job (the lifted job runs). GOWA_EDGE_NATIVE_STATS=1 prints the counts
 * every 65536 jobs.
 *
 * A job's input set, as the job module finds it when the job starts:
 *  - two buffer sets, 0x11000-0x1CFFF and 0x1D000-0x28FFF. The job module keeps the previous
 *    job's at LS 0x10918 (base) and 0x1091C (end); the job's own is the other one;
 *  - the DMA list, the set's last 176 bytes: 22 (size, EA) slots, each with one role; an empty
 *    slot takes no room, the others follow one another from the set's base, 16-byte aligned;
 *  - the descriptor at LS 0x0C00: words 0-13 are the job manager's (zero by now), 14-39 the job's.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t ej_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The pieces seen at the gate and elsewhere, as small numbers for the counts. */
static const uint32_t ej_out_pieces[] = { 0x765B70, 0x755B80, 0x762470, 0x756DE0 };
static const uint32_t ej_in_pieces[] = { 0x764D10, 0x75EB40, 0x75BD10, 0x758590, 0x764520, 0x75E180, 0x75AA50 };
static const uint32_t ej_meth_pieces[] = { 0x7661C0, 0x75FE80, 0x765170, 0x7613D0 };
#define EJ_NOUT (int)(sizeof ej_out_pieces / sizeof ej_out_pieces[0] + 1)
#define EJ_NIN (int)(sizeof ej_in_pieces / sizeof ej_in_pieces[0] + 1)
#define EJ_NMETH (int)(sizeof ej_meth_pieces / sizeof ej_meth_pieces[0] + 1)

static int ej_index(const uint32_t* t, int n, uint32_t ea)
{
    for (int i = 0; i < n; i++)
        if (t[i] == ea) return i;
    return n;
}

int ej_read(const spu_context* ctx, ej_view* v)
{
    const uint8_t* ls = ctx->ls;
    memset(v, 0, sizeof *v);
    const uint32_t prev = ej_be32(ls + 0x10918);
    v->base = prev == 0x11000 ? 0x1D000 : 0x11000;
    v->end = v->base + 0xC000;
    const uint8_t* lst = ls + v->end - 176;
    uint32_t a = v->base;
    for (int i = 0; i < EJ_SLOTS; i++) {
        const uint32_t sz = ej_be32(lst + 8 * i) & 0x7FFFFFFF, ea = ej_be32(lst + 8 * i + 4);
        if (!sz) continue;
        v->slot_lsa[i] = a;
        v->slot_size[i] = sz;
        v->slot_ea[i] = ea;
        a += (sz + 15) & ~15u;
    }
    if (a > v->end - 176) return EJ_BAD_LAYOUT;
    for (int i = 0; i < 40; i++) v->desc[i] = ej_be32(ls + 0xC00 + 4 * i);
    /* the slots every job has */
    static const int need[] = { 0, 1, 2, 3, 4, 5, 11, 13, 14, 16, 17, 18 };
    for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++)
        if (!v->slot_size[need[i]]) return EJ_BAD_LAYOUT;
    if (ej_be32(ls + v->slot_lsa[0]) != 0x90) return EJ_BAD_LAYOUT;       /* the light set's header */
    if (v->slot_size[13] != 16 || ls[v->slot_lsa[13]] != 0x87) return EJ_BAD_LAYOUT;
    v->out_piece = v->slot_ea[1];
    v->in_piece = v->slot_ea[2];
    v->meth_piece = v->slot_ea[3];
    v->skinned = v->slot_size[6] != 0;
    const uint8_t* h = ls + v->slot_lsa[13];
    v->nverts = (uint16_t)(h[8] << 8 | h[9]);
    v->nidx = (uint16_t)(h[10] << 8 | h[11]);
    if (v->out_piece == 0x765B70) v->kind = v->skinned ? EJ_POS_SKIN : EJ_POS;
    else if (v->out_piece == 0x755B80) v->kind = v->skinned ? EJ_LIT_SKIN : EJ_LIT;
    else v->kind = EJ_OTHER;
    return 0;
}

/* ---------------------------------------------------------------- the counts */
static volatile long long s_jobs, s_bad;
static volatile long long s_kind[EJ_KINDS];
static volatile long long s_combo[EJ_NOUT][EJ_NIN][EJ_NMETH][2];

static void ej_stats(void)
{
    static const char* const kn[EJ_KINDS] = { "position only", "position only, skinned", "lit", "lit, skinned", "other" };
    fprintf(stderr, "[edge-job] %lld jobs: %lld not read (layout)", s_jobs, s_bad);
    for (int k = 0; k < EJ_KINDS; k++) fprintf(stderr, ", %s %lld", kn[k], s_kind[k]);
    fprintf(stderr, "\n");
    for (int o = 0; o < EJ_NOUT; o++)
        for (int i = 0; i < EJ_NIN; i++)
            for (int m = 0; m < EJ_NMETH; m++)
                for (int sk = 0; sk < 2; sk++) {
                    const long long n = s_combo[o][i][m][sk];
                    if (!n) continue;
                    fprintf(stderr, "[edge-job]   out %06X in %06X methods %06X%s: %lld\n",
                            o < EJ_NOUT - 1 ? ej_out_pieces[o] : 0, i < EJ_NIN - 1 ? ej_in_pieces[i] : 0,
                            m < EJ_NMETH - 1 ? ej_meth_pieces[m] : 0, sk ? ", skinned" : "", n);
                }
}

int gowa_edge_job_ad98(spu_context* ctx)
{
    static int on = -1, stats;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_JOB");
        stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        on = e && e[0] == '1';
        if (on && stats) atexit(ej_stats);
    }
    if (!on) return 0;
    ej_view v;
    const int bad = ej_read(ctx, &v);
    const long long n = __atomic_add_fetch(&s_jobs, 1, __ATOMIC_RELAXED);
    if (bad) __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(&s_kind[v.kind], 1, __ATOMIC_RELAXED);
        const int o = ej_index(ej_out_pieces, EJ_NOUT - 1, v.out_piece);
        const int i = ej_index(ej_in_pieces, EJ_NIN - 1, v.in_piece);
        const int m = ej_index(ej_meth_pieces, EJ_NMETH - 1, v.meth_piece);
        __atomic_add_fetch(&s_combo[o][i][m][v.skinned], 1, __ATOMIC_RELAXED);
    }
    if (stats && !(n & 0xFFFF)) ej_stats();
    return 0;    /* not yet: the lifted job runs */
}
