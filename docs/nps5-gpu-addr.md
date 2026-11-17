# nps5: guest (GPU-visible) addresses in the AGC driver

nativeps5 runs titles in **base mode**: `host = guest + bias` (docs/m1/memory.md, section 8). The AGC driver used to
treat every GPU-visible guest address (PM4 words, registers, descriptors, shader headers, command buffers, labels) as a
host pointer. This branch (`nps5-gpuaddr`) adds one choke point and routes the driver through it. With the default bias
of 0 every function below is the identity, so the x86 configuration behaves as before.

## The choke point

`core/libs/prx/libSceAgcDriver/Execution/include/GuestAddr.hpp` (state and the out-of-line functions live in
`Execution/src/GuestMemory.cpp`).

| API | Meaning |
|---|---|
| `AgcSetGuestBias(int64_t)` / `AgcGetGuestBias()` (C linkage) | set once by the host runtime (`g_nps5_arena.bias`); default 0 |
| `AgcSetGuestArena(lo, hi)` | guest range the checked variants accept (default `[0, 2^40)`) |
| `AgcSetGuestScratch(lo, bytes)` | guest range reserved for driver-owned memory that needs a guest address (below) |
| `GuestAddr::GuestToHost(gpu_va, len)` | guest address to host `void*`; NULL stays NULL; no validation |
| `GuestAddr::HostPtr<T>(gpu_va)` / `HostAddress(gpu_va)` | typed pointer / integer forms of the same |
| `GuestAddr::GuestToHostChecked(gpu_va, len)` / `HostPtrChecked<T>` | fails loudly (`std::runtime_error` naming address and arena) when the range is null, wraps or is outside the arena |
| `GuestAddr::HostToGuest(ptr)` / `GuestAddress(host)` | the reverse direction |
| `GuestAddr::Scratch` | driver-owned block with a guest address (see "Host to guest") |
| `GuestMemory::CheckHostRange` / `AccessibleHost` | validate a *host* pointer (an HLE argument the thunk already translated) |

### Address domains (the rules the code follows)

* A `std::uint64_t` address in the driver is a **guest address**. The page-state cache and the host OS/libc APIs
  (`GuestWriteWatch`, `GuestArena`, `DirectMemoryAliased`, `mach_vm_region`, `mmap`) are keyed by **host** address, and are
  entered through `GuestAddr::HostAddress` (and their callbacks leave through `GuestAddress`).
* `GuestMemory::CheckRange/Accessible(const void*)` and the label store take *guest-valued* pointers; they translate
  inside. Dereference only after `GuestAddr::HostPtr`.
* `GuestAllocations` (the registry of GPU-visible ranges; `Range::address`, `Lease`, `Mutation::Add`) is keyed by
  **guest** address. Its producers (the host runtime, `agc_frame_replay`, `Lifecycle.cpp`) register guest-valued pointers.
  `Execution/src/GuestMemory.cpp` translates when it forwards registry events to the host-keyed page-state cache.
* Export arguments: `const Packet*` and other HLE descriptor pointers are host pointers (translated by the thunk);
  `Packet::addr`, `const Shader*` and the pointers inside `Shader`/`ShaderUserData` are GPU-visible and stay
  guest-valued (the thunk generator must not translate them). Inside `Driver::Submit` the command words are read through a
  host pointer made once from `Packet::addr`.
* Silent-miss hazard: a missed translation reads whatever lives at the *guest* number in the host process (on macOS
  `0x2_0000_0000` is the dyld shared cache, readable). The replay proves the converted paths by image comparison.

## Host to guest

* Labels, fences, semaphores and waited values are read/written through guest addresses (`HostPtr`); nothing the driver
  writes into guest memory contains a host address.
* Addresses the driver hands to shaders (`HostImport::base`, the BDA page table, descriptors) are guest values; the
  device addresses stay separate (`HostImport::address`).
* Driver-owned host memory that a shader or `GuestMemory` call must name (the global data share, the float and
  float16 probe buffers) cannot have a 48-bit guest name when it lives below the bias, so it is allocated from the scratch
  window with `GuestAddr::Scratch`. With bias 0 `Scratch` is ordinary heap memory (guest == host). (Before this, the float
  probes silently read `0xdeadbeef` under a bias and the flush decision was made on nothing.)
* Callbacks from host-keyed libraries (`ResolveLabelTrap`, `PrepareHostRead/Write`, write-watch run callbacks) convert
  their host ranges with `GuestAddress` before touching driver state.

## Native replay (`agc_frame_replay`, macOS)

`reserveGuestWindow` maps one `PROT_NONE` window of 2^40 bytes at the first free multiple of 2^40 (2^40 preferred; a
hint mapping without `MAP_FIXED`, so an occupied slot is released and the next one tried), calls `AgcSetGuestBias`,
`AgcSetGuestArena` and `AgcSetGuestScratch` (scratch at guest `0xF0_0000_0000`, 256 MiB), and maps each captured range at
`guest + bias` with `MAP_FIXED` **only** inside that window and **only** over still-inaccessible reservation pages
(`mapGuestRange` checks with `mach_vm_region` and refuses anything else). `--bias <host base>` forces a slot;
`--no-bias` keeps the old identity placement. See docs/nps5-arm64.md for the results.

## Converted sites

198 converted lines (every added line using `GuestAddr::` in the diff; 186 in the driver, 12 in
`tools/FrameReplay.cpp`). Paths are relative to `core/libs/prx/libSceAgcDriver/`; line numbers are for the commit that
added this file.

| site | kind |
|---|---|
| Execution/include/GuestMemory.hpp:25 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/include/GuestMemory.hpp:30 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/DisplayBuffer.cpp:246 | guest->host pointer (CPU read of guest data) |
| Execution/src/DisplayBuffer.cpp:271 | guest->host pointer (CPU read of guest data) |
| Execution/src/DisplayBuffer.cpp:289 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Dispatch/Dispatch.cpp:79 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Dispatch/VariantValidation.cpp:75 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Lifecycle.cpp:91 | bias / arena check |
| Execution/src/Driver/Lifecycle.cpp:92 | bias / arena check |
| Execution/src/Driver/Lifecycle.cpp:93 | guest->host mapping (inside the reservation only) |
| Execution/src/Driver/Lifecycle.cpp:98 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Lifecycle.cpp:103 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Lifecycle.cpp:104 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Lifecycle.cpp:109 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Lifecycle.cpp:112 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Memory/BufferCopy.cpp:222 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/BufferCopy.cpp:251 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/BufferCopy.cpp:252 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/BufferCopy.cpp:253 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/BufferCopy.cpp:272 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/BufferCopy.cpp:273 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/CaptureValidation.cpp:215 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Memory/WriteEvidence.cpp:144 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Queues/Submission.cpp:232 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Queues/Submission.cpp:249 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Queues/Submission.cpp:302 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Queues/Submission.cpp:483 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Queues/Submission.cpp:484 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Queues/Submission.cpp:693 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Queues/Submission.cpp:703 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Queues/Submission.cpp:704 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Queues/Submission.cpp:755 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Driver/Queues/Submission.cpp:770 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Queues/Submission.cpp:1029 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Queues/Submission.cpp:1030 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:646 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:658 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:672 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:697 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:2204 | guest->host pointer (CPU read of guest data) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:2655 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:2665 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:2667 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Shaders/ShaderRegistry.cpp:2735 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/Driver/Synchronization/WaitMemory.cpp:42 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Synchronization/WaitMemory.cpp:161 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Synchronization/WaitMemory.cpp:419 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Driver/Synchronization/WaitStatistics.cpp:56 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Float16ConversionProbe.cpp:41 | driver-owned memory given a guest address (scratch window) |
| Execution/src/Float16ConversionProbe.cpp:78 | driver-owned memory given a guest address (scratch window) |
| Execution/src/Float16ConversionProbe.cpp:79 | driver-owned memory given a guest address (scratch window) |
| Execution/src/FloatFlushProbe.cpp:183 | driver-owned memory given a guest address (scratch window) |
| Execution/src/FloatFlushProbe.cpp:193 | driver-owned memory given a guest address (scratch window) |
| Execution/src/FloatFlushProbe.cpp:194 | driver-owned memory given a guest address (scratch window) |
| Execution/src/FloatFlushProbe.cpp:195 | driver-owned memory given a guest address (scratch window) |
| Execution/src/FrameRecorder.cpp:113 | guest->host pointer (import / alias / map) |
| Execution/src/FrameRecorder.cpp:389 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/FrameRecorder.cpp:403 | guest->host pointer (CPU read of guest data) |
| Execution/src/FrameRecorder.cpp:413 | guest->host pointer (CPU read of guest data) |
| Execution/src/FrameRecorder.cpp:497 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/FrameRecorder.cpp:694 | guest->host pointer (CPU read of guest data) |
| Execution/src/GuestMemory.cpp:176 | driver-owned memory given a guest address (scratch window) |
| Execution/src/GuestMemory.cpp:177 | other |
| Execution/src/GuestMemory.cpp:178 | bias / arena check |
| Execution/src/GuestMemory.cpp:179 | other |
| Execution/src/GuestMemory.cpp:946 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:1191 | bias / arena check |
| Execution/src/GuestMemory.cpp:1192 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:1193 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:1256 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:1258 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:1398 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:1419 | guest->host pointer (CPU read of guest data) |
| Execution/src/GuestMemory.cpp:1435 | guest->host pointer (CPU read of guest data) |
| Execution/src/GuestMemory.cpp:1511 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:1625 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:1636 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:2044 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:2045 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:2137 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:2403 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:2512 | guest->host pointer (import / alias / map) |
| Execution/src/GuestMemory.cpp:2698 | guest->host address for an OS/host API |
| Execution/src/GuestMemory.cpp:2699 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:2700 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:2769 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:2771 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:2802 | guest->host key for the write-watch / DirectMemory host API |
| Execution/src/GuestMemory.cpp:3394 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:3517 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/GuestMemory.cpp:3556 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3617 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3639 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3694 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3705 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3712 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/GuestMemory.cpp:3771 | guest->host pointer (CPU read of guest data) |
| Execution/src/GuestMemory.cpp:3834 | guest->host pointer (CPU read of guest data) |
| Execution/src/GuestMemory.cpp:3880 | guest->host pointer (import / alias / map) |
| Execution/src/GuestMemory.cpp:3900 | guest->host pointer (CPU read of guest data) |
| Execution/src/Pm4.cpp:58 | bias / arena check |
| Execution/src/Pm4.cpp:59 | driver-owned memory given a guest address (scratch window) |
| Execution/src/Pm4.cpp:75 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Pm4.cpp:81 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Execution/src/Pm4.cpp:666 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Execution/src/Pm4.cpp:1184 | guest->host pointer (CPU read of guest data) |
| Execution/src/ShaderMemory.cpp:388 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/ShaderMemory.cpp:447 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/ShaderMemory.cpp:467 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/ShaderMemory.cpp:577 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/ShaderMemory.cpp:723 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/ShaderMemory.cpp:724 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/VulkanDevice.cpp:3547 | guest->host pointer (CPU read of guest data) |
| Execution/src/VulkanDevice.cpp:3548 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/VulkanDevice.cpp:3558 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/VulkanDevice.cpp:3559 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Execution/src/VulkanDevice.cpp:3657 | guest->host pointer (CPU read of guest data) |
| Execution/src/VulkanDevice.cpp:3999 | guest->host pointer (CPU read of guest data) |
| Execution/src/VulkanDevice.cpp:5156 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DccMetadata.cpp:334 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DccMetadata.cpp:371 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DccMetadata.cpp:513 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DccMetadata.cpp:628 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DepthSurface.cpp:814 | guest->host pointer (CPU read of guest data) |
| Graphics/src/DepthSurface.cpp:839 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/DepthSurface.cpp:1790 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/DepthSurface.cpp:2740 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/DepthSurface.cpp:2783 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/DepthSurface.cpp:2879 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/DepthSurface.cpp:4196 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Draw.cpp:1571 | guest->host pointer (CPU read of guest data) |
| Graphics/src/Draw.cpp:1954 | guest->host pointer (CPU read of guest data) |
| Graphics/src/Draw.cpp:2150 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Draw.cpp:2776 | guest->host pointer (CPU read of guest data) |
| Graphics/src/GuestBufferMemory.cpp:755 | guest->host pointer (import / alias / map) |
| Graphics/src/GuestBufferMemory.cpp:1109 | guest->host address for an OS/host API |
| Graphics/src/GuestBufferMemory.cpp:2477 | guest->host key for the arena host API |
| Graphics/src/GuestBufferMemory.cpp:2478 | guest->host key for the arena host API |
| Graphics/src/GuestBufferMemory.cpp:3278 | guest->host pointer (CPU read of guest data) |
| Graphics/src/GuestBufferMemory.cpp:4509 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4511 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4514 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4530 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/GuestBufferMemory.cpp:4552 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Graphics/src/GuestBufferMemory.cpp:4553 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4585 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Graphics/src/GuestBufferMemory.cpp:4586 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4588 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/GuestBufferMemory.cpp:4590 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/GuestBufferMemory.cpp:4627 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Graphics/src/GuestBufferMemory.cpp:4628 | CPU read/atomic of guest memory (label, fence, semaphore, wait value) |
| Graphics/src/GuestBufferMemory.cpp:4630 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/GuestBufferMemory.cpp:4640 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/GuestBufferMemory.cpp:5892 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/GuestBufferMemory.cpp:5972 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/GuestBufferMemory.cpp:6022 | guest->host pointer (CPU read of guest data) |
| Graphics/src/GuestBufferMemory.cpp:7050 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/MultisampleTarget.cpp:1219 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/MultisampleTarget.cpp:1228 | guest->host pointer (CPU read of guest data) |
| Graphics/src/Recorder.cpp:860 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Recorder.cpp:888 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Recorder.cpp:996 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Recorder.cpp:1017 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Recorder.cpp:3591 | guest->host pointer (CPU read of guest data) |
| Graphics/src/Recorder.cpp:3603 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Recorder.cpp:3653 | guest->host key for the arena host API |
| Graphics/src/Recorder.cpp:3677 | guest->host key for the arena host API |
| Graphics/src/ResidentMirror.cpp:195 | guest->host pointer (CPU read of guest data) |
| Graphics/src/ResidentMirror.cpp:196 | guest->host pointer (CPU read of guest data) |
| Graphics/src/ResidentMirror.cpp:293 | guest->host key for the arena host API |
| Graphics/src/ResidentMirror.cpp:304 | guest->host key for the arena host API |
| Graphics/src/ShaderResources.cpp:4176 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/ShaderResources.cpp:4739 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/ShaderResources.cpp:5503 | guest->host pointer (CPU read of guest data) |
| Graphics/src/ShaderResources.cpp:5520 | guest->host pointer (CPU read of guest data) |
| Graphics/src/ShaderResources.cpp:5545 | guest->host pointer (CPU read of guest data) |
| Graphics/src/ShaderResources.cpp:6989 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/ShaderResources.cpp:6997 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/ShaderResources.cpp:7005 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Graphics/src/Texture.cpp:4942 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Graphics/src/Texture.cpp:4943 | host->guest (driver/host address named in guest domain, or host callback range back to guest) |
| Graphics/src/Texture.cpp:5581 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/Texture.cpp:7014 | guest->host key for the write-watch / DirectMemory host API |
| Graphics/src/UnitShadow.cpp:356 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| Submit/src/Dcb.cpp:64 | guest->host pointer (CPU read of guest data) |
| Submit/src/Dcb.cpp:153 | guest->host pointer (CPU read of guest data) |
| tools/FrameReplay.cpp:165 | guest->host pointer (CPU read of guest data) |
| tools/FrameReplay.cpp:339 | guest->host address for an OS/host API |
| tools/FrameReplay.cpp:360 | guest->host address for an OS/host API |
| tools/FrameReplay.cpp:409 | guest->host pointer (import / alias / map) |
| tools/FrameReplay.cpp:451 | guest->host pointer (import / alias / map) |
| tools/FrameReplay.cpp:478 | guest->host pointer (import / alias / map) |
| tools/FrameReplay.cpp:726 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| tools/FrameReplay.cpp:745 | guest->host pointer (CPU read of guest data) |
| tools/FrameReplay.cpp:790 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| tools/FrameReplay.cpp:832 | guest->host pointer (CPU read of guest data) |
| tools/FrameReplay.cpp:863 | CPU copy/compare of guest memory (shader, descriptor, metadata, index/vertex data, DMA, indirect args) |
| tools/FrameReplay.cpp:1034 | guest->host pointer (CPU read of guest data) |

## libSceAgc (the guest-side command-buffer builder)

Convention (`prx/libSceAgc/include/GuestPtr.hpp`, `Agc::Guest::{Host, Guest, Address}`; identity with bias 0):

* A pointer **parameter** of an exported function is a **host** pointer: the runtime's thunk translated it (`Ptr`), and the
  library dereferences it directly (`cmd`, `registers`, `data`, marker strings, `Shader*` headers, `CommandBuffer*`).
* A pointer the library **encodes into GPU-visible data** is a **guest** value: PM4 address words (`Address(ptr)`), Shader
  header fields it resolves, CommandBuffer cursors it advances, and what it stores through an out-slot
  (`sceAgcCreateShader` `*dst`, `sceAgcGetSemaphoreLabel` `*label`, `sceAgcGetDataPacketPayloadAddress/Range`). The thunk
  must **not** translate those slots back (they are raw guest slots, not `PtrSlot`).
* A pointer **read out of** GPU-visible data is a guest value and is dereferenced only through `Host()`: the
  `CommandBuffer` cursors (`bottom/top/cursor_up/cursor_down`; `Allocate` advances them in the guest domain and returns the
  host pointer of the packet), the `Shader` header's `user_data/code/cx_registers/sh_registers/specials/*_semantics` and the
  `ShaderUserData` tables (`Shader/include/ShaderView.hpp` builds a host-pointer snapshot; the driver gets
  `view.guest()`), the driver's static table (`StaticBuffer.cpp`).
* Integer address arguments (`uint64_t dst_address`, `src_address_or_immediate`, `sceAgcDcbDmaData` addresses) are guest
  values by construction and are copied into the packet unchanged.
* Library-owned structures that the title or GPU reads live in driver scratch memory (`GuestAddr::Scratch`, which has a
  guest address) with guest-valued internal pointers: the register-defaults tables (`Command/src/RegisterDefaults.cpp`) and
  fused geometry programs (`Misc/src/ShaderFusion.cpp`). They are never freed (process lifetime).
* The `CommandBuffer::callback` / `user_data` pair is guest code and data and is passed through untouched; the callback
  installs guest-valued cursors.

Converted sites (file, lines; every use of `Agc::Guest::` outside tests):

| file (under `core/libs/prx/libSceAgc/`) | lines |
|---|---|
| Acb/src/Dispatch.cpp | 15 |
| Acb/src/Sync.cpp | 15 |
| Cb/src/Branch.cpp | 19, 20, 21, 35, 36 |
| Cb/src/Sync.cpp | 34, 66 |
| Command/src/Control.cpp | 7, 23, 34 |
| Command/src/Memory.cpp | 48, 54, 89 |
| Command/src/Packet.cpp | 76, 119, 157, 166 |
| Command/src/RegisterDefaults.cpp | 126, 128, 142 |
| DcbDraw/src/DrawIndexed.cpp | 14, 52, 87, 88 |
| DcbDraw/src/DrawNonIndexed.cpp | 26 |
| DcbDraw/src/Instancing.cpp | 23 |
| DcbDraw/src/Stats.cpp | 18 |
| DcbFlow/src/Memory.cpp | 20 |
| DcbFlow/src/Sync.cpp | 15, 36 |
| Misc/src/PacketInfo.cpp | 25, 27, 41, 47 |
| Misc/src/Semaphore.cpp | 66 |
| Misc/src/ShaderFusion.cpp | 292, 320, 324, 326, 329, 330, 331, 332, 333, 334, 343, 356, 358, 370, 387, 389, 411, 426 |
| Misc/src/StaticBuffer.cpp | 19 |
| Patch/src/Branch.cpp | 15, 35 |
| Patch/src/Jump.cpp | 15 |
| Patch/src/QueueEndOfPipeAction.cpp | 15 |
| Patch/src/WaitRegMem.cpp | 50 |
| Shader/include/ShaderUtils.hpp | 18 |
| Shader/src/CreateShader.cpp | 32, 48, 56, 58, 64, 65 |
| Shader/src/GsOversubscription.cpp | 69 |
| Shader/src/GsPrimPayload.cpp | 25 |
| Shader/src/InterpolantMapping.cpp | 70, 139 |
| Shader/src/LinkShaders.cpp | 18 |
| Shader/src/PrimState.cpp | 38 |

`FrameRecorder` (live capture, `Execution/src/FrameRecorder.cpp`, lines 389, 657, 680, 722, 759, 799, 808, 809): the macOS region walk is
`AgcDriver::AppleWalkGuestRegions(guest_first, guest_end)`: it asks `mach_vm_region_recurse` about `HostAddress(cursor)` and
translates each region back, so every recorded `Range` is guest-addressed. The capture format stays guest-addressed
(`arena_base` in the header is the *guest* arena base), so a capture taken at one bias replays at any other. The Linux
`HostMapping::Walk` and Windows `VirtualQuery` walks only had their arena base converted (bias is 0 there).

## What the host runtime must call

C header: `core/libs/prx/libSceAgcDriver/include/AgcGuestMemory.h` (implemented in `Execution/src/GuestMemory.cpp`). Every
address is a **guest** address; the driver finds memory at `host = guest + bias`.

At start-up, once, before any other thread enters the driver and before any AGC call:

1. Reserve the guest window (base mode: `g_nps5_arena`) and call `AgcSetGuestBias(bias)` with the arena's `host - guest`.
2. `AgcSetGuestArena(guest_lo, guest_hi)`: the guest range backed by the reservation (the checked translations reject
   anything else; default `[0, 2^40)`).
3. `AgcSetGuestScratch(guest_lo, bytes)`: a range inside the arena that stays inaccessible (`PROT_NONE`) and is never handed
   to the guest. The driver maps pages in it with `MAP_FIXED` for its own memory that needs a guest address. Required when the
   bias is nonzero (256 MiB is plenty); the driver throws on first use otherwise.

For the lifetime of the title, for every guest mapping that exists in the host process (after the host mapping is made):

* `AgcGuestAllocationRegister(guest, bytes, AGC_GUEST_READ | AGC_GUEST_WRITE | AGC_GUEST_GPU)`: `READ` = CPU readable
  (`prot & 3`), `WRITE` = CPU writable (`prot & 2`, needs `READ`), `GPU` = GPU mapped (`prot & 0x30`; also notifies the GPU
  map observer). Overlapping an existing registration, a null or wrapping range fails with -1 and
  `AgcGuestAllocationLastError()`. Register what `sceKernelMap*Memory`/`mmap` committed, not reservations.
* `AgcGuestAllocationUnregister(guest, 0)` removes the whole allocation that starts at `guest`;
  `AgcGuestAllocationUnregister(guest, bytes)` removes `[guest, guest + bytes)` of it (partial munmap). Call it **before**
  the host memory is released or reused: it waits for GPU work that still leases the range (up to `APS5_PIN_WAIT_MS`, default
  60 s) and invalidates the driver's caches.
* `AgcGuestAllocationProtect(guest, bytes, flags)` before the host `mprotect`.

Rules: pass guest values only (a host pointer registers a range the driver never finds); the registry is keyed by the same
guest address every PM4 word, register block and `Shader` carries. All functions are callable from any thread except the
three init calls. The registry reports misuse by exceptions that cannot unwind through the arm64 `libc.prx` build, which is
why the C API validates (null, wrap, overlap, not registered) before touching it: do not rely on catching.
`GuestAllocations::Mutation` (the C++ registry API in `libc/include/GuestAllocations.hpp`) takes the same guest-valued
pointers; the runtime should use the C functions.

Library entry points still need the thunk generator to follow the libSceAgc convention above (translate pointer
parameters; leave out-slots and returned `Packet`/`Shader*` guest values to the library).

## Tests

* `agc_guest_bias_tests` (`libSceAgc/tests/GuestBias.cpp`, macOS): builds command buffers in a reservation mapped at a
  nonzero bias and checks jump/atomic/prime/indirect/cond-write/wait packets and patches carry guest addresses, the
  cursors stay guest-valued and the callback growth path works, out-slots (semaphore label, payload address) are guest
  values, a `Shader` header with a guest-valued `cx_registers` reads correctly, register-defaults tables are scratch memory
  with guest pointers, `AppleWalkGuestRegions` reports guest-addressed regions, and the C allocation API keys the registry by
  guest address. `agc_create_shader_tests` also runs `sceAgcCreateShader` under a bias. Mutating `Agc::Guest::Address` to the
  identity makes the first fail.
* `agc_frame_replay` on `work/s3/rosetta/captures_old/{A,B,C}` is still 100 % pixel-identical (max channel diff 0), also at
  `--bias 0x30000000000`.

## Not converted

* Windows-only blocks (`VirtualQuery`, the Windows `GuestArena*` calls, the `coverage` tracker) in `GuestMemory.cpp`,
  `GuestBufferMemory.cpp` and `Pm4.cpp`; the Windows `VirtualQuery` and Linux `HostMapping::Walk` capture walks in
  `FrameRecorder.cpp` (their results are host-keyed): bias is 0 there; not built or run here.
* The DMA-buf import path (Linux): its backing lookups are converted, the path is untested.
* `tools/` other than `FrameReplay.cpp` (`OracleDispatch`, `OracleRunner`, `ShaderReplay`, `Pm4Analysis`) and `tests/`.
* `GuestAllocations::RegisterImage/RegisterMainImage` (libc) still query the host process image (x86 world only).
* The host runtime's producers of the registry entries and its calls to the functions above live in nativeps5 (nps5_mem),
  not in this repository.
