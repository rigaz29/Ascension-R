# L8, before the work: the main thread's own work, and what the frame waits on

The plan was to measure the main thread's own work on the first level and try a lifter change
(guest registers held in host variables inside a function) on one hot PPU function. The
measurement says that would not change the frame rate now, so no prototype was built.

## The main thread: about a fifth of the frame is its own work

`PS3_SAMPLE_THREAD=ppu main`, 75 s of the level, symbolized against the map of the build that
recorded it (`symbolize --map`: a later build moves the runtime's functions, and an older
samples file read with a newer map named `sys_timer_usleep` as `sj_wait`). Samples by what the
thread was doing:

| | share |
|---|---|
| `usleep`: `sys_timer_usleep` and the waits under indirect calls (the trace's 38 ms blocked a frame) | 29.9% |
| the job-graph scan polling (`func_0044CA48`, `func_0044CD20`, `func_0044D120`) | 26.7% |
| progress-counter waits: `func_0049D688`, and `func_0049B76C`'s loop of up to 999,999 rounds over `func_0049B35C` / `func_0049CB6C` (the seven 16-bit counters' minimum) / `func_0049B2FC` | 21.1% |
| coherency notifies from the stores inside those loops | ~3.5% |
| everything else: the frame's own work | ~19%, about 25 ms of a 133 ms frame |

A 30 us `usleep` is a `SwitchToThread` busy-wait in the runtime (`SYS_USLEEP_SPIN_MAX_US`, 200),
which is why `ppu main` shows ~95% CPU while it mostly waits. The earlier note's "own work ~57
ms, ~17 flips/s with free SPU code" counted these loops as work and was wrong: with the waits
gone, the main thread alone would allow about 40.

## While it waits, the SPU workers are half idle

The same ticks from a run that sampled the main thread and the six policy-module workers
together (each worker sample carries the workload it was running, or none):

| main thread in | share of ticks | workers in a run | mostly |
|---|---|---|---|
| `usleep` (the EDGE chain's frame counter) | 23% | 2.25 of 6 | audio 0.6, each EDGE list 0.3 |
| the job-graph scan | 19% | 3.85 | audio, w2, EDGE |
| `func_0049B35C` family | 19% | 3.26 | w6 always (1.0), audio, w12 |
| `func_0049D688` | 8% | 3.73 | w2 0.8, EDGE |

So the frame is neither bound by the main thread nor by SPU throughput: the job chains it waits
on run in short runs with gaps, and for a good part of the wait no worker is on them at all.

## The hand-off between policy-module runs

`SPURS_DISPATCH_PROF=<s>` (new): a run ends on its worker, which wakes the coordinator; the
coordinator's pass picks the next workload and wakes the worker -- two thread wake-ups through
one condition variable all six workers and the coordinator sleep on, each a
`WakeAllConditionVariable`. Measured on the level, ~21,000 runs/s:

- coordinator to worker: 16 us mean, 1.7-3.3 ms worst per 10 s;
- a run that found work ending to the same slot getting the same workload again: 20 us mean;
- ~19,000 coordinator passes/s, ~120 of them on its 1 ms timeout.

An EDGE run is ~140 us, so each hop of a chain pays ~36 us (+25%) on top; on the SPU the
kernel picks the next workload on the same core.

## What to do next

1. **L6, the hand-off**: a worker whose run found work runs its workload again, or takes the
   next one itself, instead of going through the coordinator; one wake-up per slot instead of
   waking all seven threads. Measure with `SPURS_DISPATCH_PROF` and `SPURS_WID_PROF`, then
   flips/s.
2. **What the EDGE lists wait on** when no worker runs them during the main thread's `usleep`:
   their job chains' own dependencies (ROADMAP: RsxSync entries wait on report slots the RSX
   writes). Needs a job-level view: which job, waiting on what, for how long.
3. The main thread's own work (L8) and the lifters' register handling come after: at ~25 ms a
   frame they are not what holds the level at 7.

## Result: L6, the worker picks its next workload itself

`cellSpurs.c`: when a policy-module run ends, its worker -- already holding the instance lock --
runs the coordinator's pick for its own slot (`spurs_pick`, the coordinator's per-slot body
moved into a function both use): the next workload round robin from the cursor, kicked (ready
count, signal, workload flag) or found work in its last run (idle 0). The idle back-off probes
stay with the coordinator, whose passes count them. Only when that finds nothing does the
worker wake the coordinator. Each worker also sleeps on a condition variable of its own, so the
coordinator wakes the one it handed a run instead of all seven threads. `SPURS_SELF_PICK=0`
goes back to coordinator picks.

`SPURS_DISPATCH_PROF` on the level: wake 15-17 us -> 2.2 us, the same workload again after a
working run 18-19 us -> 1.2 us, coordinator passes 20,000/s -> 5,500/s (more of them now on the
1 ms timeout). EDGE runs 127-137 us -> 123 us, ~4,300 -> ~4,900 runs/s per list.

First level, from 10 s in, three ways alternated, two runs each, with `GOWA_THREADS`,
`SPURS_WID_PROF` and `SPURS_DISPATCH_PROF`:

| | flips/s median (mean) |
|---|---|
| before | 7.15 (7.02) |
| own condition variables only (`SPURS_SELF_PICK=0`) | 7.50 (7.30) |
| self-pick | 9.44 (9.09), +32% |

To watch: with fewer passes, a workload that is only ever started by a back-off probe (never
kicked) is probed less often in real time -- every 16 passes at idle 4 is now ~3 ms, was ~0.8 ms.
