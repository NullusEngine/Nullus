# Feature Specification: Editor Static View Cache

**Feature Branch**: `editor-static-view-cache`
**Created**: 2026-06-09
**Status**: Implemented
**Input**: User requested large-scene optimization items 2 and 4: static Scene View frame cache / dirty redraw, and reduced unnecessary RHI / FrameGraph submissions.

## User Scenarios & Testing

### User Story 1 - Static Scene View Reuses Last Frame (Priority: P1)

When the editor Scene View camera, scene content, and editor overlays are unchanged, the view should keep presenting the last rendered texture instead of submitting another full renderer frame.

**Why this priority**: Trace evidence showed static editor frames still spend significant time in `AView::RenderView`, `AView::RendererBeginFrame`, `AView::DrawFrame`, and threaded RHI submission. Skipping the full render path on unchanged frames directly reduces CPU and RHI pressure.

**Independent Test**: Unit tests verify an unchanged view renders once, then skips additional `BeginFrame`, `DrawFrame`, and `EndFrame` calls.

**Acceptance Scenarios**:

1. **Given** a Scene View has already rendered a valid frame, **When** the next editor UI frame has the same camera, size, scene revision, and overlay state, **Then** the renderer frame is skipped and the existing view texture remains the presentation source.
2. **Given** static caching is enabled, **When** no explicit RHI texture is available because the test/no-op backend is active, **Then** the cache policy can still be tested without requiring a texture view.

### User Story 2 - Dirty Inputs Force Redraw (Priority: P1)

Any change that can affect rendered pixels or editor interaction feedback must invalidate the static cache before presentation.

**Why this priority**: A static cache that misses dirty inputs can freeze the Scene View, hide gizmo/selection/drag preview updates, or reuse stale picking/readback frames.

**Independent Test**: Unit tests verify camera movement, scene render-content revision changes, and resize events each force a new renderer frame.

**Acceptance Scenarios**:

1. **Given** a cached view frame, **When** the camera changes, **Then** the next frame renders normally.
2. **Given** a cached view frame, **When** scene render content is marked changed, **Then** the next frame renders normally.
3. **Given** a cached view frame, **When** the view size changes, **Then** the cache is invalidated and the next frame renders normally.

### User Story 3 - Reduce Unnecessary RHI/FrameGraph Work (Priority: P2)

The optimization should skip the whole renderer submission path on cache hits, not just avoid drawing some passes after the frame is already started.

**Why this priority**: Item 4 targets RHI and FrameGraph overhead. Skipping before `EnsureRenderer`, `InitFrame`, `BeginFrame`, `DrawFrame`, and `EndFrame` avoids descriptor setup, frame graph building, and threaded RHI publication for unchanged Scene View frames.

**Independent Test**: The static cache test counts renderer begin/draw/end calls and proves they do not advance on cache hits.

**Acceptance Scenarios**:

1. **Given** the static frame cache hits, **When** `AView::Render` runs, **Then** no renderer begin/draw/end calls are made.
2. **Given** the view needs immediate readback, picking, drag preview, camera motion, or validation output, **When** `AView::Render` runs, **Then** the cache is bypassed.
3. **Given** a trace shows no static-cache hits, **When** `AView::Render` evaluates the cache decision, **Then** the trace identifies the exact miss reason instead of requiring source-level guessing.

### Edge Cases

- Immediate readback consumers must not use a stale cached frame.
- Validation readback must continue rendering until the readback is written.
- Drag-preview placement and readiness changes must force redraw.
- Selection, highlight, gizmo operation, gizmo pivot/space, and Scene View camera focus state must participate in the Scene View cache key.
- Hover picking may use an existing readable picking sample without forcing a full Scene View redraw; click picking and first-sample picking remain conservative.
- No explicit RHI backend must still allow deterministic unit coverage.

## Requirements

### Functional Requirements

- **FR-001**: `AView` MUST expose a protected static-frame cache policy hook that can be enabled by derived views.
- **FR-002**: `AView` MUST skip renderer submission before `EnsureRenderer` and `InitFrame` when the cache key matches the last rendered frame and no force-render condition is active.
- **FR-003**: `AView` MUST invalidate the cache when the resolved view size changes.
- **FR-004**: `AView` MUST include camera projection settings, camera transform, clear flags, visible layer mask, scene render-content revision, and view size in the base cache key.
- **FR-005**: `SceneView` MUST enable static-frame caching.
- **FR-006**: `SceneView` MUST force redraw for camera motion, active camera control, pending click picking, hover/click picking requests, imported asset drag preview, and validation readback.
- **FR-007**: `SceneView` MUST include selection, highlight, gizmo state, focus state, prefab-stage state, and drag-preview state in its cache key.
- **FR-008**: `SceneManager::MarkCurrentSceneDirty()` MUST advance scene render-content revision every call, even when the scene is already marked dirty.
- **FR-009**: Cache hits MUST preserve the latest successful `FrameInfo` snapshot rather than inventing a new frame.
- **FR-010**: `AView` MUST publish profiler scopes for static cache hit/miss reasons, including disabled, invalid, key changed, resize, immediate readback, forced render, and missing texture cases.
- **FR-011**: `SceneView` MUST NOT force a full static-frame redraw solely for hover picking when a readable picking sample already exists.

### Key Entities

- **Static Frame Cache Key**: A hash of view size, camera state, scene render-content revision, and Scene View editor overlay state.
- **Scene Render Content Revision**: A monotonic scene-level revision used by editor rendering to detect visual scene changes independently from saved/unsaved dirty state.
- **Force Render Condition**: A conservative condition that bypasses the cache when interaction, readback, validation, or transient preview state needs a fresh render.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Unit tests prove unchanged cached frames do not call renderer begin/draw/end more than once.
- **SC-002**: Unit tests prove camera movement, scene render-content revision changes, and resize invalidate the cache.
- **SC-003**: Unit tests prove repeated scene dirty marks advance render-content revision every time.
- **SC-004**: Static Scene View trace captures should show fewer `AView::RendererBeginFrame`, `AView::DrawFrame`, `AView::RendererEndFrame`, `CompositeRenderer::DrawRegisteredPasses`, and `RhiThreadCoordinator::SubmitThreadedRhiFrame` samples during idle/static editor frames.
- **SC-005**: Trace captures should contain `AView::StaticCacheHit` or `AView::StaticCacheMiss::*` scopes for every eligible view render decision, allowing zero-hit captures to be diagnosed by reason.

## Assumptions

- Most editor scene mutations already call `SceneManager::MarkCurrentSceneDirty()`; this change makes repeated dirty calls usable as a render invalidation source.
- Some runtime-side transform/material mutations may not yet call the editor dirty path; those remain future work unless they are editor interactions covered by existing editor action paths.
- Game View and Asset View are not enabled for static caching in this slice because they can contain dynamic content or preview-resource churn that needs separate policy.
