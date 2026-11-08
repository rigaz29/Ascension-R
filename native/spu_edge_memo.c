/*
 * EDGE's job results, kept: most of a frame's jobs are last frame's. GOWA_EDGE_MEMO: 1 on (the
 * default in the game), 0 off (the default in a replay), 2 the check: every stage runs, and what a
 * result says is compared with what the stages make (GOWA_EDGE_NATIVE_STATS=1 prints the counts).
 *
 * A skinned job's are kept in another form (spu_edge_gpuskin.c): its list, and its vertexes before
 * skinning, which go to the vertex pass with the palette of the frame.
 *
 * An unskinned job's indexes and vertexes depend only on its inputs: the index and vertex streams,
 * the formats, the segment header, the matrix and the pieces. At the gate 95% of such jobs (77% of
 * all jobs) have inputs seen before, in the fight 91% (62%): the same meshes, drawn every frame, and
 * in the depth pass and the lit pass. The camera comes in through the vertex program, and the lit
 * jobs' only other dependence, on their lights, is the record the GPU's pass takes. So:
 *  - the end of 0x9990, which fills the table of the slots' places from the DMA list the job manager
 *    handed over, hashes those inputs (gowa_edge_memo_9a40; the job's own entry can only guess
 *    which buffer set is its own, and after another module's jobs the guess fails); a job without
 *    a result keeps what its stages make, and a job with one leaves them out:
 *      - unpack (the input piece's native; it still fills the attribute list) and the transforms
 *        (0xF538) make arrays only the packing reads;
 *      - the index stage (0xE4D0) copies its list back;
 *      - the output piece (0x755B80, 0x765B70) copies the vertexes back: 12 bytes each for a
 *        position, the first 24 of the 54 for a lit one (the pass writes the rest);
 *  - lit jobs only with the lighting stage skipped and culling left to the RSX: both the lit bytes
 *    and the index list would depend on the lights and the camera otherwise. Only a job whose stage
 *    was skipped leaves a result. A later one whose stage runs on the SPU all the same (too many
 *    lights are seen at the entry; declined, late, or no stage) gets the arrays made late, from the
 *    arguments kept (edge_memo_revoke), and goes on as without a result. The stream does not
 *    survive that long in the local store, so a lit result keeps it too. A position's array comes
 *    back from the result itself.
 *
 * GOWA_EDGE_RESIDENT=1 (the default in the game; 0 off, as in a replay or a capture): a job with a
 * result puts nothing in the ring but its vertexes' first 16 bytes. The block is published to the
 * backend (rsx_kept.h), which draws from a resident copy: the same bytes went to the ring and from the
 * ring to the GPU every frame.
 *
 * A result is kept in an arena, the second time its job is seen: 128-bit hash, list and records.
 * Results are never written after they are published. When an arena is full the table moves to the other one, emptied: the jobs
 * still reading a result of the old one end long before it is used again (a change waits 5 s).
 * Not in a capture: its jobs must run as they ran. GOWA_EDGE_VERIFY: off.
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_edge_job.h"
#include "spu_edge_memo.h"
#include "spu_edge_gpuskin.h"
#include "spu_edge_verify.h"
#include "rsx_kept.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__AVX2__)

#define MEMO_ARENA  (128u << 20)
#define MEMO_SLOTS  (1u << 17)
#define MEMO_PROBES 16
#define MEMO_IDX_MAX 32768u
#define MEMO_STREAM_MAX 65536u
#define MEMO_DEFER  8

typedef struct memo_entry {
    uint64_t k0, k1;
    uint32_t idx_bytes;         /* the list as the index stage leaves it, passes included */
    uint32_t nverts, rec;       /* records and their size */
    uint32_t stream_bytes;      /* a lit job's vertex stream, for a late unpack */
    uint32_t skin_flags;        /* a skinned job's: what 0xE550 does (EK_PN, EK_PT) */
    rsx_kept kb;                /* the backend's view of the list and the records */
    uint8_t  data[];            /* the list, 16-aligned; the records, 16-aligned; the stream */
} memo_entry;

static uint8_t* s_arena[2];
static memo_entry* volatile s_tab[2][MEMO_SLOTS];
static volatile int s_cur;
static volatile long long s_used;
static volatile long s_lock;
static clock_t s_flipped;

static volatile long long s_jobs, s_hits, s_stored, s_revoked, s_bytes, s_flips, s_bad, s_dropped;
static volatile long long s_tail_i, s_tail_v, s_bad_v, s_bad_lit;

typedef struct {
    uint32_t kind;              /* 0 lit unpack, 1 transform, 2 position unpack, 3 0xE550 */
    u128 r[10];                 /* a transform: r0, then r3-r11 */
    uint8_t mat[64];            /* and its matrix, which may not stay where r6 says */
    uint32_t a[5];              /* an unpack: stream, records, arrays */
} memo_call;

static SPU_THREAD_LOCAL struct {
    int state;                  /* 0 none; 1 keeping; 2 a result; 3 a result, checked */
    int lit;
    int skin;                   /* a skinned job's (spu_edge_gpuskin.c) */
    int ring;                   /* a result that goes out whole after all */
    uint32_t nverts, nidx;      /* the segment's exact counts */
    uint64_t k0, k1;
    const memo_entry* e;
    uint8_t* idx;               /* the list of a job keeping its result */
    uint32_t idx_bytes;
    uint8_t* stream;            /* and its vertex stream */
    uint32_t stream_bytes;
    int ncalls;
    memo_call calls[MEMO_DEFER];
} t_m;

static int memo_mode(void)
{
    static volatile int m = -1;
    int v = m;
    if (v >= 0) return v;
    const char* e = getenv("GOWA_EDGE_MEMO");
    v = e ? atoi(e) : getenv("SPU_JOB_REPLAY") ? 0 : 1;
    if (v < 0 || v > 2) v = 0;
    if (getenv("SPU_JOB_CAPTURE") || edge_verify_on()) v = 0;
    m = v;
    return v;
}

static int memo_resident_mode(void);
/* GOWA_EDGE_SKIN: 1 on (the default), 0 off, 2 the check: every skinned job's vertexes worked out on
 * the CPU and compared with what the job packs.
 *
 * The game draws its skinned meshes twice, a depth pass and a lit pass that tests EQUAL against the
 * depth pass's depth, and the two jobs of one mesh are taken or left here each on its own: in the
 * prison of the damned (the opening), ~45% of skinned jobs stay on the SPU (103,765 lit jobs never
 * reach this path, 10,241 have more than 32 lights, 45,738 position jobs never reach it either)
 * while the rest are skinned in the vertex pass. So the pass works the position out the SPU's way,
 * bit for bit, in integer operations (spu_edge_gpulight.c): with the GPU's own rounding the depths
 * differed in the last bit and the lit pass left holes, through which the never cleared colour
 * target showed earlier frames as trails (docs/notes/2026-11-08-prison-trails.md). */
static int memo_skin_mode(void)
{
    static volatile int m = -1;
    int v = m;
    if (v >= 0) return v;
    const char* e = getenv("GOWA_EDGE_SKIN");
    v = e ? atoi(e) : 1;
    if (v < 0 || v > 2 || memo_mode() != 1) v = 0;
    if (v == 1 && memo_resident_mode() != 1) v = 0;
    m = v;
    return v;
}

/* GOWA_EDGE_RESIDENT: 1 on, 0 off, 2 the blocks published with the ring's bytes put as well */
static int memo_resident_mode(void)
{
    static volatile int m = -1;
    int v = m;
    if (v >= 0) return v;
    const char* e = getenv("GOWA_EDGE_RESIDENT");
    v = e ? atoi(e) : 1;
    if (v < 0 || v > 2 || memo_mode() != 1) v = 0;
    m = v;
    return v;
}

static void memo_report(void)
{
    fprintf(stderr, "[edge-memo] %lld jobs looked up: %lld with a result, %lld kept, %lld gave it up; "
            "%.1f MB in the arena, %lld arena changes, %lld not kept; the check: %lld lists differ, %lld vertexes "
            "(%lld of them lit), and past the counts %lld lists, %lld vertexes\n",
            s_jobs, s_hits, s_stored, s_revoked, (double)s_used / 1048576.0, s_flips, s_dropped, s_bad, s_bad_v,
            s_bad_lit, s_tail_i, s_tail_v);
}

/* ------------------------------------------------------------------------------- the hash */
typedef struct { __m256i a, b, k; } memo_h;

static inline void memo_h_init(memo_h* h)
{
    h->a = _mm256_set_epi64x((long long)0x9E3779B97F4A7C15ull, (long long)0xC2B2AE3D27D4EB4Full,
                             (long long)0x165667B19E3779F9ull, (long long)0x27D4EB2F165667C5ull);
    h->b = _mm256_set_epi64x((long long)0x85EBCA77C2B2AE63ull, (long long)0xFF51AFD7ED558CCDull,
                             (long long)0xC4CEB9FE1A85EC53ull, (long long)0x2545F4914F6CDD1Dull);
    h->k = _mm256_set_epi64x(0x1B873593CC9E2D51ll, 0x4CF5AD432745937Fll, 0x52DCE729D6E8EC2Bll, 0x38495AB5A5CB9E35ll);
}

/* 64 bytes: the product of each word's halves, the key moving with the position */
static inline void memo_h_block(memo_h* h, const uint8_t* p)
{
    const __m256i step = _mm256_set1_epi64x((long long)0x9FB21C651E98DF25ull);
    const __m256i d0 = _mm256_loadu_si256((const __m256i*)p), d1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    const __m256i k0 = _mm256_xor_si256(d0, h->k);
    h->k = _mm256_add_epi64(h->k, step);
    const __m256i k1 = _mm256_xor_si256(d1, h->k);
    h->k = _mm256_add_epi64(h->k, step);
    h->a = _mm256_add_epi64(_mm256_add_epi64(h->a, _mm256_shuffle_epi32(d0, _MM_SHUFFLE(1, 0, 3, 2))),
                            _mm256_mul_epu32(k0, _mm256_srli_epi64(k0, 32)));
    h->b = _mm256_add_epi64(_mm256_add_epi64(h->b, _mm256_shuffle_epi32(d1, _MM_SHUFFLE(1, 0, 3, 2))),
                            _mm256_mul_epu32(k1, _mm256_srli_epi64(k1, 32)));
}

static inline void memo_h_bytes(memo_h* h, const uint8_t* p, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 64 <= n; i += 64) memo_h_block(h, p + i);
    if (i < n) {
        uint8_t t[64] = { 0 };
        memcpy(t, p + i, n - i);
        t[63] ^= (uint8_t)(n - i);
        memo_h_block(h, t);
    }
}

static inline uint64_t memo_fold(uint64_t x, uint64_t y)
{
    x ^= y * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    return x ^ (x >> 32);
}

static inline void memo_h_end(const memo_h* h, uint64_t* k0, uint64_t* k1)
{
    uint64_t a[4], b[4];
    _mm256_storeu_si256((__m256i*)a, h->a);
    _mm256_storeu_si256((__m256i*)b, h->b);
    uint64_t x = 0x243F6A8885A308D3ull, y = 0x13198A2E03707344ull;
    for (int i = 0; i < 4; i++) { x = memo_fold(x, a[i]); y = memo_fold(y, b[i]); x = memo_fold(x, b[3 - i]); y = memo_fold(y, a[3 - i]); }
    *k0 = x; *k1 = y;
}

/* ------------------------------------------------------------------------------- the table */
static inline const memo_entry* memo_find(uint64_t k0, uint64_t k1)
{
    memo_entry* volatile* t = s_tab[s_cur];
    uint32_t i = (uint32_t)k0 & (MEMO_SLOTS - 1);
    for (int p = 0; p < MEMO_PROBES; p++, i = (i + 1) & (MEMO_SLOTS - 1)) {
        const memo_entry* e = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
        if (!e) return NULL;
        if (e->k0 == k0 && e->k1 == k1) return e;
    }
    return NULL;
}

static inline const uint8_t* memo_records(const memo_entry* e)
{
    return e->data + ((e->idx_bytes + 15) & ~15u);
}

static void ek_fill(const rsx_kept* k, const void* skin, uint8_t* ring, uint32_t n);
static void memo_keep(uint64_t k0, uint64_t k1, const uint8_t* idx, uint32_t idx_bytes,
                      const uint8_t* src, uint32_t n, uint32_t stride, uint32_t rec,
                      const uint8_t* stream, uint32_t stream_bytes,
                      const uint8_t* ring, uint32_t ring_stride, uint32_t skin_flags)
{
    const uint32_t lb = (idx_bytes + 15) & ~15u;
    const size_t rb = ((size_t)n * rec + 15) & ~(size_t)15;
    const size_t need = (sizeof(memo_entry) + lb + rb + stream_bytes + 63) & ~(size_t)63;
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) _mm_pause();
    if (!s_arena[0]) {
        s_arena[0] = (uint8_t*)malloc(MEMO_ARENA);
        s_arena[1] = (uint8_t*)malloc(MEMO_ARENA);
        s_flipped = clock();
    }
    if (!s_arena[0] || !s_arena[1]) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); s_dropped++; return; }
    if ((size_t)s_used + need > MEMO_ARENA) {
        if (clock() - s_flipped < 5 * CLOCKS_PER_SEC) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); s_dropped++; return; }
        const int nx = s_cur ^ 1;
        for (uint32_t i = 0; i < MEMO_SLOTS; i++) s_tab[nx][i] = NULL;
        __atomic_store_n(&s_cur, nx, __ATOMIC_RELEASE);
        s_used = 0;
        s_flipped = clock();
        s_flips++;
    }
    const int cur = s_cur;
    memo_entry* e = (memo_entry*)(s_arena[cur] + s_used);
    s_used += (long long)need;
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);

    e->k0 = k0; e->k1 = k1;
    e->idx_bytes = idx_bytes; e->nverts = n; e->rec = rec; e->stream_bytes = stream_bytes;
    memcpy(e->data, idx, idx_bytes);
    uint8_t* d = e->data + lb;
    if (stride == rec) memcpy(d, src, (size_t)n * rec);
    else for (uint32_t v = 0; v < n; v++) memcpy(d + (size_t)v * rec, src + (size_t)v * stride, rec);
    if (stream_bytes) memcpy(d + rb, stream, stream_bytes);
    e->skin_flags = skin_flags;
    memset(&e->kb, 0, sizeof e->kb);
    e->kb.list = e->data; e->kb.verts = d;
    e->kb.list_bytes = idx_bytes; e->kb.nverts = n; e->kb.rec = rec; e->kb.stride = ring_stride;
    e->kb.skin = ring != src;
    e->kb.fill = ek_fill;
    memcpy(e->kb.tag, ring, 16);

    uint32_t i = (uint32_t)k0 & (MEMO_SLOTS - 1);
    for (int p = 0; p < MEMO_PROBES; p++, i = (i + 1) & (MEMO_SLOTS - 1)) {
        memo_entry* expect = NULL;
        if (__atomic_compare_exchange_n(&s_tab[cur][i], &expect, e, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&s_stored, 1, __ATOMIC_RELAXED);
            return;
        }
        if (expect->k0 == k0 && expect->k1 == k1) return;     /* another worker's */
    }
    __atomic_add_fetch(&s_dropped, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------------------- the job */
static inline uint32_t ej_be32_at(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

extern int edge_gpul_lean_on(void);
extern int edge_cull_left_out(void);
extern void rsx_d3d12_vertex_pass_publish(uint32_t io_offset, uint32_t nverts, const void* rec, uint32_t bytes,
                                          uint32_t applied, const void* tag16, const void* skin, uint32_t skin_bytes);

/* ------------------------------------------------------------------- a skinned job's inputs */
static SPU_THREAD_LOCAL struct {
    int taken;                  /* the palette and the arrays seen */
    uint32_t pa, na, ta, count;
    ek_rec* rec;
    uint8_t* in;                /* the inputs of a job keeping its result */
} t_k;
static volatile long long s_k_jobs, s_k_declined, s_k_vtx, s_k_pos_diff, s_k_pos_ulp, s_k_n[4], s_k_t[4], s_k_other, s_k_flags[4];
static volatile long long s_k_hits;
static void ek_report(void)
{
    fprintf(stderr, "[edge-skin] %lld jobs checked, %lld declined, %lld left to the pass, %lld vertexes; position words "
            "differing %lld (most %lld ulp); normal units 0/1/2/more %lld %lld %lld %lld; tangent %lld %lld %lld %lld; other %lld;",
            s_k_jobs, s_k_declined, s_k_hits, s_k_vtx, s_k_pos_diff, s_k_pos_ulp, s_k_n[0], s_k_n[1], s_k_n[2], s_k_n[3],
            s_k_t[0], s_k_t[1], s_k_t[2], s_k_t[3], s_k_other);
    for (int i = 0; i < 4; i++) if (s_k_flags[i]) fprintf(stderr, " f%d:%lld", i, s_k_flags[i]);
    fprintf(stderr, "%c", 10);
}

/* not this way after all: nothing more is kept, and a result's arrays are made */
static void ek_decline(spu_context* ctx)
{
    __atomic_add_fetch(&s_k_declined, 1, __ATOMIC_RELAXED);
    if (t_m.state == 2) edge_memo_revoke(ctx);
    t_m.skin = 0;
    t_m.state = 0;
}

/* the backend's way when a draw does not go through the pass */
static void ek_fill(const rsx_kept* k, const void* skin, uint8_t* ring, uint32_t n)
{
    const uint8_t* in = (const uint8_t*)k->verts;
    for (uint32_t v = 0; v < n; v++, in += k->rec) {
        if (k->rec == EK_IN_LIT) ek_vertex_lit((const ek_rec*)skin, in, ring + (size_t)v * k->stride);
        else ek_vertex_pos((const ek_rec*)skin, in, ring + (size_t)v * k->stride);
    }
}

/* 0xE550's entry: 1 = left out */
int edge_memo_skin_e550(spu_context* ctx)
{
    if (!t_m.skin || !t_m.state) return 0;
    uint8_t* ls = ctx->ls;
    const uint32_t c = ctx->gpr[3]._u32[0] & (SPU_LS_SIZE - 16);
    const uint32_t pal = ctx->gpr[4]._u32[0], nmat = ctx->gpr[5]._u32[0], bones = ctx->gpr[6]._u32[0];
    const uint32_t count = ((uint32_t)ls[c + 8] << 8) | ls[c + 9];
    const uint32_t pa = ej_be32_at(ls + c + 0x5C), xa = ej_be32_at(ls + c + 0x60);
    const uint32_t na = ej_be32_at(ls + c + 0x64), ta = ej_be32_at(ls + c + 0x68);
    t_k.pa = pa; t_k.na = na; t_k.ta = ta; t_k.count = count;
    t_k.taken = 1;
    /* the matrix format 0xE804 lays out, the stream set that takes 0xEC08 and 0xF104 */
    if (ls[c + 7] != 2 || (ls[c] & 0x90) != 0x80 || (ls[c + 6] & 15) != 3 ||
        !nmat || nmat > EK_MAX_MATS || !count || count > 0x4000 ||
        pal + nmat * 64u > SPU_LS_SIZE || xa || !pa || (t_m.lit ? (!na || !ta) : (na || ta))) {
        ek_decline(ctx);
        return 0;
    }
    if (!t_k.rec) t_k.rec = (ek_rec*)malloc(sizeof(ek_rec));
    if (!t_k.in) t_k.in = (uint8_t*)malloc((size_t)0x4008 * EK_IN_LIT);
    if (!t_k.rec || !t_k.in) { ek_decline(ctx); return 0; }
    ek_rows(t_k.rec, ls + pal, nmat);
    if (t_m.state == 2) {
        /* left out: what stays of it is the free space it hands back, c + 0x58 */
        t_k.rec->flags = t_m.e->skin_flags;
        memo_call* m = &t_m.calls[t_m.ncalls++];
        m->kind = 3;
        m->r[0] = ctx->gpr[0];
        for (int r = 3; r < 12; r++) m->r[r - 2] = ctx->gpr[r];
        const uint32_t free_at = pal < bones ? pal : bones;
        ls[c + 0x58] = (uint8_t)(free_at >> 24); ls[c + 0x59] = (uint8_t)(free_at >> 16);
        ls[c + 0x5A] = (uint8_t)(free_at >> 8); ls[c + 0x5B] = (uint8_t)free_at;
        { static int st = -1;
          if (st < 0) st = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
          if (!(__atomic_add_fetch(&s_k_hits, 1, __ATOMIC_RELAXED) & 0xFFFF) && st) ek_report(); }
        return 1;
    }
    t_k.rec->flags = 0;
    const uint32_t stride = t_m.lit ? EK_IN_LIT : EK_IN_POS;
    for (uint32_t i = 0; i < count; i++)
        ek_take(t_k.in + (size_t)i * stride, ls + pa + 16u * i, t_m.lit ? ls + na + 16u * i : NULL,
                t_m.lit ? ls + ta + 16u * i : NULL, ls + bones + 8u * i);
    return 0;
}

/* 0xBBC8's entry: 0xE550's own calls, at its end. Left out with it, known from when the result was kept. */
void edge_memo_skin_bbc8(spu_context* ctx)
{
    if (!t_m.skin || !t_k.taken) return;
    const uint32_t a = ctx->gpr[3]._u32[0], link = ctx->gpr[0]._u32[0];
    if ((link != 0xF484 && link != 0xF490) || (a != t_k.na && a != t_k.ta)) { ek_decline(ctx); return; }
    t_k.rec->flags |= a == t_k.na ? EK_PN : EK_PT;
}

/* 0xF538's entry: a skinned job that goes on to a transform stays on the SPU */
static void ek_transform(spu_context* ctx)
{
    if (t_k.taken) ek_decline(ctx);
}

static inline uint32_t ek_be(const uint8_t* p) { return ej_be32_at(p); }
static inline void ek_bucket(volatile long long* b, int d)
{
    if (d < 0) d = -d;
    __atomic_add_fetch(&b[d > 3 ? 3 : d], 1, __ATOMIC_RELAXED);
}
/* the packing's n vertexes against the CPU version */
static void ek_check(const uint8_t* src, uint32_t n, uint32_t stride)
{
    if (!t_k.taken) return;
    const uint32_t m = t_m.nverts < n ? t_m.nverts : n;
    const long long j = __atomic_add_fetch(&s_k_jobs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_k_flags[t_k.rec->flags & 3], 1, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < m; i++) {
        uint8_t o[24];
        const uint8_t* s = src + (size_t)i * stride;
        if (t_m.lit) ek_vertex_lit(t_k.rec, t_k.in + (size_t)i * EK_IN_LIT, o);
        else ek_vertex_pos(t_k.rec, t_k.in + (size_t)i * EK_IN_POS, o);
        for (int l = 0; l < 3; l++) {
            const int32_t a = (int32_t)ek_be(s + 4 * l), b = (int32_t)ek_be(o + 4 * l);
            if (a != b) {
                __atomic_add_fetch(&s_k_pos_diff, 1, __ATOMIC_RELAXED);
                const long long d = a > b ? (long long)a - b : (long long)b - a;
                if (d > s_k_pos_ulp) s_k_pos_ulp = d;
            }
        }
        if (t_m.lit) {
            for (int l = 0; l < 3; l++) {
                const int a = (int16_t)(((uint32_t)s[12 + 2 * l] << 8) | s[13 + 2 * l]);
                const int b = (int16_t)(((uint32_t)o[12 + 2 * l] << 8) | o[13 + 2 * l]);
                ek_bucket(s_k_n, a - b);
            }
            if (memcmp(s + 18, o + 18, 2)) __atomic_add_fetch(&s_k_other, 1, __ATOMIC_RELAXED);
            const uint32_t ta = ek_be(s + 20), tb = ek_be(o + 20);
            ek_bucket(s_k_t, ((int32_t)(ta << 21) >> 21) - ((int32_t)(tb << 21) >> 21));
            ek_bucket(s_k_t, ((int32_t)(ta << 10) >> 21) - ((int32_t)(tb << 10) >> 21));
            ek_bucket(s_k_t, ((int32_t)ta >> 22) - ((int32_t)tb >> 22));
        }
    }
    __atomic_add_fetch(&s_k_vtx, (long long)m, __ATOMIC_RELAXED);
    if (!(j & 0x3FFF)) ek_report();
}

void edge_memo_job(void)
{
    t_m.state = 0;
    t_m.skin = 0;
    t_m.ring = 0;
    t_k.taken = 0;
    t_m.ncalls = 0;
    t_m.idx_bytes = 0;
    t_m.stream_bytes = 0;
}

void gowa_edge_memo_9a40(spu_context* ctx)
{
    const int mode = memo_mode();
    if (!mode) return;
    static int stats = -1;
    if (stats < 0) { stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL; if (stats) atexit(memo_report); }
    ej_view v;
    if (ej_read(ctx, &v, ej_be32_at(ctx->ls + 0x106B0)) || (v.skinned && !memo_skin_mode())) return;
    if (v.kind == EJ_LIT || v.kind == EJ_LIT_SKIN) {
        if (!edge_gpul_lean_on() || !edge_cull_left_out()) return;
    } else if (v.kind != EJ_POS && v.kind != EJ_POS_SKIN) {
        return;
    }
    t_m.lit = v.kind == EJ_LIT || v.kind == EJ_LIT_SKIN;
    t_m.skin = v.skinned;
    t_m.nverts = v.nverts;
    t_m.nidx = v.nidx;
    /* more lights than the stage's record takes: it will run on the SPU */
    if (t_m.lit && ej_be32_at(ctx->ls + v.slot_lsa[0] + 0x20) > 32) return;
    const uint8_t* ls = ctx->ls;
    /* a skinned job: the weights too, not the matrix, which goes with the palette */
    static const int slots_plain[7] = { 4, 5, 11, 13, 14, 16, 18 }, slots_skin[7] = { 4, 5, 11, 13, 14, 16, 10 };
    const int* const slots = v.skinned ? slots_skin : slots_plain;
    static const uint8_t words[] = { 14, 18, 19, 24, 25, 26, 27, 32, 33, 35, 36, 37, 38 };
    memo_h h;
    memo_h_init(&h);
    {   /* the words that shape the job, the pieces, the slots' sizes */
        uint32_t w[32];
        int n = 0;
        for (unsigned i = 0; i < sizeof words; i++) w[n++] = v.desc[words[i]];
        w[n++] = v.desc[34] >> 16;
        w[n++] = v.out_piece; w[n++] = v.in_piece; w[n++] = v.meth_piece;
        for (unsigned i = 0; i < 7; i++) w[n++] = v.slot_size[slots[i]];
        memo_h_bytes(&h, (const uint8_t*)w, (uint32_t)n * 4u);
    }
    for (unsigned i = 0; i < 7; i++)
        memo_h_bytes(&h, ls + v.slot_lsa[slots[i]], v.slot_size[slots[i]]);
    memo_h_end(&h, &t_m.k0, &t_m.k1);
    __atomic_add_fetch(&s_jobs, 1, __ATOMIC_RELAXED);
    t_m.e = t_m.skin && memo_skin_mode() == 2 ? NULL : memo_find(t_m.k0, t_m.k1);
    if (t_m.e) {
        __atomic_add_fetch(&s_hits, 1, __ATOMIC_RELAXED);
        t_m.state = mode == 2 ? 3 : 2;
    } else {
        t_m.state = 1;
    }
    if (stats && !(s_jobs & 0xFFFF)) memo_report();
}

int edge_memo_left_out(spu_context* ctx)
{
    if (t_m.skin) ek_transform(ctx);
    if (t_m.state != 2) return 0;
    if (t_m.ncalls == MEMO_DEFER) { edge_memo_revoke(ctx); return 0; }     /* not seen */
    memo_call* c = &t_m.calls[t_m.ncalls++];
    c->kind = 1;
    c->r[0] = ctx->gpr[0];
    for (int r = 3; r < 12; r++) c->r[r - 2] = ctx->gpr[r];
    memcpy(c->mat, ctx->ls + (ctx->gpr[6]._u32[0] & (SPU_LS_SIZE - 16)), 64);
    return 1;
}

int edge_memo_unpack(spu_context* ctx, int lit, uint32_t src, uint32_t cnt, uint32_t p0, uint32_t n0, uint32_t t0)
{
    if (t_m.state == 1 && lit) {              /* kept for a late unpack */
        const uint32_t n = 18u * cnt;
        if (n > MEMO_STREAM_MAX) { t_m.state = 0; return 0; }
        if (!t_m.stream) t_m.stream = (uint8_t*)malloc(MEMO_STREAM_MAX);
        if (!t_m.stream) { t_m.state = 0; return 0; }
        memcpy(t_m.stream, ctx->ls + src, n);
        t_m.stream_bytes = n;
        return 0;
    }
    if (t_m.state != 2) return 0;
    if (lit && t_m.e->stream_bytes != 18u * cnt) { edge_memo_revoke(ctx); return 0; }
    if (t_m.ncalls == MEMO_DEFER) { edge_memo_revoke(ctx); return 0; }     /* not seen */
    memo_call* c = &t_m.calls[t_m.ncalls++];
    c->kind = lit ? 0 : 2;
    c->a[0] = src; c->a[1] = cnt; c->a[2] = p0; c->a[3] = n0; c->a[4] = t0;
    return 1;
}

/* the index stage: the list in place, or the list noted */
int edge_memo_idx(spu_context* ctx, uint32_t src, uint32_t* bytes)
{
    if (t_m.state != 2) return 0;
    *bytes = t_m.e->idx_bytes;
    memcpy(ctx->ls + src, t_m.e->data, t_m.e->idx_bytes);
    return 1;
}

void edge_memo_idx_note(const spu_context* ctx, uint32_t src, uint32_t bytes)
{
    if (t_m.state == 3) {
        const uint32_t need = 2u * t_m.nidx;
        if (bytes != t_m.e->idx_bytes || need > bytes || memcmp(ctx->ls + src, t_m.e->data, need))
            __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED);
        else if (memcmp(ctx->ls + src + need, t_m.e->data + need, bytes - need))
            __atomic_add_fetch(&s_tail_i, 1, __ATOMIC_RELAXED);
        return;
    }
    if (t_m.state != 1) return;
    if (bytes > MEMO_IDX_MAX) { t_m.state = 0; return; }
    if (!t_m.idx) t_m.idx = (uint8_t*)malloc(MEMO_IDX_MAX);
    if (!t_m.idx) { t_m.state = 0; return; }
    memcpy(t_m.idx, ctx->ls + src, bytes);
    t_m.idx_bytes = bytes;
}

void edge_memo_cancel(void)
{
    if (t_m.state == 1) t_m.state = 0;
}

const uint8_t* edge_memo_vtx(uint32_t n, uint32_t rec)
{
    if (t_m.skin) return t_m.state == 2 && t_m.e->nverts == n ? t_m.e->kb.tag : NULL;
    if (t_m.state != 2 || t_m.e->nverts != n || t_m.e->rec != rec) return NULL;
    return memo_records(t_m.e);
}

/* This job's list and vertexes stay out of the ring. */
int edge_memo_resident(void)
{
    return t_m.state == 2 && !t_m.ring && memo_resident_mode() == 1;
}

/* the output stage cannot leave this result out of the ring */
void edge_memo_ring(spu_context* ctx)
{
    if (t_m.skin) edge_memo_revoke(ctx);
    else t_m.ring = 1;
}

int edge_memo_skip_decode(void) { return t_m.state == 2 && t_m.skin; }

/* a skinned result's part of the record, or NULL */
const void* edge_memo_skin_part(uint32_t* bytes)
{
    if (t_m.state != 2 || !t_m.skin || !t_k.taken) return NULL;
    *bytes = EK_REC_BYTES(t_k.rec->nmat);
    return t_k.rec;
}

/* Every job's output stage: the block at vio is a kept result's, or nobody's. */
void edge_memo_publish(uint32_t vio, uint32_t nverts)
{
    if (!memo_resident_mode()) return;
    const int kept = edge_memo_resident();
    if (kept) ((rsx_kept*)&t_m.e->kb)->put_verts = nverts;
    if (kept && t_m.skin && !t_m.lit) {         /* a position's record: no lights, the palette */
        static const uint8_t none[32];
        rsx_d3d12_vertex_pass_publish(vio, nverts, none, 32, 0, t_m.e->kb.tag, t_k.rec, EK_REC_BYTES(t_k.rec->nmat));
    }
    rsx_d3d12_kept_publish(vio, kept ? (rsx_kept*)&t_m.e->kb : NULL);
}

void edge_memo_vtx_note(const uint8_t* src, uint32_t n, uint32_t stride, uint32_t rec)
{
    if (t_m.skin && memo_skin_mode() == 2) { ek_check(src, n, stride); t_m.state = 0; return; }
    if (t_m.state == 3) {
        const uint8_t* d = memo_records(t_m.e);
        int bad = t_m.e->nverts != n || t_m.e->rec != rec, tail = 0;
        for (uint32_t v = 0; !bad && v < n; v++) {
            if (memcmp(d + (size_t)v * rec, src + (size_t)v * stride, rec)) {
                if (v < t_m.nverts) bad = 1; else tail = 1;
            }
        }
        if (bad) {
            __atomic_add_fetch(&s_bad_v, 1, __ATOMIC_RELAXED);
            if (t_m.lit) __atomic_add_fetch(&s_bad_lit, 1, __ATOMIC_RELAXED);
        } else if (tail) {
            __atomic_add_fetch(&s_tail_v, 1, __ATOMIC_RELAXED);
        }
        t_m.state = 0;
        return;
    }
    if (t_m.state != 1 || !t_m.idx_bytes) return;
    t_m.state = 0;
    /* kept the second time it is seen: a moving object's jobs have new inputs every frame */
    static volatile uint64_t seen[1u << 16];
    volatile uint64_t* const sn = &seen[t_m.k0 >> 48];
    if (*sn != t_m.k1) { *sn = t_m.k1; return; }
    if (t_m.skin) {
        const uint32_t in = t_m.lit ? EK_IN_LIT : EK_IN_POS;
        if (t_k.taken && n == t_k.count)
            memo_keep(t_m.k0, t_m.k1, t_m.idx, t_m.idx_bytes, t_k.in, n, in, in, NULL, 0, src, stride,
                      t_k.rec->flags & (EK_PN | EK_PT));
        return;
    }
    memo_keep(t_m.k0, t_m.k1, t_m.idx, t_m.idx_bytes, src, n, stride, rec, t_m.stream, t_m.stream_bytes, src, stride, 0);
    __atomic_add_fetch(&s_bytes, (long long)n * rec, __ATOMIC_RELAXED);
}

/* The arrays are wanted after all: unpack and the transforms now, in their order, from the arguments
 * they came with (all below the stack pointer is free where this is called). */
extern void edge_vtx_unpack_late(spu_context* ctx, const uint8_t* stream, const uint32_t a[5]);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000F538(spu_context* ctx);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E550(spu_context* ctx);
void edge_memo_revoke(spu_context* ctx)
{
    if (t_m.state != 2) return;
    t_m.state = 0;
    __atomic_add_fetch(&s_revoked, 1, __ATOMIC_RELAXED);
    const int skin = t_m.skin;
    t_m.skin = 0;
    if (skin && t_k.taken && t_k.pa && (!t_m.lit || (t_k.na && t_k.ta))) {   /* the arrays as the input piece left them */
        const uint8_t* in = memo_records(t_m.e);
        uint8_t* const ls = ctx->ls;
        const uint32_t n = t_k.count < t_m.e->nverts ? t_k.count : t_m.e->nverts;
        for (uint32_t v = 0; v < n; v++, in += t_m.e->rec)
            ek_give(in, ls + t_k.pa + 16u * v, t_m.lit ? ls + t_k.na + 16u * v : NULL, t_m.lit ? ls + t_k.ta + 16u * v : NULL);
    }
    if (!t_m.lit && !skin) {
        /* a position's array is what the packing would read: the records, w = 1.0 */
        for (int i = 0; i < t_m.ncalls; i++) {
            if (t_m.calls[i].kind != 2) continue;
            const uint8_t* r = memo_records(t_m.e);
            uint8_t* d = ctx->ls + t_m.calls[i].a[2];
            static const uint8_t one[4] = { 0x3F, 0x80, 0, 0 };
            for (uint32_t v = 0; v < t_m.e->nverts; v++) { memcpy(d + 16 * v, r + 12 * v, 12); memcpy(d + 16 * v + 12, one, 4); }
        }
        t_m.ncalls = 0;
        return;
    }
    u128 regs[128];
    memcpy(regs, ctx->gpr, sizeof regs);
    const int ie = ctx->int_enable;
    ctx->int_enable = 0;
    const int32_t si = (int32_t)ctx->image_id;
    for (int i = 0; i < t_m.ncalls; i++) {
        const memo_call* c = &t_m.calls[i];
        if (c->kind == 0 || c->kind == 2) {
            if (skin) continue;
            edge_vtx_unpack_late(ctx, memo_records(t_m.e) + (((size_t)t_m.e->nverts * t_m.e->rec + 15) & ~(size_t)15), c->a);
            continue;
        }
        ctx->gpr[0] = c->r[0];
        for (int r = 3; r < 12; r++) ctx->gpr[r] = c->r[r - 2];
        const uint32_t link = c->r[0]._u32[0];
        ctx->host_depth++;
        if (c->kind == 3) {
            jobmod_e8f44fbc6dbf_e6D98_spu_func_0000E550(ctx);
        } else {
            const uint32_t mat = (ctx->gpr[1]._u32[0] - 0x800u) & (SPU_LS_SIZE - 16);
            memcpy(ctx->ls + mat, c->mat, 64);
            ctx->gpr[6]._u32[0] = mat;
            jobmod_e8f44fbc6dbf_e6D98_spu_func_0000F538(ctx);
        }
        spu_drain_call(ctx, link);
        ctx->host_depth--;
        spu_img_restore(ctx, si);
    }
    t_m.ncalls = 0;
    ctx->int_enable = ie;
    memcpy(ctx->gpr, regs, sizeof regs);
}

/* the lighting stage runs on the SPU, or the packing reads what it did not write */
void edge_memo_revoke_lit(spu_context* ctx)
{
    if (t_m.lit) edge_memo_revoke(ctx);
}

#else
void edge_memo_job(void) { }
void gowa_edge_memo_9a40(spu_context* ctx) { (void)ctx; }
int edge_memo_left_out(spu_context* ctx) { (void)ctx; return 0; }
int edge_memo_unpack(spu_context* ctx, int lit, uint32_t src, uint32_t cnt, uint32_t p0, uint32_t n0, uint32_t t0)
{ (void)ctx; (void)lit; (void)src; (void)cnt; (void)p0; (void)n0; (void)t0; return 0; }
int edge_memo_idx(spu_context* ctx, uint32_t src, uint32_t* bytes) { (void)ctx; (void)src; (void)bytes; return 0; }
void edge_memo_idx_note(const spu_context* ctx, uint32_t src, uint32_t bytes) { (void)ctx; (void)src; (void)bytes; }
void edge_memo_cancel(void) { }
const uint8_t* edge_memo_vtx(uint32_t n, uint32_t rec) { (void)n; (void)rec; return NULL; }
int edge_memo_resident(void) { return 0; }
void edge_memo_publish(uint32_t vio, uint32_t nverts) { (void)vio; (void)nverts; }
void edge_memo_vtx_note(const uint8_t* src, uint32_t n, uint32_t stride, uint32_t rec) { (void)src; (void)n; (void)stride; (void)rec; }
void edge_memo_revoke(spu_context* ctx) { (void)ctx; }
void edge_memo_revoke_lit(spu_context* ctx) { (void)ctx; }
void edge_memo_ring(spu_context* ctx) { (void)ctx; }
int edge_memo_skip_decode(void) { return 0; }
const void* edge_memo_skin_part(uint32_t* bytes) { (void)bytes; return NULL; }
int edge_memo_skin_e550(spu_context* ctx) { (void)ctx; return 0; }
void edge_memo_skin_bbc8(spu_context* ctx) { (void)ctx; }
#endif
