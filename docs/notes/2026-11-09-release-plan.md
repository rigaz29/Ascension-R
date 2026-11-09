# Release plan: Ascension-R (2026-11-09)

The goal is a public source repository, with the runtime as a submodule, and a download a player unpacks, points at their own game and plays. Nothing in the download is
derived from the game.

## Decisions

| | |
|---|---|
| Name | `Ascension-R`: `gowa-recomp` renamed and made public. `ps3recomp` stays a submodule linked to `rigaz29/ps3recomp` |
| Model | The zip holds the program and the tools. The game's part is built once on the player's PC, from the player's files |
| Keys and firmware | The fork is not cleaned: it stays as upstream has it. The download holds no keys or firmware. The player's own RPCS3 decrypts `GOWA.SELF` and `libsre.sprx` |
| Identity | Commits stay as they are, no history rewrite |
| Version | `v0.1.0-alpha.1`, semantic versions after that |
| Update | Replace the program's folder from Releases; the launcher only says a new version exists |
| Data | `%LOCALAPPDATA%\Ascension-R`, or beside the exe with `portable.txt` |
| Support | GitHub Issues, with the report zip |
| Signing | None at first; the README explains the SmartScreen prompt |

Keeping the history means everything in it becomes readable when the repository opens: the notes, one commit's worth of personal paths, and the author's
name and address on all 248 commits. No game file, key or firmware was ever committed to this repository.

## What is built today (measured, 4600H)

| Part | Objects | Compile CPU | Object size | Built by |
|---|---|---|---|---|
| Lifted PPU | 201 | 1,657 s | 87 MB | player |
| Lifted SPU | 90 | 567 s | 38 MB | player |
| Lifted libsre | 2 | 14 s | 1 MB | player |
| Runtime and libraries | 198 | 149 s | 6 MB | release |
| Native routines | 24 | 40 s | 1 MB | release |
| Port (main, launcher, ...) | 16 | 18 s | 1 MB | release |

The game's part is 91.5% of the compile time, so building the rest in the release saves little time. What it saves is the toolchain: only the game's part has to
compile on a machine with no Visual Studio. Five jobs take about 8 minutes, the lift 3.5; at the default of three jobs the whole is about 16 minutes.

Lifted PPU objects reference 33 symbols outside themselves: `vm_read*`, `vm_write*`, `vm_base`, `g_ppu_mm`, `ps3_indirect_call`, `ps3_hle_call`, `lv2_syscall`,
`ppu_stwcx32`, `ppu_stdcx64`, `ppu_timebase_now`, `gowa_poll_wait`, the cheat hooks and patch guards, `sqrt`, `sqrtf`, `log2f`, and what the compiler adds
(`_fltused`, the security cookie). Lifted SPU objects also take the native routines, thread-local storage and a few CRT calls in diagnostic paths.

## The download

```
Ascension-R-0.1.0-alpha.1-windows.zip
  Ascension-R.exe          runtime, libraries, native routines, launcher, setup; no lifted code
  amd_fidelityfx_*.dll, dxcompiler.dll, dxil.dll, FFmpeg's LGPL DLLs
  setup\python\            embeddable Python, capstone, construct
  setup\tools\             the lifters
  setup\llvm\              clang.exe, lld-link.exe, clang's own headers
  setup\sdk\               the game module's headers, the host's import library
  licenses\
```

Built on the player's PC, under the data folder: the decrypted `GOWA.elf` and `libsre.prx`, the lifted sources, the objects, and `ascension_game.dll`.

## Phases

### 1. Prepare the source: 1-2 days

- A build switch, `PS3RECOMP_NO_DECRYPT`, leaves the decryption out of the release: the keys compiled from `libs/filesystem/edat.c` and `runtime/ppu/sdata_decrypt.h`, and the `.dec` files `edat_resolve`
  writes beside the game's. It is the only change this plan makes in the fork; its files otherwise stay as upstream has them. The game has no `.sdat` and one EDAT, which already fails for
  want of a RIF key, so nothing should change. Test that.
- `third_party/at3_standalone` is LGPL and linked statically; its source is public through the submodule. List it in `THIRD_PARTY_NOTICES.md`, with FFmpeg and DXC, and put their licences in the zip.
- `native/` stays: hand-written, and no constant table in it long enough to be copied data. Decide again if a lawyer says otherwise.
- Exit: route, fight and the CYN050 cutscene as before; the key bytes are not in the built exe.

### 2. Rename: 1 day

- The submodule stays, on `rigaz29/ps3recomp`, branch `gowa`.
- Rename what a player sees: the exe, the window title, the issue URL, the settings file (read the old name once). Internal `gowa_` names stay.
- `gh repo rename Ascension-R`, still private.
- CI on this repository: reject key patterns and game file types, and run the runtime's unit tests.
- Exit: a fresh `git clone --recursive` builds and runs on this PC.

### 3. Split the program from the game: 5-8 days, the gate

- The game's part becomes `ascension_game.dll`, built with no CRT and no Windows SDK against the host's own import library: `/GS-`, `memcpy` and the math from the host, printf
  through a host function, `__declspec(thread)` with constant initialisers only.
- The host loads it, and refuses one built for another ABI version.
- **Gate:** the fight bench, static against split, two runs a side in ABBA order. Lifted time per flip from samples within 2%, as flips/s is too noisy.
- If it fails: inline `vm_read*` and `vm_write*` in the module first. If that fails too, the setup installs Visual Studio Build Tools and does the full build, a much larger download.

### 4. Toolchain and pipeline: 4-6 days

- A cut-down LLVM release in the zip, and embeddable Python.
- One script from `gowa.py lift` and `lle`, and a compile driver in place of CMake and Ninja: jobs by RAM and cores, resume after a stop, progress lines, a log for the report.
- Exit: in Windows Sandbox, with no Visual Studio and no Python, the module builds and matches the one built here. Time and peak memory recorded.

### 5. Setup in the launcher: 7-10 days

- CPU check first: AVX2 and FMA, with a plain message. Then disk and RAM.
- The game's folder: the player picks an extracted PKG or RPCS3's own copy, with the launcher's existing search.
- Decrypting, with nothing more for the player to do:
  1. Find RPCS3: the one the game's folder is in, else by search, else the player picks `rpcs3.exe` once.
  2. Copy `USRDIR/GOWA.SELF`, `dev_flash/sys/external/libsre.sprx` and the game's `.rap` from `dev_hdd0/home/*/exdata` into the data folder.
  3. Run that RPCS3 with `--decrypt` on the copies, `RPCS3_CONFIG_DIR` pointing into the data folder so it writes nothing in its own. RPCS3 refuses a temporary folder.
  4. Check `GOWA.elf` by SHA-256. The `lle` step checks that libsre exports what the port needs, so another firmware's copy is fine if it does.
  5. The pipeline relocates and lifts libsre from there.
  - "I have decrypted files" stays as the other way in. With neither, a plain message says what is missing: RPCS3 with the firmware and the game's licence.
  - Not tried yet on this project. The first thing to test here is `--decrypt` with the redirected folder, on a `.rap` copied into it.
- Build with an estimate, progress, stop and resume. Then the shader precompile, then Play.
- The player's game folder is only read.
- Optional: copy the saves from RPCS3.
- A new release rebuilds the module only when the ABI version or the lifter changes.

### 6. Packaging: 2-3 days

- `scripts/package.py` makes the zip and stops if it finds a lifted symbol, a key pattern or a game file type. Only the lifters the pipeline runs go in `setup\tools`, not the fork's whole `tools` folder.
- Host on the static CRT if the Phase 3 bench allows it, else the three Visual C++ DLLs beside it.
- Release workflow on a tag: the host needs no game to build. SHA-256 file with the zip.

### 7. Testers: 2-3 weeks of calendar

- Three to five people on NVIDIA, AMD and Intel GPUs, 16 GB and 8 GB, SSD and hard disk, Windows 10 and 11.
- At least one playthrough to the end, which has never been done.
- Exit: no blocker open, and a requirements table from what they report.

### 8. Go public: 1 day

- New README for players: what it is, requirements, install, status and known issues, disclaimer, credits. `docs/BUILDING.md` for developers.
- `docs/notes` and `PLAN.md` out of the tree; they stay readable in history.
- Make the repository public, tag `v0.1.0-alpha.1`, upload the zip.

Making it public and uploading the zip are each confirmed separately before they are done.

## Total

About 21 to 31 working days, plus the testers. Phase 3 decides whether the small download is possible, so it comes before any setup work.

## Risks

- A takedown is still possible: the notes and the native routines describe the game's code, and recompilation projects get notices. The download holds none of the game's code.
- The linked fork is public with upstream's keys in its source and six decrypted firmware modules in its history. A takedown of the fork would also break `git clone --recursive` of this repository.
- The player needs RPCS3 with the firmware installed and the game's licence, for the decrypt.
- Only NPUA80918 1.12. Other regions and the disc release are not lifted.
- 16 GB is assumed for the build. An 8 GB PC may need one or two jobs and a long wait.
- Build time on a slow CPU is unmeasured. Phase 7 gives real figures.
