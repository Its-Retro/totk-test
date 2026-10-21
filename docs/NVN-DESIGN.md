# Native graphics path: replace NVN at the API level

Status: research and design only. Nothing in the live tree has been changed.
Author: gpu4 agent, 2026-10-15. Data and scripts are in `nvn-work/` (see "Reproducing the inventory").

## 0. Goal and principle

Today the game's NVN driver (AOT-translated code from the `sdk` module) writes Maxwell
pushbuffers and GPFIFO entries. Our host code in `host_gpu_*.h` then decodes those methods,
runs the GPU macro programs, and records D3D12 work from them. That is GPU emulation.

The target is different. Every NVN API function the game calls becomes a host C function
that records D3D12 work directly. The NVN driver in `sdk` would never run, so no Maxwell
command buffers, GPFIFO, macros, TIC/TSC words or QMDs would exist anywhere.

Code we keep is code that works at the API level:
- the AOT shader translation (`shader-manifest.tsv` → DXIL, keyed by the hash of the guest program bytes);
- the D3D12 device, heap and descriptor code;
- texture format tables and detile code (still needed for pixel data the game hands over in Maxwell layouts);
- the render-target, texture and PSO caches;
- the present path.

## 1. NVN entry points the game uses (inventory)

### Method

This is static analysis of the decompressed modules. The relocated images come from
`call-chain/module_loader.py`, with bases main 0x7110000000, subsdk0 0x7120000000 and sdk 0x7130000000.

1. **Bootstrap.**
   - `main+0xe9d510` calls the imported `nvnBootstrapLoader` (PLT `main+0x2aa51e0`). The import resolves to the sdk export at `sdk+0x4900b0`.
   - It is called with `"nvnDeviceInitialize"` and `"nvnDeviceGetProcAddress"`.
   - Internally the sdk bootstrap jumps through a driver interface pointer, so the sdk's own name table is not a plain pointer array. We don't need it.
2. **Proc loader.** The game's loader (`nvnLoadCProcs` pattern) is `main+0x2146180 .. main+0x214a000`.
   - For each name it does `x0 = getProcAddress(device, "nvnXxx")`, then `str x0, [GOT[k]]`.
   - It resolves **534 names** into 534 globals at `main+0x4597d98...` (one pointer each, reached through the GOT at `main+0x45409a0...`).
3. **Call sites.** We scanned main `.text` for `adrp`+`ldr` pairs that load one of those GOT slots outside the loader.
   - **270 of the 534 functions** are referenced by game code, from **12,437 call sites** (many are inlined sampler and vertex-state setup).
   - 264 are never referenced.
   - All 270 have call sites inside `discovered-main-ranges.json`, our translated coverage. Coverage includes statically discovered neighbourhoods, so "covered" does not mean "executed at runtime"; stage 0 below gives the runtime list.
4. **Outside the loader table:**
   - Graphics init imports from main into sdk: `nv::SetGraphicsAllocator`, `nv::InitializeGraphics` (and `nvnPrivate0`).
   - `subsdk0` is the multimedia/NVDEC library. It resolves 5 NVX interop names itself: `nvnDeviceGetProcAddress`, `nvnTextureGetNvRmSurfaceNVX`, `nvnSyncGetNvRmSyncNVX`, `nvnSyncInitializeFromNvRmSyncNVX`, `nvnSyncFinalize`. It needs only video playback (stage 4).

### Where the data is

| File | Contents |
|---|---|
| `nvn-entry-points.json` | `executed` (the 270, named that way in the file), `referenced_only` (empty), `unreferenced` (264), `site_counts`, `covered_site_counts`, and the `global` and `got_slot` address of every name |
| `nvn-main-usage.json` | Per-name call-site addresses in main |

### Count per object family

The figures are referenced / in the table. The number in brackets after a function is its call-site count.

| Family | Used | Functions |
|---|---|---|
| CommandBuffer | 81/156 | `AddCommandMemory`, `AddControlMemory`, `Barrier`(117), `BeginRecording`, `EndRecording`, `CallCommands`(19), `Bind{BlendState, ChannelMaskState, ColorState, DepthStencilState, MultisampleState, PolygonState, VertexAttribState(167), VertexStreamState(171), Program, Texture(142), TexturesDeferred, Image, StorageBuffer, UniformBuffer(62), VertexBuffer(92)}`, `UpdateUniformBuffer`(116), `Clear{Color, DepthStencil, Buffer, Texture}`, `Copy{BufferToTexture, TextureToBuffer, TextureToTexture}`, `Downsample`, `Draw{Arrays, ArraysInstanced, Elements(131), ElementsBaseVertex, ElementsInstanced, ElementsIndirect}`, `DispatchCompute`(13), `DispatchComputeIndirect`, `ReportCounter`(46), `ReportValue`(21), `ResetCounter`, `SetRenderEnable`(32), `SetRenderEnableConditional`(25), `SetRenderTargets`, `SetViewport(s)`, `SetScissor(s)`, `SetDepthRange(s)`, `SetDepthBounds`, `SetDepthClamp`, `SetStencil{Mask, Ref, ValueMask}`, `SetBlendColor`, `SetAlphaRef`, `SetLineWidth`, `SetPointSize`, `SetPatchSize`, `SetPolygonOffsetClamp`, `SetPrimitiveRestart`, `SetRasterizerDiscard`, `SetSampleMask`, `SetConservativeRasterEnable`, `SetColorReduction{Enable, Thresholds}`, `SetTexturePool`, `SetSamplerPool`, `SetShaderScratchMemory`, `SetTiledCache{Action, TileSize}`, `SetMemoryCallback(+Data, CommandMemoryCallbackEnabled)`, `Get{Command, Control}Memory{Used, Free}`, `Initialize`, `Finalize` |
| Texture / TexturePool | 22/39 | `Initialize`, `Finalize`, getters (`Width`, `Height`, `Depth`, `Format`, `Flags`, `Levels`, `Samples`, `Stride`, `Swizzle`, `Target`, `MemoryPool`, `MemoryOffset`, `ViewOffset`), `Write/Read/Flush/InvalidateTexels`, `SetDebugLabel`, `TexturePoolInitialize`, `TexturePoolRegisterTexture`(13) |
| TextureBuilder | 22/44 | `SetDefaults`, `SetDevice`, `SetTarget`, `SetFormat`, `SetSize2D`, `SetWidth`, `SetHeight`, `SetDepth`, `SetLevels`, `SetSamples`, `SetFlags`, `SetSwizzle`, `SetDepthStencilMode`, `SetStride`, `SetStorage`, `SetPackagedTextureData/Layout`, `GetStorageSize/Alignment/Class`, `GetZCullStorageSize`, `GetFlags` |
| TextureView | 10/14 | `SetDefaults`, `SetTarget`, `SetFormat`, `SetLevels`, `SetLayers`, `SetSwizzle`, `SetDepthStencilMode`, getters |
| SamplerBuilder / Sampler / SamplerPool | 22/25, 14/23 | `Set{Defaults, MinMagFilter, WrapMode, LodBias, LodClamp, LodSnap, Compare, MaxAnisotropy, ReductionFilter, BorderColor(i/ui)}`, matching getters, `SamplerInitialize/Finalize`, `SamplerPoolInitialize`, `SamplerPoolRegisterSampler` |
| MemoryPool(+Builder) | 9/10, 4/8 | `Initialize`, `Finalize`, `Map`, `MapVirtual`, `GetFlags`(185), `GetBufferAddress`, `Flush/InvalidateMappedRange`(175/39), builder `SetDefaults/Device/Flags/Storage` |
| Buffer(+Builder) | 7/11, 3/7 | `Initialize`, `Finalize`, `GetAddress`(222), `Map`, `Flush/InvalidateMappedRange`, builder `SetDefaults/Device/Storage` |
| Program | 4/8 | `Initialize`, `Finalize`, `SetShaders`, `SetShadersExt` |
| State objects | 29 total | BlendState 4, ChannelMaskState 2, ColorState 4, DepthStencilState 7, MultisampleState 5, PolygonState 5, VertexAttribState 3, VertexStreamState 3 |
| Device(+Builder) | 9/32, 2/3 | `GetInteger`, `GetTextureHandle`(28), `GetImageHandle`, `GetTimestampInNanoseconds`, `RegisterFastClearColor/Depth`, `SetDepthMode`, `SetWindowOriginMode`, `InstallDebugCallback` (`nvnDeviceInitialize` comes from the bootstrap) |
| Queue(+Builder) | 7/14, 8/19 | `Initialize`, `SubmitCommands`, `PresentTexture`, `FenceSync`, `Flush`, `Finish`, `GetError`, builder size and memory setters |
| Sync | 2/6 | `Initialize`, `Wait` |
| Window(+Builder) | 5/12, 6/12 | `Initialize`, `AcquireTexture`, `SetCrop`, `SetNumActiveTextures`, `SetPresentInterval`, builder setters |
| Event(+Builder) | 0/11 | unused |

**Total to implement: 270 API functions, plus the 2 bootstrap names, the 2 sdk init functions, and later the 5 subsdk0 NVX names.**
About a third are trivial: setters and getters on builders and state objects.

## 2. Native implementation strategy per family

These rules hold throughout:
- **Object state lives in the guest object.** Each NVN object's state is stored inside its own guest storage. NVN objects are opaque fixed-size structs allocated by the game (sizes to confirm in stage 0, see section 4).
  - Builders and state objects are copyable values in NVN, so their guest bytes must be self-contained.
  - Heavier objects (Texture, Sampler, Buffer, MemoryPool, Program, CommandBuffer, Queue, Sync, Window) also carry a host handle: a magic plus an index into a host table.
  - Getters read the guest bytes. Host caches are keyed by contents, so a copied object still works.
- **No CPU waits except where NVN defines them.** CPU waits happen only at `nvnSyncWait`, `nvnQueueFinish`, `nvnWindowAcquireTexture` (present pacing) and `Map`/`InvalidateMappedRange` of memory written by the GPU. In particular there is no wait after compute dispatches. Inside a submission the GPU orders its own work, which removes the biggest stall of the emulated path (11-15% of the main thread).

| NVN | Native D3D12 implementation | Reuse |
|---|---|---|
| `nv::SetGraphicsAllocator`, `nv::InitializeGraphics` | Record the allocator callbacks; nothing else, since there is no driver to initialise. | — |
| `DeviceBuilder*`, `DeviceInitialize` | `BootGpu` device, root signature, descriptor heaps. | `host_gpu.h`, `boot_render_root_signature` |
| `DeviceGetInteger` | Return NVN capability values: alignments (uniform buffer 256, texture/sampler descriptor size, pool page size 4 KiB), API version, limits. Values are captured once from the real driver in stage 0 and stored as a table. | — |
| `DeviceGetTextureHandle(tex, smp)`, `GetImageHandle` | Same encoding the translated shaders already decode, `tex \| smp << 20` (verify in stage 0). Shaders index the per-dimension descriptor tables with it. | `maxwell_runtime.hlsli` bindless path |
| `MemoryPoolBuilder*`, `MemoryPoolInitialize` | The pool's guest storage becomes one zero-copy D3D12 region (`OpenExistingHeapFromAddress`, as `boot_draw_memory` does today). Allocate a GPU VA range for the pool and enter it in the shader page table (`ptable`, which maps 64 KiB pages to arena offsets). Flags: CPU_NO_ACCESS, CPU_UNCACHED/CACHED, GPU_CACHED, SHADER_CODE, COMPRESSIBLE, VIRTUAL/PHYSICAL. | `host_gpu_draw_exec.h` region/arena code, `boot_render_map` |
| `MemoryPoolMap`, `GetBufferAddress`, `GetFlags` | Map returns the guest storage pointer. Buffer address is pool VA + offset. | — |
| `MemoryPool{Flush,Invalidate}MappedRange` | **Flush**: the CPU wrote the range. Mark textures and caches over it dirty (texture rebuild, cb page epochs). **Invalidate**: the CPU will read it. Wait for the fence of the last GPU work that wrote the pool, if not already complete. | `boot_render_invalidate`, `boot_render_cb_written`, fence ranges from gpu3 |
| `MemoryPoolMapVirtual` (sparse) | Reserved resources and `UpdateTileMappings`, or a page-table remap for buffer access. Stage 3. | — |
| `BufferBuilder*`, `BufferInitialize`, `GetAddress`, `Map` | A buffer is (pool, offset, size). GPU address = pool VA + offset. No D3D12 object; views are made at bind time. | — |
| `TextureBuilder*`, `TextureInitialize` | Compute size, alignment and storage class the way NVN does (the game uses `GetStorageSize`/`Alignment` to sub-allocate pools, so these values must match the real driver; tables captured in stage 0). Create the D3D12 texture:<br>• **GPU-only pools:** a placed resource in a per-pool DEFAULT heap at the same offset, so TOTK's memory aliasing of render targets becomes D3D12 aliasing (with aliasing barriers when the active alias changes).<br>• **CPU-visible pools:** a committed texture plus a "guest-layout source" flag. Data written by the CPU in Maxwell block-linear layout is detiled on Flush/first use with the existing detile code. | format tables in `host_texture_desc.h`, detile in `host_texture_layout.h`, texture pool from gpu3 |
| `TextureView*` | Format, swizzle, levels and layers → SRV/RTV/DSV desc, cached per (texture, view bytes). | RT-alias SRV cache |
| `TexturePoolInitialize`, `TexturePoolRegisterTexture(id, tex, view)` | Write the SRV directly into the shader-visible table of the right dimension at index `id`. Same heap layout as today (`BOOT_RENDER_HEAP_TEX0 + dim * 65536 + id`). No TIC words. | heap layout |
| `SamplerBuilder*`, `SamplerInitialize`, `SamplerPoolRegisterSampler(id)` | `D3D12_SAMPLER_DESC` from the builder bytes. Write regular and comparison tables at `id`. | `boot_render_sampler` mapping (minus TSC decode) |
| `Texture{Write,Read}Texels`, `Flush/InvalidateTexels` | CPU copies with tile/detile into or out of the texture's guest storage, plus the dirty flag. Readback of a GPU-written texture uses the gpu3 GPU write-back shader. | `tile.hlsl`/`host_gpu_tile_dxil.h` |
| `ProgramInitialize`, `ProgramSetShaders(Ext)` | For each stage, hash the program bytes at the shader data address (the same canonical id: SPH + code), look up the DXIL in the manifest, and store the shader ids in the Program. A missing program is dumped for AOT translation, as today. | shader cache index, manifest, missing-shaders dump |
| State objects (Blend, ChannelMask, Color, DepthStencil, Multisample, Polygon, VertexAttrib, VertexStream) | Setters write fields into the guest bytes. Binding copies them into the command buffer's current pipeline key. | PSO key and cache in `host_gpu_render_draw.h` |
| `CommandBufferInitialize`, `AddCommandMemory`, `AddControlMemory`, `SetMemoryCallback*`, `Get*MemoryUsed/Free` | The host owns recording. Guest command and control memory is accounted but never written. `Used` grows by a small per-command charge so the game's bookkeeping behaves, and the out-of-memory callback is never needed (risk R3). | — |
| `BeginRecording` … `EndRecording` | Record a compact **host command stream** (typed structs, not Maxwell methods) into host memory owned by the command buffer. `EndRecording` returns an `NVNcommandHandle` token that indexes the recorded stream. Streams are immutable and can be submitted many times, and `CallCommands` nests them. | — |
| `Bind*`, `Set*` (dynamic state) | Append state-change records. At draw time, fold them into the current D3D12 state: root tables, PSO key, viewports, scissors, stencil ref, blend factor, depth bounds. | `boot_render_draw` PSO and descriptor code |
| `BindUniformBuffer(stage, slot, addr, size)` | CBV on the arena VA (zero-copy; NVN uniform alignment is 256, as D3D12 requires). No CPU snapshot: the GPU reads memory at execution time, as on Switch. | — |
| `UpdateUniformBuffer(addr, size, offset, data)` | Inline data is copied into the host stream at record time. On replay it becomes an upload-ring staging copy and a `CopyBufferRegion` into the guest buffer, GPU-ordered, barrier-batched per run of updates. When an update covers the whole bound range, bind the staging copy directly (rename) and defer the copy. | upload ring |
| `BindVertexBuffer`, `BindVertexAttribState`, `BindVertexStreamState` | VBVs on the arena VA (or the translated vertex shaders' pull path). Input layout or attribute fetch from the attrib/stream state. | current vertex attrib path |
| `BindTexture`, `BindImage`, `BindStorageBuffer` | Per-stage bindings for bound (non-bindless) texture units. Root descriptor tables or copies into a per-draw table. | — |
| `SetRenderTargets`, `Clear*`, `Downsample` | RTV/DSV handles of the textures and views directly; no RT table keyed by guest address. `ClearColor`/`ClearDepthStencil` use the D3D12 clears. `ClearBuffer` is a UAV clear or compute fill on the arena range. `Downsample` is an MSAA resolve. | `boot_render_clear`, present |
| `Draw*`, `Draw*Indirect` | `DrawInstanced`, `DrawIndexedInstanced`, `ExecuteIndirect` (the indirect buffer is in the arena). | — |
| `DispatchCompute(Indirect)` | Compute PSO from the bound program and a dispatch. No CPU wait afterwards. | compute DXIL and root signature from `host_gpu_compute.h` |
| `Barrier(flags)` | D3D12 UAV and transition barriers per NVN barrier bits (ORDER_FRAGMENTS, INVALIDATE_TEXTURE, …). | — |
| `Copy*` | `CopyTextureRegion`/`CopyBufferRegion`; detile or tile when a side is a guest-layout buffer. | detile, tile shaders |
| `ReportCounter`, `ReportValue`, `ResetCounter` | Timestamps via a TIMESTAMP query resolved into the arena. Occlusion (ZPASS) via occlusion queries resolved and accumulated. `ReportValue` uses `WriteBufferImmediate`. Results land in guest memory on the GPU timeline. | occlusion query code |
| `SetRenderEnable(Conditional)` | D3D12 predication (`SetPredication`) on a 64-bit value in the arena. | — |
| `SetTiledCache*`, `SetShaderScratchMemory`, `SetColorReduction*`, `SetConservativeRasterEnable` | Tiled cache and colour reduction are performance hints on Maxwell (no-op, no output effect). Scratch memory: shaders use host scratch. Conservative raster: PSO flag. | — |
| `QueueBuilder*`, `QueueInitialize` | One D3D12 direct queue with a fence. Queue memory is accounted, not used. | `BootGpu` |
| `QueueSubmitCommands(n, handles)` | Replay the host streams into D3D12 command lists and execute them. No wait. | segment ring, submit sites |
| `QueueFenceSync(sync)`, `SyncInitialize`, `SyncWait`, `QueueWaitSync` | Signal a D3D12 fence value and store it in the guest `NVNsync`. `SyncWait` is a CPU wait on that value with timeout (NVN return codes). `WaitSync` is a GPU-side `ID3D12CommandQueue::Wait`. | `boot_gpu_wait` |
| `QueueFlush`, `QueueFinish` | Execute recorded lists; `Finish` also waits on the fence. | — |
| `Window*`, `WindowAcquireTexture`, `QueuePresentTexture` | The window owns N game textures. Acquire returns the next index once its present fence passed. Present copies or flips the texture to the DXGI swapchain. | `host_gpu_present.h`, `boot_render_present_target` |
| `DeviceGetTimestampInNanoseconds` | QPC-based GPU timestamp, consistent with the counter reports. | `host_gpu_timestamp.h` |
| `InstallDebugCallback`, `*SetDebugLabel` | Store or ignore. | — |
| subsdk0 NVX interop (stage 4) | Host video decode path (out of scope here); `NvRmSurface`/`NvRmSync` handles map to host textures and fences. | — |

## 3. Interception mechanism (implemented: patches/02-host-functions.patch, shared with svc-work)

One mechanism for NVN and native SDK services (`host_fn.h`, interface in its header comment):

- **Registry.** Two kinds of entries.
  - **Fake-address functions** (`boot_host_fn_register(name, fn, user, flags, steps)`).
    - The address is `0x7ff000000000 + 16*hash24(name)` (FNV-1a), so it depends on the name only. It stays stable across processes, checkpoints, DLL swaps and builds.
    - The range is above the 39-bit guest address space and outside every module, so it never collides with a page or a guest mapping.
    - A hash collision is reported at registration.
  - **Guest-entry hooks** (`boot_host_fn_hook(module, offset, name, ...)`, the svc-work design). The host function runs in place of the translated function, or declines and the translated code runs.
- **Dispatch.** Both the page dispatch (`BOOT_PAGE_DISPATCH`) and `boot_execute` ask `boot_host_fn_dispatch` first when the target has no page, or its page is marked in `boot_host_fn_mark`.
  - Every page has the mark check, so hook marks can change without regenerating pages.
  - The translated hit path costs one byte load.
  - `boot_host_fn_dispatch` is a runtime handler (`split_runtime.py`): pages see only its prototype, so adding or changing host functions rebuilds `runtime_support` only (with 01-slim-page-header). The `BOOT_PAGE_DISPATCH` change itself is page-visible, so it needs one full rebuild.
- **Return codes.** DONE (continue at `*target`, by default the return address), DECLINE (run the translated code), STOP (status). Stops re-enter the function on resume.
- **`BOOT_HOST_FN_KERNEL`.** Runs the function as pseudo-SVC `0x100` through `boot_svc`. With `TOTK_CORES>1` it then runs under the kernel lock on the master context and can switch threads like an SVC.
- **`steps`.** Charged to the guest step counter per completed call.
- **Switches.** `TOTK_HOST_FN_DENY` registers listed entries disabled; `boot_host_fn_enable` toggles them at run time.
- **Guest stack frames** (`boot_host_call_guest`, `boot_host_return_to`, `boot_host_frame_pop`). These cover callbacks into guest code, return capture and shadow comparison (decline, then compare in the continuation) with all state in guest memory.
- **Registry lifetime.** The registry is host state, rebuilt per process by `boot_host_fn_subsystems()` on the first dispatch and at each `boot_execute` entry. It is never checkpointed and adds no `BootContext` field.

NVN attaches with a guest-entry hook on `sdk+0x4900b0` (`nvnBootstrapLoader`, reached through main's PLT) and answers it with fake addresses. `TOTK_NVN=off|trace|native` selects the mode (default off: nothing registered, the game runs unchanged).
- **trace:** stage 0 wrappers around the real driver.
- **native:** host functions; anything unimplemented stops with its name.

## 4. Guest-memory state the game sees, and keeping it consistent

| Object | Game-visible storage | Plan |
|---|---|---|
| NVN object structs (Device 12288, Queue 8192, CommandBuffer 160, Texture 192, TextureBuilder 128, TextureView 40, Sampler/SamplerBuilder 96, Buffer 48, BufferBuilder/MemoryPoolBuilder 64, MemoryPool 256, Texture/SamplerPool 32, Program 192, Sync 64, Window 384, state objects 4-24 bytes; sizes from NVN headers, **to confirm in stage 0**) | Opaque bytes the game allocates and passes in | Store our state in them, self-contained and copyable. Never rely on the game not copying. Validate with magic plus host index. |
| Memory pool storage | Game memory (often CPU-mapped) | Zero-copy D3D12 region. CPU writes are seen by the GPU at submit (pools are coherent on Switch for CPU_UNCACHED; `FlushMappedRange` marks host caches dirty). GPU writes are seen by the CPU after the sync the game waits on. |
| Buffer and texture addresses (`GetAddress` 222 sites, `GetBufferAddress`) | Game stores the addresses in its own structures and in uniform data | Our VA allocator gives stable addresses; shaders resolve them through the page table. |
| Texture handles (`GetTextureHandle`) | Written by the game into uniform and storage buffers for bindless access | Same bit layout as Maxwell, so already-translated shaders work unchanged. |
| Texture and sampler pools | Game memory sized by `DeviceGetInteger` (descriptor size × count) | Not written; the host descriptor heap is the pool. |
| Counter and value reports | Game reads report memory (GPU timings, occlusion results, render-enable predicates) | Written into the arena on the GPU timeline; the CPU sees them after the game's sync. |
| Syncs | `NVNsync` struct | Holds our fence value. `SyncWait` with timeout 0 is a poll. |
| Queue, command and control memory | Game-provided | Unused. Usage counters are synthetic but monotonic per recording. |
| Window textures | Game-created NVN textures | The swapchain source. |
| Shader code pool | Game memory with the program binaries | Read once per `ProgramSetShaders` and hashed. |

The emulated path's oracle data (stage 0) also gives exact values for every getter whose result the game
relies on: storage sizes and alignments, `DeviceGetInteger` values, memory-used growth.

## 5. Staged plan

**Stage 0 — NVN call tracer (oracle; runs on the current emulated path, no behaviour change).**
- Build the host window and dispatch (section 3), but in trace mode the host `GetProcAddress` wrapper:
  1. calls the real driver's `getProcAddress`;
  2. stores the real pointer;
  3. returns a host trampoline id.
- The trampoline logs name, arguments and a hash of pointed-to structs, then jumps to the real driver function, so rendering is unchanged.
- Output:
  - the runtime-used function list and per-frame call counts for the prologue;
  - object sizes (from allocation patterns);
  - every enum value used;
  - `DeviceGetInteger` and storage size/alignment answers;
  - pairs (NVN call → Maxwell methods and TIC/TSC words the driver emitted). These derive the enum → D3D12 mapping tables, cross-checked by decoding the driver's TIC/TSC with `host_texture_desc.h`.

Emulation is used here only as a test oracle, as the project rules allow.

**Stage 1 — first frames (prologue boot and Nintendo logo).**
- Device, Queue, Sync, MemoryPool, Buffer, builders and state objects, Window, Present.
- CommandBuffer recording plus `SetRenderTargets`, `Clear*`, `SetViewport`/`Scissor`, `Barrier`, `SubmitCommands`, `FenceSync`, `SyncWait`.
- Textures in GPU-only pools; `TexturePool`/`SamplerPool` registration; `GetTextureHandle`.
- Unimplemented names trap with their name, so gaps show up immediately.

**Stage 2 — drawing.**
- Program/SetShaders (manifest DXIL), `BindProgram`, vertex attrib, stream and buffer bindings, `BindUniformBuffer`, `UpdateUniformBuffer`, `BindTexture`, `DrawElements`/`Arrays`/`Instanced`/`BaseVertex`, blend, depth and polygon state.
- Texture uploads (`CopyBufferToTexture`, `WriteTexels`, CPU-pool textures with detile).
- Target: prologue frames identical to the current renderer.

**Stage 3 — the rest of the frame.**
- `DispatchCompute(Indirect)`, `CopyTextureToTexture`/`ToBuffer`, `Downsample`, `ClearBuffer`, `ReportCounter` (timestamps, ZPASS), `SetRenderEnableConditional`, `DrawElementsIndirect`, `CallCommands`.
- Sparse pools (`MapVirtual`), MSAA, `BindImage`/`BindStorageBuffer`, conservative raster.
- Then remove the Maxwell decoder from the live path.

**Stage 4 — video.**
- subsdk0 NVX interop with a host video decoder.

## 6. Testing against the current renderer

1. **Differential frames.** Run the same prologue under the old path (with the stage 0 tracer) and the new path from a cold boot with the same inputs. Compare:
   - the presented frames (pixel diff, as `render-fixtures`/`frames` are used today);
   - every render target at present (`frames\dump-rts.request` exists on both paths).

   Small differences can come from timestamp-dependent content; the tracer records timestamps so they can be pinned.
2. **Offline replay.** Stage 0 traces plus a snapshot of guest pool memory replay through the host NVN layer in a unit-test harness, with no game. This is the style of `test_render_*.c` with a real D3D12 device. Golden images come from the old path.
3. **Mapping tests.** For each format, sampler and state enum seen in the traces, check that the host D3D12 description matches the decoded description of the TIC/TSC/registers the real driver produced for the same call. These are generated tests, one per unique call.
4. **Getter tests.** Storage size/alignment and `GetInteger` values must equal the traced driver answers exactly.
5. **Performance.** Same `perf:` counters and the sample profiler. Expected effects:
   - No compute post-dispatch waits (11-15% of the main thread today).
   - No pushbuffer method decode, macro execution, draw-event recording or per-draw register decode (about 25-35% on stack today).
   - No per-draw constant buffer snapshot copies (5.9 GB per 100 submissions today).
   - No guest-address render-target aliasing machinery.

## 7. Risks and open questions

- **R1 Enum/encoding fidelity.** NVN headers are not in the repo; enum values and struct sizes come from the stage 0 oracle and the translated sdk driver code (readable as documentation). deko3d (`gpu-reference/deko-*`) documents the same Maxwell state. Mitigation: oracle-generated mapping tests.
- **R2 Storage size/alignment.** The game sub-allocates pools with `TextureBuilderGetStorageSize`/`Alignment`. These must reproduce the driver's Maxwell block-linear sizing (GOB/tile math, compression tags) even though the host texture has its own memory. The existing layout code already computes block-linear sizes.
- **R3 Command memory accounting.** If the game relies on memory callbacks or exact `GetCommandMemoryUsed` values (101 call sites), we need a faithful-enough model or host→guest callbacks. Stage 0 measures what the game does with these values.
- **R4 CPU-visible textures and aliasing.** Textures in CPU-mapped pools written in Maxwell layout need detile and dirty tracking (as today). GPU-only aliasing maps to placed resources and aliasing barriers; a missing aliasing barrier gives wrong frames, so test the aliased render-target chains (gpu3 bench has 6) explicitly.
- **R5 `UpdateUniformBuffer` ordering.** This must be GPU-ordered. Batched copies cost barriers; renaming (bind the staging copy) avoids most of them but must still materialise the data when the memory is read another way.
- **R6 Ownership.** The interception touches `boot_runtime.h` (dispatch miss branch), `partition_native.py` (`boot_execute` loop) and optionally `translate_startup.py` (entry replacement list). All are owned by other agents and need coordinated patches.
- **R7 GPU timestamps and occlusion** feed the game's dynamic resolution and culling. Results must arrive on the same frame cadence as on the current path, or behaviour diverges (not wrong, but frames will not match bit-exactly during tests).
- **R8 subsdk0 video** stays on the emulated nvdrv path until stage 4. It uses NvRm interop, so a hybrid period must keep `/dev/nvhost-*` working for the multimedia module only.

## Reproducing the inventory

Python 3.12 with capstone 5. (The `nvn-work/tools/` analysis scripts below are development-only and are not in this repository; the release builder makes `host_nvn_names.h` with `startup/gen_game_headers.py` instead.)
Scripts are in `nvn-work/tools/`:
- `imgs.py` caches the relocated images.
- `xref.py <module>` finds ADRP+ADD references to `nvn*` name strings.
- `slots.py` maps loader names to GOT slots and globals, and counts call sites.
- `cov.py` cross-checks call sites against `discovered-main-ranges.json` and writes `nvn-entry-points.json`.
- `da.py <module> <hexoff> <n>` disassembles.

The scripts write their JSON next to themselves. Copies of the outputs are in `nvn-work/`.

## Appendix A — NVN driver ABI recovered from the stage 0 trace (2026-10-15)

Sources:
- `trace-summary.*`, `trace-functions.txt`, `trace-pairs.json` and `trace-objects.json`, from `nvn-trace.bin` (24M records, 6 boots).
- Driver disassembly (`tools/da.py`).
- `frames/draw-trace*.txt`.
- BNSH control sections (`tools/nvn_shader_headers.py`).

**Enums and handles**
- **NVN format enum:** the public order (R8=1 … RG8=0xd, RG16F=0x11, RG16=0x12, RG16SN=0x13, RG32F=0x16, RGB32F=0x22, RGBA8=0x25, RGBA8SN=0x26, RGBA16F=0x29, RGBA16=0x2a, RGBA32F=0x2e, R11G11B10F=0x3f). Confirmed by `VertexAttribStateSetFormat` and the texture builders. 0x69 (a 4-byte vertex format) is still open.
- **Shader stage enum:** 0 VERTEX, 1 FRAGMENT, 2 GEOMETRY, 3 TESS_CONTROL, 4 TESS_EVAL, 5 COMPUTE. Driver table at sdk+0xabc0c4 = {0x11, 0x51, 0x41, 0x21, 0x31}.
- **`nvnDeviceGetTextureHandle`:** `(tex | smp << 20) | 1 << 32`. Shaders read the low word.

**Graphics constant buffers**
- c0 is one driver buffer shared by all stages. SSBO `(stage, i)` lives at c0 + `table[stage]*16 + 16*i` as `{addr lo, addr hi, size}`. Shaders read VS 0x110.., PS 0x510...
- c1 holds the program constants: c1 = shader data VA + `control[0x700]`, bound with 64 KiB. `control[0x704]` is the code size and `control[0x70c]` probably the register count.
- c2 is the bound-texture table. Graphics bind group g (Maxwell group: 0 VS … 4 PS) uses driver buffer + 0x7000 + g*0x200. `BindTexture(stage, i, handle)` writes the low 32 bits of the handle at c2 + 0x20 + 8*i. Shader texture words are all even, minimum 8.
- c(3+i) is uniform buffer i, with the size rounded up to 256. Unbound slots point at a 256-byte dummy buffer.

**Compute**
- Compute binds are deferred records: 8 texture, 0xc UBO, 0xd SSBO, 0xe dispatch.
- Compute c0 reads: 0x200/0x204 and global pointers at 0x260..0x2d0 and 0x310..0x3a0. The exact slot map is pending calibration (`03-nvn-calibration.patch`, compute-cb dump).

**Shader binaries**
- 0x30-byte wrapper `{0x12345678, 0…}`, then the 0x50-byte SPH, then code. The driver's program offset is data + 0x30.
- The control section is 0x880 bytes. Varying fields: 0x6f8, 0x6fc, 0x700 (c1 offset), 0x704 (code size), 0x70c, 0x714, 0x718, 0x724–0x734 (fragment), 0x7d0–0x7e4 (hash), 0x7f8, 0x804.
- Compute programs are not in the BNSH archives.

**Driver object layouts** (driver-owned in hybrid mode)
- **MemoryPool:** +0x08 device, +0x10 flags, +0x18 GPU VA, +0x20 second VA, +0x30 storage, +0x38 size.
- **Texture/TextureBuilder:**
  - +0x08 device, +0x10 pool, +0x18 offset (bits 48+ are flags; low 48 bits are the offset).
  - Builder: +0x28 width, +0x2c height, +0x30 depth, +0x34 levels, +0x38 format, +0x40 swizzle.
  - Texture: +0x20 width, +0x24 u16 height, +0x26 u16 depth, +0x2a u8 format.
  - The texture GPU address is `pool[+0x18] + (offset & 0xffffffffffff)`. Verified: 0x514480000 is R11G11B10F, Maxwell RT format e0.

**State objects** (driver packing)
- **BlendState (8 B):** b0 target, b3 = colorEq | alphaEq<<4, b4..b7 = srcColor, dstColor, srcAlpha, dstAlpha. NVN BlendFunc: ZERO=1, ONE=2, SRC_COLOR=3, … ; BlendEquation: ADD=1, …
- **ColorState (4 B):** b1 per-target blend-enable bits, b2 logic op (COPY=3), b3 alpha-test func (ALWAYS=8).
- **ChannelMaskState (4 B):** a nibble per target, target i in nibble i.
- **DepthStencilState (8 B):**
  - b0: bit0 depth test, bit1 depth write, bit2 stencil test, bits4-7 depth func (NEVER=1 … ALWAYS=8).
  - b4/b5 front stencil: `{fail<<4 | func, zfail<<4 | zpass}`; b6/b7 back stencil, same encoding.
  - b1..b3 are unused (garbage).
- **PolygonState (4 B):** bits0-1 cull face, bit2 front face, bits3-4 polygon mode (FILL=2 → 0x10), byte1 bit7 fill offset enable.
- **VertexAttribState (4 B):** `0x40 | offset<<7 | nvnFormat<<21 | stream` (stream in bits 0-4).
- **VertexStreamState (8 B):** u32 stride, u32 divisor.

**`nvnDeviceGetInteger` answers:** {0:55, 1:15, 4:256, 0xf:32, 0x10:32, 0x11:256, 0x12:256, 0x14:8, 0x18:256, 0x19:4096, 0x1a:131072, 0x28..0x2a:1, 0x32:1, 0x36:32, 0x39:128, 0x3b:1, 0x42..0x4c:1, 0x57:1, 0x5a:0}

**Plan: hybrid native**
- The driver keeps the CPU-only object functions: pools, buffers, textures, samplers, programs, state objects, texture and sampler pool registration (it writes TIC/TSC). None of these emit GPU command streams.
- The host owns command buffers, queues, syncs, window and present.
- The host executor decodes the objects above into the renderer's draw description (`BootDrawState` + cbuf bindings) and calls the existing `boot_render_draw`, clear, compute and present code.
- No Maxwell pushbuffers, GPFIFO, method decoding or macros run in native mode.
