# AGENTS.md

nativeps5-anyps5 is the system-library component of nativeps5 (a fork of AnyPS5): reimplemented PS5 system
libraries (`libkernel`, `libc`, `libSceAgc`, `libSceAgcDriver` with its RDNA to SPIR-V shader recompiler,
VideoOut, audio, pad) that translated programs link against. On macOS arm64 the build
(`-DAPS5_HOST_ARM64=ON`) produces `libkernel.prx`, `libc.prx`, `libSceAgc.prx`, `libSceAgcDriver.prx` and
`libnps5_gpu.dylib`, the bridge the program calls (it also carries the host side of the Kyty back end). It is not an
emulator. Upstream targets Linux and Windows relinking; here the primary host is macOS arm64.

The project-wide rules are in https://github.com/computerguy526/nativeps5 [AGENTS.md](https://github.com/computerguy526/nativeps5/blob/main/AGENTS.md). Read it first. In short: the game-data rule (never game data or anything derived from a title in a commit, issue, PR or prompt; derived files live under `NPS5_SCRATCH`); the run safety rules (one GPU-using process on the machine, title runs only through `scripts/run-title.sh` of the entry repository, never raise `NPS5_SELF_MEM_LIMIT_MB`, no Metal counter heaps or GPU timestamp queries, no screenshots, kill only what you started); evidence for every speed or picture claim (`nps5 report`, A/B with interleaved runs, scene named, commits of every component); and git: the identity comes from git's configuration (never `--author`, `-c user.*` or `--no-verify`), no pushes to an upstream, and the pull request template states whether AI assisted.

## Upstream rules (still apply)

The rules are [CONTRIBUTING.md](CONTRIBUTING.md) and [CONVENTIONS](docs/dev/CONVENTIONS.md); these are the ones
agents break.

- No comments in code, except `#endif` and namespace ends.
- An export you cannot implement calls `NotImplemented_nid_no_patch(__func__)`; never return a made-up success.
- Implement the function for every input. Never special-case a value, ID or path seen in one title's trace (and
  no title-specific branches); if the general behaviour is unknown, throw and record it in
  `docs/dev/debt/<library>.md`.
- New third-party code is a submodule at `3rdparty/<name>` with a `.gitmodules` entry
  (`git submodule add <url> 3rdparty/<name>`), built from source with CMake. Never `find_package` or copy sources in.
  Never commit submodule contents or symlinks into `3rdparty/`.
- Known gaps go in `docs/dev/debt/<library>.md`; notes, reports and logs go in the pull request. Add no other files.
- Commit subjects and PR titles are Conventional Commits.
- Before starting a function run `python3 tools/pr_overlap.py --names <function>`; before pushing run
  `python3 tools/check_conventions.py --base origin/main`; before opening run
  `python3 tools/pr_overlap.py --branch <branch>`.
- A speed claim shows `tools/trace_digest.py` summaries of `main` and of the change from the same machine and scene.
- AGC, shader recompiler and VideoOut changes: run the same focused tests on `main` and the change with
  `ANYPS5_REQUIRE_VULKAN=1`, `ANYPS5_NO_SHADER_CACHE=1`, `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and
  `VK_LOADER_DEBUG=layer` via `ctest --test-dir build -R <pattern> -V`; confirm the loader inserted the layer, that
  the change adds no new `VUID-` errors (ctest can report `Passed` with errors logged), and use a layer at least as
  new as `3rdparty/Vulkan-Headers` (1.4.335+), or every GPU test fails on a spurious `pNext` VUID. On macOS the
  layer runs on KosmicKrisp; say which ICD.

Where nativeps5 differs: the pull request template is [.github/pull_request_template.md](.github/pull_request_template.md)
(its `AI assistance` section is answered honestly) and evidence is `nps5 report` output from the entry repository.
The template's sections are What and why, Evidence, Safety and AI assistance.

## Where things live

| Path | What |
|---|---|
| `core/libs/prx/` | the reimplemented libraries; `libSceAgcDriver/Graphics` holds the AGC driver, pipeline cache and `GpuCacheStore` |
| `core/libs/nps5/` | nativeps5 additions: `GpuBridge.cpp` (`libnps5_gpu`), `KytyHost.cpp`, tests |
| `core/shader`, `core/relinker`, `core/Decoder` | RDNA shader recompiler, the relinker, instruction decoding |
| `docs/dev/` | BUILD, CONVENTIONS, ARCHITECTURE, debt per library; `docs/nps5-*.md` are the nativeps5 design notes |
| `tools/` | PR checks, `trace_digest.py`, `select_tests.py`, gpucache and hw-oracle helpers |

## Build and test

```sh
cmake -S . -B build-arm64 -G Ninja -DAPS5_HOST_ARM64=ON -DCMAKE_BUILD_TYPE=Release   # submodules first; scripts/build.sh anyps5 does it
cmake --build build-arm64 -j 4 --target libSceAgc libSceAgcDriver libkernel libc nps5_gpu
cmake -S . -B build-relinker -DANYPS5_RELINKER_ONLY=ON && ctest --test-dir build-relinker --output-on-failure   # relinker only, no submodules
```

`.nps5-check-tests` builds and runs the CPU-only tests of the additions (`agc_no_init_allocator_tests`,
`agc_driver_runtime_buffer_spans_tests`, `agc_guest_bias_tests`, `agc_driver_memory_probe_tests`,
`gpucache_store_tests`, `pipeline_jobs_tests`). `tools/select_tests.py` picks the tests a change touches
(`docs/dev/BUILD.md`). Tests that need a GPU are not run by the checker; one GPU-using process at a time.

## Rules specific to this repository

- New behaviour is off by default behind an `APS5_*` / `NPS5_*` switch unless proven neutral or better; the entry
  repository's `docs/env_switches.tsv` lists them (`scripts/gen_env_switches.py`), and `tools/env_reference.py` is the
  upstream generator.
- `core/libs/nps5/KytyHost.cpp` is a C boundary: the ps5recomp runtime reaches `Nps5KytyHost*` through `dlsym`, so
  no C++ exception may leave those functions (return `ErrorInternal`), and a change to them bumps `Nps5KytyHostAbi`
  together with the Kyty and ps5recomp sides.
- `GpuCacheStore` (`gpucache.sqlite` per title, kinds spirv/msl/metallib/archive/vkcache) drops entries by stage
  version (recompiler, mesa, macos, metalc, gpu). Changing what a stage produces means bumping its version; a cache is
  derived from a title, so it lives under `NPS5_SCRATCH` and is never committed.
- No GPU timestamp queries or counter heaps (`APS5_PROFILE_PASSES`, `APS5_PROFILE_GPU` are forbidden on a title); GPU
  loops keep their caps (`KK_LOOP_CAP`) and watchdog.
- `.nps5-check-allow` lists large upstream or table files that are not dumps; do not add to it to dodge a finding.

Pull requests use `.github/pull_request_template.md` (it points at the entry repository's CONTRIBUTING.md; fill the evidence block with `nps5 report`, see docs/agents/evidence.md there). The entry repository's `scripts/check.sh --tree`, run from inside this checkout, must report 0 failures; this repository's `.nps5-check-tests` lists the CPU-only tests it runs.
