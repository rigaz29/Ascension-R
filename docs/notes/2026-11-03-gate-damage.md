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
