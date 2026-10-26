# Progress

Current status and milestones. The plan is in [PLAN.md](PLAN.md).

## Status

| Phase | Status |
|---|---|
| 0 Setup | done: repo, submodule (`rigaz29/ps3recomp`, branch `gowa`), `scripts/gowa.py`, decrypted executables |
| 1 Analysis | done: see [docs/ANALYSIS.md](docs/ANALYSIS.md) |
| 2 First build | done: `GowARecomp.exe` links, with the 4 SPU programs, the `Wws_Job` policy module and the lifted `libsre` |
| 3 First boot | in progress: the title reads its archives and starts its intro loader, which then waits; no picture yet |
| 4 Missing HLE | boot path done: SPURS2, Taskset2/Task2, workload flag, `cellGamePatchCheck` |
| 5-13 | not started |

## Milestones

| Date | Milestone |
|---|---|
| 2026-10-26 | `EBOOT.BIN` and `GOWA.SELF` decrypted (RPCS3). `EBOOT.BIN` is a launcher that starts `GOWA.SELF`, which is what the port lifts. `GOWA.elf`: 7,643 functions, about 8.4 MB of PPU code, 350 imports from 25 libraries (259 registered in the runtime), 4 embedded SPU ELFs. The port's files are carried over from the God of War HD port (script, build, runner, settings window). |
| 2026-10-26 | First build (22,320 PPU functions, 188 MB of C++) and four boots ([notes](docs/notes/2026-10-26-first-boot.md)). Runtime: the SPURS2 entry points, `cellGamePatchCheck`, and reachable SPU function detection for programs that keep data in their code segment. Port: the Edge Zlib queue through the lifted `libsre`, and the `Wws_Job` policy module lift, both from the God of War HD port. No import on the boot path is unregistered. |

## Next

1. The `Wws_Job` job code: where it comes from, and lifting it (it runs in the SPU interpreter now).
2. What `IntroLoaderThread` polls in `0x2C2DA8`; the main thread waits for it.
3. The Taskset2 workers' PUT to EA 0.
4. Bring the `gowa` runtime branch up to date with upstream (380 commits, some for this title).
