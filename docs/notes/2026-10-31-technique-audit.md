# Optimization checklist against the code: what is done, what is worth trying

Written 2026-10-31 at 16.26 flips/s on the first level (fork `df08790d`, port `72ef5c3`).

A list of the usual PS3 recompilation optimizations was checked item by item against the code.
It had three parts: general PPU/SPU/LV2/RSX techniques, techniques only a static recompiler can
use, and notes for the Ryzen 5 4600H. For each item the code was read. Where the code alone
could not answer, a small clang probe was run. Each item gets a status and a verdict.

The verdict follows what bounds the frame now (performance plan 2.8). That is not SPU
throughput: the workers are 4.65 of 6 busy. It is the chains of job lists. Each workload made
25% longer (`SPURS_WID_SLOW`), and the main thread's own work made 25% longer
(`GOWA_MAIN_SLOW`), cost:

| made 25% longer | flips/s change |
|---|---|
| EDGE w14-16 | -7.0% |
| RenJobSet w8-11 | -3.3% |
| main thread's own work (against its own base, 16.34) | -2.2% |
| RenderJobs w12 | -1.9% |
| General6 w2, audio w7 | none |

The frame clock thread has not been probed this way yet (finding F4).

Status words: **done**, **partial**, **not done**, **n/a** (does not apply to this title or
this port). Verdicts: **try** (with the first measurement), **later** (only after something
else), **no** (with the reason).

## 1. New findings

### F1. The PPU's unaligned vector loads and `vperm` are byte loops

The lifter (`ps3recomp/tools/ppu_lifter.py`) emits these as C loops over bytes:
- `lvlx` and `lvlxl` (`:3011`);
- `lvrx` and `lvrxl` (`:3021`);
- `lvsl` and `lvsr` (`:3055`);
- `vperm` (`:3070`).

Counts in the 200 lifted files:

| instruction | sites |
|---|---|
| `lvlx` | 13,610 |
| `vperm` | 12,711 |
| `lvrx` | 506 |
| `lvsl` / `lvsr` | 196 |
| `stvlx` / `stvrx` | 63 |

The same loops were compiled with the port's flags (`-O2 -fno-strict-aliasing -fwrapv -mfma`).
clang unrolls them fully:
- `vperm`: 119 x86 instructions;
- `lvlx`: 85 x86 instructions;
- no `pshufb` in either.

With SSSE3 the same work is about 5-8 instructions:
- `vperm`: two `pshufb` and a blend on bit 4 of the control byte;
- `lvlx`: one aligned load and a `pshufb` with a mask taken from a 32-byte sliding table.

The vector registers are kept in guest byte order (the loops index bytes directly), so the
control bytes need no remapping.

The remaining VMX helpers in `ps3recomp/runtime/ppu/ppu_vmx.h` still run one lane at a time
(`VMX_FUN`, `VMX_CMP`, `vmx_vcfsx`):

| helper | sites |
|---|---|
| `vcfsx` | 3,705 |
| `vcmpgtfp` | 2,963 |
| `vrsqrtefp` | 1,917 |
| `vrefp` | 876 |
| `vcmpequw` | 800 |
| `vslw` | 577 |
| `vslb` | 542 |
| `vmaxfp` | 411 |
| `vcmpgefp` | 387 |
| `vminfp` | 303 |
| `vctsxs` | 210 |

- `vrefp` and `vrsqrtefp` are computed as `1.0f / f` and `1.0f / sqrtf(f)`. `_mm_div_ps` and
  `_mm_sqrt_ps` give the same results four lanes at a time: each step is correctly rounded in
  both forms.
- `rcpps` and `rsqrtps` would not give the same results. They have 12 bits of precision and
  their own rounding, so they belong behind an option only.

Why this ranks high:
- These loops are inline, so the function-level sampler (`PS3_SAMPLE_THREAD`, `symbolize`)
  books them to the lifted function that holds them. They have never shown up under their own
  name.
- The last VMX step proved the lever. `vaddfp`, `vsubfp`, `vmaddfp` and `vnmsubfp` as vector
  code (fork `b5a91584`) took the level from 15.68 to 16.14 flips/s (+2.9%).

### F2. Guest registers are reloaded after every guest store

- Lifted functions keep every guest register in `ctx` (`ctx->gpr`, `cr`, `xer`, `vr`), and
  guest memory is reached through `vm_base`.
- With `-fno-strict-aliasing`, clang cannot tell a store into guest memory from a store into
  `ctx`. So after each guest store it reloads every register it reads from `ctx`.
- A small copy of the lifted pattern (read32, write32, add, write32, multiply) shows it. The
  context is accessed 9 times without `__restrict`, 5 with it:
  - without: `gpr[3]` and `gpr[5]` are read back from `ctx` after the store, although they are
    still in host registers;
  - with: the first reads and the last writes only.
- The SPU lifted code has the same problem. Its functions take `spu_context* ctx` (no
  `restrict`). LS stores go through `ctx->ls`, which for a SPURS context points at the
  context's own `ls_store` array (`spu_context.h:197`, `:493`). So each LS store also reloads
  `ctx->ls` itself.

`__restrict` on the lifted functions' `ctx` parameter is a one-line lifter change. It is not
free of risk, because restrict lets the compiler move accesses to `ctx` across calls that do
not take `ctx`:
- **`ppu_stwcx32`** (`ppu_loader.cpp:1215`) is called from lifted code without `ctx`. It
  reads `reserve_addr` through `g_active_ctx`. The lifted `lwarx` writes `ctx->reserve_addr`
  just before, so with restrict the compiler may sink that store past the call. The fix is to
  pass `ctx` to the store-conditional helpers (or keep the reservation fields out of the
  restrict context).
- **`ppu_resv_break_line`** (`:1155`) clears `reserve_addr` of other threads' contexts while
  they run. It already races with plain loads. The lifted `stwcx.` must keep reading that field
  through the helper, not from a value the compiler kept.
- **The diagnostics** that read `g_active_ctx` (`PPU_WWATCH`, the null-page sweep, the call
  traces) may see register values the function has not written back yet. That is acceptable
  for diagnostics, but it should be documented.
- **SPU:** other threads write SPU context fields (event status from the coherency notifies,
  mailboxes, `stop_request`). Lifted SPU code reads those only through `spu_rdch` and other
  calls that take `ctx`. Check this first: grep the runtime for SPU context fields read by
  inline helpers.

Expected gain: unknown. Each reload is an L1 hit, but it adds a store-to-load step to every
dependency chain that crosses a store.
- On the SPU side, putting registers in locals helped General6's straight-line `4F00` only once
  the body had no calls left (plan 2.1). Restrict is the lighter form: the compiler decides.
- The replay measures it in minutes.

### F3. Build flags for this CPU

- The lifted PPU code is built with `/O2` plus `-mfma` only (`CMakeLists.txt`, `recomp_opts`).
  That brings AVX but not AVX2, BMI1/BMI2, LZCNT or MOVBE.
- The lifted SPU code gets `-O2 -msse4.1 -mavx2 -mfma` (`spu_opts`).
- No `-march` or `-mtune` anywhere, and no LTO.
- The port already requires AVX2 (`PORT_SPU_AVX2` is on by default), so `-march=x86-64-v3`
  (AVX2, BMI1/2, FMA, LZCNT, MOVBE) costs no portability.
- `-mtune=znver2` only changes scheduling.
- What it would change:
  - `cntlzw` / `clz` as `lzcnt` instead of `bsr` plus a zero fix-up;
  - variable shifts as `shlx` / `shrx` / `sarx` without moving the count to `cl`;
  - `andn`.
- What it would not change: **MOVBE**. With `-march=znver2`, clang still emits `mov` plus
  `bswap` for the relaxed atomic loads and stores the memory model uses (`vm_read32_mm` & co.).
  On Zen 2, `bswap r32` is one micro-op anyway.
- **PDEP/PEXT**, which Zen 2 runs in microcode, are used nowhere in the runtime, libs or native
  code. **Gathers** are avoided on purpose (the LUT helpers in `spu_helpers.h` say why).

Expected gain: small, 0-1%. It is cheap to try: one PPU rebuild (~8 min).

### F4. The frame clock thread may be the next limit

- `bench-nat3` (16.34 flips/s, a 61 ms frame): the thread used 60.5% of a core, about 37 ms of
  CPU a frame.
- `rf_open` (`rsx_d3d12_backend.c:8974`) still calls `wait_for_gpu()` at the start of every
  batch. That wait was 20-25 ms a frame when it was measured on 2026-10-29 (plan 2.4).
- CPU plus that wait is roughly the whole frame. The walker stands still during the wait, and
  the reports, labels and parks the SPU jobs wait for stand still with it.
- Plan 1.4 (rings instead of the full drain) and 1.5 (descriptors) are still open. No slowdown
  probe has been run on this thread, so it is not known whether it sets the frame.
- The ZPASS reports (fork `5c906fb2`) are read in `rf_open` after that drain (`zq_read`).
  Removing the drain means delivering them when their batch's fence completes.

### F5. PPU indirect calls: a target cache, and a dozen diagnostic checks first

- `ps3_indirect_call` (`ppu_loader.cpp:2794`) and its `_impl` test over a dozen environment
  gates in the hot path before the lookup. Each is a function-static loaded and compared:
  `PPU_CSCHECK`, `PS3_ICALL_GATE`, `BCTRL_RING`, `PS3_BREADCRUMB`, `PPU_TOCWATCH`, the
  `PS3_CALLTRACE*` family, `FLOW_FORCE_RTCFG`, `SPURS_TRACE`, `PS3_PRX_CALLTRACE`, `PPU_TOCFIX`,
  `PPU_RETWATCH` and others.
- The call also stores `g_active_ctx`, and the vcall stack and depth counter.
- The lookup has a per-thread 512-entry target cache (`ppu_lookup_icall`, `:2398`). There is no
  per-call-site cache.
- The SPU side solved the same problem with a short path for the common branch (fork
  `6a2015f4`, +3.0%).
- The main thread makes about one indirect call per microsecond of its own code (the rate
  `GOWA_MAIN_SLOW` charges at), and its own work moves the frame by ~0.09% per 1%. Expected
  gain: under 0.5%. It only removes work.

### F6. A native replacement of a guest function only reaches indirect calls

- `ppu_register_function(addr, fn)` replaces the hash-table entry (see `YDKJ_MEMFIX` at
  `ppu_loader.cpp:2458`). Direct `bl` calls are C calls from one lifted function to another and
  never look at the table.
- GoW:A imports no libc (`analysis/GOWA.imports.json` has no `sys_libc`), so its `memcpy`,
  `memset` and math are static guest code.
- Replacing them with host versions needs a lifter-level list: emit the call to the native
  function at every direct call site too.

## 2. The checklist

### A. PPU code generation

| # | item | status | evidence | verdict |
|---|---|---|---|---|
| 1 | Guest registers in host registers | not done | every access is `ctx->...`; only callee-saved restores read an entry copy (`_cs_N`, `ppu_lifter.py:1392`) | **try** F2 (`__restrict`) first; locals later (plan 2.3), main thread's work moves the frame little |
| 2 | Lazy CR and XER | not done | `cmp` writes `ctx->cr` read-modify-write; `addic` always computes CA into `ctx->xer` | comes with #1: with restrict or locals the compiler drops the dead updates |
| 3 | 4 GB reservation, base + ea | done | `vm_base + ea` after one range compare (`ppu_mm`, `PPU_MM_SNAP`); stores also check the barrier watch and the SPU coherency bitmap | - |
| 4 | MOVBE, no double swaps | n/a | clang keeps `mov` + `bswap` for relaxed atomics even with `-march=znver2`; `bswap` is one micro-op | no |
| 5 | Direct calls, native return | done | `bl` lifts to a C call, `blr` to a return; `DRAIN_TRAMPOLINE` after each call (a TLS load and a test) | - |
| 6 | Indirect branch cache, devirtualisation | partial | per-thread 512-entry target cache; jump tables lift to `switch` (79 in all); over a dozen diagnostic gates first | **try** F5 |
| 7 | `lwarx`/`stwcx.` as host atomics | partial | `stwcx.` is a CAS helper (`ppu_stwcx32`) behind a lock per line; idiom loops are not turned into `lock xadd` | no: few atomics on the hot path (2 in a sampled 14K-line file) |
| 8 | FMA3, FPSCR, NJ, `vperm`, `vrefp` | partial | `fma()` is `vfmadd` (`-mfma`); FPSCR not tracked per op; NJ flush implemented; 4 VMX float ops vectorized | **try** F1 (`vperm`, `lvlx`, the rest of VMX) |
| 9 | Spin loops | done | four main-thread polling loops wait with MWAITX (`poll_wait.cpp`, +1.75%) | later: wake on the polled line (MONITORX on it) |
| 10 | -O3, LTO, PGO | partial | `/O2`; no LTO; PGO wired for SPU code only (`PORT_SPU_PGO`, no gain) | **try** F3 `-march`; PPU PGO later; LTO no (200 large TUs, little cross-TU work) |

### B. SPU code generation

| # | item | status | evidence | verdict |
|---|---|---|---|---|
| 11 | Registers in locals | partial | speculative locals for float-heavy straight-line functions (`--float-spec`, 154 functions); loops and RenderJobs did not gain (plan 2.1) | **try** F2 (`__restrict`) on the replay |
| 12 | Byte-reversed registers, `shufb` constants, XFloat tiers | partial | words byteswapped per lane, `shufb` as `pshufb` (SSE4.1), float exact / lean / speculative / native; no constant-control specialisation (`spu_lifter.py:942`) | no for the layout (a rewrite); `shufb` constants later, inside native stages |
| 13 | Skip the LS address mask | not done | `lsa &= SPU_LS_MASK & ~0xF` in `spu_ls_read128` (`spu_context.h:651`) | no: one AND |
| 14 | Channel waits sleep | done | condition-variable waits (`spu_channels.c:1414`); SPURS workers sleep when idle | - |
| 15 | DMA as synchronous copies | done | `mfc_do_transfer` copies at once; list DMA still goes through per-element channel emulation | later (plan 1.2) |
| 16 | Reservation version counters | not done | PUTLLC compares the 128-byte line under one global lock-line lock (`spu_channels.c:603`) | no: ~4 atomics an EDGE run; the lock, not the compare, is what costs (plan 1.2, striping) |
| 17 | SPURS in host code | done | the kernel is host code (`cellSpurs.c`), policy modules are lifted, workers pick their next workload themselves (L6) | - |
| 18 | Fewer host threads, CCX placement | partial | six workers, Windows' placement; CPU sets and pinning tried, not kept (plan 1.7) | **later**: a worker-count knob (5 vs 6) to cut SMT sharing |

### C. LV2 and libraries

| # | item | status | evidence | verdict |
|---|---|---|---|---|
| 19 | Sync primitives on host primitives | done | per-object host locks and condition variables; a table lock only for id lookups (`sys_mutex.c`) | no: not seen in the main thread's samples |
| 20 | Hot libc as native code | not done | the title links libc statically; overrides reach indirect calls only (F6) | **later**: only if the sampler shows guest `memcpy`/`memset` hot |
| 21 | Codecs on the host | done | FFmpeg for movies and compressed audio (`PS3RECOMP_FFMPEG_RUNTIME`); in-level audio is the title's own SPU mixer (w7) | later: w7 as native code (plan 3.2) frees a core but does not move the frame |
| 22 | Timer resolution, `mftb` | done | high-resolution waitable timer (`sys_timer.c:266`); `mftb` from one 79.8 MHz clock (`ppu_timebase_now`) | - |

### D. RSX and graphics

| # | item | status | evidence | verdict |
|---|---|---|---|---|
| 23 | FIFO on its own thread | partial | one frame clock thread walks the FIFO and runs the D3D12 backend | **try** F4 first (probe), then plan 1.4 |
| 24 | Vertex swap on the GPU | done | the vertex shader reads raw guest bytes (`ByteAddressBuffer vp_raw`); indices are swapped on the CPU | - |
| 25 | Texture cache, no readbacks | done | hash lookup, bounded by bytes; blit snapshots on the GPU instead of readbacks | - |
| 26 | Shader cache, async compile | done | disk cache (`graphics.shader_cache`), worker builds (`D3D12_ASYNC_SHADERS`); no `ID3D12PipelineLibrary` (the driver caches PSOs) | later: pipeline warm-up (plan 4.4) |
| 27 | Asynchronous queries | partial | ZPASS reports carry GPU counts, read after the per-batch drain | with plan 1.4 (F4) |

### E. What a static recompiler adds

| # | item | status | evidence | verdict |
|---|---|---|---|---|
| 28 | Per-title patches and native functions | partial | native EDGE loops (`native/`, `spu_native_hooks.json`), PPU insn hooks (`ppu_hooks.json`) | **try**: the EDGE stages go on (plan 2.8) |
| 29 | Profiling by guest symbol | done | `PS3_SAMPLE_THREAD`, `symbolize` with the map, `PS3_FTRACE`, `SPURS_WID_PROF` | - |
| 30 | Differential testing | partial | SPU job replay with output hashes; PPU conformance and VMX tests; screenshots at 150/250/350 s; no state diff with RPCS3 | enough for the levers above |

### The static-only list

| item | status | verdict |
|---|---|---|
| Stack slots as host locals | not done; 29% of guest accesses in a sampled file are `r1`-relative, all relaxed atomics | **later**, after F2. A cheaper first step: plain (non-atomic) accesses for `r1`+constant slots in functions that never take an `r1` address (the lifter already tracks taken addresses, `_CS_ADDR_OF_RE`), so clang can forward a spill to its reload |
| Custom calling conventions | not done | no: restrict plus locals give most of it without changing every call |
| `.rodata` / TOC folding, jump tables | n/a / done | this title builds addresses with `lis`/`addi` (1 `r2` load in 14K lines); jump tables are already `switch` |
| Dropping SMC and reservation checks | partial | **try** the narrow one: the job manager compares the 52 KB EDGE module before every job (~1.5 us of a 91 us run, plan 2.8). Treat the source range as immutable for this title, checked by a write watch in a diagnostic build |
| Removing SPU DMA | not done | only inside whole-job native replacements; DMA is ~10% of an EDGE run |
| Hot functions rewritten natively | in progress | EC08 and A950 done (+1.4% together); see plan 2.8 for the next stages |
| Engine-level graphics HLE | not done | no: the RSX path works and is not what sets the frame |
| Assets converted at build | not done | no: textures are cached by hash at run time, shaders are in the disk cache |
| Little-endian private data | not done | no: whole-program alias analysis, high risk, small gain |
| Build for this CPU, PGO | partial | **try** F3; PPU PGO later |
| Threading and timing in the title | not done | later: frame pacing, the 60 FPS patch (plan 4.1, 4.3) |

## 3. The work, in order

Each step has its own measurement, under the plan's rules (plan section 8): ABBA, two runs a side,
on the charger.

1. **PPU vector loads and VMX, phase 2** (F1).
   - What: lift `lvlx`/`lvrx`/`lvsl`/`lvsr`/`vperm` as SSSE3 code; then vectorize `vcfsx`,
     `vctsxs`, the float compares, `vmaxfp`/`vminfp`, `vrefp`/`vrsqrtefp` (div/sqrt, same
     results) and the shifts.
   - Tests: each new form against the current byte loop and lane loop, random inputs plus edge
     cases (NJ, NaN, every `sh`), in the style of `tools/test_ppu_vmx_float_vec.py`.
   - Measure: level ABBA. Expected +1-3%. Cost: lifter and header work, one PPU rebuild a
     round.
2. **Probe the frame clock thread** (F4).
   - What: `RSX_SLOW=<pct>`, the same idea as `GOWA_MAIN_SLOW`: spin at each batch for <pct> of
     the thread's own time since the last one.
   - Also: a steady-state GPU-wait figure in the `[PERF]` line (`s_hl_gpuwait_s` is only printed
     on hitches now).
   - If +25% costs more than the main thread's -2.2%: plan 1.4, then 1.5, comes before the
     next EDGE stage.
3. **`__restrict` on the lifted context** (F2).
   - SPU first: lifter flag, replay of `logs/jobcap2` (EDGE) and `logs/jobcap3` (RenJobSet,
     MeshProc), outputs must stay identical, then the level.
   - PPU after `ppu_stwcx32` / `ppu_stdcx64` take `ctx`.
   - Expected: unknown. Measured so far only on a hand copy of the pattern (9 -> 5 context
     accesses).
4. **`-march=x86-64-v3 -mtune=znver2`** for lifted PPU and SPU code (F3).
   - One CMake option, default on, beside `PORT_SPU_AVX2`.
   - Expected 0-1%; it rides along with step 1's rebuild.
5. **EDGE, on from plan 2.8.**
   - Native `F620` (4x4 transform), then `7BB4` (lighting) or `D92C`+`D4DC` (culling, needs a
     per-lane path).
   - The module-compare skip (~1.5 us an EDGE run).
6. **PPU indirect-call short path** (F5).
   - One flag for all diagnostics, read once; the common call does the cache lookup and the
     call only.
   - Expected under 0.5%. Kept if it does not lose: it only removes work.
7. **Later, each on evidence first:**
   - stack slots as plain accesses (after 3);
   - guest `memcpy`/`memset` as native code (after a sampler shows them, with the lifter-level
     replace list of F6);
   - PPU PGO;
   - a SPURS worker-count knob;
   - MONITORX on the polled lines.

**Not pursued, with the reason in the tables:** LS mask removal, a byte-reversed SPU register
layout, reservation version counters, `lwarx`/`stwcx.` idioms as `lock xadd`, custom calling
conventions, little-endian private data, engine-level graphics HLE, assets converted at build,
LTO, CCX pinning (tried, -1.7% and worse), MOVBE.
