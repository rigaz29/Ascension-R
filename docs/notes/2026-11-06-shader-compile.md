# Compiling shaders: where the time goes, and DXC

Written 2026-11-06. The user asked whether shader compiling can be made faster, for instance with LZ4 or by
loading through RAM.

**The result:** the time was in the compiler, not in reading anything, so LZ4 and RAM would not have touched it.
Guest programs now compile with DXC (`libs/video/rsx_dxc.c`): 2.4 times less compiling, half the skipped draws, and
the fight at full speed 20 s sooner. The picture and the frame rate are the same. D3DCompile stays as the fallback.

## Where the time went

The scripted fight once on an empty cache (`PERF=1`, 2 workers below normal priority, beside the game):

| | D3DCompile | DXC |
|---|---|---|
| programs compiled | 494 | 539 |
| compiler time | 145.1 s | 61.4 s |
| a program | 294 ms | 114 ms |
| pipelines | 440 in 0.7 s | 492 in 23.9 s |
| draws skipped waiting for a shader | 725,639 | 370,816 |
| flips/s reach the fight's level | t = 130 s | t = 110 s |

- The pipelines' 23.9 s is the driver's own compiler meeting bytecode it has not seen. D3DCompile's 0.7 s had that
  driver cache warm, so the two columns are not the same case there. It happens once: the next run built its 491
  pipelines in 282 ms. On a new machine D3DCompile's programs would pay it too (not measured).
- DXC's run compiled more programs because it got further in the same time.

**Reading the cache** is not the problem. `d3d12_shader_cache.bin` and its `.pso`, 184 MB together, read in 0.07 s
from the system's file cache, once a session, and from then on the programs are in memory. So they are already
loaded through RAM.

**LZ4** would make the files smaller, not the compiling faster. They compress well (zlib at its fastest level leaves
9%), so it would save ~150 MB of disk.

## The compilers side by side

The first 400 programs of that run (`D3D12_SHADER_MISS_DUMP`), one thread, nothing else running:

| compiler | total | median | slowest | code a program |
|---|---|---|---|---|
| D3DCompile, level 1 | 50.5 s | 100 ms | 1,036 ms | 10.8 KB |
| D3DCompile, level 0 | 46.7 s | 100 ms | 1,056 ms | 11.1 KB |
| D3DCompile, no optimisation | 36.4 s | 80 ms | 975 ms | 21.8 KB |
| DXC, default floats | 18.7 s | 54 ms | 312 ms | 12.3 KB |
| DXC, `-Gis` | 19.0 s | 55 ms | 323 ms | 13.0 KB |
| **DXC, `-Gis -Qstrip_reflect`** | **18.8 s** | **54 ms** | **320 ms** | **10.5 KB** |

The slowest 10% are 30% of D3DCompile's time: vertex programs of 18-19 KB and a 50 KB fragment program.

## What compiling did not show

All 400 programs compiled with DXC on the first try. Three things were still wrong.

**DXC's default floats remove the NaN test.** The fragment decompiler ends a program with `(x == x) ? x : 0`. By
default DXC marks every float operation fast, and `x == x` folds to true, with no warning. D3DCompile keeps it (`eq
r2, r1, r1`). `-Gis` keeps it (`fcmp ord`) and costs no compile time. `tests/test_dxc_guards.py` reads the output
with and without it. The vertex decompiler's `(a == 0 || b == 0) ? 0 : a * b` survives either way.

**A pipeline cannot hold DXBC and DXIL:** `0x80070057` from `CreateGraphicsPipelineState`. Every guest pipeline's
programs now come from one compiler, including `compile_vp`'s, whose vertex shader goes on to pair with guest
fragment programs (`vp_vs_blob`). The two compilers keep their caches apart (`<cache>.dxil`, `<cache>.dxil.pso`).
The compute pass and the backend's own pipelines stay on D3DCompile: they mix with nothing.

**The first run on DXC had a black screen.** 478 pipelines refused, and only the HUD left. The debug layer's reason
for all of them:

> Shader sampler descriptor range (BaseShaderRegister=0, NumDescriptors=16, RegisterSpace=0) is not fully bound in
> root signature

`vp_root_sig` has one table per sampler. D3DCompile keeps of `rsx_samp[16]` only the samplers used, a register each.
For DXC it is a range of 16, which D3D12 wants inside one table. The decompiler only indexes it with constants, so
`rsx_dxc.c` gives DXC sixteen names and leaves the decompiler's text alone: the Vulkan and Metal paths read it, and
so do the backend's own searches of it. The `Texture2D rsx_tex[16]` of some programs is fine: the SRV table is one
range. Offline, on the same root signature with the debug layer on: 209 fragment, 103 vertex and 88 raw vertex
programs through the real `rsx_dxc.c`, all accepted.

## Checks

- Fight, cold and warm: 0 refused, 0 failed compiles. Gate, cold and warm: the same.
- **Picture:** the gate at 80 s, D3DCompile against DXC in the same conditions: the median of 144 blocks differs by
  2.6 of 765, and the signed mean by -0.09 of 255 a channel. Two DXC runs differ by more (17.0), from the camera's
  timing. Fight pictures match by eye.
- **Speed:** gate 34.06 (D3DCompile) against 33.84 (DXC). GPU 25-28% on both.
- **Fallback:** an exe without the DLLs says `guest shaders from D3DCompile: no dxcompiler.dll and dxil.dll beside the
  exe` and uses the plain file. `graphics.shader_compiler = fxc` the same. No shader model 6: not tried here.
- A program DXC refuses is dropped as one D3DCompile refuses, with a line in the log. None so far, but only these two
  scenes have been through it. `fxc` is the way back.

## Reading the fight's numbers

Two things moved the fight's medians during this work, and neither was the compiler.

- **Restarting Windows:** the same build went from ~30.6 flips/s (21:04) to 34.9-36.2 (22:00 on). Before the restart
  `Winmgmt`'s svchost held 6.2 GB. The gate went from ~31.6 to 34. Old medians do not compare with new ones.
- **Where Kratos is at the moment:** on the chain 34.3-36.9 fps, on the floor 32.0-33.7, on either compiler, and which
  one a run gets changes with its timing. So one run against one run says little: D3DCompile's three runs read
  36.17, 35.08 and 34.88, DXC's two 37.23 and 33.59.

## Left

- More workers while the game is loading, and two during play: not measured.
- A bench baseline in a folder of its own needs the two DLLs too, or it runs D3DCompile on another cache.
- The old `d3d12_shader_cache.bin` (126 MB, 15,657 programs, most from earlier versions of the HLSL) stays until
  someone removes it. DXC's file starts empty: 408 programs after the gate.
