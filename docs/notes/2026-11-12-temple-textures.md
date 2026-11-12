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

## Still open

- What on their machine brings it about. A run held to 12 threads (`start /affinity FFF`), one without VSync and one
  without the render thread would tell the CPU's part from the display's and the backend's.
