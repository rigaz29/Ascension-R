# Precompiling every shader, and a smaller cache: review (2026-11-08)

The player asked whether all of the game's shaders could be precompiled (as an option), and whether the
cache could be compressed so it does not grow too large. A and B are built (the last two sections); C and D are not.

## What there is now

- **Shader cache** (`graphics.shader_cache`; with DXC, `<file>.dxil`). Format PS3RSHC2: each compiled
  program keyed by its HLSL, target and flags.
- **Pipeline cache** (`<file>.pso`, GWPSOC01). Each pipeline the game asked for: a key, its description,
  and both programs' bytecode.
- At start, a thread ("pipeline cache") builds every recorded pipeline before the title needs it. New
  programs are compiled by the shader workers while the game goes on (`graphics.async_shaders`); the
  draws that need them are skipped until they are built.

What is on disk:

| File | Content | Size |
|---|---|---|
| `build/d3d12_shader_cache.bin.dxil` (player) | 2,290 programs, 2,287 distinct | 28.5 MB |
| `build/d3d12_shader_cache.bin.dxil.pso` (player) | 2,309 pipelines | 54.9 MB |
| `logs/edge-ana/fightcache/dx2.bin.dxil` (bench) | 2,370 programs, 1,920 distinct | 26.7 MB |
| `logs/edge-ana/fightcache/dx2.bin.dxil.pso` (bench) | 6,143 pipelines | 102.2 MB |

The player's folder also held the FXC cache from before DXC (`d3d12_shader_cache.bin` 126.6 MB, `.pso`
58.2 MB, 11-05/06), no longer read; it went to the Recycle Bin.

### Where the size goes

- **The pipeline file repeats bytecode.** Each pipeline stores its two programs whole:
  - the player's 2,309 pipelines use 673 distinct vertex and 1,575 distinct pixel programs, so 53.4 MB of
    bytecode is 28.1 MB distinct;
  - the bench's 6,143 use 210 and 1,184, so 98.8 MB is 17.5 MB;
  - and those programs are already in the shader cache. The descriptions alone are 1.5 MB and 3.3 MB.
- **The pipeline table holds at most 6,143 pipelines** (8,192 slots, three quarters full). The bench's
  file is at that limit, so later pipelines are no longer kept.
- **The bytecode is DXIL**, LLVM bitcode inside a container: little to gain inside one program, much
  across programs.

On the player's 28.4 MB of distinct bytecode:

| Method | Size | Ratio | Compress | Decompress |
|---|---|---|---|---|
| zlib 6, each program | 18.5 MB | 1.5x | | |
| zlib 6, one stream | 13.6 MB | 2.1x | | |
| Windows XPRESS + Huffman (Cabinet.dll) | 14.9 MB | 1.9x | 0.1 s | 0.07 s |
| Windows LZMS (Cabinet.dll) | 13.0 MB | 2.2x | 6.6 s | 0.36 s |
| zstd 3, one stream | 9.7 MB | 2.9x | 0.2 s | 0.05 s |
| zstd 19, one stream | 6.2 MB | 4.6x | 9.1 s | 0.07 s |
| zstd 19 long-distance (window 2^27) | 5.8 MB | 4.9x | 9.7 s | 0.06 s |
| LZMA 6, one stream | 5.7 MB | 5.0x | | |
| zstd 19, each program, 110 KB trained dictionary | 10.2 MB | 2.8x | | |

The gain is across programs: a compressor with a long window over all of them (zstd long, LZMA) reaches
~5x, while one program at a time stays under 3x. zstd is BSD-licensed and decompresses the lot in
0.06 s.

## Can every shader be compiled before it is seen?

The title's shaders are in its own files, as RSX microcode. 80 fragment programs were dumped from a run
(`FP_DUMP=80`, the first 32 bytes of each as the RSX reads them), and eight were looked for in the
archives, every entry decompressed in memory, read only:

- no PSARC holds Cg binaries (`CgBinaryProgram`, profiles 7003/7004);
- RESOURCES.PSARC (3,780 MB unpacked) and HECA.PSARC (2,778 MB) had three of the eight, verbatim,
  inside `.WAD` and `.AS` files;
- one of them is in 173 WADs (each character's WAD carries its own copy), another in Kratos's `.AS` and
  in `R_UI_SP.WAD`.

The other five are in other archives, or start with constants the engine writes into the program in
memory, so their first 32 bytes differ from the file's.

So the programs can be found without playing. What it would take:

1. **The WAD's shader record.** Where each program starts and ends, vertex or fragment, and what goes
   with it: the fragment program's control word, register count, texture use. Reverse engineering from
   the known places, the bytes around the 173 copies, and the engine's loader in the lifted code.
2. **Not all of the state is in the file.** The backend's HLSL for a fragment program also depends on
   what is bound at the draw: texture kinds, sRGB, the depth export. A pipeline depends on blend,
   depth, the target's format and the vertex layout. An offline build can make the common variant of
   each program; other variants still compile when met. How many runtime programs it would cover can
   only be measured: build from the files into an empty cache, play the gate and the fight, and count
   cache hits against compiles (the backend's `[PERF] shaders` line).
3. **Time.** All 37 GB of archives read and unpacked, then each program decompiled and compiled with
   DXC (tens of ms each, on the shader workers). Likely a few minutes, once, as an optional step with
   progress.
4. **Distribution.** The cache comes from the title's code, so it is built on the player's machine from
   the player's game, never shipped.

## Options

**A. A smaller, unbounded cache.** Certain; medium work.

- One store of programs by content hash, each once.
- Pipelines refer to their programs by hash. The pipeline file is then descriptions only (MBs), and its
  table grows instead of stopping at 6,143.
- The store in solid zstd packs (vendored zstd). New programs are appended to an uncompressed journal
  during play; at the next start the journal is folded into a pack, level 19 long, in the background.
- Reading the old files once to convert them.
- The player's 83 MB would become ~6 MB; the old FXC files (185 MB) could go to the Recycle Bin.

**B. Build the cached pipelines before the menu.** Small work; optional in the launcher.

- The "pipeline cache" thread's work, waited for with a progress screen, instead of racing the first
  scenes. Nothing seen before would compile or pop in.

**C. Precompile every program from the game's files.** Uncertain coverage; large work.

- A research spike first: the WAD shader record, and the cache hit rate an offline build gives.
- Only then an optional launcher action, "Precompile all shaders": every archive read and every program
  compiled into the store from A, with progress.

**D. Compile a level's programs while it loads.** Same research as C.

- The backend sees the WAD read through the file system and compiles its programs during the loading
  screen. Incremental, no long first step.

## Recommendation

A first: it fixes the size whatever comes after, and removes the 6,143 cap that already loses pipelines.
Then B, which costs little. C needs the spike to say whether its hit rate is worth the work; D reuses
C's record parser.

## A, built

The store is in `libs/video/rsx_d3d12_backend.c` ("Compiled guest shaders on disk"), with zstd 1.5.7 in
`ps3recomp/third_party/zstd/`.

- Each program is kept once, under a hash of its bytes (FNV-1a 64). Sources (SRCK) and pipelines (PIPE) name
  programs by that hash. Bytes under a known hash are compared before they are taken, so one program never stands
  for another.
- `<file>` is the pack: the whole store in one zstd frame (level 19, long-distance matching, checksum).
  `<file>.journal` takes each new record as it is made.
- A fold packs the store again on a thread of its own, at idle priority. It runs when:
  - the journal has passed an eighth of the pack (4 MB at least);
  - the pack is damaged;
  - the files are from before the store.
- After a fold the journal starts over, under a new id, with what was appended while the fold ran. The pack records
  how much of which journal it holds, so a fold cut short between the two steps loses nothing and repeats nothing.
- A record cut short ends the read, and the journal is cut back to the records before it.
- A file that is there but cannot be read (open elsewhere, out of memory) stops the fold for that session, and a
  journal that cannot be read is left alone: neither can then overwrite what is in them.
- The pipeline table grows, and the store keeps every pipeline. Built pipelines cost memory, though, so the table
  holds at most 8,192 built (`D3D12_PSO_CACHE_MAX`). The start builds the most recently used first: a session notes
  each pipeline it uses and journals the notes (USED) every 10 s.
- Clear shaders in the launcher takes the journal and a cut-short fold's `.tmp` files too.

### What it gives

| Cache | Before | After | Holds | Packed in |
|---|---|---|---|---|
| The player's (a copy) | 83.0 MB | 5.9 MB | 2,287 programs, 2,290 sources, 2,309 pipelines | 11.3 s |
| The fight bench's (a copy) | 128.9 MB | 4.8 MB | 1,920 programs, 2,370 sources, 6,143 pipelines | 9.1 s |

`tools/shader_cache_check.py` read both packs against their old files. Every source and pipeline was there with
the same bytes, every program under the hash of its bytes, and no record was damaged.

The fight bench (FSR 1, frame generation off, 1600x900, one run each):

- Reading the store at the start: 230 ms from the old files, 58 ms from the pack. No program was compiled in a warm
  run.
- Building the 6,143 pipelines: 1.27 s on the pipeline cache's thread. They took 373 MB more while they were built,
  and the run peaked at 4,340 MB, against 4,059 MB with `D3D12_PSO_CACHE_MAX=1`.
- Frame rate: medians 35.50 (the converting run), 33.39 (warm) and 32.79 (`MAX=1`). Before the store,
  `rtn1` gave 35.23. The runs' timing against the fight differed, so these sit within one run's spread.
- From an empty cache, the fight compiled 502 programs into a 6.1 MB journal. The next start folded it to 1.3 MB
  in 1.5 s, and 458 KB that came in during that run stayed in the new journal.
- A journal cut short and a pack held open by another process: the journal was cut back to its last whole record,
  and the pack was neither folded nor changed (same SHA-256).

## B, built

### What there was to win

The start's build begins at the first guest pipeline, which comes on the boot's first logo, not at the title.
With the driver's own cache warm it was over 1.1 s into the boot, about 28 s before the title (the intro movie
closes at 30.7 s in the fight bench).

A whole warm session was logged with `RSX_CENSUS=600 RSX_HITCH_MS=1000`: every present that skipped a draw or
queued a build. The session ran from the boot through the menus and 140 s of the fight. It queued one build, at
2.48 s on the first logo (0.8 ms of shader wait), and skipped no draw. So a wait changes nothing while the driver's
cache is warm. It matters when the driver compiles every pipeline again (after a driver update), and the build then
runs into what the boot shows.

### What it does

- The start's build runs on several threads (`D3D12_PSO_CACHE_THREADS`; half the logical processors, at most 8),
  each taking the next pipeline in the order of last use.
- `graphics.pipeline_wait` (on by default; "Build pipelines first" in the launcher's Graphics tab): the title waits
  for the build where its first guest pipeline is asked for, before its first picture. The wait is 30 s at most,
  and the window's messages are seen to meanwhile; without the render thread, this thread owns the window.
- The builders run at normal priority while the title waits for them, below normal when it does not.
- No progress screen: nothing is presented during the wait, and it is shorter than such a screen would be worth.
  With the wait off, the overlay's "Building pipelines" ring shows the build as before.

### Numbers (the fight bench's 6,143 pipelines, warm driver cache)

| Run | Build | The title waited |
|---|---|---|
| One thread | 1,019 ms | 1.02 s |
| Six threads | 319 ms | 0.32 s |
| Six threads, render thread off | 314 ms | 0.31 s |
| Six threads, the whole fight | 330 ms | 0.33 s |

- The whole fight with the wait on had a median of 34.49 fps (earlier runs: 33.4 to 37.5) and peaked at 4,347 MB.
  The intro movie closed at 30.7 s, as without the wait.
- Not measured: a cold driver cache. Emptying it would have meant deleting the system's driver cache.
