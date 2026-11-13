# Issue #12 again: Proton tried here, and the port's own shaders from DXC (2026-11-13)

- **The second report:** with `GOWA_EDGE_GPU=0` the hang is gone (`2026-11-13-vertex-pass-compile.md`), but every 3D
  picture is black, the main menu's too: its UI, the cinematics and the sound are right, at 59 fps and 842 draws a
  frame. The logs have no error; 417 pipelines built, none failed; the guest programs from DXC.

## Proton's graphics layers on this PC

- vkd3d-proton 3.1 (`d3d12.dll`, `d3d12core.dll`) and DXVK 3.1.1 (`dxgi.dll`, `d3d11.dll`) beside a copy of the exe, in a
  folder outside the tree, from their GitHub releases (SHA-256 as GitHub gives it). None of the four is a known DLL of
  Windows, so the copy loads them. `VKD3D_FILTER_DEVICE_NAME` and `DXVK_FILTER_DEVICE_NAME` keep them on the NVIDIA GPU.
  It is on when the log's driver reads `35.0.65535.65535`, as in the report, and vkd3d-proton writes its log
  (`VKD3D_LOG_FILE`).
- It does not take in Wine itself: there `D3DCompile` is Wine's, on vkd3d-shader; here it is Microsoft's. Nor Linux's
  driver, Wayland or CachyOS's own build of vkd3d-proton.
- **It draws as Windows does.** The temple replay of issue #9, from the same full shader cache: eight screenshots from
  4 to 35 s after the level's first file, the same moments to within their luma, share of black and detail. The intro
  and the menu's 3D as well. vkd3d-proton logs no error.
- **The same pace.** 40 s of the level, one run each: flips a second 52.6 on average against 51.0, median 54.7 against
  52.7, the least 34.7 against 32.9, none skipped. Both fall to 30-40 in the same fight, with 7 to 9 of the 12 threads
  busy.
- **A false alarm first.** The first runs seemed to show it: a black screen with an orange cloud. That was against a
  run from an empty shader cache, whose replay drifted. From 25 to 35 s after the level's first file the replay itself
  stands in that spot, on Windows as well. And screenshots timed from the start fall at other moments of the replay;
  from the level's first file they line up. Screenshots and captures pause the game, which reads as stutter on screen.

## The port's own shaders

- Besides the guest programs (DXC), the backend compiles programs of its own with `D3DCompile`: the presentation pass
  and its vertex shader, which FSR 1's two passes and the upscaler's last pass share; the overlay; the basic and textured
  fallbacks; the depth-as-colour copy (dsconv); the Y16 copy of the luminance chain; SMAA's six; FSR 3's inputs pass;
  and the vertex pass (`2026-11-13-vertex-pass-compile.md`). Under Wine they all went through vkd3d-shader, the compiler
  that hung on the vertex pass. Two of them take part in 3D only: dsconv and the Y16 copy.
- **`rsx_compile_own`** (fork, `rsx_dxc.c`): `D3DCompile`'s arguments; `D3DCompile`, or DXC. For DXC, the include
  handler's files go in place of their `#include` lines (FSR 1's two headers, `SMAA.hlsl`), the macros before the text,
  the entry and the stage as given (`cs_5_0` becomes `cs_6_0`), the optimisation level as `D3DCompile`'s flags say.
- **Which one:** `D3DCompile` on Windows, as before. Under Wine (`wine_get_version` in its `ntdll`), DXC when the guest
  programs come from DXC, so `D3D12_SHADER_COMPILER=fxc` still puts everything through `D3DCompile`.
  `D3D12_OWN_SHADER_COMPILER=dxc` or `fxc` chooses. The choice is made once the device is, before the first of them. The
  vertex pass follows it; `VTX_LIT_COMPILER` still chooses for it alone.
- A pipeline holds one compiler's shaders: the vertex shader shared by the presentation, FSR 1 and the upscaler is
  compiled the same way as they are. The programs paired with a guest vertex shader (the frame generation's object
  pass, the VP path's pixel shaders) already go with the guest programs (`d3d_compile_cached`).
- **Logs:** `[d3d12] the port's own shaders from DXC, on Wine <version>` under Wine or with the variable; `[d3d12] slow
  own shader compile: ...` for one of 100 ms or more.

## Measured

The temple replay, settings that use the most of them: FSR 3 upscaling from 1280x720 to 1920x1080, FSR 1 and SMAA on
the frames without a scene, the overlay, the vertex pass. One run each.

| run | own shaders | built | screenshots |
|---|---|---|---|
| D3D12 | DXC (`D3D12_OWN_SHADER_COMPILER=dxc`) | all, none slow (the vertex pass 0.7 s) | the level's as the next run's |
| D3D12 | `D3DCompile` | all | the profile menu, the level |
| vkd3d-proton | DXC | all, no vkd3d-proton error | the profile menu with its 3D behind it, the level, as on Windows |

- No program of theirs took 100 ms with DXC. The first run reached the level 3 s after the second, which is the replay's
  spread, not the compiles.
- **Not tried:** Wine. Whether this brings back the reporter's picture, only their PC can say: with this, nothing of the
  port's goes through vkd3d-shader any more, and what remains is the path tried here.

## The reporter's answer

- Later the same day the reporter found it on their side: the prefix Quiver launcher had made, which also kept
  winetricks and Lutris from opening it. In a new prefix made by Lutris, 0.1.0-alpha.7 plays, with no variable and
  without Microsoft's `d3dcompiler_47`: no stop after the logos, and the 3D is there. They told Quiver launcher.
- What that prefix had that the new one lacks is not known here: its `D3DCompile` did not finish the vertex pass, and
  its 3D was black. The changes stand on their own: the port neither waits on a prefix's `D3DCompile` nor compiles
  its own shaders with it under Wine any more. 0.1.0-alpha.9 has them.
