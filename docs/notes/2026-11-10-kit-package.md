# The Kit's package (2026-11-10)

The fourth step of the Kit release, after the DLL split, the toolchain and the setup (`2026-11-10-game-module.md`,
`2026-11-10-kit-toolchain.md`, `2026-11-10-kit-setup.md`):

    py scripts/gowa.py package kit [--test]

## What it does

1. Builds the Kit's exe in `build-kit/` with `PORT_GAME_MODULE=ON`. The build's cache must hold the release settings,
   and `PORT_GAME_MODULE` must be on for the Kit and off for the Prebuilt.
2. Puts beside the exe the DLLs the Prebuilt has, the licenses plus `Python.txt`, `LLVM.txt`, `LLVM-clang.txt` and
   `LLVM-lld.txt`, and `README.txt` and `THIRD-PARTY.txt` with the Kit's own lines (`KIT_TEXT`: what it needs, THE
   GAME'S CODE, the update, IN THE KIT FOLDER). The Prebuilt's two texts come out byte for byte as before. Then
   `kit\` (`kit-tree`).
3. `release_problems`, now over every file: every program and library finds what it imports beside it or in
   System32, and no file but the licenses holds a path of this PC, the Windows user name, or the git name or e-mail.
4. `package_audit` looks at every file for the game:
   - by name, type and folder (an ELF, SELF, PKG, PSARC, EDAT, RAP, EBOOT.BIN, PARAM.SFO, `ascension_game.dll`, a
     map, an object; `game-code`, `recompiled`, `spu`, `analysis`, `work`), and by its first bytes;
   - lifted code: more than 2,000 lines with `ctx->gpr[` (the hand-written routines have at most 310);
   - PS3 keys (`check_repo`) in every file this port makes;
   - runs of `GOWA.elf`: its windows of 64 bytes at every 32nd offset, those with more than 8 different bytes, looked
     for at every offset of every file, and each one found grown both ways, so no run of 96 bytes or more is missed.
     A file this port makes may hold a run of less than 256 bytes and less than 1 KB in all. The makers' binaries
     (the Python, LLVM, the DLLs beside the exe) may hold less than 16 KB and 64 KB: they carry the tables of the
     libraries the game uses too.
5. `kit_build_check`: the Kit as packaged builds the DLL, as the launcher has it do, from this tree's `GOWA.elf` with
   its own Python, into `logs/kit-check`. `kit\` must be unchanged afterwards. The DLL's map goes beside the zip:
   the DLL is the same on every PC, so that map reads the Kit's crash stacks.
6. The zip, the maps, the FFmpeg source, `build-info.txt` (with the Kit's id, the Python zip's SHA-256, LLVM's
   version, commit and programs' SHA-256, and the checked DLL's SHA-256) and `audit.txt`, which says what was looked
   for and what was found.

## Calibrating the runs of GOWA.elf

| File | Bytes of GOWA.elf | Longest run |
|---|---|---|
| The Kit's `Ascension-R.exe` (3.3 MB) | 65 in 1 run | 65, a table of the C runtime |
| The Kit's own 71 files | 0 | |
| The Prebuilt's `Ascension-R.exe` (85 MB) | 1,098,877 in 94 runs | 115,884 |
| `clang-cl.exe`, `lld-link.exe` | 1,487 and 1,256 | 1,028: zlib's CRC-32 table as a big-endian machine has it |

The first package stopped on LLVM's two programs: the game and LLVM both link zlib, whose table for big-endian CPUs is
the game's own. The other runs there are the C library's `0123456789abcdef…` and its month and day names. So the makers'
binaries got their own limits, and `audit.txt` names the longest run in them.

## Four things the first packages showed

- Started from PowerShell, the build's configure lost clang-cl: `msvc_env.environ()` copies the environment and adds
  what vcvars' `set` prints, by name, and cmd spells PATH `Path` when the shell that started it does. The environment
  then held both, and LLVM went into the one CMake did not read. `toolchain_env` now keeps one name each, upper case
  as Python has them, the later (vcvars') value. Git Bash spells it `PATH`, which is why every build until now worked.
  That failed configure had also made CMake take the compiler for another one and start `build-kit` over: it dropped
  the objects, and with LLVM off its path it took mingw's `ar` and `ld` for the archiver and the linker. `build-kit`
  was configured again from no cache and built whole.

- The tools' own Pythons write `ps3recomp\tools\__pycache__` into the Kit when they run: the Kit's Python runs with
  `-B`, but the ones it starts do not, and the `._pth` makes them ignore `PYTHONDONTWRITEBYTECODE`. On a player's PC
  that is harmless (and skipped where the folder is read-only). The Kit's id leaves `__pycache__` out, and the package
  removes what its build check wrote before zipping.
- Two DLLs built from the same sources differed in two bytes: the time stamp in the PE header. The link now has
  `/Brepro`, which makes the stamps a hash of the file; two links are byte for byte the same, and a player's DLL
  should be the file whose SHA-256 is in `build-info.txt`. `/Brepro` also adds a small debug directory to `.rdata`, so
  this DLL is laid out a little differently from the ones before it.
- `package --test` empties `dist/test/<release>` first; a shell standing in it kept a folder there. It now stops
  when the folder is not gone, rather than zip what is left.

## What the zip showed as a new player has it

The test zip unpacked into a new folder with `portable.txt`, and started with no settings: the launcher looks for the
game by itself. It took `D:\...\dev_hdd0\game\NPUA80918-DATA` for the game and said "Not decrypted yet", so there was
no Build to press. RPCS3 makes that folder beside the game for its data: a PARAM.SFO with the same title, an icon and a
small USRDIR, and no GOWA.SELF. The search stops at the first copy that is this release (`GAME_NO_ELF` is good enough,
`find_thread`), the folders found are walked last first, and `game_probe` called any folder with the title and no
GOWA.elf "not decrypted yet". It now asks for GOWA.SELF too; without either file the folder is no game, and the search
goes on to the game beside it. This was in the Prebuilt as well: every player with the game in RPCS3 has that folder.

## Results (Ryzen 5 4600H, 16 GB)

- `package kit --test`: a zip of 143.5 MB, 461 files and 325.8 MB unpacked, in 11 minutes, 20 with `build-kit` built
  whole.
- `audit.txt`: 65 bytes of `GOWA.elf` in the files this port makes, one run in the exe (a table of the C runtime);
  3,443 bytes in 14 runs in the makers' binaries, the longest zlib's CRC-32 table (1,028 bytes); at most 310 lines with
  `ctx->gpr[` in a file; no PS3 key in the 88 files this port makes; no trace of this PC; every import found.
- The zip as a new player has it: unpacked into a new folder with `portable.txt`, the launcher found the game in RPCS3
  by itself, Build built the game's code from the zip's `kit\` in 7:46, and Play went on to the game's brightness
  screen at 60 fps with no crash. The DLL it built is byte for byte the one the package's check built: SHA-256
  `ed41a3de87f08d4d37733ddb6f98c26658c15501dc6dd57c095f323c0e6dd27e`.
- The Prebuilt's files pass the stricter `release_problems` (looked at in a scratch folder, with no zip made).
- The lift's line on the Play tab no longer counts functions and files: with the time left it was cut short.

## Not done

- Decrypting and then building, on a PC whose game is not decrypted yet: this PC's RPCS3 holds a `GOWA.elf` beside
  the game's `GOWA.SELF`, so the launcher finds the game ready and the decrypting does not run here. Testers will.
- A Windows with no Visual Studio, Python or LLVM: Windows Sandbox, once it is enabled on this PC.

## The first release

0.1.0-alpha.3 is the Kit's first real package (`dist/0.1.0-alpha.3/kit`), with controllers through SDL3
(`2026-11-10-controllers.md`): a zip of 144.6 MB, 463 files and 328.6 MB unpacked, SHA-256
`544796a08b7d11802527e484785cf5b02cd3b136f8d98a502e08828947d84eb1`. Its audit found what the test packages had, and
its build check came out with the same DLL. Unpacked as a new player has it, it found the game, built the DLL in 7:40
(the same SHA-256 again) and played. On GitHub it went up as a draft pre-release with `audit.txt` and FFmpeg's
source, and was published the same day as `v0.1.0-alpha.3` (the tag at the version's commit, `9add610`), before a
controller had been tried on it. Its notes have the install steps, EDGE on the GPU, the speed against RPCS3 and a
status table. README.md points at the releases page now.

## The package 0.1.0-alpha.4 (2026-11-11), not released

- **The first package stopped at the DLL's link:** `calloc` and `atol` undefined, from `native/spu_edge_memo.c` since
  the unskinned jobs' transforms went to the vertex pass. One exe takes them from the C runtime, so no build of this
  tree had seen it. Both went into `GAME_MODULE_IMPORTS`, `GAME_MODULE_ABI` 2 (`bf591d4`), and
  `tests/test_game_module_crt.py` now reads `native/` for C runtime calls the table lacks, in CI. With the table before,
  it names those two.
- **The package:** a zip of 144.6 MB, 464 files and 328.7 MB unpacked, SHA-256
  `fbf14dcc69b326344cf7fd3743768c081c6d63bbda62c8f0e5dd13830b9c44a9`. Its audit found what alpha.3's had. Its build
  check made the DLL in 479 s: SHA-256 `44ee25b2c2f426dfb4844c0c3a1680e34817e2486ba9a7ea894cdd3f30087525`.
- **The zip on another drive** (issues #1 and #6):
  - unpacked on D:, with `LOCALAPPDATA` pointed at a folder on C: so the data folder was there and this PC's own was
    left alone;
  - `kit-build` run as the launcher runs it, with the Kit's Python and no `GOWA_*` or `PYTHON*`, built the DLL in
    503 s, byte for byte the build check's. Its three lift messages named the folders on C: in full, where alpha.3
    stopped;
  - the Kit's exe, from D:, with that data folder and default settings, loaded the DLL (22,320 lifted functions) and
    played the first fight at 34.3 fps, the sound clean.
- **The bench stopped on the D: exe** too, as it named the exe relative to the tree (`e52b4a6`).
- **Not done:** the launcher's window, a fresh decrypt, a controller.

## The package 0.1.0-alpha.5 (2026-11-11)

- **Why not alpha.4:** alpha.4 was packaged from `bf591d4` and went into a draft that was never published. The fix of
  frame generation's freeze at the start (`e91872b`, and `0420b4d6` in the fork) came after it, and a version is
  packaged once. alpha.4's maps stay in `dist/0.1.0-alpha.4/`, for the Prebuilt made from it.
- **The review before it**, alpha.3 to `e91872b` and the fork's five commits, changed two things (`1c96a31`):
  - the launcher offered frame generation off after any freeze with it on, in place of the report, so a freeze of the
    game's own with it on (issue #7) would never have been reported. It now needs the freeze watch's line that the
    GPU stopped behind frame generation's swap chain, which every frozen start of the runs had;
  - a game that cannot start for a file of its folder ended itself, the way that set off the freeze; another process
    ends it now, as when the window closes.
- **The package:** a zip of 144.6 MB, 464 files and 328.7 MB unpacked, SHA-256
  `8dd27aa98c08ed6dc9218bd21002a6af9d93f5eafbe3b7ea6b6d8470a0b2a77b`. Its audit found what alpha.4's had. Its build
  check made the DLL in 470 s, the same file as alpha.4's (`44ee25b2c2f426dfb4844c0c3a1680e34817e2486ba9a7ea894cdd3f30087525`):
  nothing it is built from changed since.
- **Tried on it:** the tree's own build, from the launcher with frame generation on, closed by its window after 40 s:
  no stuck fence, no freeze, 1493 flips; the five SPU pieces it ran interpreted are the ones every start has had. The
  Kit's exe, the zip's (SHA-256 `3a3181d1991b6a1791c0a071c13ecad826a1f33a51ac71ef4edc0c91d87851c4`), with
  `--end-process` on a ping started for it: it ended the ping with the code it was given, and gave 1 for a process
  that is not there.
- **Not done:** the zip unpacked and played as a player has it. Its DLL is the one alpha.4's run of that played; its
  exe is new.
- **Published** the same day as `v0.1.0-alpha.5`, a pre-release, the tag at the version's commit (`800226d`): alpha.4's
  draft renamed, its zip and audit.txt replaced (FFmpeg's source is the same file), its notes alpha.4's with the
  freeze's fix, the controller lines, one pad tried in the status table and issue #7 under Known issues. GitHub's
  SHA-256 of the zip and of audit.txt are the ones here. Issue #6 got the link.

## The release 0.1.0-alpha.6 (2026-11-12)

- **What it is for:** issue #7, the Grotto freeze (`5b17005`: e77C8's jobs start with no EDGE memo state, and the memo
  never runs e6D98's calls in another module's job), with the lift of e77C8 past 0xB90C and a report's saves that
  load (`27f7cae`, fork `ab764f6f`). docs/notes/2026-11-12-grotto-freeze.md has the cause and the runs.
- **The package**, from `383a2f1` and fork `ab764f6f`: a zip of 144.6 MB, 464 files and 328.7 MB unpacked, SHA-256
  `82b7c382b5b9d46ac93464305f3ad94bec43e23626b954d7c53e62708312d326`. Its audit found what alpha.5's had. Its build
  check made the DLL in 460 s, SHA-256 `4c9166c75b2340ff18f98e2859784638366b7766b2fe4ad829af04cc8f4c4a2d`; its map
  has `gowa_edge_job_b7c8` and e77C8's functions past 0xB90C (0xDB88, 0xE35C). The Prebuilt, 88.9 MB, stays here.
- **Published** the same day as `v0.1.0-alpha.6`, a pre-release, the tag at the version's commit, from a draft whose
  three files GitHub gave the SHA-256 of the ones here. Issue #7 got the link, and the reporter the Grotto to try again.
- **Not done:** the zip unpacked and played as a player has it; the tree's own build played the Grotto (three replays
  of the reporter's session) and the first fight.

## The release 0.1.0-alpha.7 (2026-11-12)

- **What it is for:** issue #9, the Delphi temple's broken textures and freeze on one PC: the VRAM guards (`523875d`,
  fork `8d34fda7`; docs/notes/2026-11-12-temple-textures.md has the cause as far as it is known, and the runs). With
  it the skinned fill's rounding (`8b6bd74`), and the vertex shaders built before the first start of a version
  (`97294b1`) behind a screen drawn in code (`69af2d7`).
- **The package**, from `3026645` and fork `8d34fda7`: a zip of 144.7 MB, 464 files and 328.8 MB unpacked, SHA-256
  `fc5e4923d2e501b27d67e17f51031e59abc07a713903493e8748d4e8f9e56aa7`. Its audit found what alpha.6's had. Its build
  check made the DLL in 475 s, SHA-256 `d686e28434f0d4b13b9abd8528b8f7e846ac599a423fedf002194e316a58f309`; its map
  has the three VRAM hooks. The Prebuilt, 88.9 MB, stays here.
- **Published** the same day as `v0.1.0-alpha.7`, a pre-release, the tag at the version's commit, from a draft whose
  three files GitHub gave the SHA-256 of the ones here. Issue #9 got the link and the cause as far as it is known,
  and its reporter the temple to play again. A reply cannot carry the Kit itself: a comment takes files of 25 MB at
  most, and only from the web page.
- **Not done:** the zip unpacked and played as a player has it. The tree's own build played the temple replay with
  the guards, and with one allocation's size made negative on purpose.

## A Kit unpacked over an older one (2026-11-13)

A player who unpacks a new Kit over the old folder keeps the old Kit's files the new one no longer has. The DLL
compiled every `native/spu_*.c` there was, so a source a later version takes out or renames would have gone into it.

- `kit-tree` writes `kit\kit-files.txt`, every file the Kit ships (435 in one made here), before its id, which so
  covers it.
- `kit-build` compiles of `native/spu_*.c` only those the list names (`kit_native_sources`), and says in `build.log`
  the files in the Kit's folder the list does not have (`kit_extras`). Nothing is removed. The work folder needs
  nothing: the PPU lift drops what its new lift lacks (`sync_dir`), and the SPU lifts start from empty folders. A
  source tree has no list and takes every native source, as before.
- Left as it is: an older Kit's Python module in `scripts\` with the name of one now in `ps3recomp\tools\` would come
  first on the Kit's path. No module has moved so far.
- From 0.1.0-alpha.7 to the next, no file of this tree or of the fork (`8d34fda` to `88ac02a`) was taken out or
  renamed: unpacking over alpha.7 would have been safe without this.
- Tried: `tests/test_kit_files.py` (4 cases); a Kit folder made by `kit-tree`, read by its own `gowa.py`: fresh, 20
  native sources and nothing extra; with a `native\spu_old_kit.c` put in, still 20, that one left out and named.
- `README.txt`, TO UPDATE: unpack into a new folder (or over the old one), keep `%LOCALAPPDATA%\Ascension-R` (the
  saves are in it), and with `portable.txt` unpack over the old folder.
