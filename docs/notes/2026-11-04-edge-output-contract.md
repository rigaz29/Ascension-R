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

- A 16-byte job entry, a 160-byte job descriptor, and a 176-byte DMA list.
- The list's elements are fetched with `GETL`:
  - a 736-byte parameter block;
  - the code pieces (`0x755B80`, `0x75BD10`, `0x75FE80`, ...);
  - the index stream;
  - the vertex stream (the 18-byte records of `0x75BD10`/`0x764D10`, or the fixed-point
    formats of `0x758590`/`0x75EB40`);
  - the 16-byte EDGE context header (`c + 0x00`);
  - attribute descriptors (32 + 80 bytes);
  - the 272-byte viewport block, and a 64-byte matrix;
  - for lit jobs, lighting data, the fragment program object and its constants.

## What this means for a stub job

The job's observable effects are three things:
- the ring allocation (one atomic line);
- the ring data, which only the RSX reads;
- the hole, a fixed list of methods.

Anything that makes the same three, in the same order and with the same labels, can stand in
for the SPU job.

**Still to map:**
- the descriptor's, parameter block's and attribute descriptors' fields;
- which inputs feed which stage (the input contract);
- the wrap path, exercised (it needs a longer run than the 120 jobs).
