# SPU code generation: what the level's profile says, and the design

The [performance map](2026-10-28-performance-map.md) found that a level frame is a serial chain of
SPU job lists, so the frame rate follows the latency of one SPU job. This note profiles those
jobs, measures where the lifted code's instructions go, and orders the changes to the SPU lifter
and its helpers. The plan going in was to keep SPU registers in locals ("register promotion");
the measurements put two other changes first.

## 1. Profile (first level, 60 s, 185,000 samples of the six SPU workers)

`PS3_SAMPLE=1`, `PS3_SAMPLE_THREAD=spurs pm`, `--spu-lines` build, `gowa.py symbolize --wid
--lines --callers`.

| | share of samples |
|---|---|
| lifted SPU code | 45.9% |
| SPU runtime: channels/DMA/dispatch 10.7%, coherency 3.5% (`spu_lockline_lock` 1.7%, `spu_coh_notify_write` 1.6%), drain 1.9%, interpreter 1.2% | ~17% |
| outside the executable (mostly the workers waiting for work) | 35.9% |

By workload, on the serial chain:

| workload | lifted code | hottest | operations (share of the workload) |
|---|---|---|---|
| General6 w2 | 95.6% | one function, `jobmod_a67e549fa0ab_e41E8_spu_func_00004F00`, **47%** of the workload; `4AB0` 11%, `63B4` 9% | float `fa`/`fm`/`fma`/`fs` ~30%, the float helpers' checks (`x8_z`, `all4`, `flush`, `inrange`) ~15%, `shufb` 12.6%, LS reads 8.9% |
| Batch w6 | | | `fma` 12.4%, `shufb` 9.9%, `fm` 7.8%, LS reads 7.9%, LS writes 6.4% |
| EDGE w14-16 | | `e8f44fbc6dbf` functions `EC08`, `7BB4`, `A950` | `shufb` 11.8%, LS reads 10.5%, `fma` 7.8%, LS writes 6.0%, `fm` 5.6% |
| RenderJobs w12 | 68.7% | spread (`6D90` 8.2%) | LS reads 16.3%, **`SPU_DIRECT` 13.0%**, LS writes 9.6%, function entries 9.2%, `shufb` 8.2%; runtime 18% channels/DMA, 3.4% drain |
| audio w7 | | | LS reads 14.5%, LS writes 8.8%, `SPU_DIRECT` 7.5%, `shufb` 7.0% |

Two kinds of job, then: float-heavy (General6, Batch, EDGE) and transfer/LS-heavy (RenderJobs,
audio).

## 2. Where the instructions go

`4F00` is straight-line code: ~450 SPU instructions (505 lifted lines, 95 registers, 51 LS
reads, no loop, call, channel or LS write). It compiles to **19,525 x86 instructions, ~43 per SPU
instruction**:

| | x86 instructions |
|---|---|
| moves and spills (`vmovdqa`, `vmovaps`, `movq`, `movl`) | ~4,300 |
| range checks and the +0 flush (`vpcmpeqd`, `vpand`, `vpcmpgtd`, `vpandn`, `vtestps`) | ~3,350 |
| constants broadcast again for each operation | ~1,510 |
| **the exact path's double conversions** (`vcvtps2pd`, `vextractf128`, `vxorpd`) | ~870 |
| branches, calls to the fix paths, `vzeroupper` | ~1,200 |
| `vpshufb` (`shufb`) | 344 |

Every float operation is compiled in **both** forms, selected at run time by `g_spu_float_rz`
(`SPU__X8_FA` and friends): the round-toward-zero form and the full exact form, whatever mode the
process runs in.

### Variants, compiled with the build's flags (`/O2 -msse4.1 -mavx2 -mfma -fno-strict-aliasing`)

x86 instructions per function (`variants.sh`; `rz` = the RZ forms chosen at compile time, still
bit-exact; `rz+reg` = that plus SPU registers in locals; `approx` = native single precision
only):

| function | now | rz | rz+reg | approx |
|---|---|---|---|---|
| General6 `4F00` | 19,525 | 7,709 (-61%) | 9,486 | 3,135 (-84%) |
| General6 `4AB0` | 2,334 | 2,195 (-6%) | 3,398 | 2,118 |
| General6 `63B4` | 24,402 | 8,923 (-63%) | 8,571 | 1,920 (-92%) |
| EDGE `EC08` | 4,767 | 2,136 (-55%) | 2,870 | 1,152 |
| EDGE `7BB4` | 6,670 | 2,693 (-60%) | 3,072 | 961 |
| EDGE `A950` | 10,743 | 4,745 (-56%) | 4,475 | 1,786 |
| EDGE `D92C` | 2,809 | 1,677 (-40%) | 2,155 | 1,203 |
| EDGE `7ED8` | 2,951 | 1,173 (-60%) | 1,328 | 480 |
| RenderJobs `6D90` | 238 | 238 | 296 | 238 |
| RenderJobs `13070` | 327 | 327 | 597 | 327 |
| RenderJobs `8E30` | 949 | 881 | 1,292 | 844 |

- **Choosing the float form at compile time** halves the float-heavy code without changing a
  bit of its results.
- **Approximate float** (what RPCS3 runs this title with: `XFloat Accuracy: Approximate`)
  removes another half to two thirds.
- **Registers in locals, done plainly, makes the code larger**: the write-backs and reloads at
  every exit and call outweigh what they save, and these functions spill to the stack anyway
  (95 live SPU registers against 16 xmm). `__restrict` on the context (the cheap way to say LS
  stores do not touch `ctx->gpr`) changes nothing. Neither is the first lever here.
- RenderJobs' functions are small, with no float: its cost is the transfers between them
  (`SPU_DIRECT`, function entries) and the runtime (dispatch, DMA).

Static counts are not run time; each step below is kept only if the level's per-workload time
(`SPURS_WID_PROF` us/run) and flips/s say so.

## 3. Design, in order

**Step 1. Float form chosen at compile time (bit-exact).**
- `spu_helpers.h`: with `SPU_FLOAT_RZ_ONLY` defined, `SPU__X8_FA`, `SPU__X8_FMA` and `spu_fm`
  take the RZ forms directly; the exact per-lane fix paths stay, out of line and cold.
- The port builds the lifted SPU code (job modules, PIC pieces, the Wws PM, the embedded
  programs) with it; the interpreter and the runtime keep the run-time choice.
- Lifted code compiled this way must run in the RZ mode: `spu_run_with_halt` switches MXCSR
  whenever such code is present (`SPU_FLOAT_RZ=1` is already the port's default).
- Check: `test_spu_float_exact.c` (the RZ forms, three x86 builds), an in-game
  `-DSPU_FLOAT_RZ_CHECK` run, the level A/B. Expected: General6, Batch and EDGE runs shorter.

**Step 2. Job modules lifted with `--merge-chunks`.**
- `gowa.py lift_jobmods` passes `--merge-chunks` (the embedded programs already use it): the
  chunks between call targets become one C function with gotos instead of `SPU_DIRECT`
  transfers, each a Windows x64 frame. In God of War HD it took the particle job from 6.0 to
  4.7 ms. Aimed at RenderJobs (13% `SPU_DIRECT`, 9% function entries) and audio.
- Check: the job modules' interrupt entries (merged chunk entries hand pending interrupts back
  to the drain), the level A/B.

**Step 3. An approximate float option.**
- `SPU_FLOAT_APPROX` (compile time): native `addps`/`mulps`/`vfmadd` in the RZ mode, no range
  check, no +0 flush, no fix path. Off by default; turned on for this title only if the level
  looks the same (screenshots against the exact build) and it pays in the A/B.

**Step 4. LS accesses without the diagnostic gate in release builds.**
- `spu_ls_read128`/`write128` check `g_spu_ls_watch_n` and keep a cold call to the diagnostic
  path at every access (35 such call sites in `EC08` as compiled, with what clang inlines into
  it). A compile-time switch for
  release builds removes both; the diagnostics stay in the default build.

**Step 5. Cheaper transfers.**
- `preserve_none` for lifted-to-lifted calls and tail calls (measured before L6, when SPU
  latency was not the limit: no change; measure again), and a lighter `SPU_DIRECT` (its checks
  of `g_spu_direct`, the step count, the stack-reset table and pending interrupts on every
  transfer).

**Step 6. Registers in locals, where it pays (after 1-5).**
The rules are worked out (below); what is missing is doing it without the bloat:
- write back only the registers dirty since the last write-back (a forward pass over the
  lifted body; at a label, every register written in the function counts as dirty);
- after a call to compiler-generated code, reload only the SPU ABI's volatile registers
  (r3-r79) and keep r80-r127, per module, where the module is known to follow the ABI;
- promote only functions where barriers sit inside loops (LS writes, calls, channel
  operations); leave the straight-line ones as they are.

Rules: SPU registers become locals, loaded at entry; **written back** before every exit or
transfer (`return`, `SPU_DIRECT`, `SPU_RET`/`SPU_RET_REG`, a trampoline), every call
(`brsl`/`bisl`/indirect, `host_depth++`), `spu_stop`, `spu_halt`, `spu_unsupported`, and every
`rdch`/`rchcnt`/non-MFC `wrch` (a read of an empty inbound mailbox can park the SPU through
`spu_halt`, and it resumes later from `ctx->pc` and `ctx->gpr`); **reloaded** after a call and a
resuming stop. Interrupts are taken only at transfer points, which write back already; an
`iret` that unwinds handler frames resumes from the context. The prototype (`promote.py`)
applies these rules to lifted C and compiles.

## 4. Results of steps 1 and 2

First level, two runs a side alternated, `GOWA_THREADS` + `SPURS_WID_PROF`; flips/s from 185 s
after the movie, time per run averaged over the level's windows:

| | flips/s median | General6 | RenJobSet w11 | EDGE | audio | RenderJobs | Batch |
|---|---|---|---|---|---|---|---|
| before | 9.40 | 1,643 us | 560 | 122 | 220 | 648 | 892 |
| step 1: `SPU_FLOAT_RZ_ONLY` | 9.46 | 1,403 (-15%) | 511 (-9%) | 112 (-8%) | 202 (-8%) | 609 (-6%) | 862 (-3%) |
| steps 1 + 2: job modules with `--merge-chunks` | 9.54 | 1,382 (-16%) | 493 (-12%) | 111 (-9%) | 198 (-10%) | 616 (-5%) | 851 (-5%) |

- Step 1 is kept although the frame rate did not move, as the leaner float forms were: it is
  less code (the executable 6 MB smaller) with the same results (`test_spu_float_exact`
  446,840,832 lanes bit-identical in three builds), and nothing to keep correct beyond running
  SPU code in the RZ mode, which the port now always does. Fork `b184d749`; `CMakeLists.txt`
  option `PORT_SPU_FLOAT_RZ_ONLY`.
- Step 2 is not kept: 1-6% more off a few workloads, nothing on the frame, and 13 MB more lifted
  C (every chunk is lifted alone as well as in its group). To try again once the SPU chain is the
  limit on its own.

Why the jobs got faster and the frame did not: with `TEX_BUDGET=50` (the D3D12 backend skips
almost every texture bind -- wrong textures, a probe only) the level ran **12.1-12.3 flips/s,
+30%**, its texture time 28-59 ms a frame down to 0.5-4 ms. The texture work on the frame
clock thread, which also walks the FIFO, is on the frame's path together with the SPU chain:
shortening one moves little until the other shortens too. The texture cache comes next
([performance map](2026-10-28-performance-map.md), section 6).

## 5. What is measured after each step

- Per workload: `SPURS_WID_PROF` us/run on the level (now: General6 1,610, Batch 866, RenderJobs
  627, EDGE ~118, audio 214).
- The chain: `PS3_FTRACE` + `ft_groups.py` (now: General6 54 ms, RenderJobs 32 ms, EDGE 73 ms open,
  RenJobSet 25 ms a frame; frame median 97 ms).
- flips/s, two runs a side, alternated.
