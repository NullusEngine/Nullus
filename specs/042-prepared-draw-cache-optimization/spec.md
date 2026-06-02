# Feature Specification: Prepared Draw Cache Optimization

**Feature Branch**: `[042-prepared-draw-cache-optimization]`  
**Created**: 2026-05-31  
**Status**: Draft  
**Input**: User asked to optimize low FPS when draw call count is high, using `App/Win64_Release_Runtime_Shared/trace.json` as evidence.

## User Scenarios & Testing

### User Story 1 - Reduce Repeated Deferred Draw Preparation (Priority: P1)

When a scene contains many visible opaque objects, the deferred renderer should avoid repeating expensive per-draw preparation work that is identical across stable draws in the same frame or across frames.

**Why this priority**: The provided trace shows `DeferredSceneRenderer::BeginFrame` and repeated `CaptureThreadedPreparedDraw` / `GetOrCreateGBufferMaterial` dominating slow Scene View frames before RHI command recording.

**Independent Test**: Unit tests can repeatedly resolve the same source material and capture repeated prepared draws in one frame, then verify cache hits without changing per-object binding behavior.

**Acceptance Scenarios**:

1. **Given** repeated opaque draws using the same source material in one frame, **When** the renderer resolves GBuffer materials, **Then** only the first resolve performs persistent cache lookup/sync and later resolves reuse the frame-local result.
2. **Given** repeated prepared draws using the same material, mesh, pass binding, pipeline state, and render-state inputs, **When** threaded prepared draws are captured in the same or later frames, **Then** the renderer reuses cached static draw state while still capturing per-object binding sets per draw.
3. **Given** the source material changes parameters, render state, shader generation, or material binding state, **When** the renderer resolves or captures again, **Then** stale cached state is invalidated.

### User Story 2 - Preserve Existing Rendering Correctness (Priority: P1)

Existing deferred GBuffer material cache behavior, material binding state, mesh state, shader generation changes, and object-data binding behavior must remain correct.

**Why this priority**: The optimization sits on hot render preparation paths. Incorrect reuse can silently bind stale textures, stale samplers, stale pipelines, or stale object data.

**Independent Test**: Existing `DeferredSceneRendererMaterialCacheTests`, object binding tests, and renderer stats tests must continue to pass, with new tests covering cache invalidation.

**Acceptance Scenarios**:

1. **Given** two distinct runtime source material instances with different texture bindings, **When** they resolve GBuffer materials, **Then** they still resolve to distinct cached GBuffer materials.
2. **Given** a material shader generation or sampler override changes during a frame, **When** prepared draw capture runs again, **Then** the cached static base misses and refreshes.
3. **Given** two draws differ only by object transform/object index, **When** prepared draw capture runs, **Then** the static base may be reused but object binding sets remain distinct.

### User Story 3 - Make Preparation Wins Observable (Priority: P2)

Renderer diagnostics should expose cache hit/miss counters so future traces and UI checks can distinguish CPU preparation caching wins from RHI command recording work.

**Independent Test**: Renderer stats and FrameInfo panel tests verify the new counters are recorded and formatted.

### User Story 4 - Keep Build Ownership Aligned With Module Boundaries (Priority: P2)

Build files should discover module-local sources without hardcoded cross-module implementation lists.

**Independent Test**: The normal `NullusUnitTests` build target links editor/game/launcher project code through their project static libraries and links rendering-owned resource-manager implementations through `NLS_Render`.

### User Story 5 - Make Trace Evidence Reliable And Isolate Editor UI Cost (Priority: P1)

Trace export must keep profiling event names stable and allow editor UI/profiler panel overhead to be excluded when analyzing scene draw-call cost.

**Why this priority**: The provided trace contains corrupted names such as `rawData`, `l`, and `nel`, and slow frames are heavily affected by `Editor::RenderEditorUI`, `Canvas::DrawPanels`, and `DX12UIBridge::RenderDrawData`.

**Independent Test**: Profiler unit tests verify `ProfilerEvent` owns names until export and trace export can filter editor UI events without filtering scene draw preparation scopes.

### User Story 6 - Preserve Dynamic Instancing At Trace Scale (Priority: P1)

Compatible static opaque objects with the same mesh/material/pipeline should be merged into instance groups before prepared draw capture, including the 259-draw scale observed in the trace.

**Independent Test**: Render scene cache tests verify 259 compatible static opaque objects produce one submitted draw and one dynamic instance group.

## Requirements

### Functional Requirements

- **FR-001**: The deferred renderer MUST cache frame-local GBuffer material resolve results by source material identity and freshness stamp.
- **FR-002**: The frame-local GBuffer resolve cache MUST be cleared at the start of each deferred frame.
- **FR-003**: A frame-local GBuffer cache hit MUST return the same GBuffer material pointer without incrementing persistent sync counters.
- **FR-004**: Source material parameter, render-state, shader instance, or shader generation changes MUST invalidate the frame-local GBuffer resolve entry.
- **FR-005**: Existing persistent GBuffer material cache key behavior MUST remain correct for runtime texture isolation.
- **FR-006**: Threaded prepared draw capture MUST persistently cache only static base state: graphics pipeline, material binding set, pass binding set, RHI mesh, and fallback GPU instance count.
- **FR-007**: Prepared draw capture MUST continue capturing per-draw object binding state and per-draw submit fields such as instance count, vertex start, and vertex count.
- **FR-008**: Prepared static-base cache freshness MUST include material instance, parameter revision, render-state revision, binding revision, shader instance id, shader generation, mesh instance id/content revision, pass binding set, primitive mode, depth compare, pipeline state, and pipeline overrides.
- **FR-009**: `EngineFrameObjectBindingProvider` SHOULD cache indexed-object-data shader support queries per frame by shader instance id and generation.
- **FR-010**: Renderer stats and FrameInfo UI MUST expose GBuffer resolve and prepared static-base hit/miss counters.
- **FR-011**: Tests MUST cover frame-local hit, revision invalidation, shader-generation invalidation, binding-revision invalidation, per-object binding isolation, and stats/UI reporting.
- **FR-012**: CMake source ownership MUST avoid hardcoded cross-module source lists for project/test targets and rendering-owned resource-manager implementations.
- **FR-013**: Timeline profiler events MUST own their event names until trace export and UI rendering, rather than storing pointers into recycled allocator pages.
- **FR-014**: Trace export SHOULD provide an editor UI filtering mode that excludes editor/UI/profiler panel scopes while preserving scene draw preparation scopes.
- **FR-015**: Dynamic instancing MUST reduce compatible opaque static objects at the observed trace scale of 259 objects to one submitted scene draw when object-data limits permit.

### Non-Goals

- **NG-001**: This slice does not claim full UE-style `FMeshDrawCommand` persistence or full draw command reuse for complete recorded command buffers.
- **NG-002**: This slice does not add a new RHI in-render-pass parallel recording architecture; DX12 bundle changes are limited to correctness guards for the existing path.
- **NG-003**: This slice does not claim final performance closure without a fresh post-change high draw-count trace.

### Key Entities

- **GBufferMaterialSyncStamp**: Identifies the source material instance, source revisions, shader instance id, and shader generation used to validate resolved GBuffer material freshness.
- **Frame GBuffer Material Resolve Cache**: Per-frame map from source material identity to the resolved GBuffer material and stamp.
- **PreparedRecordedDrawStaticBase**: Reusable persistent static draw state that excludes object binding and per-draw submit ranges.
- **PreparedRecordedDrawStaticBaseCacheKey**: Full freshness key for static prepared draw state.
- **Indexed Object Data Shader Support Cache**: Per-frame shader support cache keyed by shader instance id and generation.
- **Trace Export UI Filter**: Timeline trace export option that suppresses editor UI/profiler scopes for scene-cost analysis.
- **Dynamic Instance Group**: A merged opaque draw containing multiple object transforms and an object-data range for compatible static objects.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Unit tests verify repeated same-frame source material resolves hit a frame-local cache.
- **SC-002**: Unit tests verify prepared static-base reuse keeps object binding sets distinct.
- **SC-003**: Unit tests verify shader generation and material binding revisions invalidate cached state.
- **SC-004**: Existing deferred material cache, object binding, renderer stats, and FrameInfo panel tests continue to pass.
- **SC-005**: A new post-change trace should show fewer or shorter CPU preparation samples for repeated material/static draw work compared with the baseline trace.
- **SC-006**: Build verification proves moved resource-manager implementations are linked through the rendering target and project executables/tests link through project libraries.
- **SC-007**: Unit tests verify trace event names are owned by `ProfilerEvent` and exported through stable names.
- **SC-008**: Unit tests verify editor UI trace filtering excludes profiler/editor UI scopes without excluding `CaptureThreadedPreparedDraw`.
- **SC-009**: Unit tests verify 259 compatible static opaque objects are reduced to one submitted draw.

## Assumptions

- The current optimization slice targets CPU render preparation before RHI command recording, because the provided trace shows that stage dominating slow frames.
- Existing RHI threaded recording remains the path for Nullus in-render-pass child command recording; this spec narrows the current work to reducing CPU preparation feeding that path and tightening DX12 bundle guards.
- A fresh runtime trace is required before making any final FPS or trace-improvement claim.
