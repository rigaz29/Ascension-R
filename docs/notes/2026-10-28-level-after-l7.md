# The first level after L7: what bounds it, and the next lever

Re-measured on the first level (from 10 s after it starts) with the per-thread sampler, on the
build with SPU float on the host's RZ mode. Runs with the sampler on: flips/s are ~5% low.

## Tooling added for this

- Waits named by their caller: a sample outside the executable (ntdll, kernelbase, the CRT)
  also records the first return address into the executable's `.text` found on the thread's
  stack, taken while the thread is suspended, accepted only where the code before it is a
  `call`. `symbolize --callers` then lists such a sample as `(wait) <caller>`. Without the call
  check a function pointer kept in a frame (`spu_indirect_branch`, handed to
  `spu_run_with_halt`) took 1.7% of the workers' samples; with it, 0.1%.
- Offsets inside a function to source lines: the same translation unit compiled again with
  DWARF line tables (`/clang:-gdwarf /clang:-gline-tables-only /Gy-`, otherwise the build's
  flags) gives the same instructions (checked against the executable) with lines; the
  sampler's offsets map onto them. Used for `func_0044CA48` and the runtime's dispatch.

## The main thread waits for the SPU side

`ppu main` is at 93% CPU, but: 21% blocked, 13% in the progress-counter wait loop
(`func_0049D688`), and 36% in `func_0044CA48`, `func_0044CD20` and `func_0044D120`.

`func_0044CA48` is a job-graph scan: for each entry (0x70 bytes, count at `+0xA`) it reads a
counter and three dependency counters (`+0x1C/+0x20/+0x24`, each must be 0) and a flag, runs
the ready entries through an indirect call, and repeats while anything ran.
`func_0044CD20` calls it in a loop until the graph is done. Three quarters of its samples are
on the dependency reads of entries that are not ready (lines 10592-10612 and the loop's end),
none on the jobs it runs (those are other functions). So it polls. Making the main thread's
lifted code faster would not shorten the frame now (L8 waits).

## The SPU side

Policy-module workers (6 threads): 29% of their samples wait for work in `spurs_pm_worker`,
40% are lifted SPU code, 14% the runtime, 10% waits inside the runtime. Inside a run of audio
(w7, 85% of a core) or EDGE (w14-16, 63-67%):

| | audio w7 | EDGE w14 |
|---|---|---|
| lifted SPU code | 55% | 60% |
| `mfc_for`, own | 9.9% | 6.6% |
| waits called from `mfc_for` | 5.5% | 6.2% |
| `mfc_do_transfer` (the DMA copies, `memcpy`) | 4.7% | 3.8% |
| dispatch (`spu_indirect_branch`, `spu_lifted_lookup*`, `spu_lookup`, `spu_drain_call`) | ~12% | ~7% |

`mfc_for` finds the MFC engine of a context. It runs on every MFC channel access (several per
DMA: LSA, EA, size, tag, command, tag status). It takes a shared SRW lock and walks a linked
list of every context's engine; 91% of its own time is that walk (lines 494-495), and the
waits it causes are the lock's acquire and release (lines 493, 497). The list is long because
policy-module contexts persist, one per workload and simulated SPU (about 18 workloads here,
up to 6 SPUs each). Its share grows a little through the level (4.6 -> 6.1% of the workers).

The runtime already has the fix, off by default: `SPU_MFC_LOOKUP_CACHE=1` keeps the last
context's engine per host thread, dropped by a generation that every engine release bumps.
A worker stays on one context for a whole run (hundreds of MFC accesses), so it hits nearly
always. One run each, same build, sampler on:

| | cache off | cache on |
|---|---|---|
| `mfc_for` with its waits, share of the workers | 9.7% | 0.8-0.9% of audio and EDGE |
| audio w7 | 344 us/run | 283 (-18%) |
| EDGE w14-16 | 170-182 us/run | 140-150 (-17%) |
| w10, w12 | 228, 969 us/run | 145, 590 |
| policy modules, all | 431% of a core | 380% |
| level flips/s, 10-70 s in | 5.0-6.7 (median 5.4) | 5.9-7.5 (median 6.9) |

The level rendered as before. The cache's own test (`test_spu_mfc_lookup_cache.c`) is POSIX
only and cannot run on this host.

Smaller items, for after: the PIC pieces' first-dispatch verification is a `memcmp` of the
piece per GET (~1% of the workers, `spu_lifted_lookup_full` line 2114); coherency
(`spu_coh_notify_write`, `spu_lockline_lock`) 2.4%; the rest of dispatch ~7%; in lifted audio
code, LS reads and writes are 21% and function entries ~11%.

## Memory

The game's private bytes rise to ~1.9 GB as the level loads and stay at 1.92-1.97 GB from 70 s
into the level to the end of the run; no growth. The host had 4.8-5.1 GB physical memory free
while it ran: the background run that was stopped earlier was the system's pressure, not the
game's.

## Order

1. `SPU_MFC_LOOKUP_CACHE=1` as a port default; A/B on menu and level with only
   `GOWA_THREADS`, two runs each.
2. Profile again; then the dispatch path (`spu_indirect_branch`, `spu_lifted_lookup`,
   `spu_drain_call`) and the PIC verification.
3. Lifted code: LS access and function entries.

## Result: the lookup cache as the port default

`SPU_MFC_LOOKUP_CACHE=1` in `main.cpp`. A/B against `=0`, two runs each, alternated, only
`GOWA_THREADS`:

| | `=0` | `=1` |
|---|---|---|
| menu, from 25 s after the movie | 18.5-19.8 (median ~19.05) | 19.7-20.4 (median ~20.0), +5% |
| first level, 10-70 s in | 4.7-6.9 (median 6.26) | 5.7-8.0 (median 7.31), +17% |

At every matching point of the level the cached side was equal or higher. The menu gains less:
it is bound by workload 2 and the main thread's own work, not by DMA lookups.

Seen in both sides' level screenshots, so not from this: in the scene after the first
quick-time event the background behind the characters is black where a sky should be (with the
black patch above, for a comparison with RPCS3).
