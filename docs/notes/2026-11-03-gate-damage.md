# The broken picture and the halting after the first fight

## Symptom

Reported 2026-11-03 with two screenshots (borderless, 1920x1080). After the first fight, at the
gate and in the area after it, the picture breaks, and Kratos "cannot move on, halting".

- **Screenshot 1, at the gate.**
  - A horizontal seam at row 931 of 1080 (row ~621 of the 720p frame). Below it, another,
    larger picture.
  - Above Kratos, a column of stretched copies of his back.
  - In the gate's opening, a rectangle holding a blurred, offset copy of the far scene.
  - Kratos himself is clean.
- **Screenshot 2.** Walls and floor in sawtooth and comb shapes. Kratos and his blade are clean.

The session:

- **Settings** (`gowa.ini`, saved 06:22): borderless, SMAA, sharpening high, Disable MLAA on,
  Disable Mesh Trimming on (new that session).
- **Route.** It started at the first fight's checkpoint and saved the gate's checkpoint at
  06:25.
- **No log.** A normal launch writes none.

## Runs

All from the gate checkpoint, on a copy of the save (`--set paths.hdd0=`, the game folder
linked), windowed 1600x900.

| run | what | result |
|---|---|---|
| `logs/repro-gate` | Disable Mesh Trimming on and off, stick forward | no damage in 18 screenshots; 14.9 / 15.2 flips/s |
| `logs/repro-gate2` | walking and jumping on into the next area | no damage; 16.1 flips/s |
| `logs/rt64` | 64 slots (below) / 20 slots | no damage, 17.3 flips/s / see below |
| `logs/rt28` | 28 slots, the old and the new eviction | no damage, 17.3 flips/s both |

**The area is slower than the fight.**

- All six SPU threads run at ~97% (580% of 600), and the PPU main thread at 95%.
- For ~30 s after arriving at the gate it runs 11-15 flips/s, then ~17. The fight runs ~21.
- At the ledge Kratos runs into the wall until he jumps ("Jumping: press X"), as the bench's
  input made him do.

## The render-target table

The backend's table of offscreen render targets had 32 slots.

- **Occupancy** (the new census line): a session from the gate checkpoint holds 29 targets
  (34 made, 5 remade).
- **What a full table did.** It evicted the first slot not yet bound this frame. Early in a
  frame that is any of the frame's own targets.
  - The eviction waits for the GPU.
  - The evicted target loses what it held: a new target starts empty (`RSX_RT_READ=0`), and a
    texture read of an evicted one reads guest memory.
- **`RSX_RT_SLOTS=28`** (one short of 29), with either eviction choice:
  - the three display buffers evicted each other every frame for ~25 s;
  - the frame rate was the same as with 64 slots, 11-12 flips/s at that time in all three runs;
  - the screenshots were clean.
- **`RSX_RT_SLOTS=20`:**
  - 1,226 evictions and 1,152 refused draws;
  - 9.2 flips/s, with windows down to 1.8;
  - no screenshot was written.
- **Changed** (fork 395dc4e5):
  - 64 slots;
  - a full table evicts the slot bound or sampled longest ago among those not bound this frame
    (`RSX_RT_EVICT=first`: the old choice);
  - `RSX_RT_SLOTS=<n>` uses fewer slots;
  - evictions are logged, and `RSX_CENSUS` has a render-target line.

## Not found

The damage was not reproduced. The user's session had two things these runs did not:

- the fight and its cinematic before the gate;
- borderless 1920x1080.

A table with 3 spare slots after a fresh start makes a full one in a longer session plausible.
That it caused the damage is not shown.

## Next

`build/GowARecomp-debug.bat` runs the game as usual. With it:

- **The log** goes to `logs/user-<time>.log`, with:
  - the render-target census;
  - per-thread CPU;
  - every frame that took 100 ms or more.
- **F12** also saves that frame's targets and draw list, and the next frame's
  (`RSX_CAP_SHOT`), to `logs/user-cap`.

From a session that shows the damage:

- the census says how many targets that session held;
- a capture of a damaged frame shows which pass read what.

## The slow gate

The user then played a New Game on profile 2 with `GowARecomp-debug.bat`
(`logs/user-20261103-073254.log`, one F12 capture). The picture was right. What was left was "a
freeze of ~20 s" once Kratos could walk after the whipping scene.

**From the session's log:**

- **No freeze.** No present came more than 201 ms after the one before, except 378 ms at the
  F12 capture.
- **The slow stretch.** From the second autosave (~245 s), when the title unmaps and maps IO
  memory for the next section, frames took 80-100 ms: 10-11 flips/s for ~20 s.
- **The render-target table** held 29 targets and evicted none.

**At the gate, from profile 2's checkpoint.**

- `gowa.py bench --profile 2` (new) continues another profile's checkpoint: Down between the
  first two Crosses.
- Kratos idle, `logs/slow` to `logs/slow3`.

| | flips/s | frame clock thread | EDGE w14-16, a run |
|---|---|---|---|
| Disable Mesh Trimming on (the user's setting) | 11.67 | 85% | 7.6-7.7 ms |
| Disable Mesh Trimming off | 13.12 (+12%) | 65% | 7.3-7.6 ms |
| Disable Mesh Trimming off, Disable Motion Blur on | 14.01 (+20%) | 68% | 6.8-7.0 ms |

- **The SPU threads are full** (575-595% of 600).
  - The three EDGE workloads take 117-120% of a core each, at 7.3-7.7 ms a run.
  - In the fight (2026-11-02) they took ~95% each, at 4.2-4.5 ms a run.
  - EDGE's native routines do run (skinning ~99% native), and no SPU code is interpreted.
- **Disable Mesh Trimming** makes EDGE keep the triangles it would trim.
  - At 720p that adds nothing to the picture.
  - Here it costs the frame clock thread a fifth of its time, and 12% of the frame rate.
- **Disable Motion Blur.** With Disable MLAA the motion blur's output is overwritten
  ([FSR 3 note](2026-11-03-fsr3.md), section 3). So turning the blur off costs nothing that is
  seen.
  - It takes the velocity pass's objects out of EDGE's work: 6.8-7.0 ms a run.
  - It gains another 7% here.
- **Walking on.** From the gate into the jumping area the frame rate rises to ~17 (the runs
  above).
