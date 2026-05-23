# Tasks: ImGuizmo Transform Gizmo

**Input**: Design documents from `/specs/012-imguizmo-transform/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/scene-view-gizmo.md, quickstart.md

**Tests**: Include focused unit tests for the adapter/helper where deterministic coverage is practical, plus required manual Scene View validation from quickstart.md.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Vendor ImGuizmo and make it buildable without changing editor behavior yet.

- [X] T001 Add upstream ImGuizmo source files and provenance under `ThirdParty/ImGuizmo/ImGuizmo.h`, `ThirdParty/ImGuizmo/ImGuizmo.cpp`, `ThirdParty/ImGuizmo/LICENSE`, and `ThirdParty/ImGuizmo/README.md`
- [X] T002 Create an `ImGuizmo` static library target linked to `ImGui` in `ThirdParty/imgui.cmake`
- [X] T003 Register the `ImGuizmo` target folder and include path from `ThirdParty/CMakeLists.txt`
- [X] T004 Link the `Editor` target against `ImGuizmo` in `Project/Editor/CMakeLists.txt`
- [X] T005 Build the dependency wiring with `cmake --build Build --config Debug --target Editor -- /m:1`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the adapter and Scene View overlay hook that all user stories use.

**CRITICAL**: No user story work can begin until this phase is complete.

- [X] T006 [P] Add `SceneViewGizmoOperation`, `SceneViewGizmoInteraction`, and `SceneViewGizmoMatrices` declarations in `Project/Editor/Core/SceneViewImGuizmo.h`
- [X] T007 [P] Add deterministic operation mapping and snap-value helper tests in `Tests/Unit/ImGuizmoTransformAdapterTests.cpp`
- [X] T008 Implement operation mapping, snap modifier detection, and snap value selection in `Project/Editor/Core/SceneViewImGuizmo.cpp`
- [X] T009 Add `Project/Editor/Core/SceneViewImGuizmo.cpp` and `Project/Editor/Core/SceneViewImGuizmo.h` to the `NullusUnitTests` target sources in `Tests/Unit/CMakeLists.txt`
- [X] T010 Add protected `DrawViewportOverlay()` hook and last image bounds accessors in `Project/Editor/Panels/AView.h`
- [X] T011 Call `DrawViewportOverlay()` after `UI::PanelWindow::_Draw_Impl()` in `Project/Editor/Panels/AView.cpp`
- [X] T012 Add Scene View gizmo state fields and method declarations in `Project/Editor/Panels/SceneView.h`
- [X] T013 Build and run adapter tests with `cmake --build Build --config Debug --target NullusUnitTests -- /m:1` and `ctest --test-dir Build -C Debug --output-on-failure -R NullusUnitTests`

**Checkpoint**: ImGuizmo is available to Editor, deterministic adapter logic is covered, and Scene View can draw a post-image overlay.

---

## Phase 3: User Story 1 - Manipulate Selected Actors in Scene View (Priority: P1) MVP

**Goal**: A selected actor can be moved, rotated, and scaled from the Scene View ImGuizmo overlay.

**Independent Test**: Select an actor, drag move/rotate/scale handles, and verify actor transform and inspector values change visibly.

### Tests for User Story 1

- [X] T014 [P] [US1] Add matrix round-trip tests for transform-to-gizmo and gizmo-to-transform conversion in `Tests/Unit/ImGuizmoTransformAdapterTests.cpp`

### Implementation for User Story 1

- [X] T015 [US1] Implement matrix conversion between `NLS::Maths::Matrix4` and ImGuizmo float matrices in `Project/Editor/Core/SceneViewImGuizmo.cpp`
- [X] T016 [US1] Implement selected actor matrix read and transform write-back helpers in `Project/Editor/Core/SceneViewImGuizmo.cpp`
- [X] T017 [US1] Implement `SceneView::DrawViewportOverlay()` to render ImGuizmo over the Scene View image bounds in `Project/Editor/Panels/SceneView.cpp`
- [X] T018 [US1] Pass Scene View camera view/projection matrices, selected actor transform, operation, and snap values to ImGuizmo in `Project/Editor/Panels/SceneView.cpp`
- [X] T019 [US1] Apply ImGuizmo manipulation results through `TransformComponent::SetWorldPosition`, `SetWorldRotation`, and `SetWorldScale` in `Project/Editor/Core/SceneViewImGuizmo.cpp`
- [X] T020 [US1] Stop sending custom gizmo draw descriptors for the selected actor from `Project/Editor/Panels/SceneView.cpp`
- [X] T021 [US1] Remove default selected-actor custom gizmo drawing from `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [X] T022 [US1] Remove default selected-actor custom gizmo command capture from `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [X] T023 [US1] Build and run `NullusUnitTests` after transform manipulation integration with `cmake --build Build --config Debug --target Editor NullusUnitTests -- /m:1` and `ctest --test-dir Build -C Debug --output-on-failure`

**Checkpoint**: User Story 1 is functional and testable independently as the MVP.

---

## Phase 4: User Story 2 - Preserve Existing Editing Shortcuts (Priority: P2)

**Goal**: W, E, and R keep selecting move, rotate, and scale modes, and the toolbar reflects the same state used by ImGuizmo.

**Independent Test**: Focus or hover Scene View, press W/E/R, and verify the visible gizmo operation changes before manipulation.

### Tests for User Story 2

- [X] T024 [P] [US2] Add operation mapping tests for `EGizmoOperation` to ImGuizmo operations in `Tests/Unit/ImGuizmoTransformAdapterTests.cpp`

### Implementation for User Story 2

- [X] T025 [US2] Keep W/E/R shortcut updates routed through `SceneView::SetCurrentGizmoOperation()` in `Project/Editor/Panels/SceneView.cpp`
- [X] T026 [US2] Ensure `EditorTopBar` toolbar controls continue reading and setting `SceneView::GetCurrentGizmoOperation()` and `SetCurrentGizmoOperation()` in `Project/Editor/Panels/EditorTopBar.cpp`
- [X] T027 [US2] Ensure `SceneView::DrawViewportOverlay()` uses `m_currentOperation` for every ImGuizmo draw in `Project/Editor/Panels/SceneView.cpp`
- [X] T028 [US2] Build and run shortcut-related unit coverage with `cmake --build Build --config Debug --target NullusUnitTests -- /m:1` and `ctest --test-dir Build -C Debug --output-on-failure -R NullusUnitTests`

**Checkpoint**: User Story 2 works independently on top of the Scene View overlay.

---

## Phase 5: User Story 3 - Avoid Interference with Selection and Camera Control (Priority: P3)

**Goal**: Actor picking, empty-space unselection, UI editing, and camera control remain usable while the gizmo exists.

**Independent Test**: In one Scene View session, interact with empty space, actor surfaces, gizmo handles, UI controls, and right-mouse camera navigation with no unintended selection or transform changes.

### Tests for User Story 3

- [X] T029 [P] [US3] Add interaction-state tests for hovered/using suppression flags in `Tests/Unit/ImGuizmoTransformAdapterTests.cpp`

### Implementation for User Story 3

- [X] T030 [US3] Store ImGuizmo `isHovered` and `isUsing` state after each overlay draw in `Project/Editor/Panels/SceneView.cpp`
- [X] T031 [US3] Skip actor picking, empty-space unselection, and hover-picking updates while ImGuizmo is using the mouse in `Project/Editor/Panels/SceneView.cpp`
- [X] T032 [US3] Prevent ImGuizmo manipulation while right-mouse camera control is active in `Project/Editor/Panels/SceneView.cpp`
- [X] T033 [US3] Preserve non-gizmo actor picking and empty-space unselection behavior when ImGuizmo is not hovered or using input in `Project/Editor/Panels/SceneView.cpp`
- [X] T034 [US3] Remove obsolete custom gizmo direction state from `Project/Editor/Panels/SceneView.h` and `Project/Editor/Panels/SceneView.cpp`
- [X] T035 [US3] Remove obsolete custom gizmo picking branches from `Project/Editor/Rendering/PickingRenderPass.cpp` and `Project/Editor/Rendering/PickingRenderPass.h`
- [X] T036 [US3] Build and run interaction validation coverage with `cmake --build Build --config Debug --target Editor NullusUnitTests -- /m:1` and `ctest --test-dir Build -C Debug --output-on-failure`

**Checkpoint**: User Story 3 preserves normal Scene View input behavior.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Clean up unused custom gizmo code, document provenance, and record verification evidence.

- [X] T037 [P] Remove unused `Project/Editor/Core/GizmoBehaviour.cpp` and `Project/Editor/Core/GizmoBehaviour.h` if no remaining references exist
- [X] T038 [P] Remove unused `Project/Editor/Rendering/GizmoRenderer.cpp` and `Project/Editor/Rendering/GizmoRenderer.h` if no remaining references exist
- [X] T039 [P] Remove custom gizmo-only material/model references from `Project/Editor/Rendering/PickingRenderPass.cpp` and `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [X] T040 [P] Document ImGuizmo upstream URL, commit or release source, and MIT license in `ThirdParty/ImGuizmo/README.md`
- [X] T041 Run full quickstart automated validation from `specs/012-imguizmo-transform/quickstart.md`
- [ ] T042 Perform manual Scene View verification from `specs/012-imguizmo-transform/quickstart.md` and record backend/platform evidence in `specs/012-imguizmo-transform/quickstart.md`
- [X] T043 Run `rg -n "GizmoBehaviour|GizmoRenderer|highlightedGizmo|DrawGizmo|CaptureGizmo" Project/Editor Tests` and remove or justify any remaining old custom gizmo references
- [X] T044 Run `git status --short --branch` and confirm only intended feature files changed, preserving unrelated `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; starts immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational; MVP.
- **User Story 2 (Phase 4)**: Depends on Foundational and is most meaningful after US1 draws the overlay.
- **User Story 3 (Phase 5)**: Depends on US1 interaction state and should follow US1.
- **Polish (Phase 6)**: Depends on completed desired user stories.

### User Story Dependencies

- **US1**: Can start after Phase 2; no dependency on US2 or US3.
- **US2**: Can start after Phase 2, but visual verification requires US1 overlay draw.
- **US3**: Requires US1 because it gates actor picking against ImGuizmo state.

### Within Each User Story

- Write or update tests before implementation tasks in that story.
- Adapter helpers before Scene View integration.
- Scene View overlay before old renderer/picking cleanup.
- Build/test at each checkpoint before moving forward.

### Parallel Opportunities

- T006 and T007 can run in parallel after Setup.
- T014 can run while T015/T016 are being prepared if adapter interfaces are agreed.
- T024 can run independently from T025/T026 once the operation mapping interface exists.
- T029 can run independently from Scene View picking edits once interaction-state helper names exist.
- T037, T038, T039, and T040 can run in parallel after US3 cleanup identifies remaining references.

---

## Parallel Example: User Story 1

```text
Task: "T014 [US1] Add matrix round-trip tests in Tests/Unit/ImGuizmoTransformAdapterTests.cpp"
Task: "T017 [US1] Implement SceneView::DrawViewportOverlay in Project/Editor/Panels/SceneView.cpp"
```

These can proceed in parallel only after T006 defines the adapter API and T010/T011 expose the overlay hook.

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational.
3. Complete Phase 3: User Story 1.
4. Stop and validate move, rotate, scale manipulation before shortcut and input-polish work.

### Incremental Delivery

1. Setup + Foundational: ImGuizmo builds and adapter boundaries exist.
2. US1: selected actor transform manipulation works.
3. US2: shortcuts and toolbar operation state are confirmed.
4. US3: actor picking and camera control are protected from gizmo input.
5. Polish: remove unused old gizmo code and record validation.

### Validation Discipline

- Run targeted build/tests at each checkpoint.
- Run manual Scene View verification before claiming completion.
- State exactly which backend/platform was manually validated.
- Do not edit generated files under `Runtime/*/Gen/`.
- Preserve unrelated working-tree changes.
