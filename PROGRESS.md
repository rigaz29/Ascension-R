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

## Next

Speed, toward RPCS3's on this machine or better (first level at 12.3 flips/s; RPCS3 22.9 FPS in
the same fight): the [performance plan](docs/notes/2026-10-29-performance-plan.md). Phase 0 is
done (`gowa.py bench`, `gowa.py frames`, SPU job capture and `gowa.py replay`, RPCS3 measured at
the same scene); next phase 1 (approximate SPU float judged on the replay, the SPU runtime's
share, the main thread's polls, no full GPU drain per batch, descriptors, MLAA on the GPU). Still open from the [rendering accuracy plan](docs/notes/2026-10-28-rendering-accuracy-plan.md):
the rectangular seams (an RPCS3 reference), the menu's one-frame flash.
