# nps5: mesh shaders on KosmicKrisp (design decision and status)

## 1. What the title submits

Measured with `APS5_MESH_CENSUS=skip` (Execution/src/Driver/Draw/Draw.cpp: one line per distinct
geometry-path draw shape; `skip` drops those draws on a device without mesh shaders so a whole frame
can be surveyed) on the user's UE5 PS5 title, frame 1, KosmicKrisp without mesh shaders:

| VGT_SHADER_STAGES_EN | what it is | front binary | group shape | draw |
|---|---|---|---|---|
| `0x2000` (PRIMGEN_EN only) | NGG **primitive shader with a workgroup** (vertex program that needs LDS / subgroup culling) | type 2 (VS) | triangle list, 21 prims / 63 verts per group, 64 threads, wave64, max 64 verts / 64 prims, LDS 640 or 2304 dwords | indexed, **indirect** (index counts 162 .. 16128) |
| `0x2030` (PRIMGEN_EN, GS_EN, ES=REAL) | NGG **geometry shader** (merged ES+GS) | type 2 | list: 8 prims / 24 verts per group, 256 threads, max 252 verts / 224 prims, LDS 2688 dwords; strip (input primitive 6): 62 prims / 64 verts per group, 192 threads, max 192 verts / 64 prims | indexed direct (6 indices) and non-indexed instanced (4 verts x 64 instances) |

So: PS5 has no API-level mesh or task shaders. The title submits RDNA2 NGG programs; AnyPS5 already
recompiles the NGG "geometry path" (`ShaderPath::Geometry`) to **one Vulkan mesh shader per program
pair, no task shader** (SpirvModuleEmitter `EmitMeshEntryPoint`), and draws with
`vkCmdDrawMeshTasksEXT(groups, layers, 1)` (direct) or `vkCmdDrawMeshTasksIndirectEXT` (driver-resolved
indirect). 6 shapes in the first 36 mesh draws; the frame continues after them.

What the recompiled mesh SPIR-V needs from the device (ShaderValidation.cpp, Pipeline.cpp):
SPIR-V 1.4, `MeshShadingEXT`, triangles (points/lines possible), `SetMeshOutputsEXT`, per-vertex
position / parameters / clip+cull distances, per-primitive `CullPrimitiveEXT`, `Layer` and user
per-primitive outputs (barycentric emulation), workgroup memory (LDS, up to ~11 KiB here plus staging),
`LocalInvocationIndex` / `WorkgroupId`, push constants, physical storage buffers, subgroup ops in the
mesh stage (wave ops), and limits: >= 256 output vertices, >= 256 output primitives, >= 256 (128 when a
wave64 program runs on 32-wide subgroups) invocations per workgroup.

## 2. Routes

| | (a) KK NIR->MSL emits Metal mesh functions | (b) KK + SPIRV-Cross for task/mesh stages | (c) direct-Metal side path for mesh draws |
|---|---|---|---|
| Effort | new NIR lowering pass + ~300 lines of MSL emission + pipeline/draw plumbing | SPIRV-Cross integration into the Mesa build + a second descriptor/push-constant/BDA ABI + the same pipeline/draw plumbing | re-implement render pass, dynamic state, descriptors, depth/stencil and blending for those draws outside Vulkan |
| Risk | Metal mesh semantics (thread model, limits) to learn once | **ABI mismatch**: KK shaders do not use Metal resource bindings; `kk_nir_lower_descriptors` turns every descriptor, push constant and BDA access into raw loads from `buf0`/`sampler_table` argument buffers. SPIRV-Cross MSL would need its resource model rewritten to that layout, and the fragment stage (KK-compiled) must link by `[[user(...)]]` names against a SPIRV-Cross mesh struct | highest: mesh draws happen mid render pass, interleaved with vertex draws, sharing targets/depth; MTL4 encoders cannot be shared with KK without patching KK anyway |
| Fidelity | same lowering as every other stage (robustness, null descriptors, workarounds) | two compilers disagree on edge cases (bool/int widths, robustness) | lowest; state duplicated |
| Evidence | spirv_to_nir already handles mesh (anv/radv/nvk); `xcrun metal -std=metal4.0` accepts `[[mesh]]` functions with `threadgroup` arrays, `threadgroup_barrier`, threadgroup atomics, `simd_ballot/sum/shuffle/broadcast_first` and all the simdgroup/threadgroup attributes KK already emits | Homebrew `spirv-cross` 1.4.363 (Apache-2.0, compatible) does emit MSL mesh shaders (`--msl-version 30000`: threadgroup staging arrays, thread 0 sets counts, strided `set_vertex` loop), so the back half exists; nxnative uses it because its whole chain is SPIRV-Cross | none positive; `VK_EXT_metal_objects` exports device/queue/textures but not KK's live MTL4 render encoder |

Compute-emulation fallback (if Metal mesh shaders prove unusable for some program): run the mesh
shader as a compute kernel that writes the staged vertices/indices to a buffer, then an indexed draw
with a pass-through vertex shader, exactly how KK already emulates GS/tessellation (libkk + poly).
Works on any Metal version, costs a compute pass + render-pass split per draw and a buffer per draw;
kept as the fallback, not implemented.

## 3. Decision: route (a)

KosmicKrisp's own compiler emits Metal mesh functions and KK exposes `VK_EXT_mesh_shader`
(mesh stage only; `taskShader = false`, which the AGC recompiler never uses). Reasons: no second
resource ABI, the fragment stage links with the mesh stage through the same varying naming, the
AnyPS5 side needs no change beyond accepting the device (its mesh path is already complete and tested
on other drivers), and SPIRV-Cross's own generated MSL (route b) served as the reference for the Metal
mesh idiom, so the design is known to work. Route (b) is rejected on the descriptor ABI mismatch,
route (c) on state duplication; a full direct-Metal backend stays out of scope.

### Design of the KK change (mesa/patches/0003)

* `compiler/msl_nir_lower_mesh.c`: Vulkan mesh outputs are arbitrary per-component writes to any
  vertex/primitive from any invocation; Metal wants `mesh.set_vertex(i, whole struct)`. The pass turns
  every mesh output variable into workgroup (threadgroup) memory, stages `SetMeshOutputsEXT` counts
  there, and appends an epilogue: barrier, `set_primitive_count`, then each invocation emits the
  vertices and primitives `i ≡ invocation (mod workgroup size)` via `store_per_vertex_output` /
  `store_per_primitive_output` into a thread-local struct followed by a commit intrinsic
  (`mesh_commit_vertex_kk` -> `set_vertex`, `mesh_commit_primitive_kk` -> `set_index` x n +
  `set_primitive`). Clip distances go to `[[clip_distance]]`, cull distances are folded into
  `[[primitive_culled]]` together with `CullPrimitiveEXT`.
* `nir_to_msl.c` / `msl_iomap.c`: `[[mesh]]` entry point, `MeshVertexOut` / `MeshPrimitiveOut` structs,
  `metal::mesh<...>` type from max vertices/primitives/topology, compute-style system values.
* Vulkan side: the mesh shader is the pipeline holder like the vertex shader
  (`MTL4MeshRenderPipelineDescriptor`), argument tables are bound for the mesh render stage,
  `vkCmdDrawMeshTasksEXT` -> `drawMeshThreadgroups`, `...IndirectEXT` / `...IndirectCountEXT` ->
  `drawMeshThreadgroupsWithIndirectBuffer` (same 3 x u32 layout as `VkDrawMeshTasksIndirectCommandEXT`).
* Subgroups: `subgroupSupportedStages` and `requiredSubgroupSizeStages` gain the mesh stage (size 32).

## 4. Status (2026-11-14)

Done:

* `mesa/patches/0003-kosmickrisp-ext-mesh-shader-metal-mesh-functions.patch`, built by
  `mesa/build-kosmickrisp.sh` into `work/s3/kk-mesh/install-arm64`.
* `mesa/tests/mesh/run.sh <icd json>`: three cases, 19 pixel checks, all pass on the patched driver
  (and the test reports `FAIL: device has no meshShader feature` on the unpatched one):
  `tri` (1 invocation, per-vertex + per-primitive colour, mesh-stage push constant, direct draw),
  `grid` (64 invocations x 2 workgroups, workgroup memory + barrier, SSBO + UBO in the mesh stage,
  subgroup ballot, `gl_CullPrimitiveEXT`, `vkCmdDrawMeshTasksIndirectEXT`, vertex-pipeline draws before
  and after in the same pass), `big` (256 invocations, 255 vertices / 85 primitives, clip and cull
  distances, `vkCmdDrawMeshTasksIndirectCountEXT`).
* The AnyPS5 recompiler needed no change: with the device reporting `meshShader` its existing NGG ->
  mesh SPIR-V is accepted and compiled by KosmicKrisp for all six shapes of the title
  (`KK_MESH_DEBUG=1`: 32/96/128 threads, up to 252 vertices / 224 primitives, 4.6 to 26.5 KiB of
  threadgroup memory including the staged outputs).
* The title passes frame 1: 11 frames presented in 69 s of a warm run, 256+ mesh draws, no GPU error.
* Unchanged: Calculator3D frame 0 (same bytes as before and as the x86 reference), Syobon and
  Greenhollow render (Greenhollow frame 85 equals the old stack's).

Facts learnt about Metal mesh functions (macOS 26, M4 Pro):

* Threadgroup memory of a mesh function is limited to 32768 bytes (pipeline creation error
  "Threadgroup memory size (N) exceeds the maximum threadgroup memory allowed (32768)").
* A mesh function cannot take a `[[threadgroup(n)]]` buffer argument (KK_WORKAROUND_19 is skipped
  for the stage) and a `[[point_size]]` member is rejected for non-point topologies.
* A fragment input fed by a per-primitive mesh output links by `[[user(name)]]` like any varying
  (`[[flat]]` on it is accepted).
* `VK_SHADER_STAGE_ALL_GRAPHICS` lacks the mesh bit: KK dropped `vkCmdPushConstants(MESH)` before.

Not done / limits:

* No task (object) stage: `taskShader = false`. Not needed by the AGC recompiler.
* Staging doubles the memory of outputs the recompiler already stages itself. A "direct" mode (every
  store group writes a whole vertex/primitive in one block -> fill the struct and `set_vertex` in
  place, no threadgroup copy) would remove the copy and the 32 KiB coupling; the title's largest
  shader is at 26.5 KiB today.
* A fragment shader reading `gl_ClipDistance`/`gl_CullDistance` after a mesh shader, multiview mesh,
  mesh shader queries, `gl_DrawID` in the mesh stage and depth-clip-control (`-1..1`) lowering are
  not implemented for the mesh stage.
* The title's frames 0 to 10 are black, and it then stops on a guest null dereference (CPU side).
  Run with `kk_fake_depth_bounds=true` (the title needs `depthBounds`; KosmicKrisp only fakes it).
  Shaders the recompiler refuses on this device (64-bit buffer atomics, multisampled storage images)
  are reported at registration and not yet drawn with.

## 5. Workgroup id base (mesa/patches/0007, 2026-11-15)

The driver draws a mesh pipeline with `vkCmdDrawMeshTasksEXT(groups, instances, layers)`, and the
recompiled shader takes the primitive group, the instance and the layer from `gl_WorkGroupID.xyz`.
Patch 0003 left one thing of the compute path on for the mesh stage: `kk_preprocess_nir` lowered the
compute system values with `has_base_workgroup_id`, so the MSL added a base to the workgroup id, read
from the root table at `cs.base_group`. A mesh shader runs with the graphics root table, where those
three words are `draw.buffer_strides[0..2]` (the structs share a union). `kk_flush_dynamic_state`
writes the stride of every valid vertex binding there and nothing clears them before the command
buffer ends.

Effect: a mesh draw recorded after a draw that read vertex buffers in the same command buffer ran
with its workgroup id shifted by the strides of bindings 0 (group), 1 (instance) and 2 (layer). With
a shifted group the recompiled shader's vertex window lies past the draw's vertex count and it emits
nothing. On PPSA30552 this was Unreal's 32^3 tonemap LUT draw, which only changed the LUT when no
vertex-buffer draw preceded it in its command buffer (the dark gameplay after a level load; the
interim driver workaround was `prepareDraw` submitting the open batch before a draw into a volume
target, `APS5_NO_VOLUME_DRAW_OWN_BATCH=1` turns that off).

Patch 0007 asks for the base in compute shaders only. Test: `mesa/tests/vol3d/run_layered.sh`
(cases `after-vertex-buffer-draw-pass`). Driver-side counter, off by default:
`APS5_TRACE_MESH_BASE=1` prints `[mesh-base]` lines: per tonemap LUT draw the vertex strides its
command buffer was left with, and every 600 presents how many mesh draws were recorded over nonzero
strides, by colour target.

## 6. Direct outputs (mesa/patches/0008, 2026-11-16)

ASTRO BOT (PPSA21564) entering its world map: `[kk-mesh] mesh shader needs 48088 B of threadgroup
memory ... Metal allows 32768 B: pipeline creation fails` -> `vkCreateGraphicsPipelines`
VK_ERROR_INVALID_SHADER_NV -> the title aborts. The shader (64 threads, 80 vertices, 40 primitives,
barycentric emulation: 2 per-vertex and 11 per-primitive outputs) has 27.9 KiB of workgroup memory of
its own (LDS + the recompiler's staging); KosmicKrisp's staged copy of the declared outputs added
20 KiB. The recompiler does not size anything from the advertised mesh limits (they are only checked,
`ValidateMeshLimits`), so advertising other limits would not have produced a smaller shader.

Patch 0008 implements the "direct mode" of section 4 for the case that matters: the recompiler's mesh
epilogue (`EmitMeshEntryPoint`) writes whole vertices and primitives at an index that depends on the
invocation alone (`LocalInvocationIndex`; `i + (i & ~31) + 32 * lane` with two lanes; `(SubgroupId <<
5) + SubgroupLocalInvocationId` for a spanned wave). The lowering evaluates such indices for every
invocation, proves that no element has two writers, keeps the outputs in function-local variables and
lets each invocation commit the elements it owns. It is only used when the staged copy would exceed
the limit; `KK_MESH_DIRECT=1` forces it (PPSA30552's cap4 replay is bit-identical with it: its two
mesh shaders go 16920 -> 9496 B), `KK_MESH_DIRECT=0` disables it.

Limits that remain:

* The shader's own workgroup memory must fit 32 KiB (27.9 KiB on the ASTRO BOT shader: 4.8 KiB left).
* Metal limits the mesh itself to 32 KiB: max vertices x `MeshVertexOut` + max primitives x
  `MeshPrimitiveOut` ("Total mesh size (N) exceeds the maximum mesh size allowed (32768)" from
  pipeline creation; `KK_MESH_DEBUG=2` prints the structs). Not checked by the driver. With direct
  outputs the varying linker removes what the fragment shader does not read, staged outputs are always
  emitted in full.
* A class with an output that must stay staged (cull distances; clip distances, whose indirect
  accesses nir turns into an if-ladder over constant indices) is only direct when the owners cover
  every element, which cannot be shown for indices by subgroup id: such a shader over the limit still
  fails.
* Writes that are not owner-indexed (index from a loop counter, from data, one invocation writing for
  others) keep the staged copy.
