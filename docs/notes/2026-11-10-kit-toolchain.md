# The game's DLL without Visual Studio (2026-11-10)

The second step of the Kit release, after the DLL split (`2026-11-10-game-module.md`): `ascension_game.dll` is built
with clang-cl and lld-link alone, the way the Kit's setup will build it on a player's PC.

    py scripts/gowa.py module [--jobs N] [--out DIR]

With `GOWA_WORK` and `GOWA_ELF` set, `analyze`, `lift` and `module` work in another folder from another
`GOWA.elf`: that is the setup's pipeline.

## What it takes

- The lifted code, the native routines and `port/game_module.c` include about 50 of Microsoft's headers through
  `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<malloc.h>`, `<time.h>`, clang's `intrin.h` (`<setjmp.h>`)
  and, in the PPU sources, `<atomic>`. `port/kit/include` replaces the C ones with this project's own declarations,
  made the way the Universal C Runtime exports each function. `port/kit/prelude.h` replaces `<atomic>`: the lifted
  code needs it only for `PPU_FENCE`. clang's own headers give the rest.
- clang-cl runs with `/X`, so it searches no system folder, and with `-fms-compatibility-version=19.51.36247`.
  clang-cl takes the version of the Visual C++ it finds, and 19.33 when it finds none, so a fixed version gives the
  same objects on every PC. The other options are CMake's for the DLL.
- lld-link with `/NODEFAULTLIB` and one import library, made from `port/kit/kernel32.def`.
- The two steps CMake used to run: `build_spu_workloads.py` for the SPU programs inside the ELF, and
  `spu_to_native.py`, which writes General6's native routines from the lifted module into the build folder. The
  copy in `native/` is not compiled.
- The compile driver keeps an object when its command, its source and the headers have not changed, so a stopped
  build carries on from where it stopped. It writes `module.log` and, per translation unit, time and peak memory to
  `module-stats.tsv`.

## Three things that were missing

- `_byteswap_ulong` and the other byte swaps: the Universal C Runtime declares them in `stdlib.h`.
- `ldexpf` is not an export of the 64-bit runtime: its header makes it from `ldexp`, and so does this one.
- With `python -I` the lifters cannot import their neighbours (3.11 and later leave the script's folder off
  `sys.path`): the tools run with `-E -s`.

## Results (Ryzen 5 4600H, 16 GB)

- `/showIncludes` on a PPU and an SPU source: only this tree, `port/kit` and clang's own folder. The link reads
  `kernel32.lib` from the build folder and nothing else.
- Against CMake's build of the DLL, before linking: 39,199 code sections byte for byte the same, with the same
  relocations. 233 differ and 68 exist only in CMake's: Visual C++'s `atomic_thread_fence` was a call in some PPU
  functions and is inline in the Kit's, and `fprintf` is this project's. All other lifted functions are the same.
- The game with the Kit's DLL: to the level and the fight, no crash.
- The setup's pipeline in a fresh folder: `analyze` 2 s, `lift` 183 s, `module` 321 s at 5 jobs. The lifted sources
  are the same as this tree's, and the DLL is byte for byte the same in every section as the one built here.
- Compiling took 1,525 s of compiler time. No compile went above 325 MB of commit (SPU) or 225 MB (PPU), so
  `--jobs` now defaults to one compile per core and per GB of free memory.

## Not done

- A Windows with no Visual Studio at all: Windows Sandbox is not enabled on this PC. Here `/X`, a bare `PATH`
  and `/NODEFAULTLIB` keep Visual Studio out, and `/showIncludes` shows that it stays out.
- The Kit's own Python: the pipeline ran on this PC's Python in isolated mode, with the standard library only. The
  embeddable Python still has to be downloaded and tried.
- The launcher's step that runs this, and `package kit`.
