# The Kit's toolchain files

What the Kit release needs to build the game's DLL (`ascension_game.dll`) on a player's PC with clang-cl and
lld-link alone: no Visual Studio, no Windows SDK, no import library of a C runtime.

- `include/`: the C library declarations that the lifted code and the native routines use (`stdio.h`, `stdlib.h`,
  `string.h`, `math.h`, `malloc.h`, `time.h`, plus `setjmp.h` for clang's `intrin.h`). Each function is declared
  the way the Universal C Runtime exports it, so the DLL calls the same functions (through `port/game_module.c`) as a
  build with Microsoft's headers does. Written for this project, MIT like the rest of it. clang's own headers cover
  the rest (`stdint.h`, `stddef.h`, `stdarg.h`, the intrinsics).
- `prelude.h`: forced into the lifted PPU sources. It provides the guest barriers without C++'s `<atomic>`.
- `kernel32.def`: the DLL's one import. Windows gives a DLL's thread-locals an index only if the DLL imports
  something. `lld-link /lib /def:kernel32.def /machine:x64` turns it into a library.

`scripts/gowa.py module` builds the DLL with these files (`docs/notes/2026-11-10-kit-toolchain.md`). When the code
starts to use something else from the C library, a compile error names it, and it goes into these headers.

`scripts/gowa.py kit-tree` puts them, with the rest of what the build reads, into the Kit's folder, and the
launcher runs `gowa.py kit-build` from there (`docs/notes/2026-11-10-kit-setup.md`).
