# CLAUDE.md: working on totk-test

A native Windows x86-64 port of *The Legend of Zelda: Tears of the Kingdom* (base game 1.0.0). The game's ARM64 code is translated ahead of time to C and compiled into a DLL. NVN, the Switch graphics API, is reimplemented natively on Direct3D 12, and the game's Maxwell shaders are translated to HLSL/DXIL. This is the Windows side of [nxport](https://github.com/nxporting/nxport) (MIT; Apple Silicon TOTK). The development tree lives on the project owner's PC (Its-Retro); this repository is synced from it.

## Hard rules

- **No Nintendo files, ever.** Never commit anything derived from the game: generated C, translated or dumped shaders, shader-label tables, inventories built from the game, captures, frame dumps, saves, keys, title keys or decrypted data. The builder makes all of that on the player's own PC from their own dump. `.gitignore` covers the output folder; check `git status` before every commit.
- **The builder needs only the extracted game.** Everything else ships in `vendor/` (zig, DXC, Python + wheels, library sources, libvpx headers), each checked against the hashes in `builder/pins.py`. Don't add downloads, Git or Perl requirements.
- **Native only.** No emulation and no interpreters. NVN is replaced at the API level. **Never decode or replay Maxwell command buffers**, and never skip or merge GPU work to gain speed.
- **No GPL code.** Suyu and yuzu are not used, not even as citations. Ryujinx (MIT), Mesa (MIT), LASRecomp and SuperMarioWonderRecomp (MIT) may be cited or adapted with credit in `THIRD-PARTY-NOTICES.md`.
- **Pin and credit.** New third-party code or tools: permissive licence only, exact version, SHA-256 in `builder/pins.py`, and an entry in the notices.

## How changes flow (important)

- `main` is updated from the development tree by a three-way merge, so a direct commit to `main` can be overwritten by the next sync.
- **Work on a branch and open a pull request.** The owner merges it into the development tree first, and the next sync carries it back to `main`.
- Three files carry release-only edits that the sync keeps; leave them as they are:
  - `startup/host_nvn.h` and `startup/host_nvn_exec.h`: NVN format tables come from the builder's `generated_nvn_format_*.h`;
  - `startup/host_gpu_render.h`: repo-relative comment paths.
- Check `docs/STATUS.md` first: it says what is in progress, so two people don't redo or undo the same work.

## Build and play it yourself (Claude: you do this, the user shouldn't have to)

You can build and launch the game for the user. Nothing needs installing: `vendor/` has every tool, and the builder never prompts.

1. **The dump paths.** The first time, ask the user where their own extracted ExeFS folder (with `main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`) and RomFS (the extracted folder or a `romfs.bin`) are. Write them to `local\build-inputs.txt`; `local/` is git-ignored. Read them from there next time instead of asking again.
2. **Check the inputs** (seconds): `build.cmd -ExeFS <exefs> -RomFS <romfs> -CheckInputs`. It confirms the base game 1.0.0 with no update, and explains any mismatch in plain words.
3. **Build:** `build.cmd -ExeFS <exefs> -RomFS <romfs>`.
   - Run it **in the background**: the first build takes about 3-5 hours, and later builds after a `git pull` only redo what changed.
   - Follow progress in the console's heartbeat lines and in `output\logs\<step>.log`.
   - If it stops, the message says why. Fix it and run the same command again: finished steps are skipped.
   - `-From <step>` redoes a step and everything after it; `-DryRun` shows the plan.
4. **Play:** start `output\play.cmd` **in the background**, since it waits until the game window closes. Then read the logs below.
5. On a 16 GB PC, don't run a build and the game at the same time.
6. After the user pulls new commits: run the same build command again (step 3), then play.

## Logs
- Logs from a play session, all in the output folder:
  - `logs\play-<time>-stderr.log`: frame pacing ("present pacing: ... interval N" = the game's frame-rate cap, 2 = 30 fps, 3 = 20 fps), crashes;
  - `draw-path.log`: renderer counters every 300 presents: "render draw cost" (µs per draw by phase), "data-snapshot frame", "nvn native: async-xlate" (async translation), "blacktex", texture budget;
  - `frames\`: present timing CSVs and occasional frame dumps.
- `draw-path.log` grows large. Read it with grep/tail, never load it whole, and keep tools well under 1-2 GB of RAM (the main test PC has 16 GB).

## Settings (environment variables; `play.ps1` sets the defaults)

Every `TOTK_*` switch is read once at start. A value already set in the shell wins over the default.

| Switch | Default | Meaning |
|---|---|---|
| `TOTK_OVERLAP` | 0 | 1 overlaps game and GPU frames; that caused vertex explosions (the game rewrites data the GPU still reads) |
| `TOTK_ASYNC_XLATE` | 2 | async translation: 0 old path, 1 private translation thread with the render thread waiting, 2 submit returns after the capture (frame fence still full), 3 early frame fence (in development) |
| `TOTK_ASYNC_XLATE_CHECK` | off | 1: sentinel self-check of the async capture (for tests) |
| `TOTK_DATA_SNAPSHOT_PLAN` | 1 | snapshot plan skip (~3 ms/frame); 0 walks every slot; 2 self-check |
| `TOTK_GPU_TS` | ns | GPU timestamps the game uses for dynamic resolution and its 30/20 fps cap: `ns` holds detail lower, `gpu` = real GPU time (planned once async translation is done) |
| `TOTK_VERTEX_FORMATS` | 1 | decode vertex format 100 (0 stretched distant tree/terrain cards) |
| `TOTK_OCCLUSION` | fake | occlusion queries answer "visible" (real answers arrive late) |
| `TOTK_TEX_LIVE_TRIM` / `TOTK_TEX_MIN_AGE` | 1 / 120 | keep textures within the budget every submission |
| `TOTK_SLOT_VERSIONING` | 0 | staged texture-slot updates (TOTK_TEX_KEEP already removes the GPU waits) |
| `TOTK_BC7` | off | 1: ASTC textures re-packed to BC7 on the GPU (stage 1, partial coverage) |

Most fixes have their own off switch (`TOTK_TEX_KEEP`, `TOTK_STORE_LOCK_V2`, `TOTK_DATA_SNAPSHOT`...). The comment beside each switch in the code says what 0 restores.

## Debugging rules of thumb

- **Steady broken geometry** (it appears and disappears with distance or LOD) is a decoding or translation bug. **Flicker or flashing** is a timing race.
- Test a speed change at a fixed spot with nothing else heavy running; route and draw count dominate session averages.
- Before designing a fix in rendering, memory or sync, check how nxport, LASRecomp and SMW solved it.
- Explosions and flashing are unpleasant to watch: test risky visual changes in a hidden boot first and keep a kill switch.

## Code style

Match the surrounding code: dense comments that explain *why*, `boot_*` / `boot_render_*` / `boot_nvn_*` naming, environment switches read once into a static. Runtime headers use CRLF line endings; keep them exactly (`.gitattributes` has `* -text`).
