# macOS (Apple Silicon)

This fork is being brought to macOS on Apple Silicon (M1 and later). This page tracks what is done, how to build it,
and what is left.

## Status

The port **builds** for arm64 against ps3recomp's small sample program (the "smoke lift"), and the `macOS` workflow
(`.github/workflows/macos.yml`) builds it on a GitHub Apple Silicon runner on every push. It has **not run the game on
a Mac yet**: that needs the game's own lifted code, built on a Mac from your copy of the game.

| Area | On macOS |
|---|---|
| Runtime (ps3recomp) | Builds, with `patches/ps3recomp/` applied |
| Graphics | ps3recomp's Metal backend (`libs/video/rsx_metal_backend.m`). Much less complete than D3D12 |
| Controllers, audio | SDL2 |
| Game code (PPU) | Lifted C, built for arm64 |
| SPU code | Lifted C with the runtime's NEON forms; the round-toward-zero float mode is set in FPCR |
| Native SPU routines (`native/`) | The AVX2 ones step aside, so the lifted code runs those jobs (slower, same result). `spu_edge_gpuskin.c` has plain C forms, checked against the SSE ones |
| Settings file | `~/Library/Application Support/Ascension-R/ascension-r.ini`, or beside the program if one is there (or `portable.txt`) |
| Launcher window | Windows only. Start with `--no-launcher` and set `[paths]` in the settings file |
| Mods, texture replacement | Windows only for now (stand-ins in `port/posix_stubs.cpp`) |
| Shader pre-build, loading screen | D3D12's; not on Mac |
| Frame generation, FSR, SMAA, ANR | D3D12's; not on Mac |
| Ultrawide | D3D12's; the Mac stays at 16:9 |
| Trophy notices | Written to the log, not drawn |
| Multiplayer | Off |
| Problem reports, update check | Windows only |

## Building on a Mac

You need Xcode's command-line tools (`xcode-select --install`) and [Homebrew](https://brew.sh).

```sh
brew install cmake ninja python sdl2 glslang spirv-cross
git clone --recursive <this fork>
cd Ascension-R
sh scripts/apply_ps3recomp_patches.sh      # this fork's runtime changes (see below)
python3 -m pip install -r ps3recomp/requirements.txt
```

`glslang` and `spirv-cross` matter: without them the Metal backend cannot translate the game's shaders and only draws
with its fixed-function path.

### Check the toolchain first (no game needed)

This builds the same thing CI does:

```sh
cmake -S ps3recomp -B build-rt -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rt --target ps3recomp_runtime ps3recomp_smoke_lift
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DRECOMP_DIR="$PWD/build-rt/smoke/lift"
cmake --build build-mac
./build-mac/Ascension-R --no-launcher      # says "No ELF given": expected
```

### The game

The game's code is lifted from your own copy, as on Windows (README, *Building from source*). RPCS3 runs on Apple
Silicon, so the files can be installed and `GOWA.SELF` decrypted there (RPCS3: Utilities > Decrypt PS3 Binaries). Then:

```sh
python3 scripts/gowa.py doctor
python3 scripts/gowa.py setup --game-dir <the game's folder>
python3 scripts/gowa.py lift
python3 scripts/gowa.py build
./build/Ascension-R --no-launcher
```

`gowa.py doctor` and `build` run on macOS. `setup` and `lift` are Python and should, but have not been tried on a Mac
yet. The lifted code is large: lifting and building want about 20 GB of disk and plenty of memory. Lower
`gowa.py build --jobs` if the build runs out of memory.

## What was changed

In this repository:

- `port/port_compat.h`: one place for what the port's files need on every host: the MSVC string functions,
  `port_mem_mapped` (VirtualQuery's job, done with `mincore`), `PORT_TRY`/`PORT_EXCEPT` (SEH on Windows, a plain block
  elsewhere, where the guest's whole 4 GB is mapped and a guest read cannot fault), `port_cpu_relax`, `port_exe_dir`.
- `port/main.cpp`:
  - The guest's 4 GB is `mmap`ed whole, as on Windows. The old `calloc` of 3.5 GB was not page-aligned (memory commits
    failed) and left out the raw SPU windows above `0xE0000000`.
  - On macOS the game's main thread runs on a thread of its own (256 MB stack), and the process's main thread turns
    the run loop. AppKit only works on the main thread, and the Metal backend creates its window from the frame clock
    thread with `dispatch_sync` onto the main queue. With the guest on the main thread that would never be served, and
    the game would hang before its first frame. This is the same as ps3recomp's `templates/project/main.cpp`.
  - The settings file's folder (above).
  - The mods' pack step is Windows only.
- `port/posix_stubs.cpp`: what stands in, off Windows, for the files that are Windows only so far. It reads the game's
  edition from `PARAM.SFO` itself, since `game_find.cpp` is Windows only.
- Windows-only files, built empty elsewhere (`#ifdef _WIN32`): `mods.cpp`, `textures.cpp`, `update_check.cpp`,
  `anr_get.cpp`, `portable_copy.cpp`, `game_prepare.cpp`, `precompile.cpp`, `precompile_music.cpp`,
  `loading_screen.cpp`, `multiplayer.cpp`, `cpu_check.c`. `launcher.cpp`, `report.cpp` and `game_find.cpp` already were.
- Made portable: `cheats.cpp`, `vram_guard.cpp`, `online.cpp`, `poll_wait.cpp`, `trophies.cpp`, `edge_prop.c`,
  `skel_prop.c`.
- `native/spu_native_simd.h`: `<immintrin.h>` on x86 only. The plain SSE in `spu_edge_ctx.c` (loop A),
  `spu_edge_gpuskin.c` and `spu_edge_idx.c` has a non-x86 form or steps aside, and `spu_edge_memo.c`'s timer works off
  Windows.
- `CMakeLists.txt`: the x86 flags (`-msse4.1`, `-mavx2`, `-mfma`) only on x86 (`PORT_HOST_X86`), `PORT_SPU_AVX2` off on
  arm64, CoreFoundation on Apple, `port/posix_stubs.cpp`.
- `scripts/gowa.py build`: the Windows DLL copies (DXC, FSR, SDL3, the Visual C++ runtime) only on Windows.

In ps3recomp (`patches/ps3recomp/0001-build-off-windows.patch`):

- `libs/input/cellPad.c`: `input.device`, the stick tuning (`ps3_pad_tune`) and the sampler thread were inside the
  XInput and Win32 blocks, so the SDL2 build did not compile. Now they're shared, and the sampler thread uses pthreads.
- `libs/system/cellSaveData.c`: a backup's file times on POSIX (`st_atimespec` on macOS, `st_atim` on Linux).
- `runtime/spu/spu_dma.h`: the list prefetch uses `__builtin_prefetch` where `_mm_prefetch` is not there.
- `runtime/ppu/ppu_loader.cpp`: `ps3_sampler_start` had its non-Windows definition inside a Windows-only block.

### Moving the ps3recomp patches into a fork of your own

The `ps3recomp` submodule points at `rigaz29/ps3recomp`, which this fork cannot push to. Until there is a fork of it
under this fork's owner, its changes live in `patches/ps3recomp/` and `scripts/apply_ps3recomp_patches.sh` applies
them. To move them:

1. Fork `rigaz29/ps3recomp` on GitHub.
2. In `ps3recomp/`: `git checkout -b mac`, apply the patches, commit, push to the fork.
3. Point `.gitmodules` at the fork (`url` and `branch`), `git submodule sync`, commit the new submodule commit.
4. Delete `patches/ps3recomp/` and the apply step in `.github/workflows/macos.yml`.

## What is left, in order

1. **Boot on a Mac.** Lift the game, build it, run with `--no-launcher`. Expect Metal backend gaps: the D3D12 backend
   has had most of the work. `PS3RECOMP_METAL_HEADLESS=1` runs without a window, to tell a graphics problem from the
   rest.
2. **Performance on arm64.**
   - `poll_wait.cpp`'s back-off is x86 only (`rdtsc`, `monitorx`). On arm64 the polling loops spin. A version with
     `cntvct_el0` and `wfe`/`yield` would give the SPU workers their cores back.
   - The native SPU routines step aside on arm64. The hottest (`spu_edge_light.c`, `spu_edge_cull.c`,
     `spu_g6_routines.c`) are worth NEON versions once a profile says so. `scripts/spu_to_native.py` writes the G6
     ones and could write NEON too.
   - The Mac's performance and efficiency cores: the SPU workers and the frame clock on performance cores
     (`pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)`).
3. **The Metal backend** toward the D3D12 one: render-target handling, the game's post chain, the overlay line and
   trophy notices (`rsx_d3d12_backend_overlay_line`, `_trophy` in `posix_stubs.cpp`), ultrawide (`_wide_ratio`).
4. **Mods and textures.** `mods.cpp` and `textures.cpp` keep paths as `std::wstring` and use Win32 for files and WIC
   for PNGs. They need UTF-8 paths, `<filesystem>` and a PNG decoder (stb_image) to run on macOS.
5. **A launcher.** `launcher.cpp` is a Win32 window. Something cross-platform (SDL3 or Dear ImGui) for macOS, or a
   small AppKit one.
6. **Packaging.** An `.app` bundle (CMake's `MACOSX_BUNDLE`), an icon, code signing, and notarization so Gatekeeper
   opens it.
7. **Preparing the game on a Mac** (`game_prepare.cpp`, `game_find.cpp`): finding RPCS3's folders on macOS
   (`~/Library/Application Support/rpcs3`) and decrypting from there.
