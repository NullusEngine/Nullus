# Feature Specification: Scene Camera Focus

**Feature Branch**: `013-scene-camera-focus`
**Created**: 2026-05-04
**Status**: Draft
**Input**: User description: "Design a camera focus concept: click rotation always orbits the focus; camera focus changes as the camera pans, rotates, zooms, or focuses; middle mouse panning distance scales by focus distance so focus movement appears synchronized with mouse movement."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - ViewGizmo Orbits The Current Focus (Priority: P1)

As an editor user, I want Scene View orientation clicks to rotate the camera around the current visual focus point, so changing view direction does not unexpectedly shift the object or area I am inspecting.

**Why this priority**: ViewGizmo click rotation is the behavior currently being tuned and is most visible when the camera rotates around a fixed artificial length instead of the user's current focus.

**Independent Test**: Set a camera position, rotation, focus point, and focus distance; click a ViewGizmo face; verify the resulting camera looks at the same focus point from the new direction and keeps the same focus distance.

**Acceptance Scenarios**:

1. **Given** the Scene View camera has a focus point 25 units away, **When** the user clicks a ViewGizmo side face, **Then** the camera rotates around that focus point instead of around a hard-coded short distance.
2. **Given** a ViewGizmo click is rotating toward a face on the opposite side, **When** the route would otherwise pass over the top or bottom pole, **Then** the camera continues to route around world Y while preserving the same focus point.
3. **Given** the user starts right-mouse camera look while a ViewGizmo click rotation is still in progress, **When** right-mouse control begins, **Then** the click rotation is canceled and cannot overwrite the right-mouse camera result.

---

### User Story 2 - Camera Controls Maintain A Coherent Focus (Priority: P2)

As an editor user, I want the camera's focus point to move consistently when I pan, rotate, zoom, or focus content, so subsequent orbit and ViewGizmo actions operate on the place I am actually working.

**Why this priority**: A focus point only helps if all Scene View camera controls update it coherently.

**Independent Test**: Exercise camera pan, right-mouse look, scroll zoom, and focus-to-selection from known camera/focus states; verify the focus point and distance update according to the behavior rules.

**Acceptance Scenarios**:

1. **Given** a camera has a focus point and distance, **When** the user pans the camera, **Then** the camera and focus point move by the same world-space delta.
2. **Given** a camera has a focus distance, **When** the user rotates with right mouse, **Then** the focus point updates to the new point on the camera forward ray at that distance.
3. **Given** a camera has a focus point, **When** the user zooms along the view direction, **Then** the focus point remains stable and the focus distance changes to match the new camera position.
4. **Given** an actor is selected, **When** the user focuses the selected actor, **Then** the focus point becomes the selected actor focus center and the focus distance becomes the framing distance.

---

### User Story 3 - Middle-Mouse Pan Feels Screen-Synchronized (Priority: P3)

As an editor user, I want middle-mouse panning to move the focus plane in proportion to mouse movement, so the focused scene point appears to track the mouse without drifting faster or slower at different distances.

**Why this priority**: Middle-mouse panning is usable today, but its fixed drag speed does not account for the distance to the current focus.

**Independent Test**: Compare pan deltas at multiple focus distances and viewport heights; verify the world-space delta uses the camera FOV, viewport height, and focus distance so screen-space movement remains proportional.

**Acceptance Scenarios**:

1. **Given** two focus distances where one is twice as far as the other, **When** the same middle-mouse pixel drag is applied, **Then** the farther focus state produces twice the world-space pan distance.
2. **Given** a middle-mouse drag occurs at a known viewport height, **When** the camera pans, **Then** the focus point and camera move together by the same world delta.

### Edge Cases

- If no focus exists yet, the Scene View initializes one from the current camera forward direction and a default focus distance.
- If the focus distance becomes extremely small, camera controls clamp it to a safe minimum so pan scaling and orbit math remain stable.
- If the viewport height is zero or unavailable, middle-mouse panning falls back to the previous safe focus distance behavior and avoids division by zero.
- Top and bottom ViewGizmo clicks remain allowed as destination views, but transitions to opposite horizontal faces must not flip over the poles.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Scene View MUST maintain a current camera focus point and focus distance for editor camera operations.
- **FR-002**: ViewGizmo click rotation MUST orbit around the current focus point and preserve the current focus distance.
- **FR-003**: ViewGizmo opposite-side routing MUST continue to avoid top/bottom pole flips while orbiting around the current focus point.
- **FR-004**: Right-mouse camera look MUST have priority over ViewGizmo click rotation and MUST cancel any active ViewGizmo click rotation.
- **FR-005**: Right-mouse camera look MUST update the focus point to the point on the new camera forward ray at the current focus distance.
- **FR-006**: Middle-mouse camera pan MUST move the camera position and focus point by the same world-space delta.
- **FR-007**: Middle-mouse camera pan MUST scale world movement from mouse pixel movement using the current focus distance, camera field of view, and viewport height.
- **FR-008**: Scroll zoom MUST update focus distance to match the new camera-to-focus separation while keeping the focus point stable.
- **FR-009**: Focus-to-selection behavior MUST update both camera destination and focus state to the selected actor's focus center and framing distance.
- **FR-010**: Existing FPS keyboard movement MUST keep the focus point coherent with camera translation, preserving the current focus distance.

### Key Entities

- **Scene Camera Focus**: The editor-only focus state containing a world-space focus point, the distance from camera to focus, and whether the focus has been initialized.
- **Camera Focus Update**: A camera control result that may change camera position, camera rotation, focus point, and focus distance together.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a controlled test, ViewGizmo click rotation keeps the final camera forward vector aimed at the original focus point within 0.5 degrees.
- **SC-002**: In a controlled test, middle-mouse pan at doubled focus distance produces doubled world-space pan delta for the same viewport and mouse movement within 1% tolerance.
- **SC-003**: In manual Scene View validation, starting right-mouse camera look during an active ViewGizmo click rotation prevents any later click-rotation overwrite.
- **SC-004**: In unit tests, focus state remains finite and stable for zero viewport height, near-zero focus distance, top/bottom view directions, and ordinary side view directions.

## Assumptions

- The focus concept is editor-only and applies to Scene View camera navigation, not runtime game cameras.
- Existing selected-actor focus distance behavior remains the basis for framing selected content.
- A small positive minimum focus distance is acceptable to avoid singular camera math.
- The current validated platform for this work is the Windows Debug Editor build and unit test target.
