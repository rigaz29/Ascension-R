/*
 * The EDGE geometry job as a whole (spu_edge_job.c): what the job's start tells about it.
 */
#ifndef SPU_EDGE_JOB_H
#define SPU_EDGE_JOB_H

#include "spu_context.h"
#include <stdint.h>

#define EJ_SLOTS 22

enum { EJ_POS, EJ_POS_SKIN, EJ_LIT, EJ_LIT_SKIN, EJ_OTHER, EJ_KINDS };
enum { EJ_BAD_LAYOUT = 1 };

/* A job's input set and descriptor, read from the local store at the job's entry. */
typedef struct {
    uint32_t base, end;                 /* the job's buffer set */
    uint32_t slot_lsa[EJ_SLOTS];        /* each filled slot's place in the set (0: empty) */
    uint32_t slot_size[EJ_SLOTS];
    uint32_t slot_ea[EJ_SLOTS];
    uint32_t desc[40];                  /* LS 0x0C00: words 14-39 are the job's */
    uint32_t out_piece, in_piece, meth_piece;   /* slots 1, 2, 3 (their EAs) */
    int skinned;                        /* slot 6 filled: the matrix palette */
    int kind;                           /* EJ_POS ... EJ_OTHER */
    uint16_t nverts, nidx;              /* the segment header's counts (slot 13) */
} ej_view;

/* 0, or EJ_BAD_LAYOUT when the input set does not look as described. */
int ej_read(const spu_context* ctx, ej_view* v);

#endif
