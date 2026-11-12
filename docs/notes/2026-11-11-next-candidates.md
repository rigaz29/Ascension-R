# What is left to gain in the fight (2026-11-11)

The question, after [B](2026-11-11-edge-rigid.md) and [A](2026-11-11-edge-velocity.md): what else could raise the
fight's frame rate, by 5% or more a piece if it can be had, and is there more CPU work to move to the GPU? The earlier
looks are [what CPU work can still go to the GPU](2026-11-10-gpu-candidates.md) and
[the worker review](2026-11-06-worker-time-review.md).

The answer in short:
- **Nothing safe gives 5% alone.** Four small EDGE items may give 3-7% together.
- **One large item could pass 5%,** at weeks of work and high risk, and the last estimates of its kind ran high.
- **One item moves work to the GPU.** The others make the CPU's work smaller.
- **More worker threads make the fight slower** on this laptop.

The fight runs 29-31.5 flips/s uncapped. Frame generation at 60 Hz holds the game at 30, so each percent here is
headroom for that 30.

## Setup

- **The player's settings** (`ascension-r.ini`):
  - FSR 3.1 at render scale 1.5, borderless 1920x1080, SMAA, the stats overlay, Disable MLAA and Disable Shadows;
  - moving objects for FSR on (`graphics.frame_generation_objects=on`), so the motion blur draws and the velocity
    jobs run.
- **Uncapped.** Frame generation and VSync off, so the CPU's limit shows.
- **The fight.** Megaera, seconds 70-130 of a run, with infinite health (`cheats.damage_taken=0`).
- **The build.** Port `e7b9ee6`, runtime `7c8da01d`, on the Ryzen 5 4600H laptop. Nothing was built for this look: the
  runs differ by their environment alone. Test saves, a copy of the player's shader cache, `GOWA_THREADS=10`.
- **Counters** in every run: `SPURS_WID_PROF=10`, `GOWA_EDGE_JOB=1`, `GOWA_EDGE_NATIVE_STATS=1`.
- **Runs,** in this order:

  | run | started | what | flips/s |
  |---|---|---|---|
  | 1 | 11:41 | the sampler (`PS3_SAMPLE=1`) | 26.14 |
  | 2 | 11:46 | a guestscope capture, 60 s | 29.25 |
  | 3 | 12:06 | base | 31.48 |
  | 4 | 12:09 | two lanes for RenderJobs (`SPURS_LANE=7:5,12:4,12:5`) | 29.22 |
  | 5 | 12:12 | the same | 29.10 |
  | 6 | 12:15 | base | 30.12 |

## Where a frame goes

Run 2: 1,729 frames in 60 s, the median 35.3 ms, the mean 34.7. The SPU time of a frame, the mean:

| workload | SPU-ms a frame |
|---|---|
| EDGE, three lists | 102.8 |
| RenderJobs | 32.5 |
| RenJobSet | 23.2 |
| audio | 19.0 |
| General6 | 9.6 |
| MeshProc | 6.1 |
| the rest | 9.0 |
| free | 19.2 |

- **Audio has a host worker of its own.** The six others carry 183 SPU-ms a frame. Packed with no gap that is 30.5 ms
  against 34.7: +14% is the most any scheduling could give, and no order of work comes near it.
- **EDGE is still the chain.** It may use SPUs 0-3 only. Of a frame, four SPUs are in EDGE for 21.8 ms, three for 3.3,
  two for 2.1, one for 1.4 and none for 6.1.
  - Of the 6.1 ms with none, 2.5 have no list ready.
  - The other 3.6 have a list ready. Over all six SPUs, 1.76 are then in RenderJobs, 0.83 in RenJobSet, 0.55 in
    audio, 0.36 in General6, 0.24 in MeshProc, 1.27 in the rest, and 1.38 are free. This look did not split them by
    SPU, so it cannot say how often one of SPUs 0-3 was the free one.
  - A list holds an SPU 30.0 ms of the median frame. EDGE's last SPU ends 35.1 ms after the publish.
- **The main thread is not the limit.** Run 1: 38.3 ms a flip, 44.9% of it in `poll_mwaitx` and 14.4% in
  `sys_timer_usleep`.

## The chain within a frame

The 876 frames within 15% of the median, by the millisecond after the RenderJobs publish:

| ms after the publish | SPUs held |
|---|---|
| 0-13 | RenderJobs 1.8-2.7, EDGE 3.0-3.75 |
| 14-21 | RenderJobs ends. General6 peaks at 1.28 and MeshProc at 0.95, at ms 17-18. EDGE dips to 2.2-2.4. Up to 0.97 free |
| 22-37 | RenJobSet up to 2.62 (ms 29), EDGE 2.6-3.5 |

- **Priorities,** a lower number first: EDGE0 3, RenJobSet3 7, EDGE1, EDGE2 and RenJobSet0-2 8, RenderJobs and
  MeshProc 12, General6 13.
- **Ready with every SPU taken,** ms a frame: MeshProc 10.8, General6 7.9, EDGE2 4.8, EDGE1 4.7, RenJobSet2 3.3,
  EDGE0 1.4.
- **RenderJobs** takes 13.2 ms from its publish to its last job, on 2.5 SPUs. MeshProc's last job is taken 33.1 ms
  after the publish, General6's 20.2.

## EDGE's time by group

Run 1, the workers' samples while in EDGE: 99,586, or 115.9 ms a flip. That run was the slowest, so its milliseconds are
larger than run 2's. The shares are what counts.

| group | share | ms a flip |
|---|---|---|
| lifted: the job module | 14.10% | 16.35 |
| lifted: the job manager | 11.52% | 13.35 |
| runtime: dispatch | 9.79% | 11.35 |
| runtime: DMA bookkeeping | 9.46% | 10.96 |
| native: unpacking (`decode_304a0` 5.15, `v9_decode` 1.91, `decode_30310` 1.00) | 8.51% | 9.86 |
| runtime: coherency and atomics | 6.37% | 7.38 |
| the DMA copies themselves | 5.35% | 6.20 |
| native: the memo, the key, the job reader | 3.99% | 4.63 |
| lifted: pieces | 3.85% | 4.46 |
| native: binormals and normals (`gowa_edge_normals_a950` 3.14) | 3.68% | 4.27 |
| runtime: channels | 3.51% | 4.07 |
| native: the output stage | 3.37% | 3.90 |
| other waits | 2.42% | 2.80 |
| the backend's publish | 1.94% | 2.25 |
| runtime: interpreter and interrupts | 1.71% | 1.98 |
| native: SPU lighting | 1.67% | 1.94 |
| native: skinning | 1.62% | 1.88 |
| native: indexes | 1.55% | 1.80 |
| runtime: SPURS | 1.26% | 1.46 |

- **The runtime under the jobs is 32%,** ~37 ms a flip, and the copies 5% more.
- **The job manager and the job module are 26%.** A kept job runs both: the module's driver still walks its stages.
- **The native steps are 24%.** Unpacking is the largest, and a kept job still unpacks.

## The jobs by class

The second half of run 6's counters, the steady state. The counts are today's. The cost of a job is from the job-time
probe of [A's runs](2026-11-11-edge-velocity.md).

| class | of the jobs | a frame | µs a job | SPU-ms a frame |
|---|---|---|---|---|
| lit | 32.5% | 883 | 33.7 | 29.8 |
| position | 31.0% | 843 | 26.6 | 22.4 |
| velocity | 14.8% | 402 | 46.2 | 18.6 |
| layout not read | 7.6% | 207 | 47.9 | 9.9 |
| velocity, skinned | 3.8% | 103 | 95.7 | 9.9 |
| lit, skinned | 5.3% | 144 | 45.0 | 6.5 |
| position, skinned | 4.9% | 133 | 27.8 | 3.7 |

They sum to 100.8 ms, against the 102.8 measured.

- **Kept.** Of 7.54 M jobs looked up in run 6, 6.99 M had a result (93%): 92% of the unskinned ones, 96% of the
  unskinned velocity ones.
- **So the misses are spent.** What is left is what a kept job still costs, 27-46 µs, and the two classes never kept:
  the skinned velocity jobs and the layout the reader does not know.

## The runtime's hot spots

- **Fifteen functions hold 23.8% of EDGE's samples** (run 1): `spu_overlay_note_get` 3.40, `spu_indirect_branch` 2.85,
  `mfc_do_transfer` 2.72, `spu_lifted_lookup` 2.26, `spu_coh_notify_write` 1.87, `spu_lockline_lock_ea` 1.86,
  `spu_drain_call_full` 1.35, `mfc_submit` 1.30, `spu_drain_call` 1.05, `spu_wrch_full` 1.04, `spu_atomic_lean` 0.95,
  `mfc_run_list` 0.82, `spu_rdch` 0.81, `spu_lifted_lookup_full` 0.78, `mfc_for` 0.71.
- **`spu_lifted_lookup`:** 47% of its samples are at one compare, the load of the dispatch cache entry, cold after
  the DMA copies. [The first pass](2026-11-04-spu-runtime-hot-paths.md) found the same.
- **`spu_lockline_lock_ea`:** 65% of its samples are in its spin, after
  [the stripes](2026-11-09-lock-line-stripes.md).
- **What the two passes gave.**
  - The first: the runtime from 26.7% to 24.9% of EDGE's workers, the gate from 18.73 to 19.19 flips/s (+2.5%).
  - The stripes: `spu_lockline_*` from 3.2% to 1.5%, and no change of the frame rate that could be seen.
- **The share has grown since,** as the jobs' own work went to the GPU and the runtime around each job stayed.

## The other workloads

Run 1's samples by workload, with run 2's time:

| workload | SPU-ms a frame | where its samples are |
|---|---|---|
| RenderJobs | 32.5 | lifted code 73.5%: `0x6A08` 13.81% (4.5 ms), `0x12CE8` 5.22, `0xB010` 3.91, `0x12380` 3.75. DMA bookkeeping 7.6%, the copies 7.1% |
| RenJobSet | 23.2 | lifted code 54.1%: `0xB668` 5.61, `0x7A18` 5.28. Coherency 9.62% |
| audio | 19.0 | lifted code 68.4%, flat |
| General6 | 9.6 | `0x4ED0` 16.66%, the natives 36% |
| MeshProc | 6.1 | `0x4BD4` 44.31% (2.7 ms), the copies 12.5% |

All seven workers, 267.8 ms a flip in run 1: lifted job modules 30.8%, idle 18.4%, native EDGE code 11.1%, dispatch
6.7%, DMA bookkeeping 6.5%, the job manager 6.3%, the copies 4.5%, coherency 4.2%, channels 2.3%.

## More worker lanes: slower

- **What a lane is.** A workload named in `SPURS_LANE` gets a host worker of its own, which runs it as the SPU named.
  Audio has one by default (workload 7 as SPU 5), so the base has seven host workers.
- **The test.** RenderJobs (workload 12) on two lanes, as SPUs 4 and 5. That is nine host workers, and SPUs 0-3 are
  left to EDGE for more of the frame.
- **The result.** 29.22 and 29.10 against 31.48 and 30.12 flips/s: both below both, about -5%. The two base runs are
  4.4% apart.
- **Why.** Six cores carry the nine workers, the main thread and the RSX threads.
  [The plan](2026-10-29-performance-plan.md) has the same from earlier tries: on this laptop more waiting threads
  means slower ones.

## What a millisecond is worth

The frame rate gained for 1 SPU-ms less a frame:

| | % |
|---|---|
| in theory, packed over six workers | 0.48 |
| the slope test of 2026-11-06, EDGE | 0.18 |
| the motion blur draws off, 2026-11-10: -14.8 ms, +4.8% | 0.32 |
| A, from runs not ordered against each other: -25 ms, +14% | 0.57 |

- In the slope test the other workloads were worth more: RenderJobs 0.40, General6 0.38, RenJobSet 0.47.
- The estimates below use 0.2-0.5% a millisecond.

## What is left

| | what | SPU-ms a flip | frames, an estimate | work | risk |
|---|---|---|---|---|---|
| D | a velocity job's late binormals left out | 0 to -3.5 | 0 to +1.5% | hours, after a probe | low |
| U | kept unskinned jobs without unpacking | -4 to -7 | +1 to +3.5% | days | medium |
| V | skinned velocity jobs kept, K's way with two palettes | -4 to -6 | +0.8 to +3% | days to a week | medium |
| C | the layout the reader does not know | 0 to -4 | 0 to +2% | unknown, a probe first | unknown |
| R | the runtime's hot spots, a third pass | -3 to -6 | +0.6 to +3% | days | medium |
| W | natives for RenderJobs' and MeshProc's hottest functions | -2 to -6 | +0.5 to +2.5% | weeks | medium |
| S+J | a stub for kept jobs, the job manager's load path native | -19 to -35 | +4 to +10% | weeks | high |
| F | the lifted code built for AVX2 (`-march=x86-64-v3`) | not measured | 0 to +1% | hours | low |

- **D.** `edge_gpul_binorm_now` (spu_edge_gpulight.c) runs the binormals late for a job whose lighting stage did not
  hand over to the GPU. A velocity job has no lighting stage, so nothing should read them.
  - The normals hook is 3.14% of EDGE's samples: 3.12 M calls in run 6, which had 1.53 M velocity jobs.
  - If each velocity job makes one call, leaving them out is about -1.8 ms.
  - The counters by class print only at exit, so a probe run comes first. `GOWA_EDGE_VEL=2` is the check: it compares
    every normal and tangent byte.
- **U.** Unpacking is 8.5% of EDGE's samples, and 93% of jobs are kept. A kept job unpacks only so that a job given up
  can go back to the SPU's stages. The result could hand those arrays back instead.
  - A caution: K's kept skinned position job already leaves its unpacking out, and costs 27.8 µs against the
    unskinned one's 26.6. The gain, if any, is in the lit and velocity jobs' normals and tangents (`decode_304a0`).
- **V.** A skinned velocity job is the dearest class, 95.7 µs, and is never kept: 103 a frame, 9.9 ms.
  - It has the palette twice: slots 6 and 7 this frame's, 8 and 9 the previous frame's.
  - K's way with both would make it a kept job at ~45 µs, the previous position made in the vertex pass.
- **C.** 207 jobs a frame at 47.9 µs. The job reader (`ej_read`) gives a layout up for its base, a slot past the
  end, one of twelve slots empty, slot 0's word, or slot 13's size or first byte. Which one is not counted.
  - If they are a known class with one slot different, they go the kept way. If not, nothing.
- **R.** Two passes have been made. What is left is spread thin: the largest function is 3.4%. A third pass would
  take `spu_overlay_note_get`, the lookup's cold entry and the transfer bookkeeping, each change under what the frame
  rate can show.
- **W.** RenderJobs' `0x6A08`, `0x12CE8`, `0xB010` and `0x12380` and MeshProc's `0x4BD4` are 11.4 ms a frame together.
  Each native is its own reverse engineering and its own check.
- **S+J.** From [the worker review](2026-11-06-worker-time-review.md):
  - S is a stub that runs for a kept job in place of the job module. The rings, `0x17E0`, the draw constants and the
    record must still run.
  - J is the job manager's load path as native code.
  - The review's second look gave them -8 and -11 ms. With 93% of jobs kept now, more.
  - It put both at weeks, and noted that its last estimates had run high by a factor of four.
- **F** is the audit's `-march`, never tried. It would need a build for older CPUs beside it.
- **D, U, V and C together:** -8 to -20 ms, roughly +3 to +7%.

## The GPU question

- **V is the one move left:** two palette transforms a vertex go to the vertex pass. C may be another, if its jobs
  turn out to be a known class.
- **D and U move nothing.** They leave out CPU work that nothing reads, or that the vertex pass already replaces.
- **The rest is not GPU work,** as [the table of 2026-11-10](2026-11-10-gpu-candidates.md) has it:
  - RenderJobs, RenJobSet and MeshProc build the command stream;
  - General6's results are read back within the frame;
  - audio needs low latency;
  - the runtime is the emulation itself.
- **The GPU has the room.** It worked 12-15 ms of a frame on 2026-11-10. The fight's limit is the SPU work around
  each job, which stays on the CPU however the vertexes are made.
- **The render thread's drain** before each batch is still next, and still not the limit.

## The objects setting

`graphics.frame_generation_objects = off` takes the velocity jobs out with the title's motion blur draws. FSR 3.1 and
frame generation then get the camera's motion alone.

- Before A it measured 30.6 against 29.2 flips/s (+4.8%).
- A made those jobs cheaper, so it gives less now. Not measured again.

## Tried or ruled out before

From [the plan](2026-10-29-performance-plan.md), [the audit](2026-10-31-technique-audit.md) and
[the worker review](2026-11-06-worker-time-review.md):

- `preserve_none` on the lifted functions: slower;
- approximate SPU floats (`PORT_SPU_FLOAT_APPROX`): within the noise at the level, and not bit-exact;
- CCX pinning: -1.7% and worse;
- profile-guided builds of the lifted SPU code: no gain;
- LTO: not tried, as the lifted code is 200 large units with little work across them;
- EDGE on SPUs 4 and 5: 35.3 against 35.7 flips/s, the pipeline file from 12 to 54 MB;
- three workers for General6: no setting to change (27.87, 26.26 and 27.46 flips/s);
- a worker watching its slot with MWAITX: 31.80 flips/s without, 30.12 and 29.48 with;
- and now more lanes.

## The order

1. **One probe run:** the late binormals by class, and `ej_read`'s failures by check. Both are counters.
2. **D,** if the probe finds the calls.
3. **U, then V,** each with its check, as B and A had.
4. **C,** if the probe names a class.
5. **Before S+J, a slope probe:** a calibrated delay on every kept job, in ABBA order. It gives what an EDGE
   millisecond is worth now, and so the most S+J could give, before weeks go into it.

## How much to trust it

- **Runs.** Six, one of them for the sampler and one for the capture. The two base runs are 4.4% apart, and in A's
  pairs the order of the runs moved every class by 12-16%.
- **A gain under ~3% cannot be shown** by a few runs of the fight. D, C and R would be judged by their counters and
  by EDGE's SPU time a flip, not by the frame rate.
- **The costs by class** are from A's probe, not from today. They sum within 2% of the time measured.
- **The sampler** made run 1 the slowest. Its shares are used, its milliseconds only as a guide.
- **The estimates** are ranges. The worker review's ran high by a factor of four, and S+J's range is of that kind.
- **The millisecond's worth** spans 0.18-0.57% by three measurements, the highest from runs with heat in them.

## Step 1: the probe (2026-11-12)

One fight run with temporary counters, taken out again after it: port `fdc6bf7`, the setup above, 31.30 flips/s. The
counts are the last 48.8 s of it, 1,528 frames. The memo found a result for 92.6% of the jobs it looked up.

The jobs a frame, 2,487: position 777, position skinned 119, lit 816, lit skinned 128, velocity 358, velocity skinned 92,
not read 198, other 1.

**D, the late binormals.**

| kind | run late a frame | ms a frame |
|---|---|---|
| velocity | 332.6, every job | 1.43 |
| velocity, skinned | 85.5, every job | 0.45 |
| the others | none (other: 0.9) | 0.01 |

- Every velocity job runs them late, and nothing reads them: 1.88 SPU-ms a frame to leave out. The time is the late
  run's own, the lifted `0xA880` with its native loop.
- A velocity job goes through the normals loop (`0xA950`) a second time, outside the late run: 665 calls a frame for
  333 jobs, 2.38 ms with the late ones, and the skinned 171 calls and 0.77 ms. What that call is for is not looked at
  yet. If nothing reads it either, D comes to about 3.5 ms.

**C, the layouts not read.** All 198 a frame fail one check: slot 13 is 16 bytes, but its first byte is `0x86` or `0x83`
where the reader takes `0x87` alone. The byte is likely EDGE's flags and uniform table count: 7, 6 or 3.

| output piece | input pieces | method piece | first byte | a frame |
|---|---|---|---|---|
| `756DE0` | `75AA50`, `760F30` | `7613D0` | `0x86` | 115.0 |
| `764040` | `75AA50`, `760F30` | `766080` | `0x83` | 39.8 |
| `765B70`, position | `75E180`, `764520` | `7661C0` | `0x86`, `0x83` | 24.8 |
| `762120` | `75AA50` | `764FB0` | `0x86` | 2.8 |

- Only the position output is a known class here: about 25 jobs a frame, ~0.5 ms if they were kept at a position job's
  26.6 µs against the 47.9 of one not read.
- The rest have output pieces the memo does not take (`764040` is not even in the reader's list): new classes, not a
  known one with one slot different.

So D is worth doing, 1.9 ms for certain and 3.5 if the second call goes too. C is small unless its pieces are learned.

## D, tried and not kept (2026-11-12)

The change: the job reader's kind kept for the job (at `0x9A40`), and the hook at `0xA928` leaving both of a velocity
job's calls to `0xA880` out (back at `0x54F8` and `0x5AEC`), none of them run late. `GOWA_EDGE_VEL_BINORM=0` made them as
before.

- **Right.** With `GOWA_EDGE_VEL=2` the check compared 1,198,356 velocity jobs and 381,164,480 vertexes: no position
  word and no normal or tangent byte differed. The normals loop went from about 3.2 million calls a run to 31 thousand.
- **Not faster.** The fight, ABBA and then BAAB, eight runs:

  | | flips/s, the four runs | mean |
  |---|---|---|
  | made (`=0`) | 28.92, 29.80, 30.05, 29.72 | 29.62 |
  | left out | 28.30, 27.39, 27.90, 27.41 | 27.75 |

  Every run without them was below every run with them. Every workload's time a flip rose with it, RenderJobs' as much
  as EDGE's: the frames got longer, not EDGE's work larger. Nothing found explains it. Within a run the 10 s windows
  move between 22 and 33 flips/s with the fight itself.
- **The gate,** ABBA: 45.73, 41.64, 40.93, 38.78 (made, left out, left out, made). Each run held within ±1 flip/s, and
  the four fell with the laptop's heat, by more than D could ever move it.
- **So it was not kept.** It saves ~3.5 SPU-ms a frame by the probe's clock, +0.7-1.8% by the estimate above, which
  runs of either scene cannot show on this laptop. A measurement that turns it on and off within one run could; not
  done.
