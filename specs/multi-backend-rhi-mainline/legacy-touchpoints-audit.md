# Legacy Touchpoints Audit

Date: 2026-03-28

This note records the remaining renderer-mainline touchpoints that still depend on legacy pipeline, binding, or immediate `IRenderDevice` behavior. It supports `T010` and will be updated as User Story 1 work lands.

## Material

- `Runtime/Rendering/Resources/Material.cpp`
  - `BuildGraphicsPipelineDesc()` still produces legacy `RHI::GraphicsPipelineDesc`.
  - `GetBindingSetInstance()` still exposes legacy `BindingSetInstance`.
  - `SetDepthWriting()` and `SetColorWriting()` still feed legacy-style state mutation patterns that downstream renderer/editor code relies on.
- Main implication:
  - `Material` remains dual-track. Formal-RHI production exists, but renderer callers can still select the legacy descriptor and binding path.

## Renderer

- `Runtime/Rendering/Core/ABaseRenderer.cpp`
  - Formal path exists via `BuildExplicitGraphicsPipelineDesc(...)` and `commandBuffer->BindGraphicsPipeline(...)`.
  - Legacy fallback still constructs `material->BuildGraphicsPipelineDesc()` and calls `m_driver.BindGraphicsPipeline(..., &material->GetBindingSetInstance())`.
- Main implication:
  - Renderer mainline is not yet formal-RHI-only. Pipeline creation and binding still branch back to legacy descriptors and driver-side immediate submission.

## Driver

- `Runtime/Rendering/Context/Driver.cpp`
  - Still forwards immediate operations such as `ActivateTexture()`, `BindFramebuffer()`, `DrawArrays*()`, `DrawElements*()`, `BindGraphicsPipeline()`, `SetColorWriting()`, `SetDepthWriting()`, `SetCapability()`, and `SetCullFace()`.
- Main implication:
  - `Driver` still exposes a renderer-facing legacy state machine instead of acting only as a formal-RHI device/frame entry point.

## Explicit Compatibility Layer

- `Runtime/Rendering/RHI/ExplicitRHICompat.cpp`
  - Still owns the main bridge between formal `RHIGraphicsPipeline` / `RHIBindingSet` / `RHICommandBuffer` objects and legacy `IRenderDevice`.
  - Still builds `GraphicsPipelineDesc`, merges `BindingSetInstance`, applies legacy state mutators, binds framebuffers, and issues `DrawArrays*()` / `DrawElements*()` calls.
- Main implication:
  - This file is currently the largest concentration of compatibility behavior and remains the boundary that must shrink as Tier A backends become native.

## Engine And Editor Integration

- `Runtime/Engine/Rendering/DeferredSceneRenderer.cpp`
  - Still binds framebuffer `0` directly in one path.
  - Still mutates material depth/color write state before lighting and target-copy passes.
- `Project/Editor/Rendering/OutlineRenderFeature.cpp`
  - Still disables color writing through material state.
- `Project/Editor/Rendering/GridRenderPass.cpp`
  - Still disables depth writing through material state.
- `Project/Editor/Rendering/DebugSceneRenderer.cpp`
  - Still disables depth writing through material state.
- `Project/Editor/Rendering/GizmoRenderFeature.cpp`
  - Still disables depth writing through material state.
- Main implication:
  - Product-facing rendering features still assume legacy material-state knobs and occasional driver framebuffer control.

## Priority Ordering For User Story 1

1. Move `Material` output and `ABaseRenderer` consumption to a single formal-RHI-first path.
2. Reduce renderer use of `Driver::BindGraphicsPipeline()` and driver immediate draw/state helpers.
3. Migrate engine/editor render features away from material state toggles and explicit default-framebuffer binds.
4. Confine `IRenderDevice` usage to `ExplicitRHICompat.cpp` and backend-internal compatibility code only.
