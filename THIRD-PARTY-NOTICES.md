# Third-party notices

This repository is MIT-licensed (see `LICENSE`). It is the Windows / x86-64 side of nxport. Parts of it are adapted from the open-source projects below; each place in our source that uses one of them carries a comment naming the project and the file it came from.

**No Nintendo material is included.** The game's code, data, shaders and tables are read from the user's own dump at build time, and everything made from them stays in the user's `output` folder, which git ignores.

**Reference-only projects (no code copied).** Suyu (GPL-3.0), yuzu (GPL-2.0-or-later) and Ryujinx were used as references for how the hardware and the system software behave. Our source cites them in comments where it matches a behaviour they document. No code from Suyu or yuzu is included. The one file whose logic comes from Ryujinx (by way of nxport's Rust port of it) is listed under Ryujinx below. Hardware and format references used the same way: the NVIDIA Tegra X1 TRM, NVIDIA open-gpu-doc class headers (MIT), deko3d (zlib), libnx (ISC), switchbrew and envytools documentation.

---

## Code adapted into this repository

### nxport
https://github.com/nxporting/nxport. Ideas and logic adapted in `startup/translate_game_shaders.py` (early depth/stencil flag, position invariance), `startup/maxwell_hlsl.py` (loop guard), `startup/shader-tools/extract_early_z.py`, `startup/host_nvn_exec.h`, `startup/host_nvn_play.h`, `startup/host_nvn_overlap.h` and `startup/host_audren_voices.h` (audio renderer voice progress: the wave-buffer and play-state logic of nxport's `crates/engine/src/horizon/audio/renderer.rs`).

```
MIT License

Copyright (c) 2026

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

### Ryujinx
https://github.com/Ryujinx/Ryujinx. `startup/host_audren_voices.h` follows Ryujinx's audio renderer server (`Ryujinx.Audio/Renderer/Server`: `VoiceInfo`, `VoiceUpdateState`, `Dsp/DataSourceHelper`) through nxport's port of it (see nxport above): how voices take wave buffers, play, loop, flush and stop, and how far a voice advances in one renderer frame. Only that sample accounting is used; no decoding, resampling or mixing.

```
MIT License

Copyright (c) Ryujinx Team and Contributors

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

### LASRecomp
https://github.com/peiikai/LASRecomp. Only LAS Recomp's own code is covered here; its `third_party/` components keep their own licences, and none are used unless listed separately below. Used in `startup/host_movie.h` (movie player design), `startup/host_gpu_render.h` and `startup/host_gpu_render_draw.h` (render-target and output-mask handling), `startup/host_nvn_play.h`.

```
MIT License

Copyright (c) 2026 Peiikai

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

### SuperMarioWonderRecomp
https://github.com/BelmanteGu/SuperMarioWonderRecomp. Used in `startup/host_gpu_render.h` (linear-content check, GPU-authoritative render targets) and `startup/host_nvn_play.h` (present pacing).

```
MIT License

Copyright (c) 2026 BelmanteGu

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

### envytools
https://github.com/envytools/envytools. `startup/maxwell_isa.py` transcribes the Maxwell (gm107) instruction tables of `envydis/gm107.c`. `startup/build-envydis.ps1` can build envydis from a local checkout for the optional shader statistics.

```
Copyright (C) 2013 Marcelina Kościelnicka
Copyright (C) 2013 Ben Skeggs
Copyright (C) 2013 Martin Peres
Copyright (C) 2013 Maarten Lankhorst
Copyright (C) 2013 Roy Spliet
Copyright (C) 2013 Christoph Bumiller
Copyright (C) 2013 Marcin Ślusarz
Copyright (C) 2013 Emil Velikov
Copyright (C) 2013 Francisco Jerez

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## Libraries built into the game DLL

These are not in this repository. The builder downloads each one at a pinned version (checked by SHA-256 or git commit, see `builder/pins.py`) into `output/startup/third_party/` and compiles it into `totk-play.dll`. Their full licence texts are in those downloaded folders.

### libvpx (v1.15.2)
https://chromium.googlesource.com/webm/libvpx. VP9 decoder only, for cutscene movies (built by `startup/third_party/build_vpx.py`, used through `startup/host_movie_vpx.c`). BSD 3-clause licence, with the additional patent grant in `libvpx/PATENTS`.

```
Copyright (c) 2010, The WebM Project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google, nor the WebM Project, nor the names
    of its contributors may be used to endorse or promote products
    derived from this software without specific prior written
    permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libwebm (1.0.0.32)
https://chromium.googlesource.com/webm/libwebm. mkvparser only (built by `startup/third_party/build_webm.py`, used through `startup/host_movie_webm.cpp`). BSD 3-clause licence, with the additional patent grant in `libwebm/PATENTS.TXT`.

```
Copyright (c) 2010, Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libopus (1.5.2)
https://opus-codec.org. Built by `startup/third_party/build_opus.py` for the hwopus service. BSD 3-clause licence; Opus is also subject to the royalty-free patent licences listed in its `COPYING` file.

```
Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
                    Jean-Marc Valin, Timothy B. Terriberry,
                    CSIRO, Gregory Maxwell, Mark Borgerding,
                    Erik de Castro Lopo, Mozilla, Amazon

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### SDL 3 (3.4.16)
https://www.libsdl.org. Gamepad input only (built by `startup/third_party/build_sdl.py`, used through `startup/host_input_sdl.c`). zlib licence; the full text, with the author's contact address, is in the downloaded `SDL3-3.4.16/LICENSE.txt`.

```
Copyright (C) 1997-2026 Sam Lantinga

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

---

## Build tools (downloaded by the builder, not part of the game)

| Tool | Version | Licence |
|---|---|---|
| Zig (used as the C/C++ compiler and linker) | 0.16.0 | MIT (bundled LLVM/Clang: Apache-2.0 with LLVM exception; mingw-w64 headers and libraries: see the Zig distribution) |
| DirectX Shader Compiler (DXC) | v1.9.2609 | University of Illinois/NCSA and MIT (see the `LICENSE-*.txt` files in the download) |
| Python | 3.12 | PSF License |
| capstone | 5.0.7 | BSD 3-clause |
| python-zstandard | 0.25.0 | BSD 3-clause |
| NumPy | 2.3.5 | BSD 3-clause |
| Git for Windows (git and perl) | user-installed | GPL-2.0 (used as separate programs only) |
