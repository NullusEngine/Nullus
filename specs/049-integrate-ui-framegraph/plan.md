# Implementation Plan: Integrate UI FrameGraph

**Branch**: `049-integrate-ui-framegraph` | **Date**: 2026-06-13 | **Spec**: `specs/049-integrate-ui-framegraph/spec.md`
**Input**: Feature specification from `/specs/049-integrate-ui-framegraph/spec.md`

## Summary

Move editor UI rendering out of the DX12 native `DX12UIBridge::RenderDrawData` direct-submit path and into the normal RHI frame lifecycle. UI generation remains on the ImGui/UI thread, but `ImDrawData` is copied into a frame-owned snapshot and published to `Driver`. The RHI frame then records an `RHIFrameGraph::UIOverlay` swapchain pass after scene/editor rendering and before present, using the existing acquire, resource transition, queue submit, fence, and present path.

DX12 Editor is the MVP target. Other backends remain capability-gated and are not claimed validated until they implement the same overlay pass contract and have backend evidence.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus RHI, FrameGraph, ImGui draw data, DX12 backend, CMake, GoogleTest, TimelineProfiler, RenderDoc tooling
**Storage**: N/A; frame-owned in-memory UI snapshots and renderer-owned GPU resources
**Testing**: `NullusUnitTests`, focused source/contract tests, DX12 Editor TimelineProfiler trace, DX12 RenderDoc capture where available
**Target Platform**: Windows DX12 Editor for MVP; Game/Launcher must remain runnable; other graphics backends are gated
**Project Type**: Desktop/editor rendering architecture change
**Performance Goals**: Remove recurring main-thread `DX12UIBridge::WaitForBackbufferReuse` from at least 99% of DX12 Profiler UI frames over a 300-frame validation run; avoid UI-owned extra queue submissions/presents for ordinary UI frames; measure snapshot-copy CPU cost, dynamic-buffer reallocations, submit counts, and before/after frame-time deltas
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; do not restore a renderer-side legacy fork; UI draw data must be copied before the next `ImGui::NewFrame()`; no direct DX12 UI queue submit on migrated path; no backend support claims beyond explicit validation
**Scale/Scope**: One swapchain UI overlay pass per frame at most, including UI-only frames; minimum font atlas upload/binding is part of the DX12 MVP; registered texture views must survive resize/release lifetimes

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This is rendering/RHI/frame-graph architecture work and is tracked under `specs/049-integrate-ui-framegraph/`.
- **Validation matches subsystem**: PASS. Plan requires unit/source tests plus DX12 TimelineProfiler and RenderDoc/RHI evidence; it explicitly avoids using DX12 evidence as proof for Vulkan, DX11, OpenGL, or Metal.
- **Generated code/backend boundaries**: PASS. No generated files are part of the planned edit surface. DX12 backend behavior is capability-gated and the mainline path moves toward formal RHI ownership.
- **Incremental verified delivery**: PASS. Work is split into snapshot/resource foundation, P1 overlay path, P2 UI-only routing, P3 texture/font compatibility, and validation.
- **Product runtime preservation**: PASS. Editor and Launcher remain runnable during migration; fallbacks must be explicit and capability-driven.

## Project Structure

### Documentation (this feature)

```text
specs/049-integrate-ui-framegraph/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── ui-draw-snapshot-contract.md
│   ├── rhi-ui-overlay-pass-contract.md
│   ├── ui-frame-routing-contract.md
│   └── legacy-dx12-ui-bridge-exclusion.md
├── validation/
│   └── final-diagnostics.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/UI/
├── UiDrawDataSnapshot.h
├── UiDrawDataSnapshot.cpp
├── RHIImGuiOverlayRenderer.h
├── RHIImGuiOverlayRenderer.cpp
├── RHIImGuiTextureRegistry.h
├── RHIImGuiTextureRegistry.cpp
├── RHIImGuiFontAtlas.h
├── RHIImGuiFontAtlas.cpp
└── shader artifacts registered through the existing ShaderArtifact/ShaderManager path

Runtime/Rendering/Context/
├── DriverAccess.h
├── Driver.cpp
├── DriverInternal.h
├── RenderScenePackageBuilder.h
├── RenderScenePackageBuilder.cpp
├── RhiThreadCoordinator.cpp
├── ThreadedRenderingLifecycle.h
└── ThreadedRenderingLifecycle.cpp

Runtime/Rendering/FrameGraph/
├── FrameGraphExecutionTypes.h
└── FrameGraphExecutionPlan.h

Runtime/Rendering/RHI/
├── RHITypes.h
├── Core/RHICommand.h
├── Core/RHIDevice.h
├── Core/RHIResource.h
├── Utils/RHIUIBridge.h
├── Utils/RHIUIBridge.cpp
└── Backends/DX12/
    ├── DX12Device.cpp
    └── DX12UIBridge.cpp

Runtime/UI/
├── UIManager.h
├── UIManager.cpp
├── Widgets/Visual/Image.cpp
└── Widgets/Buttons/ButtonImage.cpp

Project/Editor/Core/Editor.cpp
Project/Launcher/Core/Launcher.cpp

Tests/Unit/
├── UiDrawDataSnapshotTests.cpp
├── RHIUiOverlayPassTests.cpp
├── RHIUiTextureRegistryTests.cpp
├── ThreadedRenderingLifecycleTests.cpp
├── FrameGraphSceneTargetsTests.cpp
├── ProfilerDestinationTests.cpp
└── RHIUiOverlaySourceGuardTests.cpp
```

**Structure Decision**: Put UI draw snapshots and the RHI ImGui renderer under `Runtime/Rendering/UI` because `NLS_Render` already depends on ImGui headers and `NLS_UI` depends on `NLS_Render`. This avoids a Render-to-UI module cycle while letting `UIManager` publish snapshots into the renderer-owned frame path.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | N/A | N/A |

## Capability Rollout Rule

`RHIDeviceFeature::UIOverlayFrameGraph` may be added early as a type/capability plumbing point, but DX12 product routing must remain `runtimeSelectable == false` until overlay recording, UI-only package routing, font atlas upload/binding, texture registry retention, legacy direct-submit exclusion, and validation hooks are implemented. Tests may force internal overlay construction before that point; Editor/Launcher must not enter a half-migrated runtime path.

## Phase 0: Research Output

See `research.md` for decisions on snapshot lifetime, RHI overlay recording, UI-only routing, texture/font resource identity, capability gating, and validation.

## Phase 1: Design Output

See `data-model.md` and `contracts/` for the frame-owned snapshot model, UI texture identity, font atlas resource, overlay pass contract, UI-only frame routing, and the legacy DX12 bridge exclusion rules.

## Post-Design Constitution Check

- **Spec-first major change**: PASS. Plan, research, data model, contracts, quickstart, and tasks are kept in one spec bundle.
- **Validation matches subsystem**: PASS. Automated tests cover snapshot lifetime and routing contracts; DX12 runtime evidence remains required before sign-off.
- **Generated code/backend boundaries**: PASS. Planned files avoid generated directories and keep backend-specific behavior behind capability and DX12 implementation boundaries.
- **Incremental verified delivery**: PASS. User Story 1 is independently testable before UI-only and resource compatibility work.
- **Product runtime preservation**: PASS. The migration keeps explicit fallback states and requires Editor/Launcher smoke validation.
