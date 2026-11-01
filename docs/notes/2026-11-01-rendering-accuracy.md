# Rendering accuracy: what is wrong, why, and the plan (2026-11-01)

Reported: the main menu flickers (textures seem to blink); from the start of the opening scene to
Kratos being whipped some textures or objects break into blocks, most with fast motion; and the
picture tends to be dark. Reference: RPCS3 (Vulkan) on the same machine, a screenshot of the
main menu.

## 1. Method

- **Captures.** `RSX_CAP=t<s>:<count>:<stride>` takes whole frames: the backbuffer, every
  offscreen colour target as a BMP, and a manifest of every op with its targets, shaders and
  texture state. `RSX_CAP_SHOT=1` does the same for each bench screenshot. The manifest now also
  says whether a draw encodes its colour outputs to sRGB (`srgb=`, fork).
- **Toggles, one per run, same route.** The title's own switches (`GOWA_PATCH_DISABLE_MLAA`,
  `_DEPTH_OF_FIELD`, `_MOTION_BLUR`) and the backend's (`RSX_ZPASS=0`, `BLIT_SNAP=0`,
  `TEX_NOCACHE=1`, `RT_FEEDBACK=0`, `GCM_REPORT_SPU_HOLD_MS`).
- **The flicker is counted in screenshots, once a second for 30-55 s.** It comes and goes from
  run to run, and a capture reads every target back each frame, which changes the timing it
  depends on: two runs of 8 and 32 captured frames happened to have none. The first
  version of this note blamed MLAA on such a run; it was wrong (2.1).
- **Numbers, not eyes.** `scripts/rsxcap_check.py`: flicker of consecutive frames (mean luma,
  its spread, the change from frame to frame at lags 1-4), sRGB pairing per target, the texture
  formats in use. Pictures are judged from full-resolution crops: a downscaled view of a 1600x900
  screenshot showed banding that the image does not have (427 distinct colours along 640 pixels
  of its sky).
- **RPCS3 as a map.** Its configuration for this game was read (not changed): Vulkan,
  1280x720, **Write Color Buffers on**, and the game patches **Disable MLAA** and **Disable Mesh
  Trimming** on. Its RSX code was read for how it does things, not copied.

## 2. Findings

### 2.1 The menu's flicker is the occlusion queries' answers

What it looks like (a recording from the user, and our screenshots): in about one frame in three,
large parts of the scene are not drawn and show black (the cloth in front, parts of the sky),
with stepped edges that do not follow the objects; the next frame has them back.

Screenshots once a second on the menu, VSync on, a frame counted as one of those when it has
more than 4 points more near-black pixels than the run's quieter frames:

| run | such frames |
|---|---|
| default | 15 of 30 (and 20 of 55, 17 of 55 with VSync off, in another run) |
| MLAA off | 10 of 30 |
| `GCM_REPORT_SPU_HOLD_MS=20` | 10 of 30 |
| **`RSX_ZPASS=0`: every occlusion query answered "visible"** | **0 of 30** |

- The black is already in the display when the scene has been drawn (the copy the title takes of
  it then has it: 24% near-black against 11%), so not the post-processing.
- The title asks ~400 occlusion queries a frame (a bounding shape per object between
  `CLEAR_REPORT_VALUE` and `GET_REPORT`) and its SPU jobs drop the objects whose count is 0.
  When the walker reaches `GET_REPORT` it writes the report at once, with the last count measured
  for that report's address (0xFFFF until there is one); the GPU's count of this query reaches
  the report a batch later, after the next batch's wait for the GPU. So the jobs that cull read
  counts one or two frames old, or another object's if the title moves its reports. Objects in
  view are dropped: the black regions.
- MLAA is not it: off, the menu flickers still. (The first version of this note said otherwise,
  from 8 captured frames that happened to have none.)

### 2.2 The blocks in the opening cinematic

- **A band across the frieze at ~8-9 s** (two runs of four, MLAA on and off): a full-width strip,
  darker and shifted. In a captured frame (MLAA off) it starts in the screen-space shadow mask,
  0x01D90000: the strip's edge (y=554) is there first and longest (802 px), then in every target
  after it. The mask is cleared (op 29), built by 358 full-screen passes (op 706-1190: blended,
  one channel, depth-bounds and stencil tests over the linear depth and the shadow map
  0x01550000) and read by the lit draws (op 1193-1402) in the same frame.
- **Rectangles with straight edges** whose contents belong elsewhere, at 14 s and 32 s of other
  runs.
- With `RSX_ZPASS=0`, one run of 23 screenshots 2 s apart (6-50 s): no band and no rectangle
  (a long-straight-edge search finds none). The same culling would explain both: a shadow pass or
  an object dropped on an old count, more often with fast motion, where old counts are further
  from the truth. One run only: the default runs it was to be compared with were stopped (the
  machine ran short of memory), so this is to confirm in P1.
- Unchanged: the title's MLAA path takes a copy of the display for its SPUs and later draws
  sample it; the backend binds a GPU snapshot for those, found in the same or the previous batch
  only (frames with no copy, or the read between two, miss it and read the stale copy). Whether
  that shows is not established.

### 2.3 Some textures decode wrongly (not MLAA, not the texture cache)

- Rainbow stripes on a brazier's stand and on a pillar, a saturated blue patch on Megaera's leg.
  They show with MLAA off and with `TEX_NOCACHE=1`.
- What the cinematic samples (16 captured frames): DXT1 and DXT45 with the swizzle flag
  (18,346 / 4,106 samples), X32_FLOAT, A8R8G8B8 linear (also with the component remaps 0xAA1B,
  233, and 0xAA93, 105), B8 swizzled, W16Z16Y16X16_FLOAT linear (173), and Y16_X16_FLOAT
  **swizzled with remap 0x29E4** (42). The non-identity remaps and the swizzled float format are
  the first suspects: 0xAA1B reverses the channel order, so a red decal decoded without it is
  blue.

### 2.4 The picture is darker than RPCS3's

Same menu, regions of the still parts (luma):

| region | RPCS3 | port, MLAA off | port, default |
|---|---|---|---|
| whole picture | 75.5 | 60.9 (-19%) | 59.3 |
| sky | 137.2 | 117.0 (-15%) | 119.0 |
| amber shard | 132.4 | 124.2 (-6%) | 126.6 |
| cloth in shadow, left | 32.0 | 20.4 (-36%) | 9.4 |

- The menu's dropped objects (2.1) pull its average down further: with every query answered
  visible the screenshots' median luma is 57.8 against 53.7, but the frames without dropped
  objects were 56.6 already. Most of the gap to RPCS3 is in every frame.
- Not uniform: shadows lose most, highlights least, as if the port's picture were RPCS3's raised
  to ~1.2, which is what a lower exposure before a filmic tonemap looks like.
- Not a setting: the system save (where the title keeps its brightness) is byte for byte RPCS3's.
- Not the sRGB pairing: every target a draw writes colour to with the encode is read with the
  decode, and the targets written linear are the luminance chain's (read raw as Y16_X16_FLOAT)
  or have no colour writes (the audit, `rsxcap_check.py srgb`). The backend follows
  `SET_SHADER_PACKER` as RPCS3 does.
- The suspect: the luminance chain (scene -> 640x360 -> 160x180 -> 40x45 -> 8x9 -> **1x1 at
  0x00B20000**) ends in a target no draw reads, and that pixel is 0 in all 16 captured frames.
  Its reader is outside the draws: most likely the PPU reading video memory to set the exposure.
  No SPU DMA touches it. Our backend never writes a target back to guest memory, so that reader
  sees whatever was there; RPCS3's Write Color Buffers serves it. Not proven yet (2.4 is plan
  step P2's first experiment).

### 2.5 Not found

- No banding at full resolution (see Method).
- The depth of field's double-buffered linear depth (0x02EC0000 / 0x03244000, each frame reads
  the other) is served from the GPU targets: not a cause.
- The sub-viewport and screen-copy substitutions are off or never bind here.

## 3. How RPCS3 does it (map)

- **Surfaces.** A texture read looks up render targets by byte range: a texture inside a target,
  at an offset or another pitch, partial overlaps, the newest write first; a different format is
  served by bpp-aware conversion. Ours matches by exact offset, plus special cases (Y16 from RGBA8,
  depth as colour, blit snapshots).
- **Write Color Buffers.** Targets are written back to guest memory (`write_to_dma_buffers`) so
  that CPU, SPU and copy engine readers see what the GPU drew. Ours has no write-back.
- **sRGB output.** From `SET_SHADER_PACKER`, as ours.

## 4. Plan

In order of value for effort. Each step is verified with the same captures and numbers as above
(`scripts/rsxcap_check.py`, the bench's screenshots at fixed times).

**P1. Occlusion queries answered right (fixes 2.1, likely 2.2).**
- First, confirm 2.2: default against `RSX_ZPASS=0` on the cinematic, two runs each, dense
  screenshots.
- Never answer a query with another object's or an old count: at `GET_REPORT`, a query whose own
  count is not in yet reads "visible" (what the walker writes before the first count), and the
  count goes in as soon as the GPU has it (a fence polled each walker pass, not the next batch's
  wait). Then find how late the culling jobs read: if a count can only be late, the title's
  culling of moving objects is a frame behind (as on hardware, where counts also come back
  later than the draws).
- RPCS3's way, for reference: real occlusion queries, and a read of a report not yet written
  waits for it (`Relaxed ZCULL Sync` off in the reference setup). Our reads of the report area
  can be found where they happen: SPU DMA (ours), `cellGcmGetReport*` (HLE).
- Measure what culling saves: level frame rate with `RSX_ZPASS=0` against the fix.
- Verify: 0 of 30 menu screenshots with dropped objects, with culling on.

**P2. The dark picture: prove the exposure path first (2.4).**
- Experiment: after each frame, write the 1x1 luminance target back to its guest address
  (a copy to a readback buffer, read one frame later: no stall). If the menu's luma moves toward
  RPCS3's (75.5 overall, 32 in the shadowed cloth), the reader is the PPU, and P5 generalises it.
- If not: find the exposure in the final passes' constants and trace where the PPU takes it from.

**P3. The band in the shadow mask (2.2), if P1 leaves it.**
- Bisect the 358 mask passes of the captured frame (the mask after each group of passes) to the
  one that writes the strip; then compare what it is given with what the RSX does: its depth
  bounds against the depth values under the strip, its stencil state, its scissor, the shadow
  map region it samples.
- The title's MLAA copy (2.2, last point) after P1, if anything in the picture still points at
  it: MLAA off by default (RPCS3's reference setup), or the copy written back before the SPUs
  read it (P5).

**P4. Texture decode audit (2.3).**
- Find the draws behind the rainbow stripes and the blue patch in captured frames (the
  manifest's textures for that object; `TEX_SAVE` / `FP_IDCOLOR`).
- Unit-test the suspects against the RSX semantics: the component remap crossbar (0xAA1B,
  0xAA93, 0x29E4), swizzled float formats (Y16_X16_FLOAT, W16Z16Y16X16_FLOAT), the swizzle flag
  on DXT. Fix the decoder; verify on the cinematic screenshots.

**P5. Write-back of targets to guest memory (Write Color Buffers, where needed).**
- Track the guest ranges the GPU owns (targets drawn since their memory was last written by
  anything else).
- Readers we see: SPU DMA GETs (`mfc_do_transfer`), RSX copies (blits, transfers). A read of an
  owned range flushes it first: exact and cheap, only what is read.
- The PPU: guard pages on chosen ranges, or the rule "a target no draw samples is written back".
- Needed if P2 confirms the exposure reader, and for the title's own MLAA to work (P3).

**P6. Surface lookup by range (RPCS3's model).**
- Targets found for a texture inside them, at another pitch, in part; the newest write wins.
  Replaces special cases (blit snapshots, screen copies) with one rule. Larger; after P1-P5 show
  what is still wrong.

**P7. References.**
- RPCS3 screenshots at the same points as the bench's (menu, cinematic at fixed times), with its
  current configuration, to compare brightness and detail region by region.
