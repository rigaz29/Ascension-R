# EDGE's job results, kept

Written 2026-11-05, branch `edge`. This is what came of option C of the
[review](2026-11-05-edge-gpu-review.md), the job as C code. The code is
`native/spu_edge_memo.c`; `GOWA_EDGE_MEMO=0` turns it off.

**The result:** most of a frame's EDGE jobs repeat a job already done, so their results are kept and
the stages left out. The gate went from 22.89 to 24.40 flips/s, with A and B before it (performance plan 2.26). The warm fight
went from 21.17 to 21.56. Every checked result was exact: 0 lists and 0 vertexes differ in 4.5 M checked jobs.

## Why not the glue first

C's plan was to port the lifted glue function by function, position-only jobs first. The sampler
says that is a poor trade now:
- the glue is 12.6% of the EDGE workers' samples, spread over 277 functions;
- the largest is 0.9% (`0x8930`), then 0.8% (`0xE550`), 0.77% (`0xBFB8`), and after the first six
  each is under 0.3%;
- the job manager is 8.7% over 113 functions, flat in the same way;
- a function's port gives at most ~0.4% of the SPU work, and the review's estimate (-15 to -25% of
  EDGE) needs nearly all of them.

## What the probe found

With culling left to the RSX (2b.2), a job's indexes and vertexes no longer depend on the camera: it
comes in through the vertex program. A temporary probe hashed each job's geometry inputs in the game:

| | gate: share of jobs | seen before | fight: share of jobs | seen before |
|---|---|---|---|---|
| position only | 39.5% | 96.2% | 33% | 92.7% |
| lit | 41.3% | 95.3% | 34% | 91% |
| position only, skinned | 7.8% | 0.1% | 16% | 2.3% |
| lit, skinned | 11.2% | 0.5% | 17% | 2.6% |

So 77% of the gate's jobs and 62% of the fight's redo work already done: the same meshes every
frame, and each mesh in the depth pass and the lit pass. What misses is what moves: the matrix
(slot 18) in 7-9% of the unskinned jobs, and slot 4 in 5-14% of the repeats.

## How it works

- **The key.** A 128-bit hash of what the geometry depends on: slots 4, 5, 11, 13, 14, 16 and 18, the
  three pieces' EAs, the slots' sizes, and the descriptor words that shape a job (14, 18, 19, 24-27,
  32, 33, 35-38 and the high half of 34). Not the light set, not the view block.
- **Where it is taken.** At the end of `0x9990`, whose table names the job's own buffer set. The
  job's entry can only guess it from the previous job's, and after another module's jobs the guess
  is wrong: a first version hashed the other set, and 1 hit in 80,000 was another job's result.
- **Unskinned jobs only**, of the two common output pieces. Lit jobs only with the lighting stage
  skipped and culling left out.
- **A miss** keeps the list and the records, the second time the key is seen: a moving object has
  a new key every frame. With that, the gate holds 17 MB and the fight 39 MB (two arenas of 128 MB).
- **A hit leaves out:**
  - the decoding in the input pieces' natives, `0x75BD10` and `0x764D10` (they still fill the
    attribute list and the context's table: hooking `0xDBC8` above them left the list empty);
  - the transforms, `0xF538`;
  - the index decompression, `0xE4D0`: the list is copied back, and `c + 0x50` and `c + 0x58` set;
  - the output pieces' packing, `0x755B80` and `0x765B70`: the records are copied back, 24 bytes of
    a lit vertex, 12 of a position.
- **The glue around them runs as before**, so the job manager, the rings and the hole are the
  lifted code's, and the lighting record is taken from this job's lights.
- **Giving a result up.** A lit job whose stage runs on the SPU after all needs the arrays:
  `edge_memo_revoke` makes them late, from the arguments kept. Three things did not survive until
  then and are kept: the matrix (`r6` points at a frame), the stream (later allocations reuse it, so
  a lit result carries its stream), and the binormals, which run after. In the game this has not
  happened at the gate or in the fight.

## Checks

- `GOWA_EDGE_MEMO=2` runs every stage and compares with the result, in the game:
  - the gate: 2,339,993 hits, 0 lists, 0 vertexes differ;
  - the fight: 2,174,627 hits, 0 lists, 0 vertexes differ;
  - past the exact counts, 2-11% of the hits differ: the padding records, which no index names and
    which come from stale local store (`GOWA_EDGE_NATIVE_STATS=1` prints these apart).
- Replay, the ten corpora, plain: 0 differences. With results on, three runs a job, requests, PUT
  bytes and registers: 0 differences (position only: the corpora were captured with culling on).
- A new corpus with culling off, `logs/jobcap-nc`, every result given up at the output stage
  (`GOWA_EDGE_MEMO=1 GOWA_EDGE_GPU=2 GOWA_EDGE_GPU_DEFER=1 GOWA_EDGE_NOCULL=1`, three runs):
  123 of 146 reports with no PUT byte different, the rest 3-5 bytes, in the vectors whose code
  reads the caller's stack.
- Screenshots by region the same at the gate; the fight by eye.

## What it gave

Results kept against not, on top of A and B, the gate, three runs a side (`logs/edge-ana/memo6`):
23.63 -> 24.61 flips/s, +4.1% (runs 24.02, 24.03, 22.70 against 24.67, 24.45, 24.79).

All of the day's work against none of it, the same build by its switches:

| | before | after | |
|---|---|---|---|
| the gate, three runs a side | 22.89 (22.65, 23.09) | 24.40 (24.49, 24.34, 24.30) | +6.6% |
| the warm fight, three runs a side | 21.17 (21.20, 21.17, 21.17) | 21.56 (21.73, 21.43, 21.56) | +1.8% |
| EDGE, ms of worker time a frame, the gate | 114 | 87 | -24% |
| the SPU workers, the gate | 594% | 575% | |
| the frame clock thread, the gate | 83% | 87% | near 28 flips/s at most |

- The third run before hung at the level load (`logs/edge-ana/ab2-gate`, `old2`): see below.
- Audio: no gaps in the six fight runs.
- In the game with results on: 2,296,600 hits of 2,490,368 lookups at the gate (92%), 2,160,860 of
  2,424,832 in the fight (89%); none given up in either.

- The EDGE workers' samples: -14% at the gate.
- A note on speed in replays: a replayed job's second run is ~3x faster with or without results (a
  warm cache), so replay times say nothing about this. Only the game does.

## What is left

The EDGE workers' samples now: the runtime 38%, the glue 13%, DMA copies 9.5%, the job manager 9%,
skinned jobs' natives ~8%, the packing's copy 3%, the output stage 3%.
- Skinned jobs keep their whole cost: 19% of the jobs at the gate, 33% in the fight.
- A hit could leave more out: `0x88B0` and `0xBFB8` (~2%) if the context were restored.
- The rest of C, the glue, stays poor value for now.
- The workers' largest single job now is not EDGE's: General6 (w2) takes ~64 ms a frame at the gate
  and 186% of a core in the fight, and has no native code. Its noise subroutine is ~45% of it
  (performance plan 2.21, where it could not show: nothing was full then).

## A hang that is not new

One run of the last bench stopped at the level load, with every switch of this work off: flips at 0
from t = 35 s, and `[spu-dma] ... 256 identical transfers to ea=0x73E81000 -- halting the SPU` from
the job manager's poll at `0x25E4`. The limiter halts only after 30 s of repeats, so it reports a
stall rather than making one. It is in 4 of the 558 run logs kept, the first of 2026-11-02. Not looked
into.
