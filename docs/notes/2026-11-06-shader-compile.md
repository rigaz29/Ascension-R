# Compiling shaders: where the time goes

Written 2026-11-06. The user asked whether shader compiling can be made faster, for instance with LZ4 or by
loading through RAM. Measured at port `169f570`. Nothing changed yet.

**The result:** the time is in the compiler, not in reading anything. LZ4 and RAM would not touch it. DXC in place
of D3DCompile would: 2.8 times faster on the same programs.

## Where the time goes

The scripted fight once on an empty cache (`logs/edge-ana/sc1/run2`, `PERF=1`):

| | count | time |
|---|---|---|
| D3DCompile, on 2 workers below normal priority, beside the game | 494 programs | 145 s of worker time |
| pipelines (`CreateGraphicsPipelineState`) | 440 | 0.7 s |
| draws skipped while their shader was built | 725,639 | |

- So ~70 s from the first new program to the last, 99.5% of it D3DCompile.
- Alone on the machine the same programs take 126 ms each on average. Beside the game, 294 ms.
- Nothing is read during any of it: a compiled program goes to the file once.

**Reading the cache** is not the problem. `d3d12_shader_cache.bin` and its `.pso`, 184 MB together, read in 0.07 s
from the system's file cache, once a session, and from then on the programs are in memory. So they are already
loaded through RAM.

**LZ4** would make the files smaller, not the compiling faster. They compress well (zlib at its fastest level leaves
9%), because bytecode repeats itself, so it would save disk: ~150 MB.

## What was tried offline

The first 400 programs of that run (`D3D12_SHADER_MISS_DUMP`), one thread, nothing else running:

| compiler | total | median | slowest | code a program |
|---|---|---|---|---|
| D3DCompile, level 1 (now) | 50.5 s | 100 ms | 1,036 ms | 10.8 KB |
| D3DCompile, level 0 | 46.7 s | 100 ms | 1,056 ms | 11.1 KB |
| D3DCompile, no optimisation | 36.4 s | 80 ms | 975 ms | 21.8 KB |
| **DXC, `-O1 -HV 2018`, in process** | **18.0 s** | **53 ms** | **302 ms** | 12.3 KB |
| DXC, `-O3` | 18.8 s | 55 ms | 309 ms | 12.2 KB |

- All 400 compile with DXC. `-HV 2018` is needed: one program has a ternary on a vector.
- The slowest 10% are 30% of D3DCompile's time: vertex programs of 18-19 KB and a 50 KB fragment program.
- `dxcompiler.dll`, `dxil.dll` and `dxcapi.h` are in the Windows SDK.
- Lowering D3DCompile's level gives little, and doubles the code at the end.

## What DXC would need

- `d3d_compile_cached` on DXC for the guest programs, `vs_6_0` and `ps_6_0`, with D3DCompile left as the fallback
  when the DLLs are not there.
- The two DLLs next to the exe.
- **One cold start:** the cache's key changes, so every program is compiled once more. That also drops the old
  entries: the file has 15,657 programs, and a fight needs ~500; most are from earlier versions of the HLSL.
- **Checks, not done yet:** the pipelines' time with DXIL, the GPU's time, and that pictures stay the same. The
  programs compiled here were never drawn.
- Whether a pipeline can mix DXBC and DXIL has to be looked at: guest programs and the backend's own shaders meet
  in none today, but the vertex pass's has to be checked.

## Also

- More workers while the game is loading, and two during play: not measured.
- The file only grows. A cache started over after a change to the HLSL would keep it at what the game uses.
