# nativeps5-anyps5

The PS5 system libraries and AGC graphics driver of [nativeps5](https://github.com/computerguy526/nativeps5):
[AnyPS5](https://github.com/boykopovar/AnyPS5)'s host libraries built as native arm64 macOS images
(`libkernel.prx`, `libc.prx`, `libSceAgc.prx`, `libSceAgcDriver.prx`, `libnps5_gpu.dylib`) that a natively
translated PlayStation 5 program loads. Every title run of nativeps5 needs them, with either graphics back end:
they provide the system libraries, and `libnps5_gpu.dylib` carries the host side of the bridge to the Kyty GPU
library (`core/libs/nps5/KytyHost.cpp`).

**Start at the entry repository**: https://github.com/computerguy526/nativeps5 has the build instructions
(`BUILDING.md`), the contribution rules (`CONTRIBUTING.md`, `AGENTS.md`), the pre-commit check and the
KosmicKrisp (Vulkan on Metal) patch series the AGC driver runs on. This repository is one of its components and
is normally cloned by its `scripts/fetch-components.sh` as `anyps5/`.

## Where this comes from

| Layer | Source | Licence |
|---|---|---|
| AnyPS5 | [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5), history up to commit `ea0c471cbc` (2026-11-14), unchanged, with its authors | GPL-2.0 |
| nativeps5 | the commits on top: arm64 macOS host build, AGC driver and shader recompiler work for Apple GPUs on KosmicKrisp, the bridge for the nativeps5 runtime, tests, tools and design notes (`docs/nps5-*.md`) | GPL-2.0 |

Upstream's own README is kept as [docs/AnyPS5-README.md](docs/AnyPS5-README.md); upstream's documentation under
`docs/dev` and `docs/user` is unchanged. Upstream's GitHub workflows are not carried (they release, publish pages
and label pull requests for upstream's repository). The KosmicKrisp patch series is not here: it lives in the
entry repository under `kosmickrisp/` (MIT), where the design notes' `mesa/` paths now point.

**No game data is here and none is accepted.** The media and image byte arrays in `core/libs/tests`
(`GuestAvPlayer.cpp`, `GuestAudiodec.cpp`, `GuestAjm.cpp`, `GuestVideodec2.cpp`, `GuestPngDec.cpp`) are upstream
AnyPS5's test vectors, unchanged: clips of at most one second and 100x60 pixels (70 bytes to 6 KB) that carry
their encoder's tags (FFmpeg `Lavf`/`Lavc`, x264, libvpx, LAME), produced with those tools for the tests and not
cut from any game. Not affiliated with or endorsed by Sony Interactive Entertainment or any game publisher.

## Build (Apple silicon, macOS 26)

```sh
brew install cmake ninja sdl2-compat zstd glslang freetype vulkan-headers vulkan-loader
git submodule update --init --depth 1 3rdparty/ZArchive 3rdparty/stb 3rdparty/libjpeg-turbo 3rdparty/zstd \
    3rdparty/Vulkan-Headers 3rdparty/SPIRV-Headers
cmake -S . -B build-arm64 -G Ninja -DAPS5_HOST_ARM64=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j 4 --target libSceAgc libSceAgcDriver libkernel libc nps5_gpu   # -> build-arm64/core/libs/libs/unpatched/
```

The entry repository's `scripts/build.sh anyps5` runs exactly this. The runtime of
[nativeps5-recomp](https://github.com/computerguy526/nativeps5-recomp) finds the build through
`NPS5_ANYPS5_BUILD=<path>/build-arm64`. Only the arm64 macOS host libraries are built and maintained in this
fork; upstream's relinker, Windows and Linux targets and web converter are untouched and not tested here.
`docs/nps5-arm64.md` describes the port.

## Licence

GPL-2.0 ([LICENSE](LICENSE)), as upstream. Credits: [NOTICE](NOTICE).
