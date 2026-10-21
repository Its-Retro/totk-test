# Development history (startup notes)

> This is the development log of the native startup work, kept for history. Several scripts and files it
> names are development-only and are not in this repository. For building and playing, see the README in
> the repository root.

# Native Windows startup: verified graphics-initialization entry

**Current checkpoint (2026-09-16):** see [CURRENT-STATUS.md](CURRENT-STATUS.md).
The latest verified startup has 411,489 selected instructions and 236 passing
test groups. Independent D3D12 tests now include an AOT-translated SDK vertex
shader and the matching fragment shader's mode-zero specialization. They use
test geometry, are not integrated with startup and do not count as game frames.

## Historical startup notes

The verified standalone build translates **384,053 selected instructions** and passes **230 test groups**, the native Win32 lifecycle test, and three standalone-host checks. It completes initial runtime linking, SDK core OS setup, logging-client initialization, virtual-address memory initialization, power-manager client initialization, application/applet interface construction, and system-worker initialization. The original worker executes, blocks on a mutex, resumes, waits, handles cancellation, and returns to waiting while the main thread continues. A Windows host window now supplies applet messages and focus state. Verification date: 2026-09-13. The user confirmed visible desktop ACTIVE/BACKGROUND transitions; see `WINDOW-TEST.md` and `interactive-window-run.txt`.

The game does not boot, render frames, or play audio. This is a bounded ahead-of-time recompilation experiment with selected Switch OS compatibility behavior. There is no runtime ARM opcode interpreter or JIT; implementing OS dependencies is still necessary.

## Run the verified standalone executable

```powershell
& '<root>\startup\totk-native-startup.exe'
```

Keep `unrelocated-startup.bin` beside the executable. Python and the separate DLL are not required to run it. The executable checks its original module-data bundle against a compiled-in SHA-256 before executing translated code. Original cartridge and key files remain unchanged.

For the interactive host-window test, double-click **`test-native-window.cmd`**. It runs `totk-native-startup.exe --window`, shows ACTIVE/BACKGROUND focus status, and saves `interactive-window-run.txt`. Switch away and back, then close the window. See `WINDOW-TEST.md`. No game graphics or audio are expected.

Actual verified output:

```text
Native x64 startup experiment: original module data verified.
Stopped: status=10, module=sdk, offset=0x158400, SVC=0x18
Native instruction steps: 102398442; memory queries: 26; entropy queries: 2
Layout queries: 16; thread queries: 3
Port connections: 1; SM registrations: 1
Service lookups: 3; CMIF controls: 4; logger opens: 1; physical allocations: 1
Application proxy opens: 1
Tick reads: 3; event handles: 3; stack aliases: 1
Guest heap bytes: 0
Threads created: 1; started: 1; switches: 12; current: 0
Reached an explicit startup compatibility boundary; see offset and SVC above.
This experiment does not boot the game or render graphics.
```

The headless boundary is an infinite notification wait with no runnable alternative and no attached host window. The injected hidden-window fixture completes foreground configuration, add-on-content/error-context client setup, filesystem access-log configuration, and permitted parental-control client initialization. Original main-module `nninitStartup` then obtains a 0x75400000-byte (1.83 GiB) Windows-backed heap, initializes the allocator and TLS allocation callbacks, and returns. SDK, subsdk0, and all 10,539 main initialization entries return. Execution enters the exported `nnMain` thunk and body, applies FPCR flush-to-zero mode and the SuspendHomeSleepNotify focus policy, enables resume notifications, then reaches main+`0xe9d4fc` (status 11, next untranslated game-code block) after 112,571,005 native instruction steps. The original SDK successfully mounts RomFS after six reads totaling 23,711,320 bytes; graphics/audio remain unimplemented. `window-probe-trace.json` and `native-window-fixture-run.txt` label injected focus; real desktop focus transitions were separately confirmed by the user.

## Implemented and verified at this milestone

- Original runtime code performs 514,885 early RELATIVE relocations across main/subsdk0/sdk, 20 in rtld, 30,501 data-symbol relocations, preparation of 13,497 lazy PLT slots, and five runtime export injections. Preparing a slot does not translate its target.
- Core OS setup constructs the main-thread object, TLS, address reservations and memory accounting. All four module images match an independent expected-state model at the core-OS checkpoint. TinyMT matches the pinned upstream implementation in 166 cases.
- Typed service-manager endpoints resolve `lm`, `apm`, `appletOE`, `aoc:u`, `ectx:aw`, and `pctl`. Sessions retain synthetic process identity and support close handling. CMIF pointer-buffer negotiation and domain conversion create tracked objects. The original SDK accepts a logger, power-manager handle, application proxy, and its eight interfaces.
- The applet interfaces are application functions, library-applet creator, common-state getter, self controller, window controller, audio controller, display controller, and debug functions. Their construction does not implement all their commands. A stable resource ID is assigned within the single-application laboratory profile.
- Physical mapping supplies an actual zero-initialized backing allocation. The SDK maps and uses a 4 KiB page and completes virtual-memory initialization. Exact mappings can be unmapped, repeated maps preserve data, and reset/host exit release owned allocations. Range, budget, overlap and capacity checks precede allocation.
- The source manifest reserves 24 MiB for process memory management. Laboratory memory accounting now charges that reservation consistently. No Windows/Switch secure kernel pool is created, and querying used kernel-pool memory remains unsupported.
- Selected `subsdk0` IPC parsing code now executes natively, following the original weak-symbol resolution. Input module hashes are pinned. Additional original integer, string, copy, locking and condition-code paths are tested.

## Scope and validation

All **230 groups** passed under `-O2`: 2 manifest-service, 8 startup, 5 memory-query, 3 module-startup, 5 linker/instruction, 3 SDK, 7 OS, 6 IPC, 23 logging/service/instruction, 7 application-display-service, 5 native-window/Binder, 2 managed-layer, 3 session-clone, 2 performance-service, 2 native-performance, 8 filesystem-client, 3 native-storage, 1 original-RomFS-mount, 1 original-file-read, 1 original-graphics-entry, 2 runtime-cache-split, 8 physical-memory, 6 heap, 3 transfer-memory, 5 driver-initialization, 11 applet, 5 event, 7 alias-memory, 12 thread/instruction, 6 wait, 12 window-service, 8 parental-client lifecycle, 2 main-startup, 11 condition/instruction, 1 conditional-add, 1 unsigned-bitfield-insert, 20 scalar/SIMD/ABI, 2 rotate/extension/division, 1 widening-multiply, 1 literal-load, 1 byte-reversal, 2 integer-to-float, 1 float-to-integer, 1 CRC, 1 AOT reachability, 1 function-pointer-candidate and 2 constructor-observation groups. These include original startup through main's initializer, exhaustive standard condition truth tables, allocator arithmetic, heap lifetimes and aliases, denied service lookup, object/session close, priority restoration and active-execution window close. The separate native Win32 lifecycle test and all three standalone checks passed. The standalone AMD64 executable matches both headless and injected-focus boundaries and rejects a mismatched data bundle before execution.

The configured 2 GiB heap reservation, 4 GiB alias reservation, 256 MiB stack reservation, 64 GiB ASLR range and 3 GiB process budget are laboratory choices. Reservations do not themselves allocate memory. Physical mapping accepts whole page-aligned allocations up to 256 MiB each, with 64 total region slots. Stack aliases share backing storage, lock source access, restore it on exact unmap, and do not double-count physical memory. Live thread stacks cannot be unmapped. Partial physical mapping operations and full kernel error behavior remain unfinished. SetHeapSize reserves a stable Windows virtual range, commits 2 MiB-aligned sizes, decommits shrinking tails, protects aliases/live stacks and releases owned memory on reset. The original guest allocator manages that backing memory. Thread/process/resource IDs are synthetic. Main priority 44, its 1 MiB stack, permitted priorities 28ÃƒÂ¯Ã‚Â¿Ã‚Â½59 and virtual cores 0ÃƒÂ¯Ã‚Â¿Ã‚Â½2 come from the manifest.

Up to 16 statically translated threads have private integer/vector registers, flags, stacks and TLS. Scheduling is cooperative; this is not parallel multicore execution or a complete Horizon scheduler. Mutex blocking/handoff, infinite waits, zero-timeout polls, cancellation, event reset and joining an exited thread are implemented within the tested profile. An attached host window supplies the idle wake/close path; a headless all-blocked process stops explicitly. Finite timers and nested lock-owner dependency chains remain boundaries. Windows manual-reset events retain independent references while a guest thread waits. Windows QPC supplies the 19.2 MHz system tick.

The Win32 UI thread owns its window and shares only a locked notification queue with the native guest scheduler. Activation maps to applet focus notifications, closing queues an exit request, and CommonStateGetter commands 0/1/9 expose the event, messages and focus state. Automated tests inject activation into a hidden window and clearly label that fixture. The user confirmed real visible-window ACTIVE/BACKGROUND changes on 2026-09-13. The interactive log reached the foreground-rights call and the process subsequently exited; clean close behavior was not separately confirmed by the user.

Foreground acquisition tracks ownership of the active host window without forcing Windows activation. The observed SuspendHomeSleep and SuspendHomeSleepNotify configurations, with disabled out-of-focus suspension, are supported in the desktop profile. Other focus policies stop explicitly. Resume subscriptions use registered Win32 power messages; an observed suspend/resume cycle queues one message 16 when enabled. Tests inject these messages without sleeping the machine. Add-on-content and error-context clients can initialize, but their content/report commands are not implemented. The filesystem client now opens and clones a tracked domain, initializes process identity, queries the disabled logging policy, and mounts the read-only RomFS image through native storage reads. See filesystem-scope.md for exact boundaries and verification.

Common-state command 900 now stores the next-program exit setting. It does not implement program launching. Suspended-time-change and GPU-error event handles are accepted by the original SDK; a GPU-error notification handle does not provide a graphics backend.

Unsupported service commands and instruction targets stop explicitly. There is no blanket success handler. Logging-client setup is implemented; log packet delivery is not. Audio/display interface objects do not provide rendering or playback. Approximately 110 million executed steps mostly repeat loops and are not a whole-game completion percentage. No original-console differential execution was performed.

## Verified game data

`../assets/romfs.bin` contains 17,069,139,640 bytes. Its 304,207 file paths and 2,665 directories were indexed; all paths agree with hactool's independent listing. All six IVFC levels passed integrity verification, along with 384 sampled reads and 13 synthetic reader cases. The image SHA-256 is `4751542f544d8c3406cb63ccfc68b42690ff0387228d1f0af021d1f0037dafcf`. The reader is read-only and bounded. **The guest filesystem is not yet connected to this image.** See `../assets/README.md` and `romfs-verification.json` there.

## Build and evidence

```powershell
& '<root>\startup\build-and-test.ps1' -CachedObjects -PrecompiledHeader
```

Python/Capstone and portable Zig are build/test dependencies. The generated code is partitioned into bounded native functions covering 4 KiB source pages, with direct local branches and a nonrecursive cross-page return loop. All target labels remain static; there is no runtime opcode decoder. This replaces the monolithic function whose O0 build exhausted the host stack and whose optimized compile took several minutes. The optimized partition prototype compiled in 21.68 seconds; all 140 behavioral groups plus native/standalone checks pass with the adopted layout. Final builds use `-O2`. The default full build compiles one object for DLL and EXE; `build-and-test.ps1 -CachedObjects` links both artifacts from content-checked page objects and runs the same complete validation suite. The current verified standalone uses this cached mode. Development now uses optimized cached objects through `compile_cached.py`. Source, recursively included project headers, compiler executable and flags enter the cache key; cached object contents are checked before reuse. Header snapshots and per-unit source are immutable. Observed warm O2 rebuild: 1.57 seconds for two changed units. Both cache optimization modes passed 140 tests, and a fixed-entropy/fixed-clock comparison against the regular build matched CPU state, counters, all non-heap regions and the first 9 MiB of heap at the same constructor boundary. The earlier O0/O1 benchmark is historical; this Zig driver mapped the attempted O1 compile to Clang O2.

`advance_supported.py` adds bounded, strictly supported rtld/main/SDK/subsdk0 ranges, follows module transitions and stops for unsupported instructions, services or its hop limit. `provenance.json` and discovered-range files record the selected source instructions. `build-and-test.log`, the verification JSON files, and `native-run.txt` preserve checks and output. The current development DLL and trace can advance beyond the last verified standalone milestone described above.

`run_window_probe.py` and `advance_supported.py 25 --focus-fixture --precompiled-header` use the explicitly injected, hidden-window activation fixture and a separate trace. The production `--window` mode does not inject focus. The native host's `--window-fixture` mode is an automated validation path only.

Remaining work includes subsequent applet/OS configuration and events, the remaining SDK initialization calls, guest filesystem access and further game startup, then graphics initialization, GPU memory/commands, shaders and presentation. No reliable completion percentage or graphics date is established.

## Primary references

- [libnx SVC ABI](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h), [TIPC](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/tipc.h), [CMIF](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/cmif.h), and [applet client](https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c).
- [Atmosphere physical-memory SVC behavior](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_physical_memory.cpp), [NPDM layout](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/ldr/ldr_types.hpp), and [logging interface](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/source/lm/sf/lm_i_log_service.hpp).
- [Atmosphere stack aliasing](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp), [mutex ownership](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp), [wait/cancellation semantics](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_synchronization_object.cpp), and [kernel result values](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/svc_results.hpp).
- [Windows activation messages](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-activate) and [libnx applet message/focus definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/applet.h).
- [Pinned TinyMT reference](https://github.com/MersenneTwister-Lab/TinyMT/blob/fabd14128db25498d6d3baf9cd25e15983bc12fd/tinymt/tinymt32.c), cached with its BSD license under `reference-tinymt` for tests.

Verified executable SHA-256: `1008477a2b270ecdedfe26c5af6e25501bb98dd04963ee3d0d0d219e3083cc47` (57,511,424 bytes). Verified DLL SHA-256: `b4691f454524fc9ceca1c677f6e16c14c2a543602be07c368265df410334fbf0` (57,498,112 bytes).

The generated service allowlist is parsed from the hash-pinned original NPDM and independently matches hactool: 48 permitted client names, including only `pctl` among its privilege variants. Disallowed valid names return SM NotAllowed; permitted but unimplemented services stop explicitly. Parental-control creation/initialization and domain/session closure are supported; policy queries remain boundaries. FastLoad maps to a restorable Windows execution-thread priority and does not reproduce Switch hardware clocks or GPU throttling.

The verified constructor path includes C++ locale/character-conversion setup, additional SDK TLS initialization, and NVIDIA runtime allocation/heap registration (`NvOsHeapAllocatorAlloc`, `NvMemoryProfilerRegisterHeap`). This is runtime initialization, not GPU command execution. The expansion tool follows bounded direct-call dependencies and batches supported initialization entry blocks while retaining strict instruction checks and original execution order.

`verified-provenance.json`, `verified-startup-trace.json` and `verified-window-probe-trace.json` remain tied to the verified standalone build while the regular traces advance. DMB ISH/ISHST/ISHLD use a full host fence for the supported normal-RAM profile; no device-memory ordering model is provided.

## Scalar and SIMD initialization support

The verified checkpoint includes checked scalar/vector memory operations, lane copies, immediate and register bit copies, scalar/vector negation, signed division, rotations/extensions and CRC32. Scalar arithmetic includes finite S/D addition, subtraction and multiplication, S division and square root, S/D conversion and comparisons. Default nearest-even and FZ profiles have independent arithmetic tests; unsupported cases stop explicitly. FPCR/FPSR are private to cooperative threads, and the loader rejects stale context ABIs before native cleanup. See `floating-point-scope.md` for exact coverage and limitations.

The development batch budget is now 4,096 instructions, with an 8,192-instruction selection bound. Known missing targets near the current stop are prioritized before later constructors. Up to eight compiler workers build independent units (half the available logical cores, capped at eight). Builds reuse verified objects; observed small batches took 2ÃƒÂ¯Ã‚Â¿Ã‚Â½7 seconds, while batches changing 57ÃƒÂ¯Ã‚Â¿Ã‚Â½83 functions took 15ÃƒÂ¯Ã‚Â¿Ã‚Â½20 seconds. These numbers measure development throughput, not game completion.

The constructor-return milestone is preserved in `main-constructors-returned-trace.json`: it contains main constructor entry **10,539 of 10,539**, its return through main's initializer, and subsequent loader execution. `nnmain-entry-trace.json` separately records execution of the exported nnMain thunk and its body. These are phase milestones, not a game-completion percentage. Immutable object response files are used for the standalone EXE link.


The 384,053-instruction checkpoint additionally verifies resume subscriptions through ISelfController command 14 and registered Win32 suspend/resume messages. Synthetic-message tests exercise one notification per observed suspend/resume cycle, duplicate suppression, disabling, and cleanup; no actual machine sleep was triggered. All 230 groups and standalone checks pass. `build-and-test.ps1 -CachedObjects -PrecompiledHeader` enables the verified PCH cache, and `advance_supported.py 25 --focus-fixture --precompiled-header` reuses it for expansion. The measured full cold rebuild fell from 790.92 seconds to 345.56 seconds across adjacent 4,055-unit builds; this is not an overall game-speed measurement.





Service rebuilds now isolate eight thread/service handlers in one runtime-support object. A measured handler edit rebuilt one object and reused 4,080 in 5.16 seconds; declaration, context-layout, and other shared-header changes still rebuild affected translated pages. Both the full test suite and original startup trace matched after this separation. See runtime-split-incremental-build.json and split_runtime.py.

`original-file-read-verification.json` additionally proves delivery of the complete 1,524-byte heap-definition file to original game code, byte-for-byte against the independent RomFS reader. The verified fixture performs seven storage reads totaling 23,712,844 bytes and applies the scoped native HighQoS profile. Decompression and graphics are not established by this check.

The latest verified checkpoint reaches original nv::InitializeGraphics after setting the graphics allocator, creates 16 MiB of transfer memory, opens and initializes nvdrv, clones the session, validates the application-resource identity, uses the disabled firmware-margin host policy, acquires the application display service, acquires its Binder relay, opens the tracked Default display, allocates the managed display/recording layer pair, opens the layer, constructs the SDK Surface and obtains Binder and compositor events, then reaches main+0xe9d4fc. See display-initialization-scope.md for the service-denial fix and application-only policy. See graphics-scope.md and graphics-entry-verification.json. The English desired-language profile and numeric conversions used on this path are verified. No GPU commands, shaders, presentation or rendered frames are implemented yet.




The opaque window cache boundary reduces private window-state rebuilds to one object in the measured case: 5.716 seconds, 4,192 reused objects. See window-cache-scope.md and window-cache-verification.json. The native DWM timing test also passes. No game frames exist yet.
