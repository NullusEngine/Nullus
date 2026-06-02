# Research: Optimize Draw-Call Scalability

## Decision 1: Follow UE's layered draw-call strategy, not a single batching trick

**Decision**: Model the Nullus solution after UE 4.27's layered approach: cached draw commands, state-bucket dynamic instancing, pass-level parallel setup/draw, and a later ISM/HISM-style persistent instance path.

**Rationale**: UE's `FMeshDrawCommand` is a compact draw description captured just above RHI submission and designed for caching. `FVisibleMeshDrawCommand` carries view-specific visibility/sort state separately, with `StateBucketId` used to merge compatible visible commands. `SortAndMergeDynamicPassMeshDrawCommands` sorts visible commands, builds primitive-id data, and increases `NumInstances` when adjacent commands share a state bucket. `FParallelMeshDrawCommandPass::DispatchPassSetup` and `DispatchDraw` then distribute setup and draw submission across task graph workers.

**Local UE references**:

- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Public\MeshPassProcessor.h`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\MeshDrawCommands.cpp`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private\SceneVisibility.cpp`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Engine\Private\InstancedStaticMesh.cpp`
- `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Engine\Private\HierarchicalInstancedStaticMesh.cpp`

**Alternatives considered**:

- Only add explicit instanced components: rejected because it does not help imported scenes or repeated normal mesh renderers.
- Only add parallel command recording: rejected because compatible repeated objects would still waste command list and binding work.
- Jump directly to GPU indirect rendering: rejected as too large for the current RHI and validation surface.

## Decision 2: Extend Nullus' existing RenderScene cache and dynamic instancing instead of replacing it

**Decision**: Keep `RenderScene` as the owner of persistent primitives and cached draw state, then add explicit optimization stats and stronger state-bucket/instance-group handling around the existing opaque merge path. The MVP keeps instance grouping per-frame; persistent group caches are a follow-up only if profiling shows the merge step itself remains a bottleneck after draw submission is reduced.

**Rationale**: Nullus already caches render commands in `Runtime/Engine/Rendering/RenderScene.cpp`, sorts opaque drawables, and merges compatible opaque commands into instanced drawables when shaders support indexed object data. Existing tests cover cache reuse, material invalidation, visibility, transparent ordering, dynamic instancing, and object-index ranges. Building on this avoids a large renderer rewrite and keeps the current `EngineFrameObjectBindingProvider` object-data upload path.

**Alternatives considered**:

- Move grouping into `CompositeRenderer`: rejected because by then visibility, sorting, and scene identity are already flattened.
- Store groups only in material or mesh objects: rejected because grouping depends on scene object visibility, render state, culling, and per-frame object-index ranges.

## Decision 3: Add pass slicing and DX12 in-render-pass child recording for large recorded draw sets

**Decision**: Split large `RenderPassCommandInput.recordedDrawCommands` arrays into ordered `ParallelCommandWorkUnit` slices. Attachment-free passes can replay slices as ordered serial work units. Attachment-backed DX12-capable passes use UE-inspired draw-range partitioning with `D3D12_COMMAND_LIST_TYPE_BUNDLE` child command buffers: the parent owns render pass begin/end, clears, barriers, and dependency visibility; workers record draw ranges into child command buffers; the parent executes children in source order inside one render pass. Unsupported or unsafe paths fall back to the original unsliced `passCommandInputs`.

**Rationale**: Nullus creates work units per pass in `RenderScenePackageBuilder` and FrameGraph plan materialization. UE's draw dispatch splits visible mesh commands across task graph workers inside a pass. DX12 bundles provide the nearest backend-native equivalent for recording draw-range command buffers that can execute inside a parent command list's render pass without duplicating attachment begin/end or visibility transitions. Child command pools and command buffers are cached on the frame-context slot and reset only after that slot's fence is confirmed, matching UE's reuse/lifetime goal rather than UE 4.27's exact D3D12 command-context implementation while preserving Nullus RHI ownership rules. This keeps attachment state single-owner, preserves deterministic child execution order, avoids per-frame bundle allocator/list creation on heavy draw paths, and maintains the unsliced pass input as the authoritative fallback.

**Alternatives considered**:

- Create one work unit per draw: rejected due to command buffer, allocator, and scheduling overhead.
- Split all passes unconditionally: rejected because small passes, external editor outputs, and backends without in-render-pass child command buffers must stay simple and safe.
- Move splitting into the backend: rejected because pass semantics and draw ordering are renderer-level concepts.

## Decision 4: Keep HISM-style cluster culling as a follow-up phase

**Decision**: Document dense-instance hierarchy culling as a follow-up after MVP draw grouping and command slicing.

**Rationale**: UE's `UHierarchicalInstancedStaticMeshComponent` builds a cluster tree and uses it for dense-instance visibility/LOD decisions. UE 4.27's legacy foliage path can expose `InstanceRuns`, but the Mesh Draw Command path explicitly does not rely on that mechanism. Nullus should treat HISM as a future way to produce visible instance/object-id data or draw commands for the current cached-command path, not as a direct `InstanceRuns` port. The reported issue can be improved first by reducing submission overhead for repeated opaque objects and improving parallel recording of non-groupable draws.

**Follow-up insertion points**:

- `Runtime/Engine/Rendering/RenderScene.h`: keep the MVP `RenderCachedDrawCommand` compact. A future cluster path should add a separate persistent dense-instance owner keyed by mesh/material/state bucket, rather than attaching cluster-tree arrays to every cached draw command.
- `Runtime/Engine/Rendering/RenderScene.cpp`: `FinalizeOpaqueQueue` is the current dynamic grouping point. A future HISM-style path can replace the linear visible-object run with cluster-visible instance lists or draw commands before `AssignVisibleObjectIndices` assigns object-data ranges.
- `Runtime/Engine/Rendering/EngineFrameObjectBindingProvider.cpp`: instance matrices already flow through `DrawableObjectDescriptor::instanceModelMatrices`. A future persistent instance-buffer path should preserve this descriptor contract for small dynamic groups and add a separate buffer-backed descriptor only when cluster culling owns the instance storage.
- `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`: pass-level draw slicing remains orthogonal to cluster culling. Cluster culling should reduce recorded draw inputs before package slicing decides whether a large pass needs ordered work-unit slices.

**MVP boundary**: The current change intentionally does not add per-instance bounds, cluster nodes, GPU culling buffers, instance-run state, or indirect draw argument buffers to runtime data structures. `RenderSceneCacheTests.DenseCompatibleInstancesStayBoundedBySubmittedDrawLimit` locks the dense-instance boundary by verifying large compatible fields still collapse into bounded submitted draws without requiring HISM runtime fields.

**Alternatives considered**:

- Implement HISM first: rejected because it would not address general mesh-renderer scenes and would delay the simpler high-impact fixes.
- Ignore HISM entirely: rejected because the grouping data model should not block later cluster culling.

## Decision 5: Validate with counters and RenderDoc, not FPS alone

**Decision**: Treat FPS as a final smoke signal, but use draw counts, instance counts, cache rebuild counts, command work unit counts, and RenderDoc captures as acceptance evidence.

**Rationale**: FPS can be limited by GPU, windowing, editor UI, shaders, or capture overhead. Draw-call optimization should prove that draw submissions and command recording work decrease while rendered output stays correct.

**Alternatives considered**:

- Use only unit tests: rejected because runtime backend submission can still regress.
- Use only RenderDoc: rejected because cache invalidation and edge cases are easier to cover with deterministic unit tests.
