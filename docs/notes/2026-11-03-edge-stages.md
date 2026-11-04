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

## Later the same day

**The frame.** A whole-function native routine now writes the lifted prologue's saves too:
- `0x6AC8` saves r80-r85, r126 and r1; `0xD250` saves r80, r126 and r1.
- Without them `0x6AC8`'s frame bytes stayed as they were. The replay counted 27 of 40 w14 jobs
  different, with the PUTs and registers the same.
- The check now compares the whole local store, and both are identical:
  - in the replay;
  - in the game: 544,769 calls of `0x6AC8` and 622,592 of `0xD250`.

**The fused form's redo** (fork `gowa-edge`, `spu_helpers.h`):
- Four lanes at once in double precision under rounding toward zero.
- Exact against the lane-by-lane forms on 3,000,000 random vectors (`tools/test_spu_float_redo.py`).
- At the gate its share of EDGE went 1.8% -> 1.05%. The calls stay: EDGE makes extended-range
  masks all the time.

**Read, not done yet:**
- `0xDBF8`/`0xDBC8` (11%) only dispatches into the decompression pieces.
- `0x9080`'s big child `0xD158` calls the compression piece `0x755B80` (8.6%). That piece, and
  `0x75BD10`'s `0x302E8` (3.8%), are shuffle and select loops; AVX2 gains little on those.
- The culling triangle loop's decode is under way:
  - its writer packs the raw 24 bytes of a group two passes back, with the keep mask, through
    three 16-entry tables;
  - the plan is the keep masks native and the writer's statements as they are.

**Gate, the first two native stages** (`logs/edge-p1/ab1`, ABBA, two runs each, the redo in both):
- `0xD250` and `0x6AC8` off: 14.40 flips/s (14.55 / 14.35).
- On: 14.81 (14.85 / 14.75), +2.8%; both runs above both.
- The model said ~+3% (4.1% and 2.7% of EDGE, ~3x and ~2x faster).
- Both are on by default now (`GOWA_EDGE_NATIVE` bits 128 and 256, like the other natives).

## Culling's triangle loop, native (2026-11-03, later)

`0xD618`/`0xD640` as one native routine (`native/spu_edge_cull.c`, `GOWA_EDGE_NATIVE` bit 512).
- Each group of four triangles' cull mask, eight triangles a pass, in the SPU's exact forms:
  - the records gathered with AVX2;
  - degenerate after snapping, outside one plane, facing by the mode;
  - Mesh Trimming's patch;
  - the last group's lanes past the count.
- The in-place writer runs statement for statement, with the runtime's SPU forms:
  - its three shuffle tables;
  - stores past the end into the frame's scratch;
  - the pipeline's two fill passes.
  - So the index buffer, the frame and the scratch end the same, byte for byte.
- The frame, the vertex pass's call, the count at `ctx + 0x190` and `0xB600`'s word at
  `ctx + 0x58` are as the lifted function leaves them.
- The count is the halfword at `ctx + 0x190` (bytes 0-1: `rotqbyi 14` and `0xFFFF`), not
  `+0x19E`, as first read.
- The check (`spu_edge_verify.h`) now keeps one copy per level. The lifted culling's own call of
  the vertex pass is checked inside it, and with one copy that inner check overwrote the outer one.

**Checked:** identical on 82 calls in the replay, 520,193 in the game, and the strict replay.

**Speed.**
- Replay, the lit jobs: 105.1-106.0 -> 101.6-101.9 us (-3.6%). The routine is ~1.7x the
  lifted one: its masks and the literal writer are about half each.
- Gate, ABBA: 14.56 -> 14.74 flips/s (+1.2%), within the runs' spread.
- On by default: exact, faster in replay, no loss at the gate.

## The transform, and one range test a call (2026-11-04)

**`0xF538`, the 4x4 transform** (`native/spu_edge_xform.c`, bit 1024):
- Eight vertices a pass in AVX2.
- The last pass runs with the runtime's forms and leaves every register as the lifted loop does.
  Without that, the strict replay saw 36 jobs differ: later code spills registers the transform
  left.
- The call-by-call check can now compare all 128 registers (`all_regs`).

**Profile** (rdtsc around calls, the 1,200 gate jobs of the replay):
- lighting `0x73E0`: 26%;
- output `0x9080`: 20%, nearly all of it the compression pieces `D158` calls;
- `0xDBF8`: 11%, a tail jump into the decompression pieces;
- `0xE4D0`: 6%;
- culling: 5%.

Inside lighting, the native loops `7BB4`/`7ED8` were 64% of the stage.
- They cost ~255 cycles an iteration against ~347 lifted.
- Their per-op range test (and, compare, movemask, branch, then +0: ~7 instructions an op)
  was most of it.

**The fast pass** (`spu_edge_light.c`, `spu_edge_simd.h`'s `ecf_*`):
- The ops run native with nothing tested.
- Under round-toward-zero, an exact result beyond FLT_MAX leaves FLT_MAX and sets MXCSR's
  overflow flag. A conversion out of range sets invalid.
- Operands with exponent 255 are looked for in what the ops read.
- Any of these redoes the work with the exact forms from the same start:
  - a whole call, where the output is apart from the input;
  - else one pass, from its inputs still in registers.
- +0 is applied only where values leave (stores, registers). A zero's sign never changes a
  non-zero result of mul, add, sub or the fused forms.
- `GOWA_EDGE_EXACT=1` forces the redo, to test it.

**The flags need ordered reads.** clang takes the float ops for pure and moved some across a
plain `_mm_getcsr`. In the game the vertex pass stored FLT_MAX where the SPU saturates to
`0x7FFFFFFF`, in 6,923 calls. The fix:
- `ldmxcsr`/`stmxcsr` go through volatile asm with a memory clobber;
- the pass's register inputs come out of an empty asm after the clear;
- its results go into one before the read.

With `GOWA_EDGE_VERIFY=1`, each fast call of the two lighting loops is also checked against the
exact pass.

**Results.**
- Checked in the game:
  - transform: 1.13M calls;
  - culling: 414K calls;
  - point lights: 352K calls;
  - the lighting self-check: clean;
  - all identical.
- Strict replay: 0 differences, both ways.
- Replay, the lit jobs: 100.3 -> ~88 us.
- Gate, ABBA against main: 15.34 -> 15.94 flips/s (+3.9%).
- At this level (~2%), code layout moves the replay times: with the culling natives off,
  builds that differ only in them differ by 3-5 us.

**`0xE4D0` -> `0xDCC0`, index decompression (6%): deferred.**
- Five stages in place over the compressed buffer:
  1. bit unpack;
  2. delta prefix sum;
  3. the new-vertex bitmask;
  4. a copy;
  5. two-bit triangle codes through the tables at `0xFC00`/`0xFD00`.
- Two loops (`E3C4`, `E210`) are 87% of it, all dynamic shuffles.
- Tested by scrambling after the lifted call:
  - r2-r79 it leaves do not matter;
  - its scratch past the output is sent out by DMA: PUT bytes differ when it is not
    reproduced.
- A native would have to rebuild every stage's scratch bit for bit, for at most ~3-4% of the
  job time.

## Lighting's outer loop, native (2026-11-04)

`0x79B0`, the loop around the two native light loops, is native now (`spu_edge_light.c`,
`GOWA_EDGE_NATIVE` bit 2048). It is the lighting stage's lifted rest (~10.7M of its ~47M cycles
in the replay), plus the inner loops' last iterations that were left lifted (~3.3M).

**Mapped by a symbolic run of the lifted statements.** Every word carries a label; `shufb` and
`selb` run on the known word-granular controls; float ops become lanewise expressions.
- Index entry k (`r103 + 16k`, word 3 the offset) is vertex k in lane k: group A lanes 0-3,
  group B lanes 4-7.
- Light 0 begins as `0x7BB4` begins a light. The sum of weights starts at 1e-4, the summed
  directions at 0.
- Between the loops:
  - `1/WS` by `frest`/`fi`/`fnms`/`fma`;
  - the light position `fma(A, 1/WS, V)`;
  - the direction `A * rsqrt(|A|^2 + eps)`;
  - N.L of light 0 from `0x7BB4`'s arrays.
- Stores, each quadword at base + offset:
  - `r73`: (NR, NG, NB, w), where w is what the SPU's transpose leaves (NR[3], NG[3], 0, 0 in
    each group);
  - `r75`: (Px, Py, Pz, Pz);
  - `r74`: (PR, PG, PB, PB);
  - all of `r73` first, then `r75`, then `r74`.
- Of what the loop reads, only `r103` (+128) and `r114` (+8) change from pass to pass. The
  routine takes every pass but the last, which stays lifted, so the registers end as the lifted
  loop leaves them.
- Each pass runs fast first (one flag read) and is redone exact before its stores when needed.
  `0x7BB4`'s arrays are checked to lie apart from what a pass reads.

**Checked.**
- `GOWA_EDGE_VERIFY=1` runs the whole stage, `0x73E0`, on a copy with the routine and compares
  it with the stage without it: local store and all 128 registers.
  - Replay: 82 calls identical.
  - Game: 278,529 calls identical.
- Strict replay: 0 differences, both ways.

**Results.**
- Replay, the lit jobs: ~89 -> ~79 us (-11%).
- Gate, same build, ABBA: off 16.01 -> on 16.80 flips/s (+4.9%).

## The output compression, native (2026-11-04)

The position-independent piece `0x755B80` (8.6% of EDGE in the game) is native now
(`spu_edge_cmp.c`, `GOWA_EDGE_NATIVE` bit 4096). `0xD158` calls it for the lit jobs, and it
packs their vertices for the RSX. This is step 1 of the groundwork for phase 2b: each piece's
format, mapped and native.

**Mapped from probed calls.** A temporary probe wrote eight replay calls whole: registers, and
the local store before and after. A script then fit each output field against the inputs, over
the 3,344 vertices.
- The piece finds attributes 1, 3, 4, 27, 28, 29 and 30 by id in the list at `c + 0x160` (the
  first match of each). Their arrays come from the table at `c + 0x10`, a quadword a vertex.
- The count is `r53 - 1`. The caller's `r4` holds it too, but the piece does not read it.
- The output is 54 bytes a vertex, big-endian:

| bytes | attribute | encoding |
|---|---|---|
| 0-11 | 1, the position | x y z copied |
| 12-19 | 3, the normal | x y z w as I16N: the high halfword of `cflts(fma(v, 32767.5, -0.5), 157)` |
| 20-23 | 4, the tangent | X11Y11Z10, x in the low bits: fields of `cflts(fma(v, s, b), 142)` |
| 24-35 | 27 | x y z copied |
| 36-53 | 28, 29, 30 | x y z as F16, by the piece's own rule |

- The F16 rule:
  - `|high halfword| >= 0x4780`: `0x7BFF`;
  - below `0x3800`: +0;
  - else `((bits - 0x38000000) >> 13) & 0x7FFF`, so exponent 112 gives denormal-like values;
  - the sign copied, but for +0.
- What else the piece leaves:
  - **One junk quadword past the end.** Each pass does two vertices and writes the 8 quadwords
    from its start rounded down. For n a multiple of 8 (every call seen), only one quadword
    past the end is left: 4 zero bytes, then the output's last 12 bytes. The zeros are the
    piece's own: the inputs' w lanes vary.
  - **Its frame.** r84-r117, r126 and r1 are saved.
  - **Registers.** r80-r83 come back from slots the piece never writes, and r3 = 0. The other
    registers do not matter: the strict replay with r2-r79 scrambled after every call gave 0
    differences.

**The routine.**
- Two vertices go in a 256-bit register. Each vertex's 54 bytes go out as four 16-byte stores,
  built with shuffles.
- A fast pass runs per block of 8 vertices, and is redone exact on a flag, as in the other
  routines.
- For other counts, the routine goes up to a multiple of 8 that leaves 8-15 vertices, and the
  lifted piece does the rest.

**Checked.**
- `GOWA_EDGE_VERIFY=1` compares the local store (frame included) and r0, r1, r3, r80-r127:
  - replay: 82 calls identical, fast and exact;
  - game: 315,392 calls identical. 349 were declined, and ran lifted.
- Strict replay: 0 differences, both ways.

**Results.**
- Replay, the lit jobs: 77.9 -> 68.8 us (-11.7%).
- A first version gave -6%. It stored byte by byte (42 cycles a vertex) and left the last 8
  vertices to the lifted piece (~3,300 cycles a call, mostly its set-up).
- Gate, same build, ABBA: off 16.87 (17.05 / 16.58) -> on 17.82 (17.55 /
  17.96) flips/s (+5.6%).

## The position-only output, native (2026-11-04)

The piece `0x765B70` is native now (`spu_edge_cmp.c`, `GOWA_EDGE_NATIVE` bit 8192). The output
stage calls it from `0xD1E0` for the other two lists' jobs (w15 and w16).

**Mapped from its code, then checked on probed calls.**
- It finds attribute 1 by id, as `0x755B80` does. It copies x y z of `(n + 1) & ~1` vertices,
  12 bytes each.
- A pass does two vertices in three stores, from its start rounded down:
  - the bytes before the start are kept, from what the previous pass wrote;
  - past the last vertex, the last pass leaves `24 - ph` bytes, where `ph` is its start's low 4
    bits: 8 zero bytes, the last vertex's y z, then 8 zero bytes.
- With the output 16-aligned and n a multiple of 4, `ph` is 8. What is left is one quadword:
  8 zero bytes, then the output's last 8 bytes.
- Its one constant is the identity pattern.
- r3 is 0 after the call. There is no frame, and no register above r44 is touched.
- A first reading of the code gave the wrong junk: `r24` is built from `r8` before `r8` is
  overwritten. The captures showed it, and the rule above then held for the whole local store
  in 12 of 12 calls.
- The strict replay with r2-r79 scrambled after every call gave 0 differences.

**Checked.**
- Replay: 156 calls identical.
- Game: 335,875 calls identical, none declined.
- Strict replay: 0 differences.

**Results.**
- Replay: w15 26.8-27.1 -> 25.4-25.9 us a job (-4.7%), w16 27.5 -> 26.4-26.6 (-3.6%).
- Gate, same build, ABBA: off 17.56 (17.70 / 17.48) -> on 17.91 (17.72 /
  17.91) flips/s (+2.0%, within the spread; both rounds the same way).

## The vertex unpacking, native (2026-11-04)

The piece `0x75BD10` (3.8% of EDGE in the game) is native now (`spu_edge_vtx.c`,
`GOWA_EDGE_NATIVE` bit 16384). The lit jobs call it to unpack their vertex stream into the
position, and two unit vectors with a handedness.

**Mapped from probed calls with a bit-exact model.** The model used the SPU's float ops (toward
zero) and the runtime's `frsqest`/`fi` tables, and matched all 10,224 vectors of 12 calls.
- A record is 18 bytes: x y z (F32), then a 48-bit code.
- The piece adds attributes 1, 3 and 4 to the list, in its first three free slots. Their arrays
  are the pool's words for those slots (`c + 0x10 + 4 slot`). The same words also go to
  `c + 0x5C`, `c + 0x64` and `c + 0x68`.
- Attribute 1 is x y z, with w = 1.0.
- Attributes 3 and 4 come from the code's high and low 24 bits. Each is a unit vector stored as
  two 10-bit components and a 4-bit pattern:
  - `c = fma(A, f(0x3F800000 | m), B)`: m = 0..1023 becomes -0.7071..0.7071;
  - the third component is `c3 = t * fi(t, frsqest(t))`, with `t = 1 - c1^2 - c2^2`;
  - the pattern is one of 12 the piece writes to its frame. It puts c3 at x, y or z, gives it a
    sign, and sets w to +-1. In attribute 3, w is -1 for nearly half the vertices.
- Patterns 12-15 read past the table, into the caller's stack. Those vectors come out as the
  piece's shuffle makes them from whatever is there. This happened for 16 pairs in the
  replay's 68 calls. The routine does those pairs with the piece's own operations.
- What else the piece leaves:
  - the 12 patterns in its frame;
  - two zero quadwords past the arrays of 3 and 4, from its first pass;
  - r1 + 224, and r3 = 0.
- The hook comes after the piece's first eight instructions. The dispatcher runs those: the
  piece list lifts the piece from +0x20, but the job calls it at +0. Most pieces are listed
  that way. Lifting them from entry 0 made no measurable difference in replay, so the list
  stays as it is.

**Checked.**
- Replay: 68 calls identical, fast and exact.
- Game: 188,416 calls identical, none declined.
- Strict replay: 0 differences, both ways.

**Results.**
- Replay, the lit jobs: 68.3 -> 64.0 us (-6%).
- Gate, same build, ABBA: off 17.95 (17.90 / 17.95) -> on 17.91 (17.71 / 18.10) flips/s. That is no change
  beyond the spread; the two rounds went opposite ways.

## The position-only unpacking, native (2026-11-04)

The piece `0x764D10` is native now (`spu_edge_vtx.c`, `GOWA_EDGE_NATIVE` bit 32768). It is
`0x75BD10` for the jobs of the other two lists (w15, w16):
- it reads the same 18-byte records, but unpacks only attribute 1 (x y z, w = 1.0), into the
  first free slot's array, and puts that array's word at `c + 0x5C`;
- it writes the same 12 patterns to its frame, from its own pool, which holds them in the
  frame's order;
- it leaves no junk past the array, r1 + 224, and r3 = 0.
- Its hook comes after its first six instructions (stored back chain, r1 - 224, the list in
  r45, r44 = 1, r25 = r4 + 36, the first pattern in r57).

The routine was written from the code alone, without a probe. The per-call check compares
the whole local store and the registers, and it held from the first build.

**Checked.**
- Replay: 114 calls identical.
- Game: 172,032 calls identical, none declined.
- Strict replay: 0 differences.

**Results.**
- Replay: w15 25.4 -> 23.6-23.9 us a job (-6.5%), w16 26.4 -> 24.6-25.2 (-5.5%).
- Gate, same build, three variants alternated, two runs each (`logs/edge-cmp/ab4`):
  - this piece off: 18.50 (18.25 / 18.76) flips/s;
  - all on: 18.29 (18.25 / 18.38), so this one is within the spread;
  - the four pieces of today off (`GOWA_EDGE_NATIVE=4095`): 16.78 (17.12 / 16.68). The four
    together are +9.0%.

## The index decompression, mapped and native (2026-11-04)

`0xE4D0` (it calls `0xDCB0`, which runs on into `0xDCC0` and ends in `0xB600`) is native now
(`spu_edge_idx.c`, `GOWA_EDGE_NATIVE` bit 65536). Every EDGE job calls it, 6% of the job.

**How it was mapped.**
- A probe snapshotted the whole local store at each stage, the first time a call reached each
  loop head of `0xDCC0`.
- Each stage's input and output were then fit with a script.
- A model of the five stages was then checked against the whole local store after the call:
  identical in 16 calls.

**The stream** (in place: the indices end up where it was):
- The count of indices is the halfword at `c + 0x190`, and T = count / 3 (the piece divides by
  multiplying by 0xAAAB). A count of 0 does nothing but the frame.
- The type is `c[6] >> 4`. Only 2 and 3 are decompressed, but every nonzero count sets
  `c + 0x50` word 0 = r4.
- An 8-byte header:
  - h0: the number of reused indices;
  - h1: a delta bias;
  - h2: the bitmask's bytes;
  - k: the bits of a reuse;
  - one more byte.
- Then three sections: h2 bytes of bitmask, `ceil(2T / 8)` bytes of 2-bit triangle codes, and
  the k-bit reuse values.

**The stages:**
1. `n1 = 32 ceil(h0 / 32)` k-bit values, MSB first, become halfwords at
   `S = r4 + align16(8 + h2 + codes)`. The piece expands them in place, from the end.
2. In place: `s2[i] = s2[i - 8] + s1[i] - h1`. These are eight running sums, interleaved, so
   the SPU does them a quadword at a time.
3. The vertex sequence goes to `Q = r4 + align16(6T)`, from `16 ceil(8 h2 / 16)` bits of the
   bitmask, MSB first:
   - a 1 takes the next reuse;
   - a 0 takes a new vertex, counting from 0.
4. The code bytes are copied to just after the sequence. The copy is `align16(codes)` bytes, so
   its tail is whatever stage 2 left there.
5. The triangles, 8 a pass, against the triangle (a, b, c) before:
   - code 3: three sequence values (a new strip);
   - code 2: (b, a, next);
   - code 1: (c, b, next);
   - code 0: (a, c, next).
   So each triangle either starts fresh or turns on an edge of the last one, reversed, which
   keeps the winding.
   The last pass makes up to 7 more triangles from what follows. Then `c + 0x58` is set to
   `align16(r4 + 48 ceil(T / 8))`.

**What else it leaves.**
- E4D0's frame.
- `r3 = cwd(r1, 0)`.
- The sequence and the code copy past the indices. This is the scratch that goes out by DMA,
  so a native has to make it too, byte for byte.

**The routine.**
- Stages 1 and 2: a bit reader, 32 bits at a time.
- Stage 3: a mask byte at a time, `pshufb` expanding the next reuses and new vertices.
- Stage 5: the triangle before is kept in a register, and each triangle is one `pshufb` of it
  with the sequence's next four.
- Declined, and run lifted instead (4 of the replay's 240 calls):
  - addresses not 16-aligned;
  - a count that is not a multiple of 3, or T < 16;
  - k = 0 or k > 16;
  - regions that would overlap where the order matters.

**Checked.**
- Replay: 240 calls identical.
- Game: 561,152 calls identical. 48,517 of them (8.6%) were declined and ran lifted.
- Those declines were streams with k = 0 (no k-bit values). There the piece zero-fills
  `8 ceil(h0 / 8)` halfwords (`0xE0C8`) instead of unpacking. Stage 2 still runs over `n1`, so
  the halfwords between come from what the store held. With that path added, the game showed
  536,576 calls identical, and 3.2% declined.
- Strict replay: 0 differences.

**Results.**
- Per call: 9,154 -> 4,737 cycles. A first, byte-by-byte version was slower than the lifted
  code (9,656).
- Replay: w14 -1.5%, w15 -5%, w16 -5%.
- Gate, same build, ABBA: off 18.14 (17.87 / 18.30) -> on 18.71 (18.54 / 18.89) flips/s (+3.1%); both
  rounds went the same way.
