# Choosing the discrete or the integrated GPU (2026-11-09)

The renderer always asked Windows for the high-performance adapter. Now `graphics.gpu` says `discrete` (the default, the same as
before) or `integrated`, the low-power adapter; `D3D12_GPU` sets it from the environment. `CELLMARK_IGPU` still works.

## Fix

Fork ad3336e8: `d3d_device_create` takes the preference from the setting. The game and the precompile both come through it. An adapter
that cannot run Direct3D 12 is skipped as before, so a choice that cannot be honoured falls to the next one, and the log's
`[D3D12] adapter:` line says which it was.

On a UMA adapter `s_vram_mb` now counts the memory it shares. The texture cache's budget is a third of it: from the 495 MB the Radeon
in a 4600H has of its own that was the 256 MB floor, the size of the PS3's whole video memory.

`launcher.cpp`, Graphics tab: a row with Discrete and Integrated and, under it, the two names. It asks Windows the same two questions the
renderer does, so the name shown is the GPU the game runs on, and the Ready check line follows the choice. The same adapter for both means
one GPU, and the row is not shown.

The shader cache needs nothing: it holds programs, not a driver's binaries, and the pipelines are built on the device at each start.

## Measured

Ryzen 5 4600H with its Radeon, GTX 1650 Ti, one run each.

| | Route, FSR 3.1 and frame generation, scale 1.25 | Scripted fight, FSR 1, scale 1 |
|---|---|---|
| GTX 1650 Ti | 21.9-30.3 flips/s | medians 27.7-36.1, eight earlier runs |
| Radeon | 14.3-14.8 flips/s | 17.2-23.2 |

Below 25 flips/s frame generation stands aside, so on the Radeon it adds nothing.

The first start on a GPU builds its pipelines through a cold driver cache: 4,518 in 55.7 s on the Radeon, against 0.5 s on the GTX 1650 Ti
whose driver had them. The next start took 3.1 s. The right picture on the route on both GPUs.

## Not tested

Only this laptop: no PC with one GPU, so the hidden row was not seen, and no desktop with the monitor on the discrete GPU.

A second run of the route on the Radeon did not get past the menu in 400 s. The game was alive (21 flips/s throughout, no `[freeze]`), but ten
of the files the recorded route waits for did not come in the 5 s it allows, and its presses went on without them. That recording is too
tight for a GPU this slow. The scripted fight, which reaches the game another way, ran.
