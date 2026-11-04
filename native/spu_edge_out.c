/*
 * EDGE's output stage, 0x9080 of the job module (e8f44fbc6dbf), as native code: the C job's first
 * piece (docs/notes/2026-11-04-edge-host-reference.md). GOWA_EDGE_NATIVE bit 131072, on unless
 * GOWA_EDGE_NATIVE leaves it out; GOWA_EDGE_NATIVE_STATS=1 counts.
 *
 * What the stage does, for a job that draws:
 *  - the ring allocation: round128(round16(vertexes * stride) + 2 * round8(indexes)) bytes of the
 *    SPU's main ring (0xC628): the ring line's GETLLAR and PUTLLUC around the room check (0xB700),
 *    native when the GPU's position is known and there is room, else the lifted 0xB700 (the label
 *    read and the wait);
 *  - the index PUT (0xC340) and the vertex PUT(s) (0xC490), through the DMA helper 0xAF38 (pieces
 *    of at most 16 KB), each between a read of SPU_RdMachStat and interrupts back on;
 *  - the vertex packing (0xD158): the output piece, called through c + 0x8C;
 *  - the hole, put together in the local store at the hole's own 128-byte offset (0xC9A8), a word
 *    at a time: the method piece (0x5D68, lifted), the program binding (0x84C0, lifted, when there
 *    is one), and per pass the viewport (0xB3D0), the scissor (0xB2D0), 0x1EFC x5 (0xB1A8),
 *    INVALIDATE_VERTEX_FILE, INDEX_ARRAY_ADDRESS, BEGIN_END and DRAW_INDEX_ARRAY;
 *  - the end of the hole (0xCC98): the labels, the methods split at 128-byte lines, a NOP over the
 *    rest, and its fenced PUT.
 * The words the stage does not write keep what the local store held: so does this.
 *
 * Its arithmetic is the lifted code's, op for op, with the same SPU helpers on locals: it leaves
 * out the context's register file, the interrupt checks at every label and the dispatcher's round
 * trips at every interrupt enable and disable. It runs only with no interrupt pending (the job
 * manager's handler has never run during this stage in 280 captured jobs) and with no stride
 * callback (c + 0x88). Its channel operations are the lifted code's, in its order.
 *
 * Checked by the replay's state mode (`gowa.py replay`, its default: SPU_JOB_REPLAY_CMP=state
 * SPU_JOB_REPLAY_STATE=0-3E000,3F780-40000, all but the frames below the job's main frame).
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u128 spu_rdch(spu_context* ctx, uint32_t channel);   /* the runtime's (spu_channels.c) */
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000C628(spu_context*);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000C9A8(spu_context*);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_00005D68(spu_context*);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_000084C0(spu_context*);

#define LD(a)    spu_ls_read128(ctx, (uint32_t)(a))
#define ST(a, v) spu_ls_write128(ctx, (uint32_t)(a), (v))
#define W(x)     ((x)._u32[0])

static volatile long long s_calls, s_draws, s_empty, s_declined_irq, s_declined_cb, s_declined_args,
                          s_c9a8_lifted, s_c628_lifted, s_b700_lifted, s_program, s_halts;
static int s_stats;                     /* GOWA_EDGE_NATIVE_STATS: the counts (six threads share them) */
#define EO_COUNT(v) do { if (s_stats) __atomic_add_fetch(&(v), 1, __ATOMIC_RELAXED); } while (0)

static void eo_stats(void);

/* A call to lifted code, as the lifted caller makes it. */
static void eo_call(spu_context* ctx, uint32_t link, void (*fn)(spu_context*))
{
    ctx->gpr[0] = spu_link(link);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++;
    fn(ctx);
    spu_drain_call(ctx, link);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
}

static void eo_calli(spu_context* ctx, uint32_t link, uint32_t target)
{
    ctx->gpr[0] = spu_link(link);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->pc = target;
    ctx->host_depth++;
    spu_indirect_branch(ctx);
    spu_drain_call(ctx, link);
    ctx->host_depth--;
    spu_img_restore(ctx, si);
}

/* The lifted code's halt on a failed assertion. */
static int eo_halt(spu_context* ctx, uint32_t pc)
{
    extern void spu_halt(spu_context*);
    fprintf(stderr, "[spu] HALT-ASSERT heqi pc=0x%05X img=%d (edge-out native)\n", pc, ctx->image_id);
    __atomic_add_fetch(&s_halts, 1, __ATOMIC_RELAXED);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
    spu_halt(ctx);
    return 1;
}

/* 0xD118: the output stride (byte 1 of the format at c + 0x74; 0 without one). */
static u128 eo_d118(spu_context* ctx, u128 r3)
{
    const u128 r12 = LD(W(r3) + 0x70);
    u128 r10 = spu_rotqbyi(r12, 4);
    const u128 r11 = spu_ai(r10, 1);
    const u128 r9 = spu_ai(r10, 14);
    const u128 r6 = spu_ceqi(r10, 0);
    const u128 r8 = LD(W(r11));
    const u128 r7 = spu_rotqby(r8, r9);
    const u128 r4 = spu_andi(r7, 255);
    return spu_selb(r4, spu_il(0), r6);
}

/* 0xAF38: a transfer in pieces (16 KB, then the 16-byte multiple, then 8, 4, 2, 1). */
static void eo_af38(spu_context* ctx, u128 r3, u128 r4, u128 r5, u128 r6, u128 r7)
{
    const u128 r12 = r6, r11 = r7;
    const u128 r13 = LD(0xF860);
    u128 r9 = spu_shlqbyi(r3, 0), r8 = r4;
    const u128 r14 = spu_il(16383);
    for (;;) {
        r3 = r9;
        const u128 r10 = spu_shlqbyi(r8, 4);
        r4 = spu_clgti(r5, 3);
        r6 = spu_clgti(r5, 1);
        if (W(r5) == 0) return;
        const u128 r30 = spu_il(1), r31 = spu_il(2);
        const u128 r27 = spu_ceqi(r4, 0);
        spu_wrch(ctx, MFC_LSA, r3);
        const u128 r26 = spu_selb(r30, r31, r6);
        const u128 r15 = spu_shlqbyi(r8, 0);
        const u128 r29 = spu_clgti(r5, 7);
        const u128 r28 = spu_il(4);
        const u128 r25 = spu_clgti(r5, 15);
        const u128 r23 = spu_ceqi(r29, 0);
        const u128 r22 = spu_selb(r28, r26, r27);
        spu_wrch(ctx, MFC_EAH, r15);
        const u128 r24 = spu_il(8);
        spu_wrch(ctx, MFC_EAL, r10);
        const u128 r21 = spu_clgt(r5, r14);
        const u128 r18 = spu_andi(r5, -16);
        const u128 r19 = spu_ceqi(r25, 0);
        const u128 r20 = spu_selb(r24, r22, r23);
        const u128 r6b = spu_ceqi(r21, 0);
        const u128 r17 = spu_selb(r18, r20, r19);
        const u128 r16 = spu_il(16384);
        const u128 r7b = spu_selb(r16, r17, r6b);
        r5 = spu_sf(r7b, r5);
        const u128 r4b = spu_rotqmbyi(r7b, -4);
        r9 = spu_a(r9, r7b);
        spu_wrch(ctx, MFC_Size, r7b);
        spu_wrch(ctx, MFC_TagID, r12);
        spu_wrch(ctx, MFC_Cmd, r11);
        const u128 r3c = spu_cg(r8, r4b);
        u128 r2 = spu_shufb(r3c, r3c, r13);
        r2 = spu_addx(r8, r4b, r2);
        r8 = r2;
    }
}

/* 0xC340 (indexes) and 0xC490 (vertexes): one PUT into the ring, at the allocation's cursor. */
static int eo_put(spu_context* ctx, int vertexes, u128 c, u128 count, u128 local, u128 out, u128 sp)
{
    if (vertexes) {
        const u128 r6 = LD(W(c));
        const u128 r4 = spu_rotqbyi(r6, 13);
        if (W(spu_andi(r4, 16)) != 0) return 0;
    } else if (W(count) == 0) {
        return 0;
    }
    const u128 ms = spu_rdch(ctx, SPU_RdMachStat);
    ctx->int_enable = 0;
    const u128 r84 = spu_cwd(sp, 0), r85 = spu_cwd(sp, 4), r22 = spu_cwd(sp, 8);
    u128 bytes, r29, r81, src;
    if (vertexes) {
        const u128 r43 = LD(W(c));
        const u128 stride = eo_d118(ctx, c);
        r29 = LD(W(local));
        const u128 r41 = LD(W(out));
        const u128 r38 = LD(W(c));
        const u128 r37 = LD(W(c) + 0x50);
        (void)r43;
        r81 = spu_rotqbyi(r29, 4);
        const u128 r35 = spu_shufb(r29, r41, r84);
        const u128 r34 = spu_rotqbyi(r38, 6);
        src = spu_rotqbyi(r37, 4);
        const u128 r24 = spu_shufb(r81, r35, r85);
        const u128 r30 = spu_mpyu(r34, stride);
        const u128 r29b = spu_mpyh(stride, r34);
        ST(W(out), r24);
        const u128 r27 = LD(W(local));
        const u128 r23 = spu_rotqbyi(r27, 8);
        bytes = spu_andi(spu_ai(spu_a(r29b, r30), 15), -16);
        ST(W(out), spu_shufb(r23, r24, r22));
    } else {
        const u128 r27 = spu_ai(count, 7);
        r29 = LD(W(local));
        const u128 r31 = LD(W(out));
        const u128 r24 = spu_andi(r27, -8);
        src = LD(W(c) + 0x50);
        r81 = spu_rotqbyi(r29, 4);
        bytes = spu_a(r24, r24);
        const u128 r25 = spu_shufb(r29, r31, r84);
        const u128 r21 = spu_shufb(r81, r25, r85);
        ST(W(out), r21);
        const u128 r23 = LD(W(local));
        const u128 r20 = spu_rotqbyi(r23, 8);
        ST(W(out), spu_shufb(r20, r21, r22));
    }
    const u128 tag = LD(W(c) + 0x150);
    if ((W(r29) & 15) || (W(src) & 15) || W(tag) > 31)
        return eo_halt(ctx, vertexes ? 0xC5A0 : 0xC40C);
    eo_af38(ctx, src, spu_rotqmbyi(r29, -4), bytes, tag, spu_il(32));
    const u128 r83 = spu_a(r29, bytes);
    const u128 r13 = LD(W(local));
    const u128 r10 = spu_a(r81, bytes);
    const u128 r11 = spu_shufb(r83, r13, r84);
    ST(W(local), spu_shufb(r10, r11, r85));
    if (W(ms) & 1) ctx->int_enable = 1;
    const u128 r6 = LD(W(local) + 0x10);
    ST(W(local) + 0x10, spu_shufb(spu_il(1), r6, spu_cbd(sp, 13)));
    return 0;
}

/* 0xD158: the vertex packing: the output piece through c + 0x8C; c + 0x50 word 1 = its buffer. */
static void eo_d158(spu_context* ctx, u128 c, u128 sp)
{
    /* sp: 0xD158's own frame (the caller's - 80), for the piece's call */
    const u128 r5 = LD(W(c));
    if (W(spu_andi(spu_rotqbyi(r5, 13), 16)) != 0) return;
    const u128 buf = spu_rotqbyi(LD(W(c) + 0x50), 8);                 /* 0xB5F0 */
    const u128 r8 = LD(W(c) + 0x50);
    const u128 r3 = spu_rotqbyi(LD(W(c)), 6);
    ST(W(c) + 0x50, spu_shufb(buf, r8, spu_cwd(sp, 4)));
    if (((uint16_t)W(r3)) == 0) return;
    const u128 r12 = LD(W(c));
    const u128 r10 = LD(W(c) + 0x80);
    const u128 r2 = spu_rotqbyi(r10, 12);
    ctx->gpr[3] = spu_shlqbyi(c, 0);
    ctx->gpr[4] = spu_and(spu_ila(0xFFFF), r3);
    ctx->gpr[5] = buf;
    ctx->gpr[6] = spu_andi(spu_rotqbyi(r12, 1), 255);
    ctx->gpr[1] = sp;
    if (W(r2) != 0) eo_calli(ctx, 0xD1E4, W(r2));
    const u128 r80 = spu_rotqbyi(LD(W(c) + 0x50), 8);                 /* 0xB5F0 */
    const u128 stride = eo_d118(ctx, c);
    const u128 r20 = spu_rotqbyi(LD(W(c)), 6);
    const u128 r16 = spu_a(spu_mpyh(stride, r20), spu_mpyu(stride, r20));
    const u128 r4 = spu_a(r80, spu_andi(spu_ai(r16, 15), -16));
    ST(W(c) + 0x50, spu_shufb(r4, LD(W(c) + 0x50), spu_cwd(sp, 8)));  /* 0xB600 */
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000AFF0(spu_context*);

/* The hole's overflow: 0xAFF0 (its callback, else give up), lifted. Nonzero = give up. The hole is
 * sized for its content (c + 1), so this has not happened; it is here as the lifted code has it. */
static int eo_overflow(spu_context* ctx, u128 cursor, uint32_t words, uint32_t link, u128 sp)
{
    ctx->gpr[1] = sp;
    ctx->gpr[3] = cursor;
    ctx->gpr[4] = spu_il((int32_t)words);
    eo_call(ctx, link, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000AFF0);
    return ((uint16_t)W(spu_xsbh(spu_ceqbi(ctx->gpr[3], 0)))) == 0;
}

/* 0xC9A8 (the hole's buffer and cursor) for one entry of `local` (n = 1) or none. 0 = done; 1 = the
 * entry asks for the label read (a 4-byte GET; never seen), which the caller leaves to the lifted
 * function, before anything here has changed. */
static int eo_c9a8(spu_context* ctx, u128 c, u128 cursor, u128 hole_ea, u128 local, uint32_t n, u128 sp)
{
    /* the entry loop's first test reads the word at LS 0, then each entry's word 2 */
    if (W(LD(0)) == 0) return 1;
    if (n == 1) {
        const u128 e = LD(W(local) + 8);
        if (W(spu_rotqby(e, spu_ai(local, 8))) == 0) return 1;
    } else if (n != 0) {
        return 1;
    }
    const u128 r80 = spu_andi(hole_ea, 127);
    const u128 s = spu_rotqbyi(LD(W(c) + 0x50), 8);                   /* 0xB5F0 */
    ST(W(c) + 0x50, spu_shufb(spu_ai(s, 16), LD(W(c) + 0x50), spu_cwd(sp, 8)));
    const u128 f = spu_rotqbyi(LD(W(c) + 0x50), 8);
    const u128 r32 = LD(W(cursor));
    const u128 r36 = spu_andi(f, -128);
    const u128 r33 = spu_cwd(cursor, 0);
    const u128 r30 = spu_cwd(cursor, 8);
    const u128 r34 = spu_or(r36, r80);
    const u128 r11 = spu_cwd(cursor, 4);
    const u128 r12 = spu_cwd(cursor, 12);
    const u128 r3 = spu_clgt(f, r34);
    const u128 r35 = spu_ai(r34, 128);
    const u128 r19 = spu_selb(r34, r35, r3);
    ST(W(cursor), spu_shufb(r19, r32, r33));
    const u128 r29 = LD(W(cursor) + 8);
    ST(W(cursor) + 8, spu_shufb(r19, r29, r30));
    const u128 r26 = LD(W(c));
    const u128 r14 = LD(W(cursor) + 4);
    const u128 r25 = spu_rotqbyi(r26, 14);
    const u128 r24 = spu_ceqbi(r25, 0);
    const u128 r23 = spu_andi(r25, 255);
    const u128 r22 = spu_xsbh(r24);
    const u128 r20 = spu_shli(r23, 4);
    const u128 r21 = spu_ceqhi(r22, 0);
    const u128 r18 = spu_fsm(r21);
    const u128 r16 = spu_a(r19, r20);
    const u128 r17 = spu_andc(spu_il(4), r18);
    const u128 r15 = spu_a(r16, r17);
    ST(W(cursor) + 4, spu_shufb(r15, r14, r11));
    const u128 r7 = LD(W(cursor) + 12);
    ST(W(cursor) + 12, spu_shufb(spu_fsmbi(0x0), r7, r12));
    ST(W(c) + 0x50, spu_shufb(spu_andi(spu_ai(r15, 127), -128), LD(W(c) + 0x50), spu_cwd(sp, 8)));
    /* the entries' flags: a 32-byte block of methods first when one asks for it (0xCB84..0xCC00) */
    u128 r6 = local, r7f = spu_il(0), r10 = spu_fsmbi(0x0);
    const u128 r85 = spu_il((int32_t)n);
    for (;;) {
        const u128 r74 = spu_ceq(r10, r85);
        const u128 r5 = spu_ai(r6, 28), r8 = spu_ai(r6, 41), r3b = spu_ai(r6, 29), r9 = spu_ai(r6, 42);
        const u128 r4 = spu_ceqbi(r7f, 0);
        r10 = spu_ai(r10, 1);
        if (W(r74) != 0) {
            if (((uint16_t)W(spu_xsbh(r4))) != 0) return 0;
            break;
        }
        const u128 r72 = LD(W(r5));
        const u128 r73 = spu_xsbh(r4);
        const u128 r69 = LD(W(r6) + 0x10);
        const u128 r70 = LD(W(r3b));
        const u128 r60 = spu_ceqhi(r73, 0);
        const u128 r71 = spu_rotqby(r72, r8);
        const u128 r67 = spu_rotqby(r69, r6);
        const u128 r64 = spu_rotqby(r70, r9);
        const u128 r68 = spu_ceqbi(r71, 0);
        r6 = spu_ai(r6, 32);
        const u128 r66 = spu_xsbh(r68);
        const u128 r63 = spu_ceqi(r67, 0);
        const u128 r65 = spu_ceqhi(r66, 0);
        const u128 r62 = spu_selb(r7f, r64, r65);
        const u128 r61 = spu_selb(r62, r7f, r63);
        r7f = spu_selb(r61, r7f, r60);
    }
    {
        const u128 r17 = spu_ai(cursor, 8);
        const u128 r11b = spu_cwd(cursor, 8);
        const u128 r12b = LD(0xF960);
        const u128 r2 = spu_il(-1);
        const u128 r16b = LD(W(r17));
        const u128 r13 = LD(0xF970);
        const u128 r76 = spu_rotqby(r16b, r17);
        const u128 r15b = spu_ai(r76, 32);
        const u128 r9b = spu_andi(r76, 15);
        const u128 r6b = spu_shufb(r15b, r16b, r11b);
        const u128 r4b = spu_sfi(r9b, 0);
        const u128 r5b = spu_sfi(r9b, 16);
        const u128 r79 = spu_rotqmby(r2, r4b);
        ST(W(cursor) + 8, r6b);
        const u128 r85b = LD(W(r76));
        const u128 r10b = LD(W(r76) + 0x20);
        const u128 r7b = spu_shlqby(r13, r5b);
        const u128 r78 = spu_shlqby(r12b, r5b);
        const u128 r82 = spu_rotqmby(r13, r4b);
        const u128 r81 = spu_rotqmby(r12b, r4b);
        ST(W(r76) + 0x20, spu_selb(r7b, r10b, r79));
        ST(W(r76) + 0x10, spu_selb(r78, r82, r79));
        ST(W(r76), spu_selb(r85b, r81, r79));
    }
    return 0;
}

/* 0xB3D0: the viewport (0x0A00, 0x0394, 0x0A20 x8). Nonzero = gave up. */
static int eo_b3d0(spu_context* ctx, u128 sp, u128 r3, u128 r4, u128 r5, u128 r6, u128 r7, u128 r8, u128 r9)
{
    const u128 r82 = r3, r85 = r4, r86 = r5, r83 = r6, r84 = r7, r80 = r8, r81 = r9;
    const u128 r10 = LD(W(r3));
    u128 r28 = spu_rotqbyi(r10, 8);
    if (W(spu_clgt(spu_ai(r28, 60), spu_rotqbyi(r10, 4))) != 0) {
        if (eo_overflow(ctx, r82, 15, 0xB5D8, spu_ai(sp, -144))) return 1;
        r28 = spu_rotqbyi(LD(W(r82)), 8);
    }
    const u128 r2 = spu_iohl(spu_ilhu(0x8), 0xA00);
    const u128 r79 = LD(W(r28));
    const u128 r31 = spu_cwd(r28, 0), r30 = spu_cwd(r28, 4), r13 = spu_cwd(r28, 8), r33 = spu_cwd(r28, 12);
    const u128 r78 = spu_shli(r83, 16);
    const u128 r76 = spu_shli(r84, 16);
    const u128 r24 = spu_fsmbi(0x0);
    const u128 r77 = spu_shufb(r2, r79, r31);
    const u128 r75 = spu_or(r78, r85);
    const u128 r27 = spu_cwd(sp, 8);
    const u128 r62 = spu_iohl(spu_ilhu(0x8), 0x394);
    const u128 r70 = spu_or(r76, r86);
    ST(W(r28), r77);
    const u128 r74 = LD(W(r28) + 4);
    const u128 r51 = spu_iohl(spu_ilhu(0x20), 0xA20);
    const u128 r26 = spu_cuflt(r83, 154);
    const u128 r71 = spu_cuflt(r85, 155);
    const u128 r73 = spu_shufb(r75, r74, r30);
    const u128 r63 = spu_cuflt(r84, 154);
    const u128 r68 = spu_cuflt(r86, 155);
    const u128 r58 = spu_ilhu(0x3F00);
    const u128 r65 = spu_fa(r80, r81);
    ST(W(r28) + 4, r73);
    const u128 r69 = LD(W(r28) + 8);
    const u128 r47 = spu_fa(r26, r71);
    const u128 r43 = spu_fa(r63, r68);
    const u128 r40 = spu_fm(r65, r58);
    const u128 r64 = spu_ilhu(0x8000);
    const u128 r67 = spu_shufb(r70, r69, r13);
    const u128 r57 = spu_fs(r81, r80);
    const u128 r25 = spu_xor(r63, r64);
    ST(W(r28) + 8, r67);
    const u128 r29 = spu_ai(r28, 60);
    const u128 r61 = LD(W(r28) + 12);
    const u128 r21 = spu_fm(r57, r58);
    ST(W(r28) + 12, spu_shufb(r62, r61, r33));
    ST(W(r28) + 0x10, spu_shufb(r80, LD(W(r28) + 0x10), r31));
    ST(W(r28) + 20, spu_shufb(r81, LD(W(r28) + 20), r30));
    ST(W(r28) + 24, spu_shufb(r51, LD(W(r28) + 24), r13));
    ST(W(r28) + 28, spu_shufb(r47, LD(W(r28) + 28), r33));
    ST(W(r28) + 0x20, spu_shufb(r43, LD(W(r28) + 0x20), r31));
    ST(W(r28) + 36, spu_shufb(r40, LD(W(r28) + 36), r30));
    ST(W(r28) + 40, spu_shufb(r24, LD(W(r28) + 40), r13));
    ST(W(r28) + 44, spu_shufb(r26, LD(W(r28) + 44), r33));
    ST(W(r28) + 0x30, spu_shufb(r25, LD(W(r28) + 0x30), r31));
    ST(W(r28) + 52, spu_shufb(r21, LD(W(r28) + 52), r30));
    ST(W(r28) + 56, spu_shufb(r24, LD(W(r28) + 56), r13));
    ST(W(r82), spu_shufb(r29, LD(W(r82)), r27));
    return 0;
}

/* 0xB2D0: the scissor (0x08C0). Nonzero = gave up. */
static int eo_b2d0(spu_context* ctx, u128 sp, u128 r3, u128 r4, u128 r5, u128 r6, u128 r7)
{
    const u128 r80 = r3, r84 = r4, r83 = r5, r81 = r6, r82 = r7;
    const u128 r9 = LD(W(r3));
    u128 r13 = spu_rotqbyi(r9, 8);
    if (W(spu_clgt(spu_ai(r13, 12), spu_rotqbyi(r9, 4))) != 0) {
        if (eo_overflow(ctx, r80, 3, 0xB3B8, spu_ai(sp, -112))) return 1;
        r13 = spu_rotqbyi(LD(W(r80)), 8);
    }
    const u128 r27 = spu_iohl(spu_ilhu(0x8), 0x8C0);
    const u128 r28 = spu_cwd(r13, 0), r24 = spu_cwd(r13, 4), r19 = spu_cwd(r13, 8);
    const u128 r12 = spu_cwd(sp, 8);
    const u128 r23 = spu_or(spu_shli(r81, 16), r84);
    const u128 r18 = spu_or(spu_shli(r82, 16), r83);
    ST(W(r13), spu_shufb(r27, LD(W(r13)), r28));
    ST(W(r13) + 4, spu_shufb(r23, LD(W(r13) + 4), r24));
    ST(W(r13) + 8, spu_shufb(r18, LD(W(r13) + 8), r19));
    ST(W(r80), spu_shufb(spu_ai(r13, 12), LD(W(r80)), r12));
    return 0;
}

/* 0xB1A8: 0x1EFC x5 (the value, then the four words at r5). Nonzero = gave up. */
static int eo_b1a8(spu_context* ctx, u128 sp, u128 r3, u128 r4, u128 r5)
{
    const u128 r81 = r3, r82 = r4, r80 = r5;
    const u128 r8 = LD(W(r3));
    u128 r17 = spu_rotqbyi(r8, 8);
    if (W(spu_clgt(spu_ai(r17, 24), spu_rotqbyi(r8, 4))) != 0) {
        if (eo_overflow(ctx, r81, 6, 0xB2B4, spu_ai(sp, -80))) return 1;
        r17 = spu_rotqbyi(LD(W(r81)), 8);
    }
    const u128 r45 = spu_iohl(spu_ilhu(0x14), 0x1EFC);
    const u128 r23 = spu_cwd(r17, 0), r13 = spu_cwd(r17, 4), r37 = spu_cwd(r17, 8), r30 = spu_cwd(r17, 12);
    const u128 r16 = spu_cwd(sp, 8);
    const u128 r46 = LD(W(r80));
    const u128 r36 = spu_rotqby(r46, r80);
    ST(W(r17), spu_shufb(r45, LD(W(r17)), r23));
    const u128 r40 = LD(W(r80) + 4);
    const u128 r29 = spu_rotqby(r40, spu_ai(r80, 4));
    ST(W(r17) + 4, spu_shufb(r82, LD(W(r17) + 4), r13));
    const u128 r33 = LD(W(r80) + 8);
    const u128 r22 = spu_rotqby(r33, spu_ai(r80, 8));
    ST(W(r17) + 8, spu_shufb(r36, LD(W(r17) + 8), r37));
    const u128 r26 = LD(W(r80) + 12);
    const u128 r18 = spu_rotqby(r26, spu_ai(r80, 12));
    ST(W(r17) + 12, spu_shufb(r29, LD(W(r17) + 12), r30));
    ST(W(r17) + 0x10, spu_shufb(r22, LD(W(r17) + 0x10), r23));
    ST(W(r17) + 20, spu_shufb(r18, LD(W(r17) + 20), r13));
    ST(W(r81), spu_shufb(spu_ai(r17, 24), LD(W(r81)), r16));
    return 0;
}

extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B700(spu_context*);
extern void jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B9D0(spu_context*);

/* 0xB678: the ring's free space, from the line copy at `line` (word 6 the GPU's position, 0 = none;
 * word 3 the current pointer; words 0 and 2 the ring's start and end). */
static u128 eo_b678(spu_context* ctx, u128 r3)
{
    const u128 r8 = spu_ori(r3, 0);
    const u128 r6 = spu_ai(r8, 24), r7 = spu_ai(r8, 12);
    const u128 r4 = LD(W(r6));
    const u128 r2 = spu_rotqby(r4, r6);
    if (W(r2) == 0) return spu_fsmbi(0x0);
    const u128 r9 = spu_ai(r8, 8);
    const u128 r12 = LD(W(r6)), r11 = LD(W(r7));
    const u128 pos = spu_rotqby(r12, r6);
    const u128 r10 = spu_rotqby(r11, r7);
    if (W(spu_clgt(r10, pos)) != 0) {
        const u128 r14 = spu_rotqby(LD(W(r9)), r9);
        const u128 r15 = spu_rotqby(LD(W(r7)), r7);
        const u128 r9b = spu_rotqby(LD(W(r6)), r6);
        const u128 r13 = spu_rotqby(LD(W(r8)), r8);
        return spu_a(spu_sf(r15, r14), spu_sf(r13, r9b));
    }
    const u128 r8b = spu_rotqby(LD(W(r6)), r6);
    const u128 r20 = spu_rotqby(LD(W(r7)), r7);
    return spu_sf(r20, r8b);
}

/* 0xB700's first pass when it needs no channel at all: the GPU's position known (line word 6 not
 * 0), no refresh asked (c + 0x190 byte 2, bit 7), and the space there. Then the line's current
 * pointer and local words 0-2 (EA, RSX offset, line word 4), and 1; else -1, nothing written, and
 * the caller runs the lifted 0xB700 (its label reads and its wait). */
static int eo_b700(spu_context* ctx, u128 r3, u128 r4, u128 r5, u128 r6, u128 r7, u128 sp)
{
    if ((int32_t)W(spu_xswd(sp)) == -1) return -1;   /* the stack check: the lifted code halts */
    const u128 r19 = spu_ai(r4, 8);
    const u128 r17 = LD(W(r4));
    const u128 r11 = spu_ai(r4, 20);
    const u128 r14 = spu_shlqbyi(r5, 0);
    const u128 r5b = LD(W(r11));
    const u128 r13 = spu_shlqbyi(r4, 0);
    const u128 r9 = spu_a(r14, r14);
    const u128 r4b = LD(W(r19));
    const u128 r12a = spu_ai(r13, 12);
    const u128 r15 = spu_rotqby(r17, r13);
    const u128 r18 = LD(W(r12a));
    const u128 c = r3;
    const u128 r16 = spu_rotqby(r4b, r19);
    const u128 r10a = spu_rotqby(r5b, r11);
    const u128 local = r7;
    const u128 r3b = spu_sf(r15, r16);
    const u128 r8 = spu_rotqby(r18, r12a);
    const u128 r9b = spu_clgt(r9, r3b);
    const u128 r18b = spu_a(r15, r14);
    u128 r12 = r8;
    (void)r10a; (void)r6;
    if (W(r9b) != 0) return -1;                    /* 0xB9BC: more than half the ring */
    const u128 r28 = spu_ai(r13, 24);
    const u128 r27 = LD(W(r28));
    u128 r10 = spu_a(r12, r14);
    const u128 r25 = spu_rotqby(r27, r28);
    const u128 r7b = spu_sfi(spu_ceqi(r25, 0), 0);
    if (((uint16_t)W(spu_xsbh(spu_ceqbi(r7b, 0)))) == 0) return -1;     /* no position: a label read */
    {
        const u128 r36 = LD(W(c) + 0x190);
        const u128 r35 = spu_rotqbyi(r36, 15);
        if (((uint16_t)W(spu_xsbh(spu_cgtbi(r35, -1)))) == 0) return -1;  /* a refresh asked */
    }
    /* 0xB85C: does [cur, cur + size) clear the GPU's position (wrapping to the start when it would
     * run past the end)? */
    const u128 r76 = spu_ai(r13, 24);
    const u128 r78 = spu_clgt(r10, r16);
    const u128 r56 = spu_il(0);
    const u128 r75 = LD(W(r76));
    const u128 r77 = spu_nor(r78, r78);
    const u128 r71 = spu_sfi(r77, 0);
    const u128 r70 = spu_rotqby(r75, r76);
    const u128 r74 = spu_clgt(r70, r18b);
    const u128 r73 = spu_clgt(r70, r10);
    const u128 r72 = spu_sfi(r74, 0);
    const u128 r69 = spu_nor(r73, r73);
    const u128 r67 = spu_or(r71, r72);
    const u128 r68 = spu_clgt(r70, r12);
    const u128 r66 = spu_sfi(r69, 0);
    const u128 r65 = spu_sfi(r68, 0);
    const u128 r64 = spu_ceqbi(r67, 0);
    const u128 r63 = spu_and(r65, r66);
    const u128 r62 = spu_nor(r64, r64);
    const u128 r61 = spu_ceqbi(r63, 0);
    const u128 r59 = spu_fsm(r62);
    const u128 r60 = spu_xsbh(r61);
    const u128 r57 = spu_ceqhi(r60, 0);
    const u128 r58 = spu_sfi(r59, 0);
    const u128 r55 = spu_selb(r58, r56, r57);
    const u128 r53 = spu_xsbh(spu_ceqbi(r55, 0));
    if (((uint16_t)W(r53)) != 0) return -1;       /* 0xB8F4: no room yet, the wait */
    if (W(spu_clgt(r10, r16)) != 0) {             /* past the end: from the start */
        r12 = spu_ori(r15, 0);
        const u128 r5c = LD(W(local) + 0x10);
        r10 = spu_ori(r18b, 0);
        ST(W(local) + 0x10, spu_shufb(spu_il(1), r5c, spu_cbd(sp, 12)));
    }
    /* 0xB94C, the pass's end */
    const u128 r21 = spu_cwd(r13, 12);
    const u128 r24 = spu_ai(r13, 4);
    const u128 r7c = spu_cwd(sp, 0);
    const u128 r17b = spu_sf(r15, r12);
    const u128 r27b = LD(W(r13) + 12);
    const u128 r19b = spu_cwd(sp, 4), r9c = spu_cwd(sp, 8);
    ST(W(r13) + 12, spu_shufb(r10, r27b, r21));
    const u128 r20 = LD(W(local));
    const u128 r16b = spu_shufb(r12, r20, r7c);
    ST(W(local), r16b);
    const u128 r23 = LD(W(r24));
    const u128 r15b = spu_rotqby(r23, r24);
    const u128 r10b = spu_a(r15b, r17b);
    const u128 r18c = spu_shufb(r10b, r16b, r19b);
    ST(W(local), r18c);
    const u128 r12b = LD(W(r13) + 0x10);
    const u128 r8b = spu_rotqby(r12b, r13);
    ST(W(local), spu_shufb(r8b, r18c, r9c));
    return 1;
}

/* 0xC628: the ring allocation (r3 c, r4 the descriptor's ring quadword, r5 the size, r6 local, r7 the
 * SPU). Its r3 (1, or 0 when it could not) is not used by 0x9090: returns nonzero after a halt. `fb`
 * is the caller's frame. */
static int eo_c628(spu_context* ctx, u128 fb, u128 r3, u128 r4, u128 r5, u128 r6, u128 r7)
{
    if (W(spu_andi(r4, 2)) != 0) {                /* an EA of its own, not the ring: lifted */
        ctx->gpr[1] = fb;
        ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6; ctx->gpr[7] = r7;
        EO_COUNT(s_c628_lifted);
        eo_call(ctx, 0x9260, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000C628);
        return 0;
    }
    const u128 r80 = spu_ori(r6, 0), r82 = spu_ori(r4, 0), r83 = spu_ori(r3, 0);
    const u128 reloc = spu_sf(spu_ila(0x8660), spu_link(0xC660));
    const u128 sp = spu_ai(fb, -224);
    if ((int32_t)W(spu_xswd(sp)) == -1) return eo_halt(ctx, 0x0C69C);
    {
        const u128 r10 = LD(W(r80) + 0x10);
        const u128 z = spu_il(0);
        const u128 a = spu_shufb(z, r10, spu_cbd(sp, 13));
        const u128 b = spu_shufb(z, a, spu_cbd(sp, 12));
        ST(W(r80) + 0x10, spu_shufb(z, b, spu_cbd(sp, 14)));
    }
    const u128 r90 = spu_cbd(sp, 14);
    const u128 r85 = spu_andi(spu_ai(r5, 127), -128);
    const u128 r89 = spu_rdch(ctx, SPU_RdMachStat);
    ctx->int_enable = 0;
    const u128 r49 = spu_a(spu_ila(0xCC00), reloc);
    const u128 r47 = spu_a(spu_shli(r7, 7), spu_ai(r82, 128));
    const u128 r42 = spu_sfi(spu_ceqi(spu_andi(r49, 127), 0), 0);
    const u128 r84 = spu_shlqbyi(spu_rotqmbyi(r47, -4), 4);
    const u128 r41 = spu_sfi(spu_ceqi(spu_andi(r84, 127), 0), 0);
    const u128 r86 = spu_and(r42, r41);
    if ((int32_t)W(r86) == 0) return eo_halt(ctx, 0x0C758);
    const u128 r88 = spu_a(spu_ila(0xCC00), reloc);
    spu_wrch(ctx, MFC_LSA, r88);
    spu_wrch(ctx, MFC_EAH, spu_il(0));
    spu_wrch(ctx, MFC_EAL, spu_shlqbyi(r84, 0));
    spu_wrch(ctx, MFC_Size, spu_il(128));
    spu_wrch(ctx, MFC_TagID, spu_fsmbi(0x0));
    spu_wrch(ctx, MFC_Cmd, spu_il(208));                    /* GETLLAR */
    (void)spu_rdch(ctx, MFC_RdAtomicStat);
    {
        const u128 fr = eo_b678(ctx, spu_shlqbyi(r88, 0));
        const u128 r33 = LD(W(r80) + 0x10);
        ST(W(r80) + 0x10, spu_shufb(fr, spu_shufb(fr, r33, spu_cwd(sp, 4)), spu_cwd(sp, 8)));
    }
    u128 r81;
    int r5f = 0;
    const u128 r29 = LD(0x10C00);
    if (W(spu_ceq(spu_rotqbyi(r29, 8), r29)) != 0) {       /* line word 2 == word 0: 0xB9D0 */
        ctx->gpr[1] = sp;
        ctx->gpr[3] = r83; ctx->gpr[4] = r82; ctx->gpr[5] = r85; ctx->gpr[6] = r80;
        eo_call(ctx, 0xC7E8, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B9D0);
        r81 = spu_ori(ctx->gpr[3], 0);
        goto l878;
    }
    {
        const u128 line = spu_a(spu_ila(0xCC00), reloc);
        int ok = eo_b700(ctx, r83, line, r85, spu_fsmbi(0x0), r80, spu_ai(sp, -288));
        if (ok < 0) {
            EO_COUNT(s_b700_lifted);
            ctx->gpr[1] = sp;
            ctx->gpr[3] = spu_shlqbyi(r83, 0); ctx->gpr[5] = spu_ori(r85, 0); ctx->gpr[6] = spu_fsmbi(0x0);
            ctx->gpr[4] = line; ctx->gpr[7] = spu_shlqbyi(r80, 0);
            eo_call(ctx, 0xC80C, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B700);
            r81 = spu_shlqbyi(ctx->gpr[3], 0);
        } else {
            r81 = spu_il(ok);
        }
        if (((uint16_t)W(spu_xsbh(spu_ceqbi(r81, 0)))) != 0) {   /* failed: 0xB9D0, then 0xB700 again */
            ctx->gpr[1] = sp;
            ctx->gpr[4] = spu_ori(r82, 0); ctx->gpr[3] = spu_shlqbyi(r83, 0); ctx->gpr[5] = spu_ori(r85, 0);
            ctx->gpr[6] = spu_shlqbyi(r80, 0);
            eo_call(ctx, 0xC834, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B9D0);
            r81 = spu_ori(ctx->gpr[3], 0);
            if (((uint16_t)W(spu_xsbh(spu_ceqbi(ctx->gpr[3], 0)))) == 0) goto l878;
            ctx->gpr[1] = sp;
            ctx->gpr[3] = spu_ori(r83, 0); ctx->gpr[5] = spu_shlqbyi(r85, 0); ctx->gpr[4] = line;
            ctx->gpr[7] = spu_shlqbyi(r80, 0); ctx->gpr[6] = spu_il(1);
            eo_call(ctx, 0xC860, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000B700);
            r81 = spu_ori(ctx->gpr[3], 0);
            ST(W(r80) + 0x10, spu_shufb(spu_il(1), LD(W(r80) + 0x10), r90));
        }
    }
    {   /* 0xC904: the line back, with the new current pointer */
        const u128 fr = eo_b678(ctx, spu_a(spu_ila(0xCC00), reloc));
        ST(W(r80) + 0x10, spu_shufb(fr, LD(W(r80) + 0x10), spu_cwd(sp, 8)));
        if ((int32_t)W(r86) == 0) return eo_halt(ctx, 0x0C924);
        spu_wrch(ctx, MFC_LSA, spu_ori(r88, 0));
        spu_wrch(ctx, MFC_EAH, spu_il(0));
        spu_wrch(ctx, MFC_EAL, spu_shlqbyi(r84, 0));
        spu_wrch(ctx, MFC_Size, spu_il(128));
        spu_wrch(ctx, MFC_TagID, spu_il(0));
        spu_wrch(ctx, MFC_Cmd, spu_il(176));                /* PUTLLUC */
        (void)spu_rdch(ctx, MFC_RdAtomicStat);
        r5f = 1;
    }
l878:
    if (W(spu_andi(r89, 1)) != 0) ctx->int_enable = 1;
    if (((uint16_t)W(spu_xsbh(spu_ceqbi(r81, 0)))) != 0) return 0;
    if (r5f) {
        const u128 r74 = LD(0x10C10);
        ST(W(r80) + 0x10, spu_shufb(spu_rotqbyi(r74, 4), LD(W(r80) + 0x10), spu_cwd(sp, 0)));
    } else {
        ST(W(r80) + 0x10, spu_shufb(spu_il(0), LD(W(r80) + 0x10), spu_cwd(sp, 0)));
    }
    {
        const u128 r82b = LD(W(r80));
        ST(W(r80), spu_shufb(spu_a(r82b, r85), r82b, spu_cwd(sp, 12)));
    }
    return 0;
}

/* 0xCC98: the hole's end (r3 c, r4 cursor, r5 hole EA, r6 local, r7 entries):
 *  - per entry with a label: SEMAPHORE_OFFSET / TEXTURE_READ_SEMAPHORE_RELEASE (the template at
 *    LS 0xF930, its offset and the ring's new current pointer), merged at the write pointer;
 *  - a method whose arguments would cross a 128-byte line is split there (a header of its own for
 *    the rest), the rest of the hole moved one word on (0xCDBC, 0xCE60);
 *  - a NOP over what is left, to the reserved size;
 *  - between SPU_RdMachStat and interrupts back on, the fenced PUT of the whole hole. */
static void eo_cc98(spu_context* ctx, u128 sp, u128 r3in, u128 r4in, u128 r5in, u128 r6in, u128 r7in)
{
    const u128 r31 = spu_ori(r5in, 0), r30 = spu_ori(r3in, 0), r14 = spu_shlqbyi(r4in, 0);
    if (W(r31) == 0) return;
    u128 r3 = spu_ai(r4in, 8);
    u128 r15 = spu_il(0);
    const u128 r20c = spu_cwd(sp, 4);
    const u128 r18c = spu_il(4080);
    u128 r2 = LD(W(r3));
    const u128 r17m = spu_il(-1);
    const u128 r19c = spu_cwd(sp, 12);
    u128 r8 = spu_rotqby(r2, r3);
    u128 r6 = r6in;
    const u128 r7n = r7in;
    for (;;) {                                     /* 0xCD1C / 0xCCD0 */
        const u128 r29 = spu_andi(r8, 15);
        const u128 r28 = spu_ceq(r15, r7n);
        u128 r9 = LD(0xF930);
        const u128 r11 = spu_sfi(r29, 0);
        const u128 r13 = spu_shlqbyi(r8, 0);
        const u128 r12 = spu_ai(r6, 12);
        const u128 r10 = spu_rotqmby(r17m, r11);
        const u128 r16 = spu_sfi(r29, 16);
        r15 = spu_ai(r15, 1);
        if (W(r28) != 0) break;
        const u128 r21 = LD(W(r6) + 0x10);
        const u128 r4 = spu_rotqby(r21, r6);
        r6 = spu_ai(r6, 32);
        r9 = spu_shufb(spu_and(r4, r18c), r9, r20c);
        if (W(r4) == 0) continue;
        const u128 r27 = LD(W(r12));
        const u128 r22 = LD(W(r8));
        const u128 r24 = LD(W(r8) + 0x10);
        r8 = spu_ai(r8, 16);
        const u128 r26 = spu_rotqby(r27, r12);
        const u128 r25 = spu_shufb(r26, r9, r19c);
        const u128 r12b = spu_rotqmby(r25, r11);
        const u128 r23 = spu_shlqby(r25, r16);
        ST(W(r13), spu_selb(r22, r12b, r10));
        ST(W(r13) + 0x10, spu_selb(r23, r24, r10));
    }
    u128 r7, r21;
    {
        const u128 r16 = spu_ai(r14, 8);
        const u128 r32 = spu_cwd(r14, 8);
        r7 = spu_il(0);
        const u128 r20 = LD(W(r14) + 8);
        ST(W(r14) + 8, spu_shufb(r8, r20, r32));
        const u128 r17 = LD(W(r14));
        const u128 r15b = LD(W(r16));
        const u128 r6s = spu_rotqby(r17, r14);
        const u128 r13 = spu_rotqby(r15b, r16);
        r21 = spu_sf(r6s, r13);
        if (W(r21) != 0) {
            const u128 r17t = LD(0xF980);
            u128 r18 = spu_il(0);
            const u128 r33 = spu_fsmbi(0xFFF);
            const u128 r24 = spu_iohl(spu_ilhu(0xE003), 0xFFFF);
            u128 r9 = spu_shlqbyi(r6s, 0);
            const u128 r7a = spu_andi(r21, -4);
            u128 r22 = spu_shlqbyi(r21, 0);
            const u128 r34 = spu_ila(0x10203);
            u128 r5 = spu_shlqbyi(r18, 0);
            u128 r3c = spu_shlqbyi(r18, 0);
            const u128 r29 = spu_selb(r34, r17t, r33);
            u128 r26 = spu_shlqbyi(r18, 0);
            const u128 r32b = spu_il(8188);
            u128 r25 = spu_shlqbyi(r18, 0);
            u128 r20b = spu_a(r6s, r7a);
            for (;;) {                             /* 0xCDBC */
                const u128 r6b = LD(W(r9));
                const u128 r35 = spu_clgt(r20b, r5);
                r3c = spu_ceq(r18, r3c);
                r18 = spu_andi(r9, -128);
                if (W(r35) == 0) { r7 = spu_ori(r22, 0); break; }
                const u128 r19 = spu_ori(r25, 0);
                const u128 r10 = spu_ori(r5, 0);
                r25 = spu_rotqby(r6b, r9);
                const u128 r36 = spu_ori(r3c, 0);
                const u128 r44 = spu_rotqmbyi(r19, -3);
                const u128 r45 = spu_ai(r10, 128);
                const u128 r43 = spu_ai(r10, 4);
                const u128 r16b = spu_andi(r45, -128);
                const u128 r40 = spu_rotqmbyi(r25, -2);
                r5 = spu_ori(r9, 0);
                const u128 r42 = spu_rotqmbii(r44, -4);
                const u128 r8b = spu_sf(r43, r16b);
                const u128 r39 = spu_rotqmbii(r8b, -2);
                const u128 r41 = spu_andi(r42, 4);
                const u128 r27 = spu_sf(r8b, r26);
                const u128 r38 = spu_xori(r41, 4);
                r26 = spu_and(r32b, r40);
                const u128 r28 = spu_mpyu(r38, r39);
                const u128 r37 = spu_a(r9, r26);
                const u128 r11 = spu_shli(r8b, 16);
                r3c = spu_andi(r37, -128);
                r9 = spu_ai(r37, 4);
                if (W(r36) != 0) continue;
                const u128 r54 = spu_ceqi(r8b, 0);
                const u128 r53 = spu_selb(r11, r19, r24);
                const u128 r50 = LD(W(r10));
                const u128 r52 = spu_sf(r16b, r20b);
                const u128 r51 = spu_cwd(r10, 0);
                const u128 r49 = spu_andc(r53, r54);
                const u128 r48 = spu_andi(r52, -128);
                const u128 r23 = spu_ai(r16b, -128);
                u128 r6c = spu_a(r48, r16b);
                ST(W(r10), spu_shufb(r49, r50, r51));
                u128 r15c;
                for (;;) {                         /* 0xCE60: the rest one word on, a line at a time from the last */
                    const u128 r56 = spu_ori(r6c, 0);
                    r15c = LD(W(r6c));
                    const u128 r61 = LD(W(r6c) + 0x10), r72 = LD(W(r6c) + 0x20), r71 = LD(W(r6c) + 0x30);
                    const u128 r70 = LD(W(r6c) + 0x40), r62 = LD(W(r6c) + 0x50), r63 = LD(W(r6c) + 0x60);
                    const u128 r65 = LD(W(r6c) + 0x70), r64 = LD(W(r6c) - 0x10);
                    r6c = spu_ai(r6c, -128);
                    const u128 r69 = spu_shufb(r61, r72, r17t), r68 = spu_shufb(r72, r71, r17t);
                    const u128 r67 = spu_shufb(r71, r70, r17t);
                    const u128 r55 = spu_ceq(r6c, r23);
                    const u128 r66 = spu_shufb(r70, r62, r17t), r60 = spu_shufb(r63, r65, r17t);
                    const u128 r59 = spu_shufb(r64, r15c, r17t), r58 = spu_shufb(r62, r63, r17t);
                    const u128 r57 = spu_shufb(r15c, r61, r17t);
                    ST(W(r56) + 0x20, r69); ST(W(r56) + 0x30, r68); ST(W(r56) + 0x40, r67); ST(W(r56) + 0x50, r66);
                    ST(W(r56) + 0x70, r60); ST(W(r56), r59); ST(W(r56) + 0x60, r58); ST(W(r56) + 0x10, r57);
                    if (W(r55) != 0) break;
                }
                const u128 r77 = spu_shli(r27, 16);
                r9 = spu_shlqbyi(r16b, 0);
                const u128 r79 = spu_ai(r20b, 4);
                r5 = spu_shlqbyi(r16b, 0);
                const u128 r78 = spu_ai(r22, 4);
                r3c = spu_shlqbyi(r18, 0);
                r20b = spu_ori(r79, 0);
                const u128 r76 = spu_selb(r77, r19, r24);
                r22 = spu_ori(r78, 0);
                const u128 r75 = spu_a(r76, r28);
                ST(W(r16b), spu_shufb(r75, r15c, r29));
            }
        }
    }
    u128 r11, r13;
    {   /* 0xCF10: a NOP over the rest */
        const u128 r20 = spu_ai(r14, 8);
        r13 = spu_cwd(r14, 8);
        const u128 r33 = spu_sf(r21, r7);
        const u128 r34 = LD(W(r30));
        const u128 r32 = LD(W(r20));
        const u128 r10 = spu_andi(r33, -4);
        const u128 r21b = spu_ai(r14, 4);
        const u128 r24 = spu_il(4);
        const u128 r7b = spu_rotqbyi(r34, 14);
        const u128 r8b = spu_rotqby(r32, r20);
        const u128 r18 = spu_ceqbi(r7b, 0);
        u128 r9 = spu_a(r10, r8b);
        const u128 r15b = spu_xsbh(r18);
        const u128 r17 = spu_andi(r7b, 255);
        const u128 r26 = spu_shufb(r9, r32, r13);
        const u128 r29 = spu_ceqhi(r15b, 0);
        const u128 r22 = spu_shli(r17, 4);
        const u128 r5 = spu_fsm(r29);
        ST(W(r14) + 8, r26);
        const u128 r4 = LD(W(r21b));
        r11 = spu_selb(r24, r22, r5);
        const u128 r3e = spu_rotqby(r4, r21b);
        const u128 r2b = spu_rotmai(spu_sf(r9, r3e), -2);
        if (W(r2b) != 0) {
            const u128 r43 = spu_ai(r2b, -1);
            const u128 r28 = LD(W(r10) + W(r8b));
            const u128 r39 = spu_cwx(r10, r8b);
            const u128 r38 = spu_or(spu_shli(r43, 18), spu_ori(spu_ilhu(0x4000), 256));
            const u128 r27 = spu_a(spu_shli(r43, 2), r9);
            ST(W(r10) + W(r8b), spu_shufb(r38, r28, r39));
            r9 = spu_ai(r27, 4);
            ST(W(r14) + 8, spu_shufb(r9, LD(W(r14) + 8), r13));
        }
    }
    /* 0xCFB4: the fenced PUT */
    const u128 ms = spu_rdch(ctx, SPU_RdMachStat);
    ctx->int_enable = 0;
    const u128 r45 = LD(W(r14));
    const u128 r12 = LD(W(r30) + 0x150);
    const u128 r5 = spu_rotqmbyi(r31, -4);
    const u128 r10 = spu_rotqby(r45, r14);
    u128 size, tag;
    if (W(spu_clgti(r11, 15)) == 0) {
        const u128 r67 = spu_ai(r11, -1);
        const u128 r69 = spu_ceqi(r11, 4);
        const u128 r68 = spu_clgti(r67, 1);
        const u128 ea = spu_shlqbyi(r5, 4);
        const u128 r65 = spu_sfi(r69, 0);
        const u128 r66 = spu_nor(r68, r68);
        const u128 r57 = spu_and(r10, r67);
        const u128 r64 = spu_sfi(r66, 0);
        const u128 r59 = spu_ceqi(r11, 8);
        const u128 r63 = spu_or(r64, r65);
        const u128 r60 = spu_andi(r10, 15);
        const u128 r62 = spu_ceqbi(r63, 0);
        const u128 r61 = spu_andi(ea, 15);
        const u128 r58 = spu_xsbh(r62);
        const u128 r56 = spu_clgti(r12, 31);
        const u128 r55 = spu_ceq(r60, r61);
        const u128 r54 = spu_sfi(r59, 0);
        const u128 r53 = spu_ceqhi(r58, 0);
        const u128 r51 = spu_ceqi(r57, 0);
        const u128 r50 = spu_nor(r56, r56);
        const u128 r23 = spu_sfi(r55, 0);
        const u128 r49 = spu_selb(r54, spu_il(1), r53);
        const u128 r6s = spu_sfi(r51, 0);
        const u128 r48 = spu_sfi(r50, 0);
        const u128 r25 = spu_and(spu_and(r23, r49), spu_and(r6s, r48));
        if ((int32_t)W(r25) == 0) { eo_halt(ctx, 0x0D050); return; }
        spu_wrch(ctx, MFC_LSA, r10);
        spu_wrch(ctx, MFC_EAH, spu_il(0));
        spu_wrch(ctx, MFC_EAL, ea);
    } else {
        const u128 ea = spu_shlqbyi(r5, 4);
        const u128 r15a = spu_ceqi(spu_andi(r10, 15), 0);
        const u128 r26 = spu_clgt(r11, spu_il(16384));
        const u128 r29 = spu_ceqi(spu_andi(ea, 15), 0);
        const u128 r22 = spu_clgti(r12, 31);
        const u128 r78 = spu_and(spu_sfi(r15a, 0), spu_sfi(r29, 0));
        const u128 r79 = spu_sfi(spu_nor(r26, r26), 0);
        const u128 r76 = spu_sfi(spu_ceqi(spu_andi(r11, 15), 0), 0);
        const u128 r77 = spu_sfi(spu_nor(r22, r22), 0);
        const u128 r73 = spu_and(spu_and(r78, r79), spu_and(r76, r77));
        if ((int32_t)W(r73) == 0) { eo_halt(ctx, 0x0D0C4); return; }
        spu_wrch(ctx, MFC_LSA, r10);
        spu_wrch(ctx, MFC_EAH, spu_il(0));
        spu_wrch(ctx, MFC_EAL, ea);
    }
    size = spu_ori(r11, 0);
    tag = spu_ori(r12, 0);
    spu_wrch(ctx, MFC_Size, size);
    spu_wrch(ctx, MFC_TagID, tag);
    spu_wrch(ctx, MFC_Cmd, spu_il(34));                     /* PUTF */
    if (W(spu_andi(ms, 1)) != 0) ctx->int_enable = 1;
}

/* The hole's own overflow test in 0x9090: the callback at cursor + 12, called as the lifted code
 * calls it. Nonzero = give up this pass. */
static int eo_cb(spu_context* ctx, u128 cursor, int32_t words, uint32_t link, u128 sp)
{
    const u128 r56 = LD(W(cursor) + 12);
    const u128 r55 = spu_rotqby(r56, spu_ai(cursor, 12));
    ctx->gpr[1] = sp;
    ctx->gpr[3] = spu_shlqbyi(cursor, 0);
    ctx->gpr[4] = spu_il(words);
    eo_calli(ctx, link, W(r55));
    return W(ctx->gpr[3]) != 0;
}

/* 0x9344..0x97CC: the passes (w34's byte 1: high nibble the count, low nibble a mask against the
 * table at 0x10B940), each: viewport, scissor, 0x1EFC, INVALIDATE_VERTEX_FILE, INDEX_ARRAY_ADDRESS,
 * BEGIN_END 5, DRAW_INDEX_ARRAY in 256-index pieces, BEGIN_END 0. */
static void eo_passes(spu_context* ctx, u128 sp, u128 r82, u128 r84, u128 r89)
{
    u128 r27 = LD(W(r84) + 0x40);
    u128 r4 = spu_rotqbyi(r27, 6);
    const u128 r26 = spu_shli(r4, 24);
    const u128 r25 = spu_rotmi(r26, -28);
    if (W(r25) == 0) return;
    const u128 r93 = spu_iohl(spu_ilhu(0x4), 0x1808);
    const u128 r94 = spu_iohl(spu_ilhu(0x7), 0xFF00);
    const u128 r92 = spu_fsmbi(0x0);
    const u128 r91 = spu_ila(0xFFFF);
    u128 r87 = spu_shlqbyi(r84, 0), r88 = spu_fsmbi(0x0);
    u128 r96 = spu_a(spu_ila(0xB940), ctx->gpr[126]);
    u128 r13, r15, r80, r81, r83, r85, r86, r97, r98, r14;
    goto l9360;
l9344: {
        const u128 r72 = spu_shli(r4, 24);
        r88 = spu_ai(r88, 1);
        r96 = spu_ai(r96, 4);
        r87 = spu_ai(r87, 2);
        const u128 r71 = spu_rotmi(r72, -28);
        if (W(spu_cgt(r71, r88)) == 0) return;
    }
l9360: {
        const u128 r34 = spu_andi(r4, 15);
        const u128 r36 = LD(W(r96));
        const u128 r35 = spu_rotqby(r36, r96);
        if (W(spu_and(r34, r35)) == 0) goto l9344;
        const u128 r48 = spu_csflt(r88, 155);
        const u128 r58 = spu_cwd(sp, 0), r54 = spu_cwd(sp, 4), r52 = spu_cwd(sp, 8), r50 = spu_cwd(sp, 12);
        const u128 r57 = spu_il(0);
        r86 = spu_il(68);
        r81 = spu_ai(r87, 82);
        const u128 r60 = LD(W(r84) + 0x40);
        const u128 r56 = LD(W(r87) + 68);
        const u128 r53 = spu_shufb(r48, r57, r58);
        r83 = spu_il(8);
        r85 = spu_il(4);
        const u128 r7 = spu_and(r91, r60);
        const u128 r59 = spu_rotqbyi(r60, 14);
        r97 = spu_ai(r82, 8);
        const u128 r51 = spu_shufb(r48, r53, r54);
        r98 = spu_ai(r82, 4);
        const u128 r55 = spu_rotqby(r56, r81);
        const u128 r6 = spu_and(r91, r59);
        const u128 r49 = spu_shufb(r48, r51, r52);
        const u128 r5 = spu_and(r91, r55);
        const u128 r47 = spu_shufb(r48, r49, r50);
        ST(W(sp) + 0x50, r47);
        eo_b3d0(ctx, sp, r82, spu_il(0), r5, r6, r7, spu_il(0), spu_ilhu(0x3F80));
        const u128 r45 = LD(W(r87) + 68);
        const u128 r43 = LD(W(r84) + 0x40);
        const u128 r44 = spu_rotqby(r45, r81);
        const u128 r7b = spu_and(r91, r43);
        const u128 r42 = spu_rotqbyi(r43, 14);
        const u128 r5b = spu_and(r91, r44);
        const u128 r6b = spu_and(r91, r42);
        eo_b2d0(ctx, sp, r82, spu_il(0), r5b, r6b, r7b);
        eo_b1a8(ctx, sp, r82, spu_fsmbi(0x0), spu_ai(sp, 80));
        const u128 r41 = LD(W(r82) + 8);
        const u128 r40 = LD(W(r82) + 4);
        const u128 r39 = LD(W(sp) + 0x60);
        r13 = spu_rotqby(r41, r97);
        r15 = spu_rotqby(r40, r98);
        r80 = spu_rotqbyi(r39, 4);
        if (W(spu_clgt(spu_ai(r13, 28), r15)) != 0) {
            if (eo_cb(ctx, r82, 7, 0x98B8, sp)) goto l987C;
            r13 = spu_rotqby(LD(W(r82) + 8), r97);
            r15 = spu_rotqby(LD(W(r82) + 4), r98);
        }
    }
    {   /* 0x9458 */
        const u128 r38 = spu_iohl(spu_ilhu(0x4), 0x1714);
        const u128 r37 = LD(W(r13));
        const u128 r9 = spu_ai(r13, 8);
        const u128 r39 = spu_cwd(r13, 0), r32 = spu_cwd(r13, 4);
        const u128 r20 = spu_iohl(spu_ilhu(0x8), 0x181C);
        r86 = spu_cwd(r82, 8);
        const u128 r36 = spu_iohl(spu_ilhu(0x1FFF), 0xFFFF);
        const u128 r21 = spu_cwd(r13, 8);
        const u128 r11 = spu_cwd(r9, 4);
        const u128 r10 = spu_il(17);
        const u128 r12 = spu_cwd(r9, 8);
        const u128 r17 = spu_and(r80, r36);
        r14 = spu_shlqbyi(r13, 0);
        const u128 r71 = spu_ai(r13, 20);
        const u128 r74 = spu_il(5);
        const u128 r75 = spu_cwd(r71, 4);
        ST(W(r13), spu_shufb(r38, r37, r39));
        const u128 r34 = spu_ai(r17, 127);
        const u128 r31 = LD(W(r13) + 4);
        const u128 r33 = spu_andi(r34, -128);
        const u128 r30 = spu_sf(r17, r33);
        r81 = spu_rotmi(r30, -1);
        const u128 r28 = spu_ceqi(r81, 0);
        ST(W(r13) + 4, spu_shufb(r92, r31, r32));
        const u128 r27b = spu_clgt(r89, r81);
        const u128 r26b = LD(W(r82) + 8);
        const u128 r4b = spu_nor(r28, r28);
        const u128 r25b = spu_sfi(r27b, 0);
        const u128 r24 = spu_sfi(r4b, 0);
        const u128 r22 = spu_and(r24, r25b);
        const u128 r23 = spu_shufb(r9, r26b, r86);
        const u128 r68 = spu_xsbh(spu_ceqbi(r22, 0));
        ST(W(r82) + 8, r23);
        ST(W(r13) + 8, spu_shufb(r20, LD(W(r13) + 8), r21));
        ST(W(r9) + 4, spu_shufb(r17, LD(W(r9) + 4), r11));
        ST(W(r9) + 8, spu_shufb(r10, LD(W(r9) + 8), r12));
        ST(W(r82) + 8, spu_shufb(r71, LD(W(r82) + 8), r86));
        ST(W(r13) + 20, spu_shufb(r93, LD(W(r13) + 20), r32));
        r13 = spu_ai(r13, 28);
        ST(W(r71) + 4, spu_shufb(r74, LD(W(r71) + 4), r75));
        ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), r86));
        if (((uint16_t)W(r68)) != 0) {      /* 0x9888 */
            r80 = spu_ori(r89, 0);
            r81 = spu_fsmbi(0x0);
            goto l95B8;
        }
        r80 = spu_ai(r14, 36);
        if (W(spu_clgt(r80, r15)) != 0) {
            if (eo_cb(ctx, r82, 2, 0x9978, sp)) goto l987C;
            r13 = spu_rotqby(LD(W(r82) + 8), r97);
            r15 = spu_rotqby(LD(W(r82) + 4), r98);
        }
    }
    {   /* 0x9570: the indexes up to the first 128-byte boundary of the index data */
        const u128 r51 = spu_iohl(spu_ilhu(0x4), 0x1824);
        const u128 r50 = LD(W(r13));
        const u128 r53 = spu_ai(r81, -1);
        const u128 r52 = spu_cwd(r13, 0), r48 = spu_cwd(r13, 4);
        const u128 r47 = spu_shli(r53, 24);
        r80 = spu_sf(r81, r89);
        ST(W(r13), spu_shufb(r51, r50, r52));
        ST(W(r13) + 4, spu_shufb(r47, LD(W(r13) + 4), r48));
        r13 = spu_ai(r13, 8);
        ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), r86));
    }
l95B8:
    while (W(spu_clgt(r80, r94)) != 0) {      /* 0x95C0: 2047 pieces of 256 under one header */
        const u128 r86b = spu_a(r13, spu_il(8192));
        if (W(spu_clgt(r86b, r15)) != 0) {
            if (eo_cb(ctx, r82, 2048, 0x9850, sp)) goto l987C;
            r13 = spu_rotqby(LD(W(r82) + 8), spu_ai(r82, 8));
            r15 = spu_rotqby(LD(W(r82) + 4), spu_ai(r82, 4));
        }
        const u128 r72 = spu_iohl(spu_ilhu(0x5FFC), 0x1824);
        const u128 r68 = spu_cwd(r82, 8);
        u128 r8 = spu_ori(r81, 0);
        const u128 r70 = spu_ori(spu_ilhu(0xFFF8), 256);
        u128 r5 = spu_il(2047);
        ST(W(r13), spu_shufb(r72, LD(W(r13)), spu_cwd(r13, 0)));
        r80 = spu_a(r80, r70);
        const u128 r9 = spu_ilhu(0xFF00);
        r13 = spu_ai(r13, 4);
        ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), r68));
        do {
            const u128 r77 = spu_or(r8, r9);
            r5 = spu_ai(r5, -1);
            r8 = spu_ai(r8, 256);
            ST(W(r13), spu_shufb(r77, LD(W(r13)), spu_cwd(r13, 0)));
            r13 = spu_ai(r13, 4);
        } while (W(r5) != 0);
        r81 = spu_a(r81, r94);
        ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), spu_cwd(r82, 8)));
    }
    {   /* 0x9678: the rest, ceil(rest / 256) pieces under one header */
        const u128 r17 = spu_ai(r80, 255);
        r83 = spu_rotmi(r17, -8);
        const u128 r16 = spu_shli(r83, 2);
        const u128 r12 = spu_ai(spu_a(r16, r13), 4);
        if (W(spu_clgt(r12, r15)) != 0) {
            if (eo_cb(ctx, r82, (int32_t)W(spu_ai(r83, 1)), 0x98F0, sp)) goto l987C;
            r13 = spu_rotqby(LD(W(r82) + 8), spu_ai(r82, 8));
            r15 = spu_rotqby(LD(W(r82) + 4), spu_ai(r82, 4));
        }
        const u128 r34 = spu_shli(r83, 18);
        const u128 r35 = spu_iohl(spu_ilhu(0x4000), 0x1824);
        const u128 r29 = spu_cwd(r82, 8);
        const u128 r26 = spu_clgti(r80, 256);
        ST(W(r13), spu_shufb(spu_or(r34, r35), LD(W(r13)), spu_cwd(r13, 0)));
        r13 = spu_ai(r13, 4);
        ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), r29));
        if (W(r26) != 0) {
            const u128 r6 = spu_ilhu(0xFF00);
            u128 r36;
            do {
                const u128 r39 = spu_or(r81, r6);
                r80 = spu_ai(r80, -256);
                r81 = spu_ai(r81, 256);
                r36 = spu_clgti(r80, 256);
                ST(W(r13), spu_shufb(r39, LD(W(r13)), spu_cwd(r13, 0)));
                r13 = spu_ai(r13, 4);
            } while (W(r36) != 0);
            ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), spu_cwd(r82, 8)));
        }
        if (W(r80) != 0) {                    /* 0x971C */
            const u128 r48 = spu_ai(r80, -1);
            const u128 r47 = spu_shli(r48, 24);
            ST(W(r13), spu_shufb(spu_or(r81, r47), LD(W(r13)), spu_cwd(r13, 0)));
            r13 = spu_ai(r13, 4);
            ST(W(r82) + 8, spu_shufb(r13, LD(W(r82) + 8), spu_cwd(r82, 8)));
        }
        if (W(spu_clgt(spu_ai(r13, 8), r15)) != 0) {     /* 0x9758 */
            if (eo_cb(ctx, r82, 2, 0x9940, sp)) goto l987C;
            r13 = spu_rotqby(LD(W(r82) + 8), spu_ai(r82, 8));
        }
        /* 0x9768: BEGIN_END 0, then the next pass */
        const u128 r68 = spu_cwd(r13, 0), r65 = spu_cwd(r13, 4);
        const u128 r59 = spu_ai(r13, 8);
        r88 = spu_ai(r88, 1);
        r96 = spu_ai(r96, 4);
        const u128 r60 = spu_cwd(r82, 8);
        r87 = spu_ai(r87, 2);
        ST(W(r13), spu_shufb(r93, LD(W(r13)), r68));
        ST(W(r13) + 4, spu_shufb(r92, LD(W(r13) + 4), r65));
        const u128 r61 = LD(W(r84) + 0x40);
        r4 = spu_rotqbyi(r61, 6);
        ST(W(r82) + 8, spu_shufb(r59, LD(W(r82) + 8), r60));
        const u128 r71 = spu_rotmi(spu_shli(r4, 24), -28);
        if (W(spu_cgt(r71, r88)) != 0) goto l9360;
        return;
    }
l987C:
    r27 = LD(W(r84) + 0x40);
    r4 = spu_rotqbyi(r27, 6);
    goto l9344;
}

int gowa_edge_out_9080(spu_context* ctx)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_EDGE_NATIVE");
        s_stats = getenv("GOWA_EDGE_NATIVE_STATS") != NULL;
        on = !e || (atoi(e) & 131072) != 0;
        if (on && s_stats) atexit(eo_stats);
    }
    if (!on) return 0;
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
        EO_COUNT(s_declined_irq);
        return 0;
    }
    const u128 c = ctx->gpr[3], desc = ctx->gpr[4], r5 = ctx->gpr[5], st = ctx->gpr[6], mpiece = ctx->gpr[8];
    const u128 r0e = ctx->gpr[0], sp0 = ctx->gpr[1], r126e = ctx->gpr[126];
    if (W(c) != 0x107A0 || W(desc) != 0x10650) {
        EO_COUNT(s_declined_args);
        return 0;
    }
    if (W(spu_rotqbyi(LD(W(c) + 0x80), 8)) != 0) {           /* a stride callback (0xD118) */
        EO_COUNT(s_declined_cb);
        return 0;
    }
    const long long n = s_stats ? __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED) : 1;
    /* 0x9080 / 0x9090's frame, as the lifted code writes it */
    ST(W(sp0) - 0x140, r126e);
    for (int r = 80; r <= 98; r++) ST(W(sp0) - 0x10 * (r - 79), ctx->gpr[r]);
    ST(W(sp0) + 0x10, r0e);
    ST(W(sp0) - 0x1D0, sp0);
    const u128 sp = spu_ai(sp0, -464);
    ctx->gpr[1] = sp;
    ctx->gpr[126] = spu_sf(spu_ila(0x5090), spu_link(0x9090));
    const u128 r5h = spu_xsbh(spu_ceqbi(r5, 0));
    int draw = 0;
    if (((uint16_t)W(r5h)) != 0) {
        const u128 r9 = LD(W(desc));
        if (W(spu_and(spu_rotqbyi(r9, 12), spu_il(2048))) != 0) draw = 1;
        else if (((uint16_t)W(spu_rotqbyi(LD(W(c) + 0x190), 14))) != 0) draw = 1;
    }
    if (!draw) {                                             /* 0x9128 */
        EO_COUNT(s_empty);
        const u128 r80 = spu_ai(sp, 48);
        const u128 r13 = LD(W(desc));
        ctx->gpr[3] = c;
        ctx->gpr[4] = spu_shlqbyi(r80, 0);
        ctx->gpr[5] = spu_rotqbyi(r13, 4);
        ctx->gpr[6] = spu_fsmbi(0x0);
        ctx->gpr[7] = spu_il(0);
        eo_call(ctx, 0x9158, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000C9A8);
        ctx->gpr[1] = sp;
        ctx->gpr[3] = spu_ori(st, 0);
        ctx->gpr[4] = spu_shlqbyi(r80, 0);
        eo_call(ctx, 0x9164, jobmod_e8f44fbc6dbf_e6D98_spu_func_000084C0);
        ctx->gpr[1] = sp;
        const u128 r12 = LD(W(desc));
        eo_cc98(ctx, sp, spu_shlqbyi(c, 0), spu_shlqbyi(r80, 0), spu_rotqbyi(r12, 4), spu_il(0), spu_il(0));
        goto done;
    }
    EO_COUNT(s_draws);
    {
        const u128 spu = spu_rotqbyi(LD(0x1C0), 8);                          /* 0xF770 */
        const u128 r16 = LD(W(desc));
        u128 icount;
        if (W(spu_and(spu_rotqbyi(r16, 12), spu_il(2048))) != 0) {         /* 0x97F8 (0xB630 returns) */
            const u128 r22 = LD(W(c) + 10);
            icount = spu_and(spu_rotqby(r22, spu_ai(c, 24)), spu_ila(0xFFFF));
        } else {
            icount = spu_and(spu_rotqbyi(LD(W(c) + 0x190), 14), spu_ila(0xFFFF));
        }
        /* 0xBB38: the allocation's size */
        u128 size;
        {
            const u128 stride = eo_d118(ctx, c);
            const u128 r10 = spu_andi(spu_ai(icount, 7), -8);
            const u128 r80 = spu_ai(spu_a(r10, r10), 127);
            const u128 r81 = spu_rotqbyi(LD(W(c)), 6);
            const u128 r5b = spu_a(spu_mpyh(stride, r81), spu_mpyu(r81, stride));
            size = spu_andi(spu_a(spu_andi(spu_ai(r5b, 15), -16), r80), -128);
        }
        const u128 local = spu_ai(sp, 112);
        if (eo_c628(ctx, sp, spu_shlqbyi(c, 0), LD(W(desc)), spu_ori(size, 0), spu_shlqbyi(local, 0),
                    spu_shlqbyi(spu, 0)))                                /* the ring allocation */
            goto done;
        ctx->gpr[1] = sp;
        if (eo_put(ctx, 0, c, icount, local, spu_ai(sp, 96), spu_ai(sp, -144))) goto done;
        eo_d158(ctx, c, spu_ai(sp, -80));
        if (eo_put(ctx, 1, c, icount, local, spu_ai(sp, 32), spu_ai(sp, -176))) goto done;
        const u128 cursor = spu_ai(sp, 64);
        const u128 r32 = LD(W(desc));
        if (eo_c9a8(ctx, c, cursor, spu_rotqbyi(r32, 4), local, 1, spu_ai(sp, -144))) {
            EO_COUNT(s_c9a8_lifted);
            ctx->gpr[1] = sp;
            ctx->gpr[3] = spu_shlqbyi(c, 0);
            ctx->gpr[4] = spu_ori(cursor, 0);
            ctx->gpr[5] = spu_rotqbyi(r32, 4);
            ctx->gpr[6] = spu_shlqbyi(local, 0);
            ctx->gpr[7] = spu_il(1);
            eo_call(ctx, 0x92C0, jobmod_e8f44fbc6dbf_e6D98_spu_func_0000C9A8);
        }
        {   /* 0x5D68: the method piece */
            const u128 r31 = LD(W(desc) + 0x40);
            const u128 r29 = LD(W(sp) + 0x20);
            ctx->gpr[1] = sp;
            ctx->gpr[8] = spu_ori(mpiece, 0);
            ctx->gpr[5] = spu_ori(cursor, 0);
            ctx->gpr[3] = spu_ori(c, 0);
            ctx->gpr[6] = spu_rotqbyi(r29, 8);
            ctx->gpr[7] = spu_rotqbyi(r29, 4);
            ctx->gpr[4] = spu_andi(spu_rotqbyi(r31, 9), 255);
            eo_call(ctx, 0x92F0, jobmod_e8f44fbc6dbf_e6D98_spu_func_00005D68);
        }
        {   /* 0x84C0: the program binding, when there is one */
            const u128 w = spu_rotqby(LD(W(st) + 24), spu_ai(st, 24));
            if (W(w) != 0) {
                EO_COUNT(s_program);
                ctx->gpr[1] = sp;
                ctx->gpr[4] = spu_ori(cursor, 0);
                ctx->gpr[3] = spu_shlqbyi(st, 0);
                eo_call(ctx, 0x92FC, jobmod_e8f44fbc6dbf_e6D98_spu_func_000084C0);
            }
        }
        eo_passes(ctx, sp, cursor, desc, icount);
        {   /* 0xCC98: the hole's end and its fenced PUT */
            const u128 r87 = LD(W(desc));
            eo_cc98(ctx, sp, spu_shlqbyi(c, 0), spu_shlqbyi(cursor, 0), spu_rotqbyi(r87, 4),
                    spu_ori(local, 0), spu_il(1));
        }
    }
done:
    ctx->gpr[3] = spu_il(0);
    ctx->gpr[1] = sp0;
    ctx->gpr[0] = r0e;
    ctx->gpr[126] = r126e;
    if (s_stats && !(n & 0xFFFF)) eo_stats();
    return 1;
}

static void eo_stats(void)
{
    fprintf(stderr, "[edge-native] output 9080: %lld calls (%lld draws, %lld empty), declined %lld (interrupt) %lld (stride callback) %lld (arguments), "
            "hole set-up lifted %lld, allocation lifted %lld, room check lifted %lld, with a program %lld, halts %lld\n",
            s_calls, s_draws, s_empty, s_declined_irq, s_declined_cb, s_declined_args, s_c9a8_lifted, s_c628_lifted,
            s_b700_lifted, s_program, s_halts);
}
