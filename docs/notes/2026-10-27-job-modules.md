# Step 7, lever L1: the 25 job modules the lift skipped

## Result

All 31 WWS job modules in `GOWA.elf` run lifted. Runs with only `GOWA_THREADS` on, same machine as
[PERF.md](../PERF.md):

| Scene | Before | After |
|---|---|---|
| Opening cinematic | 21.7 flips/s | 45-48 |
| Main menu | 0.40-0.50 | 2.03 |
| Profile screens | not measured | 9.0-9.7 |
| Story cinematic | 10-12 | 37-50 |
| First level | 0.17-0.23 | 3.2 on arrival, then 1.1-2.1 |

In the level, workload 2 went from 192 ms a run to 0.8 ms, workload 3 from 96 ms to below the table,
and the EDGE workloads from 11-12 ms a run to 0.6-0.8 ms. The policy modules use 206% of a core
instead of ~400%.

## What was done

1. **The second header form.** `lift_jobmods.py --engine-elf` took only headers with `0x4000` at
   `+0x28`. 25 of the 31 have `0` there. `SPU_DMA_RANGE=6F4A00-7A8000` showed each copied from its
   header's EA to LS `0x4000`, like the six. Their `+0x14` is not the loaded size (`0x2B1A0` for a
   10,128-byte module), so they are bounded by the next header in the segment (fork).
2. **Two modules stalled the game once lifted.**
   - `0x6F8800` ended its policy-module run at `0x5658` in the intro loader.
   - `0x79C400` ended its run at `0x81C8` at the start of the level load.

   Both runs ended at the return point of a call into the job API's atomic-update helper. That helper
   calls a callback through a register (`bisl $0,$87`) between its GETLLAR and PUTLLC. The callback
   was not a lifted entry, so it ran in the interpreter. The interpreter then went on through the rest
   of the helper and into the lifted caller's frame, and the run ended there.

   Holding interrupts while a module's lifted code runs was tried first and changed nothing: the
   interrupts were all taken in policy-module code.
3. **Entries from the runs.**
   - `SPU_INTERP_ENTRIES=<lo>-<hi>` (fork) logs every distinct pc the interpreter is entered at,
     with the image that owns it.
   - `gowa.py jobmods-entries <logs>` adds the ones inside lifted modules to
     `scripts/jobmods_extra.json`.
   - The cycle is lift (`gowa.py lift --jobmods-only`, a minute), build, run, merge. It converged
     after three rounds on the intro and two on the level route: 178 entries in 18 modules, with no
     new ones on the last run and no `[pm-end]`.
4. **Tools for the next misbehaving module.**
   - `SPU_JOBMOD_SKIP` / `SPU_JOBMOD_ONLY=<ea,...>` leave modules out at run time, to bisect without
     a lift or a build. The modules above were found this way, in five runs.
   - `--skip-ea` and `JOBMOD_SKIP` in `gowa.py` leave one out of the lift (empty now).

## What is next

- **L2.** The interpreter still runs 18-22 M steps a second, now at LS `0x11000-0x12400` in the
  level (`0xD800`, `0xEC00`, `0x11400` on the menu): the streamed fragments.
- **Audio.** Workload 7 (audio) is the largest SPU user left, at 82% of a core.
- **Trace the level again.** The level slows from 3.2 to ~1.3 flips/s as it goes on. Trace it
  again (`PS3_FTRACE`) to see what bounds that frame now.
