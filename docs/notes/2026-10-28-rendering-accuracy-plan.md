# Rendering accuracy: what is wrong and the order of work

Reported on 2026-10-28, after the texture cache: menu transitions flicker; parts of characters,
objects and backgrounds come out black, transparent, white or in the wrong colour; the picture
and the sound stutter; 3D scenes flicker or break up. This note sets what was measured against
what is suspected, and orders the work. Speed work waits until it is done.

## 1. The renderer

Three RSX paths live in the runtime:

| path | used here | notes |
|---|---|---|
| `rsx_commands.c` state + `rsx_d3d12_backend.c` (the vtable backend) | **yes** | all of this port's D3D12 work: fetch plan, texture cache, record ahead, sRGB, RT feedback |
| `rsx_live_draw.c` (NV4097 -> D3D12, from caner's Yakuza port) | no | `RSX_LIVE_DRAW=1` feeds it methods, but nothing in the port calls `rsx_live_draw_init()`, so it has never drawn here: the 2026-10-27 comparison ("the same image to the pixel") compared the vtable backend with itself |
| `rsx_dispatch.c` + `rsx_draw_engine.c` | no | registers with the Vulkan backend, which this build does not include |

Most of what this note finds sits in code both D3D12 paths share (`rsx_fp_decompiler.c`,
`cellGcmSys.c`) or in state the vtable path drops before the backend sees it. The plan stays on
the vtable backend; wiring the live engine as a second renderer to compare scenes against is
step 0.4.

## 2. What was measured

One run with the census added for this (`RSX_CENSUS=60`, fork `3b35f298`), `AUDIO_RATE=1`,
`CELLMARK_BLINKDBG=1`, the menus and the first level; screenshots of the base and the texture
cache build at the same moments.

**Fragment programs.** 2,245 decompiled (pipeline variants), 54,613 instructions. **92% of the
programs contain fp16-precision instructions** (16,073 instructions, 29%), 12 contain fx12 ones,
and 43% of all instructions write an fp16 (H) register. The decompiler does not decode the
precision field (DWORD0 bits 23:22) at all: everything runs at fp32, so values the RSX would
clamp, round or turn to zero (overflow, NaN, Inf) reach the colour outputs. No unhandled opcode,
no CAL/RET.

**Texture binds.** Every bind in the level got a texture (no null SRV, no format left out:
B8, A8R8G8B8, DXT1, DXT45, X16, X32F, Y16X16F, W16Z16Y16X16F, linear and swizzled). **60% of the
binds carry a mip chain; only level 0 is uploaded** (`MipLevels = 1` in `vp_upload_tex_slot`),
so distant surfaces are minified without mips.

**RSX state written but not kept** (`rsx_process_method` returns -1; the backend never sees it),
counts per minute of the level:

| method | count | last value | what it is |
|---|---|---|---|
| `0x0A68`, `0x0A78`, `0x0A7C` | 163k, 81k, 81k | 0, 2.0, 240672 | polygon offset fill enable, factor, bias |
| `0x036C` | 156k | `0xE` | blend enable for MRT 1-3 |
| `0x0370` | 171k | `0xFFF0` | colour masks for MRT 1-3 |
| `0x0380`, `0x0384`, `0x0388` | 8k, 129k, 129k | 0 | depth bounds test enable, min, max |
| `0x031C` | 39k | 0 | blend colour (the field exists in `rsx_state`, the method is not decoded) |
| `0x0394`, `0x0398` | 389k | 0.0, 1.0 | depth range (`clip_min/max` exist, not decoded) |
| `0x1D7C`, `0x1D78` | 325k, 156k | `0xFFFF0000`, 1 | anti-aliasing control (alpha to coverage off), z min/max control |
| `0x0B40-0x0B64`, `0x0B00+`, `0x1D00+` | 127k each | | texture coordinate control, per-unit texture control |
| `0x0100`, `0x1714`, `0x1800`, `0x17C8` and others | | | NOP, vertex cache invalidate, reports: no visible effect expected |

Fog, user clip planes and primitive restart were not written in these windows.

The backend maps the `CONSTANT_COLOR` / `CONSTANT_ALPHA` blend factors to
`D3D12_BLEND_BLEND_FACTOR` but never calls `OMSetBlendFactor`: every draw that blends with the
blend colour uses D3D12's default factor instead of the title's.

**Draws dropped.** The batch holds `MAX_DRAWS` = 2048 draw records (sized for God of War HD's
~1841) and drops anything past that. Level frames carry 900-2048 draws; **18% of the level's
presents stopped at exactly 2048**: the tail of those frames is not drawn, and which objects
fall off changes with the view.

**Menus.** 70 presents carry less than half the draws of both neighbours (e.g. 284 -> 82 -> 259)
in the menu stretch. A desktop recording at 25 fps found no whole-frame flash on the scripted
menu path; the flicker reported may be on other screens or local to UI elements.

**Pacing.** Presents wait for the vblank (`Present(1, 0)`), so at ~12 flips/s the intervals snap
to 67, 83 or 100 ms. Shader builds: draws are skipped while their pipeline compiles (objects
appear a moment late, the first time), and the first windows show hitches up to 687 ms.
A new texture costs ~1-1.5 ms to create on the frame clock thread.

**Audio.** The host mixer keeps real time (187.5 blocks/s, no device underrun, nothing
dropped), but in the level **20-78% of the blocks mixed were silent** (0-8% in the menus): the
title does not fill its audio port in time. That is the sound stutter, and it is throughput
on the title's side (the audio SPU workload, ~0.8 of a core), not the output.

**Freezes.** The FIFO park hang came back three times in the last thirteen runs, all on texture
cache builds (2 in 76 runs before). Each time the walker stood at a self-JUMP park right after
a `SET_REFERENCE`, for the whole 10 s `GCM_PARK_GRACE_MS`, with `put` elsewhere; the command
stream before it is coherent. EDGE reserves such holes for an SPU job to fill: the job did not
run or did not finish. A SPURS-side stall that the faster frame clock now hits more often.

**Visible already.** The texture cache drew the sky and fixed the blurred frames; it also drew
an effect around the Fury as hard-edged translucent red panels with vertical streaks (it sampled
nothing before).

## 3. Another port of this title as a map

`otavix010/gow-ascension-pc` (MIT; the account and repository went away on 2026-10-28 while this
note was written; its README, changelog and commit titles were read first) renders this title
with the live engine and reports, among its fixes: RSX precision modifiers in the fragment
decompiler for yellow hands and blown-out brightness; NaN guards for yellow or grey skin;
reusing IO space returned by unmap for half-drawn textures and black cutscenes after ~8
minutes; the FIFO walker holding and rechecking a block instead of resyncing for a rare
freeze; shaders built off the drawing thread, a pipeline library on disk (hitches of up to
65 ms down to 2 ms) and a pre-built shader pack; a larger audio buffer; texture decoding off the
RSX thread as their next step. As with refs A and B, each item is a lead to check with our own
probe, not a fix to copy.

## 4. The plan

Each step lands only with its check: the census numbers it targets, screenshots at the same
moments before and after (menu, level +150/+185/+210 s), and flips/s not lower.

**0. Before fixing**
- 0.1 **The freeze** (blocks testing and play). On the next one, which job list has entries
  and no runner (`GOWA_WWS_WATCH`, SPURS kick and workload state, at the moment the park
  starts); then the race behind it. Map: their walker fix is for a different cause (reading
  a block the PPU was still writing); check ours does not do that too.
- 0.2 Reference images: the same moments in RPCS3 on this machine (the user's install,
  unchanged settings), so "wrong" is measured against something.
- 0.3 Menu flicker: dump exactly the presents whose draw count dips against both neighbours,
  and the screens the user sees it on.
- 0.4 The live engine as a second renderer behind `RSX_LIVE_DRAW=1` (initialised on the null
  backend's window, presenting the current display buffer): an independent image of the same
  frame for any effect in doubt.

**1. Shared correctness (largest effect, all in shared code)**
- 1.1 **RSX precision in the fragment decompiler**: fp16 instructions and H registers rounded and
  clamped as the RSX does, fx12 clamped to [-2, 2), the non-finite rules (what becomes 0); from
  the hardware documentation, checked per instruction against values the RSX produces. Targets
  the wrong colours, white and black surfaces, blown-out brightness.
- 1.2 **No dropped draws**: split a batch that fills (execute and continue in a new one) or size
  `MAX_DRAWS` from what a level frame needs; `FRAME_BUDGET=1` must report no truncation.
- 1.3 **Polygon offset**: factor and bias into the pipeline (depth bias, slope-scaled), units
  converted for the depth format. Targets decals and shadows flickering against surfaces.
- 1.4 **Mip chains**: upload every level the texture has (RSX mip layout, swizzled and linear),
  counted in the cache's bytes. Targets the shimmer of distant surfaces.

**2. The state the title writes and the backend drops**
- 2.1 Blend colour: decode `0x031C`, set it per draw (`OMSetBlendFactor`), and map
  `CONSTANT_ALPHA` to the factor's alpha. First suspect for the red panels.
- 2.2 MRT blend enables and colour masks (`0x036C`, `0x0370`) into the pipeline's per-target
  blend state.
- 2.3 Depth bounds test (`0x0380`-`0x0388`; D3D12 depth bounds where the adapter supports it).
- 2.4 Depth range and z min/max control into the viewport's depth range and depth clip.

**3. Stutter**
- 3.1 Pipeline library on disk and pipelines warmed at boot from the shader cache, so first
  sightings neither skip draws nor stall.
- 3.2 Texture creation and decoding off the frame clock thread (or a new texture taking over an
  idle one's resource, measured on 2026-10-28 and parked).
- 3.3 Presentation pacing at a steady interval below the vblank rate when the title runs slower,
  against the census histogram.
- 3.4 Audio: how many blocks the title fills per second and why it falls behind (the audio
  workload's scheduling and priority); a larger buffer covers jitter only.

**4. Per effect, with the references from 0.2-0.4**
- 4.1 The red panels around the Fury (after 2.1).
- 4.2 Menu transitions (after 0.3).
- 4.3 Whatever the reference images still show different, one effect at a time.

## 5. Progress

**0.1, the freeze** (paused by the user to move on). `GCM_PARK_DUMP_MS=<ms>` (fork `c1d2fa4e`)
dumps the FIFO, the last SPU-written blocks and the SPURS scheduler as `spurs_pick` sees it once
a park has stood that long. Two level runs with it and `GOWA_WWS_WATCH=2` did not freeze; it
stays on for the next one.

**0.2, RPCS3 on this machine** (RPCS3 0.0.43, the user's settings unchanged, booted from the
command line with the keyboard input profile for that run only). It first rebuilt its caches
(~4 minutes), then: menus 42-56 FPS, gameplay **33-40 FPS** -- in Chapter 3 (The Guardhouse),
since "Continue" loaded the user's save, not the opening this port's runs reach. Same-scene
images need a new profile in that save; not done without asking.

**Step 1, done** (fork `8dad92bf`, `ee59fa6e`, `61ccc5fe`, `8c2916e2`):
- 1.1 RSX precision in the fragment decompiler (`RSX_FP_PRECISION=0` turns it off).
- 1.2 `MAX_DRAWS` 8192: the level's frames need up to 3,226 draws; two thirds of them had lost
  what came after 2,048, the tutorial prompts ("Dodge") among it, drawn now.
- 1.3 Polygon offset as the pipeline's depth bias.
- 1.4 Mip chains, with two corrections the first run showed: a min filter without mipmapping
  reads level 0 only and the LOD clamps of `TEXTURE_CONTROL0` hold, and a linear texture's
  levels keep level 0's pitch (`TEX_MIPCHECK`: a linear A8R8G8B8 chain read half padding).
  Before them the Fury's legs came out as torn beige ribbons; after, as before step 1 but mipped.
- Level: 10.3-10.9 flips/s against 11.9 before (two runs): a frame now draws about half as
  many draws again as it did, and uploads whole mip chains.
- Still as before: the red panels (step 2.1 next).

**Step 2, done** (fork `428d3289`, `b0122755`). The state is decoded and recorded per draw first,
and `RSX_CENSUS` counted how the level uses it (a minute): ~2,500 draws blend with the constant
colour (fade levels such as `0xDCDCDCDC`); the depth bounds test on ~300,000 draws (min ~0.97);
MRT colour masks `0xFFF0` and `0xEEE0` (the G-buffer's alpha left alone); z min/max control
`0x1` and `0x110`; MRT blend enables always all on or all off; no depth range other than 0..1;
no alpha to coverage.
- 2.1 The blend colour is set per draw (`RSX_BLEND_COLOR=0` turns it off). Not the red panels.
- 2.3 **The red panels were the depth bounds test**: a red light curtain in the far background,
  drawn with bounds of ~0.97-1.0, went over the whole scene without them. With the test (a
  pipeline state stream for those draws, `OMSetDepthBounds`) it stays behind everything; with
  `RSX_DEPTH_BOUNDS=0` the panels come back, as in every earlier run at that moment.
- 2.2 MRT 1-3 take their own colour masks and blend enables; 2.4 depth clipping follows
  `SET_ZMIN_MAX_CONTROL` (the guest pipelines clamped every draw).
- Level: 10.4-10.6 flips/s, the same as after step 1.
- RPCS3 with Profile 2 hung on its own at boot ("nv406e::semaphore_acquire has timed out"),
  black at 1 FPS; to try again.
- The FIFO park freeze came once more (a depth-bounds-off run); `ab_runner.ps1` now keeps a
  frozen try's logs.

**Step 3, in progress** (fork `1d379926`, `f1b353a5`, `0ec36a53`, `c2e87108`, `f62a4f6a`,
`fa85c725`, `92a2f7e8`, `531d2252`):
- 3.1 Fragment programs read their inline constants from b1 again (`c2e87108`), one pipeline
  per program instead of one per set of constants. A level run: draws skipped waiting for a
  build 1,399,836 -> 6,303, frames over 150 ms 11 -> 6, pipeline creation 43.5 -> 3.4 s,
  flips/s 10.58 -> 11.38. Then programs are read to their end (16 KB: the depth of field pass
  has 249 constants and ran past 4 KB) and each draw's b1 slot is as long as its program needs
  (`531d2252`): in a minute of the level no program is compiled (194-375 before) and no draw
  skipped (243-4,761), and the wide semi-transparent red bands, which the PS3 does not show,
  are gone. Still 1,900-3,500 pipelines made a minute from the disk cache, ~0.1 ms each; the
  pipeline library itself is not done.
- That exposed a crash: with the two shader workers the level died with a corrupted heap in 8
  of 15 runs. d3dcompiler_47 counts a blob's references without atomics, and the disk cache
  shared its blobs between the workers and the drain thread (all 8 dumps: a blob freed twice,
  from the same call). It now hands out copies (`f62a4f6a`): 8 of 8 runs clean. The shader
  caches replace their least recently used entries (`fa85c725`): the level's first minute
  draws with 106 vertex programs and holds 7,188-8,128 fragment pipelines.
- 3.2 A new texture takes over an idle one's resource (`f1b353a5`, `RSX_TEX_TAKEOVER=0` off):
  0.74 -> 0.41 resource creations a frame, but as many frames over 150 ms as before (7-8 a
  run). In those, creation is about half of the upload time and decoding a tenth
  (`0ec36a53`): next, textures placed in a heap.
- 3.4 `AUDIO_RATE` reports the ring (`0ec36a53`): 45-52 short gaps per 5 s inside sound, and a
  quarter of the reads with nothing written past the block read. The mixer's workload at
  above-normal priority (`SPURS_BOOST_WID`, `1d379926`, off by default): silent blocks
  17.3/17.6% -> 15.0/15.2%, the gaps not clearly fewer. A mixer paced by the clock is
  written, not measured yet.
- 3.3 not started.
- Level: 11.4-11.9 flips/s (8 runs), against 10.4-10.6 after step 2. The few seconds of
  stutter at the start of the Megaera scene the user saw: to look at again on this build.

**Step 3, continued** (fork `f2d1cc0a`..`2e681a20`; asked for: the audio A/B, then 3.1-3.3):
- 3.4 Audio, A/B: the mixer takes a block every 5.33 ms by the performance counter instead of
  whenever the device has room (`f2d1cc0a`; shared-mode WASAPI frees ~10 ms at a time, so blocks
  went in pairs): short gaps inside sound 27-35 -> 8-9 per 5 s. With SPURS workload 7, the
  title's mixing, at above-normal priority on top (now the port's default): 4-6, silent blocks
  10-11% -> 8-10%. Flips/s unchanged.
- 3.2 Textures are placed in shared 64 MB heaps instead of an allocation each (`197cc661`):
  creation in the 110 s around entering the level 183-222 -> 15-16 ms, the longest frame 458 ->
  352-381 ms. Uploads past the 32 MB ring share reused spill chunks (`51f8bddd`): 43-110 upload
  buffers -> 1-2.
- 3.1 `PSO_MISS_WHY` showed the polygon offset as the key field new pipelines most often
  differed in. Set per draw (dynamic depth bias, `a0fde298`) it is out of the key: pipelines
  made in a minute of the level 2,000-3,000 -> 12-57, flips/s 11.6-11.9 -> 12.2. No pipeline
  library warmed at boot is needed while that holds.
- The stutter at the start of the Megaera scene was mostly one frame on the GPU: 65-170 ms of
  draws, in runs of up to 128 particles that each sample the scene target they draw into, each
  copying the 1280x720 target first. The title sends no texture cache invalidate between them,
  so on the RSX they read the target as it was at the last one; the copy is now made there
  (`2e681a20`): GPU wait at frame starts 167 -> 11 ms, frames over 150 ms 4 -> 2. Found with GPU
  timestamps per frame and per pass in the hitch log (`8005c95f`, `1648cea9`).
- 3.3 Presents are not paced: the title flips in VSYNC mode with a vblank handler, so its frames
  are likely timed by vblanks, and one held back would show later than it was made for. The
  vblank beat itself was 62.5 Hz (16 ms steps); it is 59.94 Hz now, as in `wave/main.cpp`.
- Left around entering the level: one frame of 350-490 ms of which the backend takes ~50 ms,
  and one frame of 67 ms on the GPU copying 122 new textures.
- Seen in passing: rectangular seams in screenshots of the opening (a block of the platform
  darker than the rest), with either copy mode; for step 4.
- Level: 12.2-12.3 flips/s; around entering it 3 frames over 150 ms (730-740 ms, the longest
  361-373), against 3-8 (0.9-2.3 s, the longest 434-485) before.

**0.1 found, and where 4's seams and 0.3's flicker stand** (asked for: the freeze, then the
seams and the menu flicker, before speed again):
- **The freeze was an SPU task's assertion.** Every frozen run in the logs (8 of 8) printed
  `HALT-ASSERT heqi pc=0x0CA10 img=323211119` seconds before its park; four other runs printed
  it and went on. Image 323211119 is the particle work's first phase: its last task creates the
  next five in the sibling Taskset2 (`0x70031F80`) from the SPU. The task library's create
  (`0xC3E0`-`0xCA78` in that image): find a free id and set its enabled bit, PUT the TaskInfo,
  PUT the task's exit-code entry (`+0x1980 + 16 * id`) with state 1, set the pending bit,
  signal the taskset's workload, then read the entry back and halt unless its state is 1 or
  0x12, and set 0x10. The runtime starts a task at the commit that makes it pending, so a short
  task could exit before that read, and the runtime's exit wrote 2 there where libsre's taskset
  policy module writes 0x12 (its exit path in `libsre.prx`, file offset `0x25694`: `il r15, 18`
  and the byte insert at +15, the exit code at +8). The halted task's frame never finished:
  RsxSync had entries published and no runner, and the RSX stood at the park the frame would
  have patched. Fix (fork): the exit leaves 0x12, with a test of the entry against the
  creator's check. Proved with a local build that sleeps 5 ms after the pending-bit commit
  (the race made certain): with 2 both runs halted at the first early exit and froze, with 0x12
  five early exits in a row and the frames went on. No halt in ~20 runs since (before: ~5% of
  runs).
- The park dump now prints the job lists' ticket rows (`[park-dump] w12-17`, port), and
  `RSX_CAP_DIP=<n>` captures frames drawing less than half of the frame before; `RSX_CAP_SHOT=1`
  captures the frame of each screenshot and the one after it. Manifests carry each draw's
  scissor, depth surface, depth/stencil key, depth bounds and samplers.
- **Seams.** *Corrected 2026-11-02 (rendering note, section 6): not a light pattern. Unit 5 is
  the light's shadow map (DEPTH24_D8 at 0x01550000, compared GEQUAL), which the backend dropped
  when it was bound, so the passes sampled a stale texture; and each tile is two inline
  triangles.* Their vertical edges sit exactly on an 8x8 grid of screen tiles (x = 160, 320, 480,
  640, ...). They come from a screen mask (`0x01D90000`, red channel) the object lighting reads
  the batch after: per frame, seven depth slices, each one quad per screen tile with its own
  depth bounds (from the SPU, e.g. 0.9116..0.9158), a stencil test, and a fragment program that
  projects the position from the linear depth copy onto one 256x512 DXT1 light pattern
  (wrapped, so no border). Adjacent tiles get the pattern through different slices at the same
  depth, and the mask jumps at the tile edge (e.g. ~200 -> 0 at x = 640). With the depth bounds
  test off the edges are gone, but the scene comes out darker, so that is not the fix. Ruled
  out: the scissor (now applied, but the title's always matches its viewport; only its
  shadow-map clears are smaller than the target), the offscreen target cache (28 targets, no
  eviction), SPU reads of RSX memory (none, list DMA included), the occlusion count (1 and
  0xFFFF give the same seams). Open: what the SPU bases each tile's bounds on. The title issues
  ~400 occlusion queries a frame, all answered "visible" here (0 culls nearly the whole scene),
  and its 1408x1408 depth-only pass (722 draws) gets no depth surface: the six-entry pool is
  full, so it renders into the shared buffer that every pass switch clears.
- **Menu flicker.** One frame with a strong orange radial blur shows when the title screen
  turns into the main menu by itself (~75 s, before any button). It is a whole frame the title
  draws through its 3D path and post chain and flips (one flip per present, buffers in
  rotation), and its blur samples the depth copy (`0x02EC0000`) the frame before should have
  written. Not the target cache (no eviction) or aliasing (nothing overlaps that copy). Whether
  the PS3 shows the same frame needs a reference from RPCS3.
- Testing note: with the laptop on battery and nobody at it, the display turns off after 30
  minutes and vsync'd presents drop the game to ~3.8 flips/s (exes that ran at 60 in the menus
  did the same). Unattended runs use `D3D12_VSYNC=0`.

**The seams, continued** (fork `740c1b62`, `5c906fb2`):
- Both suspects left above are fixed, and the seams are still there. ZPASS reports now carry the
  GPU's counts: each query is `CLEAR_REPORT_VALUE(1)`, one indexed bounding shape,
  `GET_REPORT(1)`, and the D3D12 backend counts the draws between them with occlusion queries;
  ~30% come back hidden, the level runs ~2% faster (median 12.32-12.51 against 12.12-12.20) and
  nothing goes missing. A depth surface is now made only for a pass that uses it, so the
  1408x1408 depth-only pass has its own.
- The depth-bounds commands are written by SPU job modules (all 345,000 seen were in lines an
  SPU PUT; the writes come from the job manager's context, called from modules such as
  `jobmod_e8f44fbc6dbf`). Of what the RSX produces, the SPUs read only the reports (~5,000
  16-byte reads a second from the report area); no GET from local memory at all, list DMA
  included, and no NV0039 copy out of it.
- So each tile's bounds are worked out on the SPU from the title's own data. Whether the PS3
  shows the same seams needs a reference: in RPCS3 the opening is reached from a new profile in
  the user's save, which they have to allow first.
