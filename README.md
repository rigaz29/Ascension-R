# Ascension-R

A native Windows port of **God of War: Ascension** (PS3, NPUA80918, version 1.12), made by static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp) (fork [rigaz29/ps3recomp](https://github.com/rigaz29/ps3recomp), branch `gowa`).

**No game files are in this repository, and none will be.** You need your own copy of the game, installed in RPCS3 with its
license, and you decrypt it yourself.

## Status

It builds, boots, plays the cinematics and goes through the menus to the levels. The two scenes measured so far, a fight of
the first level and the gate, run above 30 frames a second on a Ryzen 5 4600H with a GTX 1650 Ti, with the Smooth preset. It
has not been played through. Only the digital US release, version 1.12, is supported, and only on Windows.

There is no download yet: for now you build it yourself. Help is welcome, see [Contributing](#contributing).

## What you need

- Windows 10 or 11, 64-bit. It has been tested on Windows 11.
- A CPU with AVX2 and FMA, and a GPU with Direct3D 12.
- The game, installed in RPCS3 with its license.
- To build: Python 3.10 or later, Git, CMake, Ninja, Visual Studio with "C++ Clang Compiler for Windows", a Windows SDK, and
  about 20 GB free.
- For the game's movies: an LGPL shared build of FFmpeg with its headers, unpacked under `..\ffmpeg`, for example
  `win64-lgpl-shared` from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases).
- Optional, for AMD FSR 3.1: [FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), unpacked under
  `..\fsr-sdk`.

## Build

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

`doctor` lists anything that is missing. The lift and the first build take about 13 minutes on a Ryzen 5 4600H. `run` starts
the game straight away, with its log in `logs\`. To play, open
`build\Ascension-R.exe`: it shows the launcher. [docs/FEATURES.md](docs/FEATURES.md) describes the launcher's options: shaders,
FSR 3.1, frame generation, internal resolution, cheats, logs and recordings.

## Reporting a problem

**Report a problem**, down the launcher's sidebar, makes a zip in `logs\` beside the settings file, for you to attach to an
[issue](https://github.com/rigaz29/Ascension-R/issues/new?template=problem.yml). The launcher offers it by itself after a session
that crashed, lost the graphics device, froze or stopped with no reason in its log. `Ascension-R --report [--with-saves]` makes
one without the window.

The zip holds the logs and pad recordings of the last three sessions the game ran in, your settings, the build, your hardware and
driver versions, and Windows' records of crashes and hangs of this exe in the last two weeks. Folder paths and your Windows account
and PC names are hidden. The game sends nothing by itself and no game file goes in. Saves go in only if you tick the box, without
the game's images and sounds.

If no new frame comes for 15 seconds, the log gets what each thread was doing, and again at 60 (`GOWA_FREEZE_WATCH=<seconds>`, `0` = off).

## Contributing

Anyone can help: by playing and reporting, or with changes to the port or to the runtime under it.
[CONTRIBUTING.md](CONTRIBUTING.md) says where help is most useful, where a change goes, and what must never be committed.

## Layout

- `scripts/gowa.py`: the pipeline (doctor, setup, analyze, lift, build, run, symbolize).
- `main.cpp`, `launcher.cpp`, `stubs.cpp`, `CMakeLists.txt`: the port itself.
- `native/`: hand-written routines for the SPU code that costs the most.
- `tools/hle_coverage.py`: which of the game's imports the runtime answers.
- `docs/`: what the game is made of, what has been measured, what the launcher offers.
- `ps3recomp/`: the toolkit and runtime (submodule).

## Related work

Two other ports of God of War: Ascension on ps3recomp, of the disc releases, are further along and
were a great help reading this game's frame pipeline:
[MaChInEgUn3/God-of-War-Ascension-Recompiled](https://github.com/MaChInEgUn3/God-of-War-Ascension-Recompiled)
and [arnaldolima1/gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc).

## License

MIT, see [LICENSE](LICENSE). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
God of War is a trademark of Sony Interactive Entertainment; this project is not affiliated with or endorsed by it.
