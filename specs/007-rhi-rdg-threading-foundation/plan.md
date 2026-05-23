# Implementation Plan: UE5-Style RHI/RDG Threading Foundation

**Branch**: `007-rhi-rdg-threading-foundation` | **Date**: 2026-04-19 | **Spec**: [spec.md](D:/VSProject/Nullus/specs/007-rhi-rdg-threading-foundation/spec.md)
**Input**: Feature specification from `/specs/007-rhi-rdg-threading-foundation/spec.md`

## Summary

Rebuild Nullus rendering foundation toward a UE5-style architecture for DX12 and Vulkan by establishing explicit Game Thread, Render Thread, and RHI Thread ownership; making the render graph authoritative for scheduling and resource lifetime; enabling truthful capability-driven rollout for parallel recording and async compute; and centralizing PSO plus descriptor lifetime management behind backend-aware services.

The implementation proceeds in two layers:

1. a delivered foundation layer that establishes truthful capability reporting, threaded ownership boundaries, graph-backed execution-plan plumbing, ordered multi-command-buffer submission, and hooks plus diagnostics for async compute, transient lifetime, PSO, and descriptor systems;
2. a remaining UE-alignment closure layer that must still make the Render Thread and RDG authoritative owners, replace serial ordered-submit fallback with true parallel recording and translation, promote compute work to real async-compute scheduling, and move descriptor/transient/PSO systems from hooks into real main-path adoption.

The first slices have already landed the foundation layer. The remaining work in this plan is the second-stage closure required before the feature can be considered meaningfully aligned with UE5-style RHI, multithreaded rendering, and RDG architecture rather than just directionally compatible.

## Technical Context

**Language/Version**: C++17  
**Primary Dependencies**: Nullus Runtime/Engine rendering stack, DX12, Vulkan, GoogleTest, CMake/MSBuild, RenderDoc  
**Storage**: N/A  
**Testing**: `NullusUnitTests`, targeted runtime validation, RenderDoc captures for rendering acceptance  
**Target Platform**: Windows editor/game validation first, DX12 and Vulkan as Tier A backends for this feature  
**Project Type**: Desktop engine/runtime  
**Performance Goals**: Preserve runnable editor/game flows while moving frame execution toward threaded, graph-driven submission; enable future parallel recording and async compute without architecture forks  
**Constraints**: Must preserve `Driver` as the top-level graphics entry point during this phase; must not overclaim unsupported backend features; must keep editor/game runnable during migration  
**Scale/Scope**: Multi-subsystem rendering foundation spanning `Runtime/Rendering`, `Runtime/Engine/Rendering`, and `Tests/Unit`

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- `Spec-First Major Changes`: Pass. This feature has a dedicated spec bundle under `specs/007-rhi-rdg-threading-foundation/`.
- `Validation Matches The Subsystem`: Pass with staged evidence. Early slices use targeted unit tests for capability and ownership logic; later slices require DX12 and Vulkan runtime validation plus RenderDoc evidence.
- `Generated Code And Backend Boundaries Are Sacred`: Pass. No generated files under `Runtime/*/Gen/` are touched. Backend capability gaps will be expressed through capability reporting and gating instead of renderer-side silent forks.
- `Incremental, Verified Delivery`: Pass. Work is split into explicit slices: capability model, thread ownership convergence, RDG authority, parallel recording, async compute, and PSO/descriptor centralization.
- `Product Runtime Preservation`: Pass with migration guardrails. DX12 and Vulkan remain runnable targets during migration; legacy backends degrade truthfully to legacy paths rather than pretending full support.

## Project Structure

### Documentation (this feature)

```text
specs/007-rhi-rdg-threading-foundation/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── render-foundation-contract.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/
├── Engine/
│   └── Rendering/
│       ├── BaseSceneRenderer.*
│       ├── ForwardSceneRenderer.*
│       └── DeferredSceneRenderer.*
└── Rendering/
    ├── Context/
    │   ├── Driver.*
    │   └── ThreadedRenderingLifecycle.*
    ├── FrameGraph/
    │   └── *
    ├── RHI/
    │   ├── Backends/
    │   │   ├── DX12/
    │   │   ├── Vulkan/
    │   │   ├── DX11/
    │   │   ├── OpenGL/
    │   │   └── Metal/
    │   ├── Core/
    │   ├── Utils/
    │   │   ├── DescriptorAllocator/
    │   │   ├── PipelineCache/
    │   │   └── ResourceStateTracker/
    │   ├── RHITypes.h
    │   └── *
    └── Settings/
        └── GraphicsBackendUtils.h

Tests/
└── Unit/
    ├── GraphicsBackendUtilsTests.cpp
    ├── ThreadedRenderingLifecycleTests.cpp
    └── *
```

**Structure Decision**: Keep all work inside the existing engine layout. `Driver` remains the integration seam, `Runtime/Rendering/RHI` owns backend and capability truth, `Runtime/Rendering/FrameGraph` evolves into the authoritative scheduler, and `Tests/Unit` carries the first validation layers before runtime capture-based acceptance.

## Delivery Status

### Completed Foundation Slices

- Truthful DX12/Vulkan capability reporting and routing gates are in place.
- Threaded ownership boundaries are explicit enough to keep DX12/Vulkan on the new path instead of legacy direct-submit as the normal steady-state flow.
- A graph-backed compiled execution-plan layer exists and is consumed by `ForwardSceneRenderer` and `DeferredSceneRenderer`.
- Ordered multi-command-buffer submission and parallel-work-unit plumbing exist, but still run through serial recording fallback.
- Hooks plus diagnostics exist for async compute, transient lifetime, PSO cache, and descriptor allocator services.
- Shared GPU `LightGrid` compute prepass (`Injection` + `Compact`) is integrated into both forward and deferred shading paths.

### Remaining UE-Alignment Closure

- Make Render Thread and RDG the authoritative frame-build owners instead of allowing the calling thread to publish authoritative render packages.
- Remove steady-state `Driver` fallback scene-package construction from the normal DX12/Vulkan path.
- Promote graph compilation from renderer-metadata compilation to true graph-owned execution and resource scheduling.
- Replace ordered-submit skeleton plus serial fallback with real parallel recording and translation workers.
- Promote compute workloads from graphics-queue compute prepasses to truthful async-compute scheduling on dedicated compute paths where supported.
- Move descriptor allocation, transient resource retirement, and PSO caching from hooks/diagnostics into real main-path adoption.

## Complexity Tracking

No constitution violations are currently required. Complexity is managed through staged migration rather than parallel architectures.
