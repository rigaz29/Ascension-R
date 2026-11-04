# EDGE's output contract (phase 2b, step 2)

Written 2026-11-04, branch `edge`. Step 1 made every EDGE piece and the index decompression
native and mapped their formats ([edge stages](2026-11-03-edge-stages.md)). Step 2 maps what
a job hands out, and what it takes in, so that something other than the SPU job can produce
the same result.

Source: the 120 gate jobs in `logs/jobcap-gate` (40 from each of w14, w15, w16), and for the
input side also 160 consecutive jobs in `logs/jobcap-seq`. Scratchpad scripts parse the capture
format (`SPUJCAP2`: a 56-byte header, the context, then GET / PUT / atomic / channel events
with their data, then the registers and the local store at the end).

## The jobs at the gate

| list | kind | jobs | indices | out (bytes) | in (bytes, + code) |
|---|---|---|---|---|---|
| w14 | lit: 7 attributes, stride 54 | 39 | ~500-530 | ~23,000 | ~9,000-11,000 + ~5,000 |
| w14 | position only | 1 | 1467 | 9,712 | 11,328 + 4,000 |
| w15 | position only, stride 12 | 39 | ~1,370 | ~7,500 | ~8,300 + 1,400 |
| w16 | position only, stride 12 | 38 | ~1,240 | ~6,800 | ~9,400 + 1,400 |
| w15, w16 | lit (one with 3 attributes, stride 24) | 3 | 363-2850 | 11,000-33,000 | |

24 of the 120 holes bind a fragment program. 22 of those jobs wrote the program to the second ring
themselves, position-only ones among them (depth passes with a patched program, likely).

## What a job writes

Every job, in this order:
1. **A ring allocation.**
   - GETLLAR / PUTLLUC on its SPU's line, `0x73254300 + 0x80 (spu + 1)`. The game sets up
     lines for SPUs 0-3 only.
   - The line's words: main ring start (EA), its RSX offset, main ring end (2.75 MB), the
     main ring's current pointer, 1, the main label (`0x20000C80 + 16 spu`), and a position the
     allocator compares against (unchanged in the 120 jobs). Then for a second ring: its label (`0x20000CE0 + 16 spu`), start, size
     (`0x60000`), limit, and current pointer.
   - The job moves the current pointer by its data, rounded to 128 bytes.
   - At a wrap (`0xB700` -> `0xB9BC`), it polls the label with 4-byte GETs until the GPU has
     released the space. The polling never happened in the 120 jobs.
2. **PUTs into the main ring:**
   - the indices: u16 triangle lists, from the index decompression, rounded to 16 bytes;
   - the vertices: 12-byte positions (`0x765B70`), or the lit 54-byte layout (`0x755B80`).
3. **For 22 jobs, a second-ring allocation and a fragment program.**
   - The program is copied from an object in main memory, without its 128-byte header.
   - Two 16-byte constant blocks are patched in, at offsets the header gives. The values come
     from a small per-draw block (48 bytes; for example a light vector and 1280/720).
4. **The hole.**
   - A fenced PUT (`cmd 0x22`) of 224-432 bytes of RSX methods into the command buffer, at
     the address the job was given.
   - The rest of the hole is NOPs, whose words are whatever the job's buffer held.

## The hole

The same content in every job. Only the grouping into method headers differs (34 shapes in 120
jobs); the hole is put together a quadword at a time.
- `VERTEX_DATA_ARRAY_FORMAT` / `OFFSET` for each attribute:
  - position only: slot 0 = `0xC32` (3 floats, stride 12);
  - lit, stride 54: slot 0 (position, F32x3, +0), slot 4 (I16N x4, +12), slot 5 (X11Y11Z10,
    +20), slot 7 (F32x3, +24), slots 8, 14, 15 (F16x3, +36, +42, +48).
  - The offsets are main-ring addresses, with bit 31 set.
- Holes that bind a fragment program (24) also have:
  - `SEMAPHORE_OFFSET` = the second label, and `TEXTURE_READ_SEMAPHORE_RELEASE` = the start of
    this job's second-ring allocation;
  - `SHADER_PROGRAM` = the patched program (location 2) and `SHADER_CONTROL`;
  - ten `0x0B40` words (zeros).
- The viewport block: `0x0A00` (1280 x 720), `0x0394` (0, 1), the viewport offset and scale
  (640, 360, 0.5, 0 / 640, -360, 0.5, 0), the scissor (1280 x 720), and `0x1EFC` x5 (zeros).
  The values are in a 272-byte input block (116 of the 120 jobs; the other 4 not looked at).
- `INVALIDATE_VERTEX_FILE`.
- `INDEX_ARRAY_ADDRESS` = the indices in the main ring, `0x11` (main memory, u16).
- `BEGIN_END` 5 (triangles); `DRAW_INDEX_ARRAY` in 256-index pieces; `BEGIN_END` 0.
- `SEMAPHORE_OFFSET` = the main label, then `TEXTURE_READ_SEMAPHORE_RELEASE` = the main ring's
  new current pointer.
  - This is how the GPU hands the ring back: once it has read past this draw, the label says
    that the space before it is free.

## What a job reads

Mapped from two capture sets:
- the 120 gate jobs (`logs/jobcap-gate`);
- 160 consecutive jobs (`logs/jobcap-seq`: w14 and w15, every job from 60 s on, all but 3 of
  them position only).

Which function reads what comes from a replay of both sets with every local-store load and store
of the job module and the pieces logged under the marked function that made it (temporary,
natives off, 0 differences). A "read" of an input below is a read of its bytes before the
job's own code has written over them.

### How the inputs arrive

The job manager double-buffers.
- The local store has two buffer sets, `0x11000-0x1CFFF` and `0x1D000-0x28FFF`. While a job runs
  from one, the job manager fetches the next job's inputs into the other:
  - its 160-byte descriptor, to LS `0x0C00`;
  - its 176-byte DMA list, as the set's last 176 bytes (`0x1CF50` or `0x28F50`);
  - the list's elements, with `GETL`, one after another from the set's base.
- Checked on the consecutive captures (156 jobs with a capture before them on their SPU):
  - in 132, every element the job before fetched was in place when the job started;
  - in 20, the job before fetched nothing (the inputs came between jobs);
  - in 4, a job that was not captured ran between them.
- So a job's own inputs are in the snapshot of its own capture, not in its events. The job
  module keeps the previous job's set at LS `0x10918` (base) and `0x1091C` (end); the job's own
  set is the other one. Laid out that way, the three pieces of all 120 gate jobs match the
  executable's code at their EAs.
- When the job starts, the descriptor's words 0-13 are zero (the job manager has taken them)
  and words 14-39 are in place. The job body copies the 160 bytes to `0x10610`, and the stages
  read the copy.
- The context `c` (`0x107A0`) is filled by `0xBFB8`, inside `0x88B0`, from a 128-byte block the
  job body writes at `0x10720`.

So a stand-in takes a job's inputs from main memory, through the job's descriptor and DMA list,
and keeps the job manager's prefetch going for the job after.

### The DMA list: 22 slots

Each of the list's 22 `(size, EA)` pairs has one role. An empty slot (size 0) takes no room; the
others follow one another, 16-byte aligned.

| slot | what | bytes | read by |
|---|---|---|---|
| 0 | the light set (below) | 256-1616 | lit jobs: `0x9A60`, lighting `0x73E0`, point lights `0x6AC8`. Position-only jobs get one too, and never read it. |
| 1 | the output piece: `0x765B70` (positions), `0x755B80` (lit), `0x762470`, `0x756DE0` | 336-2368 | itself (its constants) |
| 2 | the input piece: `0x764D10`, `0x75BD10`, `0x75EB40`, `0x758590`, `0x764520`, `0x75E180`, `0x75AA50` | 672-2144 | itself, called from `0xDBC8` |
| 3 | the attribute-method piece: `0x7661C0`, `0x75FE80`, `0x765170`, `0x7613D0` | 192-960 | (code only) |
| 4 | 16 bytes: byte 0 a bit pattern (`0xFF` in 104 of 280 jobs, `0xCF`, `0x3F`, ...), byte 1 now and then, the rest 0 | 16 | the index stage, in 5 of 280 jobs only |
| 5 | the index stream ([its format](2026-11-03-edge-stages.md)) | 32-1488 | the index stage (`0xDCB0`, `0xE4D0`) |
| 6, 7 | skinned jobs: the matrix palette, 64 bytes a matrix, in two pieces (slot 7 can be empty) | 64-11456 | skinning `0xE550` |
| 10 | skinned jobs: 8 bytes a vertex (bone indices and weights) | 160-4096 | skinning `0xE550` |
| 11 | the vertex stream, in the input format | 240-10080 | the input piece |
| 13 | the segment header (next section) | 16 | the job body, into `c + 0x00` |
| 14 | the input vertex format | 32-48 | `0x88B0`, `0xDBC8` |
| 16 | the output vertex format | 64-80 | `0x88B0`, `0xDBC8` |
| 17 | the view block: the hole's viewport and scissor values (1280 x 720, the 640 / 360 offset and scale, ...). Many jobs share one (2-3 EAs in 80 consecutive jobs). | 272 | the job body |
| 18 | a 4x4 matrix (at the gate an identity with a tiny translation) | 64 | the job body |
| 8, 9, 12, 15, 19-21 | empty in all 280 jobs | | |

Three patterns of filled slots in the 280 jobs:
- 0-5, 11, 13, 14 and 16-18 (178 jobs);
- those and 6, 7, 10 (91 skinned jobs);
- those and 6, 10 (11 skinned jobs).

The palette's size is the descriptor's matrix count (word 18) x 64 in every skinned job, and slot
10's is 8 bytes x the vertex count, rounded up to 16.

### The descriptor

The readers are per quadword (the SPU loads 16 bytes), so a row's readers are those of its
quadword.

| words | what | read by |
|---|---|---|
| 0-13 | the job manager's part: (LS address, flags) pairs of the buffer layout, the DMA list's EA (word 9), ... | the job manager; zero when the job starts |
| 14 | `0xA00` in every job | the job body |
| 16 | the ring lines' base, `0x73254300` (an SPU's line is at + 0x80 (spu + 1)) | the driver, the rings (`0x41A0`), the output |
| 17 | the hole's EA: the fenced PUT goes there in all 280 jobs | the same |
| 18 | high half: the number of matrices in slots 6 and 7 (skinned jobs; 0 otherwise) | the same |
| 19 | `0x32005A08` (position only), `0x3200520C` or `0x3204520C` (lit) | the same |
| 20 | a frame counter (`0x98D`, `0x98E`, `0x98F` over the gate's three lists) | the job body; in no output |
| 22 | lit jobs: the EA of a 16-byte record the job fetches itself (below) | the job body |
| 24-27 | the RSX attribute slots of the attributes the job puts out, a byte each, `0xFF` ends: `[0]` position only, `[0, 4, 5, 7, 8, 14, 15]` lit | `0x88B0`, `0x5CF8` |
| 28, 29 | the fragment program object's EA and a 48-byte block's EA; both 0 when the job writes no program | the driver, `0x67C0`, `0x61B0` |
| 30 | high half: the program object's size (`0x890`, 2,192 bytes); low byte `0xFE` in every job | the same |
| 31 | high half: the 48-byte block's size (`0x30`, also `0x60`, `0x70`); low half 2-6 | the same |
| 32 | the screen, `1280 << 16 \| 720`, which goes into the hole | the job body, `0x88B0`, the driver, the output |
| 33 | `0x3101` in every job | the same |
| 34 | `0x0213xxyy` (position only) or `0x0013xxyy` (lit), xx and yy changing | the same |
| 35 | `0x04000000` (lit), `0x02000000` or 0 | the same |
| 36-38 | three signed integers, 0 in about half the jobs | `0x88B0` (all skinned jobs, and some others) |
| 15, 21, 23, 39 | 0 | |

At the gate, the 22 jobs with word 28 set are the 22 that write a program to the second ring.
Two more holes bind a program with word 28 at 0: one another job wrote.

### The light set (slot 0)

- `+0x00`: the offsets of four arrays: positions, colours, directions, and a byte a light.
- `+0x10`: the number of lights of each kind, n0, n1, n2 (then 0).
- `+0x20`: n0 + n1 + n2 (the arrays' length), 1, n0 + n1 + n2 again, 0. `+0x30` to `+0x8F`: 0.
- The arrays, a quadword a light:
  - position: x, y, z, and 1/radius in w;
  - colour: r, g, b (HDR), and a signed weight in w;
  - direction: x, y, z (a unit vector) for the first n0.
- The lights come in kind order:
  - `[0, n0 + n1)` go through the lighting stage's loop (`0x7BB4`, over `r83` = n0 + n1
    lights). The first n0 use their direction (`r100` = 16 n0); the others use the vector to the
    vertex.
  - `[n0 + n1, n0 + n1 + n2)` are point lights, which `0x6AC8` adds to the vertex colour (its
    `r9` and `r10`; for example 20-21 in a job with 8 + 12 + 1 lights).
- The fourth array is a byte a light, padded to 16 bytes (`0x00`, `0x01`, `0x02`, `0x80` seen).
  In 6 of the 41 lit jobs, `0x9A60` reads it and sets some lights' colours to 0 before the
  lighting stage: a light selection, whose rule is not mapped.
- All of this holds in all 280 jobs' light sets.

### The vertex formats (slots 14 and 16)

- A format: byte 0 the number of attributes, byte 1 the stride; from byte 8, 8 bytes an
  attribute: offset, type, components, attribute id, size, and 3 bytes more.
- The output format's types are the RSX's (1 I16N, 2 F32, 3 F16, 6 X11Y11Z10), the same as the
  hole's `VERTEX_DATA_ARRAY_FORMAT` types. The input format adds two of EDGE's own:
  - 8: fixed point, 16 bits a component (6 bytes for x y z);
  - 9: a unit vector in 3 bytes.
- At the gate there are few formats:
  - input: an F32x3 position and two type-9 vectors (ids 3 and 4), stride 18; or a type-8
    position and the same, stride 12;
  - output: 8 attributes, stride 66: id 1 F32x3 +0, id 3 I16N x4 +12, id 4 X11Y11Z10 +20,
    id 27 F32x3 +24, ids 28-30 F16x3 +36 / +42 / +48, id 31 F32x3 +54.
- The job puts out the output format's first attributes, as many as the descriptor's slot list
  (words 24-27) names, each to its RSX slot:
  - lit: seven, stride 54 (id 31 left out);
  - position only: one, stride 12.
  - That is each hole's vertex arrays in 278 of the 280 jobs; in the other 2, slot 16 holds
    something else.
- The vertex count: the job rounds the segment's count up to a multiple of 8 (all 265 jobs with
  the two common output pieces). It puts out that many records: the vertex PUTs are the rounded
  count x the stride, in pieces of at most 16 KB.

### What a lit job fetches itself

- The 16-byte record of word 22, twice at the start, to `0x10710`.
  - Its words: `0x0005FAE0` (or `0xFFFFFFFF`), a word with bit 31 set that changes from job to
    job, `0x0000FFFF` (or `0xFFFFFFFF`), 0.
  - Only the job body reads it, and none of it is in the job's output.
- When word 28 is set: the 48-byte block (word 29) and the program object (word 28), into the
  job's buffer set. The block's words go into the second ring with the program: it is the
  per-draw block the program's constants are patched from.

### Not mapped yet, on the input side

- What words 19, 20 and 33-35 and the 16-byte record do inside the job. None reaches an output.
- Slot 4, which the job module hardly reads.
- The light selection in `0x9A60`.
- The palette's matrix layout and the 8-byte vertex weights, from the skinning code.

## The segment header, and the pieces each format takes

The segment header is slot 13. The job copies it to `c + 0x00` and fills in four of its
fields; the table is `c`'s.
- In the element, bytes 2, 4 and 5 are `0xFF` (278 of 280 jobs), byte 1 is a larger number
  (`0x28`-`0x2B`) and bytes 8-9 are the exact vertex count.
- In all 280 jobs the hole PUT is `c`'s byte 1 x 16 bytes.

| byte | field | seen in `c` |
|---|---|---|
| 0 | flags | 0x87 |
| 1 | hole size, in 16 bytes (the job's) | 0x0E-0x1D |
| 2 | input vertex format (the job's number, one per input piece) | 0x00, 0x01, 0x03, 0x09, 0x0A, 0x0C |
| 3 | secondary input format | 0 |
| 4 | output vertex format (the job's number, one per output piece) | 0x00, 0x02, 0x04 |
| 5 | vertex delta format | 0xFF (none) |
| 6 | index kind (high nibble), skinning (low) | 0x20, 0x23 |
| 7 | skinning matrix format | 0x02 |
| 8-9, 10-11 | vertex count (rounded up to 8), index count | 24-568, 99-2850 |
| 12-15 | index offset | 0xFFFFFFFF |

Every job loads three pieces: an input unpacker, an output packer, and a piece that writes the
vertex attribute methods into the hole.

| input -> output | jobs | pieces |
|---|---|---|
| 0x01 -> 0x00 (position only) | 52 | `0x765B70` (12-byte out), `0x764D10` (18-byte records in), `0x7661C0` (the hole's slot-0 format/offset) |
| 0x01 / 0x0A -> 0x00, fixed-point in | 23 (16 skinned) | `0x765B70`, `0x75EB40`, `0x7661C0` |
| 0x03 -> 0x04 (lit) | 27 | `0x755B80` (54-byte out), `0x75BD10` (records in), `0x75FE80` (the seven slots) |
| 0x03 / 0x0C -> 0x04, fixed-point in | 12 (3 skinned) | `0x755B80`, `0x758590` (or `0x75BD10`), `0x75FE80` |
| others | 6 | `0x756DE0` / `0x760F30` / `0x7613D0`, `0x762470` / `0x765170`, ... |

`0x7661C0` is straight-line code: it writes `VERTEX_DATA_ARRAY_FORMAT[slot] = 0xC32` and
`VERTEX_DATA_ARRAY_OFFSET[slot]` into the hole buffer. `0x75FE80` does the same for the lit
layout, but has not been read yet.

## The pipeline, end to end

Mapped with a call trace of the replay (temporary, not kept). Marks at every call in the lifted
module recorded:
- the call's arguments and cycles;
- the attribute list at `c + 0x160` after it;
- the local-store ranges it wrote, named by the attribute arrays of the pool at `c + 0x10`.

The 120 jobs fall into 9 call sequences, four of them common. The stage driver is `0x51D0`;
its second half runs inside `0x17E0`.

| step | function | position only | lit | skinned |
|---|---|---|---|---|
| set-up, DMA | `0x9990`, `0xAF38`, `0x88B0` | fills the context (header, tables, empty list) | same | same |
| unpack | `0xDBF8` -> `0xDBC8` -> the input piece | id 1 | ids 1, 3, 4 | id 1 (and 3, 4 for lit) |
| transform | `0xF538` (`r6` the matrix) | id 1 (unchanged at the gate: an identity) | ids 1, 3, 4 (two matrices) | - |
| skinning | `0xE550` -> `0xBBC8` per array | - | - | ids 1, 3, 4 in place, a matrix buffer, a temporary array |
| the rings | `0x41A0` -> `0xF770` (SPU number), `0x4110` x2 | both lines to `0x3F580` | same | same |
| indices | `0xE4D0` | in place | same | same |
| draw constants | `0x67C0` | a 16-byte record on the stack | the 176-byte constant block | as its kind |
| fragment program | `0x61B0` | (some) | copy and patch, 2,192 bytes | as its kind |
| culling | `0xD618` (-> `0xD640`, `0xD250`) | - | the index list compacted, a temporary array | lit only |
| more attributes | `0xBFA8` x4, `0xBEE0`, `0x9A60` | - | ids 27-30 added | lit only |
| attribute methods | `0xA880` (the pieces at `0x111C0` / `0x111F0`) | stack only | stack only | same |
| lighting | `0x73E0` | - | ids 27 (F32x3) and 28-30 (F16x3) written | lit only |
| output | `0x9080` | `0x765B70`, the ring, the hole | `0x755B80`, the second ring, the hole | as its kind |

So:
- Positions take unpack, transform, indices and output. Culling is not done at all for them;
  they are the depth and shadow passes.
- Lit vertices add the frame (normal and tangent), culling, the per-draw constants and program,
  and the lighting. Lighting makes the four attributes (27-30) that `0x755B80` packs as F32x3
  and F16x3.
- Skinning replaces the transform.
- Small accessors (`0x5C88`, `0xB638`-`0xB660`, `0xBEE0`, `0xBFA8`, `0xB600`) look up and register
  attribute arrays in the context's tables.

## What this means for a stub job

The job's observable effects are three things:
- the ring allocation (one atomic line);
- the ring data, which only the RSX reads;
- the hole, a fixed list of methods.

Anything that makes the same three, in the same order and with the same labels, can stand in
for the SPU job.

**Still to map:**
- the input side's open points (above): some descriptor words, the 16-byte record, slot 4,
  the light selection;
- `0x67C0`, `0x61B0`, `0x9A60`, `0xA880` and `0x75FE80` inside, and the skinning's formats;
- the wrap path, exercised (it needs a longer run than the 120 jobs).
