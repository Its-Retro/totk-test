# Status (updated with each sync from the development tree)

Last updated: 2026-10-22.

## Goal: a steady 30 fps

**Where the time goes:**
- The render thread runs the game's own drawing code and then our NVN-to-D3D12 translation, one after the other.
- Translation costs ~5.6 µs per draw at ~3,000-5,000 draws per frame.
- Frames take ~50-75 ms. The GPU is not the limit: about 9-15 ms per frame.

**The plan, in order:**
1. **Async translation** (`TOTK_ASYNC_XLATE`): the game's next frame runs while a separate thread translates the current one.
   - Stages 1 and 2: done, tested, stage 2 on by default.
   - Stage 3: in progress. The early frame fence (the game gets its GPU feedback one frame late, as nxport does), plus a write guard on data the in-flight frame still reads.
   - Expected: about the longer of the two jobs instead of their sum, so ~40-45 ms in heavy areas and 30 fps in lighter ones.
2. **Real GPU timestamps** (`TOTK_GPU_TS=gpu`) after stage 3. With `ns` the game always picks its 20 fps cap.
3. **Cheaper or split translation** to bring heavy areas under 33 ms.

## Recently fixed (2026-10-21/22)

- **Sky Island vertex explosions:** frame overlap race, so `TOTK_OVERLAP` is now 0.
- **Black floors and objects, and black sprite squares:** the depth image switched between view and copy within one list. Fixed by `TOTK_TEX_KEEP`, which also removed 2-3 full GPU waits per frame.
- **Distance-dependent sheets on trees and terrain:** vertex format 100 was not decoded. Fixed with `TOTK_VERTEX_FORMATS=1`.
- **Memory:** a 4.4 GB command-record leak is fixed, and the texture budget is enforced every submission.
- **Data snapshot v2:** guest data the GPU reads is copied at translation time; the plan costs ~0.7-1.6 ms per frame.

## Known open items

- Async stage 3, in progress.
- BC7 texture coverage: only a small share of textures reach the transcoder yet.
- Disk use of a build:
  - read the extracted RomFS folder directly instead of packing a 17 GB image;
  - clean intermediates after a successful build (caches, generated C, objects).
- Occlusion queries are faked (`TOTK_OCCLUSION=fake`). Real ones need a report guard.
- No audio yet; frame rate first.
