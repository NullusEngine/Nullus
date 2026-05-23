# Implementation Plan: UE4.27 Render Architecture Alignment

**Branch**: `022-ue427-render-architecture` | **Date**: 2026-05-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/022-ue427-render-architecture/spec.md`

## Summary

Align Nullus rendering architecture with UE4.27's core data-flow contracts by adding staged compatibility contracts for RHI command lists, RDG-style pass/resource ownership, shader parameter binding groups, and parallel draw command batches. Implementation starts with targeted contract tests and narrow metadata/facade additions before any backend or renderer rewrite.

## Technical Context

**Language/Version**: C++20 and HLSL where shader binding/runtime validation touches shader assets  
**Primary Dependencies**: Nullus Runtime Rendering, Engine scene renderers, FrameGraph, RHI command/binding/pipeline APIs, Shader reflection utilities, ThreadedRenderingLifecycle, GoogleTest  
**Storage**: N/A for the first architecture slice; existing settings remain unchanged  
**Testing**: `NullusUnitTests`, targeted renderer contract tests, Editor Debug build, DX12 RenderDoc evidence before runtime parity claims  
**Target Platform**: Windows DX12 for first runtime validation; backend-neutral contracts where possible  
**Project Type**: Desktop engine/editor runtime  
**Performance Goals**: Reduce hidden synchronization and repeated binding/graph work by making command list, graph dependency, shader parameter, and draw batch ownership explicit; preserve current performance while enabling later parallel recording optimizations  
**Constraints**: Do not hand-edit generated files; do not directly copy UE source; do not claim backend/platform parity without evidence; keep Editor and Game runnable during staged migration  
**Scale/Scope**: Runtime Rendering RHI command contracts, FrameGraph execution plan validation, shader binding layout utilities, Engine scene package preparation, threaded rendering lifecycle telemetry/tests

## Constitution Check

*GATE: Must pass before implementation.*

- Spec-first scope: PASS. This rendering architecture change is tracked under `specs/022-ue427-render-architecture/`.
- Validation matches subsystem: PASS. Plan requires targeted unit tests, Editor build validation, and DX12 runtime evidence before parity claims.
- Generated code boundary: PASS. No `Runtime/*/Gen/` or `Project/Editor/Gen/` files will be hand-edited.
- Incremental verified delivery: PASS. Work is split into RHI, frame graph, shader binding, and threaded draw command phases with tests first.
- Product runtime preservation: PASS. Existing Editor/Game paths remain active; new contracts are introduced as additive compatibility layers first.

## Project Structure

### Documentation

```text
specs/022-ue427-render-architecture/
├── spec.md
├── checklists/requirements.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/RHI/Core/RHICommand.h
Runtime/Rendering/RHI/Core/RHIDevice.h
Runtime/Rendering/RHI/Core/RHIBinding.h
Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h
Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h
Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder*.h
Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder*.cpp
Runtime/Rendering/Resources/ShaderBindingLayoutUtils.h
Runtime/Rendering/Resources/ShaderBindingLayoutUtils.cpp
Runtime/Rendering/Context/ThreadedRenderingLifecycle.h
Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp
Runtime/Engine/Rendering/BaseSceneRenderer.h
Runtime/Engine/Rendering/BaseSceneRenderer.cpp
Runtime/Engine/Rendering/ForwardSceneRenderer.cpp
Runtime/Engine/Rendering/DeferredSceneRenderer.cpp
Tests/Unit/RenderFrameworkContractTests.cpp
Tests/Unit/FrameGraphSceneTargetsTests.cpp
Tests/Unit/ShaderBindingLayoutUtilsTests.cpp
Tests/Unit/ThreadedRenderingLifecycleTests.cpp
F:/Epic Games/UE_4.27/Engine/Source/Runtime/RHI/Public/RHICommandList.h (read-only reference)
F:/Epic Games/UE_4.27/Engine/Source/Runtime/RenderCore/Public/RenderGraph.h (read-only reference)
F:/Epic Games/UE_4.27/Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp (read-only reference)
F:/Epic Games/UE_4.27/Engine/Source/Runtime/RenderCore/Public/ShaderParameterStruct.h (read-only reference)
F:/Epic Games/UE_4.27/Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.h (read-only reference)
```

**Structure Decision**: Add additive compatibility contracts around existing Nullus RHI, FrameGraph, shader binding, and threaded rendering types. Keep existing renderer entry points in place, then migrate their internals only after tests lock the UE4.27-aligned behavior.

## Complexity Tracking

No constitution violations.
