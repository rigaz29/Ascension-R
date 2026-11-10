# The game's code as a DLL of its own (2026-11-10)

The Kit release builds the code translated from the game on the player's PC. Its first step: that code goes into
`ascension_game.dll`, and the program is built without it. `PORT_GAME_MODULE` (CMake, off by default) switches it. Off,
the build is one exe as before, which is what the Prebuilt release ships.

    GOWA_BUILD_DIR=build-kit py scripts/gowa.py build -D PORT_GAME_MODULE=ON

## What crosses between the two

Measured with `llvm-nm` on the objects of the build: the lifted PPU and SPU code and the native routines together take

- 42 functions of the program (the hooks, `vm_read*`/`vm_write*`, the syscall and HLE entries, the SPU channel and
  dispatch calls, four of the D3D12 backend's vertex functions),
- 8 functions of the C runtime called by name and 13 called through an import (all in diagnostic paths but `memcpy`,
  `memcmp`, `memmove`, `sqrt`, `sqrtf`),
- 9 pieces of data the program settles before the guest runs (`vm_base`, `g_ppu_mm`, `g_spu_direct`,
  `g_spu_ijmp_local`, the five patch guards),
- the SPU's pending transfer `g_spu_trampoline_fn` (a thread-local) and `g_spu_stack_reset_hint` (written while the guest
  runs).

The other way, the program takes only the lifted function table. `port/` calls no lifted code.

## How

- `port/game_module.h` lists all of it. The program fills one table (`game_module_host.c`) and the DLL takes it in
  `ascension_game_init` (`game_module.c`). Each function becomes a thunk of one jump; each data item a copy, made once
  (`game_module_sync`) after the patches are applied and before the guest runs, so lifted code reads it as before.
- The DLL is linked with `/NODEFAULTLIB /NOENTRY`. It runs its own constructors, where the lifted SPU code registers
  itself, and defines `_fltused`, `__chkstk` and the TLS directory that the C runtime would otherwise provide.
- The pending transfer stays the program's thread-local. A DLL cannot name another module's, so the DLL finds it the
  way compiled code finds one, from the thread's array of TLS blocks at `gs:0x58`, with the index and offset the
  program passes (`spu_context.h`, `PS3_GAME_MODULE`).
- `g_spu_stack_reset_hint`: the DLL reads the program's array through a pointer.
- The DLL's `spu_indirect_branch` is its thunk, so it has a different address from the program's. The drain loop
  accepts both (`spu_drain.c`, `PS3_GAME_MODULE_HOST`).
- The program copies the lifted function table into its own `function_table`, which the runtime reads as before.
- `spu_dispatch_mt.c` is left out of the Kit build: nothing calls `spu_indirect_branch_mt`.

## What broke on the way

- clang 23.1.3 crashed on numbered local labels (`1:`, `2f`) in file-scope assembly. Named labels work.
- `s_host` is a winsock macro.
- **Windows sets up a DLL's thread-locals only if the DLL imports something.** With no imports at all, the loader
  never fills `_tls_index`. The DLL's thread-locals then read and write the program's block (index 0): the native
  routines' thread-locals overwrote the program's, and the SPU code called a garbage pointer. A small test DLL showed
  it: index 0 with no imports, 2 with one (with or without an entry point). The DLL now imports
  `GetCurrentThreadId`, and the program refuses a DLL whose index is its own.

## Results

- Prebuilt unchanged: built after this change, its `.text`, `.data`, `.pdata`, `.tls`, `.rsrc` and `.reloc` are byte
  for byte those of the exe before. `.rdata` differs only in the 15 bytes of the two commit names.
- Kit: `Ascension-R.exe` 3.4 MB, `ascension_game.dll` 84.9 MB. The DLL imports one function and exports one.
- Gate: the fight in the test saves (`logs/user-p2c`, no damage to Kratos, FSR 1 at 1280x720), 8 runs in the order
  Prebuilt, Kit, Kit, Prebuilt, Kit, Prebuilt, Prebuilt, Kit, with 9 windows from 5 to 95 s after the load:

  | | median flips/s | CPU s a flip | PPU threads | SPU threads |
  |---|---|---|---|---|
  | Prebuilt | 33.72 | 0.2394 | 0.0320 | 0.1548 |
  | Kit | 33.71 | 0.2432 | 0.0323 | 0.1583 |
  | Kit against Prebuilt | -0.04% | +1.6% | +0.7% | +2.3% |

  Between runs of the same build, CPU per flip spread 7.7% (Prebuilt) and 11.7% (Kit). Most of the difference comes from
  the session's first pair: Prebuilt in the first run, on a cool laptop, used 0.2268 s a flip, and no other run went
  below 0.23. Without that pair, Kit used 2.3% less CPU a flip. The split costs nothing that these runs can measure.

## Not done

- Building the DLL without Visual Studio: the lifted code still pulls about 50 of Microsoft's headers (`<atomic>`,
  `<string.h>`, `<math.h>`, ...) and the compile goes through CMake and Ninja.
- The launcher step that builds the DLL, and `gowa.py package kit`, which still refuses.
- A DLL from another release: `GAME_MODULE_ABI` is checked, but nothing yet changes it when a structure changes.
