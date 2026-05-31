# Feature Specification: Unity-Ready Model Drop

**Feature Branch**: `unity-ready-model-drop`
**Created**: 2026-05-31
**Status**: Draft
**Input**: User description: "调研unity是怎么做的，和unity对齐，为什么unity可以一拖入场景就能立马看到正确的效果；开始对齐unity方案"

## User Scenarios & Testing

### User Story 1 - Dragged Models Appear Textured Immediately (Priority: P1)

An editor user drags an imported model asset from the asset browser into the scene and the first visible committed scene object already has its intended mesh, material, and texture resources.

**Why this priority**: This is the direct failure mode reported by the user: Sponza appears as a white model after import/drop.

**Independent Test**: Drop a generated model prefab whose manifest includes mesh, material, and texture artifacts. The operation commits only when renderer dependencies are ready and no fallback/white material is the first visible state.

**Acceptance Scenarios**:

1. **Given** a generated model prefab with current prefab, mesh, material, and texture artifacts, **When** it is dropped into the hierarchy, **Then** the drop commits and the resulting object is visible with renderer resources ready.
2. **Given** a generated model prefab whose texture artifact is missing or invalid, **When** it is dropped into the hierarchy, **Then** the drop is rejected or marked pending before a scene object is committed.

---

### User Story 2 - Import Pending Instead Of White Placeholder (Priority: P2)

An editor user drags a model whose import or renderer dependencies are still stale. The editor reports an import/resource pending state instead of inserting a white placeholder into the scene.

**Why this priority**: Unity pays import cost before the asset becomes a ready prefab; Nullus should not expose half-ready generated model instances.

**Independent Test**: Simulate a current prefab manifest that lacks a valid texture dependency. The bridge must return `pendingImport`/diagnostics and the scene must remain unchanged.

**Acceptance Scenarios**:

1. **Given** a current model source and stale renderer artifacts, **When** the asset is dropped, **Then** no scene object is created and the result describes the missing renderer dependency.
2. **Given** a background import request for a stale model, **When** import completes, **Then** the completion path may instantiate only after the same renderer readiness check passes.

---

### User Story 3 - Old Async Resolution Path Cannot Reintroduce White Models (Priority: P3)

If a legacy or existing call path still instantiates generated model prefabs before renderer resources are bound, it must either stay hidden until ready or fail cleanly without leaving hidden objects behind.

**Why this priority**: Recent fixes hid generated roots while resolving resources, but review found failure and cancellation edge cases that could restore white models or leave invisible objects.

**Independent Test**: Exercise non-native texture paths and cancellation paths in renderer resource resolution; unresolved paths must fail and live cancelled roots must restore their original active state.

**Acceptance Scenarios**:

1. **Given** a material references a texture path that cannot be queued for native artifact loading, **When** renderer resolution runs, **Then** the task fails instead of completing with a fallback texture.
2. **Given** a generated root is hidden while resolving resources and resolution is cancelled while the root still exists, **When** cancellation completes, **Then** the original root active state is restored.

### Edge Cases

- A prefab artifact exists but one material or texture artifact file was deleted from `Library/Artifacts`.
- A material references a non-`.ntex` texture path after generated model import.
- The dragged asset handle points to an old asset id after the file was reimported.
- Renderer resource resolution is cancelled because scene generation changed, not because the object was destroyed.

## Requirements

### Functional Requirements

- **FR-001**: Generated model prefab drag/drop MUST treat renderer dependency readiness as part of asset readiness.
- **FR-002**: Generated model prefab drag/drop MUST NOT commit a visible scene object when required mesh, material, or texture artifacts are missing or invalid.
- **FR-003**: Fast prefab loading MUST verify current manifest dependencies and renderer artifact files, not only the prefab artifact file.
- **FR-004**: Texture artifacts required by generated model materials MUST be native texture artifacts that can be decoded by the current texture artifact reader.
- **FR-005**: If renderer readiness fails, the user-visible result MUST describe the missing or invalid dependency.
- **FR-006**: Existing async renderer resolution MUST treat an uncacheable texture path as a failure, not as success.
- **FR-007**: Existing async renderer resolution cancellation MUST not leave a still-live generated model root permanently hidden.

### Key Entities

- **Generated Model Prefab**: Imported model prefab artifact produced from an external model source.
- **Renderer Dependency**: Mesh, material, and texture artifacts that must be available before the generated model is visible.
- **Renderer Readiness Gate**: Validation step that determines whether a generated model prefab can be instantiated without fallback resources.
- **Transactional Drop**: Drop flow that commits a scene object only after readiness validation succeeds.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A generated model prefab with all renderer dependencies ready commits in one drag/drop operation and creates exactly one scene root.
- **SC-002**: A generated model prefab with a missing texture artifact creates zero scene objects during drag/drop.
- **SC-003**: Targeted unit tests cover ready, missing dependency, invalid texture artifact, and legacy async texture miss paths.
- **SC-004**: A RenderDoc validation capture for the reported model no longer shows representative GBuffer draws bound only to the 1x1 fallback texture after resources are ready.

## Assumptions

- This phase targets the editor/DX12 workflow reported by the user; Vulkan/macOS/Linux claims require separate validation.
- The existing DirectXTex `.ntex` pipeline remains the texture artifact producer.
- Missing model source texture files remain an import diagnostic; drag/drop should not silently create a white scene object to mask that import failure.
- The first implementation may validate artifact existence and texture header readability without eagerly creating GPU texture resources for every dependency.
