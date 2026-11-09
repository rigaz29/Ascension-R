# Trails over the prison of the damned (2026-11-08)

The player's report: in profile 3's save, Kratos chained at the game's opening, the characters and the
walls left trails. Copies of Kratos stacked along the camera's path, walls streaked. The report's shots
were at FSR 3.1 with frame generation; the trails showed in every configuration tried after.

Every run below: the player's save copied to `logs/user-p3` (profile 3), shots 14-20 s after the level
loads, FSR 1 with no frame generation unless said, one switch per run.

## Not these

Trails in each of: the render thread off; FSR 1; frame generation off; the title's motion blur on and
off; depth of field off; `RSX_ZPASS=0`; `D3D12_RECORD_AHEAD=0`; `RT_GROW_NOW=1`; `VTX_RAW=0` (every
draw converted on the CPU); and a `precise` position in the decompiled vertex programs (not kept).

## What the frame does

A captured frame with trails (`RSX_CAP_SHOT=1`):

- The scene's colour target, 0x00E10000, is never cleared. Its depth, 0x011B0000, is cleared every frame.
- 423 draws write depth only, 406 of them with one vertex program: the position through c[264..267].
- The same 423 then draw colour with depth test EQUAL and no depth writes.
- The linear depth the title builds between the two passes (0x02EC0000) shows the walls where the
  colour shows earlier frames.

So the colour pass failed EQUAL there, and the target kept what it had.

## The cause

The trails go with any one of these off: `GOWA_EDGE_MEMO=0`, `GOWA_EDGE_NATIVE=0`,
`GOWA_EDGE_RESIDENT=0`, `GOWA_EDGE_SKIN=0`. The first two and the resident switch take the skinned path
down with them.

The skinned path (K, 2026-11-06) leaves a skinned job's skinning to the vertex pass on the GPU. A job
the path does not take is skinned on the SPU. The two jobs of one mesh, its depth pass and its lit
pass, are taken or left each on its own.

A count at 0xE550's entry over the scene:

| Jobs | Position (depth pass) | Lit (colour pass) |
|---|---|---|
| Taken (skinned in the vertex pass) | 140,232 together | |
| Never on the path (0x9A40 did not take them) | 45,738 | 103,765 |
| On the path, but more than 32 lights | 0 | 10,241 |

The pass rounds as the GPU does. The SPU rounds toward zero, with denormals as zero. Where one mesh's
depth job was skinned in one place and its lit job in the other, the two depths differ in the last bit.
The gate and the fight, where K was measured, were not seen to split.

## The fix

First, the same morning: `GOWA_EDGE_SKIN` off by default. Every skinned job went back to the SPU and both
passes landed on the same depth, at a cost of ~4% in the fight (one run each with the player's settings:
27.94 against 28.30 flips/s).

Then the vertex pass was made to skin the SPU's way, bit for bit, and `GOWA_EDGE_SKIN` went back on. With
that, either job of a mesh may be skinned on either side.

- **The SPU's fm and fma in 32-bit integer operations only** (`spu_fma` in `native/spu_edge_gpulight.c`):
  - the two 24-bit mantissas are multiplied exactly in 12-bit halves (48 bits);
  - the product and the addend go into a 64-bit window, the larger's top bit at 62 and the smaller
    shifted with a sticky bit;
  - they are added or subtracted, a shifted-out remainder of a subtrahend counting as one more;
  - the result is normalised and cut toward zero to 24 bits;
  - a denormal in or out is zero, a zero result +0, and past the range gives the largest float.

  The same code in C (`tests/native/skin_fma_test.c`) was checked against the x86 FMA under the SPU's
  MXCSR (RZ, FTZ, DAZ) over 160 million operations of six kinds: any, near one, cancelling, far apart,
  skinning's, zeros and denormals. No difference.
- **In the shader as loops**, 4 bones by 12 row elements and 3 lanes by 3 terms, in `ek_blend`'s and
  `ek_position`'s order. Written out, FXC took 592 s over the shader (the game stalled before its first
  movie). As loops it takes 5.6 s, against 1.5 s for the pass before.
- **That compile now runs on a thread of its own** from the moment the pass is set, and goes into the
  shader cache file (fork). The first boot after the change waited 5.0 s for it; the next read it back
  in 0.0 s.

Results with the player's settings (FSR 3.1, frame generation, VSync):

- The prison: no trails, Kratos whole, 30 flips/s.
- The fight, ABBA in one build:

| | Median | Windows from the 7th on | Min |
|---|---|---|---|
| Skinning on | 29.52, 29.92 | 29.50, 30.00 | 28.52, 26.89 |
| Skinning off | 27.36, 27.91 | 27.49, 27.96 | 26.23, 26.29 |

That is ~+7%, more than the 4% measured on 11-06; the fight now sits at the 30 cap.

## Tried, not kept

The rows and position through doubles in the cs_5_0 pass: a double's `a * b + c`, cut toward zero to a
float. Kratos's mesh came out in spikes in both runs, each with a different way of cutting the double. The
cause was not found; doubles in DXBC under FXC or the driver (a GTX 1650 Ti here) are the first suspect.
The integer form needs no doubles, which some GPUs lack.
