# Progress

Current status and milestones. The plan is in [PLAN.md](PLAN.md).

## Status

| Phase | Status |
|---|---|
| 0 Setup | done: repo, submodule (`rigaz29/ps3recomp`, branch `gowa`), `scripts/gowa.py`, decrypted executables |
| 1 Analysis | done: see [docs/ANALYSIS.md](docs/ANALYSIS.md) |
| 2 First build | next |
| 3-13 | not started |

## Milestones

| Date | Milestone |
|---|---|
| 2026-10-26 | `EBOOT.BIN` and `GOWA.SELF` decrypted (RPCS3). `EBOOT.BIN` is a launcher that starts `GOWA.SELF`, which is what the port lifts. `GOWA.elf`: 7,643 functions, about 8.4 MB of PPU code, 350 imports from 25 libraries (259 registered in the runtime), 4 embedded SPU ELFs. The port's files are carried over from the God of War HD port (script, build, runner, settings window). |

## Next

1. Phase 2: `gowa.py setup`, `analyze`, `lift`, `build`.
2. Phase 3: the first boot, and from there whatever it stops on. The SPURS task API 2 functions
   (docs/ANALYSIS.md) are the first likely wall.
3. Read the arguments the launcher (`EBOOT.elf`) passes to `GOWA.SELF`.
