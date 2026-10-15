# Graphics Backends and Large-Scene Features

This page covers the backend support matrix, how capability is queried, and the large-scene features that depend on backend capability. The rule is that capability comes from device queries, not from backend names.

## Platform runtime backends

| Backend | Platform | Status |
| --- | --- | --- |
| DX12 | Windows | Supported runtime line |
| Vulkan 1.1 | Linux / WSL | Supported runtime line; the WSLg baseline is `llvmpipe` |
| Metal | macOS | Native implementation and build target; unsupported on non-Apple platforms |

## Gated backends

| Backend | Status |
| --- | --- |
| DX11 | Explicitly gated: startup reports it as unsupported and exits cleanly |
| OpenGL | Explicitly gated: on the current Windows build, startup reports it as unsupported and exits cleanly |
| Metal (non-Apple) | Unsupported, and not exposed through a null-device fallback |

These backends must not be used as delivery targets, and they are never silently substituted.

## Backend capability

Every backend must report these capabilities explicitly, and both editor startup and runtime feature selection read them:

- Swapchain
- Offscreen framebuffer
- Framebuffer readback
- Framebuffer blit
- Depth blit
- UI texture handles
- Cubemaps
- MRT
- Compute queue
- Copy queue
- Explicit barriers

Capability bits are necessary but not sufficient: claiming a backend works also requires current smoke evidence and, for supported explicit backends, RenderDoc evidence.

## Backend-independent RHI constraints

- `DriverRendererAccess` and `DriverUIAccess` are the supported entry points.
- The immediate-style device wrapper is gone and must not be reintroduced as an extension point for new features.
- Renderer instantiation must go through a single selection entry point so backend choice and renderer choice stay centralized.
- Resource state transitions are declared by the FrameGraph rather than special-cased per backend.

## Large-scene rendering

### Spatial index and visibility

The scene maintains a spatial index so visibility collection queries the index instead of scanning linearly. Index rebuilds use explicit telemetry, last-good data, dirty overlays, staged rebuilds, or clear budgets; a multi-frame synchronous rebuild that shows up as an unexplained stall is a defect.

The index supports serial, parallel, and full-scan comparison modes, and all three must produce identical visibility sets and sparse handle output.

### LOD and HLOD

- Static mesh LODs are built during import and the runtime only selects a level.
- LOD and HLOD decisions stay view local and never write shared state.
- HLOD children selected in the editor must remain inspectable.
- Transparent, order-dependent, skinned, and animated children do not participate in HLOD suppression until explicit proxy compatibility exists.

### HZB occlusion

HZB occlusion and async/readback-assisted history are selected from `RHIDevice::GetCapabilities()` and texture format capabilities, not from backend-name guesses.

Conservative requirements:

- Invalid, stale, moved, representation-changed, ineligible-depth, or capability-gated history always stays visible.
- Ordinary occlusion frames introduce no synchronous readback, blocking map, or CPU/GPU fence wait.
- Unsupported capability combinations stay conservative and record the fallback reason.

Validation method and required evidence are in [Visibility and Occlusion](../large-scenes/visibility-occlusion.md).

### Residency

The residency system separates the interest set from the actually resident set, deduplicates requests, enforces CPU/GPU memory and upload budgets, and preserves frame-retirement pins. See [Residency and Budgets](../large-scenes/streaming.md).

## Related tooling

- Use RenderDoc to inspect passes, resources, and state transitions; see [Profiling and Graphics Debugging](../quality/profiling.md).
- Use Tracy for pass times and queue distribution on the same page.
- Use focused unit tests for FrameGraph dependencies and occlusion contracts; see [Debugging and Testing](../quality/debugging.md).

## Next steps

- [Rendering Overview](overview.md) puts these features back into the whole frame.
- [Visibility and Occlusion](../large-scenes/visibility-occlusion.md) covers the evidence requirements for HZB.
