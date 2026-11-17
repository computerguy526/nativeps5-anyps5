# nps5: libSceVideoOut / libScePad / libSceUserService on arm64 macOS

Status: all three compile natively (clang -arch arm64, macOS SDK) with **no source changes**. The
tree already carries the macOS paths: the window is SDL2 with `SDL_WINDOW_VULKAN`
(`DisplayWindow.cpp`, same as the x86 macOS build; Win32 subclassing is `_WIN32`-guarded), pad
input is SDL GameController events collected in `VideoOut/src/PadInput.cpp` and `InputMapping.cpp`
and published into `libScePad` (`Pad::` state), user IDs come from the self-contained
`libSceUserService/Export.cpp`. `APS5_VABI` is empty off Windows, so the exports use AAPCS64. No
x86 intrinsics, inline asm or sysv_abi in these three directories (grep clean).

## Dependency map

- libSceVideoOut: SDL2 (window, `SDL_Vulkan_*`, events), AgcDriver (`AgcDriver*_nid_postfix`:
  attach window, present buffer/clear, flip, loader lock), libScePad state, libSceMouse /
  libSceKeyboard / libSceIme (input publishing), AppMetadata (title/icon), Decoder::Png (icon),
  libkernel Time (`TimedWait.cpp`, compiled into the VideoOut lib).
- Vulkan swapchain hand-off: VideoOut builds `AgcDriver::PresentationWindow` (surface creation via
  `SDL_Vulkan_CreateSurface`, drawable size) and hands it to AgcDriver; AgcDriver owns the swapchain.
- libScePad: only `Pad::` state (`PadState.cpp`) and guest copy helpers; fed by VideoOut.
- libSceUserService: libSceSystemService (`sceSystemServiceParamGetInt`), guest copy helpers.

## Build

`tools/nps5_video_arm64_build.sh [dir]` compiles every source and makes `libSceUserService.a`,
`libScePad.a`, `libSceVideoOut.a`, `libSceVideoOutDeps.a` (Mouse+Keyboard+Ime sources). It needs
`AGC_GEN` = a configured AnyPS5 `build/core/libs` for the two generated headers
(`TraceRevision.hpp`, run-report). Artifacts used here: `build-video/` (gitignored).
Sizes: VideoOut 564 KB, Pad 153 KB, UserService 277 KB, Deps 152 KB.

CMake (top-level untouched): each library's own CMakeLists gained EXCLUDE_FROM_ALL static targets,
verified by a full `cmake -G Ninja` configure:

| target | defined in | content |
|---|---|---|
| `nps5_UserService` | libSceUserService/CMakeLists.txt | Export, InventoryExports, Inventory1360Exports, LegacyExports |
| `nps5_Pad` | libScePad/CMakeLists.txt | Export, InventoryExports, PadState, HostVolumeState |
| `nps5_VideoOut` | libSceVideoOut/CMakeLists.txt | 16 src/*.cpp + Unimplemented + InventoryExports + libkernel/Time/TimedWait |
| `nps5_VideoOutDeps` | libSceVideoOut/CMakeLists.txt | libSceMouse, libSceKeyboard, libSceIme sources |
| `nps5_video_pad_smoke` | libSceVideoOut/CMakeLists.txt | host smoke test |

(CMake still forces x86_64 on macOS at the top level, so the arm64 objects here were built with the
script; the link agent's arm64 option will make the CMake targets produce the same objects.)

## Undefined symbols (`nm -u`)

Raw `nm -u`: VideoOut 387, Pad 83, UserService 71, mostly libc++/libSystem and 81 `_SDL_*`.
After linking against SDL2, libc++, CoreFoundation/Cocoa, the three libs + Deps leave **65** names:

1. **Provided by existing arm64 AnyPS5 objects (expected), 53**: `AgcDriver*_nid_postfix` (24),
   `ComputeContainSize`, `Equeue*`, `Libc*`, `Aps5RunOnMainThread`, `ResolvePath`, `GuestCopyIn/Out`,
   `CopyGuestString`, `NotImplementedAt`, `GetApp*/HasAppIcon`, `KernelWakeSleepers`,
   `PreciseKernelTimerSleep`, `sceKernelPwrite`, and similar. Just add `rosetta/arm64obj` to the link.
2. **Truly missing (no arm64 object yet), 12**:
   - libkernel File/Time: `sceKernelOpen/Close/Read/Write/Lseek/Stat`, `sceKernelGetFsSandboxRandomWord`,
     `sceKernelGetProcessTime`, `sceKernelGetProcessTimeCounter`, `sceKernelReadTsc` (File fd layer, Time.cpp; already on the audit's B/A lists, owned by the link/kernel agents).
   - `sceSystemServiceParamGetInt`: libSceSystemService (`Export.cpp`; not compiled here).
   - `Decoder::Png::Decode`: `core/Decoder/Png/src/Png.cpp` (needs `stb_image_write.h`, not present in this checkout's 3rdparty; the smoke test stubs it, the icon is optional).
3. Mouse/Keyboard/Ime exports (`Ime*`, `Keyboard*`, `ModalDialog*`, `Mouse*`, 14 names) are
   resolved by `nps5_VideoOutDeps`; they were missing without it.
4. SDL (`_SDL_*`), libc++, libSystem, CoreFoundation: host libraries (`-lSDL2 -lc++ -framework CoreFoundation`).

Linking everything plus the full arm64obj set still leaves 145 names; that is the known DirectMemory/
File/Pthread cascade of the existing objects, not caused by these libraries.

## Smoke test (host, no guest code)

`core/libs/prx/libSceVideoOut/smoke/VideoPadSmoke.cpp` (+ `HostStubs.cpp`, stubs for the kernel/AgcDriver
symbols above). Uses the real `DisplayWindow` (SDL window, `SDL_WINDOW_VULKAN`), creates a Vulkan
instance/swapchain through the loader on the KosmicKrisp ICD, clears and presents 30 frames, and calls
`sceUserServiceInitialize/GetInitialUser`, `scePadInit/Open/ReadState`.

```
VK_DRIVER_FILES=work/s3/install/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib ./smoke
user: init=0 initial=0 id=268435456 | pad: init=0 handle=1 read=0 connected=1 buttons=0x0
window ok, drawable 1814x1020
vulkan device: Apple M4 Pro (api 1.4)
surface: 5 formats, first fmt=50, extent 1814x1020, usage=0x8009f
presented 30 cleared frames
SMOKE OK
```

Not covered: `sceVideoOut*` exports and the AgcDriver present path (`AgcDriverPresentBuffer`) need the
full AgcDriver + libc/kernel link, which is blocked on DirectMemory/Thread (see REPORT S3b).

## What the link agent must wire in

Objects/libs: `libSceVideoOut.a libSceVideoOutDeps.a libScePad.a libSceUserService.a` (or the
`nps5_*` targets) + existing arm64obj; host libs `-lSDL2 -lc++ -framework CoreFoundation -framework Cocoa`;
set `SDL_VULKAN_LIBRARY` / `VK_DRIVER_FILES` at run time. Still needed from others: libkernel File+Time
objects, libSceSystemService (`sceSystemServiceParamGetInt`), `Decoder::Png` (or a stub), and the
generated headers from `agc_trace_revision`.
