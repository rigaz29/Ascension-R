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

Not reproduced: in every replay of the session (three, and the earlier one of the session of 17:06) Kratos falls off
the bridge before the decay; the bridge itself runs at 42-59 flips/s on the test laptop. The reporter's overlay had
the PPU and the SPUs at 100% and the GPU at 17%, and their log has the game's own `BATCHJOB: AddJob: waiting for room
in job list` around then: the SPUs' job lists full. A save just before the decay would let it be measured.
