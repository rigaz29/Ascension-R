# The frame generation freeze came from the session before (2026-11-11)

The freeze of 2026-11-10 (`2026-11-10-fg-freeze.md`) came back in one of the player's four sessions with frame
generation on: at the boot logos, 11 frames in, `wait_for_gpu STUCK: want 9 got 7`, the `rsx render` thread at 104% of
a core in `amd_fidelityfx_framegeneration_dx12.dll`. A second "freeze" that day was a 17 s shader build in a load, which
the freeze watch blamed on frame generation too.

The laptop: an AMD Radeon iGPU drives the display, a GTX 1650 Ti renders. The player's settings: borderless 1920x1080,
VSync on, FSR 3.1 at render scale 1.5 with frame generation, SMAA, the stats overlay.

## Reproduced

A script (`fg_launch.ps1`, not in the repo) starts the exe on a test settings file with the player's settings, a copy of
their shader cache and fresh test saves, presses Play in the launcher, waits 35 to 40 s and closes the window, then
starts the next run. The freeze came back: 4 of 10 runs, then 4 of 8, every time at 11 to 13 frames, every time
`STUCK 2s: want 10 got 8`. The 24 boots of 2026-11-10 had none because `fg_boots.sh` ended every run with `timeout`, from
outside (below).

## What the GPU waits for

The spinning thread's stack, named from the DLL's exports, `.pdata` and code (FSR SDK 2.4.0, swap chain 3.1.7): a loop
on `RtlQueryPerformanceCounter` and a fence's `GetCompletedValue`, at the tail of the proxy's `Present`, waiting for the
next game buffer's last copy (the fence the DLL names `AMD FSR ReplacementBufferFence`).

With generation off, as at the boot logos, a present goes this way, all on the game's thread:

- game queue: wait for `CompositionFence GPU` of the last present; signal `GameFence`;
- present queue: wait for `GameFence`; copy the game's buffer into the real swap chain's; signal
  `ReplacementBufferFence` and `CompositionFence GPU`; the real swap chain's `Present`; signal `PresentFence`;
- game queue: wait for `PresentFence`.

A temporary probe read the proxy's own fences at the stall, at the offsets its code uses (`+0x16D0` the game queue,
which matched ours, `+0x16E0` to `+0x1708` the six fences). Two frozen runs, the same numbers: game 11, replacement 11,
composition 11, frames sent 11, **present 10**. The present queue copied and signalled frame 11, then stopped in the
real swap chain's `Present` of it, and never got to `PresentFence`: the game queue waits for that, our fence with it,
and the render thread spins in the next `Present`.

It is not the proxy's own doing. A variant that put the swap chain behind the proxy only at the first scene (the boot
on our own swap chain) stopped in 5 of 10 runs anyway, our own queue stuck at fence 6 in the same way: whatever swap
chain presents at that start, its present does not complete.

## What it is not

Each of these, changed alone, still froze:

- the launcher (without it, from PowerShell: 2 of 3);
- the console window (none, `CreateNoWindow`: 1 of 3);
- the session log, the pad recording, the missed-code recording (output into files: 1 of 5);
- the launcher's Direct2D on the Radeon, which brings AMD's hook `amdihk64.dll` in early (software Direct2D: 1 of 2);
- our pacing of the swap chain's threads (AMD's own, `D3D12_FG_SPIN=0`: 1 of 2);
- the game window being in front: it was not, in any of 8 runs of which 4 froze;
- minimizing or hiding the frozen window: the present stays stuck.

Frame generation off never froze: 0 of 10, and 0 of 6 with frame generation's flip interval set by hand.

## The cause

The runs froze in turn, 2, 4, 6 and 8: a run after a frozen one was always fine, a run after a fine one froze. A frozen
run was killed by the script, a fine one ended by its window, and the game ends itself then, `TerminateProcess` on its
own process. So the session before matters, and how it ends:

| How the session before ended | Froze |
|---|---|
| the window closed, the game ended itself (as it always has) | 4 of 8, 4 of 10 |
| the window closed, a thread with no window ended it | 4 of 8 |
| killed from outside, while it ran | 0 of 8 |
| the window closed, the game left standing, killed from outside 3 s later | 0 of 8 |
| started from Git Bash and ended by `timeout` | 0 of 49 |
| started from PowerShell, output into files, killed at 30 s | 2 of 15 |

A session with frame generation on that ends itself leaves the next one's start to stop its GPU about every other
time; one ended by another process seldom does. Why is not known: the difference is the kernel's teardown of a process
that terminates itself and of one terminated from outside, and what it leaves the display path of this laptop.
Sessions with frame generation off end the same way and leave nothing.

## What changed

- **The game is ended by another process (`main.cpp`).** When the window closes, the exe starts a copy of itself with
  `--end-process <pid> <code>`, handled first in `main`, which terminates the game and waits for it. When that cannot
  happen within 5 s, the game ends itself as before. The game is gone 91 ms after the close, and no process is left.
  A game that cannot start for a file of its folder (`game_open_failed`) ends the same way.
- **The freeze watch blames frame generation only for a GPU that stopped (`report.cpp`, fork
  `rsx_d3d12_backend_gpu_stuck`).** A stopped GPU is a wait for its fence that ran out and a fence that has not moved
  since. With frame generation on the log says so and where to turn it off; with it off it says the GPU stopped. A long
  load says nothing.
- **The launcher offers frame generation off only after that line (`report.cpp` `gowa_last_session`, `launcher.cpp`
  `fg_offer`).** It used to after any freeze with frame generation on, in place of the offer to make a report: a
  freeze of the game's own, such as a job that never ends (issue #7), would have asked about frame generation and
  never for the report. The last freeze of the session must be followed by "the GPU has stopped, behind frame
  generation's swap chain", as in every frozen start of the runs above.

## Measured

On the script above, frame generation on, each run closed by its window: 0 of 8 froze, and 0 of 6 again on the build
without the probes. The same script froze 4 of 8 just before, on the build that ended itself. The PowerShell row above
says an end from outside does not always save the next start: 2 of 15 there. The player's first start after this change may
still freeze once, left from a session of the old build.
