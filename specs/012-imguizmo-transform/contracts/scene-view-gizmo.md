# Contract: Scene View Transform Gizmo

## Scope

This contract describes the expected behavior between Scene View, actor selection, editor input, and the ImGuizmo-based transform manipulator. It is an internal UI behavior contract, not a network or public plugin API.

## Inputs

- Active Scene View render image bounds in screen coordinates.
- Active Scene View camera view and projection matrices.
- Current selected actor, if any.
- Current transform operation: `TRANSLATE`, `ROTATE`, or `SCALE`.
- Current mouse and keyboard input state.
- Existing editor snap modifier and snap unit settings.

## Outputs

- Optional transform changes applied to the selected actor's transform component.
- Gizmo interaction state exposed to Scene View picking logic:
  - `isHovered`
  - `isUsing`
- Unchanged actor selection behavior for non-gizmo clicks.

## Behavioral Rules

1. If no editable actor is selected, the gizmo does not draw and reports `isHovered = false`, `isUsing = false`.
2. If an editable actor is selected, the gizmo draws over the Scene View image using the image bounds as its manipulation rectangle.
3. W, E, and R select translate, rotate, and scale modes respectively when Scene View is the active editing target and no UI control is being edited.
4. Toolbar operation controls and keyboard shortcuts update the same operation value.
5. When the gizmo is active, Scene View must not perform actor selection or empty-space unselection from the same mouse input.
6. When right-mouse camera navigation is active, the gizmo must not apply transform changes.
7. When snapping is active, the current operation uses the matching snap unit.
8. Transform output is applied through existing actor transform APIs so inspector and scene rendering observe the same state.
9. The old model-rendered custom gizmo is not drawn as the default selected-actor transform tool once the ImGuizmo replacement is active.

## Verification Matrix

| Case | Expected Result |
|------|-----------------|
| Selected actor + move drag | Actor position changes; selection remains unchanged |
| Selected actor + rotate drag | Actor rotation changes; selection remains unchanged |
| Selected actor + scale drag | Actor scale changes; selection remains unchanged |
| Selected actor + W/E/R | Visible operation changes to move/rotate/scale |
| Selected actor + right mouse camera drag | Camera moves; actor transform does not change |
| Selected actor + click another actor outside gizmo | New actor selection follows existing picking behavior |
| Selected actor + click empty space outside gizmo | Selection clearing follows existing behavior |
| No selected actor | No gizmo is drawn; actor picking continues |
| Actor destroyed during interaction | Interaction stops without applying to invalid actor |
