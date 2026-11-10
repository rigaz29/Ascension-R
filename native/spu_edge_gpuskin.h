/*
 * EDGE's skinning in the vertex pass (spu_edge_gpuskin.c): what a skinned job hands over, and its
 * version on the CPU.
 */
#ifndef SPU_EDGE_GPUSKIN_H
#define SPU_EDGE_GPUSKIN_H

#include <stdint.h>

#define EK_MAX_MATS 256u

/* 0xE550 ends by normalizing the normal, the tangent */
enum { EK_PN = 1, EK_PT = 2 };

/* What changes from frame to frame, host order: the palette, each matrix as the three rows 0xE804
 * lays out. Part of a job's vertex pass record. */
typedef struct {
    uint32_t flags, nmat, pad[2];
    float    rows[EK_MAX_MATS][3][4];
} ek_rec;
#define EK_REC_BYTES(n) (16u + 48u * (uint32_t)(n))

/* What stays, a vertex's inputs in host order: a lit job's position, normal, tangent, and bone bytes
 * (weight, index) x 4. A position job's has the position and the bones. */
#define EK_IN_LIT 64u
#define EK_IN_POS 32u

void ek_rows(ek_rec* r, const uint8_t* palette, uint32_t nmat);
/* one vertex's input record from the local store's arrays, and its arrays back */
void ek_take(uint8_t* in, const uint8_t* p, const uint8_t* n, const uint8_t* t, const uint8_t* bones);
void ek_give(const uint8_t* in, uint8_t* p, uint8_t* n, uint8_t* t);
/* the first 24 bytes of a lit vertex, or the 12 of a position's, from its input record */
void ek_vertex_lit(const ek_rec* r, const uint8_t* in, uint8_t* out);
void ek_vertex_pos(const ek_rec* r, const uint8_t* in, uint8_t* out);

/* An unskinned job's transforms in the vertex pass (GOWA_EDGE_RIGID, spu_edge_memo.c): 0xF538's
 * constants, the four vectors its set-up makes from a matrix, for the position (the call that links
 * back to 0x5850) and in a lit job the normal (0x5A18) and the tangent (0x5A50). In a record's part
 * where a skinned job has its palette: ER_RIGID in flags tells the two apart. */
enum { ER_RIGID = 0x100 };
enum { ER_POS = 0, ER_NRM = 1, ER_TAN = 2 };
typedef struct {
    uint32_t flags;                     /* ER_RIGID; EK_PN, EK_PT: 0xBBC8 normalizes the normal, the tangent */
    uint32_t arrays, pad[2];            /* bit k: array k's constants */
    float    c[3][4][4];                /* array, constant c0..c3, lane */
} er_rec;
#define ER_REC_BYTES ((uint32_t)sizeof(er_rec))
/* What stays, a vertex's inputs in host order, as 0xF538 reads them: a lit job's position, normal and
 * tangent; a position job's position. */
#define ER_IN_LIT 48u
#define ER_IN_POS 16u

/* 0xF538 on one vertex: out[l] = fma(v[l+3], c3[l], fma(v[l+2], c2[l], fma(v[l+1], c1[l], v[l] * c0[l]))),
 * the lanes mod 4; exact under the SPU's MXCSR */
void er_xf(const float c[4][4], const float v[4], float o[4]);
void er_vertex_lit(const er_rec* r, const uint8_t* in, uint8_t* out);
void er_vertex_pos(const er_rec* r, const uint8_t* in, uint8_t* out);

#endif
