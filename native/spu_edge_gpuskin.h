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

#endif
