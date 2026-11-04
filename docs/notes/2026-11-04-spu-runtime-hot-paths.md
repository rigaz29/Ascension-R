# The SPU runtime's hot paths inside EDGE jobs: map, changes, results (path B)

Written 2026-11-04. Path A (the C job, glue by glue) left the lifted glue at 12.7% of the EDGE
workers' time and the runtime's own machinery at 26.7%, so the runtime went first. This note is
the map (what each hot path does per job, where its time goes per instruction), the changes made
in the fork's `runtime/spu`, and what they gave.

## How it was measured

- **The sampler in the game**, gate, 75 s, `PS3_SAMPLE_THREAD=spurs pm`, default build with all
  natives (`logs/edge-ana/on.rvas`), and the same with the output native off (`off.rvas`).
  `symbolize --wid 14/15/16 --top 5000 --callers`, then grouped by category.
- **Per instruction:** each sample's address against the build's disassembly (`llvm-objdump`),
  so a function's time splits into the instructions it stalls on (no PDB is needed).
- **Per job, in the game:** a `--jobcap` build with `SPU_JOB_TIME=20` (the runtime's own
  counters per job: branches, lookups, DMA, atomics, the lock, channels, code residency),
  `SPU_IBFULL=20` (the full-path branches by target) and `SPU_COH_STATS=1` (the notify walk).
  `logs/edge-ana/map1`.

## Where the EDGE workers' time goes (all natives on)

| | share |
|---|---|
| native stages | 40.5% |
| lifted job glue and body | 12.7% |
| lifted job manager (wwspm) | 5.8% |
| native output stage | 1.9% |
| runtime, not the copies | 26.7% |
| DMA `memcpy` | 5.8% |

The runtime's 26.7%: dispatch 7.6 (`spu_indirect_branch_full` 2.3, `spu_lifted_lookup` 2.1,
the drain 1.7, `spu_lifted_lookup_full` 0.6), MFC/DMA 6.7 (`mfc_do_transfer` 2.2,
`spu_overlay_note_get` 1.8, `mfc_submit` 1.0, `mfc_for` 0.9, `mfc_channel_write` 0.5),
coherency and atomics 4.4 (`spu_lockline_lock` 1.8, `spu_coh_notify_write` 1.1,
`spu_coh_is_reserved` 0.8, `ppu_resv_break_line` 0.4), channels 3.1 (`spu_wrch` 1.7,
`spu_rdch` 1.3), waits outside the executable 2.7, the rest 2.2.

RenderJobs (w12) has the same share of runtime (~24%); General6 (w2) little (~4%, 91% lifted).

An EDGE job, per job, in the game: 60 indirect branches (8.05 on the full path: 0.40 special
addresses, 0.98 interrupt returns, ~6.7 misses), 70 lookups (5.8 missed), 21 drain loops;
GET 15.9 (12.4 KB), PUT 3.4 (14.5 KB), 2 lists (23.6 elements); GETLLAR 3.4, PUTLLC 1.14,
PUTLLUC 1.19, the lock-line lock 2.73 times (0.72 of them waited); rdch 30, wrch 51;
2 interrupts; 2.3 code spans ended by a GET and 2.3 new ones.

## What the per-instruction profile shows

The time is mostly memory latency, not work:
- **Scattered static switches.** `mfc_do_transfer`, `mfc_submit`, `spu_wrch` and `spu_rdch`
  each test 15-40 `getenv` switches (`static int s_x = -1; if (s_x < 0) s_x = getenv(..)`),
  each its own static in its own place. A DMA copy of 4-16 KB evicts them, so every transfer
  reloads a few dozen cache lines. The samples sit on those loads.
- **False sharing.** `g_spu_put_hook` (read on every PUT), `g_spu_get_observer` (every GET)
  and `g_spurs_kernel_poll` share one cache line with `g_spu_putllc_count`, which every PUTLLC
  of every SPU increments. Its only reader casts it to void.
- **`mfc_for` on every MFC channel.** `spu_wrch` looks the engine up for LSA, EAH, EAL, Size,
  TagID and WrTagMask too, which only store into the context; `spu_rdch` for every MFC read,
  none of which uses the engine. Six or more lookups a DMA, each a call, a TLS load and a cold
  static.

## What was changed

### The DMA path and the channels (items 2 and 3)

- **One switch for the DMA path's diagnostics.**
  - `spu_dma_diag()` (`spu_dma.h`) is 1 when any environment variable read by the optional
    blocks of `mfc_do_transfer`, `mfc_submit` and `mfc_channel_write` is set (or `SPU_LS_WATCH`
    arms a line, or `SPU_DMA_DIAG=1`), else 0. It is decided once and kept in a cache line of its
    own.
  - 29 blocks run only when it is 1, and then decide by their own switches as before.
  - Blocks that act with no variable set are as they were: validation, the malformed-transfer
    rejection, the committed-memory check, the copy, the observer, `spu_overlay_note_get`,
    coherency, the PUT hook, the repeat watchdog, the image-22 flag and the 368-byte descriptor
    note.
- **The channels.**
  - `spu_wrch` stores MFC_LSA, EAH, EAL, Size, TagID and WrTagMask into the context itself (what
    `mfc_channel_write` did with them); the rest goes on to `spu_wrch_full`.
  - `spu_rdch` reads the MFC channels and SPU_RdMachStat itself, after the stop check.
  - The engine (`mfc_for`) is looked up for MFC_Cmd, WrTagUpdate and WrListStallAck only, and no
    read uses it any more.
  - `spu_chan_diag()` gates SPU_CHHIST, SPU_POLLTRACE and SPU_WHOPOLLS (with its SPU_LSDUMP) in
    the same way.
- **False sharing.** `g_spu_putllc_count` and `g_spu_putllc_sync_hit` now count only while the PM
  flow trace's sync watch is armed. Every PUTLLC of every SPU wrote them, and they shared a cache
  line with `g_spu_put_hook`, `g_spu_get_observer` and `g_spurs_kernel_poll`.

### `spu_overlay_note_get` (item 1)

- **`resident_pages`.** It goes in the context's padding before `dbg_getllar`, so the context's
  size and the captures stay as they were. It holds the 8 KB pages that hold or held a resident
  span (a superset):
  - a GET into none of them skips the 16-slot loop;
  - a span's pages are added where it becomes resident (`spu_overlay_note_get`,
    `spu_pic_adopt`);
  - the replay rebuilds it after restoring a context.
- **What it can give is bounded.** By the captures, 25% of an EDGE job's GETs do touch a resident
  span: the code pieces it streams into the same buffer sets as its data. Only 39% fall in pages
  with none (52% at 4 KB, 62% at 1 KB, and the padding has no room for either).
- **A first version rebuilt the mask at every change** (4.6 a job), which cost what it saved. Now
  it only grows.

### The interrupt stub (item 4)

- **Two stubs at one address.** The WWS handler builds two stubs at 0x3FEC0 in turn from
  templates at LS 0xA40 and 0xAD0: 76 `stqa` then `bi $79` on entry, 76 `lqa` then `bi $79` on
  exit.
- **The memo.**
  - The full path remembers, per thread, the last two straight stubs it ran through
    `spu_smc_microstep`, with what its way there rested on.
  - When the fast path finds the same stub in the same state, it runs it from the list decoded
    then: the micro-step's loads and stores in order, byte for byte, then the branch.
  - Base-register forms (`lqd`/`stqd`), stores into the stub itself, or an armed local-store
    diagnostic go through the micro-step as before.
- **A first version kept one entry**, and it missed every time: save and restore came in turn.

### Not done

- **The lock-line lock.** 73% of `spu_lockline_lock`'s samples were its spin. The notify under
  it scans every slot's hint (85 slots; 0.14 holders on the written line), and those hints are
  written by other threads' GETLLARs. An exact per-line holder index has to stay right against the
  lock-free GETLLAR path, so it is a step of its own.
- **`spu_lifted_lookup`.** Its cost is mostly one load, the dispatch cache entry, cold after the
  DMA copies.
- **The full path's remaining branches.** These are:
  - the interrupt return;
  - kernel service 0x9D0;
  - four computed jumps into the EDGE module (0x62F4, 0x65EC, 0x6774, 0x6A54, ~0.8 a job). These
    are no function's entry, so the micro-step interprets them up to the next branch: most of
    its remaining 0.7%.

## Checked

- **Replay, state mode:** 820 jobs (the gate's, the consecutive ones, out1, out2, and 180
  captured with this runtime), 0 differences.
- **Strict mode:**
  - the gate and the consecutive jobs with the output native off: 0 differences;
  - out2 and the new captures with it on, as they were recorded: 0 differences.
  - The context's size is unchanged: the old captures load.
- **In the game:** a gate run with captures shows no new log lines (the same eight zero-size
  GETs rejected as before), and its captures replay with 0 differences.

## What it gave

- **Per EDGE job** (`SPU_JOB_TIME`, before the decoded run):
  - full-path branches 8.05 -> 4.10;
  - lookups 70.4 -> 62.5;
  - lock waits 0.74 -> 0.27 (pauses 41 -> 10).
  - `SPU_IBFULL`: 7.43 -> 4.27 million full-path branches in 20 s, and 0x3FEC0 is gone from the
    list.
- **The sampler, EDGE workers:** the runtime but the copies 26.7% -> 24.9% (by run share):

  | part | before | after |
  |---|---|---|
  | dispatch and interpreter | 7.9 | 7.4 |
  | MFC/DMA | 6.7 | 5.8 |
  | channels | 3.1 | 2.9 |
  | other runtime | 1.9 | 1.6 |
  | coherency | 4.4 | 4.4 |
  | `spu_overlay_note_get` | 1.8 | 1.9 |
- **Gate, ABBA, 2 runs each, the old runtime against the new:** 18.73 -> 19.19 flips/s (+2.5%).
  Runs 18.70 / 18.79 against 19.04 / 19.44: both new above both old. The first version (one memo
  entry, the mask rebuilt) had given 18.69 -> 18.93.
