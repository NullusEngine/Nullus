# Implementation Plan: Multi-Backend RHI Mainline

**Branch**: `multi-backend-rhi-mainline` | **Date**: 2026-03-27 | **Spec**: `/specs/multi-backend-rhi-mainline/spec.md`
**Input**: Feature specification from `/specs/multi-backend-rhi-mainline/spec.md`

## Summary

Complete the formal RHI mainline so `Driver`, renderer, frame graph, material, and wrapper code consume one `RHI/Core` contract, while `DX12` and `Vulkan` become Tier A explicit backends, `DX11` and `OpenGL` remain supported through the same formal RHI entry surface as Tier B backends, and both `Editor` and `Game` stay runnable with validated rendering correctness.

## Technical Context

**Language/Version**: C++20, HLSL/GLSL shader assets, CMake  
**Primary Dependencies**: Nullus runtime/editor modules, backend-specific graphics APIs, RenderDoc tooling, ImGui backend bridge code  
**Storage**: Repository source files plus committed spec bundle under `specs/multi-backend-rhi-mainline/`  
**Testing**: `NullusUnitTests`, targeted runtime smoke validation, RenderDoc captures, focused backend correctness checks  
**Target Platform**: Windows-first backend bring-up with explicit backend-specific notes; cross-platform claims remain limited to validated evidence  
**Project Type**: Native engine and editor repository  
**Performance Goals**: Preserve current scene-rendering viability while transitioning the mainline to formal RHI; do not regress frame startup, swapchain present, or editor/game interactivity during smoke validation  
**Constraints**: Do not hand-edit generated output under `Runtime/*/Gen/`; do not assume one backend proves another; keep `Driver` as the entry point; keep `Editor` and `Game` runnable during staged migration  
**Scale/Scope**: Rendering architecture change spanning `Runtime/Rendering`, `Runtime/Engine/Rendering`, `Project/Editor`, `Project/Game`, tests, demos, docs, and backend capability reporting

## Constitution Check

This plan satisfies the current repository workflow if it:

- Treats the work as a major rendering change and keeps all planning artifacts in `specs/multi-backend-rhi-mainline/`
- Preserves the rule that `Runtime/*/Gen/` remains generated output
- Uses RenderDoc-first validation for Tier A rendering correctness and records backend-specific evidence
- Avoids claiming cross-backend or cross-platform correctness without explicit validation
- Uses the existing build, test, and application entrypoints instead of inventing a parallel rendering workflow

No constitution violations are required by the current plan. The main risk is execution complexity, not process non-compliance.

## Project Structure

### Documentation (this feature)

```text
specs/multi-backend-rhi-mainline/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/
├── Editor/
│   ├── Core/
│   ├── Panels/
│   └── Rendering/
├── RenderingDemos/      # new
└── Game/
    └── Core/

Runtime/
├── Engine/
│   └── Rendering/
├── Rendering/
│   ├── Context/
│   ├── Core/
│   ├── FrameGraph/
│   ├── Resources/
│   ├── RHI/
│   │   ├── Backends/
│   │   │   ├── DX12/
│   │   │   ├── DX11/          # new
│   │   │   ├── OpenGL/
│   │   │   ├── Vulkan/
│   │   │   └── Null/
│   │   ├── Core/
│   │   └── Utils/
│   ├── Settings/
│   ├── ShaderCompiler/
│   └── Tooling/
└── UI/

Tests/
└── Unit/

Docs/
└── Rendering/
```

**Structure Decision**: Keep the main architectural split already established under `Runtime/Rendering/RHI/Core` and `Runtime/Rendering/RHI/Backends`. Add `DX11` as a new backend slice under `Runtime/Rendering/RHI/Backends/DX11`, add a new `Project/RenderingDemos/` area for the minimum demo set, keep renderer and frame graph migration inside their existing modules, and centralize validation notes under the committed spec bundle plus `Docs/Rendering/`.

## Backend Tier Model

### Tier A Backends

- `Runtime/Rendering/RHI/Backends/DX12/`
- `Runtime/Rendering/RHI/Backends/Vulkan/`

These backends must own the formal explicit RHI execution path directly:

- `RHIDevice`
- `RHIQueue`
- `RHISwapchain`
- `RHIFence`
- `RHISemaphore`
- `RHICommandPool`
- `RHICommandBuffer`
- `RHIGraphicsPipeline` and `RHIComputePipeline`
- `RHIBindingLayout` and `RHIBindingSet`
- `RHIBuffer`, `RHITexture`, `RHITextureView`, upload, barrier, and copy behavior

Tier A backends are the primary proof that the formal RHI architecture is real rather than compatibility-backed.

### Tier B Backends

- `Runtime/Rendering/RHI/Backends/DX11/` (new)
- `Runtime/Rendering/RHI/Backends/OpenGL/`

These backends must enter through the same formal `RHIDevice`-based API as Tier A, but may retain translation layers or compatibility-backed internals where the backend model is constrained. Their limits must be documented in a capability matrix rather than hidden in renderer-side legacy branching.

## Mainline Migration Strategy

### 1. Freeze The Architectural Entry Point In `Driver`

`Runtime/Rendering/Context/Driver.cpp` and `Runtime/Rendering/Context/Driver.h` remain the authoritative place for:

- backend selection
- render device and explicit device creation
- swapchain lifecycle
- per-frame context creation
- queue submit and present ownership
- RenderDoc capture plumbing

The plan does not move these responsibilities elsewhere. Instead, it shifts the implementation behind them from legacy `IRenderDevice` calls toward formal RHI objects.

### 2. Migrate The Rendering Mainline Before Declaring Backend Victory

The renderer side must converge in this order:

- `Runtime/Rendering/Resources/Material.*`
- `Runtime/Rendering/Core/ABaseRenderer.*`
- `Runtime/Rendering/FrameGraph/*`
- `Runtime/Rendering/Resources/Texture.*` and buffer wrappers
- `Runtime/Engine/Rendering/*`
- `Project/Editor/Rendering/*`

The goal is that these modules consume formal pipeline, binding, resource, and command abstractions regardless of which backend is underneath.

### 3. Upgrade Backend Execution Under That Mainline

Tier A backend work follows the dependency chain needed by the mainline:

1. native queue, swapchain, and sync ownership
2. native command pool and command buffer ownership
3. pipeline, binding, and resource creation
4. barrier, copy, upload, and shared utility integration

This prevents the project from landing backend-heavy work that the renderer mainline still cannot consume directly.

### 4. Downgrade `IRenderDevice` To Compatibility Infrastructure

`Runtime/Rendering/RHI/IRenderDevice.h` remains temporarily available for:

- Tier B adaptation
- residual glue during staged migration
- backend-internal fallback support

It must stop acting as the primary renderer contract. The plan treats removal of renderer-side dependence on its immediate draw and state APIs as a core success condition.

## Planned Workstreams

### Workstream A - Backend Surface And Configuration

Scope:

- add `DX11` to backend enums, parsing, launcher, and runtime configuration
- update backend factories and native backend reporting
- ensure `Driver`, `Editor`, `Game`, and RenderDoc/tooling understand the expanded backend matrix

Primary files:

- `Runtime/Rendering/Settings/EGraphicsBackend.h`
- `Runtime/Rendering/Settings/GraphicsBackendUtils.h`
- `Runtime/Rendering/Settings/DriverSettings.h`
- `Runtime/Rendering/RHI/RHITypes.h`
- `Runtime/Rendering/RHI/Backends/RenderDeviceFactory.*`
- `Runtime/Rendering/RHI/Backends/ExplicitDeviceFactory.*`
- `Project/Editor/Core/*`
- `Project/Game/Core/*`

### Workstream B - Formal RHI Mainline Migration

Scope:

- remove renderer-mainline dependence on `GraphicsPipelineDesc` and `BindingSetInstance`
- make material output formal RHI pipeline and binding objects
- finish frame graph migration to formal resources, views, barriers, and render-pass descriptions
- align wrappers and engine rendering code with formal RHI resource ownership

Primary files:

- `Runtime/Rendering/Resources/Material.*`
- `Runtime/Rendering/Core/ABaseRenderer.*`
- `Runtime/Rendering/FrameGraph/*`
- `Runtime/Rendering/Resources/Texture.*`
- `Runtime/Rendering/Buffers/*`
- `Runtime/Engine/Rendering/*`
- `Project/Editor/Rendering/*`

### Workstream C - Tier A Backend Completion

Scope:

- replace compatibility-backed `DX12ExplicitDeviceFactory` and `VulkanExplicitDeviceFactory` implementations with backend-owned formal RHI devices
- integrate queue, swapchain, synchronization, command recording, pipeline creation, resources, and shared utilities
- preserve Editor/Game viability while backend internals are replaced

Primary files:

- `Runtime/Rendering/RHI/Backends/DX12/*`
- `Runtime/Rendering/RHI/Backends/Vulkan/*`
- `Runtime/Rendering/RHI/Utils/DescriptorAllocator/*`
- `Runtime/Rendering/RHI/Utils/PipelineCache/*`
- `Runtime/Rendering/RHI/Utils/ResourceStateTracker/*`
- `Runtime/Rendering/RHI/Utils/UploadContext/*`

### Workstream D - Tier B Backend Formal Entry Support

Scope:

- create a new `DX11` backend slice
- keep `OpenGL` available through formal RHI entry points
- ensure neither backend requires the renderer mainline to fall back to legacy immediate rendering paths
- document and expose capability limitations

Primary files:

- `Runtime/Rendering/RHI/Backends/DX11/*`
- `Runtime/Rendering/RHI/Backends/OpenGL/*`
- `Runtime/Rendering/RHI/ExplicitRHICompat.cpp`
- `Runtime/Rendering/RHI/Backends/OpenGL/Compat/*`
- `Runtime/UI/*`

### Workstream E - Validation, Demos, And Delivery Surface

Scope:

- maintain runnable `Editor` and `Game`
- add minimum demo and smoke coverage
- add targeted correctness checks
- record backend-specific evidence, docs, and capability matrix

Primary files:

- `Project/Editor/*`
- `Project/RenderingDemos/*`
- `Project/Game/*`
- `Tests/Unit/*`
- `Docs/Rendering/*`
- `Tools/RenderDoc/*`
- `specs/multi-backend-rhi-mainline/*`

## Phase Outline

### Phase 0 - Baseline And Backend Matrix Preparation

- add `DX11` to backend selection and runtime settings
- define backend tier language in docs and spec artifacts
- identify current explicit and legacy touchpoints that still block formal mainline migration

### Phase 1 - Mainline Contract Cleanup

- migrate material, renderer, frame graph, and wrappers so their mainline contracts are formal RHI-first
- isolate or retire renderer-side uses of `GraphicsPipelineDesc`, `BindingSetInstance`, and immediate `IRenderDevice` draw-state APIs
- keep `Driver` authoritative for frame lifecycle while reducing legacy exposure

### Phase 2 - Tier A Native Explicit Backend Completion

- replace compatibility-backed `DX12` and `Vulkan` explicit devices with backend-owned formal devices
- finish native queue, swapchain, fence, semaphore, command pool, and command buffer ownership
- then complete pipeline, binding, resource, barrier, copy, upload, and shared utility integration

### Phase 3 - Tier B Formal Entry Alignment

- create `DX11` backend support
- adapt `OpenGL` and `DX11` to formal RHI entry without restoring renderer legacy mainlines
- expose backend limitations through capability reporting and docs

### Phase 4 - Product Validation And Correctness Evidence

- validate `Editor` and `Game` on each supported backend
- gather RenderDoc captures for Tier A
- perform focused runtime checks for Tier B
- confirm representative rendering correctness for geometry, materials, textures, depth, offscreen passes, and final present

### Phase 5 - Delivery Surface Completion

- add minimum demo set
- add smoke matrix and capability matrix
- update docs and spec bundle validation notes

## Validation Strategy

### Automated Validation

- build the affected runtime, `Editor`, `Game`, `NullusUnitTests`, and `ReflectionTest`
- extend `Tests/Unit` with focused correctness tests where stable entrypoints exist
- add backend capability assertions where possible

### Runtime Validation

- launch `Editor` and `Game` through the supported backend matrix
- validate startup, swapchain creation, resource loading, frame loop, resize handling, and present
- use representative scenes rather than synthetic empty windows only

### Render Correctness Validation

- Tier A: RenderDoc capture and inspect pass order, state transitions, bindings, and present
- Tier B: focused runtime verification with explicit notes about weaker evidence
- use demos and real product paths together, not as substitutes for one another

### Documentation And Review Output

- maintain a capability matrix for `DX12`, `Vulkan`, `DX11`, and `OpenGL`
- maintain a smoke matrix covering demos plus `Editor` and `Game`
- record actual evidence, not aspirational support claims

## Risks And Mitigations

- **Risk**: The renderer mainline keeps accidental dependencies on legacy APIs while backend work appears complete.
  **Mitigation**: Treat removal of `GraphicsPipelineDesc` and `BindingSetInstance` from the mainline as explicit tracked tasks and review gates.

- **Risk**: `DX11` introduction expands scope late because backend selection, UI, and product startup paths assume only four backends.
  **Mitigation**: Land backend enum, parsing, and product configuration work early in Phase 0.

- **Risk**: `Editor` and `Game` remain runnable on one backend but regress on others during staged migration.
  **Mitigation**: Keep backend-specific smoke validation in every phase and record backend-by-backend evidence rather than one aggregate pass.

- **Risk**: Tier B backends quietly diverge from the formal contract.
  **Mitigation**: Require capability matrix entries and explicit degraded-behavior documentation for every known limitation.

- **Risk**: Shared utilities remain disconnected helpers rather than real backend infrastructure.
  **Mitigation**: Tie utility integration to the Tier A backend completion phase rather than treating it as optional cleanup.

## Complexity Tracking

No planned complexity exception is required. The work is large, but the architecture stays within the repository's intended rendering workflow: one committed spec bundle, incremental backend migration, targeted validation, and no generated-file edits.
