# Feature Specification: Render Feature Refactor

**Feature Branch**: `005-render-feature-refactor`  
**Created**: 2026-04-18  
**Status**: Draft  
**Input**: User description: "Refactor the current ARenderFeature-based rendering extension model so it no longer acts as the core draw-time pipeline and binding assembly mechanism. Preserve current rendering behavior while migrating responsibilities into clearer systems: renderer-owned frame/object binding, explicit render passes for debug draw, lighting as a data provider, renderer-owned stats, and a path to remove or narrow ARenderFeature after responsibilities are migrated. The feature should prevent new rendering work from adding shader-variant, pass-binding, or pipeline-state responsibilities to ARenderFeature, and should support validation for editor/runtime rendering, debug draw, lighting, and stats behavior."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Preserve Frame Rendering While Removing Core Feature Dependency (Priority: P1)

As a rendering maintainer, I need frame-level and object-level rendering data to be owned by the renderer rather than by an optional render feature, so that normal scene drawing remains stable even when optional renderer extensions are absent or disabled.

**Why this priority**: Scene rendering depends on this data for every visible object. This is the highest-risk responsibility currently coupled to `ARenderFeature`.

**Independent Test**: Run a normal runtime or editor scene with renderer extensions disabled where possible; opaque, transparent, skybox, and object transforms remain correct and no draw depends on an optional feature to provide core frame/object data.

**Acceptance Scenarios**:

1. **Given** a scene with mesh objects and camera movement, **When** the frame is rendered, **Then** camera-dependent and object-dependent shader data are available from renderer-owned state.
2. **Given** optional renderer extensions are unavailable or disabled, **When** a normal scene is rendered, **Then** scene objects still render with correct transforms and camera projection.
3. **Given** a draw uses explicit rendering resources, **When** the renderer prepares the draw, **Then** core frame/object bindings are prepared before material resources are submitted.

---

### User Story 2 - Make Debug Drawing an Explicit Rendering Capability (Priority: P2)

As an editor/runtime diagnostics user, I need debug points, lines, surfaces, shapes, and grid helpers to be submit-able from any runtime or editor subsystem through a clearly owned debug drawing capability, so that diagnostic rendering does not depend on a broad draw hook mechanism or caller-owned pipeline state.

**Why this priority**: Debug draw is active in editor workflows and current debug primitive work. It must remain usable while `ARenderFeature` is narrowed or removed.

**Independent Test**: Submit points, lines, triangles, boxes, spheres, frustums, light volumes, bounding volumes, and grid axes in an editor scene; submitted primitives appear according to a unified global/category visibility model, lifetime, depth mode, style, and frame limits.

**Acceptance Scenarios**:

1. **Given** debug primitives are submitted during a frame from runtime or editor code, **When** the debug draw stage runs, **Then** visible primitives are rendered once with their requested style without the caller providing pipeline state.
2. **Given** the global debug draw toggle or a debug category is disabled, **When** debug primitives in that scope are submitted, **Then** those primitives do not appear while enabled scopes still render.
3. **Given** persistent and duration-based debug draw entries exist, **When** frames advance, **Then** entries expire or remain visible according to their lifetime rules.
4. **Given** editor-selected cameras, lights, or mesh objects need helper visualization, **When** the editor submits frustums, light volumes, or bounding volumes, **Then** those helpers use the same debug draw queue and visibility controls as manually submitted primitives.

---

### User Story 3 - Provide Lighting Data Without Render Feature Hooks (Priority: P3)

As a renderer author, I need lighting data to be available as scene data rather than as a render feature hook, so that forward and deferred rendering passes can consume the same lighting source without hidden draw-time side effects.

**Why this priority**: Lighting is shared across renderer paths, but it is less foundational than frame/object binding and debug draw migration.

**Independent Test**: Render a scene with at least one directional or spot light through supported scene renderer modes; lighting-dependent passes receive the same scene lighting information as before the refactor.

**Acceptance Scenarios**:

1. **Given** a scene contains supported lights, **When** forward rendering draws lit objects, **Then** lighting data is available from scene-owned renderer data.
2. **Given** a deferred lighting pass runs after scene collection, **When** it evaluates lighting, **Then** it consumes the same lighting data source used by forward rendering.

---

### User Story 4 - Keep Renderer Statistics Without Optional Feature Registration (Priority: P4)

As a tools user, I need frame statistics such as draw counts, instance counts, and primitive estimates to remain available without registering a draw hook feature, so that statistics represent renderer behavior consistently.

**Why this priority**: Statistics are useful for editor feedback, but they can move after core rendering behavior is protected.

**Independent Test**: Render a scene with known mesh and instance counts; statistics are updated during draw submission and remain available to editor panels or diagnostics.

**Acceptance Scenarios**:

1. **Given** a scene submits multiple drawables, **When** the frame completes, **Then** renderer statistics report the submitted draw and instance counts.
2. **Given** no drawables are visible, **When** the frame completes, **Then** renderer statistics report zero submitted draw work for the frame.

### Edge Cases

- Optional extensions are disabled while core rendering still needs frame/object data.
- Debug draw is submitted when no debug draw rendering stage is active.
- Debug draw queues exceed per-frame primitive or drawable limits.
- A scene has no lights, unsupported light types, or only non-directional lights.
- Renderer statistics are queried before the first frame, after a skipped frame, or after a renderer reset.
- Existing editor passes expect `ARenderFeature` lookup to succeed during the transition period.
- Multiple scene renderer modes share the same debug draw and lighting data in one editor session.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST keep existing scene rendering behavior intact while reducing reliance on `ARenderFeature` for core draw preparation.
- **FR-002**: The system MUST make frame-level and object-level rendering data available through renderer-owned state rather than an optional render feature.
- **FR-003**: The system MUST prepare core frame/object data for each applicable draw before material-specific resources are submitted.
- **FR-004**: The system MUST keep debug primitive submission behavior available for points, lines, triangles, boxes, spheres, cones, frustums, light volumes, bounding volumes, and polylines.
- **FR-005**: The system MUST render submitted debug primitives through an explicitly owned debug drawing path with global enable, category visibility, lifetime, style, depth mode, fill mode, and frame-limit behavior preserved.
- **FR-006**: The system MUST expose lighting information as scene or renderer data that can be consumed by forward and deferred rendering without draw-time feature hooks.
- **FR-007**: The system MUST keep renderer frame statistics available without requiring an optional render feature to be registered.
- **FR-008**: The system MUST define a transition rule that prevents new rendering work from adding shader-variant, pass-binding, or pipeline-state responsibilities to `ARenderFeature`.
- **FR-009**: The system MUST either remove `ARenderFeature` after all responsibilities are migrated or narrow it to lifecycle-only extension behavior.
- **FR-010**: The system MUST keep existing editor workflows that rely on grid, debug model, debug shape, lighting, and scene rendering behavior operational during the migration.
- **FR-011**: The system MUST provide validation evidence for scene rendering, debug draw behavior, lighting data availability, and renderer statistics after each migration slice.
- **FR-012**: The system MUST document the new ownership boundaries so renderer, pass, debug, lighting, stats, and shader-variant responsibilities are not confused.
- **FR-013**: The system MUST allow debug primitive callers to submit diagnostic geometry without constructing render pipeline state, materials, meshes, or renderer-pass-specific resources.

### Key Entities

- **Renderer Core State**: Renderer-owned frame data, object data, and per-frame draw orchestration required for normal scene rendering.
- **Debug Drawing Capability**: The submission queue, global/category visibility rules, lifetime rules, primitive style, depth/fill behavior, high-level shape helpers, and rendering stage for transient diagnostic primitives.
- **Lighting Data Source**: Scene-derived lighting information consumed by renderer passes without hidden feature hooks.
- **Renderer Statistics**: Per-frame diagnostic counters derived from submitted draw work.
- **Renderer Extension**: Optional lifecycle-only behavior that may remain after core draw preparation responsibilities are migrated away from `ARenderFeature`.
- **Migration Boundary**: Rules that identify which responsibilities are allowed to remain in optional extensions and which must belong to renderer core, render passes, or data providers.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of normal scene draws in the targeted renderer paths receive frame/object rendering data without requiring an `ARenderFeature` draw hook.
- **SC-002**: Existing debug draw unit coverage continues to pass, and manual/editor verification confirms submitted debug primitives render with preserved category and lifetime behavior.
- **SC-003**: Forward and deferred scene renderer smoke validation shows lighting data remains available in scenes with at least one supported light and in scenes with no lights.
- **SC-004**: Renderer statistics report correct zero/non-zero draw work across at least one empty scene and one populated scene.
- **SC-005**: No remaining core draw path requires `ARenderFeature` to mutate pipeline state or bind required frame/object/pass resources.
- **SC-006**: The migration documentation identifies the owner for each former `ARenderFeature` responsibility and has no unresolved ownership gaps.

## Assumptions

- The refactor is scoped to rendering framework ownership boundaries, not to a full shader variant or pipeline cache implementation.
- Existing supported runtime backends and editor workflows must keep their current behavior unless a separate spec changes backend support.
- Debug draw behavior from the current debug draw primitive work remains in scope and should be preserved.
- The migration may be incremental, but each completed slice must leave the renderer in a runnable and testable state.
- `ARenderFeature` may remain temporarily as a compatibility layer while its responsibilities are moved.
- Generated files under `Runtime/*/Gen/` remain out of scope for hand edits.
