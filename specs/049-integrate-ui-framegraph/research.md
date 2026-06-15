# Research: Integrate UI FrameGraph

## Decision 1: Copy ImGui draw data into a frame-owned snapshot

**Decision**: After `ImGui::Render()`, `UIManager` must copy the current `ImDrawData` into `Render::UI::UiDrawDataSnapshot` before any later `ImGui::NewFrame()` call. The snapshot owns draw lists, vertices, indices, command metadata, clip rects, texture IDs, display position, display size, framebuffer scale, and frame/debug identifiers.

**Rationale**: ImGui documents `ImGui::GetDrawData()` as valid after `Render()` and only until the next `NewFrame()`. `ThirdParty/ImGui/imgui_draw.cpp` provides `ImDrawList::CloneOutput()` for deep-copying draw list output. The RHI worker can consume UI work later than the UI thread, so raw pointers would be a lifetime bug.

**Alternatives considered**:
- Pass raw `ImDrawData*` to the RHI thread. Rejected because the data becomes invalid on the next ImGui frame.
- Render UI immediately on the UI/main thread. Rejected because it preserves the independent submission path and main-thread stalls.
- Rebuild UI on the render thread. Rejected because input, docking, platform backend, and widget state belong to the existing UI thread.

## Decision 2: Record UI through RHI commands instead of `ImGui_ImplDX12_RenderDrawData`

**Decision**: Implement a renderer-owned `RHIImGuiOverlayRenderer` that converts a `UiDrawDataSnapshot` into normal RHI command buffer work: update/upload dynamic vertex/index buffers, bind an RHI graphics pipeline, bind font/texture binding sets, set viewport/scissor, push projection constants, and issue `DrawIndexed` commands.

**Rationale**: The current DX12 path calls `ImGui_ImplDX12_RenderDrawData` and then submits a native command list directly with `ID3D12CommandQueue::ExecuteCommandLists`. That path must disappear from the migrated frame. RHI commands let the existing frame context own queue waits, resource state, descriptor lifetime, submission, device-lost handling, and present.

**Alternatives considered**:
- Keep the ImGui DX12 backend but feed it the frame command list. Rejected for MVP because it still depends on native descriptor heaps and backend-specific renderer ownership instead of the RHI contract.
- Only increase swapchain/backbuffer buffering. Rejected because it can reduce stalls but does not remove the second owner of backbuffer and queue submission.
- Create a separate UI compositor queue. Rejected because UI must share present ownership and swapchain resource transitions with the scene frame.

## Decision 3: Use a final swapchain overlay pass in the existing package/FrameGraph path

**Decision**: Add a `UIOverlay` render pass kind and package-owned UI overlay payload. Scene frames append `RHIFrameGraph::UIOverlay` after scene/editor swapchain passes and before present. UI-only frames create a normal `RenderScenePackage` with no scene draws and one UI overlay pass.

**Rationale**: Nullus FrameGraph compilation currently materializes RHI work through `RenderScenePackage.passCommandInputs` and `ParallelCommandWorkUnit`. The normal frame path already acquires `swapchainBackbufferView`, records passes, submits graphics batches, signals `renderFinishedSemaphore`, and presents. Hanging UI off this package path gives UI the same frame lifetime and synchronization as scene rendering.

**Alternatives considered**:
- Add an unrelated UI frame executor beside `RhiThreadCoordinator`. Rejected because it would duplicate the lifecycle that caused the current wait.
- Treat UI as ordinary mesh `RecordedDrawCommandInput`. Rejected because ImGui uses per-command scissor/texture state and dynamic buffers that do not match the scene material/object binding model.
- Delay UI to the next scene frame only. Rejected because UI-only interaction must remain responsive.

## Decision 4: Replace native UI texture descriptors with stable UI texture identities

**Decision**: UI widgets should pass opaque `UiTextureId` values through ImGui. A renderer-owned `RHIImGuiTextureRegistry` maps those identities to retained `RHITextureView` objects and creates/updates binding sets under frame lifetime rules. Old native DX12 descriptor handles must not escape as ImTextureID on the migrated path.

**Rationale**: Direct native descriptor handles are owned by the old bridge heap and can become stale across resize, descriptor heap recycling, or texture release. Stable identities let the RHI renderer retain views until safe, substitute a visible fallback for released textures, and bind through the active RHI descriptor allocator.

**Alternatives considered**:
- Keep `DX12UIBridge::ResolveTextureView` as the source of ImTextureID. Rejected because it preserves backend-native descriptor lifetime in UI code.
- Store raw `RHITextureView*` in `ImTextureID`. Rejected because UI draw commands can outlive caller references; the registry must own retention and release.
- Require all widgets to upload CPU images again. Rejected because existing panels already have RHI texture views and previews.

## Decision 5: Font atlas is an RHI resource owned by the overlay renderer

**Decision**: `RHIImGuiFontAtlas` owns the font atlas texture, view, sampler, and binding set. `UIManager::NotifyFontAtlasChanged()` invalidates the atlas generation; the overlay renderer uploads or rebuilds the atlas on the next frame through frame-owned RHI upload/transition work, not through a side `ExecuteCommandLists`/`Signal`/blocking fence path.

**Rationale**: The current DX12 backend initializes ImGui font texture inside native backend state. After migration, text rendering must work without `ImGui_ImplDX12`. Keeping atlas upload inside the RHI overlay renderer lets first-use, rebuild, device-lost handling, descriptor retention, and upload-resource retirement share the normal RHI frame model. It also prevents the stall from moving from `DX12UIBridge::WaitForBackbufferReuse` into a hidden one-off DX12 upload submit.

**Alternatives considered**:
- Keep font atlas in `DX12UIBridge`. Rejected because UI overlay rendering must not depend on the direct native bridge.
- Build the atlas every frame. Rejected due to unnecessary CPU/GPU upload cost.
- Bake one static atlas at startup only. Rejected because UI font loading and scale changes can rebuild fonts.

## Decision 6: Capability-gate migrated UI overlay support

**Decision**: Add an RHI device feature such as `RHIDeviceFeature::UIOverlayFrameGraph` and only enable the migrated product path when the active backend reports it and the UI overlay runtime can create the renderer, font atlas, texture registry, UI-only frame route, and legacy exclusion guards. DX12 capability plumbing may exist earlier for tests, but runtime selection must remain disabled with an explicit reason until the complete migrated path is wired. Other backends keep explicit unsupported/fallback states until migrated.

**Rationale**: The spec targets a DX12 stall. Nullus constitution forbids claiming other backend correctness from one backend. Capability gating lets Editor/Launcher choose between the new frame-owned path and documented unsupported behavior without silent backend splits.

**Alternatives considered**:
- Enable the new path for every backend by default. Rejected because Vulkan/DX11/OpenGL/Metal behavior is not validated.
- Keep old bridge fallback silently for DX12. Rejected because the user explicitly asked to merge the UI bridge into the full RHI frame graph immediately.

## Decision 7: Validate with source guards, unit tests, TimelineProfiler trace, and RenderDoc

**Decision**: Use layered validation:

1. Unit/source tests prove snapshot lifetime, overlay pass ordering, UI-only routing, texture retention, and direct-submit exclusion.
2. TimelineProfiler trace proves the main UI path no longer records per-frame `DX12UIBridge::WaitForBackbufferReuse` and does not replace it with a second UI queue submit, standalone UI present, or direct native signal.
3. DX12 RenderDoc/RHI evidence provides backend evidence for UI pass placement before present and inspected resource-state contracts. UI visibility still requires product smoke or visual evidence rather than RenderDoc metadata alone.
4. Performance evidence records hardware, build, reproducible UI workload, before/after wait counts, submit/present counts, snapshot-copy CPU time, per-frame allocation/reallocation counts, and frame-time deltas.

**Rationale**: Unit tests catch contract regressions quickly, but rendering correctness and synchronization ordering require backend evidence. This aligns with `Docs/Rendering/RenderDocDebugging.md`.

**Alternatives considered**:
- FPS-only validation. Rejected because FPS does not prove queue ownership, submit/present ownership, or resource-state correctness.
- RenderDoc-only validation. Rejected because snapshot lifetime, source guards, and UI-only routing are easier to cover deterministically in tests.

## Industry Reference

- There is no exact local `plan-review` benchmark entry for "UI overlay inside swapchain frame/present ownership". The closest local rendering benchmark entry is `rendering_layout.md` / `RHI In-Render-Pass Parallel Draw Recording`, which applies only to RHI command ownership, backend gates, explicit resource synchronization, and frame-retired lifetime. This plan does not cite it as a direct UI-overlay algorithm match.
- Unreal Engine 4.27 `Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp` records Slate windows through `FRHICommandListImmediate&` on the render thread (`DrawWindow_RenderThread`) and brackets viewport drawing through `BeginDrawingViewport` / `EndDrawingViewport`. This is an RHI command-list and viewport-lifecycle analog, not a FrameGraph overlay analog.
- Unreal Engine 4.27 `Engine/Source/Runtime/RHI/Public/RHICommandList.h` defines `FRHICommandListImmediate` and `FRHICommandListExecutor::GetImmediateCommandList`, grounding the "UI renderer records into the engine RHI command stream" comparison.
- Unreal Engine 4.27 `Engine/Source/Runtime/D3D12RHI/Private/D3D12Viewport.cpp` keeps DX12 viewport present in `RHIEndDrawingViewport` / `FD3D12Viewport::Present`, grounding the "single RHI viewport/present owner" comparison.
- The local `rendering_layout.md` `Rendering Large-Scene Visibility And Residency` sign-off guidance is used only for validation discipline: final evidence must name backend, build, feature gates, hardware, reproducible workload, and before/after deltas before making performance claims.
- Follow-up benchmark hygiene: after this migration lands, add a dedicated `RHI UI Overlay / Present Ownership` benchmark entry to the project review registry so future UI-frame changes have an exact local reference.
