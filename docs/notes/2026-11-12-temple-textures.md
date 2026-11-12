# Issue #9: the Delphi temple's broken textures, and speckled skinned meshes (2026-11-12)

Issue #9: an RX 7900 XT, the 0.1.0-alpha.6 Kit, Internal resolution 1.5 with FSR 3.1. Loaded at the Delphi temple, every
texture came out striped or noisy, the HUD's text too, every time, and the second session froze.

## What the reports show

- Right after the temple's level loads (TEMBAS, TEM300, TEM350, TEM700), the game prints errors of its own: `invalid size`,
  then thousands of `CopyLocalMemory` and `MoveLocalMemory ... offset error`, with offsets past the 256 MB of local memory
  (`0xF153680` + `0xF46D00` = `0x1009A380`). Its video memory manager's bookkeeping is damaged, and what it moves comes out
  wrong.
- In the second session the same damage then sent the game through NULL function pointers (`bctr to NULL`, `null-call`),
  an SPU program halted on an assert, and the frames stopped.
- It is not the GPU's. The Grotto run `memo-1` of [issue #7](2026-11-12-grotto-freeze.md), on a GTX 1650 Ti and before
  that issue's fix, printed the same errors, and its screenshot has the same stripes. There the damage came from an EDGE job.
- In both sessions the first error comes 36 and 84 lines after the first `[vtx-kept] resident part full: started over`:
  the temple filled the resident part's indexes, 59 of their 64 MB, with 160 of the 256 MB of vertexes.

## Tried here

The Grotto, the #7 reporter's saves and pad recording, one run of each, 150 s:

| Run | The game's errors | The picture |
|---|---|---|
| `VTX_RESIDENT_MB=8`: 3,527 restarts, fills all the time | none | Kratos speckled |
| Internal resolution 1.5, FSR 3.1, depth of field on | none | as usual |
| `GOWA_EDGE_MEMO_MB=16`: six arena changes | none | as usual |
| `GOWA_EDGE_RESIDENT=0` | none | as usual |
| `VTX_RESIDENT_MB=0`: every kept block filled (90 s) | none | as usual |

A play session here in the CYN levels restarted the resident part seven times, without an error either. Restarts and
fills alone do not do it: it seems to need the temple. The reporter was asked for their save, and for a run with
`GOWA_EDGE_RESIDENT=0`.

## The speckled skinned meshes: fixed

The game draws a skinned mesh twice, a depth pass and a lit pass that tests EQUAL against its depth. Either can come from
the vertex pass, which works the position out the SPU's way bit for bit, from the SPU, or from a fill (`ek_fill`, for a
draw that does not take the resident part). `ek_fill` runs on the backend's thread, under the host's rounding, where the
SPU's is toward zero with denormals flushed. With one pass from each, the positions differed in the last bit and the lit
pass left holes all over the mesh. `er_fill`, the unskinned version, already set the SPU's MXCSR for its call; `ek_fill`
now does the same.

The same run after it (`VTX_RESIDENT_MB=8`, 3,462 restarts): Kratos as usual in its three screenshots. A fill takes only
a draw the resident part turns down, so the default settings met this where a level fills the resident part, as the
temple did. The video memory errors are not explained by it.

## Their save, and the cache off (later the same day)

The reporter sent two more reports, with their saves: one more session at the temple, and one started from a `test.bat`
with `GOWA_EDGE_RESIDENT=0`, which froze.

- **The resident part is not the cause.** With it off (no `[vtx-kept]` line at all) the game printed the same errors.
- **It is the same every time.** All three sessions at the temple print one `invalid size` and then 482 `CopyLocalMemory`
  errors: one loop, a copy every 16 MB from offset `0x6000000` up to `0xF7000000`, which is a block whose size went
  negative. The offsets differ from session to session, the count does not.
- **Main memory is damaged too.** The frozen session's threads call through a function descriptor of the game's own
  data, at `0x00833908`, that now holds `A0F800F8 55550000 7F7E0000 00499224`.
- **It does not happen here.** Their save, their settings and their pad recording of the session before (`PAD_REPLAY`),
  one run each: this tree's build, 0 errors; the 0.1.0-alpha.6 Kit itself, its DLL built here by `kit-build` as the
  launcher does, 0 errors. Both reached the temple's level.
- **So it is their machine's.** The SPU workers are the same (six, audio on a lane of its own). What differs: a Ryzen 7
  5700X3D, 8 cores and 16 threads, against 6 and 12 here; a Radeon RX 7900 XT; a 165 Hz display with VSync on.

## What the game does with it

The lines are the game's own, from its VRAM manager, and the lift shows what they come from:

- **CopyLocalMemory** is `func_003A2690` (destination, source, size). A size of 0x20000000 or more prints `invalid size`
  and then meets `tw 31, r1, r1`, a trap: on a PS3 the game would end there, and the lift leaves traps out. It goes on
  and copies 16 MB at a time, NV3089 blits of 4096 rows of 4 KB, until the size is 0. Session 204820's size was
  between -192 and -176 MB: 244 copies, the first three inside local memory, 48 MB over the blocks after the one
  moved, and 241 past it, each printing a source and a destination line.
- Its one caller is **MoveLocalMemory**, `func_003A2ADC`, whose test for an overlap wraps in 32 bits with such a size
  and so calls it directly. Its callers are the defragmenter, `func_0039F06C`, and the queue of moves it leaves for
  later, `func_003A12A0`, both with the size of the block they move, its +0xC.
- The blocks come from `func_0039E3F8`, a stack growing down from the top of the pool. It takes the size it is given:
  a negative one moves the top up by as much, and the blocks after it overlap others or run past local memory. The
  first report's `MoveLocalMemory src offset error: F153680 F46D00`, a block ending 631 KB past local memory, is one
  of those. A texture's size is `[texture + 0x20] - [texture + 0x1C]` (`func_0039B93C`), `func_0039F7C8`'s is at
  `[[r30] + 0x20]`, and the defragmenter's is the moved block's.
- **The copies past local memory reached main memory.** `cellGcmResolveLocated` gives `0xC0000000 + offset` in 32
  bits, so an offset past 256 MB went into the PPU stacks at 0xD0000000 and, past 4 GB, around into main memory. In
  the frozen session (destination 0x93E6600) the 0x37th copy landed on 0x003E6600 to 0x013E6600: the game's data,
  with the function descriptor at 0x00833908 its threads then called through. A real RSX reaches local memory through
  a DMA object whose limit is its size.

What gave a block a negative size there, and not here, is still not known.

## What changed

- **The fork, `cellGcmSys.c`:** an NV3089 copy leaves out the rows, pixels or swizzled surface whose span in local
  memory ends past its size (`gcm_local_span`), and says it in the log the first four times.
- **The port, `port/vram_guard.cpp`, called by the lift (`scripts/ppu_hooks.json`):**
  - `gowa_vram_texture`, in `func_0039B93C` before the size is used: a texture whose size is negative or past 256 MB is
    said in the log with its first 0x50 bytes and its caller, and given an empty block.
  - `gowa_vram_alloc`, on the allocator's entry: such a size from any caller is made 0.
  - `gowa_vram_move`, on MoveLocalMemory's entry: a move either side of which runs past local memory is made a move of
    nothing.
  - `GAME_MODULE_ABI` 3, and `tests/test_game_module_hooks.py`, in CI: a hook missing from `GAME_MODULE_FUNCS` would
    fail the Kit's DLL on a player's PC.

## How it was tried

The temple replay above, one run each, 150 s. A probe, taken out after, made one allocation's size 0xF4300000 (the
size of session 204820's) 50 s in, as their PC had it:

| Run | What the log has |
|---|---|
| the texture and move guards, no probe | nothing new: no error |
| probe, the allocator's guard passed over, the move guard on | 7,514 offset errors from MoveLocalMemory and CopyLocalMemory, every move left out; one call through a NULL function pointer |
| the same, the move guard off too | `invalid size` 10 times and 13,219 offset errors, as in the reports; the NV3089 guard left the copies past local memory out; no freeze |
| probe, every guard on | `[vram] an allocation of 0xF4300000 bytes, from 0x0039F10C: made 0`, and nothing else |

Once the pool's top has gone up the damage is everywhere, so the allocator's guard is the one that keeps it away; the
other two stand behind it for a block whose size goes bad after it got its place. All three callers allocate at the
temple: textures while it loads, then the defragmenter and func_0039F7C8; the run had made 4,096 by 83 s.

## Still open

- What on their machine brings it about. The next report from it says, in its `[vram]` lines, which texture or
  allocation it was and what the texture held.
