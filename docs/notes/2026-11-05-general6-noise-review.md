# General6's noise: review

Written 2026-11-05, branch `edge`, at port `6bfc470`. The question: General6 (w2) is now the largest
workload after EDGE, and the largest of all in the fight, and it has no native code. Its noise
subroutine is about half of it at the gate. Should that become native code? Nothing here is
implemented; the measurements came from a temporary hook (not kept).

**The result:** no native code is needed. In the levels the game sets the noise's scale to zero, so
every call computes the noise at (0, 0, 0, 0) and gets 0. A test of the four arguments at the
routine's entry is the whole fix, and it is exact.

| temporary hook at `0x4930`, all arguments zero: return 0 | off | on | |
|---|---|---|---|
| the gate, three runs a side | 24.20 (24.38, 24.25, 24.15) | 25.63 (25.63, 25.57, 25.77) | +5.9% |
| the warm fight, two runs a side | 21.20 (21.73, 20.67) | 23.15 (23.35, 22.95) | +9.2% |
| General6, ms of worker time a frame, the gate | 64 | 32 | -50% |
| General6, the same, the fight | 89 | 51 | -43% |
| the SPU workers, the gate | 576% | 537% | |

Audio: no gaps in the four fight runs. All on runs above all off runs, in both scenes.

## What General6 is

Two job modules, two kinds of job.

| | share of General6's samples, gate | fight |
|---|---|---|
| `a67e549fa0ab`, jobs at `0x81E8`: the noise chain (`0x4930`, `0x4AB0`, `0x4E9C`, `0x4F00`) | 49% | 36% |
| the same module, the loop around it (`0x63B4`) | 7% | 5% |
| `1bdd67b54c3b`, jobs at `0xBF60`: `0x5C40`, `0x53E0`, `0xA588`, `0x4ED0`, ... | ~38% | ~45% |
| the runtime | ~6% | ~9% |

Its code is 91-94% lifted. It takes 155% of a core at the gate and 185-196% in the fight, where EDGE
now takes 155%.

## What a noise job does

- **Its output.** One job writes a tile of 128 x 16 pixels, RGBA F16: 16 rows of 1 KB, 16 KB apart. 16
  jobs make a band of 2048 x 16, and three bands, 256 KB apart (`0x72717580`-`0x727D7580`), take
  turns frame by frame. As an image it is a smooth colour gradient, row 0 of a band one colour. It
  looks like a sky or fog table; what reads it was not looked for.
- **Its input.** A 320-byte record at `0x72717410`, read and put back unchanged by every job: a 3x3
  rotation and a position (words 0-15), a drift that moves every frame (words 20-22), a scale (words
  28-30), and colours.
- **Its loop.** Four pixels a group: four calls of `0x4930` (four octaves, four arguments each, a
  pixel a lane) are summed, and each channel is clamp(sum x 0.5 x gain + base), then F16.
- **A call** is ~860 SPU instructions, ~0.5 µs lifted. A job makes 2,048 calls, 16 jobs a frame.

## What the probes found

1. **The output does not change.** Every 1 KB row a job put was compared with what it replaced. The
   gate: 100% the same, for minutes. The fight: 97%.
2. **The input does.** Words 20 and 22 of the record change every frame, in both scenes. In the fight
   the rotation and position change too, ~5 and ~12 times a second. So a key on the record, as for
   EDGE, would miss every frame.
3. **The noise has no effect.** Replay of the 40 General6 jobs in `logs/jobcap6`, with `0x4930`
   returning a constant:
   - 0: no PUT byte different (state mode), and in strict mode only 72-75 bytes of dead stack, all
     registers the same;
   - 0.25, -0.25, 1, 100: all 23 noise jobs differ, 3,840 bytes each.

   So the output does depend on the noise, and the noise it gets is 0.
4. **The noise is 0 because its arguments are.** All 176,640 values in that replay were 0, from
   arguments r3-r6 all zero.
5. **In the game.** `G6_FAST=2` ran the routine and checked it:
   - the gate: 56.7 M of 67.1 M calls had all arguments zero, the fight 81.7 M of 92.3 M;
   - every one of them returned 0;
   - the calls with other arguments all came in the main menu; from the level on, every call is
     zero.
6. **Why.** The record in the menu and in the level:

   | words | the menu | the gate |
   |---|---|---|
   | 28-30, the scale | 9.9, 5.1, 1.5 | 0, 0, 0 |
   | 31 | 0.295 | 0.05 |
   | 32-34 | 1, 0.221, 0.456 | 0.25, 0.1, 0.35 |

   The fight's and the first level's have the same zeros. This is the game's own data: the level
   turns the noise off by its scale, and the job computes it anyway. The PS3 spent the same SPU time.

## Why the earlier oracle saw less

Performance plan 2.21 made the noise free the same way and saw at most +3%, and wrote a native
routine off. Nothing was full then: EDGE took up what General6 gave back. Now the workers are.

## The options

| | what | gain | work | risk |
|---|---|---|---|---|
| A | a hook at `0x4930`: r3-r6 all zero, return 0 | as measured above | a few lines | low |
| B | the noise as native code | only where the scale is not zero: the main menu so far | days | medium: the float forms, ~860 instructions |
| C | keep the jobs' output, as for EDGE | none over A: the record changes every frame | days | medium |
| D | A, and leave the job's pixel loop out too when the record's other words repeat | up to ~7% of General6 more | days | medium |

**A is the recommendation.** B stays in reserve: a level that sets the scale would bring the cost
back, with nothing lost against today. The game's log could count such calls, to see whether any
level does.

**How A would be checked.**
- Replay: state mode, 0 differences; strict mode, the dead stack only (above).
- A check mode, `G6_FAST=2` as in the probe: the routine runs and its result is compared with 0.
- It is measured, not proved: the routine's code was read only in part. It floors the arguments and
  interpolates values from a table at LS `0x4680`; 138 M calls with zero arguments returned 0, and
  the check mode stays for what the two scenes do not show.
- `0x4930` is a function with its own frame: it returns r3 and restores r80 and up, so leaving it at
  its entry changes only r3.

## What sets the frame after it

With the hook on, each part 25% slower, the gate, two runs each (`logs/edge-ana/g6p`):

| +25% on | flips/s | |
|---|---|---|
| nothing | 25.66 | |
| the frame clock thread | 22.16 | -13.6% |
| EDGE | 24.54 | -4.4% |
| the main thread | 24.75 | -3.5% |

- **The gate's limit moves to the frame clock thread:** 92% of a core at 25.5 flips/s, ~36 ms a flip.
  It is full near 27.7. Its work did not change; the workers stopped hiding it.
- So the render thread (fork branch `perf/d3d12-render-thread`, mode 2) or less work on that thread
  comes next at the gate. The render thread cost frames when it was tried (performance plan, phase
  2b plan's second round) because the workers were the limit then.
- **The fight is different:** the frame clock thread at 62%, the workers at 507% of 600%, the main
  thread at 94%. What sets it there was not probed.
- The `0xBF60` jobs are General6's other half, ~33 ms a frame at the gate, ~45 in the fight. Their
  output changes every frame, so nothing can be kept from it. They were not looked at further.
