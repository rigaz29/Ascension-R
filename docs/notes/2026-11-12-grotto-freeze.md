# The Grotto freeze (issue #7) and job code past code_end (2026-11-12)

## What the reports have

The reporter: the Kit 0.1.0-alpha.3, an i9-12900K and an RTX 3080, 2560x1440 at 240 Hz, render scale 2, frame
generation off, Disable Mesh Trimming and the other four patches on.

Three sessions froze the same way, each a few seconds into play from a save near the Grotto (CYN900, CYN950): at
151,394, 5,702 and 2,803 frames. Each had one `[SPU] BRANCH-TO-0 unresolved`, `lr=0x0DC34` in the two short ones, then
`FIFO resync (jump-park)` and the freeze watch's line: the job that should have filled the command buffer ended early.

Their `missed_code.txt` (76 SPU rows, 31.0 G steps interpreted): the Grotto's EDGE jobs run
`jobmod_9b03908f93a9_e77C8` (ea 0x71C500, its image LS 0x4000-0xFF00), the second build of EDGE's geometry job, which
the first levels do not run. It ran LS 0xB914-0xF1BC interpreted, 27 entries; its lift ended at 0xB90C. `lr=0x0DC34` is
in there. The issue's first reply put the drain steps' addresses in `jobmod_e8f44fbc6dbf_e6D98`, the first build: they
are e77C8's.

The same, one function or two just past the end, in modules every session runs (the five rows of every boot are these):
e6978 at 0xAC50 (8.8 G steps in the reporter's sessions), e7B40 0xBE18, e0B48 0x4CA0, e11A08 0x1B6E0, e0270 0x43B8 and
0x4620, e0450 0x4598 and 0x4B00, e8F70 0xD2D8, eE430 0x175C0.

## Why

`lift_jobmods.py`'s code_end is one past the highest instruction reachable by direct branches from a module's seeded
entries. Code only a pointer reaches lies past it and was never lifted, and `missed_code.py` refused such rows ("outside
the code that was lifted"), so `lift` never took them in.

Disable Mesh Trimming has an edit in each EDGE module (`scripts/title_patches.json`), and the lifted code takes them. The
one in e77C8, at 0xE524, was in the code past 0xB90C: in the Grotto the patch did nothing, while e6D98's jobs ran with
it.

## What changed

- **`missed_code.py` (fork):** a row past a module's code_end is an extra entry of the module when the module's image
  holds the row's 16 bytes there. As a seed of `module_code_end`'s walk it moves the end out to it. A row whose bytes
  leave the image is refused. `tools/test_missed_code.py` has one of each.
- **`scripts/jobmods_extra.json`:** the reporter's list and the Grotto runs below. Code ends: e77C8 0xB90C -> 0xF1C4
  (351 -> 562 functions), e6978 0xAC4C -> 0xAD40, e7B40 0xBE14 -> 0xBF08, e0270 0x43B4 -> 0x4708 (and 0x4708 itself for
  the next lift), e0450 0x4594 -> 0x4BB8, e0B48 0x4C8C -> 0x4D30, e8F70 0xD2D4 -> 0xD3C8, e11A08 0x1B6DC -> 0x1B770,
  eE430 0x175BC -> 0x17650; eAF80 three entries inside. Disable Mesh Trimming's edit in e77C8 is lifted now.
- **`report.cpp`:** a report's saves could not be loaded. The game refused them: the stat callback gave -3 for
  `BCUS98232-SYSDATA` and -5 for `BCUS98232-PROFDATA-0`, and "The preferences save data failed to load" came back
  again and again. The report left out `ICON0.PNG`, and the zip gave every file the report's time, where a profile's
  `DATFEST` holds the time it was written (a big-endian u32 at 4, the time its file has). The icon goes into the report
  now, each file's entry has the file's time, and `save/times.txt` has them to the second.

## How it was tried

The reporter's saves on a test hdd0, `ICON0.PNG` taken from this PC's saves and every file at DATFEST's time, their
patches, and their session 073649 played again with `PAD_REPLAY`: it loads the save, reaches CYN900 and the end of the
recording, where their session froze, and goes on about 100 s with the last report held. 150 s a run:

| | Froze | Interpreted |
|---|---|---|
| 0.1.0-alpha.5 | 0 of 2 | 38 rows, 1.2 G steps, e77C8 past its end the most |
| with the lift | 0 of 2 | 1 row, 516 k steps (e0270 0x4708) |

Screenshots at the same places are the same. The frame rate the overlay showed there: 35 and 38 against 44 and 44.

The Megaera fight, one run each: alpha.5 29.47 fps with 8 rows and 29 M steps interpreted; with the lift 28.30 fps (28.33
in another run) and 1 row of 522 k steps. The fight's runs move by up to 10%. No module lost its residency to the longer
spans: a function that did would be interpreted all the same, and listed.

## Not known

The freeze did not happen here on alpha.5 either. What turns the interpreted path into a branch to an address with no
lift on the reporter's PC is not known, nor whether the patch's half-applied state was part of it. The lift takes that
path out of the interpreter and applies the patch in both modules; whether that ends the freeze is for their PC to
show.
