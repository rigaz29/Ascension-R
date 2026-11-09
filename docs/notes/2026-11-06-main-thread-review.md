# The main thread, reviewed

2026-11-06. What sets the fight, what the main thread spends its time on, what the lifted PPU code
paid for each guest instruction, and what came of it.

**In short**
- The fight is bound by the main thread: 25% more of its work costs 14% of the frames, and 25% more
  of anything else costs nothing.
- Its time is 72% lifted PPU code in a flat profile, so no single function is worth a native port.
  What pays is the code every function shares: the load and store helpers, and what follows a call.
- Those came down in two rounds, `PPU_MM_LEAN`: the executable's `.text` 111.9 -> 93.1 MB.

| two runs each, one executable name | warm fight | gate |
|---|---|---|
| before | 22.85, 23.15 | 25.05, 25.84 |
| first round | 23.84, 24.43 | 26.05, 26.08 |
| second round | 24.79, 24.89 | 26.66, 26.32 |

So +8% in the fight and +4% at the gate (`logs/edge-ana/tw1`, `tw2`, `twg`). Job outputs are identical
on the three replay lists, and the gate's colours are the same.

The next step is lazy CR and XER: about 13% fewer instructions in the lifted code, see the end.

## What sets the fight

Each part 25% slower, the warm fight, two runs each (`logs/edge-ana/limf`):

| +25% on | flips/s | |
|---|---|---|
| nothing | 23.05, 22.81 | |
| the main thread | 19.78, 19.58 | -14.2% |
| EDGE | 22.72, 23.15 | 0 |
| the frame clock thread | 23.05, 22.72 | 0 |
| General6 | 22.55, 23.15 | 0 |
| RenderJobs | 22.95, 23.54 | 0 |
| RenJobSet | 22.91, 22.42 | 0 |

The main thread was at 94% of a core, the workers at ~505% of 600%, the frame clock thread at 62%.

The gate is different. There the frame clock thread is now at 94%, ~35.7 ms a flip, full near 28
flips/s. It sat out of the chain at 25.5 ([the noise review](2026-11-05-general6-noise-review.md)); at
26.4 it may not, so the render thread (fork branch `perf/render-thread-2`) is worth another run.

## What the main thread does

38,916 samples in the fight, before the helpers changed (`logs/edge-ana/m1f.rvas`, map `m1.map`):

| | share |
|---|---|
| lifted PPU code | 72.5% |
| of it the hottest function, `func_002BEA7C` | 1.6% |
| the next, `func_002EB798` | 1.0% |
| every other function | at most 0.7% |
| `poll_mwaitx`: waiting | 10.2% |
| `ps3_indirect_call_impl` and `ps3_indirect_call` | 4.2% |
| stores to reserved lines: `spu_coh_notify_write`, `spu_lockline_lock`, `ppu_resv_break_line` | 3.7% |
| other waits: `func_001C110C`, `cellSpursReadyCountStore`, `cellPadGetData`, `sys_timer_usleep` | 5.0% |

Its own work is ~31 ms a frame. Nothing stands out, so per-function work does not pay here.

## What a guest access cost

The helpers in `ppu_recomp.h`, as `func_002EB798` compiled:

| | before | after |
|---|---|---|
| function entry | ~20 instructions building the memory model's copy | 2 loads |
| store | ~17: two diagnostic globals, a NULL test, the line's bit, an alignment branch | 12, the address's own included |
| 32-bit load | range and poll word against registers, an alignment branch | 8, both against immediates |
| `lvx`, `stvx` | two 8-byte accesses, each with an alignment branch, `vm_base` reloaded after any store | one 16-byte access |
| after each guest call | thread-local address, TLS guard, test | one compare in `ctx` |
| an indirect call | a dozen and more loads and branches for diagnostics | the target's lookup |

### First round (`ee1cf003`)

- The copy of the memory model is made once, by `ppu_mm_fix`.
- A store no longer reads `g_barrier_sync_watch` and `g_null_sweep_hi`.
- One instruction at any alignment: `*(volatile uint32_t __attribute__((aligned(1)))*)`. An aligned
  access is as atomic as before.

### Second round (`53745909`)

- The inline range and the ref-poll word are constants, which `ppu_mm_fix` checks. One compare against
  an immediate at any size; the range's last 16 bytes go out of line.
- The read and store diagnostics (`PPU_RWATCH`, `PPU_WWATCH`, ...) cannot narrow the range then. They
  need `-DPORT_PPU_MM_LEAN=OFF`, and `ppu_mm_fix` says so if one is armed.
- A store reads the reserved-line bitmap a byte at a time, 8 lines or 1 KB of guest memory, and looks
  for the line's bit only if that byte is not 0. `spu_coh_bits_publish` gives the bitmap before the
  guest runs, so nothing tests for NULL.
- A lifted tail call goes through `ctx->tramp`, not the thread-local `g_trampoline_fn`.
- `ps3_indirect_call` goes straight to its target while no indirect-call diagnostic is armed.
- `/Zc:tlsGuards-`. Every thread-local access in the C++ code carried a load of `__tls_guard`, a branch
  and a cold call to `__dyn_tls_on_demand_init`, because the variable might need dynamic set-up. In an
  executable the CRT sets them up as each thread starts.

## Tried, left off

### Profile-guided build (`PORT_PPU_PGO`)

Instrumented run at the gate and in the fight (20 and 17 flips/s), then a build with the profile:
`.text` 107.3 -> 89.4 MB, most of it cold code built for size.

- Warm fight 24.10 -> 24.33, gate 26.02 -> 26.40 by windows and no change in the steady rows.
- `build-pgo/gowa.ini` had the defaults, no FSR 1 or SMAA, so even that 1% is not all PGO's.
- A profile has to be remade after any change to the lift. The option stays, off.

### `__restrict` on the context (`PORT_PPU_RESTRICT`)

`__restrict` on the context, `noescape` on everything it is passed to, and the capture limit raised.
The reloads after a store go (`func_002EB798`: 97 accesses to `ctx` -> 72), but the instruction count
stays: the values move to stack slots. `.text` +1.4%.

| two runs each | plain | restrict |
|---|---|---|
| warm fight | 25.09, 24.73 | 25.32, 25.15 |
| gate | 26.46, 26.45 | 26.73, 26.90 |

- About +1.3%, the same way in both scenes, and the gate's colours are the same.
- `noescape` is not strictly true: `ps3_indirect_call` keeps `ctx` in `g_active_ctx`. It holds because
  only diagnostics read a context through that from a call that does not take it.
- `ppu_stwcx32` does read `reserve_addr` that way, and other threads write it, so the lifted code's
  accesses to it are volatile now, with the option on or off.
- The option stays, off. It is worth another run after lazy CR, when more can stay in registers.

**Why the SPU test of 2026-11-01 found nothing:** LLVM's capture tracking stops at 100 uses of a
pointer (`-capture-tracking-max-uses-to-explore`) and calls it captured, and any lifted function of
size has more uses of `ctx` than that. So `noalias` did nothing there. The bit-identical output of
`func_002EB798` with and without `__restrict` showed it. It takes that flag raised, and `noescape` on
everything `ctx` is passed to.

### What else was looked at

- The VMX float helpers: already vector code.
- `-march=x86-64-v3`: 2.4% fewer instructions in one file, all from `movbe`. Not measured.

## A trap in the bench: the executable's name

The same binary ran slower as `build/m1.exe` than as `GowARecomp.exe` in another folder.

| same binary, warm fight | flips/s |
|---|---|
| as `m1.exe` | 23.64, 23.57, 23.64, 23.87, 23.70 |
| as `GowARecomp.exe` | 23.94, 24.04, 24.33, 24.00, 24.10, 24.10, 24.49, 23.84, 24.43 |

- That is ~2%, in the fight. The gate does not show it (26.09 against 26.02).
- Why is not known. No per-program GPU preference, game-mode entry, compatibility layer or image
  option names `GowARecomp`, and Defender's exclusions cannot be read without admin.
- A baseline goes in a folder of its own under the same name, with a copy of `build/gowa.ini`: the file
  `gowa.py build` writes for a new tree has only the paths, so it runs without FSR 1 and SMAA.
- The second round first read +6.6% in the fight because of this. It is +2.9%.
- Earlier comparisons with a renamed baseline carry the same bias: the hot-first link order (22.91
  against 23.52) is the known one, and it stays a no.

## What is left

### Lazy CR and XER

**Done** (`ps3recomp/tools/ppu_lazy_flags.py`, `gowa.py lift --lazy-flags`, on by default). CR fields 0, 1,
5, 6, 7 and XER[CA] live in function locals, CR2-4 stay in `ctx`. They are dead at calls, returns and
indirect tail calls; a direct tail call flushes what its target reads before writing. `--lazy-flags check`
keeps the exact accesses and compares at every read: no difference.

| | before | after |
|---|---|---|
| `.text` | 93.1 MB | 84.7 MB |
| `ppu_recomp_047.cpp`, instructions | 196,923 | 167,685 |
| gate, flips/s | 26.41 | 26.77 |
| fight, flips/s | 24.56 | 24.90 |

+1.4% in both scenes, against the 3-5% estimated below. The workers are nearly full in the fight
(pm 556-565%), so a faster main thread has less to win than its own share suggests.

What follows is the review as written.

In `ppu_recomp_047.cpp`, 22.5% of the statements touch `ctx->cr` or `ctx->xer`: ~1,900 compares, ~1,900
conditional branches and ~2,000 carry updates.

- A compare is a read-modify-write of `ctx->cr`, ~12 instructions, and reads `ctx->xer` for SO.
- `addic`, which this compiler uses for plain address arithmetic, is a read-modify-write of `ctx->xer`.
- None of that can go while `ctx` is shared with every callee.

A text transform of that one file, for size only (`cr_size.py`):

| | instructions | |
|---|---|---|
| now | 196,923 | |
| CA writes gone where nothing reads CA, SO not read | 185,524 | -5.8% |
| CR0, 1, 5, 6 and 7 in locals | 180,302 | -8.4% |
| both | 171,083 | -13.1% |

Bit-level liveness over all 22,320 lifted functions, from their text:

| | |
|---|---|
| CA writes | 158,292 |
| of them never read | 157,352 (99.4%) |
| functions reading a volatile CR bit before writing it | 182 |
| functions reading one after a call with no write between | 25 (152 calls) |
| functions reading CA before writing it | 62 |
| calls with CA read after them | 346 |

- The PPC ABI makes CR0, 1, 5, 6, 7 and CA volatile across calls, and the code keeps to that in
  99% of its functions.
- The exceptions look like function boundaries that cut a compare from its branch, such as
  `func_00012CA0`, which opens on a `bge`. They keep today's exact code, and a fragment exit flushes.
- `mfcr` is 1,314 sites; the volatile fields it saves are never restored, but it has to see the
  non-volatile ones.

So the lifter would run this liveness per function, keep volatile CR bits in locals, write `ctx->cr`
only for what a call, return or fragment exit can see, and drop dead CA writes. SO can go if nothing
in the binary sets it.

### Smaller ones

- `PORT_SPU_RESTRICT` with the capture limit raised, on the replay: the test of 2026-11-01 never took
  effect.
- `PORT_PPU_FAST_FP`, not tried on this title yet.
- r1-relative accesses without the range compare: 3 of a stack store's 12 instructions.
- The coherency byte as `cmpb`, not `movzbl` and `testl`: 1 instruction a store.
- Stores to lines once reserved, 3.7% of the thread. Marks are never cleared, so each pays a lock, a
  fence and a walk whether or not an SPU holds the line now. It needs care: the fence is what
  `spu_coh_reserve_fast` rests on.

### A rare hang

`[spu-dma] ... 256 identical transfers ... halting the SPU` at level load, 30 s into a stall, came up
again in `logs/edge-ana/nm2`: 5 of some 600 logs since 2026-11-02, on the old code as well. Not
investigated.
