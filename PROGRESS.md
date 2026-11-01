# Progress

Current status and milestones. The plan is in [PLAN.md](PLAN.md).

## Status

| Phase | Status |
|---|---|
| 0 Setup | done: repo, submodule (`rigaz29/ps3recomp`, branch `gowa`), `scripts/gowa.py`, decrypted executables |
| 1 Analysis | done: see [docs/ANALYSIS.md](docs/ANALYSIS.md) |
| 2 First build | done: `GowARecomp.exe` links, with the 4 SPU programs, the `Wws_Job` policy module and the lifted `libsre` |
| 3 First boot | done: the title reaches its first-use screens (brightness calibration, then screen adjust), which read correctly and take pad input; ~2.4 frames/s |
| 4 Missing HLE | boot path done: SPURS2, Taskset2/Task2, workload flag, `cellGamePatchCheck` |
| 5 SPU | started: the 4 embedded SPU programs (post-process, two-phase particle tasks, Edge Zlib), the `Wws_Job` policy module and its 6 engine-embedded job modules run lifted |
| 6-13 | not started |

## Milestones

| Date | Milestone |
|---|---|
| 2026-10-26 | `EBOOT.BIN` and `GOWA.SELF` decrypted (RPCS3). `EBOOT.BIN` is a launcher that starts `GOWA.SELF`, which is what the port lifts. `GOWA.elf`: 7,643 functions, about 8.4 MB of PPU code, 350 imports from 25 libraries (259 registered in the runtime), 4 embedded SPU ELFs. The port's files are carried over from the God of War HD port (script, build, runner, settings window). |
| 2026-10-26 | First build (22,320 PPU functions, 188 MB of C++) and four boots ([notes](docs/notes/2026-10-26-first-boot.md)). Runtime: the SPURS2 entry points, `cellGamePatchCheck`, and reachable SPU function detection for programs that keep data in their code segment. Port: the Edge Zlib queue through the lifted `libsre`, and the `Wws_Job` policy module lift, both from the God of War HD port. No import on the boot path is unregistered. |
| 2026-10-27 | Runtime branch merged with upstream (380 commits). The `Wws_Job` job modules linked into `GOWA.elf` are lifted (upstream `lift_jobmods --engine-elf`, with per-module extra entries), so jobs run lifted instead of interpreted. The GCM offset tables the title reads directly now follow every map and unmap: the intro loader's `put` was -1. ([notes](docs/notes/2026-10-27-jobs-and-intro-loader.md)) |
| 2026-10-27 | SPURS2 with 32 workloads (EDGE2 and RsxSync were refused), the MLAA tasks no longer signalled at creation, Taskset2 event flags, the RSX waiting on its post-process semaphore. Two other ports of this title studied for the frame pipeline ([notes](docs/notes/2026-10-27-frame-pipeline.md)). |
| 2026-10-27 | Roadmap steps 0 and 1: a frame pipeline probe (`GOWA_PIPE`), a per-workload policy trace, a relift with the merged lifter; EDGE2 and RsxSync's policy modules now run (a wid < 16 check), RsxSync takes its job list and the MLAA tasks get their batches ([notes](docs/notes/2026-10-27-rsxsync.md)). |
| 2026-10-27 | **First screen.** Roadmap steps 2 and 3: a report waits while the post-process tasks run a batch (batch N+1's signal ended batch N's barrier waits), and tasks an SPU creates in a sibling taskset now start (the two-phase particle work). The brightness calibration screen is drawn and Cross moves on ([notes](docs/notes/2026-10-27-first-screen.md)). |
| 2026-10-27 | Readable text: the D3D12 path binds the X16 font atlas with the right view format, the fragment decompiler does DDX/DDY, and the shader packer's sRGB output and texture gamma are modelled ([notes](docs/notes/2026-10-27-text.md)). |
| 2026-10-27 | **To the first level**, driven with `PAD_FILE`: both cinematics play, then the main menu, profile creation (save data written), difficulty, and the first level (`R_MEGAERAN`) with its pause menu. Its 3D scene is almost black, at ~0.35 frames/s ([notes](docs/notes/2026-10-27-to-the-first-level.md)). |
| 2026-10-27 | Speed mapped ([docs/PERF.md](docs/PERF.md)): per-thread CPU (`GOWA_THREADS`), per-workload SPU time (`SPURS_WID_PROF`), traces and samples of the menu and the level. Both are bound by SPU jobs running in the interpreter: 25 of the 31 WWS job modules are not lifted. |
| 2026-10-27 | All 31 WWS job modules lifted (25 had a header form the lift skipped; two needed callback entries found from the runs): cinematic 21.7 -> 45 flips/s, menu 0.5 -> 2.0, first level 0.2 -> 1.1-3.2 ([notes](docs/notes/2026-10-27-job-modules.md)). |
| 2026-10-27 | `usleep` from 200 us on a high-resolution timer (a core back), per-task logs quiet, EDGE lists on three SPUs: cinematic 50, menu 2.0 -> 3.6, level 2.4-4.4 flips/s. The remaining interpreted code is position-independent EDGE fragments ([notes](docs/notes/2026-10-27-host-waste-and-lanes.md)). |
| 2026-10-27 | **3D scenes on screen.** The D3D12 path binds texture units 4-15, draws inline arrays (the whole post chain), keeps each display buffer as a target of its own, replays the display copies the post chain reads, and handles a pass that samples its own target and memory used as both depth and colour. The main menu and the first level's opening scene render, without the yellow flashes ([notes](docs/notes/2026-10-27-render-units.md)). |
| 2026-10-27 | **L2: EDGE fragments lifted.** The 26 position-independent code pieces the EDGE jobs stream to changing LS addresses run lifted (`spu_lifter.py --pic`, runtime regions resident wherever they land): menu 3.5 -> 15 flips/s, first level 2-5 -> 5-10. Flips on the title's own command buffers are queued in the FIFO again, so the menu keeps its UI at that speed ([notes](docs/notes/2026-10-27-l2-analysis.md)). |
| 2026-10-29 | **The FIFO park freeze, fixed.** It was an SPU task's assertion: the particle work's first phase creates tasks in a sibling Taskset2 and checks each one's exit-code entry, and a task the runtime ran to its exit first left 2 there where libsre writes 0x12. Progress on the rendering plan, the seams and the menu flicker are in [the plan](docs/notes/2026-10-28-rendering-accuracy-plan.md), section 5. |
| 2026-11-01 | **The menu-start freeze, fixed** (about one run in fifteen, right after the opening movie). The main thread re-arms the counter an EDGE job list's first entry waits on, for the next frame's list. When it did so before the kicked list's policy module had read it, that list was never kicked again, and the RSX stood at its first hole. A park that stands 100 ms now re-kicks such a list (`GCM_PARK_STALL_MS`). A/B on one binary: 2 of 22 runs froze without it, none of 22 with it ([notes](docs/notes/2026-11-01-menu-freeze.md)). |

## Next

Speed, toward RPCS3's on this machine or better (first level at 18.9 flips/s on 2026-10-31 after
the SPURS fixes, from 16.2; RPCS3 24.0 FPS in the same fight, so the port is at ~79% of it; 12.3
against 22.9 on 2026-10-29): the [performance plan](docs/notes/2026-10-29-performance-plan.md). Phase 0 is
done (`gowa.py bench`, `gowa.py frames`, SPU job capture and `gowa.py replay`, RPCS3 measured at
the same scene). Phase 1.1 is done: leaner exact SPU float forms (General6 jobs -20%, EDGE -14%,
bit-identical); native float breaks this title's EDGE masks, so no approximate mode by default.
The level moved only 1-2%: its critical path is a serial chain of two one-lane job lists
(RenderJobs, then General6) and the main thread between frames. Dispatch is done (misses cached,
reloads of the same code stay verified): 12.0 -> 12.6 flips/s; the main thread's polling loops
wait with MWAITX: 12.70 -> 12.92; lifted SPU code without the LS diagnostic gates: 13.05 ->
13.52; a short path for the common indirect branch and inline MFC parameters: 13.80 -> 14.21.
Phase 2 (code generation) has started. Speculative float bodies for General6's float-heavy
functions made its jobs 10% shorter in the game but left the level as it was (14.39 / 14.31):
General6 no longer bounds the frame. A fresh trace and a probe that slows one workload at a
time found the three that do: RenJobSet, EDGE and RenderJobs (performance plan, 2.7). Their
runtime share came first: the SPU coherency notifiers now scan an array of reserved lines
instead of walking ~99 contexts under the lock-line lock, 14.50 -> 15.49; vector insertion
controls (`cwx` & co.) and no stack cookies in the lifted SPU code, 15.28 -> 15.68. Next: the
crossing between lifted functions (`--merge-chunks` again, `preserve_none`), dispatch, then
the rest of their lifted code; no full GPU drain per batch, descriptors, MLAA on the GPU. On 2026-10-31 Batch's SPU code went from the interpreter to lifted code (position-independent pieces found by their bytes): 4.3M -> 0.2M interpreted steps a second and a third of a core back, the level as it was (18.51 / 18.59), so the frame clock thread (plan 1.4, 1.5) is next. Its texture views are now copied from a cache instead of made per draw (14 -> 7.4 ms a frame): 18.69 -> 20.27 flips/s, ~84% of RPCS3's 24.0. On 2026-11-01 EDGE's two fixed-point vertex decoders (position-independent pieces) became native code, the small one two vertices to a 256-bit op, checked against their lifted loops on random inputs and by replay: 19.42 -> 20.09, then 20.03 -> 20.17 with the second; EDGE jobs -5.6% in replay between the two (performance plan, 2.11). Then the broad levers, one at a time: job modules lifted as merged runs of chunks (but General6's) and shufb controls a loop doesn't change prepared before it, 1-8% off most workloads' jobs in replay and 19.98 -> 20.66 flips/s at the level; `preserve_none` and a hoisted `ctx->ls` were slower and are not kept (2.12). RPCS3 runs with its Disable MLAA patch on; the port's title patches are now switches, off unless switched on (`[patches]` in gowa.ini): Disable MLAA gives 20.61 -> 21.63 flips/s, ~90% of RPCS3's 24.0, Depth of Field, Motion Blur and Mesh Trimming (SPU edits in the EDGE modules) are there too, with a Game patches group in the launcher ([notes](docs/notes/2026-11-01-title-patches.md)). Switching MLAA off showed the D3D12 backend compiling a shader for each binding of a fragment program and building some from programs the title had already written over; now a program is one shader, built from the bytes its draw recorded: 1624 -> 305 pipelines a run, and with MLAA off no new shader after the first run (2.13). Level, ABBA, two runs each: MLAA on 20.03 -> 20.20 flips/s (within the spread); MLAA off 21.94 -> 22.33, without the compile dips (lowest window 18.9 -> 21.6). With MLAA off the frame clock thread (77-79% of a core) now presents ~0.8 a second fewer frames than the title flips: the next limit there. Doing each fragment program's per-draw work once a batch took that thread from 77% to 68% of a core: with MLAA off every flip is presented again (presents 20.99 -> 22.04, nothing skipped), and with MLAA on 20.28 -> 20.77 flips/s (2.14). Inside an EDGE job about a third of the time is the runtime's, spread over a dozen mechanisms of 1-2 us each (2.15): the dispatch cache now survives the position-independent pieces each job loads (misses 21 -> 6 a job), the job manager's interrupt handler takes the fast branch path, and the event acknowledge no longer takes the lock-line lock (4.4 -> 2.45 a job); 20.75 -> 20.98 flips/s. Still open from the [rendering accuracy plan](docs/notes/2026-10-28-rendering-accuracy-plan.md):
the rectangular seams (an RPCS3 reference), the menu's one-frame flash.
