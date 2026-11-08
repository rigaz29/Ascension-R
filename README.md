# gowa-recomp

A static recompilation of **God of War: Ascension** (PS3, NPUA80918, version 1.12) to native
Windows, built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) (fork
[rigaz29/ps3recomp](https://github.com/rigaz29/ps3recomp), branch `gowa`).

It builds, boots, plays the cinematics and goes through the menus to the levels. The two scenes
measured so far, a fight of the first level and the gate, run above 30 frames a second on a Ryzen 5
4600H with a GTX 1650 Ti, with the Smooth preset. It has not been played through. See
[PROGRESS.md](PROGRESS.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

**No game files are in this repository, and none will be.** You need your own copy of the game,
installed in RPCS3 with its license, and you decrypt it yourself.

## Docs

- [PLAN.md](PLAN.md): phases, rules, workflow.
- [PROGRESS.md](PROGRESS.md): where it stands.
- [docs/ANALYSIS.md](docs/ANALYSIS.md): what the title is made of.
- [docs/notes/](docs/notes/): one note per session.

## Quick start (Windows)

```
git clone --recursive https://github.com/rigaz29/gowa-recomp
cd gowa-recomp
python -m venv ..\.venv
..\.venv\Scripts\pip install -r ps3recomp\requirements.txt

# 1. Decrypt USRDIR\GOWA.SELF to GOWA.elf with RPCS3 (Utilities > Decrypt PS3 Binaries).
# 2. Point the project at the installed game. It is only read.
..\.venv\Scripts\python scripts\gowa.py setup --game-dir <RPCS3>\dev_hdd0\game\NPUA80918
..\.venv\Scripts\python scripts\gowa.py doctor
..\.venv\Scripts\python scripts\gowa.py analyze
..\.venv\Scripts\python scripts\gowa.py lift
# 3. The SPURS lock-free queues run the firmware's own libsre: copy
#    dev_flash\sys\external\libsre.sprx from RPCS3 to firmware\, decrypt it there to
#    libsre.prx (Utilities > Decrypt PS3 Binaries), then:
..\.venv\Scripts\python scripts\gowa.py lle
..\.venv\Scripts\python scripts\gowa.py build
..\.venv\Scripts\python scripts\gowa.py run
```

The launcher shows the game's folder in its System tab. Browse picks another one, and Auto detect looks for the game
on every disk; it starts by itself when the game is not where the settings say.

`build` copies `dxcompiler.dll` and `dxil.dll` from the Windows SDK next to the exe, and guest shaders compile with
them. Without the two files they compile with D3DCompile, about 2.4 times slower. `graphics.shader_compiler = fxc`
asks for that too. The two compilers keep their caches apart, so a first run on either compiles everything once.
Each cache keeps every program once and is packed with zstd: 2,287 programs and 2,309 pipelines take 5.9 MB, where
the files before took 83 MB. A cache from an older build is converted at the first start, on a thread of its own.
The pipelines kept in it are built on several threads at the start, and the title waits for them, a third of a second
here: no pipeline the cache has is then built in play. `graphics.pipeline_wait = off` (Build pipelines first, in the
Graphics tab) lets the game go on while they build.
Precompile shaders, in the launcher's sidebar, reads the game's archives and compiles every vertex shader in them into
the cache before you play: 3,455 programs, about a minute and a half here, 3 MB more on disk. A session of the first
level then compiled 4 vertex shaders where it had compiled 212. Fragment shaders are still compiled as the game reaches
them: their text depends on the textures and state of each draw, which the files do not say.
Clear shaders, in the launcher's sidebar, moves both to the Recycle Bin.

The GPU's work is recorded, submitted and presented on a thread of its own, `rsx render`, beside the one that walks
the game's command buffer. Where that walk was the limit it gives about 10% (the gate with shadows on, 30 to 33 frames
a second). `graphics.render_thread = off` (Render thread in the launcher's Graphics tab) puts both back on one thread.

## FSR 3.1

Two rows in the launcher's Graphics tab, both off by default, measured on a GTX 1650 Ti. `build` copies their DLLs from
an [FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) unpacked under `../fsr-sdk`, or from
local.json's `fsr_dir`. Without the DLLs the log says so and the game goes on without FSR 3.

- **Scaling: AMD FSR 3.1** (`graphics.scaling = fsr3`) rebuilds scene frames from 720p with jitter, depth and motion, in
  place of SMAA. Logos, menus and loading screens keep FSR 1. On a GTX 1650 Ti it takes 2.0 ms at 1600x900, where FSR 1 and
  SMAA take 1.1. The HUD stays at 720p sharpness.
- **Frame generation** (`graphics.frame_generation = on`) shows two frames for each of the game's, so keep VSync on. It
  holds the game at half the monitor's refresh rate: 30 frames a second at 60 Hz, 60 at 120 Hz. Past 120 Hz it halves that
  again, as 60 is the most a PS3 flips and a frame should stay on screen for a whole number of refreshes: 36 at 144 Hz, 60
  at 240 Hz. Only 60 Hz has been tried on a real display, and the game has not been played above 39 frames a second.
  With VSync on, up to 120 Hz, that pace comes from VSync itself, which keeps the game up to about 15 ms ahead of the
  display and takes up what a slow frame loses. A Frame rate cap comes first: it gives those milliseconds back for a less
  even pace, and suits a PC that cannot hold the rate, 30 on a 120 Hz monitor.
  `graphics.frame_generation_refresh` gives the rate by hand, for a VRR display: 120 on a 144 Hz one is 60 frames a second.
  While the game is under 25 generation stands aside (`graphics.frame_generation_floor`).

Both take what moves from the title's motion blur draws, so Disable Motion Blur is left off with them: EDGE goes from 46
to 67 ms a flip, and a fight at the 30 cap loses about 0.3 frames a second on a cool machine, more on a hot one.
`graphics.frame_generation_objects = off` goes back to the camera's motion alone, and the patch applies. Both need Disable
MLAA on: with MLAA the depth at the final composite is the next frame's, so FSR 3 stays off and the log says why. The
[plan](docs/notes/2026-11-07-fsr31-plan.md) has the measurements.

## Cheats

The launcher's Cheats tab: Damage to Kratos (half, a quarter or none), Kratos's damage (2x, 5x or one hit), Max red orbs,
Infinite magic and items, Infinite rage and Infinite double jumps. All are off by default and hook the title's own code, its
damage, its meters and its jump ([note](docs/notes/2026-11-07-cheats.md)).

## Layout

- `scripts/gowa.py`: the pipeline (doctor, setup, analyze, lift, lle, build, run, symbolize).
- `main.cpp`, `launcher.cpp`, `stubs.cpp`, `CMakeLists.txt`: the port itself, carried over from the
  God of War HD port ([rigaz29/gow1hd-recomp](https://github.com/rigaz29/gow1hd-recomp)).
- `tools/hle_coverage.py`: which of the title's imports the runtime answers.
- `ps3recomp/`: the toolkit and runtime (submodule).

## Related work

Two other ports of God of War: Ascension on ps3recomp, of the disc releases, are further along and
were a great help reading this title's frame pipeline:
[MaChInEgUn3/God-of-War-Ascension-Recompiled](https://github.com/MaChInEgUn3/God-of-War-Ascension-Recompiled)
and [arnaldolima1/gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc).

## License

MIT, see [LICENSE](LICENSE). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
God of War is a trademark of Sony Interactive Entertainment; this project is not affiliated with
it.
