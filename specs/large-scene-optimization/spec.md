# Feature Specification: Large Scene Optimization

**Feature Branch**: `large-scene-optimization`
**Created**: 2026-06-03
**Status**: Design Complete
**Input**: User request: "Open a worktree and design a complete large-scene optimization plan. Do not leave an intermediate half-complete state. Deliver the complete package directly."

## User Scenarios & Testing

### User Story 1 - Large Scene Bottlenecks Are Measurable (Priority: P1)

An engine developer can load or synthesize a large scene and see where frame time is spent across scene synchronization, spatial queries, visibility, LOD, occlusion, draw-command reuse, draw submission, streaming, and editor presentation.

**Why this priority**: Nullus already has retained render-scene and draw-call optimization work, but a complete large-scene plan must keep each stage measurable so future implementation does not guess at the next bottleneck.

**Independent Test**: A synthetic large-scene unit or runtime harness reports deterministic counters for primitive count, candidate count, visible count, culled counts by reason, LOD/HLOD selections, occlusion test counts, streaming backlog, command rebuilds, grouped draws, and frame-stage timings.

**Acceptance Scenarios**:

1. **Given** a scene with many registered primitives, **When** a frame is rendered, **Then** renderer-owned stats expose total primitives, spatial candidates, visible primitives, visible meshes, and culled counts by stage.
2. **Given** a stable scene rendered for two consecutive frames, **When** telemetry is inspected, **Then** cached command rebuilds and spatial-index rebuilds are zero or limited to explicitly dirty primitives.
3. **Given** a large-scene validation run, **When** evidence is recorded, **Then** the report includes backend, build configuration, scene size, camera path, threaded-rendering mode, and hardware note.

### User Story 2 - Visibility Scales Without Full Per-Frame Primitive Scans (Priority: P1)

An engine user can navigate a large scene without the renderer evaluating every primitive every frame when most primitives are outside the current view or outside distance/layer budgets.

**Why this priority**: UE and Unity both layer visibility, LOD, occlusion, retained render state, and draw extraction so large scenes do not become "submit everything" frames. Their fallback paths may still scan primitive or renderer ranges, so Nullus's spatial candidate stage is an explicit local improvement that must prove it reduces the number of primitive records touched.

**Independent Test**: A partitioned synthetic scene with at least 100,000 static primitives and a moving camera proves the candidate set and the number of primitive records actually touched by visibility are significantly smaller than the registered primitive set for localized views, while serial and parallel visibility results remain identical.

**Acceptance Scenarios**:

1. **Given** static primitives distributed across multiple scene regions, **When** a camera views one region, **Then** visibility starts from spatial, layer, and distance candidate sets rather than every registered primitive.
2. **Given** dynamic primitives move between cells, **When** visibility runs after movement, **Then** stale bounds are not queried and moved primitives are either visible or culled according to their current bounds.
3. **Given** multiple views such as Scene View and Game View, **When** both render the same scene, **Then** each view owns independent visibility results while sharing retained primitive and spatial-index data.
4. **Given** the JobSystem is disabled or rejects work, **When** visibility requests parallel evaluation, **Then** the result falls back to the serial path without changing visible output.

### User Story 3 - LOD And HLOD Reduce Work Before Draw Submission (Priority: P1)

An engine user can inspect large imported environments, repeated prop fields, and distant scenery with stable LOD choices and cluster proxies so distant detail does not generate unnecessary per-object draw work.

**Why this priority**: Frustum culling alone cannot make a city, forest, or large imported environment interactive. UE uses HLOD and Unity uses LODGroup masks before draw extraction; Nullus needs both per-object LOD and cluster-level replacement above the existing draw-command cache.

**Independent Test**: A scene containing LOD groups and HLOD clusters verifies that per-view LOD masks choose exactly one active representation per group, HLOD proxy visibility hides child primitives when selected, and fade transitions do not double-submit outside configured transition windows.

**Acceptance Scenarios**:

1. **Given** a primitive with multiple LOD representations, **When** its screen-relative size crosses thresholds, **Then** visibility selects the expected LOD and records the selection.
2. **Given** a cluster whose proxy is selected for a distant view, **When** visible command queues are built, **Then** the proxy is submitted and child primitives are suppressed for that view.
3. **Given** a cluster is near the transition threshold, **When** hysteresis or fade settings apply, **Then** the result is stable across small camera movements and avoids popping where fade is enabled.
4. **Given** editor tools need to inspect individual objects, **When** HLOD is disabled for that view or selection mode, **Then** children remain individually visible without corrupting the shared cluster state.

### User Story 4 - Occlusion Culling Removes Hidden Work Without GPU Stalls (Priority: P2)

An engine user can fly through occluded areas of a large scene and avoid preparing draws for primitives hidden behind previously rendered depth without forcing CPU/GPU synchronization.

**Why this priority**: UE 4.27 uses HZB occlusion and occlusion queries with history; Unity uses Umbra precomputed static occlusion plus dynamic culling. Nullus should adopt a backend-safe HZB/history path first because the engine already owns RHI and frame-graph abstractions but does not have Umbra-style baked data.

**Independent Test**: A deterministic occlusion harness renders occluders and occludees, records HZB build and occlusion decision telemetry, and proves stale or missing occlusion data only causes conservative visibility, not missing objects.

**Acceptance Scenarios**:

1. **Given** no valid occlusion history exists, **When** visibility runs, **Then** primitives are treated as visible after frustum/LOD stages rather than incorrectly hidden.
2. **Given** valid HZB history indicates a primitive is hidden, **When** the next frame uses a compatible occlusion history key that matches view, depth convention, render scale, viewport, jitter policy, depth resource, primitive bounds generation, representation, and occludee eligibility, **Then** the primitive can be culled before draw submission and the decision is counted.
3. **Given** the camera cuts, projection changes, depth target changes, primitive bounds/transform generation changes, LOD/HLOD representation changes, material depth-write eligibility changes, or backend resources are invalid, **When** occlusion runs, **Then** history is invalidated and the system falls back to conservative visibility.
4. **Given** DX12 or another backend executes the HZB path, **When** RenderDoc or RHI validation inspects pass ordering, **Then** depth write, HZB build, occlusion test, and later consumption have explicit resource-state transitions.

### User Story 5 - Scene And Asset Residency Follow A Frame Budget (Priority: P2)

An editor or runtime user can move through a large world while scene cells, mesh LODs, textures, and HLOD proxies stream in and out under bounded per-frame CPU, IO, and GPU upload budgets.

**Why this priority**: Large scenes hitch when visibility asks for resources that are not resident or when streaming work commits too much in one frame. UE World Composition and Unity asset/render-node pipelines support separating visibility from residency or asset preparation. Nullus's CPU, IO, GPU upload, CPU memory, and GPU memory budgets are a local engine requirement that must be validated directly rather than inferred from UE or Unity.

**Independent Test**: A large tiled scene fixture drives a camera across cell boundaries and verifies that pending, resident, visible, and evicted states change only through budgeted transitions with fallback proxies or placeholders when high-detail resources are not ready.

**Acceptance Scenarios**:

1. **Given** a visible cell is not resident, **When** visibility selects it, **Then** the streaming system requests the required residency without blocking the render frame.
2. **Given** streaming work completes, **When** the frame budget allows commit, **Then** a bounded number of resources become render-ready and telemetry records the commit cost.
3. **Given** resources are over budget and not visible recently, **When** eviction runs, **Then** the system evicts safe resources without removing currently visible required resources.
4. **Given** editor asset drag/drop or imported prefab resolution is active, **When** large-scene streaming is also active, **Then** both paths share background/job/upload budgets rather than creating competing unbounded work.

### User Story 6 - Editor Debugging Remains Trustworthy (Priority: P2)

An editor user or renderer developer can inspect culling, LOD, HLOD, occlusion, streaming, and draw-call decisions in Scene View and FrameInfo without changing the decisions being debugged.

**Why this priority**: Large-scene optimizations are easy to misdiagnose. Debug overlays and stats must reveal why something was culled or streamed without forcing sync drains, cold asset loads, or full scene traversal.

**Independent Test**: Editor contract tests verify FrameInfo fields and debug overlays consume renderer-owned snapshots instead of rescanning the live scene or synchronously loading assets.

**Acceptance Scenarios**:

1. **Given** FrameInfo is open, **When** a large scene renders, **Then** it shows visibility, LOD, HLOD, occlusion, streaming, and draw optimization counters from the last renderer snapshot.
2. **Given** a primitive is not drawn, **When** debug culling inspection is enabled, **Then** the reported reason distinguishes layer, distance, frustum, LOD child suppression, HLOD child suppression, occlusion, not resident, and invalid material/resource.
3. **Given** an ordinary Scene View navigation frame, **When** debug overlays are disabled, **Then** the large-scene systems do not add editor-only traversal or synchronization work.

## Edge Cases

- A primitive may be registered before its mesh, material, or bounds are ready; visibility must conservatively skip or fallback without synchronous asset loading.
- A primitive may be destroyed while a visibility job is in flight; jobs must operate on immutable frame snapshots or generation-checked records.
- Additive scenes may be added, removed, or rendered with a different view; retained state must stay per scene and aggregate only at view-build time.
- A stable scene may render for many frames; large-scene `RenderScene::Synchronize` must not keep full-sweeping all `Scene::FastAccessComponents` when no components changed, except in baseline/debug fallback mode.
- Static primitives may change transform through editor operations; the spatial index must dirty and refit or rebuild their records.
- Static spatial topology may churn during streaming or editor batch operations; queries must prefer a last-good index plus dirty overlay or bounded rebuild over periodic O(N) fallback spikes.
- Dynamic primitives may move every frame; the dynamic index path must avoid rebuilding the static tree and must avoid scanning every dynamic primitive for localized dynamic-heavy views.
- Layer masks, editor selection visibility, camera clipping, disabled renderers, inactive objects, and custom culling modes must stay composable.
- Transparent objects cannot be HLOD-merged or occlusion-culled in a way that breaks order-dependent rendering without an explicit compatibility rule.
- HLOD proxies must not hide selected or edited children in editor views unless that view opts into proxy-only visualization.
- Occlusion history must not survive camera cuts, projection changes, projection jitter incompatibility, viewport-size or origin changes, render-scale changes, depth-resource identity changes, reversed-Z/depth-format changes, MSAA sample-count changes, primitive bounds/transform generation changes, LOD/HLOD representation changes, material depth-write eligibility changes, or backend reset.
- HZB occlusion depth must be built only from eligible opaque depth-writing scene geometry; transparent, overlay, editor gizmo, debug, non-depth-write, and custom-order passes must not be occluders unless a later contract explicitly proves their safety.
- HZB read/write resources must not be consumed in the same frame without explicit frame-graph or RHI barriers.
- Streaming must avoid evicting resources referenced by in-flight frames, prepared render packages, or pending RHI command buffers.
- Async mesh or texture uploads must not block the main/editor thread while waiting for a GPU fence unless an explicit validation/readback path requires it.
- Generated files under `Runtime/*/Gen/` remain out of scope and must not be hand-edited.
- Windows DX12 evidence does not prove Vulkan, Linux, or macOS behavior; backend and platform evidence must be named per validation run.

## Requirements

### Functional Requirements

- **FR-001**: The renderer MUST expose per-frame large-scene telemetry for primitive counts, spatial candidates, primitive records touched, visibility-tested primitives, visible primitives, visible meshes, culled counts by reason, LOD/HLOD choices, occlusion decisions, streaming residency, memory residency, cached command rebuilds, grouped draws, and frame-stage timings.
- **FR-002**: Telemetry MUST be renderer-owned and snapshot-based so editor panels can display it without rescanning the live scene.
- **FR-003**: `RenderScene` MUST keep persistent primitive records in a slot-map/free-list/tombstone model with stable handles and generation checks rather than relying only on raw component pointers or dense vector indices during async visibility work.
- **FR-004**: Static and dynamic primitive bounds MUST be represented separately so static spatial data can be refit or rebuilt less frequently than dynamic data.
- **FR-004a**: Large-scene `RenderScene::Synchronize` MUST expose the current live-scene source sweep through telemetry and MUST maintain downstream spatial-index, visibility, and queue-finalization data from dirty handles or sparse visible handles. Fully O(changed) source-scene synchronization requires dirty-list, event-driven, or equivalent source change data and remains distinct from the reported full-sweep fallback.
- **FR-005**: The visibility pipeline MUST avoid full primitive scans in large-scene mode by deriving the initial candidate set from spatial, layer, distance, and active-state index data or from another maintained O(changed) cache with equivalent results.
- **FR-005c**: Per-candidate activity, layer, and distance validation MUST still run after candidate generation so stale index metadata cannot make an invalid primitive visible.
- **FR-005a**: Layer filtering MUST have an explicit source of truth by combining `GameObject::GetLayer()` with a view-level `LayerMask` supplied by `Camera`/`CameraComponent`, editor view overrides, or a documented all-layers default.
- **FR-005b**: Distance filtering MUST use primitive visibility settings with named min/max draw distances or a documented disabled-distance default; distance thresholds MUST NOT be hidden as magic constants inside visibility loops.
- **FR-006**: Visibility output MUST remain bitset-backed or equivalently compact and must be suitable for serial and parallel range evaluation.
- **FR-007**: Serial and parallel visibility MUST produce identical visible primitive, visible mesh, and cull-reason outputs for the same frame snapshot.
- **FR-008**: Spatial index queries MUST avoid scanning every registered primitive for localized views in partitioned scenes.
- **FR-009**: Static spatial index updates MUST support dirty refit for bounds changes and bounded rebuild when topology changes exceed a configured threshold.
- **FR-009a**: Static spatial index rebuilds MUST be bounded by rebuild budgets, last-good index reuse, dirty overlays, async/double-buffer rebuilds, or an equivalent strategy that avoids unreported O(N) rebuild spikes.
- **FR-010**: Dynamic spatial index updates MUST support per-frame movement without rebuilding static data and localized dynamic queries MUST report bounded dynamic candidate and touched counts.
- **FR-011**: Per-object LOD selection MUST use a documented screen-relative metric, camera parameters, LOD bias, hysteresis, and optional fade state.
- **FR-012**: HLOD cluster selection MUST be view-local and MUST suppress child primitive submission only for views where the proxy is active.
- **FR-013**: HLOD cluster data MUST be buildable from imported model hierarchies and authored scene groups without requiring runtime-only heuristics.
- **FR-014**: HLOD proxy resources MUST have residency state so selecting a proxy never blocks on synchronous load.
- **FR-015**: Occlusion culling MUST be conservative when history, HZB resources, or backend support are unavailable.
- **FR-016**: HZB occlusion MUST avoid CPU readback stalls by using prior-frame or asynchronously produced visibility information.
- **FR-017**: HZB build and occlusion passes MUST declare explicit resource access, subresource ranges, exported transitions, and producer-consumer queue dependencies for DX12 and any future backend.
- **FR-018**: Occlusion decisions MUST invalidate on camera cut, projection change, projection jitter incompatibility, viewport-size or origin change, render-scale change, near/far/depth convention change, depth-resource identity change, depth-format or sample-count change, primitive bounds/transform generation change, representation change, material depth-write eligibility change, and backend reset.
- **FR-019**: Streaming requests MUST be generated from visibility, LOD, HLOD, and editor-interest results without blocking the rendering frame.
- **FR-020**: Streaming commit and eviction MUST be controlled by per-frame CPU, IO, GPU upload, CPU memory, and GPU memory budgets.
- **FR-021**: Streaming MUST preserve resources referenced by in-flight frames and prepared render packages until retirement.
- **FR-022**: Draw command cache invalidation MUST continue to depend on mesh/material/render-state inputs, not transform-only changes.
- **FR-023**: Queue finalization MUST preserve transparent back-to-front ordering and must not merge incompatible transparent or order-dependent draws.
- **FR-024**: The visibility pipeline MUST output masks, sparse visible primitive handles, sparse eligible command handles, representation decisions, cull reasons, and command eligibility; `RenderScene` MUST remain the single owner of visible queue finalization, opaque sort/merge, dynamic instancing, and object-index allocation until a later spec explicitly migrates that ownership.
- **FR-024a**: Large-scene queue finalization MUST consume sparse visibility/command lists or cached command-offset tables so it does not rescan all registered primitives after visibility has already produced a bounded candidate set.
- **FR-025**: Dynamic instancing and future dense-instance clusters MUST consume the same visibility and object-index allocation semantics.
- **FR-026**: Editor debug views MUST show cull reasons and residency decisions from snapshots and MUST not force Scene View render-thread drains during ordinary navigation.
- **FR-027**: The implementation MUST include targeted automated tests for telemetry, spatial index updates, visibility equivalence, LOD/HLOD decisions, occlusion fallback, streaming budgets, and draw-cache integration.
- **FR-028**: Runtime rendering claims MUST be supported by targeted runtime evidence or RenderDoc/RHI validation when GPU pass ordering, barriers, or visual correctness are involved.
- **FR-029**: HZB and occlusion feature gates MUST be derived from `RHIDevice::GetCapabilities()` / `RHIDeviceFeature` and texture-format capabilities, not backend-name checks or a parallel capability source.
- **FR-030**: HZB compute and graphics paths MUST support explicit texture visibility transitions for depth SRV and HZB UAV/SRV, plus explicit buffer resource access for HZB primitive result resources, including narrow subresource ranges where textures are used. The current HZB shader path declares mip0-only HZB ranges until a real mip-chain build lands.
- **FR-031**: Occlusion readback or history update paths MUST prove ordinary render frames do not call synchronous readback fallback such as `ReadPixelsChecked`, wait for `BeginReadPixels` completion, wait on GPU fences, block on readback-buffer mapping, or otherwise synchronize CPU/GPU for current-frame culling.
- **FR-032**: The plan MUST not claim cross-backend correctness until each backend has direct evidence.
- **FR-033**: Large-scene settings MUST be configuration-driven with named constants and debug labels, not hard-coded magic thresholds in hot paths.
- **FR-034**: The large-scene systems MUST degrade gracefully to the existing retained render-scene path when a feature flag is disabled or a required backend capability is absent.
- **FR-035**: Scene streaming MUST model representation-to-resource dependency closure, request deduplication, residency tickets, priority aging, cancellation, and eviction safety rather than treating cells as a flat list of artifacts.
- **FR-036**: The implementation review path MUST promote the large-scene benchmark draft into the plan-review benchmark registry before claiming industry-best-practice sign-off for implementation.

### Key Entities

- **ScenePrimitiveRecord**: Retained render-side record for a mesh renderer, bounds, resources, visibility flags, and generation.
- **ScenePrimitiveSnapshot**: Immutable frame-local view of retained primitive records, dense iteration metadata, handle mappings, dirty-sync lists, and command-offset tables used by parallel visibility and queue finalization.
- **SceneSpatialIndex**: Static and dynamic bounds acceleration structure used to produce view candidate ranges.
- **VisibilityFrameInput**: Immutable per-view data for camera, frustum, layer masks, LOD bias, occlusion history, streaming interest, and debug flags.
- **VisibilityFrameResult**: Per-view snapshot of visible bits, LOD/HLOD decisions, cull reasons, and command eligibility consumed by `RenderScene` queue finalization.
- **LODGroupRecord**: Per-object LOD thresholds, fade state, and member primitive handles.
- **HLODClusterRecord**: Cluster bounds, child primitive handles, proxy primitive/resource references, thresholds, and residency state.
- **OcclusionHistory**: Per-view and per-primitive conservative history used by HZB occlusion without synchronous readback.
- **StreamingCell**: Scene or asset residency unit with bounds, dependencies, request priority, and budgeted state.
- **StreamingResourceDependency**: Dependency edge from a selected representation or cell to mesh, texture, material, HLOD proxy, and placeholder resources.
- **ResidencyTicket**: Deduplicated request token used to prioritize, cancel, coalesce, commit, and safely evict streaming resources under budgets.
- **LargeSceneTelemetry**: Renderer-owned counters and timings surfaced to FrameInfo and validation logs.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Before claiming performance or industry benchmark sign-off, a large-scene baseline report documents current primitive count, visible count, draw count, render-scene sync time, sync touched count, visibility time, finalization time, finalization touched command count, draw submission time, streaming/resource stalls, backend, build configuration, hardware, scene fixture, camera path, feature gates, fallback reasons, and comparable before/after per-stage deltas. Until that report is populated with concrete numbers, this branch may claim only functional, contract, RenderDoc/RHI, and telemetry-surface validation.
- **SC-002**: A partitioned 100,000 primitive synthetic scene reports candidate counts below 30% of registered primitives and visibility-tested primitive counts below 35% of registered primitives for localized views, while sync touched counts, finalization touched counts, and serial/parallel visibility results remain bounded and match exactly.
- **SC-003**: A stable second frame in the same scene reports zero cached command rebuilds for unchanged primitives, explicitly reports any live-scene source sync sweep fallback, performs no static spatial-index rebuild unless topology changed, and performs no queue-finalization full primitive scan.
- **SC-003a**: A dynamic-heavy scene with localized moving primitives reports bounded dynamic candidate, dynamic records touched, and finalization touched counts rather than scanning all dynamic primitives.
- **SC-004**: A LOD/HLOD validation scene shows expected per-view LOD masks, child suppression, and proxy fallback behavior with no missing selected child in editor inspection mode.
- **SC-005**: An occlusion validation scene proves missing or invalid occlusion history is conservative, and a valid history path reduces submitted hidden primitives without CPU readback waits.
- **SC-006**: A streaming validation path crosses scene-cell boundaries and records bounded request, commit, eviction, resident CPU bytes, and resident GPU bytes per frame with no main-thread synchronous resource load from visibility.
- **SC-007**: A 1,000 compatible opaque object stress scene retains the draw-call optimization expectation from `specs/038-optimize-draw-calls`: at least 90% fewer submitted scene draws than one draw per object.
- **SC-008**: A comparable editor Scene View run remains interactive under ordinary navigation, and any remaining stalls are attributed to named large-scene telemetry fields rather than generic panel or render scopes.
- **SC-009**: DX12 HZB or GPU occlusion validation includes RenderDoc or RHI-event evidence for pass order, read/write resources, mip0 HZB transitions, and barriers before any GPU occlusion correctness claim is made.
- **SC-010**: No implementation phase hand-edits generated files or claims unvalidated backend/platform correctness.

## Assumptions

- The design package is complete, but implementation should land in verified phases because the affected systems span `Runtime/Engine/Rendering`, `Runtime/Rendering/RHI`, editor panels, asset residency, and tests.
- Existing `RenderScene` retained primitive state, bitset visibility, parallel visibility, dynamic instancing, object-index assignment, and draw-call telemetry are foundations to extend, not systems to replace.
- DX12 on Windows is the first runtime validation target because the current active renderer and recent draw-call work already validate DX12-specific paths.
- Vulkan, Linux, and macOS remain required future validation targets, but this design does not claim them from Windows-only evidence.
- Unity Umbra data is used as a reference for the value of precomputed occlusion, not as a dependency for Nullus. Nullus should start with HZB/history because it fits the current RHI and FrameGraph architecture.
