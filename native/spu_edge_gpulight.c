/*
 * EDGE's lighting on the GPU, phase 2b.1 (docs/notes/2026-11-04-phase-2b-plan.md): the SPU side,
 * and the pass the D3D12 backend runs on a lit draw's vertex copy.
 *
 * GOWA_EDGE_GPU:
 *  - 2, the default in the game: the SPU skips the lighting stage (0x73E0), and the backend lights
 *    the draws (the gate, ABBA, two rounds: 19.41 -> 20.69 flips/s, +6.6%);
 *  - 1, the check: the SPU lights as before and the record is published as already applied; the
 *    backend lights its copy too and compares (VTX_LIT_CHECK);
 *  - 0: off. The default with SPU_JOB_REPLAY or SPU_JOB_CAPTURE: a replay is compared with what
 *    the SPU's lighting put out, and a capture records it. GOWA_EDGE_VERIFY's check of the lighting
 *    natives needs it too (with 2 the stage does not run).
 *
 * The record (eg_rec) is taken at 0x73E0's entry, from its arguments: the light set's counts
 * n0 n1 n2, the flags (r13), the intensity (r14), and each light's position (w: 1/radius),
 * colour (w: a weight) and direction, as host floats. The output stage native publishes it after
 * the vertex PUT, keyed by the vertexes' IO offset (rsx_d3d12_vertex_pass_publish): the hole that
 * draws them is put after that, so the backend has the record before the draw. A job without a
 * record clears its key, and a record carries the first 16 bytes the job put out there, which the
 * backend finds in guest memory before it takes the record: an older job's record at the same
 * place in the ring is never taken.
 *
 * The stage writes ids 27-30 (the output piece packs them into the 54-byte layout at +24, +36,
 * +42, +48). Per vertex, from its position V (id 1) and S, the normal it reads (the eighth array:
 * id3.w * normalize(cross(id3, id4)), rebuilt here from the ring's I16N normal and X11Y11Z10
 * tangent):
 *  - the first n0 + n1 lights: C' = intensity * colour, d = P.xyz - V, L = the light's direction
 *    for the first n0, else d / |d|; t = 1 - min(|d| P.w, 1), att = (t^2 (3 - 2t))^2 C'.w; the
 *    weight |att| min(|S.L / 2 + 1/2|, 1) (|d| from |d|^2 + 1e-4);
 *  - id 27 = V + A / WS: A the weighted sum of the L, WS the sum of the weights from 1e-4;
 *  - D = A / sqrt(|A|^2 + 1e-4); each light's C'.xyz att times min(D.L, 1) where D.L > 0: id 28
 *    sums the positive parts, id 29 the negative ones (as positive numbers);
 *  - id 30, the last n2 (point lights): sum of (a^2 (3 - 2a))^2 C'.w C'.xyz, a = 1 - min(|d| P.w,
 *    1), from 0.5 when the flags have 0x100;
 *  - F16 by the output piece's rule (toward zero, 0x7BFF above, +0 below 2^-15).
 * Mapped from a dump of the gate replay's 41 lit calls and checked there in plain float math:
 * 86% of the F16 values the same as the SPU's, 11.5% one unit apart.
 *
 * Taken only on the gate's path: the context at 0x107A0, the lit layout (ids 1 3 4 27-30 at
 * c + 0x160, their arrays in the arguments), r5 set, r8 = 0, the flags without 0x800, at most
 * EG_MAX_LIGHTS lights. Anything else runs on the SPU with no record, and the backend draws what
 * the SPU wrote.
 *
 * A skipped stage that the output stage cannot publish for (it runs lifted: an interrupt pending,
 * a stride callback) is run at the output stage's entry instead, from the registers it was called
 * with (edge_gpul_unskip): nothing between the two reads what lighting writes or changes what it
 * reads. GOWA_EDGE_GPU_DEFER=1 does that for every skipped stage, a test: the jobs' output must be
 * what GOWA_EDGE_GPU=0 gives.
 *
 * GOWA_EDGE_GPU_SELFTEST=1 with GOWA_EDGE_GPU=1 checks the C version of the pass (the backend's
 * fallback) against the SPU: at the publish, over the packed vertexes the job is putting out, for
 * the vertexes the stage lit (its list, in the eighth array's fourth words); counts at exit.
 *
 * With the stage skipped (GOWA_EDGE_GPU_LEAN=0 keeps them) two things before it have no reader:
 *  - the binormals, 0xA880's loop at 0xA950 into the eighth array: its result is the stage's r10
 *    and nothing else's. Its hook at 0xA928, with the array in place, leaves the loop out and
 *    returns as 0xA880 does. Unless the stage ends up skipped, 0xA880 runs after all, from its
 *    arguments (eg_binorm_late);
 *  - ids 27-30 in the output piece: the pass writes over them. It packs bytes 0-23 of a vertex
 *    (spu_edge_cmp.c); the rest of the 54 are whatever the local store held.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_memo.h"
#include "spu_edge_gpuskin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EG_MAX_LIGHTS 32
#define EG_STRIDE     54

/* the lit layout: the attribute ids at c + 0x160 */
static const uint8_t eg_ids[8] = { 1, 3, 4, 27, 28, 29, 30, 0 };

/* the record, as the backend copies it for the shader (host order, 4-byte words) */
typedef struct {
    uint32_t n0, n1, n2, flags;
    float    inten[4];
    float    light[EG_MAX_LIGHTS][3][4];     /* position, colour, direction */
} eg_rec;
#define EG_REC_BYTES(n) (32u + 48u * (uint32_t)(n))

/* the backend's vertex pass (libs/video/rsx_d3d12_backend.c) */
extern void rsx_d3d12_vertex_pass_set(const char* hlsl, uint32_t stride, uint32_t max_record,
                                      void (*cpu)(const void* rec, const uint8_t* src, uint8_t* dst, uint32_t n));
extern void rsx_d3d12_vertex_pass_publish(uint32_t io_offset, uint32_t nverts, const void* rec,
                                          uint32_t bytes, uint32_t applied, const void* tag16,
                                          const void* skin, uint32_t skin_bytes);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0(spu_context* ctx);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000A880(spu_context* ctx);

static void eg_cpu(const void* rec, const uint8_t* src, uint8_t* dst, uint32_t n);
static const char* eg_hlsl(void);
static void eg_selftest(const uint8_t* ls, const eg_rec* r, uint32_t n);

static volatile long long s_eg_taken, s_eg_skipped, s_eg_declined, s_eg_published, s_eg_unskipped, s_eg_lost;
static volatile long long s_eg_binorm, s_eg_binorm_late;
static int s_eg_stats;
#define EG_COUNT(v) do { if (s_eg_stats) __atomic_add_fetch(&(v), 1, __ATOMIC_RELAXED); } while (0)
static void eg_report(void)
{
    fprintf(stderr, "[edge-gpu] lighting: %lld records taken (%lld stages skipped), %lld declined; "
            "%lld published, %lld skipped stages run late, %lld records dropped; "
            "binormals left out %lld, %lld of those run after all\n",
            s_eg_taken, s_eg_skipped, s_eg_declined, s_eg_published, s_eg_unskipped, s_eg_lost,
            s_eg_binorm, s_eg_binorm_late);
}

static int eg_mode(void)
{
    static volatile int mode = -1;
    static volatile int lock;
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) { }
    m = mode;
    if (m < 0) {
        const char* e = getenv("GOWA_EDGE_GPU");
        m = e ? atoi(e) : (getenv("SPU_JOB_REPLAY") || getenv("SPU_JOB_CAPTURE")) ? 0 : 2;
        if (m < 0 || m > 2) m = 0;
        /* the record is published by the output stage native: without it, nothing */
        const char* nat = getenv("GOWA_EDGE_NATIVE");
        if (nat && (atoi(nat) & 131072) == 0) m = 0;
        s_eg_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        if (m) {
            rsx_d3d12_vertex_pass_set(eg_hlsl(), EG_STRIDE, (uint32_t)(sizeof(eg_rec) + sizeof(ek_rec)), eg_cpu);
            if (s_eg_stats) atexit(eg_report);
            fprintf(stderr, "[edge-gpu] lighting on the GPU: mode %d (%s)\n", m,
                    m == 1 ? "check: the SPU lights too" : "the SPU skips it");
        }
        __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return m;
}

/* what a lit job carries from its lighting stage to its output stage */
static SPU_THREAD_LOCAL struct {
    int      state;            /* 0 none; 1 a record (the stage ran); 2 a record, the stage skipped */
    uint32_t n;                /* lights */
    uint8_t  head[16];         /* c + 0x00 (the job's segment header): the job the record is of */
    u128     regs[16];         /* r0-r15 at the stage's entry, to run it late */
    int      binorm;           /* 0xA880's loop left out; its arguments */
    u128     binorm_c, binorm_n;
    eg_rec   rec;
} t_eg;
static SPU_THREAD_LOCAL int t_eg_late;    /* running a skipped stage late: no record, no skip */

static inline uint32_t eg_be32(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & (SPU_LS_SIZE - 1));
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int eg_lean(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GOWA_EDGE_GPU_LEAN"); v = !(e && e[0] == '0'); }
    return v;
}

/* 0xA880's loop is about to run (its hook at 0xA928, the array in place), from the stage driver:
 * leave it out and return as the function does at 0xAD48. It runs after all unless the stage is
 * skipped, which also proves the lit layout (ids 27-30 are not in the list yet here): at the stage's
 * entry when it is declined, or before the packing when the driver does not call it (no lights). */
int gowa_edge_binorm_a928(spu_context* ctx)
{
    if (eg_mode() != 2 || t_eg_late || !eg_lean()) return 0;
    const uint32_t sp = ctx->gpr[1]._u32[0];
    t_eg.binorm = 0;
    if (ctx->gpr[80]._u32[0] != 0x107A0 || eg_be32(ctx->ls, sp + 0x80) != 0x54F8) return 0;
    t_eg.binorm = 1;
    t_eg.binorm_c = ctx->gpr[80];
    t_eg.binorm_n = ctx->gpr[83];
    EG_COUNT(s_eg_binorm);
    ctx->gpr[3] = ctx->gpr[53];
    ctx->gpr[0] = spu_ls_read128(ctx, sp + 0x80);
    ctx->gpr[1] = spu_ai(ctx->gpr[1], 112);
    const uint32_t s = sp + 112;
    ctx->gpr[126] = spu_ls_read128(ctx, s - 0x50);
    ctx->gpr[80] = spu_ls_read128(ctx, s - 0x10);
    ctx->gpr[81] = spu_ls_read128(ctx, s - 0x20);
    ctx->gpr[82] = spu_ls_read128(ctx, s - 0x30);
    ctx->gpr[83] = spu_ls_read128(ctx, s - 0x40);
    return 1;
}

/* The stage runs on the SPU after all: the binormals first, all registers kept (all below the
 * stack pointer is free at the stage's entry and the output stage's). */
static void eg_binorm_late(spu_context* ctx)
{
    if (!t_eg.binorm) return;
    t_eg.binorm = 0;
    u128 regs[128];
    memcpy(regs, ctx->gpr, sizeof regs);
    ctx->gpr[3] = t_eg.binorm_c;
    ctx->gpr[4] = t_eg.binorm_n;
    EG_COUNT(s_eg_binorm_late);
    ctx->gpr[0] = spu_link(0x54F8);
    const int ie = ctx->int_enable;
    ctx->int_enable = 0;
    const int late = t_eg_late;
    t_eg_late = 1;
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_0000A880(ctx);
    spu_drain_call(ctx, 0x54F8);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_eg_late = late;
    ctx->int_enable = ie;
    memcpy(ctx->gpr, regs, sizeof regs);
}

/* the output stage native, before the packing: a job with no stage skipped has its binormals */
void edge_gpul_binorm_now(spu_context* ctx)
{
    if (t_eg.state != 2) { edge_memo_revoke_lit(ctx); eg_binorm_late(ctx); }
}

/* spu_edge_memo.c: the stage is skipped in this run, and the bytes it leaves are the pass's */
int edge_gpul_lean_on(void)
{
    return eg_mode() == 2 && eg_lean();
}

/* the output piece (spu_edge_cmp.c): the stage is skipped, and the pass will write ids 27-30 */
int edge_gpul_lean_pack(void)
{
    return t_eg.state == 2 && eg_lean();
}

static inline float eg_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* What the stage writes that is put out besides ids 27-30: the index list (u16, at c + 0x50's first
 * word, its count at c + 0x190) padded to a multiple of 8 with its first index, for the stage's
 * own marking loop (eight a pass); 0xB670 returns the list, the stage's start stores the padded
 * quadword. The output stage puts the list out, so a skipped stage pads it too. */
static void eg_pad_indexes(uint8_t* ls, uint32_t c)
{
    const uint32_t ib = eg_be32(ls, c + 0x50);
    const uint32_t cnt = ((uint32_t)ls[c + 0x190] << 8) | ls[c + 0x191];
    const uint32_t rem = cnt & 7u;
    if (!cnt || !rem) return;
    const uint32_t q = (ib + 2u * cnt - 2u) & (SPU_LS_SIZE - 16u);
    const uint8_t i0 = ls[ib & (SPU_LS_SIZE - 1)], i1 = ls[(ib + 1) & (SPU_LS_SIZE - 1)];
    for (uint32_t k = rem; k < 8; k++) { ls[q + 2 * k] = i0; ls[q + 2 * k + 1] = i1; }
}

/* 0x73E0's entry (its hook, gowa_edge_light_73e0): take the record; 1 = the stage is skipped */
int edge_gpul_capture(spu_context* ctx)
{
    const int mode = eg_mode();
    if (!mode || t_eg_late) return 0;
    t_eg.state = 0;
    const uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0], r5 = ctx->gpr[5]._u32[0], r8 = ctx->gpr[8]._u32[0];
    const uint32_t r12 = ctx->gpr[12]._u32[0], flags = ctx->gpr[13]._u32[0];
    int ok = c == 0x107A0 && r5 != 0 && r8 == 0 && (flags & 0x800) == 0 && !memcmp(ls + c + 0x160, eg_ids, 8);
    /* the arguments name the pool's arrays: r9 id 1, r4-r7 ids 27-30 */
    if (ok)
        ok = ctx->gpr[9]._u32[0] == eg_be32(ls, c + 0x10) && ctx->gpr[4]._u32[0] == eg_be32(ls, c + 0x1C) &&
             r5 == eg_be32(ls, c + 0x20) && ctx->gpr[6]._u32[0] == eg_be32(ls, c + 0x24) &&
             ctx->gpr[7]._u32[0] == eg_be32(ls, c + 0x28);
    uint32_t n0 = 0, n1 = 0, n2 = 0, o0 = 0, o1 = 0, o2 = 0;
    if (ok) {
        o0 = eg_be32(ls, r12); o1 = eg_be32(ls, r12 + 4); o2 = eg_be32(ls, r12 + 8);
        n0 = eg_be32(ls, r12 + 0x10); n1 = eg_be32(ls, r12 + 0x14); n2 = eg_be32(ls, r12 + 0x18);
        const uint64_t n = (uint64_t)n0 + n1 + n2;
        ok = n <= EG_MAX_LIGHTS && r12 < SPU_LS_SIZE &&
             (uint64_t)r12 + o0 + 16 * n <= SPU_LS_SIZE && (uint64_t)r12 + o1 + 16 * n <= SPU_LS_SIZE &&
             (uint64_t)r12 + o2 + 16 * n <= SPU_LS_SIZE;
    }
    if (!ok) { EG_COUNT(s_eg_declined); edge_memo_revoke_lit(ctx); eg_binorm_late(ctx); return 0; }
    const uint32_t n = n0 + n1 + n2;
    eg_rec* r = &t_eg.rec;
    r->n0 = n0; r->n1 = n1; r->n2 = n2; r->flags = flags;
    for (int k = 0; k < 4; k++) r->inten[k] = eg_f(ctx->gpr[14]._u32[k]);
    for (uint32_t j = 0; j < n; j++)
        for (int k = 0; k < 4; k++) {
            r->light[j][0][k] = eg_f(eg_be32(ls, r12 + o0 + 16 * j + 4 * k));
            r->light[j][1][k] = eg_f(eg_be32(ls, r12 + o1 + 16 * j + 4 * k));
            r->light[j][2][k] = eg_f(eg_be32(ls, r12 + o2 + 16 * j + 4 * k));
        }
    t_eg.n = n;
    memcpy(t_eg.head, ls + c, 16);
    memcpy(t_eg.regs, ctx->gpr, sizeof t_eg.regs);
    t_eg.state = mode == 2 ? 2 : 1;
    EG_COUNT(s_eg_taken);
    if (mode == 2) {
        EG_COUNT(s_eg_skipped);
        eg_pad_indexes(ctx->ls, c);
    }
    return mode == 2;
}

/* the output stage native, at its entry: a job that will publish keeps its record */
int edge_gpul_defer_test(void)
{
    static int d = -1;
    if (d < 0) { const char* e = getenv("GOWA_EDGE_GPU_DEFER"); d = e && e[0] == '1'; }
    return d;
}

/* The output stage runs lifted, or has nothing to draw: a skipped stage runs now, from the
 * registers it was called with (all below the stack pointer is free at the stage's entry), with
 * interrupts held off; the record is dropped. */
void edge_gpul_unskip(spu_context* ctx)
{
    if (t_eg.state != 2) {
        t_eg.state = 0;
        edge_memo_revoke_lit(ctx);
        eg_binorm_late(ctx);
        return;
    }
    t_eg.state = 0;
    EG_COUNT(s_eg_unskipped);
    edge_memo_revoke_lit(ctx);
    eg_binorm_late(ctx);
    u128 regs[128];
    memcpy(regs, ctx->gpr, sizeof regs);
    for (int r = 2; r < 16; r++) ctx->gpr[r] = t_eg.regs[r];
    ctx->gpr[0] = spu_link(0x55C0);
    const int ie = ctx->int_enable;
    ctx->int_enable = 0;
    t_eg_late = 1;
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++;
    jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0(ctx);
    spu_drain_call(ctx, 0x55C0);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_eg_late = 0;
    ctx->int_enable = ie;
    memcpy(ctx->gpr, regs, sizeof regs);
}

/* The output stage native, after the vertex PUT: the vertexes at IO offset vio, n of them, with
 * the given stride and attribute list. Every job's: one without a record clears the key. */
/* What edge_gpul_publish needs of a job whose vertexes it will not see in the local store. */
int edge_gpul_can_publish(const spu_context* ctx, uint32_t stride)
{
    const uint8_t* ls = ctx->ls;
    return t_eg.state == 2 && stride == EG_STRIDE && !memcmp(ls + 0x107A0 + 0x160, eg_ids, 8) &&
           !memcmp(ls + 0x107A0, t_eg.head, 16);
}

void edge_gpul_publish(spu_context* ctx, uint32_t vio, uint32_t nverts, uint32_t stride)
{
    if (!eg_mode()) return;
    t_eg.binorm = 0;
    if (!t_eg.state) { rsx_d3d12_vertex_pass_publish(vio, 0, NULL, 0, 0, NULL, NULL, 0); return; }
    const int applied = t_eg.state == 1;
    t_eg.state = 0;
    const uint8_t* ls = ctx->ls;
    const uint32_t buf = eg_be32(ls, 0x107A0 + 0x54);      /* what the vertex PUT took (c + 0x50, word 1) */
    if (stride != EG_STRIDE || memcmp(ls + 0x107A0 + 0x160, eg_ids, 8) || memcmp(ls + 0x107A0, t_eg.head, 16) ||
        buf > SPU_LS_SIZE - 16) {
        /* not the job or the layout the record was taken for */
        EG_COUNT(s_eg_lost);
        rsx_d3d12_vertex_pass_publish(vio, 0, NULL, 0, 0, NULL, NULL, 0);
        return;
    }
    uint32_t skin_bytes = 0;
    const void* const skin = edge_memo_skin_part(&skin_bytes);
    rsx_d3d12_vertex_pass_publish(vio, nverts, &t_eg.rec, EG_REC_BYTES(t_eg.n), (uint32_t)applied, ls + buf, skin, skin_bytes);
    EG_COUNT(s_eg_published);
    if (applied) {
        static int st = -1;
        if (st < 0) { const char* e = getenv("GOWA_EDGE_GPU_SELFTEST"); st = e && e[0] == '1'; }
        if (st) eg_selftest(ctx->ls, &t_eg.rec, nverts);
    }
}

/* ---------------------------------------------------------------------------------------------
 * The pass on the CPU: the backend's fallback (a draw that does not take the raw fetch, or a full
 * lit buffer). n vertexes of the 54-byte layout from src to dst (may be the same), big-endian. */
static inline uint32_t eg_rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void eg_wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline int eg_rds16(const uint8_t* p) { return (int16_t)(((uint32_t)p[0] << 8) | p[1]); }
static inline void eg_wr16(uint8_t* p, uint32_t h) { p[0] = (uint8_t)(h >> 8); p[1] = (uint8_t)h; }
static inline uint32_t eg_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* the output piece's F16 rule */
static uint32_t eg_f16(float x)
{
    const uint32_t b = eg_u(x), hi = (b >> 16) & 0x7FFFu;
    if (hi >= 0x4780u) return 0x7BFFu | ((b >> 16) & 0x8000u);
    if (hi < 0x3800u) return 0;
    return ((((b & 0x7FFFFFFFu) - 0x38000000u) >> 13) & 0x7FFFu) | ((b >> 16) & 0x8000u);
}

static void eg_cpu(const void* rv, const uint8_t* src, uint8_t* dst, uint32_t nv)
{
    const eg_rec* r = (const eg_rec*)rv;
    const float EPS = eg_f(0x38D1B717u);
    const uint32_t n0 = r->n0, n01 = r->n0 + r->n1;
    uint32_t nall = r->n0 + r->n1 + r->n2;
    if (nall > EG_MAX_LIGHTS) nall = EG_MAX_LIGHTS;
    const float* I = r->inten;
    for (uint32_t v = 0; v < nv; v++) {
        const uint8_t* s = src + (size_t)v * EG_STRIDE;
        uint8_t* o = dst + (size_t)v * EG_STRIDE;
        if (o != s) memcpy(o, s, EG_STRIDE);
        const float V[3] = { eg_f(eg_rd32(s)), eg_f(eg_rd32(s + 4)), eg_f(eg_rd32(s + 8)) };
        const float N[3] = { (eg_rds16(s + 12) + 0.5f) / 32767.5f, (eg_rds16(s + 14) + 0.5f) / 32767.5f,
                             (eg_rds16(s + 16) + 0.5f) / 32767.5f };
        const float nw = eg_rds16(s + 18) < 0 ? -1.0f : 1.0f;
        const uint32_t tw = eg_rd32(s + 20);
        const float T[3] = { (float)(((int32_t)(tw << 21)) >> 21) / 1023.0f, (float)(((int32_t)(tw << 10)) >> 21) / 1023.0f,
                             (float)(((int32_t)tw) >> 22) / 511.0f };
        float S[3] = { N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2], N[0] * T[1] - N[1] * T[0] };
        const float l2 = S[0] * S[0] + S[1] * S[1] + S[2] * S[2];
        const float sc = l2 > 1e-20f ? nw / sqrtf(l2) : 0.0f;
        S[0] *= sc; S[1] *= sc; S[2] *= sc;
        float WS = EPS, A[3] = { 0, 0, 0 };
        for (int pass = 0; pass < 2; pass++) {
            float D[3] = { 0, 0, 0 }, pos[3] = { 0, 0, 0 }, neg[3] = { 0, 0, 0 };
            if (pass) {
                const float d2 = A[0] * A[0] + A[1] * A[1] + A[2] * A[2] + EPS;
                const float rs = 1.0f / sqrtf(d2);
                D[0] = A[0] * rs; D[1] = A[1] * rs; D[2] = A[2] * rs;
            }
            for (uint32_t j = 0; j < n01 && j < EG_MAX_LIGHTS; j++) {
                const float* P = r->light[j][0];
                const float Cp[4] = { I[0] * r->light[j][1][0], I[1] * r->light[j][1][1], I[2] * r->light[j][1][2], I[3] * r->light[j][1][3] };
                const float d[3] = { P[0] - V[0], P[1] - V[1], P[2] - V[2] };
                const float sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + EPS;
                const float rs = 1.0f / sqrtf(sq);
                const float L[3] = { j < n0 ? r->light[j][2][0] : d[0] * rs, j < n0 ? r->light[j][2][1] : d[1] * rs,
                                     j < n0 ? r->light[j][2][2] : d[2] * rs };
                const float m = fminf(sq * rs * P[3], 1.0f), t = 1.0f - m;
                const float sm = t * t * (3.0f - 2.0f * t), att = sm * sm * Cp[3];
                if (!pass) {
                    const float H = fminf(fabsf((S[0] * L[0] + S[1] * L[1] + S[2] * L[2]) * 0.5f + 0.5f), 1.0f);
                    const float W = fabsf(att) * H;
                    A[0] += L[0] * W; A[1] += L[1] * W; A[2] += L[2] * W; WS += W;
                } else {
                    const float nl = D[0] * L[0] + D[1] * L[1] + D[2] * L[2];
                    const float l = nl > 0.0f ? fminf(nl, 1.0f) : 0.0f;
                    for (int k = 0; k < 3; k++) {
                        const float x = Cp[k] * att * l;
                        if (x > 0.0f) pos[k] += x; else if (x < 0.0f) neg[k] -= x;
                    }
                }
            }
            if (!pass) {
                const float inv = 1.0f / WS;
                for (int k = 0; k < 3; k++) eg_wr32(o + 24 + 4 * k, eg_u(A[k] * inv + V[k]));
            } else {
                for (int k = 0; k < 3; k++) {
                    eg_wr16(o + 36 + 2 * k, eg_f16(pos[k]));
                    eg_wr16(o + 42 + 2 * k, eg_f16(neg[k]));
                }
            }
        }
        float pt[3];
        pt[0] = pt[1] = pt[2] = (r->flags & 0x100u) ? 0.5f : 0.0f;
        for (uint32_t j = n01; j < nall; j++) {
            const float* P = r->light[j][0];
            const float Cp[4] = { I[0] * r->light[j][1][0], I[1] * r->light[j][1][1], I[2] * r->light[j][1][2], I[3] * r->light[j][1][3] };
            const float d[3] = { P[0] - V[0], P[1] - V[1], P[2] - V[2] };
            const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float a = 1.0f - fminf(dist * P[3], 1.0f);
            const float s = a * a * (3.0f - 2.0f * a), w = s * s * Cp[3];
            for (int k = 0; k < 3; k++) pt[k] += w * Cp[k];
        }
        for (int k = 0; k < 3; k++) eg_wr16(o + 48 + 2 * k, eg_f16(pt[k]));
    }
}

/* ---------------------------------------------------------------------------------------------
 * The pass on the GPU (cs_5_0). The backend's vertex-pass contract: t0 the batch's raw copies
 * (vp_vb), t1 the batch's table, u0 the lit copies (vp_lit), b0 the dispatch's first segment; a
 * group a segment. A segment's entry, 32 bytes at index * 32 of t1: the source copy's header (16
 * attribute bases), the lit copy's header, the start of attribute 0's copy from the header, its
 * vertex count, the record's offset in t1, the segment's bytes from the header, where the vertexes
 * are if not in the copy (a kept block's, resident), and a skinned block's part of the record in t1
 * (bit 0: a position's block, 12-byte vertexes and no lights). The segment is copied word by word
 * but for attribute 0's vertexes, the header's bases moved to the lit copy; then a thread does two
 * vertexes (108 bytes, 27 words, 4-aligned since the copy starts 16-aligned). A skinned block's
 * vertexes are worked out first (spu_edge_gpuskin.c). */
static const char s_eg_hlsl[] =
    "ByteAddressBuffer vsrc : register(t0);\n"
    "ByteAddressBuffer vtab : register(t1);\n"
    "RWByteAddressBuffer vdst : register(u0);\n"
    "cbuffer VPass : register(b0) { uint seg_base; uint3 vp_pad; };\n"
    "#define MAXL 32\n"
    "groupshared float4 gP[MAXL];\n"
    "groupshared float4 gC[MAXL];\n"
    "groupshared float4 gQ[MAXL];\n"
    "groupshared uint4 gHead;\n"
    "groupshared float4 gInt;\n"
    "uint bs(uint x) { return (x << 24) | ((x << 8) & 0xFF0000u) | ((x >> 8) & 0xFF00u) | (x >> 24); }\n"
    "uint rd32(uint w[27], uint o) {\n"
    "    uint i = o >> 2; uint v;\n"
    "    if ((o & 3u) == 0u) v = w[i]; else v = (w[i] >> 16) | (w[i + 1] << 16);\n"
    "    return bs(v);\n"
    "}\n"
    "int rds16(uint w[27], uint o) {\n"
    "    uint i = o >> 2; uint v;\n"
    "    if ((o & 2u) != 0u) v = w[i] >> 16; else v = w[i] & 0xFFFFu;\n"
    "    v = ((v & 0xFFu) << 8) | (v >> 8);\n"
    "    return ((int)(v << 16)) >> 16;\n"
    "}\n"
    "void wr32(inout uint w[27], uint o, uint x) {\n"
    "    uint i = o >> 2; uint v = bs(x);\n"
    "    if ((o & 3u) == 0u) w[i] = v;\n"
    "    else { w[i] = (w[i] & 0xFFFFu) | (v << 16); w[i + 1] = (w[i + 1] & 0xFFFF0000u) | (v >> 16); }\n"
    "}\n"
    "void wr16(inout uint w[27], uint o, uint h) {\n"
    "    uint i = o >> 2; uint v = ((h & 0xFFu) << 8) | ((h >> 8) & 0xFFu);\n"
    "    if ((o & 2u) != 0u) w[i] = (w[i] & 0xFFFFu) | (v << 16); else w[i] = (w[i] & 0xFFFF0000u) | v;\n"
    "}\n"
    "uint f16spu(float x) {\n"
    "    uint b = asuint(x); uint hi = (b >> 16) & 0x7FFFu; uint sg = (b >> 16) & 0x8000u;\n"
    "    if (hi >= 0x4780u) return 0x7BFFu | sg;\n"
    "    if (hi < 0x3800u) return 0u;\n"
    "    return ((((b & 0x7FFFFFFFu) - 0x38000000u) >> 13) & 0x7FFFu) | sg;\n"
    "}\n"
    "void light_vertex(inout uint w[27], uint o) {\n"
    "    const float EPS = asfloat(0x38D1B717u);\n"
    "    float3 V = float3(asfloat(rd32(w, o)), asfloat(rd32(w, o + 4u)), asfloat(rd32(w, o + 8u)));\n"
    "    float3 N = (float3(rds16(w, o + 12u), rds16(w, o + 14u), rds16(w, o + 16u)) + 0.5) / 32767.5;\n"
    "    float nw = rds16(w, o + 18u) < 0 ? -1.0 : 1.0;\n"
    "    uint tw = rd32(w, o + 20u);\n"
    "    float3 T = float3((float)(((int)(tw << 21)) >> 21) / 1023.0, (float)(((int)(tw << 10)) >> 21) / 1023.0,\n"
    "                      (float)(((int)tw) >> 22) / 511.0);\n"
    "    float3 cr = cross(N, T);\n"
    "    float l2 = dot(cr, cr);\n"
    "    float3 S = l2 > 1e-20 ? cr * (nw * rsqrt(l2)) : float3(0.0, 0.0, 0.0);\n"
    "    uint n0 = gHead.x, n01 = min(gHead.x + gHead.y, (uint)MAXL), nall = min(gHead.x + gHead.y + gHead.z, (uint)MAXL);\n"
    "    float4 I = gInt;\n"
    "    float WS = EPS; float3 A = float3(0.0, 0.0, 0.0);\n"
    "    uint j;\n"
    "    for (j = 0u; j < n01; j++) {\n"
    "        float4 P = gP[j]; float4 Cp = gC[j] * I;\n"
    "        float3 d = P.xyz - V;\n"
    "        float sq = dot(d, d) + EPS; float rs = rsqrt(sq);\n"
    "        float3 L = j < n0 ? gQ[j].xyz : d * rs;\n"
    "        float t = 1.0 - min(sq * rs * P.w, 1.0);\n"
    "        float sm = t * t * (3.0 - 2.0 * t); float att = sm * sm * Cp.w;\n"
    "        float W = abs(att) * min(abs(dot(S, L) * 0.5 + 0.5), 1.0);\n"
    "        A += L * W; WS += W;\n"
    "    }\n"
    "    float3 Po = A * (1.0 / WS) + V;\n"
    "    float3 D = A * rsqrt(dot(A, A) + EPS);\n"
    "    float3 pos = float3(0.0, 0.0, 0.0), neg = float3(0.0, 0.0, 0.0);\n"
    "    for (j = 0u; j < n01; j++) {\n"
    "        float4 P = gP[j]; float4 Cp = gC[j] * I;\n"
    "        float3 d = P.xyz - V;\n"
    "        float sq = dot(d, d) + EPS; float rs = rsqrt(sq);\n"
    "        float3 L = j < n0 ? gQ[j].xyz : d * rs;\n"
    "        float t = 1.0 - min(sq * rs * P.w, 1.0);\n"
    "        float sm = t * t * (3.0 - 2.0 * t); float att = sm * sm * Cp.w;\n"
    "        float nl = dot(D, L);\n"
    "        float3 x = Cp.xyz * (att * (nl > 0.0 ? min(nl, 1.0) : 0.0));\n"
    "        pos += max(x, 0.0); neg += max(-x, 0.0);\n"
    "    }\n"
    "    float3 pt = (gHead.w & 0x100u) != 0u ? float3(0.5, 0.5, 0.5) : float3(0.0, 0.0, 0.0);\n"
    "    for (j = n01; j < nall; j++) {\n"
    "        float4 P = gP[j]; float4 Cp = gC[j] * I;\n"
    "        float3 d = P.xyz - V;\n"
    "        float a = 1.0 - min(sqrt(dot(d, d)) * P.w, 1.0);\n"
    "        float s = a * a * (3.0 - 2.0 * a);\n"
    "        pt += (s * s * Cp.w) * Cp.xyz;\n"
    "    }\n"
    "    wr32(w, o + 24u, asuint(Po.x)); wr32(w, o + 28u, asuint(Po.y)); wr32(w, o + 32u, asuint(Po.z));\n"
    "    wr16(w, o + 36u, f16spu(pos.x)); wr16(w, o + 38u, f16spu(pos.y)); wr16(w, o + 40u, f16spu(pos.z));\n"
    "    wr16(w, o + 42u, f16spu(neg.x)); wr16(w, o + 44u, f16spu(neg.y)); wr16(w, o + 46u, f16spu(neg.z));\n"
    "    wr16(w, o + 48u, f16spu(pt.x)); wr16(w, o + 50u, f16spu(pt.y)); wr16(w, o + 52u, f16spu(pt.z));\n"
    "}\n"
    "float4 ldf(uint a) { return asfloat(vtab.Load4(a)); }\n"
    "/* A bone the palette does not have is its first, as on the CPU (ek_blend). t1 is a root view, read\n"
    "   with no bounds: past the palette lies the next record, or past the half's end nothing. */\n"
    "uint bone(uint i, uint nm) { return i < nm ? i : 0u; }\n"
    "void skin_rows(uint sk, uint2 bb, out float4 R0, out float4 R1, out float4 R2) {\n"
    "    const float inv = asfloat(0x3B808080u);\n"
    "    uint pal = sk + 16u, nm = vtab.Load(sk + 4u);\n"
    "    float w = (float)(bb.x & 0xFFu) * inv;\n"
    "    uint a = pal + bone((bb.x >> 8) & 0xFFu, nm) * 48u;\n"
    "    R0 = w * ldf(a); R1 = w * ldf(a + 16u); R2 = w * ldf(a + 32u);\n"
    "    w = (float)((bb.x >> 16) & 0xFFu) * inv;\n"
    "    a = pal + bone(bb.x >> 24, nm) * 48u;\n"
    "    R0 = mad(w, ldf(a), R0); R1 = mad(w, ldf(a + 16u), R1); R2 = mad(w, ldf(a + 32u), R2);\n"
    "    w = (float)(bb.y & 0xFFu) * inv;\n"
    "    a = pal + bone((bb.y >> 8) & 0xFFu, nm) * 48u;\n"
    "    R0 = mad(w, ldf(a), R0); R1 = mad(w, ldf(a + 16u), R1); R2 = mad(w, ldf(a + 32u), R2);\n"
    "    w = (float)((bb.y >> 16) & 0xFFu) * inv;\n"
    "    a = pal + bone(bb.y >> 24, nm) * 48u;\n"
    "    R0 = mad(w, ldf(a), R0); R1 = mad(w, ldf(a + 16u), R1); R2 = mad(w, ldf(a + 32u), R2);\n"
    "}\n"
    "float3 skin_pos(float4 p, float4 R0, float4 R1, float4 R2) {\n"
    "    precise float3 o;\n"
    "    o.x = mad(p.z, R2.x, mad(p.y, R1.x, mad(p.x, R0.x, R0.w)));\n"
    "    o.y = mad(p.x, R2.y, mad(p.z, R1.y, mad(p.y, R0.y, R1.w)));\n"
    "    o.z = mad(p.y, R2.z, mad(p.x, R1.z, mad(p.z, R0.z, R2.w)));\n"
    "    return o;\n"
    "}\n"
    "float4 skin_rot(float4 v, float4 R0, float4 R1, float4 R2) {\n"
    "    float4 o;\n"
    "    o.x = mad(v.z, R2.x, mad(v.y, R1.x, v.x * R0.x));\n"
    "    o.y = mad(v.x, R2.y, mad(v.z, R1.y, v.y * R0.y));\n"
    "    o.z = mad(v.y, R2.z, mad(v.x, R1.z, v.z * R0.z));\n"
    "    o.w = v.w;\n"
    "    return o;\n"
    "}\n"
    "float4 skin_norm(float4 v) { return float4(v.xyz * rsqrt(dot(v.xyz, v.xyz)), v.w); }\n"
    "void skin_vertex(inout uint w[27], uint o, uint a, uint sk, uint fl) {\n"
    "    float4 p = asfloat(vsrc.Load4(a)), n = asfloat(vsrc.Load4(a + 16u)), t = asfloat(vsrc.Load4(a + 32u));\n"
    "    float4 R0, R1, R2;\n"
    "    skin_rows(sk, vsrc.Load2(a + 48u), R0, R1, R2);\n"
    "    float3 P = skin_pos(p, R0, R1, R2);\n"
    "    float4 N = skin_rot(n, R0, R1, R2), T = skin_rot(t, R0, R1, R2);\n"
    "    if ((fl & 1u) != 0u) N = skin_norm(N);\n"
    "    if ((fl & 2u) != 0u) T = skin_norm(T);\n"
    "    wr32(w, o, asuint(P.x)); wr32(w, o + 4u, asuint(P.y)); wr32(w, o + 8u, asuint(P.z));\n"
    "    int4 ni = (int4)(mad(N, 32767.5, -0.5) * 65536.0);\n"
    "    wr16(w, o + 12u, ((uint)ni.x) >> 16); wr16(w, o + 14u, ((uint)ni.y) >> 16);\n"
    "    wr16(w, o + 16u, ((uint)ni.z) >> 16); wr16(w, o + 18u, ((uint)ni.w) >> 16);\n"
    "    float3 tt = mad(T.xyz, float3(asfloat(0x3F7FE000u), asfloat(0x3F7FE000u), asfloat(0x3F7FC000u)),\n"
    "                    float3(asfloat(0xBA000000u), asfloat(0xBA000000u), asfloat(0xBA800000u)));\n"
    "    int3 ti = (int3)(tt * 2147483648.0);\n"
    "    wr32(w, o + 20u, ((((uint)ti.x) >> 21) & 0x7FFu) | (((((uint)ti.y) >> 21) & 0x7FFu) << 11) | (((((uint)ti.z) >> 22) & 0x3FFu) << 22));\n"
    "}\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 gid : SV_GroupID, uint ti : SV_GroupIndex) {\n"
    "    uint e = (seg_base + gid.x) * 32u;\n"
    "    uint4 s0 = vtab.Load4(e);\n"
    "    uint2 s1 = vtab.Load2(e + 16u);\n"
    "    uint sh = s0.x, dh = s0.y, data = s0.z, nv = s0.w, rec = s1.x, sv = vtab.Load(e + 24u), sk = vtab.Load(e + 28u);\n"
    "    uint dw0 = data >> 2, dw1 = (data >> 2) + ((nv * 54u + 3u) >> 2), nw = (s1.y + 3u) >> 2;\n"
    "    for (uint i = ti; i < nw; i += 64u) {\n"
    "        if (i >= dw0 && i < dw1) continue;\n"
    "        uint v = vsrc.Load(sh + i * 4u);\n"
    "        if (i < 16u) v += dh - sh;\n"
    "        vdst.Store(dh + i * 4u, v);\n"
    "    }\n"
    "    if ((sk & 1u) != 0u) {\n"
    "        sk &= ~1u;\n"
    "        for (uint v = ti; v < nv; v += 64u) {\n"
    "            uint a = sv + v * 32u;\n"
    "            float4 R0, R1, R2;\n"
    "            skin_rows(sk, vsrc.Load2(a + 16u), R0, R1, R2);\n"
    "            float3 P = skin_pos(asfloat(vsrc.Load4(a)), R0, R1, R2);\n"
    "            vdst.Store3(dh + data + v * 12u, uint3(bs(asuint(P.x)), bs(asuint(P.y)), bs(asuint(P.z))));\n"
    "        }\n"
    "        return;\n"
    "    }\n"
    "    if (ti == 0u) { gHead = vtab.Load4(rec); gInt = asfloat(vtab.Load4(rec + 16u)); }\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    uint nall = min(gHead.x + gHead.y + gHead.z, (uint)MAXL);\n"
    "    if (ti < nall) {\n"
    "        uint a = rec + 32u + ti * 48u;\n"
    "        gP[ti] = asfloat(vtab.Load4(a)); gC[ti] = asfloat(vtab.Load4(a + 16u)); gQ[ti] = asfloat(vtab.Load4(a + 32u));\n"
    "    }\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    uint np = (nv + 1u) >> 1;\n"
    "    for (uint p = ti; p < np; p += 64u) {\n"
    "        uint a = (sv != 0u ? sv : sh + data) + p * 108u, b = dh + data + p * 108u;\n"
    "        uint w[27]; uint4 q; uint k;\n"
    "        if (sk != 0u) {\n"
    "            [unroll] for (k = 0u; k < 27u; k++) w[k] = 0u;\n"
    "            uint fl = vtab.Load(sk);\n"
    "            skin_vertex(w, 0u, sv + p * 128u, sk, fl);\n"
    "            if (p * 2u + 1u < nv) skin_vertex(w, 54u, sv + p * 128u + 64u, sk, fl);\n"
    "        } else {\n"
    "            [unroll] for (k = 0u; k < 24u; k += 4u) { q = vsrc.Load4(a + k * 4u); w[k] = q.x; w[k + 1u] = q.y; w[k + 2u] = q.z; w[k + 3u] = q.w; }\n"
    "            uint3 q3 = vsrc.Load3(a + 96u); w[24] = q3.x; w[25] = q3.y; w[26] = q3.z;\n"
    "        }\n"
    "        light_vertex(w, 0u);\n"
    "        if (p * 2u + 1u < nv) {\n"
    "            light_vertex(w, 54u);\n"
    "            [unroll] for (k = 0u; k < 24u; k += 4u) vdst.Store4(b + k * 4u, uint4(w[k], w[k + 1u], w[k + 2u], w[k + 3u]));\n"
    "            vdst.Store3(b + 96u, uint3(w[24], w[25], w[26]));\n"
    "        } else {\n"
    "            [unroll] for (k = 0u; k < 12u; k += 4u) vdst.Store4(b + k * 4u, uint4(w[k], w[k + 1u], w[k + 2u], w[k + 3u]));\n"
    "            vdst.Store2(b + 48u, uint2(w[12], w[13]));\n"
    "        }\n"
    "    }\n"
    "}\n";
static const char* eg_hlsl(void) { return s_eg_hlsl; }

/* GOWA_EDGE_GPU_SELFTEST: the C pass over the packed vertexes (c + 0x50's second word, the buffer the
 * vertex PUT took) against the SPU's, for the vertexes in the stage's list */
static volatile long long s_st_verts, s_st_units[5], s_st_calls;
static double s_st_max27, s_st_maxh[3];
static volatile int s_st_lock;
static float eg_half(uint32_t h)
{
    const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    const float v = e == 0 ? (float)m * (1.0f / 16777216.0f) : ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    return (h & 0x8000u) ? -v : v;
}
static void eg_st_report(void)
{
    const double n = (double)s_st_verts * 9.0;
    if (!s_st_verts) { fprintf(stderr, "[edge-gpu] selftest: no vertexes\n"); return; }
    fprintf(stderr, "[edge-gpu] selftest, the C pass against the SPU: %lld jobs, %lld vertexes; id 27 max |d| %.3g; "
            "ids 28-30 as F16: %.2f%% the same, %.2f%% 1 unit, %.2f%% 2-3, %.3f%% 4-15, %.3f%% more; max |d| %.3g %.3g %.3g\n",
            s_st_calls, s_st_verts, s_st_max27, 100.0 * s_st_units[0] / n, 100.0 * s_st_units[1] / n,
            100.0 * s_st_units[2] / n, 100.0 * s_st_units[3] / n, 100.0 * s_st_units[4] / n,
            s_st_maxh[0], s_st_maxh[1], s_st_maxh[2]);
}
static void eg_selftest(const uint8_t* ls, const eg_rec* r, uint32_t n)
{
    const uint32_t c = 0x107A0, buf = eg_be32(ls, c + 0x54), s7 = eg_be32(ls, c + 0x2C);
    if (!n || n > 4096 || (uint64_t)buf + (uint64_t)n * EG_STRIDE > SPU_LS_SIZE || (uint64_t)s7 + 16ull * n > SPU_LS_SIZE)
        return;
    uint8_t* out = (uint8_t*)malloc((size_t)n * EG_STRIDE);
    if (!out) return;
    eg_cpu(r, ls + buf, out, n);
    while (__atomic_exchange_n(&s_st_lock, 1, __ATOMIC_ACQUIRE)) { }
    static int armed;
    if (!armed) { armed = 1; atexit(eg_st_report); }
    s_st_calls++;
    uint32_t prev = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t off = eg_be32(ls, s7 + 16 * k + 12);
        if ((off & 15) || off / 16 >= n || (k && off <= prev)) break;
        prev = off;
        const uint32_t v = off / 16;
        const uint8_t* g = out + (size_t)v * EG_STRIDE;
        const uint8_t* s = ls + buf + (size_t)v * EG_STRIDE;
        s_st_verts++;
        for (int i = 0; i < 3; i++) {
            const double d = fabs((double)eg_f(eg_rd32(g + 24 + 4 * i)) - (double)eg_f(eg_rd32(s + 24 + 4 * i)));
            if (d > s_st_max27) s_st_max27 = d;
        }
        for (int a = 0; a < 3; a++)
            for (int i = 0; i < 3; i++) {
                const uint32_t ha = ((uint32_t)g[36 + 6 * a + 2 * i] << 8) | g[37 + 6 * a + 2 * i];
                const uint32_t hb = ((uint32_t)s[36 + 6 * a + 2 * i] << 8) | s[37 + 6 * a + 2 * i];
                const uint32_t ma = ha & 0x7FFFu, mb = hb & 0x7FFFu;
                const uint32_t du = ((ha ^ hb) & 0x8000u) ? ma + mb : (ma > mb ? ma - mb : mb - ma);
                s_st_units[du == 0 ? 0 : du == 1 ? 1 : du < 4 ? 2 : du < 16 ? 3 : 4]++;
                const double d = fabs((double)eg_half(ha) - (double)eg_half(hb));
                if (d > s_st_maxh[a]) s_st_maxh[a] = d;
            }
    }
    __atomic_store_n(&s_st_lock, 0, __ATOMIC_RELEASE);
    free(out);
}
