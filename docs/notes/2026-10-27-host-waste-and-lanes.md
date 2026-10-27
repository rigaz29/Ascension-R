# Step 7, after L1: host waste, the level's critical path, EDGE lanes, and what L2 really is

## Results

Flips per second, runs with only `GOWA_THREADS` on:

| Change | Cinematic | Menu | Level | Other |
|---|---|---|---|---|
| after L1 | 45-48 | 2.0 | 3.2, then 1.1-2.1 | process CPU ~450% |
| L3 (`usleep` on a high-resolution timer) + L4 (task log lines) | 50-52 | 2.0 | not measured | process CPU 352%, log ~170 KB a run |
| EDGE on three SPUs (`SPURS_MAXCONT`) | 48-50 | 3.6 | 2.4-4.4 | no abnormal policy-module end |

## L3 and L4 (fork `09e2a316`, `d41da61e`)

- **L3.** `usleep` of 200 us and more now sleeps on a per-thread high-resolution waitable timer;
  shorter waits still spin. Against `SYS_USLEEP_SPIN_MAX_US=1000` (the old spin): `EdgeWatchdog`
  97% to 3% of a core, process 447% to 352%, cinematic +5-8%. The menu is unchanged, because it is
  bound on the SPU side.
- **L4.** The five per-task lines are kept for the first 32 tasks, then only with `PS3_VERBOSE`.
  The gain in frame rate is too small to measure; a run's log is ~170 KB instead of tens of MB.

## The level's critical path

`PS3_FTRACE` 540-560 s, frame 860 ms:

- The main thread waits 780 ms of each frame.
- SPU 0 is busy 90%: EDGE 16, 15 and 14 take 29%, 29% and 26% of the window, ~1,000 runs a frame
  at ~0.75 ms each.
- SPU 5 runs the audio (82%). SPUs 1-4 are 3-12% busy.

Every workload has a maximum contention of 1 (`0x7001AD50`), and the EDGE workloads' per-SPU
priorities allow SPUs 0-3, so the three lists ran one after another on SPU 0.

## EDGE lanes (fork `a35c36f1`, port default)

`SPURS_MAXCONT=14=3,15=3,16=3` lets each EDGE list run on up to three SPUs. Menu 2.0 to 3.6, level
2.4-4.4. The menu renders as before. The level's 3D scene is still black, so geometry order cannot
be checked yet; check it again when it draws. `SPURS_MAXCONT=14=1,15=1,16=1` restores the title's
value.

## L2 is position-independent code

The interpreter still runs ~20 M steps a second, at LS `0x11000-0x12400`.

- **Tried and ruled out.** Residency loss and failed verification: `SPU_OVL_CLEARLOG` shows only
  module changes at `0x4000`, and `SPU_OVL_VERIFYLOG` shows no rejection. Keeping a big module's
  tail resident (it is loaded at `0x4000`, then smaller modules over its head) changed nothing and
  was dropped.
- **What it is.** `[interp-entry]` with code bytes and the resident spans found it. Module 210
  (`0x737580`, span up to `0xF77C`) calls code at `0x11000-0x11F00` that no module owns. Those bytes
  come from EA `0x74F540-0x766280`, between two module headers. The same bytes run at
  `0x1149C`, `0x1150C`, `0x1153C`, `0x115EC`, `0x116CC` and `0x117CC`: position-independent
  fragments packed into a job's buffer, at a different place each time.
- **What it needs.** Ref A's one-image-per-placement does not fit that. It needs position-
  independent lifting (ref B 0023/0031): PC-derived values relative to where a fragment lands, the
  fragments recognized by content wherever they land, and lifted calls into them dispatched to that
  placement.
