# What CPU work can still go to the GPU (2026-11-10)

The question: the GPU is idle much of the time. Is there CPU work that could be moved to it, or skipped? Measured with
the player's own settings, after [the RPCS3 comparison](2026-11-10-rpcs3-vs-port-cpu.md). The earlier looks are
[the third GPU review](2026-11-06-gpu-review-3.md) and [the worker review](2026-11-06-worker-time-review.md).

## Setup

- **The player's settings** (`ascension-r.ini`):
  - FSR 3.1 at render scale 1.5, borderless 1920x1080, SMAA, the stats overlay, Disable MLAA and Disable Shadows;
  - frame generation on at 60 Hz, which holds the game at 30;
  - with FSR 3.1 on, Disable Motion Blur is left off, as FSR takes object motion from the blur's draws.
- **Uncapped.** Frame generation and VSync off, so the CPU's limit shows.
- **Runs.** Port `5c43eda`, test saves, a copy of the player's shader cache:

  | run | scene | what was on | flips/s |
  |---|---|---|---|
  | 1 | the fight | motion blur draws on, sampler | 29.2 |
  | 2 | the fight | motion blur draws off (`graphics.frame_generation_objects=off`), sampler | 30.6 |
  | 3 | the gate | sampler, `PERF=1` | 43.7 |
  | 4 | the fight | job-kind and key probes | 29.6 |
  | 5 | the fight | job-time probe | 29.6 |

- **Temporary probes**, all removed:
  - every hit doing nothing, so the fight stays in its first phase;
  - the GPU's time a frame from the overlay's timestamps;
  - a key without the matrix for unskinned EDGE jobs;
  - each EDGE job's time from its entry to the next job's, by class.

## The GPU is half idle

| | the fight | the gate |
|---|---|---|
| GPU time a frame (timestamps) | 12.0-14.7 ms | 9.4-9.8 ms |
| share of the wall | 30-40% | 38-43% |
| nvidia-smi, whole run | 47% busy, 1.64 GHz | 53% busy, 1.61 GHz |

- The clock is high (up to 1.95 of 2.1 GHz), so the low share is not a slow clock.
- In the fight, a frame's GPU time splits into:
  - 4.6-8.5 ms before the draws, the vertex pass among them;
  - 0.3-3.9 ms of draws;
  - 3.7-3.9 ms after them: FSR 3.1, SMAA and the present.

## What bounds the fight: EDGE, on the SPUs

- **The workers are full: 589% of a core.** The SPU time a flip (`[wid-prof]`):

  | | ms a flip |
  |---|---|
  | EDGE | 114.6 |
  | RenderJobs | 29.7 |
  | audio | 22.8 |
  | RenJobSet | 22.2 |
  | General6 | 8.6 |
  | MeshProc | 6.7 |

- **EDGE is the chain.** An EDGE list holds SPUs 32.1 ms of the 35.9 ms median frame, with 115.9 SPU-ms of EDGE work
  (`guestscope.py frames`).
- **The main thread is not the limit.** It shows 94% of a core, but half of its samples are waits: `poll_mwaitx` 40%,
  `sys_timer_usleep` 10%.
- **Neither are the RSX threads.** The frame clock is at 59% of a core, ~45% of its samples waits, 19% of them for the
  render thread. The render thread is at 52%, and 43% of its samples wait for the GPU.

At the gate the game runs 43.7 flips/s uncapped, above the cap of 30, with the workers at 488%. With frame generation on,
only the fight falls short of 30.

## EDGE's jobs in the fight

The kinds (`GOWA_EDGE_JOB=1`):
- position only 31%, lit 32%, skinned 10% (K);
- the velocity jobs 17%;
- a layout the reader does not know 10%.

The cost of each class (run 5):

| class | jobs | µs a job | share of EDGE |
|---|---|---|---|
| velocity | 1.54 M | 73.5 | 28.6% |
| unskinned lit, no result | 1.67 M | 49.4 | 20.8% |
| unskinned position, no result | 1.56 M | 32.2 | 12.7% |
| layout not read | 0.86 M | 52.5 | 11.3% |
| unskinned lit, kept result | 1.12 M | 33.7 | 9.6% |
| unskinned position, kept result | 1.11 M | 27.0 | 7.6% |
| skinned, kept result | 0.82 M | 27.3-44.7 | 7.6% |
| skinned, no result | 0.07 M | 54.8-86.1 | 1.2% |

A kept result still costs 27-45 µs: the job manager, the DMA and the output stage run for it.

### The velocity jobs

- **What they are:** output piece `0x75F580` and methods `0x763980`, on the lit meshes' input pieces (`0x758590`,
  `0x75BD10`), with and without skinning. They make the previous frame's positions that the title's velocity draws read
  (`v[7]`, [the FSR 3.1 plan](2026-11-07-fsr31-plan.md)).
- **They are never kept and never go to the GPU pass.** The job reader takes them for "other": only `0x765B70` and
  `0x755B80` are known output pieces.
- **With Disable Motion Blur applied** they all but vanish (3,700 left of 1.45 M). The frame then had 288 fewer EDGE jobs
  and 14.8 ms less EDGE SPU time, and ran 30.6 against 29.2.

### Why unskinned jobs miss

- **The rule.** A result is kept the second time its key is seen. An unskinned job's key includes its matrix (slot 18).
  A skinned one's does not: the matrix goes with the palette, which K applies on the GPU.
- **The probe.** It hashed each unskinned job a second time without slot 18:

  | | looked up | no result | of those, seen before without the matrix |
  |---|---|---|---|
  | unskinned position | 2.70 M | 1.55 M (57%) | 1.46 M (94%) |
  | unskinned lit | 2.83 M | 1.66 M (59%) | 1.57 M (94.5%) |
  | skinned position (K) | 0.42 M | 33 K (8%) | |
  | skinned lit (K) | 0.47 M | 32 K (7%) | |

- **What that means.** Nearly every miss is a mesh seen before with another matrix. The probe cannot tell whether the
  objects moved or the matrix follows the camera; the GPU way needs neither answered.
- **Against the header's numbers.** The memo's header comment measured 91% kept in the fight. This fight, with its
  camera following Kratos, keeps 41-43% of unskinned jobs.

## What can go to the GPU, and what it gives

| | what | EDGE SPU time | size |
|---|---|---|---|
| A | velocity jobs kept, the previous position made in the vertex pass from last frame's palette or matrix | up to ~15% (73.5 -> ~34 µs a job) | medium |
| B | unskinned jobs' matrix out of the key, applied in the vertex pass as a one-matrix palette (K's way) | ~8% (94% of misses become kept: lit 49.4 -> 33.7, position 32.2 -> 27.0 µs) | medium |
| C | the 10% the job reader does not know | up to 11% | unknown: first find out what they are |

- **The GPU's side is small.** It is a few hundred thousand vertex transforms a frame, inside the half the GPU leaves.
- **A and B together** take ~24% of EDGE's SPU time, ~27 ms a frame. The fight would then meet the workers' total,
  ~185 ms a flip over ~5.9 workers or ~31 ms: about 32 flips/s uncapped from 29.2-29.6 (+8-10%), and frame generation's
  30 held. This is an estimate.
- **B helps every setting.** A helps FSR 3.1 and frame generation with moving objects, the player's configuration.

## Not for the GPU

| work | why |
|---|---|
| RenderJobs, RenJobSet, MeshProc | integer and branchy code that builds the command stream |
| General6 | joints and constraints the game reads back within the frame |
| audio | small blocks that need low latency |
| the main thread | game logic, a flat profile |
| the SPU runtime | a third of the workers' time: the emulation itself, which only the CPU can make cheaper |

## Not the limit now, but next

**The render thread drains the GPU before each batch.**
- `rf_open` waits for all earlier GPU work unless it is done. Its comment dates from bring-up: "a few draws/frame, so
  full serialisation costs little".
- The buffers it protects (vertexes, constants, texture staging) are already double, one half a frame.
- So it could wait for the batch that used the same half only. Recording and the GPU's work would then overlap.
- It is not the fight's limit while EDGE is. In the gate run it waited 7-26 ms every 20 frames, 0.4-1.3 ms a frame, all
  at line 13349.

## How much to trust it

- **Runs.** One run for each setting. Runs 1, 4 and 5 are the same setting: 29.2, 29.6 and 29.6.
- **Instrumentation.** The sampler, the job counters and the probes cost time; the shares hold across the runs.
- **The estimates.** The per-class costs include the job manager between jobs, measured as on 2026-11-06. What A and
  B save assumes a velocity job and an unskinned miss cost what a kept lit or position job costs now.
