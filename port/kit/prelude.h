/*
 * Ascension-R -- put ahead of every lifted PPU source when the Kit builds the game's DLL (gowa.py module)
 *
 * ppu_recomp.h spells a guest barrier with C++'s <atomic>, which the Kit does not carry. These are the same fences as
 * Visual C++'s library makes them on x86-64: nothing for relaxed, a compiler barrier for the others, and for seq_cst a
 * locked increment of a local between two barriers.
 */
#pragma once

#define PPU_FENCE(o) PPU_FENCE_##o()
#define PPU_FENCE_relaxed() ((void)0)
#define PPU_FENCE_consume() _ReadWriteBarrier()
#define PPU_FENCE_acquire() _ReadWriteBarrier()
#define PPU_FENCE_release() _ReadWriteBarrier()
#define PPU_FENCE_acq_rel() _ReadWriteBarrier()
#define PPU_FENCE_seq_cst()                                                                                                   \
    do {                                                                                                                      \
        _ReadWriteBarrier();                                                                                                  \
        volatile long _ppu_fence_guard;                                                                                       \
        (void)_InterlockedIncrement(&_ppu_fence_guard);                                                                       \
        _ReadWriteBarrier();                                                                                                  \
    } while (0)
