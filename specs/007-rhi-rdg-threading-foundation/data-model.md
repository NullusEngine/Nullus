# Data Model: UE5-Style RHI/RDG Threading Foundation

## RHIDeviceCapabilities

`RHIDeviceCapabilities` remains the authoritative runtime-facing capability snapshot for a backend/device pair.

### Existing fields that remain relevant

- `backendReady`
- `supportsGraphics`
- `supportsCompute`
- `supportsSwapchain`
- `supportsCurrentSceneRenderer`
- `supportsOffscreenFramebuffers`
- `supportsFramebufferReadback`
- `supportsEditorPickingReadback`
- `supportsUITextureHandles`
- `supportsCubemaps`
- `supportsMultiRenderTargets`
- `supportsExplicitBarriers`

### New phase-1 foundation fields

- `supportsAsyncCompute`
  - Meaning: The engine-wired backend path can schedule compute work asynchronously from graphics with explicit synchronization.
- `supportsDedicatedComputeQueue`
  - Meaning: The current backend/device path exposes a compute queue that can be owned separately from graphics work for the new architecture.
- `supportsCopyQueue`
  - Meaning: The current backend/device path exposes an explicit copy queue that can participate in engine-managed submission.
- `supportsParallelCommandRecording`
  - Meaning: Independent work can be recorded in parallel and later merged into ordered submission by the engine mainline.
- `supportsParallelCommandTranslation`
  - Meaning: The engine can translate graph work into backend command buffers in parallel without bypassing ownership rules.
- `supportsTransientResourceAllocator`
  - Meaning: The backend participates in an explicit transient resource lifetime path suitable for RDG-owned temporary resources.
- `supportsCentralizedDescriptorManagement`
  - Meaning: Descriptor lifetime and reuse are owned by the centralized descriptor allocator path rather than ad hoc backend-local allocation only.
- `supportsPipelineStateCache`
  - Meaning: Graphics and compute PSO reuse is serviced through centralized cache lookup and diagnostics.

### Semantics

- These fields describe **engine-wired support**, not raw API potential.
- A backend may support lower-level API concepts but still report `false` until Nullus owns submission, lifetime, and diagnostics on that path.
- Derived helpers in `GraphicsBackendUtils.h` use these fields to decide whether a backend is ready for the UE5-style foundation slices.

## Derived Readiness Helpers

### Tier A Foundation Ready

A backend is considered ready for the UE5-style foundation baseline when it can satisfy:

- backend readiness,
- graphics and compute support,
- swapchain and current scene renderer support,
- offscreen framebuffer and MRT support,
- explicit barrier support,
- centralized descriptor management,
- centralized PSO cache participation.

This deliberately does **not** imply async compute or parallel recording are already enabled.

### Render Graph Transient Resource Ready

Transient render-graph lifetime readiness additionally requires:

- `supportsTransientResourceAllocator`

### Async Compute Ready

Async compute readiness additionally requires:

- `supportsAsyncCompute`
- `supportsDedicatedComputeQueue`

### Parallel Recording Ready

Parallel recording readiness additionally requires:

- `supportsParallelCommandRecording`
- `supportsParallelCommandTranslation`

## Validation Targets

- `GraphicsBackendUtilsTests.cpp` validates helper semantics and prevents capability regressions.
- Backend factory creation points set the truth source for DX12 and Vulkan capability values.
