# Multi-Backend Explicit RHI Architecture

## Scope

This document defines the active migration target for the engine rendering architecture:

- `DX12`, `Vulkan`, and `Metal` are the primary explicit backends.
- `OpenGL` stays as a fallback backend on the same RHI surface.
- `runtime` and `editor` consume the same device, queue, swapchain, command buffer, binding, texture, and framebuffer semantics.
- backend-specific APIs stay inside backend implementations.

The old immediate-style `IRenderDevice` path is now a migration bridge only. New rendering work must target the explicit RHI object model first and use compatibility shims only while older passes are being moved over.

## Backend Tiers

### Primary

- `DX12`
- `Vulkan`
- `Metal`

Primary backends are expected to receive new graphics capabilities first:

- offscreen rendering
- framebuffer readback
- cubemap sampling
- MRT
- depth blit
- editor UI texture presentation

### Compatibility

- `OpenGL`

OpenGL must remain functional for forward rendering, editor views, UI textures, cubemaps, and tooling passes, but it is not the leading implementation target for new graphics features. The API is designed from explicit backends outward; OpenGL adapts to that model rather than shaping it.

## Binding Model

The binding model is fixed and must not drift between backends:

- `set0`: `Frame / Scene`
- `set1`: `Material`
- `set2`: `Object`
- `set3`: `Pass / Rare`

Resource kinds covered by reflection and runtime binding:

- constant buffers
- sampled textures
- samplers
- structured/storage buffers

Material-space data belongs to `Material`.
Pass constants and pass textures belong to renderer-controlled pass bindings, not to material instances.

## Shader Ownership

- HLSL is the only maintained shader source.
- GLSL, DXIL, and SPIR-V are generated artifacts.
- Reflection is generated from compiled artifacts and consumed by runtime binding code.

No new rendering feature may land as a maintained `HLSL + handwritten GLSL` pair.

## Public RHI Contracts

The public RHI must expose first-class objects for:

- `RHIAdapter`, `RHIDevice`, `RHIQueue`, `RHISwapchain`
- `RHIBuffer`, `RHITexture`, `RHITextureView`, `RHISampler`
- `RHIShaderModule`, `RHIBindingLayout`, `RHIBindingSet`, `RHIPipelineLayout`
- `RHIGraphicsPipeline`, `RHIComputePipeline`
- `RHICommandPool`, `RHICommandBuffer`
- `RHIFence`, `RHISemaphore`

Required command buffer coverage:

- `Begin`, `End`, `Reset`
- `BeginRenderPass`, `EndRenderPass`
- `SetViewport`, `SetScissor`
- `BindGraphicsPipeline`, `BindComputePipeline`, `BindBindingSet`, `PushConstants`
- `BindVertexBuffer`, `BindIndexBuffer`
- `Draw`, `DrawIndexed`, `Dispatch`
- `CopyBuffer`, `CopyBufferToTexture`, `CopyTexture`
- `Barrier`

Required synchronization model:

- `Fence` for CPU <-> GPU completion
- `Semaphore` for acquire/present and queue-to-queue ordering
- `Barrier` for resource state transitions and hazards

## Capability Matrix

Each backend must report explicit support for:

- swapchain
- offscreen framebuffer
- framebuffer readback
- framebuffer blit
- depth blit
- UI texture handles
- cubemaps
- MRT
- compute queue
- copy queue
- explicit barriers

Editor startup and runtime feature selection must consume these capabilities rather than guessing from backend name alone.

## Migration Boundary

- `Driver::GetExplicitDevice()`, `Driver::BeginExplicitFrame()`, and `Driver::EndExplicitFrame()` are the only public bridge points for the migration.
- `ExplicitRHICompat` exists only to move legacy renderer code onto the new frame orchestration model. It must not become the place where new rendering features are added.
- `FrameGraphExecutionContext` and resource wrappers should carry explicit-RHI objects or migration shims, not expose `IRenderDevice&` directly.
- Renderer instantiation must still go through one selection entry point so backend choice and renderer choice remain centralized.

## Acceptance Matrix

### Runtime

- launch and render default scene on `OpenGL`, `DX12`, `Vulkan`, `Metal`
- validate basic materials, skybox, cubemap sampling, debug shapes, resize, and scene switching

### Editor

- `DX12`, `Vulkan`, and `Metal` must run without fallback
- `SceneView` and `GameView` must display valid imagery
- launcher/logo and regular UI textures must render correctly
- `picking`, `outline`, `gizmo`, `grid`, and `billboard` must remain interactive

### Architecture Gates

- backend-external code must not call backend-native APIs directly
- new passes must not reintroduce `SetUniform*`, shader program handles, or handwritten binding slots
- new renderer work must not add dependencies on `Driver::Draw`, `Driver::BindGraphicsPipeline`, or raw backend texture/buffer IDs as primary resource handles
