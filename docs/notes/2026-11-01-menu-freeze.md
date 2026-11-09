# The menu-start freeze: an EDGE list nobody kicked again

Written 2026-11-01. The runtime side is on the fork's `gowa` branch.

## Symptom

About one run in fifteen, the port froze right after the opening movie closed (~30.5 s after the
start), before the main menu drew its first quad.
- The RSX walker stood at a self-JUMP park, an EDGE command-buffer hole, in the first frames
  after the movie.
- The 10 s `GCM_PARK_GRACE_MS` ran out, the resync dropped the commands up to `put`, and the
  title never flipped again.
- The main thread spun at ~98% of a core.
- No SPU halt or assert in the log. The 2026-10-29 park freeze (an SPU task's assertion) was a
  different cause.

The bench retried such runs ("frozen in the menus"); a player would have had to restart.

## How it was found

A hunt script started the port again and again, went through the movie and stopped ~14 s after
it (~46 s a run). `GCM_PARK_DUMP_MS=2000` dumps a park that stands 2 s; the hunt kept that run's
logs. Ten captures over the day, each dump richer than the last. All of it is off unless
`GCM_PARK_DUMP_MS` is set:

- **SPU writes.** Every SPU PUT into RSX-visible memory, in a ring of 256K, with the SPURS
  workload that wrote it.
- **The walker's writes.** The newest main-memory GET_REPORTs and NV406E semaphore releases,
  each with its slot or label as it reads now; the GET_REPORT headers and parks in the FIFO from
  256 KB before the park to 512 KB after it.
- **The PPU side.** Every guest thread's in-flight syscall, last call site and guest call chain
  (`ppu_threads_dump`).
- **The SPU tasks.** The signal-driven tasks, parked or busy, and their signal bits.
- **The title's state** (port).
  - The job lists of every workload, entry by entry: each wait with the halfword it waits on,
    and the job descriptors.
  - The batch counters, the MLAA block and barriers, the frame label.
- **One ring for both sides.** `SPU_SIG_RING=1` keeps `SPU_SIG_TRACE`'s lines (atomic commits to
  watched lines; task WAIT, WOKE and SIGNAL) and `PPU_WWATCH`'s stores in one ring.
  - It is printed at the dump. `SPU_SIG_RING_DUMP_S=<s>[,<s>...]` prints the whole dump, ring
    included, at those seconds, for a run that does not park.
  - Printing each line as it happened slowed the tasks whose timing was being traced, so it is
    kept in memory instead.

## How a frame's EDGE work is scheduled (as traced)

- **The counters.** All have the form `(count << 16) | (wid << 8) | flags`; whoever takes one to
  zero kicks that workload. Per frame the main thread arms (`func_0041C344`, `func_0041C474`):
  - an MLAA batch counter, 6: the five MLAA tasks and RsxSync's job A;
  - the MeshProc batch counter at `0xA0B724`, 5: one per MeshProc job;
  - two frame-slot counters, 1 each, which RsxSync's job A releases.
- **The EDGE lists.** EDGE0-2 (wids 14-16) take turns, one list per frame. The main thread writes
  it after the counters:
  - entry 0 waits on the MeshProc counter, entry 1 on a frame-slot counter, then nops;
  - then one job per hole; each job descriptor holds its hole's address at `+0x44`.
  - `func_0049CA88` resets a list's ticket row when the list is reused; `func_0049DDCC` and
    `func_0049DEBC` write the entries.
- **The MeshProc counter is one word**, re-armed for each frame's list.
- **RsxSync, per frame:** wait for report slot 2k, then job A. Job A takes the last count off the
  previous MLAA batch's counter and releases the frame-slot counters. Then wait for slot 2k+1,
  then job B, which posts the next MLAA batch (taskset `0x70037280`, five tasks, label, counter,
  control block `0xA1FDB0`).
- **At the transition** the main thread also syncs with the RSX. While `[0x84BCEC] != 0`, the top
  of its frame (`func_003F5748`) waits for label `0x4F0 == 1`, a texture-read semaphore the RSX
  releases, and clears it.

## Root cause

In every frozen capture the park was the **first hole of one EDGE list**:
- both of the list's waits were met;
- its ticket row was 0/0;
- nothing had kicked it.

The list had been written, but its policy module never took entry 0. The signal ring showed why:

1. The MeshProc counter (wid W) reached 0 and kicked W.
2. Six events later the main thread had already re-armed the same counter for the next frame:
   count 5, the next wid. The next MeshProc batch took it to 0 again and kicked the next list.
3. W's policy module read its entry-0 wait after the re-arm, saw 5, and went idle.
4. Nothing kicks W after that. The SPURS idle probes do run its module, but without a kick it
   does not look at its lists.
5. Its holes were never filled, so the RSX stood at the first one. The main thread then stood in
   its texture-label sync for good.

The opening movie's last frames are trivial, so the main thread set up two frames back to back.
On the real machine it takes a frame's time between the two.

**Proof.** A probe that signalled the EDGE workloads from the park dump, 2 s into the park,
brought the frozen run back: no resync, 29.8 flips/s in that window and 53 in the next.

## Fix

- **Fork.**
  - `GCM_PARK_STALL_MS=<ms>`: once a park has stood that long, and every `<ms>` after while it
    stands, the walker calls the port's `gcm_park_stall_hook`.
  - `spurs_signal_workload`: raises a workload's signal bit the way an SPU's
    `cellSpursSendWorkloadSignal` does.
  - `spurs_workload_unkicked`: tells whether a workload is neither running nor asked for.
  - `tools/test_gcm_park_stall.py`: no call before `<ms>`, one per `<ms>` after it, and a hole the
    hook fills is followed to `put`.
- **Port.**
  - `wws_rekick` (`GCM_PARK_STALL_MS=100` by default): an EDGE list whose next entry is a wait
    that is already met, on a workload nothing runs or has kicked, is signalled.
  - The wait being met is the very condition the policy module waits for, so the signal only lets
    it see that.
  - It acts only while the RSX stands at a park, and only on a stranded list.

## Verification

- **Interleaved A/B, one binary** (44 runs; the run was stopped there). Odd runs had the default
  (100 ms); even runs had `GCM_PARK_STALL_MS=0`, no re-kick.
  - Without the re-kick, 2 of 22 runs froze at the menu start, both as described above: EDGE0's
    list (run 8) and EDGE2's (run 36), both waits met, the ticket row 0/0, nothing kicked.
  - With it, none of 22.
- **Every run with the re-kick** (four hunts, 65 runs): no menu-start freeze. Two runs logged a
  re-kick, each 109 ms into a park on EDGE1's first hole, and went on at the usual rate:
  ```
  [wws-rekick] RSX at park 0x005F4BE0 for 109 ms; w15 EDGE1 L3 entry 0 waits on 0x00A0B724, which is met, and nothing runs it: signalled
  ```
  - The window after it had 37.7 and 35.8 flips/s, as the runs around them.
- **The level's speed**, an A/B bench, two runs each: the committed build 19.65 flips/s (runs
  19.50, 20.12), with the re-kick 20.27 (20.36, 20.27), within the spread. Peak memory the same,
  ~3.3 GB.
- **The runtime's tests** (WSL): 119 pass, 5 fail, all from before (they were 110 and 13). The
  eight GCM walker fixtures link again: they had lacked a `spurs_debug_dump` stub since the park
  dump began to call it.
- **Another stall, open.** Three runs of the first build with the fix stopped during the opening
  movie, at the first post-process frame (~23 s).
  - The main thread waits in `func_0049D688` for a job list to finish. RsxSync's policy modules
    are on all five SPUs, polling its list, which is done. No workload is kicked. The RSX stands
    at the write head (`0x2FD8D8`) or idles.
  - Not the re-kick: none of the three logged one, and the stall did not come back in the 74 runs
    after them (30 of the committed build, 44 of the A/B, with the re-kick and without).
  - The three came at 06:35, 06:43 and 07:01, all in the first 43 runs of that build.
  - For next time: the park dump now shows all 18 workloads (they share one policy module) and
    marks a list a main-thread wait would still wait for (`INCOMPLETE`).
    `SPU_SIG_RING_DUMP_S=<s>` prints that dump at a set time, for a stall with no park.

## Ruled out on the way

- **A report the RSX wrote before the PPU armed its slot.** The reports before the park were all
  written, and their slots were released.
- **A lost MLAA signal**, as on 2026-10-27. All five tasks did their batch; the barrier was
  clean.
- **Two releases of a flag label merged, one lost.** Label `0x4F0` is not written by NV406E
  releases. A bounded hold on such releases never triggered in 45 runs, and one of the next runs
  froze. The hold was dropped.
- **The MLAA counter one short "by itself".** It is one short because job A, its sixth
  decrement, waits for the next frame's report.
