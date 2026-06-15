# Feature Specification: Integrate UI FrameGraph

**Feature Branch**: `049-integrate-ui-framegraph`
**Created**: 2026-06-13
**Status**: Draft
**Input**: User description: "Integrate the editor UI bridge into the RHI FrameGraph lifecycle immediately. The DX12 UI path must stop submitting an independent UI command list directly to the swapchain; UI draw data should become a FrameGraph/RHI swapchain overlay pass that shares frame acquisition, resource transitions, queue submission, and present synchronization with the normal RHI frame. Preserve runnable Editor behavior, maintain texture/font atlas support, and validate that DX12 profiler captures no longer show per-frame DX12UIBridge::WaitForBackbufferReuse on the main UI render path."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - UI Rendering Uses The Frame Lifecycle (Priority: P1)

As an editor user running the DX12 backend, I want the editor UI to render as part of the same frame lifecycle as the scene so that opening UI-heavy panels does not create a separate swapchain submission path or main-thread backbuffer reuse stall.

**Why this priority**: This removes the root architectural cause of the visible `DX12UIBridge::WaitForBackbufferReuse` main-thread wait while keeping the safety guarantees restored by the DX12 hang fix.

**Independent Test**: Run the Editor with DX12, open the Profiler panel, capture a TimelineProfiler trace, and confirm the UI is visible while the main UI render path no longer records per-frame `DX12UIBridge::WaitForBackbufferReuse`.

**Acceptance Scenarios**:

1. **Given** the Editor is running on DX12 and a scene targets the swapchain, **When** the UI is rendered, **Then** UI draw work is included in the normal swapchain frame before present rather than submitted through an independent UI command list.
2. **Given** the Profiler panel is open and producing a dense UI, **When** a timeline trace is captured, **Then** the trace shows a FrameGraph/RHI UI overlay pass and does not show `DX12UIBridge::WaitForBackbufferReuse` every frame on the main UI render path.
3. **Given** the DX12 device reports failure during UI overlay recording or submission, **When** the frame is processed, **Then** existing device-lost and quarantine behavior remains explicit and the Editor does not silently continue with unsafe GPU work.

---

### User Story 2 - UI-Only Frames Use The Same Present Path (Priority: P2)

As an editor user interacting with UI while scene rendering is idle or unavailable, I want UI-only frames to remain responsive without falling back to the old standalone UI explicit frame path.

**Why this priority**: The existing standalone UI path is the other owner of direct swapchain UI submission. It must be replaced so the architecture has one present and backbuffer ownership model.

**Independent Test**: Exercise an editor frame with no scene swapchain work and confirm the UI still presents through a normal frame package that contains only the UI overlay work.

**Acceptance Scenarios**:

1. **Given** no scene render package is available for the frame, **When** the UI still needs to update, **Then** the system submits a UI-only swapchain frame through the normal frame acquisition, resource transition, queue submission, and present path.
2. **Given** a pending swapchain resize occurs while UI-only frames are active, **When** resize is applied, **Then** UI rendering releases old swapchain resources through the normal frame lifecycle and does not require a UI bridge submitted-work drain.
3. **Given** a backend lacks the new RHI UI overlay capability, **When** UI-only rendering is requested, **Then** the product uses an explicit capability-gated fallback or reports the unsupported state without silently using the DX12 direct-submit path.

---

### User Story 3 - UI Resources Stay Stable Across Frames (Priority: P3)

As a developer using editor UI widgets, I want font atlas and texture handles to keep working after the renderer migration so existing UI panels and image previews render correctly.

**Why this priority**: The migration changes ownership of UI rendering resources. Texture identity, font atlas upload, and draw-data lifetime are the highest-risk compatibility points.

**Independent Test**: Render UI that uses the font atlas and at least one registered RHI texture view, then verify the output remains correct across multiple frames, resize, and texture release.

**Acceptance Scenarios**:

1. **Given** UI panels use text and icons, **When** the UI overlay pass renders, **Then** the font atlas is available and text/icons display correctly.
2. **Given** a UI panel resolves an RHI texture view for display, **When** the UI overlay pass renders, **Then** the resolved texture appears in the correct draw command without using stale native descriptor handles.
3. **Given** an RHI texture view is released after being used by UI, **When** later frames render, **Then** the texture registry stops referencing the released view after it is safe to do so.

### Edge Cases

- The UI produces no draw lists for a frame; the system must skip the UI overlay work without creating an empty swapchain hazard.
- The UI draw data is produced on the main thread but consumed later by the render/RHI thread; the system must preserve a safe snapshot until the frame is consumed.
- Swapchain resize occurs between UI draw-data capture and RHI recording; stale swapchain backbuffer views must not be used.
- The DX12 font atlas upload fails or the device is lost; the failure must be logged with actionable context and must not leave the UI renderer in a partially valid state.
- A texture handle is referenced by a UI draw command after the source view has been released; the renderer must use retention or a visible fallback instead of dereferencing a stale object.
- The Profiler panel is opened while timeline export or GPU query resolve work is active; the new UI overlay path must not introduce a second direct queue submit outside RHI synchronization.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The DX12 editor UI render path MUST stop submitting UI draw data through an independent UI command list that directly targets the swapchain.
- **FR-002**: UI draw data for swapchain frames MUST be represented as work owned by the normal RHI frame lifecycle before present.
- **FR-003**: The system MUST preserve a safe UI draw-data snapshot when UI generation and RHI recording occur on different threads or at different times.
- **FR-004**: The normal swapchain frame path MUST own UI overlay ordering, backbuffer acquisition, resource state transitions, queue submission, and present synchronization.
- **FR-005**: UI overlay work MUST execute after scene/editor swapchain rendering and before the swapchain is presented.
- **FR-006**: UI-only editor frames MUST use the normal swapchain frame path and MUST NOT use the old standalone DX12 UI explicit frame path.
- **FR-007**: Font atlas rendering MUST remain available after the migration, including first-use upload and atlas rebuild handling.
- **FR-008**: UI texture-view rendering MUST remain available through stable texture identities that do not expose stale backend-native descriptors to UI code.
- **FR-009**: Swapchain resize and shutdown MUST release or retire UI overlay resources through normal frame lifetime rules instead of requiring a direct UI bridge drain.
- **FR-010**: Timeline traces for the migrated DX12 path MUST expose the new UI overlay work with a specific trace name and MUST NOT show per-frame `DX12UIBridge::WaitForBackbufferReuse` on the main UI render path.
- **FR-011**: Existing device-lost and unsafe-GPU-work quarantine behavior MUST continue to protect frames that fail while recording or submitting UI overlay work.
- **FR-012**: Backend support MUST be capability-gated so the migration does not claim Vulkan, DX11, OpenGL, or Metal correctness until each backend has explicit validation evidence.
- **FR-013**: The migrated DX12 UI overlay path MUST NOT replace the old wait with another UI-owned native queue submit, private upload fence wait, or standalone present outside the normal RHI frame.

### Key Entities

- **UI Draw Data Snapshot**: A frame-owned copy of UI draw lists, vertex/index data, draw commands, clip rectangles, texture references, display position, display size, and framebuffer scale.
- **RHI UI Overlay Pass**: The final swapchain pass that renders the UI snapshot after scene/editor passes and before present.
- **UI Texture Registry**: A renderer-owned mapping from UI texture identities to retained RHI texture views and binding data.
- **UI Font Atlas Resource**: The font texture and associated binding state required for text and icon rendering in the overlay pass.
- **UI Frame Capability**: Backend-reported support for recording UI overlay work through the normal RHI frame path.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a DX12 Editor trace with the Profiler panel open for at least 300 frames, `DX12UIBridge::WaitForBackbufferReuse` appears in fewer than 1% of main UI render frames.
- **SC-002**: The Editor remains visually usable on DX12 after migration: text, icons, docked panels, scene view overlays, and texture previews render for at least 300 consecutive frames.
- **SC-003**: Opening the Profiler panel on DX12 does not produce `DEVICE_HUNG`, unsafe GPU work quarantine, or rejected readback logs during the validation run.
- **SC-004**: A UI-only frame presents visible UI through the normal swapchain path without invoking the old standalone UI explicit frame path.
- **SC-005**: Resize validation shows no stale backbuffer view usage and no UI-specific submitted-work drain is needed before swapchain resource recreation.
- **SC-006**: Focused automated regression tests cover UI draw-data lifetime, overlay pass ordering, legacy direct-submit exclusion, texture registry retention, and UI-only frame routing.
- **SC-007**: In the same DX12 validation run, migrated swapchain frames have one RHI-owned graphics submit/present path and zero UI-owned native direct submits, standalone presents, or private UI upload fence waits.
- **SC-008**: Final performance diagnostics record hardware/build/workload, before/after wait and frame-time deltas, graphics submit counts, snapshot-copy CPU time, and overlay dynamic-buffer allocation/reallocation counts.

## Assumptions

- The MVP targets DX12 Editor behavior first because the reported stall and hang investigation are DX12-specific.
- Other graphics backends may keep their current UI path until a later capability-gated migration, but they must not be represented as validated by this feature.
- The Editor must remain runnable throughout implementation; any temporary fallback must be explicit, capability-driven, and recorded in this spec bundle.
- The existing RHI command buffer can bind graphics pipelines, binding sets, vertex/index buffers, scissor rectangles, and issue indexed draws needed for ImGui output.
- The current ImGui platform backend remains responsible for input and frame setup; this feature changes renderer ownership, not UI input handling.
- RenderDoc evidence is preferred for final DX12 runtime validation, with TimelineProfiler traces used to confirm the main-thread wait behavior.
