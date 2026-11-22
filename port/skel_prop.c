/*
 * Ascension-R -- a mod's proportions for a whole skeleton, where the animation puts its joints together
 *
 * The hero's animation job (job module 0243f9351a06, entry 0xBB40) takes, for a skeleton, its joint table (8 bytes a
 * joint, the parent in the fourth u16: the skeleton entry's records from +0x24), then the joints' local 4x4 matrices
 * (row vectors, the translation in row 3, so the bones' lengths; 64 bytes a joint, in GETs of at most 16 KB), then
 * last frame's world matrices; it builds the world matrices from the locals in its local store and puts them out.
 * Everything else is made from those: the skinning's palettes (inverse bind x world), what hangs on a joint (weapons,
 * chains, the blades on the back), effects. See docs/notes/2026-11-21-hero-skeleton-job.md.
 *
 * A file (scripts/proportions.py, "where": "animation") names a table by its size and a hash of its bytes, and gives
 * each joint a ratio: its new length over its own, or three, one for each of its translation's x, y and z in its
 * parent's axes (a weapon on the back brought in to a slimmer back). When that job (its code in the local store) GETs
 * that table, its
 * next GETs of the locals' sizes are the locals: their translations are scaled in the local store. The game's own
 * copy stays as it was, so nothing builds up from frame to frame, and last frame's world matrices (the next GETs) are
 * left alone. Before the first piece is scaled, its translations are checked against the file's bind lengths (most
 * joints keep theirs in an animation): a piece that does not look like the skeleton's locals is let by. Other jobs
 * GET the same table (one before its code is even loaded); their transfers are not this hook's.
 *
 * A mesh made for the new lengths needs the inverse binds of its rest pose in its skeleton (proportions.py --binds).
 *
 * File: "GWSKEL01", u32 joints (1 to 1024), u32 the table GET's size, u64 FNV-1a of the table's 8 x joints bytes,
 * then per joint f32 ratios x, y, z, f32 bind length (the translation's length in the skeleton's local table);
 * little-endian.
 * Files come in GOWA_MOD_PROPORTIONS with port/edge_prop.c's ("|" between them); each reads its own kind.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_compat.h"

#include "spu_context.h"

typedef void (*skel_get_hook_fn)(spu_context* spu, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd);
extern skel_get_hook_fn g_spu_get_hook;   /* runtime/spu/spu_dma.h */

#define SKEL_FILES 4
#define SKEL_MAX_JOINTS 1024
#define SKEL_PIECE 16384u

typedef struct {
    uint32_t n, table_bytes;
    uint64_t hash;
    float* ratio;       /* 3 a joint */
    float* len;
    const char* name;
    volatile long applied, refused;
} skel_file;

static skel_file* s_sk[SKEL_FILES];
static int s_nsk;

/* The animation job: job module 0243f9351a06's first code, at LS 0x4030 while it runs. */
#define SKEL_JOB_CODE_LSA 0x4030u
static const uint8_t k_job_code[32] = {
    0x40, 0x80, 0x02, 0x36, 0x34, 0x00, 0x81, 0xBA, 0x40, 0x80, 0x13, 0x32, 0x3E, 0xC0, 0x01, 0x95,
    0x40, 0x80, 0x12, 0x2E, 0x38, 0x8D, 0x81, 0xB8, 0x40, 0x80, 0x12, 0xAA, 0x3E, 0xC1, 0x01, 0x94 };

/* Per SPU thread: the table the job took last, how much of its locals came since, and how many GETs of the locals'
 * size were something else (last frame's world matrices come after the locals; a call may take a table and no
 * locals). */
#define SKEL_OTHER_MAX 4
static SPU_THREAD_LOCAL skel_file* t_f;
static SPU_THREAD_LOCAL const spu_context* t_ctx;
static SPU_THREAD_LOCAL uint32_t t_done, t_other;

static int in_job(const spu_context* spu)
{
    return !memcmp(spu->ls + SKEL_JOB_CODE_LSA, k_job_code, sizeof k_job_code);
}

static uint64_t fnv64(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static inline float get_bef(const uint8_t* p)
{
    const uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline void put_bef(uint8_t* p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

/* Most joints with a bone of some length keep it in an animation; a world matrix array does not. */
static int looks_local(const skel_file* f, const uint8_t* m, uint32_t first, uint32_t count, uint32_t* tried_out,
                       uint32_t* kept_out)
{
    uint32_t tried = 0, kept = 0;
    for (uint32_t k = first; k < first + count; k++) {
        const float want = f->len[k];
        if (want < 0.05f) continue;
        const uint8_t* t = m + 64 * (k - first) + 48;
        const float x = get_bef(t), y = get_bef(t + 4), z = get_bef(t + 8);
        const float l = sqrtf(x * x + y * y + z * z);
        tried++;
        if (fabsf(l - want) <= 0.02f * want) kept++;
    }
    *tried_out = tried;
    *kept_out = kept;
    return tried >= 8 && kept * 10 >= tried * 7;
}

static void scale(const skel_file* f, uint8_t* m, uint32_t first, uint32_t count)
{
    for (uint32_t k = first; k < first + count; k++) {
        const float* r = f->ratio + 3 * k;
        if (r[0] == 1.0f && r[1] == 1.0f && r[2] == 1.0f) continue;
        uint8_t* t = m + 64 * (k - first) + 48;
        for (int i = 0; i < 3; i++) put_bef(t + 4 * i, get_bef(t + 4 * i) * r[i]);
    }
}

static void skel_get(spu_context* spu, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    (void)ea;
    (void)cmd;
    lsa &= SPU_LS_MASK;
    if (lsa + size > SPU_LS_SIZE) return;
    for (int i = 0; i < s_nsk; i++) {
        skel_file* f = s_sk[i];
        if (size != f->table_bytes || fnv64(spu->ls + lsa, (size_t)f->n * 8) != f->hash) continue;
        if (in_job(spu)) {
            t_f = f;
            t_ctx = spu;
            t_done = 0;
            t_other = 0;
        }
        return;
    }
    skel_file* f = t_f;
    if (!f || t_ctx != spu) return;
    const uint32_t total = 64 * f->n, left = total - t_done;
    if (size != (left < SKEL_PIECE ? left : SKEL_PIECE) || (lsa & 15) || !in_job(spu)) return;
    const uint32_t first = t_done / 64, count = size / 64;
    uint32_t tried = 0, kept = 0;
    if (first == 0 && !looks_local(f, spu->ls + lsa, first, count, &tried, &kept)) {
        /* A skeleton coming in has locals of no length for its first frames; after that, a table whose locals do not
         * come is a frame of the old lengths: said, up to a point. */
        if (++t_other < SKEL_OTHER_MAX) return;
        t_f = NULL;
        const long r = __atomic_add_fetch(&f->refused, 1, __ATOMIC_RELAXED);
        const long a = __atomic_load_n(&f->applied, __ATOMIC_RELAXED);
        if (r <= 3 || (a && r <= 20))
            fprintf(stderr, "[props] %s: no joints came after the skeleton's table (the last GET: %u of %u bone "
                    "lengths), left alone (%ld so far, %ld scaled)\n", f->name, kept, tried, r, a);
        return;
    }
    scale(f, spu->ls + lsa, first, count);
    t_done += size;
    if (t_done >= total) {
        t_f = NULL;
        const long a = __atomic_add_fetch(&f->applied, 1, __ATOMIC_RELAXED);
        if (a == 1 || a % 20000 == 0)
            fprintf(stderr, "[props] %s: %u joints take the new lengths where the animation puts them together (%ld "
                    "times, %ld tables without them)\n", f->name, f->n, a, __atomic_load_n(&f->refused,
                    __ATOMIC_RELAXED));
    }
}

static void load_one(const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return;
    char magic[8];
    uint32_t n = 0, tb = 0;
    uint64_t hash = 0;
    int ok = fread(magic, 1, 8, fp) == 8 && !memcmp(magic, "GWSKEL01", 8);
    if (!ok) {             /* port/edge_prop.c's kind, or not a proportions file: that one says so */
        fclose(fp);
        return;
    }
    ok = fread(&n, 4, 1, fp) == 1 && fread(&tb, 4, 1, fp) == 1 && fread(&hash, 8, 1, fp) == 1 && n &&
         n <= SKEL_MAX_JOINTS && tb >= 8 * n && tb <= 8 * n + 15 && s_nsk < SKEL_FILES;
    skel_file* f = ok ? (skel_file*)calloc(1, sizeof *f) : NULL;
    float* ratio = ok ? (float*)malloc(12 * (size_t)n) : NULL;
    float* len = ok ? (float*)malloc(4 * (size_t)n) : NULL;
    ok = f && ratio && len;
    for (uint32_t k = 0; ok && k < n; k++) {
        float* r = ratio + 3 * k;
        ok = fread(r, 4, 3, fp) == 3 && fread(&len[k], 4, 1, fp) == 1;
        for (int i = 0; ok && i < 3; i++) ok = r[i] > 0.0f && r[i] < 8.0f;
    }
    fclose(fp);
    if (!ok) {
        fprintf(stderr, "[props] %s: not a skeleton proportions file this build reads\n", path);
        free(f);
        free(ratio);
        free(len);
        return;
    }
    const char* b = strrchr(path, '\\');
    if (!b) b = strrchr(path, '/');
    f->name = _strdup(b ? b + 1 : path);
    f->n = n;
    f->table_bytes = tb;
    f->hash = hash;
    f->ratio = ratio;
    f->len = len;
    uint32_t other = 0;
    for (uint32_t k = 0; k < n; k++)
        other += ratio[3 * k] != 1.0f || ratio[3 * k + 1] != 1.0f || ratio[3 * k + 2] != 1.0f;
    s_sk[s_nsk++] = f;
    fprintf(stderr, "[props] %s: a skeleton of %u joints, %u of them another length (at the animation)\n", path, n,
            other);
}

void gowa_skel_prop_start(void);

/* Once at the start, before the guest runs. */
void gowa_skel_prop_start(void)
{
    const char* e = getenv("GOWA_MOD_PROPORTIONS");
    if (!e || !*e) return;
    char* buf = _strdup(e);
    if (!buf) return;
    char* next = NULL;
    for (char* s = strtok_s(buf, "|", &next); s; s = strtok_s(NULL, "|", &next)) load_one(s);
    free(buf);
    if (s_nsk) g_spu_get_hook = skel_get;
}
