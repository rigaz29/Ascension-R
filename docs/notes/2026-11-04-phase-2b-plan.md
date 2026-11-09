# Phase 2b: EDGE on the GPU, the plan and its progress

Written 2026-11-04, branch `edge` (fork: `gowa-edge`). This note is the plan for phase 2b and
the place its progress is kept: each step's status is updated here as it moves.

## Status

| step | what | status | commits |
|---|---|---|---|
| 2b.0 | measure: EDGE's draws, vertex bytes and backend time a frame; the GPU's load | done | fork `b1121f5d` |
| 2b.1 | the backend's compute, and the first stage on the GPU: lighting | done | fork `ab93538e` |
| 2b.2 | culling on the GPU: the RSX's own face culling, no compute | done, on by default since a draw's pieces go in one call | fork `825afdcd` |
| 2b.3 | the other stages, back to front | replaced: see [the second review](2026-11-06-edge-gpu-review-2.md): kept results in GPU memory, then skinning from cached inputs | |
| 2b.4 | a stub job: EDGE off the SPUs | not started | |
| fc.1 | the frame clock thread, first round: per-draw work shared along runs of draws | done | fork `43a1386f` |
| fc.2 | the frame clock thread, second round: a render thread | tried, not merged: ~3% slower (the SPU workers set the frame); mode 1 hangs the GPU | fork branch `perf/d3d12-render-thread` (`88bb0c16`) |
| later | zero-copy rings: the GPU reads EDGE's ring bytes where the SPU wrote them | when the frame clock thread binds | |

The steps were renumbered after 2b.0 (see its results): the native draw it planned first, the
vertexes decoded on the GPU, is what the backend's raw fetch already does.

## Why now

- **The per-function path gives little now.** Steps 2-4 of the host reference's progress
  ([host reference](2026-11-04-edge-host-reference.md), items 3.2-3.4) gave ~1-2% at the gate
  together.
- **Where the EDGE workers' time is now** (sampler at the gate, after those steps):
  - native stages (the math, what the GPU would take over): 45%;
  - the SPU runtime (dispatch, MFC, DMA, coherency, channels): ~30%;
  - lifted glue: 10%;
  - the job manager: 6%.
- **The gate is still SPU-bound.**
  - EDGE is ~57% of the SPU work ([plan](2026-10-29-performance-plan.md) 2.23).
  - 2.22's model puts EDGE on the GPU at ~35-40% less SPU work: the gate up to the frame clock
    thread's limit (~29-32 flips/s), about +40-60% from ~19 now.
- **The GPU has room.** It was 25-29% busy, clocked down to 645-720 MHz (plan 2.5, 1.5).

## What is ready, and what is not

**Ready (the SPU side):**
- every stage on the gate's path has a native routine that is bit-exact, a reference for each
  stage's GPU version;
- the formats are mapped ([output contract](2026-11-04-edge-output-contract.md)):
  - the lit 54-byte and the 12-byte position layouts;
  - the hole;
  - the rings and the labels;
- the tools:
  - ten capture corpora (1720 jobs);
  - replay, in state and strict mode;
  - the call-by-call check (`GOWA_EDGE_VERIFY`);
  - the per-block profile (a `--spu-lines` build).

**Not ready:**
1. **The backend has no compute.** No compute pipeline, UAV or dispatch in the D3D12 backend
   (`rsx_d3d12_backend.c`, the one this port runs).
   - Its vertex path for EDGE's draws is the raw fetch: the guest's vertex bytes are copied into
     an upload buffer as they are, and the vertex shader decodes them (2b.0).
   - EDGE's ~1,650 jobs a frame (about 10,000 a second on each of its three lists) make ~6,850
     indexed draw calls a frame.
2. **Nothing links an SPU job to its draw in the backend.**
   - A per-draw record is needed: what the GPU has to compute.
   - The job's dynamic inputs (the light set, matrixes) are copied when the job runs: the PPU may
     overwrite them before the backend reaches the draw.
3. **A stub job** (EDGE wholly off the SPUs) still needs:
   - the C job's frame (the host reference's design, not built);
   - the output contract's open points: the light selection `0x9A60`, the draw constants
     `0x67C0`, the program patch `0x61B0`, `0xA880` and `0x75FE80`, the ring's wrap path.

   Only step 2b.5 needs it.

## How a GPU stage is checked (decided 2026-11-04)

The GPU's float results cannot be bit-exact against the SPU's:
- the SPU rounds toward zero and flushes denormals;
- its `frsqest`, `frest` and `fi` are table estimates;
- the GPU rounds to nearest, and D3D12 allows its own `rsqrt` and division error.

So a stage moved to the GPU is checked against its native routine within a tolerance, not to
0 differences:
- a dual-run mode computes the stage on both sides and compares, reporting the largest error
  per attribute;
- screenshots at fixed points against the SPU path;
- the estimates (`frsqest`, `frest`, `fi`) are integer table rules, so they are ported to HLSL
  exactly. Only the rounding of the arithmetic differs.

The SPU side keeps its 0-difference checks: replay and the call-by-call check, with the GPU
path off.

## The steps

### 2b.0 Measure (done 2026-11-04)

**Results**, at the gate (profile 2, the default patches; `logs/edge-ana/2b0-*`):
- **EDGE's draws.**
  - About 6,850 indexed draw calls a frame come from EDGE's rings: ~1,650 jobs, each hole
    drawing in 256-index pieces. Other indexed draws: ~61 a frame.
  - All of EDGE's take the backend's **raw fetch** (`upload_tris_vp_raw`): the guest's vertex
    bytes are copied as they are and the vertex shader decodes them. No CPU conversion at all.
  - They copy ~28 MB of ring bytes (530 MB/s) and byte-swap ~1.5 M indices a frame.
- **The frame clock thread** (the FIFO walker and the D3D12 backend):
  - 63% of a core, 33 ms of CPU a frame at 19 flips/s: a limit near 30 flips/s, after the SPUs'
    19.
  - `VTX_STATS=1`: EDGE's uploads 121 ms a second (~6.5 ms a frame), the others' 6.7 ms.
  - Its sampler (75 s): idle 27%; the raw upload ~9.7% (`memcpy` 7.3, own 2.4), ~5 ms a frame;
    the FIFO walk and methods ~15%; the prepass ~9%; textures ~8%; draw recording ~6%.
- **The GPU** (GTX 1650 Ti, `nvidia-smi` every second): 37% busy (median; at most 41%), at 780
  MHz (median) of 2,100, mostly in P5, ~9 W.
- **The tools.**
  - The port registers EDGE's rings with the backend: the output stage native (`0xC628`'s ring
    line), once per ring and thread. That is 4 rings of 2.75 MB, IO `0x03254700`, `0x03514700`,
    `0x037D4700`, `0x03A94700`.
  - The backend's `VTX_STATS=1` now splits its indexed draws by ring:
    `rsx_d3d12_register_vertex_ring` in `rsx_d3d12_backend.c`.
  - An earlier attempt counted in `rsx_live_draw.c`, which this port does not run (it uses
    `rsx_d3d12_backend.c`); it was taken out.

**What it changes.**
- The native draw planned as 2b.1 (EDGE's vertexes decoded on the GPU) is what the raw fetch
  already does.
- What EDGE still costs the frame clock thread is copying: ~5-6.5 ms a frame. That frees nothing
  at the gate while the SPUs bind (19 against ~30), so it moves to a later step: zero-copy rings.
- So 2b.1 is the backend's compute together with the first stage, lighting, where the gain is.
  The compute shader can read the ring bytes the raw fetch has already put in a GPU buffer.

**The plan as measured before:**

- EDGE's draws a frame, their vertices and vertex bytes, against all draws.
- The frame clock thread's time on EDGE's draws: the vertex fetch and decode that 2b.1 moves to
  the GPU, and the rest of a draw's cost there, which stays.
- The GPU: 3D engine busy and clock (`nvidia-smi`, the Windows GPU engine counters).
- The frame clock thread's CPU a frame, and the gate's flips/s, as the baseline.
- **How.** The port registers EDGE's output rings with the backend: the output stage native
  reads each SPU's ring line anyway. The backend sorts each draw by whether its vertex arrays
  lie in a registered ring, and counts behind an environment switch. The same test is 2b.1's
  way to find EDGE's draws.
- **Done when:** the numbers are in this note and the baseline is set.

### 2b.1 The backend's compute, and lighting on the GPU (done)

**Sub-steps.**

| sub-step | what | status |
|---|---|---|
| a | map the lighting stage: its inputs, its outputs, its math, checked by a float model | done |
| b | the SPU side: the record, the skip, the publish | done |
| c | the backend's compute: lit segments, the dispatches, the draws reading the lit copy | done |
| d | the check: the GPU's attributes against the SPU's, within a tolerance; screenshots | done |
| e | default on, or not, from the bench | done: on |

**What the stage reads and writes** (a probe dumped the 41 lit calls of the gate replay: the
registers at entry, the local store before and after; `logs/edge-ana/probe73`, not kept).
- Its arguments: r3 the context `c`; r4-r7 the arrays of ids 27, 28, 29, 30; r9 the positions
  (id 1); r10 the eighth array of the pool (`c + 0x2C`), r11 the vertex count, r12 the light
  set, r13 flags, r14 the intensity (a byte over 255, in all four words). r8 is 0 at the gate.
- The eighth array holds a unit vector a vertex, perpendicular to the normal (id 3) and the
  tangent (id 4): `S = id3.w * normalize(cross(id3, id4))` within 2e-4. The stage reads it as
  the normal. Its fourth word is the list the stage builds: the offsets of the vertices that the
  culled indices use, rising. Only those vertices are lit; the others keep what the arrays held.
- Outputs, x y z of each (the w words are not put out):
  - id 27: `V + A / WS`, a light position for the vertex, where `A` is the weighted sum of the
    lights' directions and `WS` the sum of the weights (from 1e-4);
  - id 28: the sum of the positive parts of each light's colour, times `min(D.L, 1)` where
    `D.L > 0` and `D = A / |A|`; id 29: the same for the negative parts;
  - id 30: the point lights' colour.
- A light `j` (of the first n0 + n1 of the light set): `C' = intensity * colour`;
  `d = P.xyz - V`, `L` its direction for the first n0 lights, else `d / |d|`;
  `t = 1 - min(|d| P.w, 1)`, `att = (t^2 (3 - 2t))^2 C'.w`; the weight
  `|att| min(|S.L / 2 + 1/2|, 1)`; its colour `C'.xyz att`.
- A point light (the last n2): `a = 1 - min(|d| P.w, 1)`, the colour
  `sum (a^2 (3 - 2a))^2 C'.w C'.xyz`, from 0.5 when the flags have bit 0x100 (never at the gate).
- **The model.** In plain float math (as a GPU would do it), against the SPU's results for the
  41 calls (2,345 lit vertices):
  - with the stage's own `S`: ids 28-30 as F16, 87.8% identical, 11.0% one unit apart, the rest
    further, all where values are small or the summed direction is ill-conditioned (`|A|` small);
    id 27 within 9e-6;
  - with `S` rebuilt from the ring's normal (I16N) and tangent (X11Y11Z10): 86.3% identical,
    11.5% one unit apart; the largest absolute errors 5e-3 (ids 28-29, HDR colours up to ~10).
  - So the GPU needs only the ring's bytes and the light set.

**The design.**
- **The SPU side** (`GOWA_EDGE_GPU`: 2 on, the default in the game; 1 check; 0 off, the default
  in a replay or a capture).
  - The hook at `0x73E0` copies a record: the three counts, the flags, the intensity and the
    lights (position, colour, direction), as host floats. With 2 it skips the stage.
  - The output stage native publishes the record after the vertex PUT, keyed by the IO offset of
    the vertexes, which is the hole's `VERTEX_DATA_ARRAY_OFFSET` for slot 0 (bit 31 off). The
    hole is put after it, so the backend sees the record before the draw.
  - When the output stage runs lifted (an interrupt pending, about never), a skipped stage runs
    there first, from the saved registers: nothing between the two reads what lighting writes.
  - Only the gate's path is taken: flags without 0x800, r8 = 0, the lit layout (stride 54,
    ids 1, 3, 4, 27-30), at most 64 lights. Anything else runs on the SPU, with no record.
- **The backend** (`rsx_d3d12_backend.c`): a vertex pass, a compute shader the port registers.
  - A draw whose attribute 0 lies at a published key gets its raw copy lit: the segment's
    vertexes are copied on the GPU into a default-heap buffer (`vp_lit`), with ids 27-30
    computed, and the segment's draws read that copy.
  - The dispatches go in a compute command list of their own, recorded as the draws are recorded
    (record-ahead included), and executed before the batch's command list: no barrier between
    draws.
  - One thread group a segment, a thread two vertexes (108 bytes, 27 words).
  - Fallbacks: a draw that does not take the raw fetch, or a full `vp_lit`, gets the record's
    lighting on the CPU, the port's C model.
- **The check** (1): the SPU lights as before and the record is published; the GPU's attributes
  are read back and compared with the SPU's, as F16 units and absolute errors per attribute.
- **Expected:** ~+8-10% at the gate (b measures the bound: lighting skipped, colours wrong).

**Progress.**
- **b, the SPU side** (`native/spu_edge_gpulight.c`, the hooks in `spu_edge_light.c` and
  `spu_edge_out.c`).
  - The record is taken in `0x73E0`'s hook: 32 bytes and 48 a light, at most 32 lights.
  - With 2 the stage is skipped. Besides ids 27-30, it writes one thing that is put out: the
    index list padded to a multiple of 8 with its first index (for its own marking loop). The
    skip pads it too.
  - The output stage native publishes the record after the vertex PUT. The key, the vertexes' IO
    offset, comes from the ring line (start EA, IO offset, end EA).
  - A skipped stage that the output stage cannot publish for runs there, before the packing.
    `GOWA_EDGE_GPU_DEFER=1` does that for every skipped stage: the gate, l1, l3 and l5 corpora
    replay with 0 differences (state mode). With 1, 0 differences as well.
- **c, the backend** (`rsx_d3d12_backend.c`, "The vertex pass").
  - The records: a pool of 4,096 slots, and an index per registered ring (a word per 128 bytes;
    each 128-byte block of a ring holds at most one job's vertexes).
  - A raw segment whose attribute 0 is a published key gets a lit copy in `vp_lit` (default heap,
    2 x 40 MB). The segment is copied whole and attribute 0's array is rewritten: EDGE's lit draws
    keep 1-3 arrays enabled from earlier draws (8-10 in all), so their raw copies have more than
    one group.
  - The pass is a compute command list executed before the batch's: a group a segment, about 600
    instructions, no indexable temporaries.
  - Fallbacks on the CPU, the port's C model: a draw on another path (its vertexes lit in guest
    memory, once a record), no room, no pipeline.
  - At the gate: ~33,000 lit segments a second (5.7 M vertexes); ~570 a second of the lit layout
    in a ring had no record (captures declined).
- **d, the check** (`GOWA_EDGE_GPU=1 VTX_LIT_CHECK=1`, the gate; `logs/edge-ana/2b1-check3`).
  - Every lit segment of a batch (up to 64) is read back after the batch and compared with the
    SPU's bytes, for the vertexes the segment's first batch names. The vertexes of culled
    triangles inside a batch's index range are lit on the GPU only: the SPU's arrays keep what
    they held there (counted at first, they made 38% of the values look 16 units or more apart).
  - At the gate, 1.4 M vertexes: ids 28-30 as F16 96.1% the same, 3.0% one unit apart, 0.3% two
    or three, 0.4% 4-15, 0.16% more (values near the F16 rule's flush to 0, where a unit is tiny).
    The largest absolute errors: id 29 0.009 (HDR colours up to ~10), ids 28 and 30 0; id 27
    0.04 (a light position near the vertex, where the summed weights are small). Over the menus and
    the load: 91-93% the same, at most 0.03.
  - Screenshots at 40 and 80 s, the GPU's attributes drawn (`VTX_LIT_DRAW=1`) against the SPU's:
    the same lighting.
  - **The largest differences, explained.** Later in the gate (`logs/edge-ana/2b1-check6`) 87% of
    the values are the same and 0.5% 16 units or more apart, and a few vertexes of one job (lights
    6 + 14 + 1) come out at about half the SPU's id 29, frame after frame (1.32 against 0.63).
    The layout of those draws is the usual one. The model shows the same on the replay's dumps
    (l3, call 4, vertex 16: the SPU 0.096, the model with the ring's normal and tangent 0.046, with
    the stage's own S 0.113): the term is a light almost square to the summed direction D
    (D.L = 0.008), and `min(D.L, 1)` is that light's whole share. The ring's tangent
    (X11Y11Z10) turns S by ~1e-3 rad, which moves D by a few 1e-3 and halves such a term. The
    SPU's own rounding has the same reach there, so there is no exact value to match; the
    screenshots show no difference. If one ever shows, the cure is S itself on the GPU (6 bytes a
    vertex next to the record), not a closer formula.
- **e, the bench and the default.**
  - The gate, ABBA, `GOWA_EDGE_GPU` 0 against 2 (`logs/edge-ana/2b1-ab1`): 19.57 and 19.34
    against 20.27, 21.09 and 21.05 flips/s, 19.45 -> 20.80 (+6.9%); every run with the GPU's
    lighting ahead of every run without. A third run without it was left out: a build and replays
    ran beside it (17.25).
  - Again after the stale-record fix below (`logs/edge-ana/2b1-ab2`, two runs a side): 19.77 and
    18.94 against 20.41 and 20.62, +6.0%. All nine clean runs: 19.41 -> 20.69 (+6.6%).
  - So 2 is the default in the game. A replay or a capture (`SPU_JOB_REPLAY`, `SPU_JOB_CAPTURE`)
    defaults to 0: a replay is compared with the SPU's lighting, and a capture records it. With
    the defaults the gate, l2, l5 and out2 corpora replay with 0 differences.
  - `GOWA_EDGE_VERIFY`'s check of the lighting natives needs `GOWA_EDGE_GPU=0`: with 2 the stage
    does not run.
  - The C version of the pass (the fallbacks) was checked in the replay against the SPU
    (`GOWA_EDGE_GPU=1 GOWA_EDGE_GPU_SELFTEST=1`, the gate, l1, l3 and l5 corpora, 47,407 vertexes):
    83-85% the same as F16, 11-12% one unit apart; the largest absolute error 0.4 (id 29, on large
    HDR values, one or two units).
  - The records: a pool of 8,192 slots; `VTX_STATS=1` counts the records written over before
    their draw (none expected: a ring set holds far fewer jobs).
  - **Stale records.** The first run with the default found, ~70 times a second, the record of an
    older job at the same place in a ring, for a job that had none (its capture declined, so the
    SPU lit it). Taken, it would have lit that job with another job's lights. Now every job's output
    stage writes its key (the record, or a clear), and a record carries the first 16 bytes its job
    put out there: the backend takes it only while guest memory holds them (that also covers an
    output stage run lifted, which writes no key).

### 2b.2 Culling on the GPU (done 2026-11-05; on by default)

On by default since a draw's pieces go to the backend in one call: performance plan 2.26. The
text below is as written when it was off.

The plan was a compute pass that culls and writes the count of an indirect draw. The RSX culls
EDGE's triangles already, so neither is needed.

**What EDGE's culling does** (`0xD618` -> `0xD640`, with the vertex pass `0xD250`; both native
since phase 2a, `native/spu_edge_cull.c`).
- A triangle goes when:
  - its three vertexes lie outside one bound (x, y or z);
  - it faces away, by the mode in r4: 0 no culling, 1 no facing test, 2 and 3 the two signs;
  - it is degenerate after snapping to 720p ("trimming", which the Disable Mesh Trimming patch
    turns off: it can only add geometry).
- The kept indexes are compacted in place and their count goes to `c + 0x190`.
- In the game: ~11.6 µs of a lit job's 113.4 (the host reference's split).

**What the RSX does with EDGE's draws.** `VTX_STATS=1` now counts a vertex ring's draws by their
face culling. At the gate all of them, lit or not, are drawn culling back faces (front face CW).
So the RSX culls what the facing test culls, clips what lies outside, and draws no pixel for most
degenerate triangles. EDGE's culling only spares the RSX work, as the performance plan's 3.3
supposed. The stage's own path for no culling (mode 0) leaves the whole index list and its count.
`GOWA_EDGE_NOCULL=1` takes it (the hook at `0xD618` clears r4).

**The cost.**
- Culling keeps 31-37% of a lit job's indexes (the lighting stage's dumps of three corpora; the
  gate's lit jobs 1,416 -> 518 on average).
- Without it the gate's ring draws go from ~6,700 to ~10,000 a frame (pieces of 256 indexes) and
  the indexes from ~1.5 M to ~2.5 M.
- The first try went black and fell to 13 flips/s: past the backend's index buffer (2^21 indexes
  a frame) the batches fall to the CPU's conversion, which dropped most of the geometry. The
  buffer is 2^22 now (32 MB of upload heap), with a note when it fills; the default was at 1.5 M
  of the 2 M.

**The result** (the gate, ABBA, two runs a side, `logs/edge-ana/2b2-ab2`):
- 20.56 and 20.62 against 20.82 and 21.43 flips/s, +2.6%; the screenshots the same.
- But the frame clock thread goes from ~75% to ~91% of a core (the overlay's RSX figure; its
  upload time ~7.9 -> ~9.0 ms a frame, and more draw calls to walk and record).
- In a scene the frame clock thread binds, the extra work would cost more than this gains: off
  by default until that thread has room. The frame clock thread is the next wall in any case:
  at ~75% at 20.6 flips/s it allows ~27 at the gate, below what 2b.3 and 2b.4 aim at.

### The frame clock thread, first round (2026-11-05)

After 2b.2 the frame clock thread (the FIFO walk, the D3D12 backend's recording, the presents)
is the next limit: ~76% of a core at 20.6 flips/s, ~27 flips/s at most at the gate, and 2b.2's
switch takes it to ~91%.

**Where its time goes** (the sampler, 75 s of the gate, `logs/edge-ana/fc-now.rvas`; % of the
thread's samples, idle 23%):
- the prepass, textures and pipeline lookups ~23% (`rf_prepass` 8, `vp_upload_tex_slot` 5.6, the
  texture layouts ~3, `vp_get_fp_pso` 1.8, the programs' hash 1.2);
- the FIFO walk and the RSX methods ~16%;
- the vertex and index uploads ~15% (the ring bytes' `memcpy` 8.5);
- the draws' recording ~11% (the per-draw constants' copy 3.1);
- ZPASS ~2.4%; the vertex pass's CPU lighting for draws off the raw path 1.4%.

**Done** (`rsx_d3d12_backend.c`), work a run of draws repeated:
- **The constant slot.** Every draw wrote its whole constant bank (8 KB) to the upload heap,
  ~20 MB a frame. It is made in host memory now, and a draw takes the slot of the last one written
  in its batch when the two are the same: 84% of the draws at the gate. `VP_CB_SHARE=0` writes
  every slot. (The projection fallback's read of the bank no longer reads the upload heap.)
- **The prepass's units.** A draw whose texture state, depth-read mask and program are the last
  draw's takes its window and its units' targets and copies, without resolving its sixteen units
  again: 33% of the draws. Not from a draw whose window the draw pass rewrites: a depth plane
  (its view becomes null while its surface is the draw's own) or a unit reading the draw's own
  target (`RT_FEEDBACK`'s copy). The lit draws read targets, so these are the rules that let
  them share. `PREPASS_SHARE=0` resolves every draw's.
- **EDGE's spread draws on the raw path.** A first batch whose indexes spread much wider than its
  count went to the ix path, where the vertex pass lights the block on the CPU; one the pass
  lights stays on the raw path. The ring draws off the raw path went to 0, the CPU lighting too.

**Measured** (the gate, ABBA, two runs a side, the thread table every 10 s,
`logs/edge-ana/fc-share2-ab`): the thread 76.8% -> 72.5% (every new reading below every old),
flips 20.55 -> 20.76; the same screenshots. With `GOWA_EDGE_NOCULL=1` on top: 21.27 flips/s,
the thread ~82.6%.

**Next on this thread:**
- the ring bytes' copy (8.5%): a job's pieces copy overlapping index ranges (the ranges add up to
  ~1.7 times the distinct vertexes), so one copy a job would do;
- the FIFO walk (~16%);
- a walk thread and a render thread (the prepass, the recording and the presents on a thread of
  their own): the larger step, for when the rest is not enough.

### The frame clock thread, second round: a render thread (tried 2026-11-05, not merged)

The first round left the thread at ~72.5% of a core, and 2b.2's switch takes it to ~82.6%. The
step the performance plan left for this (2.14, RPCS3's "multithreaded RSX") was tried: the
recording and the presents on a thread of their own. It is on the fork branch
`perf/d3d12-render-thread` (`88bb0c16`), not on `gowa-edge`.

**What was built** (`RSX_RENDER_THREAD`, `rsx_d3d12_backend.c`):
- The frame clock thread (the walk) keeps the FIFO, the capture of each draw (its record and
  uploads), the flips' timing and the window. A render thread, "rsx render", records what the
  walk publishes (all records but the last, which the next batch of indexes may still extend,
  record-ahead's rule; published at each draw), runs the batch's end and the present, and polls
  the ZPASS counts and main-memory surfaces.
- A present, or a render at a display clear, is handed over while the walk waits, answering what
  other threads send to its window. Mode 1: the walk goes on once `render_frame` has reset the
  batch, while the render thread submits and presents. Mode 2: once the frame is presented.
  Mode 3, a diagnostic: the walk records ahead itself and only the batch's end moves.
- The batch's state (records, segments, upload offsets, the parity) is not doubled. What the two
  threads share besides the records, found with a call graph of the backend (the render entry
  points cut off, the globals each side mentions): the raw-fetch variants of the vertex programs
  (a lock), the base pipeline's build (once, under the render thread's lock), and the walker's
  table of ZPASS reports (a lock in `cellGcmSys.c`, against a count checked on the old report
  and written over a new one). Every GPU buffer the walk writes is addressed by the batch's
  parity, and `rf_open` waits for the previous batch before the descriptor windows (indexed by
  record) are rewritten.

**Measured** (the gate, the thread table, `logs/edge-ana/rt-gate1`, `rt-ab`; % of a core over
the level's windows):

| | the walk | the render thread | ppu main | flips/s |
|---|---|---|---|---|
| off | 72-74% | - | 94% | 20.87 (20.73, 21.10) |
| mode 2 | 40-45% | 34-36% | 85-92% | 20.18 (20.40, 19.98) |
| mode 3 | 64% | 10% | 94% | 20.61 (one run) |

- The walk falls by ~30 points and the frame rate does not rise: both mode 2 runs below both off
  runs, ~3% slower; the two threads together take a few points more than the one did.
- Mode 1 put the GPU in a hang (`DXGI_ERROR_DEVICE_HUNG`) in two of four runs, after ~2,000 and
  ~4,000 frames (the menu's picture broke first). It needs both the walk going on past the
  reset and the render thread recording ahead; modes 2 and 3 did not hang in four and two
  runs. Not found: the D3D12 debug layer is not installed here (Windows' "Graphics Tools"), and
  the one run with DRED's breadcrumbs on did not hang.

**Tried again, 2026-11-05,** on the current fork (branch `perf/render-thread-2`), with the gate at
25.3 flips/s and the thread at 91%: 25.34 -> 25.48 flips/s, three runs a side, the walk at 53% and
the render thread at 49%. No gain above the spread, ten runs without a hang. What sets the gate now
is the main thread and EDGE ([the noise review](2026-11-05-general6-noise-review.md)).

**Why no gain:** the SPU workers set the frame, not the frame clock thread, as 2.14 of the
performance plan found on 2026-11-01 and its 2.25 measured again (the thread's work +25%: -1.6%).
The main thread's ~94% is mostly waiting. A second RSX thread only adds CPU. It is the step for
when the frame clock thread binds: 2b.2's switch and 2b.3/2b.4 put more on it, and less on the
SPUs.

### 2b.3 The other stages, back to front

[Reviewed 2026-11-05](2026-11-05-edge-gpu-review.md): the stages left are 37% of a job, its shell
56%, and two things lighting's move left behind have no reader. Proposed order: those two, room
on the frame clock thread (then 2b.2's switch), the job as C code, and the stages after that.

- The normals (`0xA950`), the renormalization (`0xBBC8`).
- Transform (`0xF538`) and skinning (`0xE550`).
- Unpacking (the input pieces) and the index decompression (`0xE4D0`).
- Each moves with the dual-run check, with the stages before it still on the SPU.

### 2b.4 A stub job

- EDGE leaves the SPUs: a job that makes the job manager's calls, the ring allocation, the
  fragment program and the hole, and records the draw for the GPU.
- Needs the open points of the output contract, and the C job's frame.

### Later: zero-copy rings

- **When:** once the frame clock thread binds. Today it copies ~5-6.5 ms a frame of EDGE's ring
  bytes and indices.
- **Option 1:** a D3D12 heap over the guest memory that holds the rings
  (`ID3D12Device3::OpenExistingHeapFromAddress`), with the vertex shader pulling the
  big-endian indices itself.
- **Option 2:** the SPU's PUTs into a ring written to a GPU-visible copy, if nothing but the RSX
  reads the rings.
- Both need a probe first.

## Risks

- **The backend is new ground.** D3D12 and HLSL in the fork (~26,000 lines of backend). The
  Vulkan backend is left as it is.
- **The frame clock thread binds next** (~31 ms of CPU a frame, ~29-32 flips/s). Splitting it into
  a walk thread and a render thread may be needed (plan 2.14).
- **The ring protocol stays as it is.** The hole is written as now, so the labels and semaphores
  that hand the ring back keep working.

## Progress log

- 2026-11-06: [third review](2026-11-06-worker-time-review.md): after G and K, General6 before S or J.
- 2026-11-06: G and K done, on by default ([second review](2026-11-06-edge-gpu-review-2.md), "What came of
  it"). Kept results stay in GPU memory (`GOWA_EDGE_RESIDENT`, `VTX_RESIDENT_MB`, `rsx_kept.h`): gate +10%,
  fight +4%. Skinned jobs hit on their geometry, and the vertex pass applies the palette (`GOWA_EDGE_SKIN`,
  `spu_edge_gpuskin.c`): gate +3.9%, at the game's 30 flips/s, fight +4.7%.
- 2026-11-06: [second review](2026-11-06-edge-gpu-review-2.md). EDGE counts again in both scenes. The
  stages are 18-23% of it, jobs with a kept result 49-63%, and the step around the job manager's `0x17E0`
  12.5 µs of every job. Proposed: kept results in GPU memory, then skinning from cached inputs.
- 2026-11-04: plan written; tolerance checking for GPU stages agreed; 2b.0 started.
- 2026-11-04: 2b.0 done. EDGE's draws already take the raw fetch, so the native draw step was
  folded in, and the steps were renumbered: 2b.1 is now the compute with lighting. Zero-copy
  rings moved to later.
- 2026-11-04: 2b.1 started. The lighting stage mapped (its arguments, the vector it reads as the
  normal, its outputs) and checked by a float model against 41 calls; the design written above.
- 2026-11-04: 2b.1 done. The backend's vertex pass (a compute command list before the batch's),
  EDGE's lighting on it, the SPU's stage skipped: within tolerance of the SPU at the gate, the
  same screenshots, +6.6% at the gate. On by default in the game.
- 2026-11-05: [EDGE's job results, kept](2026-11-05-edge-job-results.md), in place of the review's
  C: EDGE 114 -> 87 ms of worker time a frame with A and B, the gate 22.89 -> 24.40 flips/s.
- 2026-11-05: the review's A and B done (performance plan 2.26): the binormals and ids 27-30 left
  out with the stage, a draw's pieces in one call, the constant bank by generation, no culling on
  the SPU by default. The gate 22.96 -> ~24.1 flips/s.
- 2026-11-05: [review](2026-11-05-edge-gpu-review.md) before 2b.3: a new order proposed.
- 2026-11-05: audio on a host worker of its own (performance plan 2.25): SPU 5 no longer kept for
  it, the gate 20.6 -> 22.7 flips/s (+10%), the fight +4.5%, the menu +5%, audio as before.
- 2026-11-05: the frame clock thread, second round: a render thread, tried and not merged (fork
  branch `perf/d3d12-render-thread`). The walk thread 73% -> 41% of a core, the frame rate ~3%
  lower (20.87 -> 20.18 flips/s): the SPU workers set the frame.
  Its mode 1 (the walk going on past the batch's reset) hung the GPU in two of four runs.
- 2026-11-05: the frame clock thread, first round: the constant slots and the prepass's units
  shared along runs of draws, EDGE's spread draws kept on the raw path. The thread 76.8% ->
  72.5% of a core at the gate.
- 2026-11-05: 2b.2 done as a switch. EDGE's draws are drawn with the RSX's face culling on, so
  the SPU's culling can simply be left out (`GOWA_EDGE_NOCULL=1`): +2.6% at the gate, but the
  frame clock thread ~75% -> ~91%, so off by default. The backend's frame index buffer doubled
  (the first try overflowed it). The frame clock thread is the next wall.
