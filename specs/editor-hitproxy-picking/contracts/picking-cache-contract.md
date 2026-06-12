# Contract: Picking Cache And Selection Separation

## Picking Cache Contract

- `PickingRenderPass` owns picking-frame submission and readable-frame decode.
- `SceneView` owns user intent: hover request, click request, mouse coordinate, and camera-control suppression.
- `SceneViewPickingPolicy` owns pure decision helpers that can be unit-tested without renderer state.
- A picking frame is reusable only when the current request signature matches the readable frame signature.
- Click requests record a minimum acceptable serial and must not resolve against older frames.
- Hover requests may be skipped by budget; click requests must bypass hover budget.

## Selection Separation Contract

- Selection outline/highlight uses selected-object debug draw data and `SelectionOutlineMaskRenderer`.
- Changing selected object state does not force picking cache invalidation unless pickable geometry/visibility/transform changed.
- Picking readback unavailability does not clear current selection outline.
- Picking may update hover highlight only when a compatible readable frame exists.

## Diagnostics Contract

- Profiler scopes distinguish:
  - `EditorPicking::Rebuild`
  - `EditorPicking::Reuse`
  - `EditorPicking::SkipHoverBudget`
  - `EditorPicking::WaitReadback`
  - `EditorPicking::ResolveClick`
- FrameInfo displays separate fields for submitted serial, readable serial, click minimum serial, rebuild count, reuse count, hover skips, and visible pickable draw count.
- Multiple visible render views aggregate diagnostics by summing counters and showing the newest serial per field.
