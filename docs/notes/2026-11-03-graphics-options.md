# The graphics options in God of War: Ascension

Checked 2026-11-03. The options came with the runtime from the God of War HD port:
`[graphics] scaling`, `sharpening`, `antialiasing` and `anisotropy` (the launcher's Graphics
page). The question was whether they work in this title.

## Where they act

- `scaling` (bilinear, sharp bilinear, AMD FSR 1), `sharpening` (FSR 1's RCAS) and
  `antialiasing` (SMAA 1x) act in the presentation pass (`present_to_backbuffer`). That pass takes
  each presented frame from the 1280x720 display target into the window's buffer.
  - The window is the display's size times the system's DPI scale unless `display.window_size`
    says otherwise. This laptop's 125% gives 1600x900, so the picture is always scaled up.
  - FSR 1 only scales up: a window no larger than 1280x720 gets bilinear.
- `anisotropy` acts in the per-draw samplers (`rsx_sampler_desc`):
  - A sampler that filters linearly both ways is made anisotropic at that level.
  - Point-sampled lookups stay as they are.
- The fragment decompiler samples with `Sample` (implicit derivatives) for TEX, so the level
  applies. TXL (`SampleLevel`) is left as the title wrote it.

## Runs

Level runs, `gowa.py bench` with the options given per variant (new: a variant's fourth field is
the executable's own arguments, `--set section.key=value`; `gowa.ini` is not changed). Window
1600x900 unless noted.

| option | presentation line | result |
|---|---|---|
| bilinear | `1280x720 -> 1600x900, bilinear` | reference |
| sharp | `..., sharp bilinear` | text and edges crisper |
| FSR 1, sharpening off / high / max | `..., FSR 1` | sharper in that order (mean absolute Laplacian of the luma 7.1 / 12.2 / 14.3, bilinear 5.5); no ringing at max |
| SMAA | `..., bilinear, SMAA 1x` | staircase edges smoothed (the floor's rim, the chains) |
| anisotropy 16 | (sampler census) | 15 of the level's 21 samplers raised to 16x; the other 6 are point-sampled |
| borderless, FSR 1 + SMAA + 16x | `1280x720 -> 1920x1080, FSR 1, SMAA 1x` | correct picture |

- No run logged an error: no device hang, no failed pipeline, no "unavailable".
- **The title asks for no anisotropic filtering.** With `anisotropy = game` all 21 samplers are
  point or linear. The new census line shows this:
  `[census] samplers made: N point or linear, N anisotropic at the title's level, N raised to
  graphics.anisotropy`.
- Cost, ABBA, two runs each (`logs/gfx-opts2`):

  | | bilinear | SMAA | anisotropy 16 |
  |---|---|---|---|
  | flips/s | 20.97 | 20.86 (-0.5%) | 20.94 (-0.1%) |

  Both are within the runs' spread. FSR 1 and sharp bilinear, one run each (`logs/gfx-opts`):
  21.38 and 21.48 against 21.63.
- Screenshots are in `logs/gfx-opts`, `logs/gfx-opts2` and `logs/gfx-opts3`.
  - Each run's screenshot is of a slightly different moment.
  - So the variants were compared by crops of the same features (the HUD text, the floor's rim,
    the chains), not pixel by pixel.

## The frame rate on screen

`display.show_fps = overlay` (the launcher's "On screen, with CPU/GPU load") draws a panel in the
top left corner, made 2026-11-03 at the user's asking.

| row | what it is |
|---|---|
| FPS, MS | presents a second, and their mean interval, over the last half second |
| CPU | the process's CPU time, of all the machine's logical processors |
| GPU | the frames' GPU time, from their timestamps, over their wall time |
| PPU | the host thread "ppu main" |
| SPU | the "spurs ..." and "spu ..." threads; six of them make 100% |
| RSX | the "frame clock" thread: the FIFO walk, the recording and the present |
| graph | the last 96 frame times, 0-66.7 ms, with a line at 33.3 ms (30 frames a second) |

- **Look:** labels in colour, values in white, on a rounded box of 80% near-black. The text is
  1/60 of the window's height (12-28 pixels).
- **Sampling:** a thread of its own ("overlay stats", below normal priority) reads the threads'
  CPU times every half second. The GPU timestamps are now taken with the panel, not only with
  `RSX_CENSUS`.
- **Cost:** GDI draws the panel at most ten times a second. The fight ran 29.98 flips/s with it,
  against 30.10 without (`logs/overlay1`, `logs/combo1`).
- **Screenshots:** they show it (F12, `display.screenshot = output`).
