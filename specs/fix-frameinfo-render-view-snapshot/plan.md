# Implementation Plan: FrameInfo Render View Snapshot

**Branch**: `fix-frameinfo-render-view-snapshot` | **Date**: 2026-05-26 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/fix-frameinfo-render-view-snapshot/spec.md`

## Summary

FrameInfo remains an independent dockable editor panel, but its data source moves from live renderer/editor UI metrics to the targeted render view's cached `FrameInfo` snapshot. Editor and renderer paths avoid blocking telemetry reads, RHI frame begin paths fail closed on fence wait failure, and the editor UI standalone present path receives priority over background offscreen threaded RHI submissions.

## Technical Context

**Language/Version**: C++20 in the existing Nullus Visual Studio/MSBuild project  
**Primary Dependencies**: Existing Editor panels, ImGui UI system, Runtime Rendering driver/RHI, GoogleTest  
**Storage**: N/A  
**Testing**: `NullusUnitTests.exe` with focused GoogleTest filters  
**Target Platform**: Windows editor/runtime test build; backend claims limited to unit/contract coverage  
**Project Type**: Desktop editor + runtime rendering engine  
**Performance Goals**: Opening/refreshing FrameInfo must not introduce blocking lifecycle telemetry waits on the UI draw path, and opening the panel must not starve editor swapchain presents behind continuous render-view submissions  
**Constraints**: Preserve generated-file boundaries; preserve formal RHI fail-closed resource ownership; do not hand-edit generated output  
**Scale/Scope**: Scene/Game/Asset render views, FrameInfo panel, renderer stats, driver telemetry, and RHI frame begin contracts

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec scope: Major editor/rendering/RHI change, tracked in `specs/fix-frameinfo-render-view-snapshot/`.
- Generated boundaries: No hand edits under `Runtime/*/Gen/` or `Project/*/Gen/`; generated-file diff check required.
- Backend/platform validation: Unit/contract tests cover driver/RHI behavior with contract devices; DX12 editor smoke validates the reported FrameInfo present-starvation path.
- Product runtime viability: Editor panel remains registered as an independent dockable panel; FrameInfo data source changes to cached view snapshots.
- Final evidence path: MSBuild unit-test build, PanelWindowHookTests, RendererStats/RenderFramework/ThreadedRenderingLifecycle focused tests, DX12 editor smoke with FrameInfo open, `git diff --check`, generated-file diff check, plan-review/multi-agent review.

## Project Structure

### Documentation (this feature)

```text
specs/fix-frameinfo-render-view-snapshot/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Core/
└── Editor.cpp

Project/Editor/Panels/
├── AView.cpp
├── AView.h
├── FrameInfo.cpp
├── FrameInfo.h
├── FrameInfoRendererStats.cpp
└── ViewFrameLifecycle.h

Runtime/Rendering/Context/
├── Driver.cpp
├── DriverAccess.h
├── DriverInternal.h
├── RenderThreadCoordinator.cpp
├── RenderThreadCoordinator.h
├── RhiThreadCoordinator.cpp
└── RhiThreadCoordinator.h

Runtime/Rendering/Core/
├── ABaseRenderer.cpp
├── CompositeRenderer.cpp
├── RendererStats.cpp
└── RendererStats.h

Tests/Unit/
├── PanelWindowHookTests.cpp
├── RendererStatsTests.cpp
└── RenderFrameworkContractTests.cpp
```

**Structure Decision**: Keep the change inside existing editor panel, renderer stats, driver/RHI coordinator, and unit-test ownership boundaries. Do not add new runtime subsystems.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| RHI fail-closed tests included with FrameInfo fix | The freeze investigation exposed unsafe blocking/fence paths adjacent to the panel regression | Only changing FrameInfo would leave known resource reuse and drain failure hazards that can still freeze or corrupt rendering |

## Phase 0 Research

- Cached diagnostics panels should render from snapshots rather than querying live producer state during UI draw.
- RHI frame resources must not be reset or reused when the associated fence wait fails.
- Editor UI synchronization must clear stale wait semaphores every frame, not only when a new scene semaphore exists.

## Phase 1 Design

- `AView` owns `std::optional<Render::Data::FrameInfo>` and exposes a const snapshot getter.
- `FrameInfo::RefreshForView()` formats the target view snapshot or an empty snapshot; it does not access renderer internals.
- `CompositeRenderer::EndFrame()` updates renderer stats with `TryGetThreadedFrameTelemetry()` and reuses prior telemetry if try-read is busy.
- `AView` uses nonblocking telemetry reads and `TryDrainThreadedRendering()` for resize/drain-sensitive paths.
- RHI begin functions return failure where needed so callers avoid resetting/reusing frame resources after fence wait failure.
- UI-to-present sync drops zero-valued UI completion fences at the driver boundary and rejects them in the DX12 present backend.
- DX12 semaphore reset clears only the pending per-frame wait value while preserving the backend fence's monotonically increasing signal value.
- `RhiThreadCoordinator::PrepareUIRender()` marks a pending UI standalone frame request before acquiring the shared threaded RHI submission lock; worker-attributed RHI submissions yield while that request is pending, while synchronous drains remain allowed.
- Unit tests cover snapshot source, busy telemetry behavior, post-render drain updates, UI wait clearing, UI-present starvation prevention, and RHI fail-closed contracts.

## Post-Design Constitution Check

- Spec bundle present and updated to match implementation.
- Generated-file check required before completion.
- Validation commands listed in tasks and final summary.
- Runtime evidence is limited to the DX12 editor smoke described in the validation tasks; no broader backend matrix claim is made.
