# nps5: GPU and guest memory coherence on Apple unified memory

On a PS5 the GPU reads and writes the title's memory directly. The AGC driver emulates that with Vulkan images and
buffers plus copies between them and guest memory. This document maps those copies and waits, says which of them the
title pays for under nativeps5, and describes the model the branch `nps5-coherence` moves to. The runtime half lives in
ps5recomp (`docs/m3/writewatch.md`, branch `m3-wwatch`).

Status of the numbers: the flush counts come from `APS5_TRACE_FLUSH` on a 150 s level run of PPSA30552 (boot3 run p3) and
from the offline replay of the in-game capture cap4 (2 frames, 100 submissions). Frame times on the title with this
branch are not measured yet; see "Measurements".

## 1. What the driver already does

The driver is not a copy-everything design. Three mechanisms decide every copy:

| Mechanism | What it gives | Needs |
|---|---|---|
| Host imports (`VK_EXT_external_memory_host`, `GuestBufferMemory.cpp`) | Guest memory is the Vulkan buffer: buffers, vertex and index data, and the write-back target of images are bound in place, no CPU copy | an import of the range (KosmicKrisp: `newBufferWithBytesNoCopy`) |
| Page write watch (`libc/src/GuestWriteWatch.cpp`; macOS: `mprotect` plus a fault handler) | "Has the CPU written this range since generation N?" answered from 64 KiB block stamps | the range is registered with the watch and every write to it faults or is bracketed |
| Read traps (same file; macOS only) | A pending GPU result stays on the GPU; its guest pages are inaccessible and the first CPU access stores it | the range is watched |

A storage image (render target, compute output, anything the GPU writes) lives in a Vulkan image, because PS5 surfaces
are tiled and a Vulkan image cannot alias tiled bytes. Its results are *pending* until a *flush* (`StorageTexture::FlushPending`)
retiles them into guest memory: on the GPU into the host import when there is one, through the CPU otherwise.

Without the watch the driver cannot tell whether the CPU changed an image's memory, so it keeps a full CPU copy of
every cached storage image (`StorageTexture::original`) and compares (`compareUntracked`). That is the fallback path.

## 2. What nativeps5 was paying for

`AgcGuestAllocationRegister` never put a range under the watch, so on nativeps5 everything ran on the fallback path:

- every reuse of a storage image compared up to 37 MB against `original` (memcmp 30 to 56 ms per frame);
- `original` held 1.2 GB of cold copies (boot log entry 27);
- read traps could not be set, so images were stored instead.

### Where the 21.6k flushes of the 150 s run come from

| Reason (`APS5_TRACE_FLUSH`) | Flushes | Share | What triggers it |
|---|---|---|---|
| `idle` | 10,093 | 47 % | queue 0 found every queue idle for 50 ms and stored every pending image so the CPU can read it. With 150 ms to 2 s frames this happens between almost any two submissions |
| `storage refresh` | 6,625 | 31 % | an image is used again and other images are pending in the same memory. UE's render-target pool reuses memory: one 37 MB target is pending while small targets are created inside its range |
| `readback before label` | 1,510 | 7 % | linear and small images stored before a completion label, because the CPU may read them once it sees the label |
| `imported buffer region` | 1,237 | 6 % | a shader binds memory as a buffer while an image is pending there |
| `buffer fill` | 991 | 5 % | a fill kernel or DMA fill over memory with pending results |
| `stencil plane store` | 736 | 3 % | a depth surface's stencil plane is stored over a pending image |
| `storage image creation`, `memory access`, others | 433 | 2 % | |

Which images (replay of cap4, where the trace also prints shape, format and tile mode; the level run has the same
address pattern): all flushed images are **tiled** (tile mode 7 or 4), none is linear. They are the frame's render
targets and compute outputs: 2848x1600 RGBA16F and R11G11B10 scene colour and history (19 and 37 MB), 3840x1648 G-buffer
and 3840x2160 output targets (25 to 33 MB), an 8192x2048 depth atlas (32 MB), 64x64x63 volumes, and small 1x1 to 240x103
readback targets. The thin linear same-base image that caused the Astro Bot port's flush storm does not occur here.

Does the guest CPU read them? Not established. The small 1x1 to 240x103 targets are what a title reads back
(exposure, occlusion and similar feedback), and the driver already treats them that way (stored or trapped at labels).
For the large tiled targets the trace cannot tell: the replay has no guest CPU, and the level run stored them at idle
before any read could be observed. With this branch `APS5_TRACE_FLUSH` prints `read trap ... resolved for a CPU access`
for every trapped range the CPU touches, so one title run answers it per image.

### Waits on the GPU

| Wait | Where | When |
|---|---|---|
| Recorder sync in a flush | `FlushPending` (`recorder->Sync()`, `FinishUpTo`) | the flushed layers have an unfinished GPU writer and the store is not GPU-ordered |
| Idle publish | `waitForIdlePublish` | after an idle store that published unit shadows |
| Flush hook | `Recorder.cpp` hook, from `GuestMemory::FlushGpuWrites` | the driver or an HLE call reads guest bytes a recorded GPU write covers |
| Completion labels | `DeferredLabels.cpp` | label batches are submitted on a 1 ms deadline; the CPU waits only for labels stored by completion |
| Read-trap resolve | `ResolveLabelTrap` | a CPU access to a trapped page: the faulting thread waits until the image is stored |

A flush of an imported, watched image at a label is GPU-ordered: the copy into the import is recorded behind the
writer and nobody waits on the CPU. A flush on the fallback path (not imported, or unwatched and compared) waits.

## 3. The model

Apple Silicon is unified memory and KosmicKrisp imports host pointers without copying, so the cheapest correct model is
an ownership model per page range:

1. **Buffers are guest memory.** Every buffer the GPU reads or writes is the host import of the guest range. No copy in
   either direction; the only cost is knowing when the CPU wrote (write watch) for the caches derived from the bytes.
   This is what the driver did already; the watch makes its validation a block-stamp scan instead of a compare.
2. **Tiled and swizzled images are GPU-owned until read.** The image content lives in the Vulkan image. Guest memory
   is brought up to date only when something needs the bytes: a CPU access (read trap), a GPU use of the same memory
   through another view (refresh, buffer binding, fill), or a label for small and linear images.
3. **Linear images** use the same rule today (image plus GPU-ordered copy into the import at labels, or a read trap).
   Aliasing them as texel buffers or linear images over the import would remove that copy too; it is not implemented
   (see "Not done").
4. **CPU writes are found by the watch**, not by comparing. A range is compared only where the watch cannot cover it:
   memory mapped at two guest addresses, and imports on a driver that drops write protection (not KosmicKrisp: the
   probe reports "imported ranges stay watched").
5. **Waits are batched**: a store that can be ordered on the GPU never waits on the CPU; the CPU waits only where it
   consumes bytes.

### Correctness rules

When a GPU write must be visible to the CPU:

- R1. Before the guest CPU reads or writes a page of a pending image. Enforced by the read trap (the page is
  inaccessible; the fault stores the image first) or, for an image that could not be trapped, by the store at idle and at
  labels as before.
- R2. Before a completion label the guest waits on is signalled, for linear and small images (the title may read them
  right after the label): trapped or stored (`StoreReadbackBeforeLabel`, unchanged).
- R3. Before the driver itself reads the bytes (texture upload from the same memory, DMA copy, indirect arguments,
  buffer binding): the flush hook and `FlushPending` with the reason (unchanged).
- R4. Before a kernel-mode or library read of the bytes (file write, socket send): `GuestArenaPrepareHostRead`, called by
  the runtime's I/O wrappers. A kernel read of a trapped page would fail with `EFAULT` otherwise.
- R5. Before the memory is unmapped or its protection changes: `AgcGuestAllocationUnregister` / `Protect` wait and store.

When a CPU write must reach the GPU:

- W1. Before the next use of a cached image or buffer built from those bytes. The watch stamps the 64 KiB blocks of the
  written pages; `Revalidate`/refresh sees the stamp and uploads those blocks. A store to a protected page faults once per
  page per collect (plus fault-ahead inside a 64 KiB block).
- W2. A CPU write over a pending image wins for the blocks it touched ("keeping the CPU's blocks"); with a read trap the
  image is stored first and the write lands on top, which is the console's order.
- W3. A kernel-mode write (file read, socket receive) into guest memory does not fault: it is bracketed by
  `GuestArenaBeginHostWrite` / `EndHostWrite`, which opens the pages and records them as written.
- W4. Memory with two guest addresses is never watched: a store through one address does not trip the protection of the
  other. It stays on the compare path.

What the model deliberately does not promise: a debugger or another process reading guest memory sees stale image bytes
until a trap resolves, as before.

## 4. What this branch changes

| Change | File | Effect |
|---|---|---|
| `AGC_GUEST_WATCH` flag on `AgcGuestAllocationRegister`, `AgcGuestAllocationWatch`, `AgcGuestWriteWatchAvailable` | `Execution/src/GuestMemory.cpp`, `include/AgcGuestMemory.h` | the runtime's GPU-visible ranges are watched; unregister drops them; a range found to be aliased leaves the watch |
| `GuestWriteWatchHandleAccess` | `libc/src/GuestWriteWatch.cpp` | one fault entry for a foreign handler: read traps and write records. The runtime's crash handler sits above the watch's own handler and has to offer it every fault |
| Idle read traps | `Graphics/src/Texture.cpp` (`FlushAllPendingUntil`) | when the queues go idle, every pending image whose pages are watched (and whose page neighbours are trapped too) is left on the GPU behind a read trap instead of being stored. `APS5_NO_IDLE_READ_TRAP` restores the stores |
| Logical protection of watched pages | `Execution/src/GuestMemory.cpp` (`describePagesHost`) | a read-trapped page is readable to the guest; the driver's accessibility check no longer fails with "guest memory is not readable" on it (first seen with the watch under a bias) |
| `APS5_TRACE_FLUSH` prints shape, format, tile mode, mips, imported/copied, watched/unwatched, units/whole | `Graphics/src/Texture.cpp` | the per-resource ranking above |
| `APS5_REPLAY_WATCH` | `tools/FrameReplay.cpp` | the offline replay watches the ranges it maps, as the runtime does |

Contract for the runtime (also in `AgcGuestMemory.h`): pass every memory fault to `GuestWriteWatchHandleAccess` on every
thread; change host protections only through `GuestWriteWatchProtect`; bracket kernel-mode writes and prepare kernel-mode
reads; never watch doubly mapped memory. `NPS5_AGC_WRITE_WATCH=0` turns the whole thing off on both sides.

## 5. Measurements

Offline replay of cap4 (2 frames, 100 submissions; both frames pixel-identical to the capture's reference in every row):

| Configuration | Flushes | idle | label readback | refresh + creation | Replay time |
|---|---|---|---|---|---|
| base (`nps5-cpudraw`): unwatched | 222 | 108 | 35 | 58 | 0.97 s |
| watched (`APS5_REPLAY_WATCH=1`, `APS5_NO_IDLE_READ_TRAP=1`) | 195 | 111 | 7 | 53 | 1.22 s |
| watched, idle read traps | 83 | 0 | 7 | 53 | 0.59 s |

Steady state, the same capture repeated 30 times without pacing (58 frames), with `sample` at 2 ms over the whole run
(per-frame times are samples in the function's subtree, all threads, divided by the frames):

| ms per frame | base, unwatched | watched, idle stores | watched, idle read traps |
|---|---|---|---|
| replay frame, p50 | 151.7 | 52.5 | 47.7 |
| `VulkanDevice::Draw` (committer) | 53 | 16 | 15 |
| `refreshResidentTarget` | 26 | 2.4 | 3.2 |
| `compareUntracked` / `memcmp` / `captureGuestBytes` | 4.5 / 8 / 3 | 0 / 0.2 / 0 | 0 / 0.1 / 0 |
| `FlushPending` | 5 | 21.5 | 2.2 |
| `Recorder::Sync` | 4.3 | 13 | 15 |

The watch is the large step (the fallback path's compares, snapshots and the work they forced in the draw path); the
idle traps remove the stores the watch alone would still make. Time under `Recorder::Sync` rises per frame (4 to 15 ms; these are samples of
threads waiting, not CPU time). That is not explained yet and is the next thing to look at.

The single-pass replay is too short for the driver's 10 s timing lines. The 53 refresh and
creation flushes are the render-target pool aliasing; they store only the 64 KiB units the new view overlaps.

Title: two 120 s runs with this branch and the watch on (main's watch runs user1, user2) reached the level and ran to
the time limit with no fault. user2 (warm pipeline cache): 162 frames, frame p50 105 ms, p95 3.5 s (the multi-second
level gaps remain). The controlled comparison (same binary pair, base libraries against these, flush count, level p50,
footprint, a thread sample inside a gap) has not been run; see "Handoff".

## 6. Tests

- `core/libs/tests/write_watch`: new mode `host-runtime` (a foreign handler above the watch's that forwards to
  `GuestWriteWatchHandleAccess`): a range registered while inaccessible and then protected read-write, a guest read-only
  page (not claimed), an unwatched fault (not claimed), six threads storing while pages are re-armed, a trapped load and a
  trapped store resolved on the resolver thread, a system call into a trapped page with and without a host write. 27 of
  27 pass.
- `agc_frame_replay` of cap4 with `APS5_REPLAY_WATCH=1`: 2 of 2 frames identical, 0 channel difference.
- Runtime side: `TestHostAccessHooks`, `TestGuestVM` (alias report) in ps5recomp.
- Homebrew gate on the rebased tips with the watch on (`tools/regress.sh --full --only calc3d,syobon,greenhollow`):
  PASS. Calculator3D frame 0 identical to the reference (max channel difference 0), Syobon 30.3 fps, Greenhollow
  119.5 fps, all reference frames found. A traced Calculator3D run prints `write watch: on for GPU-visible guest memory`.

## 7. Risks

- **A missed kernel-mode write.** A system call that fills guest memory outside the wrapped paths fails with `EFAULT`
  on a protected page. The watch protects only pages the driver collected (textures and buffers it validated), so the
  exposure is file and socket reads straight into GPU-visible data; those paths are wrapped. A new HLE path must use
  `nps5_kio.h`.
- **Fault cost.** Each first store to a collected page is a signal (a few microseconds). A title that rewrites many
  watched pages every frame pays per page; `APS5_WRITE_WATCH_FAULT_AHEAD` (32 pages) bounds it for sequential writers.
- **Trapped images that the GPU side keeps needing.** An image trapped at idle and then used through another view is
  stored at that use, as before; no extra cost, but no gain for those.
- **A fault with SIGSEGV/SIGBUS blocked** kills the process. The runtime no longer lets the guest block them.
- **Readers that do not fault.** A lazy store only works if every reader either faults or asks first. The readers here:
  guest code and runtime HLE code (fault: there is one mapping of guest memory and the trap makes it inaccessible to
  every thread), system calls (asked through `nps5_kio.h`), the driver's own reads (the flush hook, rule R3), the GPU
  through a host import (the `imported buffer region` and `buffer fill` flushes; a trapped image stays in the pending
  list, so they still find it). One hole is inherited from the label traps: a driver thread that holds the GPU mutex
  and touches a trapped page gets the page opened without a store (it cannot wait for itself), and the page then no
  longer faults for the guest. The idle traps keep that rare by trapping an image only together with every pending
  image that shares its pages. RPS5E (GPL-2.0) reports the same class of bug from its on-access flush (a menu broken by
  a reader that did not fault); `APS5_NO_IDLE_READ_TRAP=1` is the switch to rule it out.
- **Aliased direct memory** is unwatched from the moment the second mapping appears and stays so.

## 8. Not done

- Linear images as texel buffers or linear `VkImage`s over the import (zero-copy for the linear case). The flushed
  images of this title are all tiled, so it would not move its numbers.
- Keeping pending images out of `storage refresh` when a new view overlaps them only partly (the render-target pool
  case): the units are borrowed on the device when the block stamps allow it; what is left stores per unit.
- The periodic multi-second gap every fourth level frame is not attributed yet.

## 9. Handoff

Branches: AnyPS5 `nps5-coherence` (rebased on `nps5-link` a95df689b3; pushed as `nps5-coherence-r1`, the remote
`nps5-coherence` still has the pre-rebase commits), ps5recomp `m3-wwatch` (rebased on main 7306779; pushed as
`m3-wwatch-r1`). Worktrees `wt/anyps5-coherence` (build in `build-arm64`) and `wt/wwatch`.

To finish the title measurement (each run at most 120 s, one at a time, tell main first):

```
cd work/ppsa30552/boot/coh
./go.sh t_base base "APS5_TRACE_FLUSH=1"     # wt/anyps5-cpudraw libraries, b2/native_out1
./go.sh t_new  new  "APS5_TRACE_FLUSH=1"     # these libraries, coh/native_out1 (runtime patch applied to coh/out1)
./go.sh t_new2 new                           # clean timing
./sum.sh <tag>                               # level p50/p95, gaps over 3 s, flushes by reason, footprint, faults
```

`go.sh` takes a footprint at 105 s (`mem/<tag>.fp`) and a 12 s `sample` at 75 s (`mem/<tag>.sample.txt`); look in the
sample for what the flip thread and the queue workers wait on during a gap (`Recorder::Sync`, a label wait, a trap
resolve, `enqueueDmaPacket` reading over a pending GPU write). Both configurations must use the same pipeline-cache
state (the coh cache was prewarmed by main). Offline: `scratchpad/coh/rp.sh` is just `agc_frame_replay
work/ppsa30552/boot/dark/cap4` with the KosmicKrisp mesh ICD; add `APS5_REPLAY_WATCH=1`.

Open items, in order: the controlled title comparison; the multi-second level gaps (not attributed); `Recorder::Sync`
from DMA packets (15 ms per replay frame); a count of read traps opened by the GPU-mutex bypass.
