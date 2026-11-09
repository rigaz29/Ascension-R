# A profile of what the runs missed (2026-11-08)

The first item of [the survey](2026-11-08-recomp-feature-survey.md). SPU code with no lift runs in the interpreter, and
from outside that is only a slow scene. The 20 EDGE pieces behind the brothel room's 3.5 frames a second
([the note](2026-11-08-brothel-illusion.md)) were found by hand. Now the run writes down what it missed and the next
lift takes it in.

## What it does

- `ps3_missed_code_to(file)` (`runtime/diag/missed_code.c`) keeps one row per interpreter entry: the 16 bytes there, the
  GET that brought that code in, the steps and the calls. It also keeps the PPU calls that found no lifted function.
- Streamed code runs from wherever it lands, so the transfer names it. Each thread keeps its last 64 GETs out of the
  executable, and an entry goes to the newest one that covers it.
- Each launcher session records to `build/logs/missed_code.txt`, adding to what earlier sessions left.
  `PS3_MISSED_CODE=<file>` records in any run, `=0` not at all. The file is rewritten within 10 s of a new row.
- Recording is off until asked for: the three hooks are NULL, and a run with them NULL does what it did before.
- `tools/missed_code.py` checks each row's bytes against the ELF, so a profile from another build is refused, and sorts
  the rows: an entry of a job module goes to `scripts/jobmods_extra.json`, one inside a listed piece to that piece's
  `entries`, and code a GET brought from the ELF to a new piece in `scripts/spu_pic_pieces.json`, unless it holds an
  absolute branch. PPU calls go to `scripts/ppu_extra_functions.json`. Rows listed already and interpreted all the same
  are reported, since a further lift will not change them.
- `gowa.py lift` applies it first and moves the profile to `.applied`, so the next session shows what the lift did not
  cure. `gowa.py missed` only shows it.

## How it was checked

The player's route, replayed, with the 20 pieces left out of the lift at run time (`SPU_PIC_SKIP`):

| | |
|---|---|
| Flips/s in the room | 3.49, as in the player's report |
| Profile | 27 rows, 388 M interpreted steps |
| The tool, on a copy of the list with the 20 taken out | all 20 back, with the EA, size and entry found by hand |
| That list against the one in the repository | byte for byte the same |

With nothing left out and the recorder on, flips/s in the windows 40-50 s and 50-60 s from the load were 26.8 and 23.6;
three runs without it gave 25.0 to 27.4 and 20.8 to 24.1. It is one run, and would not show a difference under about
one frame a second.

`tools/test_missed_code.py` covers the sorting, `--apply`, the refusals and the recorder. The PPU path is tested only
there: this title makes no call that finds no lifted function.

## What it showed

Normal play leaves 7 rows, 7.2 M steps in a 100 s run. All are in job modules, just past where the lift stopped:
`jobmod_82d19f847a87_e6978` runs 0xAC50 and its `code_end` is 0xAC4C. The tool leaves them alone. This is the 0.1 M
interpreted steps a second that the room shows after the fix, and it is not worth a lift of its own yet.

One replay during this work hung while loading: `256 identical transfers to ea=0x70039D00 -- halting the SPU` on every
lane, and no flip after it. The same message is in 3 of about 340 older runs (`lc2`, `sh2`, `ski1`), from before this
change. It is rare, and it stops the game for good.
