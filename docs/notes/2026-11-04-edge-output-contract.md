# EDGE's output contract (phase 2b, step 2)

Written 2026-11-04, branch `edge`. Step 1 made every EDGE piece and the index decompression
native and mapped their formats ([edge stages](2026-11-03-edge-stages.md)). Step 2 maps what
a job hands out, and what it takes in, so that something other than the SPU job can produce
the same result.

Source: the 120 gate jobs in `logs/jobcap-gate` (40 from each of w14, w15, w16). Scratchpad
scripts parse the capture format (`SPUJCAP2`: the context, then GET / PUT / atomic / channel
events with their data).

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

The job manager double-buffers jobs.
- The GETs in a job's capture are mostly the **next** job's inputs: its descriptor, DMA list and
  buffers, fetched while this job runs.
- A job's own inputs are already in the local store when it starts. They come in the context
  snapshot of its capture, not as events.
- The context at `c` is filled during the job, by `0x88B0`.
- One example of the trap this sets: `job_w14_0002`'s GETs carry a 544-vertex segment, but the
  job itself draws 48 vertices.

So a stand-in has to take a job's inputs from main memory, through the job's own DMA list. It
also has to keep the job manager's prefetch going for the job after.

### A job's input set

Mapped from 160 consecutive jobs captured at the gate (`logs/jobcap-seq`: w14 and w15, every
job, from 60 s on). Each capture holds the input set of the job after it on its SPU.
- The **job descriptor**: 160 bytes, to LS `0x0C00`.
- The **DMA list**: 176 bytes, up to 22 `(size, EA)` pairs, to LS `0x1CF50` or `0x28F50`.
- The list's **elements**, fetched with `GETL` one after another from LS `0x11000` or
  `0x1D000`, the two buffer sets of the double buffering. In every set they matched the list.

| element | what | size |
|---|---|---|
| 0 | the parameter block (below) | 448-880 |
| 1 | the output packer piece (`0x765B70`, `0x755B80`, `0x762470`, ...) | 336-848 |
| 2 | the input unpacker piece (`0x764D10`, `0x75BD10`, `0x75EB40`, `0x758590`, ...) | 672-1552 |
| 3 | the attribute-method piece (`0x7661C0`, `0x75FE80`, ...) | 192-448 |
| 4 | a 16-byte control word (`0x41`, `0xF1`, `0x3F7F...`) | 16 |
| 5 | the index stream (the header of [the index decompression](2026-11-03-edge-stages.md)) | 32-1328 |
| 6 | the vertex stream (records; for example 432 vertexes x 18 = 7776) | 64-9648 |
| 7 | the segment header (copied to `c`) | 16 |
| 8 | the input vertex format | 32-80 |
| 9 | the output vertex format | 64-80 |
| 10 | the viewport block, one per frame (the same EA in every set) | 272 |
| 11 | the local-to-world matrix (4x4) | 48-80 |
| 12-14 | lit jobs: more blocks, and a second matrix | 64-272 |

**The job descriptor.** Most of it is the same in every job.
- Word 9 is the DMA list's EA.
- Word 16 is the ring lines' descriptor (`0x73254300`).
- Word 17 is the hole's EA in the command buffer.
- Words 18, 34 and 36-38 change from job to job:
  - 18: a count in its high halfword;
  - 34: `0x0213xxyy`;
  - 36-38: three signed integers, zero for some jobs.
- The first words pair LS addresses with flags (`0x200 / 0x10340100`, `0x220 / 0x44300200`, ...):
  the job manager's buffer layout.

**The parameter block** (element 0) starts with four offsets of sub-blocks (`0x90`, `0x120`,
`0x1B0`, `0x240`, 144 bytes each but the last), then eight counts (for example 2, 4, 3, 0, 9, 1,
9, 0). The sub-blocks read as the lights:
- positions (-681.7, -98.7, 0), with what looks like an attenuation in w (1/6, 2/9);
- HDR colours (3.33, 2.74, 2.01);
- unit directions (0.649, 0.658, -0.381).
The lighting stage takes them. Which word goes where is still to map, from its native code.

**The vertex formats** (elements 8 and 9) are a 4-byte header (attributes, stride) and an 8-byte
entry per attribute: offset, type, components, attribute id, size.
- The types are the RSX's vertex types: 1 I16N, 2 F32, 3 F16, 6 X11Y11Z10. So a format turns
  straight into the hole's `VERTEX_DATA_ARRAY_FORMAT` words.
- For example, one output format:
  - 7 attributes, stride 58;
  - id 1 F32x3 at +0;
  - id 2 X11Y11Z10 at +12;
  - id 27 F32x3 at +16;
  - ids 28-30 F16x3 at +28, +34, +40;
  - id 31 F32x3 at +46.
- With that, the unpack and pack pieces could be one generic routine driven by the formats.

## The segment header, and the pieces each format takes

The 16 bytes at `c + 0x00` (one of the DMA list's elements, copied there) describe the segment.
In every job the header's hole size matched the hole it wrote.

| byte | field | seen |
|---|---|---|
| 0 | flags | 0x87 |
| 1 | hole size, in 16 bytes | 0x0E-0x1D |
| 2 | input vertex format | 0x01, 0x03, 0x0A, 0x0C |
| 3 | secondary input format | 0 |
| 4 | output vertex format | 0x00, 0x02, 0x04 |
| 5 | vertex delta format | 0xFF (none) |
| 6 | index kind (high nibble), skinning (low) | 0x20, 0x23 |
| 7 | skinning matrix format | 0x02 |
| 8-9, 10-11 | vertex count, index count | 24-568, 99-2850 |
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
- the descriptor's, parameter block's and attribute descriptors' fields, and which input block
  feeds which step (the matrices, the lights, the skinning matrices);
- `0x67C0`, `0x61B0`, `0x9A60`, `0xA880` and `0x75FE80` inside, and the skinning's formats;
- the wrap path, exercised (it needs a longer run than the 120 jobs).
