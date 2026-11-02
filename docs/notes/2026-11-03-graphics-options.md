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
