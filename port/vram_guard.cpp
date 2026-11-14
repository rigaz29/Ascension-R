/*
 * Ascension-R -- the game's VRAM manager kept from wrecking what it holds (scripts/ppu_hooks.json)
 *
 * Issue #9: at the Delphi temple, on one PC and in every session, the game printed "invalid size" and then 482
 * "CopyLocalMemory ... offset error" lines, the textures broke, and a session froze. Its VRAM manager keeps a stack of
 * blocks growing down from the top of its pool (func_0039E3F8), which takes the size it is given: a negative one
 * moves the top up instead of down, and the blocks after it overlap others or run past local memory. A texture's size
 * is [texture + 0x20] - [texture + 0x1C], the end of its data less its start (func_0039B93C). The defragmenter
 * (func_0039F06C) moves a block with the size it has, through MoveLocalMemory (func_003A2ADC) and CopyLocalMemory
 * (func_003A2690), 16 MB at a time: with a negative one, over the blocks after it and then, past local memory, over
 * main memory. The game's own checks print "invalid size" and stop on a trap, which on a PS3 would have ended the game
 * and which the lift leaves out.
 *
 * What gives a block such a size: the streaming pass trims the oldest texture's block (func_0039EA58), cutting the
 * mips above the ones it keeps off the block's front, and it takes a texture loaded with only its smaller mips too, for
 * which the cut is larger than the block (issues #7 and #9, found with #7's save in HRTN). gowa_vram_cut makes such a
 * cut nothing. The others stay for any other way: they say it in the log, with what a texture held, and keep the
 * damage from spreading: such a texture or allocation gets an empty block, and a move past local memory does not
 * happen. A size made negative on purpose (one replay of the temple) printed one line and nothing else.
 *
 * With 0.1.0-alpha.7 the same came the other way in: the texture mover (func_003A0000), which grows a streamed texture
 * into a larger block, took a size of -15 MB from a range that ends before it starts, and the bytes the frame had
 * moved went down by as much, past its budget. Its size is checked now too, and the run compactor's said
 * (docs/notes/2026-11-12-temple-textures.md).
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint32_t GCM_STATE = 0x00A23500;   /* the game's: +0x8 the size of local memory its checks use */

static int is_there(uint32_t addr, uint32_t bytes)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(vm_base + addr, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return (uint8_t*)mi.BaseAddress + mi.RegionSize >= vm_base + (uint64_t)addr + bytes;
}

/* func_0039B93C at its first branch: r31 the texture, r3 its data's size as the function took it. */
extern "C" void gowa_vram_texture(ppu_context* ctx, uint32_t)
{
    const uint32_t size = (uint32_t)ctx->gpr[3];
    if (size < 0x10000000u) return;   /* within local memory's 256 MB; past it, or negative */
    static volatile LONG said;
    const uint32_t tex = (uint32_t)ctx->gpr[31];
    if (InterlockedIncrement(&said) <= 8) {
        fprintf(stderr, "[vram] texture 0x%08X: its data ends before it starts or is past local memory, size 0x%08X, "
                        "from 0x%08X: given an empty block\n", tex, size, (uint32_t)ctx->lr);
        if (is_there(tex, 0x50)) {
            fprintf(stderr, "[vram]   0x%08X:", tex);
            for (uint32_t k = 0; k < 0x50; k += 4) fprintf(stderr, "%s %08X", k && !(k & 0xF) ? " |" : "", vm_read32(tex + k));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    ctx->gpr[3] = 0;
}

/* The pool's allocator (func_0039E3F8) on entry: r4 the size, from a texture (func_0039B93C), from func_0039F7C8 at
 * [[r30] + 0x20], or from the block the defragmenter moves. Past local memory or negative, it is made 0: a negative
 * one would move the pool's top up. */
extern "C" void gowa_vram_alloc(ppu_context* ctx, uint32_t)
{
    const uint32_t size = (uint32_t)ctx->gpr[4];
    if (size < 0x10000000u) return;
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 8) {
        fprintf(stderr, "[vram] an allocation of 0x%08X bytes, from 0x%08X: made 0\n", size, (uint32_t)ctx->lr);
        fflush(stderr);
    }
    ctx->gpr[4] = 0;
}

/* func_003A0000 (and func_0039FFFC, the same code a word earlier) moves a streamed texture's data into the larger block
 * it grows into: the texture at the head of [0x9B26AC] (r28), its range [+0x1C, +0x20), out of its block (r30) into
 * [0x9B1DEC] (r31). Here r6 is the range's size, about to be added to the bytes the frame has moved ([0x9AF4BC]) and
 * queued, or moved at once. Nothing checked it: a range that ends before it starts took a negative size into the
 * frame's 16 MB budget, after which the defragmenter moved without a limit (issue #9, 0.1.0-alpha.7: four moves of
 * -15 to -105 MB, then hundreds past local memory). Past local memory or negative, it is made 0, and said with the
 * texture and both blocks. GOWA_VRAM_STREAM_GUARD=0 leaves it as the game has it, to try the guard. */
extern "C" void gowa_vram_stream(ppu_context* ctx, uint32_t)
{
    const uint32_t size = (uint32_t)ctx->gpr[6];
    if (size < 0x10000000u) return;
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("GOWA_VRAM_STREAM_GUARD");
        on = !(e && e[0] == '0');
    }
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 8) {
        const uint32_t tex = (uint32_t)ctx->gpr[28], from = (uint32_t)ctx->gpr[30], into = (uint32_t)ctx->gpr[31];
        fprintf(stderr, "[vram] streamed texture 0x%08X: its range 0x%08X..0x%08X ends before it starts or is past local "
                        "memory, size 0x%08X, from 0x%08X: %s\n", tex, (uint32_t)ctx->gpr[26],
                (uint32_t)ctx->gpr[26] + size, size, (uint32_t)ctx->lr, on ? "moved as 0 bytes" : "left as it is");
        for (int k = 0; k < 2; k++) {
            const uint32_t b = k ? into : from;
            if (is_there(b, 0x20))
                fprintf(stderr, "[vram]   %s block 0x%08X: offset 0x%08X, size 0x%08X\n", k ? "into" : "from", b,
                        vm_read32(b + 0x8), vm_read32(b + 0xC));
        }
        if (is_there(tex, 0x50)) {
            fprintf(stderr, "[vram]   0x%08X:", tex);
            for (uint32_t k = 0; k < 0x50; k += 4) fprintf(stderr, "%s %08X", k && !(k & 0xF) ? " |" : "", vm_read32(tex + k));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    if (on) ctx->gpr[6] = 0;
}

/* func_0039F3C4 packs a run of blocks up against the one above: r3 the run's size, the sum of its blocks' sizes, r29
 * its first block's index and r30 the one past its last, in the table at 0x9AF51C. A block's size is checked when it
 * is allocated (gowa_vram_alloc), so a bad sum is a block whose size went bad after: said with the run's blocks, and
 * left as it is (the moves that follow are guarded, and the blocks' new places come from it). */
extern "C" void gowa_vram_runs(ppu_context* ctx, uint32_t)
{
    const uint32_t size = (uint32_t)ctx->gpr[3];
    if (size < 0x10000000u) return;
    static volatile LONG said;
    if (InterlockedIncrement(&said) > 4) return;
    const uint32_t first = (uint32_t)ctx->gpr[29], end = (uint32_t)ctx->gpr[30];
    fprintf(stderr, "[vram] a run of blocks %u..%u, size 0x%08X, from 0x%08X: a block's size went bad after it was "
                    "allocated\n", first, end, size, (uint32_t)ctx->lr);
    for (uint32_t i = first; i < end && i < first + 8 && i < 0xA28; i++) {
        const uint32_t b = vm_read32(0x009AF51C + 4 * i);
        if (b && is_there(b, 0x20))
            fprintf(stderr, "[vram]   block %u 0x%08X: offset 0x%08X, size 0x%08X, +0x18 0x%08X, +0x1C 0x%08X\n", i, b,
                    vm_read32(b + 0x8), vm_read32(b + 0xC), vm_read32(b + 0x18), vm_read32(b + 0x1C));
    }
    fflush(stderr);
}

/* func_0039EA58 trims a texture's block: it cuts [texture + 0x1C] bytes, the mips above the ones the texture keeps,
 * off the block's front, and moves its offset and RSX address up by as much. That holds for a block with the whole
 * chain, [0, [texture + 0x20]). The streaming pass (around 0x003A0D88) also takes, once it is the oldest, a texture
 * loaded with only its smaller mips, whose block is [+0x20] - [+0x1C] bytes: the cut is larger than the block, its
 * size goes negative and its offset past its data, and the moves after it wreck local memory (#9; #7: one 1024x1024
 * texture at the Sparta cinematic's level, cut by 640 KB from a 43 KB block every frame).
 * At 0x0039EB1C r29 is the cut, r4 the block's size, r30 the block, r31 the texture. A cut larger than the block is
 * made what the block holds in front of the kept mips: nothing, for such a block. The trim then changes nothing in
 * place, and its other work stays as the game has it. So that the pass takes another texture next, this one's LRU
 * stamp (+8 of its block's state entry, in the table at [0x9B2298]) is moved on by the 240 frames the pass waits
 * before a texture counts as old; a pass that takes it every frame finds nothing to free and stays short of memory. */
extern "C" void gowa_vram_cut(ppu_context* ctx, uint32_t)
{
    const uint32_t cut = (uint32_t)ctx->gpr[29], size = (uint32_t)ctx->gpr[4];
    if (cut <= size) return;
    const uint32_t tex = (uint32_t)ctx->gpr[31], blk = (uint32_t)ctx->gpr[30];
    const uint32_t end = is_there(tex, 0x48) ? vm_read32(tex + 0x20) : 0;
    const uint32_t lead = end >= size ? end - size : 0;   /* where in the chain the block begins */
    const uint32_t fixed = cut >= lead && cut - lead <= size ? cut - lead : 0;
    ctx->gpr[29] = fixed;
    uint32_t stamp = 0;
    if (is_there(0x009B2298, 8)) {
        const int32_t d = (int32_t)(blk - vm_read32(0x009B229C));
        const uint32_t st = vm_read32(0x009B2298) + (uint32_t)((d / 128) * 16);   /* srawi 7 + addze: toward zero */
        if (is_there(st, 16)) {
            stamp = vm_read32(st + 8);
            if (stamp) vm_write32(st + 8, stamp + 240);
        }
    }
    static volatile LONG said;
    const LONG n = InterlockedIncrement(&said);
    if (n <= 8 || !(n & (n - 1))) {
        fprintf(stderr, "[vram] texture 0x%08X: a trim of 0x%X bytes from a block of 0x%X that holds only its mips from "
                        "0x%X made 0x%X; its stamp 0x%X put 240 frames on (%ld so far)\n", tex, cut, size, lead, fixed,
                stamp, (long)n);
        fflush(stderr);
    }
}

/* MoveLocalMemory (func_003A2ADC) on entry: r3 the destination, r4 the source, r5 the size, as offsets in local
 * memory. A move either side of which runs past local memory is made a move of nothing. */
extern "C" void gowa_vram_move(ppu_context* ctx, uint32_t)
{
    const uint64_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    const uint64_t local = vm_read32(GCM_STATE + 0x8);
    if (size < 0x20000000u && src + size <= local && dst + size <= local) return;
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 8) {
        fprintf(stderr, "[vram] a move of 0x%08llX bytes from 0x%08llX to 0x%08llX runs past local memory (0x%llX), "
                        "from 0x%08X: left out\n", (unsigned long long)size, (unsigned long long)src,
                (unsigned long long)dst, (unsigned long long)local, (uint32_t)ctx->lr);
        fflush(stderr);
    }
    ctx->gpr[5] = 0;
}
