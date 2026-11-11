# Ascension-R

[![CI](https://github.com/rigaz29/Ascension-R/actions/workflows/ci.yml/badge.svg)](https://github.com/rigaz29/Ascension-R/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-lightgrey.svg)
![Status: alpha](https://img.shields.io/badge/status-alpha-orange.svg)

A native Windows port of **God of War: Ascension** (PS3), made by static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp). The game's PPU and SPU code is translated to x86-64 ahead of time and the RSX
is drawn with Direct3D 12, so no emulator runs while you play.

> [!IMPORTANT]
> This repository holds no game files. You build the port from your own copy of the game. See [Legal](#legal).

## Contents

- [Status](#status)
- [Features](#features)
- [Download](#download)
- [What you need](#what-you-need)
- [Building from source](#building-from-source)
- [Playing](#playing)
- [Reporting a problem](#reporting-a-problem)
- [How it works](#how-it-works)
- [Project layout](#project-layout)
- [Contributing](#contributing)
- [Legal](#legal)
- [Credits](#credits)
- [License](#license)

## Status

It boots, plays the cinematics and goes through the menus into the levels. It has not been played from start to finish, so expect
problems further in.

- **Game:** God of War: Ascension, digital US release, NPUA80918, version 1.12: the game and its updates 1.04 to 1.12.
- **Platform:** Windows 10 or 11, 64-bit. Tested on Windows 11.
- **Speed:** above 30 frames a second in the two scenes measured, a fight of the first level and the gate, on a Ryzen 5 4600H with
  a GTX 1650 Ti and the Smooth preset.
- **Online:** not supported. The port is single player.
- **Downloads:** the Kit, on the [releases page](https://github.com/rigaz29/Ascension-R/releases). It holds none of the game's code
  and builds it on your PC from your copy.

## Features

- **Launcher:** a ready check, three presets (Smooth, Balanced, Faithful) and every setting below. It finds the game's folder by
  itself.
- **Picture:** internal resolution from 1x to 4x or native, AMD FSR 1 and FSR 3.1 scaling, FSR 3.1 frame generation, SMAA 1x and
  anisotropic filtering.
- **Display:** windowed or borderless, ultrawide, VSync, a frame rate cap, and frame rate and load on screen.
- **Shaders:** a compressed cache, pipelines built before the title, and vertex shaders that can be compiled before you play.
- **Game patches:** switch off MLAA, depth of field, motion blur, mesh trimming or dynamic shadows.
- **Cheats:** damage to and from Kratos, max red orbs, infinite magic and items, infinite rage and double jumps.
- **Controls:** a controller or keyboard and mouse. Xbox and other XInput pads, and through SDL3 the DualShock 4, the
  DualSense, Switch controllers and many others; stick dead zone, confirm button, vibration and controller API settings.
- **Saves:** backups kept before the game writes over a save.
- **Problem reports:** one button makes a zip of the logs, with your paths hidden.

[docs/FEATURES.md](docs/FEATURES.md) describes each one and its setting.

## Download

The [releases page](https://github.com/rigaz29/Ascension-R/releases) has the Kit: a zip of the program without the game's code.
Its launcher finds your copy of the game, decrypts it with RPCS3 and builds the game's code on your PC, once, in about ten minutes.
There is nothing else to install: the Kit brings its own Python and LLVM. Each release's notes have the steps.

Your copy needs all nine of the game's updates, 1.04 to 1.12, installed in RPCS3 in that order. Each brings its own files:
1.12's is the EBOOT alone, and the archives come with the ones before it. With 1.12 alone the game stops on its first
loading screen.

## What you need

To build it from source:

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 and FMA, and a GPU with Direct3D 12.
- The game, installed in RPCS3 with its license, and its updates 1.04 to 1.12, installed in that order.
- Python 3.10 or later, Git, CMake, Ninja, Visual Studio with "C++ Clang Compiler for Windows", a Windows SDK, and about 20 GB free.
- For the game's movies: an LGPL shared build of FFmpeg with its headers, unpacked under `..\ffmpeg`, for example
  `win64-lgpl-shared` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases).
- Optional, for AMD FSR 3.1: [FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), unpacked under
  `..\fsr-sdk`.

## Building from source

```
git clone --recursive https://github.com/rigaz29/Ascension-R
cd Ascension-R
python -m venv ..\.venv
..\.venv\Scripts\pip install -r ps3recomp\requirements.txt

# 1. Decrypt USRDIR\GOWA.SELF to GOWA.elf with RPCS3 (Utilities > Decrypt PS3 Binaries).
# 2. Point the project at the installed game. It is only read.
..\.venv\Scripts\python scripts\gowa.py setup --game-dir <RPCS3>\dev_hdd0\game\NPUA80918
..\.venv\Scripts\python scripts\gowa.py doctor
..\.venv\Scripts\python scripts\gowa.py analyze
..\.venv\Scripts\python scripts\gowa.py lift
..\.venv\Scripts\python scripts\gowa.py build
..\.venv\Scripts\python scripts\gowa.py run
```

`doctor` lists anything that is missing. The lift and the first build take about 13 minutes on a Ryzen 5 4600H. `run` starts the
game straight away, with its log in `logs\`.

## Playing

Open `build\Ascension-R.exe`. The launcher checks the game's folder and your setup, and **Play** starts the game.

The first start of each version builds the game's vertex shaders before the game begins, on a screen of its own: a
minute or two the first time, seconds after an update. Fragment shaders are built the first time they are needed, so
expect some stutter at first. **Precompile shaders**, in the launcher's sidebar, does the vertex shaders on demand.

A build from source keeps its settings, logs and shader caches in `build\`, and its saves in `gamedata\`.

## Reporting a problem

**Report a problem**, in the launcher's sidebar, makes a zip in `logs\` beside the settings file, for you to attach to an
[issue](https://github.com/rigaz29/Ascension-R/issues/new?template=problem.yml). The launcher offers it by itself after a session
that crashed, lost the graphics device, froze or stopped with no reason in its log. `Ascension-R --report [--with-saves]` makes one
without the window.

The zip holds the logs and pad recordings of the last three sessions the game ran in, your settings, the build, your hardware and
driver versions, and Windows' records of crashes and hangs of this exe in the last two weeks. Folder paths and your Windows account
and PC names are hidden. The game sends nothing by itself and no game file goes in. Saves go in only if you tick the box, without
the game's images and sounds.

If no new frame comes for 15 seconds, the log gets what each thread was doing, and again at 60 (`GOWA_FREEZE_WATCH=<seconds>`,
`0` = off).

## How it works

1. `gowa.py lift` translates the PPU code of `GOWA.elf` to C++, and `build` does the same to C for the SPU programs and job modules.
   Both are compiled for x86-64 with clang-cl.
2. The [ps3recomp](https://github.com/sp00nznet/ps3recomp) runtime stands in for the PS3: its system calls and libraries, SPURS, the
   file system, audio, pads and save data.
3. The game's RSX commands are drawn with Direct3D 12. Its shaders are translated to HLSL and compiled with DXC.
4. Movies are decoded with FFmpeg.
5. SPU code that was not translated ahead of time runs in an interpreter. The session records it, and the next `lift` adds it.

More detail: [what the game is made of](docs/ANALYSIS.md), [what has been measured](docs/PERF.md), and the
[development notes](docs/notes), one per session.

## Project layout

```
port/          the port: entry point, launcher, game-specific code
native/        hand-written routines for the SPU code that costs the most
scripts/       gowa.py, the pipeline (doctor, setup, analyze, lift, build, run, symbolize), and the bench
tools/         which of the game's imports the runtime answers, shader cache checks, RenderDoc helpers
tests/         tests for the launcher's game search, shader compile guards and native routines
docs/          analysis, measurements, features and development notes
packaging/     text files that go beside the program in a package
third_party/   zlib
ps3recomp/     the toolkit and runtime, as a submodule (fork rigaz29/ps3recomp, branch gowa)
```

## Contributing

Anyone can help: by playing and reporting, or with changes to the port or to the runtime under it.
[CONTRIBUTING.md](CONTRIBUTING.md) says where help is most useful, where a change goes, and what must never be committed.

## Legal

No game files are in this repository, and none will be. You need your own copy of the game and its license, and you decrypt it
yourself.

What you build from your copy contains code translated from the game. Keep it on your machine: do not commit it or share it.

God of War is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by it.

## Credits

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet. The other code and libraries the port uses, among them
FFmpeg, AMD FSR, SMAA, zlib and Zstandard, are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

The code in this repository is under the MIT License, see [LICENSE](LICENSE). It does not cover the game or anything made from it.
