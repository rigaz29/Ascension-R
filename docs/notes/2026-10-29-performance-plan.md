# Performance plan: RPCS3's speed or better, playable on a mid laptop

Written 2026-10-29, at 12.0-12.5 flips/s on the first level, after a fresh trace and sample of
it. This replaces the order of work in the [performance map](2026-10-28-performance-map.md),
which stays as the record of the 2026-10-28 measurements, and builds on the
[SPU codegen design](2026-10-28-spu-codegen-design.md). The rule is unchanged: a lever is kept
only if an A/B on the level shows more flips/s (or it fixes something), and anything that
changes what the game shows is an option, never a silent default.

## 1. The goal

- **Machine:** Ryzen 5 4600H (Zen 2, 6 cores / 12 threads in two 3-core CCXs, 4 MB L3 each),
  GeForce GTX 1650 Ti 4 GB, 15.4 GB RAM. A mid laptop.
- **Done means**, on this machine, on the charger, at 1280x720:
  - the first level (the Megaera fight) at 30 flips/s or more, median;
  - no frame over 50 ms in a 60 s window outside loading;
  - audio without gaps;
  - the same in the later chapters.
- **RPCS3 on this machine** runs Chapter 3's Guardhouse (the user's save) at 33-40 FPS, measured
  2026-10-28 with the user's settings: PPU and SPU LLVM, XFloat Approximate, SPU block size
  Safe, 6 SPURS threads, Vulkan at 100%, Write Color Buffers on, Multithreaded RSX off.
  - That is not the scene the port is measured on; step 0.3 measures both at one scene.
- **Now:** a frame takes 80.5 ms (median). 30 flips/s means 33 ms, so it has to get 2.4 times
  shorter.

## 2. Where a frame goes now

Measured 2026-10-29 on the first level (fork `5c906fb2`, port `fe46ad6`), from 10 s into it:
- a 12 s `PS3_FTRACE` (146 frames) with `GOWA_WWS_TRACE` line events and `SPURS_WID_PROF`;
- the GPU's engine counters and per-thread CPU over 20 s;
- a 100 s `PS3_SAMPLE_THREAD` profile of the main thread, the six SPU workers, the task threads
  and the frame clock thread (1 ms; the sampler costs ~15% of the flips).

### 2.1 The frame is a chain of SPU job lists

A typical frame (`ft_frame.py`, 2 ms columns):

| ms | what runs |
|---|---|
| 0-26 | RenderJobs (w12) on one SPU, ~40 jobs of ~0.8 ms that the main thread publishes at the frame's start; Batch (w6) beside it |
| 26-32 | MeshProc (w13) |
| 32-80 | EDGE (w14-16) on three SPUs, ~1,600 jobs of ~0.11 ms a frame; General6 (w2) on one SPU from ~28 ms to the end, ~32 jobs of 1.5 ms; RenJobSet (w8-11) from ~50 ms |
| always | audio (w7) |
| RSX | drains in bursts at 2-6 and 34-58 ms; reaches the flip at 62-70 ms, before the chain ends |

So **frame ≈ RenderJobs + MeshProc + max(EDGE on three lanes, General6) ≈ 26 + 5 + 50 ms**. The
lists are serial by design: widening them with `SPURS_MAXCONT` gave nothing (2026-10-28).

SPU time a frame, all lanes: EDGE 171 ms, audio 62, General6 52, RenJobSet 42, RenderJobs 34,
Batch 18, MeshProc 5, others 5. **389 ms on six workers**, 4.7 cores busy.

Per run (`SPURS_WID_PROF`, 30 s):

| workload | us/run | runs/s |
|---|---|---|
| General6 w2 | 1,518 | 389 |
| Batch w6 | 963 | 219 |
| RenJobSet w11 | 812 | 175 |
| RenderJobs w12 | 790 | 525 |
| MeshProc w13 | 418 | 155 |
| audio w7 | 223 | 3,460 |
| EDGE w14-16 | 110 | ~6,400 per list |

### 2.2 Inside an SPU run

Share of each workload's samples:

| | RenderJobs w12 | General6 w2 | EDGE w15 |
|---|---|---|---|
| lifted SPU code | 69% | 95% | 68% |
| runtime: dispatch (`spu_indirect_branch`, `spu_lifted_lookup*` with a lock wait, `spu_lookup`, `spu_drain_call`) | ~12% | ~1% | ~10% |
| runtime: DMA and channels (the copies, `mfc_*`, `spu_wrch`/`rdch`, `spu_overlay_note_get`) | ~13% | ~2% | ~12% |
| coherency (`spu_lockline_lock`, `spu_coh_notify_write`) | ~2% | 0.6% | ~6% |

- **General6 is float code.** Three functions are 63% of it: `4F00` 41% (one straight-line
  block of ~450 SPU instructions), `4AB0` 13% and `63B4` 9%.
- **EDGE:** `EC08` 8%, `A950` and `7BB4` 4% each, the PIC fragments ~9%. The other third of a
  run is the runtime.
- **RenderJobs:** small functions, no float. Its cost is the transfers between them and the
  runtime (`SPU_DIRECT` 13% and function entries 9% of its lifted samples in the design note).
- Over all six workers: 53% lifted code, ~26% runtime, ~2% interpreter, 19% waiting for work.
  - So **the runtime is about 30% of the time the workers are busy.**

### 2.3 The main thread

- 97% of a core. About 65% of its samples poll for SPU progress or wait:
  - **the job-graph scan:** `func_0044CA48`, `0044CD20` and `0044D120`, 28%. By the 2026-10-28
    line profile, three quarters of it reads entries that are not ready.
  - **the progress-counter loops:** `func_0049D688`, `0049B35C`, `0049CB6C`, `0049B2FC` and
    `0049B76C`, 32%.
  - the coherency notifies of the stores in those loops, plus `usleep`.
- **Its own work is ~35% of its samples, ~25-28 ms a frame** (L8 found ~25 ms). It does not set
  the frame now, but at 33 ms it would be the next limit.
- The VMX float helpers run lane by lane, out of line: `vmx_vmaddfp` alone is 4.2% of the
  thread.

### 2.4 The frame clock thread (FIFO walker and D3D12 backend)

- **CPU:** 45% of a core, ~36 ms of CPU a frame.
  - SRV descriptor writes ~14 ms a frame: ~2,000 draws, 16 units each, one
    `CreateShaderResourceView` per unit.
  - Textures ~3.5 ms, PSO lookups ~2.5 ms.
  - The vertex upload (`upload_tris_vp_raw`) ~7% of the thread, `rsx_process_method` 6%.
- **Waiting for the GPU: ~28% of its samples, ~20-25 ms a frame.**
  - `rf_open` drains the GPU completely at the start of every batch, so the upload buffers are
    not overwritten while in use.
  - It was written for test scenes of a few draws ("full serialisation costs little").
  - While it waits, the walker does not walk: reports, labels and the parks the SPU jobs release
    wait with it.
- **Idle** (`pump_messages`): 20%.

### 2.5 The GPU

- The 3D engine is 25% busy on average (36% at most), at **645 MHz of 2,100** (P5).
- It is idle most of the frame and clocked down: there is room for work taken off the CPU.

### 2.6 The task threads and MLAA

- The six `spu job` threads cost 0.2-0.5 of a core (the thread table says 47%).
- 91% of their samples wait for a signal. The MLAA and particle tasks' own code is small.
- **The MLAA work is wasted here.**
  - The title copies each frame to main memory (in 1024- and 256-pixel-wide pieces), its five
    SPU tasks anti-alias the copy, and later draws sample it.
  - In this port the frame exists only on the GPU. The backend's blit snapshots hand those draws
    a GPU copy (`rsx_d3d12_backend.c`, "Blit snapshots").
  - So the tasks process stale bytes, and nothing shows their output.
  - They still sit in the signalling path: an RsxSync job signals them, and they write the RSX
    label and decrement the counter that kicks an EDGE list.

## 3. The budget for 33 ms

| part | now | at 30 flips/s |
|---|---|---|
| serial head: RenderJobs + MeshProc | ~31 ms | ≤ 12 ms |
| EDGE on three lanes, beside General6 on one | ~50 ms | ≤ 20 ms |
| all SPU work | 389 ms on six workers | the chain's lanes plus audio within six workers that also share cores with the threads below |
| main thread's own work | ~25-28 ms | ≤ 20 ms (headroom) |
| frame clock thread | ~36 ms CPU + 20-25 ms waiting for the GPU | ≤ 20 ms CPU, no full-GPU waits |
| GPU | 25% busy at 645 MHz | not a limit |

## 4. What RPCS3 does that the port does not, and the reverse

| area | RPCS3 (the user's settings) | the port now | plan |
|---|---|---|---|
| SPU code | LLVM; SPU registers live in SSA values inside a compiled block and reach memory at its exits | lifted C; every instruction loads and stores `ctx->gpr` | 2.1, 2.2 |
| SPU float | XFloat Approximate: native single precision | round toward zero, bit-exact (range checks, +0 flush) | 1.1 |
| SPU atomics | reservations per 128-byte line | one global lock-line lock; each notify walks ~100 contexts | 1.2 |
| SPU job transfers | compiled code jumps within LLVM functions | `SPU_DIRECT`, lifted-function lookups, MFC channel emulation per DMA | 1.2, 2.2 |
| PPU code | LLVM; registers in SSA | registers, CR and XER in `ctx`; VMX float lane by lane | 2.3 |
| RSX thread | one thread decodes and records; Multithreaded RSX optional | one thread walks, records, and drains the GPU at every batch | 1.4, 1.5 |
| colour buffers to guest memory | Write Color Buffers on (the SPU MLAA needs it) | not written; the frame stays on the GPU, the SPU MLAA output is unseen | 1.6 |
| title patches | Disable MLAA / Depth of Field / Motion Blur / Mesh Trimming; a 60 FPS patch (canary) | none | 4.3 |
| beyond an emulator | - | the title's own code, statically: native replacements for its hottest SPU jobs, GPU passes in their place, profile-guided builds | phase 3, 2.4 |

## 5. The plan

### Phase 0: measure faster (first; the rest depends on it)

- **0.1 `gowa.py bench level`.** The scratchpad A/B runner as a repo tool.
  - It drives to the level and takes 10 s windows from 175 s after the movie.
  - It writes medians and JSON, and compares them with a baseline.
  - The same run records the GPU engine counters and CPU per thread.
- **0.2 SPU job replay.** Capture N jobs of General6, RenderJobs, EDGE and audio at their start:
  the LS image, the registers, and the main memory their DMA reads. Replay them in a loop in a
  test executable, with the time per job and a hash of everything each job writes (LS and DMA
  out).
  - A codegen change is then a deterministic measurement of seconds, with an output check,
    instead of 25 minutes of level runs that can only see the frame.
  - Ref A built the same for its EDGE work (capture, replay, replace); we take the idea, not the
    code.
- **0.3 RPCS3 and the port at one scene**, with RPCS3's performance overlay on the same windows.
  Either option needs the user's OK:
  - (a) the user's Chapter 3 save in the port: a copy of `BCUS98232-PROFDATA-*` into the port's
    `gamedata`;
  - (b) a new RPCS3 profile, to reach the first level.
- **0.4 One frame report:** `ft_groups`, `ft_chain`, `ft_frame` and `ft_sema` from the
  scratchpad into `tools/`.

### Phase 1: the chain's cheapest time, waits and waste

Each item is small and is measured on its own.

- **1.1 Approximate SPU float** (design step 3: `SPU_FLOAT_APPROX` at compile time).
  - What: native `addps`/`mulps`/FMA, no range checks, no +0 flush. This is RPCS3's setting for
    this title.
  - Evidence: compiled x86 instructions fall from 7,709 to 3,135 in General6 `4F00`, and from
    4,745 to 1,786 in EDGE `A950`.
  - Expected: General6 and EDGE runs 20-35% shorter (estimate).
  - Check:
    - on the replay, how many outputs differ and by how much;
    - screenshots at fixed times against the exact build;
    - a play-through of the first level.
- **1.2 The SPU runtime's share** (~30% of a RenderJobs or EDGE run):
  - **dispatch:** an indirect branch looks its target up through `spu_lifted_lookup` and the
    "full" lookup with its lock. Cache the target per call site or per module.
  - **DMA:** a fast path for the common sizes, and list DMA without channel emulation per
    element.
  - **coherency:** notify only lines with a live reservation; stripe the lock-line lock by line.
  - Coherency holder counts and `preserve_none` were tried on 2026-10-28, when the chain did not
    set the frame. Each moved its piece and not the frame. Measure again here, on the replay and
    then the level.
- **1.3 The main thread's polls sleep.**
  - Replace the job-graph scan's wait loop and the progress-counter loops (`func_0044CD20`,
    `func_0049B76C` / `func_0049D688`) at their call sites with a host wait on the words they
    read.
  - The SPU's atomic commits and DMA writes to those lines wake it. The runtime already sees
    every commit (`PS3_FT_LINE`).
  - This gives back most of a core that the six workers share the six physical cores with, and
    the coherency notifies of those loops.
  - Measure: the workers' us/run (less SMT sharing), then flips.
- **1.4 No full GPU drain per batch.**
  - Give the vertices, the constants and the texture staging FRAME_COUNT rings each.
  - `rf_open` then waits only for the batch that last used the same ring.
  - Expected: the walker stops standing still for 20-25 ms a frame, and the reports, labels and
    parks the SPU jobs wait for move earlier.
  - Risk: the torn geometry the drain was added for. Check it with the capture tools.
- **1.5 Descriptors.**
  - One SRV per cached texture, made at upload. A draw's 16-entry table is copied
    (`CopyDescriptorsSimple`), or reused when it equals the last draw's.
  - Expected: ~14 ms down to ~2-3 ms a frame of the frame clock thread.
  - The null-SRV skip (-4 ms) gave +1% when that thread did not limit; this is for when it does.
- **1.6 MLAA on the GPU.**
  - Answer the five tasks at their signal: the label, the counter and the kick, without the work.
    Ref A does the same; check it with our own probe.
  - Run an SMAA (or FXAA) pass on the GPU at the snapshot the title samples, so the game gets
    its anti-aliasing back. The port shows none today.
  - It takes the tasks out of the RsxSync-to-EDGE path; the GPU pass is under 1 ms at 720p
    (estimate).
- **1.7 Thread placement on Zen 2.**
  - CCX-aware affinity: the SPU workers and the coordinator in one CCX, the main and frame clock
    threads in the other.
  - Five against six SPU workers, and the audio worker's priority.
  - RPCS3 has the same knobs: thread scheduler mode, max SPURS threads.

### Phase 2: code generation, the work RPCS3 does in LLVM

- **2.1 SPU registers in locals where it pays** (design step 6).
  - Write back only the dirty registers, reload only the ABI's volatile ones, and promote only
    functions whose barriers sit in loops.
  - Judge it by replay cycles. Static instruction counts (2026-10-28) said it grows the code;
    they cannot see the store-to-load chains it removes.
  - General6 `4F00`, ~450 instructions in one block, is the case where RPCS3's SSA pays most.
- **2.2 Cheaper transfers between lifted functions:** the checks in `SPU_DIRECT`, function
  entries, drain calls; `preserve_none` and `--merge-chunks` measured again on the replay. These
  are RenderJobs' 13% and 9%.
- **2.3 PPU:**
  - `PORT_PPU_FAST_FP` on: host NaNs, compared by screenshots. It made hot float code markedly
    shorter in God of War HD.
  - The VMX float helpers as vector code.
  - `-mavx2` for the lifted PPU code (it gets FMA, so AVX, today).
  - Then guest registers and CR fields in locals inside a function, with 2.1's rules, starting
    with the main thread's hottest functions.
- **2.4 Profile-guided builds** of the lifted code (clang's instrumented build, profiles from the
  bench route).

### Phase 3: native and GPU replacements, beyond an emulator

- **3.1 Native hot functions.**
  - Targets: General6's `4F00`, `4AB0` and `63B4`, and EDGE's stages (skinning, per-vertex
    lighting, transforms, culling; ref A's stage map names them).
  - Each as native AVX2 code behind a switch, checked against the replay: bit-exact, or within
    1.1's tolerance.
  - Ref A measured +8-9% in game for each EDGE stage.
- **3.2 Audio** (w7, 0.77 of a core at any frame rate): after 1.1 and 2.1, or its mixer as
  native code.
- **3.3 EDGE on the GPU** (research). EDGE is the largest SPU consumer, 171 ms of SPU time a
  frame.
  - Once 3.1 knows the stages' inputs and outputs, a compute pass could produce the vertex
    streams the RSX draws.
  - Its culling is there to spare the RSX, and this GPU has room.
  - First question: does anything but the RSX read what EDGE writes?
- **3.4 Vertex and index data read by the GPU from guest memory**, without upload copies:
  `ID3D12Device3::OpenExistingHeapFromAddress`, or upload rings filled by the walker only for the
  ranges a draw uses.

### Phase 4: playable and comfortable

- **4.1 Frame pacing:** a steady 30 when 60 cannot be held. The title flips on vblank; present
  every second one.
- **4.2 Resolution scale with FSR 1 / CAS:** the GPU has the room. RPCS3's Disable Mesh Trimming
  patch is what lets the title draw correctly above 720p.
- **4.3 RPCS3's patches as port options**, applied to the ELF and SPU images before lifting:
  - Disable MLAA (with 1.6, the title also stops its copies);
  - Depth of Field, Motion Blur;
  - Mesh Trimming;
  - later, the 60 FPS patch.
- **4.4 Hitches:** first-play pipeline and texture stalls, and pipeline cache warm-up from the
  disk cache.

## 6. What the phases should add up to

These are estimates, each to be replaced by its measurement. They take the chain of section 2.1
and shorten each link by the shares in section 2.2.

| after | RenderJobs + MeshProc | EDGE ∥ General6 | frame | flips/s |
|---|---|---|---|---|
| now | 31 ms | ~50 ms | 80 ms | 12.3 |
| phase 1 (approximate float on General6/EDGE; the runtime's ~30% cut by a third; less SMT sharing) | ~27 | ~36-38 | ~62-65 | ~15-16 |
| phase 2 (registers in locals; transfers; PGO) | ~19 | ~25 | ~45 | ~22 |
| phase 3 (native hot functions; audio) | ~15 | ~17 | ~33 | ~30 |

- RPCS3 gets its speed with phase 1 and 2 kinds of work: LLVM register allocation and
  approximate float.
- Phase 3 is what a static port can add on top.
- Whether the first level is heavier than RPCS3's Chapter 3 scene is step 0.3's question.
- Per step, the main thread and frame clock thread budgets (section 3) are checked too. 1.3,
  1.4, 1.5 and 2.3 keep them under the chain.

## 7. Tried before, and what comes back

| lever | when | result | now |
|---|---|---|---|
| `/GS-` on lifted SPU code | 2026-10-28, 7.3 flips/s | no change | with 2.2, on the replay |
| `preserve_none` for lifted SPU code | 2026-10-28 | EDGE function entries 5.1% -> 2.7% of its lifted samples; frame unchanged | 2.2 |
| coherency holder counts | 2026-10-28 | coherency 5.5% -> 4.5% of an EDGE run; frame unchanged | 1.2 |
| job modules with `--merge-chunks` | 2026-10-28 | 1-6% off some workloads, frame unchanged, +13 MB of C | 2.2 |
| null SRV left alone | 2026-10-28 | -4 ms a frame of CPU, +1% | 1.5 (the whole descriptor path) |
| widening the serial lists (`SPURS_MAXCONT`) | 2026-10-28 | nothing: the jobs come one at a time | ruled out |
| more frequent SPURS idle probes | 2026-10-28 | hangs | ruled out |

They were measured while something else set the frame (the texture work on the frame clock
thread, the main thread's waits). The chain sets it now, so what shortens a chain job counts.

## 8. How each step is measured

- One change at a time. Two runs a side, alternated. Medians of 10 s windows from 175 s after
  the movie.
- On the charger. `D3D12_VSYNC=0` for unattended runs: on battery the display turns off after
  30 minutes and vsync'd presents drop to ~3.8 flips/s.
- Frame-rate claims with only `GOWA_THREADS` on; the trace and sampler runs are for the
  breakdown.
- SPU code changes: the replay (0.2) first (time per job, output hashes), then the level.
- Anything visible: screenshots at 150, 250 and 350 s against the previous build, and a
  play-through.
- RPCS3 is a behaviour reference only (its GPL code is not copied). The other ports' notes are a
  map; every claim is measured here first.
