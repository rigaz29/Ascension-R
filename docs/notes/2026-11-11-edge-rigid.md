# B: unskinned EDGE jobs' transforms in the vertex pass (2026-11-11)

From [what CPU work can still go to the GPU](2026-11-10-gpu-candidates.md):
- an unskinned EDGE job's key held its matrix, so a moving object's jobs never had a result;
- in the fight 57-59% of them had none, and 94% of those were meshes seen before with another matrix.

B keys those jobs without the matrix and has the vertex pass apply it, as K already does for a
skinned job's palette. `GOWA_EDGE_RIGID`: 1 on (the default), 0 off, 2 the check.

## What the job does with its matrices

Found with temporary probes in the fight, then checked as below.

- **The calls to `0xF538`.**
  - A position job calls it once: the positions, in place, the matrix at `0x10480`, linking back to
    `0x5850`.
  - A lit job calls it three times: the positions as above, then the normals (`0x5A18`) and the
    tangents (`0x5A50`), both with a matrix at `0x3F4F0` that the job works out itself.
  - Each call transforms `(count + 7) & ~7` vertexes, eight a pass.
- **The normalization.** A lit job then normalizes the normals and the tangents with `0xBBC8`, in
  place: `s = fi(L, frsqest(L))`, the SPU's estimate.
- **The packing.** The lit output piece's conversion saturates, as `cflts` does. A normalized
  component just above 1.0 reaches it, which x86's conversion turns into `0x80000000`.
- **Slot 4.** 16 bytes, a bit pattern the index stage reads in a few jobs. It changes from frame to
  frame for the same mesh.
  - With it still in the key, results kept coming anew. A 128 MB arena changed 24 times in a run and
    561,000 results could not be kept. A 512 MB one held 493 MB after two minutes.
  - Without it, the check found every kept list the same as the job's. Culling is left to the RSX.

## What changed

- **The key.** An unskinned job's key leaves out slots 18 and 4, and a word keeps these results apart
  from the others.
- **The result** keeps what `0xF538` reads: 16 bytes a vertex for a position job, 48 for a lit one,
  taken at the call's entry (`er_note`).
- **A job with a result** still unpacks. It leaves out `0xF538` and `0xBBC8`, and takes:
  - the four constant vectors `0xF538`'s set-up makes from each matrix (`edge_xf_consts`);
  - which arrays `0xBBC8` normalizes.

  It hands them over in the record's part where a skinned job has its palette (`er_rec`, `ER_RIGID`
  in its first word).
- **The vertex pass** does `0xF538`, `0xBBC8` and the packing.
  - The position is worked out the SPU's way with K's `spu_fma`, bit for bit. The game's depth pass
    and lit pass test EQUAL, and either may be done there or on the SPU.
  - The normal and the tangent keep the GPU's rounding, as K's do.
- **The CPU's version** is `er_xf` and `er_vertex_lit` (`spu_edge_gpuskin.c`). The backend's fallback
  runs it under the SPU's MXCSR.
- **A result given up later** runs what was left out, in its order: `0xF538` works in place, so the
  arrays are still there.
- **`GOWA_EDGE_MEMO_MB`**: an arena's size, 128 MB unless set.

## The check

`GOWA_EDGE_RIGID=2` runs every stage on the SPU. It compares the CPU's vertexes, from what `0xF538`
read and this job's constants, with what the job packs: all 24 bytes of a lit vertex, 12 of a
position. It also compares a kept result's inputs and list with the job's own.

| run | jobs | vertexes | positions | normals, tangents | kept inputs | lists |
|---|---|---|---|---|---|---|
| before `0xBBC8` was known | 4.67 M | 1.19 billion | 0 differ | 1.8 billion bytes differ | 0 | 0 |
| before the saturation | 4.77 M | 1.25 billion | 0 | 11.9 million bytes | 0 | 0 |
| with slot 4 still in the key | 5.07 M | 1.21 billion | 0 | 0 | 0 | 0 |
| final | 4.85 M | 1.34 billion | 0 | 0 | 0 | 0 |

The shader was not read back against the CPU's version. It uses K's `spu_fma`, which was checked
against x86's under the SPU's MXCSR over 160 million operations. The fight's and the gate's
screenshots show nothing out of place and no holes in the depth.

## Measured

The player's settings, uncapped: FSR 3.1 at 1.5, borderless 1920x1080, motion blur draws on for FSR's
motion, frame generation and VSync off. Every hit doing nothing, by a temporary probe, so the fight
stays in its first phase.

| | B off | B on |
|---|---|---|
| the fight, unskinned jobs with a result | 41-43% | 92% |
| the fight, EDGE's SPU time a flip (`[wid-prof]`, one pair) | 138.2 ms | 128.4 ms (-7%) |
| the fight, flips/s, ABBA | 28.90, 21.63 | 28.38, 29.07 |
| the fight, flips/s, the capture pair | 24.09 | 25.91 |
| the gate, flips/s, one pair | 40.10 (windows 39.9-40.3) | 44.28 (42.5-45.6) |
| the gate, default build | | 43.08 |
| the gate's overlay: SPU, RSX thread | 94%, 63% | 80%, 47% |

- **The gate gains 10%.** Its windows do not overlap.
- **In the fight EDGE's SPU time drops 7%, as the classes' costs said it would.** The frames stay
  within the fight's spread. The 21.63 is an outlier, six points below every other run of the day.
- **The arena.** At the gate it holds 41 MB with one change. In the fight it changed 8 times in a run,
  4-5 before B. Of the 62,662 results kept in that run, 22,008 were B's. Most of the rest are K's,
  whose key still has slot 4.
- **Why not more in the fight.** A kept result still costs 27-45 µs: the job manager, the DMA, the
  unpacking and the output stage run for it. The estimate in the GPU note took a hit at the old kept
  results' cost, which left unpacking out for a third of the jobs. B's hits unpack.

## Next

- **Rigid hits without unpacking**, as K's: the arrays given back from the result on a revoke. About 4%
  of EDGE's SPU time, an estimate.
- **Slot 4 out of K's key.** It would cut K's misses and the arena's changes. It needs this check for K's
  lists and inputs first.
- **A: the velocity jobs** (output piece `0x75F580`), 28.6% of EDGE's SPU time with frame generation's
  moving objects.
