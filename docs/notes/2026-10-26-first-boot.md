# First build and first boots

## Build: the SPU programs

**Symptom.** The first configure stopped in the SPU pipeline: 0 of the 4 embedded SPU programs
registered. Three were refused with "unsupported instructions emitted by lifter" (300 `.word`s in
one of them), and the fourth was dropped with them because the pipeline registers all or none.

**Cause.** The programs have no section headers, and their read-only data (Edge Zlib's strings,
`" ZLIB 1.2.3"` and friends) sits in the executable segment after the code. The function detector
seeds a function at every direct branch target and at every `il`/`ila` immediate that is an address
in the segment. An `ila` that loads a string's address made the string a "function", and its words
lifted as unsupported instructions, or as branches far past the image.

**Fix (fork `1490435`).** When a program lifts with unsupported words, the pipeline detects its
functions again from what is reachable: the entry, the symbols and the indirect scans, keeping a
scan target only if the straight line from it reaches a return or an unconditional branch before a
non-instruction word or a branch past the image; then the direct branch targets of what was found,
until nothing new appears. Programs that lift cleanly the default way are untouched (God of War HD's
four, LBP's). All four of this title's programs now register.

The `Wws_Job` policy module (a raw SPU image at `0x7A7B00`, 12,288 bytes, FNV-1a
`0xC8D774963A96DCF2`, a newer build than God of War HD's) is lifted by `gowa.py lift` as in that
port.

## Boot 1: SPURS2

The title initializes SPURS with `cellSpursInitializeWithAttribute2` ("Big", 6 SPUs) and enables
the system workload. Neither was registered, so both were faked as `CELL_OK`: the instance was never
created, `cellSpursCreateTaskset2` (also unregistered) did nothing, the 13 `Wws_Job` workloads were
refused and the next taskset creation was rejected as "unregistered spurs".

**Fix (fork `83871e2`).** The SPURS2 initializer (an 8 KB CellSpurs2; the runtime still schedules
16 workloads, and the title adds 13 plus its tasksets), the system workload switch, the Taskset2
attribute and creation. `cellGamePatchCheck` (fork `6132e61`) now says the title is not a patch
instead of a faked success.

## Boot 2: Edge Zlib's queue

FIOS hands Edge Zlib inflate requests to an SPU task through an ANY2ANY SPURS lock-free queue
(owner the SPURS instance, 32-byte entries, depth 12), and `_cellSpursLFQueuePushBody` was
unregistered: the same wall as God of War HD. The port now carries that port's answer: `gowa.py lle`
relocates and lifts the firmware's `libsre`, and `stubs.cpp` runs the queue initializer and
PushBody as the firmware code, with the attach and the waiter's signal done by the port. The queue
initializes, attaches (SPU ports `0x10`/`0x11`) and the task wakes on every push.

## Boot 3: tasks and the workload flag

`_cellSpursTaskAttribute2Initialize`, `cellSpursCreateTask2`, `cellSpursGetWorkloadFlag` and
`_cellSpursWorkloadFlagReceiver` were unregistered. With them (fork `83871e2`) the five workers of
taskset `0x70037280` (SPU program `F4C2A58BDE92CE5E`, entry `0x6D1B00`) are created and signalled.

## Where it stands (boot 4)

No import on the boot path is unregistered any more. The title reads its PSARC archives, creates its
trophy context, lists its saves and starts `IntroLoaderThread`, then stops making progress:

- The main thread loops in `0x4193C0` on `while (*(u8*)0xA0B113) sys_timer_usleep(1000)`. The flag
  is set by `IntroLoaderThread` (in `0x41A0C0`) and never cleared.
- `IntroLoaderThread` calls `sys_timer_usleep(30)` about 28,000 times a second, inside the
  resource load at `0x2C2DA8` (called from `0x419BD0`). What it polls is not known yet.
- The `Wws_Job` workloads run, but the job code the policy module streams into LS `0x1A000` to
  `0x1D000` is not lifted and goes to the SPU interpreter; the module then repeats the same GET
  from its workload data (`data + 0x300`, `pc=0x25E4`) until the 30 s guard halts it.
- Each worker of taskset `0x70037280` issues a 4-byte PUT to EA 0 at `pc=0x37A0`, which the MFC
  refuses.

Ruled out: the main thread is not in an HLE call (watchdog), and the loader is not blocked in the
host (it is polling).

## Next

1. Find where the `Wws_Job` job code comes from (raw images in `GOWA.elf`, or the archives) and lift
   it; see whether the loader is waiting on one of those jobs.
2. The workers' PUT to EA 0: which field of their argument (`0xA1FDB0`) or context is null.
3. Bring the `gowa` branch up to date with upstream: 380 commits, some from another port of this
   title (Edge jobs, textures, the Vulkan backend).
