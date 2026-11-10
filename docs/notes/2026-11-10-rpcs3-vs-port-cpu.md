# RPCS3 against the port with EDGE on the CPU (2026-11-10)

The question: with none of EDGE's GPU steps, is RPCS3 still the faster one, and if so, what does the port wait for:
EDGE, General6 or something else? Measured as on 2026-10-31 ([the job-level comparison](2026-10-31-rpcs3-vs-port.md)),
from outside both processes.

## Setup

- Port `70645f1` (fork `7c8da01d`), the Prebuilt, with EDGE's GPU steps off: `GOWA_EDGE_GPU=0` (lighting),
  `GOWA_EDGE_NOCULL=0` (culling), `GOWA_EDGE_MEMO=0` (kept results; it also turns off the resident geometry and the
  skinning). The natives stay on.
- RPCS3 0.0.43-20572, a copy of the player's (Vulkan, 720p, PPU and SPU on LLVM, VSync off), with its Disable MLAA
  patch.
- The same picture on both: 720p in a 1280x720 window, Disable MLAA alone, no SMAA, no frame generation.
- The Megaera fight, from Continue Game of Profile 1 on fresh test saves, driven as `gowa.py bench` drives it.
  - Every hit did nothing, so the fight stayed in its first phase on both. RPCS3 used its test patch at 0x87460. The
    port used a temporary probe at the head of `func_00087440`, removed after the runs.
- `guestscope.py capture --log`: 60 s from 66 s after the level's TTY line. It also took the window titles (both show
  their FPS) and each thread's CPU time.
- Runs:
  - RPCS3: 1.
  - The port: 2. The second had the SPU workers' sampler on (`PS3_SAMPLE_THREAD=spurs pm`).

## Results

| | RPCS3 | port, run 1 | port, run 2 (sampler) |
|---|---|---|---|
| FPS, the titles' mean over the 60 s | 18.5 | 21.2 | 23.5 |
| flips/s, the bench's median of 70-130 s | | 21.9 | 23.8 |
| frames in the 60 s (RenderJobs publishes) | 1,111 | 1,270 | 1,416 |
| median frame | 56.1 ms | 48.9 ms | 44.0 ms |
| SPUs in workloads | 5.79 of 6 | 6.07 | 6.07 |

The port's 6.07 includes audio's own worker (`SPURS_LANE=7:5`).

SPU time a frame by workload (`report`, lane-ms a frame, RPCS3 against port run 1):

| | RPCS3 | port |
|---|---|---|
| EDGE, three lists | 151.7 | 168.7 |
| RenderJobs | 48.1 | 36.5 |
| audio | 35.7 | 27.1 |
| General6 | 34.3 | 9.9 |
| RenJobSet0-3 | 29.3 | 29.1 |
| MeshProc | 8.3 | 6.6 |
| the rest | 5.2 | 9.0 |
| all | 312.6 | 286.9 |

Threads, as a share of a core:

| | RPCS3 | port |
|---|---|---|
| SPU threads / workers | 6 at 98-99% | 56-97% each, plus audio's |
| render thread | `rsx::thread` 94% | `frame clock` 52%, `rsx render` 42% |
| PPU main | 42% | 93-94%, poll waits with MWAITX counted as CPU |
| all threads | 7.56 cores | 8.17 and 8.33 cores |

## What bounds each

Both keep the SPUs in workloads nearly all the time, 5.79 of 6 and 6.07. So a frame lasts as long as its SPU work
takes on six SPUs: 312.6 SPU-ms a frame in RPCS3, 286.9 in the port.

The median frame's parts (`frames`):

| | RPCS3 | port 1 | port 2 |
|---|---|---|---|
| an EDGE list has an SPU | 40.4 ms | 46.4 | 42.0 |
| EDGE jobs | 3,017 | 3,035 | 3,015 |
| EDGE's SPU time | 155.7 ms | 174.9 | 157.9 |
| RenderJobs, publish to last job taken | 31.7 ms, 1.5 SPUs | 18.8, 2.0 | 17.1, 2.0 |
| MeshProc ready with no SPU | 1.4 ms | 18.4 | 16.7 |
| General6 ready with no SPU | 36.2 ms | 14.9 | 13.2 |
| General6's last job, after the publish | 54.7 ms | 33.1 | 28.7 |

- **The port is bound by EDGE.**
  - An EDGE list holds SPUs for 95% of the frame, at 3.77 SPUs on average.
  - EDGE may run on SPUs 0-3 only: the game gives it priority 0 on SPUs 4-5. Its job manager asks for four SPUs (ready
    count 4), and the kernel never gives it a fifth.
  - So the frame is about EDGE's SPU time over 3.8: 174.9 / 3.77 = 46.4 ms.
- **RPCS3 is bound by everything else.**
  - It has 15.7 ms a frame with no EDGE list running.
  - Its RenderJobs, General6 and audio take 45 SPU-ms a frame more than the port's: 11.6, 24.4 and 8.6. General6
    waits 36 ms a frame for an SPU.
  - Its EDGE alone would allow ~39 ms (155.7 / 4).
- **Nothing in the port is blocked.**
  - No workload holds jobs it is not ready for, beyond a millisecond a frame (`states`). RsxSync is the exception:
    it waits for the RSX by design.
  - MeshProc's and General6's waits are queues behind EDGE and RenderJobs. The game's priorities put those first
    (a lower number is stronger): EDGE 3 or 8, RenderJobs 10-12, MeshProc 10-12, General6 11-13. EDGE runs one episode
    after another meanwhile, so the waits cost nothing.
- **An EDGE list shows no jobs pending while it works.** Its published and taken counts move together: jobs are taken
  as they are published.

## An EDGE job

- **The whole job:**
  - RPCS3: 51.6 µs of SPU time (155.7 ms / 3,017).
  - The port: 57.6 and 52.4 µs in its two runs.
- **Per SPU** (`edge`):
  - with four SPUs in the list: RPCS3 55.1 µs, the port 62.5 and 57.2;
  - with one: 29.0 against 35.5 and 36.8;
  - both nearly double from one SPU to four.

Where the port's job goes, from 105,366 samples of the EDGE workers (run 2):

| | share | µs a job |
|---|---|---|
| native stages | 39.2% | 22.6 |
| lifted job code and pieces | 16.3% | 9.4 |
| the job manager, lifted | 6.8% | 3.9 |
| the runtime | 32.6% | 18.8 |
| the output native | 2.3% | 1.3 |

The runtime's part, as shares of EDGE's time:
- DMA bookkeeping 7.1%: `mfc_do_transfer` 2.4, `spu_overlay_note_get` 2.3, `mfc_submit` 1.0;
- the copies 6.4%;
- dispatch 6.3%: `spu_indirect_branch` 1.9, `spu_lifted_lookup` 1.6;
- coherency and atomics 4.8%: `spu_coh_notify_write` 1.4, `spu_lockline_lock_ea` 1.0;
- waits outside the executable 2.9%, among them 1.3% for the SPURS kernel's lock in `spurs_kernel_poll`, which every
  poll takes exclusively;
- channels 2.3%;
- the rest 2.8%.

The largest single functions:
- the lighting loop `0x79B0`, 7.3%, which EDGE on the GPU skips;
- normals 4.2%, the decoder 4.1%, culling 4.0%, the transform 3.2%, indices 3.1%.

## Why EDGE cannot simply have more workers

- **SPUs 4 and 5.** A job takes its output space from its own SPU's ring line, `0x73254300 + 0x80 (spu + 1)`, with
  GETLLAR and PUTLLUC ([the output contract](2026-11-04-edge-output-contract.md)). The game sets up lines for SPUs 0-3
  only, so a worker as SPU 4 or 5 would take space from a line that was never set up.
- **A second worker as one of SPUs 0-3.** It would race the first on that SPU's pointer: PUTLLUC writes it
  unconditionally.
- **The lanes.** A `SPURS_LANE` worker runs a workload in place of the SPU's own worker, not beside it, so EDGE stays at
  four.
- **More workers.** They would need rings of the port's own for SPUs 4-5, and host cores to spare. The process already
  uses 8.2-8.3 of the 12 logical processors.

## What follows

- **The answer to the question.**
  - In these runs the port without EDGE on the GPU is ahead: 21.2 and 23.5 against 18.5.
  - On 2026-11-09 RPCS3 ran 22.4, 17.5 and 19.6, so its best run passes the port's slower one. Usually ahead, not
    always.
  - With EDGE on the GPU (the default) the same fight was 25.5 and 26.8 on 2026-11-09.
- **Where the CPU path gains.**
  - With the frame bound by EDGE on four SPUs, a microsecond off an EDGE job is ~1.7% of the frame: 3,030 jobs over 3.8
    SPUs is 0.8 ms of a 44-49 ms frame.
  - The runtime's ~19 µs a job is the biggest part. The pieces above are 1-4 µs each.
  - EDGE on the GPU takes whole stages off these four SPUs, and that is what makes the default faster.
- **What does not.** General6, RenderJobs and audio: in the port they run beside EDGE, on SPUs 4-5 and audio's worker.

## How much to trust it

- **Runs.** One RPCS3 run and two port runs, one of them with the sampler (~5% slower).
- **Spread.** A run's median moves by 20% or more in this fight ([2026-11-09](2026-11-09-rpcs3-comparison.md)).
- **What holds.** The breakdown is the same in both port runs: SPU time a frame, EDGE's share of the frame, the waits.
- **Not measured.** How much time the RPCS3 SPU threads spend spinning inside a workload: they run at 98% whether or not
  they have work.

## How to repeat it

```
py scripts/guestscope.py capture --proc rpcs3.exe --log <rpcs3>/log/TTY.log --delay 66 --seconds 60 --out rp.json
py scripts/guestscope.py capture --proc Ascension-R.exe --log <bench>/<tag>1.log --delay 66 --seconds 60 --out port.json
py scripts/guestscope.py report rp.json port.json
py scripts/guestscope.py jobs rp.json port.json
py scripts/guestscope.py edge rp.json port.json
py scripts/guestscope.py states rp.json port.json
py scripts/guestscope.py frames rp.json port.json
```

- Start the capture before the program: it waits for the process and for the level's line.
- RPCS3's TTY line comes 3-4 s after the load's thread starts, so 66 s after it is about 70 s after the load, where
  the bench's windows start.
