# Roadmap step 4: from the first-use screens to the first level

No code change: with steps 0-3 and 5 in, the title runs from boot to its first level when driven
with `PAD_FILE`. The screen-adjust screen did not hang; an earlier test pressed too early.

## The path, as driven (one run, about 25 minutes)

| Input | Screen |
|---|---|
| boot, ~55 s | brightness calibration (first use only) |
| Cross | screen adjust (scale, location X/Y) |
| Cross | the opening cinematic (`cellVdec`, libavcodec), then a loading screen with embers |
| (none) | "Welcome to God of War: Ascension", the March 2013 news, version `01.12F0.1` |
| Circle | main menu: Single Player, Latest News, Options, a 3D background |
| Cross | Game Profiles, four empty |
| Cross, Cross | "Create a new Profile?" Yes: `BCUS98232-PROFDATA-0` saved |
| Cross | Profile 1: New Game, New Game+, Continue Game, Chapter Select |
| Cross | Difficulty: Easy, Normal, Hard, Titan |
| Cross (Normal) | the story cinematic, then the first level loads (`R_MEGAERAN`, `R_HERON`) |
| Start | the pause menu (L1/R1 tabs, button icons) |

Saves go to the port's own `gamedata/dev_hdd0` (`gowa.ini`), not to RPCS3's.
`BCUS98232-SYSDATA` and `-PROFDATA-0` now exist there, so the next boots skip the calibration
screens and can use Continue Game.

## What is wrong in the level

- **Speed.** About 2.4 frames/s on the 2D screens, slower on the main menu's 3D background (a
  screenshot took ~20 s to come), about 0.35 frames/s in the level (the particle tasks, created
  five plus five per frame, count the frames: ~140 in 7 minutes). Every test past the menus takes
  minutes; this is the first thing to fix (step 7).
- **The 3D scene is almost black.** Only particles and a few glows show, in the level and behind
  the pause menu. The main menu's 3D background is blurred colour blobs.
- **The pause menu has no labels**, only its button icons and L1/R1 tabs; the 2D screens before
  it draw their text.
- Each particle task starts on a new host thread (`dispatch HIT (async) ... spawning thread`), ten
  thread starts per frame; and the per-task log lines are most of the log (12k each in 4 MB).

## Next

Step 7 before the rest: measure where an in-level frame goes (PPU, the lifted SPU jobs and tasks,
the walker, the backend), with `GOWA_PIPE` and the existing perf switches, using the saved profile
to get there faster. Then the black scene (step 5 for 3D: render targets, the units 4-15 the D3D12
path does not bind, the EDGE geometry) and the pause menu's text.
