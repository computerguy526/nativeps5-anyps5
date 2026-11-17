# nps5: native arm64 macOS host build

`-DAPS5_HOST_ARM64=ON` configures the host-side libraries and tools for arm64 on Apple silicon. The default
(x86_64) configuration is unchanged; `cmake -S . -B build` still configures as before.

## Configure and build

```
brew install sdl2 zstd glslang          # sdl2-compat also works; Vulkan/SPIRV headers come from the submodules
cmake -S . -B build-arm64 -G Ninja -DAPS5_HOST_ARM64=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 --target agc_frame_replay agc_shader_replay libs nps5_video_pad_smoke
```

Needs the usual submodules (3rdparty/{ZArchive,stb,libjpeg-turbo,zstd/lib headers,Vulkan-Headers,SPIRV-Headers}).
Do not commit anything under 3rdparty/.

Outputs: `build-arm64/core/libs/prx/libSceAgcDriver/{agc_frame_replay,agc_shader_replay}`,
`build-arm64/core/libs/libs/*.prx` (patched libc, libkernel, libSceAgcDriver, libScePngDec, libSceJpegDec) and
`nps5_video_pad_smoke`. Runtime: `SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib
VK_DRIVER_FILES=<KosmicKrisp icd json>`.

## What is wired

- `cmake/HostArm64.cmake`: brew SDL2 (`SDL2-static` interface target), brew libzstd (`libzstd_static`), SDK libz,
  CoreFoundation, brew glslang (`glslang`, `SPIRV`, `glslang-standalone`), ZArchive on the brew zstd, PNG/JPEG decoders.
  Vulkan-Headers/SPIRV-Headers stay on the submodule copies.
- `core/libs/CMakeLists.txt`: only the `APS5_HOST_ARM64_LIBS` allowlist is configured: libkernel, libSceAgcDriver, libSceAgc (its tests build too; `agc_guest_bias_tests` covers the builder under a nonzero bias),
  libScePngDec, libSceJpegDec, plus libSceVideoOut/libScePad/libSceUserService (merged from nps5-video) which only
  provide their static `nps5_*` targets (their .prx needs libSceSystemService etc., so they are not part of `libs`).
  Everything after the `libs` target (tests, other libraries) is skipped.
- libc: the guest C library files that cannot be compiled for arm64 (x87 long double, SSE, GuestCpu: Math, MathExact,
  Bessel, J0f, J1f, FloatBuiltins, FloatingPoint, Formatting, FormattingWide, ScanningWide, WideConversion, Errors,
  DinkumwareInternals, LegacySignals, Syslog, AllocatingStrings, SigvecExport) are filtered out in
  `libc/CMakeLists.txt`; `libc/src/specifics/arm64/HostLibcShims.cpp` supplies `snprintf` and the three registration
  hooks libkernel calls. Strings.cpp omits strtold/wcstold/_WS* on non-x86.
- libkernel: System/src/Exception.cpp (x86 register contexts) is replaced by `ExceptionArm64.cpp` (the two log-only
  debug exports). All other libkernel sources, including Thread.cpp and DirectMemory.cpp, compile and link as-is.

## Arch guards (x86 path unchanged)

| site | arm64 behaviour |
|---|---|
| libc AtomicBuiltins.cpp cmpxchg16b | `__atomic_compare_exchange_n` on 16 bytes (portable, seq_cst) |
| libkernel SyncOnAddress.cpp unaligned load | `__builtin_memcpy` |
| libkernel Thread.cpp `Aps5CallGuestEntry`, fldcw | plain call of the entry; fldcw skipped (NOT the guest-ABI entry) |
| libkernel Time.cpp ReadTsc / TSC frequency | `cntvct_el0` / `cntfrq_el0` |
| libkernel GuestTlsMacOs.cpp | AVX flag 0; `Aps5GuestTlsAllocate` is a runtime-fatal stub |
| libc JumpBuffer.cpp, libkernel SignalSets.cpp | setjmp/longjmp/sigsetjmp/siglongjmp are runtime-fatal stubs (x86-64 guest jmp_buf ABI) |
| libkernel UserContext.cpp | getcontext/setcontext/swapcontext use the existing NotImplemented exports |

## Result

`agc_frame_replay`, `agc_shader_replay`, `libs` and `nps5_video_pad_smoke` link natively with no undefined symbols.
`agc_shader_replay` recompiles the Calculator3D requests (69719 SPIR-V words for the big VS, same as x86), and
`agc_frame_replay <capture> --shaders` parses captures.

## Remaining / not done (memory and thread work)

These link but are not functional for arm64 guests; they belong to the guest memory model and thread/TLS work:

- libkernel DirectMemory.cpp and libc GuestArena.cpp (guest arena at 0x2_0000_0000, MAP_FIXED, 1000 GiB reservation):
  compiled unchanged, address model not ported.
- libkernel Thread.cpp: guest thread entry, gs/fs TLS (GuestTlsMacOs.cpp stub), signal-mask and x87 control word
  handling are x86 assumptions; `Aps5CallGuestEntry` is a plain call.
- Runtime-fatal stubs listed above (jmp_buf, sigsetjmp, TLS allocate), unsupported ucontext, no
  `sceKernelInstallExceptionHandler`/`RaiseException`, no guest long double or guest printf/math in libc.

## Frame replay on arm64

`agc_frame_replay <capture>` now replays natively on arm64 macOS. The capture's memory (guest `0x2_0000_0000`...) is
mapped at `guest + bias` inside a 2^40-byte host reservation and the driver translates every guest address through
`GuestAddr` (docs/nps5-gpu-addr.md). Run with
`DYLD_LIBRARY_PATH=/opt/homebrew/lib VK_DRIVER_FILES=<KosmicKrisp icd json> agc_frame_replay <capture dir>`
(`--frames <dir>` writes the replayed and reference frames as BMP, `--repeat N` replays N times,
`--cold-pipelines` ignores the pipeline journal).

Results, Apple M4 Pro, KosmicKrisp arm64 ICD, captures `work/s3/rosetta/captures_old/{A,B,C}` (3 frames each, 1920x1080):

| | A | B | C |
|---|---|---|---|
| steady-state frame time, `--repeat 40` (p50 / p90 / max, wall) | 1.27 / 1.70 / 1.79 ms | 1.20 / 1.61 / 1.71 ms | 1.25 / 1.80 / 2.01 ms |
| CPU per frame (process user+sys, repeat 40 minus repeat 1) | 2.5 ms | 5.9 ms | 4.6 ms |
| first pass, cold (empty shader/pipeline cache) | 0.495 s | 0.511 s | 0.521 s |
| first pass, warm (second process) | 0.033 s | 0.031 s | 0.035 s |
| whole process, cold / warm | 1.03 / 0.85 s | 1.01 / 0.87 s | 1.01 / 0.89 s |
| frames compared with the capture's x86 reference | 2 | 4 | 4 |
| exact-match pixels / max abs diff / PSNR | 100 % / 0 / inf | 100 % / 0 / inf | 100 % / 0 / inf |

The capture's reference frames are the Rosetta x86 renderer's output (`work/s3/rosetta/refframes/*.raw` are the same
bytes). Against `work/s3/rosetta/frames_x86/*.bmp`, which are frames 0 to 2 of one live x86 run: replay A frames 0 and 1
equal `frame_000.bmp` and `frame_001.bmp` (100 %, max 0). Captures B and C were taken later in the title, so those BMPs
are different frames (B: 78.8 to 79.3 % exact, max 73, PSNR 27.9 dB; C: 76.3 to 77.1 %, max 74, PSNR 28.2 to 28.4 dB);
their same-frame references are the embedded ones above. "Wall" frame time is submit to flip with the GPU work included;
GPU time is not instrumented separately. Cold versus warm is the shader recompile and pipeline creation (about 0.46 to
0.49 s for the three captures' shaders) against the pipeline journal replay; the warm process still pays about 0.37 s of
journal replay and device start-up before the first frame.

Caveats: the replay does not place Host-kind capture ranges (pages outside the arena); none are GPU-visible in A, B
or C, and a GPU-visible one is fatal. Hardware write watching and host-import write protection are not available on this
path (`write-watch ... no`); imports stay watched by comparison.
