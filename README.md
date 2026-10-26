# gowa-recomp

A static recompilation of **God of War: Ascension** (PS3, NPUA80918, version 1.12) to native
Windows, built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) (fork
[rigaz29/ps3recomp](https://github.com/rigaz29/ps3recomp), branch `gowa`).

It is at the very start: the executable has been analysed and the port's files are in place; it
does not build or boot yet. See [PROGRESS.md](PROGRESS.md).

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
..\.venv\Scripts\python scripts\gowa.py build
..\.venv\Scripts\python scripts\gowa.py run
```

## Layout

- `scripts/gowa.py`: the pipeline (doctor, setup, analyze, lift, build, run, symbolize).
- `main.cpp`, `launcher.cpp`, `stubs.cpp`, `CMakeLists.txt`: the port itself, carried over from the
  God of War HD port ([rigaz29/gow1hd-recomp](https://github.com/rigaz29/gow1hd-recomp)).
- `tools/hle_coverage.py`: which of the title's imports the runtime answers.
- `ps3recomp/`: the toolkit and runtime (submodule).

## License

MIT, see [LICENSE](LICENSE). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
God of War is a trademark of Sony Interactive Entertainment; this project is not affiliated with
it.
