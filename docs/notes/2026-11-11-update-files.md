# Stuck on the first loading screen: the base game's data under 1.12's EBOOT (issue #2, 2026-11-11)

## Symptom

A player's Kit alpha.3 (i5-9300H, GTX 1650) showed the warning and the Santa Monica screens, then stayed on the
loading screen until the window was closed. Four sessions, the same each time.

In each log, after `IntroLoaderThread` ends there is no `MapMainMemory(ea=0x75200000, size=0x100000)` and no
`voice thread`. Every run here has both, right before the brightness screen of a first boot.

## Hypotheses

The runs: four first boots, with no saves or game data, the language en-US, the settings at their defaults and a
copy of the shader cache. Each one went past the point where the player's stopped.

| run | what | result |
|---|---|---|
| A | the whole folder | the brightness screen at 55 s |
| B | `USRDIR/PATCHES` hidden, by a temporary probe in `cellFsOpen` and `cellFsStat` | the same |
| C | the Kit's build: the game's code in `ascension_game.dll`, 22,320 functions as in theirs | the same |
| D | A, with cross pressed (`PAD_SCRIPT`, `0x4000`) | past it: the first save made, the menu's scene drawn |

- **The Kit's module.** C ruled it out.
- **A first boot, the language, the default settings.** A ruled them out.
- **The PATCHES file alone.** B ruled it out.
- **The rest of their log.** These lines are in ours too:
  - `missed_code.txt`'s six SPU pieces;
  - the eight `REJECTED malformed transfer` lines;
  - the audio set-up.

  The order threads start in changes from run to run.

## Root cause

Their folder has 1.12's EBOOT and PARAM.SFO. Alpha.3 checked `APP_VER` and GOWA.elf's hash, and the threads' entry
points are ours. Its data is the base game's:

- **No `USRDIR/PATCHES`.** In 1.12 it holds a count, `5`, and the game opens `TPPS1.PSARC` to `TPPS5.PSARC` from it.
  Their logs never open those five.
- **Another `TPPS.PSARC`.** `[fs] read ... total=` is the session's bytes read so far, so the totals of two first
  reads give what was read between them.
  - Theirs: the game reads 48 bytes and then 64. It reads an archive's table length and 16 bytes more, as the
    reads of TPPS1 to TPPS5 in issue #3 show, so their table is 96 bytes.
  - 1.12's: 204,782,061 bytes, a 23,346-byte table, 375 files.
- **What only their logs fail to open.** The game opens a file as a loose one when no archive has it. Only their logs
  fail on these, and all four are in 1.12's `TPPS.PSARC`:
  - `SOUND/PS3_BE/ANNOUNCER.CSV`
  - `SOUND/PS3_BE/PATCHEDSTREAMS.TXT`
  - `WAD/PS3_BE/R_MULTIPLAYER.WAD.PATCH`
  - `WAD/PS3_BE/R_UI.WAD.PATCH`

In the game folder here, the updates' files were written last, after the base game's: every `TPPS*.PSARC`,
`PATCHES`, EBOOT.BIN, GOWA.SELF and PARAM.SFO.

The base game's `TPPS.PSARC` was not at hand. That 1.12's code waits on the base game's data is drawn from the above,
not seen.

## Fix

The check in the launcher (since `49d1ed6`) would have refused their folder. It did not say what to do, and it left
`PATCHES` out.

- **`game_files.inc`** has `USRDIR/PATCHES`, 51 files now. `gowa.py game-files` checks its count against
  `TPPS1.PSARC` to `TPPS5.PSARC`.
- **The launcher's line** says what to install:
  - every update up to 1.12, in order, when only the updates' files are missing or other (`TPPS*.PSARC`, `PATCHES`:
    `game_file_update`);
  - the game again otherwise.

  The line holds about a hundred characters.
- **The message** when the game stops on a missing file says the same. A missing `PATCHES` now stops the game there.
- **`gowa.py doctor`** says the same.

## The updates are not cumulative

The player was first told to install the 1.12 update again, and the first version of the fix said so too. They did,
and RPCS3 then showed 1.12, but the folder still had no `PATCHES` and no TPPS1 to TPPS5.

- **1.12's patch is 8644 KB**, by PlayStation Lifestyle's article on it.
- **The files written last here** are EBOOT.BIN, GOWA.SELF, ICON0.PNG, PARAM.SFO and PS3LOGO.DAT: 8,850,941 bytes,
  8,643.5 KiB.

So 1.12's PKG is its EBOOT alone. The archives and `PATCHES` come with the updates before it, each with files of its
own, and every one has to be installed, in order.

**Which update brings what.** The PS3's list for NPUA80918 has nine updates, 1.04 to 1.12: 3,145,826,976 bytes in all,
2.93 GiB. Their sizes are from the server's headers, and nothing was downloaded. "The EBOOT" is EBOOT.BIN, GOWA.SELF,
PARAM.SFO, ICON0.PNG and PS3LOGO.DAT, 8,850,941 bytes here. The files are matched by when the update was installed
here.

| update | package, bytes | its files here | the rest |
|---|---|---|---|
| 1.04 | 235,061,456 | `TPPS.PSARC`, the four `TPPS-*.PSARC`, `TLOUCOUNTDOWN.SELF`, `TROPDIR/NPWR00802_00/TROPHY.TRP` | the EBOOT + 1,013 |
| 1.05 | 8,851,344 | | the EBOOT + 403 |
| 1.06 | 653,674,688 | `TPPS1.PSARC` | the EBOOT + 661 |
| 1.07 | 8,851,472 | | the EBOOT + 531 |
| 1.08 | 654,462,928 | `TPPS2.PSARC` and the four `TPPS2-*` | 8,721,068 |
| 1.09 | 950,015,600 | `TPPS3.PSARC` and the four `TPPS3-*` | the EBOOT + 1,236 |
| 1.10 | 331,612,128 | `TPPS4.PSARC` and the four `TPPS4-*` | the EBOOT + 1,218 |
| 1.11 | 294,445,616 | `TPPS5.PSARC`; `PATCHES` too, going by its `5` | the EBOOT + 941 |
| 1.12 | 8,851,744 | | the EBOOT + 803 |

- **"The rest"** is a package's size less its files here.
- **What a package takes beyond its files** is about a kilobyte: its header and file table.
- **1.08's rest** is 130 KB smaller than the EBOOT. What it holds was not looked at, since the package's file table is
  encrypted.
- **Every package brings an EBOOT and a PARAM.SFO**, so the last one installed sets the version the folder has.

## Verification

- `tests/test_game_find.py`: 30 tests. The list has two of the updates' files now. The cases:
  - the updates' files missing;
  - one of them of another version;
  - one of the base game's missing as well.
- After installing 1.12 again: still no `PATCHES` and no TPPS1 to TPPS5 (the player's reply).
- After installing every update in order: not reported yet.

## Issue #3: the same TPPS.PSARC

Another Kit alpha.3 player (i9-12900K, RTX 3080) had a black screen with the loading icon. Only the main thread ran,
in the game's code. Their report holds three sessions.

- **The two short ones** quit by themselves. `HECA.PSARC`, then `PHI.PSARC`, could not be opened, and the game read
  NULL and exited. The player had put the folder together from more than one copy.
- **The long one** opens `PATCHES` and TPPS1 to TPPS5, and the five tables are 1.12's sizes (44,180, 38,964, 57,974,
  28,588 and 24,572 bytes). `TPPS.PSARC`'s is 96 bytes, as in issue #2.
  - As in #2, after `IntroLoaderThread` ends there is no `MapMainMemory(ea=0x75200000, size=0x100000)` and no
    `voice thread`.
- **The failed opens.** 9 of its 11 fail on a whole 1.12 folder as well (run A):
  - `INTRO/BOOT0_EN.GTF`, `BOOT3_EN.GTF` and `BOOT4_EN.GTF`;
  - `WAD/PS3_BE/R_FX.WAD.PATCH`;
  - `DC/PS3_BE/WAD_R_FX.DCB.PATCH` and `WAD_R_PERM.DCB.PATCH`;
  - `TPPS1-ENGLISH.PSARC`, `TPPS5-ENGLISH.PSARC` and `DCDS-ENGLISH.PSARC`.

  The other two, `SOUND/PS3_BE/ANNOUNCER.CSV` and `WAD/PS3_BE/R_MULTIPLAYER.WAD.PATCH`, are in 1.12's `TPPS.PSARC`.
- **The main thread's stack.** The player gave return addresses in their `ascension_game.dll`.
  - The three outer ones follow calls in this tree's Kit build too: `func_00010410`, `func_00010534` and
    `func_00010230`, which calls `func_00367D00`.
  - The inner ones do not, since this tree's DLL is laid out otherwise than alpha.3's (84.9 MB here, 81.0 MB
    theirs). They were not named.
- **The Kit's build on their PC:** 4:13, no error.

An update that brings `TPPS.PSARC` is missing from theirs, or the base game's copy went over it while the folder was
put together. Which of the two is not known.
