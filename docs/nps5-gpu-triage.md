# GPU blocker triage for UE 5.6 titles on KosmicKrisp (nps5-gputriage)

Scope: GDS, async compute, VRS, and other packets/imports the title may hit after mesh shaders. Mesh shaders, 64-bit atomics, multisampled storage images and ray tracing are owned elsewhere. Findings come from reading the code in this tree plus the title's feature scan (counts and names only) and the b2 run logs.

## Summary

| item | current behaviour | risk to the title | fix |
|---|---|---|---|
| mem.gds | GDS is a 64 KiB host allocation with a guest address (`Pm4::GdsAddress`, shared-memory backed and imported into Vulkan on integrated GPUs). Shaders see it as a `DescriptorRole::Gds` storage buffer; DS ops with the GDS bit, `ds_append`/`ds_consume` and `ds_ordered_count` lower to device-scope atomics on it. CP packets (DMA_DATA, COPY_DATA) read/write it. This is the same model as KytyPS5 (64 KiB buffer plus barriers). | Low. The scan found 0 compiled shaders needing GDS; the title imports only `sceAgcAcbAtomicGdsGetSize`, not the packet writer. Gaps: `ds_gws_*` (global wave sync) throws at recompile, so such a shader would be refused; `ds_ordered_count` is a plain atomic add with no cross-workgroup ordering wait; RELEASE_MEM data select 5 (GDS) is rejected. If the host import fails on KK the driver falls back (`RejectGdsImport`) to a copy path. | No code change. Documented. If a GDS shader appears: ordered count is only wrong for ordered-output NGG culling and is acceptable; GWS needs a device-wide barrier emulation (not worth doing speculatively). |
| queue.async_compute | AnyPS5 already runs one host worker thread per guest queue (gfx = queue 0, ACBs from 0x20). Threads parse PM4 concurrently but all GPU work is recorded into one shared recorder and submitted on the single Vulkan queue in a deterministic order (`received` serials). WAIT_REG_MEM blocks only that guest queue's host thread (spin, then sleep, with a 1 s timeout warning), never the Vulkan queue. Waits are first satisfied from a label table of writes the recorder holds but has not submitted, and the recorder is flushed (`SubmitRecorded`) when the awaited address overlaps open writes. Ordering: queue 0 yields to earlier wait-free ACB submissions; an ACB containing a wait is released only after earlier queue-0 work, unless queue 0 is itself blocked on an address that ACB will write (`orderReleased`). | Low for host-side deadlock; this is equivalent to KytyPS5's suspend-on-wait software scheduler, so I did not adopt Kyty code. Remaining GPU-side risk: a compute shader that spins on a flag written by a different dispatch (persistent-thread patterns) would hang on a serialized queue. Nothing in the scan indicates this; Nanite/Lumen use ordinary dispatches. MEM_SEMAPHORE is emulated with host atomics and also blocks only its thread. | **ATOMIC_MEM (PM4 0x1e) implemented** (see below): the title imports `sceAgcAcbAtomicMem` and the validator rejected the opcode as unknown, which would have aborted the run on first use. |
| frag.vrs | No code reads VRS state. PA_CL_VRS/DB VRS registers are stored in the per-queue context map and ignored; shaders render at 1x1. HTILE with a VRS encoding is parsed (`htileVrsEncoding`) but only matters to the native multisample-depth import, which refuses it. | Low: performance only. Shaders that export a shading rate from the vertex stage (pos1 misc export) have no Vulkan consumer; the value is dropped. Ensure `r.VRS.Enable=0` in the cvar set as belt and braces. | None needed. |
| other: unimplemented packets/imports | See the list below. | Medium for `sceAgcCbMemsetExclusive`, low for the rest. | ATOMIC_MEM implemented; the rest listed with proposals. |

## ATOMIC_MEM (fix made)

`sceAgcAcbAtomicMem` / `sceAgcDcbAtomicMem` emit PM4 0x1e (9 dwords). The driver's opcode table did not list 0x1e, so `Pm4::UnsupportedReason` returned "opcode is not known in the reference" and submission validation threw, which kills the run.

Now supported in `core/libs/prx/libSceAgcDriver/Execution/src/Pm4.cpp`:

- Op encoding in dword 1 bits [6:0]: bit 6 = 64-bit operand; low 5 bits: 7 swap, 8 compare-swap (compare in dwords 6-7), 15 add, 16 sub, 17 smin, 18 umin, 19 smax, 20 umax, 21 and, 22 or, 23 xor, 24 inc (wrap to 0 at src), 25 dec (wrap to src). Bit 5 (return flag) is ignored: ATOMIC_MEM has no destination.
- Execution is a host compare-exchange loop on guest memory after `GuardRead`/`FlushGpuWrites`/`CheckRange`, stored through `StoreOwnBytes` so write tracking sees it, the same path MEM_SEMAPHORE uses.
- Command field 0, 2, 3 run as a single atomic. Command 1 (loop until compare satisfied) and values above 3 are rejected with a clear message. Unknown ops and misaligned or null addresses are rejected.
- Added to the opcode table, `AccessesMemory` and the packet-effect description (`runTime` store), so ordering against GPU writes uses the existing flush logic.

Caveat: the op numbering follows the AMD TC_OP_ATOMIC encoding (consistent with the existing packet test values 0x2f and 0x6f). It has not been checked against a console capture.

Tests: `testAtomicMem` in `core/libs/prx/libSceAgcDriver/tests/Pm4.cpp` (target `agc_driver_pm4_tests`): validation failures, 32/64-bit add, swap, compare-swap, signed/unsigned min/max, inc/dec wrap, and/or/xor, executed through a compute queue (0x20).

## Other findings

- `sceAgcCbMemsetExclusive` (imported by the title) is a `NotImplemented_nid_no_patch` stub and aborts when called. The prototype in `libSceAgc/include/Agc.hpp` (`dst, pattern*, size`) looks like a guess, so I did not implement it blind. Proposal: confirm the real argument order from the title call site (log args in a probe build), then emit a DMA_DATA fill (data select immediate) or WRITE_DATA, both of which the executor supports.
- Debug exports the title imports (`sceAgcDriverGet/SetSubmitValidation*`, `GetShaderDebuggingStatus`, `SetValidationErrorOutputFrequency`) return benign codes; fine.
- Validator rejections that a UE title could still hit: SET_PREDICATION with query operations (occlusion predicate; memory predication works), DRAW_INDEX_MULTI_INST, register-space WAIT_REG_MEM/COND_WRITE, MEM_SEMAPHORE mailbox waits, RELEASE_MEM GDS data, CE/DE counter packets, GET_LOD_STATS. None appear in the b2 logs. Each aborts with a named reason, so the next boot run will report it.
- Shaders that could not be prepared in b2 logs: 64-bit buffer atomics (4) and multisampled storage images (4), both owned by other agents. 96+ shaders report "s_swappc_b64 calls a computed target" and are prepared at first use, not an error.
- Unrecorded: `ds_gws_*` appear nowhere in the scan.

## Disk

Fresh `build-arm64` with `-DAPS5_HOST_ARM64=ON` (Release, arm64), target `agc_driver_pm4_tests` only. 3rdparty submodule dirs are symlinked to `anyps5-arm64/3rdparty` as in the mesh worktree; these symlinks are local and must not be committed.
