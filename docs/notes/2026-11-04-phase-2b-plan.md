# Phase 2b: EDGE on the GPU, the plan and its progress

Written 2026-11-04, branch `edge` (fork: `gowa-edge`). This note is the plan for phase 2b and
the place its progress is kept: each step's status is updated here as it moves.

## Status

| step | what | status | commits |
|---|---|---|---|
| 2b.0 | measure: EDGE's draws, vertex bytes and backend time a frame; the GPU's load | done | fork `b1121f5d` |
| 2b.1 | the backend's compute, and the first stage on the GPU: lighting | not started | |
| 2b.2 | culling on the GPU, with indirect draws | not started | |
| 2b.3 | the other stages, back to front | not started | |
| 2b.4 | a stub job: EDGE off the SPUs | not started | |
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

### 2b.1 The backend's compute, and lighting on the GPU

- **The compute.** A compute root signature and pipelines in `rsx_d3d12_backend.c`, the barriers
  between a dispatch and the draws that read its output, and a per-draw record that tells the
  backend what the GPU has to do for an EDGE draw.
- **Where the GPU reads.** The raw fetch copies the draw's ring bytes into the vertex upload
  buffer. The compute shader reads them there and writes the lit attributes before the draw.
- **Why lighting first.**
  - ~15% of the EDGE workers' time: `0x73E0`, its native loops, the point lights `0x6AC8` and
    the set-up.
  - Its outputs (attributes 27-30) are read only by the GPU.
  - Its inputs are in the ring data (position, normal, tangent) and the light set.
  - The index count does not change.
  - Plan 2.22: skipping lighting gave +14% at the gate, when its share was larger.
- **How.**
  - The SPU job skips lighting.
  - The output stage native publishes a record for the draw: its ring address and a copy of
    the light set and parameters.
  - A compute shader writes attributes 27-30 into the GPU copy of the draw's vertices.
- **Check:** the dual-run mode against the native lighting (tolerance), and screenshots.
- **Expected:** ~+8-10% at the gate.

### 2b.2 Culling on the GPU

- Culling (`0xD618`: `0xD640` and `0xD250`) compacts the index list, so the index count is known
  only after it.
- The draw becomes indirect: the compute shader writes the count.

### 2b.3 The other stages, back to front

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

- 2026-11-04: plan written; tolerance checking for GPU stages agreed; 2b.0 started.
- 2026-11-04: 2b.0 done. EDGE's draws already take the raw fetch, so the native draw step was
  folded in, and the steps were renumbered: 2b.1 is now the compute with lighting. Zero-copy
  rings moved to later.
