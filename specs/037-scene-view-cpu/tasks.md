# Tasks: Editor Scene View CPU Frame-Time Optimization

**Input**: Design documents from `specs/037-scene-view-cpu/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. Write each regression test first and verify it fails before implementation.

## Phase 1: Setup

**Purpose**: Confirm baseline and locate executable entrypoints.

- [ ] T001 Record current git status and active branch before implementation
- [ ] T002 Confirm `NullusUnitTests` executable or build target location for focused filters in `specs/037-scene-view-cpu/quickstart.md`

---

## Phase 2: Foundational

**Purpose**: Establish tests before production edits.

- [ ] T003 [P] Add failing stable-size GBuffer reuse regression test in `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp`
- [ ] T004 [P] Add failing incomplete trace event export regression test in `Tests/Unit/ProfilerDestinationTests.cpp`
- [ ] T005 Run focused tests and record the expected failing assertions for T003 and T004

**Checkpoint**: Regression tests fail for the intended missing behavior.

---

## Phase 3: User Story 1 - Stable Scene View Rendering Is Smoother (Priority: P1)

**Goal**: Avoid redundant deferred Scene View resource preparation when dimensions are unchanged.

**Independent Test**: The renderer test proves stable-size calls reuse prepared GBuffer wrappers, while resize still refreshes them.

### Tests for User Story 1

- [ ] T006 [US1] Verify `DeferredSceneRendererMaterialCacheTests` stable-size regression fails before implementation

### Implementation for User Story 1

- [ ] T007 [US1] Expose minimal renderer test access for GBuffer target preparation in `Runtime/Engine/Rendering/DeferredSceneRenderer.h`
- [ ] T008 [US1] Implement stable-size no-op behavior in `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- [ ] T009 [US1] Preserve resize and zero-size invalidation behavior in `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
- [ ] T010 [US1] Run focused renderer tests and update validation notes in `specs/037-scene-view-cpu/quickstart.md`

**Checkpoint**: User Story 1 is independently testable and passing.

---

## Phase 4: User Story 2 - Frame-Time Evidence Is Trustworthy (Priority: P2)

**Goal**: Ensure trace export emits only completed non-negative duration events.

**Independent Test**: The profiler export test proves incomplete events are skipped and exported durations are valid.

### Tests for User Story 2

- [ ] T011 [US2] Verify `ProfilerDestinationTests` trace export regression fails before implementation

### Implementation for User Story 2

- [ ] T012 [US2] Extract or reuse trace export event eligibility logic in `Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp` or an included helper
- [ ] T013 [US2] Filter incomplete or non-positive tick events during trace export in `Runtime/UI/ImGuiExtensions/TimelineProfiler/ProfilerWindow.cpp`
- [ ] T014 [US2] Run focused profiler tests and update validation notes in `specs/037-scene-view-cpu/quickstart.md`

**Checkpoint**: User Story 2 is independently testable and passing.

---

## Phase 5: Polish & Review

**Purpose**: Validate, self-review, and run required quality gates.

- [ ] T015 Run combined focused test filter from `specs/037-scene-view-cpu/quickstart.md`
- [ ] T016 Inspect diff for generated-file edits, backend overclaims, and unrelated churn
- [ ] T017 Run `/plan-review` quality gate until repository thresholds are satisfied
- [ ] T018 Summarize validation evidence and remaining runtime trace limitations

---

## Dependencies & Execution Order

- Phase 1 must complete before tests are added.
- Phase 2 must complete before production code changes.
- User Story 1 and User Story 2 can be implemented independently after Phase 2.
- Phase 5 depends on all implemented stories.

## Parallel Opportunities

- T003 and T004 can be written in parallel because they affect different test files.
- US1 implementation files and US2 implementation files are independent after their tests fail.

## Implementation Strategy

1. Complete Setup and Foundational phases.
2. Deliver US1 first because it targets the dominant Scene View CPU cost.
3. Deliver US2 second because it improves future performance evidence quality.
4. Run focused tests after each story, then combined validation and plan-review.
