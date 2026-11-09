# Why RPCS3 runs the first level 1.5 times faster: measured at the job level

Written 2026-10-31, port `d00cd75` (fork `8dcd4671`), RPCS3 0.0.43-20415 with the user's settings
(plan section 1, the "Disable MLAA" patch on). In the Megaera fight RPCS3 runs 24.0 FPS and the
port 16.3 flips/s, a 41.7 ms frame against 61.5 ms.

## 1. Method

- Both processes run the same game code on the same guest data. So the title's SPURS instance and
  its Wws job lists can be read from outside either one. `scripts/guestscope.py` does that with
  ReadProcessMemory only: nothing in RPCS3 is changed or attached.
  - `capture` finds the guest memory base (the ELF's load segment at guest 0x10000) and the SPURS
    instance (the one whose `wklInfo` tables hold the most entries with the Wws policy module
    0x007A7B00).
  - Then, every ~120 us, it reads the instance's first 0x80 bytes and each Wws workload's ticket
    rows: `data + 0x40 + 0x10 * list`, `published << 16 | consumed`.
    - The 0x80 bytes hold the ready counts, current contention (how many SPUs are in each
      workload), min and max contention, flags and signals.
  - `report`, `jobs` and `edge` turn a capture into SPUs per workload, jobs per list and EDGE jobs
    per contention level.
- Where things are:
  - RPCS3's guest memory is at host 0x300000000. Its SPURS instance is at guest 0x3001AD00, the
    port's at 0x7001AD00.
  - The workload ids differ: RPCS3 has tasksets at wids 0-3 and 5, so its EDGE lists are wids
    19-21, the port's 14-16. The title adds its 18 Wws workloads in the same order in both, so
    they are named by that order.
- Each capture is 90 s in the fight, from ~20 s in. RPCS3 was reached through New Game, Normal in
  the measurement profile; the fight starts 132 s after Normal. The port was driven by
  `gowa.py bench`.
- The capture takes a host core. Frame rates below are the ones measured during the capture:
  RPCS3 24.7 (24.0 without it), the port 15.6 (16.3).
- **Both run exactly the same work a frame.** Jobs taken a frame:

  | list | RPCS3 | port |
  |---|---|---|
  | each EDGE list | 534 | 537 |
  | General6 | 26.1 | 26.0 |
  | RenderJobs | 40.3 | 40.0 |
  | RenJobSet0-3 | 59.9 / 37.9 / 107.8 / 9.1 | 58.7 / 37.6 / 107.4 / 9.0 |
  | MeshProc | 6.1 | 6.0 |
  | Batch | 14.9 | 14.4 |

  The difference is only in how the jobs run.

## 2. What the two do in a frame

The busy time is the time a workload has at least one SPU in it; "SPUs" is the average while
busy; "max" is the workload's maximum contention as the instance holds it.

| workload | RPCS3 busy ms/frame | SPUs | max | port busy ms/frame | SPUs | max |
|---|---|---|---|---|---|---|
| EDGE (each of 3 lists; they never overlap) | 9.2 | 3.78 | 8 | 17.7 | 2.61 | 1 (3 by the port's `SPURS_MAXCONT` default) |
| General6 | 16.7 | 2.09 | 6 | 39.6 | 1 | 1 |
| RenderJobs | 17.9 | 1.75 | 6 | 26.4 | 1 | 1 |
| RenJobSet2 | 2.9 | 3.75 | 8 | 13.1 | 1 | 1 |
| Batch | 0.9 | 1.88 | 6 | 13.1 | 1 | 1 |
| MeshProc | 3.4 | 1.50 | 5 | 5.3 | 1 | 1 |
| audio | 29.2 | 1 | 1 | 42.6 | 1 | 1 |

- **The EDGE phase is 27.6 ms of RPCS3's frame and 52.8 ms of the port's.** The 25 ms between
  them is about the whole frame-time difference. Plan 2.8 found EDGE the most sensitive
  workload.
- SPUs in workloads on average: RPCS3 5.8 of 6, the port 4.7.

EDGE jobs taken per second while a list has n SPUs in it, over all three lists (`edge`):

| SPUs in the list | RPCS3 | port | port, `SPURS_MAXCONT` with RPCS3's values |
|---|---|---|---|
| 1 | 25.0K/s, 40 us a job per SPU | 18.4K/s, 54 us | 10.6K/s, 95 us |
| 2 | 35.4K/s, 57 us | 28.0K/s, 71 us | 19.3K/s, 104 us |
| 3 | 50.3K/s, 60 us | 31.7K/s, 95 us | 35.0K/s, 86 us |
| 4 | 58.6K/s, 68 us (53 of RPCS3's 61.5 s of EDGE time) | - | 39.1K/s, 102 us |

## 3. The causes, largest first

### 3.1 The maximum contention the title asks for is lost

- The title creates each workload with `_cellSpursWorkloadAttributeInitialize(attr, revision,
  sdkVersion, pm, size, data, priority, minContention, maxContention)`.
  - That is nine arguments. On PPC64 the ninth is in the caller's parameter save area (SP + 0x70),
    not in a register.
  - The HLE adapter forwards r3-r10 only, and the port writes 1 for it
    (`libs/spurs/cellSpurs.c:2007`: "9th arg is beyond the 8-GPR adapter").
- RPCS3 reads it. **The title asks for 6 SPUs for General6, RenderJobs, HighestPrio6 and Batch, 8
  for the EDGE and RenJobSet lists, 5 for CollisL, MeshProc and RsxSync, 1 for audio.** The names
  say so too: "General6", "HighestPrio6".
- In the port every Wws workload runs on one SPU at a time. EDGE runs on three only because
  `main.cpp` sets `SPURS_MAXCONT=14=3,15=3,16=3`, a workaround from 2026-10-27 for exactly this.
- `cellSaveDataUserAutoLoad` had the same problem and reads its ninth argument from SP+112
  (`libs/system/cellSaveData.c`).
- The port also writes the minimum contention at +0x40 (`cellSpurs.c:1816`). RPCS3 leaves it at 0
  for a 32-workload instance, where the field is unused.

### 3.2 The kernel does not choose a workload the way SPURS does

RPCS3's SPU kernel (`cellSpursSpu.cpp`, `spursKernel2SelectWorkload`) does what the real one does:
- **Admission.** A workload can take an SPU if all of these hold:
  - it is runnable;
  - its priority on that SPU is not 0;
  - its maximum contention is above its current contention;
  - its flag or signal is set, or its **ready count is above its current contention**. The ready
    count is the number of SPUs the job manager asks for; the kernel never changes it.
- **Choice.** The highest weight wins. The weight is `(0x10 - priority) << 4`, so a smaller
  priority number wins. The workload already on that SPU gets `| 4`, and a tie goes to the lower
  id.
- **The title's priorities on the first level** (`wklInfo.priority[spu]`):

  | priority | workloads |
  |---|---|
  | 1 | CollisL, audio (SPU 5 only) |
  | 2 | Batch |
  | 3 | EDGE (SPUs 0-3 only) |
  | 8 | RenJobSet |
  | 10-12 (12 on SPUs 0-2) | RenderJobs |
  | 11-13 (13 on SPUs 0-2) | General6 |
- **Yield.** A policy module that polls (`cellSpursModulePollStatus`) is told to exit when another
  workload now outweighs it.

The port's kernel (`spurs_pick`, `cellSpurs.c:2405`) differs in all four:
- round robin over the eligible workloads, with priority used only as zero or not (`:2503`);
- it **takes one from the ready count** at each pick (`:2522`), as if it were a kick counter;
- it has no preference for the current workload;
- its answer to a poll is always "keep running" (`runtime/spu/spurs_policy.c`, header comment).

With every maximum at 1 this hardly mattered. With the title's maxima it decides who gets the
SPUs:
- `SPURS_MAXCONT` set to RPCS3's values (capture `mc1`):
  - General6 39.6 -> 11.1 ms a frame (4.3 SPUs while busy);
  - RenderJobs 26.4 -> 7.7 ms (3.9 SPUs);
  - RenJobSet2 13.1 -> 6.2 ms;
  - MeshProc's jobs waiting with no SPU 23.4 -> 4.1 ms a frame.
  - **The frame rate did not move (15.56 -> 15.59).** EDGE, the limit, now often had one SPU:
    15.7 s of its ~72 s at one SPU, against 3.7 s before. General6 and RenderJobs took SPUs 0-3
    that RPCS3's kernel would have given to EDGE (priority 3 against 12-13).
- EDGE alone at 4 (`SPURS_MAXCONT=14=4,15=4,16=4`, capture `e4`): EDGE had four SPUs for only
  10.5 s of ~71 s, because the others held SPUs 0-3. When it had them, its throughput was 41.6K
  jobs/s against 31.0K at three (+34%), with no loss per job.
- This also overturns two 2026-10-28 conclusions (performance map, "Ruled out"): that the serial
  lists cannot use more SPUs, and that priorities do not matter. Both were measured with
  maximum 1, under the priority-blind kernel.

### 3.3 An EDGE job takes ~1.4 times as long

- At any contention, an EDGE job takes the port ~1.4 times what it takes RPCS3: 54 against 40 us
  with one SPU in the list, 95 against 60 with three.
  - Both slow down the same way as SPUs are added (x1.7 from one to four SPUs). So that part is
    the machine (SMT siblings, memory), not the port's locks.
- RPCS3's whole job, policy module included, takes less than the port's job code alone on the
  replay (52 us).
- The difference is code generation and the policy module's runtime cost:
  - the registers in memory (audit F2);
  - the exact float forms where RPCS3 uses its approximate ones;
  - ~24 us of policy-module work a job (plan 2.8: interrupts from the DMA lists, full-path
    branches, channel emulation).

### 3.4 A Batch job takes ~8 times as long

- Batch: 14.4 jobs a frame. In the port each takes 0.9 ms on one SPU. In RPCS3 the whole list
  takes 0.9 ms a frame (1.9 SPUs), ~110 us of SPU time a job.
- Nothing else is that far apart. General6, RenderJobs and RenJobSet take about the same SPU time
  a job in both (General6 ~1.3-1.5 ms, RenderJobs ~0.7 ms, RenJobSet ~0.1 ms); there the whole
  difference is how many SPUs work on them.
- Batch jobs did not finish on the replay either (plan 2.1). The suspects are the SPU interpreter
  (code that is not lifted) or its transfers.
- Batch has priority 2, above EDGE. Once priorities count, a slow Batch takes SPUs from EDGE, so
  it has to be fast first or alongside.

### 3.5 Smaller things seen on the way

- The MeshProc kick word at 0x009B504C holds MeshProc's workload id in its low half (0x0D08 in
  the port, 0x1208 in RPCS3) and a count above it. So it is not a plain flag.
- RPCS3's six SPU threads spin at 96-98% of a core each. Its main thread uses 50% of a core, the
  port's 95% (the port polls with waits; plan 1.3).

## 4. What to implement, in order

1. **The ninth argument.** A context handler for `_cellSpursWorkloadAttributeInitialize` (NID
   0xEFEB2679, as `cellSaveData_register_ctx_handlers` does it) reads `maxContention` from SP +
   0x70.
   - Drop the `SPURS_MAXCONT` default in `main.cpp`.
   - Stop writing the minimum contention into a 32-workload instance.
   - Check: the instance holds 5/6/8 as in RPCS3 (`guestscope.py report`).
2. **The kernel's choice as SPURS makes it**, in `spurs_pick` and the poll intercept:
   - the admission rule above, with the ready count as a request level, not decremented;
   - the weight `(0x10 - priority) << 4`, with `| 4` for the workload already on that SPU;
   - a policy module's poll answered "exit" when another workload outweighs it.
   - Watch every place that relied on the decrement: the job manager stores the ready count
     itself (`cellSpursReadyCountStore`/`Swap` and inline atomics), and the coordinator's idle
     back-off.
   - Check with `guestscope.py`: EDGE at ~4 SPUs for most of its time, as RPCS3's 86%.
   - Expected, from the `e4` rate at four SPUs: the EDGE phase ~53 -> ~40 ms a frame, the frame
     ~64 -> ~52 ms, **~19 flips/s**. That is about where the frame clock thread starts to bind
     (`RSX_SLOW`, plan 2.9), so plan 1.4/1.5 follow.
   - Risk: General6, RenderJobs and RenJobSet run on several SPUs for the first time. The lists
     are built for that, and RPCS3 runs them so, but the runtime's DMA, reservations and
     coherency are now exercised in parallel. Check with screenshots and long runs.
3. **Batch's 8x**: a sampler run on a worker running w6 (`PS3_SAMPLE_THREAD`), and
   `SPURS_WID_PROF` for its runs. Is it in the interpreter, in DMA, or in waits?
4. **The EDGE job's 1.4x**:
   - the remaining native stages (plan 2.8);
   - `__restrict` on the lifted context (audit F2);
   - the policy module's cost per job (its interrupts from the DMA lists, the module compare).
5. **The frame clock thread** (plan 1.4 rings, 1.5 descriptors), before ~19-20 flips/s.

## 4.1 Result of steps 1 and 2 (2026-10-31, fork `1a1aa4f9`)

- Level, ABBA, two runs each: 16.19 -> 18.92 flips/s (+16.9%). The port is at ~79% of RPCS3's
  24.0 (68% before).
- A finding on the way: the poll intercept answered every policy module with workload 0. The
  module's `cellSpursModulePollStatus` (LS 0x3750 in the Wws module) compares the answer with
  `wklCurrentId` (LS 0x1DC) and ends its run when they differ.
  - So every workload but 0 ran about one job a run (EDGE ~1.3) and paid the module's entry for
    each.
  - With the kernel's answer an EDGE run takes ~45 jobs (3.7-4.1 ms).
- On the new build, by `guestscope.py`:

  | | RPCS3 | port before | port now |
  |---|---|---|---|
  | EDGE phase, ms a frame | 27.6 | 52.8 | 36.5 |
  | EDGE time at four SPUs | 86% | 0% (max 3) | 79% |
  | EDGE job per SPU at four SPUs | 68 us | - | 85 us |
  | General6 / RenderJobs SPUs while busy | 2.1 / 1.8 | 1 / 1 | 1.8 / 2.0 |

- The frame clock thread went from 60% to 72% of a core. It is now the likely limit (plan 2.9).

## 4.2 Result of step 3, Batch (2026-10-31, fork `0ff31a6b`)

- On the build after 4.1, a worker running w6 (`PS3_SAMPLE_THREAD`) spent 52% of Batch's
  samples in the SPU interpreter and 38.6% resolving where to go next (`spu_lifted_lookup`);
  7.3% in lifted code. `SPU_INTERP_STATS`: 4.3M interpreted steps a second, Batch 43.7% of a
  core (2226 us a run).
- The interpreted code was position-independent pieces that Batch's module (image 230) runs.
  - Four were not in `scripts/spu_pic_pieces.json`: EA `0x6EEE30`, `0x6F1730`, `0x718C30`
    and `0x7529B0`. With them lifted: 2.1M steps a second, Batch 1056 us a run.
  - The rest was the 2.9 KB piece at `0x718C30`, placed at LS `0x35380`. It never comes from
    its EA: the job GETs 16 KB blocks from PPU heap buffers (`0x720E6DE0` -> LS `0x31DA0`,
    `SPU_DMA_LSA`), and the piece is inside one. So it was never marked resident.
  - It also stores into its own constant pool (`stqr` at +0x834 to +0xB70). The registered
    bytes included the pool, so even a piece found in place failed the compare once it had run.
- The fix (fork `0ff31a6b`):
  - Where dispatch finds nothing and no resident span decides, the 16 bytes there are looked up
    among the starts of the pieces' lifted functions. The piece's instructions are compared
    where that start places them, and on a match the placement is resident, as after a GET
    from its EA. A miss the dispatch cache kept is looked at again, since a block with a piece
    inside changes no resident span. Each SPU adopts the piece once (6 adoptions in a run).
  - `lift_spu_pic.py` registers only the instructions; the lifted code reads the pool from local
    store, as the SPU does.
- After: 0.2M interpreted steps a second; Batch 264 us a run, 5.7% of a core.
- `guestscope.py jobs` on the new build (30 s, 17.8 flips/s): Batch takes 14.7 jobs a frame, its
  lists busy 1.4 ms a frame at 2.2 SPUs, so ~210 us of SPU time a job. Before: ~0.9 ms; RPCS3
  ~110 us. The 8x is now ~1.9x (an EDGE job: 85 us against 68, 1.25x).
- Level, ABBA, two runs each: 18.51 -> 18.59 flips/s, within the spread (runs 18.49/18.59
  against 18.82/18.39). The SPU workers use 520% of a core instead of 554%; the frame
  clock thread stays at 72%.
- So Batch did not set the frame rate. Its priority (2) is above EDGE's, but the 0.38 of a core
  it gave back did not move the frame. The frame clock thread is next (plan 1.4, 1.5), then the
  EDGE job.

## 4.3 The frame clock thread, first lever (2026-10-31, fork `d1e368c7`)

- `PERF=1` on the build after 4.2: the thread's `render_frame` took 19.5 ms a frame, 14.1 of
  it making the draws' texture views (16 `CreateShaderResourceView` calls a draw).
- Views copied from a cache and equal windows shared (plan 1.5): the view work 7.4 ms,
  `render_frame` 10.8 ms.
- Level A/B: 18.69 -> 20.27 flips/s (+8.5%). The port is at ~84% of RPCS3's 24.0.
- The GPU wait in `rf_open` (plan 1.4) was ~10 ms a frame, but after the batch before had
  finished: removing it changed nothing (plan 1.4 for the details and the thread's profile).

## 4.4 What sets the frame after 4.3 (2026-11-01)

- The +25% probes again, one run each, in two orders (the first round's base run was low,
  18.66, against 20.2-20.5 for the same build otherwise):

  | +25% on | round 1 | round 2 | against ~20.3 |
  |---|---|---|---|
  | EDGE (`SPURS_WID_SLOW=14:25,15:25,16:25`) | 18.86 | 19.06 | -6.5% |
  | the frame clock thread (`RSX_SLOW=25`) | 19.85 | 19.63 | -2.8% |
  | the main thread (`GOWA_MAIN_SLOW=25`) | 20.29 | 20.33 | ~0 |

- EDGE sets the frame now, the frame clock thread a little; the main thread has slack. So the
  EDGE job (step 4 above) is next.
- Between the runs `PresentMon_x64` (NVIDIA FrameView SDK, running since 2026-10-17) was seen
  taking about one logical CPU with nothing else running: a likely source of the spread
  between runs.

## 4.5 EDGE: where its workload's time goes, and the indirect branches (2026-11-01)

- The sampler on the workers, EDGE's workload (w14): ~70% lifted and native SPU code (the
  most: the pieces' `304A0` 4.8% and `30600` 3.9%, the module's `7BB4` 4.5% and `D92C` 3.3%);
  ~28% the runtime: indirect dispatch ~6%, DMA copies ~5%, lock-line locks 2.5%, `note_get`
  and `wrch` 1.6% each, drains 2%.
- `SPU_IBFULL=<s>` (fork `ecfe6586`, the indirect branches the full path takes): ~800K a second
  across the workers.
  - Misses: return points a few instructions before entries already lifted (the EDGE module's
    `0x4B14`, `0x7030`, `0xC44C`, `0xC5E0`; others in modules 203, 216, 219, 220, 222), and three
    EDGE pieces entered at their start where their lifted entry is further in. Added to
    `scripts/jobmods_extra.json` and `scripts/spu_pic_pieces.json`: 16M -> 9.7M full-path
    branches in 20 s after the first half, interpreted steps 0.2M -> 0.1M a second.
  - The level: A/B, two runs each, 20.12 -> 20.23 flips/s, within the spread. Kept: less work.
  - What is left by design: the Wws job manager's interrupt vector (`0xA2C`, ~83K a second, below
    the fast path's bound), the register save/restore stub its handler writes at `0x3FEC0` and
    runs per interrupt (~165K a second, run by the micro-interpreter), and the interrupt returns.
    Together ~3% of the workers' time; a lifted copy of the stub would need the stub's bytes
    checked at every run, as SPU stores write it.
- The FIFO self-jump park froze two of ~30 runs on 2026-10-31/11-01, both 31-40 s after the
  start (the menu's first frames): the walker on a jump to itself at IO `0x648BE0` and
  `0x748BE0`, after the same two words (`0x00041778`, `0x0004177C`), no SPU halt in the log. The
  bench reports it and runs again. Found on 2026-11-01: an EDGE job list nobody kicked again,
  now re-kicked ([notes](2026-11-01-menu-freeze.md)).

## 4.6 Where the gap is after plan 2.12 (2026-11-01)

The port at `8d301e3`: 20.66 flips/s against RPCS3's 24.0 (86%). `guestscope.py` again, 40 s of
the fight; the port ran 19.2 flips/s under the capture (it takes a host core), RPCS3 24.7 under
its own (section 2).

| | RPCS3 | port after 4.1 | port now |
|---|---|---|---|
| EDGE phase, ms a frame | 27.6 | 36.5 | 35.9 |
| the frame outside it, ms | 12.9 | - | 16.2 |
| SPUs in an EDGE list while busy | 3.78 | - | 3.76 |
| EDGE's busy time at four SPUs | 86% | 79% | 86% |
| an EDGE job per SPU at four SPUs | 68 us | 85 us | 88 us |
| SPUs in workloads, of six | 5.8 | - | 5.63 |

SPU time a frame per workload (busy time times SPUs):

| workload | RPCS3 | port now |
|---|---|---|
| EDGE, three lists | 104 ms | 135 ms (1.30x) |
| General6 | 35 | 47 (1.35x) |
| audio | 29 | 37 (1.26x) |
| RenderJobs | 31 | 29 (0.94x) |
| RenJobSet2 | 11 | 12 |
| MeshProc | 5.1 | 6.1 |
| Batch | 1.7 | 3.3 |

- The scheduling matches now: the same jobs, and EDGE gets the same SPUs. 8.3 of the 11.6 ms
  between the two frames is the EDGE phase, and all of that is the time a job takes: 88 against
  68 us per SPU.
- The port's EDGE job alone on the replay, one SPU and nothing else running: 45.6 us. RPCS3's
  whole job with one SPU in the list, its policy module's work included: 40 us in the game.
- Where an EDGE worker's time goes (the sampler, plan 2.12's `--spu-lines` build): the job's
  code 62% (lifted 54, native 7.5, the exact float redo 1.3), the policy module 7%, the runtime
  31%: indirect dispatch 9, DMA and MFC 6, copies and compares in the CRT 6.5, lock-line locks
  and coherency 5, channels 3.5. The runtime is 30% of RenderJobs' time, 42% of RenJobSet's,
  4% of General6's.
- The host: the port's process uses 8.2-8.6 logical CPUs, RPCS3 7.4.
  - Six SPURS workers at ~92% each, as RPCS3's six SPU threads.
  - The main thread 81-90% (RPCS3 50-73%) and the frame clock thread 68-71% (`rsx::thread` 87%).
  - Six task threads (`spu job`) at 73-78% that RPCS3 does not have. There a taskset's tasks run
    on the SPURS SPUs, and with "Disable MLAA" the MLAA tasks do not run at all. Here their
    threads mostly wait for a signal, and they sit in the path that kicks an EDGE list (plan 2.6).

What is left, largest first:
1. The EDGE job: the runtime around and inside it, and its code (registers in memory, the exact
   float forms where RPCS3 runs XFloat Approximate).
2. The frame outside the EDGE phase: 3.3 ms longer.
3. The MLAA tasks, which RPCS3 does not run (plan 1.6, 4.3). Now a switch, off unless on
   ([title patches](2026-11-01-title-patches.md)): with it on, 20.61 -> 21.63 flips/s, ~90% of
   RPCS3's 24.0.

## 5. How to measure again

```
# RPCS3: rpcs3_drive.ps1 (scratchpad) boots it with the "automation" keyboard profile.
# Route: Start + Cross through the intro, Cross on Profile 1, left stick up twice (W),
# Cross on New Game, Cross, Cross, Cross on Normal; the fight starts ~132 s later.
py scripts/guestscope.py capture --proc rpcs3.exe --at <Normal + 152> --seconds 90 --out rp.json
# the port: `gowa.py bench` in the background, then (its level windows begin ~120 s after the
# process starts since the menu route, 2026-10-31; 220 before)
py scripts/guestscope.py capture --proc GowARecomp.exe --at 125 --seconds 60 --out port.json
py scripts/guestscope.py report rp.json:<fps> port.json:<fps>
py scripts/guestscope.py jobs rp.json:<fps> port.json:<fps>
py scripts/guestscope.py edge rp.json port.json
```
