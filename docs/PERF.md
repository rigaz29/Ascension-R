# Speed: where the frame goes, and the order of work (roadmap step 7)

Measured 2026-10-27 on the development machine (Ryzen 5 4600H, 6 cores / 12 threads, GTX 1650 Ti,
Windows 11), port `8b59dca` + fork `a2b01654` with the tools below. All numbers come from the runs
listed at the end; nothing here is estimated unless it says so.

**Target** ([ROADMAP.md](ROADMAP.md) step 7): the first level at a steady 30 flips/s or more on
the machine above, with only `GOWA_THREADS` on. Each lever is kept only if an A/B shows more
flips/s; less CPU alone is not a reason. At 2026-10-28: menu ~20, first level ~7.3 (median) in
the morning, ~11.9 after the texture cache in the evening.

**2026-10-28: the target is now RPCS3's speed on this machine, or better.** What is measured, what
is suspected, and the work in order: [performance map](notes/2026-10-28-performance-map.md).
Paused the same evening for rendering accuracy (wrong colours, dropped draws, flicker, stutter,
freezes): [rendering accuracy plan](notes/2026-10-28-rendering-accuracy-plan.md).

**2026-10-29: speed again, at 12.3 flips/s.** The frame measured afresh (a chain of SPU job
lists, 80 ms; the GPU 25% busy at 645 MHz) and the work in phases, from faster measurement to
native and GPU replacements of the hottest SPU jobs:
[performance plan](notes/2026-10-29-performance-plan.md). It replaces the performance map's
order of work.

**2026-10-31: the port at 16.3 flips/s, RPCS3 at 24.0 FPS**, measured the same afternoon in the
same fight and the same windows (RPCS3 with the user's settings, its "Disable MLAA" patch on as
it was on 2026-10-29). The port is at 68% of RPCS3's speed, from 54%: plan section 1.

| Lever | Done | Menu | Level | Note |
|---|---|---|---|---|
| The PPU's `lvlx`/`lvrx`/`lvsl`/`lvsr`/`vperm` as SSSE3 code instead of byte loops; its word compares, `vmaxfp`/`vminfp`, estimates, roundings, `vcfsx` and `vctsxs` as vector code (fork `a015ff00`) | 2026-10-31 | not measured | 16.57 median (16.42 before; ABBA, two runs each, within the spread) | The main thread's own work 11.58 -> 10.73 ms a frame (-7.3%; `GOWA_MAIN_SLOW=1`, the same 18.4K indirect calls a frame either way). 27,023 lifted sites; the executable 123 -> 112 MB; every vector form equals its scalar form in 55.5M operations ([audit](notes/2026-10-31-technique-audit.md), F1) |
| EDGE's normal loop (LS `0xA950`: cross product, scale, normalise) as native AVX2 code (`native/spu_edge_normals.c`) | 2026-10-31 | not measured | 16.26 median with both native EDGE loops (16.04 with `GOWA_EDGE_NATIVE=0`; ABBA, two runs each, both native runs above both off) | Replay: EDGE jobs 54.1 -> 52.0 us on top of the skinning loop, outputs identical. In the level 56.6M iterations, none needed the exact forms ([plan](notes/2026-10-29-performance-plan.md), 2.8) |
| EDGE's skinning loop (LS `0xEC08`) as native AVX2 code, the two vertices of an iteration in one register (`native/spu_edge_skin.c`, `scripts/spu_native_hooks.json`) | 2026-10-31 | not measured | 16.06 median (15.88 with `GOWA_EDGE_NATIVE=0`; ABBA, two runs each, both native runs above both off) | Replay: EDGE jobs 56.7 -> 53.7 us, outputs identical in all 40 captured jobs. In the level 0.05% of its iterations fall back to the lifted loop (a result out of the single range) ([plan](notes/2026-10-29-performance-plan.md), 2.8) |
| The PPU's VMX float ops (`vaddfp`, `vsubfp`, `vmaddfp`, `vnmsubfp`) as vector code (fork `b5a91584`) | 2026-10-31 | not measured | 16.14 median (15.68 before; ABBA, two runs each, both new runs above both old) | `vmx_vmaddfp` alone was 5.5% of the main thread, a byte at a time and out of line. The main thread's own work decides when each EDGE batch can start: it releases the MeshProc job that kicks the batch. Bit-exact against the scalar forms in 12M random ops ([plan](notes/2026-10-29-performance-plan.md), 2.8) |
| The common SPU indirect branch and drain call out of their slow paths' frames (fork `d3ff2bdb`) | 2026-10-30 | not measured | 15.88 median (15.75 before; three runs each, alternating; within the spread) | Every branch and every return from a lifted call set up the slow path's frame (eight registers, xmm6, a stack cookie; a jmp_buf for the drain). Replay: RenderJobs -3.5%, RenJobSet -2% to -2.6%, outputs identical ([plan](notes/2026-10-29-performance-plan.md), 2.2) |
| The SPU generate-controls (`cwx` & co.) as vector compares; no stack cookies in the lifted SPU code (`/GS-`; fork `86e29fdc`) | 2026-10-30 | not measured | 15.68 median (15.28 before; three runs each, alternating, no overlap) | Built in memory, an insertion control was read back in a way that missed store-to-load forwarding: `cwx` alone was 7.7% of RenderJobs' lifted code. Replay: RenderJobs -15% a job, RenJobSet w10 -15%, w9 -10%, outputs identical ([plan](notes/2026-10-29-performance-plan.md), 2.7) |
| The SPU coherency notifiers scan an array of reserved lines instead of walking every SPU context (fork `54bdd0eb`) | 2026-10-30 | not measured | 15.49 median (14.50 before; three runs each, alternating, no overlap) | Every atomic commit and every write to a reserved line walked ~99 contexts under the global lock-line lock, ~400,000 times a second, to find 0.13 holding the line on average. That lock was 5-17% of RenJobSet's and EDGE's time, and both bound the frame ([plan](notes/2026-10-29-performance-plan.md), 2.7) |
| Speculative float bodies for float-heavy straight-line lifted SPU functions (`spu_lifter.py --float-spec 4`; fork `39bbe622`) | 2026-10-30 | not measured | 14.31 median (14.39 before; three runs each, alternating) | 154 functions in 23 images: SPU registers in locals, native float ops checked once at each exit, the exact body as the fallback. General6 -12% a job on the replay and -10% a run in the game (1,240 -> 1,112 us), outputs identical. The level does not move: General6 no longer bounds the frame ([plan](notes/2026-10-29-performance-plan.md), 2.1) |
| A short path for the common SPU indirect branch; MFC command parameters stored inline (fork `6a2015f4`) | 2026-10-30 | not measured | 14.21 median (13.80 before; three runs each, alternating) | RenderJobs, one lane of the frame's critical path, -3.8% a job on the replay, outputs identical ([plan](notes/2026-10-29-performance-plan.md), 1.2) |
| Lifted SPU code's LS accesses without the diagnostic gate (`PORT_SPU_LS_DIAG` off; fork `c0925006`) | 2026-10-30 | not measured | 13.52 median (13.05 before; three runs each, alternating) | a load and a branch less at every lqd/stqd: RenderJobs jobs -7.7%, EDGE -10.5% on the replay, outputs identical ([plan](notes/2026-10-29-performance-plan.md), 1.2) |
| The main thread's polling loops wait with MWAITX (lift hooks, `poll_wait.cpp`; fork `2accf30f`) | 2026-10-30 | not measured | 12.92 median (12.70 before; three runs each, alternating) | 20 us waits after 8 passes free the core for its SMT sibling. The frame's critical path is RenderJobs -> MeshProc -> General6 -> the main thread, two one-lane job lists that run 33-55% slower in the game than alone; a 0.5 ms OS sleep and a core kept for that chain did not help ([plan](notes/2026-10-29-performance-plan.md), 1.3, 1.7) |
| SPU dispatch: misses cached, a reload of the same code stays verified (fork `cc8af52a`) | 2026-10-30 | not measured | 12.61 median (12.02 before; ABBA, two runs each) | every 5 s: full resolutions 25M -> 4.2M, streamed code compared 7.3 -> 0.26 GB ([plan](notes/2026-10-29-performance-plan.md), 1.2) |
| SPU float: leaner exact forms; `PORT_SPU_FLOAT_APPROX` option, off (fork `6544cbc8`) | 2026-10-30 | not measured | 11.86 median (11.71 before; ABBA, two runs each); 11.96 with the option | replay: General6 jobs -20%, EDGE -14%, bit-identical; the frame rate barely follows. Native float without the range check breaks EDGE's culling masks (13 of 40 jobs differ), so the planned approximate mode is not used ([plan](notes/2026-10-29-performance-plan.md), 1.1) |
| ZPASS reports carry the GPU's occlusion counts (fork `5c906fb2`) | 2026-10-29 | not measured | 12.32-12.51 median (12.12-12.20 with every query "visible"; two runs a side, alternating); again in ABBA order, one executable: 11.69 against 11.21 with `RSX_ZPASS=0` (+4.3%) | a correctness change first: of the ~400 queries a frame ~30% now come back hidden and the title skips those objects; screenshots at 150/250/350 s show nothing missing ([plan](notes/2026-10-28-rendering-accuracy-plan.md), section 5) |
| Rendering accuracy step 3, stutter: textures in heaps, upload spill chunks, polygon offset per draw, own-target copies at cache invalidates, paced audio, vblank at 59.94 Hz (fork `2e681a20`) | 2026-10-29 | not measured | 12.2-12.3 (11.4-11.9 before) | the gain is the polygon offset's (pipelines made a minute 2,000-3,000 -> 12-57); the rest is stutter: around entering the level frames over 150 ms 3-8 (0.9-2.3 s) -> 3 (0.73 s), GPU wait at frame starts 194-317 -> 19-77 ms; audio gaps 27-35 -> 5-7 per 5 s ([plan](notes/2026-10-28-rendering-accuracy-plan.md), section 5) |
| Rendering accuracy step 3 so far: FP constants from b1, programs read to their end, per-draw b1 slots, texture takeover, no shader blob shared between threads (fork `531d2252`) | 2026-10-29 | not measured | 11.4-11.9 (8 runs; 10.4-10.6 after step 2) | the gain is `c2e87108`'s (10.58 -> 11.38: one pipeline per program, not per set of constants); after it no program is compiled and no draw skipped in a minute of the level, and the heap corruption that ended 8 of 15 runs is gone ([plan](notes/2026-10-28-rendering-accuracy-plan.md), section 5) |
| Rendering accuracy step 1: FP precision, MAX_DRAWS 8192, polygon offset, mip chains (fork `8c2916e2`) | 2026-10-28 | not measured | 10.3-10.9 (11.9 before) | not a speed lever: the frame now draws the ~50% of its draws that were dropped and uploads whole mip chains; kept for correctness ([plan](notes/2026-10-28-rendering-accuracy-plan.md), section 5) |
| D3D12 texture cache bounded by bytes, hash lookup (fork `39ffb3b3`) | 2026-10-28 | not measured | 11.9 median (9.2 before), +29% | the 192-slot cache evicted and re-uploaded 20-50 textures a frame (25-55 ms on the frame clock thread) and refused 400-2,200 binds, which drew the sky black; now 2-3 ms a frame. Skipping unchanged null SRVs on top: -4 ms a frame on that thread, +1%, not kept ([map](notes/2026-10-28-performance-map.md), section 6) |
| SPU float forms chosen at compile time (`PORT_SPU_FLOAT_RZ_ONLY`) | 2026-10-28 | not measured | 9.46 median (9.40 before) | SPU job time -3..-15% (General6 1,643 -> 1,403 us), bit-identical, executable 6 MB smaller; kept as the leaner float forms were. Job modules with `--merge-chunks` on top: 9.54, not kept (+13 MB of C). `TEX_BUDGET=50` probe: 12.1-12.3 flips/s, so the D3D12 texture work is on the frame's path ([notes](notes/2026-10-28-spu-codegen-design.md)) |

## 0. Progress

| Lever | Done | Cinematic | Menu | Level | Note |
|---|---|---|---|---|---|
| baseline | 2026-10-27 | 21.7 | 0.40-0.50 | 0.17-0.23 | flips/s |
| L1 job modules | 2026-10-27 | 45-48 | 2.03 | 3.2 on arrival, 1.1-2.1 after | [notes](notes/2026-10-27-job-modules.md) |
| L3 usleep timer, L4 task logs | 2026-10-27 | 50-52 | 2.0 | not measured | process CPU 447% -> 352%; [notes](notes/2026-10-27-host-waste-and-lanes.md) |
| EDGE on three SPUs | 2026-10-27 | 48-50 | 3.6 | 2.4-4.4 | `SPURS_MAXCONT`, port default |
| L2 EDGE fragments (26, position-independent) | 2026-10-27 | 48-50 | 13.7-15 | 9.2-10.5 on arrival, 4.8-6 later | interpreted steps 40 -> 1.3-4 M/s; EDGE 1.3-1.8 -> 0.20-0.24 ms a run; menu now bound by workload 2 (98%); [notes](notes/2026-10-27-l2-analysis.md) |
| re-measured with the 3D scenes drawn | 2026-10-27 | 44-50 | 3.4-3.5 | 2.0-5.3 on arrival, 1.2-1.5 later in the opening scene | profile screens 14.7; process CPU 550-640%, `spurs pm` x6 400-450% in menu and level; with `SPURS_MAXCONT=14=1,15=1,16=1`: menu 1.9-2.0, profile screens 9, level 1.1-2.4. Geometry order with EDGE on three SPUs: the same poses render the same as on one SPU, no broken meshes |
| L7a SPU single precision on the host's RZ mode (x86), `shufb` | 2026-10-28 | not measured | 18.0-19.1 (13.0 with `SPU_FLOAT_RZ=0`) | 5.0-6.9 (4.4-5.9 with `=0`) | per-thread profiler: float emulation was ~70% of workload 2, 25-38% of audio and EDGE; bit-identical (unit test, and 8.4 billion ops checked in game); [notes](notes/2026-10-28-l7-analysis.md) |
| MFC engine lookup cached per thread | 2026-10-28 | not measured | 19.7-20.4 (19.05 with `SPU_MFC_LOOKUP_CACHE=0`) | 5.7-8.0, median 7.31 (6.26 with `=0`) | the locked walk over every context's DMA engine was ~10% of the SPU workers; the main thread polls the job graph, so L8 waits; [notes](notes/2026-10-28-level-after-l7.md) |
| EDGE job cost: `/GS-`, coherency holder counts, `preserve_none`, leaner float forms | 2026-10-28 | not measured | not measured | no change (7.3-7.45 median either way) | each a few percent of an EDGE run, ~1 ms of a 137 ms frame; only the float forms kept (less code, bit-identical); the main thread waits ~81% of the frame, SPU workers half idle meanwhile ([L8 notes](notes/2026-10-28-l8-main-thread.md)); [notes](notes/2026-10-28-edge-job-cost.md) |
| L6 a SPURS worker picks its next workload itself | 2026-10-28 | not measured | 22.8-23.7 (20.4-21.5 before) | 9.44 median (7.15 before), +32% | the hand-off between policy-module runs was two thread wake-ups, ~36 us against a ~140 us EDGE run; now ~3 us ([notes](notes/2026-10-28-l8-main-thread.md)) |

## 1. Baseline

| Scene | How it is reached | Flips/s | Frame (ftrace median) | What bounds it |
|---|---|---|---|---|
| First-use calibration | first boot only, ~55 s | 2.4 | not traced | not measured |
| Opening cinematic | every boot, ~30-250 s | 21.7 (target 29.97) | 45.7 ms | the main PPU thread: blocked only 7 ms of the 45.7 |
| Main menu (3D background) | boot, no input, from ~300 s | 0.4-0.5 | 2120 ms | SPU: workload 2 (General6), one job at 77 ms a run |
| Story cinematic | New Game, Normal | 10-12 | not traced | not measured |
| First level (`R_MEGAERAN`) | the route in section 6, from ~700 s | 0.17-0.23 | 4588 ms | SPU: workloads 2 (192 ms a run) and 3 (96 ms a run) |

The process uses 5.1-6.3 of the 12 logical CPUs. The frame is not bound by the RSX walker on any
of these screens (drain 4-9 ms a frame) or by the D3D12 backend.

## 2. Where a frame goes

### Main menu (frame 2120 ms)

- **Main thread**: blocked 1617 ms in `usleep@0041D5C8`, the per-slot frame wait. It waits for
  SPU work; its own time is ~500 ms.
- **SPU** (six policy-module workers, 276% of one core): workload 2 (General6) 94% of one core,
  12 runs a second at 77 ms each; EDGE 0/1/2 (14-16) ~100% together; audio (7, BrBDblBufWrkld)
  45%; RenderJobs (12) 15%.
- **Interpreter**: 13-25 M SPU steps a second. The hottest pages are LS `0x4800-0x5400` of the
  job-module image, then `0xD800` and `0x11400`.

### First level (frame 4588 ms)

- **Main thread**: busy 99%, blocked 3.8 ms a frame. The sampler puts 67% of its samples in
  `func_0041D4E8` at `loc_0041DB50`, a spin loop with no syscall on a halfword at
  `[*(r30+0xC)+0xE0]`. Another 13% is in `func_0049D688`, a scan loop with the `or r31,r31,r31`
  spin hint. So the main thread mostly **waits** here too, without sleeping.
- **SPU** (397-413% of one core):

  | Workload | Busy (one core) | Runs/s | Per run |
  |---|---|---|---|
  | 2 General6 | 100% | 5 | 192 ms |
  | 7 BrBDblBufWrkld (audio) | 80% | 3129 | 0.26 ms |
  | 3 SysMisc | 55% | 6 | 96 ms |
  | 14/15/16 EDGE0/1/2 | 29/28/37% | 24-32 | 11-12 ms |
  | 12 RenderJobs | 22% | 27 | 8 ms |
  | 11 RenJobSet | 15% | 143 | 1 ms |

- **Interpreter**: ~12 M steps a second, at LS `0x4800-0x5C00`, `0x7C00`, `0xEC00`.

### Opening cinematic (frame 45.7 ms)

The main thread is busy ~38 ms a frame (`cellVdecGetPicture` 2.7 ms, `cellVdecDecodeAu` 2.0 ms).
Workload 4 (ModelServer) takes 73% of one core at 5.8 ms a run. Taskset task threads ("gone" in
the thread table) take 30-45% of a core.

### Waste on the host side (every scene)

- `usleep` under 1 ms spins (`SwitchToThread` to a QPC deadline). `EdgeWatchdog` polls with
  800 us sleeps (12k calls in 10 s) and holds a core at 97-99%. `IntroLoaderThread` calls
  `usleep(30)` 320k times in 10 s on the menu.
- Each taskset task starts a new host thread: ten a frame in the cinematic.
- Every task dispatch and start logs about five lines (60+ lines a frame in the cinematic).

On 6 cores, two to three spinning threads compete with four busy SPU workers.

## 3. Why the SPU side is slow

**The executable holds 31 WWS job modules (`C0DEC0DE` headers); 6 are lifted.**
`lift_jobmods.py --engine-elf` accepts only headers with `0x4000` at `+0x28`. The other 25 have
`0` there, and their code starts at `+0x30`, as the lifted ones' does at `0x4030`. The policy module
loads every module at LS `0x4000`, and the interpreter's hottest pages are in that range. Ref A
found the same: its 33 embedded modules lifted at `0x4000` took interpreted WWS job time from 90-94%
to 0 and the frame rate from 1.5 to 2.8 (its M0.27). A job that takes 77-192 ms interpreted is
expected to take a few ms lifted (estimate; ref A's EDGE job went to 0.3 ms).

**Streamed code fragments** (EDGE and other jobs) run at LS `0xD800`, `0xEC00` and `0x11400`, and
are entered from lifted image `0xCA`. Ref A lifted one image per (source EA, LS placement); ref B
added position-independent lifting (`--pic`).

The lifted SPU code is already built with `-O2 -msse4.1 -mavx2` (`CMakeLists.txt`), which ref A
(M0.28) and ref B (0024) had to add.

## 4. Levers, in order

Each lever has the measurement that says it worked. "Gain" is the evidence for the expected effect.

| # | Lever | Side | Gain (evidence) | Measure | Risk |
|---|---|---|---|---|---|
| L1 | Lift the 25 job modules with `0` at `+0x28`, at LS `0x4000`, with extra entries from `SPU_INTERP_STATS` | SPU | Workloads 2 and 3 are 70% of SPU time in the level; ref A +87% | interpreted steps at `0x4000-0x7FFF` near 0; w2/w3 us/run; menu and level flips/s | the header variant's meaning; check the load address once with `SPURS_PM_TRACE_WID=2` |
| L2 | Lift the streamed fragments at their LS placements | SPU | EDGE ~94% of a core; ref A 2.8 -> 8.6 with modules and fragments | interpreted steps at `0xD800`/`0xEC00`/`0x11400`; EDGE us/run | placements may vary (ref B); start with the fixed ones |
| L3 | `usleep` of ~200 us and more on a high-resolution waitable timer | host | 1-2 cores back on a 6-core host (ref B 0034: same rate, 6% less CPU) | process CPU; `EdgeWatchdog`/`IntroLoaderThread` in the thread table; flips/s | sleep overshoot; keep short waits spinning |
| L4 | Per-task and per-dispatch log lines behind `PS3_VERBOSE` | host | ref A M0.29: 18 -> 27 frames/s | log bytes per second; cinematic flips/s | none |
| L5 | A thread pool for taskset tasks instead of a thread per start | host | 30-45% of a core in the cinematic is in threads that end within 10 s | started/ended per window; "gone" CPU | task lifetime and per-taskset LS reuse |
| L6 | SPURS scheduling latency between dependent jobs | SPU | ref A greedy passes 2.9 -> 6.2; ref B report wake 24 -> 9 ms of EDGE | gaps between dependent spans in `PS3_FTRACE` | measure first: no evidence yet here |
| L7 | Hot SPU helpers in lifted code (selb, rotqby, float compares, FP mode) | SPU | ref B 0027/0032/0033 (menu 12 -> 30) | host samples on the workers (`PS3_SAMPLE` + `--rvas`) | after L1/L2, when the code is lifted |
| L8 | The main thread's own work once SPU stops bounding: `func_0044CA48`, `func_0044CD20`, guest stores (`vm_write64`, ref A/B 10-18%), the cinematic decode | PPU | only visible after L1 | main busy minus spin in the sampler; cinematic flips/s | none yet |

Audio (workload 7) is real work at 45-80% of a core; it stays a fixed cost unless the interpreter
shows it.

## 5. Order of work

1. **L1** (job modules), done: all 31 lifted, with 178 extra entries found from the runs; menu
   x4, level x6-15. Interpreted steps left are at the fragments' pages, not `0x4000-0x7FFF`.
2. **L3, L4, L5** together (host waste, small and independent). Exit: process CPU and the spinning
   threads down, flips/s not lower.
3. **L2** (fragments). They are position-independent: the same fragment runs at changing LS
   addresses, so it needs PIC lifting (ref B 0023/0031), not one lift per placement. Exit:
   interpreted steps under 1 M/s overall. Done for the 26 EDGE fragments (>99% of the
   interpreted steps): 1.3-4 M/s left, mostly the `0x718C30` block
   ([notes](notes/2026-10-27-l2-analysis.md)).
4. Measure again (section 6) and take L6, L7 or L8 by what bounds the frame then.
   Done for L7's first two items (float on the host's round-toward-zero mode, `shufb`): menu +42%
   ([notes](notes/2026-10-28-l7-analysis.md)); the per-thread profile says what is next.

Expected after 1-3 (estimate): the level bound moves from the job modules to EDGE, audio and the
main thread; ref A was at 1-1.5 level frames/s at the equivalent point and went to ~10 with the
later work (lanes, native EDGE loops, walker costs).

## 6. How to measure

**Scenes.**

- *Cinematic*: boot, no input, windows at 10-60 s.
- *Menu*: skip the opening movie, then windows from 20 s after it closes.
- *Level*: from the movie's close: 45 s, Cross; 35 s, Cross; 25 s, Cross; 25 s, Cross; the level
  from ~165 s. Windows from 165 s.

Square skips the opening movie (Start does not): press it every few seconds from 15 s until the log
shows `[cellVdec] Close`. Times above count from that line.

**Switches** (all off by default):

| Switch | What it gives |
|---|---|
| `GOWA_THREADS=<s>` | flips and presents per second, CPU per named host thread, threads started/ended (cheap; keep it on) |
| `SPURS_WID_PROF=<s>` | policy-module time per workload: busy, runs/s, us/run, empty runs |
| `SPU_INTERP_STATS=<s>` | interpreted SPU steps per second, hottest pages and entries |
| `PS3_FTRACE=<start>:<secs>:<file>` + `ps3recomp/tools/ftrace_report.py` | per-frame timeline: PPU blocking by call site, SPU busy by workload, RSX drain |
| `PS3_SAMPLE=<ms>` `PS3_SAMPLE_MAIN=1` `PS3_SAMPLE_RVAS=<file>` + `gowa.py symbolize --rvas <file> --last <s>` | the main thread's time by lifted PPU function and runtime function |
| `PS3_SAMPLE_THREAD=spurs pm,ppu main,...` with the above, + `symbolize --thread <name> --wid <n> --lines` | the same per named thread; a SPURS worker's samples also by the workload it ran; with a `build --spu-lines` build, the lifted SPU samples by SPU instruction and helper |
| `PS3_POLLTOP=<s>` | `usleep` call sites by thread |

**Rules.**

- One change at a time.
- Two runs per side, alternated; compare medians of 30 s windows.
- Same power state (on the charger, same power plan).
- Frame-rate claims with only `GOWA_THREADS` on: the other switches cost time.
- Keep the opening cinematic's 21.7 flips/s as the noise check, since it does not depend on the
  route.

**Tooling still to add.**

- `gowa.py bench --scene cinematic|menu|level`: start, drive the route with `PAD_FILE`, collect
  the windows, write JSON, and compare with a saved baseline (as `gow1hd.py check` does).

## Runs behind these numbers

- Thread tables through boot, cinematic, menu and the profile screens.
- `PS3_FTRACE` windows on the cinematic (220-240 s) and the menu (390-410 s).
- `SPU_INTERP_STATS` on the menu.
- One instrumented run to the level: trace 760-785 s, workload and interpreter tables.
- One main-thread-sampled run to the level (`symbolize --rvas`, last 100 s).
