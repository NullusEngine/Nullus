# Tasks: FrameInfo Render View Snapshot

**Input**: Design documents from `/specs/fix-frameinfo-render-view-snapshot/`  
**Prerequisites**: plan.md, spec.md

**Tests**: Tests are required because this is a behavior-changing editor/rendering/RHI fix.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Phase 1: Setup

- [x] T001 Review existing FrameInfo, AView, RendererStats, Driver telemetry, and RHI frame begin paths.
- [x] T002 Identify stable unit/contract test entrypoints for editor panel refresh, renderer stats, and RHI fail-closed behavior.

---

## Phase 2: Foundational

- [x] T003 [P] Add/adjust FrameInfo panel tests in `Tests/Unit/PanelWindowHookTests.cpp`.
- [x] T004 [P] Add/adjust renderer telemetry tests in `Tests/Unit/RendererStatsTests.cpp`.
- [x] T005 [P] Add/adjust RHI fence fail-closed tests in `Tests/Unit/RenderFrameworkContractTests.cpp`.

---

## Phase 3: User Story 1 - Open FrameInfo Without Freezing The Editor (Priority: P1)

**Goal**: FrameInfo refreshes from cached view snapshots and does not block on live threaded telemetry.

**Independent Test**: `PanelWindowHookTests.FrameInfoPanelRefreshReturnsWhenLifecycleTelemetryIsBusy`

- [x] T006 [US1] Add `AView` last-rendered `FrameInfo` snapshot in `Project/Editor/Panels/AView.h`.
- [x] T007 [US1] Publish the snapshot after successful render completion in `Project/Editor/Panels/AView.cpp`.
- [x] T008 [US1] Update `FrameInfo::RefreshForView()` to read the `AView` snapshot in `Project/Editor/Panels/FrameInfo.cpp`.
- [x] T009 [US1] Make renderer telemetry refresh nonblocking in `Runtime/Rendering/Core/CompositeRenderer.cpp` and `Runtime/Rendering/Context/Driver.cpp`.

---

## Phase 4: User Story 2 - Show Render View Metrics Only (Priority: P2)

**Goal**: FrameInfo shows render viewport stats only and excludes editor UI panel draw metrics.

**Independent Test**: `PanelWindowHookTests.FrameInfoPanelExcludesEditorUiPanelDrawMetrics`

- [x] T010 [US2] Remove FrameInfo target panel draw-time display paths in `Project/Editor/Panels/FrameInfo.*`.
- [x] T011 [US2] Keep FrameInfo panel independent and dockable in `Project/Editor/Core/Editor.cpp`.
- [x] T012 [US2] Use focused/open render views to select the FrameInfo target in `Project/Editor/Core/Editor.cpp`.

---

## Phase 5: User Story 3 - Keep RHI/Threaded Rendering Failures Fail-Closed (Priority: P3)

**Goal**: Fence and drain failures do not reset/reuse resources or stall panel refresh paths.

**Independent Test**: `RenderFrameworkContractTests.StandaloneFrameDoesNotReuseResourcesWhenFenceWaitFails`, `RenderFrameworkContractTests.ThreadedRhiFrameDoesNotReuseResourcesWhenFenceWaitFails`

- [x] T013 [US3] Add driver telemetry mutex and nonblocking try telemetry path in `Runtime/Rendering/Context/Driver.cpp` and `DriverInternal.h`.
- [x] T014 [US3] Return actionable drain results through driver access in `Runtime/Rendering/Context/DriverAccess.h`.
- [x] T015 [US3] Fail close standalone UI, standalone renderer, and threaded RHI begin paths in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`.
- [x] T016 [US3] Propagate standalone begin failure through `RenderThreadCoordinator` and `ABaseRenderer`.
- [x] T017 [US3] Update AView resize and post-render drain paths to use `TryDrainThreadedRendering()`.
- [x] T018 [US3] Clear stale UI wait semaphores every editor UI frame in `Project/Editor/Core/Editor.cpp`.
- [x] T018a [US3] Guard UI-to-present synchronization against zero-valued UI completion fences in `Runtime/Rendering/Context/Driver.cpp`, `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp`.
- [x] T018b [US3] Preserve monotonically increasing DX12 semaphore fence values across frame resets in `Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.cpp`.
- [x] T018c [US3] Add UI standalone frame pending priority so worker RHI submissions yield while the editor swapchain UI frame is waiting in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`.

---

## Phase 6: Validation & Review

- [x] T019 Build `Build\Tests\Unit\NullusUnitTests.vcxproj` with MSBuild Debug x64.
- [x] T020 Run `PanelWindowHookTests.*`.
- [x] T021 Run `RendererStatsTests.*`, `CompositeRendererExplicitDrawOrderTests.*`, selected threaded lifecycle tests, and `RenderFrameworkContractTests.*`.
- [x] T021a Run zero-valued UI composition signal regression coverage in `ThreadedRenderingLifecycleTests.StandaloneExplicitFramePresentIgnoresZeroUiCompositionSignalValue`.
- [x] T021b Run DX12 backend contract coverage in `RenderFrameworkContractTests.DX12QueueRejectsZeroUiFenceWaitBeforePresent`.
- [x] T021c Run DX12 semaphore reset monotonicity coverage in `RenderFrameworkContractTests.DX12SemaphoreResetDoesNotLowerFenceToZero`.
- [x] T021d Run UI present starvation coverage in `ThreadedRenderingLifecycleTests.ThreadedUiRenderWaitsForBriefRhiSubmissionLockContention` and `ThreadedRenderingLifecycleTests.ThreadedRhiWorkerYieldsWhileUiStandaloneFrameIsPending`.
- [x] T021e Run DX12 editor smoke with `--editor-validation-open-frame-info --editor-log-render-draw-path`; verify repeated UI submits/presents and no DX12 device/fence/texture errors.
- [x] T022 Run `git diff --check`.
- [x] T023 Verify no generated-file diffs under `Runtime/*/Gen` or `Project/*/Gen`.
- [x] T024 Complete plan-review and multi-agent quality gate; resolve all P0/P1 findings.

## Dependencies & Execution Order

- T001-T002 precede tests and implementation.
- T003-T005 define regression coverage before or alongside implementation.
- US1 is the MVP and must complete before final FrameInfo behavior is considered fixed.
- US2 depends on US1's snapshot source but is independently verifiable through panel text.
- US3 can be validated independently through RHI contract tests.
- T019-T024 are final gates.

## Notes

- No generated files are intentionally edited.
- Manual editor runtime validation was performed on DX12 through the startup validation path; RenderDoc capture was not required because the failure mode was present starvation and synchronization-log evidenced.
