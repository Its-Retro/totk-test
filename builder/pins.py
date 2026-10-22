"""Every version, download and hash the builder depends on. Hashes only; no game data."""

GAME = {
    'name': 'The Legend of Zelda: Tears of the Kingdom',
    'title_id': '0100F2C0115B6000',
    'version': '1.0.0 (base game, no update applied)',
    'main_build_id': '082ce09b06e33a123cb1e2770f5f9147709033db',
}

# ExeFS files exactly as dumped (compressed NSOs): name -> (bytes, sha256)
EXEFS = {
    'main':      (36549681, 'faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab'),
    'main.npdm': (1588,     'f5a5eecc49bbc31d6508c72f2e702fa9dcf2911efcc458d45963f1bdc2722485'),
    'rtld':      (7140,     '28317605926caa40a5cbee3e3cb290c65783b4d9f8c3697b43ca41ab2866a44d'),
    'sdk':       (6132082,  '91573d4444d0dcd07080db78fece2b098b76080d77cdb2e1d301a2a41d7c4eea'),
    'subsdk0':   (3428817,  '9d04c98997977e99c12a48a2dbfd3064b8da96b5152e0119064957a43f6c3f99'),
}
# First 20 bytes of each NSO's module id (GNU build id), used to explain a mismatch.
BUILD_IDS = {
    'main':    '082ce09b06e33a123cb1e2770f5f9147709033db',
    'rtld':    '7501abfe55fa41cffeb46bd619bdcbf45b9cae3a',
    'sdk':     'b9046c31eb5d31271be970fe732d38df49c6aa21',
    'subsdk0': 'a6bb7c6ffa9673769fa74ed3d7b054191f25d29c',
}
# After decompression (the same values translate_startup.py checks): name -> (bytes, sha256)
UNCOMPRESSED = {
    'rtld':    (13232,    '8ebd0cfabdaf5221cf0548b6e0ffb619d97bf52a1a8069ef86fedddbd6c860aa'),
    'main':    (72972944, '529c01b70ac0ecc4c6c7aed73be7f998d5e3680a5b657712409cb1bb1980133b'),
    'subsdk0': (5831672,  '69d204488f84d269f22d75c8954c2036dca13697b2a8b9085824158e92a56c47'),
    'sdk':     (13746064, '8501ee7f9d3ce3654171899e25e9b913e4d9001ca43b3c03d0da6b2751b640f0'),
}
# The base game's RomFS. The packer (romfs_pack.py) reproduces the original image byte for byte from a
# correct extracted folder, so the whole image hash checks every file's name, size and contents.
ROMFS = {
    'files': 304207,
    'dirs': 2665,                    # including the root
    'image_bytes': 17069139640,
    'image_sha256': '4751542f544d8c3406cb63ccfc68b42690ff0387228d1f0af021d1f0037dafcf',
    'metadata_sha256': '330f687bca12c3e63a090babef695204260ab147d061af98787effc35b32177a',   # names + sizes + layout
    'key_files': ['Pack/ZsDic.pack.zs', 'Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'],
}

# Toolchain, unpacked under <output>/tools/
ZIG = {
    'version': '0.16.0',
    'url': 'https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip',
    'sha256': '68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e',
    'bytes': 97217739,
    'dir': 'zig-x86_64-windows-0.16.0',          # top folder inside the zip; the scripts expect this name
}
DXC = {
    'version': 'v1.9.2609 (dxcompiler 1.9.2609.5)',
    'url': 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip',
    'sha256': 'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1',
    'bytes': 32287592,
    'dir': 'dxc',                                # the zip has no top folder: bin/x64/dxc.exe etc.
}
# Python: build.ps1 uses Python 3.12 if installed, otherwise this private copy (python.org's NuGet package).
PYTHON = {
    'version': '3.12.10',
    'url': 'https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg',
    'sha512_base64': 'u9pNz2iKlCEbYtUJaKkbOPMF0LjR7NkCafdKhvigpPzrt8oWKgdTpHaR6z3wyWQAm9PYGUxv0Zr66NX9AeHMDw==',
}
PY_PACKAGES = {'capstone': '5.0.7', 'zstandard': '0.25.0', 'numpy': '2.3.5'}   # hashes: builder/requirements.txt

# Third-party sources built into the game DLL, unpacked under <output>/startup/third_party/
OPUS = {
    'version': '1.5.2',
    'url': 'https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
    'sha256': '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1',
    'dir': 'opus-1.5.2',
}
SDL = {
    'version': '3.4.16',
    'url': 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.zip',
    'sha256': '5f399fdfbc040169ef4418a3cb99ccc5a0641681f7ccc1da66eeec3c97fa4076',
    'dir': 'SDL3-3.4.16',
}
LIBVPX = {
    'version': 'v1.15.2',
    'git': 'https://chromium.googlesource.com/webm/libvpx',
    'mirror': 'https://github.com/webmproject/libvpx',
    'tag': 'v1.15.2',
    'commit': 'd168454ecd099805c675d4a98c66f4891373302a',
    'dir': 'libvpx',
    'vendor': 'libvpx-v1.15.2.tar.gz',   # git archive of the commit above, shipped in vendor/ (no git needed)
    'vendor_sha256': 'ef95e72111d63ff475bdd5ba7be103a782d21a911d640780460a567d68f22914',
}
LIBWEBM = {
    'version': '1.0.0.32',
    'git': 'https://chromium.googlesource.com/webm/libwebm',
    'mirror': 'https://github.com/webmproject/libwebm',
    'tag': 'libwebm-1.0.0.32',
    'commit': 'f2a982d748b80586ae53b89a2e6ebbc305848b8c',
    'dir': 'libwebm',
    'vendor': 'libwebm-1.0.0.32.tar.gz',
    'vendor_sha256': '79166fc27c821f86603cea53e85fe9c62a15b09672a8a7af14c80c65aafe84e6',
}

# Disk space the build needs on the output drive (bytes), with and without packing the RomFS.
DISK_NEEDED = 60 * 10**9
DISK_NEEDED_NO_PACK = 42 * 10**9

# libvpx's run-time CPU dispatch headers, generated once by libvpx's own build/make/rtcd.pl (perl) for this exact
# configuration (x86_64, --disable-mmx --disable-avx512, build_vpx.py's vpx_config.txt) and shipped in vendor/libvpx-rtcd,
# so the build needs no perl. name -> sha256
VPX_RTCD = {
    'vp9_rtcd': '50950b34ec921b6adad0482e04cbb9b76129b65f9c222fd3beffb364bd033e1f',
    'vpx_dsp_rtcd': '2a6444ccd194492e00a9645de44e3943572839c90795fae3ee1bbd1370c26a43',
    'vpx_scale_rtcd': '14295d4093779a9fcc8c3b5dda4164f15fef46df1241645cec116acb6de2c02e',
}
