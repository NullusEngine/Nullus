# Tasks: Scene View Camera State Machine

**Input**: Design documents from `specs/040-scene-view-camera-state-machine/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Test-first is required for the new interaction-state helper and for the Scene View blocking regression because this feature changes editor interaction behavior.

**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently after the shared foundation is in place.

## Phase 1: Setup (Shared Infrastructure)

- [x] T001 Create `Project/Editor/Core/SceneViewCameraInteractionStateMachine.h` with the state ids, input snapshot, transition result, and state-machine entry points.
- [x] T002 [P] Add `Project/Editor/Core/SceneViewCameraInteractionStateMachine.cpp` and `Tests/Unit/SceneViewCameraInteractionStateMachineTests.cpp` to `Tests/Unit/CMakeLists.txt` so the helper is compiled into `NullusUnitTests`.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish the pure interaction-state helper before integrating it into Scene View and CameraController.

**⚠️ CRITICAL**: No user story work should start until these tasks are complete.

- [x] T003 [P] Write failing transition tests in `Tests/Unit/SceneViewCameraInteractionStateMachineTests.cpp` for `Neutral -> Fly`, `Neutral -> Pan`, `Neutral -> Orbit`, `Any -> Blocked`, and navigation-state cleanup back to `Neutral`.
- [x] T004 [P] Extend `Tests/Unit/CameraControllerInputTests.cpp` with failing assertions for event-driven cursor ownership expectations and no-op behavior while the interaction state remains unchanged.
- [x] T005 Implement the pure transition and side-effect decision logic in `Project/Editor/Core/SceneViewCameraInteractionStateMachine.cpp`.
- [x] T006 Refactor `Project/Editor/Core/CameraController.h` to store explicit interaction-state data instead of relying on scattered cursor-control booleans alone.
- [x] T007 Run focused foundational tests with `NullusUnitTests.exe --gtest_filter=SceneViewCameraInteractionStateMachineTests.*:CameraControllerInputTests.*`.

**Checkpoint**: The interaction-state helper exists, its transitions are test-covered, and CameraController can depend on it.

---

## Phase 3: User Story 1 - Stable Text Entry In Scene View (Priority: P1) 🎯 MVP

**Goal**: Text entry reliably blocks Scene View camera interaction and prevents camera cursor ownership from reclaiming control.

**Independent Test**: Activate a text-editing control while Scene View is hovered and verify that the camera transitions into the blocked state, releases any camera cursor ownership, and keeps the text-edit cursor stable.

- [x] T008 [P] [US1] Write failing blocking/regression assertions in `Tests/Unit/PanelWindowHookTests.cpp` for text-input blocking inside Scene View bounds and forced cleanup when text entry begins during navigation.
- [x] T009 [US1] Update `Project/Editor/Panels/SceneViewPickingPolicy.h` so Scene View computes explicit camera-block conditions, including active text input within Scene View bounds.
- [x] T010 [US1] Update `Project/Editor/Panels/SceneView.cpp` to feed text-input and modal block conditions into the interaction state machine and reset camera interaction when the blocked state is entered.
- [x] T011 [US1] Update `Project/Editor/Core/CameraController.cpp` so blocked-state entry releases camera cursor ownership, infinite wrap, and transient mouse-capture state exactly once.
- [x] T012 [US1] Run focused regression tests with `NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.SceneViewBlocksCameraInputDuringTextEntryEvenInsideView:SceneViewCameraInteractionStateMachineTests.*`.

**Checkpoint**: Text entry is stable and no longer competes with Scene View cursor ownership.

---

## Phase 4: User Story 2 - Predictable Navigation Mode Transitions (Priority: P2)

**Goal**: Fly, pan, and orbit keep their current behavior, but cursor changes only happen on state entry and exit.

**Independent Test**: Start and stop each navigation gesture independently and verify the corresponding state transition, cursor acquisition, and cursor release happen once per transition.

- [x] T013 [P] [US2] Add failing navigation-mode assertions in `Tests/Unit/SceneViewCameraInteractionStateMachineTests.cpp` for fly, pan, orbit, modifier-driven pan/orbit switching, and gesture release cleanup.
- [x] T014 [US2] Refactor `Project/Editor/Core/CameraController.cpp` so fly, pan, and orbit mode selection comes from the explicit interaction state instead of per-frame cursor-setting branches.
- [x] T015 [US2] Move camera cursor acquisition/release and infinite-wrap ownership transitions in `Project/Editor/Core/CameraController.cpp` to state entry/exit handling.
- [x] T016 [US2] Update `Project/Editor/Core/CameraController.cpp` so camera movement math still maps correctly to `Fly`, `Pan`, and `Orbit` execution modes without reintroducing per-frame cursor reassignment.
- [x] T017 [US2] Run focused navigation regression tests with `NullusUnitTests.exe --gtest_filter=SceneViewCameraInteractionStateMachineTests.*:CameraControllerInputTests.*`.

**Checkpoint**: Navigation behavior is preserved and cursor ownership becomes transition-driven instead of frame-driven.

---

## Phase 5: User Story 3 - Centralized Interaction Model For Contributors (Priority: P3)

**Goal**: Contributors can reason about Scene View interaction through one centralized model and extend it safely.

**Independent Test**: Review the helper and its tests to confirm state entry, exit, block, and cursor rules are centralized and no duplicate per-frame cursor writes remain in Scene View camera logic.

- [x] T018 [P] [US3] Add contributor-facing structure assertions in `Tests/Unit/SceneViewCameraInteractionStateMachineTests.cpp` for neutral, blocked, and active-state transition semantics that future modes must preserve.
- [x] T019 [US3] Remove or consolidate redundant cursor-control branches in `Project/Editor/Core/CameraController.cpp` so state transitions are the single source of truth for cursor ownership.
- [x] T020 [US3] Update `Project/Editor/Core/CameraController.h` and `Project/Editor/Panels/SceneView.cpp` to expose the new centralized interaction model clearly at the collaboration boundary between panel gating and camera execution.
- [x] T021 [US3] Self-review `Project/Editor/Core/SceneViewCameraInteractionStateMachine.*`, `Project/Editor/Core/CameraController.*`, and `Project/Editor/Panels/SceneView.*` for duplicated state rules or leftover frame-driven cursor writes.
- [x] T021a [US3] Preserve and restore the pre-interaction cursor shape for Scene View navigation and UI-managed infinite drag cursor ownership.
- [x] T021b [US3] Add regression coverage for UI infinite drag cursor leases so an unrenewed drag restores the captured cursor at frame end.

**Checkpoint**: The interaction model is centralized, testable, and maintainable for future contributor changes.

---

## Final Phase: Polish & Cross-Cutting Validation

- [x] T022 [P] Run the focused quickstart regression suite from `specs/040-scene-view-camera-state-machine/quickstart.md`.
- [x] T023 Run `NullusUnitTests.exe --gtest_filter=ReflectedPropertyDrawerTests.*:PanelWindowHookTests.*:CameraControllerInputTests.*:SceneViewCameraInteractionStateMachineTests.*`.
- [x] T024 Run `& 'F:/Microsoft Visual Studio/2022/MSBuild/Current/Bin/MSBuild.exe' 'Build/Tests/Unit/NullusUnitTests.vcxproj' /p:Configuration=Debug /p:Platform=x64 /m:1 /nologo /v:minimal`.
- [ ] T025 Validate the Editor build remains runnable and summarize manual Scene View cursor/state evidence in the implementation notes.

## Dependencies & Execution Order

- Phase 1 can start immediately.
- Phase 2 depends on Phase 1 and blocks all user stories.
- US1 depends on Phase 2 and is the MVP because it resolves the reported text-entry regression.
- US2 depends on the foundational helper and should follow US1 because it migrates the remaining navigation flows onto the same state model.
- US3 depends on US2 because centralization only makes sense after fly/pan/orbit are fully state-driven.
- Final validation depends on all selected user stories.

## Parallel Opportunities

- T002 can run in parallel with T001 once file names are agreed.
- T003 and T004 can run in parallel because they cover different test files.
- T008 and T013 are parallelizable test-authoring tasks in separate files once the foundational helper shape is known.
- T022 and T024 can run in parallel at the end because one is focused runtime/test validation and the other is build validation.

## Implementation Strategy

1. Establish a pure interaction-state helper with failing tests first.
2. Use User Story 1 to wire blocked/text-input behavior into the new model and eliminate the reported cursor regression.
3. Migrate fly/pan/orbit cursor and capture control onto state transitions without changing camera math semantics.
4. Clean up any leftover frame-driven cursor branches so the helper becomes the single source of truth.
5. Finish with focused automated validation and manual Scene View verification.
