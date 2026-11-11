# The Grotto freeze (issue #7): the EDGE memo's state in the other EDGE module (2026-11-12)

## What the reports have

The reporter: the Kit 0.1.0-alpha.3, an i9-12900K and an RTX 3080, 2560x1440 at 240 Hz, render scale 2, frame
generation off. Four sessions froze the same way a few seconds into play from a save near the Grotto (CYN900, CYN950),
three with Disable Mesh Trimming on and one with it off: at 151,394, 5,702, 2,803 and 10,418 frames. Each had
`[SPU] BRANCH-TO-0 unresolved`, `lr=0x0DC34` in the last three, then `FIFO resync (jump-park)` and the freeze watch's
line: an EDGE job ended early, and the command buffer it fills never got its commands.

The Grotto's EDGE jobs run `jobmod_9b03908f93a9_e77C8` (ea 0x71C500), the second build of EDGE's geometry job; the first
levels run only `jobmod_e8f44fbc6dbf_e6D98`. The issue's first reply named e6D98; the addresses are e77C8's.

The register dump of the last session:

- `lr=0x0DC34` is the return of `brsl $r0, 0xDB48` at 0xDC30, in the function at 0xDB88. 0xDB48 reads a function pointer
  from the object in r3 (+0x88) and branches to it: r3 came from r81, where 0xDB88 keeps its object.
- r81, r82 and r83 held `90111213 14151617 80010203 3F800000` and two more such quadwords: the vertex piece's
  (EA 0x75BD10) patterns, quadwords 3, 11 and 2 of its pool, which it stores in its frame at r1 + 0x20 + 16 k. In the
  frame's order (pool quadwords 5 6 7 8 9 10 0 1 2 11 3 4) they are slots 10, 9 and 8, at r1 - 0x20, - 0x30 and - 0x40
  of the piece's caller: where a function entered with that r1 restores r81, r82 and r83 from. Something had run an
  epilogue, without its prologue, right after the piece.
- The lane's last drain steps were 0xDB88 and then the piece: 0xDB88 calls a callback of the object at 0xDC10, the
  vertex piece, and the next use of r81, at 0xDC14, had it broken.

## Why

The EDGE native code keeps a job's state per thread: the memo of kept results (`t_m`, with `t_k` and `t_r`,
`spu_edge_memo.c`). `edge_memo_job()` empties it when an e6D98 job starts (`gowa_edge_job_ad98`, its entry), and the
hooks in e6D98's functions fill it. The seven hooks on the position-independent pieces (vertex unpacking, decoding,
positions, velocity, the compare) read it too, and both EDGE modules call those pieces. Nothing emptied it for e77C8's
jobs: one started with what the last e6D98 job on its thread had left there.

With a kept result's state (2), the vertex piece's hook calls `edge_memo_unpack`, which gives the result up
(`edge_memo_revoke`): it runs the calls that job had left out, e6D98's 0xF538, 0xE550 and 0xBBC8 by name, on this job's
local store with that job's arrays and matrices, and leaves r0 as the last of them had it. The piece then returns there,
into e77C8's code: the epilogue that restored r81-r83 from the piece's frame.

A probe on the Grotto replay below, one run before the fix: 12,652 of 1.57 M calls into the memo from those hooks found
an e6D98 job's state in an e77C8 job, and 6 of the first 40 gave a result up, `r0=0x0DC14 r1=0x3F560`: the reporter's
frame. That run froze too (4,843 frames, the FIFO resynced on an unmapped JUMP and CALL), the first freeze here. Whether
a job of e77C8 follows a job of e6D98 with a kept result on the same thread is up to the job order, which is why it
came on one PC and not another. Disable Mesh Trimming did not matter.

## What changed

- **`gowa_edge_job_b7c8` (`native/spu_edge_job.c`, `scripts/spu_native_hooks.json`):** e77C8's entry (0xB7C8) empties
  the state as e6D98's does. Its jobs never fill it: none of those hooks is in its code.
- **`edge_memo_revoke`:** the calls left out are e6D98's, so it never runs them in a job of another module (the module
  header's entry offset at LS 0x4010 is not 0x6D98): it drops the state and says so once in the log
  ("`[edge-memo] a job of the module with entry 0x... had a kept result's state: dropped`").
- **The lift of e77C8 past 0xB90C** (`27f7cae`, fork `ab764f6f`): the code only a pointer reaches, which the Grotto's
  jobs ran interpreted, is lifted, and Disable Mesh Trimming's edit in e77C8 applies. It did not end the freeze by
  itself: the run that froze had it.
- **`report.cpp`:** a report's saves could not be loaded. The game refused them: the stat callback gave -3 for
  `BCUS98232-SYSDATA` and -5 for `BCUS98232-PROFDATA-0`, and "The preferences save data failed to load" came back
  again and again. The report left out `ICON0.PNG`, and the zip gave every file the report's time, where a profile's
  `DATFEST` holds the time it was written (a big-endian u32 at 4, the time its file has). The icon goes into the report
  now, each file's entry has the file's time, and `save/times.txt` has them to the second.

## How it was tried

The reporter's saves on a test hdd0 (`ICON0.PNG` from this PC's saves, every file at DATFEST's time), their patches,
and their session 073649 played again with `PAD_REPLAY`: it loads the save, reaches CYN900 and the end of the recording,
where their session froze, and goes on about 100 s with the last report held. 150 s a run:

| Build | Runs | Froze | |
|---|---|---|---|
| 0.1.0-alpha.5 | 2 | 0 | 38 rows, 1.2 G steps interpreted, e77C8 past its end the most |
| with the lift | 3 | 0 | 1 row, 516 k steps interpreted |
| with the lift, a probe in the memo | 1 | 1 | the probe above |
| with the fix, the probe | 1 | 0 | 0 of 3.1 M calls found another job's state |
| with the fix | 2 | 0 | the drop never said |

The Megaera fight on the fix, one run: 29.22 fps; the memo found a result for 92.5% of the jobs it looked up, 91.8% of
the unskinned and 95.7% of the velocity jobs, as before; the drop never said. Another probe, in one of the lift's runs,
found every one of 10.5 M interrupts gave r80-r127 back unchanged: the interrupt handler was not it.

## Not done

The freeze came once in six runs here before the fix, so runs alone cannot show it gone; the probe shows its cause
gone. The reporter's PC, where it came every time, is the test.
