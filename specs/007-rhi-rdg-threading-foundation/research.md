# Research: UE5-Style RHI/RDG Threading Foundation

## Decision 1: Capability reporting must describe engine-wired support, not raw API possibility

- **Decision**: New RHI capability fields will describe what the current Nullus backend path can safely use through the engine mainline, not what DX12 or Vulkan could theoretically do with future work.
- **Why**: Existing `RHIDeviceCapabilities` already gate runtime/editor support. If async compute or parallel recording are reported as available before the engine owns the queue, synchronization, and lifetime path, later rollout logic will overclaim support and create architecture forks.
- **Consequence**: DX12 and Vulkan can report `supportsCompute = true` while still reporting `supportsAsyncCompute = false` until dedicated queue wiring and cross-queue retirement are implemented.

## Decision 2: DX12 and Vulkan are the only Tier A backends for this feature

- **Decision**: Phase 1 explicitly targets DX12 and Vulkan. DX11, OpenGL, and Metal remain legacy or degraded paths for this feature bundle.
- **Why**: UE5-style Render Thread, RHI Thread, RDG, explicit barriers, descriptor management, and async compute map naturally onto explicit APIs first. Requiring full parity on implicit or older backends would block core architecture progress.
- **Consequence**: New foundation gating helpers must distinguish Tier A readiness from legacy backend runtime support.

## Decision 3: Rollout is staged from capability truth to execution ownership

- **Decision**: Implementation proceeds in this order:
  1. truthful capability model and backend gating,
  2. Render Thread and RHI Thread ownership convergence,
  3. RDG authoritative compilation and resource lifetime,
  4. parallel command recording,
  5. async compute,
  6. PSO and descriptor centralization diagnostics.
- **Why**: This minimizes overreach. Each later slice depends on the previous slice exposing truthful state and stable ownership.
- **Consequence**: Early slices may add new gating helpers and tests before visible frame execution changes.

## Decision 4: Async compute readiness requires more than compute shader support

- **Decision**: Async compute is treated as a separate readiness signal from generic compute support.
- **Why**: UE5-style async compute needs queue intent, compile-time pass eligibility, explicit synchronization, and frame retirement rules. A single-queue graphics+compute backend path does not satisfy that contract.
- **Consequence**: Backends can advertise compute without async compute until queue separation and scheduling exist.

## Decision 5: Descriptor and PSO management stay centralized even before full backend-native specialization

- **Decision**: Nullus will keep centralized `PipelineCache` and `DescriptorAllocator` ownership in the runtime path and evolve them toward backend-aware diagnostics instead of scattering allocation and cache policy inside renderers.
- **Why**: UE5-style rendering relies on stable lifetime and reuse infrastructure that is visible at the engine core, not buried in per-pass code.
- **Consequence**: Capability reporting must identify whether a backend participates in the centralized PSO/descriptor path, even if the implementation is still an initial version.
