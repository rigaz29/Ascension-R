# Upstream sync, the WWS job modules, and the intro loader's `put`

Where the [first boots](2026-10-26-first-boot.md) stopped: the main thread waits on
`IntroLoaderThread`, which polls with `sys_timer_usleep(30)`, and the `Wws_Job` job code runs in
the SPU interpreter.

## Upstream sync

The `gowa` branch was 380 commits behind upstream, some of them from another port of this title.
Merged (fork `19532978`, no rebase, so no force push). 19 files conflicted; where both sides had
done the same thing upstream's form was kept (the x86 SPU integer helpers, the one-gate LS
diagnostics, the drain hop, the store watch), and this branch's own work stays on top: the D3D12
backend and its PC features, the FIFO segments, the idle lock-line backoff, the quadword bit-shift
SSE forms, the settings keyboard map. The merge message lists each resolution. The build, the SPU
helper tests (`test_spu_int_x86`, `test_spu_int_neon`, `test_spu_helpers`) and the Python tool
tests pass, and the boot reached the same point as before.

## The WWS job modules

`GOWA.elf` links its job modules into its own image: 31 `0xC0DEC0DE` headers, six of them modules
linked at LS `0x4000` (80 to 134 KB each). The `Wws_Job` policy module streams one from its address
into LS `0x4000` and runs it. Upstream's `lift_jobmods.py --engine-elf` lifts them and registers each
as a verified region at its address; `gowa.py lift` now runs it into `spu/jobmods/`.

Lifted, the jobs still entered the interpreter. A temporary log at the dispatch miss showed the
resident region set and verified, and the targets themselves not lifted: return points of indirect
calls and other indirect targets the function finder doesn't reach (24 in module
`3105a33a1cbe`, 4 in `d3eebe50d457`), plus the policy module's interrupt vector `0xA2C` and two
more of its entries. `SPU_INTERP_STATS` names the address the interpreter starts at, which is a
few instructions past the miss when the micro-interpreter took the first steps; the miss address
is the one to lift.

`$LBP_JOBMOD_EXTRA_FUNCS` offers an address to every module that spans it, and these modules
overlap: two other jobs' data tails became a few hundred unsupported words each. Fork `c780c481`
adds `--extra-funcs NAME=ADDR,...` for one module; `gowa.py` keeps the per-module lists. With
them the interpreter is no longer entered often enough to report (under 65,536 steps per 10 s).

## The intro loader's `put`

`IntroLoaderThread`'s loop at `0x419D1C` is an inline `cellGcmFinish`: convert the command
buffer's current pointer to an IO offset through the GCM offset table (`ioAddress[ea >> 20]`, read
directly from the table `cellGcmGetOffsetTable` handed out), and sleep 30 us until the RSX `get`
equals it.

- `GCM_DRAINDBG`: `put` was already `0xFFFFFFFF` at the first drain.
- `PPU_WWATCH=20002000`: the loader itself wrote it (`0x41A0C0`, after the flush at `0x41A400`),
  from the same inline conversion, which returns -1 for an unmapped page.
- The guest table was right for the first command buffer (`ioAddress[0x700] = 0`), so the
  pointer was elsewhere: in main memory mapped by `cellGcmMapMainMemory` (`0x75200000`) after the
  title had taken the table. The runtime copied the tables into the guest only inside
  `cellGcmGetOffsetTable`.
- With `put` at -1 the walker read past the commands into fragment program microcode at IO
  `0x14000` (`3E8101C0 C8051C9D ...`) and stopped at its first word, a "jump" to an unmapped
  offset.

Fix (fork `f68412b7`): once the tables have been handed out, every map and unmap writes the entries
it changes through to the guest copy. `test_gcm_io_mappings` checks it.

## Where it stands

The loader gets through its boot screens (`BOOT0`, `BOOT3`, `BOOT4`) and the title goes on to load
`ENGLISH.PSARC`, `MP-ENGLISH.PSARC` and the `TPPS*-ENGLISH` archives. No frame has been flipped yet.
After about 50 s a self-JUMP park at IO `0x2FFC40` stands past the 10 s grace and the runtime
resyncs past it: an SPU job that writes commands into the ring and then releases the park did not
release it.

## Next

1. The park at `0x2FFC40`: which job writes that part of the ring (an Edge geometry job in one of
   the Taskset2s, or a `Wws_Job` render job), and why it does not finish.
2. The Taskset2 workers' 4-byte PUT to EA 0 (`pc=0x37A0`), still there.
3. The 25 other `0xC0DEC0DE` headers in `GOWA.elf` (link base 0): what they are.
