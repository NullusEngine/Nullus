# Tasks: Fix DX12 MultiFramebuffer Barriers

**Input**: Design documents from `specs/031-fix-dx12-multiframebuffer-barriers/`
**Prerequisites**: `plan.md`, `spec.md`

## Phase 1: Setup

**Purpose**: Capture the failure scope and choose the smallest stable regression entrypoint.

- [X] T001 Inspect threaded RHI barrier and extraction flow in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T002 Inspect tracker barrier resolution in `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`

---

## Phase 2: User Story 1 - DX12 Deferred Frame Does Not Lose Device (Priority: P1)

**Goal**: Prevent stale MultiFramebuffer color/depth transitions after GBuffer resources have already been transitioned for reads.

**Independent Test**: Run the targeted threaded rendering lifecycle regression test and verify it catches redundant stale extraction barriers before the fix.

### Tests for User Story 1

- [X] T003 [US1] Add a failing regression test in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`

### Implementation for User Story 1

- [X] T004 [US1] Make derived texture visibility use sampled attachment end states in `Runtime/Rendering/Context/Driver.cpp`
- [X] T005 [US1] Re-run targeted threaded rendering lifecycle tests from `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`

---

## Phase 3: Polish & Review

**Purpose**: Validate and review the completed fix.

- [X] T006 Run relevant targeted validation commands for threaded rendering lifecycle tests
- [ ] T007 Run required plan-review quality gate for the code changes
- [ ] T008 Summarize validation evidence and any backend-specific limits

---

## Dependencies & Execution Order

- Phase 1 must complete before writing the regression test.
- T003 must fail before T004 production code changes.
- T005-T008 depend on T004.

## Implementation Strategy

Implement the P1 story as a single TDD slice: write the failing test, make the smallest code change, then verify targeted tests and review.
