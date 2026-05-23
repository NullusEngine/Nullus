# Feature Specification: Deferred Editor Overlays

**Feature Branch**: `024-deferred-editor-overlays`  
**Created**: 2026-05-12  
**Status**: Draft  
**Input**: User description: "DeferredSceneRenderer 增加 editor overlay/debug pass 支持"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Scene View Uses Deferred Scene Lighting (Priority: P1)

As an editor user, I want Scene View to use the same deferred scene renderer as Game View and runtime gameplay so lighting and material results match while I inspect and edit the scene.

**Why this priority**: The current Scene View uses a forward debug renderer, which can hide deferred-only behavior and make point light, deferred lighting, and material debugging inconsistent.

**Independent Test**: Create a Scene View renderer and verify its main scene execution is built from the deferred GBuffer and deferred lighting passes rather than the forward opaque/skybox/transparent pass set.

**Acceptance Scenarios**:

1. **Given** the editor creates a Scene View renderer, **When** the renderer prepares a frame, **Then** the main scene pass plan contains deferred GBuffer and deferred lighting passes.
2. **Given** the game or Game View path uses the default scene renderer, **When** Scene View renders the same scene, **Then** both views use the deferred scene lighting path for the main scene.

---

### User Story 2 - Editor Helpers Remain Visible Over Deferred Output (Priority: P1)

As an editor user, I want the grid, camera helpers, light helpers, selected actor outline, and debug draw primitives to remain available in Scene View after the deferred lighting pass.

**Why this priority**: Scene View must remain usable for editing; matching deferred rendering is not acceptable if editor-only overlays disappear.

**Independent Test**: Enable the debug renderer with a grid descriptor, lights, cameras, selected actor, and debug draw primitives; verify the prepared pass package includes the deferred main passes followed by editor overlay/debug passes.

**Acceptance Scenarios**:

1. **Given** Scene View has grid and debug helper passes enabled, **When** it renders through the deferred debug renderer, **Then** helper passes are scheduled after the deferred lighting output pass.
2. **Given** a selected actor exists, **When** Scene View prepares the frame, **Then** the selected actor outline pass remains present and draws over the lit scene output.
3. **Given** editor debug draw is enabled and visible primitives exist, **When** the frame is built, **Then** the debug draw pass contributes to the Scene View frame.

---

### User Story 3 - Picking And Readback Still Work (Priority: P2)

As an editor user, I want actor picking in Scene View to continue working after switching the main Scene View renderer to deferred.

**Why this priority**: Picking is critical for editing, but it can be validated separately after the rendering path and visible overlays are in place.

**Independent Test**: Request a picking frame and verify the prepared render scene package includes a picking pass, registers a preferred readback texture, and preserves the existing readback ownership contract.

**Acceptance Scenarios**:

1. **Given** Scene View requests picking, **When** the frame package is prepared, **Then** a picking pass is appended after the deferred scene passes.
2. **Given** a picking pass provides a color attachment, **When** the package is finalized, **Then** that attachment is registered as the preferred readback texture.

---

### Edge Cases

- If deferred renderer resources are missing or a GBuffer target cannot be allocated, Scene View should degrade with existing deferred renderer warnings rather than crashing.
- If no editor helper is visible, the deferred Scene View package should contain only the required deferred scene passes and no empty helper pass commands.
- If picking is disabled, no picking pass or preferred readback texture should be registered.
- Explicit `ForwardSceneRenderer` construction must remain available for tests and any future specialized forward-only view.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Scene View's debug renderer MUST use the deferred scene rendering path for its main scene output.
- **FR-002**: Deferred scene rendering MUST provide an extension point that allows editor overlay/debug pass inputs to be appended after deferred lighting.
- **FR-003**: Existing editor helper passes MUST remain editor-owned; runtime deferred rendering MUST NOT gain hard dependencies on editor-only pass classes.
- **FR-004**: Grid, debug cameras, debug lights, selected actor outline, debug draw, and picking passes MUST remain schedulable for Scene View.
- **FR-005**: Threaded render scene package preparation MUST preserve deferred GBuffer/lighting pass metadata and append editor helper pass metadata in deterministic order.
- **FR-006**: Picking MUST continue to register its preferred readback texture when a picking frame is requested.
- **FR-007**: The explicit forward scene renderer MUST remain constructible and testable independently of the default deferred scene renderer.

### Key Entities

- **Deferred Debug Scene Renderer**: Editor-facing renderer that combines deferred main scene rendering with editor helper and picking passes.
- **Editor Overlay Pass Input**: Prepared render pass command input for grid, helpers, outline, debug draw, or picking appended after deferred lighting.
- **Prepared Render Scene Package**: Threaded rendering artifact containing compute dispatches, deferred scene passes, editor overlay passes, and optional readback texture registration.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A regression test verifies Scene View's debug renderer is based on deferred rendering rather than forward rendering.
- **SC-002**: A renderer package test verifies deferred GBuffer and deferred lighting passes are present before editor overlay/debug passes.
- **SC-003**: Existing picking lifecycle tests continue to pass without changing user interaction semantics.
- **SC-004**: Full `NullusUnitTests` passes after the renderer migration.

## Assumptions

- Scene View should switch to deferred main rendering now that the default game renderer is deferred.
- The editor overlay/debug passes can remain in `Project/Editor` and use existing `AddPass`/prepared pass mechanisms.
- RenderDoc validation is preferred for final visual confidence, but automated renderer package tests are the first verification layer for this implementation.
- The current validated backend focus is DX12 phase 1; this feature does not claim new cross-backend support.
