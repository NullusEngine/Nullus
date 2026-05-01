# Multi-Backend Explicit RHI Architecture

## Scope

This document defines the active migration target for the engine rendering architecture:

- During the UE5 DX12 alignment phase, `DX12` is the only active runtime backend exposed for Editor/Game on the validated Windows path.
- `Vulkan` remains in source only as a future backend implementation target behind backend-neutral architecture boundaries; it is not an active runtime path during this phase.
- `DX11` and `OpenGL` remain in the source tree, but startup now gates them unsupported until their runtime path and evidence are repaired.
- `Metal` is explicitly unsupported on non-Apple builds and must not be exposed through null-device fallback paths.
- `runtime` and `editor` consume the same device, queue, swapchain, command buffer, binding, texture, and framebuffer semantics.
- backend-specific APIs stay inside backend implementations.

The old immediate-style `IRenderDevice` path is now a migration bridge only. New rendering work must target the explicit RHI object model first and use compatibility shims only while older passes are being moved over.

## Backend Tiers

### Active Phase-1 Runtime Backend

- `DX12` - Full native explicit RHI implementation

This phase intentionally permits only one accepted runtime mainline so threaded ownership, RDG authority, editor-path unification, and startup failure behavior can converge without compatibility branches.

### Preserved Future Backend Implementations

- `Vulkan` - Kept in source as a future backend implementation target, but intentionally gated unsupported for the active UE5 DX12 alignment phase

Only `DX12` may currently be reported as supported in docs, startup messaging, or UI/backend-selection flows for the active alignment phase.

Validated backends receive new graphics capabilities first:
- offscreen rendering
- framebuffer readback
- cubemap sampling
- MRT
- depth blit
- editor UI texture presentation

### Gated Or Unsupported Backends

- `DX11` - partial explicit-device path exists, but sampler, binding layout, binding set, and pipeline layout support are still incomplete
- `OpenGL` - current Windows startup smoke still exits early, so it is gated unsupported instead of being used as a fallback
- `Metal` - unsupported on non-Apple builds; Apple-native device/presentation work still needs separate validation

These backends must fail explicitly or stay hidden from supported-runtime claims until their implementation and evidence are restored.

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
Capabilities alone are not sufficient to keep a backend exposed: current smoke evidence and, for supported explicit backends, RenderDoc evidence are required before a support claim is truthful.

## Migration Boundary

- `Driver::GetExplicitDevice()`, `Driver::BeginExplicitFrame()`, and `Driver::EndExplicitFrame()` are the only public bridge points for the migration.
- `ExplicitRHICompat` exists only to move legacy renderer code onto the new frame orchestration model. It must not become the place where new rendering features are added.
- `FrameGraphExecutionContext` and resource wrappers should carry explicit-RHI objects or migration shims, not expose `IRenderDevice&` directly.
- Renderer instantiation must still go through one selection entry point so backend choice and renderer choice remain centralized.

## Implementation Status

### Native Backend Implementations

The following formal RHI objects are implemented natively in Tier A backends:

**DX12** (`Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`):
- `NativeDX12Buffer` with `GetGPUAddress()` for vertex/index buffer binding
- `NativeDX12Texture`, `NativeDX12Sampler`, `NativeDX12BindingLayout`, `NativeDX12BindingSet`
- `NativeDX12PipelineLayout`, `NativeDX12ShaderModule`, `NativeDX12GraphicsPipeline`, `NativeDX12ComputePipeline`
- `NativeDX12CommandPool`, `NativeDX12CommandBuffer`, `NativeDX12Fence`, `NativeDX12Semaphore`

**Vulkan** (`Runtime/Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.cpp`):
- `NativeVulkanBuffer` with `GetNativeBufferHandle()` for vertex/index buffer binding
- `NativeVulkanTexture`, `NativeVulkanSampler`, `NativeVulkanBindingLayout`, `NativeVulkanBindingSet`
- `NativeVulkanPipelineLayout`, `NativeVulkanShaderModule`, `NativeVulkanGraphicsPipeline`, `NativeVulkanComputePipeline`
- `NativeVulkanCommandPool`, `NativeVulkanCommandBuffer`, `NativeVulkanFence`, `NativeVulkanSemaphore`

### Current Windows Runtime Matrix For UE5 DX12 Alignment Phase (2026-04-21)

- `DX12` - supported; this is the only accepted active runtime backend for the current alignment phase
- `Vulkan` - preserved in source but intentionally gated unsupported while the DX12-only authoritative mainline is being closed
- `DX11` - gated unsupported; startup now reports explicit unsupported warnings and exits cleanly instead of attempting any alternate runtime route
- `OpenGL` - gated unsupported on the current Windows build; startup now reports explicit unsupported warnings and exits cleanly instead of attempting any alternate runtime route
- `Metal` - unsupported on non-Apple builds

### Utilities Integration

- `DescriptorAllocator`, `PipelineCache`, `ResourceStateTracker`, `UploadContext` are created by Driver and stored in per-frame `RHIFrameContext`
- These utilities work with formal RHI resources created by native devices

### UE5 DX12 Alignment Status (2026-04-23)

- The active DX12-aligned mainline now requires central descriptor allocation for explicit binding-set creation.
  - Missing allocator state is treated as a rejected mainline condition, not as permission to return a raw backend binding set.
- Graphics and compute PSO acquisition stay on `PipelineCache`.
  - Render code may supply backend creation lambdas to the cache, but accepted-path code no longer keeps direct PSO fallback branches outside the cache-owned flow.
- Transient lifetime and readback visibility stay on `ResourceStateTracker` plus frame-graph/external-resource bridging.
- Threaded diagnostics expose descriptor, PSO, transient-lifetime, and retirement mainline activity through `ThreadedFrameTelemetry`, `RendererStats`, and `FrameInfo`.

## Acceptance Matrix

### Runtime

- `DX12` must launch and survive smoke for both `Editor` and `Game`
- `Vulkan` must remain behind backend-neutral architecture boundaries and must not be exposed as an active runtime path during this phase
- `DX11`, `OpenGL`, and `Metal` must report explicit unsupported/gated behavior instead of crashing or relying on silent fallback
- backend support claims must be backed by direct smoke results plus RenderDoc capture evidence for supported explicit backends

### Editor

- `DX12` must run without fallback (Tier A)
- `Vulkan` editor execution is out of scope for the current alignment phase and must remain gated unsupported in runtime selection surfaces
- `DX11` and `OpenGL` must not be presented as supported on the current Windows matrix until their runtime path is repaired and revalidated
- `SceneView` and `GameView` must display valid imagery
- launcher/logo and regular UI textures must render correctly
- `picking`, `outline`, `gizmo`, `grid`, and `billboard` must remain interactive

### Architecture Gates

- backend-external code must not call backend-native APIs directly
- new passes must not reintroduce `SetUniform*`, shader program handles, or handwritten binding slots
- new renderer work must not add dependencies on `Driver::Draw`, `Driver::BindGraphicsPipeline`, or raw backend texture/buffer IDs as primary resource handles
- unsupported backends must not route through a crashing alternate runtime path; startup must log a truthful unsupported state and stop cleanly
