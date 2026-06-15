# Data Model: Integrate UI FrameGraph

## UI Draw Data Snapshot

**Purpose**: Owns a safe copy of one ImGui frame's draw output until the RHI frame has recorded it.

**Fields**:
- `frameId`: RHI/UI frame identifier used for tracing and retention.
- `displayPos`, `displaySize`, `framebufferScale`: copied from `ImDrawData`.
- `totalVertexCount`, `totalIndexCount`, `drawListCount`: copied counts for validation and buffer sizing.
- `drawLists`: vector of `UiDrawListSnapshot`.
- `hasVisibleDraws`: true when at least one draw command has non-zero element count, the framebuffer size/scale is recordable, and the command does not require an unsupported callback or unsupported texture ID.
- `containsUnsupportedUserCallback`: true when a command has a callback other than an explicitly supported reset-render-state callback.

**Validation rules**:
- A snapshot may be empty; empty snapshots must not create an overlay pass.
- Vertex/index/cmd buffers must be deep-copied and independent of ImGui's next frame.
- Capture cost is O(total vertices + total indices + total draw commands) time and storage. The copy is required for cross-thread lifetime safety and must be measured in final diagnostics.
- Display size or framebuffer scale of zero makes the snapshot non-recordable and should skip overlay work with a diagnostic.
- Unsupported callbacks must be visible in telemetry/tests and must not execute arbitrary UI-thread callbacks on the RHI thread.

**State transitions**:
- `Captured`: created after `ImGui::Render()`.
- `Published`: attached to `Driver` or a `RenderScenePackage`.
- `Recorded`: consumed by `RHIImGuiOverlayRenderer`.
- `Retired`: released after frame submission/retirement.

## UI Draw List Snapshot

**Purpose**: Owns one copied ImGui draw list.

**Fields**:
- `vertices`: copied `ImDrawVert` data.
- `indices`: copied `ImDrawIdx` data.
- `commands`: vector of `UiDrawCommandSnapshot`.

**Validation rules**:
- Command `idxOffset` and `vtxOffset` must remain within copied buffers.
- Draw lists with no commands or no elements are ignored.

## UI Draw Command Snapshot

**Purpose**: Describes one scissored textured indexed draw.

**Fields**:
- `elementCount`
- `indexOffset`
- `vertexOffset`
- `clipRect`
- `textureId`
- `callbackKind`

**Validation rules**:
- `elementCount == 0` commands are skipped.
- Clip rects are transformed by `displayPos` and `framebufferScale` before scissor setup.
- Texture ID `0` resolves to the font atlas.

## UI Texture ID

**Purpose**: Stable opaque value passed through ImGui instead of a backend-native descriptor handle.

**Fields**:
- `value`: non-zero integer identity encoded as `ImTextureID`.
- `generation`: required registry generation encoded with or resolved from `value` to detect stale references.

**Validation rules**:
- Identity `0` is reserved for the font atlas/default texture.
- IDs must remain stable for the lifetime of registered views.
- Reused registry slots must increment generation; a stale `value`/`generation` pair must resolve to a visible fallback, not a released view.
- Released views stay retained until frame retirement or resolve to a visible fallback.

## UI Texture Registry Entry

**Purpose**: Maps a UI texture identity to RHI resources.

**Fields**:
- `id`: `UI Texture ID`.
- `textureView`: retained `std::shared_ptr<RHITextureView>`.
- `bindingSet`: backend/RHI binding set for the active descriptor allocator.
- `bindingSetDeviceCacheIdentity`: device/cache identity used to create `bindingSet`; mismatches require binding-set rebuild before recording.
- `descriptorLifetimeToken`: retained descriptor/binding allocation lifetime owned by the renderer when the backend requires explicit descriptor heap retention.
- `lastUsedFrameId`
- `releaseRequested`
- `safeToReleaseFrameId`

**Validation rules**:
- Entries must not expose native DX12 descriptor handles to UI widgets.
- `textureView` must remain alive while any published snapshot can reference its ID.
- Registered texture views are previous-frame/static sampled inputs for the 049 MVP. Same-frame render-target previews require a future explicit producer dependency and must not rely on the registry to infer producer order from a raw view.
- Binding sets and descriptor allocations must remain alive while any submitted frame can still execute a draw command referencing the ID.
- Released entries must not be physically erased until no in-flight snapshot or submitted command buffer can reference their view, binding set, or descriptor allocation.

## UI Font Atlas Resource

**Purpose**: Owns the font atlas texture and binding state needed by the overlay renderer.

**Fields**:
- `generation`
- `texture`
- `textureView`
- `sampler`
- `bindingSet`
- `descriptorLifetimeToken`
- `retiredBindings`: old texture/view/binding/descriptor allocations waiting for frame retirement after an atlas rebuild.
- `pixelWidth`, `pixelHeight`
- `uploadState`

**Validation rules**:
- First overlay frame with visible text must ensure the atlas is uploaded.
- Font rebuild increments generation and retires the old atlas safely.
- Upload and rebuild work must be scheduled through frame-owned RHI upload/transition work; it must not submit a standalone native DX12 command list or wait on a private upload fence from the UI path.
- Atlas texture upload must declare copy/host-write to shader-read visibility, and atlas binding/descriptor allocations must be retained until submitted frames are retired.
- Upload failure must be logged and must not leave partially valid binding state.

## RHI UI Overlay Pass Descriptor

**Purpose**: Attaches UI work to a normal swapchain frame.

**Fields**:
- `debugName`: fixed to `RHIFrameGraph::UIOverlay`.
- `snapshot`: shared immutable UI draw data snapshot.
- `targetsSwapchain`: always true for MVP.
- `renderWidth`, `renderHeight`
- `queueType`: graphics.
- `loadColor`: true; no clear.
- `usesDepthStencilAttachment`: false.
- `swapchainColorAccess`: `TextureResourceAccess` write access for the current backbuffer subresource in `RenderTarget` state.
- `sampledTextureAccesses`: shader-read `TextureResourceAccess` entries for the font atlas and every registered UI texture view referenced by the snapshot.
- `dynamicBufferAccesses`: vertex/index-read `BufferResourceAccess` entries for overlay dynamic buffers after upload/host writes are visible.
- `exportedSwapchainTransition`: exported transition that leaves the swapchain backbuffer ready for the existing present path.

**Validation rules**:
- Must be ordered after scene/editor swapchain passes and before present.
- Must not run when `swapchainBackbufferView` is null.
- Must not create an independent native command list submission.
- Must publish dependency edges from the previous swapchain writer to `RHIFrameGraph::UIOverlay` and from the overlay pass to the present transition when resource state tracking requires explicit visibility.
- Resource accesses must include concrete `RHISubresourceRange` values; MVP uses the acquired swapchain image subresource and the subresource ranges exposed by each sampled view.
- Recording cost is O(visible draw commands + uploaded vertex/index bytes). Ordinary frames should reuse overlay buffers and binding sets; allocations are expected only on growth, atlas rebuild, texture registration, release retirement, or swapchain/device reset.

## UI Frame Capability

**Purpose**: Backend-reported support for frame-owned UI overlay rendering.

**Fields**:
- `supported`
- `backend`
- `reason`
- `runtimeSelectable`: true only when product code may choose the migrated path.
- `validationState`: implementation/test status used for diagnostics; it is not a substitute for backend-specific sign-off evidence.

**Validation rules**:
- DX12 `runtimeSelectable` is true only after overlay renderer, UI-only routing, font atlas upload, texture registry retention, legacy direct-submit exclusion, and required validation are implemented.
- Early capability plumbing must report unsupported/not-selectable with an explicit reason rather than allowing Editor/Launcher to enter a half-migrated path.
- Other backends default to unsupported with an explicit reason.
- Product code must branch on the capability instead of backend string checks alone.
