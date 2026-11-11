# The precompile gate: the vertex shaders before the first start of each version (2026-11-12)

Issue #8 saw things missing for a moment after walking into a new place. The backend builds a shader the first time
a draw needs it; it waits for it up to 3 ms a frame and then leaves that draw out, so new places can pop in. Precompile
shaders (`precompile.cpp`) builds every vertex shader of the game ahead, but only when the player presses it. Now the
game does it before its first start of each version, and does not start before it is done.

## How it goes

- **The gate** (`gowa_precompile_gate`, `port/loading_screen.cpp`), in `main` before the frame clock starts: before
  the D3D12 window, its swap chain and the shader cache are made, and after the ELF is loaded (an `SPU_JOB_REPLAY` run
  has gone its own way by then).
- **Done for this version:** `<shader cache>.precompiled` says `Ascension-R <version>` and a pack of the cache is there.
  `precompile.cpp` writes it when its build is done. Clear shaders moves it to the Recycle Bin with the cache.
- **Not done:** a window laid out as the game's will be (windowed at its size, or borderless over the monitor) shows an
  8-bit screen while a process of its own builds them: this exe with `--precompile-shaders`, this one's settings file,
  and `--set` for the cache, the ELF and the GPU, as a script may have given them on the command line. Its progress
  comes on a pipe, the lines the launcher reads. The cache's lock is that process's, and free again when it ends, so
  the game opens the cache as always. `GOWA_PRECOMPILE_FOREGROUND` keeps it at normal priority: the player waits for it.
- **The screen goes** when the game's window is up (`gowa_loading_screen_close` after `rsx_backend_init`), so nothing
  of the desktop shows between them.
- **Closing it while it builds** quits: the build is stopped, nothing is marked. There is no way past it.
- **A build that fails** says why on the screen for 6 s, and the game starts all the same, unmarked: the next start
  tries again. A player is not kept out of the game by a disk or a lock.
- **The programs found** go beside the cache as `<cache>.vprogs` (1.9 MB here), with the archives' count and bytes;
  while those are the same the archives are not read again, so a version after this one checks in seconds.
- **`GOWA_PRECOMPILE_FIRST=0`** (`graphics.precompile_first`) leaves the gate out. `bench.py` sets it: a bench measures
  the game, on a copy of a cache.

## The screen

Drawn on a 640x360 canvas with GDI, scaled up by whole pixels where that fills 90% of the window, else to fit, nearest
either way:

- three pictures behind (`port/art`, kept out of git like every picture; a build embeds those it finds as RCDATA
  201-203 through `loading_art.rc`, which CMake writes): the two 1080-wide panoramas pan from left to right over the
  14 s each is shown, and the next comes in through a 4x4 ordered dither on 2x2 blocks;
- the lower part darker in dithered steps;
- a panel with a gold frame and cut corners; the title at 2x in a 5x7 pixel font of its own, the percentage, a bar of
  28 segments from red through orange to gold, lit from above, the next one blinking; the archive or the count of
  shaders, the time left, and the version.

`Ascension-R.exe --loading-preview <folder>` writes it in five states as BMP files at 2x.

## Tried

On a test folder (`logs\gate`) with a copy of the player's cache, the game started with `--no-launcher`:

| Start | Screen | Game window | |
|---|---|---|---|
| No mark, no list | 0.5 s to 109.6 s | at 108.4 s | the build 108 s; marked, 1.9 MB of programs kept |
| Mark taken away, the list kept | 0.3 s to 2.0 s | at 1.4 s | checked in 1 s |
| Marked | none | at 0.3 s | |
| No mark, no list, the screen closed at 5 s | 0.3 s to 5.6 s | none | quit; nothing marked, nothing kept, no process left |

At 125% the window's client is 1600x900: the canvas at 2.5x to fit, and the screenshot showed the panel whole.

## Not done

- Fragment shaders are still built the first time they are needed, so a new place can still leave something out for
  a moment the first time: the gate takes the vertex shaders' part of that away.
- The Kit's player has a build of the game's code first (about ten minutes) and then this, on the first Play.
