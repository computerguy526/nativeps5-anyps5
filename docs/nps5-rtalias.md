# nps5: render-target aliasing without write-backs (branch `nps5-rtalias`)

Unreal's render-target pool hands the same guest memory to one surface after another. In the driver each surface is a
`StorageTexture` (a Vulkan image plus per-unit state), and results of an image that guest memory has not received are
*pending*. When an image is created or used (`refresh`) while another image has pending units in its range, the driver
stores those units first (`FlushPending`, reasons `storage refresh` and `storage image creation`). This document
attributes those flushes, gives the ownership model, says which part of it is provable today and implemented
(`APS5_RTALIAS=1`, default off), and what the rest needs. It continues `docs/nps5-coherence.md` section 10.

## 1. Attribution

Measured on the offline replay of `work/ppsa30552/boot/dark/cap4` with `APS5_REPLAY_WATCH=1 APS5_TRACE_FLUSH=1`
(tooling in `work/rtalias/`: `r.sh`, `fd.sh`). The trace now also prints, per flush, how long it took and whether it
waited (`[flush-done]`), and per refresh that flushed, the use that asked for it and why it was or was not GPU-ordered
(`[refresh]`, `[alias]`).

Steady state, the capture repeated 30 times (58 frames), eager path:

| Refresh that flushed other images | Count | Per frame | What the use does with the overlap |
|---|---|---|---|
| draw target, blending off, full scissor and write mask | 472 | 8.1 | writes; whether every texel is covered is not known to the driver |
| draw target, blending off, partial scissor or write mask | 267 | 4.6 | writes part; the rest of the target keeps the old bytes |
| sampled or bound read-only (`resource`) | 375 | 6.5 | reads the old image's bytes through the new view |
| storage image written by a shader | 9 | 0.2 | writes; coverage unknown |
| total | 1,123 | 19.4 | |

- Pairs (old image, new image): ten small targets (1x1 to 960x412, 64 KiB to 2 MiB) inside one 2848x1600 image
  (`0x305a1b0000+0x23f0000`, 36 MiB); three 64x64x63 volumes under a 1424x800 target; and two surfaces used under two
  formats of the same shape (`0x305f3c0000+0x12b0000`, formats 36 and 50, with DCC keys). The first single pass adds 23
  `storage image creation` flushes with the same pairs; in steady state there are none (the images are cached).
- Waits: 945 of the 1,123 refresh flushes are already GPU-ordered (the store is recorded behind the writer, nothing
  waits), 161 found the writer finished, 17 waited (`sync`/`finish`). All refresh flushes together take 3.2 ms of CPU
  per frame (the frame is about 50 ms); the 17 that wait take 59 % of that. Not GPU-ordered are the surfaces with DCC
  keys (148) and targets that hold pending units of their own (60).
- A third of the refreshes are reads (375): the bytes the old image left are sampled through the new view. For these
  the old results are not dead; they must reach the new image whatever the model.
- Does the guest CPU read the overlap afterwards? Not established: the replay has no guest CPU. Its `memory access`
  flushes are whole-image stores once per replay pass (29 or 30 per image in 30 passes), not reads inside a frame. A
  title run answers it per image (`read trap ... resolved for a CPU access` in the flush trace). The small 1x1 to
  240x103 targets are stored at labels (`readback before label`) as before.

So the cost of this class in the replay is small (3 ms of a 50 ms frame) and most of it is already a device-side
inherit: store into the host import, upload from the import, both recorded on the GPU with no CPU wait.

## 2. Model

State per 64 KiB unit of guest memory (the driver's block units; an image whose layout does not split into units is
one unit and never takes part):

- *owner*: the image whose Vulkan content is the truth for the unit, or guest memory. An image owns a unit exactly
  when `layerPending[unit]` is set. `layerGeneration[unit]` is the write generation at which the image last matched
  the memory; the write watch stamps a unit when the CPU or a driver store writes it. Order between two GPU writers of
  one unit is the order of their `markLayersPending` calls (`writes`, `version`).
- *hand-over*: image N is used over units owned by image O. Instead of storing O, N records O in `handedOver` (with
  O's write count) and the units in `handedUnits`. O stays the owner and stays in the pending registry, so every
  reader that asks for the memory (`FlushPending` by range, the flush hook, read traps, labels, buffer bindings,
  fills, unmap) still finds O and stores it then: the write-back is lazy and comes from the still-alive old image.
- *transfer*: when N's own results become pending over a handed unit (`markLayersPending` -> `supersedeHandedOver`),
  O's pending bit for the unit is dropped without a store, provided O was not written since the hand-over. N is now
  the owner; its write-back stores every byte of the unit.
- *cancel on clear*: a hand-over is allowed only when N's content does not depend on the memory: N's DCC keys are a
  clear code, so its upload is a GPU clear that reads nothing. This is the only write-before-read proof the driver
  has (see section 4).

This follows the shape of RPCS3's surface store (age-ordered ownership, inherit on first read, cancel on clear;
`research/rpcs3.md`), as an idea only: no RPCS3 code is used, so NOTICE is unchanged. The difference that keeps it
exact is that the only inherit here is the bit-exact one (retile through the import), and a hand-over happens only
where no inherit is needed.

### What existed, what this branch adds

`clearHandOver` (refresh) and `PrepareCreation` (creation) implemented the hand-over for clear-keyed images, but
refused whenever a pending source shares DCC keys with the new image, because a store of the source could write keys
and so change the keys the decision was read from. That refusal covers the common case of one surface used under two
formats (the title fast-clears it through its keys and draws through the other view): 89 flushes of 19 MiB per 58
frames, all of them not GPU-ordered.

`APS5_RTALIAS=1` allows the hand-over when every source that shares keys has `uploadedKeys == Uncompressed`. A
write-back stores keys only under `uploadedKeys != Uncompressed` (three sites in `writeBackLayers`/`writeBack`), so
such a source's store writes no key: the keys read before the decision are the keys after the eager flush, and the
decision is the eager path's.

It also closes a gap of the existing hand-over. A handed-over image O that is itself refreshed under clear keys
before N is written used to drop its pending results (rule "new keys that are a clear code make every result dead");
the eager path had stored them at N's refresh. With the flag, O remembers the write count it was handed over at
(`handedWrites`), and a refresh under clear keys stores those results instead of dropping them.

## 3. Correctness

Claim: with `APS5_RTALIAS=1` every observer reads the bytes it reads on the eager path. Let N be refreshed (or
created) at time t0 over units U owned by O, with the hand-over taken.

1. *N's content at t0.* Eager: O's units are stored, then N reads its keys. The stores write no key (O's
   `uploadedKeys` is `Uncompressed`), so the keys are the clear code read before; N is cleared on the GPU and reads no
   texel memory. Lazy: the same keys, the same clear. N's image is identical.
2. *Readers between t0 and N's first write t1.* O still owns U and is registered. Every path that reads guest bytes
   asks `FlushPending` for the range (rules R1 to R5 of `nps5-coherence.md`; CPU accesses arrive through the read
   trap or the idle/label stores, which also go through the registry). It stores O's units then. O's content is the
   content at t0 unless O was written again; then eager would hold the newer results pending too and store the same.
   A refresh of O itself under clear keys stores instead of dropping (the `handedWrites` rule). So a reader sees O's
   bytes, as on the eager path, only stored later.
3. *N's first write at t1.* `MarkDirty` marks every unit of N pending (a 2D target has no partial write state) and
   stores N's keys as uncompressed. For each handed unit u wholly inside N with O unwritten since t0, O's bit is
   dropped. From here N owns u. Eager: memory held O's bytes in u, N's image held the clear plus the draw. Lazy:
   memory holds older bytes in u, N's image is the same.
4. *Readers after t1.* Any reader of u makes `FlushPending` store N's unit: the whole unit from N's image (the
   condition `layerBytes == trackedLayerBytes` and `from >= begin`, `to <= end` at the hand-over makes a handed unit
   exactly one unit of N). Both paths write the same bytes over u, so the difference in the bytes below is never
   visible. A unit of O only partly inside N, or of another size, refuses the hand-over at t0.
5. *O written again before t1.* `writes` differs from the recorded count: nothing is dropped, O keeps its newer
   results, which are newer than N's clear in submission order, as on the eager path.
6. *CPU writes.* A CPU store into u before t1 faults (trap) or is stamped (watch); O's store rule keeps the CPU's
   block (W2), the same rule at the later store as at the eager one, because the stamp is compared with O's
   generation, which no hand-over moves. After t1 N's generation for a handed unit moves only if no CPU stamp is on
   it (`supersedeHandedOver`).
7. *Labels and readbacks.* `StoreReadbackBeforeLabel` and the idle read traps walk the pending registry; O is in it
   until t1 and N from t1.
8. *Mips, array slices, depth.* Hand-over needs block units on both sides and an unscoped refresh; scoped refreshes
   (`RefreshLayers`), thick and layered surfaces, depth planes and images that store whole take the eager path.
9. *Unwatched memory* takes the eager path (`Watched` is required), as does a source on another device.

Not covered by the argument, and the same on the eager path: a debugger reading guest memory without faulting, and
the GPU-mutex bypass of read traps (`nps5-coherence.md` section 7).

## 4. Why the general case is not implemented

"The new target overwrites the old results" is true on the console for a transient target, but the driver cannot
prove it for a draw or a dispatch: coverage depends on the vertex shader's output, on discard, depth and stencil
tests, and for a shader store on the indices written. `MarkDirty` marks the whole target pending after any draw, so
an image that skipped its upload and was only partly covered would write undefined texels over the old bytes at its
write-back. Without a proof the old bytes must be in the new image before its first write, which is the inherit the
eager path performs; 84 % of those are already GPU-ordered and cost no wait.

The attribution bounds what a coverage proof could remove: at most the 472 + 9 write-first refreshes of 1,123, none
of the 375 reads, and the 267 partial writes only with per-texel knowledge. What such a proof needs is listed in
the handoff.

## 5. Verify mode

`APS5_RTALIAS_VERIFY=1`:

- at every transfer (a unit dropped unstored) checks the obligations of step 3 and 4: the unit is one unit of the new
  owner, the new owner has it pending, its keys are uncompressed, the memory is watched; a failure prints
  `[rtalias-verify] MISMATCH` and counts;
- prints `[obs] access <range> <hash>` for the guest bytes after every flush-hook access that stored or published,
  and at the end of a replay stores everything, waits, and prints `[obs] final <range> <hash>` for every live image;
- `work/rtalias/vset.sh "<eager logs>" "<lazy logs>"` compares them. Eager runs differ among themselves on some
  ranges (which of two overlapping pending images is stored last at the final flush depends on timing; pool memory
  never written by the capture), so a point counts only when every eager run agrees on it.

Result on cap4 (single pass): 10 eager runs give 77 observation points, 48 stable (all `final`); 6 runs with
`APS5_RTALIAS=1` match all 48, 0 mismatches, 0 invariant failures, 3 hand-overs and 2,969 units transferred per run,
frames pixel-identical (max channel difference 0). The handed surface `0x305f3c0000+0x12b0000` is among the stable
ranges. The eager path itself is not run in the same process: the comparison is between runs.

## 6. Numbers

Steady state, 58 frames, `APS5_TRACE_FLUSH=1`:

| | eager | `APS5_RTALIAS=1` |
|---|---|---|
| refresh flush calls | 1,123 | 1,034 |
| images stored for `storage refresh` | 1,683 | 1,589 |
| refresh flushes not GPU-ordered | 178 | 89 |
| refresh flushes that waited (`finish`/`sync`) | 4 to 17 | 1 |
| hand-overs under shared keys | 0 | 89 (one 19 MiB store and re-read each) |
| frames identical to the reference | yes | yes |

Frame p50 could not be separated: the machine was shared and three alternating runs gave 68, 94, 75 ms (eager)
against 98, 83, 94 ms (flag), inside each other's spread. The expected gain is the removed store and upload of
19 MiB, 1.5 times per frame, and the waits of the non-ordered path; from the flush timer that is 1 to 2 ms of CPU per
frame in the replay.

### Title (fast baseline, 1280x720, 120 s, `APS5_TRACE_FLUSH=1 APS5_RTALIAS=1 APS5_RTALIAS_VERIFY=1`)

Libraries: branch `nps5-rtalias-link` (this commit on nps5-link a00d5ff82f, worktree `wt/anyps5-rtalias-link`; the fast
binary needs the nps5-stall clock hook, which `nps5-coherence2` does not have). One run, tag `rta_on`: 2,803 frames in
108.7 s, p50 33.6 ms, p95 59.8 ms, no fault, 5 read traps resolved for a CPU access.

- 2,803 hand-overs under shared keys, one per frame (a surface under format 50, keys 0000), 0 `MISMATCH` lines.
- Refreshes that flushed other images: 17,074 (6.1 per frame). 11,411 are reads (`resource`; 8,403 of a 640x360
  format 5 image, 2,658 of a 1024x512 mip chain), all GPU-ordered. 5,613 are not GPU-ordered, 5,312 of them one
  3836x2158 format 50 draw target that holds pending units of its own (two per frame).
- Time inside refresh flushes: 3.4 s of 108.7 s (1.2 ms per frame): 2.3 s in 5,118 that found the writer finished,
  0.8 s in 495 that waited (`finish`), 0.35 s in 11,461 GPU-ordered. All flushes of all reasons: 11.0 s (3.9 ms per
  frame); after refresh the largest are `buffer fill` (35,589 image stores) and `imported buffer region` (13,435).
- The flag-off half of the A/B did not run: the first attempt hit a script being edited, the second waited 10 minutes
  for the title lock and was refused. Command, from `work/ppsa30552/boot/fast`:
  `BIN=$PWD/native_fast_rta ANYPS5=<nps5>/wt/anyps5-rtalias-link/build-arm64/core/libs/libs/unpatched ./run.sh rta_off 120 "APS5_TRACE_FLUSH=1"`
  (a private copy of the binary, because every `run.sh` ends with a `pkill` of its binary path and kills a queued run
  of the same path). The verify mode hashes accessed ranges, so a timing pair should leave `APS5_RTALIAS_VERIFY` out.

On the title the write-first class of section 1 is small; the remaining cost is the 3836x2158 target that is
refreshed with units of its own pending (item 2 below), and reads, which no ownership model removes.

## 7. Handoff

Done: attribution trace, model, shared-key hand-over with the store-not-drop rule, verify mode, cap4 verification,
homebrew gate with the flag off (PASS: Calculator3D frame 0 max channel difference 0, Syobon 30.2 fps, Greenhollow
117.4 fps; `work/regress-rtalias`). The gate cannot pass the flag (the sandbox clears the environment), so the homebrew
titles have not run with `APS5_RTALIAS=1`.
Open:

1. The flag-off title run (section 6), then a clean timing pair without the trace and without verify.
2. GPU-ordered refresh for targets with DCC keys or pending units of their own (the 89 left in the replay, 5,613
   per 109 s on the title: the largest item). The refusal comes from
   `orderedSkipped` re-uploading the whole image; it needs the upload restricted to the units the flush stored.
3. A coverage proof for write-first targets (the 472): scissor and viewport cover the target, blending and tests
   off, no discard, and geometry proven to cover (rect list or a full-screen triangle whose positions the driver can
   read without running the shader). Then the hand-over of section 2 applies unchanged, with the upload skipped.
4. An in-process eager oracle for verify mode (store the handed units into a scratch copy at the hand-over and
   compare at each later store), so a title run can be checked, not only a replay.
5. Unit tests in `tests/Recorder.cpp` (`clearHandOverTests`) for the shared-key case and the store-not-drop rule.
