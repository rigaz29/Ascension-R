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

### The level trace (10 s, 90 frames, median 97 ms)

`PS3_FTRACE` with the new `PS3_FT_LINE` events (every atomic commit to a job list's ticket row,
with who made it; `GOWA_WWS_TRACE=1` registers the rows), `PERF=1`, `nvidia-smi`. The first
attempt was stopped twice for low system memory: commit charge was 21.8 of 27.5 GB, 7.1 GB of it
NVIDIA Overlay's private memory; with the overlay restarted, 13.5 GB.

Per frame, averaged over the 90 frames (`ft_groups.py`):

| | SPU time | time with a run open | runs at once |
|---|---|---|---|
| EDGE w14-16 | 189 ms | 73 ms | 2.6 |
| General6 w2 | 54 ms | 54 ms | 1 |
| RenderJobs w12 | 32 ms | 32 ms | 1 |
| RenJobSet w8-11 | 39 ms | 25 ms | 1.55 |
| Batch w6 | 17 ms | 17 ms | 1 |
| MeshProc w13 | 5 ms | 5 ms | 1 |
| audio w7 | 81 ms | 81 ms | 1 (SPU 5 only) |
| all SPU work | **~420 ms** on 6 workers | | 4.35 busy on average |

- Main thread: 74 ms outside `usleep` (its own work plus the polls without a syscall), 28 ms in
  the wait for the RSX (`ref`), 7 ms in the EDGE counter's.
- RSX drain: 14 ms a frame. **D3D12 backend in the level: 50-85 ms a frame** on the same thread
  (textures 28-59 ms with 5,800-7,300 binds a frame, SRV writes 35-67 ms, recording 6-20 ms);
  `frame clock` 49% of a core.
- GPU: 27% busy on average in the level, 43% at most.
- Who publishes: the main thread publishes RenderJobs (355 jobs/s, ~4 a frame), MeshProcJobs
  (45/s) and RsxSync's four lists (~12/s each). **The EDGE lists publish their own jobs**
  (w14/15/16 100%, ~5,000/s each, each publish matched by one take): they run as self-renewing
  chains, fed by what RenderJobs and the main thread set up.

A typical frame (`ft_frame.py`, frame 27, 102 ms): RenderJobs runs on one SPU, run after run,
from 0 to 34 ms while the main thread works and polls; MeshProc 34-40 ms; the main thread waits
for the RSX 36-64 ms; EDGE runs two to three wide 36-80 ms; General6 34-70 ms; RenJobSet 66-94
ms; the RSX reaches the flip at ~100 ms and the main thread asks for the next one at 102 ms.

## 3. The chain that sets the level's frame

1. The main thread waits until the RSX has finished the previous frame (`ref`).
2. The RSX's FIFO walker reaches holes in the command stream that EDGE jobs fill, and waits.
3. The EDGE lists are mostly empty: their jobs arrive in bursts, from a producer not yet named.
4. The walker shares its thread with the whole D3D12 backend: while that thread converts
   textures, writes descriptors and records draws, the walker does not walk, and a park's
   "waiting" time includes that.

Two hypotheses, settled by the level trace above:

- **H1** (the frame clock thread is the cap): **not the limit today.** The drain shows as short
  bursts where the SPU chain lets it go on; the backend's 50-85 ms a frame fits in the gaps. But
  it is more than a 30 flips/s frame on its own, so it is the next limit once the SPU chain
  shortens.
- **H2** (the EDGE producer publishes late): **resolved differently.** EDGE feeds itself; what
  sets the frame is a **serial chain of SPU job lists** -- RenderJobs, MeshProc, EDGE, General6,
  RenJobSet -- with the main thread waiting for the RSX in the middle. Most of these lists run on
  one SPU at a time because the title gives every workload a maximum contention of 1; EDGE was the
  same until `SPURS_MAXCONT` let it use three SPUs, with the same geometry.

Widening the serial lists does not help: with `SPURS_MAXCONT=2=3,6=2,8=2,9=2,10=2,11=2,12=3,
13=2,14=3,15=3,16=3` the level ran 7.35-10.14 flips/s (median ~9.45, the same as without), and
the extra lanes ran empty (General6 24% -> 42% empty runs, Batch 24% -> 35%). Their jobs come
one at a time, each after the last. **What sets the frame is how long one SPU job takes**, along
a chain the title runs serially by design -- not SPU throughput or parallelism. On the PS3 the same
chain fits in a frame because each job is short; RPCS3 gets close with fast SPU code.

So, in order: **the latency of SPU job code** limits the frame now; the D3D12 backend's CPU
time is the next wall; the main thread's own work (~25 ms) after that.

## 4. The gap, by part

### SPU (4.3 of the 6.6 busy cores)

The frame waits on a serial chain of SPU jobs (section 3), so what matters first is how long
one job takes, not how many SPUs there are: each job has to get about 3x faster for 33 ms. The
total (~420 ms of SPU time a frame) has to roughly halve as well to fit six workers at 30 flips/s.

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
| serial SPU chain (RenderJobs, MeshProc, EDGE, General6, RenJobSet, with the main thread's waits) | ~97 ms | ≤ 33 ms: each job ~3x faster |
| all SPU work | ~420 ms on 6 workers (audio 81 ms of it, fixed in time) | ≤ ~200 ms |
| frame clock thread (walker + D3D12 backend) | 64-100 ms (14 drain + 50-85 backend) | ≤ 20 ms |
| main thread's own work | ~25 ms | ≤ 25 ms, with its waits no longer spinning |
| GPU | 27% busy in the level | not the limit |

## 6. The map, in order

Each item is kept only if an A/B on the level shows more flips/s, as before.

**0. Measure first**
- 0.1 Done: the level trace above (who publishes, the per-frame chain, the backend, the GPU).
- 0.2 RPCS3 at the same point, with and without its patches.
- 0.3 Where General6, RenderJobs, RenJobSet and EDGE jobs spend their time (`PS3_SAMPLE_THREAD`
  with workload tags, `--spu-lines`): which lifted functions, which helpers, how much is runtime.
- 0.4 D3D12: texture uploads against cache hits a frame, SRV writes a frame, GPU time from
  timestamp queries; backend spans in the ftrace.

**1. SPU code generation** (sets the frame now: the latency of each serial job)
- 1.1 Registers in locals inside a lifted function, written back at calls, exits and runtime
  entries; guest memory stores marked so they do not alias the context. The largest single
  change, and what RPCS3's SPU LLVM does.
- 1.2 Cold paths out of line (LS diagnostics compiled out of release builds, float fix paths in a
  cold section); a lifted-to-lifted convention without the Windows x64 frame (`preserve_none`,
  measured again now that SPU job latency is the limit).
- 1.3 An approximate float option, as RPCS3's default.
- 1.4 Lock-line lock per line (striped) and per-line reservation holders for the notify.
- 1.5 Audio (w7, 0.8 of a core at any frame rate, alone on SPU 5).
- Ruled out: running the serial lists on more SPUs (`SPURS_MAXCONT`, above); honouring SPURS
  priorities; more frequent back-off probes; waking the walker on a park's release.

**2. RSX/D3D12 backend** (the next wall: 50-85 ms a frame in the level)
- 2.1 Texture cache keyed by (offset, format, size) in a hash map, sized by memory, not 192
  slots; skip the checksum for textures whose guest pages nobody wrote (a per-page dirty bit
  from the PPU store path, SPU DMA and the walker).
- 2.2 One persistent SRV per cached texture; per draw only copy descriptors (or a bindless
  table).
- 2.3 Walker on its own thread, feeding the backend through a queue; the walker never waits
  for recording.

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
