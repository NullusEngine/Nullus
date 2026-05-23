# Data Model: ImGuizmo Transform Gizmo

## Selected Actor

**Purpose**: The scene object currently targeted by viewport transform manipulation.

**Fields**:

- `worldId`: stable identity used to detect selection and destruction.
- `transform`: existing transform component containing local/world position, rotation, scale, and matrices.
- `isEditable`: actor has a transform component and is valid for editor manipulation.

**Relationships**:

- Owned by the active scene.
- Referenced by Scene View selection state.
- Read and modified by the transform gizmo session.

**Validation Rules**:

- A selected actor without a transform component is not editable by the gizmo.
- If the actor is destroyed, the active gizmo session must end before applying more changes.

## Transform Gizmo Session

**Purpose**: The transient interaction state for one Scene View gizmo manipulation.

**Fields**:

- `operation`: move, rotate, or scale.
- `bounds`: the Scene View image rectangle in screen coordinates.
- `viewMatrix`: current Scene View camera view matrix.
- `projectionMatrix`: current Scene View camera projection matrix.
- `modelMatrix`: selected actor transform matrix used by the gizmo.
- `snapEnabled`: true when existing editor snap modifier is active.
- `snapValue`: per-operation snap amount from editor settings.
- `isHovered`: true when the cursor is over a gizmo control.
- `isUsing`: true while a gizmo manipulation is active.

**Relationships**:

- References one selected actor.
- Uses one Scene View camera.
- Uses current editor snap settings.

**State Transitions**:

- `inactive` -> `hovered`: selected actor exists and cursor is over gizmo handle.
- `hovered` -> `active`: user starts a gizmo drag.
- `active` -> `inactive`: user releases the drag button, actor is unselected, actor is destroyed, or Scene View loses a valid render area.
- `inactive` -> `inactive`: no selected actor or no valid view bounds.

**Validation Rules**:

- `bounds` must match the last drawn Scene View image bounds.
- `modelMatrix` must be synchronized from the selected actor before each manipulation.
- Transform output must be decomposed and applied to existing actor transform fields.

## Transform Operation

**Purpose**: The selected manipulation mode exposed by Scene View and EditorTopBar.

**Values**:

- `TRANSLATE`: move selected actor.
- `ROTATE`: rotate selected actor.
- `SCALE`: scale selected actor.

**Validation Rules**:

- W selects `TRANSLATE`.
- E selects `ROTATE`.
- R selects `SCALE`.
- Toolbar controls and shortcuts update the same operation state.

## Snap Settings

**Purpose**: Existing editor configuration used to constrain manipulation increments.

**Fields**:

- `translationSnapUnit`
- `rotationSnapUnit`
- `scalingSnapUnit`
- `snapModifierActive`

**Validation Rules**:

- Snap values must only affect the matching operation.
- Snap modifier state must not activate while a UI input control is being edited.

## Actor Picking State

**Purpose**: Scene View state that distinguishes actor hover/selection from gizmo interaction.

**Fields**:

- `highlightedActor`
- `hasPickingSample`
- `lastPickingMousePosition`
- `gizmoIsHovered`
- `gizmoIsUsing`

**Validation Rules**:

- Actor selection must not change while `gizmoIsUsing` is true.
- Non-gizmo clicks should preserve existing actor selection and empty-space unselection behavior.
