# Tasks: DebugDrawPass Line Batching

**Input**: Design documents from `specs/033-debugdraw-batching/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required by FR-007 and TDD workflow. Tests must be written and observed failing before production implementation.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Confirm existing evidence and prepare feature artifacts.

- [x] T001 Create feature spec bundle in `specs/033-debugdraw-batching/`
- [x] T002 Document batching decisions in `specs/033-debugdraw-batching/research.md`
- [x] T003 Document focused validation commands in `specs/033-debugdraw-batching/quickstart.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add test hooks needed to validate batching without a GPU backend.

- [x] T004 Add a protected line-batch render hook declaration in `Runtime/Rendering/Debug/DebugDrawPass.h`
- [x] T005 Add failing tests for compatible line grouping in `Tests/Unit/DebugDrawPassTests.cpp`
- [x] T006 Add failing tests for incompatible line state splitting in `Tests/Unit/DebugDrawPassTests.cpp`

---

## Phase 3: User Story 1 - Select Objects Without Debug Draw Stalls (Priority: P1) MVP

**Goal**: Consecutive compatible debug lines render through one grouped line submission.

**Independent Test**: Submit multiple consecutive same-state visible lines and verify one batch containing all line segments.

### Tests for User Story 1

- [x] T007 [US1] Run `NullusUnitTests.exe --gtest_filter=DebugDrawPassTests.*` and confirm the new compatible-line grouping test fails for missing batching behavior

### Implementation for User Story 1

- [x] T008 [US1] Implement visible line collection and grouping in `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [x] T009 [US1] Implement transient line mesh rendering for grouped lines in `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [x] T010 [US1] Add vertex-position shader mode for batches in `App/Assets/Engine/Shaders/DebugPrimitive.hlsl`

**Checkpoint**: Compatible lines are submitted as one grouped line draw in tests.

---

## Phase 4: User Story 2 - Preserve Visual State Boundaries (Priority: P2)

**Goal**: Lines with incompatible state are split into separate grouped submissions.

**Independent Test**: Submit lines with different color, depth mode, and line width and verify separate batches.

### Tests for User Story 2

- [x] T011 [US2] Run focused DebugDrawPass tests and confirm state-splitting expectations pass after implementation

### Implementation for User Story 2

- [x] T012 [US2] Ensure the line batch key compares color, depth mode, and line width in `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [x] T013 [US2] Ensure batch rendering applies depth and line width state in `Runtime/Rendering/Debug/DebugDrawPass.cpp`

**Checkpoint**: Different debug line states do not merge.

---

## Phase 5: User Story 3 - Keep Existing Debug Primitive Behavior (Priority: P3)

**Goal**: Points, triangles, visibility filtering, and one-frame lifetime behavior remain stable.

**Independent Test**: Existing DebugDrawPass tests still pass with updated line-batch expectations.

### Tests for User Story 3

- [x] T014 [US3] Update existing pass tests in `Tests/Unit/DebugDrawPassTests.cpp` to record both per-primitive and line-batch paths
- [x] T015 [US3] Run focused DebugDraw tests from `specs/033-debugdraw-batching/quickstart.md`

### Implementation for User Story 3

- [x] T016 [US3] Keep point and triangle rendering on the existing `RenderPrimitive` path in `Runtime/Rendering/Debug/DebugDrawPass.cpp`
- [x] T017 [US3] Reset shader/material mode correctly between batched and per-primitive rendering in `Runtime/Rendering/Debug/DebugDrawPass.cpp`

**Checkpoint**: Existing debug primitive behavior remains covered by focused tests.

---

## Final Phase: Polish & Cross-Cutting Concerns

**Purpose**: Validate and review the complete change.

- [x] T018 Run `cmake --build .\Build --target NullusUnitTests --config Release`
- [x] T019 Run `.\Build\bin\Release\NullusUnitTests.exe --gtest_filter=DebugDrawPassTests.*:DebugDrawGeometryTests.*:DebugDrawTypesTests.*`
- [x] T020 Run `git diff --check`
- [x] T021 Run required plan-review quality gate before completion

---

## Dependencies & Execution Order

- Phase 1 is complete before implementation starts.
- Phase 2 test work precedes production code.
- User Story 1 is the MVP and must pass before state-splitting polish.
- User Story 2 depends on the batch key introduced for User Story 1.
- User Story 3 verifies unchanged behavior after line batching.
- Final validation depends on all selected user stories being complete.

## Parallel Opportunities

- `DebugPrimitive.hlsl` shader mode can be reviewed independently after the C++ batch API is known.
- Documentation artifacts can be updated independently of tests and code.

## Implementation Strategy

1. Add tests and observe the expected failures.
2. Implement the smallest line-batching path that passes the tests.
3. Preserve existing primitive behavior and shader compatibility.
4. Run focused validation and quality review before reporting completion.
