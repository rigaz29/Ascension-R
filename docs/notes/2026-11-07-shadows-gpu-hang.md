# Shadows on in the fight: a GPU hang at the level's start

Written 2026-11-07. The user asked for patches that raise the frame rate, then to profile the
shadows and drop the effect patches that did not help. Disable Shadows is on by default; the user's
settings file had it off.

## The effect patches, dropped

SSAO (`fxSSAOProcess`, update `func_004455A4`), rim light (`fxRimLightProcess`, `func_00443F88`) and
screen warp (`fxScreenWarpProcess`, `func_004447A0`) each test their enable at +0x20 the way Depth of
Field and Motion Blur do, so each had a one-instruction patch (`lwz r4,0x20(r31)` -> `li r4,0`).
All three together, FSR 1, no frame generation:

| | the three on | the three off |
|---|---|---|
| gate, two runs a side, the order swapped | 38.06, 34.09 | 34.48, 36.22 |
| fight, Disable Shadows on, one run a side | 32.94 | 33.37 |

The first run of a pair is always the fastest (the machine warms). No gain above that, and the
rim light is visibly gone from Kratos. Not kept.

## The fight with shadows on

The scripted parasite fight (`logs/parasite`), FSR 1, no frame generation, Disable Shadows off.

- **The GPU hangs as the level starts**, at frame ~3,500 (the loading screen into the fight):
  `wait_for_gpu STUCK 2s`, then `Present failed ... removed=0x887A0006` (`DXGI_ERROR_DEVICE_HUNG`).
  - Four runs of four (`logs/edge-ana/fg7/fx/sh1`, `fa`, `fb`, `hb1`).
  - The title runs on with the device gone, at 9-20 flips/s, and the screen stops changing.
  - So the 12.3 and 13.0 flips/s first read as the cost of shadows were a lost device. Those runs
    have no screenshots, which should have said so.
- **A run with the sampler at 1 ms** (`sh2`) did not lose the device. The title printed its own
  `MoveLocalMemory dst offset error` / `CopyLocalMemory ... offset error` 7,392 times (offsets past
  256 MB) and stopped flipping. Seen in no other run in `logs/`.
- **Runs that did not hang, one each:**

| | flips/s | |
|---|---|---|
| `RSX_DYN_DEPTH_BIAS=0` (`dyb0`) | 18.37 | |
| the same, sampler at 4 ms (`sh3`) | 19.71 | the profile below |
| `VTX_RESIDENT_MB=0` (`hr1`) | 23.84 | |
| `GOWA_EDGE_SKIN=0` (`hs1`) | 27.28 | |
| the bone fix below (`fx2`) | 29.37 | |
| Disable Shadows on, for comparison (`fa2`) | 32.94 | |

- **But with the bone fix, three runs of five still hung** (`fx1`, `dr2`, `db1`; `fx2` and `dr1`
  did not). One run a side only moved the timing; none of those switches is shown to be the cause.

### What the hang is

`D3D12_DRED` (a probe, not committed): DRED's page faults, and its breadcrumbs.

- **No page fault**: `GetPageFaultAllocationOutput` returns VA 0. Nothing read or wrote outside
  an allocation; the device was removed for taking too long.
- **The breadcrumbs** (one run, `db1`): the frame's command list had done 1 of its 3,219 operations,
  `BEGIN_COMMAND_LIST`. The next, where it stopped, is a `ResourceBarrier`: `46 15 15 15 8 15 15`
  (three barriers, a `CopyTextureRegion`, two barriers). No other list was in flight; the vertex
  pass's list before it had completed.
- Not found yet: which resource that first barrier transitions. The next step is a probe that keeps
  each list's first barriers (the resource, its description, the states) for the DRED report.

### The RSX thread with shadows on

`sh3`, the sampler on the frame clock thread every 4 ms, the last 60 s (14,727 samples), with
`RSX_DYN_DEPTH_BIAS=0` so that it would not hang:

- `vp_get_fp_pso` 39.5%, `rf_prepass` 10.8%, the D3D12 runtime and driver 19.6%.
- The thread is at 95%: it sets the frame.
- Without the dynamic depth bias, each polygon offset is a pipeline of its own: the run made
  3,858 of them (the fight's cache went from 2,285 to 6,143). A draw looks its pipeline up in its
  program's chain, which holds all of them. So the dynamic depth bias stays on: the hang has to be
  fixed with it.

## The bone fix

The vertex pass's skinning (`skin_rows` in `spu_edge_gpulight.c`) read a bone's matrix at the
palette + index * 48 for any index. The CPU version of the same arithmetic (`ek_blend`,
`spu_edge_gpuskin.c`), the one `GOWA_EDGE_SKIN=2` checked bit for bit against the SPU, takes a bone
the palette does not have as its first. `vtab` (t1) is a root view, read without bounds.

A probe at the jobs' first look (`GOWA_EK_PROBE`, not committed), the fight with shadows on:

- 1,536 jobs taken, 584,488 vertexes;
- 6,787 bone slots past the palette, 6,730 of them with a weight;
- e.g. a lit job of 432 vertexes, 6 matrixes, the highest bone 228.

The shader now does as the CPU does: `bone(i, nm)`, `nm` the palette's count at +4 of the skin
record. Kratos and the fight look as before (`fx2`). It is not the hang: with it, three runs of
five still hung, and DRED recorded no page fault.
