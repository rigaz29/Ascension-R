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
 * What gives a block such a size there is not known yet. These say it in the log, with what a texture held, and keep
 * the damage from spreading: such a texture or allocation gets an empty block, and a move past local memory does not
 * happen. A size made negative on purpose (one replay of the temple) printed one line and nothing else.
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
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
