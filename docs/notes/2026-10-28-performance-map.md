# Performance map: what stands between the port and RPCS3

Written 2026-10-28, after L6 (first level 9.4 flips/s median, main menu ~23, opening cinematic
~50). The new target is RPCS3's speed on this machine or better. This note collects what is
measured, what is only suspected, and the work in the order it should pay.

Machine: Ryzen 5 4600H (6 cores / 12 threads, 3.0 GHz), GeForce GTX 1650 Ti 4 GB (the D3D12
backend picks it through `EnumAdapterByGpuPreference`), Radeon iGPU, 15.4 GB RAM.

## 1. The target

- RPCS3's compatibility list still rates God of War III and Ascension "Ingame", and the reason
  given is performance. There is no public frame rate for a Ryzen 5 4600H.
- RPCS3 on this machine runs the title with PPU and SPU LLVM, **XFloat: Approximate** (not
  bit-exact), Vulkan at 1280x720, async shader compilation, Multithreaded RSX off, 6 SPURS
  threads (`config/custom_configs/config_NPUA80918.yml`).
- RPCS3's patch file has, for GOWA.SELF 1.12: Disable MLAA, Disable Depth of Field, Disable
  Motion Blur, Disable Mesh Trimming. RPCS3 recommends the first three for speed. MLAA runs on
  the SPUs (the five post-process tasks), so that one removes SPU work.

**Not measured yet: RPCS3's frame rate at the same point** (first level, 10 s in), with and
without those patches, with its performance overlay (frame rate, frame time, PPU/SPU/RSX thread
load). Every comparison below needs that number.

## 2. Where a first-level frame goes now (9.4 flips/s, ~106 ms)

| | measured | source |
|---|---|---|
| process CPU | 6.6 of 12 logical CPUs | `GOWA_THREADS` |
| `spurs pm` x6 (SPU policy modules) | 427% | same |
| `ppu main` | 98%, most of it waiting in spins and polls (81% of its samples before L6) | same, [L8 notes](2026-10-28-l8-main-thread.md) |
| `frame clock` (FIFO walker, D3D12 backend, present) | 53% | `GOWA_THREADS` |
| `spu job` x6 (taskset tasks: MLAA, particles) | 36% | same |
| SPU work by workload | audio w7 0.8 core (does not scale with the frame rate); EDGE w14-16 0.6 each (~4,100 jobs/s per list, 120-130 us a job); General6 w2 0.55; RenderJobs w12 0.3; RenJobSet w8-11 0.4; BatchJobList w6 0.16 | `SPURS_WID_PROF` |
| frame-proportional SPU work | ~3.4 cores, ~360 ms of host core time a frame | same |
| main thread's own work | ~25 ms a frame | [L8 notes](2026-10-28-l8-main-thread.md) |
| main thread's waits | `usleep` at 0x3A3698 27 ms a frame (until RSX `ref` reaches the previous frame: one frame ahead at most); EDGE frame counter 7 ms; the job-graph scan and progress counters poll without a syscall | ftrace, `func_003A3670` |
| RSX walker | stands at self-JUMP parks 61-68% of the time; every park is released by an SPU PUT; it moves on ~17 ms after the PUT (2 ms with a wake on the PUT, no frame-rate change) | `GCM_PARK_PROF`, `GCM_PARK_KICK` |
| EDGE lists | empty 79% of the time, never pending: a job is taken as soon as it is published | `GOWA_WWS_WATCH` |
| D3D12 backend on the frame clock thread | ~30 ms a frame at 13-15 flips/s before the level; **~95 ms a frame where the level starts** (textures 62 ms, 7,500 texture binds a frame, SRV writes 70 ms in all) | `PERF=1` |
| GPU | 30-34% busy in the cinematic and menus | `nvidia-smi` |
| `ref` publication | about one fence a frame, queued 0.1-0.3 ms | `GCM_REF_STATS` |

Not measured in the level yet: the D3D12 backend's steady state, GPU load, and who publishes the
EDGE jobs. The run that was to measure them (ftrace with the new `PS3_FT_LINE` events, `PERF`,
`nvidia-smi`) was stopped twice for low system memory: commit charge was at 21.8 of 27.5 GB,
7.1 GB of it NVIDIA Overlay's private memory.

## 3. The chain that sets the level's frame

1. The main thread waits until the RSX has finished the previous frame (`ref`).
2. The RSX's FIFO walker reaches holes in the command stream that EDGE jobs fill, and waits.
3. The EDGE lists are mostly empty: their jobs arrive in bursts, from a producer not yet named.
4. The walker shares its thread with the whole D3D12 backend: while that thread converts
   textures, writes descriptors and records draws, the walker does not walk, and a park's
   "waiting" time includes that.

Two hypotheses, both settled by one level trace (section 6, step 0.1):

- **H1**: in the level, the frame clock thread is the cap: walker plus D3D12 work per frame is
  most of the frame. Supported by: the backend's 30-95 ms a frame, the 17 ms from a park's release
  to the walker moving on, SPU workers and EDGE lists idle much of the time, GPU mostly idle.
- **H2**: the EDGE jobs' producer (the main thread, or an SPU job such as General6) publishes
  late, and the frame waits for that chain.

## 4. The gap, by part

### SPU (4.3 of the 6.6 busy cores)

At 30 flips/s the frame-proportional work above would need ~11 host cores. With the PPU, the
walker/backend and audio threads taking ~3, it has to get about 3.5x cheaper, or shrink.

Why lifted SPU code costs what it does (from the code and the profiles):

- **Registers live in memory.** Each instruction loads and stores `ctx->gpr`; guest memory is
  written through byte pointers, which may alias the context, so nothing stays in a register
  across a store.
- **Every lifted function has a full Windows x64 frame**: 8 pushes, the ten callee-saved xmm
  registers stored and reloaded, a `/GS` cookie; `SPU_DIRECT` transfers and `brsl` calls pay it
  each time. In the hottest EDGE function (`jobmod_e8f44fbc6dbf_e6D98_spu_func_0000EC08`,
  28 KB, 4,767 x86 instructions) 28% of the instructions are stack operands.
- **Cold paths sit in hot code**: 35 LS-diagnostic calls and 55 float range-fix calls in that
  function; they are rarely taken but constrain register allocation around them.
- **Exact float.** SPU single precision is bit-exact (round toward zero, range checks).
  RPCS3's default is approximate.
- **One global lock** (`g_lockline`) for every SPU reservation, PUTLLC/PUTLLUC and coherent PPU
  store; each notify walks ~100 contexts to find the ~50 holding a reservation.
- Audio (w7) holds 0.8 of a core whatever the frame rate.

What RPCS3 does: SPU LLVM keeps SPU registers in SSA values across a function and only spills
at calls, has its own calling convention, approximates float by default, and tracks reservations
per line.

### RSX and the D3D12 backend

- **One thread** walks the FIFO, prepares and records draws, and presents.
- **Texture cache: 192 slots** (`VP_TEX_SLOTS`), found by a linear scan, LRU eviction, and a
  sparse checksum of the guest bytes at every bind of a texture from an earlier frame. With
  thousands of binds a frame and more than 192 live textures it re-converts and re-uploads every
  frame. Suspected for the 62 ms; to be confirmed by counting uploads against hits.
- **SRV descriptors are written per draw and per texture unit.**
- No GPU timing (timestamp queries); `PERF` times the CPU side only.

What RPCS3 does: a texture cache keyed by address and invalidated by write-protecting the
guest pages, cached descriptors, optional Multithreaded RSX for vertex work.

### PPU

- The main thread's own work is ~25 ms of a 33 ms budget at 30 flips/s.
- The PPU lifter keeps registers in `ctx` memory, with the same aliasing as the SPU's. RPCS3's
  PPU LLVM keeps them in SSA values.
- Short waits spin: `usleep` under 200 us is a `SwitchToThread` loop, and the job-graph and
  counter polls spin in guest code. The main thread holds a logical CPU at ~98%, often the SMT
  sibling of an SPU worker's core.

### Bugs and races that cost speed or block measurement

- `SPURS_IDLE_MAX=0/1` (more frequent probes) hangs the game: a race when a policy module runs
  empty very often. Possibly the same as the intermittent FIFO park hang (~2 runs in 20).
- `D3D12_RECORD_AHEAD=0` hangs the device.
- Rendering differences from RPCS3: a black patch for the sky in the level, missing pause-menu
  labels.
- Memory: the runs need several GB of commit; see section 2.

## 5. Budget for 30 flips/s (33 ms) on this machine

| | now, per frame | needed |
|---|---|---|
| main thread's own work | ~25 ms | ≤ 25 ms, with its waits no longer spinning |
| frame clock thread (walker + backend) | ~45-110 ms (to confirm in the level) | ≤ 20 ms |
| frame-proportional SPU work | ~360 ms of core time | ≤ ~100 ms (~3 cores) |
| GPU | 30-34% at lighter scenes | not the limit yet |

## 6. The map, in order

Each item is kept only if an A/B on the level shows more flips/s, as before.

**0. Measure first**
- 0.1 Free memory (restart NVIDIA Overlay or turn its in-game overlay off), then one level run
  with `PS3_FTRACE` + `GOWA_WWS_TRACE=1` (who publishes EDGE jobs), `PERF=1`, `nvidia-smi`:
  settles H1/H2.
- 0.2 RPCS3 at the same point, with and without its patches.
- 0.3 D3D12: texture uploads against cache hits a frame, SRV writes a frame, GPU time from
  timestamp queries; backend spans in the ftrace.

**1. RSX/D3D12 backend** (if H1 holds; likely the largest near-term gain)
- 1.1 Texture cache keyed by (offset, format, size) in a hash map, sized by memory, not 192
  slots; skip the checksum for textures whose guest pages nobody wrote (a per-page dirty bit
  from the PPU store path, SPU DMA and the walker).
- 1.2 One persistent SRV per cached texture; per draw only copy descriptors (or a bindless
  table).
- 1.3 Walker on its own thread, feeding the backend through a queue; the walker never waits
  for recording.

**2. SPU code generation**
- 2.1 Registers in locals inside a lifted function, written back at calls, exits and runtime
  entries; guest memory stores marked so they do not alias the context.
- 2.2 Cold paths out of line (LS diagnostics compiled out of release builds, float fix paths in a
  cold section); a lifted-to-lifted convention without the Windows x64 frame (`preserve_none`,
  measured again now that the SPU side may be on the path).
- 2.3 An approximate float option, as RPCS3's default.
- 2.4 Lock-line lock per line (striped) and per-line reservation holders for the notify.
- 2.5 Profile audio (w7, 0.8 core).

**3. PPU**
- 3.1 Register caching in the PPU lifter.
- 3.2 Waits that sleep and are woken by what the guest waits for (`ref` publication, SPU
  commits) instead of spinning.
- 3.3 The main thread's hottest functions (job-graph scan) once 3.1 is in.

**4. Title options, as RPCS3 offers them**
- 4.1 Patches the player supplies (RPCS3's patch file format), applied to the ELF and the SPU
  images before lifting: MLAA off (the host's SMAA can take its place), depth of field, motion
  blur.
- 4.2 Unlocked frame rate and pacing.

**5. Beyond RPCS3** (what a static port can do that a general emulator does not)
- 5.1 Native replacements for the heaviest SPU jobs: MLAA as a GPU pass, EDGE geometry on the
  GPU or as native SIMD.
- 5.2 Profile-guided and link-time optimization of the lifted code, per-title specialization.

**In parallel**: the probe-frequency race and the park hang, `D3D12_RECORD_AHEAD=0`, the
rendering differences.
