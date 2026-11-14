# Issue #9's death screen with FSR 3.1, and the reporter's report of 2026-11-13 (2026-11-14)

The reporter of issue #9 sent a report from 0.1.0-alpha.10 (Ryzen 7 5700X3D, RX 7900 XT; internal resolution 1.5,
FSR 3.1, sharpening max, SMAA, the Smooth patches) and two screenshots: the frame rate falling to 23 "when there's a
decay" at the temple's bridge, and a "weird look" on the death screen: the scene's outlines, bright on black, behind
"ESTÁS MUERTO".

## The video memory fix holds

The session's log has 64 to 127 `[vram] texture ...: a trim of ... made 0x0` lines: 0.1.0-alpha.10's cut (see
[the temple's note](2026-11-12-temple-textures.md)) took them all, and not one `LocalMemory`, offset or `invalid size`
error came after.

## The death screen

`temple_repro.ps1` with the save of the reporter's report of 13 November 17:06 (the same checkpoint, TEM749) and
this session's pad recording, `PAD_NO_HOST=1` (the PC's own pads and keyboard left out), F12 every second around the
first death; the replay falls off the bridge at ~205 s the same way every run.

- Bilinear scaling: the death screen is black behind its text, even at eight times the brightness.
- The reporter's settings (1.5, FSR 3.1, sharpening max, SMAA): the fall's last view behind the text, its edges
  bright, the same 3 and 7 s in. The reporter's screenshot.

`up_draw` (`rsx_d3d12_backend.c`) puts the HUD on FSR's picture as a difference: the upscaled picture plus (frame minus
picture), the picture being the frame as the scene left it (`fg_marker`'s copy). The death screen draws its black
after the marker, as HUD: frame minus picture is then minus the picture, and what is left is the upscaler's and its
sharpening's own detail of the scene, its edges, on black. Any layer that darkens the picture in the HUD's part of the
frame does the same: a fade, the pause menu's dimming.

The fix: the difference where the HUD added or dimmed a little, the frame as it is where it darkened the picture a lot.
The luminance ratio, frame to picture (+0.01 each, for black): 0.9 and up the difference, 0.6 and down the frame,
between them a blend. Text, icons and glow brighten and keep FSR's picture under them; a black or dimming layer is
taken from the frame. The same replay with the reporter's settings: the death screen black behind its text at eight
times the brightness (the brightest pixel 1 of 255 before the prompt shows); the HUD in play as before, enlarged.

## The frame rate at the decay

The reporter's own replays fell off the bridge before the decay. Played by hand here from the same save (a folder of
its own, `logs\issue9-play`), the drop came, and that session's pad recording replays it every time on this laptop
(`temple_repro.ps1`, the same save, sharpening high and frame generation on as played, `PAD_NO_HOST=1`): ~30 flips/s,
13 at ~120 s, then 17-19 for as long as the run goes on (the reporter: 23 on a Ryzen 7 5700X3D).

- **The SPUs.** The workers took 532% of a core before and 566% after (`GOWA_THREADS=2`): 66% more SPU work a frame.
  The main thread stays at 96%; the GPU's threads lose time.
- **EDGE.** By workload (`SPURS_WID_PROF=2`): EDGE0, EDGE1 and EDGE2 ~67% of a core each before, ~126% after, a run
  2.0 ms then 5.7: ~2.9 times a frame. RenderJobs 2.1 times a frame. General6 and the others take less.
- **The memo holds.** `GOWA_EDGE_NATIVE_STATS=1`: EDGE jobs looked up went from ~833 a frame to ~1,725, with results
  for 92% of them, 99% of the unskinned (rigid) ones, and no new results kept: the falling pieces are many more jobs
  of meshes already kept. Given up after a result: 0.27% of the jobs before, 1.36% after.
- **Where EDGE's time goes.** The sampler on the workers with each sample's workload (`PS3_SAMPLE=1`,
  `PS3_SAMPLE_THREAD=spurs pm`, `PS3_SAMPLE_RVAS`, named by the link map), EDGE0-2 only, samples a second before and
  after: `gowa_edge_light_79b0` (the lighting stage's loop on the SPU's side) 44 -> 244, `pic_00755080`'s `0x306F0`
  34 -> 195 and `pic_0075B120`'s `0x30358` 18 -> 101 (lifted pieces), `xf_native` 7 -> 33, `pt_native` 11 -> 47: the
  whole job on the SPU, about 5.5 times. The unpacking hardly moves (`v9_decode` 1.1 times, `gowa_edge_decode_304a0`
  1.3): a rigid result's unpacking is not this scene's cost.
- **Why on the SPU.** A temporary count at the lighting stage's entry (`edge_gpul_capture`) of why the GPU's
  lighting turned stages down, every 5 s: before the decay ~3-5 thousand a second for `r8 != 0`, after ~15 thousand
  (~850 a frame); the other attribute layout ~3.5 thousand a second after; more than 32 lights (33-48) ~700 a second
  after. A stage turned down gives its job's kept result up (`edge_memo_revoke_lit`): the whole job then runs on the
  SPU, unpacking, transforms, lighting and packing.
- **What `r8` is.** Those jobs' attribute list is `01 03 04 14 1B 1C 1D 1E`: an attribute 20 (`0x14`) before 27-30,
  and `r8` names its array. Their light record counts four groups, `3 0 1 3` here: after the stage's lighting, when
  `r8` is set, it tail-calls the point lights (`0x6AC8`) once more for the fourth group, adding them to attribute 20
  (blend 1.0). The GPU's lighting knows the first layout and three groups.

So the decay's pieces are lit with a layout and a fourth light group the GPU's lighting does not take, and every
such job runs whole on the SPU. Taking them on the GPU means the pass learning that layout (attribute 20 in the vertex
it packs, ids 27-30 one place on) and the fourth group's point lights added to attribute 20, and the memo keeping
attribute 20 for them.

The pass learnt it: [2026-11-15-edge-attr20.md](2026-11-15-edge-attr20.md). The memo does not see these jobs at all
(the job reader does not read their class), so that part stays.
