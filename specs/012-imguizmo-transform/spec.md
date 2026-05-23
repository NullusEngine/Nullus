# Feature Specification: ImGuizmo Transform Gizmo

**Feature Branch**: `012-imguizmo-transform`
**Created**: 2026-05-04
**Status**: Draft
**Input**: User description: "接入 https://github.com/CedricGuillemet/ImGuizmo 这个 Dear ImGui transform gizmo 工具，在编辑器 Scene View 中替换现有自绘坐标轴，保留选中对象后的平移、旋转、缩放操作和快捷键 W/E/R。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Manipulate Selected Actors in Scene View (Priority: P1)

An editor user selects an actor in Scene View and uses the visible transform gizmo to move, rotate, or scale that actor directly in the viewport.

**Why this priority**: Replacing the current coordinate-axis tool is only valuable if the primary transform workflow remains immediately usable.

**Independent Test**: Can be tested by selecting a scene actor, dragging each operation's handles in Scene View, and confirming the actor's transform values update visibly and in the inspector.

**Acceptance Scenarios**:

1. **Given** an actor is selected in Scene View, **When** the user chooses move mode and drags a gizmo handle, **Then** the actor moves along the intended handle direction and the inspector position reflects the change.
2. **Given** an actor is selected in Scene View, **When** the user chooses rotate mode and drags a gizmo handle, **Then** the actor rotates around the intended axis and the inspector rotation reflects the change.
3. **Given** an actor is selected in Scene View, **When** the user chooses scale mode and drags a gizmo handle, **Then** the actor scales on the intended axis or axes and the inspector scale reflects the change.

---

### User Story 2 - Preserve Existing Editing Shortcuts (Priority: P2)

An editor user continues using the current W, E, and R shortcuts to switch between move, rotate, and scale modes without learning a new mode-selection workflow.

**Why this priority**: The replacement should improve the viewport manipulator without disrupting established editor muscle memory.

**Independent Test**: Can be tested by focusing or hovering Scene View, pressing W, E, and R, and verifying the visible gizmo mode changes before manipulation.

**Acceptance Scenarios**:

1. **Given** Scene View is active and no UI control is being edited, **When** the user presses W, **Then** the transform gizmo enters move mode.
2. **Given** Scene View is active and no UI control is being edited, **When** the user presses E, **Then** the transform gizmo enters rotate mode.
3. **Given** Scene View is active and no UI control is being edited, **When** the user presses R, **Then** the transform gizmo enters scale mode.

---

### User Story 3 - Avoid Interference with Selection and Camera Control (Priority: P3)

An editor user can still select actors, clear selection, and navigate the camera without the transform gizmo capturing unrelated input.

**Why this priority**: Scene View must remain usable for normal editing even when the gizmo is visible.

**Independent Test**: Can be tested by interacting with empty space, actor surfaces, gizmo handles, UI controls, and right-mouse camera navigation in the same Scene View session.

**Acceptance Scenarios**:

1. **Given** an actor is selected and the transform gizmo is visible, **When** the user right-drags to control the camera, **Then** the camera moves without changing the selected actor's transform.
2. **Given** the cursor is over a non-gizmo actor surface, **When** the user clicks that actor, **Then** actor selection behaves as before.
3. **Given** the cursor is over empty Scene View space and no gizmo drag is active, **When** the user clicks, **Then** selection clearing behaves as before.

---

### Edge Cases

- If no actor is selected, no transform gizmo should be shown and actor picking should continue to work.
- If the selected actor is destroyed or unselected while the user is interacting, gizmo interaction should stop without accessing the removed actor.
- If Scene View is resized, docked, or uses a bottom-left render target origin, gizmo placement and mouse hit testing should remain aligned with the rendered scene.
- If the current graphics backend disables scene picking readback, the transform gizmo should still avoid blocking camera navigation or unrelated UI input.
- If snapping is enabled through the existing editor snap modifier, transform changes should use the existing translation, rotation, and scaling snap settings.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Scene View MUST present the new transform gizmo at the selected actor's world transform when an editable actor is selected.
- **FR-002**: Scene View MUST use the new transform gizmo as the default viewport transform manipulator instead of the existing custom coordinate-axis gizmo.
- **FR-003**: Users MUST be able to move, rotate, and scale the selected actor through the new transform gizmo.
- **FR-004**: Users MUST be able to switch transform modes with the existing W, E, and R shortcuts when Scene View is the active editing target.
- **FR-005**: Transform changes made through the new gizmo MUST update the same actor transform data observed by the inspector, serialization, and normal scene rendering.
- **FR-006**: The new gizmo MUST respect existing editor snapping intent and snap units for translation, rotation, and scaling.
- **FR-007**: The new gizmo MUST not trigger actor selection changes while the user is actively dragging a gizmo handle.
- **FR-008**: Camera navigation and UI control editing MUST take priority over gizmo shortcuts and manipulation when those interactions are active.
- **FR-009**: The existing custom coordinate-axis rendering and picking path MUST no longer appear as the default selected-actor transform tool after the replacement is complete.
- **FR-010**: The integration MUST use ImGuizmo under a license compatible with the project and include enough dependency provenance for future maintainers to update or audit it.
- **FR-011**: The feature MUST preserve Scene View actor picking behavior for non-gizmo clicks.
- **FR-012**: Runtime game rendering outside the editor viewport MUST remain unchanged by this editor tool replacement.

### Key Entities

- **Selected Actor**: The currently selected scene object whose transform can be edited in Scene View.
- **Transform Gizmo**: The viewport manipulator shown for the selected actor, with move, rotate, and scale modes.
- **Scene View**: The editor viewport that displays the scene, handles camera navigation, actor picking, and transform manipulation.
- **Transform Operation**: The user's current manipulation mode: move, rotate, or scale.
- **Snap Settings**: Existing editor values that constrain translation, rotation, and scaling changes when snapping is requested.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a manual Scene View verification session, a tester can move, rotate, and scale a selected actor with the replacement gizmo in under 2 minutes without using inspector numeric fields.
- **SC-002**: W, E, and R switch the visible gizmo operation correctly in 100% of tested active Scene View attempts where no UI control is being edited.
- **SC-003**: During 20 consecutive mixed interactions covering actor clicks, empty-space clicks, camera navigation, and gizmo drags, no unintended selection change or transform change occurs.
- **SC-004**: The selected actor's inspector transform values match the visual result after each tested move, rotate, and scale operation.
- **SC-005**: Existing automated tests relevant to editor/runtime transform behavior continue to pass, or any unavailable test target is documented with a focused manual verification result.

## Assumptions

- ImGuizmo is an acceptable third-party dependency because it is a Dear ImGui-based transform gizmo library with MIT license terms and small source footprint.
- The v1 replacement targets the editor Scene View transform tool; runtime/gameplay gizmos are out of scope.
- The first implementation should preserve the existing single-selected-actor workflow rather than introducing multi-selection manipulation.
- The editor's existing local/world transform conventions remain authoritative; any mode not currently exposed by Nullus does not need to be added by this feature.
- Existing Scene View keyboard shortcuts, snap modifier behavior, and transform inspector are the user-facing compatibility baseline.
