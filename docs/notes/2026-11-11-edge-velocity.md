# A: the velocity jobs kept, their transforms in the vertex pass (2026-11-11)

From [what CPU work can still go to the GPU](2026-11-10-gpu-candidates.md): the velocity jobs (output piece `0x75F580`)
were never kept, 28.6% of EDGE's SPU time then. A takes [B](2026-11-11-edge-rigid.md)'s way for them.

`GOWA_EDGE_VEL`: 1 on (the default), 0 off, 2 the check.

## What a velocity job is

Found with temporary probes in the fight, with the player's settings. FSR 3.1 with frame generation's moving objects
keeps the title's motion blur draws on, and these jobs make the previous positions those draws read (`v[7]`).

- **The slots.** A lit job's, and slot 19: 64 bytes, the previous frame's matrix. Slot 18 is this frame's. A skinned
  one has the palette twice: slots 6 and 7 this frame's, 8 and 9 the previous frame's.
- **The transforms.** An unskinned one calls `0xF538` four times:

  | call | links back to | array | matrix |
  |---|---|---|---|
  | 1 | `0x5C84` | the position array, into another (r5) | `0x104C0`, the previous |
  | 2 | `0x5850` | the position, in place | `0x10480` |
  | 3, 4 | `0x5A18`, `0x5A50` | the normal, the tangent | `0x3F4F0` |

  A skinned one calls `0xE550` twice: the previous palette for the position alone, then this frame's for all three.
- **The output.** 36 bytes a vertex: a lit vertex's first 24 (the position, the normal I16N, the tangent X11Y11Z10),
  then the previous position.
- **No lighting stage.** `0x73E0` never ran in one.
- **Why never kept.** The job reader did not know the piece, and both matrices change every frame for a moving
  object.

The fight with infinite health (`cheats.damage_taken=0`), one run:

| | jobs | µs a job | of EDGE's job time |
|---|---|---|---|
| unskinned velocity | 1.44 M | 61.2 | 24% |
| skinned velocity | 0.32 M | 93.5 | 8% |
| lit, after B | 3.07 M | 32.9 | |

Hashed without their matrices, 99.93% of the unskinned ones and 99.97% of the skinned ones had been seen before.

## What changed

- **The job reader** knows the piece: `EJ_VEL`, `EJ_VEL_SKIN`.
- **An unskinned velocity job** goes B's way:
  - its key leaves out slots 18, 19 and 4, and a word keeps these results apart;
  - a result keeps what `0xF538` reads. The previous position's call reads the position array before the position's
    own call changes it, so nothing more is kept for it;
  - with a result, the four calls and `0xBBC8`'s two are left out. The job hands the vertex pass a fourth set of
    constants, `ER_PREV`, for the previous matrix (`er_rec`, `ER_VEL`);
  - the output piece does not run: the ring gets the result's first 16 bytes, the block's tag
    (`gowa_edge_vel_75f580`, spu_edge_cmp.c);
  - the job has no lighting record. Its part goes out as a position block's, and the output stage keeps its block at
    stride 36 (`edge_memo_vel_kept`);
  - the output stage no longer gives its result up, before the packing, for having no lighting stage
    (`edge_gpul_binorm_now`).
- **The vertex pass.** A position block with `ER_VEL` (`0x200`) writes 36 bytes a vertex: `rigid_vertex`'s 24, then
  `rigid_pos` with array 3. The CPU's version is `er_vertex_vel`, and the backend's fallback (`er_fill`) takes it too.
- **A skinned velocity job** stays on the SPU.

The backend took the 36-byte blocks without a change: a block with a part draws through the vertex pass at any
stride.

## The checks

- **The CPU against the SPU.** `GOWA_EDGE_VEL=2` runs every stage on the SPU.
  - 1.28 M jobs, 393 M vertexes: no position word different, this frame's or the previous, and no normal or tangent
    byte.
  - A kept result's inputs and lists: none different.
- **The GPU against the CPU.** A temporary readback in the backend compared every 20th kept velocity block's pass
  output with `er_fill`'s.
  - 39,912 blocks, 12.3 M vertexes the draws name: no position word different, nor previous position word.
  - Normals were up to 6 units apart, tangents 1: the GPU's `rsqrt`, as K's.
  - The 68 K vertexes past a job's count differ; the transforms work eight vertexes at a time. Their inputs are left
    over, NaN, which the GPU's `spu_fma` saturates as the SPU would and x86's FMA keeps. No draw names them.
- **The piece left out.** The lifted `0x75F580` stores to its output alone.
  - Around 1 in 500 kept calls, it ran with the whole local store compared: 2,400 calls, nothing different outside the
    output, r3 0 after.
  - Its code writes no register above r79 and not the stack pointer.

## Measured

The cost of each class of job, from one job's start to the next on the same worker (a probe), in the fight:

| | velocity µs a job | against lit's |
|---|---|---|
| on the SPU | 63.5 | 1.82 |
| kept, the piece still run | 62.2, its pair's second run | 1.55 |
| kept, the piece left out | 46.2 | 1.37 |

- **The order of the runs.** Each pair's second run had every class 12-16% slower, whatever the setting. The runs
  compare by their order and by the ratios.
- **A kept velocity job saves 17 µs, -27%.** That is ~21 s of SPU time a run, ~6% of EDGE's job time.
- **Kept.** 95-96% of velocity jobs had a result, and none was given up.
The flips and EDGE's SPU time a flip (`[wid-prof]`) of the day's fight runs, every one with the same script:

| run | `GOWA_EDGE_VEL` | the piece on a hit | flips/s | EDGE ms a flip |
|---|---|---|---|---|
| ABBA 1 | 0 | | 23.41 | 139.9 |
| ABBA 2 | 1 | run | 28.03 | 110.0 |
| ABBA 3 | 1 | run | 29.74 | 103.3 |
| ABBA 4 | 0 | | 28.67 | 110.6 |
| cost pair 1, first | 0 | | 28.20 | 110.1 |
| cost pair 1, second | 1 | run | 28.85 | 107.7 |
| cost pair 2, first | 1 | left out | 30.73 | 91.8 |
| cost pair 2, second | 0 | | 26.59 | 119.9 |
| the final build | 1 | left out | 30.27 | 97.3 |

- **Off:** 26.7 flips/s and 120 ms a flip, the means of four runs (27.8 without the 23.41).
- **With the piece left out:** 30.5 and 95 ms, two runs.
- **So about +10% in the fight, and EDGE -14% to -21%,** against off's fastest run and its mean. Those two runs
  were not ordered against off ones.
- **The spread has grown.** With infinite health the fight goes on to its next phases, which the old probe's
  no damage either way kept it from.

## Next

- **The skinned ones**, 8% of EDGE's job time at 94-105 µs: K's way with two palettes.
- **A kept job's unpacking**, which still runs, B's too: the arrays given back from the result on a revoke.
- **The binormals.** If a velocity job runs `0xA880`, nothing reads its result without a lighting stage. Not looked at.
