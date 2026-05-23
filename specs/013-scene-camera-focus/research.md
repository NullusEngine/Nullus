# Research: Scene Camera Focus

## Decision: Store Scene View Focus As Editor-Owned State

**Rationale**: ViewGizmo click rotation currently runs from `SceneView::DrawViewportOverlay`, while right/middle mouse camera behavior runs through `CameraController`. Storing focus in `SceneView` gives both systems one shared state without embedding editor navigation assumptions into ImGuizmo.

**Alternatives considered**:
- Store focus inside ImGuizmo: rejected because focus is broader than ViewGizmo and must also update from middle/right mouse camera controls.
- Store focus only inside CameraController: rejected because ViewGizmo integration already operates in SceneView after ImGuizmo has mutated a temporary view matrix.

## Decision: Add Pure Focus Math Helpers

**Rationale**: Camera focus behavior has clear mathematical invariants: orbit preserves focus and distance, pan moves camera/focus together, right-mouse rotation projects focus along the new forward direction, and screen pan scaling depends on FOV, viewport height, and focus distance. Pure helpers can be covered by unit tests without input-manager or ImGui setup.

**Alternatives considered**:
- Inline math in `CameraController` and `SceneView`: rejected because it would duplicate focus calculations and make regressions harder to isolate.

## Decision: Middle-Mouse Pan Uses Focus Plane Pixel Scale

**Rationale**: For a perspective camera, visible world height at focus distance is `2 * distance * tan(fov / 2)`. Dividing by viewport height gives world units per pixel at the focus plane. Using that scale makes equal mouse drags move the focused scene point by visually consistent distances.

**Alternatives considered**:
- Keep fixed drag speed: rejected because it does not scale with distance and causes near/far panning mismatch.
- Use selected actor radius only: rejected because panning should work even with no selected actor.

## Decision: Right-Mouse Control Cancels ViewGizmo Interpolation

**Rationale**: The user explicitly wants right-mouse rotation to have highest priority. The existing ImGuizmo interpolation state can otherwise continue after right-mouse control ends and overwrite the camera. Cancellation must clear the ViewGizmo click interpolation, not merely skip applying it for a frame.

**Alternatives considered**:
- Pause ViewGizmo while right mouse is down: rejected because queued interpolation would resume and still overwrite the right-mouse result.
