# The game's files, checked before anything is decrypted or built (2026-11-10)

The user tried the Kit with a folder that held PARAM.SFO, USRDIR\GOWA.SELF and the license (.rap) alone. The launcher
decrypted GOWA.SELF and built the game's code from it. `game_probe` looked at PARAM.SFO's title and version, at
GOWA.SELF, and after the decrypting at GOWA.elf's SHA-256: at nothing of the game's data.

## What the game does without its files

Run from a folder with PARAM.SFO, GOWA.SELF and GOWA.elf alone (`--no-launcher`, with saves and a shader cache of its
own): the game opens `USRDIR/ARCHIVES`, which fails, reads through a NULL pointer, and calls `sys_process_exit(0)` about
10 s after the start. A black window, then nothing, and nothing said. The session ended "normally", so the launcher did
not offer a report either. For the player that came after a build of ten minutes.

## What the game opens

The RPCS3 install here: 121 files, about 40 GB. USRDIR holds 48 archives (.PSARC) at its top, 39.5 GB, and four small
ones in `USRDIR/build/main` that no recorded session opened.

At its start the game reads `USRDIR/ARCHIVES` (19 archives, MULTIPLAYER.PSARC among them) and
`USRDIR/LOCALIZED_ARCHIVES` (8: four languages, single player and multiplayer). It opens all the first list's archives,
and the second list's for its language. Then it looks for the updates' archives by name, TPPS to TPPS5 and their
localized ones. Some of those are not in 1.12 (TPPS1-ENGLISH, TPPS5-ENGLISH, DCDS-ENGLISH): the open fails and the game
goes on. So the game's own lists are not what to check against. The list is taken from a whole copy.

## The check

- **The list.** `port/game_files.inc` comes from `gowa.py game-files --game-dir <a whole copy of 1.12>`: the 48
  archives and the two lists, 50 files, in the order the game opens them (GAME.PSARC first). Each has its size and the
  SHA-256 of its first and last 64 KiB. It holds names, sizes and hashes only, as `k_elf_sha256` does. UnleashedRecomp's
  installer checks a player's copy the same way (`install/hashes/game.cpp`, made by its `fshasher`).
- **`game_probe`** (game_find.cpp) looks, in this order:
  1. PARAM.SFO's title.
  2. GOWA.elf or GOWA.SELF. A folder with neither is no game, as RPCS3's NPUA80918-DATA is not.
  3. The version. Another one is GAME_OTHER_BUILD with `other_version`, before its files, which are another version's
     too.
  4. The files. Every size first, and the ends' hashes only when every file is there at its size. So a folder that
     lacks some is told so without a byte of it read.

  A folder that fails is GAME_INCOMPLETE, between GAME_OTHER_TITLE and GAME_NO_ELF. Auto detect counts it as a copy,
  keeps looking past it, and takes a whole copy over it.
- **The launcher.** GAME_INCOMPLETE turns off `elf_ok`, which Build, Play and Precompile shaders ask for, whatever was
  decrypted before.
  - The System tab says what the folder lacks: "Not the whole game: 48 of its 50 files are not in its folder (39.5 GB),
    GAME.PSARC first", or "2 of the game's files are not as version 1.12 has them, RESOURCES.PSARC first".
  - The ready check says "Game files missing (System tab)" or "Wrong game files (System tab)".
  - Browse or Auto detect sets such a folder, so that line shows.
  - A copy of another version now says so: "That is NPUA80918 1.00. This port runs version 1.12: install the game's
    update first". It is not decrypted.
- **The decrypting.** `game_prepare` checks too, so `Ascension-R --prepare-game` does not decrypt for such a folder.
- **The rest.**
  - The problem report has a "Game files:" line.
  - `gowa.py doctor` checks the files of `vfs/PS3_GAME`.

## When the game opens one anyway

`cellFsOpen` in the fork calls `g_ps3_open_failed_hook` after its `[fs] open FAIL` line, and main.cpp sets it. A file of
the list under `/dev_hdd0/game/NPUA80918/` that cannot be opened ends the session:

- the log gets `[quit] the game's folder has no USRDIR/ARCHIVES, or it could not be read: <host path>`;
- a message box says the same, unless a script started the game (`--no-launcher`);
- the game exits with code 3, by TerminateProcess, as a closed window ends it.

This covers a script, the launcher turned off, a file deleted or locked after the launcher looked, and the second link
(hdd0) leading somewhere else. The archives 1.12 does not have, which the game looks for, are not in the list and go by
as before.

## Measured

- `gowa.py game-files` on the RPCS3 install: 3.3 s, with Python's start.
- `game_probe` on the whole copy, the disk's cache warm: 12.6 to 14.2 ms. That includes GOWA.elf's hash, 8.7 MB, as
  before.
- `game_probe` on PARAM.SFO, GOWA.SELF and GOWA.elf alone: 1 ms, none of the game's files read.
- The game from those three files (`--no-launcher`): `[quit] the game's folder has no USRDIR/ARCHIVES, or it could not
  be read: ...` and exit code 3, 2.9 s after the start. Before, it stopped by itself after about 10 s.
- The game from the whole copy for 45 s, with saves of its own: no `[quit]`, and 29 archives opened. The opens that
  failed were, as before:
  - the archives 1.12 does not have (TPPS1-ENGLISH, TPPS5-ENGLISH, DCDS-ENGLISH);
  - `.PATCH` files;
  - regional boot images.

## Tests

`tests/test_game_find.py`, 26 tests, 11 of them new. The driver is built with a list of the test's own, three made-up
files whose ends are worked out by gowa.py's `ends_sha256`, so the Python and C hashes are checked against each other.
The new tests cover:

- the three files alone, with or without a GOWA.elf;
- one file missing, one cut short, one of its size with other bytes;
- one with its last byte changed;
- a whole copy not decrypted yet;
- another version without its files;
- the search taking a whole copy over one walked before it, which ended the search before;
- the files the game cannot start without;
- the list read back by gowa.py.

## Not covered

- **A file damaged between its ends.** Only 128 KiB of each are read. A check of all 39.5 GB (a Verify button) is not
  done yet.
- **A folder made to pass.** Names, sizes and ends can be copied from a real copy. This is open source, and a check in
  it can be taken out: it is there to stop a build that cannot be played, to say what is missing, and to make the whole
  game what the program asks for.
- **Another 1.12, if there is one.** Its files would show as not 1.12's, and the player would be told which.
