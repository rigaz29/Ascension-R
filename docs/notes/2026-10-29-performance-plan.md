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
