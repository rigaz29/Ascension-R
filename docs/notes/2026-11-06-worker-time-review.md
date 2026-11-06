# Where the workers' time goes: third review

Written 2026-11-06, branch `edge`, at port `d720ed9`, fork `f69975a0`. No code changed; every probe was
temporary. The question: J or S next, after G and K ([the second review](2026-11-06-edge-gpu-review-2.md))?

**The result**
- Neither first. General6 comes first.
- The gate is at the game's 30 flips/s. The fight is the scene left, and it has two levels: ~27.6 flips/s
  with General6 at ~49 ms a flip, ~25.5 with General6 at ~66.
- A millisecond of General6, RenderJobs or RenJobSet is worth about twice one of EDGE: EDGE has three
  lanes and slack, the others sit on the chain.
- 65% of General6 is seven routines of one module, and one of them is a plain `memset`. It is lifted code
  with no protocol around it, and the replay can check it exactly.
- S and J are worth about the same, +3% to +3.5% each, for weeks of work. They stay for after.

## What came of it

M, P and G are done, on by default (`native/spu_g6_native.c`, `native/spu_g6_routines.c`,
`scripts/spu_to_native.py`). Exact: the replay's General6 jobs are the same in strict mode.

| | estimate | measured |
|---|---|---|
| the fight, flips/s, two runs a side | +6% | 27.65 -> 28.06, +1.5% |
| General6, ms a flip, as the title runs it | -15 | 51 -> 47 |
| General6, ms a flip, on three workers | | 44 -> 35-40 |
| General6's `0xBF60` jobs in the replay | | 452 -> 322 µs |

- The work went down by a fifth, the frames did not follow: much of General6's time on six workers is
  waiting, not work. That the review had in hand (its own work ~42 ms, wide runs +7-24 ms) and did not take
  into the estimate.
- Three workers for General6 is still no setting to change: 27.87, 26.26, 27.46 flips/s.
- The first version of the tool's routines was no faster than the lifted code: a fence on every value kept
  them in memory. A test on what leaves a pass, and the flags cleared once a loop, gave the gain.
- `0x4ED0` stays lifted: it has the speculative forms already. `0x63B4` calls the noise routine four times
  a pass.

**S and J, looked at again** with the traces of this review, before starting them.

- **S.** A hit's own code is flat: 14.5 µs in some 25 places, the largest `0xDBC8` 1.6-3.1 µs and `0x5C88` 1.8-2.5.
  What a hit could leave out is ~5 µs of a position job and ~10 of a lit one, not all of the code: the
  rings, `0x17E0`, the draw constants and the record have to run. About -8 ms a flip, +1.5%.
- **J.** Of the job manager's 15.6 µs, the tag wait's 4.7 is the transfers, which no rewrite takes away. The
  other ~11 µs is 350 labels of flat code. Native at half of that: -5 µs a job, -9 ms a flip of EDGE and
  -2 of the rest, +2.4%.
- **What small steps give:** each of the cheap pieces is 0.2-1 µs a job, 0.3-1.7 ms a flip, +0.05-0.3%.
- Both are weeks, and the last estimates ran high by a factor of four. Not started.

## Where things stand

| | gate | fight |
|---|---|---|
| flips/s | 30.0 (at the cap in half of the windows) | ~27.6 or ~25.5 |
| frame clock thread | 31.5 ms a flip, 92% | 25.5 ms, 68% |
| main thread | 91% | 92% |
| workers, in a workload | 189 ms a flip | 204 ms a flip |

The workers' 204 ms, the fight's faster level:

| | ms a flip | jobs a second | µs a job |
|---|---|---|---|
| EDGE w14-16 | 57 | 39,000 | 48 |
| General6 | 49 | 1,900 | 595 |
| RenJobSet | 28 | 6,950 | 95 |
| RenderJobs | 25 | 930 | 730 |
| audio | 22 | 3,900 | 155 |
| w6 | 12 | 930 | 325 |
| the rest | 11 | | |

(Jobs and their times are from a `--jobcap` build, which is slower: EDGE takes 90 ms a flip there.)

## What sets the frame

Each part 25% slower, against the level the run was on (`logs/edge-ana/r3f1`-`r3f4`, `r3g`):

| +25% on | fight, flips/s | | worker ms added | % of the frames a ms | gate |
|---|---|---|---|---|---|
| nothing | 27.9, 27.1, 26.8 / 25.3, 25.9, 25.4 | | | | 30.30 |
| EDGE | 27.01, 26.84 | -2.4% | +13 | 0.18 | 29.50 |
| the main thread | 26.84, 26.44 | -3.5% | | | 29.02 |
| the frame clock thread | 27.06 | -2.0% | | | 25.52 |
| General6 | 26.14, 26.30 | -5% | +13 | 0.38 | |
| RenJobSet | 26.67 / 24.44 | -3.5% | +7.4 | 0.47 | |
| RenderJobs | 25.00, 24.66 (slower level) | -2.7% | +6.8 | 0.40 | |

- One or two runs each, with a spread of ±2%: these are rough.
- No part is a wall in the fight. Everything is a little on the path.
- At the gate the frame clock thread is next: full at 32 flips/s, above the cap.
- So the render thread is not needed now. `perf/render-thread-2`, mode 2 (ten runs without a hang, frames
  unchanged at the time) stays where it is.

## The fight's two levels

The same build gives 25-28 flips/s, and a run sits on a level for a minute or more.

| | faster | slower |
|---|---|---|
| flips/s | 27.6 | 25.5 |
| General6, ms a flip | 49 | 66 |
| its time a run | 1,100 µs | 1,500-1,700 µs |
| EDGE, RenderJobs, RenJobSet | the same | the same |
| General6's busy runs a flip | 17 | 17 |

- **How often:** 7 of the last 24 runs. In 9 runs with the sampler on it never came. So timing brings it on,
  not what is on screen.
- **Not thread placement.** In ten seconds every workload runs on all twelve logical processors.
- **What it looks like:** a run that follows another workload on its worker takes 3.5 ms on the faster
  level and 6-7 ms on the slower one, and General6's time on SPUs 0-3 goes from 52% of a core to 85%.
  General6 spreads wider and gets slower.
- **Limiting it** (`SPURS_MAXCONT=2=<n>`, `logs/edge-ana/r3y`):

  | workers | General6, ms a flip | all workers | flips/s |
  |---|---|---|---|
  | 6, the title's | 49-66 | 204-228 | 25.5-27.6 |
  | 3 | 44 | 200 | 26.17 |
  | 2 | 42-43 | 193-199 | 24.0, 24.8 |

  So General6's own work is ~42 ms a flip, and wide runs add 7-24 ms of waiting or cold code. But fewer
  workers lengthen it, and it is on the chain, so the frames do not follow. It is no setting to change.
- **Not explained:** what brings the slower level on, and whether the extra time is spinning at the job
  list's syncs or cold code.
- **For benches:** a single fight run says little. Read General6's µs a run with the flips, and compare ms a
  flip by workload, which hold still.

## Where the workers' time goes

The fight's samples (`logs/edge-ana/r3s_a.rvas`), by workload:

| | EDGE | General6 | RenJobSet | RenderJobs | audio | w6 | all |
|---|---|---|---|---|---|---|---|
| job code, lifted | 15% | 85% | 44% | 64% | 64% | 51% | 52% |
| the runtime | 40% | 8% | 39% | 22% | 19% | 30% | 26% |
| DMA copies | 10% | 2% | 7% | 8% | 6% | 5% | 7% |
| the job manager, lifted | 12% | 1% | 4% | 0% | 6% | 2% | 5% |
| natives | 15% | 0 | 0 | 0 | 0 | 0 | 4% |

General6 by routine, in `1bdd67b54c3b` unless marked:

| | share | ms a flip | what it is |
|---|---|---|---|
| `0x5C40` | 15.9% | 8.1 | a loop: 48 float ops, 23 shuffles, 13 loads, 8 stores a pass |
| `0x53E0` | 12.7% | 6.5 | a leaf, 45 float ops, a polynomial with constants |
| `0xA588` | 11.1% | 5.7 | 34 float ops, calls `0x53E0` |
| `0x4ED0` | 9.0% | 4.6 | float, already in the speculative forms |
| `0x63B4` (`a67e549fa0ab`) | 8.7% | 4.5 | the noise job's pixel loop |
| `0x9FB8` | 5.9% | 3.0 | **`memset`: a store and two adds a pass** |
| `0x7520` | 5.1% | 2.6 | a loop, 28 float ops, calls `0xA588` |
| `0x4138` | 5.0% | 2.6 | a loop, 32 float ops |

What the module does is still not known. Its output changes every frame, so nothing can be kept.

## What an EDGE job costs now

The fight, a temporary probe from one job's entry to the next (`logs/edge-ana/r3k`):

| class | share of EDGE | µs a job | key to output | output | output to next |
|---|---|---|---|---|---|
| position, hit | 23% | 31.6 | 21.6 | 4.0 | 4.3 |
| lit, hit | 36% | 48.1 | 31.7 | 6.7 | 6.2 |
| position, skinned, hit | 12% | 30.9 | 20.6 | 4.4 | 4.2 |
| lit, skinned, hit | 17% | 44.3 | 29.1 | 6.8 | 6.3 |
| no result yet | 8% | 39-60 | | | |
| declined | 4% | 82 | | | |

K did what it should: a skinned job costs what an unskinned hit costs, and 88% of EDGE's time is hits.

A whole job, label by label (400 traces, `logs/edge-ana/pt_g.txt`, the marks' cost taken off), in µs:

| | position hit | lit hit |
|---|---|---|
| the job's own code | 14.5 | 24.0 |
| the output stage | 3.2 | 4.8 |
| **the job manager** | **15.6** | **15.6** |

- The second review's 12.5 µs step is the job manager, as it said, but not its call: `0x17E0` itself takes
  2.8-3.5 µs. The rest is its interrupt handler, at the interrupt checks after it.
- 357 labels a job. Tag waits `0x2370` 4.7 µs (the transfers happen there), `0x2EA0` 1.0, `0x2540` 0.7,
  `0x35E0` 0.9.
- **`0xA60` and `0xAAC`: 40 passes each, 1.6 µs a job.** Two small store loops on the handler's way in.
- 52,000 jobs a second at 15.6 µs is ~29 ms a flip, 14% of the workers.

## What each way forward can take

Frames from the table above: 0.18% a ms of EDGE, ~0.4% of the others. Estimates have gone both ways
lately (lazy flags +1.4% against 3-5%, G +10% against +2%).

| | what | worker ms a flip | est. frames, fight | work | risk |
|---|---|---|---|---|---|
| M | `memset` at `0x9FB8` native | -3 | +1% | hours | low |
| P | the job manager's two 40-pass loops native | -3 | +0.7% | hours | low |
| G | General6's routines native, at half their time | -12 | +4.5% | 1-2 weeks | low to medium |
| N | the noise job's pixel loop left out when its record repeats | -4 | +1.5% | days | medium |
| R | the runtime's lookups and `spu_overlay_note_get` | -3 to -5 | +1-2% | days | medium |
| S | a stub for hits | -15 to -18 | +3% | weeks | medium to high |
| J | the job manager's load path native | -16 | +3.5% | weeks | medium |

M, P and G together are about +6%: 27.6 flips/s to ~29.3, 25.5 to ~27.

## Recommendation

1. **M and P.** Hours, and they show whether a millisecond is worth what the table says.
2. **G.** First find out what `1bdd67b54c3b` does, then the routines by size. `logs/jobcap6` has General6's
   jobs for the replay. It shortens the chain as well as the workers' time, so it should also ease the
   slower level.
3. **Measure, then choose between S and J.** J also helps RenJobSet and audio, whose milliseconds count more;
   S only EDGE.

## Risks

- **The float forms.** General6's results feed the game: they have to be exact, as EDGE's natives are. The
  replay says so, as it did for those.
- **Flat code has paid badly before.** Natives pay in loops. `0x53E0` and `0xA588` are leaves, called many
  times, so the call around them has to go too.
- **The slower level** is not understood, and could take a gain back on the runs it comes in.
