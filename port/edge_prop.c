/*
 * A mod's proportions (docs/MODDING.md): a character drawn with other bone lengths than its animations have.
 *
 * The game builds a character's joint matrices from its animations, the bones as long as its own skeleton's, and
 * EDGE's skinning job takes them as a palette: node k's matrix at base + 64 k, the inverse bind times the joint's
 * world matrix (row vectors). Neither the skeleton's local table nor its inverse binds set those lengths (both
 * tried: the animations carry them). A mod whose mesh is to have other lengths gives, per node, the ratio of its
 * length to the original's and both inverse binds, in a file of its proportions\ folder; GOWA_MOD_PROPORTIONS has
 * those of the mods that are on ('|' between them).
 *
 * native/spu_edge_prop.c calls gowa_edge_prop at each skinned job's start (gowa_edge_job_ad98) with the job's slots.
 * It is the program's, not the game DLL's: it reads files and guards its reads of guest memory, which the DLL, built
 * with no C runtime, cannot. For a job whose segment record (slot 13) lies in a mesh of one of a file's sizes (its
 * header 0x0001000D, size, 0, 0, found by going back from the record; kept per mesh): the whole array is read from
 * memory (slot 18 holds its base; the job's own copy of the pieces is the memory's at its start), each joint's world
 * matrix recovered with the original bind, its transform to its parent taken, the translation scaled by the ratio,
 * the joints put together again from the root, and the palette made with the new inverse binds; the job's copies of
 * the palette's two pieces (slots 6 and 7, and a velocity job's previous ones, 8 and 9) are written over. Rotations
 * stay the animation's: the bones keep their directions, only their lengths change. Once an array per frame: a result
 * is kept by the array's address and a hash of its bytes. The memo's skinned jobs take the palette from the local store
 * later (0xE550), and leave it out of their key.
 *
 * File (scripts/proportions.py writes them): "GWPROP01", u32 count of mesh sizes (1 to 16), the sizes, u32 joints
 * (to 512), then per joint i32 parent (-1: none), f32 ratio, f32 inverse bind[16], f32 new inverse bind[16]:
 * little-endian, rows. Parents come before children. Not covered: the skinned jobs of the Grotto's own build (b7c8).
 */
#include "port_compat.h"
/* <windows.h> (port_compat.h on Windows) keeps the 16-bit pointer words far and near as empty macros; this file has
 * a variable called far. */
#undef far
#undef near
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;

#define PROP_FILES 8
#define PROP_MAX_JOINTS 512
#define PROP_MESHES 512
#define PROP_ARRAYS 8

typedef struct {
    int n, nsizes;
    uint32_t sizes[16];
    int parent[PROP_MAX_JOINTS];
    float ratio[PROP_MAX_JOINTS];
    float bind[PROP_MAX_JOINTS][16];      /* the original bind: inverse of the original inverse bind */
    float ib_new[PROP_MAX_JOINTS][16];
} prop_file;

typedef struct { uint32_t base, size; int prop; uint32_t used; } prop_mesh;
typedef struct { uint32_t base; uint64_t hash; int prop; uint32_t used; float pal[PROP_MAX_JOINTS][16]; } prop_array;

static prop_file* s_files[PROP_FILES];
static int s_nfiles = -1;
static prop_mesh s_mesh[PROP_MESHES];
static prop_array* s_arr[PROP_ARRAYS];
static uint32_t s_tick;
static volatile long s_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) port_cpu_relax();
}
static void unlock(void) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); }

static inline uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline float bef(const uint8_t* p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }
static inline void put_bef(uint8_t* p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 8); p[3] = (uint8_t)u;
}

/* out = a b (row vectors: a then b) */
static void mmul(const float* a, const float* b, float* out)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}

/* the inverse of an affine matrix (last column 0 0 0 1): rows 0-2 a 3x3, row 3 the translation */
static int ainv(const float* m, float* out)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (det > -1e-12f && det < 1e-12f) return 0;
    const float r = 1.0f / det;
    float o[16] = { A * r, -(b * i - c * h) * r, (b * f - c * e) * r, 0,
                    B * r, (a * i - c * g) * r, -(a * f - c * d) * r, 0,
                    C * r, -(a * h - b * g) * r, (a * e - b * d) * r, 0,
                    0, 0, 0, 1 };
    const float tx = m[12], ty = m[13], tz = m[14];
    o[12] = -(tx * o[0] + ty * o[4] + tz * o[8]);
    o[13] = -(tx * o[1] + ty * o[5] + tz * o[9]);
    o[14] = -(tx * o[2] + ty * o[6] + tz * o[10]);
    memcpy(out, o, sizeof o);
    return 1;
}

static int load_one(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "GWSKEL01", 8)) {   /* a whole skeleton's: port/skel_prop.c */
        fclose(f);
        return 0;
    }
    prop_file* p = (prop_file*)calloc(1, sizeof *p);
    uint32_t ns = 0, n = 0;
    int ok = p && !memcmp(magic, "GWPROP01", 8) && fread(&ns, 4, 1, f) == 1 && ns &&
             ns <= 16 && fread(p->sizes, 4, ns, f) == ns && fread(&n, 4, 1, f) == 1 && n && n <= PROP_MAX_JOINTS;
    for (uint32_t k = 0; ok && k < n; k++) {
        int32_t par;
        float ib[16];
        ok = fread(&par, 4, 1, f) == 1 && fread(&p->ratio[k], 4, 1, f) == 1 && fread(ib, 4, 16, f) == 16 &&
             fread(p->ib_new[k], 4, 16, f) == 16 && par < (int32_t)k && ainv(ib, p->bind[k]);
        p->parent[k] = par;
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "[props] %s: not a proportions file this build reads\n", path);
        free(p);
        return 0;
    }
    p->n = (int)n;
    p->nsizes = (int)ns;
    s_files[s_nfiles++] = p;
    fprintf(stderr, "[props] %s: %u joints, for meshes of %u bytes%s\n", path, n, p->sizes[0], ns > 1 ? " and more" : "");
    return 1;
}

static void load_all(void)
{
    s_nfiles = 0;
    const char* e = getenv("GOWA_MOD_PROPORTIONS");
    if (!e || !*e) return;
    char* buf = _strdup(e);
    if (!buf) return;
    char* next = NULL;
    for (char* s = strtok_s(buf, "|", &next); s && s_nfiles < PROP_FILES; s = strtok_s(NULL, "|", &next)) load_one(s);
    free(buf);
}

/* The mesh a record lies in: its header found by going back from the record (at most the largest size a file
 * names), kept by its address. -1: not one a file is for. Under the lock. */
static int mesh_of(uint32_t rec)
{
    for (int i = 0; i < PROP_MESHES; i++)
        if (s_mesh[i].size && rec >= s_mesh[i].base && rec < s_mesh[i].base + s_mesh[i].size) {
            s_mesh[i].used = ++s_tick;
            return s_mesh[i].prop;
        }
    uint32_t far = 0;
    for (int k = 0; k < s_nfiles; k++)
        for (int j = 0; j < s_files[k]->nsizes; j++)
            if (s_files[k]->sizes[j] > far) far = s_files[k]->sizes[j];
    uint32_t base = 0, size = 0;
    PORT_TRY {
        for (uint32_t h = rec & ~15u; h + far >= rec && h >= 0x10000; h -= 16) {
            const uint8_t* q = vm_base + h;
            if (be32(q) == 0x0001000D && !be32(q + 8) && !be32(q + 12)) {
                base = h;
                size = be32(q + 4);
                break;
            }
        }
    } PORT_EXCEPT {
        base = 0;
    }
    int prop = -1;
    if (!base || rec >= base + size) {        /* in no mesh a file is for: kept as a 16-byte span */
        base = rec & ~15u;
        size = 16;
    } else {
        for (int k = 0; k < s_nfiles && prop < 0; k++)
            for (int j = 0; j < s_files[k]->nsizes; j++)
                if (s_files[k]->sizes[j] == size) prop = k;
        if (prop >= 0)
            fprintf(stderr, "[props] a mesh of %u bytes at 0x%08X: its joints take a mod's proportions\n", size, base);
    }
    int slot = 0;
    for (int i = 1; i < PROP_MESHES; i++)
        if (s_mesh[i].used < s_mesh[slot].used) slot = i;
    s_mesh[slot].base = base;
    s_mesh[slot].size = size;
    s_mesh[slot].prop = prop;
    s_mesh[slot].used = ++s_tick;
    return prop;
}

static uint64_t hash_bytes(const uint8_t* p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* The new palette of the array at base, made or kept. Under the lock. */
static const prop_array* palette_of(uint32_t base, int prop)
{
    const prop_file* p = s_files[prop];
    /* The array, copied: a file of more joints than the character has would read past it. */
    static uint8_t src[PROP_MAX_JOINTS * 64];
    int read = 1;
    PORT_TRY {
        memcpy(src, vm_base + base, (size_t)p->n * 64);
    } PORT_EXCEPT {
        read = 0;
    }
    if (!read) return NULL;
    const uint64_t h = hash_bytes(src, (size_t)p->n * 64);
    for (int i = 0; i < PROP_ARRAYS; i++)
        if (s_arr[i] && s_arr[i]->base == base && s_arr[i]->hash == h && s_arr[i]->prop == prop) {
            s_arr[i]->used = ++s_tick;
            return s_arr[i];
        }
    int slot = 0;
    for (int i = 0; i < PROP_ARRAYS; i++) {
        if (!s_arr[i]) { slot = i; break; }
        if (s_arr[i]->used < s_arr[slot]->used) slot = i;
    }
    if (!s_arr[slot]) s_arr[slot] = (prop_array*)malloc(sizeof(prop_array));
    prop_array* a = s_arr[slot];
    if (!a) return NULL;
    static float world[PROP_MAX_JOINTS][16], fresh[PROP_MAX_JOINTS][16];
    for (int k = 0; k < p->n; k++) {
        float pal[16];
        for (int j = 0; j < 16; j++) pal[j] = bef(src + 64 * k + 4 * j);
        mmul(p->bind[k], pal, world[k]);
    }
    for (int k = 0; k < p->n; k++) {
        const int q = p->parent[k];
        if (q < 0) {
            memcpy(fresh[k], world[k], 64);
        } else {
            float inv[16], local[16];
            if (!ainv(world[q], inv)) memcpy(inv, world[q], 64);
            mmul(world[k], inv, local);
            local[12] *= p->ratio[k];
            local[13] *= p->ratio[k];
            local[14] *= p->ratio[k];
            mmul(local, fresh[q], fresh[k]);
        }
        mmul(p->ib_new[k], fresh[k], a->pal[k]);
    }
    a->base = base;
    a->hash = h;
    a->prop = prop;
    a->used = ++s_tick;
    return a;
}

/* The job's copies of two palette pieces (slots s, s + 1) written over from the array at base. */
static void write_pieces(uint8_t* ls, const uint32_t* lsa, const uint32_t* size, const uint32_t* ea, int s,
                         uint32_t base, const prop_array* a, int n)
{
    for (int sl = s; sl <= s + 1; sl++) {
        if (!size[sl]) continue;
        const uint32_t off = ea[sl] - base;
        if (off & 63 || off / 64 + size[sl] / 64 > (uint32_t)n) continue;
        uint8_t* d = ls + lsa[sl];
        for (uint32_t j = 0; j < size[sl] / 64; j++)
            for (int e = 0; e < 16; e++) put_bef(d + 64 * j + 4 * e, a->pal[off / 64 + j][e]);
    }
}

/* A skinned job at its start: ls its local store, and its slots' places there, sizes and EAs (spu_edge_job.h's
 * ej_view, EJ_SLOTS of each; slot 13 the segment's record, 18 the joint array's base, 6 and 7 the palette's pieces,
 * 8 and 9 a velocity job's last ones). 1 when it wrote the pieces anew. */
int gowa_edge_prop(uint8_t* ls, const uint32_t* slot_lsa, const uint32_t* slot_size, const uint32_t* slot_ea)
{
    if (s_nfiles == 0) return 0;
    if (s_nfiles < 0) {
        lock();
        if (s_nfiles < 0) load_all();
        unlock();
        if (!s_nfiles) return 0;
    }
    if (!slot_ea[18] || !slot_size[13] || !slot_size[6]) return 0;
    int did = 0;
    lock();
    const int prop = mesh_of(slot_ea[13]);
    if (prop >= 0) {
        const uint32_t base = slot_ea[18];
        const prop_array* a = palette_of(base, prop);
        if (a) {
            write_pieces(ls, slot_lsa, slot_size, slot_ea, 6, base, a, s_files[prop]->n);
            did = 1;
        }
        if (slot_size[8]) {          /* a velocity job: last frame's array, the same offsets */
            const uint32_t prev = slot_ea[8] - (slot_ea[6] - base);
            const prop_array* b = palette_of(prev, prop);
            if (b) write_pieces(ls, slot_lsa, slot_size, slot_ea, 8, prev, b, s_files[prop]->n);
        }
    }
    unlock();
    return did;
}
