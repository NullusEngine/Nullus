# Data Model: Render Feature Refactor

## Overview

This feature does not introduce persistent project data. Its data model describes runtime ownership, migration states, and validation-visible rendering data.

## Entities

### 1. Renderer Core Binding State

**Purpose**: Own required frame-level and object-level draw data for normal scene rendering.

**Fields**:

- `frameConstantsAvailable` (bool): Whether current frame-level data is prepared for the active frame.
- `objectConstantsAvailable` (bool): Whether current object-level data is prepared for the active draw.
- `frameBindingOwner` (enum: `RendererCore`, `CompatibilityFeature`): Current owner during migration.
- `objectBindingOwner` (enum: `RendererCore`, `CompatibilityFeature`): Current owner during migration.
- `bindingOrder` (ordered list): The required draw-time order of core bindings before material bindings.
- `activeDrawCount` (integer): Number of draws that consumed renderer-owned frame/object state in the frame.

**Validation rules**:

- `bindingOrder` must place core frame/object preparation before material binding.
- `frameConstantsAvailable` must be true before any scene draw can use frame-dependent shader data.
- `objectConstantsAvailable` must be refreshed for each applicable draw that needs object-dependent shader data.

**Relationships**:

- Used by `CompositeRenderer` / `ABaseRenderer` draw preparation.
- Supplies required data to render passes and material submission.

**State transitions**:

- `Uninitialized` -> `FramePrepared` when frame begin populates frame-level state.
- `FramePrepared` -> `DrawPrepared` when object-level data is prepared for a draw.
- `DrawPrepared` -> `FramePrepared` after draw submission completes.
- `FramePrepared` -> `Uninitialized` at frame end or renderer reset.

### 2. Debug Drawing Capability

**Purpose**: Preserve submission semantics for transient diagnostic primitives while moving rendering into an explicit stage.

**Fields**:

- `queueEnabled` (bool): Global toggle for debug drawing.
- `categoryVisibility` (map): Per-category visibility state.
- `primitiveType` (enum): Point, line, triangle, or high-level helper shape expanded into these primitive forms.
- `style` (object): Color, line width, point size, depth behavior, and fill behavior requested by the submitter.
- `submitterOwnsPipelineState` (bool): Must be false in the final model; debug draw callers describe geometry and style only.
- `visiblePrimitiveCount` (integer): Visible primitives for the current frame after filtering.
- `drawableLimitState` (enum: `WithinLimits`, `AtLimit`, `OverflowRejected`): Frame-limit enforcement result.
- `renderOwner` (enum: `FeatureHook`, `ExplicitDebugStage`): Current owner during migration.

**Validation rules**:

- Disabled categories must hide only matching primitives.
- A disabled global debug draw toggle must hide all primitives without clearing retained entries.
- Lifetime rules must preserve one-frame, timed, and persistent behaviors.
- Queue limit enforcement must not corrupt existing retained entries.
- Frustum, light-volume, and bounding-volume helpers must submit through the same queue as direct point, line, and triangle primitives.
- Callers must not be required to construct `PipelineState`, materials, meshes, or pass-local resources.

**Relationships**:

- Consumes `DebugPrimitive`, `DebugDrawable`, style, category, and lifetime data from existing debug draw types.
- Feeds the explicit debug drawing stage or temporary compatibility owner.
- Used by editor selection helpers for camera frustums, light volumes, and mesh bounds.

**State transitions**:

- `Idle` -> `Collecting` when submissions occur during a frame.
- `Collecting` -> `Filtered` when visibility and limits are applied.
- `Filtered` -> `Rendered` when the debug drawing stage executes.
- `Rendered` -> `Retained` or `Expired` depending on lifetime rules.

### 3. Lighting Data Source

**Purpose**: Provide scene lighting information to renderer passes without draw hook ownership.

**Fields**:

- `lightsPresent` (bool): Whether any supported light exists in the scene.
- `supportedLightCount` (integer): Number of lights relevant to renderer consumers.
- `collectionOwner` (enum: `SceneProvider`, `CompatibilityFeature`): Owner during migration.
- `consumerModes` (set): Renderer modes currently consuming the data source, such as forward and deferred.
- `frustumFiltered` (bool): Whether the current collection applies frustum or scope filtering.

**Validation rules**:

- Forward and deferred consumers must read from the same authoritative lighting source.
- Scenes with no lights must still produce valid renderer behavior.
- Unsupported or out-of-scope light types must not corrupt consumer behavior.

**Relationships**:

- Sourced from scene light components/entities.
- Consumed by forward and deferred renderer paths.

**State transitions**:

- `Empty` -> `Collected` when scene lighting is gathered for a frame.
- `Collected` -> `Consumed` when renderer passes read the frame lighting data.
- `Consumed` -> `Empty` at frame end or scene reload.

### 4. Renderer Statistics

**Purpose**: Represent renderer-owned frame diagnostics derived from submitted draw work.

**Fields**:

- `batchCount` (integer)
- `instanceCount` (integer)
- `polyCount` (integer)
- `vertexCount` (integer)
- `validityState` (enum: `Invalid`, `Collecting`, `Finalized`)
- `statsOwner` (enum: `RendererCore`, `CompatibilityFeature`)

**Validation rules**:

- Counts must reset at frame begin.
- Counts must only increase in response to valid submitted draw work.
- Query results are only valid in `Finalized` state unless a consumer explicitly requests live values.

**Relationships**:

- Updated by renderer draw submission.
- Read by editor panels, diagnostics, and frame reporting.

**State transitions**:

- `Invalid` -> `Collecting` at frame begin.
- `Collecting` -> `Finalized` at frame end.
- `Finalized` -> `Invalid` when a new frame begins or the renderer resets.

### 5. Renderer Extension Compatibility State

**Purpose**: Track whether `ARenderFeature` is still required for compatibility or has been narrowed/removed.

**Fields**:

- `mode` (enum: `Compatibility`, `LifecycleOnly`, `Removed`)
- `coreResponsibilitiesRemaining` (set): Remaining responsibilities still attached to `ARenderFeature`.
- `allowedResponsibilities` (set): Responsibilities allowed to remain if lifecycle-only mode is retained.
- `migrationBlockers` (list): Known consumers that still require compatibility behavior.

**Validation rules**:

- `Compatibility` mode is only valid while `coreResponsibilitiesRemaining` is non-empty.
- `LifecycleOnly` mode requires no remaining core draw preparation responsibilities.
- `Removed` mode requires all compatibility consumers to be migrated.

**Relationships**:

- Governs whether renderer extension hooks are still present.
- Depends on completion of the binding, debug draw, lighting, and stats migrations.

**State transitions**:

- `Compatibility` -> `LifecycleOnly` when all core responsibilities are migrated.
- `LifecycleOnly` -> `Removed` when no compatibility consumers remain.

## Entity Relationships Summary

- **Renderer Core Binding State** is required by normal scene draws and must exist independently of optional extensions.
- **Debug Drawing Capability** uses existing debug draw data types and is consumed by an explicit render stage.
- **Lighting Data Source** is collected from the scene and shared by forward/deferred renderer consumers.
- **Renderer Statistics** are updated by renderer draw submission and exposed to diagnostics.
- **Renderer Extension Compatibility State** governs the transition path for `ARenderFeature`.
