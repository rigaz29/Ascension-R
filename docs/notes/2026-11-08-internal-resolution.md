# Internal resolution (2026-11-08)

The second item of [the survey](2026-11-08-recomp-feature-survey.md). Everything was drawn at 720p. `graphics.render_scale`
(`D3D12_RENDER_SCALE`, Internal resolution in the launcher) draws the scene larger: a number from 1 to 4, or `native` for
the window's size. The default is 1, where no fragment program changes and every target keeps its size.

## What it does

A surface is drawn larger when its guest size is the display's, and only then. In the brothel room's frame that is the
scene, its depth, the MRT and linear depth targets and the three display buffers. The 640x360 chains and below, the Y16
copies and the shadow maps stay as the game made them. [Issue #546](https://github.com/sp00nznet/ps3recomp/issues/546)
scales every target. Keeping the small ones as they are needs a factor per target instead of one for the frame.

- `OffRT` and `OffDS` keep `w, h` as the guest's: surfaces are found by them, and so are texture coordinates and the
  guest's viewport. `pw, ph` are the resource's size and `sx, sy` the factors. Only what talks to D3D12 in pixels uses them.
- Viewports, scissors and clears take the factors of what the pass draws into: its depth surface when it has no colour,
  else its colour target.
- A fragment program reads WPOS through `fp_alpha.w`, written per draw as 1 over the target's factor. The text is replaced
  only when the scale is not 1.
- Polygon offset's slope is per pixel, so it grows with the factor (dynamic depth bias only).
- What the guest reads is at its own size: a write-back to main memory is point-sampled down and occlusion counts are
  divided by the scale squared. Target contents are not seeded from guest memory when scaled.
- 2D copies between targets scale with them. Between two targets drawn at different scales a copy cannot work, so it is
  refused with a log line (`[blit-rt]`, `[ds-convert]`). None came up on the route. The Y16 conversion takes the ratio
  of its source's factor to its own.
- The shared depth buffer of passes with no depth is at least the render's size, as D3D12 clips to the smallest attachment.

FSR 3.1 and frame generation take no render larger than the display. The scale is held to the window's size there, with a
line in the log (`render scale 2: held to the display's 1.25 for FSR 3`). A window made smaller later keeps a swap chain of
that size and the flip stretches it down, as otherwise FSR 3 would stop. Bilinear and Sharp take a larger render down to
the window. EASU only scales up, so FSR 1 falls back to that pass there.

A blit snapshot keeps `w, h` as the guest's as well, since it is found by the size of the texture the title reads. The
first version stored pixels there, the snapshot was never found, and every frame came out black with the game's own effects
on. The port's defaults turn those effects off, so only a run with the four disable patches off showed it.

## How it was tested

The route into the room (`route_r1.pad`, the user's recording), window 1600x900, GTX 1650 Ti. One run of each, frames a
second in 10 s windows from the load:

| Scale | Scaling | | | | | | |
|---|---|---|---|---|---|---|---|
| 1 | FSR 3.1, frame generation | 30.3 | 27.5 | 27.7 | 26.4 | 26.1 | 22.7 |
| 1.25 (1600x900) | FSR 3.1, frame generation | 30.3 | 27.8 | 27.6 | 26.2 | 24.5 | 22.3 |
| 2 (2560x1440) | bilinear, no generation | 53.9 | 30.5 | 31.1 | 29.7 | | |

The first window of the last row is the loading screen, without generation's pace. The CPU is the limit in the room: at
1.25 and at 2 the overlay reads the SPUs at 100% and the GPU at about 30%. The windows move between runs more than between
scales: the room's last one is 20.8 to 25.1 at scale 1 over five runs. The 1.25 row predates the snapshot and resize fixes,
which do not touch what those settings draw.

Looked at: the bridge and the room at 1.25 and 2, and the room at 1 and 1.25 with the disable patches off (shadows, MLAA,
motion blur and depth of field on); the main menu's capture at 1.5 (scene 1920x1080, half-size chain still 640x360); `native`
at 1600x900 (1.25); 2 with FSR 3.1 (held to 1.25); a window shrunk in the scene at 1.25 (picture stretched, no new FSR
context, no error).

Disable Mesh Trimming is the patch meant for this. EDGE drops triangles whose corners all fall on one row or column of
the 720p grid, and RPCS3's note says the patch lets the title draw correctly above the PS3's resolutions. It stays off by
default, as at 720p it only adds geometry. One run at 2 with it on showed nothing different on the bridge next to the run
without it, whose camera differs a little, and it did not slow that run (32.2, 33.1, 34.2 against 30.5, 31.1, 29.7). A
scene with thin geometry far away could still show gaps without it.

## Below 720p (2026-11-09)

Not offered, as it gains nothing here. A trial at 0.5 (640x360, FSR 3.1 and frame generation, one run of each on the route)
gave 30.3, 24.9, 26.9, 27.6, 23.4 frames a second against 30.2, 26.8, 26.7, 25.1, 23.6 at 1. The windows of the two runs
start 5 s apart, and the difference is inside what runs at the same scale differ by. `ppu main` took 94.5% of a core at 0.5
and 94.7% at 1, `rsx render` 33.6% and 33.7%. The limit is the CPU, and fewer pixels do not move it. The code takes a
scale below 1 with only the lower clamp in `rs_init` changed, should a weak GPU ever need it.

## The crash found in play (2026-11-09)

A session at 1.5 with FSR 3.1 and frame generation froze 60 s into the CYN050 cutscene and closed 18 s later. The log had
`pipeline refused (0x887A0005)` over and over, then a crash on frame generation's thread inside the NVIDIA driver, which
only followed from the device being gone.

`blit_rt_copy` scaled a copy's position and size apart. An odd position and an odd size both rounded up, and the box
reached one pixel past the target: 1547 + 374 on one 1920 wide. The device went two frames later with
`DXGI_ERROR_INVALID_CALL`. The copy now goes by its edges, clamped to both targets. No other copy rounds: the rest take
sizes already held to the resource.

It came back from the player's last autosave, with the save slot picked on the d-pad and their presses before the cutscene
repeated. With the fix the same route ran 230 s from the load, through the cutscene and into CYN100, with no error.

A lost device now writes one line to the session log, `[D3D12] device lost, seen ...: reason 0x...`, which this crash
needed and did not have. `D3D12_DRED=1` adds what the GPU was doing and any faulting address. It showed nothing here, as a
rejected call is not a GPU fault.

## What it does not cover

- A program with more inline constants than the buffer holds is compiled with them, and has no `fp_alpha.w`. If it reads
  WPOS it reads scaled pixels. The log line is `[FP] ... more than b1 holds`; there is none on the route.
- `FP_CONSTBUF=0` holds the scale at 1, for the same reason.
- Without dynamic depth bias the slope of polygon offset is not adjusted.
- The first session at a new scale compiles the programs that read WPOS again, about 360 pipelines on the route. The
  picture is rough until they are in, as after Clear shaders.
- The scale is read once at the start, and `native` means the window's size then.
- Only the route was run. Fights, cutscenes and other levels have not been tried above 1. Frame generation at a scale
  has been looked at in still frames only, not on a display in motion.
