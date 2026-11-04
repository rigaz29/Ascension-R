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

### Risks

- **The job manager's protocol.** The calls must come in the same order with the same arguments.
  The replay checks it (requests in order).
- **Junk that depends on stale local store.** It comes out right by construction in the SPU job,
  and is masked elsewhere. The NOP data are known; lit and skinned padding records are to be
  checked.
- **Coverage.** Kinds and paths the captures lack (the wrap, other pieces) are declined.
- **The amount of work.** About 60 lifted glue functions, each taken the step-1 way: a model,
  then a native, then a call-by-call check.
