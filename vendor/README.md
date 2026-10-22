# vendor/: everything the builder needs, bundled so a build needs only your extracted game

The builder (builder/build.py, build.ps1) uses these files instead of downloading them. Each is checked against the hash pinned in builder/pins.py (Python: builder/requirements.txt hashes for the wheels, the NuGet SHA-512 for Python). All are open source; each archive contains its own licence file.

| File | What | Source | Licence | SHA-256 |
|---|---|---|---|---|
| `zig-x86_64-windows-0.16.0.zip` | zig 0.16.0 compiler | https://ziglang.org/download/0.16.0/ | MIT | `68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e` |
| `dxc_2026_09_29.zip` | DirectX Shader Compiler v1.9.2609 | https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609 | LLVM Release License (NCSA); dxil.dll under the Microsoft licence in the zip | `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1` |
| `python.3.12.10.nupkg` | Python 3.12.10 (python.org NuGet package) | https://www.nuget.org/packages/python/3.12.10 | PSF License | `0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8` |
| `opus-1.5.2.tar.gz` | libopus 1.5.2 source | https://opus-codec.org/ (xiph.org / github.com/xiph/opus) | BSD-3-Clause | `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` |
| `SDL3-3.4.16.zip` | SDL 3.4.16 source | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16 | zlib | `5f399fdfbc040169ef4418a3cb99ccc5a0641681f7ccc1da66eeec3c97fa4076` |
| `libvpx-v1.15.2.tar.gz` | libvpx v1.15.2 source (git archive of d168454e) | https://chromium.googlesource.com/webm/libvpx | BSD-3-Clause | `ef95e72111d63ff475bdd5ba7be103a782d21a911d640780460a567d68f22914` |
| `libwebm-1.0.0.32.tar.gz` | libwebm 1.0.0.32 source (git archive of f2a982d7) | https://chromium.googlesource.com/webm/libwebm | BSD-3-Clause | `79166fc27c821f86603cea53e85fe9c62a15b09672a8a7af14c80c65aafe84e6` |
| `wheels/capstone-5.0.7-py3-none-win_amd64.whl` | capstone 5.0.7 wheel | https://pypi.org/project/capstone/5.0.7/ | BSD-3-Clause | `4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1` |
| `wheels/zstandard-0.25.0-cp312-cp312-win_amd64.whl` | zstandard 0.25.0 wheel | https://pypi.org/project/zstandard/0.25.0/ | BSD-3-Clause | `ffef5a74088f1e09947aecf91011136665152e0b4b359c42be3373897fb39b01` |
| `wheels/numpy-2.3.5-cp312-cp312-win_amd64.whl` | numpy 2.3.5 wheel | https://pypi.org/project/numpy/2.3.5/ | BSD-3-Clause (bundled libraries: see the wheel) | `86945f2ee6d10cdfd67bcb4069c1662dd711f7e2a4343db5cecec06b87cf31aa` |
| `libvpx-rtcd/vp9_rtcd.h.raw` | libvpx v1.15.2 source (git archive of d168454e) | https://chromium.googlesource.com/webm/libvpx | BSD-3-Clause | `50950b34ec921b6adad0482e04cbb9b76129b65f9c222fd3beffb364bd033e1f` |
| `libvpx-rtcd/vpx_dsp_rtcd.h.raw` | libvpx v1.15.2 source (git archive of d168454e) | https://chromium.googlesource.com/webm/libvpx | BSD-3-Clause | `2a6444ccd194492e00a9645de44e3943572839c90795fae3ee1bbd1370c26a43` |
| `libvpx-rtcd/vpx_scale_rtcd.h.raw` | libvpx v1.15.2 source (git archive of d168454e) | https://chromium.googlesource.com/webm/libvpx | BSD-3-Clause | `14295d4093779a9fcc8c3b5dda4164f15fef46df1241645cec116acb6de2c02e` |
