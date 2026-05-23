# Implementation Plan: Render Feature Refactor

**Branch**: `005-render-feature-refactor` | **Date**: 2026-04-18 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `D:/VSProject/Nullus/specs/005-render-feature-refactor/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Refactor the current `ARenderFeature` model so it no longer owns core draw-time rendering responsibilities. The migration keeps scene rendering, debug draw, lighting, and frame statistics working while moving core frame/object binding into renderer-owned state, moving debug drawing into an explicit debug drawing path, exposing lighting as renderer/scene data, and converting renderer statistics into renderer-owned diagnostics. `ARenderFeature` remains only as a temporary compatibility layer until all responsibilities are migrated, then it is removed or narrowed to lifecycle-only extension behavior.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus runtime rendering modules, Formal RHI, FrameGraph, ImGui/editor integration, GoogleTest/CTest
**Storage**: Not applicable: this refactor changes runtime rendering ownership and transient frame data, not persistent project data
**Testing**: `NullusUnitTests` through CTest, targeted rendering unit tests, editor/game smoke checks, RenderDoc evidence when visual backend correctness is claimed
**Target Platform**: Desktop runtime/editor; current validated backend matrix is DX12 and Vulkan on Windows, with unsupported backends kept explicitly gated by existing runtime policy
**Project Type**: Desktop game engine/editor with multi-backend rendering runtime
**Performance Goals**: Avoid per-draw overhead regressions while preserving current scene rendering behavior; debug draw batching and renderer stats must remain frame-stable under current debug draw limits
**Constraints**: Keep Editor and Game runnable throughout staged migration; do not hand-edit generated files under `Runtime/*/Gen/`; do not expand this feature into a full shader variant or pipeline cache implementation
**Scale/Scope**: Rendering framework ownership boundary refactor spanning `Runtime/Rendering`, `Runtime/Engine/Rendering`, `Project/Editor/Rendering`, and focused unit/runtime validation

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. The feature has a dedicated spec bundle under `D:/VSProject/Nullus/specs/005-render-feature-refactor/` and is a rendering architecture change.
- **Validation matches subsystem**: PASS. The validation path includes `NullusUnitTests`, editor/game smoke checks, and RenderDoc evidence for any claimed visual/backend correctness.
- **Generated code and backend boundaries**: PASS. The plan excludes hand edits under `Runtime/*/Gen/` and keeps backend-specific behavior inside existing RHI/backend boundaries.
- **Incremental, verified delivery**: PASS. The migration is split into independently verifiable slices: core binding, stats, debug draw, lighting, extension narrowing/removal.
- **Product runtime preservation**: PASS. Editor and Game must remain runnable after each migration slice; temporary compatibility behavior is allowed only to preserve runtime behavior.

## Project Structure

### Documentation (this feature)

```text
D:/VSProject/Nullus/specs/005-render-feature-refactor/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── render-feature-ownership-contract.md
└── tasks.md
```

### Source Code (repository root)

```text
D:/VSProject/Nullus/Runtime/Rendering/Core/
├── ABaseRenderer.h
├── ABaseRenderer.cpp
├── CompositeRenderer.h
├── CompositeRenderer.cpp
├── CompositeRenderer.inl
├── ARenderPass.h
└── ARenderPass.cpp

D:/VSProject/Nullus/Runtime/Rendering/Features/
├── ARenderFeature.h
├── ARenderFeature.cpp
├── DebugShapeRenderFeature.h
├── DebugShapeRenderFeature.cpp
├── FrameInfoRenderFeature.h
├── FrameInfoRenderFeature.cpp
├── LightingRenderFeature.h
└── LightingRenderFeature.cpp

D:/VSProject/Nullus/Runtime/Rendering/Data/
├── DebugDrawTypes.h
└── DebugDrawTypes.cpp

D:/VSProject/Nullus/Runtime/Engine/Rendering/
├── BaseSceneRenderer.h
├── BaseSceneRenderer.cpp
├── EngineBufferRenderFeature.h
├── EngineBufferRenderFeature.cpp
├── ForwardSceneRenderer.h
├── ForwardSceneRenderer.cpp
├── DeferredSceneRenderer.h
└── DeferredSceneRenderer.cpp

D:/VSProject/Nullus/Project/Editor/Rendering/
├── DebugSceneRenderer.h
├── DebugSceneRenderer.cpp
├── GridRenderPass.h
├── GridRenderPass.cpp
├── DebugModelRenderFeature.h
├── DebugModelRenderFeature.cpp
├── GizmoRenderFeature.h
├── GizmoRenderFeature.cpp
├── OutlineRenderFeature.h
└── OutlineRenderFeature.cpp

D:/VSProject/Nullus/Tests/Unit/
├── CompositeRendererExplicitDrawOrderTests.cpp
├── DebugDrawTypesTests.cpp
├── DebugShapeRenderFeatureTests.cpp
├── ScenePipelineStatePresetsTests.cpp
├── RendererFrameObjectBindingTests.cpp
├── RendererStatsTests.cpp
├── DebugDrawPassTests.cpp
└── LightingDataProviderTests.cpp
```

**Structure Decision**: Keep the refactor inside existing rendering/editor/engine boundaries. New helper types should be added near their ownership domain: renderer-owned draw state under `Runtime/Rendering/Core`, engine scene data providers under `Runtime/Engine/Rendering`, and debug draw data/rendering ownership under existing rendering debug/data modules unless tasks identify a smaller local split. Existing feature files remain during compatibility migration and are removed or narrowed only after consumers move.

## Complexity Tracking

No constitution violations require justification.

## Phase 0: Research Output

Research decisions are recorded in `D:/VSProject/Nullus/specs/005-render-feature-refactor/research.md`.

## Phase 1: Design Output

Design artifacts are recorded in:

- `D:/VSProject/Nullus/specs/005-render-feature-refactor/data-model.md`
- `D:/VSProject/Nullus/specs/005-render-feature-refactor/contracts/render-feature-ownership-contract.md`
- `D:/VSProject/Nullus/specs/005-render-feature-refactor/quickstart.md`

## Post-Design Constitution Check

- **Spec-first major change**: PASS. All planning artifacts remain in the same feature bundle.
- **Validation matches subsystem**: PASS. Quickstart requires targeted unit tests, CTest, product smoke checks, and RenderDoc only for visual/backend claims.
- **Generated code and backend boundaries**: PASS. Design artifacts keep generated output and backend internals out of scope.
- **Incremental, verified delivery**: PASS. Research and data model define migration states that support independently testable slices.
- **Product runtime preservation**: PASS. Compatibility state is explicitly modeled until replacement owners are active.
