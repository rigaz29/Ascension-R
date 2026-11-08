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

`GOWA_EDGE_SKIN` is off by default (`native/spu_edge_memo.c`). Every skinned job is skinned on the SPU
again, so both passes land on the same depth.

- The prison with the player's settings (FSR 3.1, frame generation, VSync): no trails at 14-20 s.
- The fight with the player's settings, one run each: off 27.94 flips/s (windows from the 7th on:
  27.05), against on 28.30 (28.30). That is the ~4% K gave.

## Tried, not kept

The pass's rows and position done the SPU's way through doubles in the cs_5_0 pass: a double's
`a * b + c`, cut toward zero to a float, with denormals as zero and +0. Kratos's mesh came out in spikes
in both runs, each with a different way of cutting the double. The cause was not found; doubles in DXBC
under FXC or the driver (a GTX 1650 Ti here) are the first suspect.

Getting the 4% back needs the pass's position to be the SPU's bit for bit: DXC and SM 6 with doubles,
or the cut done in integers. Alternatively, a split could be found and K turned off only while one is
seen.
