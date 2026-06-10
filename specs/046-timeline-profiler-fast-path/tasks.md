# Tasks: TimelineProfiler GPU Fast Path

**Input**: Design documents from `/specs/046-timeline-profiler-fast-path/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. This change follows TDD because it changes Runtime profiler behavior.

**Organization**: Tasks are grouped by user story so the empty-frame optimization can be validated independently before preserving non-empty GPU event behavior and adding visibility.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish current state and locate the exact build/test entrypoints.

- [ ] T001 Confirm current feature branch and clean working tree with `git status --short`
- [ ] T002 [P] Inspect existing TimelineProfiler lifecycle helpers in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h`
- [ ] T003 [P] Inspect existing GPU profiler tick path in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [ ] T004 [P] Inspect existing lifecycle tests in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define the minimal lifecycle decision surface before changing DX12 command submission.

- [ ] T005 Add RED tests for empty/non-empty GPU readback decision helpers in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp`
- [ ] T006 Run targeted `TimelineProfilerGpuLifecycleTests` and confirm the new tests fail because the helper behavior is missing
- [ ] T007 Implement minimal constexpr helper decisions in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h`
- [ ] T008 Run targeted `TimelineProfilerGpuLifecycleTests` and confirm the helper tests pass

**Checkpoint**: Lifecycle decisions are covered before production tick behavior changes.

---

## Phase 3: User Story 1 - Empty GPU Frames Stay Lightweight (Priority: P1) 🎯 MVP

**Goal**: Skip GPU readback submission and waits when a GPU profiler frame contains no query work.

**Independent Test**: Empty GPU frames are eligible for no-op completion and do not require submitted readback state.

### Tests for User Story 1

- [ ] T009 [US1] Add RED tests for skipped-frame completion state in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp`
- [ ] T010 [US1] Run targeted `TimelineProfilerGpuLifecycleTests` and confirm skipped-frame completion tests fail before implementation

### Implementation for User Story 1

- [ ] T011 [US1] Track submitted readback frame slots in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h`
- [ ] T012 [US1] Mark empty GPU frames complete without calling query heap resolve/reset in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [ ] T013 [US1] Run targeted `TimelineProfilerGpuLifecycleTests` and confirm US1 tests pass

**Checkpoint**: User Story 1 is independently functional and testable.

---

## Phase 4: User Story 2 - GPU Timeline Data Remains Correct (Priority: P2)

**Goal**: Preserve normal GPU event publication for frames that record query pairs.

**Independent Test**: Non-empty frames still require readback submission, and mixed empty/non-empty ordering remains safe.

### Tests for User Story 2

- [ ] T014 [US2] Add RED tests for submitted-frame completion and mixed empty/non-empty ordering in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp`
- [ ] T015 [US2] Run targeted `TimelineProfilerGpuLifecycleTests` and confirm mixed-order tests fail before implementation

### Implementation for User Story 2

- [ ] T016 [US2] Preserve submitted-frame fence checks while treating unsubmitted frame slots as complete in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [ ] T017 [US2] Run targeted `TimelineProfilerGpuLifecycleTests` and confirm US1 and US2 tests pass together

**Checkpoint**: Empty-frame skipping does not block or invalidate non-empty GPU events.

---

## Phase 5: User Story 3 - Profiling Overhead Is Visible When Needed (Priority: P3)

**Goal**: Make profiler frame maintenance identifiable in CPU traces without adding noisy nested scopes.

**Independent Test**: TimelineProfiler frame maintenance has a named CPU scope when recording is enabled.

### Tests for User Story 3

- [ ] T018 [US3] Add RED source-structure or destination test for TimelineProfiler frame maintenance scope in `Tests/Unit/PanelWindowHookTests.cpp` or `Tests/Unit/ProfilerDestinationTests.cpp`
- [ ] T019 [US3] Run the targeted test and confirm it fails before implementation

### Implementation for User Story 3

- [ ] T020 [US3] Add a concise `TimelineProfiler::TickFrame` CPU scope around frame maintenance in `Runtime/UI/Profiling/TimelineProfilerSink.cpp`
- [ ] T021 [US3] Run the targeted test and confirm US3 passes

**Checkpoint**: Future captures can identify profiler maintenance separately from application frame work.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Validate, review, and record evidence.

- [ ] T022 Run full `NullusUnitTests` or the closest available configured unit-test command
- [ ] T023 [P] Re-run trace gap analysis on `TestProject/Logs/trace.json` or a fresh exported trace and record before/after evidence
- [ ] T024 [P] Inspect `git diff --check` and `git diff --stat`
- [ ] T025 Update `specs/046-timeline-profiler-fast-path/quickstart.md` with exact validation evidence if the runtime trace path differs from the initial plan
- [ ] T026 Run required `/plan-review` quality gate for the completed code changes, including the deeper audit loop required by AGENTS.md

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup completion; blocks user story implementation.
- **User Story 1 (Phase 3)**: Depends on Foundational completion.
- **User Story 2 (Phase 4)**: Depends on User Story 1 because it validates preservation after the empty-frame path exists.
- **User Story 3 (Phase 5)**: Depends on Foundational completion and can run after US1/US2 to avoid obscuring behavior changes.
- **Polish (Phase 6)**: Depends on desired user stories being complete.

### User Story Dependencies

- **User Story 1 (P1)**: MVP and first implementation target.
- **User Story 2 (P2)**: Must validate after US1 because it protects non-empty GPU behavior.
- **User Story 3 (P3)**: Adds observability and can be omitted only if the MVP must be narrowed.

### Within Each User Story

- Tests MUST be written and observed failing before production code changes.
- Helper decisions before DX12 command submission changes.
- Empty-frame behavior before mixed empty/non-empty behavior.
- Runtime evidence after automated tests pass.

### Parallel Opportunities

- T002, T003, and T004 can run in parallel.
- T023 and T024 can run in parallel after tests pass.
- No implementation tasks touching `Profiler.cpp` should run in parallel because they share lifecycle state.

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Setup and Foundational helper tests.
2. Implement US1 empty-frame skip.
3. Validate targeted tests.
4. Stop and inspect diff before preserving non-empty behavior.

### Incremental Delivery

1. Add lifecycle helper tests and helpers.
2. Add empty-frame skip and tests.
3. Add mixed/non-empty preservation tests and implementation.
4. Add trace visibility scope.
5. Run unit tests, trace validation, and plan-review.

### Review Notes

- This touches GPU synchronization/fence behavior, so final review must treat GPU correctness as high risk.
- Any backend support claim must be limited to the backend explicitly validated.
- Do not hand-edit generated files under `Runtime/*/Gen/`.
