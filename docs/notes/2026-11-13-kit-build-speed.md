# The Kit build on more cores (2026-11-13)

- **Question:** does the build use every core? It did not. The compiles ran as many at a time as the cores and 1 GB of
  free memory each allowed: 4 of 12 threads on a 16 GB laptop with 6.7 GB free. The PPU lift's pool had a fixed 6
  workers, and the SPU lifts went one module, one piece at a time.
- **Where the time went** (0.1.0-alpha.8's Kit check, Ryzen 5 4600H, 12 threads): `lift_ppu` 94 s, `lift_spu` 92 s,
  `spu_programs` 14 s, `compile` 325 s (4 at a time, 1,300 s of compiler time), `link` 3 s. 8:50 in all.

## The PPU lift

- A profile of one lift (cProfile, the pool's workers not in it) puts the pool's part at 4%. The rest runs in one
  process: the lazy-flags analysis 39%, writing the definitions 25% (most of it the lazy-flags rewrite), the
  mid-function wrappers 15%, disassembly and boundary recovery 10%. More workers alone save a few seconds.
- `ppu_lazy_flags.py`: `line_flags` and `_lazy_line` pass over a line that names neither `ctx->cr` nor `ctx->xer`, with
  only the tail call to rewrite. Every form they match names one of them, so the answer is the same; 81% of the 2.5
  million lines this lift writes name neither. `tests/test_build_jobs.py` (CI) fails if a form comes that does not.
- The pool takes the cores, 128 MB of free memory each (`free_jobs`). A worker took 45 to 101 MB; the main process
  takes 1.5 GB, whatever the workers.
- The 98 files of a lift with 6 workers before the change and with 12 after it are the same bytes.

## The SPU lifts

- `lift_jobmods.py --engine-elf` and `lift_spu_pic.py` take `--jobs N`. Each module or piece is a `spu_lifter.py`
  process of its own, N at a time from a thread pool, the largest module first. Each has its own temporary ELF
  (`_tmp_<name>.elf`, `_piece_<name>.elf`) and `--insn-patches` file. What each prints is kept and printed in the old
  order, so the manifest, the registration TU and the log read as before.
- `gowa.py` gives them `free_jobs("auto", 256)`: a `spu_lifter.py` run took 244 MB at most (working set, 210 MB
  private). `test_lift_jobmods_verified.py` lifts its two modules again with `--jobs 2` and finds the same files.

## The compiles

- 384 MB of free memory each in place of 1 GB, 2 GB still left over. The most one took is 325 MB in the alpha.8
  check, 321 MB in this one (357 MB working set in the sampler below).

## Measured

One Kit build as the launcher runs it: the Kit's own Python, below-normal priority, AC power, 4.1 GB free at its start.
The DLL is `a58ee18b…`, the same file as 0.1.0-alpha.8's.

| phase | alpha.8 check | now |
|---|---|---|
| `lift_ppu` | 94 s, 6 workers | 72 s, 12 workers |
| `lift_spu` | 92 s, one at a time | 31 s, 7 at a time |
| `spu_programs` | 14 s | 14 s |
| `compile` | 325 s, 4 at a time | 317 s, 5 at a time |
| `link` | 3 s | 1 s |
| all | 8:50 | 7:16 |

- The compiles are no fair comparison. Free memory gave 5 at a time (other programs were open), and the compiler time
  rose from 1,300 s to 1,581 s, each compile 22% slower: the laptop's heat or other load, not the count. With 6.7 GB
  free, as in the alpha.8 check, it is 12 at a time.
- Memory, sampled every 500 ms: the build's processes held 1.6 GB at most at once; free memory went no lower than
  2.4 GB.
- `KitProgress.SECONDS` has the new lift times.

## Left

- The PPU lift's one-process part, about 60 s here: the lazy-flags analysis and rewrite of each function and the 5,479
  mid-function wrappers could go to the pool.
- The dev `build` (CMake, `-j 3`) and `package` are as they were.
