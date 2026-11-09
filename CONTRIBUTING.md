# Contributing

Issues and pull requests are welcome. If you own the game, the most useful thing right now is to play it and report what
breaks: it has not been played through.

## Reporting a problem

Use **Report a problem** in the launcher and attach the zip to an
[issue](https://github.com/rigaz29/Ascension-R/issues/new?template=problem.yml). It says what the game was doing, and it hides
your paths and account name. The README says what goes in it.

## Where help is most useful

- Playing further than the first levels, and what goes wrong there.
- Pictures that differ from a PS3 or RPCS3.
- Speed. [docs/PERF.md](docs/PERF.md) has what has been measured and tried.
- Gaps in the runtime: a fix there helps every game on ps3recomp.
- The lifter test suites listed under `KNOWN_FAILING` in [the CI workflow](.github/workflows/ci.yml): each one is a bug to fix.

## Where a change goes

- **This repository**: the launcher, the patches and cheats, the hand-written routines in `native/` and the pipeline in
  `scripts/`. Open pull requests against `edge`.
- **`ps3recomp/`** is a submodule. The runtime and the lifters live in
  [rigaz29/ps3recomp](https://github.com/rigaz29/ps3recomp), branch `gowa`; send changes to them there. A fix that is not
  specific to this game is welcome upstream too, at [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp).

## Rules

`scripts/check_repo.py` enforces the first two on every push and pull request.

1. **No file from the game ever goes into git**: no SELF or ELF, encrypted or decrypted, no lifted code (`recompiled/`,
   `spu/`), no SPU images, archives, videos, saves or license files. No built executables or DLLs either. `.gitignore` covers
   them. Never `git add -f`.
2. **No keys, firmware or decryption tools in this repository.**
3. **GPL code is read, never copied**, RPCS3's included, so this repository stays MIT. MIT or BSD code may be copied, with its
   source, commit and license in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
4. Metadata is fine to commit: function addresses, NIDs, SHA-256 hashes, sizes.
5. Code, docs and commit messages are in English. A commit message says what changed and why.

## Testing a change

```
python scripts\check_repo.py
python tests\test_game_find.py
python tests\test_dxc_guards.py
```

For anything the game runs, measure it: `gowa.py bench` boots the game, can replay a session recorded by the launcher, and
writes frame rates and screenshots (`scripts/bench.py` lists the options). Say in the pull request what you ran and what
changed. New behaviour goes behind a setting or a build option until it has been tried in the game.

## Background

- [docs/ANALYSIS.md](docs/ANALYSIS.md): what the game is made of.
- [docs/PERF.md](docs/PERF.md): what has been measured.
- The development notes up to 2026-11-09, one per session, with the old plan and progress log, are at the tag
  [`notes-2026-11-09`](https://github.com/rigaz29/Ascension-R/tree/notes-2026-11-09/docs/notes).
