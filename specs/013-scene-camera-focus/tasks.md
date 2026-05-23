# Tasks: Scene Camera Focus

**Input**: Design documents from `specs/013-scene-camera-focus/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Test-first is required for the focus math and adapter behavior because this is behavior-changing editor navigation work.

## Phase 1: Setup (Shared Infrastructure)

- [x] T001 [P] Add `Project/Editor/Core/SceneCameraFocus.h` declarations for focus state and pure focus update helpers.
- [x] T002 [P] Add `Tests/Unit/SceneCameraFocusTests.cpp` to `Tests/Unit/CMakeLists.txt`.

---

## Phase 2: Foundational (Blocking Prerequisites)

- [x] T003 [P] Write failing focus math tests in `Tests/Unit/SceneCameraFocusTests.cpp` for initialization, orbit around focus, right-mouse rotation focus projection, zoom distance update, and distance-scaled pan.
- [x] T004 Implement focus math helpers in `Project/Editor/Core/SceneCameraFocus.cpp`.
- [x] T005 Build and run focused `SceneCameraFocusTests` to verify the foundational helper behavior.

---

## Phase 3: User Story 1 - ViewGizmo Orbits The Current Focus (Priority: P1) MVP

**Goal**: ViewGizmo click rotations preserve the Scene View focus point and focus distance.

**Independent Test**: From a known focus point and distance, apply ViewGizmo stabilization and verify the camera ends on the orbit sphere looking at the focus.

- [x] T006 [US1] Extend ViewGizmo adapter tests in `Tests/Unit/ImGuizmoTransformAdapterTests.cpp` so stabilized ViewGizmo transforms orbit an explicit focus point and distance.
- [x] T007 [US1] Update `Project/Editor/Core/SceneViewImGuizmo.h` and `Project/Editor/Core/SceneViewImGuizmo.cpp` to accept an orbit focus point for ViewGizmo camera stabilization.
- [x] T008 [US1] Store Scene View focus state in `Project/Editor/Panels/SceneView.h` and pass it to ViewGizmo camera stabilization in `Project/Editor/Panels/SceneView.cpp`.
- [x] T009 [US1] Run focused ViewGizmo and focus tests.

---

## Phase 4: User Story 2 - Camera Controls Maintain A Coherent Focus (Priority: P2)

**Goal**: Camera controls update or preserve focus according to pan, look, zoom, FPS translation, and focus-to-selection semantics.

**Independent Test**: Run controller helper tests and manually verify focus remains coherent after each camera control type.

- [x] T010 [US2] Add focus state access/update integration points to `Project/Editor/Core/CameraController.h`.
- [x] T011 [US2] Update `Project/Editor/Core/CameraController.cpp` so right-mouse look projects focus, FPS keyboard translation moves focus with camera, zoom preserves focus point while updating distance, and focus-to-selection updates focus state.
- [x] T012 [US2] Update `Project/Editor/Panels/SceneView.cpp` to initialize and synchronize focus state before camera controls and after direct ViewGizmo changes.
- [x] T013 [US2] Run focused camera focus and ViewGizmo tests.

---

## Phase 5: User Story 3 - Middle-Mouse Pan Feels Screen-Synchronized (Priority: P3)

**Goal**: Middle-mouse pan uses focus distance, FOV, and viewport height to compute world delta.

**Independent Test**: Unit tests verify doubled focus distance doubles world delta for identical viewport and mouse movement.

- [x] T014 [US3] Update `Project/Editor/Core/CameraController.cpp` middle-mouse panning to use focus-plane world-per-pixel scaling.
- [x] T015 [US3] Ensure `Project/Editor/Panels/SceneView.cpp` provides current viewport height to CameraController focus-pan calculations.
- [x] T016 [US3] Run focused pan scaling tests.

---

## Final Phase: Polish & Validation

- [x] T017 Run `ctest --test-dir Build -C Debug --output-on-failure -R "NullusUnitTests"`.
- [x] T018 Run `cmake --build Build --config Debug --target Editor -- /m:1`.
- [x] T019 Self-review `Project/Editor/Core/SceneCameraFocus.*`, `Project/Editor/Core/CameraController.*`, `Project/Editor/Core/SceneViewImGuizmo.*`, `Project/Editor/Panels/SceneView.*`, and tests for regressions and generated-file boundary compliance.

## Dependencies & Execution Order

- Phase 1 can begin immediately.
- Phase 2 depends on Phase 1 and blocks all user stories.
- US1 should be implemented before US2 because ViewGizmo focus integration establishes Scene View focus ownership.
- US2 should be implemented before US3 because pan scaling needs the shared focus state and controller integration.
- Final validation depends on all selected stories.

## Implementation Strategy

1. Build pure focus math with tests first.
2. Integrate ViewGizmo orbit around focus as the MVP.
3. Wire focus state through CameraController.
4. Replace fixed middle-mouse pan speed with focus-plane scaling.
5. Validate with unit tests and Editor build.
