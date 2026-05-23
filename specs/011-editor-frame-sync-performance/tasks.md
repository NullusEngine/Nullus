# Tasks: Editor Frame Sync Performance

**Input**: Design documents from `/specs/011-editor-frame-sync-performance/`  
**Prerequisites**: plan.md, spec.md

## Phase 1: Setup

**Purpose**: Establish the tracked scope and test surface.

- [x] T001 Confirm current branch and dirty worktree before editing with `git status --short --branch`
- [x] T002 [P] Keep `specs/011-editor-frame-sync-performance/spec.md` and `specs/011-editor-frame-sync-performance/plan.md` aligned with the implemented scope

---

## Phase 2: Foundational

**Purpose**: Add tests for synchronization semantics before production changes.

- [x] T003 [P] Add failing tests for per-backbuffer UI fence reuse in `Tests/Unit/DX12UIFrameFenceTrackerTests.cpp`
- [x] T004 Add failing test coverage for UI present fence value propagation in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`

---

## Phase 3: User Story 1 - Remove Per-Frame DX12 UI CPU Drain (Priority: P1) MVP

**Goal**: Replace unconditional CPU waits in steady-state DX12 UI rendering with per-backbuffer waits and value-based present synchronization.

**Independent Test**: `DX12UIFrameFenceTrackerTests` and `ThreadedRenderingLifecycleTests` pass; `DX12UIBridge.cpp` no longer waits immediately after each UI submission.

- [x] T005 [US1] Implement `DX12UIFrameFenceTracker` in `Runtime/Rendering/RHI/Backends/DX12/DX12UIFrameFenceTracker.h`
- [x] T006 [US1] Keep `DX12UIFrameFenceTracker` header-only so existing generated Visual Studio projects do not miss a new source file
- [x] T007 [US1] Add `uiSignalValue` to `Runtime/Rendering/RHI/Core/RHISwapchain.h`
- [x] T008 [US1] Add UI signal value accessors to `Runtime/Rendering/RHI/Utils/RHIUIBridge.h`
- [x] T009 [US1] Propagate UI signal value through `Runtime/UI/UIManager.h` and `Runtime/UI/UIManager.cpp`
- [x] T010 [US1] Propagate UI signal value through `Runtime/Rendering/Context/DriverAccess.h`, `Runtime/Rendering/Context/DriverInternal.h`, and `Runtime/Rendering/Context/Driver.cpp`
- [x] T011 [US1] Pass UI signal value into present from `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [x] T012 [US1] Use per-backbuffer wait-before-reset and non-blocking submit in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [x] T013 [US1] Wait on `RHIPresentDesc::uiSignalValue` in `Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp`
- [x] T014 [US1] Update editor handoff in `Project/Editor/Core/Editor.cpp`

---

## Phase 4: User Story 2 - Preserve Resize And Shutdown Safety (Priority: P2)

**Goal**: Keep explicit drains where resources are released or the bridge shuts down.

**Independent Test**: Unit tests pass and editor build succeeds; code review confirms release paths still drain outstanding UI work.

- [x] T015 [US2] Reset per-backbuffer tracker state during swapchain UI resource release in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [x] T016 [US2] Verify shutdown and resize paths still use explicit GPU drains in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`

---

## Phase 5: User Story 3 - Isolate Remaining SceneView Sync Stalls (Priority: P3)

**Goal**: After the DX12 UI bridge repair, remove the remaining retirement-aware SceneView drain for texture-only consumers while keeping immediate readback paths safe.

**Independent Test**: Runtime frame-time evidence identifies SceneView offscreen synchronization as the remaining bottleneck; policy and picking lifecycle tests pass.

- [x] T017 [US3] Re-measure DX12 editor frame time after UI bridge fix with `App/Win64_Debug_Runtime_Static/Editor.exe --backend dx12 D:/VSProject/Nullus/TestProject/TestProject.nullus`
- [x] T018 [US3] Add policy tests in `Tests/Unit/PanelWindowHookTests.cpp` before modifying SceneView drain behavior
- [x] T023 [US3] Add delayed picking readback lifecycle tests in `Tests/Unit/PickingReadbackLifecycleTests.cpp`
- [x] T024 [US3] Gate post-render view drains on immediate readback requirements in `Project/Editor/Panels/AView.cpp`
- [x] T025 [US3] Move threaded picking readback to a delayed readable-frame lifecycle in `Project/Editor/Rendering/PickingRenderPass.cpp`
- [x] T026 [US3] Re-measure the post-UI-bridge DX12 editor frame path and identify UI composition offscreen draining as the remaining 30 FPS sync bottleneck
- [x] T027 [US3] Add UI/offscreen lifecycle coverage in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` so UI composition can begin with only offscreen threaded frames in flight but remains rejected for swapchain frames
- [x] T028 [US3] Remove the UI composition boundary drain of offscreen-only threaded work in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [x] T031 [US3] Fix startup hang after offscreen-drain removal by making standalone UI frame setup fail fast when threaded RHI submission owns the shared frame context
- [x] T032 [US3] Add regression coverage that UI render skips without waiting/resetting frame-context resources while RHI submission ownership is held

---

## Phase 5b: User Story 4 - Profile Empty Scene Without DX12 Validation Overhead (Priority: P3)

**Goal**: Add an explicit editor performance mode so follow-up empty-scene FPS profiling can run without DX12 Debug Layer, GPU-based validation, or DRED overhead while normal debug launches keep validation enabled.

**Independent Test**: `EditorLaunchArgsTests` pass and a DX12 editor performance-mode smoke run stays responsive without validation enablement logs.

- [x] T034 [US4] Add failing Editor launch-argument tests for default debug-validation behavior and performance-mode opt-out in `Tests/Unit/EditorLaunchArgsTests.cpp`
- [x] T035 [US4] Extract Editor CLI parsing into `Project/Editor/Core/EditorLaunchArgs.h` and `Project/Editor/Core/EditorLaunchArgs.cpp`
- [x] T036 [US4] Thread the parsed RHI debug-validation setting through `Project/Editor/Main.cpp`, `Project/Editor/Core/Application.*`, and `Project/Editor/Core/Context.*`
- [x] T037 [US4] Gate DX12 DRED diagnostics on `DriverSettings::debugMode` in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`
- [x] T038 [US4] Add DX12 device-resource coverage proving default debug launches enable DRED while performance mode skips it
- [x] T039 [US4] Run DX12 editor smoke with `--editor-performance-mode` and verify a responsive 15-second sample with no DRED/GPU validation enablement logs

---

## Phase 6: Polish & Cross-Cutting Concerns

- [x] T019 Run `cmake --build Build --target NullusUnitTests --config Debug -- /m /nologo`
- [x] T020 Run `Build/bin/Debug/NullusUnitTests.exe`
- [x] T021 Run `cmake --build Build --target Editor --config Debug -- /m /nologo`
- [x] T022 Self-review synchronization changes for missing value propagation, resize safety, generated-file edits, and DX12-only validation claims
- [x] T029 Re-run DX12 editor smoke with `App/Win64_Debug_Runtime_Static/Editor.exe --backend dx12 TestProject/TestProject.nullus`
- [x] T030 Run hygiene checks for temporary perf probes, experimental frame counts, and whitespace errors
- [x] T033 Re-run DX12 editor startup responsiveness samples with and without `--dx12-log-frame-flow`
- [x] T040 Re-run `cmake --build Build --target NullusUnitTests --config Debug -- /m:4 /nologo /nodeReuse:false`
- [x] T041 Re-run `Build/bin/Debug/NullusUnitTests.exe`
- [x] T042 Re-run `cmake --build Build --target Editor --config Debug -- /m:4 /nologo /nodeReuse:false`

## Dependencies & Execution Order

- Phase 1 must complete before implementation.
- Phase 2 tests must be written and observed failing before production code.
- User Story 1 is the MVP and blocks meaningful runtime FPS validation.
- User Story 2 is required before completion because resource lifetime safety is non-negotiable.
- User Story 3 is a measured follow-up after the DX12 UI bridge fix.

## Parallel Opportunities

- T002 and T003 can run independently after T001.
- T005/T006 and T007/T008 can be reviewed independently but should merge before propagation tasks.
- T017 can only run after T019-T021.

## Implementation Strategy

Complete US1 first, validate tests/build, then re-measure runtime. Proceed into SceneView behavior changes only when evidence shows the per-view post-render drain remains dominant; require policy tests and delayed picking readback tests before removing the drain for texture-only consumers.
