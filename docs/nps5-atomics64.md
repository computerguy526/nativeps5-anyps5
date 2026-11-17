# nps5: 64-bit buffer atomics and multisampled storage images on KosmicKrisp / Metal

Status 2026-11-14, branch `nps5-atomics64` (from `nps5-mesh`). Host: Apple M4 Pro, macOS 26.6.2, Metal 4,
Metal compiler 32023.922, KosmicKrisp at `mesa/MESA_COMMIT` + patches 0001 to 0003.

## 1. What the title's refused shaders need

The user's UE 5.6 title registers 1100-odd programs; on KosmicKrisp AnyPS5 refused eight at registration
(`could not be prepared after registration`). Request dumps were taken with `APS5_DUMP_SHADERS=1` and replayed
with `APS5_DUMP_IR=all agc_shader_replay --prepare` (dumps and IR stay under `work/`, they are title data).
All eight are compute programs embedded in the eboot image, not in the title's shader library.

| program | shape | refused for | what it does with the feature |
|---|---|---|---|
| `0x701d400`, `0x701dc00`, `0x701ee00`, `0x701f700` | 64x1x1 threads, wave64, no LDS | `64-bit buffer atomics need shaderBufferInt64Atomics` | per program: 1 `BufferAtomicIAdd32` (result used), 2 `BufferAtomicSwap64` whose **result is unused**, 2 `BufferAtomicOr64` whose result is used and whose operand is `CompositeConstructU64(SelectU32(active, 0, x), SelectU32(active, 0, y))`, i.e. **zero on every active lane** |
| `0x6efe300`, `0x6efe400`, `0x6efe600`, `0x6eff200` | 8x8x1 threads, wave64 | `storage image multisampling is unavailable on the target device` | `image_store` with instruction dimension `2D_MSAA_ARRAY` (MIMG dim 7): 1, 2, 6 and 1 stores of one constant colour to (x, y, slice = group id z, sample); the sample indices are 0 / 2,3 / 2..7 / computed |

No min, max, add or compare-exchange on 64 bits, no image atomics, no shared-memory (LDS) 64-bit atomics.

The four atomics programs implement a wave-to-wave hand-over: a wave publishes a `(count, state)` pair with
one 64-bit exchange (state 1 or 2 in the high dword), and a later wave spins on `or64(slot, 0)` until the high
dword is non-zero, then uses the low dword. So the 64-bit atomics are used as **an atomic 64-bit store and an
atomic 64-bit load**; what must hold is that a reader never sees the dwords of two different stores.

The four image programs are per-sample clears of multisampled targets. The title has no multisampled pipeline
(GPUSCAN: 24 graphics pipelines, sample count 1 in all of them) and no run so far dispatched them.

## 2. What Metal offers (measured with `xcrun metal`, `-std=macos-metal2.4` to `metal4.0`)

`device atomic_ulong*`:

| operation | compiles |
|---|---|
| `atomic_max_explicit`, `atomic_min_explicit` (unsigned; return `void`) | yes, from MSL 2.4 on |
| the same with the result read, `atomic<long>` | no |
| `atomic_load_explicit`, `atomic_store_explicit`, `atomic_exchange_explicit`, `atomic_compare_exchange_weak_explicit` | no (`no matching function`) |
| `atomic_fetch_add/sub/and/or/xor/min/max_explicit` | no |

So Metal has exactly two 64-bit atomics, neither returns the old value, and a 64-bit word cannot even be
loaded or stored atomically. None of the operations of this title maps to them. (They are the right tool for a
visibility-buffer max; the title does not do that through buffer atomics.)

Multisampled textures: `texture2d_ms<T, access::read>` and `texture2d_ms_array<T, access::read>` compile and
read by sample; `access::write` and `access::read_write` fail a static assertion
(`invalid access value 'a' for 'texture2d_ms'`, `_is_any_of(a, access::read)`). A shader cannot write a
sample of a multisampled texture in Metal.

## 3. What Vulkan lets the driver advertise

* `shaderBufferInt64Atomics` is all or nothing: load, store, exchange, compare-exchange, add, sub, signed and
  unsigned min/max, and/or/xor. Two native operations without results are not a feature.
* `shaderStorageImageMultisample` covers loads **and stores**. Reads alone are not a feature either.

Neither can be advertised honestly on Metal without emulation in the driver.

## 4. The lock emulation that was tried in KosmicKrisp, and why it is not in the patch series

`mesa/experimental/kosmickrisp-int64-buffer-atomics-lock-emulation.patch.disabled` (not applied by
`build-kosmickrisp.sh`): a NIR pass turns every 64-bit `global_atomic` / `global_atomic_swap` / atomic 64-bit
load / store into a critical section on a 32-bit lock word (device-wide table of 16384 words, address through
the root descriptor table, index = hash of the address), exposed through drirc `kk_int64_atomics` (default
off). Three things were learnt on the M4 Pro, each of them a reason against it:

1. **A spin lock can hang the GPU.** `loop { if (exchange(lock, 1) == 0) { critical section; break } }` compiles
   to a loop whose exit block is the critical section. The invocations of a SIMD group run in lockstep, so the
   one that took the lock waits, lock held, for the others of its group that spin on the same lock: any two
   invocations of one group on one word hang the dispatch forever. One such run hung the GPU for eight minutes
   and very likely caused the window server watchdog reboot of the machine at 21:47. The loop now leaves
   through a `simd_ballot(!done) == 0` exit taken by the whole group (the structure AnyPS5 uses for its LDS
   lock) and gives up after 2^22 failed attempts.
2. **Relaxed atomics do not make a lock.** Metal only has `memory_order_relaxed`. Without
   `atomic_thread_fence` around the critical section, updates were lost between thread groups (4096 adds on
   64 words: 40 words wrong). With fences the read-modify-write operations pass (24 cases x up to 262144
   invocations: add, signed/unsigned min/max, and, or, xor, exchange, compare-exchange, all correct against
   the CPU), but concurrent atomic stores against atomic loads still show a word made of two stores about
   once per 10^5 loads, whether the lock is released by an atomic store or an exchange and whether the data
   is written as two atomic dwords, two exchanges or one 64-bit store. The cause was not found.
3. **A contended lock is very slow**: about 0.5 ms per hand-over between SIMD groups (32768 invocations on one
   word: 17 to 20 s; 262144 on 4096 words: 15 to 30 ms).

A feature that is occasionally wrong and can stall the GPU is not advertised, also not as an option in the
build the title runs on. The patch file and the test cases stay for whoever continues; the driver the project
builds is unchanged (patches 0001 to 0003).

## 5. Decision: lower exactly the two forms in the AGC recompiler

AnyPS5 already lowers 64-bit buffer atomics on a device without the feature when it can
(`bufferInt64AtomicsSplit`: or/and/xor with an unused result as two 32-bit atomics). This adds a second,
exact-form lowering.

`SpirvAnalysis.cpp`:

* `IsPlainLoadBufferAtomic64`: `BufferAtomicOr64` / `Xor64` / `IAdd64` whose data operand is a literal zero or
  a `CompositeConstructU64` of two values that are each a literal zero or `SelectU32(active, 0, _)` with
  `active` the instruction's own active-lane argument. The memory is not changed by such an instruction.
* `IsPlainStoreBufferAtomic64`: `BufferAtomicSwap64` whose result no instruction reads.
* `bufferInt64AtomicsPlainOnly`: every 64-bit buffer atomic of the program has one of the two forms (an
  address-space 64-bit atomic, any other opcode or a read result clears it).

`SpirvEmitter.cpp` / `SpirvModuleSetup.cpp` / `SpirvMemoryInstructions.cpp`: on a target without
`Int64Atomics`, a `PlainOnly` program is emitted in `bufferInt64AtomicsPlain` mode: the existing
`EmitRuntimeBufferAtomic(…, 64, …)` path (same descriptor, bounds, alignment-fault and BDA handling as the
real atomic) receives one `OpStore` or one `OpLoad` of the 64-bit word with `Volatile|Aligned 8`, followed
(the load also preceded) by the device-scope `OpMemoryBarrier` every AGC atomic gets. The module declares
`Int64` but not `Int64Atomics`. A device that has the feature takes the unchanged atomic path; a program
with any other 64-bit atomic is refused as before.

**Assumption, and its evidence.** A naturally aligned 8-byte load or store of device memory is one access on
the host GPU: a load never returns the dwords of two different stores. Vulkan does not promise this. It is
measured for Apple GPUs by `mesa/tests/atomics` (section 7): 50 million plain 64-bit loads against as many
plain 64-bit stores on 1, 64 and 4096 contended words, no torn word, while the negative control (the same
loads against dword-wise stores) sees more than 99 % torn words on contended words. KosmicKrisp emits such
an access as a single `*(volatile device ulong*)` dereference. On a driver where the test fails the lowering
must be disabled; it only engages when the device lacks `shaderBufferInt64Atomics`, which among the drivers
AnyPS5 runs on is KosmicKrisp and MoltenVK (both Metal).

What is not preserved: nothing orders the plain access against a 32-bit atomic or a 64-bit atomic of another
program on the same word beyond the barrier; the forms that are lowered do not need that (a store replaces
the word, a load returns some stored word).

## 6. Multisampled storage images: single-sample lowering

Not implementable as a driver feature (section 2). The recompiler change (`SpirvDescriptors.cpp`,
`StorageMultisampleLowered`): on a target without `StorageImageMultisample`, a storage image whose guest
dimension is multisampled is declared as the single-sample image of the same shape; a store to sample 0 is
the texel store, a store to any other sample is dropped, a load of another sample returns zero. That is the
behaviour of a one-sample image. One warning is logged the first time a program is compiled this way.

The binding metadata still reports the multisample shape, so the AGC driver binds such a program only to a
guest texture of multisample dimension, and on a device without the feature it cannot create a multisampled
storage image (`MultisampleTarget.cpp`, `MultisampleUpload.cpp`, `SampleOrder.cpp` all require the feature and
refuse the pass with their existing messages). So the lowered program can only ever run on a one-sample
image, for which it is correct. A title that really renders with MSAA and clears or uploads samples from
compute still needs a real implementation (for example a layered single-sample shadow image plus a
per-sample fragment copy); nothing here provides that.

## 7. Tests

`mesa/tests/atomics/run.sh <icd json> [dir] [operation [words]]` (GLSL through glslang, CPU reference in
`atomics_test.c`, whole run under `gtimeout -s KILL 60`, no shader loop without a bound):

* `plain` (always): writers store 64-bit words whose high dword is the complement of the low one, readers
  load them, plain accesses on a `volatile` block, through a storage buffer binding and through a
  `buffer_reference` pointer; 262144 invocations x 64 rounds on 1, 64, 4096 words. Result on the M4 Pro:
  0 torn words in 6 x 8388608 loads.
* `control` (always): the writers store the two dwords separately; must show torn words (it shows 8382880 of
  8388608 on one word), otherwise the `plain` case proves nothing.
* With `kk_int64_atomics=true` on a driver built with the experimental patch: add, umin, umax, smin, smax,
  and, or, xor, exchange, compare-exchange (one winner), a bounded load + compare-exchange add loop, and
  atomic load/store. Checked per case: final words against the CPU fold, and that the returned values are
  consistent with some serial order (`{returned} + {final} == {op(returned, operand)} + {initial}` per word).
  Result: 70 of 72 cases pass, `loadstore` on 64 words fails with up to 73 torn words per 524288 loads (2 of 81 cases in a full run).

Recompiler: the eight request dumps of the title replayed with `agc_shader_replay --prepare --spv` now all
prepare; the SPIR-V passes `spirv-val --target-env vulkan1.3`; the atomics programs contain four
`OpStore … Volatile|Aligned 8` and four `OpLoad %ulong … Volatile|Aligned 8` and no `Int64Atomics`
capability; the image programs declare `OpTypeImage … 2D 0 arrayed 0 2 Unknown` (not multisampled).
No synthetic recompiler unit test was added (the AGC test suite is not part of the arm64 configuration).

## 8. Limits

* The plain lowering covers the two forms above only. A real 64-bit read-modify-write (Nanite's software
  rasteriser on a buffer, for example) is still refused on Metal; for unsigned max/min without a result
  Metal's two native operations would be the route, for anything else there is no sound one yet.
* The no-tearing property is measured on one GPU (M4 Pro), not guaranteed by any specification.
* Multisampled storage images are single-sample only (section 6).

## 9. Verification

Homebrew, AnyPS5 libraries of this branch against those of its base (`nps5-mesh`), KosmicKrisp kk-mesh:

* Calculator3D: frame 0 at full size is byte-identical to the Rosetta x86 reference
  (`work/regress/refs/calc3d/frame0_rosetta.bmp`).
* Syobon: the regression gate's title check passes (1584 frames in 60 s, largest flip gap 0.6 s, reference
  frames and audio within thresholds).
* Greenhollow: 66 of 66 dumped frames of a 35 s run are byte-identical to the run with the base libraries.
* None of the three compiles a program through either lowering (no warning line, no 64-bit buffer atomic).

(The gate's own Calculator3D and Greenhollow checks abort in guest code on an unresolved
`__cxa_allocate_exception` when its freshly built binaries run on any `nps5-mesh`-based library build, base
included; the comparison above therefore uses the gate's existing binaries with `DYLD_LIBRARY_PATH`.)

Title (one 150 s run, cold caches, default output resolution, AnyPS5 libraries of this branch, KosmicKrisp
kk-mesh, boot2's translated build): all 1125 programs the title registered were recompiled, none refused
(before: 8 refused in every run); the multisample warning is logged once; no GPU error. 33 frames were
presented in 137 s; the first four are black, from frame 4 on the title's main menu is drawn (title text and
menu entries, 13578 to 27203 non-black pixels at 960x540; the background behind the menu is black). The run
did not get past the menu, and nothing in the log shows whether any of the eight programs was dispatched,
so the run proves that they compile on the device, not that their results are right.
