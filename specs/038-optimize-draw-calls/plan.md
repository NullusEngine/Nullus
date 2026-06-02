# Implementation Plan: Optimize Draw-Call Scalability

**Branch**: `038-optimize-draw-calls` | **Date**: 2026-05-29 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/038-optimize-draw-calls/spec.md`

## Summary

Nullus already has UE-like cached scene draw state and a first-pass dynamic instancing merge for compatible opaque drawables. The plan extends that into a measurable draw-call scalability path: strengthen cached state buckets and per-frame dynamic instance grouping, add telemetry proving draw-call reduction, split attachment-free large recorded draw ranges into bounded ordered serial work units, and record attachment-backed DX12-capable scene passes through bundle-backed in-render-pass child command buffers. Full persistent instance grouping, HISM-style hierarchy culling, GPU-driven indirect draws, and non-DX12 secondary-command-buffer implementations are documented as follow-up work rather than the MVP.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: CMake, GoogleTest, Nullus Formal RHI, DX12 backend, RenderDoc tooling  
**Storage**: In-memory scene/render data only  
**Testing**: `NullusUnitTests`, targeted GoogleTest suites, DX12 RenderDoc capture evidence  
**Target Platform**: Windows DX12 runtime first; other backend enum paths remain capability-gated  
**Project Type**: Desktop engine/runtime renderer  
**Performance Goals**: Reduce compatible repeated-object draw submissions by at least 90%; record large non-groupable draw sets through multiple command work units on DX12 threaded rendering  
**Constraints**: Preserve existing visual output, object-indexed shader data, transparent ordering, render pass clear/load semantics, and Editor/Game runtime viability  
**Scale/Scope**: MVP validates 1,000 compatible opaque objects and 2,000 non-groupable recorded draw commands; dense-instance hierarchy culling is a follow-up phase

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. Rendering pipeline behavior changes are tracked under `specs/038-optimize-draw-calls/`.
- **Validation matches subsystem**: PASS. Plan requires targeted unit tests plus DX12 RenderDoc evidence for runtime rendering claims.
- **Generated/backend boundaries**: PASS. No generated files are in scope; DX12-specific behavior is capability-gated and must not claim other backend correctness.
- **Incremental verified delivery**: PASS. Work is split into independently testable grouping, command splitting, telemetry, and follow-up culling tasks.
- **Product runtime preservation**: PASS. Editor/Game paths must keep serial fallbacks and must not require parallel rendering to remain correct.

## Project Structure

### Documentation (this feature)

```text
specs/038-optimize-draw-calls/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Engine/Rendering/
├── RenderScene.h
├── RenderScene.cpp
├── BaseSceneRenderer.cpp
├── DeferredSceneRenderer.cpp
├── ForwardSceneRenderer.cpp
└── EngineFrameObjectBindingProvider.cpp

Runtime/Rendering/Context/
├── ThreadedRenderingLifecycle.h
├── ThreadedRenderingLifecycle.cpp
├── RenderScenePackageBuilder.cpp
└── RhiThreadCoordinator.cpp

Runtime/Rendering/FrameGraph/
├── FrameGraphExecutionTypes.h
└── FrameGraphExecutionPlan.h

Runtime/Rendering/Core/
├── RendererStats.h
├── RendererStats.cpp
└── CompositeRenderer.cpp

Runtime/Rendering/Data/
├── FrameInfo.h
└── ObjectDataLimits.h

Tests/Unit/
├── RenderSceneCacheTests.cpp
├── RendererFrameObjectBindingTests.cpp
├── ThreadedRenderingLifecycleTests.cpp
├── RendererStatsTests.cpp
└── DX12PipelineLayoutUtilsTests.cpp
```

**Structure Decision**: Reuse existing renderer ownership boundaries. `RenderScene` owns scene visibility, cached draw state, compatible instance grouping, and the last gather's optimization stats. `BaseSceneRenderer` bridges those stats into `RendererStats`/`FrameInfo` after `GatherVisibleCommands`, while `EngineFrameObjectBindingProvider` remains responsible for object-data upload. Large-pass slicing is authored while render-scene packages and FrameGraph work units are materialized (`RenderScenePackageBuilder` and `FrameGraphExecutionPlan`); `RhiThreadCoordinator` consumes attachment-free ordered slices directly when safe, records attachment-backed child slices inside a single parent render pass when `InRenderPassChildCommandBuffers` is available, otherwise rebuilds/records from the original unsliced `passCommandInputs` serial fallback and reports the fallback reason.

## Complexity Tracking

No constitution violations are required. The intentional complexity is backend-gated command work-unit slicing plus DX12 bundle-backed child recording for attachment-backed passes; both remain inside the existing threaded RHI path, keep serial fallback behavior, and do not claim correctness for unsupported backends.
