# The frame pipeline: 32 SPURS workloads, MLAA tasks, the RSX semaphore

## Two other ports of this title

Two other projects port God of War: Ascension on ps3recomp, both further along (menus, the first
level), both MIT:

- [MaChInEgUn3/God-of-War-Ascension-Recompiled](https://github.com/MaChInEgUn3/God-of-War-Ascension-Recompiled)
  (US disc `BCUS98232`, Vulkan). `docs/STATUS.md`, `docs/DEBUGGING.md` and
  `docs/FEASIBILITY.pt-BR.md` describe the frame pipeline milestone by milestone.
- [arnaldolima1/gow-ascension-pc](https://github.com/arnaldolima1/gow-ascension-pc) (EU disc
  `BCES01741`), with its runtime changes as numbered patches against ps3recomp.

They target disc releases, so addresses differ from this PSN 1.12 build, but the engine and its SPU
programs are the same: the MLAA task ELF has the same fingerprint (`F4C2A58BDE92CE5E`). Their notes
named most of what follows; the code here is this port's own.

## The pipeline, as the notes describe it and this build confirms

- The main thread waits per frame slot (`(frame + 2) % 3`, here `0x41CBDC`) for batch counters
  `(count << 16) | (wid << 8) | flags` at `0xA0B740` to reach zero.
- EDGE0/1/2 (wids 14-16) and RsxSync (wid 17) are `Wws_Job` workloads. RsxSync's four job lists hang
  off `data + 0x30`; entries are `{cmd, arg}`: 1 job, 3 nop, 4 end, 2 and 5 wait on the halfword at
  `arg`. Its row of tickets is `data + 0x40` (published << 16 | consumed).
- The RSX releases RsxSync's waits with `GET_REPORT` into main memory (the report's zero word lands
  on the halfword the list waits for), and kicks it with a report onto `spurs + 0x60`, whose zero
  word is the workload flag RsxSync receives.
- RsxSync's jobs hand each frame to the five MLAA tasks through a control block (here `0xA1FDB0`:
  batch, label EA, counter EA, buffer) and a signal. Each task writes the batch number to an RSX
  label and decrements a counter.
- The RSX waits for that label with `SEMAPHORE_ACQUIRE` (here label offset `0x510 == 2` at IO
  `0x2FEE54`).

## Fixes

- **18 workloads, 16 slots** (fork `0f01ee34`): EDGE2 and RsxSync were refused. A CellSpurs2 now
  has 32 workloads with libspurs' layout for the second set (readyCount at `0x10`, state `0xC0`,
  signal `0x78`, info `0x1000`, contention in nibbles, `SF1_32_WORKLOADS` in `flags1`), and the policy
  module's kernel context gets the instance's first line in `tempArea` at entry and at each poll.
- **A hang in my own `_cellSpursWorkloadFlagReceiver`** (same commit): it lowered the flag with
  `vm_write32` under `spu_lockline_lock`, which `vm_write32` also takes; not recursive. Found with
  the watchdog ("inside _cellSpursWorkloadFlagReceiver") once wid 17 became valid.
- **The MLAA tasks' PUT to EA 0** (fork `7edb737e`): `cellSpursCreateTask2` signalled each task at
  creation, so each ran a batch from the still-empty control block and wrote its label to EA 0.
  No signal now. Taskset2 also gets its two lv2 event flags, without which the SPU task library
  refuses SPU-side task creation.
- **`GCM_SEMA_ACQUIRE=1`** in `main.cpp`: the RSX waits on the label as it should.

## Where it stands

No unregistered import, no SPU error, no FIFO resync. The RSX waits at `0x2FEE54` for MLAA batch 2;
the MLAA control block is still empty because RsxSync has not consumed even its first list entry
(tickets `0x000D0000`), although its policy module runs (it is kicked by the flag and by
readyCount, measured), its first report waits are released, and its instance fields read right.
The first port's notes saw `0x000D0001` at the same point (stopped on the first wait).

Ruled out: readyCount being consumed by our coordinator (left as a level, no change); the workload
not running (a temporary log shows runs of wids 14-17).

## Next

1. Why the `Wws_Job` policy module does not take RsxSync's first entry: trace one run of wid 17
   (the list line it caches at LS `0xD00`, the per-SPU lane it uses).
2. The first port gives each HLE taskset its own workload id (an SPU-side `cellSpursSendSignal`
   kicks the taskset's wid); here they all report wid 0.
