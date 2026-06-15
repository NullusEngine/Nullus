# Contract: UI Draw Data Snapshot

## Producer

`Runtime/UI/UIManager.cpp` remains responsible for:

- `BeginFrame()`
- drawing the active `Canvas`
- `ImGui::Render()`
- capturing `ImGui::GetDrawData()` into `Render::UI::UiDrawDataSnapshot`

No raw `ImDrawData*`, `ImDrawList*`, `ImDrawCmd*`, vertex pointer, or index pointer may be stored in `Driver`, `RenderScenePackage`, or RHI worker state after `UIManager::Render()` returns.

## Consumer

`Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp` consumes only immutable snapshots. It may cache GPU buffers and binding resources, but the snapshot is the only per-frame UI command source.

## Empty Frames

If the snapshot has no visible draw commands, `UIManager` may publish an empty result and the RHI package builder must not append `RHIFrameGraph::UIOverlay`.

## Callback Handling

The MVP supports no arbitrary user callbacks on the RHI thread. A reset-render-state callback may be represented as a known callback kind if needed. Any other callback must set `containsUnsupportedUserCallback`, emit a diagnostic, and skip or reject the affected UI command without executing the callback on the RHI thread.

## Lifetime Requirements

- Snapshot lifetime begins after `ImGui::Render()`.
- Snapshot lifetime ends only after the owning RHI frame has recorded or skipped the overlay work.
- Registry resources referenced by `textureId` must be retained at least until the latest submitted frame referencing the ID is retired.
- Retention includes the `RHITextureView`, binding set, sampler where applicable, backend descriptor allocation/lifetime token, and any upload staging resource needed by the frame.
- A copied texture ID must include generation validation or an equivalent never-reuse invariant so stale IDs resolve to a fallback instead of a newly registered texture.

## Required Tests

- Captured snapshot remains valid after a later `ImGui::NewFrame()`.
- Empty draw data does not create an overlay pass.
- Unsupported callback is detected without executing a RHI-thread callback.
- Texture IDs are copied as stable identities, not backend-native descriptor handles.
