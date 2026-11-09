# Rendering: texture units, the post chain, display buffers

The first level showed only particles and the HUD on black, and the main menu's amber crystals were
blurred blobs. A capture of a level frame (`RSX_CAP`) had 1,130 draws and the scene in its colour
target (`0x00E10000`): the geometry was drawn. The gaps were in the D3D12 vtable backend.

## What was wrong

1. **Texture units 4-15.** The backend bound units 0-3 only: a 4-entry SRV table and four sampler
   tables in the root signature, and records that kept four units. A fragment program that samples
   a higher unit declares that register, so its pipeline failed (`[FP] PSO FAIL ... 0x80070057`,
   E_INVALIDARG) and every draw with it was dropped. The log's 16-line cap filled before the menu.
2. **The pipeline cache.** With those programs building, the game crashed in the NVIDIA driver
   (`nvwgf2umx.dll`, 0xC0000005) as the level loaded. The guest-FP cache had 256 entries and its
   eviction released the oldest pipeline at once, while a frame in flight could still use it.
   With 4096 entries and an eviction that waits for the GPU, the same route runs on.
3. **A missing helper.** Some programs called `_rsx_expand_input` without its definition and
   failed to compile. The unit scanners skip an inline constant only after a non-branch
   instruction, the body walk after any, so they can disagree on a program with branches.
4. **The window position.** The title sets `SET_SHADER_WINDOW` with the origin at the bottom.
   The backend gave a program's WPOS the host position (origin at the top), so a pass that
   addresses a screen-sized buffer by WPOS read it upside down: the menu's top crystal refracted
   the "Single Player" text from the bottom of the screen.
5. **Blit copies of the display.** Each frame the title copies a display buffer to main memory
   (`0x70B30000`, NV3089, in 1024- and 256-pixel-wide pieces), and its post chain samples that
   copy as the frame's scene. The guest-memory copy moves stale bytes (the frame exists only on
   the GPU), and the display-sized sampler fell back to the previous final frame.
6. **Texel-coordinate textures** (format bit 0x40) were sampled with the texel coordinate as is.
   The title binds its 1280x720 surface at `0x1D90000` this way.
7. **Inline arrays.** With 1-6 fixed, a level capture showed the whole scene in `0x00E10000`
   (Kratos, the cliffs, the sky, lit), yet no recorded draw sampled that surface and no blit
   copied it, and the screen stayed black. The SPUs do not move it either: `SPU_DMA_RANGE` on
   the surface and `SPU_DMA_RANGE2` on the three display buffers saw no transfer from boot to the
   level. The vtable path ignored `NV4097_INLINE_ARRAY` (0x1818), vertices pushed through the
   FIFO: `INLINE_DBG=1` shows the title drawing full-screen triangles that way from the first
   frames (triangles, 6 words, attribute 0 float2, into the display buffers). They are its post
   chain: velocity, linear depth, a 640x360 chain, a depth-of-field composite into a display
   buffer, and a luminance chain down to 1x1.
8. **One backbuffer for three display buffers.** Drawing those triangles turned the opening
   video black. With the surface offset in the capture (`surf=`), one batch draws the video into
   display buffer `0x384000` and the tonemap triangle into `0x708000`; the next batch moves on
   to the next pair. The backend made every display buffer the one backbuffer, so the buffer
   being prepared overwrote the one on screen.
9. **The display-sized sampler fallback.** A texture the size of the display that was not
   matched first was bound to `s_screen_copy`, the previous final frame. The check ran before the
   offscreen-target lookup, so every 1280x720 target the post chain samples (the scene, the depth
   copies) was given the previous final frame.

With 1-9 the opening cinematic was right and the menu flickered yellow and black. The captures
below stop a frame's replay part way (`RSX_CAP_STOP`), so each stage of the menu's post chain was
looked at on its own. One menu frame: the scene is drawn into the next display buffer with the
HDR target `0x2114000` as a second target; that buffer is copied to `0x70B30000`; the next batch
downsamples the copy to 640x360 (`0x24A0000`), blends it into a 640x360 history, composites the
history and the copy into the display buffer, adds bloom (from `0x2114000`), runs a depth-of-field
pass over the display buffer into `0xA8C000`, and grades that back into the display buffer. The UI
follows.

10. **Surfaces bound smaller, and read as Y16_X16_FLOAT.** The title binds `0x24A0000` at 640x360
    and then at 40x45, and its luminance chain halves one surface down to 8x9 and a 1x1 pixel. Each
    re-bind at another size recreated the target and lost what the pass before had drawn. The
    chain reads RGBA8 targets as two big-endian halves; the backend sampled the bytes as colour.
    1x1 viewports were dropped as degenerate.
11. **Copies between targets.** Each frame the menu copies a shifted rectangle of the previous
    display buffer into `0x00E10000` (NV3089, local to local). Only the guest-memory copy ran.
12. **Stale copy fields in draw records.** A record is filled field by field over whatever its
    slot held in an earlier batch, and the snapshot and copy fields were new. A draw that landed in
    a slot a snapshot had used was replayed as that snapshot instead: the menu showed five
    snapshots from three display buffers per frame, where the title makes one copy.
13. **A pass that samples its own target.** The 640x360 history blends each frame into itself: it
    is both the render target and texture unit 2. D3D12 cannot read a resource bound as a render
    target; the result here was saturated yellow.
14. **The copy is read one batch later.** The title copies the display at the end of the batch
    that drew its scene and reads the copy at the start of the next. A snapshot counted only in its
    own batch, so the post chain read guest memory (zero): a black menu.
15. **A display buffer read as a texture.** The depth-of-field pass samples the display buffer it
    is composing. Looked up by its bare offset (0 for the first buffer, which is no key at all),
    it got the previous final frame.
16. **Memory that is both depth and colour.** `0xA8C000` is a depth surface in one pass and the
    colour target of the composite and bloom in the next (in the level); during the intro, which
    uses only two display buffers, the third one's memory is a depth surface. A texture whose
    offset matched any depth surface was read as depth, and its depth-to-colour copy shares the
    colour target's slot: it overwrote the composite with the far plane (red and green full, blue
    zero). This was the rare all-yellow frame left after 10-15, caught with `RSX_CAP_YELLOW`.

## Changes

Fork:

- `VP_TEX_UNITS` 16: draw records, SRV windows (16 per draw), root signature (t0-t15, s0-s15),
  gamma key, cube mask, bind. Units 4-15 count only when the program samples them
  (`rsx_fp_texture_units`). `D3D12_TEX_UNITS=<n>` records units below n only (diagnostic).
- Guest-FP pipeline cache: 4096 entries, indexed by ucode hash; eviction waits for the GPU.
- Fragment decompiler: a helper is emitted wherever the body calls it. The decompiler's unit
  test passes (87 checks).
- `SET_SHADER_WINDOW` is kept in `rsx_state`; a draw whose window differs from the host's has it
  in its pipeline key and its `_rsx_wpos` compiled to match. `RSX_WPOS_RAW=1` turns it off.
- Texel-coordinate units: 1/size compiled in (`rsx_fp_apply_unnorm_scale`), sizes in the key.
  A normalized unit that reads a larger reused target is scaled to the part it reads.
- Inline arrays: `rsx_commands.c` keeps a BEGIN/END's `INLINE_ARRAY` words and, at END, draws
  them through the backend's `draw_arrays`, with `g_rsx_inline` (`rsx_vertex_fetch.c`) pointing
  every enabled attribute at the batch (attributes interleaved in index order).
- `RSX_DISPLAY_RT=1` (port default): each display buffer is an offscreen target of its own;
  the present copies the flipped buffer's target to the backbuffer. A texture at a display
  buffer's offset reads that target. Offscreen targets: 32 (were 16).
- Blit snapshots: `cellGcm_blit_observer` (cellGcmSys) reports unscaled 32-bit copies out of a
  display buffer into main memory; the backend copies the buffer's target at that point of the
  batch and binds the copy for textures at the destination, in that batch and the next.
  `BLIT_SNAP=0` turns it off. `cellGcm_blit_local_observer` reports local-to-local copies, which
  are replayed between the targets that hold both ends (`BLIT_RT=0` turns it off).
- Draw records clear the snapshot and copy fields.
- A draw that samples a target it draws into reads a copy made just before it (`RT_FEEDBACK=0`
  turns it off).
- `RT_REUSE` (default 1): a re-bind of the same format no larger than the target keeps it, a
  larger one grows it. An RGBA8 target read as Y16_X16_FLOAT is read through a converted copy,
  refreshed before each draw that reads it, clamped at the texture's own size. 1x1 viewports
  are kept.
- Depth or colour: the prepass records which kind of surface last wrote each offset, in op order
  across batches, and a texture is read as depth only when a depth surface wrote it last (or
  nothing has yet). A display buffer with its own target is never read as depth.
- The display-sized fallback applies only to a texture that is not an offscreen target and
  whose guest memory is empty.
- Capture tools: `RSX_CAP=t<s>[:count[:stride]]` starts on the first frame s seconds in;
  `RSX_CAP_STOP=<op>|fp<hex>,...` replays a captured frame only up to an op (or the first op with
  that fragment program), also with record-ahead; `RSX_CAP_YELLOW=<n>` captures the batches that
  leave a display buffer saturated yellow; captures add `px.txt` (raw bytes of each target's first
  pixels), `snap_*.bmp`, only the units a program samples, and why a readback failed. Also
  `BLITSNAP_DBG=<frame>[:<n>]`, `DISP_RT_DBG=<n>`, `NV3089_LOG_AFTER=<flips>`,
  `TEXHI_AFTER=<flips>`, `D3D12_IQ=2` (debug-layer errors only, each once; the debug layer needs
  Windows' Graphics Tools), and one log line with the frame at which the device is lost.

Port: `RSX_DISPLAY_RT=1` by default.

## Results

- The opening cinematic, the main menu (the crystals with their figures, the terrain, particles,
  the ink, the UI) and the first level's opening scene (Kratos in chains, the Fury) render.
- No yellow frame in about two and a half minutes of menu and level start with `RSX_CAP_YELLOW`
  watching every batch. No pipeline or compile failures through that route.
- For test runs: Start does not skip the intro movie; Square does (`cellVdec` closes).

## Not done

- `D3D12_RECORD_AHEAD=0` loses the device (`DXGI_ERROR_DEVICE_HUNG`) about three frames into the
  menu; the port's default (record-ahead on) does not. Cause not found.
- An intermittent hang (2 of about 20 runs): the FIFO walker waits at a self-jump park until
  `GCM_PARK_GRACE_MS`, the resync drops commands, and the title stops presenting.
- What the SPUs do to the copy at `0x70B30000`, if anything, is not modelled: the post chain reads
  the display as it was copied.
- Display offsets keep an untagged target from before a buffer was registered; harmless, it is
  evicted when unused.
