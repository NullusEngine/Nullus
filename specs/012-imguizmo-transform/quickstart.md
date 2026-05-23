# Quickstart: ImGuizmo Transform Gizmo

## Prerequisites

- Work on branch `012-imguizmo-transform`.
- Keep spec artifacts under `specs/012-imguizmo-transform/` updated as implementation changes.
- Do not edit generated files under `Runtime/*/Gen/`.
- Preserve unrelated working-tree changes, including the current modified `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` if it is still present.

## Implementation Entry Points

1. Add ImGuizmo sources under `ThirdParty/ImGuizmo/` with upstream license/provenance.
2. Add an `ImGuizmo` CMake target or include ImGuizmo in the existing ImGui-related third-party build.
3. Link the Editor target to the ImGuizmo build product.
4. Add a focused Scene View gizmo adapter/helper if needed for matrix conversion and operation mapping.
5. Draw the ImGuizmo overlay after the Scene View image has valid draw bounds.
6. Use ImGuizmo hover/active state to suppress actor picking only while the gizmo needs the mouse.
7. Disable the old custom selected-actor gizmo as the default Scene View transform manipulator.

## Build And Automated Validation

From repository root:

```powershell
cmake --build Build --config Debug --target Editor NullusUnitTests -- /m:1
ctest --test-dir Build -C Debug --output-on-failure
```

If the local build directory is named `build` instead of `Build`, use:

```powershell
cmake --build build --config Debug --target Editor NullusUnitTests -- /m:1
ctest --test-dir build -C Debug --output-on-failure
```

## Manual Scene View Verification

1. Launch Editor with the locally validated backend.
2. Open or create a scene containing at least two actors.
3. Select one actor in Scene View.
4. Press W and drag the move gizmo; confirm the actor moves and inspector position changes.
5. Press E and drag the rotate gizmo; confirm the actor rotates and inspector rotation changes.
6. Press R and drag the scale gizmo; confirm the actor scales and inspector scale changes.
7. Hold the existing snap modifier and repeat one move, rotate, and scale drag; confirm changes use snap increments.
8. Right-drag in Scene View; confirm the camera moves and the selected actor transform does not change.
9. Click another actor outside the gizmo; confirm selection changes normally.
10. Click empty space outside the gizmo; confirm selection clearing behaves normally.
11. Resize or dock Scene View and repeat one move drag; confirm gizmo visual and hit testing stay aligned.

## Evidence To Record

- Build command and result.
- Test command and result.
- Backend used for manual editor verification.
- Manual verification checklist result.
- Any backend/platform behavior not validated.

## Validation Evidence

- 2026-05-04 automated validation on Windows Debug build:
  `cmake -S . -B Build; cmake --build Build --config Debug --target Editor NullusUnitTests -- /m:1; ctest --test-dir Build -C Debug --output-on-failure`
  completed successfully.
- CTest result: `NullusUnitTests` passed, 1/1 tests, 0 failures.
- Old custom gizmo reference scan:
  `rg -n "GizmoBehaviour|GizmoRenderer|highlightedGizmo|DrawGizmo|CaptureGizmo|m_gizmoRenderer|Arrow_Picking|Arrow_Translate|Arrow_Rotate|Arrow_Scale|GetShader\\(\"Gizmo|m_gizmoPickingMaterial" Project\Editor Tests -S`
  returned no matches.
- Manual Scene View interaction verification was not performed in this session; backend-specific visual alignment, drag behavior, and input interaction checks remain to be validated in the Editor runtime.
