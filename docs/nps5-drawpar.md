# nps5-drawpar: the draw path as a pipeline (2026-11-15)

Branch `nps5-drawpar`, off `nps5-link` a00d5ff82f. Title: PPSA30552 on the fast baseline
(ps5recomp fc4925b, 1280x720, kk-mesh, prewarmed cache).

## 1. What limits the frame

Measured on the fast baseline (run `dp_base`, 100 s, 24.5 fps average, p50 32.5 ms, process CPU
73.7 ms/frame; 8 s `sample` at 65 s):

| Thread | State |
|---|---|
| Guest RHI submit thread | 76% of wall time blocked in `Driver::Submit` -> `holdFlipBehindWorker` (back-pressure behind the queue worker) |
| `qworker 0x0` | 95% inside `execute()`. 55% of wall time in `DrawPipeline::Drain` (waiting for the committer to empty), 19% `draw()`, 7% `dispatch()`, 6% label packets |
| `draw-commit` | 78% not idle. 22% fence-poll kernel traps, 12% hand-off spin clock reads, 35% `commitDraw`, 8% submit, 6% buffer fills, 12.5% idle |

So the chain guest -> queue worker -> committer is the frame limiter and the committer is its
critical path. The queue worker is rarely busy with its own work; it waits.

Inside `commitDraw` the Vulkan recording itself (`recordPreparedDraw`) is a few percent. The rest is
resource coherence (`RepeatStorageSurface`, `StorageTexture::refresh`, `revalidateFull`) on shared
caches under the GPU mutex.

## 2. Why command buffers are not recorded on N workers

The brief was "record command buffers on worker threads from per-CB state snapshots". The driver
already has the snapshot half of that design, per draw instead of per command buffer:

- The queue worker decodes PM4, resolves the draw from the register banks, captures the guest memory
  the shaders read (`snapshots`, `memory`), and hands a self-contained commit closure to the
  committer (`DrawPipeline::Enqueue`). The closure owns its state; it does not read `QueueState`.
- The committer runs the closures in submission order under the GPU mutex and records Vulkan.
- Ordering with guest-visible memory is kept by write ranges on every queued item
  (`Overlaps`, `PendingLabel`, `StoredBytes`, `HeldReads`, `WrittenSince`) and by `Drain` wherever a
  packet cannot be proven independent of queued work.

Recording on several workers would parallelise only the last, cheapest step (about 3% of the
committer) while the expensive part (texture/buffer cache lookups, coherence refreshes, barrier and
layout tracking, write-watch arming) mutates shared state that assumes one ordered stream. The
Uncharted port reached the same conclusion for its GPU thread (parallel_gpu.md: render targets,
barriers and cache mutation stayed serial; the win came from preparing draws ahead).

The useful parallelism here is therefore pipeline overlap: keep the queue worker decoding while the
committer commits, and take avoidable work off the committer.

## 3. What this branch delivers

### 3.1 KosmicKrisp patch 0006: fence polls without a kernel wait

`mesa/patches/0006-kosmickrisp-fence-poll-without-kernel-wait.patch`.
`vkGetFenceStatus` and every zero-timeout wait reach `kk_timeline_wait`, which called
`-[MTLSharedEvent waitUntilSignaledValue:timeoutMS:]` even with timeout 0; that is an
`iokit_user_client_trap` per call. The recorder polls one fence per GPU label
(`Recorder::Reap` from `WriteLabelOnGpu`, "one fence status check" by design). The patch reads
`signaledValue` first: `VK_SUCCESS` when the awaited value is reached, `VK_TIMEOUT` for a zero
timeout, kernel wait only for a real blocking wait. Semantics are unchanged (the old call returned
the same answers). `KK_FENCE_POLL=0` restores the old path in the same binary, so on/off A/B shares
one pipeline cache.

cap4 committer sample: `iokit_user_client_trap` 7.9% -> 0.1% of `draw-commit`. Frames identical.

Any KosmicKrisp rebuild changes `pipelineCacheUUID`. On nps5-link that also invalidates
`pipelines.journal` (fixed in nps5-pipeline2), so a title needs one cold run before
`nps5-prewarm.sh --force` can prewarm for the new library.

### 3.2 Pipeline overlap: the three opt-in stages (all default off)

| Flag | Effect |
|---|---|
| `APS5_PIPELINE_DISPATCH=1` | a compute dispatch whose reads are clear of queued writes is queued behind the committer instead of draining it |
| `APS5_PIPELINE_DISPATCH_MISSES=1` | the same for dispatches that miss the dispatch cache or have no recipe yet |
| `APS5_DRAW_PIPELINE=1` | third stage: the second half of a draw (`finishDraw`: materialise stages, capture) runs on a `draw-finish` thread between the queue worker and the committer |

These exist upstream and were never enabled or validated on this title. On cap4, 429 of 565 packet
drains per window were compute dispatches (opcode 0x15), then 0x3c waits (107). With the two
dispatch flags: 71 drains, drain wait 818 ms -> 251 ms per window.

### 3.3 Verify modes

| Flag | Checks |
|---|---|
| `APS5_VERIFY_DRAW_PIPELINE=1` | re-proves reused epoch proofs on the finisher/committer and prints `[epoch-proofs] ... verify: N checked, M failed` |
| `APS5_VERIFY_DATA_HITS=1` | a pipelined dispatch's kept variant against a fresh build |
| `APS5_VERIFY_REGISTER_KEYS=1` | incremental register fingerprint against a full recompute, every draw |
| `APS5_VERIFY_DRAW_DATA_HITS=1`, `APS5_VERIFY_DRAW_ENTRIES=1` | a draw served from the entry cache against a full decode/build |
| frame compare | `agc_frame_replay <cap4> --frames <dir>` serial vs staged, `cmp` on `replay-*.bmp` |

### 3.4 Incremental draw resolve

Already present, so nothing new was added:

- `Registers` keeps an XOR fingerprint per bank that is updated on each register write
  (`Registers::Fingerprint`), so the draw key costs O(changed registers), not O(state).
- The key finds a draw entry holding the full decode (`DrawDecode`): a hit skips the decode.
- When only shader user words changed ("data-only hit", `shapeKey`), the kept decode is copied and
  only the user words are re-read.

cap4, per draw on the queue worker: decode + key/lookup/validate 12.6 us with it, 13.9 us with
`APS5_NO_DRAW_KEY=1`. About 190 draws per frame, so the whole resolve is ~0.3 ms/frame on a thread
that is not the critical path. The title on/off number is in section 4.

## 4. Results

Offline and gate:

| Check | Result |
|---|---|
| cap4 replay, serial vs `APS5_DRAW_PIPELINE` | identical frames |
| cap4, serial vs `APS5_PIPELINE_DISPATCH` (+`_MISSES`) | identical frames |
| cap4, serial vs all three | identical frames |
| cap4, all three + `APS5_VERIFY_DRAW_PIPELINE`, `_REGISTER_KEYS`, `_DATA_HITS` | identical frames, 0 failed checks |
| cap4, `APS5_VERIFY_DRAW_DATA_HITS`, `_DRAW_ENTRIES`, `_REGISTER_KEYS` | identical frames, no verify error |
| cap4, kk-fix-fence, fast path on / off | identical frames |
| homebrew gate, all three flags + kk-fix-fence | PASS: Calculator3D frame 0 exact (max diff 0), Syobon PASS, Greenhollow 98.69% |

Title, fast baseline, 100 s runs in one priority window (no other title runs; fps over the last
60% of frames; CPU in ms per frame). Run-to-run noise in this window is about +-1 fps; an earlier
run under machine load (`dp_base`) measured 26.7 fps for the same configuration.

| Run | Config | fps | p50 ms | p95 ms | process CPU | draw-commit | qworker | draw-finish |
|---|---|---|---|---|---|---|---|---|
| dp_base2 | baseline (kk-mesh) | 35.0 | 28.4 | 31.4 | 54.2 | 15.8 | 14.0 | - |
| dp_base3 | baseline (kk-mesh) | 35.9 | 27.6 | 29.5 | 49.6 | 14.0 | 12.7 | - |
| dp_par | three stage flags | 35.8 | 27.6 | 29.8 | 54.1 | 14.0 | 9.4 | 6.8 |
| dp_par2 | three stage flags | 36.5 | 27.4 | 28.2 | 51.4 | 13.2 | 8.9 | 6.5 |
| dp_fin | `APS5_DRAW_PIPELINE` only | 35.9 | 27.8 | 28.7 | 57.4 | 14.5 | 13.0 | 6.6 |
| dp_nokey | `APS5_NO_DRAW_KEY=1` (incremental resolve off) | 36.5 | 27.4 | 28.4 | 49.8 | 13.9 | 12.8 | - |
| kf_off1 | kk-fix-fence, `KK_FENCE_POLL=0` | 36.3 | 27.3 | 29.4 | 49.8 | 14.0 | 12.7 | - |
| kf_off2 | kk-fix-fence, `KK_FENCE_POLL=0` | 35.7 | 27.7 | 29.6 | 49.6 | 13.9 | 12.7 | - |
| kf_on1 | kk-fix-fence (fast path) | 36.9 | 27.1 | 27.7 | 45.0 | 12.4 | 10.1 | - |
| kf_par | kk-fix-fence + three stage flags | 37.2 | 26.8 | 27.3 | 47.3 | 11.4 | 7.4 | 6.4 |

(`kf_on2` was killed by the memory guard before it produced data.)

Reading:

- The fence fast path removes about 4.7 ms of process CPU per frame (-10%: committer -1.6 ms,
  queue worker -2.6 ms) and tightens p95 (29.5 -> 27.7 ms). Frame rate moves by about +2%.
- The stage flags move 4 to 5 ms of queue-worker time per frame onto the finisher and remove most
  drains; frame rate moves by about +1 to 2%. Everything together: 37.2 fps against 35.0 to 35.9.
- Incremental draw resolve on or off is inside the noise on the title, as cap4 predicted.
- The frame rate barely follows the CPU savings because, with the fence polls gone, the committer
  is not CPU-bound: in the `kf_on1` sample it spends 50% of its wall time blocked in
  `vkWaitForFences` under `storeBufferFill -> StorageTexture::FlushPending -> Recorder::FinishUpTo`
  (plus `FlushDepthPlanes`, 20%). With the old KK the same wall time was split between that wait
  (20%) and the fence polls (30%), which were in effect a busy wait for the same GPU batches.
  The frame is bound by synchronous GPU round trips at buffer fills, not by driver CPU.

## 5. What is left on the committer (owners elsewhere)

Hand-off spin clock (cpumicro), `mach_vm_region` in `GuestMemory::Accessible`/`CheckRange`
(submitcpu), `RepeatStorageSurface`/`compareUntracked` (rtalias, untracked), wait/wake primitive
(submitcpu), segment commit (segcommit). After those and 3.1, the committer's remaining cost is the
coherence work per draw and per dispatch; and the synchronous flush before buffer fills (section 4). The flush is the next limiter: a
buffer fill waits for the GPU to finish every batch that stores into the filled range.
