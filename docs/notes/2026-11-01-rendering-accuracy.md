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
  after it. The mask is cleared (op 29), built by 358 passes (op 706-1190, per-tile triangles,
  section 6: blended,
  one channel, depth-bounds and stencil tests over the linear depth and the shadow map
  0x01550000) and read by the lit draws (op 1193-1402) in the same frame.
- **Rectangles with straight edges** whose contents belong elsewhere, at 14 s and 32 s of other
  runs.
- With `RSX_ZPASS=0`, one run of 23 screenshots 2 s apart (6-50 s): no band and no rectangle
  (a long-straight-edge search finds none). One run only.
- After P1 (section 5): the band and the black frames are gone in two runs. The rectangles are
  not: at 8-14 s, in the bottom right, the same lit-floor rectangles over the shadowed floor in
  every run, with MLAA on, MLAA off and depth of field off alike. Their vertical edges fall on
  the 160-pixel screen tile grid (x = 800, 960, 1120 at 720p in the shots measured), with lit
  floor on one side and shadowed floor on the other, and the floor's depth changes with y, so a
  depth bound shows as a horizontal edge: these are the seams of the October plan (2026-10-28
  plan, section 5, "Seams"), the screen mask 0x01D90000 built per tile and depth slice with
  depth bounds the SPU writes (P3).
- P3 (section 6) found two causes, both fixed. The tile seams: every mask pass sampled a stale
  texture where the title binds its shadow map (a depth texture the backend dropped). The light
  rectangle left after that (x 392-640, y 305-360 at 10-12 s): the title's transparent effects
  drawn into a 640x360 4x-antialiased surface, which the backend drew as a 640x360 viewport in
  its targets' top-left quarter.
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
- After P3 (section 6) the cinematic is far brighter (its shadow mask was wrong), the menu not:
  -20% overall and -37% in the shadowed cloth against RPCS3, as before (8 captured frames).
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

### 2.5 A band of garbage at the loading screen's end

- What the user saw with the queries answered visible, and took for a new flicker: "yellow and
  blue, somewhat transparent, over the whole picture, a few times at the start".
- Recorded (the desktop, 60 frames a second through `ffmpeg` `ddagrab`, scanned frame by frame
  with `scripts/flash_scan.py`): for ~3 frames as the loading screen gives way to the menu, a
  full-width band of random coloured pixels across the middle of the picture, with patterns like
  raw memory shown as an image; the window then reports 20 draws. The same frames in a run with
  the queries as they are: so not the queries' doing. A texture or target is shown before it
  has contents, and what was in its memory is drawn (P7).
- The same recordings, 105 s each from the start: frames with dropped objects 415 with the
  queries as they are, 1 (the loading screen's own black frame) with every query answered
  visible.

### 2.6 Not found

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

**P1. Occlusion queries answered right (fixes 2.1, likely 2.2). Done: section 5.**
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

**P3. The tile seams in the cinematic (2.2). Done: section 6.**
- Known so far (October plan, "Seams" and "The seams, continued"): seven depth slices, one quad
  per 160x90 tile each, with depth bounds the SPU writes and a stencil test; with the depth
  bounds test off the edges go but the picture darkens; the scissor, the target cache and the
  occlusion counts were ruled out (the counts again now: the seams outlive P1).
- Capture a frame that has them (`RSX_CAP_SHOT=1` at ~10 s) and compare, for the tiles on both
  sides of an edge, each slice's bounds with the depth values in the tile (the depth target
  and the linear copy 0x02EC0000): a depth range one tile's slices leave uncovered, or cover
  twice, is the seam. Then where the SPU takes the bounds from (its jobs read nothing the RSX
  makes but the reports, so the title's own data), and an RPCS3 shot of the same moment (P8)
  to see whether the PS3 shows them.
- The title's MLAA copy (2.2, last point) is not behind them (they are there with MLAA off);
  left until something points at it (then MLAA off by default as RPCS3's reference setup, or
  the copy written back before the SPUs read it, P5).

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

**P7. The band at the loading screen's end (2.5).**
- Capture that moment (`RSX_CAP_SHOT=1` / `RSX_CAP=t<s>` at the transition) and find which
  texture or target the band comes from; then why it is shown before it is drawn or cleared
  (a target sampled before its first draw read from guest memory, or a video frame shown
  before it is decoded).

**P8. References.**
- RPCS3 screenshots at the same points as the bench's (menu, cinematic at fixed times), with its
  current configuration, to compare brightness and detail region by region.

**P1b. Occlusion counts sooner (the frame rate P1 cost).**
- After P1, 28.5% of the culling jobs' reads (menu trace) find a report "visible" because its
  count is not back yet, and two thirds of those reports end up counting 0 (19.3% of all reads):
  objects processed and drawn that a count in time would drop. Such a count arrives 16 ms after
  the read (median; 47 ms p90). The RSX writes a report when it reaches the GET_REPORT, in step
  with the command stream.
- First split that delay: walk to submit, GPU, fence to delivery (the trace has the walker's
  and the delivery's times; the submit is to add). Then the cheapest of: submitting a batch
  early when it holds reports; delivering from a thread that waits on the fences instead of
  polling between walker passes; an SPU GET (ours) that covers a report whose batch is on the
  GPU waiting, bounded, for that batch's fence (a report of the batch still being walked keeps
  "visible": no wait that could deadlock the walk).

## 5. P1 done (2026-11-01): occlusion reports as the RSX makes them

**What was wrong** (`GCM_ZPASS_TRACE`: the walker's report writes, the GPU's counts, the SPU reads
of the reports, the clears and the counter switches, in time order; 40,000 events of the menu):
- The title's groups: `CLEAR` (ZPASS and ZCULL stats), the counter on, a bounding shape, the
  counter off, then the count written into eight reports; or `CLEAR`, the counter off at once, and
  two reports of 0. It moves its reports every frame or two, and zeroes a report before it uses
  it again.
- The backend counted only the first report after a clear (and every draw, whatever the
  switch): of the second and later reports of a group, 16% and 26% ever got a count. The walker
  wrote each report with the last count its address had had, another object's as often as not.
  Counts came back when the next batch started; 2,348 came after the title had taken their
  report again.

**The fix** (fork):
- a report's count is every query since its clear, and the counting goes on after a report;
- draws count only while `SET_ZPASS_PIXEL_COUNT_ENABLE` is on (a new observer op);
- the walker writes "visible" until the report's own count is in, and a count is dropped when a
  later report has taken its address (report numbers): consecutive counts at one address are
  another object's often (two of three nonzero pairs differ fourfold or more, one transition in
  six crosses zero), so an old count is no stand-in;
- a report that counted nothing reads 0, in a batch with no query too;
- counts go back when the GPU has them: the fence each batch was submitted with is polled between
  walker passes;
- a draw skipped while its pipeline builds leaves the reports up to the next clear "visible".

**Measured:**
- Menu, a 105 s screen recording from the start scanned frame by frame (`flash_scan.py`): frames
  with dropped objects 415 before, 0 after (the seven it marks are the intro text's fade and the
  menu's fade-in). Screenshots once a second: 0 of 30.
- Counts that reach their reports (menu trace): second reports after a clear 16% -> 73%, later
  ones 26% -> 71%; counts arriving after their report was taken 2,348 -> 558.
- Opening cinematic, two runs of 24 screenshots: the black frame of the arena at 8 s and the
  frieze band gone; the tile seams at 8-14 s remain (P3).
- Level, ABBA, default builds: 22.49 (22.37 / 23.13) -> 20.66 (20.20 / 21.04) flips/s, -8%;
  again with `PERF=1`: 22.80 -> 21.13, -7%. The threads' load is the same (the SPU workers 580%,
  the frame clock 70%); per frame the fixed build draws 4,028 vs 3,806 (+5.8%), looks up 10.5%
  more textures, and its prepass takes 8.8 vs 8.2 ms. Objects culled before are drawn now: some
  the old counts dropped while in view, some read "visible" before their count is back (P1b).

**Then (fork `12187ab1`): a count reaches its report only while the report is as written.**
- The title reads a report, takes the value it finds and marks the report again for its next
  query (0xFFFFFFFF, or 0), often before the GPU's count is back. The count was then written
  over that mark, and the title took it for its next query's: of 7,481 counts in the menu trace,
  4,097 landed on a report the title had already marked.
- A report now keeps the low word of the time stamp the walker wrote with it, and a count goes
  in only while that and "visible" are still there. Menu trace: of 7,295 counts, 2,384 written,
  3,898 dropped as taken by the title, 1,013 as taken by a later report. Screenshots once a
  second: 0 of 62 with dropped objects.
- The culling jobs read a report in the millisecond the walker writes it (median; 31 ms at the
  90th percentile): what P1b has to beat.

## 6. P3 done (2026-11-02): the shadow maps and an antialiased surface

**The tile seams: the shadow maps were never sampled.**
- The screen mask 0x01D90000 is built per light: the title renders the light's shadow map into
  0x01550000 (depth only, LEQUAL, cleared to 1.0, a region such as 578x796 or 1406x664 of the
  1408x1406 surface), then draws two inline triangles per 160x90 screen tile, each with the
  tile's depth bounds and a stencil test (GREATER, mask 0x5F: not the sky), whose program
  projects the pixel from the linear depth copy and takes three taps of unit 5 (`DBT_VTX`).
- Unit 5 is the shadow map: DEPTH24_D8 at 0x01550000, zfunc GEQUAL, bilinear. The backend
  dropped depth-format textures when they were bound, so the unit kept what was bound before, a
  64x128 DXT1 texture (the October plan's "light pattern" was this), and every mask pass
  sampled it. Tiles that took another light's pass at the same depth disagreed: the seams; and
  the scene was shadowed where it is lit.
- Before the fix, in a captured frame (`RSX_CAP_RAW`): across the seam at x = 800, pixels with
  the same depth, the same stencil and the same passes whose depth bounds and stencil they meet
  had mask values 162 and 0; the passes are per tile and the tiles disagreed. The occlusion
  counts play no part: with `RSX_ZPASS=0` the seams were as many (8 and 8 against 7 and 9 on
  the tile grid in two runs each).
- Fix (fork `1b9c06ad`): a depth texture takes the depth plane of the depth surface at its
  offset, and the program the comparison the decompiler had (depth FUNC reference, four
  bilinear taps), the coordinate scaled to the light's region.
- Measured: the mask's edges follow the shadow casters (the creatures' legs on the floor), not
  the tile grid, and the cinematic is far less dark. Level frame rate unchanged with the next
  fix too (ABBA 20.66 -> 20.98). The menu's brightness is unchanged (2.4).

**The light rectangle: an antialiased surface drawn at pixel resolution.**
- One rectangle stayed at 10-12 s, x 392-640, y 305-360. It is first in 0x2114000 and
  0xA8C000, which the title's transparent effects write (`SURF_DBG`): a 640x360 surface with
  SQUARE_CENTERED_4 (2x2 samples) over 0xA8C000 + 0x2114000 and the depth 0x11B0000, pitch 5120,
  i.e. the memory of the 1280x720 targets: effects shaded at a quarter of the rate over
  full-resolution targets and depth. The backend drew them with a 640x360 viewport, into the
  targets' top-left quarter.
- Fix (fork `15450691`): an antialiased surface at its sample resolution, as the shared engine
  holds one: surface size, viewport and scissor scaled by the sample grid, WPOS per pixel
  (`rsx_fp_apply_wpos_grid`). `RSX_AA_GRID=0` turns it off.
- Measured: two runs of seven screenshots (8-14 s): the x = 640 edge is gone from every shot;
  the few hits left on the tile grid fall on scene edges (the floor's stone joints, by eye).
