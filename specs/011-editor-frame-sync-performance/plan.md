# Implementation Plan: Editor Frame Sync Performance

**Branch**: `011-editor-frame-sync-performance` | **Date**: 2026-05-03 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/011-editor-frame-sync-performance/spec.md`

## Summary

Reduce DX12 editor frame stalls by replacing per-frame CPU drains in UI rendering with per-backbuffer fence tracking and by carrying the submitted UI fence value through the UI bridge, driver, RHI present description, and DX12 present queue wait. Keep resize/shutdown drains for resource lifetime safety, remove the remaining SceneView post-render drain for texture-only consumers by moving picking to a delayed readable-frame lifecycle, allow UI composition to proceed while only offscreen threaded work is in flight, and add an explicit editor performance launch mode for profiling without DX12 validation overhead.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus RHI, DX12 backend, ImGui DX12 backend, GoogleTest
**Storage**: N/A
**Testing**: `NullusUnitTests`, `Editor` target build, focused DX12 editor runtime verification
**Target Platform**: Windows DX12 for this change
**Project Type**: Desktop editor / rendering runtime
**Performance Goals**: Remove the measured per-frame UI bridge CPU fence drain; improve editor frame rate from the observed 20-26 FPS bottleneck toward interactive DX12 editor responsiveness
**Constraints**: Preserve editor runtime, preserve resize/shutdown resource lifetime safety, do not hand-edit generated files, do not claim validation for non-DX12 backends
**Scale/Scope**: DX12 UI bridge synchronization, RHI present synchronization metadata, driver/UI handoff, SceneView retirement-aware drain policy, threaded picking readback lifecycle, UI/offscreen threaded frame boundary policy, editor launch performance-mode diagnostics gate, targeted unit tests

## Constitution Check

- Spec-first scope: PASS. This is rendering/editor behavior work and uses `specs/011-editor-frame-sync-performance/`.
- Validation matches subsystem: PASS. Plan includes unit tests, DX12 editor build, and DX12 runtime verification; no cross-backend claim.
- Generated/backend boundaries: PASS. No generated files are in scope; DX12-specific behavior stays in DX12 backend and RHI sync contract.
- Incremental verified delivery: PASS. First increment is the DX12 UI bridge wait removal with failing tests before production changes.
- Product runtime preservation: PASS. Editor must build and run; resize/shutdown drains are preserved.

## Project Structure

### Documentation (this feature)

```text
specs/011-editor-frame-sync-performance/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/RHI/Backends/DX12/
├── DX12UIBridge.cpp
└── DX12UIFrameFenceTracker.h

Runtime/Rendering/RHI/Core/
└── RHISwapchain.h

Runtime/Rendering/RHI/Utils/
└── RHIUIBridge.h

Runtime/Rendering/Context/
├── Driver.cpp
├── DriverAccess.h
├── DriverInternal.h
└── RhiThreadCoordinator.cpp

Runtime/UI/
├── UIManager.cpp
└── UIManager.h

Project/Editor/Core/
└── Editor.cpp

Project/Editor/Panels/
├── AView.cpp
└── ViewFrameLifecycle.h

Project/Editor/Rendering/
├── PickingRenderPass.cpp
└── PickingReadbackLifecycle.h

Tests/Unit/
├── DX12UIFrameFenceTrackerTests.cpp
├── PanelWindowHookTests.cpp
├── PickingReadbackLifecycleTests.cpp
└── ThreadedRenderingLifecycleTests.cpp
```

**Structure Decision**: Keep the policy helper beside DX12 backend code because it encodes DX12 fence/backbuffer reuse behavior but is pure enough for unit tests. Extend the existing RHI present description and driver/UI access path instead of inventing a parallel present workflow.

## Phase 0 Research

- **Decision**: Track UI fence values per backbuffer and wait before allocator reset only when the prior value for that backbuffer is incomplete.
  **Rationale**: Command allocators cannot be reset while GPU work using them may still execute, but waiting immediately after every submit serializes CPU/GPU every frame.
  **Alternatives considered**: Delete the wait entirely (unsafe allocator reuse), keep current wait (known performance bottleneck), globally rotate more allocators (higher memory and still lacks explicit reuse safety).

- **Decision**: Add a UI signal value to the existing RHI present path.
  **Rationale**: DX12 present currently waits on a raw fence pointer with hardcoded value `1`; the present queue must wait on the actual value submitted by the UI bridge.
  **Alternatives considered**: Wrap fence and value in an opaque allocation (more lifetime complexity), rely on CPU wait (keeps bottleneck), signal fixed value every frame (invalid after first frame).

- **Decision**: Preserve lifecycle drains on resize and shutdown.
  **Rationale**: Resource release requires stronger synchronization than steady-state rendering.
  **Alternatives considered**: Fully async release (requires a deferred deletion system outside this fix).

- **Decision**: Do not drain after every retirement-aware view render unless the consumer requires immediate readback.
  **Rationale**: Runtime probing showed the post-render view drain remained a dominant frame-time cost; texture-only UI composition can use retired textures at the composition boundary.
  **Alternatives considered**: Keep same-frame drains (preserves the bottleneck), remove all drains (unsafe for readback consumers), make picking force same-frame retirement (keeps hitching during selection).

- **Decision**: Treat threaded picking readback as delayed by one readable frame.
  **Rationale**: Picking needs CPU-visible readback data, but it can return no result until a submitted picking frame has become readable instead of blocking the frame.
  **Alternatives considered**: Same-frame drain before every pick (performance regression), decode against stale metadata (incorrect selection), disable picking under threaded rendering (feature regression).

- **Decision**: Remove the UI composition boundary drain for offscreen-only threaded work while continuing to reject UI composition during in-flight swapchain-targeting threaded frames.
  **Rationale**: Follow-up probes showed `PrepareUIRender()` spent roughly 20-40 ms draining offscreen render-build and RHI submission work before UI composition, which held the editor near 30 FPS after the UI fence fix. Offscreen-only frames do not own the swapchain frame context, while swapchain-targeting frames still must block standalone UI composition.
  **Alternatives considered**: Drain all threaded work before UI composition (preserves 30 FPS bottleneck), allow UI composition during swapchain threaded frames (unsafe frame-context ownership), increase frames-in-flight (masks pressure without removing the sync point).

- **Decision**: Guard standalone UI frame setup with a non-blocking threaded RHI submission mutex and hold that guard until UI present finalizes the standalone frame.
  **Rationale**: Runtime startup verification showed allowing UI composition during offscreen-only in-flight work could still deadlock the window thread because offscreen RHI submission and standalone UI composition share `frameContexts[currentFrameIndex]`. A non-blocking guard preserves the no-drain policy while preventing frame-context reset/fence waits during worker ownership.
  **Alternatives considered**: Restore full offscreen drain (fixes hang but restores 30 FPS bottleneck), wait on the mutex (can freeze UI), allocate a separate UI frame-context ring (larger architectural change).

- **Decision**: Add explicit `--editor-performance-mode` and `--no-rhi-debug-validation` launch flags instead of changing normal debug defaults.
  **Rationale**: After synchronization fixes, the remaining empty-scene ~80 FPS report coincided with DX12 Debug Layer, GPU-based validation, and DRED being enabled by default in Debug editor launches. Profiling needs a clean non-validation mode, but day-to-day debug launches should retain safety diagnostics.
  **Alternatives considered**: Disable validation by default (faster but less safe), require Release builds for profiling (slower iteration), only disable GPU-based validation while leaving DRED forced on (still leaves debug diagnostic overhead in the measured path).

## Phase 1 Design

### Data Model

- `DX12UIFrameFenceTracker`: stores `lastSubmittedFenceValue` globally and a vector of per-backbuffer submitted values.
- `RHIPresentDesc::uiSignalValue`: nonzero fence value that the present queue waits on when `uiSignalSemaphore` is set.
- `DriverImpl::uiRenderFinishedValue`: latest UI submitted fence value paired with `uiRenderFinishedSemaphore`.
- `PickingReadbackLifecycle`: stores pending submitted picking frame metadata and promotes it only when a readback source is available.
- `ParsedEditorLaunchArgs`: stores editor command-line options including the explicit RHI debug-validation gate used by `Context`.

### Contracts

- `RHIUIBridge::GetUISignalValue()` returns the most recent submitted UI fence value, or `0` when there is no submitted UI work to wait on.
- `DriverUIAccess::SetUISignalSemaphore(driver, semaphore, value)` records both native handle and synchronization value.
- DX12 `Present()` waits on `presentDesc.uiSignalSemaphore` only when `presentDesc.uiSignalValue > 0`.
- `AView` drains immediately after rendering only when the view both consumes retired frames and requires same-frame readback.
- `PickingRenderPass` returns no picking result until a submitted picking frame has been promoted to readable metadata.
- `PrepareUIRender()` starts the standalone UI frame when threaded in-flight work is offscreen-only, but rejects while any in-flight threaded frame targets the swapchain.
- Standalone UI frame setup fails fast when threaded RHI submission owns the shared frame context, and releases its ownership guard only after `PresentSwapchain()` finalizes the UI frame.
- `--editor-performance-mode` and `--no-rhi-debug-validation` set `DriverSettings::debugMode=false` for the editor RHI device; default launches keep `debugMode=true`.
- DX12 DRED diagnostics are enabled only when `DriverSettings::debugMode=true`, matching the Debug Layer/GPU-based validation gate.

### Quickstart Validation

1. Build the unit test target: `cmake --build Build --target NullusUnitTests --config Debug -- /m /nologo`.
2. Run unit tests: `Build/bin/Debug/NullusUnitTests.exe`.
3. Build editor: `cmake --build Build --target Editor --config Debug -- /m /nologo`.
4. Launch DX12 editor with the test project and inspect FPS/frame-time behavior: `App/Win64_Debug_Runtime_Static/Editor.exe --backend dx12 D:/VSProject/Nullus/TestProject/TestProject.nullus`.
5. Launch DX12 editor in performance mode for empty-scene FPS profiling: `App/Win64_Debug_Runtime_Static/Editor.exe --backend dx12 --editor-performance-mode D:/VSProject/Nullus/TestProject/TestProject.nullus`.

## Post-Design Constitution Check

- Spec scope remains valid.
- Tests are defined before production changes.
- DX12-only validation boundary is explicit.
- Resize/shutdown safety remains in scope.
- Editor runtime viability remains a completion gate.

## Complexity Tracking

No constitution violations.
