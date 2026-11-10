# Performance plan: RPCS3's speed or better, playable on a mid laptop

Written 2026-10-29, at 12.0-12.5 flips/s on the first level, after a fresh trace and sample of
it. This replaces the order of work in the [performance map](2026-10-28-performance-map.md),
which stays as the record of the 2026-10-28 measurements, and builds on the
[SPU codegen design](2026-10-28-spu-codegen-design.md). The rule is unchanged: a lever is kept
only if an A/B on the level shows more flips/s (or it fixes something), and anything that
changes what the game shows is an option, never a silent default.

## 1. The goal

- **Machine:** Ryzen 5 4600H (Zen 2, 6 cores / 12 threads in two 3-core CCXs, 4 MB L3 each),
  GeForce GTX 1650 Ti 4 GB, 15.4 GB RAM. A mid laptop.
- **Done means**, on this machine, on the charger, at 1280x720:
  - the first level (the Megaera fight) at 30 flips/s or more, median;
  - no frame over 50 ms in a 60 s window outside loading;
  - audio without gaps;
  - the same in the later chapters.
- **RPCS3 on this machine**, with the user's settings (PPU and SPU LLVM, XFloat Approximate, SPU
  block size Safe, 6 SPURS threads, Vulkan at 100%, Write Color Buffers on, Multithreaded RSX
  off):
  - **the first level's Megaera fight, the scene the port is measured on: 22.9 FPS median**
    (10 s windows 19.5-24.2; step 0.3, 2026-10-29);
  - the story cutscene before it: ~36 FPS; Chapter 3's Guardhouse: 33-40 FPS (2026-10-28).
  - Its threads in the fight: the six SPU threads at ~92% of a core each (they busy-wait),
    `rsx::thread` 87%, the PPU main thread 73%; 7.4 logical CPUs in all. GPU 3D ~18%.
- **Now:** a frame takes 80.5 ms (median), 12.3 flips/s.
  - RPCS3's speed in the same scene means ~23 flips/s: 1.9 times faster, a 43 ms frame.
  - 30 flips/s means 33 ms, 2.4 times faster. On this laptop that is beyond RPCS3.
- **Measured again 2026-10-31**, both the same afternoon, on the charger, at 1280x720, one test
  at a time. RPCS3 is the same build (0.0.43-20415) with the same settings; the port is at
  fork `8dcd4671`, port `5ce3649`.
  - RPCS3: New Game, Normal, in the measurement profile of 2026-10-29. The fight started 132 s
    after Normal.
    - From 20 to 145 s into the fight (the port bench's windows): **24.0 FPS median** (10 s
      windows 22.8-25.2).
    - From 5 to 165 s (step 0.3's span): 23.9, against 22.9 on 2026-10-29.
    - 7.41 logical CPUs: the six SPU threads at 96-98% each, `rsx::thread` 87%, the PPU main
      thread 50%. GPU 3D 15-19%, at P0 / 1,050 MHz.
  - The port: `gowa.py bench`, two runs: 16.06 and 16.47, **16.27 median** over both runs' 10 s
    windows. 6.96 logical CPUs. GPU 3D 27%, at P5 / 465 MHz.
  - **The port is at 68% of RPCS3's speed** (a 61.5 ms frame against 41.7), up from 54% on
    2026-10-29. RPCS3 is 1.47 times faster.
  - **The same settings, with one difference.** The user's RPCS3 patch config has "Disable MLAA"
    on for this title. The file dates from 2026-10-28 22:43 UTC, so the patch was on for
    2026-10-29's 22.9 as well. The port runs the title's MLAA tasks; their work is wasted
    (section 2.6), and plan 1.6 takes it out.

## 2. Where a frame goes now

Measured 2026-10-29 on the first level (fork `5c906fb2`, port `fe46ad6`), from 10 s into it:
- a 12 s `PS3_FTRACE` (146 frames) with `GOWA_WWS_TRACE` line events and `SPURS_WID_PROF`;
- the GPU's engine counters and per-thread CPU over 20 s;
- a 100 s `PS3_SAMPLE_THREAD` profile of the main thread, the six SPU workers, the task threads
  and the frame clock thread (1 ms; the sampler costs ~15% of the flips).

### 2.1 The frame is a chain of SPU job lists

A typical frame (`ft_frame.py`, 2 ms columns):

| ms | what runs |
|---|---|
| 0-26 | RenderJobs (w12) on one SPU, ~40 jobs of ~0.8 ms that the main thread publishes at the frame's start; Batch (w6) beside it |
| 26-32 | MeshProc (w13) |
| 32-80 | EDGE (w14-16) on three SPUs, ~1,600 jobs of ~0.11 ms a frame; General6 (w2) on one SPU from ~28 ms to the end, ~32 jobs of 1.5 ms; RenJobSet (w8-11) from ~50 ms |
| always | audio (w7) |
| RSX | drains in bursts at 2-6 and 34-58 ms; reaches the flip at 62-70 ms, before the chain ends |

So **frame ≈ RenderJobs + MeshProc + max(EDGE on three lanes, General6) ≈ 26 + 5 + 50 ms**. The
lists are serial by design: widening them with `SPURS_MAXCONT` gave nothing (2026-10-28).

SPU time a frame, all lanes: EDGE 171 ms, audio 62, General6 52, RenJobSet 42, RenderJobs 34,
Batch 18, MeshProc 5, others 5. **389 ms on six workers**, 4.7 cores busy.

Per run (`SPURS_WID_PROF`, 30 s):

| workload | us/run | runs/s |
|---|---|---|
| General6 w2 | 1,518 | 389 |
| Batch w6 | 963 | 219 |
| RenJobSet w11 | 812 | 175 |
| RenderJobs w12 | 790 | 525 |
| MeshProc w13 | 418 | 155 |
| audio w7 | 223 | 3,460 |
| EDGE w14-16 | 110 | ~6,400 per list |

### 2.2 Inside an SPU run

Share of each workload's samples:

| | RenderJobs w12 | General6 w2 | EDGE w15 |
|---|---|---|---|
| lifted SPU code | 69% | 95% | 68% |
| runtime: dispatch (`spu_indirect_branch`, `spu_lifted_lookup*` with a lock wait, `spu_lookup`, `spu_drain_call`) | ~12% | ~1% | ~10% |
| runtime: DMA and channels (the copies, `mfc_*`, `spu_wrch`/`rdch`, `spu_overlay_note_get`) | ~13% | ~2% | ~12% |
| coherency (`spu_lockline_lock`, `spu_coh_notify_write`) | ~2% | 0.6% | ~6% |

- **General6 is float code.** Three functions are 63% of it: `4F00` 41% (one straight-line
  block of ~450 SPU instructions), `4AB0` 13% and `63B4` 9%.
- **EDGE:** `EC08` 8%, `A950` and `7BB4` 4% each, the PIC fragments ~9%. The other third of a
  run is the runtime.
- **RenderJobs:** small functions, no float. Its cost is the transfers between them and the
  runtime (`SPU_DIRECT` 13% and function entries 9% of its lifted samples in the design note).
- Over all six workers: 53% lifted code, ~26% runtime, ~2% interpreter, 19% waiting for work.
  - So **the runtime is about 30% of the time the workers are busy.**

### 2.3 The main thread

- 97% of a core. About 65% of its samples poll for SPU progress or wait:
  - **the job-graph scan:** `func_0044CA48`, `0044CD20` and `0044D120`, 28%. By the 2026-10-28
    line profile, three quarters of it reads entries that are not ready.
  - **the progress-counter loops:** `func_0049D688`, `0049B35C`, `0049CB6C`, `0049B2FC` and
    `0049B76C`, 32%.
  - the coherency notifies of the stores in those loops, plus `usleep`.
- **Its own work is ~35% of its samples, ~25-28 ms a frame** (L8 found ~25 ms). It does not set
  the frame now, but at 33 ms it would be the next limit.
- The VMX float helpers run lane by lane, out of line: `vmx_vmaddfp` alone is 4.2% of the
  thread.

### 2.4 The frame clock thread (FIFO walker and D3D12 backend)

- **CPU:** 45% of a core, ~36 ms of CPU a frame.
  - SRV descriptor writes ~14 ms a frame: ~2,000 draws, 16 units each, one
    `CreateShaderResourceView` per unit.
  - Textures ~3.5 ms, PSO lookups ~2.5 ms.
  - The vertex upload (`upload_tris_vp_raw`) ~7% of the thread, `rsx_process_method` 6%.
- **Waiting for the GPU: ~28% of its samples, ~20-25 ms a frame.**
  - `rf_open` drains the GPU completely at the start of every batch, so the upload buffers are
    not overwritten while in use.
  - It was written for test scenes of a few draws ("full serialisation costs little").
  - While it waits, the walker does not walk: reports, labels and the parks the SPU jobs release
    wait with it.
- **Idle** (`pump_messages`): 20%.

### 2.5 The GPU

- The 3D engine is 25% busy on average (36% at most), at **645 MHz of 2,100** (P5).
- It is idle most of the frame and clocked down: there is room for work taken off the CPU.

### 2.6 The task threads and MLAA

- The six `spu job` threads cost 0.2-0.5 of a core (the thread table says 47%).
- 91% of their samples wait for a signal. The MLAA and particle tasks' own code is small.
- **The MLAA work is wasted here.**
  - The title copies each frame to main memory (in 1024- and 256-pixel-wide pieces), its five
    SPU tasks anti-alias the copy, and later draws sample it.
  - In this port the frame exists only on the GPU. The backend's blit snapshots hand those draws
    a GPU copy (`rsx_d3d12_backend.c`, "Blit snapshots").
  - So the tasks process stale bytes, and nothing shows their output.
  - They still sit in the signalling path: an RsxSync job signals them, and they write the RSX
    label and decrement the counter that kicks an EDGE list.

### 2.7 Update, 2026-10-30 evening: what bounds the frame at 14.4 flips/s

General6 is off the critical path. Its jobs became 10% shorter (2.1) and the level did not
move. In a trace of the build before that change, the next RenderJobs list was published 8.6 ms
before General6 ended.

- **The main thread's cycle** (`PS3_FTRACE`, median times from its RenderJobs publish):
  - 0 ms: it publishes RenderJobs, about 39 jobs in one burst.
  - 3.5 ms: the flip request. 4.7 ms: MeshProc published.
  - About 33 ms: RenderJobs' last run ends. About 37 ms: MeshProc's ends.
  - About 70 ms: the next RenderJobs burst. RenderJobs idles, its list empty, 58% of the time.
- **Where the main thread waits.** The poll loops' waits are now spans in the trace
  (`poll_wait.cpp`; `gowa.py frames` lists them as usleep@<loop head>). The main thread runs
  28 ms a frame and waits 39 ms:

  | Loop | ms a frame | When, ms after the RenderJobs publish |
  |---|---|---|
  | `49B35C` | 12.0 | 0-20 |
  | `44CD20` | 10.8 | 40-80 |
  | `44D120` | 10.7 | 30-60 |
  | `49D688` | 5.6 | 20-40 |

  - The later waits end while EDGE and RenJobSet still run; a commit to an EDGE list falls
    within 0.1 ms before most of them, but EDGE commits every ~40 us, so the timing alone does
    not prove what they wait for.
- **Which workloads bound the frame** (fork `9d16caba`, `SPURS_WID_SLOW=<wid>:<percent>`: each run
  of the workload made that much longer). One run each, every workload +25%, base 14.55
  flips/s, noise about +-2%:

  | Slowed | flips/s | Change |
  |---|---|---|
  | RenJobSet w8-w11 | 13.69 | -5.9% |
  | EDGE w14-w16 | 13.75 | -5.5% |
  | RenderJobs w12 | 14.01 | -3.7% |
  | audio w7 | 14.43 | -0.8% |
  | Batch w6 | 14.50 | -0.3% |
  | MeshProc w13 | 14.69 | +1.0% |
  | General6 w2 | 14.71 | +1.1% |

  The first three are the critical path; the other four have slack.
- **In the game they run far longer than alone.** Per run in the game (`SPURS_WID_PROF`)
  against per job on the replay: RenJobSet w8 95 vs 20 us, w10 116 vs 30, w11 667 vs 440;
  EDGE 92 vs 58; RenderJobs 652 vs 535. Their in-game profiles (sampled):

  | Share of the run | RenderJobs | EDGE w14 | RenJobSet w8 | w10 | w11 |
  |---|---|---|---|---|---|
  | lifted code | 69.5% | 64.5% | 40.0% | 55.2% | 74.8% |
  | lock-line lock and coherency | 2.5% | 7.0% | 16.8% | 11.2% | 2.7% |
  | dispatch (`spu_indirect_branch`, lookups) | 7.5% | 5.6% | 13.3% | 5.7% | 4.0% |
  | DMA (`mfc_*` and its memcpy) | ~9% | ~8% | ~10% | ~10% | ~3% |
  | `spu_drain_call` | 3.2% | 2.3% | 3.2% | 2.5% | 5.3% |
  | interpreter | 0.6% | 0.2% | 4.6% | 0.1% | 0.9% |
  - Every SPU atomic commit and every write to a reserved line takes one global lock
    (`spu_lockline_lock`). Under it, it walks every SPU context and wakes the ones that lose
    their reservation (`WakeAllConditionVariable`).
  - w11 spends 6.6% in the exact float forms' per-lane redo (`fma_redo`, `fm_redo`): its
    results often leave the single range.
- So next: the critical workloads' runtime share. That means the lock-line lock (shorter
  critical sections, or a lock per line), dispatch (2.2) and RenJobSet w8's interpreted code.
  Then their lifted code: EDGE is 2.1 cores of SPU time a frame, 64% of it lifted code.
- **The lock-line lock. Done 2026-10-30** (fork `54bdd0eb`).
  - What held it: `SPU_COH_STATS=1` showed every notify (an atomic commit, or a write to a
    reserved line) walking ~99 listed contexts. That is ~400,000 notifies a second, all under
    the lock, to find 0.13 contexts on average holding the written line. The hottest lines
    were the PPU's GCM context (0x009B5100), 0x7001AD00, the MLAA barrier and EDGE's lists.
  - Now each listed context has a slot in an array that holds the line it reserved last. The
    notifiers scan the array and ask only the contexts whose line matches.
  - A context stores its line before it takes a reservation: under the lock on the slow
    GETLLAR path, and before the fast path's fence. That is the store-buffering argument that
    already covers `resv_valid`, so the array never misses a reservation.
  - The slot sits in `spu_context`'s tail padding (12 bytes on Windows), so job captures keep
    their size.
  - Level, three runs each: 14.50 -> 15.49 flips/s (+6.8%); the runs do not overlap.
- **Insertion controls and stack cookies. Done 2026-10-30** (fork `86e29fdc`, port `CMakeLists.txt`).
  - A profile by lifted line (`--spu-lines`) of the critical workloads' lifted code:

    | Share of the lifted code | RenderJobs | EDGE |
    |---|---|---|
    | crossing between lifted functions (`SPU_DIRECT`, entries, `spu_link`) | 24.5% | 13.4% |
    | LS reads and writes | 22% | 15% |
    | `shufb` | 9% | 13% |
    | `cwx` | 7.7% | - |
    | exact float forms | - | ~14% |
    | int-float conversions | - | 6.6% |
  - `cbd`/`chd`/`cwd`/`cdd` and their `x` forms built the control in memory and read it back
    as one 16-byte value, which missed store-to-load forwarding. On x86 each is now a compare
    and a blend.
  - The lifted SPU code is built with `/GS-`. A function with a large frame set and checked a
    stack cookie at every entry and exit, and every transfer between lifted functions is both.
  - Replay: RenderJobs 552 -> 469 us a job (-15%), RenJobSet w10 34 -> 29 (-15%), w9 -10%.
    EDGE and General6 are unchanged; outputs identical.
  - Level, three runs each: 15.28 -> 15.68 flips/s (+2.6%); the runs do not overlap.
  - Next: the crossing itself, a Windows x64 frame with xmm6-15 saved at each entry. That
    means `--merge-chunks` measured again, then `preserve_none`.
- **`--merge-chunks` measured again, 2026-10-30: not kept.** Every job module but General6's was
  merged; in General6's, merging takes `4F00` its speculative body, and that job was 13%
  slower.
  - Replay: RenderJobs 473 -> 447 us a job (-5.5%), EDGE -3%, RenJobSet -1% to -4%.
  - Level, three runs each: 15.78 against 15.72 flips/s, no difference.
  - Cost: 13 MB more lifted C (28 -> 41 MB). The same answer as on 2026-10-28.

### 2.8 Update, 2026-10-31: what bounds the frame at ~16 flips/s

- **Not SPU throughput.** The six workers are 4.65 cores busy on average, and 21% of their
  samples are idle waits. Audio (w7) made twice as long (`SPURS_WID_SLOW=7:100`, +0.3 core of
  spinning): 16.47 against 16.43 flips/s.
- **The workloads, +25% each** (`SPURS_WID_SLOW`), one run each. Base 15.90 in the same session,
  16.43 earlier the same day:

  | Slowed | flips/s |
  |---|---|
  | General6 w2 | 15.89 |
  | RenderJobs w12 | 15.60 |
  | RenJobSet w8-w11 | 15.38 |
  | EDGE w14-w16 | 14.78 |

  General6 made twice as long gives 14.00. Its one lane is busy 59% of the time, so doubling it
  fills the lane.
- **How the main thread feeds General6.** `GOWA_WWS_TRACE` now watches the lists of workloads
  2-17; General6's is `70B14EF0`.
  - Each cycle the main thread publishes 25 jobs in three bursts, timed from the RenderJobs
    publish: +5 at ~19 ms, +5 at ~21 ms, +15 at ~29 ms.
  - The last burst comes ~3 ms after RenderJobs' last run. The 16 jobs left then drain in ~28 ms.
  - In every cycle, the main thread's last wait before the next RenderJobs publish (`49D688`)
    ends 0.08 ms after a General6 run. Even so, the probe above says General6 has slack: the
    cycle does not wait on it alone.
- **EDGE runs one list at a time.** The three EDGE workloads never run together: one has runs
  open 84.6% of the time, none 15.4%. Each list runs up to three jobs at once.
  - `SPURS_MAXCONT=14=6,15=6,16=6`: the same 8,700 runs a second per list, 16.25 flips/s. Three
    at once is what the lists offer (three job chains each), not the cap.
  - Episodes: ~30 a second (two a frame), median 31 ms long. Between two of them is a median
    3.9 ms with no EDGE run, 15% of the time.
  - In 63% of those gaps a MeshProc run ended within 0.2 ms before the next episode started
    (chance: ~4%). MeshProc starts each EDGE episode.
  - So EDGE uses 2.25 lanes on average, of the three it can use. EDGE jobs take 86 us in the
    game and 57 us on the replay.
- **Speculative float in EDGE's loops: not kept.** The experiment covered 13 single-block loops
  (`EC08`, `A950`, `7BB4`, ...; ~40% of an EDGE run):
  - the SPU registers in locals and the float ops in the speculative forms;
  - a range check before each store, branch and exit;
  - a failed check undoes the current segment and goes on in an exact copy.

  Replay, EDGE w15, 40 jobs, best of 10 runs:

  | Build | us a job |
  |---|---|
  | before | 57.3-58.6 |
  | the loops' registers in locals | 54.1-54.8 (-5.5%) |
  | locals and speculative float | 61.1-61.4 (+5%) |

  - The checks fail often. `D92C` fails in every call, `BD54` and `A950` in most: EDGE's masks
    and clamps leave the single range. The exact forms are not what these loops wait on.
  - A first version forgot to store the registers at the loop's exit. It replayed 35% faster,
    and 24 of 40 jobs came out different, ending early.
  - The native-float numbers in 1.1 (EDGE 61 -> 38 us) had 13 of 40 jobs diverge in the same
    way. They are no bound for what correct float code can gain.
- **Where an EDGE run goes in the game.** Fork `d71918df` adds `SPU_JOB_TIME=<s>` (measurement
  build). It times each job call to its return, the span the replay times. It also tags the
  worker's samples while a job runs (`--wid <wid + 64>`). One run, ~15.4 flips/s:

  | | us a job | us a policy-module run | replay, us a job |
  |---|---|---|---|
  | EDGE w14-w16 | 67 | 91 (1% empty) | 57-61 |
  | RenJobSet w8 | 75 | 91 (10% empty) | - |
  | RenJobSet w10 | 98.5 | 108 (12% empty) | - |
  | RenderJobs w12 | 614 | 579 (11% empty) | ~450 |

  - An EDGE run is ~74% job. The other ~26%, ~24 us, is the policy module's work around it.
  - The job itself runs 10-17% slower than alone. Inside the job, lifted code is 83% of the
    samples.
  - Outside the job, the samples go to:

    | | share |
    |---|---|
    | the `memcpy` under `mfc_do_transfer_body` | 17.6% |
    | `spu_overlay_note_get` (half of it `memcmp`) | 12.3% |
    | the MFC queue (`mfc_for`, `mfc_submit`, `mfc_run_list`, the channel writes) | ~10% |
    | the policy module's own lifted code | ~10% |
    | lock-line atomics | ~9% |
    | kernel services (`spu_indirect_branch_full`) | 8.2% |
- **The policy module loads the EDGE job module before every job.** That is 52 KB from EA
  `0x737580` (16 + 16 + 16 + 4 KB), ~14,000 times a second: 1.5 GB/s of GETs on the EDGE
  workers. Each load is copied, and its code bytes are compared with the lifted image
  (`spu_overlay_get_kept_code`).
  - On the SPU, that DMA runs beside the code. Here it is a `memcpy` and a `memcmp` on the
    worker, ~6-7 us a job.
  - Every GET of 512 bytes or more also compares 16 bytes against each registered job module's
    signature, one `memcmp` call each.
- **The policy module's GETs, 2026-10-31.**
  - Done, fork `586de054`: a GET of 512 bytes or more skips the signature scan unless its first
    8 bytes head a registered signature (a set of them, as the EA set), and compares inline. It
    only removes work and was not measured on its own.
  - Not done: skipping the module's copy and the compare of its unchanged code. Per EDGE job
    that is ~1.9 us of `memcpy` and ~1.5 us of `memcmp` out of 91 us (the GETs outside a job
    are ~113 KB, the module 52 KB of it). Both need page protection, the copy also on every
    context's local store.
- **`EC08` alone, replay** (40 jobs, best of 10): before 57.0-60.6 us a job; its registers in
  locals 56.6; locals with speculative float 57.4. The loop is bound by its instruction count
  (~650 x86 instructions an iteration), not by `ctx` traffic. A gain there needs it rewritten
  (two iterations to an AVX2 register, the shuffles' controls hoisted), estimated ~3 us a job.
- **What starts an EDGE episode: the main thread.**
  - The run that ends a gap is a 30 us MeshProc run at the gap's end, a median 7.4 ms in.
  - MeshProc's list (`0x719E4780`) begins with a type-2 entry on `0x009B504C`. Its 4-6 jobs wait
    behind that entry 34% of the time, and 62% of MeshProc's runs find nothing to do.
  - Probing it more often does not help. A cap on its idle back-off (`SPURS_IDLE_MAX=13=0`,
    experiment, not kept) gave 82,000 empty runs a second, 0.59 of a core, and 14.59 flips/s.
  - Its empty runs read last its list line, the entry, and 16 bytes at `0x009B5040`.
  - `PPU_WWATCH`: only the main thread writes `0x009B504C`, and the SPUs only read it.
    - `0x0041CFBC` and `0x003A1C30` (called from `0x003A2234`) clear it.
    - `0x003E13E0` (from `0x003E1480`) sets it to `0x00010D08`.
  - Over the 179 gaps a MeshProc run ends (12 s), the main thread runs its own code for 62% of
    the gap. In 158 of them, the 0.5 ms before the MeshProc run is its own code too.
  - So the main thread's own work decides when each EDGE episode can start, and that is why
    EDGE bounds the frame while it idles 15% of the time.
- **The main thread's own work.** At HEAD, its own work is ~38% of its samples:

  | | share of the main thread's samples |
  |---|---|
  | lifted PPU code | 23.9% |
  | the VMX float helpers (`vmaddfp` 5.5%) | 6.7% |
  | coherency notifies of its stores | 2.9% |
  | indirect calls | 2.3% |

  - **Done, fork `b5a91584`:** `vaddfp`, `vsubfp`, `vmaddfp` and `vnmsubfp` as vector code on
    FMA3 hosts. Bit-exact against the scalar forms: 12M random ops, both NJ settings, GCC and
    Clang. 41,506 `vmaddfp` call sites in the lift.
  - Level, ABBA, two runs each: 15.68 -> 16.14 flips/s (every window's median), both new runs
    above both old.
  - `PORT_PPU_FAST_FP` (the scalar PPC float ops without their NaN checks, 1.1 MB less code):
    ABBA, two runs each, 15.98 against 16.09 flips/s (every window's median); the frames look
    the same. No gain; it stays off.
  - The coherency of its stores, 3.7% of its samples (the notify 1.7%, waiting for the lock-line
    lock 1.0%, `ppu_resv_break_line` 0.8%), is not changed. That lock is what keeps a PPU store
    out of a PUTLLC's compare and commit. A store without it is safe only if no SPU can be in a
    PUTLLC on that line, and the hint array cannot promise that without a new handshake.
  - Indirect calls: `ps3_indirect_call_impl` itself is 1.0% (its lookup is cached per thread).
    The 2.3% shown as waits under it are samples outside the executable, named by the first
    return address the sampler finds on the stack: waits further down, not the call.
- **EDGE's skinning loop as native code. Done 2026-10-31** (`native/spu_edge_skin.c`).
  - The loop at LS `0xEC08` in job module `e8f44fbc6dbf` does matrix-palette skinning. Each
    iteration takes two vertices: four bones, three matrix rows each, every row blended with fm
    and then fma three times. It stores the rows at r49 and the transformed vertex in place at
    r86. It is software pipelined: an iteration loads the next pair's bone quadword and
    positions.
  - The native loop holds the two vertices in one 256-bit register, so each blend or transform
    is one AVX2 op for both. The shuffles are written for the loop's sixteen invariant registers
    (masks, 1.0, 1/255), which are checked at entry; they were the same in every call.
  - The float ops are native under the SPU code's MXCSR with the +0 rule, as the speculative
    forms. Once an iteration, before it stores anything, every result is checked to be below
    FLT_MAX. One that is not, and always the last iteration, runs in the lifted loop from the
    registers as the lifted loop has them, so the job ends with the lifted registers exactly.
  - `gowa.py lift` puts the call at the lifted function's entry, from
    `scripts/spu_native_hooks.json`. `GOWA_EDGE_NATIVE=0` turns it off;
    `GOWA_EDGE_NATIVE_STATS=1` counts calls, iterations and fallbacks.
  - Replay, `GOWA_EDGE_NATIVE` on against off on one build, three pairs: 53.6-53.9 against
    56.6-56.8 us an EDGE job (-5.1%). The outputs are identical in all 40 jobs.
  - Level: 7.6M calls, 366M native iterations, 0.05% of them fall back; no call fails the entry
    check. ABBA, two runs each: 15.88 -> 16.06 flips/s, both native runs above both off.
- **How much the main thread's own work sets the frame, 2026-10-31** (`GOWA_MAIN_SLOW=<percent>`,
  `poll_wait.cpp`, with fork `g_ppu_icall_hook`).
  - At every indirect call the main thread spins for that share of the time since the previous
    call, unless a wait came in between: a gap of 50 us or more, or a polling-loop pass in the
    last 50 us. It charges ~210 ms of its own work a second, about 60% of it, at ~340,000 calls
    a second.
  - One run each:

    | | spun | flips/s |
    |---|---|---|
    | base | - | 16.34 |
    | +25% | 56 ms/s | 15.98 (-2.2%) |
    | +100% | 196 ms/s (~12 ms a frame) | 15.44 (-5.5%) |
  - ~12 ms more of its work a frame makes the frame ~3.6 ms longer. Only about 30% of its own
    work is on the frame's path: it releases the EDGE batches, but mostly ahead of time.
  - So the main thread's lifted PPU code (registers in locals, 2.3) is worth ~1.5% for a 25%
    faster thread, not first. EDGE (-7 to -10% for +25%) stays the larger lever.
- **The job manager's work around an EDGE job, 2026-10-31** (a counting probe, not kept). Per
  EDGE policy-module run, outside the job:

  | | per run |
  |---|---|
  | full-path indirect branches | 26.7: `0x3FEC0` 7.9, `0xA2C` (the interrupt vector) 4.0, `0x1BA4`/`0x1BE8`/`0x1CC0` 3.0 each |
  | interrupts | 4.0: stall-and-notify (event `0x2`) of its DMA lists, and tag updates |
  | full drains | 19.8: `0xA84`/`0xA90`/`0xAD0` 4.0 each, all with a transfer pending |
  | channel reads | 33: `MFC_RdTagStat` 18.9, `MFC_RdAtomicStat` 4.2, the decrementer 4.0, event status 4.0 |
  | channel writes | 41: `MFC_WrTagUpdate` 18.9, `MFC_Cmd` 14.1, event ack 4.0 |
  | DMA transfers | 19.7, three of them the module's 16 KB pieces |
  | atomics | 4.2 |

  - The Wws job manager loads a job's commands through DMA lists that stall and notify. Every
    stall is an interrupt to its handler at `0xA2C`, which on the SPU runs beside the transfer.
    Here each one saves and restores the register file (2 KB each way), and returns through the
    drain loop: the full drains are where `SPU_DIRECT` hands a pending interrupt to it. That is
    how interrupts are delivered, not waste to cut.
  - Done, fork `df08790d`: while a handler's registers are saved, only the branch to the resume
    point (the iret) takes the full path. Every other branch the handler made took it too, though
    the full path's interrupt work is only for the iret. Full-path branches per run 26.7 -> 14.9
    (~1 us a run). It only removes work.
  - Not done: skipping the compare of the module's unchanged code (~1.5 us a run). That needs
    every writer of the module's source in guest memory tracked, e.g. as lines marked in the
    coherency bitmap, whose stores already take the notify path.
  - The rest is the emulation's per-operation cost spread over ~140 channel, DMA and dispatch
    operations a run, none above ~1 us. Left as is.
- **EDGE's normal loop as native code. Done 2026-10-31** (`native/spu_edge_normals.c`).
  - The loop at LS `0xA950` takes eight elements an iteration, each a pair of quadwords a and b.
    It computes the cross product `b*a.yzxx - a*b.yzxx`, scaled by b.w, with lanes 0-2 kept.
    Its squared length L goes through frsqest, fi and one Newton step, and the vector is
    normalised when L > 1.0e-30. Nothing but the pointers passes between iterations.
  - Two elements share a 256-bit register for the vector part. L and everything from it to the
    scale factor is one value an element (the SPU code keeps it in all four lanes): the eight
    elements' values share one register, so frsqest, fi and the Newton step run once for all
    eight.
  - The float ops are native, with the range check and the +0 rule on every op. A half with a
    lane out of range is done again by the exact forms; that never happened on the level.
    frsqest and fi are the runtime's. fcgt is the float compare while L is in range (DAZ, no
    NaN).
  - The first version, which ran frsqest/fi/fcgt per element in all four lanes, saved 0.5 us a
    job. Regrouped this way it saves 2.1 us.
  - `GOWA_EDGE_NATIVE` is now a mask: 1 is the skinning loop, 2 the normal loop; unset is both,
    0 neither.
  - Replay (one build, three pairs): 54.0-54.2 us an EDGE job with the skinning loop alone,
    52.0-52.1 with both. The outputs are identical.
  - Level: 1.79M calls, 56.6M native iterations. ABBA, two runs each: 16.04 (both off) -> 16.26
    flips/s (both on), both on-runs above both off-runs.
- Next (after 2.9's first steps): the other EDGE stages by their share of a job. These are `304A0` (5.6%), `A950` (5.1%),
  `7BB4` (5.1%), `30600` (4.3%) and `D92C` + `D4DC` (5.8%); `A950`, `D92C` and `BD54` fail the
  range check in most calls, so their native forms need a per-lane path, not a fallback per
  iteration.

### 2.9 Update, 2026-10-31: the optimization checklist against the code

[The audit](2026-10-31-technique-audit.md) checks a list of the usual PS3 recompilation
techniques (PPU, SPU, LV2, RSX, static-only, Zen 2) against the code: what is done, what is
not, and what is worth trying. Its new findings, and the order they go in:

1. **PPU vector loads and VMX, phase 2.** `lvlx` (13,610 sites), `vperm` (12,711) and `lvrx`
   lift to byte loops that clang unrolls to 85-119 instructions without `pshufb`. The
   remaining VMX helpers (`vcfsx`, the float compares, `vrefp`/`vrsqrtefp`, ...) run lane by
   lane. The previous VMX step gave +2.9%.
   - **Done 2026-10-31** (fork `a015ff00`): the five as `ppu_vmx.h` helpers (`pshufb`), and the word
     compares, `vmaxfp`/`vminfp`, the estimates, the roundings, `vcfsx` and `vctsxs` as vector
     forms. Each equals its scalar form in 55.5M operations.
   - The main thread's own work: 11.58 -> 10.73 ms a frame (-7.3%), by `GOWA_MAIN_SLOW=1`'s
     report (one run each, the same 18.4K indirect calls a frame).
   - Level, ABBA, two runs each: 16.42 -> 16.57 flips/s, within the spread (runs 16.84/16.34
     against 16.73/16.38). That is what the main thread's ~0.09% per 1% predicts. Kept: it only
     removes work.
2. **Probe the frame clock thread** (`RSX_SLOW`). It is 60.5% of a core at 16.3 flips/s, and
   `rf_open` still drains the GPU at every batch: CPU plus wait is about the whole frame.
   If it sets the frame, 1.4 and 1.5 come next.
   - **Done 2026-10-31** (fork `8dcd4671` `g_gcm_walker_hook`; port `poll_wait.cpp`, `main.cpp`).
     `RSX_SLOW=<pct>` reads the thread's CPU cycles (`QueryThreadCycleTime`, so the waits for the
     GPU, a kick or a present count nothing) at each FIFO command and after each present, at most
     every 100 us, and spins for <pct> of the cycles since the last reading.
   - Calibration: 608 ms/s charged + 6 spun against 62.0% of a core in the bench's thread table;
     ~37 ms of CPU a frame.
   - One executable, ABBA, two runs each: +1% (the control) 16.47/16.34, +25% 16.52/16.42 (the
     thread at 75% of a core). One run at +50% (85%): 15.87, -3.2%.
   - So the thread does not set the frame now, but its slack is only 25-50% of its work, ~9-18
     ms a frame. 1.4 and 1.5 are not first. They come before ~19-20 flips/s, where this thread
     binds.
3. **`__restrict` on the lifted context**, SPU first on the replay. Guest registers are
   reloaded from `ctx` after every guest store. The PPU needs `ppu_stwcx32`/`ppu_stdcx64` to
   take `ctx` first.
   - **Tried 2026-11-01 for the SPU** (`PORT_SPU_RESTRICT`, the lifter writes `SPU_RESTRICT`
     on the context; off by default). The replay, best of ten a job, outputs identical: General6
     871.5 -> 860.4 us, RenderJobs 466.7 -> 457.6, EDGE 52.7 -> 52.0. -1 to -2%, about the
     replay's own spread (the same build twice differs by up to 1.5%). Left off.
   - **That test could not find anything** (2026-11-06): LLVM's capture tracking gives up at 100
     uses of a pointer, so `noalias` did nothing in a function of size. See
     [the review](2026-11-06-main-thread-review.md).
   - On the way: the replay restores a captured context with its resident code slots, so the
     position-independent pieces' image ids must stay as they were. The four Batch pieces had
     been put first in `scripts/spu_pic_pieces.json`; the EDGE pieces' ids moved by four, their
     slots no longer verified, and EDGE replayed 10x slower (interpreted). New pieces go last.
4. **`-march=x86-64-v3 -mtune=znver2`** for the lifted code. The PPU code has FMA/AVX only.
5. The EDGE stages and the module-compare skip (2.8), then the PPU indirect-call short path.

### 2.10 Update, 2026-10-31: why RPCS3 is 1.5 times faster, at the job level

[The comparison](2026-10-31-rpcs3-vs-port.md) reads the title's SPURS instance and Wws ticket
rows from outside RPCS3 and the port alike (`scripts/guestscope.py`).

- Both run the same jobs a frame: 534 an EDGE list, 26 General6, 40 RenderJobs.
- RPCS3's EDGE phase is 27.6 ms a frame, the port's 52.8: about the whole difference.
- **The title asks for 5-8 SPUs a workload, and the port gives it one.**
  - The maximum contention is the ninth argument of `_cellSpursWorkloadAttributeInitialize`.
    It is on the stack, and the HLE adapter writes 1 in its place.
  - EDGE runs on three only by the `SPURS_MAXCONT` default.
- **The port's kernel ignores SPURS priorities.** It picks round robin, takes one from the ready
  count at each pick, and never makes a polling policy module yield.
  - RPCS3's kernel keeps EDGE (priority 3) on four SPUs for 86% of its time.
  - With only the maxima raised, General6 and RenderJobs take those SPUs: General6 39.6 -> 11.1 ms
    a frame, RenderJobs 26.4 -> 7.7, but EDGE starved and the frame rate stayed (15.56 -> 15.59).
- An EDGE job takes ~1.4 times RPCS3's at any contention; a Batch job ~8 times.

So the order becomes:
1. the ninth argument (the title's maxima);
2. the kernel's choice as SPURS makes it (admission by ready count, priority weights, yield on
   poll). Expected ~19 flips/s from EDGE at four SPUs;
3. Batch's 8x;
4. the EDGE job (native stages, `__restrict`, the policy module's cost);
5. the frame clock thread (1.4, 1.5), which binds near 19-20 flips/s.

The audit's `__restrict` and `-march` steps move behind these.

**Steps 1 and 2 done 2026-10-31** (fork `1a1aa4f9`, port `main.cpp` without the `SPURS_MAXCONT` default):
- the ninth argument read from SP + 0x70;
- `spurs_claim` / `spurs_pick` choose as SPURS does, and `spurs_kernel_poll` answers a polling
  policy module. The old answer, workload 0, ended every other workload's run at its first poll:
  EDGE ran ~1.3 jobs a run, now ~45 (`SPURS_WID_PROF`: ~210 runs/s a list, 3.7-4.1 ms a run);
- `SPURS_SELECT=rr` restores the old scheduler; `tests/test_spurs_select.c` covers the new one.
- Level, ABBA, two runs each: **16.19 -> 18.92 flips/s (+16.9%)**, every new window above every
  old one.
- `guestscope.py` on the new build: the EDGE lists hold four SPUs 79% of their time (RPCS3 86%).
  The EDGE phase is 52.8 -> 36.5 ms a frame, and an EDGE job takes 85 us per SPU at four SPUs
  (RPCS3 68). General6, RenderJobs, RenJobSet and Batch now run on 2-3.4 SPUs.
- The frame clock thread is at 72% of a core (60%): it is the next limit, as `RSX_SLOW` said (2.9).
  Next: Batch's 8x, then the frame clock thread (1.4, 1.5) and the EDGE job.

**Step 3 done 2026-10-31** (fork `0ff31a6b`, port `scripts/spu_pic_pieces.json`): Batch's SPU code
runs lifted.
- It was 52% in the SPU interpreter: four position-independent pieces were not in the list, and
  the biggest comes inside 16 KB blocks from PPU heap buffers, never from its EA, and stores into
  its own constant pool.
- The pieces are added; a piece is now found by its bytes where dispatch finds nothing, and
  verified on its instructions only (comparison note, 4.2).
- Interpreted SPU steps 4.3M -> 0.2M a second; Batch 2226 -> 264 us a run.
- Level, ABBA, two runs each: 18.51 -> 18.59 flips/s, within the spread. The SPU workers use 0.35
  of a core less. Kept: it only removes work.
- Batch did not set the frame. Next: the frame clock thread (1.4, 1.5), then the EDGE job.

### 2.11 Update, 2026-11-01: EDGE's stages measured again; its fixed-point decoder as native code

**Where an EDGE job's time goes now.** The sampler on the workers over the level's 80 s, the
three EDGE workloads alike; a `--spu-lines` build splits each function by the operation a sample
falls in:

| function | share of EDGE | what costs most in it |
|---|---|---|
| `304A0` (piece `0x758590`) | 4.8% | frsqest/fi ~19%, shufb 10%, the exact int-to-float conversions 8% |
| `7BB4` | 4.6% | fm/fma/fa ~32% and their range checks ~15%, frsqest/fi ~19% |
| `30600` (piece `0x755B80`) | 3.9% | shufb 22%, float-to-int conversions 12% |
| `D92C` | 3.4% | shufb 18%, float-to-int conversions 13% |
| `30310` (piece `0x75EB40`) | 3.2% | int-to-float conversions ~32%, quadword shifts ~18% |
| `EC08` (native since 2.8) | 3.0% | |

- Across all of EDGE's lifted code, by operation:
  - shufb 13.9%;
  - LS reads and writes 15.1%;
  - the crossings between lifted functions (`SPU_DIRECT`, `spu_link`) 9.3%;
  - float ops and their checks ~12%;
  - frsqest/fi 5.3%;
  - conversions 3.8%.
- `30310` and `304A0` are the same fixed-point vertex attribute decoder, 8.0% of EDGE between
  them.
  - Most of their cost is integer work and conversions, which are exact natively.
  - `30310` is the small one: 54 statements, 7 registers carried from one iteration to the next,
    ~100 iterations a call in the level. It went first.
  - Native hooks were for job modules only; `gowa.py lift` now puts them into the pieces too
    (`pic_<EA>` in `scripts/spu_native_hooks.json`).

**Done: the decoder loop `30310` as native code** (`native/spu_edge_decode.c`,
`GOWA_EDGE_NATIVE` bit 4).
- A vertex is a 24-byte record of packed bit fields; the loop makes two attributes of it into
  floats, each field an integer part plus a fraction. It is pipelined three deep:
  - an iteration fetches the next record (two shufb of the three quadwords around it);
  - it decodes the record fetched before (two gathers, the fields shifted into place by bits and
    then bytes, merged);
  - it converts and stores what was decoded before (csflt of the integer part, cuflt of the
    fraction, fa).
- Native: the same three stages in the same order.
  - The stores come before the next record is read, so outputs that overlap the input read the
    same bytes as on the SPU.
  - The format comes from the job: the gather controls and the shift counts are registers. They
    are prepared once a call, and any values work.
  - csflt is cvtdq2ps. cuflt is an unsigned conversion with the same rounding (`x >> 1` from
    2^31). Both run under the SPU code's MXCSR (toward zero, FTZ, DAZ), which is checked at entry.
  - fa is native with the range check and the +0 rule.
  - The last iteration runs in the lifted loop, so the job ends with exactly the lifted registers.
- Checked:
  - `tests/native/test_edge_native.py` takes the lifted loop from its lifted source and runs it
    against the native one: random registers and local store, and in a third of the cases the
    outputs over the input. 3000 cases are identical.
  - Seven mutants are caught: cuflt as signed, a swapped shift, a bit count, the special
    selectors, rotma's count, the record read before the stores, r18 left behind.
  - Replay (`logs/jobcap2`, w15): 0 differ, 110 calls, 12,290 native iterations.
- Speed:
  - The loop alone: 28.9 -> 13.9 ns an iteration (gcc, 100 iterations a call).
  - Replay, four alternate runs: the 40 EDGE jobs 2064 -> 2045 us (-0.9%). The six that use the
    loop save 3-5 us each.
  - Level, ABBA, two runs each: 19.42 -> 20.09 flips/s, both on-runs above both off-runs. The
    bench's first run is often low: without it, 19.73 against 19.83 and 20.21.
  - In the level: ~1.9M calls and ~200M native iterations a run, no fallback.

**Done: the decoder loop `304A0` as native code** (piece `0x758590`, `GOWA_EDGE_NATIVE` bit 8).
- The same decoder with more after it, pipelined four deep, 23 registers carried:
  - an iteration fetches record t+1;
  - it decodes record t: two attributes, and four values that are table indices and, scaled
    (fma), two coordinate pairs whose squares it keeps;
  - for vertex t-1 it converts and stores the attributes, reads four table quadwords by the
    indices, and rebuilds two third coordinates as sqrt(1 - x^2 - y^2) (frsqest, fi, fm);
  - for vertex t-2 it merges those into four quadwords by shufb controls taken from the table
    entries, and stores them.
- Native, statement for statement. The 13 local store accesses of an iteration keep the lifted
  order, so a table or record under an output reads the same bytes.
- The first form, one op for each lifted statement, was only 1.27x.
  - Making one part trivial at a time (timing only) showed where the time went: frsqest/fi 37%,
    selb ~23%, the float checks 9%.
  - Nearly all of the work comes in pairs: the two attributes, the gathers that share their
    sources and shift counts, the two coordinate pairs and their third coordinates, the two
    merges of a table entry. Each pair is now one 256-bit op, kept paired from one iteration to
    the next.
  - frsqest and fi are 256-bit ports of the runtime's x86 forms, lane for lane.
  - A selb with one operand fixed for the call keeps that operand's part prepared.
- The loop alone: 83.2 -> 46.4 ns an iteration (1.79x; gcc, 44 iterations a call).

**Done: `30310`, two vertices to a 256-bit op** (bit 16).
- The next two records are read before the two vertices are stored. It runs only where every
  store of the call misses every quadword the call's fetches read, none of it wrapping the end of
  local store: conservative spans, checked once a call. Elsewhere the one-vertex form runs.
- In the replay and the level, 99% of the loop's iterations take it.
- The loop alone: 14.2 -> 12.3 ns an iteration (3.16x -> 3.67x of the lifted loop).

**Checked, both:**
- `tests/native/test_edge_native.py`, 3000 cases each, identical.
  - `30310`: half the cases with r18 = r19, as the piece sets it, so the paired form runs: 48% of
    the native iterations.
  - `304A0`: one case in four puts the attribute outputs on the records, one in four a merged
    output on the first table entry read.
- 21 mutants caught, among them: the paired form's second store offset, its lanes swapped, its
  span check skipped; a lookup by the new index instead of the old; the store moved after the
  table read; a frsqest table row dropped; fi's product as `mul_epu32`.
- Replay (`logs/jobcap2`, w15): 0 differ, no exact float op, no call left to the lifted loop.

**Speed, against the previous step (`GOWA_EDGE_NATIVE=7`):**
- Replay, four alternate runs: the 40 EDGE jobs 2026 -> 1930 us (-4.7%). The jobs that use
  `304A0` save up to 11 us each.
- Level, ABBA, two runs each: 20.03 -> 20.17 flips/s, within the spread (runs 20.07 / 19.98
  against 20.07 / 20.57). What EDGE's share predicts is ~+1%.
- In the level: `30310` ~201M native iterations a run, `304A0` ~115M; no exact float op.

**Next:**
- The broader levers. shufb, the LS accessors and the crossings are about a third of EDGE's
  lifted time, in every function. A cheaper form of any of them helps all of EDGE, and the other
  workloads too.
- The next stages by share: `7BB4` (4.6%), `30600` (3.9%, a piece, so its hook is ready),
  `D92C` (3.4%).

### 2.12 Update, 2026-11-01: the broad levers, each measured alone

**What they act on.** The `--spu-lines` profile again, by operation, as a share of each
workload's samples:

| workload | crossings | LS reads + writes | shufb |
|---|---|---|---|
| EDGE (w14-16) | 10.6% (`SPU_DIRECT` 5.2, prologues 3.3, `spu_link` 2.1) | 6.9 + 3.1% | 8.8% |
| RenderJobs (w12) | ~20% | 19.3% | 5.2% |
| RenJobSet (w8-11) | ~17% | 10.5% | 2.8% |
| General6 (w2) | small | 14% (reads) | 13.7% |

- 60% of EDGE's shufb samples are in a shufb whose control the loop around it never writes.
- 60% of EDGE's `SPU_DIRECT` samples are a chunk tail-calling the next one: the lifter starts a
  function at every branch target.
- Each lever below was measured alone, by replay: `logs/jobcap2` (w2, w12, w15) four alternate
  runs, `logs/jobcap3` (w8-11) two; per job the best of 10, summed per workload. Every job's
  output was identical to the capture.

**Tried, slower: `preserve_none` on every lifted function.**
- Under the Win64 ABI a lifted function saves and restores the xmm6-15 and rbx/rsi/rdi it uses,
  and a tail call pays the caller's epilogue and then the callee's prologue (3.3% of EDGE).
- clang's `preserve_none` on the lifted functions, their pointer types and the indirect-branch
  entry points removes those saves. But a caller then keeps nothing in a register across a
  `brsl` into such a function: it spills whatever is live around every call.
- Replay: General6 +44.9%, RenderJobs +2.7%, EDGE +2.7%. Not kept.

**Done: job modules lifted as merged runs of chunks, but General6's** (`gowa.py lift`;
`GOWA_JOBMOD_MERGE=0` turns it off).
- With `--merge-chunks` a run of chunks between call targets is one C function with gotos,
  instead of chunks that tail-call each other.
- Every module merged: EDGE -3.1%, RenderJobs -6.1%, General6 +12.3%. A merged run has
  internal branches, and only a function without any gets a `--float-spec` body, which is where
  General6's time is.
- `lift_jobmods.py --merge-skip` leaves General6's two modules as they were. Replay, against
  nothing merged:

  | w2 (General6) | w8 | w9 | w10 | w11 | w12 (RenderJobs) | w15 (EDGE) |
  |---|---|---|---|---|---|---|
  | +0.5% | -7.8% | -2.5% | -7.8% | -1.1% | -3.6% | -2.7% |

- Level, ABBA, two runs each: 19.62 -> 20.12 flips/s (runs 19.62 / 19.54 against 20.25 / 19.48).
- A native hook's loop is then also inside the merged run that leads to it, entered there by a
  goto. The hook's call goes right after the loop's label too.
- The exe: 112.8 -> 117.2 MB.

**Done: a shufb control a loop doesn't change, prepared before it** (`spu_lifter.py`, on by
default; `--no-shufb-hoist`).
- On x86 `spu_shufb` is ten ops, six of them on the control alone: the source index, the a/b
  select, the special selectors.
- The loops: a backward branch to a label, with no other label in between and no way in but
  falling into it, and no call, indirect branch or channel op in the body. A shufb there whose
  control register nothing in the body writes gets the control prepared once before the loop
  (`spu_shufc_prep`). In the loop it is then two pshufb and two blendv (`spu_shufb_pre`).
- 2047 of the 30330 shufb sites; 626 of the EDGE module's 1881.
- Checked:
  - `runtime/spu/tests/test_spu_shufb.c`: 200000 random cases against the byte-loop reference;
  - `tools/test_spu_lifter_shufb_hoist.py`: the lifted shape, and a run against the lift
    without it on 2000 random register sets.
- Replay, against the merged lift: EDGE -4.0%, w9 -3.1%, General6 -0.6%. The rest is within
  its run-to-run spread (w10 +1.7%, its runs +-5%).
- w13 can't be measured this way: with either build, many of its jobs sometimes take 1-1.5 ms
  instead of 4 us.

**Tried, slower: `ctx->ls` read once a function.**
- The SPU code is built with `-fno-strict-aliasing`, so any store may change `ctx->ls`, and each
  LS access reads it again: 12 times in `7BB4`, most of them in its loop.
- Read once, at the function's top, and passed to the accessors: 6.1 -> 1.05 loads of it a
  function (weighted by samples), and 0.6% fewer instructions. But the pointer then holds a
  register through the whole function, calls included. The prologue's pushes change in 77 of
  the EDGE module's 1093 functions, 2.54 -> 2.63 a function weighted by samples, and the piece
  `30600` (3.9% of EDGE) saves one more register and spills once more.
- Replay: EDGE +3.2% in all four runs, the rest within noise. Not kept.

**Speed, against 38c9d6b:** level, ABBA, two runs each: 19.98 -> 20.66 flips/s (+3.4%), both
new runs above both old ones (20.82 / 20.49 against 20.01 / 19.98). That is ~86% of RPCS3's
24.0.

**Next:**
- In each EDGE iteration, `ctx->gpr` is still written to memory and read back around the LS
  stores, for the same aliasing reason. The loops' registers in locals, without the speculative
  float, were -5.5% in replay as a prototype on EDGE's single-block loops (2.8). As a lifter
  option, written back at each exit, that is the next broad lever; part of it may now be taken by
  the shufb hoist.
- The next stages by share as native code: `7BB4`, `30600` (a piece, its hook ready), `D92C`.

### 2.13 Update, 2026-11-01: one shader per fragment program, built from the program as drawn

Found switching MLAA off ([note](2026-11-01-title-patches.md), section 5): every run compiled
hundreds of fragment shaders the cache had never seen and skipped ~80,000 draws while they built.

- A buffered program had its units' sRGB decode compiled in, and which units are cubes, so it
  became a shader for each combination of its bindings. Now the decode is chosen when it runs
  (`_rsx_gamma_dyn`, the lanes in `fp_tcscale[u].z`), every unit takes its coordinate scale from
  b1, and the key leaves both out: MLAA on, 1624 -> 305 pipelines a run.
- The pipelines of a batch are looked up at its end, and with MLAA off the title had by then
  written over 0.2% of the programs (bursts of ~2,000). Now each draw keeps its program as
  recorded (`fp_snapshot`). MLAA off: 0 shaders compiled from its second run, 2082 -> 372
  pipelines, ~80,000 -> 5 draws skipped.
- Level, ABBA, two runs each: MLAA on 20.03 -> 20.20 flips/s (within the spread); MLAA off 21.94 -> 22.33, without the compile dips (lowest window 18.9 -> 21.6). With MLAA off the frame clock thread (77-79% of a core) now presents ~0.8 a second fewer frames than the title flips: the next limit there.

### 2.14 Update, 2026-11-01: the frame clock thread with MLAA off

With MLAA off (2.13) the frame clock thread presented ~0.8 a second fewer frames than the title
flipped.

**Why frames went unseen.** The thread does everything in turn: the FIFO walk with the draws'
recording, then the render (prepass, command lists, Present), record-ahead and the window's
messages. At a flip the walk stops so that the frame is presented, unless it is more than
`GCM_FIFO_CATCHUP_BYTES` (64 KB) behind; then it goes on and the frame is never presented. The
title also waits for the walk to reach its flip before it builds the next frame.

**Where the thread's time went** (the sampler, MLAA off, 80 s): idle 16%; the FIFO walk and RSX
methods ~19%; the prepass and pipeline lookups ~13%; the draw pass and the driver ~10%; textures
~9%; vertex uploads ~9%; the fragment programs' structural hash 4.2%; the draws' recording ~4%.
No one place; but some of it was done two or three times a draw:
- each program's structural hash at both of a draw's pipeline lookups, its texture units at the
  prepass (from guest memory, not the program as recorded), and its inline constants at the
  record;
- the units' gamma, cube and scale keys at both lookups, though a buffered program then drops
  them; and a 16-unit walk for each unit's cube flag in the prepass.

**Done** (fork `rsx_d3d12_backend.c`): the snapshot of a program (2.13) now keeps its structural
hash, texture units and constants, worked out once a batch for each program; the gamma, cube and
scale keys are worked out only for a program compiled with them in. And the flips never
presented are counted (`skipped=` in the thread table), with `GCM_FIFO_CATCHUP_BYTES` an
environment setting; the bench reports presents and skipped flips per run.

**Measured**, level, ABBA, two runs each, MLAA off:

| | flips/s | presents/s | skipped/s | frame clock thread |
|---|---|---|---|---|
| before | 21.50 (22.19 / 20.98) | 20.99 (21.58 / 19.93) | ~0.6 | 76-78% |
| work once a program | 22.14 (22.04 / 22.47) | 22.04 (21.99 / 22.32) | 0.00 | 68-69% |
| and catch-up at 4 MB | 22.39 (22.49 / 22.23) | 22.38 (22.44 / 22.06) | 0.00-0.10 | 68-69% |

- The thread dropped from 77% to 68.5% of a core, and every flip is presented again.
- The catch-up threshold does not matter here: 64 KB and 4 MB measured the same. This title's
  command buffer is in segments, and the backlog the walk computes at a flip is an IO distance,
  ~8 MB at every flip, so it goes on past flips with either. A frame goes unpresented only when
  the walk reaches the next flip before the ticker has taken this one: keeping up is what counts.
  It stays at 64 KB.
- MLAA on (the default), the same builds: 20.28 -> 20.77 flips/s (+2.4%), both new runs above
  both old ones (20.94 / 20.72 against 20.45 / 20.18); presents 20.32 -> 20.77; the thread 71-72%
  -> 67%.

**So neither of the larger steps is needed now:** with the thread at ~68% of a core and nothing
skipped, splitting it into a walk thread and a render thread (RPCS3's "Multithreaded RSX") would
not raise the frame rate, which the SPU side sets (2.12, comparison 4.6). It is the step to take
when the frame clock is the limit again.

**Tried 2026-11-05** (phase 2b plan, "a render thread"; fork branch `perf/d3d12-render-thread`):
the recording and the presents on a render thread took the frame clock thread from ~73% of a
core to ~41%, and the frame rate fell ~3% at the gate (20.87 -> 20.18 flips/s, two runs each).
As above: not the limit yet; the SPU workers set the frame (2.25). Its fastest mode (the walk
going on past the batch's reset) also hung the GPU in two of four runs, not yet found.

### 2.15 Update, 2026-11-01: the runtime's work inside an EDGE job

The EDGE workers set the frame (2.12, comparison 4.6: EDGE phase 35.9 against RPCS3's 27.6 ms,
the same SPUs, a job 88 against 68 us). The question was how much of an EDGE job is the runtime
rather than the job's own code, and what of it can go.

**Where an EDGE worker's time goes** (HEAD with `--jobcap`, the sampler on the `spurs pm` threads,
`SPU_JOB_TIME` tagging the samples taken inside a job; level, ~80 s):

- 92.5% inside the jobs, 7.5% between them. The job manager no longer loads the 52 KB job module
  before every job (2.8): the GETs into code inside a job are ~3 KB, the position-independent
  pieces below.
- Inside a job: the job's lifted code 55.0%, its native stages 8.0%, float redo 1.3%; the job
  manager's own code (its interrupt handler) 4.0%; dispatch 9.9%; DMA ~11% (the `memcpy` of the
  data alone 3.7%); lock-line and coherency 4.2%; channels and events ~3%; the rest ~3%.

**What the runtime does for one EDGE job** (new: `SPU_JOB_TIME` in a `--jobcap` build prints
these per workload, per job):

| | per job (79.4 us) |
|---|---|
| lifted calls / indirect branches / lookups | 103 / 63 / 70 |
| branches on the full path | 8.7: the generated register save/restore stub at 0x3FEC0 5.4, the interrupt handler's entry 0xA2C ~2, kernel polls 0.4, irets 1.0 |
| lookups missing the dispatch cache | 21 |
| GET / PUT | 15.7 (9.7 KB) / 2.9 (6.9 KB), and 2 lists (23.5 elements) |
| GETLLAR / PUTLLC / PUTLLUC | 3.1 / 1.1 / 1.0 |
| lock-line lock | 4.4, 0.7 of them waiting |
| interrupts (DMA list stall-and-notify) | 2.0 |
| rdch / rchcnt / wrch | 27 / 3.9 / 22 |

No one item is large: each mechanism costs 1-2 us of the 79.

**Done** (fork, runtime):
1. **The dispatch cache keyed per 4 KB page.** An EDGE job loads ~2 KB position-independent pieces
   at 0x11xxx and 0x1Dxxx in turn, ~1.8 span changes a job (96% of the spans a GET ends are these
   pieces), and each change dropped every cached resolution of the context. Each change now bumps
   only the pages its spans cover; a resolution keeps its page's count. Misses 21 -> 6 a job.
   Checked with `SPU_DISPATCH_CACHE=shadow` over a level run: 1.28 billion hits compared with the
   full resolver, no mismatch.
2. **The interrupt handler's entry on the fast path.** The fast path started at 0xA80 to stay
   above every special address; the special ones are 0x9C0, 0x9D0, 0xA00 and 0xA70, and the
   handler's entry 0xA2C is a plain lifted function. Full-path branches 8.7 -> 6.7 a job.
3. **`WrEventAck` without the lock-line lock.** It took the lock against a lost update of the LR
   bit; `event_status` is updated with atomic OR and AND (fork `81344af1`), which closes that
   without it. Lock acquisitions 4.4 -> 2.45 a job, waits 0.70 -> 0.38.

**Measured:**

| | EDGE job (jobcap builds, last 9 windows) | flips/s |
|---|---|---|
| before, jobcap build | 81.2 us (82.8 / 79.7) | 19.45 (19.09 / 19.59) |
| 1 + 2, jobcap build | 80.1 us (80.8 / 79.4) | 19.44 (19.29 / 19.55) |
| HEAD, default build | | 20.75 (20.75 / 20.74) |
| 1 + 2 + 3, default build | | 20.98 (21.05 / 20.89) |

- Default builds, level, ABBA, MLAA on: +1.1%, both new runs above both old ones (the new
  exe's first run, to warm the driver's cache, gave 21.00).
- The jobcap ABBA saw 1 and 2 as ~1 us of an EDGE job, inside its run-to-run spread, and no
  change in flips; the counts above are the firmer measure.

**So:** the runtime's share of an EDGE job is real (~a third), but it is spread over a dozen
mechanisms of 1-2 us each, and the frame rate moves ~0.26 for each unit of EDGE time (2.12).
Removing every one of them that is safe to remove would gain a few percent of EDGE time and ~1%
of frame rate. What is left: the copies themselves (necessary), the interrupt round trip (its
save and restore stubs are interpreted, 5.4 a job), the check after every lifted call, and the
lookups' hits.

### 2.16 Update, 2026-11-02: what sets the frame after the rendering work; the frame clock's first round

**Where the frame stands.** The level at 21.0 flips/s with MLAA off (the user's `gowa.ini`),
RPCS3 24.0. The thread table: the SPU workers 580% of six cores, the main thread 95%, the frame
clock thread 76%. That last one was 68% on 2026-11-01: the rendering fixes since (the occlusion
counts' extra draws, P3's shadow maps and antialiased surface) added ~18% to its frame.

**Each part made 25% longer** (`logs/probe-now`, ABBA, two runs each; the 2026-11-01 figures are
the [comparison note](2026-10-31-rpcs3-vs-port.md)'s 4.4):

| +25% on | flips/s | change | 2026-11-01 |
|---|---|---|---|
| nothing | 21.02 | | |
| the frame clock thread (`RSX_SLOW=25`) | 18.40 | -12.5%; presents 20.5 -> 4.6 a second | -2.8% |
| EDGE (`SPURS_WID_SLOW`, w14-16) | 19.30 | -8.2% | -6.5% |
| General6 (w2) | 20.36 | -3.1% | |
| the main thread (`GOWA_MAIN_SLOW=25`) | 20.82 | -1.0% | ~0 |

- The frame clock thread is now the most sensitive. Slowed, it falls behind, and the walk passes
  flips without presenting them. Diagnostics as light as `PERF=1` did the same (3.5 presents a
  second).
- EDGE sets the rest.

**The sampler** (`logs/prof-now`, 75 s of the level, `PS3_SAMPLE_THREAD`, `SPURS_WID_PROF`):
- The SPU workers: 590% of six cores. EDGE 2.79 cores, General6 0.95, audio 0.70, RenJobSet
  0.64, RenderJobs 0.56.
- The main thread: `poll_mwaitx` 41%. One spin loop the hooks did not have, 15%: in
  `func_0041D4E8`, at `0x41DB50`, until the u16 at `*(r30 + 0xC) + 0xE0` reads 0 (its samples
  fall on the loop's load, compare and branch). Lifted work ~28%, coherency ~5%.
- The frame clock thread:
  - idle in the message pump 20%
  - the FIFO walk and the methods ~19% (`rsx_process_method` 8.3%)
  - the vertex uploads ~9.4%
  - the textures ~10%
  - the prepass ~9%
  - the draws and the driver ~10%
- The GPU: the 3D engine 28% on average, at 525-885 MHz.

**Where the walk's time goes** (`GCM_METHOD_HIST`, words a second):

| words a second | what |
|---|---|
| ~500k | NO_OPERATION (0x0100) |
| ~790k | vertex constants (already one call a run, `rsx_process_vp_run`) |
| ~127k | DRAW_INDEX_ARRAY |
| ~88k | SET_BEGIN_END |
| ~80k | INLINE_ARRAY |
| ~70k | INVALIDATE_VERTEX_CACHE_FILE (0x1714) |
| ~33k each | the two scissor words |

`rsx_process_method` is a chain of ~150 tests ahead of the draw and scissor handlers. NO_OPERATION
and 0x1714 went through all of them to the -1 at its end. `upload_tris_vp_raw` also read each
index a byte at a time, ~1.2 million a frame (`VTX_STATS`).

**Done** (fork, and `scripts/ppu_hooks.json`):
1. `rsx_process_method`: the frequent methods jump to their handlers; NO_OPERATION and 0x1714
   return -1 at once (`RSX_METHOD_FAST=0`: the chain).
2. The walker skips NO_OPERATION words that do not increment into other methods
   (`GCM_NOP_FAST=0`).
3. The index conversion as SSE4.1 (`rsx_index_copy.h`, `VTX_IX_SIMD=0`): eight 16-bit indices an
   operation. `tools/test_rsx_index_copy.py` compares it with the scalar form on 8,000 random
   arrays (lengths, offsets, ranges).
4. The main thread's loop at `0x41DB50` calls `gowa_poll_wait`, as the other four. The PPU code
   was lifted again for it (`gowa.py lift --chunks 200`, 200 files rebuilt).

**Measured** (`logs/abba-fc1`, ABBA, two runs each):

| | flips/s | presents/s | frame clock thread |
|---|---|---|---|
| before | 21.01 (21.04 / 21.01) | 20.64 | 75.5-76.3% |
| 1-4 | 21.19 (21.23 / 20.87) | 20.84 | 71.1-71.7% |
| 1-4 and `TEX_CACHE_MB=2048` | 21.27 (21.35 / 21.09) | 21.17 | 71.5-71.7% |

- The frame clock thread's work is 6% less, back to ~71% of a core. The frame rate moved within
  the spread (+0.9%): the saturated SPU side is the other half of the limit.
- A texture cache of 2048 MB instead of 1311 (a third of the 4 GB): +0.4% flips, +1.6% presents,
  0.4 GB more private memory. The default stays.

**Next, EDGE as native code (plan 2.11's list), weighed before starting:**
- `7BB4` (lighting, 4.6% of EDGE): 59 float operations an iteration (29 fm, 18 fma, 8 fs), two
  frsqest/fi pairs, 18 shufb, 12 stores. Its work comes in pairs, so it can go two elements to a
  256-bit operation, as the normal loop (`A950`) did. Expected ~1.8x.
- `30600` (4.7%): ~130 instructions an iteration, its shufb controls computed in the loop, float
  to integer conversions, clgth/selb tests. Statement for statement it would be ~1.2-1.3x (as
  `304A0`'s first form).
- At -8.2% for +25% of EDGE, both would give ~+1-1.5% flips/s. The lifter's registers in locals
  for single-block loops (2.12 "Next", -5.5% of EDGE as a prototype) covers both loops and every
  other workload's loops.

### 2.17 Update, 2026-11-02: the audio worker's dispatch; computed jumps as gotos (off for now)

The second half of the review's cheap fixes (its option 6). 2.16 did the main thread's loop at
`0x41DB50`; this is the audio worker's dispatch.

**Where the audio worker's time went** (`logs/prof-now`, w7, 75 s of the level):
- Lifted code 66%, the runtime 22%: `spu_lifted_lookup` 7.7%, `spu_indirect_branch` 4.1%,
  `spu_drain_call_full` 4.0%, `spu_ib_resolve_task` 1.8%. About 18% of the worker's time was
  dispatch.
- Its seven hottest lifted functions (job modules `c5a3bed2f4a5`, `5bc2fdee17f4`,
  `d3eebe50d457`) are branch-free loops. Each pass ends `selb $rX, $head, $exit, $cond;
  bi $rX`: the head's address comes from an `ila` before the loop, the exit is `$r0` or the
  code after the loop.
- A `bi` that is not a return was lowered as "set pc, trampoline, return". Each pass left its C
  function and came back through the drain loop, `spu_indirect_branch`, the task resolution and
  the lookup, to enter the same function at its start again.

**Done** (fork: `spu_lifter.py --ijmp-local`, which `gowa.py lift` passes for the job modules):
- A computed jump (`bi` and the `bi*z` forms, not a return) compares its target with the
  entries of its own C function that an `ila` of the image loads: its start, or a chunk that
  `--merge-chunks` put in it.
- It goes there with a goto when `SPU_IJMP_LOCAL` allows. The tests are `SPU_DIRECT`'s: not the
  run's first transfer, not a stack-reset entry, no interrupt pending with interrupts enabled,
  no diagnostic that watches each step. Anything else goes to the dispatcher as before.
- The goto lands ahead of the entry's shufb preparations, so they are made again.
- 690 jumps in 16 job modules, ~480 of them in the four audio modules. Every other line of the
  lifted C is as before.
- A function with a `--float-spec` body keeps the dispatcher in both bodies. One has both:
  `759dedc8269b`'s `0x88D8`.
- `SPU_IJMP_LOCAL=0`: the dispatcher as before. `SPU_IJMP_LOCAL=shadow`: every goto is checked
  against the lookup and taken only if they agree.
- `tools/test_spu_lifter_ijmp_local.py` covers:
  - the loop unmerged and merged: one dispatch (the exit) with the gotos, 100 without;
  - shadow;
  - a pending interrupt, which still reaches the drain loop;
  - a `--float-spec` function, left as it was.

**Measured** (one exe, `SPU_IJMP_LOCAL=0` against `1`):
- Shadow, a level run (`logs/prof-ijmp`): ~671 million gotos, no mismatch.
- The audio worker (`SPURS_WID_PROF`):
  - 68-70% of a core -> 55-57%;
  - 3.7-4.0 ms a run -> 2.6-2.7 ms;
  - its samples in the same 75 s 16,142 -> 12,308 (`spu_lifted_lookup` 7.2% -> 1.8%).
- The time it gave back went to the other workloads: General6 87-88% -> 92% of a core, EDGE
  211-220 -> 223-227 runs a second. The workers stay at ~593% of six cores.
- Level, ABBA, six runs each (`logs/abba-ijmp`, `logs/abba-ijmp2`; the mean of each run's
  windows, then of the runs):

  | | flips/s | presents/s | skipped/s | frame clock thread |
  |---|---|---|---|---|
  | `SPU_IJMP_LOCAL=0` | 21.23 | 20.94 | 0.30 | 71.6% of a core |
  | `SPU_IJMP_LOCAL=1` | 21.41 (+0.8%, t 1.1) | 20.37 (-2.7%, t -1.8) | 1.04 (t 3.3) | 73.8% (t 4.5) |

- The frames came a little faster and fewer of them reached the screen. The frame clock thread
  took 2.2 points of a core more for 0.8% more frames, and fell behind more often.
- The likely cause is the cores it shares: six workers, the main thread and the frame clock
  thread are eight busy threads on six cores with SMT, and denser SPU code leaves a sibling
  thread less. Not tested further.
- **The port keeps it off** (`main.cpp`: `env_default("SPU_IJMP_LOCAL", "0")`);
  `SPU_IJMP_LOCAL=1` turns it on.
- The same wall stands before 2.16's next step (EDGE as native code): SPU time saved now raises
  flips, not presents. The frame clock thread needs headroom first, from its own hot spots, a
  core to itself, or the multithreaded RSX.

### 2.18 Update, 2026-11-03: computed jumps as gotos on; the replay repaired; the loops' registers in locals (off)

**Computed jumps as gotos, on.** 2.17 left `SPU_IJMP_LOCAL` off because the frame clock thread
then fell behind more often. The stutter fixes since (`docs/notes/2026-11-02-fight-stutter.md`:
the drain stops at every flip; the frame clock thread at the highest normal priority) took that
cost away. Level, ABBA, three runs each (`logs/a-ijmp`; the mean of each run's windows, then of
the runs):

| | flips/s | presents/s | skipped/s | frame clock thread |
|---|---|---|---|---|
| `SPU_IJMP_LOCAL=0` | 20.70 | 20.65 | 0 | 70.4% of a core |
| `SPU_IJMP_LOCAL=1` | 21.33 (+3.0%, t 2.1) | 21.40 | 0 | 73.1% |

- Every flip is presented in both. The long gaps are the same: one a run, at ~89 s.
- The port now sets `SPU_IJMP_LOCAL=1` (`main.cpp`).

**The replay, repaired** (fork `runtime/spu/spu_jobcap.c`). Captures taken in the fight
(`SPU_JOB_CAPTURE_T0=90`) replayed with 14 of 120 jobs off their record, then hung. There were
three causes:
1. **The kernel poll.**
   - A job manager polls the SPURS kernel inside a job (`cellSpursModulePoll`). In the game the
     kernel answers.
   - A replay has no kernel and answered 0 ("workload 0"), so the job manager went another way.
   - The capture now records the answer after the line the poll copies to LS 0x100 (format
     `SPUJCAP2`), and the replay serves it.
2. **Channel counts.** A channel count (`rchcnt`) was compared with the record as if it were
   part of the request. Every non-zero count was a divergence, from the replay's first version.
3. **The stall status.**
   - In the game, reading `MFC_RdListStallStat` clears the stall mask. A read served from the
     record did not clear the live one.
   - The stall-and-notify event came back after the handler's acknowledge, and the handler ran
     again, deeper each time, until the stack overflowed.

Also:
- A run that has gone off its record stops after 64 more requests.
- The replay takes the files in name order (its sort compared pointers).
- `SPU_JOB_REPLAY_TRACE=1` prints every request.

Two new captures, from 90 s into the level:
- `logs/jobcap6`: General6 (w2), RenderJobs (w12), EDGE (w15); 40 jobs each.
- `logs/jobcap7`: Batch (w6), RenJobSet (w8-w11), w13; 30 each.

277 of 290 jobs come out as in the game. Batch's other 13 end before their record does, the
same in every build.

`PS3_SAMPLE_MAIN` cannot profile a replay: the sampler suspended the replay's thread while it
held the C runtime's lock, and both stopped.

**The loops' registers in locals** (2.12's "Next"; fork `spu_lifter.py --loop-locals`):

How it works:
- It applies to a single-block loop that ends in its one conditional branch back, with no other
  way out. Its SPU registers live in C locals:
  - loaded before its label, after its shufb preparations;
  - renamed inside;
  - the registers it writes stored back after the branch back, and in the pending-interrupt exit
    of a merged chunk entry.
- It skips the loops a native routine takes (`--loop-locals-skip`, from
  `scripts/spu_native_hooks.json`). Their lifted copy runs one iteration after the routine, and
  the copies in and out were all it did there.
- `--loop-locals-max` (`GOWA_LOOP_LOCALS_MAX`) caps the registers a loop may name. Not used.
- It covers 438 loops: 419 in the job modules, 19 in the pieces.

The native hooks:
- `gowa.py native_hooks` puts the routine's call ahead of a loop's loads in a merged function.
- The first build had it after them: the check looked for a 4-space indent, and the lifter
  writes 8. The routine then worked on registers the loop no longer read, and 14 of 40 EDGE jobs
  replayed wrong.
- A check now stops the lift if a call lands there.

`tools/test_spu_lifter_loop_locals.py`:
- It checks the shape, a loop with another way out, a loop without a store, the skip list and
  the cap.
- It builds the lift with and without the option and runs both on 2,000 random contexts:
  unmerged, merged, with an interrupt pending at entry and with one arriving mid-loop.
- Two mutants fail it: no store after the loop, and none in the interrupt exit.

Replay, best of three passes of ten runs a job. The replay's own spread on the large workloads
is +/-0.5%.

| workload | jobs | without | with | |
|---|---|---|---|---|
| RenderJobs (w12) | 40 | 527.5 us | 505.3 us | -4.2% |
| General6 (w2) | 40 | 852.2 us | 843.8 us | -1.0% |
| EDGE (w15) | 40 | 55.9 us | 56.3 us | +0.6% |
| Batch (w6) | 30 | 23.2 us | 23.0 us | -1.1% |
| RenJobSet (w8-w11) | 120 | | | -0.4% to +1.2% |

- General6 runs two kinds of job: `0xBF60` (-7.6%) and `0x81E8` (+0.6%).
- All 290 jobs come out as without it. Batch's 13 diverge the same way.

The versions on the way (the geometric mean of the per-job ratios):

| | General6 `0xBF60` | General6 `0x81E8` | RenderJobs | EDGE |
|---|---|---|---|---|
| every loop, every register stored back | -8.7% | -0.2% | -2.0% | +3.7% |
| only loops with a store; hooked loops skipped | -9.1% | +51% | -2.0% | ~0 |
| every loop but the hooked ones; written registers stored back | -7.6% | +0.6% | -4.1% | +0.4% |

- Two changes together took EDGE from +3.7% to +0.4% (the first row against the last):
  - the hooked loops skipped (`A950` names 81 registers);
  - only the written registers stored back.
- Leaving out the loops without a store made General6's `0x81E8` jobs 51% slower:
  - In that module this changes one loop, `0x5DD8`: a float loop of 40 registers with no store.
  - Why its absence cost so much was not traced.
  - So loops without a store can gain too. A likely reason (not verified) is the float forms'
    out-of-line exact paths, which make the compiler reload `ctx->gpr` as a store does.

Level, ABBA, three runs each (`logs/b-ll`):

| | flips/s | presents/s | frame clock thread |
|---|---|---|---|
| without | 21.60 | 21.65 | 72.7% of a core |
| with | 21.55 (-0.2%, t -0.3) | 21.61 | 72.1% |

- The SPU time it saves does not reach the frame rate.
  - RenderJobs' -4.2% is ~1.3 ms of its ~32 ms a frame (40 jobs of ~0.8 ms in the game).
  - On the frame's critical path that would be ~+2.8% flips/s. It is not there.
- Like 2.12's levers without a level gain, it stays off.
  `GOWA_LOOP_LOCALS=1 py scripts/gowa.py lift --jobmods-only` turns it on.
- The fallback, `7BB4` as native code, was estimated at +0.7% (2.16). That is below what an
  ABBA of this size can see, and it is SPU time too. Not done.

**Texture decode:** see `docs/notes/2026-11-02-fight-stutter.md`, "After P1-P4":
- Swizzled B8 is decoded a 4x4 tile at a time: 3.89 -> 0.44 ns a texel.
- The first spill chunk is made at boot.

**Next:** find what bounds the frame now. Use 2.7's probe again: slow each workload, the main
thread and the frame clock thread one at a time.

### 2.19 Update, 2026-11-03: what bounds the gate with the default patches

The default patches (Disable MLAA, Motion Blur and Shadows on, Mesh Trimming off) took the fight
to ~30 flips/s. The gate after it is the slowest place: Kratos idle at profile 2's checkpoint
there, windows from 10 to 90 s after the load, on a copy of the saves (the copy's saves were
unchanged after the ten runs).

**Each part made 25% longer** (`logs/probe-gate`, ABBA, two runs each):

| +25% on | flips/s | change |
|---|---|---|
| nothing | 15.38 (15.75 / 15.19) | |
| EDGE (`SPURS_WID_SLOW`, w14-16) | 13.55 (13.72 / 13.45) | -11.9% |
| General6 (w2) | 14.25 (13.61 / 14.59) | -7.3% |
| the frame clock thread (`RSX_SLOW=25`) | 15.12 (15.12 / 15.18) | -1.7%, within the spread |
| the main thread (`GOWA_MAIN_SLOW=25`) | 15.11 (15.09 / 15.11) | -1.8%, within the spread |

- The gate is bound by the SPU side: the six workers at 516-527% of a core (the policy modules
  594%), the main thread at 92-94%, the frame clock thread at 50%.
- General6 counts for more than its share of SPU time (15%): it is on the frame's chain.

**Where the SPU time goes** (`logs/prof-gate`, one run with the sampler, 14.04 flips/s):

| workload | share of a core | ms a run |
|---|---|---|
| EDGE w14, w15, w16 | 120-122% each, 3.6 cores: 61% of SPU time | 7.8-8.2 |
| General6 (w2) | 89% | 3.7 |
| audio (w7) | 46% | 2.1 |
| RenderJobs (w12) | 39% | 3.3 |
| RenJobSet (w8-10) | 37% between them | |

- EDGE takes ~234 ms of SPU time a frame here, against ~133 in the fight (2.16). For 30 flips/s
  all the SPU work of a frame has to fit 6 x 33 ms: about half of what it is at this view.
- **One function holds 23% of EDGE here:** `jobmod_e8f44fbc6dbf_e6D98_spu_func_000073E0`, the
  same in w14, w15 and w16. It is the per-vertex lighting stage, whose inner loop at `7BB4` was
  4.6% of EDGE in the level (2.11, 2.16). At the gate it is ~14% of all SPU time. Its inner loops
  are `7BB4` (~130 lifted lines) and `7ED8` (~100); which of them is hot needs a `--spu-lines`
  profile.
- Next in EDGE: `30600` (piece `0x755B80`) 8.8%, `D640` 5.9%, `302E8` 4.0%; the runtime's DMA
  and channels ~10%.
- General6: `4F00` 24.7%, `4AB0` 14.1%, `63B4` 6.3% (module `a67e549fa0ab_e41E8`), the rest in
  `1bdd67b54c3b_e7F60`.
- Audio: spread, no function above 3.9%.
- The frame clock thread: 60% of its samples outside the executable (the driver, waits).

**What follows for the options weighed with the user:**
1. Native code where the gate's time is: the lighting loops of `73E0` (2.16 estimated ~1.8x for
   `7BB4` as two elements to a 256-bit op), then General6's `4F00` and `4AB0`, then `30600` and
   `D640`. What the probe's slopes predict: ~+4-5% flips/s at this view for the lighting stage,
   ~+4-5% for General6's two, a few % for the rest.
2. A title patch for less per-vertex lighting, if the light count of a draw can be found where
   the PPU builds EDGE's jobs: research, with a visible cost.
3. Audio as native code: its time is spread over many functions; little for the work.
4. The frame clock thread: not what bounds the gate.
5. EDGE on the GPU (Phase 3.3): the only option of the size the gate needs for 30 flips/s
   (twice the SPU throughput), and the largest work.

The fight was not probed again with the default patches.

### 2.20 Update, 2026-11-03: EDGE's two lighting loops as native code

**Where in `73E0` the time is.** The gate profile's samples in `73E0` (19,148), mapped to the
lifted lines. There is no `--spu-lines` build: the module was compiled again with the build's
own flags plus line tables (DWARF), and its 7,660 instructions were checked against the exe's,
one for one.
- The loop at `7BB4`: 57.1% of the function.
- The loop at `7ED8`: 24.2%.
- The blocks around them: the rest, none above 6.1%.
- By operation: fm 19%, fma 17%, selb 8%, LS reads 7%, fa 7%, shufb 6%, fcgt 4%.
- So the two loops were ~19% of EDGE at the gate (13.1% and 5.6%).

**What they compute.** Both are software-pipelined: an iteration finishes one light and begins
the next. Each takes one light for eight vertices, two groups of four in SoA form: A (positions
r66/r62/r60, normals r52/r54/r56) and B (r65/r63/r61, r50/r53/r55).
- `7BB4` reads the lights from three arrays (position, colour, direction: r99, r97, r98).
  - It begins a light: `v = P.xyz - vertex`, `sq = v.x^2 + 1e-4 + v.y^2 + v.z^2`,
    `rs = fi(sq, frsqest(sq))`, `k = P.w`, `c' = r93 * C`.
  - It finishes it: `d = sq * rs`, `n = v * rs`. L is n, or the light's own direction where
    r100 > 16 * index (shufb by r9, which is one of two module constants: LS 0xF920 takes the
    first operand, 0xF8A0 the second).
  - Then `att = (t*t*(3 - 2t))^2 * c'.w` with `t = 1 - min(d*k, 1)`, and
    `h = min(|N.L * 0.5 + 0.5|, 1)`.
  - It stores L and `att * c'.xyz` (96 bytes a light each), and sums `|att|*h` and `L*|att|*h`
    (eight sums).
- `7ED8` takes the lights again from those two arrays, with a normal made from the sums:
  `l = (N.L > 0 as a word) ? min(N.L, 1) : 0`. It sums the positive and negative parts of
  `colour * l` (twelve sums). It only reads.
- A and B take the same operations on different registers, in the same order.

**Native** (`native/spu_edge_light.c`, `GOWA_EDGE_NATIVE` bits 32 and 64, hooks in
`scripts/spu_native_hooks.json`).
- A and B share one 256-bit register, so one op stands for two lifted ones, with their operands.
- The SPU float ops run under the SPU code's MXCSR with the +0 rule. A result at or past
  FLT_MAX has that op done again by the exact forms; frsqest and fi are spu_edge_decode.c's
  256-bit ports.
- It runs only where:
  - the splat controls, the two choices and r9 are what the module sets;
  - the MXCSR is the SPU code's;
  - the arrays are whole quadwords inside the local store;
  - `7BB4`'s two outputs miss each other and the light arrays read on the way.
- An interrupt the lifted loop would take stops it at that iteration.
- The last iteration stays lifted, from the registers each loop carries (31 and 23), written
  back as the lifted loop has them.

**Checked.**
- `tests/native/test_edge_native.py`: each loop cut out of its standalone lifted function up to
  the back edge, 3000 cases each, identical registers and local store.
  - Half the cases have plausible floats, half random bits (which take the exact forms).
  - Some cases break a control, misalign an output, put an output on a light array or have an
    interrupt pending.
- Mutants:
  - `7BB4`: 16 of 18 caught (one as a hang). The two left are a pending interrupt taken later
    (the same end state) and fcgt as >=, which is the same where r33 is 1.0, as it always is.
  - `7ED8`: 8 of 8 caught.
- Replay of 120 EDGE jobs captured at the gate with the loops lifted (`logs/jobcap-gate`, three
  runs each): 0 differ with them native. The loops ran there (36,939 native iterations each) and
  no exact op was needed.
- In the game: ~245M native iterations of each loop a run, no call left to the lifted loop,
  ~33K exact ops.

**The first build was slower** (gate, ABBA, two runs each: 14.70 -> 14.16 flips/s).
- clang-cl had not inlined `lt_fm` & co., which held their exact fallbacks.
- On Windows x64 a `__m256` argument goes through memory, so every float op was a call with
  its operands stored and loaded again. In the profile `lt_fm` alone was 7.3% of EDGE.
- Fix: the helpers are forced inline, and the fallbacks are cold functions of their own.

**Measured after the fix.**

| gate, ABBA, two runs each (`logs/light-gate2`) | flips/s |
|---|---|
| both lifted (`GOWA_EDGE_NATIVE=31`) | 14.85 (14.90 / 14.55) |
| `7BB4` native (`=63`) | 15.04 (15.16 / 14.75) |
| both native | 14.94 (14.80 / 14.94) |

- The profile (`logs/prof-light2`):
  - the two native routines are 12.1% of EDGE, against the lifted loops' 18.7%: 1.4x for
    `7BB4`, 1.9x for `7ED8`;
  - an EDGE run is 6.6-6.9 ms, against 7.8-8.2;
  - EDGE's SPU time a frame is 239 ms, against 256: -6.6%.
- The replay: the median job -6.8% (w14), -9.9% (w15), -2.3% (w16).
- The frame rate moved within the spread: +0.6% to +1.3%. The probe's slope (2.19) predicted
  ~+3%.
- **Kept, on by default.** The loops are exact, and they take ~7% of EDGE's work off the six
  SPU threads.

**What it teaches for the next native routines.**
- The lifted code of a loop is not much slower than hand-written code: a float op costs the
  same range check and +0 rule either way, and two groups to a 256-bit register saved less
  than half.
- What the native routines still pay:
  - the check after each op (and, compare, test, branch: ~22% of `7BB4`'s samples sit after
    those branches);
  - spills, with more live values than the sixteen YMM registers;
  - a statistics counter (`lock xadd` on one line from six threads, ~4% of the routine; now
    counted only with `GOWA_EDGE_NATIVE_STATS`).
- One check an iteration, with the work redone exactly when it fails (as `--float-spec` does
  for lifted functions), would take most of the first away.
- What a routine like this gains here is ~1.5x on its own time, ~1% flips/s at the gate. Of
  the native list in 2.19, General6's `4F00`/`4AB0` count for more per ms (General6 is on the
  frame's chain); EDGE's next functions are 4-9% each.

### 2.21 Update, 2026-11-03: General6's noise subroutine, its upper bound; what the gate waits on

**What General6's top functions are.** In module `a67e549fa0ab_e41E8`, the chain
`4930 -> 4AB0 -> 4E90 -> 4E9C -> 4F00` is one subroutine. The lifter split it at branch targets.
- It is an ABI function: `4930` saves the callee-saved registers and `4F00`'s end restores them.
- The code at 0x6898-0x68F8 calls it four times, with scaled coordinates in r3-r6, and sums
  the results in r3: four octaves of a 3D noise.
- Its hashing:
  - `cflts`, `& 255`;
  - word loads from a 256-word table at LS 0x4680, through the preferred slot
    (`lqx` + `rotqby`), chained as `p[p[X]+Y]+Z`.
- A call is ~860 SPU ops: shufb 196, LS reads 116, a 82, fa 80, shli 60, rotqby 56, andi 48,
  fm 44. Two thirds are integer, shuffle and loads.
- It is ~45% of General6 at the gate (`logs/prof-g6`; `4F00` alone 24.8%, `4AB0` 13.5%).
- `4F00` is already the lifter's `--float-spec` form. `4AB0`'s ops are already vector forms
  (`cflts` ~20 instructions, `rotqby` and the LS read a few each).

**The upper bound, measured before any native code.**
- An experiment build, not kept: `GOWA_G6_ORACLE=1` returned at `4930`'s entry with r3 = 0.
- Gate, ABBA, two runs each (`logs/g6-oracle`):

| | flips/s | General6 | the policy modules | EDGE, each workload |
|---|---|---|---|---|
| the noise | 15.38 (15.78 / 14.85) | 97% of a core | 593% | 116-117%, 177 runs/s |
| no noise | 15.83 (15.83 / 15.84) | 53% | 577% | 122-124%, 203-216 runs/s |

- With the noise free, the frame rate gained at most ~3%, within the spread of the two base
  runs.
- The workers had time left over ("empty" runs 12-13% -> 19-22%). EDGE took ~18 of the 45
  points General6 gave up.
- A native noise would gain a part of that, so it was not written.

**What the gate waits on** (`logs/diag-gate`, one run, 14.74 flips/s). Nothing is full:
- **The main thread:** 92%, of which half is `poll_mwaitx`, in the hooked loops of
  `scripts/ppu_hooks.json`: SPU progress counters and the job graph's entries.
- **The six SPU workers:** ~86% each.
- **The frame clock thread:** 52%.
- **The GPU:** its 3D engine at 27%, at 855 MHz.
- So the frame is the chain the main thread waits through, not any one resource's throughput.
- The probe's slopes (2.19) are not the gains of speeding a workload up:
  - slowing a workload lengthens the chain wherever its jobs are on it;
  - speeding it up shortens only those parts.
  - EDGE's lighting loops (2.20, -6.6% of EDGE's time) gave +0.6-1.3%. General6's noise for
    free gave at most ~3%.
- Identical runs at the gate differ by 4-6%. Small gains there need more than two runs a side.

**Next, before more native code:** the chain itself. Which jobs does the main thread wait for
each frame, and what do those jobs wait for? That means the job graph's entries, with each
job's start, end and workload against the main thread's waits.

### 2.22 Update, 2026-11-03: EDGE off the SPUs, phase 0: what the gate would gain

Option 3.3 (EDGE on the GPU) is weeks to months of work, so it was measured first. Gate as in
2.19 (profile 2, copy of the saves), the default patches, Steady audio on unless said.

**The gate is SPU-bound by throughput now.** Since Steady audio, SPU 5 runs the audio workload
only, and the other five workers carry the rest. A 15 s `PS3_FTRACE` (`logs/edge-p0/gate3.ft`,
202 frames, median 74.0 ms):

| a frame | SPU busy | a run open | width |
|---|---|---|---|
| EDGE w14-16 | 227.6 ms | 65.5 ms | 3.48 |
| General6 w2 | 66.8 ms | 37.8 ms | 1.78 |
| audio w7 (SPU 5) | 34.3 ms | 34.3 ms | 1.00 |
| RenderJobs w12 | 27.3 ms | 21.4 ms | 1.43 |
| RenJobSet w8-11 | 25.1 ms | 14.4 ms | 1.97 |
| MeshProc, Batch, the rest | 14.7 ms | | |
| all | 395.7 ms | | |

- 395.7 - 34.3 = 361 ms on five workers: 98% of 5 x 74 ms.
- The main thread: 25.4 ms of its own work a frame. The rest is waiting on SPU work:
  - 22.6 ms at `0x41DB50`, the frame's job-chain counter (mostly EDGE);
  - 13.8 ms in the job-graph scan;
  - 9.0 ms on the progress counters.
- The frame clock thread: 33-35 ms of CPU a frame (48% of a core at 14 flips/s).
- The host is a Ryzen 5 4600H, 6 cores and 12 threads. The six workers, the main thread and the
  frame clock thread are eight busy threads.

**Measured: EDGE's lighting stage skipped.**
- How: an experiment build, not kept. `GOWA_EDGE_SKIP_LIGHT=1` returned at `0x73E0`'s entry.
  - Its caller `0x51D0` calls it last, and `0x4B14` overwrites the result (r3) before `0x9080`.
  - So only the lit colours go. The draws and their triangles stay.
- Same executable both sides, one round each (`logs/edge-p0/skiplight`, `logs/edge-p0/steadyoff`).
  The first round differed by more than the bench's early-stop threshold.

| | flips/s | EDGE's SPU time a frame | policy modules |
|---|---|---|---|
| Steady audio on | 14.00 -> 16.02 (+14.4%), every window above | 216 -> 171.5 ms (-20.6%) | 535% -> 533% |
| Steady audio off | 14.84 -> 16.47 (+11.0%) | 234 -> 185 ms (-20.8%) | 593% -> 587% |

- The workers stayed full. The time EDGE gave up became frames.
- A throughput model: a frame is the SPU work of a frame over the workers that carry it, plus
  ~2% (1.2% with six). It predicts 16.05 and 16.9; measured 16.02 and 16.47.
- 2.20's native loops (-6.6% of EDGE) gave +0.6-1.3% where this model says ~+4%. That is
  within the gate's 4-6% spread between runs. General6's noise for free (2.21) gave less than the
  model: EDGE took part of the freed time in its own runs.
- Steady audio costs more here than in the fight: 14.84 -> 14.00 (-5.7%, one run a side; 2.5%
  there).

**EDGE at k times its SPU time now** (the model, Steady audio on):

| k | what | flips/s |
|---|---|---|
| 1.0 | now | 14.0 |
| 0.79 | lighting skipped (measured) | 16.0 |
| 0.5 | a native EDGE job, 2x | ~20 |
| 0.33 | a native EDGE job, 3x | ~24 |
| ~0.1 | EDGE on the GPU, a stub job filling the hole | ~31 by the SPUs |

- Other limits:
  - The frame clock thread's 33-35 ms of CPU a frame stops it at ~29-30.
  - The main thread's 25 ms stops it at ~39.
- A whole-job native EDGE is ~2-3x:
  - the runtime's ~30% of a run (2.2) goes;
  - the stages run at the loops' measured 1.4-2.1x.
  - That is ~+45-70% at the gate.
- On the GPU: ~+95-115%, with the frame clock thread next.

**A job-level oracle (EDGE for free) was tried; it needs phase 1 first.** What it found:
- **The job's parameters.** 120 gate jobs of the three lists agree:
  - The crt0 stores the job context at LS `0x10B00`.
  - The parameters are 96 bytes at [context + 0x20] (LS `0xC40`). The main (`0x5110`) copies
    them to LS `0x10650` before its first job-manager call.
  - +0x04: the command-buffer hole's EA (the job's one fenced PUT).
  - +0x4A: the hole's size in 16-byte units.
  - +0x40: the viewport (1280 x 720).
  - +0x00: the output ring's descriptor (its atomic line at +0x80).
- **The output.** Index and vertex PUTs go into the ring, allocated with GETLLAR / PUTLLUC.
  - Then the job fills the whole hole: state, the draw, a semaphore release, then a
    non-incrementing NOP (method 0x100) whose data runs to the hole's end.
  - The PPU started the hole with a self-JUMP.
- **The job and the Wws job manager**, the same in every job traced:
  - The main calls API +0x34 (`0x2FD8`) with (0, 1, 0, 1), which also fetches the next job.
  - Then `0x9990` (set-up), then a tail call to `0x44E0`.
  - `0x44E0` calls `0x2FD8` (0, 1, 0, 1), `0x2FD8` (0, 2, 0, 0), +0x3C (`0x17B8`) (1, 2) and
    +0x40 (`0x17C0`) (0x1E, 0).
  - Then the work: `0x51D0` (lighting last), `0x9080`, and the rest.
  - At the end +0x44 (`0x17E0`) with (0, label `0x20000C80` or `0xC90`, a ring position).
- **Why the cuts failed.**
  - A job that returned at the main's entry, or after its first call without `0x44E0`'s calls,
    left the job manager repeating one job-loading transfer until the SPU was halted.
  - One that skipped only `0x9990` failed an assertion in `0x44E0`'s LS allocator.
  - A skipped job has to make the calls a real one makes. Phase 1 has to work that out anyway:
    the GPU path's stub job is the same thing.

**Two notes on the tools.**
- `GOWA_WWS_TRACE` (the job lists' line events) slowed the main thread ~4x under `PS3_FTRACE`
  (110 ms of its own a frame). For frame timings, use `PS3_FTRACE` alone.
- Adding or removing a `native/` file changes CMake's glob.
  - `cmake --build` from a shell without clang-cl on the PATH then reconfigures with no compiler
    found.
  - The cache keeps empty `CMAKE_*_FLAGS`, so later builds have no /O2 (the gate at 8.7 flips/s).
  - `gowa.py build` configures with the right environment. The cache was repaired from a good
    one.

**Next, on a branch** (`edge` here, `gowa-edge` in the fork, once phase 1 touches it):
1. Phase 1: the job manager calls and EDGE's job format.
2. A whole-job native EDGE (2a), behind a switch, bit-exact by replay.
3. Then the GPU (2b).

### 2.23 Update, 2026-11-04: the gate at 16.9 flips/s; the audio SPU

After phase 2a's native code (EDGE's lighting, culling and transform:
[notes](2026-11-03-edge-stages.md)), what bounds the gate. 15 s `PS3_FTRACE` as in 2.22
(`logs/gate-now/gate1.ft`, 254 frames, median 59.5 ms; 2.22's numbers in brackets):

| a frame | SPU busy | a run open | width |
|---|---|---|---|
| EDGE w14-16 | 162.6 ms (227.6) | 47.9 ms | 3.40 |
| General6 w2 | 59.7 ms (66.8) | 32.6 ms | 1.84 |
| audio w7 (SPU 5) | 26.2 ms (34.3) | 26.2 ms | 1.00 |
| RenderJobs w12 | 25.3 ms | 15.9 ms | 1.76 |
| RenJobSet w8-11 | 24.2 ms | 11.0 ms | 2.47 |
| all | 311.7 ms (395.7) | | |

**Still the SPUs' throughput.**
- SPUs 0-3 are 97% busy (EDGE ~69% of each). SPU 4 is 96.6% (General6, RenderJobs, RenJobSet).
- (311.7 - 26.2) / 5 x 1.02 = 58.2 ms, against 59.5 measured.
- SPU 5 runs audio alone (Steady audio) and is 44% busy. That worker was the sampler's 10%
  "idle".
- The main thread does 22.6 ms of its own work a frame (a limit at ~44 flips/s). The frame
  clock thread takes 31 ms of CPU (~32 flips/s).
- The title's priority tables:
  - the EDGE lists: 3 on SPUs 0-3, 0 on SPUs 4-5 (maximum contention 8);
  - audio: SPU 5 only;
  - on SPU 5 also HighestPrio6 1, Batch 2, Network 3, RenJobSet 8, RenderJobs 10, General6 11,
    SysMisc 15.

**EDGE cannot use SPU 5.**
- Every EDGE job takes one per-SPU line with GETLLAR and puts it back with PUTLLUC, at
  EA = 0x73254380 + 128 x SPU.
  - Found by fitting the 120 captured jobs' DMA and atomic addresses against their SPU number
    (LS `0x1C8`): all three lists, every job, and nothing else depends on it.
  - The line is that SPU's output ring: start, end, current, and the RSX labels
    0x20000C80 + 16 x SPU.
- In the game, the descriptor at 0x73254300 holds lines for SPUs 0-3, and the lines of SPUs 4
  and 5 are zero. The rings take 4 x 2.75 MB, the second region 4 x 0x60000. (Probed at the
  job's get-SPU-number routine, `0xF770`.)
- EDGE on SPU 5 would write its output near address 0.
- The job manager's own per-SPU state is not the obstacle: a halfword per SPU in one quadword
  of the list header, room for 8.

**SPU 5 lent to others** (fork: `SPURS_LEND=<workload>[,...]`, beside `SPURS_RESERVE`).
- A lent workload may take the reserved SPU where the title's table gives it a priority there.
- It is never as strong as the reserved workload. The Wws job manager polls the kernel between
  jobs and gives way to a stronger workload, so audio waits at most one of the lent jobs.
- The jobs at the gate (`SPU_JOB_TIME`, now with p99 and the maximum):

| workload | mean | p99 |
|---|---|---|
| EDGE | ~100 us | < 512 us |
| RenJobSet | 58-100 us | < 512 us |
| Batch | 285 us | < 1 ms |
| audio | 135 us | < 1 ms |
| General6 | 1.24 ms | < 4 ms |
| RenderJobs | 0.72 ms | < 2 ms |
| MeshProc | 0.96 ms | < 4 ms |

The measurements, one build, ABBA, two runs a side. The audio columns are a kick of workload 7
waiting for a pick (`SPURS_KICK_PROF`):

| | gate flips/s | policy modules | audio kick, mean | audio kick, max |
|---|---|---|---|---|
| reserved | 16.78 (17.09 / 16.23) | 531% | 0.08-0.30 ms | 2.1-5.0 ms |
| + RenJobSet, Batch, HighestPrio6, Network, SysMisc | 17.02 (17.17 / 16.77) | 545% | 0.11-0.16 ms | 2.8-3.9 ms |
| reserved (second round) | 16.59 (16.62 / 16.32) | 531% | 0.12-0.29 ms | 2.5-4.6 ms |
| + General6 too | 17.51 (17.51 / 17.51) | 591% | 1.2-1.4 ms | 5.7-12.5 ms |

The scripted parasite fight with an empty shader cache, the case that had gaps before Steady
audio (`logs/gate-now/fight-lend`, `fight-lend-b1`):

| | flips/s | gaps | ring 1-2 blocks ahead (reports) | audio kick, mean | audio kick, max |
|---|---|---|---|---|---|
| reserved | 17.16 / 16.77 | 0 | 4 / 5 | 0.27 / 0.49 ms | 11.7 / 19.0 ms |
| + General6 & co. | 17.51 / 18.53 | 0 | 238 / 71 | 0.73 / 0.65 ms | 24.6 / 13.0 ms |
| reserved | 17.19 / 17.44 | 0 | 5 / 50 | 0.28 / 0.43 ms | 10.9 / 16.4 ms |
| + the short-job ones | 17.71 / 17.34 | 0 | 7 / 12 | 0.19 / 0.19 ms | 7.9 / 14.8 ms |

**The short-job workloads.**
- They have little to run in parallel: their runs are half empty.
- SPU 5 takes +10-15% of a core of their work. Frames +1.2-1.4%, within the spread.
- Audio is as before.
- **Kept, on by default with Steady audio.**

**General6.**
- Lent too, it fills SPU 5 (+60% of a core): +5.5% at the gate, +6% in the fight. That is what
  Steady audio off gives (2.22: +5.7%).
- Audio's kicks wait ~1.3 ms on average and 12-25 ms at most.
- The ring was only 1-2 blocks ahead 15-50 times as often, though without a gap in these runs.
- So that trade stays the launcher's switch. Steady audio now costs ~4% of the frames.

**Next at the gate:** the SPU work itself.
- EDGE: 162.6 ms a frame, 57%.
- General6: 59.7 ms, 21% (its noise subroutine, 2.21).
- RenderJobs and RenJobSet: ~25 ms each.

### 2.24 Update, 2026-11-04: EDGE's pieces, one at a time (toward phase 2b)

Phase 2b (EDGE on the GPU) needs every stage's output format known exactly. Phase 2a's method
gives that as it goes: each of EDGE's position-independent pieces is mapped field by field, made
native and exact, and checked call by call, by strict replay and at the gate
([notes](2026-11-03-edge-stages.md)). The order:
1. the output compression `0x755B80` (8.6% of EDGE);
2. the position-only output `0x765B70`;
3. the decompressions `0x75BD10` (3.8%) and `0x764D10`;
4. then `DCC0`.

**`0x755B80`, native** (`spu_edge_cmp.c`, `GOWA_EDGE_NATIVE` bit 4096).
- 54 bytes a vertex: the position and attribute 27 copied, the normal as I16N, the tangent as
  X11Y11Z10, attributes 28-30 as the piece's own F16.
- Also reproduced: the one junk quadword past the end, the frame, and r80-r83 and r3 as the
  piece leaves them.
- Checked identical in 315,392 calls in the game, and by strict replay.
- The lit jobs in replay: 77.9 -> 68.8 us (-11.7%).
- The gate, one build, ABBA, two runs a side: off 16.87 (17.05 / 16.58) -> on 17.82
  (17.55 / 17.96) flips/s (+5.6%).

**`0x765B70`, native** (bit 8192). The position-only output of the other two lists' jobs: x y z
copied, 12 bytes a vertex.
- Also reproduced: one junk quadword past the end (8 zero bytes, then the output's last 8).
- Checked identical in 335,875 calls in the game, and by strict replay.
- Replay: w15 -4.7%, w16 -3.6%.
- The gate, ABBA: off 17.56 (17.70 / 17.48) -> on 17.91 (17.72 / 17.91) flips/s (+2.0%, within
  the spread).

**`0x75BD10`, native** (`spu_edge_vtx.c`, bit 16384). The lit jobs' vertex unpacking: 18-byte
records of a position and a 48-bit code.
- The code holds two unit vectors (attributes 3 and 4), each as two 10-bit components and a
  pattern of 12. The third component is a square root (`frsqest`/`fi`). The pattern gives its
  place and sign, and a handedness w.
- A pattern index past 11 reads the caller's stack; the routine does those pairs with the
  piece's own operations.
- Checked identical in 188,416 calls in the game, and by strict replay.
- Replay: the lit jobs -6%.
- The gate, ABBA: off 17.95 (17.90 / 17.95) -> on 17.91 (17.71 / 18.10) flips/s, unchanged within
  the spread.

**`0x764D10`, native** (bit 32768). The same records for the other two lists' jobs, positions only.
- Written from the code, without a probe. Identical in 172,032 calls in the game, and by strict
  replay.
- Replay: w15 -6.5%, w16 -5.5%.
- The gate: 18.50 off, 18.29 on, within the spread.

**The four together.** One build, three variants alternated, two runs each (`logs/edge-cmp/ab4`):
- `GOWA_EDGE_NATIVE=4095` (the four off): 16.78 (17.12 / 16.68) flips/s;
- all on: 18.29 (18.25 / 18.38) flips/s, +9.0%.

**`DCC0`, the index decompression, native** (`spu_edge_idx.c`, bit 65536; `0xE4D0` whole, every
job). Mapped stage by stage from snapshots at each loop head, then by a model that matched the
whole local store after the call.
- The format:
  - a k-bit reuse stream, decoded as eight interleaved delta sums;
  - a bitmask that chooses, for each vertex slot, a reuse or the next new vertex;
  - 2-bit triangle codes: 3 is a fresh triangle; 0-2 turn on one edge of the last triangle,
    reversed, with the next vertex.
- Its scratch past the indices, which goes out by DMA, is made byte for byte.
- Identical in 561,152 calls in the game, and by strict replay. With the k = 0 streams added (the
  piece zero-fills there), 536,576 calls identical and 3.2% declined, from 8.6%.
- 9,154 -> 4,737 cycles a call. Replay: w14 -1.5%, w15 and w16 -5%.
- The gate, ABBA: off 18.14 (17.87 / 18.30) -> on 18.71 (18.54 / 18.89) flips/s (+3.1%).

**Step 1 done.** Every EDGE piece the jobs call at the gate, and the index decompression, is now
mapped and native. Next for phase 2b is step 2: the job's output contract (what it hands the
RSX: the hole, the rings, the labels) and a stub job that produces it.

Step 2 so far ([output contract](2026-11-04-edge-output-contract.md)):
- **The output side** is mapped: the ring allocation, the ring data, the fragment program, and
  the hole's methods.
- **The input side** is mapped from 280 captured jobs and a replay that logs every local-store
  access by function:
  - how the double buffering hands a job its inputs;
  - the DMA list's 22 fixed slots and the stage that reads each;
  - the descriptor's words;
  - the light set;
  - the vertex formats, and how they and the descriptor's attribute slot list make the hole's
    vertex arrays.
- **Still open:** a few descriptor words and records that reach no output, and some stages'
  insides (`0x67C0`, `0x61B0`, `0x9A60`, the skinning formats).

The design for a C implementation of the whole job ([host reference](2026-11-04-edge-host-reference.md)):
- **The measurement.** At the gate in the game, a position-only job is 35.8 µs: natives 8, the
  job manager 10.5, glue ~17. A lit job is 113 µs: natives 57, the job manager 12, glue ~42. Lit
  jobs are 77% of EDGE's time.
- **The design.** One core works on a local-store image in the job's own layout and calls the
  existing natives. It is used in the SPU job (exact, junk included), in the replay with a new
  compare mode, and later as step 3's oracle.
- **The order of work.** It is built bottom-up, glue function by function, as in step 1.
- **The expected gain.** About -30% of EDGE's SPU time in the SPU job, so 10-15% at the gate.
  The job manager's 10-12 µs a job, mostly its prefetch DMA, is a separate target in the
  runtime's MFC path.
- **Progress.** The harness (the replay's state compare mode, a hook at the job's entry) and
  the output stage `0x9080` are done. The output stage is native and on by default
  (`GOWA_EDGE_NATIVE` bit 131072): 640 replayed jobs with 0 differences and about 2 µs less a
  job in replay. At the gate it is within the noise (18.83 off, 18.86 on, ABBA). Details are in
  the host reference's progress section.
- **Then the runtime first** ([hot paths](2026-11-04-spu-runtime-hot-paths.md)).
  - Measured in the game, the lifted glue left was 12.7% of the EDGE workers' time and the
    runtime's own machinery 26.7%.
  - The first changes in the fork:
    - one switch for the DMA path's and the channels' diagnostics;
    - the MFC channels without the engine lookup;
    - a false-shared cache line taken apart;
    - the interrupt handler's save and restore stubs run from a decoded copy;
    - a page filter on the GET observer.
  - Replay: 820 jobs with 0 differences (strict where the record allows).
  - Gate, ABBA: 18.73 -> 19.19 flips/s (+2.5%).
- **Then back to the glue** (the host reference's progress, item 3), the largest lifted pieces
  first.
  - Six interrupt continuations in the EDGE module were not lifted entries, and every jump to
    them went through the micro-step (four of them about 124,000 times each in 20 s). They are
    entries now: the micro-step went from 0.67% to 0.43% of the EDGE workers' samples.
  - The lighting stage's set-up is native (`GOWA_EDGE_NATIVE` bit 262144, on by default): the
    loops of the clear `0x5D90`, of the scatter `0x75D8` and of the running sum `0x7670`.
    - 1180 replayed jobs with 0 differences, and the stage checked call by call.
    - Replay of the gate's jobs: -1.9%.
    - In the game, `0x73E0` and `0x5D90` went from 2.97% to 1.40% of the samples, plus 0.87%
      for the natives.
  - Gate, ABBA, three runs a side, both steps against the module before them: 19.02 -> 19.19
    flips/s (+0.9%), within the noise of ±0.4 between runs.
  - The vector normalization `0xBBC8` (twice in every skinned lit job) is native too
    (`GOWA_EDGE_NATIVE` bit 524288, on by default).
    - Every call checked in replay (188 calls) and in the game (139,265 calls), and 1360
      replayed jobs, with 0 differences.
    - Replay: -0.4% and -0.7% on two sets of 180 jobs from the game.
    - In the game it went from 1.23% to 0.77% of the EDGE workers' samples (0.57% of that in
      the native).
  - Then `0xE550`'s two lifted loops (the matrix layout `0xE804`, and the skinned lit jobs'
    normal and tangent transform `0xF104`; bit 1048576) and `0x88B0`'s two table loops (bit
    2097152), all on by default.
    - Every call checked in replay (378 and 1719 calls) and in the game (90,114 and 413,696),
      and 1720 replayed jobs, with 0 differences.
    - In the game `0xE550` went from 1.63% to 1.07% of the EDGE workers' samples, and `0x8930`
      from 1.20% to 1.00%.
    - The first `0xF104` native saved nothing until its pipeline left the stack. The context
      loops cost mostly their tables' first touches in the game, which a native makes too.
    - Where the time goes inside a lifted function now comes from a `--spu-lines` build
      (`build-ln`), each sample put on the nearest label above its lifted line.
    - Gate, ABBA, three runs a side, steps 2-4 off against on: 18.84 -> 19.19 flips/s (+1.9%),
      ahead in all three pairs; the first run (off) started slow, so likely nearer +1%.
- **Phase 2b is planned** ([phase 2b plan](2026-11-04-phase-2b-plan.md)), and its progress is
  kept there.
  - Native draw in the backend comes first, then the stages move to compute shaders one at a
    time, lighting first.
  - A stage on the GPU is checked against its native routine within a tolerance (agreed
    2026-11-04).
  - 2b.1, lighting on the GPU, done 2026-11-04: the D3D12 backend's vertex pass lights EDGE's
    draws, the SPUs skip the stage. Gate, ABBA, two rounds: 19.41 -> 20.69 flips/s (+6.6%). On by
    default (`GOWA_EDGE_GPU=0` turns it off).

### 2.25 Update, 2026-11-05: what sets the gate at ~20.7 flips/s; audio on a worker of its own

The render thread (phase 2b plan) took the frame clock thread from 73% of a core to 41% and the
frame rate did not rise, so what does set it? The first answer written down, the main thread at
94%, was wrong: that figure is mostly waiting, as L8 found on 2026-10-28.

**The main thread** (the sampler, 75 s of the gate, `logs/edge-ana/now.rvas`): `poll_mwaitx` 22%,
the EDGE counter's loop `func_0041CBDC` 12%, `sys_timer_usleep` 6%, waits under indirect calls 5%.

**Each part made 25% longer** (`logs/edge-ana/probe2`, one build, two rounds, base spread out
over four runs):

| | flips/s | change |
|---|---|---|
| nothing | 20.67 (20.88 / 20.41 / 20.46 / 20.88) | |
| EDGE (`SPURS_WID_SLOW`, w14-16) | 19.36 (19.48 / 18.98) | -6.3% |
| the main thread (`GOWA_MAIN_SLOW=25`) | 20.04 (19.84 / 20.21) | -3.0% |
| the frame clock thread (`RSX_SLOW=25`) | 20.33 (20.33 / 20.33) | -1.6% |
| Steady audio off | 21.66 (21.30 / 21.71) | +4.8% |
| audio on a worker of its own | 22.07 (22.66 / 21.88) | +6.8% |

**So the SPU workers set the frame.** A 15 s trace (`logs/edge-ana/now.ft`):
- SPUs 0-3 busy 89%, SPU 4 80%, SPU 5 64% (52 points of it audio);
- five or six runs other than audio open 75% of the time; none 6%, one to three 7%;
- ~252 ms of runs a frame at 48 ms: what is not audio, over five workers, is the frame.

SPU 5 is the capacity left. Steady audio kept it for audio and the short jobs, because the title
lets audio run on SPU 5 only and a job is not interrupted.

**The fix: a host worker for audio** (fork, `SPURS_LANE=<wid>:<spu>`; the port's `steady_audio`).
- A PS3 has six SPUs, the host twelve logical CPUs. Workload 7 gets a seventh worker, as SPU 5,
  and SPU 5's own worker takes what the title's table gives it otherwise (General6, RenderJobs,
  RenJobSet, Batch and the small lists).
- A policy module's context is per workload and SPU (`spu_run_policy_module`), so nothing in it
  changes: on a PS3 the two would take turns on that SPU, here they run at once.
- The kernel's answers follow: `spurs_claim` and `spurs_kernel_poll` tell the two workers apart,
  so a General6 job on SPU 5 no longer yields to audio.
- Audio no longer waits behind another workload's job, as with the reserve, and no SPU is kept
  for it.

**Measured**, with `AUDIO_RATE` on:

| | reserved | audio's own worker | |
|---|---|---|---|
| the gate, ABBA (`lane-gate`) | 20.67 (20.72 / 20.43) | 22.98 (23.26 / 22.91) | +11% |
| the gate, its gaps | 0, 0 | 0, 0 | ring 1-2 blocks ahead: 4-8 against 4 |
| the scripted fight, warm cache (`fight2`) | 20.35 (20.41 / 20.28) | 21.26 (21.37 / 21.14) | +4.5% |
| its gaps | 1, 0 | 0, 0 | |
| the fight, empty shader cache (`fight1`) | 19.09 (19.12 / 19.06) | 18.92 (19.06 / 18.77) | the same |
| its gaps | 0, 2 | 5, 0 | the 5 at the level's load; ring 1-2 blocks ahead 24, 60 against 239, 210 |
| the main menu (`menu-ab`) | ~51.5 | ~54 | +5% |

- All the gate's runs, the five with audio's worker against the six without: 20.6 -> 22.7
  flips/s (+10%). The workers at 598% of a core (seven), the frame clock thread 82%, the main
  thread 90%, the process 860%.
- An empty shader cache is the one case not better: the frame rate is the same, and audio's
  margin is thinner when the level loads. Audio's own runs are no longer: 600 us against 690.
- The picture is the same (screenshots at the gate, the menu and the fight).
- Steady audio off is now only the title's own scheduling: 21.95 at the gate.

**A second lane adds nothing:** General6 on a worker of its own as SPU 4 (`SPURS_LANE=7:5,2:4`):
22.86 (22.89 / 22.71). The extra width is not there to take.

**What misled on the way.**
- A first batch of these probes, stopped by low memory with Windows servicing running, read
  19.18 for base and -9% for the frame clock thread. The base ran once and the machine drifted
  under it: without base spread over the batch, slopes like these mean nothing.
- `PS3_FTRACE` now slows the whole run, not its 15 s (15.8 flips/s in every level window, all
  workloads but EDGE 20-30% longer than 2.23): its shape holds, its milliseconds do not.
- The sampler over six workers shows `(wait) spurs_pm_worker` at 39%: threads suspended while
  they hold the instance's lock make the others wait. The workers' own CPU, 525% of 600%, is the
  figure.
- `SPURS_KICK_PROF`'s "to a pick" for audio covers 20-40 kicks a second: ~900 of its runs a
  second are the coordinator's probes.

**Next:** the frame clock thread is close now, 82% at 23 flips/s (a limit near 28). The SPU work
itself stays the frame: EDGE first (phase 2b).

### 2.26 Update, 2026-11-05: options A and B of the EDGE review

Options A and B of the [review](2026-11-05-edge-gpu-review.md), each measured alone, base spread
through the batch, at the gate and in the warm fight.

**The sum.** The gate, three runs a side, everything off against the defaults
(`logs/edge-ana/ab-final`): 22.96 -> 23.80 flips/s (runs 22.84, 22.91, 23.17 against 23.85, 23.75,
24.05), still without the binormals left out (see A); with them, 24.13. So 22.96 -> ~24.1, +5%. The
warm fight does not move (21.17 and 21.14 against 21.47, 20.25, 21.33, 21.27), measured before the
binormals' fix. The menu does not either.

**A: what lighting's move left behind** (port `03f66f8`). With the stage skipped, nothing read the
binormals or ids 27-30.
- A hook at `0xA928` leaves `0xA880`'s loop out when the stage driver calls it. It runs after all,
  from the arguments the hook kept, unless the stage ends up skipped: at the stage's entry if the
  stage is declined or runs late, and before the packing if the driver does not call the stage (no
  lights). Only a skipped stage proves the lit layout: ids 27-30 join the list after `0xA880`.
- The output piece packs bytes 0-23 of a vertex. Bytes 24-53 are whatever the local store held; the
  pass writes all of them, for every vertex of a segment.
- `GOWA_EDGE_GPU_LEAN=0` keeps both.
- Checked: `GOWA_EDGE_GPU=2 GOWA_EDGE_GPU_DEFER=1` over the ten corpora, 0 differences, with the
  hook counting: 42 of the gate's 120 jobs left the loop out, and one of them ran it after all.
- **The first version left nothing out.** It tested the id list at the hook, where ids 27-30 are not
  in it yet, so the test always failed, the replays passed with nothing to compare, and the first
  commit's claim was wrong. The sampler showed it: `0xA950` still 4.3% of EDGE. Its bench,
  `logs/edge-ana/lean1`, is the packing alone: 22.48 -> 22.96 flips/s (four runs a side; runs 22.17,
  23.01, 22.99, 22.22 against 22.73, 22.96, 22.73, 23.06), near the spread.
- The binormals left out as well, on top of B, three runs a side, `logs/edge-ana/lean2`: 23.48 ->
  24.13 flips/s (runs 23.66, 23.26, 23.57 against 24.17, 23.62, 24.34). The same screenshots by
  region.

**B: room on the frame clock thread, then culling on the RSX.** Frame clock time a flip, at the gate
(`logs/edge-ana/room1`, three runs a side):

| | flips/s | frame clock, ms a flip |
|---|---|---|
| base | 22.57 | 36.1 |
| the constant bank by generation | 22.50 | 35.7 |
| and a draw's pieces in one call | 22.77 | 34.1 |
| and no culling on the SPU | 23.08 | 35.4 |

- **A draw's pieces in one call** (`RSX_DRAW_RUN=0` turns it off). `DRAW_INDEX_ARRAY` words wait in
  `rsx_state` while they follow one another in the index array, and go to the backend as one draw at
  the next other method, the END at the latest. Triangles only. Ring draws per second: 133,800 ->
  35,000.
- **The constant bank by generation** (`VP_CB_GEN=0` turns it off). `rsx_state` numbers the bank's
  contents; a word that changes takes a new number. Five records in six have the bank of the one
  before, and now neither copy it to the image nor compare it. The holes write a constant four
  times a record, with the words already there, so the number moves only on a real change: 30,700
  of 30,708 shared records a second by generation.
- **No culling on the SPU, on by default** (`GOWA_EDGE_NOCULL=0` for the old way, the default in a
  replay, a capture and `GOWA_EDGE_VERIFY`). SPU worker time a flip -3%; screenshots the same at the
  gate and in the fight; `VTX_STATS` shows every ring draw culling back faces in the fight too. A
  scene that draws EDGE's output with the RSX's culling off would show back faces.

**What this corrects.**
- The review put culling's cost on the frame clock thread at 16 points. That was the pieces: 3,300
  more a frame. With a draw in one call it is +1.4 ms a flip at the gate, ~3 points.
- The sampler's `(wait) vp_record_cb`, 6.6% of the thread, was not the 8 KB copy: leaving that out
  in 84% of the records gave 0.4 ms a flip. A `(wait)` name is the first return address into the
  executable on the stack, and it can be a stale slot.
- `(wait) rsx_d3d12_backend_pump_messages`, 44% of the samples, is the idle wait in
  `cellGcm_fifo_kick_wait`, under a stale return address from the pump a loop before. The thread's
  own table says the same: ~80% busy, and the sampled run was slower.

### 2.27 Update, 2026-11-05: EDGE's job results, kept

Option C of the review went another way: [EDGE's job results, kept](2026-11-05-edge-job-results.md).
With culling on the RSX a job's output no longer depends on the camera, and 95% of the unskinned
jobs repeat one already done, so a result is kept and the stages left out. All of A, B and this: the
gate 22.89 -> 24.40 flips/s, the warm fight 21.17 -> 21.56, EDGE 114 -> 87 ms of worker time a frame.

### 2.28 Update, 2026-11-05: General6's noise reviewed

[The review](2026-11-05-general6-noise-review.md). In the levels the record sets the noise's scale to
zero, so every call of `0x4930` computes the noise at (0, 0, 0, 0). A hook returning 0 for zero
arguments (`native/spu_g6_noise.c`, on by default): the gate 24.20 -> 25.63 flips/s, the warm fight
21.20 -> 23.15, General6 halved, and the output the same. 2.21's "at most ~3%" no longer holds: nothing was
full then. With it the gate is set by the chain through the main thread and EDGE (+25% of either:
4-5%), the frame clock thread close behind at 92%, and the render thread gives nothing yet.

### 2.29 Update, 2026-11-06: the main thread

[The review](2026-11-06-main-thread-review.md). The fight is bound by the main thread: 25% more of its
work costs 14%, 25% more of anything else nothing. Its time is 72% lifted PPU code in a flat profile,
so what pays is the code every function shares. `PPU_MM_LEAN`, two rounds, took the executable's
`.text` from 111.9 to 93.1 MB: the warm fight 23.0 -> 24.8 flips/s, the gate 25.4 -> 26.4. Next is lazy
CR and XER, about 13% fewer instructions in the lifted code.

A baseline in a bench goes in a folder of its own under the name `GowARecomp.exe`, with a copy of
`build/gowa.ini`: the same binary ran ~2% slower in the fight as `m1.exe`.

### 2.30 Update, 2026-11-06: EDGE on the GPU, second review

[The review](2026-11-06-edge-gpu-review-2.md). With the main thread faster, 25% more EDGE costs 6% of the
frames in both scenes. A job with a kept result does no math and still costs 34 or 54 µs: those jobs
are 49-63% of EDGE's time, and the step around the job manager's `0x17E0` is 12.5 µs of every job. So
moving stages stays poor. What the GPU can take is the geometry itself: kept results in GPU memory,
then skinning from cached inputs.

### 2.31 Update, 2026-11-06: lazy flags, and EDGE's geometry in GPU memory

All under one executable name, two runs a side; [the second review](2026-11-06-edge-gpu-review-2.md) has
the details.

| | gate, flips/s | fight, flips/s |
|---|---|---|
| lazy CR and XER | 26.41 -> 26.77 | 24.56 -> 24.90 |
| G, kept results in GPU memory | 26.44 -> 29.09 | 25.12 -> 26.11 |
| K, skinned jobs from kept inputs, skinning in the vertex pass | 28.84 -> 29.98 | 26.16 -> 27.39 |

- **Lazy flags** gave less than estimated (+1.4% against 3-5%), **G** far more (+10% and +4%, against +2%
  and +1%): the gate was held by the frame clock thread.
- **The gate is at the game's 30 flips/s** in half of its windows.
- **The fight** now has the main thread at 91% and the workers at ~547%, the frame clock thread at 68%.
- **A bench trap.** The same binary runs ~2% slower in the fight under another name than
  `GowARecomp.exe`. A baseline goes in a folder of its own, under that name, with a copy of `build/gowa.ini`
  (a new tree's has only paths: no FSR 1, no SMAA).

### 2.32 Update, 2026-11-06: where the workers' time goes

[The third review](2026-11-06-worker-time-review.md). The gate is at the game's 30 flips/s. The fight sits on
two levels, ~27.6 and ~25.5 flips/s, with General6 at 49 or 66 ms a flip; timing brings the slower one on.
A millisecond of General6, RenderJobs or RenJobSet is worth about twice one of EDGE. Next: `memset` and two
small job manager loops native, then General6's routines. S and J after that, +3% to +3.5% each.

Done since: `memset`, the job manager's loops and five of General6's routines, written by
`scripts/spu_to_native.py`. The work went down by a fifth, the fight by +1.5% (27.65 -> 28.06), against the
+6% estimated. S and J look like +1.5% and +2.4% now, for weeks each, and are not started.

### 2.33 Update, 2026-11-06: what the GPU can still take

[The same review](2026-11-06-worker-time-review.md#what-the-gpu-can-still-take). For easy to medium work EDGE to
the GPU is done: what is left of EDGE is the job around the math, and the rest of the SPU work is integer code
or feeds the game. The fight's levels are not waiting: the slower one is 56% more of General6's work, the
game's own (`0xA588` 12,600 to 19,600 calls a flip), with the threads 98% on a processor. The noise job's
tile has 32 different pixels in 2,048, so its pixel loop makes one pass in 16: General6 -5.4 ms a flip, about
+1%.

### 2.34 Update, 2026-11-06: an idiom taken for a ladder

The target is now the fight at 30 flips/s. Looking at the job manager's time ("J") found a bug in the tools.

`brsl $2, .+4; ai $2, $2, 8; bid $2` is how the SDK turns interrupts off and goes on at the next instruction,
around every DMA of a job. `find_spu_functions` took it for a Duff's device, a computed jump into a ladder, and
seeded 17 functions after each one. So the code that sets up every DMA ran one instruction a function, with
an interrupt check each, and each idiom went through the dispatcher: 31 of an EDGE job's 67 indirect branches,
80% of a RenJobSet job's. The "flat code" of the third review was largely this.

- A ladder needs a register add on the way to the `bi`; without one only the target is seeded.
- `compute_pc_jumps` lifts the idiom as a plain branch, with its D or E bit. Enabled interrupts are taken at the
  target as before. EDGE's module goes from 1,086 functions to 592, with 4 indirect sites left.
- The job manager is lifted with `--merge-chunks` too (`GOWA_PM_MERGE=0`: not).
- `SPU_RdDec`, five times a job, did six 64-bit divides: now one multiply on the clock's own counts.

| replay, µs a job | before | after |
|---|---|---|
| RenderJobs | 541 | 450 |
| RenJobSet w8, w9, w10, w11 | 24.7, 44.7, 42.9, 547 | 19.4, 37.3, 34.2, 491 |
| MeshProc | 4.1 | 3.0 |
| EDGE w15, w16 | 19.9, 20.9 | 18.8, 19.6 |
| General6 | 818 | 800 |

Every list the same (w6's 13 differences in `logs/jobcap7` were there before).

| the fight, one exe name, medians of the windows | flips/s | all workers, ms a flip |
|---|---|---|
| before | 27.99 | 197 |
| after, two runs | 30.18, 31.41 | 184, 167 |

The two runs differ by the game's state: General6 and EDGE move with it. Over all three, flips/s follows the
workers: 166 ms gives 31.4, 170 gives 30.3, 178 gives 29.1, 187 gives 28.4, 197 gives 28. The gate: 31.55.

Then, in the same direction:
- `SPU_RdDec` on the time stamp counter: `QueryPerformanceCounter` took ~150 ns here, five times a job.
- A DMA list's elements prefetched before the first copy: a job's input is a dozen small cold blocks.
- `--loop-locals` on by default (`GOWA_LOOP_LOCALS=0`: off). It was left off in 2.18 because the frames did not
  follow; now they do.

| the fight, 5 runs (`logs/edge-ana/j4`, `j4b`) | median flips/s | lowest window |
|---|---|---|
| | 31.95, 30.62, 31.06, 30.27, 31.26 | 28.7 |

- From t=100 on every window is at 30.2 or more. The windows at t=70-90, the start of the fight, go down to
  28.7-29.6: General6 and audio are at their highest there. One run lost its last 30 s to Firefox's updater (a
  service stop in the system log at that second; every workload's ms a flip went up by the same quarter).
- There is no cap at 30: windows reach 32.3, and the gate is at 31.6. The third review's "at the game's 30
  cap" was wrong: it was the frame clock thread and the workers.
- **The levels look like a feedback.** General6 is ~920 ms a second in the windows above 30 flips/s, whatever
  the flips, and ~1,215 ms a second on the slow level: two rates of the game's own work, 1.4-1.5 apart, and
  the fast one has now held from the start in most runs. The slower the frames, the more physics a second.
  That fits sub-steps of a fixed time, 3 a frame against 2 past 33.3 ms, but that was not read in the game's code.
- The main thread has room now: 27% of its time is waiting (`poll_mwaitx`, `sys_timer_usleep`).

Eight runs of this build, 77 windows from t=70: median 30.6, 71% at 30 or more, 92% at 29 or more, against 0%
and 5% before. Two runs lost 20-30 s to the host: Firefox's updater, then Windows Update, each at the second the
system log says. Runs differ by ±1 flip/s with nothing changed: every workload's ms a flip moves together.

Tried and dropped:
- **The local store at its fixed place in the context** (`ls_store`, not `ls`), so that a store can be told
  from a guest register. 3% fewer instructions in one function, the replay the same, General6 5% slower: the values
  go to stack slots, as with the PPU's `__restrict`.
- **A worker watching its slot with MWAITX before it sleeps.** 31.80 flips/s without, 30.12 with 40 µs, 29.48 with
  150 µs. The ~27 µs "wake" did not move: it is the coordinator's lock, held for its pass, not the OS. So
  frames cost more than the cycles they use: on this laptop more waiting threads means slower ones.
- **A longer back-off for idle probes** (`si->idle` up to 6, 8): the same run rates. The ~5,600 empty runs a
  second are asked for (ready count above contention), not probes.
- **`sync` and `dsync` as a compiler barrier:** no change in the replay.

What the frame looks like now (12 s of `PS3_FTRACE`, `logs/edge-ana/j9.ft`): 372 frames, median 31.9 ms, a chain of
phases. EDGE ~10 ms, RenJobSet ~6, RenderJobs ~8, MeshProc and Batch ~3, EDGE ~10 with General6 beside it.

| ms a frame | SPU busy | with a run open | width |
|---|---|---|---|
| EDGE | 54.5 | 16.7 | 3.28 |
| General6 | 33.8 | 10.8 | 3.13 |
| RenJobSet | 23.1 | 5.6 | 4.22 |
| RenderJobs | 20.3 | 4.7 | 4.34 |
| Batch | 11.7 | 2.3 | 5.05 |
| MeshProc | 3.6 | 1.3 | 2.98 |

EDGE is half of the chain now, at width 3.3, and its limit is the title's: priority 0 on SPUs 4 and 5, and ring
lines for SPUs 0-3 only (`0x73254300 + 0x80 (spu + 1)`). The label array has room for six SPUs
(`0x20000C80 + 16 spu`, the next array at `0xCE0`). SPUs 4 and 5 stand idle in the first EDGE phase. Letting EDGE
use them needs two more ring lines.

**Tested 2026-11-06, dropped: width is not what the frame waits for any more.** EDGE on SPUs 4 and 5 at SPU 3's
priority, sharing the rings of SPUs 2 and 3 under a lock around the allocation (4,882 waits in 5.8 million
allocations, none long). One fight each, after Windows was restarted, the windows from t = 130 s to 170 s:

| | flips/s | EDGE ms a flip | all workers ms a flip | fragment pipelines |
|---|---|---|---|---|
| as the title has it | 35.7 | 46.2-47.8 | 146-152 | 348 |
| EDGE on SPUs 4 and 5 | 35.3 | 47.2-47.4 | 152-153 | 1,170 |

- The picture stayed whole in the two shots taken.
- At this rate the process keeps 8.3 logical CPUs busy of the 12, and the main thread is 96% busy. The work is the
  same, so more of it side by side only moves it.
- **Fragment pipelines tripled with no new scene,** and 409 programs were compiled on a warm cache. With SPU 4 alone
  it was 2,169 and 654, and that run, at 25-29 flips/s, was stopped by low memory at t = 140 s, so its rate says
  nothing. The pipeline file went from 12 to 54 MB. Sharing a ring leaves its label out of order, which may let
  something be overwritten early, but this was not looked into. It would have to be understood before any further
  try, with rings of their own or not.

A position hit is 30.3 µs now, a lit one 38.8, the job manager 15 of either (269 labels, was 357), of which
the input list's copies are 4.3.

### 2.35 Update, 2026-11-06: what the GPU can still take, third look

[The review](2026-11-06-gpu-review-3.md), at 37.6 flips/s. EDGE to the GPU is finished: native EDGE code is 13% of
EDGE and the runtime under the jobs 48%. Nothing else of size can go. Bindless textures are new and small (~1.6 ms
of the RSX thread, weeks). The cheapest time left is not GPU work: `tex_csum`, `PeekMessageA` and
`rsx_texture_layout` were 22% of the RSX thread. Done: gate 33.3 -> 35.3, the fight unchanged at 34.2 on a warm machine.
Heat sets the fight's numbers: a first run on a cool machine reads 36-39, one straight after another 32-36. In the fight,
compare runs from the same place in a chain.

### 2.36 Update, 2026-11-10: RPCS3 against the port with EDGE on the CPU

[The comparison](2026-11-10-rpcs3-vs-port-cpu.md), the Megaera fight, the same picture, EDGE's GPU steps off:
- **FPS.** RPCS3 18.5 against the port's 21.2 and 23.5.
- **What bounds the port.** Both keep all six SPUs busy. In the port, EDGE is the chain: a list holds SPUs for 95% of
  the frame. It may use SPUs 0-3 only, so the frame is EDGE's SPU time over ~3.8.
- **What bounds RPCS3.** Its EDGE costs about the same a job (51.6 µs against 52.4-57.6). Its RenderJobs, General6 and
  audio take 45 SPU-ms a frame more.
- **Where an EDGE job goes in the port:** a third is the runtime (~19 µs), 7% the job manager, the rest the job's own
  code.
- **More EDGE workers cannot help** without rings of the port's own: each job allocates from its SPU's ring line, set
  up for SPUs 0-3 only.

### 2.37 Update, 2026-11-10: what CPU work can still go to the GPU

[The look](2026-11-10-gpu-candidates.md), with the player's settings uncapped (FSR 3.1 at 1.5, motion blur draws on for
FSR's motion):
- **The GPU** works 12-15 ms of a 36 ms frame in the fight, under half the time.
- **The fight is bound by EDGE on the SPUs.** An EDGE list holds SPUs 89% of the frame.
- **Two kinds of EDGE job take the GPU's way:**
  - **A.** The velocity jobs (output piece `0x75F580`) are never kept: 28.6% of EDGE's SPU time, 73.5 µs a job.
  - **B.** Unskinned jobs miss 57-59% of the time, and 94% of those misses are meshes seen before with another matrix.
- **The estimate.** A and B together take ~24% of EDGE's SPU time, about +8-10% in the fight, and frame generation's 30
  held.
- **Next limits.** 10% of EDGE jobs have a layout the reader does not know. The render thread drains the GPU before
  every batch, which is next once EDGE is lighter.

## 3. The budget for 33 ms

| part | now | at 30 flips/s |
|---|---|---|
| serial head: RenderJobs + MeshProc | ~31 ms | ≤ 12 ms |
| EDGE on three lanes, beside General6 on one | ~50 ms | ≤ 20 ms |
| all SPU work | 389 ms on six workers | the chain's lanes plus audio within six workers that also share cores with the threads below |
| main thread's own work | ~25-28 ms | ≤ 20 ms (headroom) |
| frame clock thread | ~36 ms CPU + 20-25 ms waiting for the GPU | ≤ 20 ms CPU, no full-GPU waits |
| GPU | 25% busy at 645 MHz | not a limit |

## 4. What RPCS3 does that the port does not, and the reverse

| area | RPCS3 (the user's settings) | the port now | plan |
|---|---|---|---|
| SPU code | LLVM; SPU registers live in SSA values inside a compiled block and reach memory at its exits | lifted C; every instruction loads and stores `ctx->gpr` | 2.1, 2.2 |
| SPU float | XFloat Approximate: native single precision | round toward zero, bit-exact (range checks, +0 flush) | 1.1 |
| SPU atomics | reservations per 128-byte line | one global lock-line lock; each notify walks ~100 contexts | 1.2 |
| SPU job transfers | compiled code jumps within LLVM functions | `SPU_DIRECT`, lifted-function lookups, MFC channel emulation per DMA | 1.2, 2.2 |
| PPU code | LLVM; registers in SSA | registers, CR and XER in `ctx`; VMX float lane by lane | 2.3 |
| RSX thread | one thread decodes and records; Multithreaded RSX optional | one thread walks, records, and drains the GPU at every batch | 1.4, 1.5 |
| colour buffers to guest memory | Write Color Buffers on (the SPU MLAA needs it) | not written; the frame stays on the GPU, the SPU MLAA output is unseen | 1.6 |
| title patches | Disable MLAA / Depth of Field / Motion Blur / Mesh Trimming; a 60 FPS patch (canary) | switches, off unless on: Disable MLAA, Depth of Field, Motion Blur (2026-11-01); Mesh Trimming needs SPU edits | 4.3 |
| beyond an emulator | - | the title's own code, statically: native replacements for its hottest SPU jobs, GPU passes in their place, profile-guided builds | phase 3, 2.4 |

## 5. The plan

### Phase 0: measure faster (first; the rest depends on it). Done 2026-10-29

- **0.1 `gowa.py bench`. Done 2026-10-29** (`scripts/bench.py`).
  - It drives to the level (the same route as the scratchpad runner) and takes 10 s windows
    from 175 s after the movie.
  - It runs variants alternately (`--variant "name|exe|ENV=1"`, `--runs`), retries a run that
    failed, and compares with a baseline (`--baseline result.json`).
  - It records the GPU engine counters (`--gpu`), CPU per thread, private bytes and screenshots
    (`--shots`), all in `logs/bench-<stamp>/result.json`.
  - First baseline (`logs/bench-base`): 11.77 flips/s median (12 windows, 10.5-12.2), GPU 3D
    27% at 765 MHz.
  - **Faster, 2026-10-31.** A run takes 3:20 instead of ~5:40, an A/B that is clear after one
    round stops there.
    - The route was not what the notes said: the third Cross picks Continue Game (Profile 1's
      cursor rests there), whose checkpoint loads R_MEGAERA03; the fourth falls in the opening
      cinematic. Screenshots every 4 s showed the menus answer at once, and the bench waited
      25-35 s at each.
    - Now Cross 6, 12 and 18 s after the movie, the choice confirmed by the thread it starts
      (`SaveDataUtil::LoadFileSaveProfile`, a missed press made again), and every later time
      counted from that load: windows from 69 to 149 s after it, the same Dodge scene as 175 s
      after the movie before (one run: 18.89, against 18.39-18.82 on the old route).
    - 80 s of windows instead of 125: over 123 same-executable pairs since 2026-10-30 the
      spread comes from the runs, not the windows (one run a side 1.12% median difference
      against 1.29% with 12 windows; two a side 0.92% against 0.95%).
    - `--early-stop 6` (default): an A/B's first round decides when the two differ by 6% or
      more (3 of the 123 pairs did, none by 7%).
    - `--route 45,80,105,130` runs the old fixed route, counting from the movie.
- **0.2 SPU job replay. Done 2026-10-29** (fork `runtime/spu/spu_jobcap.c`; ref A built the
  same for its EDGE work, we took the idea, not the code).
  - Only in a build with `gowa.py build --jobcap` (`PORT_SPU_JOBCAP`).
    - Its hooks wrap every DMA, channel read and drain, so a default build compiles those paths
      as before.
    - What the hooks cost is below what a level A/B sees. Two A/Bs in plain alternation put
      the build without them 1.7-2.6% ahead. With the hooks compiled out entirely, it still came
      out 1.9% ahead. In ABBA order the same two builds were within 1%: 11.51 against 11.61.
    - The order made those gaps, not the hooks (`bench` now runs ABBA, section 8).
  - **Capture:** `SPU_JOB_CAPTURE=<dir>` (absolute), `SPU_JOB_CAPTURE_WIDS=2,12,15`, `_N`,
    `_EVERY`, `_T0`.
    - A job starts where the Wws job manager branches into it with its return address 0x3368 as
      the link (`SPU_JOB_CAPTURE_LINK`). The manager's own helpers live above 0x4000 too, and job
      code runs under its image id, so only the link tells a job call apart.
    - It ends at the dispatch back to 0x3368. The job modules return through a continuation
      that the drain hands back to the execution driver.
    - Recorded: the context and LS at the start; in order every GET and PUT with their bytes,
      atomic, channel read and count, outbound mailbox word, and the kernel's copy of the SPURS
      instance line into LS 0x100. Jobs that stop or halt are dropped (the audio list's jobs
      leave that way); interrupts are kept.
  - **Replay:** `gowa.py replay <dir> [--reps N] [--only _w15_]` (`SPU_JOB_REPLAY` in the
    port's `main.cpp`).
    - Each job runs from its saved context, with every input served from the record and every
      output compared.
    - It runs in the SPU float mode, and is the execution driver that `spu_restart_dispatch`
      jumps back to.
    - Output: the time per job (fastest run and median) per workload, and `replay.csv`.
  - **The gate passes:** 120 jobs (40 each of General6, RenderJobs, EDGE), 10 runs each, all
    byte-identical with the record (PUTs, LS, registers, every event used). A run costs seconds
    instead of a 25-minute level A/B.
  - **What it measured first:**

    | workload | the job alone (replay) | a policy-module run in the game |
    |---|---|---|
    | General6 w2 | 1,257 us | 1,518 us |
    | RenderJobs w12 | 629 us | 790 us |
    | EDGE w15 | 74 us | ~110 us |

    The rest of each run in the game (the job manager around the job, fetching the job's code,
    the runtime, other threads on the same cores) is a fifth of a General6 or RenderJobs run and
    a third of an EDGE run (step 1.2).
- **0.3 RPCS3 and the port at one scene. Done 2026-10-29:** 22.9 FPS in the Megaera fight
  (section 1).
  - Method: the user's RPCS3 saves of this title moved aside, then a new profile from first
    boot (calibration, screen adjust, the unskippable first-boot intro, New Game, Normal).
  - FPS from RPCS3's window title, one sample a second, 10 s window means. GPU counters and CPU
    per thread over 20 s.
- **0.4 One frame report. Done 2026-10-29:** `gowa.py frames <trace> [--rows] [--grid N]`
  (`scripts/frames.py`).
  - Per frame: each workload group's SPU time, the main thread's waits, and the walker's drains
    and semaphore waits.
  - With `--rows`, the job lists' publishers and consumers; with `--grid N`, N frames on a
    2 ms grid.

### Phase 1: the chain's cheapest time, waits and waste

Each item is small and is measured on its own.

- **1.1 SPU float. Done 2026-10-30** (fork `6544cbc8`, port `0d958e1`). The approximate float
  planned here is wrong for this title; leaner exact forms took its place.
  - Planned: native `addps`/`mulps`/FMA with no range check and no +0 flush (RPCS3's setting for
    this title). On the replay, General6 jobs took 754 us instead of 1,256 and EDGE jobs 37.9
    instead of 71.5, but 13 of 40 EDGE jobs then issued different DMA: they culled differently.
    - The title counts on the SPU's extended range. EDGE makes all-ones masks as
      `x * 0xFFFFFFFF` (-0, or -max saturated), and General6 refines 1/0 (`0x7FFFF800`) with
      `fma`. On x86 those words are NaN.
    - Clamping the reciprocal estimates to FLT_MAX changed nothing; the masks are the cause.
    - What the range check costs: native without it is 25% (General6) and 38% (EDGE) below the
      exact forms below. A check per block, or only at the sites a profile shows, would be the
      way to get some of that back.
  - Done instead, bit-exact: the round-toward-zero forms keep their result in a register (a
    union merged through the stack had cost a store and a reload per operation), the redo of a
    vector out of range is a `__vectorcall` with its operands in registers, and the +0 rule is two
    operations. `test_spu_float_exact`: 446.8M lanes identical in every x86 build mode.
  - `PORT_SPU_FLOAT_APPROX` (`gowa.py build --spu-approx`), off by default: -0 kept where the SPU
    has +0, the range check kept. On the replay only scratch words, registers and two output
    words of one EDGE job differ (-0 for +0).
  - Replay, us per job (fastest of 10 runs):

    | | before | exact, leaner | approx | native, unchecked |
    |---|---|---|---|---|
    | General6 w2 | 1,256 | 1,002 | 932 | 754 |
    | RenderJobs w12 | 627 | 617 | 613 | 611 |
    | EDGE w15 | 71.5 | 61.4 | 61.0 | 37.9 |

  - Level, ABBA, two runs each (`logs/bench-float`): before 11.71, leaner exact 11.86 (+1.3%),
    approx 11.96 (+2.1%). Within the method's noise; the screenshots look alike. The SPU workers
    used 10-17% of a core less (`spurs pm` 480% -> 463-471%).
  - So the frame is not bound by these jobs' float code: a fifth off General6 and a seventh off
    EDGE moved the frame rate 1-2%. What the chain waits on needs a fresh trace before 1.2.
- **What bounds the frame now (2026-10-30).** A trace of the level with the leaner float
  (`logs/trace-lean.txt`, 154 frames) and a profile of the six workers and the main thread
  (110 s, `PS3_SAMPLE_THREAD`):
  - Frames take 58-100 ms (median 76.7), following the scene. In the slow ones RenderJobs' one
    lane takes 36-45 ms instead of 18: it shares the cores with the previous frame's EDGE tail,
    audio and Batch.
  - The main thread runs 75 of every 77 ms; 61% of its samples poll (the job-graph scan 30%, the
    progress counters 32%), 8% are the coherency notifies of those loops. The frame clock thread
    takes half a core.
  - **The critical path is a serial chain:** RenderJobs (one lane) -> MeshProc -> General6 (one
    lane) -> the main thread -> the next frame's RenderJobs. The main thread publishes that
    list 2.7 ms (median) after General6's last run of the frame ends; RenderJobs takes 37 ms from
    there to its last run. EDGE (three lanes) ends ~5 ms after that publication: it has slack.
  - Those two lanes run slower in the game than alone: a General6 job 1.55 ms against 1.0 on the
    replay (+55%), a RenderJobs job 0.82 against 0.62 (+33%), General6 being 94% lifted code.
    They share a physical core with a busy sibling (other workers, the main thread, the frame
    clock thread) and start on cold data.
  - Not total throughput: the main thread polling with a 0.5 ms OS sleep instead (96% -> 39% of a
    core) left the workers at the same CPU and the frame rate within noise (12.70 -> 12.81).
    (This bullet first said the frame was bound by throughput; these trials say otherwise.)
  - The workers: lifted SPU code 50% of samples, idle 21%, the runtime 27.5% (35% of busy
    time). The largest runtime item was dispatch, ~11% of busy time (14% in audio).
- **1.2 The SPU runtime's share** (~30% of a RenderJobs or EDGE run; by the replay, a job alone
  takes 80% of its game run in RenderJobs and 68% in EDGE):
  - **dispatch. Done 2026-10-30** (fork `cc8af52a`).
    - Counted every 5 s in the level: 38.5M indirect branches; 25M full resolutions, 17M of them
      finding nothing (the interpreter asks at every step whether it can rejoin lifted code, and
      misses were not cached); 400K verifications of streamed code comparing 7.3 GB, because
      every GET into a verified span (the job manager loading a module again, a module's data
      beside its code) marked it pending.
    - Now misses are cached (a level run with `SPU_DISPATCH_CACHE=shadow`: 332M hits, no
      disagreement), a GET compares only the code bytes it wrote, and the same module loaded
      again keeps its slot: 4.2M full resolutions and 0.26 GB compared every 5 s.
    - Level, ABBA, two runs each: 12.02 -> 12.61 flips/s (+4.9%).
    - Left: `spu_indirect_branch`'s own checks on every branch, ~3% of worker time. A cache per
      call site in the lifted code would skip them.
    - **Done 2026-10-30** (fork `6a2015f4`): a short path for the common branch instead (a target
      at or above 0xA80, no interrupt return pending, no diagnostic set: the task resolution, the
      lookup, the stack-reset check and the tail call, without the two dozen special cases), and
      the MFC command parameters (LSA, EAH, EAL, Size, TagID, WrTagMask) as stores into the context
      from lifted code (`SPU_INLINE_MFC_PARAMS`). Replay: RenderJobs 561 -> 540 us per job (-3.8%),
      outputs identical. Level, three runs each: 13.80 -> 14.21 flips/s (+3.0%).
    - Profiling the replay with `PS3_SAMPLE_MAIN` deadlocked (the sampler suspends the thread
      that runs the jobs, which can hold a heap lock); profile the jobs in the game instead.
  - **LS diagnostic gates. Done 2026-10-30** (fork `c0925006`, `PORT_SPU_LS_DIAG` off; design
    note step 4).
    - Every lqd/stqd of lifted code tested a diagnostics gate, a load and a branch the compiler
      cannot hoist. Lifted code is now built without it (`SPU_LS_NO_DIAG`);
      `gowa.py build -D PORT_SPU_LS_DIAG=ON` brings it back for `SPU_LS_WATCH` and the like.
    - Replay: RenderJobs 610 -> 563 us per job (-7.7%), EDGE 64.8 -> 58.0 (-10.5%), General6 the
      same; every output identical. Level, three runs each: 13.05 -> 13.52 flips/s (+3.6%).
  - **DMA:** a fast path for the common sizes, and list DMA without channel emulation per
    element.
  - **coherency:** notify only lines with a live reservation; stripe the lock-line lock by line.
  - Coherency holder counts and `preserve_none` were tried on 2026-10-28, when the chain did not
    set the frame. Each moved its piece and not the frame. Measure again here, on the replay and
    then the level.
- **1.3 The main thread's polls wait. Done 2026-10-30** (fork `2accf30f` `--insn-hooks`; the
  port's `poll_wait.cpp` and `scripts/ppu_hooks.json`).
  - The lift calls `gowa_poll_wait` at the heads of four polling loops: `func_0049D688` (until
    the least of seven u16 progress counters reaches a target), `func_0049B35C` (around
    `func_0049CB6C`), and the job-graph scan's two callers `func_0044CD20` / `func_0044D120`.
  - After 8 passes in a row at one head it waits 20 us with MONITORX/MWAITX and its timer: the
    core stops issuing for the main thread, without the OS, and its SMT sibling gets it. A CPU
    without MONITORX spins on PAUSE instead. `GOWA_POLL_WAIT_US=0` turns it off.
  - Trials, ABBA, two runs each: a waitable-timer sleep (no shorter than ~0.5 ms here) 12.70 ->
    12.81, noise; MWAITX 20 us 12.60 -> 12.94 (+2.7%). As built, two runs each 12.86 -> 12.90,
    then three runs each 12.70 -> 12.92 (+1.7%; runs 12.70/12.80/12.60 against
    12.86/13.11/12.90, every run above every one before). Pooled over the three A/Bs, +1.75%.
  - A wake on the polled line itself (MONITORX on the counters' line, the scan's lines) is the
    refinement: the loop would resume when an SPU writes, not at the timer.
- **1.4 No full GPU drain per batch.**
  - Give the vertices, the constants and the texture staging FRAME_COUNT rings each.
  - `rf_open` then waits only for the batch that last used the same ring.
  - Expected: the walker stops standing still for 20-25 ms a frame, and the reports, labels and
    parks the SPU jobs wait for move earlier.
  - Risk: the torn geometry the drain was added for. Check it with the capture tools.
  - **Looked at 2026-10-31, after 1.5's first part** (fork `8dab3f5a`). The rings are not needed
    now:
    - `PERF=1` (waits by the line that asks): 9-12 ms a frame, all in `rf_open`, one a frame,
      nearly all from record-ahead, which opens only once the batch before has finished.
      `RF_WAITLOG=1` showed it had: the wait was the drain signalling anew and waiting 3-17 ms
      for that signal.
    - `rf_open` now waits only while the last value signalled is not done. The waits go to ~0 and
      the drain no longer stops, but the level is unchanged (env-only A/B, `RF_IDLE_WAIT=1` waits
      as before: 20.09/19.66 against 19.88). Not on the frame's path.
    - The GPU: 3D engine 29% busy at 720 MHz (P5), `nvidia-smi` 34%.
    - The frame clock thread by the sampler (`PS3_SAMPLE_THREAD=frame clock`): idle 31% (the
      message pump's wait), the FIFO walk ~16%, the prepass ~18% (views 6.5, texture lookups 4.2,
      the fragment program hash 3.3, pipelines 2), vertex uploads 6.5, recording ~7%, blits 3%.
      What is left is many items of 1-3 ms a frame; a second thread for the D3D12 work (RPCS3's
      multithreaded RSX) is the structural step if this thread binds again.
- **1.5 Descriptors.**
  - One SRV per cached texture, made at upload. A draw's 16-entry table is copied
    (`CopyDescriptorsSimple`), or reused when it equals the last draw's.
  - Expected: ~14 ms down to ~2-3 ms a frame of the frame clock thread.
  - The null-SRV skip (-4 ms) gave +1% when that thread did not limit; this is for when it does.
  - **First part done 2026-10-31** (fork `d1e368c7`). `PERF=1` in the level first: the views
    were 14.1 ms of the frame clock thread's ~55 ms frame (~1,800 windows a frame, 16
    `CreateShaderResourceView` calls each, ~0.5 us a call).
    - The prepass collects a draw's 16 views; a draw whose views equal an earlier draw's in
      the batch binds that window (~18% of them). The others copy their views (one
      `CopyDescriptors`) from a CPU-only heap with the null view and each texture slot's
      views, made once.
    - The view work 14.1 -> 7.4 ms a frame (3.3 of it the texture lookups); `render_frame`
      19.5 -> 10.8 ms.
    - Level A/B: **18.69 -> 20.27 flips/s (+8.5%)**, every window of the new build above every
      window of the old (one round: `--early-stop`). The frame clock thread binds now.
    - Next: windows kept across frames (most of a frame's 1,800 tables recur), then 1.4.
- **1.6 MLAA on the GPU.**
  - Answer the five tasks at their signal: the label, the counter and the kick, without the work.
    Ref A does the same; check it with our own probe.
  - The game gets its anti-aliasing back from the GPU instead. The port shows none today.
    - The runtime already has SMAA 1x on the display target before scaling
      (`graphics.antialiasing`, `libs/video/rsx_smaa.c`).
    - Better: run it at the snapshot the title samples, where the PS3 runs its MLAA.
  - It takes the tasks out of the RsxSync-to-EDGE path; the GPU pass is under 1 ms at 720p
    (estimate).
- **1.7 Thread placement on Zen 2.**
  - CCX-aware affinity: the SPU workers and the coordinator in one CCX, the main and frame clock
    threads in the other.
  - Five against six SPU workers, and the audio worker's priority.
  - RPCS3 has the same knobs: thread scheduler mode, max SPURS threads.
  - Tried 2026-10-30, not kept: one physical core kept for the critical chain's workloads
    (General6, RenderJobs, MeshProc) through Windows CPU sets, the process's default set leaving
    it out. 12.82 -> 12.60 (-1.7%), with windows down to 9 flips/s: the other ten logical CPUs
    got crowded.
  - One SPU worker per physical core, 2026-10-30, measured at ~16 flips/s while the six
    workers are full during EDGE. Worker k went on logical CPU 2k, one run of three each:
    - as Windows places them: 15.99 flips/s;
    - with that CPU as the ideal processor: 15.94;
    - pinned there: 6.96, a pinned worker cannot leave a CPU another thread needs.
    Windows' own placement stays.

### Phase 2: code generation, the work RPCS3 does in LLVM

- **2.1 SPU registers in locals where it pays** (design step 6).
  - Write back only the dirty registers, reload only the ABI's volatile ones, and promote only
    functions whose barriers sit in loops.
  - Judge it by replay cycles. Static instruction counts (2026-10-28) said it grows the code;
    they cannot see the store-to-load chains it removes.
  - General6 `4F00`, ~450 instructions in one block, is the case where RPCS3's SSA pays most.
  - **Done 2026-10-30 for float-heavy straight-line functions** (fork `39bbe622`,
    `spu_lifter.py --float-spec 4`, `SPU_FLOAT_SPEC` in `gowa.py`). A function with no store,
    channel op, call or internal branch and at least four single-precision float ops gets a
    second body:
    - the SPU registers in locals, loaded at entry where the body reads one before writing it;
    - the float ops in speculative native forms (`spu_fa_sp` & co.: the RZ result with the +0
      rule, its magnitude folded into an accumulator);
    - at each exit one range check: if a result left the single range, the exact copy (the
      ordinary body) runs from the entry state, which nothing has changed; else the registers
      written so far are stored and the exit runs.

    154 functions in 23 images, `4F00` among them.
    - Replay: General6 1,007 -> 885 us a job (-12%). RenderJobs, EDGE, RenJobSet and MeshProc
      unchanged. Outputs identical in every captured job (`logs/jobcap2`, and `logs/jobcap3`
      with w8-w11 and w13). Batch (w6) jobs do not finish in the replay with either build.
    - Checks: `test_spu_float_exact` (where the range check passes, the speculative forms equal
      the exact ones in all 446.8M lanes) and `tools/test_spu_lifter_float_spec.py`.
    - In the game, one run each with `SPURS_WID_PROF`: General6 1,240 -> 1,112 us a run (-10%),
      59.5% -> 54.5% of a core. No `_exact` copy among its sampled functions (`4F00` 37% of its
      time, `4AB0` 21%).
    - Level, three runs each: 14.39 -> 14.31 flips/s, unchanged. General6 no longer bounds the
      frame; its runs per second did not change either. Kept for when it does again: at RPCS3's
      22.9 flips/s (a 44 ms frame), its single lane, ~38 ms a frame now, has little slack.
    - Measured on the way, on General6:
      - the speculative forms with the registers left in `ctx`: -3.6%;
      - locals without them (`promote.py`, 2026-10-28): +8%;
      - the two together: -12%. With no call left in the body (the exact forms redo out of
        line), clang can keep the registers in host registers.
    - Not kept: locals for every function without calls or channel ops, stores and loops
      included. They took General6 to the same -12% but made RenderJobs and RenJobSet 2-6%
      slower.
  - **Loops, 2026-10-30: not pursued.** RenderJobs' `6D90` is a 20-instruction loop and 9.5% of
    its time. With the registers in locals:
    - the loop body stores nothing to `ctx->gpr` any more (~75 -> ~55 x86 instructions an
      iteration);
    - RenderJobs gains 1.3% on the replay.

    The loop's time is its chain (LS load, shuffle, address, load, `shufb`, store), not the
    stores.
- **2.2 Cheaper transfers between lifted functions:** the checks in `SPU_DIRECT`, function
  entries, drain calls; `preserve_none` and `--merge-chunks` measured again on the replay. These
  are RenderJobs' 13% and 9%.
  - **Dispatch and drain fast paths. Done 2026-10-30** (fork `d3ff2bdb`).
    - `spu_indirect_branch`'s common branch shared a function with its slow path. That path's
      frame (eight saved registers, xmm6, 0x338 bytes, a stack cookie) was set up and torn
      down around every branch.
    - `spu_drain_call` had the same problem with its interrupt frame's jmp_buf, around every
      return from a lifted call.
    - Each slow path is now a function of its own, which the fast path tail-calls.
    - Replay: RenderJobs 469 -> 452 us a job (-3.5%), RenJobSet -2% to -2.6%; outputs
      identical.
    - Level, three runs each: 15.75 -> 15.88 flips/s, within the spread. Kept: it only removes
      work.
- **2.3 PPU:**
  - `PORT_PPU_FAST_FP` on: host NaNs, compared by screenshots. It made hot float code markedly
    shorter in God of War HD.
  - The VMX float helpers as vector code.
  - `-mavx2` for the lifted PPU code (it gets FMA, so AVX, today).
  - **The memory helpers and what follows a call. Done 2026-11-06** (`PPU_MM_LEAN`, 2.29).
  - Lazy CR and XER next: [the review](2026-11-06-main-thread-review.md) has the sizing.
  - Then guest registers and CR fields in locals inside a function, with 2.1's rules, starting
    with the main thread's hottest functions.
- **2.4 Profile-guided builds** of the lifted code (clang's instrumented build, profiles from the
  bench route).
  - **Lifted SPU code, 2026-10-30: no gain.** `PORT_SPU_PGO=gen` instruments it, and `main.cpp`
    writes the profile to `LLVM_PROFILE_FILE` every 15 s. `PORT_SPU_PGO=<.profdata>` builds with
    the profile.
  - The profile came from one bench run cut short at 270 s; the instrumented code runs the level
    at ~2.8 flips/s. It still holds the level's jobs: EDGE's `EC08` 1.8M calls, RenderJobs'
    `6D90` 0.96M.
  - Level, three runs each: 15.85 against 15.69 flips/s.
  - The option stays, off: the same mechanism can serve the lifted PPU code.

### Phase 3: native and GPU replacements, beyond an emulator

- **3.1 Native hot functions.**
  - Targets: General6's `4F00`, `4AB0` and `63B4`, and EDGE's stages (skinning, per-vertex
    lighting, transforms, culling; ref A's stage map names them).
  - Each as native AVX2 code behind a switch, checked against the replay: bit-exact, or within
    1.1's tolerance.
  - Ref A measured +8-9% in game for each EDGE stage.
- **3.2 Audio** (w7, 0.77 of a core at any frame rate): after 1.1 and 2.1, or its mixer as
  native code.
- **3.3 EDGE on the GPU** (research). EDGE is the largest SPU consumer, 171 ms of SPU time a
  frame.
  - Once 3.1 knows the stages' inputs and outputs, a compute pass could produce the vertex
    streams the RSX draws.
  - Its culling is there to spare the RSX, and this GPU has room.
  - First question: does anything but the RSX read what EDGE writes?
  - Phase 0 (2.22): at the gate, ~+95-115% on the GPU, ~+45-70% for a native job; the frame
    clock thread is next at ~29-30 flips/s.
- **3.4 Vertex and index data read by the GPU from guest memory**, without upload copies:
  `ID3D12Device3::OpenExistingHeapFromAddress`, or upload rings filled by the walker only for the
  ranges a draw uses.

### Phase 4: playable and comfortable

- **4.1 Frame pacing:** a steady 30 when 60 cannot be held. The title flips on vblank; present
  every second one.
- **4.2 Resolution:** the GPU has the room.
  - FSR 1 output scaling exists (`graphics.scaling = fsr1`, `libs/video/rsx_fsr1.c`).
  - Missing: an internal resolution scale. RPCS3's Disable Mesh Trimming patch is what lets the
    title draw correctly above 720p.
- **4.3 RPCS3's patches as port options.** Done 2026-11-01 as switches, off unless switched on
  ([note](2026-11-01-title-patches.md)): `scripts/title_patches.json`, each instruction edit
  lifted both ways behind a guard, `[patches]` in gowa.ini (or `--set`, or `GOWA_PATCH_<ID>=1`).
  - Disable MLAA: 20.61 -> 21.63 flips/s (+5.0%, ABBA). The title stops its MLAA work; its
    tasks wait unsignalled. It no longer makes the backend compile (2.13);
  - Depth of Field, Motion Blur: one run each, 20.76 and 22.16 against 20.33;
  - Mesh Trimming: SPU edits in the EDGE modules (`spu_lifter --insn-patches`); more geometry,
    19.88 against 20.54. For drawing above 720p, not for speed;
  - the launcher's Game patches group, a checkbox for each patch the build has;
  - left: the 60 FPS patch.
- **4.4 Hitches:** first-play pipeline and texture stalls, and pipeline cache warm-up from the
  disk cache. Fewer since 2.13: a program is one shader whatever is bound (MLAA on, 305
  pipelines a run instead of 1624), and a draw's pipeline is built from the program it drew.

## 6. What the phases should add up to

These are estimates, each to be replaced by its measurement. They take the chain of section 2.1
and shorten each link by the shares in section 2.2.

| after | RenderJobs + MeshProc | EDGE ∥ General6 | frame | flips/s |
|---|---|---|---|---|
| now | 31 ms | ~50 ms | 80 ms | 12.3 |
| phase 1 (approximate float on General6/EDGE; the runtime's ~30% cut by a third; less SMT sharing) | ~27 | ~36-38 | ~62-65 | ~15-16 |
| phase 2 (registers in locals; transfers; PGO) | ~19 | ~25 | ~45 | ~22 |
| phase 3 (native hot functions; audio) | ~15 | ~17 | ~33 | ~30 |

- RPCS3 gets its speed with phase 1 and 2 kinds of work: LLVM register allocation and
  approximate float.
- Its 22.9 FPS in this scene (step 0.3) is where phase 2 should land.
- Phase 3 is what a static port can add on top, and it is what 30 needs on this laptop.
- Per step, the main thread and frame clock thread budgets (section 3) are checked too. 1.3,
  1.4, 1.5 and 2.3 keep them under the chain.

## 7. Tried before, and what comes back

| lever | when | result | now |
|---|---|---|---|
| `/GS-` on lifted SPU code | 2026-10-28, 7.3 flips/s | no change | with 2.2, on the replay |
| `preserve_none` for lifted SPU code | 2026-10-28 | EDGE function entries 5.1% -> 2.7% of its lifted samples; frame unchanged | **ruled out 2026-11-01 (2.12)**: on every lifted function, replay General6 +44.9%, EDGE +2.7%; the callers spill around each call |
| coherency holder counts | 2026-10-28 | coherency 5.5% -> 4.5% of an EDGE run; frame unchanged | 1.2 |
| job modules with `--merge-chunks` | 2026-10-28 | 1-6% off some workloads, frame unchanged, +13 MB of C | **kept 2026-11-01 (2.12)**, but General6's modules: 1-8% off the other workloads in replay |
| null SRV left alone | 2026-10-28 | -4 ms a frame of CPU, +1% | 1.5 (the whole descriptor path) |
| widening the serial lists (`SPURS_MAXCONT`) | 2026-10-28 | nothing: the jobs come one at a time | **overturned 2026-10-31 (2.10)**: General6 and RenderJobs do run on 4 SPUs; the frame needs SPURS priorities with it |
| more frequent SPURS idle probes | 2026-10-28 | hangs | ruled out |
| speculative float in EDGE's single-block loops | 2026-10-31 | replay +5% (locals alone -5.5%); the range checks fail in most calls | ruled out (2.8) |
| EDGE lists at six SPUs each (`SPURS_MAXCONT`) | 2026-10-31 | same runs a second, 16.25 flips/s | **overturned (2.10)**: EDGE is eligible on SPUs 0-3 and gets the fourth only with priority-aware picks (+34% jobs/s when it has it) |
| `ctx->ls` read once a lifted function | 2026-11-01 | replay EDGE +3.2%: 0.6% fewer instructions, but the pointer holds a register through calls | ruled out (2.12) |

They were measured while something else set the frame (the texture work on the frame clock
thread, the main thread's waits). The chain sets it now, so what shortens a chain job counts.

## 8. How each step is measured

- One change at a time. Two runs a side in ABBA order. Medians of 10 s windows from 175 s after
  the movie.
  - Plain alternation (a b a b) put the first variant of each round ahead in 6 pairs out of 6,
    by 1.7-2.6%, for builds that differed by nothing on the hot path.
  - In ABBA order (a b b a) the same builds were within 1%. A gain under ~3% measured the old
    way needs measuring again.
  - The occlusion counts (fork `5c906fb2`, +2% the old way) hold. In ABBA order, one executable,
    they gave 11.69 against 11.21 flips/s with `RSX_ZPASS=0`: +4.3%, both runs above both.
- On the charger. `D3D12_VSYNC=0` for unattended runs: on battery the display turns off after
  30 minutes and vsync'd presents drop to ~3.8 flips/s.
- Frame-rate claims with only `GOWA_THREADS` on; the trace and sampler runs are for the
  breakdown.
- SPU code changes: the replay (0.2) first (time per job, output hashes), then the level.
- Anything visible: screenshots at 150, 250 and 350 s against the previous build, and a
  play-through.
- RPCS3 is a behaviour reference only (its GPL code is not copied). The other ports' notes are a
  map; every claim is measured here first.
