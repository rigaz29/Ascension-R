# Progress

Current status and milestones. The plan is in [PLAN.md](PLAN.md).

## Status

| Phase | Status |
|---|---|
| 0 Setup | done: repo, submodule (`rigaz29/ps3recomp`, branch `gowa`), `scripts/gowa.py`, decrypted executables |
| 1 Analysis | done: see [docs/ANALYSIS.md](docs/ANALYSIS.md) |
| 2 First build | done: `GowARecomp.exe` links, with the 4 SPU programs, the `Wws_Job` policy module and the lifted `libsre` |
| 3 First boot | in progress: the intro loader gets through its boot screens and the title loads its language archives; no frame flipped yet |
| 4 Missing HLE | boot path done: SPURS2, Taskset2/Task2, workload flag, `cellGamePatchCheck` |
| 5 SPU | started: the 4 embedded SPU programs, the `Wws_Job` policy module and its 6 engine-embedded job modules run lifted |
| 6-13 | not started |

## Milestones

| Date | Milestone |
|---|---|
| 2026-10-26 | `EBOOT.BIN` and `GOWA.SELF` decrypted (RPCS3). `EBOOT.BIN` is a launcher that starts `GOWA.SELF`, which is what the port lifts. `GOWA.elf`: 7,643 functions, about 8.4 MB of PPU code, 350 imports from 25 libraries (259 registered in the runtime), 4 embedded SPU ELFs. The port's files are carried over from the God of War HD port (script, build, runner, settings window). |
| 2026-10-26 | First build (22,320 PPU functions, 188 MB of C++) and four boots ([notes](docs/notes/2026-10-26-first-boot.md)). Runtime: the SPURS2 entry points, `cellGamePatchCheck`, and reachable SPU function detection for programs that keep data in their code segment. Port: the Edge Zlib queue through the lifted `libsre`, and the `Wws_Job` policy module lift, both from the God of War HD port. No import on the boot path is unregistered. |
| 2026-10-27 | Runtime branch merged with upstream (380 commits). The `Wws_Job` job modules linked into `GOWA.elf` are lifted (upstream `lift_jobmods --engine-elf`, with per-module extra entries), so jobs run lifted instead of interpreted. The GCM offset tables the title reads directly now follow every map and unmap: the intro loader's `put` was -1. ([notes](docs/notes/2026-10-27-jobs-and-intro-loader.md)) |
| 2026-10-27 | SPURS2 with 32 workloads (EDGE2 and RsxSync were refused), the MLAA tasks no longer signalled at creation, Taskset2 event flags, the RSX waiting on its post-process semaphore. Two other ports of this title studied for the frame pipeline ([notes](docs/notes/2026-10-27-frame-pipeline.md)). |

## Next

The ordered plan is in [docs/ROADMAP.md](docs/ROADMAP.md). In short:

0. Tooling: a pipeline probe (`GOWA_PIPE`), a per-workload policy trace, `GCM_FIFO_CIRCLE_PASSES=0`,
   a relift with the merged lifter.
1. RsxSync's policy module takes no entry of its four-list job list; no frame is flipped yet.
2. The MLAA batch round trip, then the first flips.
