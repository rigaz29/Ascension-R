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

## Next (for review)

- The flashes: capture a merged present and fix what it shows.
- The skipped draws: keep the pipelines' keys between sessions and build them before the fight.
- The frame clock thread's clusters: its priority or a core of its own as a first test; texture
  uploads off it; the multithreaded RSX as the structural step.
- Even frame pacing as an option.
