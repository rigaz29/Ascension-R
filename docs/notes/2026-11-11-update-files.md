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

In the game folder here, update 1.12's files were written last: every `TPPS*.PSARC`, `PATCHES`, EBOOT.BIN, GOWA.SELF
and PARAM.SFO.

The base game's `TPPS.PSARC` was not at hand. That 1.12's code waits on the base game's data is drawn from the above,
not seen.

## Fix

The check in the launcher (since `49d1ed6`) would have refused their folder. It did not say what to do, and it left
`PATCHES` out.

- **`game_files.inc`** has `USRDIR/PATCHES`, 51 files now. `gowa.py game-files` checks its count against
  `TPPS1.PSARC` to `TPPS5.PSARC`.
- **The launcher's line** says what to install again:
  - update 1.12, in RPCS3, when only its files are missing or other (`TPPS*.PSARC`, `PATCHES`: `game_file_update`);
  - the game otherwise.

  The line holds about a hundred characters.
- **The message** when the game stops on a missing file says the same. A missing `PATCHES` now stops the game there.
- **`gowa.py doctor`** says the same.

The player was told on the issue to install the 1.12 update again.

## Verification

- `tests/test_game_find.py`: 30 tests. The list has two of the update's files now. The cases:
  - the update's files missing;
  - one of them of another version;
  - one of the base game's missing as well.
- The player's folder after installing the update again: not reported yet.

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

In the game folder here, the 1.12 `TPPS.PSARC` was written with the update's other files, within two minutes and with
no gap. Where the two players' base `TPPS.PSARC` came from is not known.
