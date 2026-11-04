# EDGE job host reference: analysis and design (phase 2b, step 2)

Written 2026-11-04, branch `edge`. The [output contract](2026-11-04-edge-output-contract.md)
maps what an EDGE job takes in and what it hands out. This note is the analysis for a C
implementation of the whole job, and its design. The implementation is to be:
- exact against the captured jobs;
- the reference that step 3's GPU path is checked against;
- able to run in the job's place.

Nothing here is implemented yet. The measurements came from temporary probes (not kept).

## What the analysis found

### The job's shape

The job module (`e8f44fbc6dbf`) is a small program.
- **`0xAD98`, the entry.** It saves the job's argument at `0x10B00` and runs the static
  constructors (the table `0xCB20`-`0xCC80`: `0x7030`, `0x44D0`). Then it calls main at `0x5110`.
- **Main returns to `0xAE44`.** That runs the destructors and returns `r3` = the job's own run
  time, in decrementer ticks.
- **Registers.** `r0`, `r1` and `r80`-`r127` come back as they were in all 120 gate jobs;
  `r2`-`r79` are scratch.

Main calls the job manager (the lifted module `wwspm`) at fixed points:

| call | from | `r3`-`r6` | what it does |
|---|---|---|---|
| `0x2FD8` | `0x4558`, `0x4598`, `0x5198` | (0, 1, 0, 1), (0, 2, 0, 0), (0, 1, 0, 1) in every job | job manager service (DMA waits and the like) |
| `0x17B8` | `0x4680` | (1, 2, `0x2000000`, `0x3014`) in every job | job manager service |
| `0x17C0` | `0x4A50` | (`0x1E`, 0 or an LS address, `0x17C0`, 0) | job manager service; tag `0x1E` is the one the job's own GETs use |
| `0x17E0` | `0x53A4` | (0, the main label `0x20000C80`, a ring position that grows, -1) | the job manager starts the next job's prefetch here |

After `0x17E0`, the job manager's interrupt handler carries the prefetch on, at the interrupt
checks of the job's code: the DMA list GET, then the `GETL` of the elements.

The job's own I/O, in order (a lit job that writes a program):
1. The 16-byte record of descriptor word 22, twice, to `0x10710`.
2. A `GETLLAR` of the SPU's ring line (`0x41A0`).
3. When word 28 is set:
   - a `GETLLAR` of the ring line;
   - the 48-byte block and the program object;
   - the program `PUT` into the second ring;
   - a `PUTLLUC` of the ring line.
4. The main ring allocation: a `GETLLAR` and a `PUTLLUC` of the ring line.
5. The `PUT`s, in this order:
   - the indices;
   - the vertices, in pieces of at most 16 KB;
   - the hole, fenced (`0x22`).

### Where the time goes, in the game

An in-game probe at the gate counted 65,536-job windows. It saw as many lit jobs as
position-only ones, so the lit jobs are 77% of EDGE's time there.

| per job | position only | lit |
|---|---|---|
| the job, entry to main's return | 35.8 µs | 113.4 µs |
| native stages | 8.0 | 56.7 (lighting 20.6, culling 11.6, unpack 7.9, transform 3.1, indices 2.9, point lights 2.0, ...) |
| job manager code inside the job | 10.5 | 11.7 |
| lifted glue, the body and the driver | ~15.5 | ~36 (`0xA880` 5.2, `0xC628` 1.8, `0xCC98` 1.6, `0x61B0` 1.3, ...) |
| the job's DMA helper `0xAF38` | 1.8 | 6.0 |

- The job manager's biggest part is its tag wait, `0x2370`: 4.1-4.7 µs a job in about two
  calls. It is five channel operations. The time goes to the prefetch's transfers, which the
  runtime carries out there (descriptor, list, 12 elements, about 20 KB).
- The replay runs the same jobs 30-40% faster (25 and 67-70 µs). Its split is the same but for
  the DMA. It serves its GETs a byte at a time, so the replay is not the place to time DMA.

### Outputs as a function of the inputs

45 pairs of position-only jobs had the same geometry inputs: pieces, streams, header, formats,
matrix, slot list. The pairs came from different lists, frames and SPUs.
- The index and vertex PUTs were identical, padding records included (the vertex count rounded
  up to 8).
- The holes were identical but for the methods that carry an address or the SPU:
  - the array offsets and the index address;
  - the label (`SEMAPHORE_OFFSET`) and the release value.
- The hole's NOP data words differed in 31 of the 45 pairs. They are whatever the local store
  held.
- The captures have no lit pairs, so the padding records of lit and skinned jobs are not
  checked yet.

### State a job reads that is not its input

These are lines a job reads before writing them, outside its input set, over 280 jobs. Lines with
one value in every job are the loaded images' tables (the job module's `0xF7B0`-`0xF94F`,
`0x10400`-`0x1042F`; the job manager's `0x1360`-`0x138F`). The rest were left by earlier work:
- `0x1C0`: the SPU number, word 2 (the kernel's; read by `0xF770`);
- `0x13F0`: a job manager word, which the body reads;
- `0x106B0`-`0x1070F` and `0x10918`/`0x1091C`: the previous job's buffer set (`0x9990`, `0xBFB8`);
- `0x3F7B0`-`0x3F7DF`: the second ring's state (its label `0x20000CE0 + 16 spu`, its current
  pointer), read by the body and `0x67C0`;
- `c + 0x290` (`0x10A30`): `0x88B0`'s table of the formats seen, which gives the format numbers;
- context lines `0xBFB8` reads before writing them. Whether any old byte survives into the new
  context is still to check.
- In 2-14 of the 280 jobs, scratch in the buffer sets: stale bytes read beside output arrays,
  and skinning's padding lanes.

### What the strict replay compares

The strict replay checks:
- every request in order (address and size);
- PUT and mailbox bytes;
- at the end, the whole local store and all registers.

A C job with the same outputs would still differ in scratch, in the stack and in `r2`-`r79`,
which nothing reads. So it needs a second compare mode.

## Design

### One core, three users

A core, `edge_job`, holds the job's logic in C. It works on a local-store image laid out as the
job lays it out:
- the input set where the DMA list put it;
- the context at `0x107A0`;
- the pools where the job's allocator puts them;
- the hole built in the job's own buffer.

It calls the existing natives unchanged, since they take a context and the local store. It goes
outside through three small interfaces:
- **in**: the job's own fetches (the 16-byte record, the program object, the 48-byte block);
- **out**: PUTs (with the fence) and the ring line (get and put of its 128 bytes);
- **jm**: the job manager calls, with their arguments, at the job's points.

It has three users:
1. **In the SPU job**, a hook at `0xAD98` (`GOWA_EDGE_JOB=1`).
   - The image is the SPU's own local store.
   - The interfaces go through the runtime's MFC, and the job manager calls go to the lifted
     job manager, drained to the same link.
   - So the job manager sees what it sees now, and the junk bytes come out the same (they are
     the same stale bytes).
2. **The replay**: the served context and records, with the compare mode below. It is also the
   development harness, since a capture is a whole test case.
3. **Later, off the SPU threads, or as step 3's oracle**: an image built from main memory (the
   input set laid out by its DMA list), with the junk masked.

### Exactness

- **Must match the lifted job:**
  - the requests in order;
  - the PUT bytes (junk included, in the SPU job);
  - the ring line's words;
  - the state regions above;
  - `r0`, `r1`, `r3`, `r80`-`r127`.
- **Need not match:** the scratch, the stack below the entry frame, and `r2`-`r79`.
- **A replay compare mode for that.** It checks requests, PUTs, mailbox and atomics as now, and
  at the end only the state regions and those registers.
- **While the body is still lifted**, each ported function is checked call by call
  (`GOWA_EDGE_VERIFY`, whole local store but the stack), as in step 1.

### Order of work

The core is built bottom-up:
- each lifted glue function becomes a native first, hooked and verified call by call while the
  lifted body still calls it;
- the body and the driver come last, which makes the job one C function.

1. **The harness.** The replay compare mode, and a hook that declines every job (the lifted job
   runs, 0 differences).
2. **Position only, unskinned** (inputs `0x764D10` / `0x75EB40`, output `0x765B70`, methods
   `0x7661C0`):
   - the body and set-up;
   - the context (`0x88B0`, `0xBFB8`);
   - the driver;
   - the rings (`0x41A0`, `0xF770`, `0x4110`);
   - indices;
   - `0x67C0`'s record;
   - `0x61B0` when a program comes;
   - `0xA880` with `0x7661C0`;
   - the output (`0x9080` and its helpers: the ring allocation, the PUTs, the hole).
   These jobs are half the jobs and 23% of EDGE's time at the gate.
3. **Skinned position-only** (`0xE550`, `0xBBC8`, `0xBB38`, and the palette's and weights'
   formats).
4. **Lit** (inputs `0x75BD10` / `0x758590`, output `0x755B80`, methods `0x75FE80`):
   - culling `0xD618`;
   - the attributes (`0x5C88`, `0xBEE0`, `0xBE88`, `0xBFA8`);
   - `0x9A60`'s light selection;
   - lighting `0x73E0` and `0x6AC8`;
   - `0x67C0`'s 176-byte block;
   - `0x61B0`'s program patch.
5. **Declined, the lifted job runs:**
   - the rare kinds (`0x762470`, `0x756DE0` and their pieces);
   - a ring wrap. It is checked before any side effect: the job reads its ring line first, and
     meanwhile only the GPU's label moves, which only frees space.
6. **On by default** after:
   - 0 differences in the new mode over all captures (the gate's, the consecutive ones, and new
     ones from the menu, the cinematic and the fight);
   - the per-function checks in the game;
   - A/B benches.

### What to expect

- **In the SPU job.**
  - The glue, the body and the driver (~15.5 of 35.8 µs position-only, ~36 of 113 µs lit)
    become a few µs; so does `0xAF38`'s DMA.
  - EDGE's SPU time should fall by about 30% (an estimate). By plan 2.22's experiment
    (EDGE -21% gave +14%), the gate should gain 10-15%.
- **Not touched by it.**
  - The job manager's 10-12 µs a job. That is mostly the prefetch's DMA, carried out inside its
    tag waits; every `wwspm` job pays it, and it is the runtime's MFC path to look at, apart
    from EDGE.
  - The natives' 8 and 57 µs, which step 3 takes off the CPU.
- **For step 3**, the core is the reference. The GPU's float results will not be bit-exact
  against it, so that check needs a tolerance, to be decided then.

### Progress

**1. The harness (done).**
- The replay's state compare mode (fork):
  - `SPU_JOB_REPLAY_CMP=state` with `SPU_JOB_REPLAY_STATE=lo-hi,...` compares, at a run's end,
    only those local-store ranges and `r0`, `r1`, `r3`, `r80`-`r127`;
  - requests, PUTs and mailbox are compared as always.
  - Checked with a poked byte:
    - strict and state both report it when it lies in a range;
    - state does not when it lies outside.
- The job's hook, `gowa_edge_job_ad98` (`native/spu_edge_job.c`, `GOWA_EDGE_JOB=1`, off by
  default).
  - It reads the job's input set and descriptor (`ej_read`): the own buffer set, the 22 slots,
    the pieces, skinning, the segment header's counts. It counts the kinds, then declines.
  - Replay of the 120 gate jobs with it on: 0 differences, every input set read. Kinds:
    - 62 position-only: 57 with `0x764D10`, 5 with `0x75EB40`;
    - 16 skinned position-only (`0x75EB40`);
    - 38 lit: 33 with `0x75BD10`, 5 with `0x758590`;
    - 3 skinned lit (`0x758590`);
    - 1 of `0x762470`.

**2. The output stage, `0x9080` (done; `native/spu_edge_out.c`, `GOWA_EDGE_NATIVE` bit 131072,
on by default).** It is the most valuable common block of glue: 4.4 µs of a position-only job
and ~7 µs of a lit job, in the game.
- **What it does.** Mapped from the lifted code (`0x9090` and its helpers):
  - the ring allocation (`0xC628`), round128(round16(vertexes x stride) + 2 x round8(indexes))
    bytes of the SPU's main ring:
    - the SPU's ring line (the descriptor's ring EA + 128 + 128 x the SPU) by `GETLLAR` into LS
      `0x10C00`;
    - the room check (`0xB700`): with the GPU's position known (line word 6) and no refresh
      asked (`c + 0x190` byte 2, bit 7), the block must not pass the GPU's position; a block
      that would run past the ring's end starts again at its start;
    - the line's current pointer (word 3) moves past the block, and the line goes back by
      `PUTLLUC`;
    - the local block gets the block's EA, its RSX offset (line word 1 plus the block's offset),
      line words 4 and 5, and the block's end;
  - the index PUT and the vertex PUT(s), through `0xAF38`'s pieces (16 KB, then the 16-byte
    multiple, then 8/4/2/1), each between a `SPU_RdMachStat` read and interrupts back on;
  - the vertex packing through the piece at `c + 0x8C`;
  - the hole, built in the local store at the hole EA's 128-byte offset, a word at a time (so
    the NOP data keep the stale bytes):
    - the method piece, and the program binding when there is one;
    - then per pass (w34 byte 1: the count in the high nibble, a mask in the low one):
      - the viewport (`0x0A00`, `0x0394`, `0x0A20` x8, with SPU float ops);
      - the scissor (`0x08C0`) and `0x1EFC` x5;
      - `INVALIDATE_VERTEX_FILE` and `INDEX_ARRAY_ADDRESS`;
      - `BEGIN_END` 5 and `DRAW_INDEX_ARRAY`: up to the index data's first 128-byte boundary,
        then 2047 x 256 indexes under one non-increment header, then the rest;
      - `BEGIN_END` 0;
  - the hole's end (`0xCC98`):
    - per local entry with a label, a 16-byte block from the template at LS `0xF930`:
      `SET_SEMAPHORE_OFFSET` (`0x1D6C`) with the label's offset, then
      `TEXTURE_READ_SEMAPHORE_RELEASE` (`0x1D74`) with the ring's new current pointer, the
      value the room check later reads back as the GPU's position;
    - a method whose arguments would cross a 128-byte line is cut at the line, and the rest gets
      a header of its own: the rest of the hole moves one word on, line by line from the last;
    - a NOP (`0x40000100 | (n - 1) << 18`) over what is left of the reserved size (16 x `c`
      byte 1);
    - the fenced PUT (`PUTF`, tag `c + 0x150`), between a `SPU_RdMachStat` read and interrupts
      back on.
- **What is native.** All of it but two parts, which it calls as the lifted code calls them
  (same link, then the drain):
  - the method piece through `0x5D68`;
  - the program binding `0x84C0`, when there is one.

  Handed to the lifted code, and never seen in the game:
  - the room check's label read and wait (the lifted `0xB700`), when the GPU's position is not
    known, a refresh is asked, or there is no room yet;
  - `0xB9D0`, when the ring line's start and end are equal or there is still no room;
  - an allocation at an EA of its own (bit 1 of the descriptor's ring word, the lifted `0xC628`);
  - the hole set-up's 4-byte label read (the lifted `0xC9A8`).

  A ring wrap is not one of them: it is the room check's own path, done as the lifted code does
  it (seen once in the captures).
- **How it is written.** Its arithmetic is the lifted code's, op for op, with the same SPU helpers
  on locals. It drops the register file, the interrupt checks at every label, and the
  dispatcher's round trip at every interrupt enable and disable.
- **When it declines.** With an interrupt pending, or a stride callback at `c + 0x88`.
- **Checked, first part** (`0xC628` and `0xCC98` still lifted):
  - replay, state mode: the 120 gate jobs and the 160 consecutive ones, 0 differences;
  - strict mode: the PUT and atomic bytes the same; only 109 bytes of callee frames and scratch
    registers differ;
  - in the game: 2,686,976 calls, none declined; 180 jobs captured with it on, replayed with it
    off: 0 differences.
- **Checked, whole** (`0xC628` and `0xCC98` native too):
  - replay, state mode (`0-3E000,3F780-40000`, all but the frames below the job's main frame):
    640 jobs, 0 differences (the gate's 120, 160 consecutive, and two sets of 180 from the
    game);
  - the paths the first 460 of them take: a label block in every draw, a method cut at a line
    502 times, the NOP in every job, the wrap once. Never: a PUT under 16 bytes, `0xB9D0`, the
    lifted `0xB700`;
  - negative tests: the RSX offset + 16, or the NOP's header + 4, made every draw job differ;
  - strict mode: no PUT, atomic or mailbox byte differs; up to 170 bytes of callee frames
    (`0x3F3B0`-`0x3F5C3`) and the scratch registers do;
  - in the game: 2,752,514 calls (64,338 of them empty draws), none declined, none handed to
    the lifted code, no halt. 180 jobs captured with it on, replayed with it off: 0 differences.
- **Speed.**
  - Replay, the gate's jobs, best of 10, ABBA, off -> on: w14 63.2 -> 61.6 µs, w15
    22.2 -> 20.3, w16 23.6 -> 21.1 (one run on at 25.1).
  - Of that, `0xC628` and `0xCC98` give little: lifted against native in one build, three runs
    each, w15 20.8 -> 20.4 µs, w16 21.9 -> 21.6. Their lifted overhead was one interrupt
    disable and enable each; their channel operations (the ring line's atomics, the hole's
    PUT) are the same either way.
  - Gate, ABBA, 2 runs each, `GOWA_EDGE_NATIVE=131071` against the default: 18.83 -> 18.86
    flips/s (runs 18.12 / 19.04 against 19.50 / 18.41). That is no difference above the noise,
    which is about ±0.5 between runs of one variant; two runs each cannot resolve what ~2 µs a
    job is worth at the gate.
  - The first part, earlier: 18.19 -> 18.70 flips/s (+2.8%). A first A/B had shown -3%: the
    stats test called `getenv` on every call, and the CRT's environment lock, taken from six
    SPU threads, cost more than the native saved. The flag is read once now, and the counts
    run only with the stats on.
- **On by default**, as `GOWA_EDGE_NATIVE` bit 131072 (`GOWA_EDGE_NATIVE=131071` leaves out only
  it). `gowa.py replay` now compares in state mode by default (`--cmp strict` for the whole
  local store and every register): with this native on, the strict check reports every EDGE
  job for its callee frames.
- **Next.** The rest of the glue, in the order above.

Natives inside the job have one more rule than step 1's. Their channel operations must be the
lifted code's, in its order: the glue reads `SPU_RdMachStat` around its critical sections, and
the replay serves every read from the record. And they must not run with an interrupt pending,
since the job manager's handler takes its turn at the lifted code's checks.

**3. The lifted glue's largest pieces, after the runtime's fixes.** Measured in the game with
the runtime's fixes in ([hot paths](2026-11-04-spu-runtime-hot-paths.md)), the EDGE module's
lifted code was 13.3% of the EDGE workers' samples, spread over 284 functions. The order taken,
the largest and simplest first:

0. the interrupt continuations;
1. the lighting stage's set-up (`0x73E0` and `0x5D90`);
2. `0xBBC8` (the skinned lit jobs' vector normalization, 1.3%);
3. the skinning driver `0xE550` (1.6%);
4. the context `0x88B0` (with `0xBFB8`, `0x5CF8`, `0xB600`; 1.6%).

None of them has a channel operation, so `GOWA_EDGE_VERIFY` can check each call (the whole
local store but the stack, and the registers).

**3.0 The interrupt continuations (done).**
- **The miss.** The glue turns interrupts on and off with a branch to the next instruction, and
  then an indirect jump to the link plus 8:
  - `spu_link`, `ai r2, 8`, `int_enable`, `bi r2`;
  - the jump's target is a lifted entry, or it misses.
- **Which ones missed.** Six such targets in the EDGE module were not entries: `0x4100`,
  `0x62F4`, `0x65EC`, `0x66B0`, `0x6774`, `0x6A54`.
  - Each jump to them took the dispatcher's full path, then the micro-step and the interpreter
    up to the next lifted entry.
  - In the game, `0x62F4`, `0x65EC`, `0x6774` and `0x6A54` each missed about 124,000 times in
    20 s.
- **The fix.** They are extra entries now (`scripts/jobmods_extra.json`), and the module was
  lifted again.
- **The effect, in the game.**
  - All four are gone from the full-path report.
  - Full-path branches went from 4.10 to 3.24 per EDGE job, and from 4.27 M to 3.81 M in all
    over 20 s.
  - The micro-step's share of the EDGE workers' samples went from 0.67% to 0.43%.

**3.1 The lighting stage's set-up (done; `native/spu_edge_light.c`, `GOWA_EDGE_NATIVE` bit
262144, on by default).** Three loop natives, all integer work: 16-byte loads and stores at
computed addresses, done in the lifted order with the same SPU helpers.
- **`0x5DA8`**, the loop of the buffer clear `0x5D90`, stores zero at 8 quadwords a pass
  (`gowa_edge_lzero_5da8`). About 49 passes (6.2 KB) a lit job.
- **`0x75D8`** stores one quadword at r81 + 16 x each 16-bit index of a list
  (`gowa_edge_lscat_75d8`).
  - It is software pipelined: a pass loads the list's next quadword before its eight stores.
  - About 64 passes (514 indexes) a lit job.
- **`0x7670`** is a running sum over four count tables, read side by side
  (`gowa_edge_lsum_7670`).
  - Before each addition, the counter for that table is stored at r81 + the sum: a counting
    sort's placement.
  - About 98 passes a lit job.
- **The loop hooks.** All three are taken the step-1 way:
  - all passes but the last run natively;
  - the loop-carried registers are written back as whole quadwords;
  - the lifted loop makes the last pass, so every scratch register ends as the lifted code
    leaves it;
  - a pending interrupt stops the native before its next pass, and the lifted code takes it at
    the loop's label, as it would have.

  The clear was a whole-function hook at first. As one, it could not hand an interrupt back in
  mid-loop, so it became a loop hook; the three instructions before its loop stay lifted.
- **Where the hooks go.** A loop head is already a lifted function (the re-entry after an
  interrupt taken at its label), so a loop hook needs no extra entry.
  - The hooks sit at the loop labels inside the merged `0x5D90` and `0x73E0`, and at the loop
    functions' heads.
  - An extra entry at a loop head is harmful: it splits the merged function there, so the code
    before the loop reaches it by `SPU_DIRECT`.
- **Checked.**
  - `GOWA_EDGE_VERIFY=1` runs `0x73E0` with its natives against the fully lifted stage (the
    reference run has them all off): 332 calls in five corpora, 0 differences.
  - Replay, state mode: 1180 jobs (the gate's 120, the 160 consecutive ones, and five sets of
    180 from the game), 0 differences.
  - Strict mode:
    - as recorded (`out2`, `rt1`, `l1`, `l2`), 0 differences;
    - on the gate's and the consecutive jobs with only the output stage off (whose callee
      frames differ by design), 0 differences. The new natives leave the whole local store and
      every register as the lifted code does.
  - In the game:
    - 180 jobs captured with the first version on (`jobcap-l1`), and 180 with the last
      (`jobcap-l2`); each set replayed with 0 differences, in state and in strict mode;
    - 1.31 M calls of `0x7670` with no halt, and no stop for an interrupt (none in replay
      either: there the events change only with the job's own channel operations).
- **Speed.**
  - Replay, the gate's 120 jobs, best of 10, one build, two runs a side: 4038 -> 3960 µs in all
    (-1.9%). The 29 jobs that changed are each 2-3 µs faster (8 µs for the first); none are
    slower.
  - The sampler and the gate measured the first version (the clear as a whole-function hook,
    no interrupt test). Its replay saving was the same, -1.8%.
  - Sampler in the game, the EDGE workers' samples:
    - `0x73E0` went from 2.67% to 1.40%, and `0x5D90` from 0.30% to 0;
    - the three natives take 0.87%;
    - the lifted EDGE module went from 13.33% to 11.37%;
    - the micro-step went from 0.67% to 0.43%, and the interpreter from 0.13% to 0.10%.
  - Gate, ABBA, three runs a side, the module before the re-lift against steps 0 and 1:
    19.02 -> 19.19 flips/s (+0.9%; runs 18.59 / 19.42 / 19.09 against 19.12 / 19.36 / 19.14).
    - That is within the noise: runs of one variant differ by about ±0.4.
    - The first run (the old module) started slow, 17.0-17.3 in its first windows.
    - The sampler's ~1% of the EDGE workers' time is about what to expect here.
- **Next.** `0xBBC8`.

**3.2 The vector normalization `0xBBC8` (done; `native/spu_edge_vnorm.c`, `GOWA_EDGE_NATIVE`
bit 524288, on by default).**
- **What it does.** It scales each quadword of an array in place (r3 the array, r4 its count,
  rounded up to 8) by an estimate of its length's reciprocal square root:
  - L = fma(z, z, fma(y, y, fm(x, x))), in the SPU's float forms;
  - s = fi(L, frsqest(L));
  - v = fm(v, (s, s, s, 1.0)), so w stays.
- **Who calls it.** Every skinned lit job, twice, and no other kind of job (the job hook's
  kinds, one replayed job at a time: 3 of 3 such jobs in the gate's corpus, 18 of 18 in
  `jobcap-l3`).
  - At `0x5A68` and `0x5A78` it normalizes the two arrays (r80, r82) the transform `0xF538` has
    just written in place, when r84 is not zero.
  - Its other call sites are `0x5AC4` and `0xF46C`-`0xF4AC` (up to four arrays).

  The gate's corpus has only 3 skinned lit jobs (6 calls). The later captures from the game
  have 13-18 in each set of 180 jobs (26-36 calls).
- **The loop `0xBD54`.** Eight quadwords a pass, software pipelined three deep:
  - a pass stores group p, with the scales in r16 | r18;
  - it finishes group p + 1's lengths (z in r9 | r7, the partial sums in r49 | r10) and scales;
  - it starts group p + 2's sums, and loads group p + 3;
  - the raw quadwords move r33-r40 -> r41-r48 -> r24-r32.

  The patterns it shuffles with (LS `0xFE60`, `0xFE70`, `0xFF30`, `0xFF60`) transpose four
  quadwords into x, y and z lanes.
- **The native.** A loop hook, the step-1 way, on 256-bit registers:
  - a group's eight quadwords, two to a register; x, y and z by unpacks and shuffles (bit moves
    only);
  - the float ops in `spu_edge_simd.h`'s forms: native under the SPU code's MXCSR with the +0
    rule, and the exact forms where a result leaves the single range (a zero vector's
    estimate does: exponent 255);
  - frsqest and fi as the runtime's bit-field rules, on eight lanes;
  - it runs only with the module's patterns, select mask (r20) and 1.0 (r23), and under the SPU
    code's MXCSR. A pending interrupt stops it before a pass.
- **Checked.**
  - `GOWA_EDGE_VERIFY=1` checks every call of `0xBBC8` (a hook at its entry; the local store
    and all 128 registers): 188 calls in the eight corpora, 0 differences. In the first seven,
    168 half-ops took the exact forms, for zero vectors.
  - `GOWA_EDGE_EXACT=1`, the exact forms for every op: 60 calls, 0 differences.
  - Replay: 1360 jobs in state mode, 0 differences. Strict mode as recorded (`out2`, `rt1`,
    `l1`, `l2`, `l3`), and with only the output stage off: 0 differences.
  - In the game:
    - `GOWA_EDGE_VERIFY=1`: 139,265 calls checked, none declined, 0 differences;
    - with it on: 589,824 calls (25.9 M passes), no halt, none declined, no stop for an
      interrupt; about one half-op in 50 passes took the exact forms;
    - 180 jobs captured with it on (`jobcap-l3`), replayed with 0 differences in state and in
      strict mode.
- **Speed.**
  - Replay, best of 10, one build, two runs a side, ABBA:
    - the 180 jobs of `l2`: 6153 -> 6127 µs (-0.4%);
    - the 180 jobs of `out1`: 6856 -> 6811 µs (-0.7%);
    - the jobs that call it are each 2-3 µs faster.
  - Sampler in the game, the EDGE workers' samples:
    - `0xBBC8`'s lifted code went from 1.23% to 0.20% (its prologue and last pass);
    - the native takes 0.57%, so `0xBBC8` is 0.77% in all;
    - the native's loop is about 280 instructions a pass: the byte swaps of 16 quadwords,
      frsqest's table as eight permutes, and a range test after each float op.
  - No gate A/B for this step alone: about 1% of the EDGE workers' time is under the gate's
    noise (about ±0.4 flips/s between runs). The gate is measured again after the next steps
    together.
- **Next.** The skinning driver `0xE550`.

**3.3 The skinning driver `0xE550`: its two lifted loops (done; `native/spu_edge_skinmat.c`,
`GOWA_EDGE_NATIVE` bit 1048576, on by default).**
- **Its shape.** `0xE550` runs once in every skinned job, and is one function from `0xE550` to
  `0xF538` (about 1040 SPU instructions):
  - the matrix layout, `0xE804` (the matrix format 2; every skinned job in the captures);
  - set-up (`0xE858`: the stream buffers from `0xB638`-`0xB660`);
  - per batch of vertexes: the position loop `0xEC08` (native since phase 2a), then a loop for
    the other attribute streams. Only `0xF104`, two streams, runs in the captures; `0xEE28`,
    `0xF034` and `0xF24C` never do;
  - the renormalization `0xBBC8` and `0xB600` at the end.

  Loop counts in replay, from a counting build:

  | corpus | `0xE550` calls | `0xE804` passes | `0xF104` passes |
  |---|---|---|---|
  | gate (120 jobs) | 19 | 768 | 266 |
  | consecutive (160) | 84 | 2736 | 0 |
  | `l3` (180) | 36 | 1191 | 1468 |
- **`0xE804`** lays each bone's matrix out in place: four quadwords q0-q3 become three,
  (q0.x q1.y q2.z q3.x) (q1.x q2.y q0.z q3.y) (q2.x q0.y q1.z q3.z), the diagonals a vector is
  multiplied with. Only word moves; it loads matrix k + 1 before it stores matrix k.
- **`0xF104`** transforms two attribute streams (the normals and tangents of the skinned lit
  jobs) by the per-vertex matrixes in that layout, four vertexes a pass, in place:
  out = fma(zxyw(v), D2, fma(yzxw(v), D1, fm(v, D0))). One stream and the matrixes are loaded a
  pass ahead.
- **The natives.** Loop hooks, the step-1 way.
  - `0xE804`: word moves on 128-bit registers.
  - `0xF104`, first version: its pipeline (twelve matrix quadwords, four vertexes and their two
    shuffles) in arrays, and `spu_edge_simd.h`'s forms with a range test after each op. The
    loop was 318 instructions a pass, 111 of them stack traffic, about what the lifted loop
    costs. In the game `0xE550`'s lifted code went from 1.63% to 0.67% of the EDGE workers'
    samples, but the natives took 0.90% (`0xF104` 0.80%): 0.06% net.
  - `0xF104`, as kept: the pipeline in eight 256-bit registers (six matrix pairs, two vertex
    pairs; the shuffled copies are made again from the vertexes), two quadwords to a load or a
    store with one byte swap, the twelve float ops of a pass native, and one range test of all
    their results. A lane at or past FLT_MAX (where an overflow toward zero lands) or NaN (from
    an operand of exponent 255) has the pass done again in the exact forms, op by op, from the
    same inputs; the +0 rule is applied to what is stored.
- **Checked** (the kept version).
  - `GOWA_EDGE_VERIFY=1` checks every call of `0xE550` (a hook at its entry): the whole driver
    with the two loops native against it with them lifted. Neither the driver nor its callees
    use a channel, so all 128 registers and the whole local store, stack included, are
    compared. 378 calls in the ten corpora, 0 differences; one pass was redone exact.
  - `GOWA_EDGE_EXACT=1`, the exact forms for every pass: 79 calls, 0 differences.
  - Replay: 1720 jobs in state mode, 0 differences. Strict mode as recorded, and with only the
    output stage off: 0 differences.
  - In the game:
    - `GOWA_EDGE_VERIFY=1`: 90,114 calls of `0xE550` checked, none declined, 0 differences;
    - with the natives on: 543,648 calls of `0xE804` and 983,040 of `0xF104` (25.9 M passes,
      4272 of them redone exact), no halt, none declined, no stop for an interrupt;
    - 180 jobs captured with them on (`jobcap-l5`, with step 4's too), replayed with 0
      differences in state and in strict mode.
- **Speed.**
  - Replay, best of 10, ABBA, natives off -> on: the 180 jobs of `l3` 6825 -> 6776 µs (-0.7%),
    of `l4` 6466 -> 6413 µs (-0.8%); 14-15 jobs faster each, none slower. The first version
    gave -0.1%.
  - Sampler in the game, the EDGE workers' samples: `0xE550` and its natives went from 1.63% to
    1.07% (0.57% lifted, 0.50% native, of that 0.40% `0xF104`). The first version had left 1.57%.

**3.4 The context builder `0x88B0`: its two table loops (done; `native/spu_edge_ctx.c`,
`GOWA_EDGE_NATIVE` bit 2097152, on by default).**
- **Where its time goes.** A sampler run of a `--spu-lines` build (a third tree, `build-ln`;
  the same code with line tables), each sample put on the lifted function and the nearest label
  above its line:
  - `0x8930` (the builder's body) 1.22% of the EDGE workers' samples: `0x8B90` 0.49, `0x89F0`
    0.20, `0x8DA0` 0.13, `0x8988` 0.09, `0x8930` 0.07, `0x8B28` 0.07, `0x8EB8` 0.07, the rest
    under 0.05 each;
  - `0xBFB8` 0.55%, spread over its blocks (none above 0.12), and `0x5CF8` and `0xB600` nothing.
  The two loops over the descriptor's tables (`0x8988`-`0x8B08` and `0x8B90`-`0x8D34`) are
  about 0.8%.
- **The loops.** A[i] is the byte at r33 + i; 0xFF marks an entry to leave out.
  - Loop A (`0x8988`, `0x89F0`, `0x8B08`; entry i in r28, its 8-byte entry at r27): an entry
    with byte 1 == 8 and byte 3 == 1 takes the long path at `0x8EB8`; any other has the 8 bytes
    at r30 + r31 + 8 copied to r26 + 8, and r26 and r32 move on. The count is r29's low byte.
  - Loop B (`0x8B90`, `0x8D34`; entry i in r29): the 8 bytes at r27 + 8 are copied to
    r28 + 8, then r32's low byte goes to r28 + 8 and A[i] to r28 + 13. Per word, unless the byte
    at r27 + 11 + 4w (in its quadword) is 27-30 while r36's word is 0, r28 moves on by 8, r34 by
    1 and r32 by the byte at r27 + 12 + 4w. The count is the first byte of r35's quadword,
    loaded again after each copy.
  - The SPU code moves each byte as a quadword load, a rotate that brings it to byte 3
    (address + 13), a shuffle with a `cbd` control and a quadword store. The native moves one
    byte, at the same address, in the same order.
- **A lesson about loop hooks.** The first version ran every pass but the last, as before. The
  call-by-call check failed in every call, on r31 only (the local store was the same, and so
  was every replayed job): when the last pass leaves an entry out, the copy's scratch registers
  keep the values of the last pass that copied, and the native had made that pass. A pass sets
  only the scratch registers of its own path. So the natives now work out the passes left
  first, and the last pass that copies, and every pass after it, is the lifted loop's. The
  passes left rest on bytes the natives' stores must not reach (the A bytes still to be read,
  r35's quadword); a copy that would reach them stops the native. The loops of steps 1-3 have
  no such paths.
- **Checked.**
  - `GOWA_EDGE_VERIFY=1` checks every call of `0x88B0` (a hook at its entry): the builder with
    the loops native against it with them lifted. Neither it nor its callees (`0x5CF8`,
    `0xBFB8`, `0xB600`) use a channel; all 128 registers and the whole local store are
    compared. 1719 calls in the ten corpora, 0 differences.
  - Replay: 1720 jobs in state mode, 0 differences; strict as recorded, and with only the output
    stage off, 0 differences.
  - In the game: `GOWA_EDGE_VERIFY=1` checked 413,696 calls, none declined, 0 differences. 180
    jobs captured with the loops native (`jobcap-l5`) replay with 0 differences, in state and in
    strict mode.
- **Speed.**
  - Replay, best of 10, ABBA, natives off -> on: 0.05 µs a job on the gate's jobs and 0.02 µs on
    `l4`, within the noise. A replayed job runs ten times over the same local store, so its
    tables are in the cache.
  - Sampler in the game: `0x8930` and the natives went from 1.20% to 1.00% (0.67% lifted, 0.33%
    native). The natives make only byte moves, yet take a third of what they replaced: in the
    game these loops cost mostly the first touches of the job's tables, which a native makes
    too.
- **Steps 2-4 at the gate.** One build, ABBA, three runs a side, `GOWA_EDGE_NATIVE=2143813631`
  (bits 524288, 1048576 and 2097152 off) against the default: 18.84 -> 19.19 flips/s (+1.9%;
  runs 18.04 / 18.94 / 19.09 against 19.00 / 19.18 / 19.59).
  - The natives' side was ahead in all three pairs, by 0.24-0.96.
  - The first run (natives off) started slow, as a session's first run tends to, so the gain
    is likely nearer 1%: about what the sampler's 1.2% of the EDGE workers' time predicts.
- **Not done.** `0xBFB8` (0.55%): straight-line field packing (`shufb` with `cwd`, `chd` and
  `cbd` controls) spread over its blocks; a native would be a transliteration of the whole
  function.

### Risks

- **The job manager's protocol.** The calls must come in the same order with the same arguments.
  The replay checks it (requests in order).
- **Junk that depends on stale local store.** It comes out right by construction in the SPU job,
  and is masked elsewhere. The NOP data are known; lit and skinned padding records are to be
  checked.
- **Coverage.** Kinds and paths the captures lack (the wrap, other pieces) are declined.
- **The amount of work.** About 60 lifted glue functions, each taken the step-1 way: a model,
  then a native, then a call-by-call check.
