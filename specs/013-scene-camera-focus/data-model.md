# Data Model: Scene Camera Focus

## SceneCameraFocusState

Represents the current editor Scene View focus.

### Fields

- `focusPoint`: World-space point the camera is currently focused on.
- `focusDistance`: Distance from camera position to `focusPoint`.
- `hasFocus`: Whether the state has been initialized.

### Validation Rules

- `focusDistance` must be finite and no smaller than the minimum safe focus distance.
- `focusPoint` must remain finite.
- If `hasFocus` is false, callers initialize from camera position, camera forward, and a default focus distance.

### State Transitions

- **Initialize**: `focusPoint = cameraPosition + cameraForward * defaultDistance`; `focusDistance = defaultDistance`.
- **Orbit**: `cameraPosition = focusPoint - newForward * focusDistance`; focus point and distance are preserved.
- **Pan**: camera position and focus point both move by the same world delta; distance is preserved.
- **Right-mouse look**: camera rotation changes; focus point becomes `cameraPosition + newForward * focusDistance`.
- **FPS translation**: camera position and focus point move by the same translation delta; distance is preserved.
- **Zoom**: camera position moves along forward; focus point is preserved; distance is recomputed from camera position to focus point and clamped.
- **Focus selected actor**: focus point becomes the actor focus center; focus distance becomes the framing distance.

## CameraFocusUpdate

Represents a camera-control result that may update camera transform and focus together.

### Fields

- `cameraPosition`
- `cameraRotation`
- `focusPoint`
- `focusDistance`

### Relationships

- Produced by Scene View camera controls.
- Consumed by Scene View camera and focus state.
