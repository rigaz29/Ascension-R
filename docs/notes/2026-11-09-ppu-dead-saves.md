# Dead stack saves in the lifted PPU code (2026-11-09)

The third item of [the survey](2026-11-08-recomp-feature-survey.md). The lifted code now does less, but the frame rate did
not move, so `--dead-saves` stays off. What the work found matters more: the main thread is no longer what holds the game back.

## What the main thread does now

`PS3_SAMPLE_THREAD=ppu main`, FSR 1, no frame generation, scale 1:

| | waits and polling | lifted code | runtime |
|---|---|---|---|
| route to the brothel room, 50 s | about 71% | | |
| opening cinematic, 30 s, four runs | 48-53% | 2.3-2.6 ms a flip | 4.5-5.8 ms a flip |
| scripted fight, 50 s, four runs | 6.6-10.6 ms a flip | 4.2-4.5 ms a flip | 3.9-4.4 ms a flip |

On the route, `func_0041CBDC` and `func_0049DB90` are 36% of the thread. They are the wait for EDGE's batch counters, with
`sys_timer_usleep` (22%) and `poll_mwaitx` (13%) beside them. So the thread's 95% of a core is mostly waiting for the SPU
workers, which were at 575-582% of 600% in the fight.

[The main-thread review](2026-11-06-main-thread-review.md) found 72% of the thread in lifted code and 31 ms of work a frame
in the fight. That no longer holds. Its work is now about 8-9 ms of a 30 ms frame, and it waits for a good part of the rest.

## The pass

`tools/ppu_dead_saves.py`, `gowa.py lift --dead-saves`. More than half of the stores in the lifted code go to the stack, 152,906
of 278,311, and most of those save a register no code reads back: the lifter restores r14-r31 from a copy it took at entry and
r2 from a literal. The pass drops those stores, gives f14-f31 and LR the same copy at entry, and writes the back chain and LR
without the range compare and the reserved-line test. Only guest stack memory changes. The file's comment has the rules.

| | |
|---|---|
| GPR saves dropped | 57,250 (1,621 kept by the rules) |
| FPR saves dropped | 3,692 |
| LR reloads from a copy | 14,623 |
| `GowARecomp.exe` | 90.3 MB -> 82.3 MB |
| `ppu_recomp_047.cpp`, instructions | 167,685 -> 138,694 (-17.3%) |
| `func_002EB798`, instructions | 474 -> 317 |

`__restrict` on top made that file longer (142,796), as it did before.

## Why it stays off

The same scripted fight, sampled, two runs a side in ABBA order:

| | lifted code, ms a flip | flips/s |
|---|---|---|
| without | 4.46, 4.46 | 27.9, 34.9 |
| with | 4.41, 4.17 | 32.9, 32.4 |

That is about 0.17 ms of a 30 ms frame, on a thread that waits anyway. An idle frame rate could not show it, and it does not
pay for changing what 22,000 functions leave on the guest stack.

`--dead-saves=poison` writes a poison value in place of each dropped save, to catch a reader the rules miss. That build ran the
route into the room, a fight, and the boss through the CYN050 cutscene into the next level, with no crash and the right picture.
It covered 36,101 of the saves, as a pattern for the flags' form was missing then. Run it again on all of them before turning
the pass on.

Guest registers as C locals, the survey's main idea for this item, was not built. With about 4.4 ms a frame in lifted code and
the thread not on the critical path, there is little for it to win.

## The benches here are noisier than they look

Medians of the same executable, one run against another:

| | |
|---|---|
| scripted fight | 34.7 and 29.2 |
| game-over screen over the first fight | 46.2 and 40.8 (steady to ±0.2 inside a run) |
| opening cinematic, 10 s windows | 3-4 flips/s apart |

Enemies and where the player dies differ from run to run: worker time per flip behind the game-over screen went from 119 to
162 ms. A few percent cannot be seen in flips/s with a handful of runs. Measure what the change is meant to change instead: the
thread's time per flip, from samples.

The next gain has to come from the SPU side.
