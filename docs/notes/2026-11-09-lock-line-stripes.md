# A lock per line for the SPU atomics, and atomics without the channel write's frame (2026-11-09)

Two items taken from the MIT fork behind JonathanDC64/Soulbrandt. Both are on by default. They removed about 2 points of the SPU
workers' time, which is what they could remove. The frame rate did not visibly change.

## Symptom

`spu_lockline_lock` was 2.8-3.2% of the worker threads and 2.4-2.8% of the main thread. It was one spinlock for every PUTLLC, every
GETLLAR that could not skip it, and every PPU or DMA store into a reserved line.

A temporary probe, per call site, on the one lock:

| | taken a second | found it held | spinning |
|---|---|---|---|
| scripted fight | 756,474 | 18.55% | about 86 ms a second |
| route to the room | 573,447 | 14.59% | about 59 ms a second |

Callers in the fight: `spu_wrch_full` 52.8% (the atomics inlined in it), `vm_write32` 25.9%, `vm_write8` 7.9%, `ppu_stdcx64` 3.6%,
`spurs_kernel_poll` 3.0%.

## Fix

Fork ebc0c255.

**One lock per (line % 64)**, each on a cache line of its own (`spu_coherency.c`). What it serializes is per line: a PUTLLC's
compare and commit, a locked GETLLAR, a store into a reserved line and its notify.

- `spu_lockline_lock_ea(ea, len)` takes the stripes a byte range touches, in ascending order. `spu_lockline_lock` takes all of
  them in the same order, so nothing can deadlock. It stays for what is not about one line: a task's exit (`ts` and `run` are
  on different lines), the taskset bitsets, GCM's copy, and adding or removing a watch.
- A notifier now holds only its own line's stripe while it walks the holders, so the registry changed. A context is added under
  one more small lock with release stores, and removed holding every stripe, so no walker can be left with a pointer to it. A
  context that is listed with a slot of its own only stores its hint.
- Eight lines share a bitmap byte: `__atomic_fetch_or` in place of `|=`.
- The ordering argument for the lock-free GETLLAR is unchanged. It rests on the fences, and the fences come before the writer
  reads the registry.
- `cellSpurs.c`'s event flags and workload fields are all in the first 128 bytes of the structure: they take that one line.

`SPU_LL_STRIPES=1` puts every line back on one stripe.

**`spu_atomic_lean`** (`spu_channels.c`): GETLLAR and PUTLLC straight from `spu_wrch`, without `spu_wrch_full`'s frame (eight
pushes, two saved XMM registers, the security cookie) and the knob checks of its dispatch and of `spu_mfc_atomic`. The same
transactions in the same order. Any atomic diagnostic set sends it the long way. `SPU_ATOM_LEAN=0` turns it off.

Their second idea is not here: deciding a PUTLLC that writes nothing without the lock. It pays in their game, whose ring consumer
issues about 5 million a second. Of 45 million PUTLLCs in a fight here, 5.0% wrote nothing.

## Measured

Eight runs of the scripted fight with `spurs pm` sampled, 50 s of each. Share of the workers' samples:

| | runs | `spu_lockline_*` |
|---|---|---|
| one lock | 2 | 3.2%, 3.2% |
| 64 stripes | 2 | 1.5%, 1.5% |

| | runs | `spu_wrch_full` | `spu_atomic_lean` | `spu_channels.c` |
|---|---|---|---|---|
| lean path off | 2 | 1.6%, 1.6% | 0.2%, 0.2% | 16.4%, 16.5% |
| lean path on | 2 | 0.8%, 0.8% | 0.7%, 0.7% | 15.9%, 15.8% |

What is left of the lock was not split further; the exchange itself and waits on lines every worker uses are all it can be. The lean
path's gain is 0.3 to 0.6 of a point.

Median flips/s of those eight runs: 36.1, 32.8 and 30.3, 32.7 (first table); 27.7, 32.6 and 33.5, 28.5 (second). That is the
scene-to-scene noise of this fight. About 2% of worker time cannot be seen in it, and on a frame that also waits it would be less
than that in fps.

## Verification

- `test_spu_coherency` 211 passed, with a new case for what each lock call holds, every case on the stripes. `test_spu_lockline_peer`
  76 passed in all three settings (defaults, `SPU_ATOM_LEAN=0`, `SPU_LL_STRIPES=1`). Both built with clang-cl; the second on a small
  pthread shim. `test_spu_lockline_wait` needs `clock_gettime` and was not run here.
- A threaded stress on the real `spu_coherency.c`, kept out of the tree, 5 s a part:
  increments through GETLLAR/PUTLLC with coherent stores beside them and transient contexts unmapped on exit (25-26 million, none lost,
  against 12.5-13.7 million on one lock); a store racing a GETLLAR, on separate stripes and on one (about 90 million rounds, every one
  either saw the store or lost the reservation with the event up); plain counters across single lines, ranges and the whole lock
  (128 million, none lost, four times one lock's rate).
- In the game, defaults: the route to the room, the scripted fight for 530 s (after about 240 s it stands on a tutorial prompt
  at 34.7 fps), and 130 s from the autosave before the CYN050 cutscene. Nine more runs of the fight, sampled, are the
  tables above and the count of no-op PUTLLCs. No hang, no `[spu-spin]`, the right picture.

Not tested: a full playthrough. If something hangs, try `SPU_LL_STRIPES=1` and then `SPU_ATOM_LEAN=0`.

## What is left on the workers

Scripted fight, share of samples: lifted SPU 39.4%, waits and the system DLLs 24.2% (15.9% under `spurs_pm_worker`, 5.0% under
`mfc_do_transfer`), the native SPU routines about 9%, `spu_channels.c` 16.2%, `spu_coherency.c` 3.7%. Inside `spu_channels.c` the larger
parts are the indirect branch lookups (`spu_indirect_branch`, `spu_lifted_lookup`, `spu_overlay_note_get`, about 5.6%) and plain DMA
(`mfc_do_transfer`, `mfc_submit`, `mfc_for`, about 4.3%). Those are the fork's other two items.
