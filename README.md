# totk-test

**An early, private test of a native Windows PC port of The Legend of Zelda: Tears of the Kingdom.**

This is not an emulator. The builder in this repository reads the game files *you* dumped from *your own* Switch and turns them into a Windows program on your PC: it translates the game's ARM64 code into C, translates its GPU shaders to DirectX 12, and compiles the result. This repository is the Windows / x86-64 side of [nxport](https://github.com/nxporting/nxport).

> **Please read first**
> - This is an early private test. Expect crashes, missing effects, low frame rates and no sound.
> - **This repository contains no Nintendo files, and the builder downloads none.** You need your own legally dumped copy of the game.
> - Everything the builder makes from your game files ends up in the `output` folder. It contains material made from Nintendo's game, so **never share it, upload it or commit it**. Please don't share this repository either.

---

## What you need

| | |
|---|---|
| **The game** | Your own dump of *The Legend of Zelda: Tears of the Kingdom*, **base game version 1.0.0 with no update applied** (title ID `0100F2C0115B6000`, main build ID `082ce09b06e33a123cb1e2770f5f9147709033db`). Updates (1.1.x, 1.2.x) and DLC are not supported yet. |
| **From the dump** | The **ExeFS** files `main`, `main.npdm`, `rtld`, `sdk` and `subsdk0` (other files such as `subsdk1`... are ignored), and the **RomFS**: either the extracted folder (304,207 files in 2,664 folders, about 17.1 GB, with `Pack`, `Shader`, `Effect`... inside) or a single RomFS image file (`romfs.bin`). Extract them with your usual tool and your own keys. |
| **Windows** | Windows 10 (version 2004 or newer) or Windows 11, 64-bit. Windows on ARM does not work. |
| **CPU** | 64-bit Intel or AMD with AVX2 (Intel 4th-generation Core "Haswell" or newer, any AMD Ryzen). |
| **GPU** | DirectX 12 with Shader Model 6.6 and an up-to-date driver; 8 GB of video memory recommended (see `-TextureBudgetMB` below for less). Only an NVIDIA RTX 4070 Laptop GPU has been tested. |
| **RAM** | 16 GB minimum, with Windows' automatic page file on a drive with plenty of free space. 32 GB recommended. The build itself is the memory-hungry part; the game uses 3.5-7 GB. |
| **Disk** | About **60 GB free** on the drive that holds the output folder (17 GB for the RomFS image, 4 GB of generated C, up to 10 GB of translator caches, several GB of compiled objects and shaders, about 1 GB for the game itself). About 42 GB if you give a RomFS image file on the same drive. An SSD helps a lot. |
| **Software** | [Git for Windows](https://git-scm.com/download/win) (the builder uses its `git` and `perl`). Python 3.12 is used if you have it; otherwise the builder downloads a private copy into the output folder. Nothing is installed system-wide and no admin rights are needed. |
| **Internet** | For the first run only: zig 0.16.0 (97 MB), the DirectX Shader Compiler (32 MB), Python 3.12 if needed (15 MB), three Python packages and the sources of libopus, SDL 3, libvpx and libwebm. Every download is pinned to an exact version and checked against its SHA-256 (or git commit) before use. |
| **Time** | About 3-5 hours for the first build on a 16-thread laptop. The PC stays usable (the builder runs at low priority) but will be busy. Later runs only redo what changed. |

## Build it (one command)

1. Get the repository (for example `git clone` it), and open a Command Prompt in its folder.
2. Optional, but recommended: check your game files first (a few minutes, writes nothing but logs):
   ```bat
   build.cmd -ExeFS "D:\dumps\totk\exefs" -RomFS "D:\dumps\totk\romfs" -CheckInputs
   ```
3. Build:
   ```bat
   build.cmd -ExeFS "D:\dumps\totk\exefs" -RomFS "D:\dumps\totk\romfs"
   ```

Use your own paths. `-RomFS` can also point at a RomFS image file. You can stop the build at any time (Ctrl+C or close the window) and run the same command again later: steps that already finished are skipped.

| Option | What it does |
|---|---|
| `-OutputDir C:\totk-output` | Put the output folder somewhere else (default: `output` inside the repository). Keep the path short: Windows has a 260-character path limit. |
| `-Workers 4` | How many jobs run at once (default: half your CPU threads, at most 8; 4 or fewer with less than 16 GB of RAM). Lower it if the PC runs out of memory. |
| `-CheckInputs` | Only check your PC and your game files, then stop. |
| `-From compile` | Redo one step and everything after it (step names: `check-pc`, `sources`, `toolchain`, `libraries`, `exefs`, `decompress`, `romfs`, `headers`, `translate`, `shaders`, `compile`, `launcher`, `play`). |
| `-Only shaders` | Run just one step. |
| `-DryRun` | Show the plan without doing anything. |

### What the builder does

1. **Checks your PC**: Windows version, CPU (AVX2), RAM, free disk space, GPU.
2. **Copies the source code** into the output folder (the repository itself is never changed).
3. **Gets the compilers**: zig 0.16.0 and DXC v1.9.2609 (downloaded and hash-checked once), and finds Git and Perl.
4. **Gets the open-source libraries**: libopus 1.5.2, SDL 3.4.16, libvpx 1.15.2 and libwebm 1.0.0.32, at pinned versions.
5. **Checks your ExeFS files** against the base game's exact SHA-256 hashes. A wrong version, an update or a damaged file is explained in plain words.
6. **Decompresses the game code**: the ExeFS modules are LZ4-compressed NSO files; the builder unpacks them itself (no keys involved) and checks the result.
7. **Prepares your RomFS**: packs the extracted folder into one image file, the form the game reads, or links your image file in. The result must be identical, byte for byte, to the base game's RomFS, which also proves every file is intact.
8. **Writes the headers made from your game files** (service list, NVN function names, driver format tables).
9. **Translates the game's ARM64 code to C** (about 13 million instructions, 3.7 GB of C, about 45 minutes).
10. **Translates the game's shaders** from the Switch GPU's machine code to HLSL and compiles them to DirectX 12 DXIL (about 22,000 shaders, 30-90 minutes), then builds the geometry shaders that draw point sprites.
11. **Compiles and links the game** into `totk-play.dll` (13,265 compile units; about 1.5-3 hours).
12. **Builds the launcher** `totk.exe`.
13. **Writes the start script** `play.cmd`.

Each step writes its log to `output\logs\<step>.log`. If something fails, the builder stops with a short explanation and the last lines of that log.

## Where everything goes

Everything generated goes into the output folder (`output\` unless you chose another one); git ignores it. You can delete it at any time to start over.

```
output\
  play.cmd                  start the game (double-click)
  startup\totk.exe          the launcher
  startup\totk-play.dll     the game, compiled for your PC (about 1 GB)
  startup\shader-hlsl\      your translated shaders
  startup\save-data\        your saves (made on first start)
  assets\romfs.bin          your RomFS as one image
  exefs\, uncompressed\     your ExeFS files and the unpacked modules
  tools\, downloads\        zig, DXC and (if needed) Python
  logs\                     build and play logs
```

## Play

Double-click **`output\play.cmd`**.

- The first start takes a while. With no save, the game goes straight into the opening (under Hyrule Castle); there is no title screen to press through.
- **Controls**: any gamepad SDL knows (Xbox, PlayStation, Switch Pro and others) works, mapped by button position like a Pro Controller. Keyboard (with the game window focused): `WASD` move, arrows = d-pad, `K` `J` `I` `U` = A B X Y, `Q` / `E` = L / R, `1` / `3` = ZL / ZR, `Enter` / `Tab` = + / -, numpad `8` `4` `5` `6` = camera.
- **Saves** are in `output\startup\save-data\0100F2C0115B6000\`. Copy that folder to back them up.
- Close the game window to quit.
- `play.cmd -TextureBudgetMB 2048` lowers the texture memory budget (default 4096) for GPUs with less video memory. `play.cmd -NoShaderWatcher` turns off the background shader translation described below.

**Shaders met for the first time.** Some shaders only show up while the game runs. The first time one is needed, the game skips that effect and saves the shader to `output\missing-shaders\`; a helper started by `play.cmd` translates it in the background, and the effect appears a moment later and stays fixed from then on. These files are made from the game: keep them to yourself, don't share them.

## Known issues (as of this commit)

- **Early test.** The opening (the prologue under Hyrule Castle) is what is being tested. The open world after it starts, but is slow and freezes a lot.
- **Frame rate**: about 10-15 fps on the RTX 4070 Laptop GPU it was developed on; expect less on slower PCs.
- **No sound at all**, on purpose for now. Cutscene movies play without audio.
- **Stutters** the first time new things appear, while their GPU pipelines are compiled. A pipeline cache (`output\startup\pso-cache-*.bin`) makes later visits smoother.
- **Missing or flickering effects** the first time they appear (see "Shaders met for the first time").
- Some visual defects remain: blocky lighting near Link, objects popping in and out during cutscenes, missing sunlight in places.
- Only the base game 1.0.0. No updates, DLC or mods.
- Only tested on one PC: Intel Core i7-12650H, 16 GB RAM, NVIDIA RTX 4070 Laptop GPU, Windows 11.

### Not yet tested end to end

Each part of the pipeline has been run in the development tree. The builder's own light steps were tested (checking real game files, the error messages, unpacking the pinned archives, compiling the helper shaders and the launcher, generating the headers). The following has **not** yet been run from a fresh clone, so the first friend builds are also the test of the builder:

- Downloading zig, DXC, Python and the libraries, and `build.ps1` finding or fetching Python and installing the packages.
- Packing a real extracted RomFS folder. The packer reproduces the base game's RomFS layout exactly (checked byte for byte against the original image's tables) and round-trips a small test folder, but a full 17 GB pack has not been run.
- The C translation, DLL compile and shader steps as the builder runs them in a fresh output folder (same commands as development), including the new parallel translation of the scanned shaders and the shader manifest with relative paths.
- The point-sprite geometry shader step (`make_point_gs.py`) as the builder runs it, on a manifest with relative paths. It is optional: if it fails, the build continues.
- The GPU macro table is built empty (the real one comes from a run-time capture). The native renderer should never need it, but no build with the empty table has been started yet.
- `play.cmd` and the background shader helper.
- A first start with no save folder.
- Any PC other than the development PC.

## If something goes wrong

- **"does not match ... different version"**: you dumped an updated game. Dump the base game (from the cartridge, or the base NSP) without the update.
- **"has the right build id but different bytes"** or **"contents of at least one file differ"**: a damaged or modified file. Dump or extract again.
- **"Fewer files than the base game"**: the RomFS extraction did not finish. Extract it again.
- **Not enough disk space / path too long**: use `-OutputDir` on another drive with a short path, such as `C:\totk-output`.
- **The PC runs out of memory during the build**: run again with `-Workers 2`; it resumes where it stopped.
- **The game stops**: `play.cmd` prints the stop line. Please send the files it names (`output\logs\play-*-stderr.log` and `output\totk-stop.txt`). Never send anything else from the output folder.

## Legal

This project is not affiliated with or endorsed by Nintendo. *The Legend of Zelda: Tears of the Kingdom* is © Nintendo. You must own the game and make your own dump; this repository contains no game code, data or keys, and the builder only uses files you give it. The output folder holds material made from your copy, for your own use only.

The source code here is MIT-licensed (`LICENSE`). Third-party code and libraries are listed in `THIRD-PARTY-NOTICES.md`. `docs/` has background for developers: `HISTORY.md` (the development log) and `NVN-DESIGN.md` (the native NVN graphics design).
