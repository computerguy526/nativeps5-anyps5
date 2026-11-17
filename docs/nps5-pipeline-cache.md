# nps5: pipeline cache and prewarm on KosmicKrisp (Vulkan on Metal)

Goal (SPEC): the second launch reaches gameplay with no shader-compile stall over 50 ms.

## What AnyPS5 persists today

| Layer | File / place | Keyed by | Holds |
|---|---|---|---|
| Shader disk cache (`ShaderDiskCache`, `[shader-disk-cache]`) | `<cache dir>/<SourceVersion>/<hash>.bin` | recompiler source hash + request | recompiled SPIR-V (GCN/RDNA -> SPIR-V), so the recompile is skipped |
| VkPipelineCache (`PipelineCache`, `[pipeline-cache] loaded ... vk-pipelines-*.bin`) | `<cache dir>/<SourceVersion>/vk-pipelines-<vendor>-<device>-<driverVersion>-<pipelineCacheUUID>.bin` | vendor, device, driver version, pipelineCacheUUID, shader source version; header checked again on load; checksum | the driver's `vkGetPipelineCacheData` blob; saved every 10 s and at teardown, merged with the file |
| Pipeline journal (`PipelineJournal`, `[pipeline-journal] loaded N pipelines`) | journal file bound to a device fingerprint (vendor, device, driverVersion, UUID) | device fingerprint | recipes (state + SPIR-V modules) of every graphics/compute pipeline the title used |
| Journal replay | pool of QOS_UTILITY threads (half the cores, `APS5_PIPELINE_JOURNAL_THREADS`) started at window attach / first draw | | rebuilds every journalled pipeline into the store and the VkPipelineCache before draws need them; `Driver::WaitForPipelineJournal` can block on it |

The cache directory is `ANYPS5_SHADER_CACHE_DIR` or `shader_cache/` beside the executable.

## Where the time goes on KosmicKrisp (instrumented)

KosmicKrisp persists nothing by itself. Its pipeline-cache blob holds MSL text. Loading it skips SPIR-V -> NIR -> MSL
(the 1.4-2.2 s for the big Calculator3D shader) but still hands the MSL to Metal: `newLibraryWithDescriptor`
(MSL -> AIR, front end) and `newRenderPipelineStateWithDescriptor` (AIR -> GPU binary). Both are cached by the OS Metal
cache only (not app controlled; evictable; lost on OS update). A big shader costs ~5 s with everything cold.

Additional finding: **`vkCreateDevice` itself stalls 275-290 ms** (1.1-1.7 s with an empty OS cache) on stock KosmicKrisp,
because it builds ~16 internal device-library Metal pipelines (`kk_device_lib.c`) on every device creation and nothing
caches them (the VkPipelineCache does not cover them).

Instrumentation (always on, `Graphics/src/PipelineCreateStats.cpp`): every `vkCreateGraphicsPipelines` /
`vkCreateComputePipelines` is timed. `[pipeline-stall] graphics ... N ms` prints for any call over 50 ms that is not
prewarm work (i.e. a draw-time stall); `APS5_LOG_PIPELINE_CREATE=1` prints every call; a totals line
(`[pipeline-create] exit: N graphics .. ms, M compute .. ms, max .., K draw-time stalls`) prints at exit.
In KosmicKrisp, `KK_PIPELINE_ARCHIVE_DEBUG=1` prints library / pipeline / archive times.

## What changed

1. **Metal pipeline archive in KosmicKrisp** (`work/s3/patches/0001-kosmickrisp-persistent-mtl4-pipeline-archive.patch`,
   against Mesa 03ac14ac). `KK_PIPELINE_ARCHIVE=<file>`: the shared `MTL4Compiler` gets an
   `MTL4PipelineDataSetSerializer` (configuration `CaptureBinaries` only: combined with `CaptureDescriptors` the flush
   returns NO with no error) and every compile looks the pipeline up in the archive loaded from `<file>` through
   `MTL4CompilerTaskOptions.lookupArchives`. The archive is rewritten at compiler release and at exit, only if a
   pipeline took > 20 ms (a real compile; archive hits are ~1 ms). Unset: behaviour unchanged.
   `KK_MSL_CACHE_BUST=<tag>` appends a comment to the MSL so the OS Metal cache misses (benchmark aid).
2. **Device setup** (`ConfigureDriverPipelineArchive`, called right before `vkCreateDevice`): when the driver is
   KosmicKrisp and `KK_PIPELINE_ARCHIVE` is unset, point it at
   `<cache dir>/<SourceVersion>/kk-archive-<vendor>-<device>-<driverVersion>-<pipelineCacheUUID>-<macOS build>.mtl4a`.
   Device + driver UUID (KK: hash of the driver build) + our shader source version + OS build, so a stale archive is
   never loaded; Metal itself also rejects an archive from another GPU/OS (we log and rebuild).
   `APS5_KK_PIPELINE_ARCHIVE=0` disables. Stock KosmicKrisp ignores the variable, so this is safe without the patch.
   `setenv` lives in `Graphics/src/DriverPipelineArchive.cpp`; `ShaderCacheVersion.cmake` exempts the literal
   `"KK_PIPELINE_ARCHIVE"` from its no-`setenv` rule (the variable never reaches the recompiler).
3. VkPipelineCache keying (device + driver UUID + version) and journal prewarm already existed and are unchanged; the
   archive makes the prewarm cheap on KosmicKrisp and removes the `vkCreateDevice` stall.

## Measurements (Apple M4 Pro, macOS 26.6.2, KK 03ac14ac, real Calculator3D VS/FS from `rosetta/spv_arm`)

Tool: `nps5_pipe_bench` (`ninja nps5_pipe_bench`; `DYLD_LIBRARY_PATH=/opt/homebrew/lib VK_DRIVER_FILES=<icd.json>
ANYPS5_SHADER_CACHE_DIR=<dir> nps5_pipe_bench [--prewarm] vs.spv fs.spv ...`). It drives the driver's own `PipelineCache`
and `ConfigureDriverPipelineArchive`, creates the two real pairs (big = 2406e0c00+2406e2200, 2.7 MB MSL; small =
2406e5e00+2406e6100) in separate processes. No guest execution, no recompile (a full frame replay is blocked by the
0x2_0000_0000 aperture, see REPORT S3b). Process 1 is not a true cold launch: earlier runs warmed the OS Metal cache
(a true first launch is ~5 s for the big pair, S3b).

| Scenario (separate processes) | big pair | small pair | vkCreateDevice |
|---|---|---|---|
| A. stock KK, no app cache, OS cache warm | 2280-4690 ms | 49-103 ms | 1100-1700 ms |
| B. stock KK + VkPipelineCache file (existing), launch 1 | 4147 ms | 7.9 ms | 516 ms |
| B. launch 2 / 3 (warm) | 7.9 / 22.6 ms | 1.4 / 1.3 ms | 274 / 286 ms |
| C. patched KK + VkPipelineCache + archive, launch 1 | 731 ms | 9.0 ms | 242 ms |
| C. launch 2 / 3 (warm) | 6.4 / 6.0 ms | 0.45 / 0.44 ms | **6.8 / 5.5 ms** |
| C. launch 2 with OS library cache cold (`KK_MSL_CACHE_BUST`) | 2340-2390 ms | 43 ms | 900-1060 ms |
| Prewarm on a background thread, C warm | 5.9 ms | 0.4 ms | render-thread longest tick gap 1.4 ms |
| Prewarm on a background thread, no caches | 369 ms | 7.8 ms | render-thread longest tick gap 3.8 ms |

Process start to last pipeline built, warm launch: B 400-550 ms, C 131-146 ms.

Reading: with the archive, a warm launch has no pipeline or device-creation stall above 7 ms, and the 275 ms
`vkCreateDevice` stall is gone. The archive removes the Metal GPU-binary compile, the `vkCreateDevice` internal
pipelines, and the dependence of the *backend* compile on the OS cache. It cannot remove the MSL -> AIR library compile
(Metal 4 archive lookup needs an `MTLLibrary` object first), so if the OS Metal cache is evicted the big shader still
costs ~2.3 s once (row "OS library cache cold"). The journal prewarm moves that cost off the render thread.

## Prebuilt Metal libraries (removes the cold-OS-cache stall)

Metal 4 archives cannot persist MSL -> AIR, so an evicted OS Metal cache costs ~2.3 s (big Calculator3D pair) plus ~1 s in
`vkCreateDevice`. `work/s3/patches/0002-kosmickrisp-prebuilt-metallib-lookup-and-msl-collect.patch` (stacked on 0001,
touches only `bridge/mtl_compiler.m`) lets KosmicKrisp skip that step with an app-provided `MTLLibrary`:

- `KK_METALLIB_DIR=<dir>`: in `mtl_new_library`, hash the MSL and look for `<dir>/<key>.metallib`; on a hit load it with
  `newLibraryWithURL` (0.1-1 ms, no MSL compile), else compile as before. Covers pipeline stages and the ~16 device-library
  pipelines of `vkCreateDevice`. `KK_METALLIB_DEBUG=1` logs hits/misses.
- `KK_MSL_COLLECT_DIR=<dir>`: every miss writes `<key>.metal` + `<key>.opts` (MSL version, math mode, stamp).
- key = SHA-256/128 of (Mesa version + git sha, macOS build `kern.osversion`, GPU name, MSL version, math options, MSL text).
  A new OS, driver build, GPU or any MSL change gives a new key, so stale libraries are never loaded.
- `tools/kk-metallib/build_metallib.sh <collect dir> <out dir>` builds each `.metal` with `xcrun -sdk macosx metal -c
  -std=metal<ver> -fmetal-math-mode=.. -fmetal-math-fp32-functions=..` then `xcrun metallib` (parallel, skips existing) and
  writes `MANIFEST.txt` (macOS build, Metal compiler version). ~60 s for Calculator3D's 20 libraries (the 2.7 MB MSL is ~50 s).
- AnyPS5 wiring (`ConfigureDriverPipelineArchive`): `KK_METALLIB_DIR=<cache dir>/<SourceVersion>/kk-metallib` is always set
  (`APS5_KK_METALLIB=0` disables); `APS5_KK_COLLECT_MSL=1` also sets `KK_MSL_COLLECT_DIR=.../kk-msl`. Workflow: run the title
  once with `APS5_KK_COLLECT_MSL=1`, run `build_metallib.sh kk-msl kk-metallib`, next launch loads them. Stock KosmicKrisp
  ignores all three variables.

Toolchain: the Metal toolchain is installed here (`xcrun -f metal` -> MobileAsset MetalToolchain v27.2.47.0, metal 32023.922).
On machines without it: `xcodebuild -downloadComponent MetalToolchain`. The script exits 2 with that hint.

Measured (M4 Pro, macOS 26.6.2, `nps5_pipe_bench`, real Calculator3D pairs, warm VkPipelineCache + archive, OS library cache
cold via `KK_MSL_CACHE_BUST`; the machine was heavily loaded, load average > 100, so absolute numbers are pessimistic):

| | big pair | small pair | vkCreateDevice |
|---|---|---|---|
| before: cold OS lib cache, no metallib (3 runs) | 2504-2626 ms | 49-62 ms | 1023-1147 ms |
| after: prebuilt metallib, same bust (3 runs) | 5.0-7.2 ms | 0.5-1.4 ms | 4.5-11.4 ms |
| stock warm (OS cache warm, no metallib) | 8.0 ms | 0.7 ms | 11.5 ms |

Prebuilt libraries are independent of the OS library cache (bust or not gives the same time). The archive still matters: the
first launch after the libraries appear builds the AIR -> GPU binary once (~0.9 s big pair on a loaded machine) and writes the
archive; archive entries for pipelines built from a source library are not reused for metallib ones. Without any app cache the
backend compile (AIR -> GPU binary) is still paid, as before.

Correctness: `agc_frame_replay` on `captures_old/{A,B,C}` with every library (35 per capture set) coming from the metallib
path (35 hits, 0 misses): 100 % identical pixels, max channel diff 0 on all compared frames (2 / 4 / 4), same as the MSL path.

## Indexed per-title store (`gpucache.sqlite`, nps5-shcache)

The loose files of the KosmicKrisp layers (`kk-archive-*.mtl4a`, `kk-metallib/*.metallib`, `kk-msl/*.metal`) are replaced by one
SQLite file per title, `<cache dir>/gpucache.sqlite` (cache dir = `ANYPS5_SHADER_CACHE_DIR`, i.e. `~/Library/Application
Support/nativeps5/<titleId>/gpu-cache` under the nativeps5 runtime). `libsqlite3` is the macOS system library (`-lsqlite3`),
zstd is the one already linked. Code: `Graphics/{include,src}/GpuCacheStore.*` (store, no Vulkan dependency),
`Graphics/src/DriverPipelineArchive.cpp` (runtime wiring), `tools/gpucache/nps5_gpucache.cpp` (CLI), tests
`tests/GpuCacheStore.cpp` (ctest `gpucache_store`).

**Schema.** One row per artefact in `artefact`: `kind` (spirv, msl, metallib, archive, vkcache), `name` (the KK key), `source_hash`
(what it derives from; a metallib's value is the content hash of its MSL), `content_hash` (SHA-256/128 of the uncompressed bytes),
the version of every stage it depends on (`v_recompiler`, `v_mesa`, `v_macos`, `v_metalc`, `v_gpu`), raw/stored bytes,
`created`, `last_used`, `use_count`, `warm` (used by the last run), `meta` (MSL options and the KK stamp, needed to rebuild a
metallib) and the zstd `data` blob. `meta` holds the stage versions last seen (`stage.*`). Blobs are zstd level 12 (MSL, SPIR-V)
or 6 (metallib, vkcache), 3 for the archive that is rewritten every run.

**Stages and what depends on what.**

| kind | recompiler | KK/Mesa | macOS build | Metal compiler | GPU |
|---|---|---|---|---|---|
| spirv | x | | | | |
| msl | x | x | | | x |
| metallib | | x | x | x | x |
| archive | x | x | x | | x |
| vkcache | x | x | | | x |

Versions at runtime: recompiler = `ShaderDiskCache::SourceVersion()`; Mesa = Vulkan `driverInfo` + `pipelineCacheUUID` + driver
version (KK hashes its build into the UUID); macOS = `kern.osversion`; GPU = `deviceName|vendor:device`; Metal compiler is not known to
the running app (empty = never compares) and is recorded by `nps5_gpucache rebuild` from `xcrun metal --version`. An empty current
version never invalidates. On startup `Store::Invalidate` compares and deletes exactly the rows whose dependent stages differ
(`[gpu-cache] stage change ( macos ): dropped 35 metallib 1 archive, kept 35 msl`). A recompiler bump leaves metallibs alone
(they are keyed by the MSL hash, a changed MSL just misses).

**Runtime flow** (`ConfigureDriverGpuCacheStore`, called from `ConfigureDriverPipelineArchive` before `vkCreateDevice`): open store,
invalidate, recover run directories of dead processes, create `<cache dir>/run/<pid>/`, decompress the archive and all metallibs
(prewarm order: warm, last used, use count; `APS5_GPUCACHE_MATERIALIZE_MB` caps the bytes) into it, point `KK_PIPELINE_ARCHIVE`,
`KK_METALLIB_DIR`, `KK_MSL_COLLECT_DIR` there, and register an `atexit` hook. The patched KosmicKrisp is unchanged. At exit (after the
driver's own archive flush, `atexit` is LIFO) new MSL and a rewritten archive are compressed back, usage flags are updated, the run
directory is deleted; a killed process is recovered by the next launch. At rest only the store file exists.
MSL is now collected by default (it is what makes offline `rebuild` possible); `APS5_KK_COLLECT_MSL=0` turns it off.

**Switches.** `KK_PIPELINE_ARCHIVE` / `KK_METALLIB_DIR` / `KK_MSL_COLLECT_DIR` set by the caller win over the store for that
artefact (no store use, as before). `APS5_KK_PIPELINE_ARCHIVE=0` and `APS5_KK_METALLIB=0` behave as before. `APS5_GPUCACHE=0` restores
the loose-file layout. New: `APS5_GPUCACHE_MAX_MB` (trim coldest rows at exit), `APS5_GPUCACHE_MATERIALIZE_MB`.
The shader disk cache (SPIR-V `<hash>.bin`, own packs) and the VkPipelineCache file are not read through the store yet; the schema
and CLI already hold them (`ingest --spirv/--vkcache`), see the sizes below.

**CLI** `build-arm64/core/libs/prx/libSceAgcDriver/nps5_gpucache <cmd> --cache <gpu-cache dir>` (`ninja nps5_gpucache`):
`list [--kind K]` (prewarm order), `stats`, `check --stage name=value` (dry run of an invalidation), `invalidate`, `trim --max-mb N`,
`ingest --msl DIR --metallib DIR --archive FILE --vkcache FILE --spirv DIR` (import loose files), `rebuild [--jobs N]`,
`verify`, `export --gpu NAME --out FILE`, `import FILE`. `rebuild` finds every valid MSL row without a metallib for the current
stamp (it recomputes KK's key: SHA-256/128 of stamp with the current macOS build, GPU name, `msl/math/fp` options, MSL text, which
matched on all 35 libraries), compiles it with `xcrun metal` + `metallib` (replaces `build_metallib.sh` for stored MSL) and stores it
with the Metal compiler version. `export` writes a self-contained store file holding the rows valid for one GPU (gpu-independent
rows included), `import` merges rows not present by (kind, name, content hash). Versions are checked on the next launch, so a bundle from another OS,
driver or recompiler is dropped, not loaded. KK keys include the full GPU name, so M4 and M4 Pro bundles do not share metallibs.

### Measurements (Apple M4 Pro, macOS 25G83 / 26.6.2, Calculator3D captures A, B, C replayed with `agc_frame_replay`)

Machine load average 60-120 during the runs (14 cores, other agents), so times are noisy and pessimistic; they are real (wall) seconds
of the whole process.

| Sizes on disk (one title: the shaders of A, B, C) | before (loose files) | after (store) |
|---|---|---|
| KK layer: MSL (35, collected) / metallibs (35) / archive | 6012 KB / 544 KB / 2672 KB = 9.2 MB (3.2 MB without MSL) | 1.32 MB file (MSL 521 KB, metallibs 334 KB, archive 389 KB) |
| per-kind ratio (raw / stored) | | MSL 11.1x, SPIR-V 3.9x, vkcache 8.0x, archive 3.4-3.8x, metallib 1.4x |
| not routed yet, measured by `ingest` | SPIR-V disk cache 2.57 MB, VkPipelineCache 7.24 MB | 674 KB, 923 KB |

| Time | legacy loose files | store |
|---|---|---|
| cold first launch (A, empty cache), process | 2.14 s | 2.05 s (exit write-back 206 ms, first time 506 ms) |
| warm launch (A/B/C), process | 0.48-0.87 s | 0.37-0.91 s (interleaved, same load: no difference) |
| open + invalidate on an empty store | n/a | 10-16 ms |
| decompress all 35 metallibs into the run dir | n/a | 1.0-1.1 ms warm (3 ms first) |
| exit write-back, warm | n/a | 8-13 ms |
| `nps5_gpucache rebuild`, 35 metallibs, `--jobs 3` | `build_metallib.sh` 9.7 s | 6.7-13.0 s |

The first warm launch after the metallibs appear still builds the archive from metallib-sourced pipelines (2.3 s, up to 4.7 s under load),
as before. Pixels: all 10 replayed frames of A, B, C are byte-identical between loose files, store cold, store warm 1 and 3, and also equal the
captures' reference frames (`cmp`, max channel diff 0). Warm runs show 35 `[kk-metallib] hit` and 0 misses.

Invalidation demo (copy of the A/B/C store; the "OS update" is simulated by editing the recorded macOS version of the rows, the machine's OS
did not change): launch logs `stage change ( macos ): dropped 35 metallib 1 archive, kept 35 msl`; the 35 MSL rows keep their ids and
`created` time, `nps5_gpucache rebuild` then rebuilds only the 35 metallibs in 7.5 s. Dry runs with `check --stage`:
macos or Mesa/GPU bump drops metallib + archive (+ MSL for Mesa/GPU); Metal compiler drops only metallibs; recompiler drops MSL + archive and keeps metallibs.
Unit tests (`gpucache_store`, 100 % pass) cover each of these, untracked stages, prewarm order, trim, materialize and bundles.

Caveat: `warm` (usage) is inferred from the access time of the materialized libraries after the run (set behind mtime at startup); APFS does not
always update it for memory-mapped reads, so it is best effort. Prewarm order only matters when `APS5_GPUCACHE_MATERIALIZE_MB` caps the set.

## Prewarm (offline PSO cache) and async pipelines (nps5-prewarm)

Design follows nxnative's offline PSO model (ideas only, no code taken; MIT): archive keyed on its exact inputs, a prebuilt stamp so the
host skips the step, bounded batches so a compiler never grows past a couple of GB, runtime builds only what is missing.

- **Record.** Already done: `PipelineJournal` records every graphics recipe (shader identities + SPIR-V modules, vertex layout, descriptor
  layout keys, canonical state) and every compute pipeline, device-fingerprinted and checksummed; the pipeline store keys on the same data, so a
  changed shader or state is a different key. Corrupt or partial journal records are dropped (`droppedRecords`); the GPU store now verifies the
  content hash of every artefact on read and drops a damaged row (`[gpu-cache] corrupt ... dropped`), so a bit-flipped metallib/archive is rebuilt, not loaded.
- **Runtime replay** (unchanged mechanism): a pool of half the cores replays the journal at window attach. It now runs at **background QoS**
  (`APS5_PIPELINE_JOURNAL_QOS=utility|user-initiated` overrides); thread count `APS5_PIPELINE_JOURNAL_THREADS`.
- **Offline step** `tools/nps5-prewarm.sh <gpu-cache dir> [--jobs N] [--batch N] [--force]` (binary `nps5_prewarm`, `ninja nps5_prewarm`): no guest, one headless device.
  Pass 1 replays the journal (KosmicKrisp compiles, the store collects MSL), `nps5_gpucache rebuild --jobs N` builds every collected MSL to a metallib, pass 2
  replays again so the Metal 4 archive is built from the metallibs. Replay runs in bounded batches (`--range 0:k*N`, cumulative: earlier entries are ~1 ms archive hits that keep
  the archive complete, only the new ones compile; one process per batch, each under `gtimeout -s KILL`), jobs default to half the cores. `nps5_prewarm` renices itself (SIP
  binaries such as nice strip DYLD_LIBRARY_PATH). Stamp `prewarm.stamp` = SHA-256 of journal bytes, macOS build, CPU, ICD json + library, tool binary, batch size: a match skips the step. The store also
  invalidates by stage version (macOS build, Mesa, GPU), so an archive of another OS never loads. `nps5_gpucache verify` must pass before the stamp is written.
- **Shader-only precompile.** MSL depends on pipeline state (vertex layout, attachment formats), so a graphics shader without a recipe cannot be turned
  into an exact metallib; compute can. Every compute pipeline is journaled when first dispatched, so nothing is left over for compute. Not done: static decode of the 543 AGC shader headers in the eboot to pipelines (needs pair/state guesses; the journal pairs are the exact ones).
- **Async creation** (`APS5_ASYNC_PIPELINES=1`, default off): on a pipeline store miss the draw path queues the build to a pool
  (`APS5_ASYNC_PIPELINE_THREADS`, default half the cores, utility QoS; job owns deep copies of the canonical state and recompiled stages) and **skips the draw**
  (`prepareDraw` returns false) until the pipeline exists. Builds there are not counted as draw-time stalls. Feasible because the draw path already has a "nothing to draw" exit and
  the journal gives a self-contained rebuild recipe; cost: skipped draws can leave a few frames of missing geometry and, if skipped draws write targets that are sampled later, wrong pixels (UE does the same with PSO precaching).
  Counters: `[async-pipeline] exit: N queued, M built, K draws skipped`. Compile stage: `[shader-compile]` lines/total (RDNA to SPIR-V time of disk cache misses).

Measured (M4 Pro, machine under load, KK mesh install, background QoS):

| Calculator3D capture A (`agc_frame_replay`) | wall | pipeline create total | draw-time stall | pixels vs reference |
|---|---|---|---|---|
| cold (empty cache, OS Metal cache warm) | 2.18 s | 1073 ms | 1065 ms | identical |
| cold, OS Metal cache cold (`KK_MSL_CACHE_BUST`) | 5.7 s (replay 3.68 s) | 3434 ms | 3380 ms | identical |
| same, `APS5_ASYNC_PIPELINES=1` | 5.3 s (replay 0.198 s) | 2845 ms on the pool | 0 ms (6 draws skipped) | frames not compared |
| warm cache | 0.54-0.57 s | 10-13 ms | 0 | identical |
| prewarmed from the journal only (`nps5-prewarm.sh`, 14 s) | 0.54-0.91 s | 4-6 ms | 0 | identical |

PPSA30552 journal (284 recipes: 238 graphics, 46 compute) in a scratch copy, no game launched: cold compile of journal slice 0:30 with a cold OS cache 7.0 s wall / 8.3 s of
pipeline creation (avg 464 ms graphics, 111 ms compute); the same slice prewarmed 2.7 s wall / 53 ms of creation (avg 2.4 ms graphics). Full offline step with 1 job under
memory pressure: pass 1 about 200 s (36 batches of 8), 308 metallibs 432 s, pass 2 about 150 s, 711 store entries verified. Existing title logs (b2/logs/r8.err): the 10-22 s frame
gaps from frame 58 on contain no pipeline stall (all 75 stalls, 30 s total, precede frame 58) but coincide with shader-disk-cache misses (RDNA to SPIR-V recompiles); `[shader-compile]` now measures them.
Prewarm does not remove those: the SPIR-V cache key is built from the recompile request (layout, resource snapshot) that exists only when a draw registers the shader, and the journal records pipelines, not requests.

## Gaps

- Library front end (MSL -> AIR) is not app-persistable through the Metal 4 API; an evicted/new-OS Metal cache costs
  ~2.3 s for the largest shader and ~1 s of `vkCreateDevice` once. Solved by the prebuilt metallib path above.
- A real first-launch cold number could not be re-measured without clearing the shared `com.apple.metal` cache (3.4 GB,
  used by other apps); `KK_MSL_CACHE_BUST` only defeats the front-end cache.
- The archive is flushed only when something compiled (> 20 ms heuristic), at compiler release and exit; a crash
  loses that run's new pipelines (the VkPipelineCache is saved every 10 s). Archive re-serialisation after a partial
  miss was observed to keep earlier entries (hits are captured) but is not stress-tested.
- Not measured end to end in a game frame loop (needs the guest-address work in `nps5-gpuaddr`).


## Cold first launch: host-only GPU setup time (nps5-gpufu)

A title can bound its own first frame (Calculator3D polls `sceVideoOutGetFlipStatus` and throws after 5 s of
`sceKernelGetProcessTime`); with empty caches the first frame needs Vulkan device creation and the pipeline compile (2-3 s,
longer on a busy machine), which a console never pays. `GpuStallScope` / `GpuStallMicros()` (`Graphics/PipelineCreateStats`)
measure the wall time in which a draw-time pipeline creation (not journal prewarm) or `vkCreateDevice` was running
(overlapping scopes count once) and `AgcDriverStallMicros()` exports it. The nativeps5 runtime subtracts it from the
process-time clocks it gives the title, so the title's timeout does not count the compile. `APS5_PIPELINE_CREATE_DELAY_MS=<n>`
adds n ms to every draw-time creation (test aid): with 3000 the title times out without the credit and renders with it.


## Auto-prewarm, priorities, fallback, persisted cache (nps5-pipeline2)

**Key list vs binary cache.** `pipelines.journal` (recipes plus the SPIR-V of every stage) is the key list. Its device fingerprint is now the
vendor and device id only: the driver version and `pipelineCacheUUID` (KosmicKrisp hashes its whole build into it) are left out, so a KK
build change no longer throws the journal away. What depends on KK (Metal archive, metallibs, MSL, the VkPipelineCache blob) lives in the
`gpucache.sqlite` store and `vk-pipelines-*.bin`, which key on the driver and are invalidated by stage version. The journal is still stamped
with the recompiler version (its directory `<SourceVersion>/`): the SPIR-V in it is recompiler output and the journal holds no recompile
requests, so after a recompiler change the old journal cannot be rebuilt (a new one is recorded). That limit is real; fixing it needs the
RDNA code and resource capture of each stage in the journal.

**Stamp.** `<cache dir>/pipeline.stamp` = recompiler version, driver (info, version, UUID), hash of `MESA_KK_*`/`KK_*`/`kk_*` switches, macOS
build, GPU. Compared at device creation (`[pipeline-stamp] fresh | none yet | stale (changed: ...)`). Written when the journal replay finishes
without failures, so a killed rebuild stays stale and resumes (finished pipelines are archive hits).

**Build pool and priorities** (`PipelineJobs.hpp`). One process-wide pool, half the cores, utility QoS (`APS5_PIPELINE_JOURNAL_QOS`,
`APS5_PIPELINE_JOURNAL_THREADS`/`APS5_ASYNC_PIPELINE_THREADS`). Now: a draw's miss. Ahead: the first `APS5_PIPELINE_JOURNAL_AHEAD` (256) journal
entries (journal order is first-use order). Idle: the rest. `Finish(key)`: a queued job is run by the waiting thread, a running one is waited for,
so nothing compiles twice. Sync draws that miss a journalled pipeline use Finish; async draws Promote it to Now. Compute already waits on an
in-flight build of the same key.

**Fallback** (`APS5_ASYNC_PIPELINES=1`). On a miss: (1) a compiled pipeline of the same family (same shaders, formats, samples, vertex input,
layouts, blend enable/masks; differing in cull, front face, depth test/write/compare, bias, stencil ops, blend factors/constants) is used, the
closest by key bytes; (2) else, if every stage writes only render targets, the draw is skipped; (3) else it waits (compute, storage writers).
Counters: `[async-pipeline] exit: ... drew with a compiled variant ..., waited ...`.

**Persisted VkPipelineCache.** Already atomic (temp + rename), header with vendor/device/UUID and checksum. Added: size cap
(`APS5_VK_PIPELINE_CACHE_MAX_MIB`, 512; an over-cap file is not read, an over-cap blob is not written), removal of stale `.tmp.*` of killed saves,
tests (`agc_pipeline_cache_allocation_tests validation`: truncated, empty, flipped byte, foreign UUID/vendor/device/magic, over cap, stale temp).

**Scripts.** `tools/nps5-autoprewarm.sh <gpu-cache> --build-dir D` (checks `nps5-prewarm.sh --check`, rebuilds offline when stale; the stamp no
longer includes the journal bytes, only the configuration). Called from work/ppsa30552/boot/coh/{run,go}.sh (`AUTO_PREWARM=0` skips).
Tests: `pipeline_jobs_tests` (priority order, waiter runs job, waits for running job, cancel, stamp compare/torn file).

### Startup prebuild with progress (nps5-pipeline2)

Before the guest's first frame, `Driver::AttachWindow` sets the prebuild mode, creates the window device (whose journal replay starts at once, every
entry as Now, pool = all cores but two, at most 8), releases the GPU mutex and waits for the replay, logging `[prebuild] Compiling shaders  N / M`
every 2 s and putting `Compiling shaders  N / M  [####------]` in the window title (new optional `PresentationWindow::setStatus`, wired in
libSceVideoOut). Only the title text is drawn; a drawn bar would need a Vulkan quad in the presenter (not done). `NPS5_PREBUILD=0` skips;
`NPS5_PREBUILD_SECONDS` (120) bounds it, after which the game starts and the replay continues in the background. A warm cache shows
`[prebuild] 2 / 2 pipelines in 0.5 s ... (nothing to compile)`.

### Handoff

Switches: `NPS5_PREBUILD`, `NPS5_PREBUILD_SECONDS`, `APS5_ASYNC_PIPELINES=1` (fallback/skip, off by default), `APS5_PIPELINE_JOURNAL_AHEAD`,
`APS5_PIPELINE_JOURNAL_QOS|THREADS`, `APS5_VK_PIPELINE_CACHE_MAX_MIB`, `AUTO_PREWARM=0`, `AUTO_PREWARM_TIMEOUT`, `NPS5_GPU_CACHE`.

Done and tested: job pool and stamp (`pipeline_jobs_tests`), cache validation (`agc_pipeline_cache_allocation_tests validation`), journal survives a KK
build change (Calculator3D, kk-mesh then kk-gputime: `[pipeline-stamp] stale (changed: driver)`, journal loaded, replay rebuilt, stamp rewritten), prebuild
on homebrew, offline hook (`nps5-autoprewarm.sh` rc 0, 16 s on the Calculator3D cache).
NOT done: recompiler-change survival (journal has no recompile requests); drawn progress bar; the title measurements (cold / auto / warm, stall totals,
prebuild on vs off) and the fallback visual-error measurement (needs async runs on homebrew captures: `APS5_ASYNC_PIPELINES=1`, compare
frames against a sync run with `ps5recomp/tools/frame_compare.py`); the homebrew regress gate (`REGRESS_DIR=work/regress-pipeline2
NPS5_ANYPS5_BUILD=wt/anyps5-pipeline2/build-arm64 tools/regress.sh --quick`); no unit test of `familyState`/`findFallbackVariant` yet; no sync
Now-Finish test with a real Vulkan device.
Commands: build `cmake --build build-arm64 -j3 --target libs nps5_gpu nps5_prewarm nps5_gpucache`; bench drivers in work/pipeline2 (hb.sh, seq1.sh);
title: work/ppsa30552/boot/coh/go.sh (runs the auto-prewarm hook first; copy the cache with `cp -c` and use another KK ICD in a private workdir
via `NPS5_GPU_CACHE`).

## Prebuild2 (nps5-prebuild2, nxnative d9f05398 a1e155a3 72d00571 0a126ac7 540f7977 bf420fda as ideas)

- **Workers from Metal** (72d00571): `Graphics/include/MetalCompileTasks.hpp` reads `MTLDevice.maximumConcurrentCompilationTaskCount` (dlopen + objc runtime, no
  link). The startup prebuild pool and `nps5_prewarm` (`--print-workers`, default for `--threads`) use it, bounded by the core count and 2.5 GiB of RAM
  per worker (a KosmicKrisp compile holds about 2 GB); `APS5_PIPELINE_PREBUILD_THREADS` overrides. Was: cores-2, at most 8. M4 Pro (14 cores): 9.
- **Archive read-ahead / map instead of load** (d9f05398, a1e155a3): NOT ported, nothing to gain here. KosmicKrisp opens the archive with
  `newArchiveWithURL` (Metal maps it itself; there is no NSData copy to avoid), and the archive is materialized from `gpucache.sqlite` into the run
  directory just before, so its pages are in the file cache. Measured `[kk-archive] loaded` 4 ms for the 19 MB archive.
- **Compute and mesh coverage** (0a126ac7, bf420fda, 540f7977): already complete. The journal records every compute pipeline (PipelineLibrary) and every
  graphics pipeline including mesh-stage state (`state.stages.mesh`); only pipelines with a `tessellationCapture` (runtime-captured items) are not journaled,
  because they depend on a live capture object. The journal is itself the seed list (first-use order = Ahead priority).
- **Cache kept across a KosmicKrisp build change**: the journal already survived (device fingerprint = vendor+device). Now also the metallibs and the Metal
  archive: `GpuCacheStore::DependencyMask` no longer ties them to the Mesa stage. A metallib is named by a hash of its MSL text and options (plus KK's version string)
  and depends on the Metal compiler/macOS/GPU only; Metal validates archive entries itself, so a stale entry misses. Only the MSL rows and the
  VkPipelineCache blob (both KK output) are dropped. A KK build whose MSL is unchanged therefore needs no Metal compile on the next start; one that changes MSL
  rebuilds exactly the pipelines whose MSL changed. A recompiler change still moves to a new `<version>/` journal (no recompile requests in the journal).
- **nps5-prewarm.sh**: the stamp and the replay use the journal of this tool's recompiler version (`nps5_prewarm --print-version`), not `ls | head -1`
  (the first of several stale version directories); the version is part of the stamp. Fallback: newest journal by mtime.
