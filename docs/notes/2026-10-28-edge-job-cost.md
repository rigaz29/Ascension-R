# What an EDGE job costs on the host, before the work

The first level's frame waits for its EDGE job chain (the job manager's per-frame counter at
`0xA0B740`; [notes](2026-10-28-level-after-l7.md)), and while it waits the SPU workers are full
of EDGE runs. So the next speed comes from making an EDGE run cheaper. Measured on the level
(70 s, `PS3_SAMPLE_THREAD` with workloads and wait callers, `--spu-lines` build), after the
float and MFC lookup changes; target: 30 flips/s, now ~7.3.

## Where an EDGE run goes (workload 15; 14 and 16 are the same within a point)

| | share of the run |
|---|---|
| lifted SPU code | 69% |
| the runtime: dispatch (`spu_indirect_branch`, `spu_lifted_lookup*`, `spu_drain_call`) | ~9% |
| DMA copies (`mfc_do_transfer` and the `memcpy` under it) | 5.6% |
| coherency (`spu_lockline_lock` 2.6%, `spu_coh_notify_write` 2.6%) | 5.5% |
| other channel work (`spu_wrch`, `spu_overlay_note_get`, ...) | ~5% |

Inside the lifted code (share of the lifted samples):

| | |
|---|---|
| single-precision float: `fma` 7.5, `fm` 5.5, `fa`/`fs`, and the native forms' checks (`spu__x8_z`, `inrange`, `all4`, the store) | ~28% |
| `shufb` | 10.2% |
| LS reads 9.7, writes 5.3 | 15% |
| crossing between lifted functions: `SPU_DIRECT` 6.4, function entries 5.1, `spu_link` 2.5 | 14% |
| integer and the rest | ~33% |

## What the generated code shows

The hottest EDGE function (`jobmod_e8f44fbc6dbf_e6D98_spu_func_0000EC08`, 29 KB, 5028
instructions):

- **Every entry and every exit is a full Windows x64 frame.** 8 pushes, a 0x758-byte frame, the
  ten callee-saved registers xmm6-15 stored and loaded back, a `/GS` stack cookie set and
  checked, and constants broadcast into stack slots. `SPU_DIRECT` (a branch to another lifted
  function) is a tail call: the source's epilogue, a few global checks, the target's prologue.
  A `brsl` call is the same around a host call, plus `spu_drain_call` after it. That is what the
  14% (and audio's 11% on function entries alone) is.
- 27% of the instructions are stack accesses: the function is large, and its constants and
  values spill. An SPU register lives in `ctx->gpr` in memory between instructions, as the
  lifter writes it.
- An LS read is already short: the address from two words of `ctx->gpr`, one check of the
  diagnostic gate, a mask, `vmovdqu` + `vpshufb`, the result stored back.
- clang 23 has `preserve_none` on Windows x64: a function with it saves nothing, and a tail call
  between two of them is a `jmp` (checked on a small case). The registers are then saved once,
  where ordinary code calls into lifted code.

`spu_coh_notify_write` runs on every write to a line that was ever reserved (the bitmap bit is
never cleared, and the job manager reserves the same lines all the time) and walks the list of
every context registered for coherency (~100 persistent ones) to find the few holding a live
reservation. Same shape as the MFC lookup was. The main thread pays it too (3.1% of its
samples). `spu_lockline_lock` is one global spin lock for every SPU atomic and coherent PPU
store; its 2.6% is workers waiting on each other.

## Levers, in order

1. **`/GS-` for the lifted SPU sources.** The cookie on every entry and exit is not a property of
   the guest code. No risk; measure on its own.
2. **A list of the contexts holding a live reservation** for `spu_coh_notify_write` to walk,
   instead of every context. Kept under the lock-line lock where the reservation is set and
   consumed. Expected: most of the 2.6% here and the main thread's 3.1%.
3. **`preserve_none` for lifted SPU functions** and the runtime functions that tail-call into
   them (`spu_indirect_branch`, the dispatch chain), so a transfer between lifted functions
   saves nothing. A wide change (every pointer type to lifted code, the lifter's prototypes, the
   musttail chain), but clang rejects a mismatched pointer, so it is checkable. Expected: most of
   the 14% in EDGE, more in audio.
4. The native float forms: one combined test for "all operands ordinary, result in range", the
   zero/flush handling only behind it.
5. Then measure again; `shufb` and LS access are near what the current lifter output allows.

## How far this goes against the target

From the trace, the main thread's own work is about a third of a 137 ms frame (~45-48 ms); with
infinitely fast SPU code the frame would still be ~21 flips/s. So 30 needs the PPU side too (L8:
the main thread's lifted code). For both lifters the large lever is the same: guest registers
held in memory, with every instruction loading and storing them. Lever 3 here removes the
per-transfer cost of that design; keeping registers in host registers inside a function is the
bigger change behind it.
