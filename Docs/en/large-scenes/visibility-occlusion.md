# Visibility and Occlusion

Large-scene performance problems are usually about computing too much rather than drawing too slowly. The visibility pipeline narrows the candidate set down to the objects that genuinely need submission before any draw work begins.

## Visibility pipeline

One solve consists of:

1. Pulling frustum candidates from the spatial index.
2. Filtering by representation and visibility rules.
3. Emitting a sparse set of primitive handles.
4. Handing the result to draw command construction.

The pipeline supports multiple execution modes for cross-checking. Serial, parallel, and full-scan paths must produce identical visibility sets and sparse handle output. Parallel paths use immutable snapshots and fall back safely when scheduling is unavailable.

Retained scene state must stay generation safe: primitive handles are scene-scoped, generation checked, and tombstoned on removal, and snapshots never retain live component pointers.

## Spatial index

Index rebuilds must not hide a full-table scan or a multi-frame synchronous rebuild. Acceptable approaches include:

- Explicit telemetry and counters so every rebuild's size is visible
- Keeping the previous good data and continuing to serve queries during a rebuild
- Dirty region marking and staged rebuilds
- A defined per-frame budget

A silent rebuild spike is a defect.

## HZB occlusion

Hierarchical Z-buffer occlusion is enabled conservatively on DX12 according to device capability:

- Capability comes from `RHIDevice::GetCapabilities()` and texture format capability, not from the backend name.
- Under unsupported capability combinations, primitives stay visible and the fallback reason is recorded.
- Ordinary occlusion frames introduce no synchronous readback, blocking map, or CPU/GPU fence wait.

### Conservative rules

Occlusion is skipped, leaving objects visible, when:

- History data is invalid or stale
- The object moved this frame
- The object's render representation changed
- Depth information is not eligible
- Device capability is not satisfied

### Validation

HZB evidence is recorded per backend. A single DX12 capture does not prove Vulkan, Metal, DX11, or OpenGL behavior.

A/B validation requires:

- The same project, scene, camera, build, occlusion stack size, and capture frame on both sides
- Setting `objects["editor.large-scene"].fields.enableHZBOcclusion` in the project's `UserSettings/editor-settings.json`
- The enabled side showing `HZBBuild` and `HZBOcclusion` dispatches before deferred lighting
- The disabled side showing deferred lighting and editor grid work with zero compute dispatches
- Runtime logs confirming the disable flag actually skipped renderer-side HZB preparation

Any change to HZB shaders, bindings, resource transitions, dispatch dimensions, readback, or pass order requires fresh evidence.

## Debug overlays

The editor provides visibility debug overlays showing rejection reasons and similar statistics.

Constraint: debug overlays and Frame Info only consume snapshots published by the renderer. When an overlay is disabled it must not traverse live scenes, synchronize render scenes, or drain resource queues.

## Related settings

| Location | Item | Notes |
| --- | --- | --- |
| `UserSettings/editor-settings.json` | `editor.large-scene.enableHZBOcclusion` | HZB occlusion toggle |
| Editor menu | `Debugging > Debug Draw` | Overlay debug information |
| Project settings | Visibility strategy options | Conservative by default |

## Next steps

- [LOD and HLOD](lod-hlod.md) reduces load once the visible set is known.
- [Graphics Backends and Large-Scene Features](../rendering/backends.md) covers capability queries and the backend matrix.
