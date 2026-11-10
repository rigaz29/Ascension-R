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
  - Theirs: the game reads the archive's header (48 bytes) and 64 more, a 112-byte table.
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
