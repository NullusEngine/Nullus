# Contract: RHI UI Overlay Pass

## Pass Identity

- Debug/profile name: `RHIFrameGraph::UIOverlay`
- Command kind: `RenderPassCommandKind::UIOverlay`
- Queue: graphics
- Target: current swapchain backbuffer

## Ordering

The UI overlay pass must be the final swapchain render pass before present:

1. acquire swapchain image through the existing RHI frame path
2. record scene/editor swapchain passes
3. record `RHIFrameGraph::UIOverlay`
4. submit through the existing graphics queue batch path
5. present through the existing swapchain present path

It must not call `ID3D12CommandQueue::ExecuteCommandLists`, `ID3D12CommandQueue::Signal`, a private upload-fence wait, or a native present API directly. This prohibition applies to `RHIImGuiOverlayRenderer`, font atlas upload/rebuild, UI texture binding creation on the render path, and any migrated-path fallback hook.

## Attachments

- Color attachment: current `frameContext.swapchainBackbufferView`
- Color load op: load
- Color store op: store
- Depth/stencil: unused
- Clear: none

If `swapchainBackbufferView` is null, the pass is not recordable and must log/telemetry a skipped overlay rather than dereferencing it.

## Resource State

The pass must use existing Nullus resource-visibility structures explicitly:

- `textureResourceAccesses` contains the current swapchain backbuffer texture with `ResourceAccessMode::Write`, `ResourceState::RenderTarget`, color-output stages/access, and the acquired image `RHISubresourceRange`.
- `textureResourceAccesses` contains the font atlas and every registered UI texture referenced by visible draw commands with `ResourceAccessMode::Read`, `ResourceState::ShaderRead`, pixel/fragment shader stages, shader-read access, and the view's concrete `RHISubresourceRange`.
- Registered UI texture views are scoped to previous-frame/static sampled resources that are already stable for UI consumption when the snapshot is captured. The MVP registry does not infer same-frame producer dependencies from arbitrary `RHITextureView` values; a UI widget that needs to sample a texture produced earlier in the same frame must add an explicit producer/dependency contract before registering that view.
- `bufferResourceAccesses` contains the dynamic vertex and index buffers with `ResourceAccessMode::Read`, vertex/index input stages, and vertex/index-read access after upload/host writes are made visible.
- `exportedTextureVisibilityTransitions` or dependency-derived transitions must leave the swapchain backbuffer in the state required by the existing present path. The overlay pass must not bypass the normal render-target-to-present ownership.
- If the previous scene/editor pass also writes the swapchain backbuffer, the work-unit graph must include a resource-visibility dependency edge from that pass to `RHIFrameGraph::UIOverlay`; present must observe the overlay's exported state.
- Empty or null subresource ranges are not acceptable for the swapchain access. Sampled texture ranges may use the normalized full-view range, but the normalization must be test-visible.

The contract is intentionally stronger than "let the tracker figure it out": tests must inspect the access records, dependency edge, exported transition, and subresource range.

## Upload And Lifetime

Font atlas upload/rebuild and dynamic vertex/index data upload must be frame-owned RHI work:

- Upload staging buffers, host-visible buffer spans, dynamic vertex/index buffers, atlas textures, sampled texture views, binding sets, and backend descriptor allocations referenced by a submitted overlay frame must be retained until frame retirement.
- The font atlas upload path must declare copy/host-write to shader-read visibility before the first draw samples the atlas.
- Dynamic buffers must grow amortized and reuse storage across frames; a resize may allocate, but ordinary frames must not churn per draw command.
- A missing or failed atlas upload makes the overlay non-recordable with actionable diagnostics; it must not fall back to `ImGui_ImplDX12_RenderDrawData`.

## Recording

The renderer must:

- ensure font atlas resources are available
- upload/update dynamic vertex and index data without per-command heap churn
- bind the UI pipeline and pipeline layout
- bind font or registered texture binding sets per draw command
- set viewport once per pass
- set scissor per draw command after clipping
- issue indexed draws only for non-zero command element counts

## Required Tests

- Overlay pass is appended after scene swapchain passes.
- Overlay pass uses load/store color with no depth/stencil.
- Overlay pass declares swapchain render-target write access, sampled font/texture shader-read access, dynamic vertex/index buffer read access, concrete subresource ranges, and an exported swapchain transition for present.
- Work-unit dependencies prove scene swapchain writer -> `RHIFrameGraph::UIOverlay` -> present visibility.
- Font atlas upload and dynamic buffer upload retain staging/binding/descriptor resources until frame retirement and do not call native DX12 direct submit/signal/wait from the overlay path.
- Overlay recording uses RHI command buffer calls, not native DX12 direct submit.
- TimelineProfiler recognizes `RHIFrameGraph::UIOverlay`.
