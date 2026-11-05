/*
 * EDGE's job results, kept (spu_edge_memo.c).
 */
#ifndef SPU_EDGE_MEMO_H
#define SPU_EDGE_MEMO_H

#include "spu_context.h"
#include <stdint.h>

/* the job's entry */
void edge_memo_job(void);
/* a transform at its entry: 1 = left out, its arguments kept */
int edge_memo_left_out(spu_context* ctx);
/* unpack, with the attribute list filled: 1 = the arrays left out, the arguments kept */
int edge_memo_unpack(spu_context* ctx, int lit, uint32_t src, uint32_t cnt, uint32_t p0, uint32_t n0, uint32_t t0);
/* the index stage: 1 = the list is in place at src, *bytes long */
int edge_memo_idx(spu_context* ctx, uint32_t src, uint32_t* bytes);
/* the stage made the list: bytes at src */
void edge_memo_idx_note(const spu_context* ctx, uint32_t src, uint32_t bytes);
/* a stage ran lifted: nothing to keep */
void edge_memo_cancel(void);
/* the packing: a result's n records of rec bytes, or NULL */
const uint8_t* edge_memo_vtx(uint32_t n, uint32_t rec);
/* the packing made n records, stride apart, the first rec bytes of each to keep */
void edge_memo_vtx_note(const uint8_t* src, uint32_t n, uint32_t stride, uint32_t rec);
/* the arrays are wanted after all: in any job; in a lit job */
void edge_memo_revoke(spu_context* ctx);
void edge_memo_revoke_lit(spu_context* ctx);

#endif
