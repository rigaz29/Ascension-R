# Analysis of God of War: Ascension (NPUA80918)

First look, 2026-10-26, before anything was lifted. Numbers come from ps3recomp's
`tools/ppu_loader.py` and `tools/extract_spu_images.py` and from `tools/hle_coverage.py`.

## The release

| | |
|---|---|
| Title | God of War: Ascension (PSN, US) |
| Title id | NPUA80918, content id `UP9000-NPUA80918_00-GOWADIGITALVERS1` |
| APP_VER | 01.12 (patched), PS3_SYSTEM_VER 04.30 |
| Size | 36.9 GB, almost all of it PSARC archives |
| Also installed | `NPUA80918-DATA`, the game's data folder (empty at install) |

## Executables

All three are NPDRM SELFs with a local license (`.rap`, license type 2), so RPCS3 decrypts them
with the license in its `exdata` (Utilities > Decrypt PS3 Binaries).

| File | Size | Decrypted | Role |
|---|---|---|---|
| `USRDIR/EBOOT.BIN` | 74,160 | `EBOOT.elf`, 71,424 bytes | A launcher. Its only notable strings are `/dev_hdd0/` and `%s/GOWA.SELF`: it builds the path and starts the game. |
| `USRDIR/GOWA.SELF` | 8,719,872 | `GOWA.elf`, 8,717,136 bytes, sha256 `4bf75c89...183462b5` | The game. This is what the port lifts. |
| `USRDIR/TLOUCOUNTDOWN.SELF` | 17,380,720 | not needed | A separate program shipped with the title (The Last of Us countdown). |

The runtime's `exitspawn` is a no-op, so the port starts `GOWA.SELF` directly
(`PS3_BOOT_PATH=/dev_hdd0/game/NPUA80918/USRDIR/GOWA.SELF`). What arguments the launcher passes, if
any, is still to be read from `EBOOT.elf`.

## GOWA.elf

- Two load segments: `0x10000`, R-X, `0x80E468` bytes; `0x820000`, RW, `0x3C5F0` in the file and
  `0x247738` in memory.
- Entry OPD `0x8283F8`: code `0x10410`, TOC `0x83F2B0`. 7,643 OPDs (functions).
- 31 sections, so the executable ones bound the code: it ends at `0x669F8C`. The rest of the R-X
  segment is read-only data and the embedded SPU images; `gowa.py lift` passes the bound.
- About 8.4 MB of PPU code.

## Imports: 350 from 25 libraries

| Library | Functions | Registered in the runtime |
|---|---|---|
| sceNp | 49 | 30 |
| cellSpurs | 45 | 31 |
| sysPrxForUser | 30 | 24 |
| cellGcmSys | 29 | 28 |
| sys_fs | 26 | 23 |
| sys_net | 23 | 12 |
| cellSysutil | 19 | 18 |
| cellHttp | 18 | 11 |
| cellNetCtl | 16 | 10 |
| cellVoice | 15 | 11 |
| sceNpTrophy | 11 | 10 |
| sceNpTus | 10 | 3 |
| cellGame | 8 | 7 |
| libvdec | 8 | 8 |
| sys_io | 7 | 7 |
| cellSsl | 7 | 4 |
| cellAudio | 7 | 7 |
| cellSysmodule | 5 | 4 |
| sceNpUtil | 4 | 4 |
| cellSysutilNpEula | 3 | 0 |
| sceNpCommerce2, sceNp2, cellSysutilAvconfExt, cellHttpUtil, cellRtc | 2 each | 1, 2, 1, 1, 2 |

259 of the 350 are registered. An unregistered NID is reported once and answered with `CELL_OK`
(`PS3_HLE_UNRESOLVED=ok`), so the title keeps going on a faked result. The missing ones, by
weight for a single-player boot:

1. **cellSpurs, the second-generation task API**: `cellSpursInitializeWithAttribute2`,
   `cellSpursCreateTaskset2`, `cellSpursCreateTask2`, `cellSpursJoinTask2`,
   `cellSpursTryJoinTask2`, `cellSpursDestroyTaskset2`, `_cellSpursTaskAttribute2Initialize`,
   `_cellSpursTasksetAttribute2Initialize`, `cellSpursAttributeEnableSystemWorkload`, the workload
   flag pair (`cellSpursGetWorkloadFlag`, `_cellSpursWorkloadFlagReceiver`),
   `cellSpursLFQueueDetachLv2EventQueue` and `_cellSpursLFQueuePushBody`. The God of War HD port
   runs the lock-free queue's push as the firmware's own code (libsre, lifted); the same may be
   needed here.
2. **PRX loading**: `sys_prx_load_module`, `..._by_fd`, `..._on_memcontainer`,
   `..._on_memcontainer_by_fd`, `sys_prx_register_library`. `GOWA.elf` names no `.sprx` file, so
   these may only matter for system modules; to be seen at boot.
3. **Spawning**: `sys_game_process_exitspawn2`, `sceNpDrmProcessExitSpawn2` (the countdown
   program, or back to the launcher).
4. **Small ones**: `cellGamePatchCheck`, `_cellGcmFunc15`, `cellFsAioWrite` and two unnamed
   `sys_fs` NIDs, `cellSysmoduleSetMemcontainer`, `cellSaveDataFixedDelete`,
   `sceNpTrophyAbortHandle`.
5. **Network and PSN** (most of the rest: sceNp, sys_net, cellHttp, cellNetCtl, cellVoice, cellSsl,
   sceNpTus, cellSysutilNpEula, sceNpCommerce2): multiplayer, online passes and the store. The
   port is offline; these should report "not connected" rather than succeed on faked results.

## SPU programs

`extract_spu_images.py` finds four embedded SPU ELFs in `GOWA.elf`:

| File offset | Size |
|---|---|
| `0x6B3E00` | 56,484 |
| `0x6C1B00` | 39,508 |
| `0x6CB580` | 41,796 |
| `0x6D5900` | 38,100 |

None of them has section headers, and their read-only data (strings) sits in the executable
segment; the toolkit's SPU pipeline lifts them with reachable function detection (see
[notes/2026-10-26-first-boot.md](https://github.com/rigaz29/Ascension-R/blob/notes-2026-11-09/docs/notes/2026-10-26-first-boot.md)). What the first boots showed:

| Program | Size | Role |
|---|---|---|
| `00E729804474449A` (`0x6D5900`) | 38,100 | Edge Zlib, an SPU task fed through an ANY2ANY lock-free queue |
| `F4C2A58BDE92CE5E` (`0x6C1B00`) | 39,508 | five worker tasks of a Taskset2 (entry `0x6D1B00`) |
| `574361C36D1A4C05`, `4DA69B4166D997AD` | | not seen running yet |

The `Wws_Job` policy module is a raw image at `0x7A7B00` (12,288 bytes, FNV-1a
`0xC8D774963A96DCF2`), handed to `cellSpursAddWorkloadWithAttribute` for 13 workloads (CollisL,
RenJobSet x4, RenderJobs, MeshProcJobs, ModelServer, Network, General6, HighestPrio6, SysMisc,
BrBDblBufWrkld). Its job code is linked into `GOWA.elf` itself: 31 headers with the `0xC0DEC0DE`
magic, six of them job modules linked at LS `0x4000` (80 to 134 KB, at `0x7AAE00`, `0x7BFC80`,
`0x7E1C00`, `0x7FF280`, `0x818600`, `0x84BE40`). The policy module streams one from its address
into LS `0x4000` and runs it; `gowa.py lift` lifts them with `lift_jobmods.py --engine-elf`. The
other 25 headers name link base 0 and are not known yet.

## Data

The game reads its own PSARC archives (`/%s%s%s.PSARC`, `GAME.PSARC`, `RESOURCES.PSARC`, one per
level, per language and for multiplayer); the runtime only provides file access. Other paths in the
executable: `/CINEMATICS/`, `/SOUND/PS3_BE/%s.sbp`, `/INTRO/BOOT%d.GTF`, `/DC/PS3_BE/%s.dcb`,
`/MOTD/`, `/PROFILES`, `/PATCHES`, `/DCDS/` (downloadable content and online data).

## Compared with God of War HD (NPUA80490)

| | God of War HD | God of War: Ascension |
|---|---|---|
| PPU code | 5.0 MB | 8.4 MB |
| Imports | 153 from 13 libraries | 350 from 25 libraries |
| Embedded SPU ELFs | 4 | 4 (more SPU code to be found) |
| Executable | `EBOOT.BIN` | `GOWA.SELF`, started by a launcher |
| Data | files and one PSARC per game | dozens of PSARC archives, 36.9 GB |

Ascension is a 2013 engine built for the PS3's SPUs from the start, with a multiplayer mode, so
expect much more SPU and SPURS work than in God of War HD.
