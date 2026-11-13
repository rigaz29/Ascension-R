# The vertex pass's program off the frame's path (2026-11-13, issue #12)

- **The report:** CachyOS through Proton, an i9-13900HX and an RTX 4070 laptop GPU, 0.1.0-alpha.7. After the logos
  came a black screen with the loading mark, then nothing. The freeze watch at 15 s and at 60 s showed:
  - an unnamed thread at 101% of a core in Wine's `libvkd3d-shader-1.dll`, in a recursion;
  - the frame clock thread waiting in `vpass_build+0x100` (`WaitForSingleObject`, `INFINITE`; the exe's map), under the
    FIFO walk (`vpass_segment`, `upload_tris_vp_raw`, `d3d12_draw_indexed`);
  - the PPU's main thread waiting on the RSX (put `0xAFC800`, get `0x934804`).
- **Why:** the vertex pass (the lighting on the GPU, `GOWA_EDGE_GPU`, mode 2 by default) has its compute program
  compiled by `D3DCompile` on a thread of its own from the moment the pass is set. The frame clock thread waited for it
  at the pass's first use. Under Proton, `D3DCompile` is Wine's `d3dcompiler_47`, on vkd3d-shader. It did not finish
  the program (about 340 lines of HLSL, loops and unrolls, the SPU's integer FMA) in a minute. The port's other
  `D3DCompile` programs are small and compiled there: the presentation, the overlay, FSR 1.
- **Nothing waits for it now.** Until the program is compiled, `vpass_build` says no and is asked again at the next
  draw. The draws are lit by the CPU (`vpass_segment`'s `vpass_cpu_copy`, the port's C model): the path a pass that
  does not build takes. A compiler that never ends costs the CPU's time, not the game.
- **The compiler:** `D3DCompile` on Windows, as before. Under Wine (its `ntdll` exports `wine_get_version`) it is DXC,
  when the guest programs come from DXC. `VTX_LIT_COMPILER=dxc` or `fxc` chooses. `rsx_dxc_compile` takes `cs_5_0`, as
  `cs_6_0`. DXC's program is kept apart from `D3DCompile`'s in the shader cache. The port's other programs followed the
  same day, and the vertex pass now takes their choice: `2026-11-13-proton-own-shaders.md`.

## Measured

The temple replay of issue #9, once each, every run from an empty shader cache:

| run | compiler | compile | the CPU lit the draws for | freeze |
|---|---|---|---|---|
| check (`GOWA_EDGE_GPU=1 VTX_LIT_CHECK=1`) | `D3DCompile` | 21.5 s | 20.8 s | none |
| check | DXC | 0.7 s | 0 s | none |
| default (mode 2) | `D3DCompile` | 20.7 s | 19.9 s | none |

- `D3DCompile` took 21.5 s here, not 5.6 s as before, with the guest programs compiling beside it on an empty cache. The
  old code would have stopped the frames for those 20 s at the pass's first use, on Windows too.
- The check at the run's end, the same place both times. `D3DCompile`'s program: 94.5% of the lit F16 values (ids
  28-30) the same as the SPU's, 2.5% one unit apart, 1.2% 4 to 15, 0.04% 16 or more. DXC's: 86.1%, 2.2%, 8.0%, 1.0%. The
  largest differences: `D3DCompile` 0, 0.0039 and 6.1e-05 (ids 28-30), 0.010 (id 27); DXC 0.00015, 0.0029, 0.00012 and
  0.009. Both are far under what shows. `D3DCompile` stays the default where it works, being the nearer.
- Screenshots at 8 and 30 s into the level: Kratos lit by his blades' fire, as usual.
- **Not tried:** Proton itself, as there is no Wine here.
- **Until a release has it:** `GOWA_EDGE_GPU=0` on Proton. The SPU lights the vertexes and the pass is never set.
