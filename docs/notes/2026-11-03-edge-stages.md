# EDGE's job, stage by stage (phase 1 of EDGE off the SPUs)

Written 2026-11-03, branch `edge`. Phase 0 (performance plan 2.22) found the gate SPU-bound by
throughput: EDGE's SPU time turns into frames at ~5 workers' worth. Phase 1 maps the job so
that phase 2a can make its stages native, one at a time, exact.

Job module `jobmod_e8f44fbc6dbf_e6D98` (LS 0x4000-0x10480), the three EDGE lists (w14-16).

## How it was measured

- **The sampler in the game**: gate, 60 s, `PS3_SAMPLE_THREAD=spurs pm`, `logs/edge-p1/samp.rvas`,
  `symbolize --wid 14/15/16`.
- **A call profile in replay** (temporary, not kept):
  - every call the lifted module makes, direct or indirect, timed with `rdtsc`;
  - the 120 gate jobs (`logs/jobcap-gate`), ten runs each, all outputs still identical.
- **Loop counts in replay**: passes at every loop head of the lighting and culling functions.

## Where an EDGE job's time goes (in the game, w14-16)

| | share |
|---|---|
| lifted module code | 35.1% |
| position-independent pieces (`0x755B80` 8.6%, `0x75BD10` 3.8%, the rest 3.9%) | 16.3% |
| native loops already in (lighting 10.3, normals 1.7, decoders 1.6, skinning 1.3) | 14.9% |
| runtime: dispatch ~5.7, DMA ~4.3 + its `memcpy` 3.4 (outside the executable), coherency 2.2, channels | ~17.5% |
| other | ~7% |

## The job's structure

Same in every job traced:
1. **The crt0** runs two constructors, then the main (`0x5110`).
2. **The main** calls the job manager (`0x2FD8`), then `0x9990` (set-up), then tail-calls
   `0x44E0`.
3. **`0x44E0`** makes three job-manager calls, then `0xAF38` and `0x88B0`, a fourth call
   (`0x17C0`), then the stage driver `0x51D0`.
4. **`0x51D0`** runs the stages, then `0x9080` writes the output.

| stage (inclusive, replay) | share of the job | calls | what it is |
|---|---|---|---|
| `0x73E0` (+ `0x6AC8`, native `7BB4`/`7ED8`) | 28.1% | 41 of 120 jobs | per-vertex lighting |
| `0x9080` (`D158` with a piece at LS `0x1D2E0`, `C628`, `C9A8`, `C490`, `C340`, `CC98`, ...) | 21.4% | every job | the output: compression, transfers, the hole |
| `0xDBF8` -> `0xDBC8` | 10.7% | every job | (not read yet) |
| `0xD618` -> `0xD640` (loop `D92C`) + `0xD250` (loop `D4DC`) | 10.3% | the 41 lit jobs | triangle culling |
| `0xE4D0` -> `0xDCB0` | 5.8% | every job | (not read yet) |
| `0xF538` | 5.4% | ~1.5 a job | a 4x4 transform (fma chains) |
| job manager `0x17E0`, at the end | 3.5% | every job | |
| `0xA880` (calls pieces at LS `0x111C0`/`0x111F0`) | 3.1% | every job | |
| `0x88B0`, `0xE550`, `0x61B0`, `0x41A0`, the rest | ~12% | | |

## Lighting (`0x73E0`), per lit job

| loop | lifted lines | passes |
|---|---|---|
| `0x75D8` (fills a table) | 23 | 65 |
| `0x7670` | 27 | 99 |
| outer `0x79B0` (around the native `7BB4` and `7ED8`) | ~300 outside them | 24 |
| `0x83E8` | 45 | 24 |
| `0x6AC8`'s `0x6BE0` | 247 | 24 |
| `0x6AC8`'s `0x6DE0` | 91 | 29 |

- The inner loops `7BB4` and `7ED8` run ~12 iterations a pass.
- In the game the lifted part of lighting is ~10.6% of EDGE, and its native loops 10.3%.

## Culling (`0xD618`)

- `0xD640`'s loop `0xD92C` (146 lifted lines, ~120 passes a call) is the triangle loop:
  - three indexes gather three positions;
  - edges with `fs`, the cross product with `fms`/`fnms`;
  - the screen positions snapped with `cflts ..., 173`;
  - degenerate tests with `ceq`, the facing test with `fcgt`;
  - the kept triangles packed (`gb`).
- The Mesh Trimming title patch edits it (`0xDAF4`).
- `0xD250`'s loop `0xD4DC` (76 lines, ~101 passes, `fma`/`fcgt`) runs beside it.
- In the game the stage is ~11% of EDGE.

## Order for phase 2a

Each stage becomes native whole, behind a switch, exact:
- checked call by call against the lifted function on real data;
- then by replay (0 differences);
- then measured at the gate.

1. Culling (`0xD640`, `0xD250`): one self-contained algorithm, ~11%.
2. Lighting's lifted rest (`0x73E0` around the native loops, `0x6AC8`): ~10.6%.
3. The output stage (`0x9080`): 21%.
4. `0xDBF8`, `0xE4D0`, `0xF538`, the pieces.

## Phase 2a, first results (2026-11-03)

**The check.** A native routine can now take a whole lifted function: a `?name` hook in
`scripts/spu_native_hooks.json` returns from it when the routine did the work.
`GOWA_EDGE_VERIFY=1` (`native/spu_edge_verify.h`) checks every call:
- it runs the lifted function on the context and the native routine on a copy;
- it compares the local store (but the frame) and r0, r1, r80-r127.

**Culling's vertex pass `0xD250`, native** (`native/spu_edge_cull.c`, `GOWA_EDGE_NATIVE` bit 128,
off by default for now).
- What it does, per vertex:
  - the matrix product A * B;
  - x', y', z', W by fused chains;
  - 1/W by frest, fi and a Newton step;
  - the projection with W's sign, the viewport scale and offset;
  - seven bound tests packed into flags;
  - a record (sx, sy, flags, 0) out.
- Eight vertices a pass, with the SPU's exact float forms.
- Checked: identical on 82 calls in the gate replay and 675,840 in the game.
- Replay: the gate's jobs -2.3%, the lit ones -3.1%. The pass is ~3.3x faster.

**Culling's triangle loop `0xD92C`.**
- Four triangles a pass, pipelined three deep:
  - twelve gathers;
  - snapping `cflts(x * s + 0.5)` with s 1.0 or 2.0;
  - the degenerate tests, the facing test, the outcode tests;
  - the Mesh Trimming patch.
- It packs the kept indexes with shuffle controls from 16-entry tables, picked by the 4-bit keep
  mask.
- Its stores merge whole quadwords, so the bytes past the last kept index come from the pipeline.
  A native loop that is to stay exact by replay has to make the same stores. Left for later
  (~7% of EDGE).

**Lighting's `0x6AC8`: point lights over vertices gathered by index.**
- Eight vertices a pass. Per light:
  - distance, frsqest/fi and Newton;
  - t = distance / radius, clamped;
  - a = 1 - t, then a^2 (3 - 2a), squared;
  - times the light's colour, added up.
- Then colour = fma(colour, scale, sum).
- ~2.7% of EDGE in the game, the rest of the lighting (`0x73E0` itself) ~6.1%.

**The fused form's slow path is 1.8% of EDGE.** `spu__x8_fma_redo` redoes, lane by lane, every
vector whose result leaves the native range. EDGE makes such values on purpose (extended-range
masks, `x * 0xFFFFFFFF`). An exact wide form for extended-range operands would serve all SPU
code.

**What it adds up to.** EDGE's time is spread over many routines:
- the output stage 21% (replay);
- lighting ~9% lifted plus 10% native;
- culling ~11%;
- `0xDBF8` ~11%, `0xE4D0` ~6%, `0xF538` ~5%;
- the pieces ~16%, the runtime ~23%.

Each native stage at ~3x is worth 1-7% of the gate's frame rate. The phase 0 estimate for a whole
native EDGE (2-3x, +45-70%) needs most of them done.
