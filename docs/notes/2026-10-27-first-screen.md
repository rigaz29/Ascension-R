# Roadmap steps 2 and 3: the post-process round trip, the two-phase tasks, the first screen

The title now draws its first-use screens: brightness calibration, then the next settings screen
once Cross is pressed. Text is drawn as blocks, the frame rate is about 2.4 a second, and the
right half of the calibration screen is lighter than the left; those belong to steps 5 and 7.

## Step 2: the MLAA batch round trip

**Tool.** `SPU_SIG_TRACE=<ea>[,...]` (fork `d71f05af`): for the listed 128-byte lines, every SPU
atomic commit with the words it changes and who made it, and for a listed taskset every
WAIT_SIGNAL enter and exit of its tasks and every signal the runtime delivers.

**Timeline** (taskset `0x70037280`, barrier `0xA1FE00`, counters `0xA0B700`):

1. RsxSync's job posts batch 1 and sets the five tasks' signal bits; all five wake.
2. Tasks 2, 3 and 4 finish their share, arrive at the barrier (count at `+0x00`, waiter mask at
   `+0x10`) and wait for its release.
3. 63 ms later, before tasks 0 and 1 arrive, the job posts batch 2 and sets all five bits again.
   Tasks 2, 3 and 4 wake from the *barrier* wait on it: the batch-2 signal is spent there.
4. Tasks 0 and 1 arrive; the last one releases the barrier, batch 1's counter reaches 0.
5. All five wait for the batch-2 signal at LS `0x3980`. It never comes again; the RSX waits on
   label `0x510 == 2`.

That is ref A's account (M0.17). The tasks share one signal bit between the batch wait and the
barrier wait; on hardware the RSX takes a frame's time between the two reports, so the race never
happens. Lifted, a batch takes about 100 ms and the walker takes none.

**Fix (fork `6d50f1ce`).** `GCM_REPORT_SPU_HOLD_MS=<ms>`: while a task that works in signal-driven
rounds is out of WAIT_SIGNAL, or parked with its bit set, a GET_REPORT stops the drain pass
unconsumed, as an ACQUIRE does, for up to `<ms>`. The port sets 3000. Ref B's patch 0029 has the
same idea; this keeps the lifted tasks (and the anti-aliasing) instead of serving batches at the
signal as ref A does.

On the way, a race in the parked-task table (fork `973e26c5`): two tasks parking at once took
the same slot and one was left out, so the hold saw it busy for its whole wait (3 s give-ups).

**Result.** Batches flow (223 by 80 s), labels `0x510`/`0x520` follow them, no hold gives up.

## Step 3: the two-phase particle tasks

**Symptom.** With batches flowing, the RSX caught up with `put` at about 40 s and stayed there.
The main thread (`GOWA_WATCHDOG`) sat in `func_003D3794`, `usleep(30)` until
`*(0x9C7668) == *(0x9C767C)` (5).

**Measured.** `SPU_SIG_TRACE=9C7600`: five tasks of taskset `0x70034900` (image `323211119`) each
add 1 to `+0x64` (1 to 6) and exit. The one that completes the count creates five tasks in the
taskset at `+0x74` (`0x70031F80`) with the ELF at `+0x78` (`0x6C3E00`, image `405488072`, already
lifted); those add to `+0x68`. At the stall, `0x70031F80` had tasks 0-4 enabled and pending, none
running, and task info pointing at `0x006C3E04` (ELF with flag bits). Ref A (M0.23) and ref B
(0019) describe the same pattern.

**Cause.** The runtime started an enabled task only when a task of the *same* taskset exited, and
`spu_taskset_start_pending` had no caller. Tasks created from another taskset never ran.

**Fix (fork `c56bc029`).** Every taskset is registered at creation; a committed SPU line that is a
registered taskset's first line starts its enabled, ready or pending tasks that are not running.

**Result.** The second phase starts every frame (810 starts in 95 s), the main thread goes on,
flips continue, and the calibration screen is drawn. Cross moves on to the next screen.

## Probe

`GOWA_PIPE` now prints the flip and present counts on its first line.

## Next

Step 4/5: the screens are there and take input; their text is blocks. D3D12 against this title:
the font atlas, sRGB and gamma, `DDX`/`DDY`, and the lighter right half. Then the save-data prompt
(`cellSaveData` already loads `BCUS98232-SYSDATA`), the opening cinematic (step 6) and speed
(step 7: ~2.4 frames/s on a static screen).
