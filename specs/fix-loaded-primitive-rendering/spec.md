# Feature Specification: Loaded Primitive Rendering

**Feature Branch**: `[fix-loaded-primitive-rendering]`  
**Created**: 2026-05-27  
**Status**: Draft  
**Input**: User description: "场景中保存的cube没有渲染出来"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Saved Cube Stays Visible (Priority: P1)

An editor user saves a scene containing a primitive cube and reopens that scene later. The cube should render in the scene/game render path without requiring the user to recreate it or manually assign a material again.

**Why this priority**: A saved scene that drops visible primitive objects breaks the core edit-save-load workflow.

**Independent Test**: Load or instantiate a scene graph containing a primitive cube with a mesh renderer and no explicit material, then verify the render scene produces one visible drawable for that cube.

**Acceptance Scenarios**:

1. **Given** a scene graph contains a primitive cube mesh reference and a mesh renderer with no explicit material entries, **When** the scene is restored and synchronized for rendering, **Then** the cube contributes a visible draw using the scene fallback material.
2. **Given** a saved primitive cube references `builtin:Primitive/Cube`, **When** the scene is restored from a cold resource cache, **Then** the primitive mesh resolves through the existing resource manager path and does not remain permanently missing.

### Edge Cases

- The default material is not already registered in the material manager when the scene is first synchronized.
- The primitive mesh alias is present in the scene file but the mesh resource has not been loaded yet.
- A mesh renderer has real material references; those references must still take precedence over fallback material behavior.
- Missing non-built-in mesh paths should continue reporting unresolved resource state instead of being silently treated as primitives.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Saved primitive cube scene objects MUST remain renderable after scene load when their mesh reference points to `builtin:Primitive/Cube`.
- **FR-002**: Mesh renderers with no explicit material entries MUST still render with the scene fallback material when a valid mesh is available.
- **FR-003**: Explicit material references MUST continue to override fallback material selection when those references resolve to valid materials.
- **FR-004**: Cold built-in primitive mesh references MUST resolve through the existing mesh resource manager without requiring manual editor interaction.
- **FR-005**: Missing non-built-in mesh or material references MUST preserve existing retry and diagnostic behavior.

### Key Entities

- **Saved Primitive Cube**: A scene object with transform, mesh filter, and mesh renderer data persisted in the object graph.
- **Scene Fallback Material**: The material used when a mesh renderer has no valid explicit material for the mesh slot.
- **Built-in Primitive Mesh Reference**: A persistent object reference whose resource path is `builtin:Primitive/<Name>`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A restored scene containing one saved primitive cube produces at least one visible scene drawable on the first render synchronization after resources are available.
- **SC-002**: The saved cube workflow is covered by an automated regression test that fails before the fix and passes after it.
- **SC-003**: Existing deferred material and missing-resource retry tests continue to pass.

## Assumptions

- The affected scene uses the existing object graph scene format.
- The cube should be visible using the established scene fallback material when no explicit material is saved.
- Validation can start with CPU-side render scene evidence; RenderDoc capture remains the preferred follow-up if GPU submission still disagrees after CPU-side draw generation is correct.
