# Feature Specification: UE4.27 Deferred Lighting Alignment

**Feature Branch**: `025-ue427-deferred-alignment`  
**Created**: 2026-05-12  
**Status**: Draft  
**Input**: User description: "Align Nullus deferred renderer with UE4.27 phase-one deferred lighting: GBuffer, SceneColor initialization, ambient, per-light additive deferred lighting, and editor overlay/debug pass; exclude shadows, SSAO, reflections, translucency, and full tiled deferred optimization."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Deferred Scene Is Lit By Scene Lights (Priority: P1)

An editor user opens the default scene with the deferred renderer and sees meshes lit by the scene's ambient, directional, and point light data instead of only the fixed ambient floor.

**Why this priority**: The current symptom is that deferred output remains extremely dark even after GBuffer generation succeeds. Visible scene lighting is the minimum viable deferred renderer behavior.

**Independent Test**: Render or inspect a deferred frame with valid GBuffer albedo and scene lights; the lighting result must exceed the ambient floor and must still work when clustered culling contributes no per-pixel entries.

**Acceptance Scenarios**:

1. **Given** a scene with visible opaque geometry and a default ambient light, **When** deferred lighting composites the GBuffer, **Then** the material receives ambient contribution from that light and not only the built-in ambient floor.
2. **Given** a scene with a directional light facing geometry, **When** deferred lighting composites the GBuffer, **Then** the directional light contributes based on the GBuffer normal and light direction.
3. **Given** a scene with a point light in range, **When** deferred lighting composites the GBuffer, **Then** the point light contributes with distance attenuation even if the clustered light list for that pixel is empty.

---

### User Story 2 - RenderDoc Shows UE-Like Deferred Stages (Priority: P2)

A rendering engineer opens a RenderDoc capture and can distinguish the deferred GBuffer stage, the SceneColor initialization / deferred lighting stage, and editor overlay/debug passes.

**Why this priority**: Debugging has been slowed by ambiguous capture stages. The requested UE alignment must remain inspectable.

**Independent Test**: Capture an editor DX12 frame and verify the event tree contains stable, descriptive marker names for the deferred phases and appended editor passes.

**Acceptance Scenarios**:

1. **Given** a deferred editor frame captured through the Nullus RenderDoc runner, **When** the event tree is inspected, **Then** the GBuffer, deferred lighting, light-grid compute support, and editor overlay/debug passes have clear names.
2. **Given** an editor overlay pass appended after the main scene, **When** deferred scene execution is compiled, **Then** the overlay pass remains after lighting and retains its debug name.

---

### User Story 3 - Scope Remains Phase-One Compatible (Priority: P3)

A renderer maintainer can compare Nullus with UE4.27 and see a small phase-one alignment without confusing it for a complete UE renderer port.

**Why this priority**: UE4.27 deferred rendering is broad. This change must fix current lighting while preserving a clear path for future tiled/shadow/reflection work.

**Independent Test**: Review the spec, plan, and code comments/contracts; they must state what is implemented now and what is intentionally deferred.

**Acceptance Scenarios**:

1. **Given** the phase-one implementation, **When** maintainers review it, **Then** they can identify GBuffer, SceneColor/deferred lighting, and overlay/debug stages without expecting shadows, SSAO, reflections, translucency, or full tiled deferred optimization.
2. **Given** clustered light-grid support remains present, **When** deferred lighting runs, **Then** clustered culling is treated as an optimization/support path rather than the only source of scene lights.

---

### Edge Cases

- A scene with no lights still produces the configured ambient floor or sky fallback rather than undefined color.
- A scene with Ambient Sphere lights treats them as global ambient contributors for deferred lighting.
- A scene with point or spot lights whose range is zero or invalid avoids division by zero and does not corrupt SceneColor.
- A frame with missing GBuffer resources skips or degrades the lighting pass without crashing.
- RenderDoc naming remains stable for threaded and non-threaded deferred execution paths.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Deferred lighting MUST consume GBuffer albedo, normal, material, depth, and the current frame's scene light data.
- **FR-002**: Deferred lighting MUST apply ambient light contribution from Ambient Box and Ambient Sphere lights independently of clustered per-pixel membership.
- **FR-003**: Deferred lighting MUST apply directional light contribution independently of clustered per-pixel membership.
- **FR-004**: Deferred lighting MUST apply point and spot light contribution from the packed scene light list for the phase-one path; clustered culling MAY remain as a later optimization but MUST NOT be required for visible contribution.
- **FR-005**: Deferred lighting MUST preserve sky/background fallback behavior for pixels without GBuffer geometry.
- **FR-006**: Deferred scene execution MUST retain clear RenderDoc/debug names for GBuffer, light-grid compute support, deferred lighting, and appended editor overlay/debug passes.
- **FR-007**: The default deferred editor path MUST remain runnable on the validated DX12 backend.
- **FR-008**: The implementation MUST avoid hand-editing generated files under `Runtime/*/Gen/`.
- **FR-009**: The phase-one change MUST explicitly exclude shadows, SSAO, reflections, translucency, and full tiled deferred optimization.

### Key Entities

- **Deferred Scene Frame**: One rendered frame containing GBuffer outputs, SceneColor output, depth, scene lights, and optional editor overlay/debug passes.
- **Scene Light List**: The per-frame packed light data built from active LightComponents and consumed by deferred lighting.
- **RenderDoc Stage Marker**: A stable debug label identifying a render or compute stage in captures.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a deferred editor DX12 RenderDoc capture with default scene lights and valid GBuffer albedo, the lit SceneColor contains values above the previous ambient-floor-only maximum.
- **SC-002**: Targeted unit/contract tests verify deferred shader/light-list behavior for ambient, directional, and point contributions independent of clustered per-pixel lists.
- **SC-003**: RenderDoc event labels include distinct deferred GBuffer, deferred lighting, light-grid compute support, and editor overlay/debug pass names.
- **SC-004**: `Editor` and relevant `NullusUnitTests` targets build successfully for the validated configuration, or any blocker is documented with exact command output.

## Assumptions

- UE4.27 is used as a structural reference, not as source to copy wholesale.
- Phase one may implement per-frame fullscreen light-list accumulation before introducing per-light draw volumes or tiled deferred optimization.
- DX12 editor rendering is the primary validation target for this fix because the user is debugging DX12 RenderDoc captures.
- Existing clustered light-grid data structures remain available for forward rendering and future deferred optimization.
