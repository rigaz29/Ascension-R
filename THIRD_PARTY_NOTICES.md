# Third-party notices

## ps3recomp

`CMakeLists.txt`, `main.cpp`, `stubs.cpp` and `config.toml` started as copies
of `templates/project/` from [ps3recomp](https://github.com/sp00nznet/ps3recomp),
and `tools/hle_coverage.py` imports ps3recomp's `tools/`. The runtime itself is
the `ps3recomp/` submodule (fork: [rigaz29/ps3recomp](https://github.com/rigaz29/ps3recomp)).

```
MIT License

Copyright (c) 2026 Ned Heller / sp00nznet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## AMD FSR API

The runtime carries the FSR API's headers, unchanged, from
[FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) in `ps3recomp/third_party/fsr_api/`, and
`gowa.py build` copies the SDK's signed DLLs beside the exe. They are not in this repository. MIT License,
Copyright (C) Advanced Micro Devices, Inc.; the full text is at the top of each header.

## AMD FidelityFX Super Resolution 1

The runtime carries `ffx_a.h` and `ffx_fsr1.h`, unchanged, from
[GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
(commit `a21ffb8f6c13233ba336352bdff293894c706575`), in
`ps3recomp/third_party/fidelityfx_fsr1/`. They are embedded in the executable for the
"AMD FSR 1" scaling option. MIT License, Copyright (c) 2021 Advanced Micro Devices, Inc.; the
full text is at the top of both files.

## SMAA

The runtime carries `SMAA.hlsl`, `AreaTex.h`, `SearchTex.h` and `LICENSE.txt`, unchanged, from
[iryoku/smaa](https://github.com/iryoku/smaa) (commit `71c806a838bdd7d517df19192a20f0c61b3ca29d`),
in `ps3recomp/third_party/smaa/`, for the "SMAA 1x" anti-aliasing option. MIT License,
Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
Gutierrez; the full text is in that folder's `LICENSE.txt`.

## Zstandard

The runtime carries the library sources of [Zstandard 1.5.7](https://github.com/facebook/zstd/releases/tag/v1.5.7),
unchanged, in `ps3recomp/third_party/zstd/` (its README lists the files taken). The D3D12 backend keeps its shader cache
in one zstd frame. Zstandard is dual-licensed, BSD or GPLv2; it is used under the BSD license, Copyright (c) Meta
Platforms, Inc. and affiliates; the full text is in that folder's `LICENSE`.

## Code taken from other ports

Whenever code is copied from another repository, add it here with its source,
commit and license before committing it. See [docs/REFERENCES.md](docs/REFERENCES.md)
for which sources may be copied and which may only be read.
