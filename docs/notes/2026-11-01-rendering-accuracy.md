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
  `_DEPTH_OF_FIELD`, `_MOTION_BLUR`) and the backend's (`BLIT_SNAP=0`, `TEX_NOCACHE=1`,
  `RT_FEEDBACK=0`).
- **Numbers, not eyes.** `scripts/rsxcap_check.py`: flicker of consecutive frames (mean luma,
  its spread, the change from frame to frame at lags 1-4), sRGB pairing per target, the texture
  formats in use. Pictures are judged from full-resolution crops: a downscaled view of a 1600x900
  screenshot showed banding that the image does not have (427 distinct colours along 640 pixels
  of its sky).
- **RPCS3 as a map.** Its configuration for this game was read (not changed): Vulkan,
  1280x720, **Write Color Buffers on**, and the game patches **Disable MLAA** and **Disable Mesh
  Trimming** on. Its RSX code was read for how it does things, not copied.

## 2. Findings

### 2.1 The menu's flicker is the MLAA path

Eight consecutive menu frames per run:

| run | mean luma (spread) | change, lags 1 / 2 / 3 / 4 |
|---|---|---|
| default (MLAA on) | 54.2 (49.6-57.9, sd 2.54) | 6.46 / 7.33 / 6.80 / 6.50 |
| **MLAA off** | 57.0 (56.5-57.5, sd 0.27) | **1.37 / 2.25 / 3.00 / 3.71** |
| depth of field off | 53.7 (sd 1.79) | 6.06 at lag 1 |
| `TEX_NOCACHE=1` | 54.4 (sd 1.79) | 6.58 at lag 1 |
| `RT_FEEDBACK=0` | 53.1 (sd 3.22) | 7.24 at lag 1 |
| `BLIT_SNAP=0` | **2.0: black** | - |

- With MLAA off the change grows with the lag, as a slowly moving scene's does. With it on the
  change is ~6.5 at every lag: each frame carries a part of its own.
- Why: the title copies the previous display buffer to main memory (0x70B30000, in 1024- and
  256-pixel pieces) for its SPU anti-aliasing, and later draws sample the copy. Our frames exist
  only on the GPU, so the copy moves stale bytes and the SPUs smooth those. The backend binds a
  GPU snapshot of the display for draws that sample the copy (`BLIT_SNAP`), but only one taken in
  the same or the previous batch; otherwise the draw reads the stale, SPU-processed copy. With
  `BLIT_SNAP=0` the menu is black: its whole final picture is composited from that copy.

### 2.2 The blocks in the opening cinematic: two kinds

**(a) Rectangles of stale picture, with MLAA on.**
- Captures (R_MEGAERA03): every frame takes a copy of one of the three display buffers
  (0x40000000 / 0x40384000 / 0x40708000) early and reads 0x70B30000 near its end. Some frames
  take no copy at all and still read it (frames 405 and 503 of the capture), others take two with
  the read between them (403): the snapshot lookup misses and the stale copy is drawn.
- Seen at 14 s and 32 s (runs with MLAA on, motion blur on or off): rectangles with straight
  edges whose contents come from elsewhere. Not seen in 38 screenshots of three runs with MLAA
  off; but the shots of different runs fall on different moments, so P1 is to confirm it.

**(b) A band across the frieze at ~8-9 s, with MLAA on and off.**
- The same view in two runs, one of each: a full-width strip, darker and shifted (a capture:
  frame 400, MLAA off).
- It starts in the screen-space shadow mask, 0x01D90000: the strip's edge (y=554) is there first
  and longest (802 px), then in every target after it (the scene 0x00E10000, the composite
  0x00A8C000, the display).
- The mask is made and used within the frame: cleared (op 29), built by 358 full-screen passes
  (op 706-1190: blended, one channel, depth-bounds and stencil tests, sampling the linear depth
  and the shadow map 0x01550000), read by the lit draws (op 1193-1402), then the flip. No
  frame's lag. Which of the passes writes the strip is the next question (P3).

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

**P1. MLAA off by default (fixes 2.1, likely 2.2a; +5% frame rate).**
- Default the `disable_mlaa` patch to on, as RPCS3's reference setup has it; keep the switch in
  the launcher for whoever wants the title as built.
- Optional: a host anti-aliasing pass (FXAA or SMAA on the final picture) in its place,
  off / on in the launcher.
- Verify: menu change at lag 1 below 2 and growing with the lag; no stale rectangles in two
  runs of 15 cinematic screenshots.
- Later, only if the title's own MLAA is wanted: P5's write-back at the copy (the display read
  back into 0x70B30000 before the SPUs run) and the SPU result drawn, at the cost of a GPU sync and
  a 3.6 MB readback each frame.

**P2. The dark picture: prove the exposure path first (2.4).**
- Experiment: after each frame, write the 1x1 luminance target back to its guest address
  (a copy to a readback buffer, read one frame later: no stall). If the menu's luma moves toward
  RPCS3's (75.5 overall, 32 in the shadowed cloth), the reader is the PPU, and P5 generalises it.
- If not: find the exposure in the final passes' constants and trace where the PPU takes it from.

**P3. The band in the shadow mask (2.2b).**
- Bisect the 358 mask passes of the captured frame (the mask after each group of passes) to the
  one that writes the strip; then compare what it is given with what the RSX does: its depth
  bounds against the depth values under the strip, its stencil state, its scissor, the shadow
  map region it samples.

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
- Needed if P2 confirms the exposure reader, and for P1's optional faithful MLAA.

**P6. Surface lookup by range (RPCS3's model).**
- Targets found for a texture inside them, at another pitch, in part; the newest write wins.
  Replaces special cases (blit snapshots, screen copies) with one rule. Larger; after P1-P5 show
  what is still wrong.

**P7. References.**
- RPCS3 screenshots at the same points as the bench's (menu, cinematic at fixed times), with its
  current configuration, to compare brightness and detail region by region.
