# Speed: where the frame goes, and the order of work (roadmap step 7)

Measured 2026-10-27 on the development machine (Ryzen 5 4600H, 6 cores / 12 threads, GTX 1650 Ti,
Windows 11), port `8b59dca` + fork `a2b01654` with the tools below. All numbers come from the runs
listed at the end; nothing here is estimated unless it says so.

**Target** ([ROADMAP.md](ROADMAP.md) step 7): the first level at a steady 30 flips/s or more on
the machine above, with only `GOWA_THREADS` on. Each lever is kept only if an A/B shows more
flips/s; less CPU alone is not a reason. At 2026-10-28: menu ~20, first level ~7.3 (median).

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
