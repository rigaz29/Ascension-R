# Shadows on in the fight: a GPU hang at the level's start

Written 2026-11-07. The user asked for patches that raise the frame rate, then to profile the
shadows and drop the effect patches that did not help. Disable Shadows is on by default; the user's
settings file had it off.

## The effect patches, dropped

SSAO (`fxSSAOProcess`, update `func_004455A4`), rim light (`fxRimLightProcess`, `func_00443F88`) and
screen warp (`fxScreenWarpProcess`, `func_004447A0`) each test their enable at +0x20 the way Depth of
Field and Motion Blur do, so each had a one-instruction patch (`lwz r4,0x20(r31)` -> `li r4,0`).
All three together, FSR 1, no frame generation:

| | the three on | the three off |
|---|---|---|
| gate, two runs a side, the order swapped | 38.06, 34.09 | 34.48, 36.22 |
| fight, Disable Shadows on, one run a side | 32.94 | 33.37 |

The first run of a pair is always the fastest (the machine warms). No gain above that, and the
rim light is visibly gone from Kratos. Not kept.

## The fight with shadows on

The scripted parasite fight (`logs/parasite`), FSR 1, no frame generation, Disable Shadows off.

- **The GPU hangs as the level starts**, at frame ~3,500 (the loading screen into the fight):
  `wait_for_gpu STUCK 2s`, then `Present failed ... removed=0x887A0006` (`DXGI_ERROR_DEVICE_HUNG`).
  - Four runs of four (`logs/edge-ana/fg7/fx/sh1`, `fa`, `fb`, `hb1`).
  - The title runs on with the device gone, at 9-20 flips/s, and the screen stops changing.
  - So the 12.3 and 13.0 flips/s first read as the cost of shadows were a lost device. Those runs
    have no screenshots, which should have said so.
- **A run with the sampler at 1 ms** (`sh2`) did not lose the device. The title printed its own
  `MoveLocalMemory dst offset error` / `CopyLocalMemory ... offset error` 7,392 times (offsets past
  256 MB) and stopped flipping. Seen in no other run in `logs/`.
- **Runs that did not hang, one each:**

| | flips/s | |
|---|---|---|
| `RSX_DYN_DEPTH_BIAS=0` (`dyb0`) | 18.37 | |
| the same, sampler at 4 ms (`sh3`) | 19.71 | the profile below |
| `VTX_RESIDENT_MB=0` (`hr1`) | 23.84 | |
| `GOWA_EDGE_SKIN=0` (`hs1`) | 27.28 | |
| the bone fix below (`fx2`) | 29.37 | |
| Disable Shadows on, for comparison (`fa2`) | 32.94 | |

- **But with the bone fix, three runs of five still hung** (`fx1`, `dr2` and a run whose folder was
  later reused; `fx2` and `dr1` did not). One run a side only moved the timing; none of those
  switches was the cause.

### What the hang is

`D3D12_DRED` (a probe, not committed): DRED's page faults, and its breadcrumbs.

- **No page fault**: `GetPageFaultAllocationOutput` returns VA 0. Nothing read or wrote outside
  an allocation; the device was removed for taking too long.
- **The breadcrumbs**, two captures. Each stopped at a `ResourceBarrier` (op 15):
  - the frame's list at its second operation of 3,219, `46 15 15 15 8 15 15`;
  - (`rb1`) at operation 987 of 6,765, right after another barrier that had completed:
    `4 4 4 4 4 15 [15] 15 15 8 15 15`; the vertex pass's list and the next frame's queued behind.
- A probe of each list's first barriers (`rbp`, the resource, its description, the states) showed
  nothing wrong in them: the backbuffer, the display targets, texture uploads.

### Switches that did not change the rate

With the bone fix, runs stopped at the hang or after 120 s:

| | hung |
|---|---|
| `RF_IDLE_WAIT=1` (every batch waits for the GPU) | 1 of 3 |
| the branch base below | 1 of 2 |
| `VP_CB_GEN=0`, `VP_CB_SHARE=0`, `PREPASS_SHARE=0` | 1 of 3 |
| `GOWA_EDGE_GPU=0`, `GOWA_EDGE_MEMO=0` (no EDGE work on the GPU at all) | 1 of 1 |
| `D3D12_SHADER_COMPILER=fxc` | 1 of 3 |

Probes in the same runs found nothing wrong: the polygon offsets (factor 2, units 1,355-14,499 in
500,000 draws), the loop constants of the five branching vertex programs (`c[314]` = 0, 1e-4, pi,
1; `c[467].x` 0 or 1).

## The cause

A probe at the two places a target is made anew (`off_rt_get`, `off_ds_get`), a run that hung:

| frame | the shadow atlas, colour 0x01550000 and its depth |
|---|---|
| 3384 | made at 48x66, then 638x668, then 980x1404: three times in one batch (records 4,340, the prepass at 2,335) |
| 3393 | 980x1408 |
| 3428 | 982x1408, then 1024x1408 |
| 3429 | 1098x1408, in the middle of the batch; **the device was lost in this frame** |
| 3461 | 1280x1408 |

The title fills its shadow atlas a light at a time and binds it at the size it has filled so far.
The prepass sizes a batch's targets before the draws' views are copied, but record-ahead runs it in
parts (`rf_prepass(pre_done, upto)`), so a later part can ask for more of a target the earlier
parts' draws already have. Made anew there:

- the draws prepassed before it sample the old resource, through the views copied into their
  windows;
- the barriers recorded for them move the new one, the slot's resource when they are recorded;
- so the old depth surface is read in `DEPTH_WRITE`.

## The fix

`grow_later` in the backend: a target the batch has already bound or sampled (`last` or
`last_frame` is this frame) keeps its size, and the size asked for is kept in `grow_w`/`grow_h`. The
next batch makes it at that size when it first takes it, before any of its draws has a view of it.
Until then the batch draws into what the target has: a few frames as the level starts, when the
atlas is being made anyway. `RT_GROW_NOW=1` makes it anew at once, as before.

- Four runs of four did not hang (`gl1`-`gl4`). At ~50% before, four in a row by chance is ~6%.
  The probe shows the atlas made at 48x66 and the larger sizes left for the next batch.
- The build with only the fix, one full fight run (`vf1`): 30.56 flips/s (windows 28.8-32.0), the
  device kept, the pictures right, Kratos and the chains with their shadows. Disable Shadows on was
  32.94 (`fa2`): shadows cost ~7% in the fight now.

## Also found

- **A fence value used twice.** `wait_for_gpu_once` incremented `fence_values[fi]` and signalled it;
  `move_to_next_frame` then signalled `fence_values[fi]` again for the batch after it, the same value,
  already complete. That batch was not waited for by the next `rf_open` (submissions 3,496 and 3,497
  of a probe run had the same last value signalled, 3,593): the next batch could reset its allocator
  and texture ring under it, and its ZPASS counts and main-memory surfaces were read as done. Not the
  hang (`RF_IDLE_WAIT=1` still hung).
  - **Fixed after:** the wait signals the frame index's next value and leaves the one after it, as
    D3D12's samples do; `D3D12_FENCE_REUSE=1` as before.
  - Fight (the user's settings had Disable Shadows on again by then), the fix first, ABBA: 36.67 (the
    first run, the machine cool), old 34.95 and 34.91, fix 34.83. Gate with shadows on, old then fix:
    27.53, 27.88. No stuck wait in any run (`fn_*`, `gn_*`).
- **The branch base.** `rsx_vp_decompile` takes branch targets as slots of the whole store less
  `rsx_vp_set_branch_base`, which only `rsx_live_draw.c` set. The D3D12 backend now sets it to the
  program's start (0 for `compile_vp`); `VP_BRANCH_BASE0=1` as before. This title's five branching
  programs start at slot 0, so it changes nothing here.

### The RSX thread with shadows on

`sh3`, the sampler on the frame clock thread every 4 ms, the last 60 s (14,727 samples), with
`RSX_DYN_DEPTH_BIAS=0` so that it would not hang:

- `vp_get_fp_pso` 39.5%, `rf_prepass` 10.8%, the D3D12 runtime and driver 19.6%.
- The thread is at 95%: it sets the frame.
- Without the dynamic depth bias, each polygon offset is a pipeline of its own: the run made
  3,858 of them (the fight's cache went from 2,285 to 6,143). A draw looks its pipeline up in its
  program's chain, which holds all of them. So the dynamic depth bias stays on: the hang has to be
  fixed with it.

## The bone fix

The vertex pass's skinning (`skin_rows` in `spu_edge_gpulight.c`) read a bone's matrix at the
palette + index * 48 for any index. The CPU version of the same arithmetic (`ek_blend`,
`spu_edge_gpuskin.c`), the one `GOWA_EDGE_SKIN=2` checked bit for bit against the SPU, takes a bone
the palette does not have as its first. `vtab` (t1) is a root view, read without bounds.

A probe at the jobs' first look (`GOWA_EK_PROBE`, not committed), the fight with shadows on:

- 1,536 jobs taken, 584,488 vertexes;
- 6,787 bone slots past the palette, 6,730 of them with a weight;
- e.g. a lit job of 432 vertexes, 6 matrixes, the highest bone 228.

The shader now does as the CPU does: `bone(i, nm)`, `nm` the palette's count at +4 of the skin
record. Kratos and the fight look as before (`fx2`). It is not the hang: with it, three runs of
five still hung, and DRED recorded no page fault.

## The RSX thread at the gate with shadows on, by line (2026-11-08)

After the fix the gate with shadows on runs ~28-30 flips/s with the frame clock thread at 97%: it
holds the gate. A sampler run (`logs/edge-ana/fg7/rp/gm`, 4 ms, the last 60 s, 14,484 samples) on a
build with line tables for the runtime (a temporary `PORT_RT_LINES`: `-gline-tables-only` on
`ps3recomp_runtime`, `/DEBUG` at the link, mapped by `llvm-symbolizer` with inlined frames):

| | % of the thread |
|---|---|
| `rf_prepass` | 16.2, of it `alias_note` 3.8, the depth pass over all 16 units 1.9, `off_rt_find` 1.9 |
| `rsx_process_method` | 8.1, spread over its chain of compares |
| `gcm_rsx_process_fifo_unlocked` | 6.1 |
| `rf_vp_draws` | 5.9, `vp_sampler` 1.4 |
| `gcm_3d_method` | 4.3, the trace's compares 1.6 |
| `vp_get_fp_pso` | 3.4, `dr_cube_mask` 1.2 |
| outside the executable (D3D12, the driver, memcpy) | 28 |

Done (fork `72ff0c97`): a 1024-bit set of each table's offsets answers `alias_kind`'s and
`off_rt_find`'s misses; the depth pass visits only the units with their `tex_zok` bit;
`gcm_3d_method` tests the trace's switch before its compares. ABBA at the gate in one build: 29.34
against 28.38 flips/s, +3.4%. The thread is still at 97-98%, and the main thread is now at 97% too.

A trap on the way: running `cmake` on `build/` by hand, outside `gowa.py`'s toolchain environment,
left mingw's tools and empty `CMAKE_*_FLAGS*` in the cache. The next rebuild compiled the lifted
code without `/O2` (the gate at 11.7). The cache was put right from `build-jc`'s entries.

### Methods the dispatcher does not take

`rsx_process_method` (8.1% of the thread) ran ~90 tests for every method that reached its chain. A
method none of them takes walked them all to the `-1` at the end, and `RSX_CENSUS` at the gate (10 s)
counts ~320,000 a second of those: `0x0B00`-`0x0B64`, `0x1D00`-`0x1D3C`, the reports `0x1800` and
`0x17C8`, `0x0300`, `0x0374`, `0x147C`, `0x1EF8`. A script over the function's compares
(`m_tests.py` in the scratchpad: equality and range, names resolved from the headers) found no test
for any of them, and found the two that look the same but are not: `0x018C` and `0x0194` end at `-1`
too, after the chain sets the colour DMAs from them. Done (fork `ec08a1a4`): the
fast switch returns `-1` for the first list.

ABBA at the gate with shadows on: 30.76 and 28.03 against 28.47 and 27.53 (the old runs dipped to
25.4 and 23.7 early in the scene), about +2-5%. The final build, one run: 30.41.

### The PPU main thread

With the RSX thread at 98% the main thread showed 97% too, but most of it waits (sampler at the
gate, 10 s, shadows on): `sys_timer_usleep` 32.8%, `poll_mwaitx` on the SPU jobs' counters 19.3%,
the title's own code 33%. `PS3_POLLTOP` puts a quarter of the thread's time at one wait, lr
`0x0041D5C8`: the frame counter of the frame's EDGE job chain, whose jobs fill the rings the RSX
thread empties. The wait for the RSX `ref` (`0x003A3698`) is 2.9%. The RSX thread stays the wall.

## The render thread (2026-11-08)

`perf/render-thread-2` (2026-11-05, three commits) records, submits and presents each batch on a
thread of its own, "rsx render", while the frame clock thread walks the FIFO and captures the draws.
It gained nothing then: the SPU workers set the frame. Picked onto the 25 commits since, with one
conflict (`vp_vs_raw`, whose raw variant's text had been split out of it). The set of target offsets
from `72ff0c97` is now built aside and copied a word at a time, since a debug print in the walk's
present (`DISP_RT_DBG`) can look a target up while the render thread rebuilds it. Everything else
added since runs on the render thread's side: `grow_later`, the alias set, `dr_cube_mask`,
`vp_sampler`, the texture checksums, frame generation's markers.

Mode 2 (the walk waits for each present), in one build:

| | off | on |
|---|---|---|
| gate, shadows on | 30.66, 28.57 | 33.65, 32.65 |
| fight | 32.20 | 35.20 |
| fight, shadows on | 27.82, 30.01 | 27.23, 28.90; 29.09, 28.92, 28.48 |
| fight, FSR 3.1, frame generation, VSync (the player's settings) | 30.01, 29.66, 29.69 | 29.64, 29.63, 29.94 |

At the gate the walk ("frame clock") falls from 98% of a core to ~50%, and "rsx render" takes ~60%.
The fights are the PPU main thread's (93-95% in every run). There the render thread changes nothing
past the runs' spread, and frame generation holds the game at 30 on this 60 Hz display anyway. The
first five windows of the player's-settings fight looked lower with it on after two runs each
(27.96, 28.22 against 29.60, 28.76), and the third pair undid it (29.55 against 28.39). No lost
device and no `wait_for_gpu STUCK` in 11 runs, 25 with the branch's. The shots match at the gate; in
the fight the pad script drifts with the rate, and the shots show no fault.

Texture uploads read guest memory on the render thread (`rf_prepass`). In mode 2 that is no later
than the batch's end, where the walk waits, which is when they were read before record-ahead.

`graphics.render_thread` (`RSX_RENDER_THREAD`; Render thread in the launcher's Graphics tab) is on by
default: `off`, or a mode's number (fork `f5ec8726`).

Mode 1 (the walk goes on from the batch's reset while the render thread submits and presents) hung
the GPU in two of four runs on 11-05. A candidate, not tried: from the reset the walk writes the
next batch into the upload halves (`vp_parity`) of the batch before, which the GPU may still be
reading (`move_to_next_frame` waits for it only after the present), and the raw-fetch draws read
through root views, unchecked.

Also seen: `rf_open` drains the GPU before it opens a batch ("full serialisation costs little",
written for the samples), so in both modes a batch is recorded only after the last one has run. The
GPU shows 35-39% in the fight, so this is latency, not yet the limit.

### The pointer and the taskbar

With the render thread on, the player saw the pointer and the taskbar flash over the borderless
window. A watcher from outside (`logs/edge-ana/tools/winmon.py`: every ~2 ms the foreground,
`IsHungAppWindow`, the cursor, `SHQueryUserNotificationState` and the taskbar's z-order, gate,
FSR 3.1 with frame generation and VSync) found the window hung for 4 to 900 ms every ~5.5 s: 23
times in 140 s, the foreground lost 43 times, the taskbar topmost again 11% of the time. With the
render thread off, never.

A probe inside put every episode in `rt_wait`, 15-31 ms after the main loop's pump. The walk, which
owns the window, waited for the render thread in `MsgWaitForMultipleObjects` for sent messages alone.
Windows takes a thread blocked in such a wait, its queue unread for 5 s, for a hung one, and a
`PeekMessage` on an empty queue does not count as reading it. Without the render thread the walk
never waits like that.

Now the walk takes every message while it waits (`rt_wait`, `rt_excl_begin`) and waits for input
too, as a window's thread should while another presents (fork `5a96891a`). The watcher found no
episode in 140 s. With the render thread on, closing the window only marks it, since that thread may
be presenting into it; the main loop ends the process on the mark, as it did.

