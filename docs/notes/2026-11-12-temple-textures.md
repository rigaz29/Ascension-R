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

## The 0.1.0-alpha.7 report (2026-11-13)

The reporter played the temple again on the alpha.7 Kit: no freeze, the textures still broken (a statue in stripes of
noise). The texture and allocation guards said nothing. The move guard said eight moves from the queue's processor
(`0x003A135C`): four of -15, -28, -70 and -105 MB inside local memory, then moves of `0x40180` bytes, each 0x280000 up
from where the one before went, past local memory. After those the game's own offset errors, about 600 in five bursts,
up to `0x46D11700`. Of the four, each move ends at its destination where the next one ends at its source.

The session's precompile had failed: `no PSARC archives in <settings>`. Their game folder has only `GOWA.SELF`; the
launcher decrypts it into the port's own folder and points `paths.elf` there, and the precompile looked for the archives
beside `paths.elf`. Their shader cache had 1,177 programs (7,364 here), so the temple built shaders while it played.

### How the VRAM manager moves blocks

- **The queue** at `0x9B2838`: entries of 12 bytes (source, destination, size, as offsets in the pool), the count at
  `0x9B2834`, room for 128 before the next global (`0x9B2E40`). The defragmenter (`func_0039F06C`), the run compactor
  (`func_0039F3C4`) and the texture mover (`func_003A0000`) add to it, and the manager from a second list (`0x9B26B0`,
  32 entries, its count at `0x9B2830`, filled by `func_0039C8E0` for the loader). No one checks the room in either.
- **The processor**, `func_003A12A0`, at the frame's end: a TEXTURE_READ_SEMAPHORE_RELEASE of label 0x4E with the
  counter at `0x9B227C`, then every move, then the count to 0.
- **The manager**, `func_003A0780`, works only once label 0x4E holds that counter, and otherwise leaves the frame. Each
  pass starts with `func_003A05F8`, which sets the frame's budget (`[0x83CD2C]`, 16 MB here) and zeroes the bytes moved
  (`[0x9AF4BC]`). Each producer adds its move's size to those, and the defragmenter stops at the budget: no one checks
  a size.
- **The pool** (`[0x83CD30]` = `0xC38CB000`): a stack of blocks down from the top, each a descriptor of 128 bytes in main
  memory (+8 the offset, +0xC the size, +0x18 the texture, +0x1C the address), listed in the table at `0x9AF51C`.

### Tried here

A probe, taken out after. The reporter's alpha.7 pad recording on their saves (the earlier report's are the session's
start: only `DAT-11` and `DAT-31` changed, during it), with their settings, one run each:

| Run | What it showed |
|---|---|
| Their shader cache's state, an empty one | stops of 15 to 17 s while shaders built, and the replay left the session's path after `TEM350AI`, back to the menu where they went on to the fight (`R_GOAT00`, `R_BRUTE10`, `TEM400`); 16,000 manager passes, 36% left while label 0x4E was behind, never two in a row; the second list never used; the queue at most 9 |
| The full cache | the same (9,607 of 26,000 left); the replay waited for `TEM350AI` and `R_BRUTE10` and went on without them; every function of the pool on the game's main thread: the loader's callback (`func_0039D858`), texture allocation, the allocator, the manager, the processor, the compactor, the defragmenter, the texture mover |

No overflow of either list and no second thread in the pool, then, and no `[vram]` line. Their first bad moves came
before the fight, while TEM300, TEMBAS and TEM350 streamed in, which both replays went through.

### The texture mover

`func_003A0000` moves a streamed texture's data into the larger block it grows into: the texture at the head of
`[0x9B26AC]`, its range `[+0x1C, +0x20)`, out of its block into `[0x9B1DEC]`. The size is `[+0x20] - [+0x1C]`, checked by
nothing, then counted against the frame's budget and queued. A range that ends before it starts is the alpha.6 reports'
texture whose data ends before it starts, which `gowa_vram_texture` catches in `func_0039B93C`; this path goes neither
through it nor through the allocator. With the size negative the bytes moved go down by as much, and the defragmenter
moves that much more before it meets the budget.

A probe made the first texture move's range end 0xED5500 before its start, the size of the reporter's first bad move,
one replay each:

| Run | What the log has |
|---|---|
| the new guard off | the queue's move of `0xFF12AB00` bytes from `0x003A135C`, left out by the move guard: the reporter's first line |
| the new guard on | `[vram] streamed texture 0x753074E0: its range 0x01400000..0x0052AB00 ends before it starts ...: moved as 0 bytes`, both blocks and the texture's 0x50 bytes, and nothing else |

Neither went on to hundreds of moves: there was nothing more to defragment then. On their PC the sizes grew, -15 to
-105 MB, and each took the bytes moved down with it.

### What changed

- **`port/vram_guard.cpp`:**
  - `gowa_vram_stream`, at `0x003A0098` (`func_003A0000` and its copy `func_0039FFFC`): a texture move's size that is
    negative or past local memory is made 0 before it counts against the budget or is queued, and said with the texture,
    its range, both blocks and its first 0x50 bytes. `GOWA_VRAM_STREAM_GUARD=0` leaves it as the game has it, to try the
    guard.
  - `gowa_vram_runs`, at `0x0039F64C` (`func_0039F3C4`): a run of blocks whose size is negative or past local memory is
    said with its blocks, and left as it is: the blocks' new places are worked out from it.
  - `GAME_MODULE_ABI` 4: a Kit's DLL is built again.
- **The precompile** (`port/precompile.cpp`): the archives are looked for where the game opens them,
  `<hdd0>\game\NPUA80918\USRDIR`, then `<vfs>\PS3_GAME\USRDIR`, then beside `paths.elf`. The gate
  (`port/loading_screen.cpp`) passes `paths.hdd0` and `paths.vfs` to it as well as `paths.elf`.

## Still open

- What gives a streamed texture a range that ends before it starts on their PC. The next report's
  `[vram] streamed texture` line says which texture, its range and both blocks.

## The two tests, and the texture cache (2026-11-14)

The reporter ran the two tests asked for, on 0.1.0-alpha.8, then 0.1.0-alpha.9 as it was (RX 7900 XT, Ryzen 7
5700X3D, a 165 Hz display, VSync on, no cap; FSR 3.1 at 1.5x, SMAA).

| session | build | | the picture | `[vram]` lines |
|---|---|---|---|---|
| 14:47 | alpha.8 | render thread off | broken within three minutes | after TEM300 |
| 14:52 | alpha.8 | render thread on, `TEX_NOCACHE=1` | right | none |
| 15:07 | alpha.9 | as it is | broken | right after R_BRUTE10, in TEM400 |

- The `TEX_NOCACHE` session went the furthest: TEM350, TEM700, TEMBAS, TEM300, TEM400 with the brute, TEM450 and
  TEM500, every place the two others broke in.
- The pictures broke further than before: nearly every texture, the menus' font, icons and HUD among them, and Kratos
  teal. The `[vram]` lines are #9's own: a descriptor of 0x2AAB0 bytes, a 512x512 DXT1 chain, less another texture's
  start.
- `TEX_NOCACHE=1` reads every texture again from the game's memory at every frame. That the picture was right through
  the whole session says the game's bytes were right then; what broke was the copy the cache holds. The render thread
  is not it.

### The cache's check

`vp_upload_tex_slot` uploads a texture again only when `tex_changed` finds its source bytes changed. Three things in it:

- The checksum was taken after the decode. A texture the title wrote while it was decoded was held half old and half
  new, with the checksum of the new bytes, so every later bind found it unchanged. The title does write textures the RSX
  is still to read: the port lets label 0x4E go (`TEXTURE_READ_SEMAPHORE_RELEASE`) before every draw ahead has
  uploaded its textures (1.9% of the releases in the texture probe above), and the streaming refills those blocks.
- It covered level 0 of face 0 only: the other levels of a chain, the other faces of a cube, were never compared.
- It is sparse, 1,024 words at most, one eighth of them at each frame's first bind. Kept: a whole new image shows in any
  part.

### What changed (fork)

- The checksum is taken before the decode, over all it reads: the chain to its last level, the six pyramids of a cube,
  every slice of a volume. After the decode it is taken again; when the two differ, the slot's `again` makes its next
  bind upload it, whatever part that bind compares, and the log says so: `[tex] <address> <size> format <f> changed while
  it was read (<n> so far): read again at its next bind`, the first eight and then every power of two.
- **F9** in the game's window marks every held texture to be read again at its next bind, with a note on screen and
  `[tex] F9: the <n> textures held are read again at their next bind` in the log. A broken picture that F9 mends was the
  held copy; one it does not mend is in the game's memory.

### Tried here

The race does not happen on this PC, so a probe (`TEX_TORN_TEST=N`, not committed) changed the first source byte of every
Nth upload right after its decode, as the title would. The temple replay, three runs:

- Every changed upload was seen and read again at its next bind: 33 changed, 32 rebinds that found `again`; 65 and 64,
  the last one each time not bound again yet when counted. F9 marked 2,869 textures; the picture after it was right.
- One of the probe runs froze before the probe had changed anything: the FIFO walk met a jump to itself (`0x202FF4A8`
  at IO 0x2FF4A8) and parked, as in #7. One temple run of 45 so far; the next one did not.
- The build without the probe, 40 s of the level, one run against one before the change: 53.5 flips a second on average
  against 52.6, median 55.5 against 54.7, the least 40.5 against 34.7. No upload changed while it was read.

### Still open

- Why `TEX_NOCACHE` also took the `[vram]` lines away: the game's own bookkeeping, not the cache. It makes each frame's
  RSX work heavier, which moves the timing.
- Label 0x4E itself: it should wait for the textures of the draws before it. Left as it is: what it lets happen, a
  texture read while it is written, now lasts one frame.
- The reporter's next run tells which it was: no break, the cache; a break F9 mends, the cache by another way; a break
  it does not mend, the game's memory, and the `[vram]` lines with it.
