# Implementation Plan: UE5-Style DX12 Render Alignment

**Branch**: `008-ue5-dx12-render-alignment` | **Date**: 2026-04-21 | **Spec**: [spec.md](D:/VSProject/Nullus/specs/008-ue5-dx12-render-alignment/spec.md)
**Input**: Feature specification from `/specs/008-ue5-dx12-render-alignment/spec.md`

## Summary

Align Nullus to a UE5-style DX12 rendering architecture by replacing the current mixed-ownership, compatibility-driven rendering mainline with one authoritative Game Thread -> Render Thread -> RHI Thread pipeline, one authoritative RDG scheduler, one unified Editor + Game frame path, and mandatory centralized low-level rendering infrastructure. The first implementation phase activates only DX12 while preserving backend-neutral abstraction boundaries for later backends.

This plan is intentionally a purity and closure plan rather than an incremental compatibility plan. It treats direct-submit fallback, driver-built compatibility rendering, compatibility acquire/present branches, and editor-only bypasses as architectural debt to delete, not behavior to preserve.

## Technical Context

**Language/Version**: C++17
**Primary Dependencies**: Nullus Runtime/Engine rendering stack, DX12, GoogleTest, CMake/MSBuild, RenderDoc, UE 5.7 public rendering documentation as the alignment baseline
**Storage**: N/A
**Testing**: `NullusUnitTests`, focused runtime smoke, RenderDoc captures, explicit startup-failure validation
**Target Platform**: Windows DX12 Editor and Game for phase 1
**Project Type**: Desktop engine/runtime
**Performance Goals**: Preserve runnable Editor and Game products while converging to a single authoritative rendering architecture; remove architectural ambiguity without sacrificing frame retirement correctness
**Constraints**: No runtime fallback or compatibility rendering path in the accepted DX12 mainline; Editor and Game must share one authoritative frame pipeline; `Driver` remains the repository graphics entry point per constitution but must lose mixed render orchestration duties
**Scale/Scope**: Multi-subsystem rendering architecture work spanning `Runtime/Rendering`, `Runtime/Engine/Rendering`, `Project/Editor/Rendering`, `Project/Editor/Panels`, `Project/Game`, and `Tests/Unit`

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- `Spec-First Major Changes`: Pass. This feature has a dedicated spec bundle under `specs/008-ue5-dx12-render-alignment/`.
- `Validation Matches The Subsystem`: Pass. The plan requires targeted unit tests, DX12 runtime validation, RenderDoc evidence, and explicit startup-failure validation.
- `Generated Code And Backend Boundaries Are Sacred`: Pass. No generated files under `Runtime/*/Gen/` are touched. Backend execution logic is intentionally being pushed down into DX12-specific implementations and narrow interfaces.
- `Incremental, Verified Delivery`: Pass. The plan is split into explicit phases: baseline audit, forbidden-path removal, ownership convergence, RDG authority, editor/runtime unification, DX12 execution closure, and central-infrastructure enforcement.
- `Product Runtime Preservation`: Pass with a strict scope. Editor and Game remain in scope throughout, but only the DX12 product path remains active during this phase.

## Project Structure

### Documentation (this feature)

```text
specs/008-ue5-dx12-render-alignment/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── render-architecture-contract.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/
├── Editor/
│   ├── Core/
│   ├── Panels/
│   └── Rendering/
└── Game/
    └── Core/

Runtime/
├── Engine/
│   └── Rendering/
│       ├── BaseSceneRenderer.*
│       ├── DeferredSceneRenderer.*
│       ├── ForwardSceneRenderer.*
│       ├── FrameGraphSceneTargets.h
│       └── ScenePassSchemas.h
└── Rendering/
    ├── Context/
    │   ├── Driver.*
    │   ├── DriverAccess.h
    │   └── ThreadedRenderingLifecycle.*
    ├── Core/
    │   ├── ABaseRenderer.*
    │   ├── CompositeRenderer.*
    │   ├── FrameObjectBindingProvider.*
    │   └── RendererStats.*
    ├── FrameGraph/
    │   ├── FrameGraphExecutionContext.h
    │   ├── FrameGraphExecutionPlan.h
    │   ├── FrameGraphBuffer.cpp
    │   └── FrameGraphTexture.cpp
    ├── RHI/
    │   ├── Backends/
    │   │   ├── DX12/
    │   │   └── RHIDeviceFactory.cpp
    │   ├── Core/
    │   └── Utils/
    │       ├── DescriptorAllocator/
    │       ├── PipelineCache/
    │       └── ResourceStateTracker/
    └── Settings/
        └── GraphicsBackendUtils.h

Tests/
└── Unit/
    ├── CompositeRendererExplicitDrawOrderTests.cpp
    ├── FrameGraphSceneTargetsTests.cpp
    ├── GraphicsBackendUtilsTests.cpp
    ├── PanelWindowHookTests.cpp
    ├── RendererFrameObjectBindingTests.cpp
    └── ThreadedRenderingLifecycleTests.cpp
```

**Structure Decision**: Keep the implementation inside the existing engine layout and retain `Driver` as the top-level graphics entry point to satisfy repository governance. The architectural change is internal: `Driver` becomes a narrow entry and coordination seam, while Render Thread, RDG, and RHI Thread ownership become the only authoritative frame pipeline beneath it.

## UE Alignment Baseline

The feature aligns to these public UE architectural contracts:

- explicit threaded rendering boundaries between Game Thread, Render Thread, and RHI execution surfaces,
- RDG as the scheduling and resource-lifetime authority,
- graph-visible external resource import/extract,
- editor rendering surfaces participating in RenderThread/RDG flows,
- centralized PSO policy and render-resource lifecycle management.

Because public documentation does not prove source-level parity, the plan includes a source-audit task before closure.

## Repository Starting-State Tension

- The current repository document [Docs/Rendering/RHIMultiBackendArchitecture.md](D:/VSProject/Nullus/Docs/Rendering/RHIMultiBackendArchitecture.md) still describes `DX12` and `Vulkan` as active validated Windows runtime backends.
- This feature intentionally narrows the active phase-1 execution surface to `DX12` only so the ownership, RDG, and editor-path cleanup can converge without preserving a live compatibility matrix.
- The multi-backend architecture document therefore must be updated early in this feature to distinguish:
  - the long-term architecture goal of backend-neutral boundaries,
  - from the short-term phase-1 runtime truth that only DX12 remains active while the aligned mainline is being closed.
- Existing partial threading or graph files already present in the worktree do not count as closure evidence by themselves; the acceptance bar remains the contracts, task plan, tests, runtime evidence, and source audit defined in this bundle.

## Forbidden Mainline Behavior

These behaviors are treated as defects if they remain in the accepted DX12 phase:

- runtime direct-submit fallback
- main-thread explicit frame recording in normal rendering
- driver-built fallback scene packages
- compatibility acquire/present behavior
- editor-only submit or readback bypasses
- renderer-local scheduling truth that bypasses RDG
- optional adoption of pipeline, descriptor, or transient-lifetime systems

## Migration Strategy

### Phase A: Baseline And Red-Line Definition

Codify the UE-alignment baseline, DX12-only phase gate, and forbidden-path test surface so the cleanup can be measured instead of argued.

### Phase B: Ownership Convergence

Move frame publication, frame build, backend submission, and retirement to explicit thread-owned artifacts and services while shrinking mixed `Driver` responsibilities.

### Phase C: RDG Authority

Make compiled graph output the only scheduling truth, and reduce renderer-local schemas to descriptive metadata rather than executable policy.

### Phase D: Editor And Runtime Unification

Move scene view, game view, offscreen, picking, gizmo, and overlays into the same frame pipeline with graph-visible work and retirement.

### Phase E: DX12-Only Execution Closure

Remove compatibility execution and make DX12 startup failure explicit rather than permissive.

### Phase F: Mandatory Central Infrastructure

Make PSO, descriptor, transient lifetime, and retirement systems mandatory and diagnosable as the only accepted low-level path.

## Validation Strategy

- Unit tests prove ownership boundaries, forbidden-path removal, graph authority, editor/runtime unification, and central-infrastructure adoption.
- Runtime smoke proves DX12 Editor and Game remain runnable on the new path.
- RenderDoc captures back visible-correctness claims.
- Explicit DX12 startup-failure validation proves no alternate runtime path survives.
- Final closure additionally requires a documented UE source audit.

## Complexity Tracking

No constitution violations are required. Complexity is handled by deleting forbidden architecture rather than keeping multiple render paths alive in parallel.
