# Roadmap: from here to the first frame, the menus and the first level

Written 2026-10-27 after the first two days of bring-up, from what this build measures and from
the notes of the two other ports of this title
([God-of-War-Ascension-Recompiled](https://github.com/MaChInEgUn3/God-of-War-Ascension-Recompiled),
"ref A"; [gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc), "ref B"). Both run
the same engine and SPU programs on ps3recomp, from disc releases, and are past the first level.

## 1. Where it stands (measured)

- Boot, archives, trophies, saves, the intro loader: fine. No unregistered import, no SPU error, no
  FIFO resync.
- No frame is flipped. The main thread waits per frame slot (`0x41CBDC`) for the batch counters at
  `0xA0B740`; the RSX waits (`SEMAPHORE_ACQUIRE`, IO `0x2FEE54`) for the MLAA tasks' label; the MLAA
  control block (`0xA1FDB0`) is empty because RsxSync (wid 17) takes no entry of its job list.
- **Update, end of 2026-10-27:** steps 0-4 and 6 done, step 5's text part done. Driven with
  `PAD_FILE`, the title goes from boot through both cinematics, the main menu, profile creation and
  difficulty to its first level and pause menu
  ([first screen](notes/2026-10-27-first-screen.md), [text](notes/2026-10-27-text.md),
  [to the first level](notes/2026-10-27-to-the-first-level.md)). The level's 3D scene is almost
  black and runs at ~0.35 frames/s. Next: step 7 (speed), then 3D drawing.
- **Update, later 2026-10-27:** the main menu and the first level's opening scene render
  ([render units](notes/2026-10-27-render-units.md)); the level runs at 2.4-4.4 flips/s.

## 2. How a frame is made (both refs, confirmed here where marked)

```
PPU frame build ──► job lists of the Wws_Job workloads (wids 0-17)
   │                  MeshProcJobs/RenderJobs ─► EDGE0/1/2 fill the command ring's holes
   │                  RsxSync (4 lists, entries wait on report slots)        [confirmed]
   ▼
RSX walker ──► GET_REPORT into report slots (+0xC = 0 releases RsxSync)      [confirmed]
           ──► GET_REPORT onto spurs+0x60 (flag = 0 kicks RsxSync)          [confirmed]
           ──► SEMAPHORE_ACQUIRE label 0x510 == batch                        [confirmed]
RsxSync jobs ─► MLAA control block {batch, label, counter, buffer} + signal to 5 tasks
MLAA tasks ──► label = batch (releases the RSX), counter-- (0 kicks an EDGE wid)
EDGE jobs  ──► patch the ring's self-JUMP parks; batch counters reach 0 ──► main thread goes on
vblank handler ─► patches the frame's park; flip
```

## 3. Gap matrix (what the refs needed, what this runtime has)

| Needed for | Item | Here |
|---|---|---|
| boot | SPURS2 entry points, Taskset2/Task2, workload flag, PatchCheck | done |
| boot | Edge Zlib queue | done (lifted libsre, ref B uses host zlib) |
| boot | GCM offset table follows map/unmap | done |
| frame 2 | 32 workloads, tempArea, flags1 | done |
| frame 2 | GET_REPORT to main memory, SEMAPHORE_ACQUIRE blocks, park/acquire exempt from stuck-get | done (upstream + port env) |
| frame 2 | RsxSync consumes its 4-list job list | done (policy modules of wids 16-31 ran nothing) |
| frame 2 | MLAA tasks get batches; Task2 without creation signal; Taskset2 event flags | done |
| frame 2 | a report waits while signal-driven tasks run a round (lifted MLAA is slow; ref B 0029) | done (`GCM_REPORT_SPU_HOLD_MS`) |
| frame 2 | each HLE taskset its own wid (SPU-side SendSignal kicks the taskset's wid; ref A allocates 31 down) | not needed so far |
| loading | SPU-created tasks in a sibling taskset start (two-stage batch, ref A M0.23, ref B 0019) | done |
| loading | one start per task, taskset bitsets under the lock line | done (upstream) |
| loading | `GCM_FIFO_CIRCLE_PASSES=0` (the title's `put` stays fixed, ring driven by parks) | done |
| loading | PPU lifter fixes: spill/reload, spilled jump-table base | done |
| stability | MFC engines released, SetFlipCommand wrap only for SDK contexts, `bisl rt==ra` | done (upstream) |
| menus | X16 font atlas, framebuffer sRGB, texture gamma, `DDX`/`DDY` | done in D3D12 (the vtable path; X16 also needed the SRV format) |
| menus | texture units 4-15 in the D3D12 vtable path (it binds t0-t3) | open; nothing visible missing yet |
| cinematic | `cellVdec` with FFmpeg | done: both cinematics play |
| speed | the other 25 `0xC0DEC0DE` job modules, indirect entries, kernel pass without the 1 ms sleep, MLAA cost (lifted ~100x slow) | **step 7** |
| frame order | hold a patched park while SPURS tasks run (ref B 0017), readyCount as a level (ref B 0011) | later, if frames misorder |

## 4. Steps

Each step ends with a measurement that says it worked. Addresses are this build's (PSN 1.12).

### Step 0: tooling and hygiene (done 2026-10-27)

- **A pipeline probe** in the port (`GOWA_PIPE=<s>`): every n seconds, one block with the main
  thread's slot and batch counters, the MLAA control block, the RsxSync and EDGE list rows, the report
  slots, the RSX get/put and the word at get, the label being acquired. Replaces a dozen manual
  `GOWA_PEEK` runs per question.
- **A per-workload policy trace** in the runtime (`SPURS_PM_TRACE_WID=<wid>`): for that wid only,
  every run with its DMAs (EA, LS, size) and the exit, so one RsxSync run can be read whole.
- `GCM_FIFO_CIRCLE_PASSES=0` in `main.cpp` (ref A M0.21).
- Relift the PPU code with the merged lifter (jump-table base fix; build ~25 min, in the background).

### Step 1: RsxSync takes its job list (done 2026-10-27)

Cause: `spu_run_policy_module` refused wids >= 16, so EDGE2 and RsxSync never ran; see
[notes/2026-10-27-rsxsync.md](notes/2026-10-27-rsxsync.md). What follows is the analysis as it
stood before.

Fact: every one-list workload (list 3 at `data+0x3C`) consumes its entries; RsxSync, the only
four-list workload, has tickets `0x000D0000` and per-list words `0x1F` untouched, though its policy
module runs, kicked by the flag and readyCount. Ref A saw `0x000D0001` at the same point.

Read one wid-17 run with the policy trace: which lines it GETs (the list line it caches at LS
`0xD00`, its EA at `0x1500`), which list index it settles on, and where it exits. Candidates, in
order: what the module expects at `data+0x10..0x2C` for a four-list workload; the SPU lane against
the list's lane mask (`0x1F`); readyCount/flag semantics (a readyCount level alone was ruled out).
Done when the tickets move and the control block at `0xA1FDB0` gets a batch.

### Step 2: the MLAA batch round trip (done 2026-10-27)

Cause: batch N+1's signal ended batch N's barrier waits; fixed by holding reports while the tasks
run a round. See [notes/2026-10-27-first-screen.md](notes/2026-10-27-first-screen.md).

Measured: batch 1 served but one task short (barrier 4 of 5, counter one short), batch 2 untouched,
all five tasks in WAIT_SIGNAL. First tool: a per-taskset signal timeline.

With batches posted: the RsxSync job's SPU-side `cellSpursSendSignal` reaches the five tasks
(taskset `0x70037280`); give HLE tasksets their own wid if the signal path needs it (step 2 of the
matrix). Lifted MLAA is slow (ref A: ~1.7 s a batch); if batches are lost, serve them at the signal
the way ref A does (no anti-aliasing) and keep the lifted path behind a switch. Done when label
`0x510` takes the batch number and the RSX passes `0x2FEE54`.

### Step 3: the first flips (done 2026-10-27)

The stop after step 2 was the two-stage SPU task batch, as expected: tasks an SPU created in a
sibling taskset never started. Screenshot: the brightness calibration screen.

Batch counters at `0xA0B740` reach 0, the main thread leaves `0x41CBDC`, flips happen. Expected
next stops from the refs: the two-stage SPU task batch (SPU-created tasks, step 3 of the matrix),
EDGE holes in the ring. Done when `gowa_drive.ps1` gets a screenshot.

### Step 4: loading screen and first-use dialog (done 2026-10-27)

No code needed: the title goes on to the main menu, creates a profile (save data in the port's own
`gamedata/dev_hdd0`) and loads its first level. See
[notes/2026-10-27-to-the-first-level.md](notes/2026-10-27-to-the-first-level.md).

The save-data prompt (`cellSaveData`, works in God of War HD), then the loading screen. Ref B reached
the first-use dialog right after the SPU-created task fix.

### Step 5: what is drawn (text done 2026-10-27)

Text reads, in its gold, on the calibration and settings screens
([notes/2026-10-27-text.md](notes/2026-10-27-text.md)). The calibration screen's lighter right half
is its design. Units 4-15 done with the 3D scenes ([notes](notes/2026-10-27-render-units.md)). Left
for later: a comparison with RPCS3 on a 3D scene.

D3D12 backend against this title: sRGB framebuffer, texture gamma, `DDX`/`DDY` in fragment programs
(ref B's live draw needed all three for readable menus), then a frame-by-frame comparison with RPCS3.

### Step 6: the opening cinematic (done 2026-10-27)

Both cinematics play through `cellVdec` and libavcodec, start to end.

`cellVdec` with FFmpeg as in God of War HD; check the cinematic format and the fade card that waits
for the movie (ref B: the "black screen" was that card).

### Step 7: speed to a usable menu (the current wall)

Mapped in [PERF.md](PERF.md). L1 done: all 31 job modules lifted (menu 0.5 -> 2.0 flips/s,
level 0.2 -> 1.1-3.2, cinematic 21.7 -> 45); see
[notes/2026-10-27-job-modules.md](notes/2026-10-27-job-modules.md). Then `usleep` on a timer, quiet
task logs, and the EDGE lists on three SPUs: menu 3.6, level 2.4-4.4
([notes](notes/2026-10-27-host-waste-and-lanes.md)). Next: position-independent lifting for the
EDGE fragments (L2).

The other 25 `0xC0DEC0DE` modules (ref A lifts 33, ref B 33 plus streamed-code fragments), indirect
entries from `SPU_INTERP_STATS`, the SPURS kernel's 1 ms sleep between passes (ref A: 2.95 → 6.25
frames/s), and the MLAA tasks.

### Step 8: the first level (reached 2026-10-27)

It loads (`R_MEGAERAN`) and pauses. Its 3D scene renders since
[notes/2026-10-27-render-units.md](notes/2026-10-27-render-units.md); the pause menu has no labels.

Ref B: a lost-task race in the taskset scheduler; ref A: frame segments and EDGE holes (M0.24) and
the shared MFC engines (fixed upstream).
