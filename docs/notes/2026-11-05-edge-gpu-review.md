# EDGE on the GPU: review before 2b.3

Written 2026-11-05, branch `edge`, at fork `79e571d6`. The question: what should the next step of
the [phase 2b plan](2026-11-04-phase-2b-plan.md) be, now that lighting is on the GPU and audio has
a worker of its own? The numbers are from the gate; the samples from `logs/edge-ana/now.rvas`, the
counts from `logs/edge-ana/rev1`.

**Since then:** A and B are done: the gate went from 22.96 to ~24.1 flips/s, A and B giving ~3% and
~2%, about the table's estimates for A and below them for B. Performance plan 2.26 has the numbers,
and what they correct here: culling cost the frame clock thread ~3 points, not 16 (that was the
pieces), and the pieces' own cost was ~1.6 ms a flip. C went another way: with culling on the RSX
most jobs repeat, so [their results are kept](2026-11-05-edge-job-results.md); with that the gate is
at 24.40. The glue stayed lifted: its profile is too flat to pay.

**The result:** 2b.3 as written, the other stages moved to the GPU one at a time, is not the best
next step. The stages are 37% of an EDGE job. The job's shell (the runtime, DMA, the lifted glue,
the job manager) is 56%, and a stage moved to the GPU leaves the shell where it is.

## Where things stand

| | now |
|---|---|
| the gate | 22.7 flips/s, 44 ms a frame |
| SPU workers | 598% of a core over seven (audio 53%): the other six ~91% busy |
| EDGE w14-16 | 270% of a core, ~119 ms of runs a frame, half of the SPU work |
| frame clock thread | 82% of a core: full at ~28 flips/s |
| main thread | 90%, mostly waiting; ~23 ms of its own work a frame |
| process | 860% of the twelve logical CPUs' 1200%, on six cores |
| GPU | 3D engine 28% busy at 855 MHz (2,100 at most): not a limit |

- A second lane added no frames (performance plan 2.25). With seven workers the limit looks to
  be moving from SPU slots to the host's six cores (not measured yet). If so, what counts now is
  CPU work taken away, wherever it is: the case for the GPU, and for anything else that does it.
- A frame's draws: ~6,800 indexed draw calls from EDGE's rings (133,800 a second), which are
  ~1,720 records; 1.5 M indexes; 28 MB of ring bytes copied (551 MB/s, 14% of the frame clock
  thread); ~1,900 lit segments, 330,000 lit vertexes on the GPU.

## What an EDGE job is made of now

The sampler, the EDGE workers' samples. The three lists are alike within 1%.

| | share of EDGE | can leave by |
|---|---|---|
| the runtime: channels, MFC, dispatch, coherency, drain | 30% | a job that is not lifted code |
| DMA copies (`memcpy`) | 6% | less data, in part |
| EDGE's lifted glue | 12.6% | a job that is not lifted code |
| the job manager (lifted) | 7.6% | only by not running the job |
| culling, `0xD618` and `0xD250` | 9.7% | the RSX (2b.2's switch) |
| indices, `0xE4D0` | 4.6% | stays, or the GPU with a job of its own |
| output pack `0x755B80` | 3.4% | see below |
| the eighth array `0xA950` | 3.2% | see below: it is dead |
| transform `0xF538` | 3.1% | the GPU |
| unpack and decode | 6.0% | the GPU, with the raw stream on the GPU |
| skinning | 2.6% | the GPU, with the palette and weights on the GPU |
| output stage `0x9080` | 2.5% | stays |
| the rest of the natives | 1.8% | |
| other, not listed | 6.5% | |

So the natives are 37% and the shell 56%. In a lit job of ~92 µs: natives ~36, glue ~36, the job
manager ~12, its DMA helper ~6. In a position-only job of 35.8 µs: natives 8, glue 15.5, the job
manager 10.5. For position-only jobs a GPU stage hardly matters: half of the jobs, 8 µs of
natives.

## Review of 2b.1

**What holds up.**
- The record's protocol: the sequence check, the 16-byte tag, a key written by every job.
- The vertex pass: 0 with no room, 0 records lost, 0 stale in 37,400 segments a second, ~650 a
  second without a record (jobs the capture declined).
- The fallbacks (the C model) ran 0 times at the gate.
- `VPT_SEGS` 8,192 against ~1,900 segments a batch; `vp_lit` 2 x 40 MB.

**What 2b.1 left behind: work with no reader.** When the stage is skipped:
1. **`0xA880` and its loop `0xA950` (3.2% + 0.5% of EDGE).** It takes the normal and tangent
   arrays and writes the eighth array, the binormal. In the stage driver its result goes to r85
   and from there only to r10 of `0x73E0`, which is skipped. The output piece does not read it
   (ids 1, 3, 4, 27-30), and the GPU rebuilds it from the ring's normal and tangent. The array's
   other accessors (`0xB648` at `0x5A80`, `0xE878`, `0xF4A4`) are on the transform's side, which
   runs before.
2. **`0x755B80`'s attributes 27-30.** The piece still packs four arrays nobody wrote (the F32
   copy and nine F16 conversions a vertex), and the vertex pass writes over them.
3. **30 of a lit vertex's 54 bytes.** They go out by DMA and are copied by the frame clock thread,
   and only bytes 0-23 are used. At 6.5 M lit vertexes a second that is 195 of the 551 MB/s. The
   ring could hold 24 bytes a vertex, with the pass reading those. This one touches the raw
   fetch's copy, so it comes after the first two.

Skipping `0xA880` needs the decision before `0x73E0`'s capture, and `edge_gpul_unskip` must run
it when a skipped stage has to run late.

**Other points.**
- The pass copies a segment whole, with the stale arrays the draw has enabled. It costs the GPU
  bandwidth only, of which there is plenty.
- The shader is compiled at the first lit draw, on the frame clock thread. Once a run; a cache
  would take that hitch away.
- `spu_overlay_note_get` (2.3%) looked like waste; it is already filtered, and what is left is
  the first touch of each GET.

## What each way forward can take

A frame is about (EDGE + the other SPU work) / (6 x 0.91), with 119 and 121 ms now. Estimates, from
that model; the model ran ahead of what speeding a workload up gave in 2.20-2.21, so take them as
upper bounds.

| | EDGE | est. flips/s | work | risk |
|---|---|---|---|---|
| A. what 2b.1 left behind (1, 2 above) | -6% | ~23.4 (+3%) | days | low |
| B. culling left to the RSX (2b.2's switch) | -10% | ~23.8 (+5%; measured +2.6%) | a switch, but it needs the frame clock thread's room | low |
| C. the job as C code (host reference) | -15 to -25% | ~24.6-26 (+8-14%) | weeks, step by step | low: exact, by replay |
| D. 2b.3 as written: unpack, decode, transform, skinning on the GPU | -12% at most | ~24 (+5%) | weeks: the raw stream, palette and weights to the GPU, per-stage tolerance | medium |
| E. a stub job (2b.4), on top of C and D | -80% | ~38 by the SPUs, ~28 by the frame clock thread | large | medium to high |

- **D alone is the poorest:** the most plumbing for the least, and each stage needs the dual-run
  check. With the job in C it becomes simple: the C job does not call the stage and records its
  inputs.
- **C is not the GPU, but it is the road to it.** E needs the job's whole logic outside the lifted
  code anyway (`0x67C0`, `0x61B0`, `0x9A60`, `0xA880`, the ring's wrap are still to map). C does
  that exactly, checked by the replay, and pays as it goes.
- **The frame clock thread stops everything at ~28.** B costs it 16 points, and any gain raises
  its load with the frame rate. Its largest EDGE-side items:
  - 133,800 draw calls a second for ~34,000 records: EDGE's 256-index pieces go through the
    walker and the backend one at a time, though they come under one non-increment header;
  - the ring's copy (14%), which item 3 above cuts by a third;
  - and when it binds, the render thread (fork branch `perf/d3d12-render-thread`, mode 2, stable
    in four runs) moves the limit past 40.

## Recommendation

1. **A:** skip `0xA880` and attributes 27-30 when the stage is skipped.
2. **The frame clock thread's room:** the draw pieces in one call, then B on by default.
3. **C, position-only jobs first** (half of the jobs, the simplest: host reference, step 2), then
   the lit jobs.
4. **Then D inside the C job, then E.** The GPU takes the stages when moving one is cheap.

Each step is measured alone, base spread over the batch (performance plan 2.25), at the gate and
in the warm fight, audio reports on.

## Risks

- One scene decides too much. The gate and the fight are the benches; the heavier scenes went
  with the Megaera save.
- The gains of A and B are near the spread between runs (about 1%): they need four runs a side.
- The model assumes the workers stay full. As EDGE shrinks, the chain's gaps (13% of the time
  with three or fewer runs open) count for more.
