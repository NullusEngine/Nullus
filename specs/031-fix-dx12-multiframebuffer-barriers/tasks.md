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

**Independent Test**: Run the targeted MultiFramebuffer, frame-graph dependency, DX12 subresource, resource-state-tracker, and threaded visibility regression tests.

### Tests for User Story 1

- [X] T003 [US1] Add regression coverage for active MultiFramebuffer attachments in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T004 [US1] Add regression coverage for overlapping texture dependency ranges in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T005 [US1] Add resource state tracker regression coverage for overlapping/fragmented texture ranges in `Tests/Unit/UploadContextTests.cpp`
- [X] T006 [US1] Add DX12 subresource range regression coverage in `Tests/Unit/DX12TextureViewUtilsTests.cpp`

### Implementation for User Story 1

- [X] T007 [US1] Capture active recorded pass attachment views in `Runtime/Rendering/Core/ABaseRenderer.cpp`
- [X] T008 [US1] Make derived texture visibility use tracker-resolved before states in `Runtime/Rendering/Context/Driver.cpp`
- [X] T009 [US1] Make threaded dependency visibility use overlapping texture ranges in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T010 [US1] Make frame-graph texture dependencies overlap-aware in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- [X] T011 [US1] Make DX12 texture barrier range expansion align with half-default range semantics in `Runtime/Rendering/RHI/Backends/DX12/DX12TextureViewUtils.cpp`
- [X] T012 [US1] Make `ResourceStateTracker` subresource-aware for overlapping texture states in `Runtime/Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.cpp`

---

## Phase 3: Polish & Review

**Purpose**: Validate and review the completed fix.

- [X] T013 Build `NullusUnitTests`
- [X] T014 Run targeted validation commands for DX12 range utilities, MultiFramebuffer transitions, ResourceStateTracker, and threaded visibility tests
- [X] T015 Run required plan-review quality gate for the code changes
- [X] T016 Summarize validation evidence and any backend-specific limits

---

## Dependencies & Execution Order

- Phase 1 must complete before writing the regression test.
- T003-T006 must fail or expose missing coverage before T007-T012 production code changes.
- T013-T016 depend on T007-T012.

## Implementation Strategy

Implement the P1 story as focused TDD slices: add coverage for the active attachment handoff, dependency range matching, DX12 range normalization, and tracker subresource state ownership; then verify targeted tests and review.
