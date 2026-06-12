# Tasks: Fix Profiler DX12 Hang

**Input**: Design documents from `specs/048-fix-profiler-dx12-hang/`
**Prerequisites**: `plan.md`, `spec.md`
**Tests**: Required because this is a DX12 synchronization bug fix.
**Organization**: Tasks are grouped by independently testable user stories.

## Phase 1: Setup (Shared Investigation)

**Purpose**: Pin down the observed failure and affected code.

- [x] T001 Inspect the reported DX12 editor log in `TestProject/Logs/2026-06-12_20-21-28.log`
- [x] T002 Inspect TimelineProfiler GPU submission code in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [x] T003 Inspect existing DX12 queue transaction patterns in `Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp`, `DX12UIBridge.cpp`, upload, and readback paths

---

## Phase 2: Foundational (Regression Harness)

**Purpose**: Add a failing contract before production changes.

- [x] T004 [P] Add a focused regression test in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp` proving TimelineProfiler internal DX12 resolve operations use the shared queue lock
- [x] T005 Run the focused TimelineProfiler regression and confirm it fails before the production fix (`TimelineProfilerGpuLifecycleTests.GpuProfilerResolveQueueOperationsUseSharedDx12QueueLock` failed while `Profiler.cpp` resolve/drain queue operations were not using the shared DX12 queue lock)

---

## Phase 3: User Story 1 - Open Profiler Without Losing DX12 Device (Priority: P1) MVP

**Goal**: TimelineProfiler internal GPU resolve work no longer races normal graphics/UI queue submissions.

**Independent Test**: Focused TimelineProfiler GPU lifecycle test passes and DX12 editor Profiler opening no longer logs the reported `DEVICE_HUNG` path.

### Tests for User Story 1

- [x] T006 [US1] Re-run `TimelineProfilerGpuLifecycleTests.GpuProfilerResolveQueueOperationsUseSharedDx12QueueLock` and confirm it passes

### Implementation for User Story 1

- [x] T007 [US1] Include the existing DX12 queue synchronization helper in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [x] T008 [US1] Wrap `GPUProfiler::QueryHeap::Resolve()` queue execute+signal with `ScopedDX12QueueLock` in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`
- [x] T009 [US1] Wrap `GPUProfiler::QueryHeap::DrainQueue()` drain fence signal with `ScopedDX12QueueLock` in `Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp`

**Checkpoint**: Profiler resolve work uses the shared native queue lock.

---

## Phase 3A: Root Cause Correction - DX12 UI Backbuffer Reuse (Priority: P1)

**Goal**: Preserve the allocator-pool optimization from `047-dx12-ui-present-wait` without bypassing per-backbuffer UI reuse safety.

**Independent Test**: `ProfilerDestinationTest.DX12UiBridgeWaitsForBackbufferAndAllocatorReuse` proves `DX12UIBridge` waits for backbuffer reuse before allocator reset/backbuffer barriers while still retaining allocator reuse waiting.

### Tests for Root Cause Correction

- [x] T009A [US1] Replace the old allocator-only source contract with a regression requiring both backbuffer and allocator reuse waits
- [x] T009B [US1] Confirm the UI bridge regression fails on the current code before restoring the backbuffer wait (`ProfilerDestinationTest.DX12UiBridgeWaitsForBackbufferAndAllocatorReuse` failed while `DX12UIBridge::WaitForBackbufferReuse` was absent)

### Implementation for Root Cause Correction

- [x] T009C [US1] Restore `DX12UIBridge::WaitForBackbufferReuse` before allocator reuse, command allocator reset, and backbuffer barriers
- [x] T009D [US1] Keep the allocator-pool reuse path from `047-dx12-ui-present-wait` instead of reverting the whole optimization

**Checkpoint**: UI bridge now gates on both current-backbuffer safety and allocator safety.

---

## Phase 3B: Review Hardening - Queue Lifetime, Metadata Ordering, Readback State (Priority: P1)

**Goal**: Close multi-agent review findings in the TimelineProfiler/DX12 queue hardening path.

**Independent Test**: Focused source-contract tests fail before the hardening changes and pass after queue state lifetime, profiler metadata ordering, and readback resource state are corrected.

### Tests for Review Hardening

- [x] T009E [US1] Add regressions for process-lifetime queue synchronization state, order-preserving profiler metadata publication, lock-scope coverage for resolve execute+signal, and readback `COPY_DEST` state
- [x] T009F [US1] Confirm those regressions fail before the hardening fix

### Implementation for Review Hardening

- [x] T009G [US1] Make exported DX12 queue synchronization state process-lifetime so `ScopedDX12QueueLock` cannot observe a destroyed mutex during wrapper teardown/reinit
- [x] T009H [US1] Reserve profiler submit order while holding the native queue lock and publish TimelineProfiler metadata outside that lock in the same queue order
- [x] T009I [US1] Create TimelineProfiler readback resources in `D3D12_RESOURCE_STATE_COPY_DEST`
- [x] T009J [US1] Ensure queued command-list metadata is still published before returning post-`ExecuteCommandLists` device-lost or signal-failure results
- [x] T009K [US1] Quarantine TimelineProfiler resolve resources and stop GPU profiler advancement when resolve `ExecuteCommandLists` succeeds but the following resolve-fence `Signal` fails

**Checkpoint**: Multi-agent P0/P1 findings for queue-lock lifetime, profiler metadata reordering, post-submit failure metadata publication, and readback resource state are addressed.

---

## Phase 4: User Story 2 - Keep Timeline GPU Data Available (Priority: P2)

**Goal**: GPU profiling remains functional after the safety fix.

**Independent Test**: Existing non-empty GPU frame test still records a valid GPU event.

### Tests for User Story 2

- [x] T010 [US2] Run `TimelineProfilerGpuLifecycleTests.NonEmptyGpuFramePublishesResolvedEvent`
- [x] T011 [US2] Run the full `TimelineProfilerGpuLifecycleTests.*` focused suite

### Implementation for User Story 2

- [x] T012 [US2] Existing GPU event publication tests passed; no GPU event publication adjustment needed

---

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Validate and review.

- [x] T013 Run `cmake --build Build --target NullusUnitTests --config Debug -- /m:1`
- [x] T014 Run focused regression suite after final code changes (`Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ProfilerDestinationTest.DX12UiBridgeWaitsForBackbufferAndAllocatorReuse:DX12UIFrameFenceTrackerTests.*:TimelineProfilerGpuLifecycleTests.*:EditorLaunchArgsTests.ParsesEditorValidationViewAndCameraInputDiagnostics:PanelWindowHookTests.EditorStartupValidationCanOpenProfilerPanel`; 30/30 passed)
- [x] T015 Run DX12 editor/runtime verification for opening the Profiler panel after final code changes (`App\Win64_Release_Runtime_Shared\Editor.exe --backend dx12 --no-renderdoc --editor-validation-open-profiler --editor-log-render-draw-path D:\VSProject\Nullus\TestProject\TestProject.nullus`; log `TestProject/Logs/2026-06-13_00-05-58.log` opened Profiler once, logged 3740 UI command-list submissions and 3798 successful native queue submits, and had 0 matching device-hung/quarantine/create-texture-failed/picking-readback-failed/resolve-signal-failed/resolve-signal-failed after the final quarantine patch)
- [x] T016 Run `git diff --check` (passed with LF-to-CRLF warnings only)
- [x] T017 Run required `/plan-review` quality gate per repository workflow (multi-agent final review completed; no P0/P1 findings remained)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Complete before regression/implementation.
- **Foundational (Phase 2)**: Complete before production code changes.
- **User Story 1 (Phase 3)**: Blocks meaningful runtime validation.
- **Review Hardening (Phase 3B)**: Complete before final TimelineProfiler/runtime validation.
- **User Story 2 (Phase 4)**: Verifies the safety fix did not disable GPU profiling.
- **Polish (Phase 5)**: Final evidence and review gate.

### Parallel Opportunities

- T004 can be reviewed independently from documentation artifacts.
- T009E source-contract additions can be reviewed independently from the runtime smoke hook.
- T010 and T011 can run after T007-T009I once the test binary is rebuilt.

## Implementation Strategy

1. Confirm the focused regression fails.
2. Restore the DX12 UI bridge backbuffer wait while keeping allocator-pool reuse.
3. Patch TimelineProfiler's direct DX12 queue operations to use shared queue synchronization, stable queue-lock lifetime, ordered metadata publication, and valid readback state.
4. Rebuild and run the TimelineProfiler GPU lifecycle tests.
5. Run DX12 editor validation if the local build/runtime is available.
6. Complete the required plan-review gate.
