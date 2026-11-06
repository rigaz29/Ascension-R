# EDGE on the GPU: second review

Written 2026-11-06, branch `edge`, at port `44b3b7d`, fork `89d6a8f4`. No code changed; the probes
below were temporary. The question: how to go on with EDGE on the GPU, after
[the first review](2026-11-05-edge-gpu-review.md), the kept results and the main thread's work.

**The result**
- EDGE counts again in both scenes: 25% more of it costs 6% of the frames. In the fight that was 0
  before the main thread got faster.
- The next step is not another stage. The stages are 18% of EDGE at the gate and 23% in the fight, and
  a job with a kept result does no math, yet costs 34 or 54 µs against 40 or 73.
- Those jobs are 63% of EDGE's time at the gate and 49% in the fight. What they pay for is the job
  around the math.
- The largest piece of any job does not look like EDGE's at all: the step around the job manager's
  `0x17E0`, 12.5 µs in every job, and about 20 µs a job with its other calls. That is ~40% of EDGE's
  worker time.
- What the GPU can still take: kept results that stay in GPU memory, then skinning from cached
  inputs. Both make the geometry resident, as in a PC game, instead of sent every frame.

## What came of it

G and K are done, on by default. One executable name, two runs a side.

| | estimate, gate / fight | measured, gate | measured, fight |
|---|---|---|---|
| G | +2% / +1% | 26.44 -> 29.09 (+10%) | 25.12 -> 26.11 (+4%) |
| K | +2.5% / +4% | 28.84 -> 29.98 (+3.9%) | 26.16 -> 27.39 (+4.7%) |

- **G was underestimated.** The gate was held by the frame clock thread more than this review thought:
  its work went from ~36 to 32.5 ms a flip.
- **K's gate figure is capped.** Half of the windows are at the game's own 30 flips/s.
- **After both**, per flip:

  | | gate | fight |
  |---|---|---|
  | frame clock thread | 31.7 ms, 92% | 25.7 ms, 68% |
  | main thread | 91% | 91% |
  | workers (pm) | 530% | 547% |

**G: what the plan missed.**
- A job's list runs past what it puts: the index stage's end, with junk after the job's count. A block's
  index range has to come from what is drawn. Over the whole list a 160-vertex block looked like 65,460.
- A lit draw has arrays outside the block, the mesh's own. They are copied as the raw path copies them.
- A fill has to stay inside what the job puts, or it writes over the next job's block in the ring, whose
  checks then fail. Found by forcing it: `VTX_RESIDENT_MB=0` fills every draw, `=8` refills the resident
  part hundreds of times a run. The default fills nothing at the gate or in the fight.

**K: how it went.**
- The probe: without the palette in the key the geometry repeats. A skinned job's slots 5, 10, 11, 13, 14
  and 16 are the same every frame, and the descriptor words in the key too.
- The upper bound, by freezing the pose (a temporary test): +7.5% at the gate, +4.9% in the fight, a run
  each.
- `GOWA_EDGE_SKIN=2` works every skinned job's vertexes out on the CPU and compares them with what the job
  packs. In the fight, 1,556,480 jobs and 565 million vertexes:
  - positions: bit for bit the same;
  - tangents: at most a unit off, 1.8% of them;
  - normals: 98.4% within 2 units of 32767. `0xBBC8` scales by an estimate and this by `1 / sqrt`.
- `0xE550` ends by calling `0xBBC8` itself (returning to `0xF484` and `0xF490`). In the gate and the fight no skinned
  job goes on to `0xF538`, so a job that does stays on the SPU: that path was never exercised, so it is not
  in the shader.
- What a hit leaves of `0xE550` is one word: `c + 0x58`, the free space it hands back.
- The resident part holds 64-96 MB in the fight, the arena 107 of its 128 MB.

## Where things stand

| | gate | fight |
|---|---|---|
| flips/s | 26.9 (27.45, 26.42) | 25.4 (25.03, 25.72) |
| SPU workers | 530% of a core, 194 ms a flip | 555%, 228 ms |
| EDGE w14-16 | 216%, 79 ms a flip | 77 ms a flip |
| General6 | 27 ms | 48 ms |
| RenderJobs, RenJobSet | 27, 26 ms | 26, 28 ms |
| frame clock thread | 94%, 35 ms a flip: full near 29 flips/s | 71% |
| GPU | 30-40% busy at 1.3-1.4 GHz, 25 W | the same: not a limit |

Each part 25% slower, two runs each (`logs/edge-ana/e2g`, `e2f1`, `e2f2`):

| +25% on | gate | fight |
|---|---|---|
| nothing | 27.45, 26.42 | 25.03, 25.72 |
| EDGE | 25.93, 24.64 (-6.1%) | 23.54, 24.20 (-5.9%) |
| the main thread | 25.63, 25.95 (-4.3%) | not rerun (-14.2% before the helpers) |

So at the margin 1% of EDGE is ~0.25% of the frames. The estimates below use that; earlier ones built
the same way ran ahead of what came (2.20-2.21), so they are upper bounds.

## What an EDGE job costs now

A temporary probe timed every job from its entry to the next job's on the same worker
(`logs/edge-ana/e4g`, `e4f`). 1,560 jobs a flip.

| class | gate: jobs | µs a job | share of EDGE | fight: jobs | µs a job | share of EDGE |
|---|---|---|---|---|---|---|
| position, kept result | 36% | 33.8 | 24% | 29% | 32.9 | 19% |
| lit, kept result | 37% | 53.6 | 39% | 30% | 52.3 | 31% |
| position, no result yet | 3% | 40.0 | 2% | 3% | 38.2 | 2% |
| lit, no result yet | 3% | 72.7 | 5% | 4% | 58.8 | 4% |
| position, skinned | 8% | 48.9 | 7% | 15% | 50.2 | 15% |
| lit, skinned | 11% | 87.3 | 19% | 16% | 86.3 | 26% |
| declined, other | 2% | ~105 | 3% | 2% | 77 | 3% |

Inside a job with a kept result, in µs (`e7g`, marks at the stage driver's calls):

| | position | lit |
|---|---|---|
| entry to the key | 1.5 | 1.6 |
| key to unpack: three job manager calls, `0xAF38`, `0x88B0`, `0x17C0` | 5.4 | 7.9 |
| unpack, the transform's skip, `0x41A0` | 2.0 | 3.4 |
| **`0x5388` to the index stage: it calls `0x17E0`, where the job manager starts the next job's load** | **12.7** | **12.4** |
| index stage to the output stage (constants, program, the light record) | 2.1 | 6.1 |
| output stage: copies back, ring allocation, PUTs, hole | 6.9 | 13.9 |
| output stage to the next job's entry | 5.7 | 5.8 |

- That step is the same in every class: 12.4-13.7 µs, skinned jobs included. It was not timed inside.
- `0x41A0`, the ring line's first read, is 1 µs.
- Interrupts are cheap: one a job, 0.9 µs.
- Atomics: 8 a job, 2.7-3 µs.
- DMA commands: 7.6 a job and 7.2 µs for a position, 11 and 13.2 µs for a lit job.

The EDGE workers' samples (`logs/edge-ana/e2g.rvas`, `e2f.rvas`, map `e2.map`):

| | gate | fight |
|---|---|---|
| the runtime: channels, MFC, dispatch, coherency, drain | 38.7% | 36% |
| EDGE's glue (lifted) | 13.2% | 12.7% |
| the job manager (lifted, 78 functions, the largest 0.8%) | 9.8% | 9.4% |
| DMA copies | 9.9% | 8.0% |
| natives: decode, skinning, indices, output, packing, the rest | 18% | 23% |
| other | 10% | 10% |

## What the frame clock thread does for EDGE

Of its samples at the gate:
- `upload_tris_vp_raw`'s copy of the ring bytes 10.6%, `rsx_index_copy_be` 3.3%: 14% in all, ~5 ms a flip.
  In the fight ~10%.
- Kept results are 75% of the vertex bytes at the gate, 56% in the fight.
- The rest of the thread is per draw and per method: `rf_prepass` 8.9%, `vp_upload_tex_slot` 6.3%,
  the FIFO walk ~17%. None of the steps below change those.

## What each way forward can take

| | what | EDGE, gate / fight | frame clock | est. frames | work | risk |
|---|---|---|---|---|---|---|
| G | kept results stay in GPU memory | -7% / -5% | -10% / -6% | +2% / +1% | days | medium |
| K | skinned jobs from cached inputs, skinning on the GPU | -10% / -15% | small | +2.5% / +4% | 1-2 weeks | medium |
| S | a stub for jobs with a kept result | -14% / -12% | 0 | +3.5% / +3% | weeks | medium to high |
| J | the job manager's load path native | -15% to -20% | 0 | +4-5% | weeks | low to medium |
| D | 2b.3 as written, the stages one at a time | under the stages' 18-23%, nothing for kept results | 0 | | weeks | medium |

G's EDGE figure assumes half of a kept result's output stage goes. Together G, K, S and J are about -47%
of EDGE, ~37 ms a flip, which would put the gate near 30 flips/s and the fight near 28, with the frame
clock thread and the main thread as the next walls.

### G: kept results in GPU memory

A job with a kept result still copies its list and records into the local store and PUTs them into
the ring, and the frame clock thread copies them out again, every frame, the same bytes.

- **The SPU side.** The output stage leaves out the list's and the vertexes' copies and PUTs. It keeps
  the ring allocation and the hole, so the labels and the hand-back work as now, and puts 16 bytes
  at the block's start: the tag that tells a live record from a stale one.
- **The record.** `rsx_d3d12_vertex_pass_publish` is keyed by the vertexes' IO offset already. Position
  jobs get a record too, and a record names the kept result.
- **The backend.** `upload_tris_vp_raw` finds the record and copies nothing:
  - the first time, the result goes into a resident part of `vp_vb` and `vp_ib`;
  - a segment's header holds byte addresses into that buffer, and a draw an index offset, so a segment
    can point at resident bytes, and it should need no shader change;
  - the vertex pass reads a lit segment from there. Lit results are 24 bytes a vertex in the arena and
    54 in the ring's layout, so either the pass takes a stride or the resident copy is padded.
- **What must hold.**
  - Nothing else reads those ring bytes. Paths that do (the `ix` path for a spread draw, the CPU's
    fallbacks) take them from the arena, or the frame clock thread writes them to the ring first.
  - An arena change invalidates the resident copies. It is rare: the gate holds 17 MB, the fight 39.

G gives little by itself. Its value is the frame clock thread: the wall moves from near 29 to near 32
flips/s, and K and S build on it.

### K: skinned jobs

Skinned jobs are 19% of the jobs at the gate and 31% in the fight, and 26% and 41% of EDGE's time: they
never hit, because the palette changes every frame.

- The geometry should not change. Take what moves out of the key (the palette in slots 6 and 7, the
  matrix in slot 18), keep the weights and the stream (slots 10, 11), and they should repeat like the
  others. [The job results note](2026-11-05-edge-job-results.md) saw 0.1-2.6% of them repeat with all
  of it hashed, so this needs measuring first.
- The kept result is then the unskinned arrays and the weights, resident, and the job hands over the
  palette (up to 11 KB) with its record.
- The vertex pass applies the palette: `spu_edge_skin.c`, `spu_edge_skinmat.c` and `spu_edge_vnorm.c`
  are the references, then normals and tangents, then lighting as now.
- Float results differ from the SPU's, so it needs 2b's tolerance check, on screenshots as well.
- It saves 15 µs of a position job and 34 µs of a lit one. The fight gains most: that is where people
  fight.

### S: a stub for kept results

A job with a kept result could go from the key straight to the output.

- It saves `0x44E0`'s path up to the output, less `0x17E0`, which has to run: ~6 µs of a position job
  and ~14 µs of a lit one. The first review's -80% assumed the job's cost was EDGE's own.
- Still to map: `0x67C0`, `0x61B0`, `0x9A60`, the 16-byte record, descriptor words 19, 20, 33-35. Those
  are the output contract's open points, and a lit job needs all of them.
- Position jobs need the least, so they would come first (24% of EDGE's time, -4% of it).

### J: the job manager

Not the GPU, and it is what takes the most. `0x17E0`, the job's end and the calls before unpack are
about 20 µs a job, in every class: ~31 of EDGE's 79 ms a flip.

- The cost is spread: 78 lifted functions, 9.8% of the samples, and much of the runtime's 38.7% under
  them. Next job's GETs are real copies.
- `wwspm` is one module with one protocol, and the replay's strict mode checks it exactly.
- It helps every job workload, EDGE most because its jobs are the smallest.
- It is the largest item here, but flat code has paid badly so far (the glue, 2.27). A cheaper first
  look: which runtime paths it goes through most, since those help everything lifted.

### D: the stages, as 2b.3 had them

Stays the poorest. Kept results have left the stages out already, and for the rest the job's shell
stays where it is. K is what remains of it, and K only pays because the geometry is cached.

## Recommendation

1. **G.** The smallest step on the GPU side, and what K stands on.
2. **Before K, a probe:** how often skinned jobs repeat without the palette in the key. A day; if it is
   as high as the unskinned jobs', K goes ahead.
3. **K.**
4. Then measure again and choose between J and S. J looks better on paper; S's position jobs are cheap.

Lazy CR and XER ([the main thread's review](2026-11-06-main-thread-review.md)) does not depend on any
of this and comes first in the fight, where the main thread is still close to the limit: G gives the
fight ~1%, lazy CR an estimated 3-5%.

Each step is measured alone, under one executable name, base spread through the batch, at the gate and
in the warm fight, audio reports on.

## Risks

- **A wrong key shows as wrong geometry, not a crash.** With G nothing recomputes a hit. `GOWA_EDGE_MEMO=2`
  stays as the check, and needs to keep working with the PUTs left out.
- **The scenes.** The gate and the fight are the only benches. A scene that streams new geometry fills
  the arena, and G has to give up cleanly there.
- **The gain is capped by others.** The frame clock thread at the gate, the main thread in the fight.
  Above ~29 flips/s at the gate the render thread (fork branch `perf/render-thread-2`) has to come in.
- **The slope is measured at +25%, not below.** Going down, more of the time may turn into waiting.
- **The rare hang at level load** (`256 identical transfers`, 5 logs) is in the job manager's poll at
  `0x25E4`, which J would touch.
