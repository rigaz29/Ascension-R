# Ultrawide (2026-11-10)

`display.ultrawide` (off by default) draws the game to the full width of a screen wider than 16:9. At 16:9, or with the
setting off, `rsx_d3d12_backend_wide_ratio()` returns 1, the hooks do nothing and no program text changes.

## The camera

The render settings are at `0x9B99B8` (getter `func_003B6180`). `+32`, `0x9B99D8`, holds the aspect, written once by
`func_003A3DD8`. The title keeps a camera's horizontal half-width and divides it by that aspect for the vertical one, so
a wider aspect alone crops the picture top and bottom. From the matrix at `c264`, at 21:9:

| | x scale | y scale |
|---|---|---|
| 16:9 | 2.2315 | 3.9671 |
| aspect only | 2.2315 | 5.2067 |
| aspect and half-width | 1.7002 | 3.9671 |

The hooks are in `port/ultrawide.cpp` and `scripts/ppu_hooks.json`:

- `0x003A3F90`: the aspect, 16:9 times the screen's ratio to 16:9.
- `0x003B05A4` in `func_003B0380`: the tangent of a perspective camera's field of view, times that ratio.
- `0x003AFE1C` in `func_003AFDC8` and `0x003B06F4` in `func_003B0380`: an orthographic camera's half-width, times that ratio.
  The 2D screens go through those cameras.
- `0x003148F0` in `func_00314780`: the camera controller turns its framing limits into angles with the aspect. That is how
  the camera follows Kratos, not what it shows, so it stays at 16:9.

A hardware watchpoint on `+32` during play caught those readers and no others. `func_00442568` reads it as well and puts
movies between bars by itself.

## 2D

With the orthographic cameras wider, the title lays its 2D out in the 16:9 in the middle without any help. A first version
also kept the HUD and movies narrow in the backend, and they came out 1.31 times too narrow (the death screen's text 638
pixels wide against 838 at 16:9). That code is gone. Two things are left to the backend:

- A flat vertex at or past the edge of the 16:9 goes as far past the edge of the screen, for draws on the display target in
  a frame with a 3D scene (a line added to the vertex programs, asked for through `vp_posoffset.w`). Fades, the death
  screen's backdrop and a cutscene's bars reach the sides that way.
- A frame with no 3D scene gets black bars beside the 16:9 (`present_to_backbuffer`).

## Drawing wider

Targets the size of the display are `width * scale * ratio` wide while the guest goes on seeing its own size, the same path
as internal resolution. On those targets `fp_alpha.w` is `-1/sy`, which tells a fragment program's WPOS to divide x by the
ratio as well.

## Verification

In a 1680x720 window (no ultrawide monitor here), with FSR 1 at 1.5x and 1x and FSR 3.1 with and without frame generation:

- Intro movie and title screen: 16:9 between black bars, the text as wide as at 16:9 (718 pixels).
- HUD in a fight, in the 16:9 in the middle. Death screen: text 836 pixels against 838, backdrop out to the edges.
- A cutscene and gameplay: nothing stretched, and more at the sides.
- Ultrawide off: 16:9 pictures as before, and no crash in any run.

## Not fixed, not tried

- The pause menu's backdrop stops short of the screen's edges, so the world shows at the far left and right. Neither
  dropping the `w` test nor taking in targets the size of the display moved it; its draw has not been found.
- Frame generation's pass for what moves (`fg_objects`) is off at these widths: with it on, close-ups of Kratos got coloured
  noise at 21:9 and not at 16:9. The cause was not found. Frame generation goes by the camera alone there.
- The first session at a wider aspect compiles the vertex programs again, because their text differs; until that is done,
  a cutscene can show dark or missing objects.
- A real ultrawide monitor and 32:9. The sides of some cutscenes may show places the camera was never meant to see.
