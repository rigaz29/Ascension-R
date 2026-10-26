# Plan: God of War: Ascension (NPUA80918) on ps3recomp

A static recompilation of God of War: Ascension (PS3, PSN US release, version 1.12) to native
Windows, on [ps3recomp](https://github.com/sp00nznet/ps3recomp). It follows the
[God of War HD port](https://github.com/rigaz29/gow1hd-recomp): same toolkit fork, same pipeline,
same rules. Progress is in [PROGRESS.md](PROGRESS.md); what the title is made of is in
[docs/ANALYSIS.md](docs/ANALYSIS.md).

## 1. What is ported

The title's `EBOOT.BIN` is a launcher that starts `USRDIR/GOWA.SELF`. The port lifts `GOWA.SELF`
(decrypted to `GOWA.elf`) and starts it directly. The game data stays in the installed game folder
and is only read.

## 2. Requirements

- Windows 10/11 x64, 16 GB RAM or more, about 30 GB free for the lift and the build.
- Visual Studio Build Tools (C++), clang-cl (Visual Studio's, or a portable LLVM in `../llvm`),
  CMake, Ninja, Git, Python 3.10+ with `ps3recomp/requirements.txt`.
- A shared FFmpeg build in `../ffmpeg` for movies and compressed audio.
- Your own copy of the game, installed in RPCS3, with its license: RPCS3 decrypts `GOWA.SELF` to
  `GOWA.elf` (Utilities > Decrypt PS3 Binaries).

## 3. Repository rules

1. **No file from the game ever goes into git**: no SELF or ELF (encrypted or decrypted), no
   lifted code (`recompiled/`, `spu/`), no SPU images, archives, videos, saves or license files.
   `.gitignore` covers them. Never `git add -f`.
2. What is fine to commit: our own code, scripts, docs, and metadata such as function addresses,
   NIDs and SHA-256 hashes.
3. Built executables are not shared, since they contain the game's code. Distribution would be a
   kit: users bring their own game and build the port on their machine.
4. GPL code (RPCS3 and others) is read, never copied, so this repo stays MIT. MIT code may be
   copied with a notice in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
5. No keys, firmware or decryption tools in the repo. Firmware files a port needs are decrypted
   and lifted locally into `firmware/`, which is git-ignored.

## 4. Folder layout

```
C:\Users\rigaz\recomp\
├── .venv\                     Python + ps3recomp requirements
└── gowa-recomp\               this repo
    ├── ps3recomp\             submodule -> rigaz29/ps3recomp, branch gowa
    ├── scripts\gowa.py        doctor / setup / analyze / lift / lle / build / run / symbolize
    ├── tools\hle_coverage.py  firmware imports the runtime doesn't implement
    ├── CMakeLists.txt         the port's build, defaults to the submodule
    ├── main.cpp               entry point, frame clock, the title's environment
    ├── launcher.cpp           the settings window shown before the game
    ├── stubs.cpp  gowa_lle.h  game-specific hooks; the lifted libsre's queue functions
    ├── docs\                  ANALYSIS.md, notes\
    ├── PLAN.md  PROGRESS.md
    │
    │   -- git-ignored (made from your game) --
    ├── vfs\PS3_GAME\          junction to the installed game (read only)
    ├── gamedata\dev_hdd0\     the port's /dev_hdd0: saves, trophies, caches
    ├── analysis\              loader output, SPU images, coverage report
    ├── recompiled\            lifted PPU code
    ├── spu\wwspm\             the lifted Wws_Job policy module
    ├── firmware\              libsre.prx (yours, decrypted) and its lift
    ├── build\                 CMake/Ninja output, gowa.ini
    └── logs\                  one log per run
```

## 5. Phases

| Phase | Goal | Done when |
|---|---|---|
| 0 Setup | Repo, submodule, script, decrypted executables | `gowa.py doctor` is all good |
| 1 Analysis | Loader output, imports, HLE coverage, SPU images | [docs/ANALYSIS.md](docs/ANALYSIS.md) |
| 2 First build | `GOWA.elf` lifted and linked | `GowARecomp.exe` links |
| 3 First boot | The entry runs; the title reaches its first screen | a splash or the first loading screen on screen |
| 4 Missing HLE | SPURS task API 2, PRX loading, spawning, the network answering "offline" | no faked result on the boot path |
| 5 SPU | The embedded images and the raw SPU code lifted and dispatched | SPURS work runs lifted, not interpreted |
| 6 Graphics | The menus and the first level drawn right | compared with RPCS3 |
| 7 Video and audio | Cinematics and sound | the intro plays with sound |
| 8 Input, saves, trophies | Pad and keyboard, saves, trophies | a save loads |
| 9 Start to finish | The campaign plays through | |
| 10 Performance | 30 fps (the console's target), then more | |
| 11 PC features | Settings, window modes, scaling (carried over from God of War HD) | |
| 12 Kit and release | Users build the port from their own copy | |
| 13 Upstream | General fixes go to ps3recomp as PRs | ongoing |

Phases 0 to 2 are done and phase 3 is under way: the boot stops on the first missing piece, and
each stop is one session.

## 6. Session workflow

1. `git pull`, `git submodule update`, then `python scripts/gowa.py doctor`.
2. Read the last note in `docs/notes/` and "Next" in `PROGRESS.md`.
3. Pick one wall: a crash, a hang, a wrong image.
4. Reproduce, form a hypothesis, measure, fix, verify. No fixing on a guess.
5. Small commits: one fix per commit, with a message that explains why.
6. Write the session note `docs/notes/YYYY-MM-DD-topic.md`: symptom, hypotheses (including the ones
   ruled out), root cause, fix, evidence.
7. Update `PROGRESS.md` on a milestone.

## 7. Debugging approach

1. **Logs first.** The runtime prints each unresolved NID once. Diagnostic variables are listed in
   `ps3recomp/docs/DIAGNOSTICS.md`.
2. **RPCS3 as the reference.** Run the same game in RPCS3 with log level *All* and compare the
   order of HLE calls, their arguments and return values.
3. **Lifter bug or runtime bug?** When one function misbehaves, compare its disassembly with the
   lifted C. Lifter bugs are fixed in the lifter, then relift.
4. **Check the God of War HD port's notes** for the same symptom: the runtime is the same, and many
   walls (SPURS queues, the FIFO, saves) were met there first.
5. **Check the other ports of this title**: [God-of-War-Ascension-Recompiled](https://github.com/MaChInEgUn3/God-of-War-Ascension-Recompiled)
   and [gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc) target disc releases on
   ps3recomp and are further along; their notes describe the same engine's walls. Read, and credit
   what is taken from them (both MIT).

## 8. Git workflow

```
sp00nznet/ps3recomp (upstream)
        │  fork
        ▼
rigaz29/ps3recomp
  ├── gow1hd         the God of War HD port's runtime
  ├── gowa           gow1hd + this port's fixes     <- this repo's submodule points here
  └── pr/<topic>     one general fix, branched from upstream -> PR upstream

rigaz29/gowa-recomp (main)
  └── ps3recomp @ a specific commit on gowa
```

- Runtime fix: commit on `gowa` (inside `ps3recomp/`), push to the fork, then commit the new
  submodule pointer here. A fix both ports need is cherry-picked to `gow1hd` too.
- Upstreaming: branch from upstream, cherry-pick, push, open a PR, as in the God of War HD port.

## 9. Risks

- **SPU work.** Most of the frame on the console runs on SPUs. Code the runtime has to interpret
  instead of running lifted will be slow; raw images have to be found and lifted one by one.
- **SPURS task API 2** is not in the runtime yet and is on the boot path of an engine like this.
- **Size.** About 1.7 times the God of War HD code, with longer lifts and builds.
- **Online parts** must fail cleanly as "offline" without the title waiting on them.
