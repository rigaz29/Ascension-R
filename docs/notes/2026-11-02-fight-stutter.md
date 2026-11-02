# The first fight's stutter, flicker and flashes

## Symptom

Reported 2026-11-02: in the first fight, mostly when Megaera hits Kratos, the game stutters, lags
and drops frames, and the picture flickers or flashes.

The bench's level segment is that fight. With no input Kratos stands and Megaera hits him
(screenshots every 5 s: her purple attacks at ~85, 105, 125 and 145 s).

## How it was measured

- `RSX_HITCH_MS=<ms>` (new, read with `RSX_CENSUS`). It writes a census hitch line for:
  - every present at least that far from the one before;
  - every present that skipped a draw or queued a pipeline build.

  Each line has its time since the process started and the frame's draws.
- `PS3_FTRACE` gave the game's own frames: 40 s of the fight, vsync on and off (`logs/stutter`).
- `PERF=1 PSO_MISS_WHY=1` gave the pipeline builds (`logs/stutter-perf`; that run's timing is
  not the real one).
- A 75 s screen recording was scanned with `scripts/flash_scan.py` (`logs/stutter-rec`).
  `gdigrab` managed 20 frames a second, not 60, so a one-frame event can fall between two
  captured frames.

## What was found

1. **The game's frames** (flip to flip, 786 frames, vsync on):
   - median 51 ms, p99 76 ms, max 92 ms;
   - without vsync: median 51.5 ms, max 83 ms.

   The long ones are the SPU side: EDGE and audio busier while Megaera attacks.
2. **The presents.** In 66 s, with vsync and without:
   - ~10 gaps of 200-550 ms, and 15-22 of 120-200 ms;
   - each such present carried 5,500-8,192 draws, two or three of the game's frames: the frame
     clock thread fell behind and presented only the last;
   - they come in clusters (97-99 s, 109-112 s, 147-149 s), when the SPU side is heaviest;
   - the walker waited on no semaphore (`gowa.py frames`: no SEMAPHORE_ACQUIRE waits);
   - texture uploads and pipeline builds explain only the first gap of a cluster.

   These gaps, not the game's frames, are the stutter that is felt.
3. **Draws skipped while their pipeline builds** (`D3D12_ASYNC_SHADERS`):
   - up to 1,562 of a frame's 3,678 draws at 80 s;
   - 241-292 at 91-92 s;
   - 1-9 in every frame for 2.5 s at 101-104 s.

   The run built 409 pipelines in all, ~1.15 ms each, every shader already in the disk cache.
   They come in bursts of 10-30 when something appears for the first time. The drain waits at
   most 3 ms in every 16 ms, then skips. Pipelines are not kept between sessions, so every
   session skips again.
4. **Texture uploads:** 61-101 textures in one frame when an effect first appears, 42-107 ms.
5. **The flashes.** The recording has 19 events. Most are Megaera's purple attacks, or dark
   shapes close to the camera (the "holes" at 127-135 s). Three are not the game's:
   - 88 s: the whole picture under flat blue, purple and pink polygons for ~0.5 s;
   - 108.6 s: the whole picture an orange smear;
   - ~92.8 s: white with cyan strokes.

   The first two fall on presents of merged frames (6,838 and 8,192 draws). The present after
   the first one had 66 draws. The title draws into its display buffers as scratch within a
   frame (the video, the tonemap, the post chain; `RSX_DISPLAY_RT`). A merged present that shows
   a display buffer mid-frame would look like this. Not proven yet: the next step is a capture of
   such a present.

   RPCS3's 37 screenshots of the same fight (`rp_level_lv`, 5 s apart) show none of this, but
   they are too sparse to rule out a half-second event.
6. **Vsync on at 60 Hz:** presents are 33, 50 or 67 ms apart (2, 3 or 4 refreshes), mostly 50.
   The uneven mix reads as judder.
7. **Not the cause:**
   - VRAM: 1,486 of 3,344 MB used.
   - The GPU: 9-20 ms a frame (30-57 ms for a merged one).
   - The constant blend colour: dynamic state, not in the pipeline key.
   - The fragment programs' inline constants: in `b1` (`FP_CONSTBUF`), not in the key.
8. **The device:** RPCS3 on this laptop runs the same fight at 17-28 FPS with 7.4 logical CPUs
   busy (2026-10-29). Six SPU workers, the main thread and the frame clock thread need about
   eight cores' worth of a six-core CPU. A locked 30 is out of reach here for either.

## Root cause

**The stutter and the flashes: the drain went on past flips.**
- The walker kept draining past a flip when it was 64 KB or more behind the title.
  `GCM_FIFO_CATCHUP_BYTES` (the runtime's default `0x10000`) is there for a title whose ring
  fills while the walker takes one flip a drain.
- `gcm_fifo_backlog` takes `put - get` as it is when `put >= get`. In the title's own command
  buffers a frame's commands ahead (hundreds of KB) was enough.
- The frame clock thread then presented two to six frames as one. `FLIP_OP_DBG=1` (new) counted
  89 such presents in 75 s of the fight: 68 of two frames, 7 of three, 6 of four, 7 of five, 1
  of six.
- Batches of four to six frames filled the 8,192 records and lost their last flip ops (9 in one
  run).
- Those presents are the 150-717 ms gaps, and the full-screen colour flashes fell on them.

**The flicker: the async pipeline builds.** Within a merged batch the 3 ms wait budget ran out,
and the draws whose pipelines were building were skipped.

## Fix

1. **P1** (port `main.cpp`): `GCM_FIFO_CATCHUP_BYTES=0x1000000`. The drain stops at every flip.
   The title waits for its flips, so it never runs far ahead, and the deadlock the catch-up
   guards against does not arise.
2. **P2** (fork, `D3D12_PSO_CACHE`, on by default): each guest pipeline built is appended to
   `<shader cache>.pso` with:
   - its key (a hash of its pixel shader's HLSL, its vertex shader and the rest of its
     description);
   - that description, written field by field;
   - both shaders' bytecode.

   The next session builds them all on a thread of its own from the first pipeline on. A draw
   finds its pipeline by the key, built.
3. **P3a** (port): the frame clock thread at `THREAD_PRIORITY_HIGHEST`
   (`GOWA_FRAME_CLOCK_PRIO`, new; 0 is normal).
4. **P4** (port, an option, off by default): `[display] frame_cap = 20` or `30`, the launcher's
   "Frame rate cap".
   - A flip completes and is presented no sooner than ~1/cap s after the one before. That holds
     on the vblank beat as on the drain's own path, which `GOWA_FLIP_MIN_MS` alone never gated.
   - While a flip waits for its turn, the drain goes no further.

## Verification

- **P1**, ABBA, three runs each (`logs/abba-catchup`):

  | | old (catch-up at 64 KB) | P1 |
  |---|---|---|
  | flips/s | 20.67 | 20.72 (t 0.1) |
  | presents/s | 19.96 | 20.72 |
  | skipped flips/s | 0.71 | 0 |
  | frame clock thread | 73.0% | 71.9% |
  | gaps of 100 ms or more, per run | 31, 18, 22 | 1, 1, 1 |
  | the longest gap, per run | 426, 248, 717 ms | 105, 119, 152 ms |

  - No present carried more than one flip.
  - A recorded run had no full-screen flash: the strongest `flash_scan` event was +44, Megaera's
    purple.
- **P2** (`logs/p2-psocache`):
  - The first session wrote 331 pipelines (8.5 MB).
  - The second built 331 of 331 in 336 ms at the start. Its draws found 338 pipelines built, and
    4 were new.
  - Draws skipped after 70 s: 5,801 before (75 frames, the worst 1,562), 0 in both sessions with
    P1.
  - Pipelines built during play: 135 in the first session, 4 in the second.
- **P3a**, two runs each (`logs/p3a-prio`); audio from `logs/p3p4-audio`:

  | the frame clock thread's priority | normal | above normal | highest |
  |---|---|---|---|
  | flips/s | 20.85 | 21.10 | 21.23 |
  | the thread's CPU | 72.6% | 72.3% | 70.7% |
  | the longest gap | 157-161 ms | 106-125 ms | 105-115 ms |
  | audio ring gaps per 5 s | 0.2 | | 0.2 |
- **P4**, one run each (`logs/p4-cap`):

  | presents apart | 33 ms | 50 ms | 67 ms | the longest | flips/s |
  |---|---|---|---|---|---|
  | no cap | 29% | 61% | 9% | 179 ms | 20.69 |
  | cap 20 | 0% | 79% | 21% | 86 ms | 19.00 |

  The pace is even, at a lower average: the frames that were ready in 33 ms now wait for 50.
- **Not done:** RPCS3 recorded at the same moments. Its 37 screenshots show no flash, but they
  are too sparse to rule out a half-second one.

## After P1-P4: the texture burst (2026-11-03)

With P1-P4 one long gap is left per session, at 88-93 s: 116-183 ms in the six runs of
`logs/a-ijmp`. Its hitch lines:

- 74-105 textures uploaded, 30.9-86.0 ms on the frame clock thread:
  - decode 17-29 ms;
  - one upload buffer made (a frame index's second spill chunk), 6.2-49.5 ms.
- 4-19 pipelines made, 2.0-11.5 ms.
- The rest, 20-70 ms over a normal frame, is not on the frame clock thread's books. It is most
  likely the game's own frame (its longest were 76-92 ms, "What was found" 1); not traced at
  this moment.

At 77-79 s each frame index made its first spill chunk of the texture ring, 5.3-7.3 ms each.

**What the decode costs**, by format (a census line every 10 s, new; `logs/c-decode`, 149 s):

| format | textures | Mtexels | ms | ns a texel |
|---|---|---|---|---|
| `0x81` B8, swizzled | 357 | 186 | 724 | 3.89 |
| `0x86`/`0x88` DXT1/DXT5 | 8,112 | 3,600 | 366 | 0.08-0.16 (a copy) |
| `0x85` A8R8G8B8, linear | 118 | 99 | 53 | 0.54 |
| `0x9F` Y16_X16_FLOAT, swizzled | 81 | 5 | 21 | 4.5 |

B8 swizzled was two thirds of the decode, at 65-223 ms every 10 s of the fight.

**Done** (fork):
- **C-1:** each frame index's first spill chunk is made at boot with the ring, and is never
  released when idle.
- **C-2:** swizzled B8, A8R8G8B8 and D8R8G8B8 are deswizzled a 4x4 Morton tile at a time with
  32-bit operations (`decode_swizzled_tiles`, `libs/video/rsx_texture_layout.c`).
  - It needs both sides a multiple of 4, at most 4096; other sizes go texel by texel.
  - `RSX_DESWIZZLE_TILES=0` turns it off.
  - `test_decode_swizzled_tiles` checks it against the Morton formula: three formats, 14 sizes
    from 1x1 to 1024x1024, both channel orders. The wrapper is `tools/test_rsx_texture_layout.py`.
- The census line: `[census] texture decode (format, swizzled, compressed: textures, Mtexels,
  ms)`.

**Measured** (`logs/c-decode2`, one run):
- B8 swizzled: 3.89 -> 0.44 ns a texel; over the run 724 -> 64 ms. All decode: ~1.17 s -> ~0.46 s.
- No frame of the fight made the first chunk: it was made at boot, 13.7 ms inside the startup's
  207 ms gap.
- The burst is still there: 143 ms at 92.83 s, with:
  - 111 textures in 33.4 ms: decode 12.5, a second spill chunk 13.1;
  - 15 pipelines in 9.1 ms.

  Most of that gap is the game's frame.

**Not done:**
- A second chunk at boot would save that frame's 6-50 ms, for 64 MB more upload memory.
- Decoding textures off the frame clock thread would take the rest of the upload work from it.
  That belongs with the multithreaded RSX.
