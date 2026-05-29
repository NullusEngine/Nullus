# Feature Specification: Editor Scene View CPU Frame-Time Optimization

**Feature Branch**: `037-scene-view-cpu`  
**Created**: 2026-05-27  
**Status**: Draft  
**Input**: User description: "Optimize editor Scene View CPU frame time by reducing redundant deferred renderer preparation work observed in trace.json, starting with stable-size GBuffer resource reuse and measurable main-thread frame-time validation."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Scene View Rendering Is Smoother (Priority: P1)

An editor user keeps the Scene View open at a stable size while inspecting a scene and expects the editor to remain responsive rather than spending most of each frame repeating unchanged render preparation.

**Why this priority**: The provided trace shows the dominant frame-time cost in the Scene View render path, so reducing redundant work there gives the largest immediate improvement.

**Independent Test**: Run the focused renderer regression tests and a short editor trace with the same stable Scene View size; verify that the Scene View render preparation scopes are reduced without visual or runtime regressions.

**Acceptance Scenarios**:

1. **Given** the Scene View size is unchanged across consecutive frames, **When** the editor renders those frames, **Then** the renderer avoids repeating resource preparation that is only needed after size or attachment changes.
2. **Given** the same scene is rendered before and after the optimization, **When** a frame is captured or traced, **Then** the Scene View remains visible and the relevant main-thread preparation scopes do not regress.

---

### User Story 2 - Frame-Time Evidence Is Trustworthy (Priority: P2)

An engine developer records timeline evidence and expects the exported trace to represent completed events so performance conclusions are not distorted by invalid or partially recorded scopes.

**Why this priority**: The supplied trace contains invalid durations and truncated-looking records outside the main thread, which makes follow-up optimization harder to validate.

**Independent Test**: Export or inspect a timeline recording and confirm exported events exclude incomplete timing records and do not emit negative durations.

**Acceptance Scenarios**:

1. **Given** a profiling event has not completed, **When** trace export runs, **Then** that event is not emitted as a completed duration event.
2. **Given** profiling data is exported, **When** it is loaded into a trace viewer or aggregation script, **Then** durations are non-negative and usable for performance analysis.

---

### User Story 3 - Selected Object Debug Rendering Is Traceable And Avoids Hidden Work (Priority: P1)

An engine developer follows the next Scene View CPU hotspot after the GBuffer fix and expects the `Debug GameObject` cost to be split into actionable child scopes, while the editor avoids selected-object debug work that cannot produce visible output.

**Why this priority**: The re-exported trace after the first optimization slice shows `Debug GameObject` as the dominant remaining Scene View cost, but the original scope is too coarse to identify the next safe optimization.

**Independent Test**: Contract tests confirm nested `DebugGameObject::*` scope markers are present, disabled debug draw categories are gated before recursive selected-object traversal, outline capture reuses selected-tree data, the threaded path does not open an empty output render pass, prepared helper inputs avoid large recorded-command copies, and renderer binding tests confirm shared `Unlit.hlsl` remains compatible with the legacy object constants path.

**Acceptance Scenarios**:

1. **Given** a selected object is rendered in the editor, **When** debug rendering and outline capture execute, **Then** the code path exposes child profiler scopes for debug element traversal and outline capture phases.
2. **Given** `Unlit.hlsl` is shared by outline, picking, and asset preview paths, **When** the shader is loaded for a backend without indexed object-data push constant support, **Then** it does not require the indexed object-data layout.
3. **Given** bounds, camera, and lighting debug draw categories are all disabled, **When** the selected object debug pass executes, **Then** it applies debug draw visibility settings but skips recursive debug element generation.
4. **Given** selected object outline capture needs stencil and shell draw commands, **When** the pass prepares threaded commands, **Then** it collects eligible outline meshes once and reuses that collection for both pass emissions.
5. **Given** threaded rendering captures selected object outline commands, **When** the Debug GameObject pass has no immediate draw work, **Then** it does not open an output render pass on the main thread.
6. **Given** selected object bounds and outline both need mesh, transform, and component data, **When** the Debug GameObject pass prepares the selected tree for a frame, **Then** it reuses one current-frame collection for debug primitives and outline command capture while preserving camera-icon outline behavior.
7. **Given** a selected subtree produces many outline draw commands, **When** the editor assembles prepared helper passes, **Then** it avoids copying the recorded command vectors across getter, appended-input, and deferred-builder boundaries.
8. **Given** non-threaded selected-object outline has no prepared draw items, **When** the Debug GameObject pass executes, **Then** it skips opening an output render pass.
9. **Given** the selected-object outline color is stable across consecutive frames, **When** outline shell commands are captured or drawn, **Then** the outline material avoids redundant color writes that would invalidate its recorded binding set.

---

### User Story 4 - Selection Outline Uses A Unity-Style Screen-Space Path (Priority: P1)

An editor user selects an object with many meshes or children and expects the Scene View to remain usable while the selection outline is visible.

**Why this priority**: The 2026-05-28 10:15:58 recaptured trace still shows `Debug GameObject` averaging about 111 ms per frame after the previous low-risk selected-object CPU reductions. Unity 2018.4 avoids a pure inflated-shell outline path for Scene View selection by rendering selected render nodes into a mask, detecting object-ID edges, blurring the mask, and compositing the final outline in screen space. Nullus adapts this as a two-pass mask/composite path, fusing edge detection and soft outline filtering into the composite shader to reduce helper pass pressure.

**Independent Test**: Renderer contract tests should exercise the actual prepared pass inputs where possible, not only source text. They confirm the selected-object path no longer records two final outline mesh draw commands per selected mesh as the primary outline output. Instead it builds selected mask draw commands, declares explicit mask texture accesses, emits bounded full-screen composite work, and records structured fallback decisions when the mask path cannot be prepared.

**Acceptance Scenarios**:

1. **Given** threaded rendering is enabled and selected meshes are available, **When** the Debug GameObject pass prepares selection outline work, **Then** it emits a selected-object mask pass plus a screen-space composite pass instead of stencil plus inflated shell commands per mesh.
2. **Given** selected roots and selected children need different outline colors, **When** mask draw items are prepared, **Then** the mask encodes a stable selection group identifier and parent/child classification without duplicating subtree traversal.
3. **Given** the selected object is partially hidden by scene depth, **When** the mask is rendered, **Then** the path preserves selected coverage so hidden selected pixels remain outlined, and preserves visible versus occluded refinement for small selections where the extra depth-refinement pass remains bounded.
4. **Given** the output frame size is unchanged and the color/depth attachments still match the current frame, **When** consecutive frames draw the same selected subtree, **Then** the outline mask framebuffer is reused instead of reallocated.
5. **Given** the mask path cannot safely prepare required resources or the backend lacks required helper-pass support, **When** selection outline is requested, **Then** the renderer records a structured fallback decision; only MSAA-safe compatibility cases use the legacy shell path, while incompatible sample-count cases skip the current outline frame instead of risking a black Scene View.
6. **Given** timeline profiling is used to validate this path, **When** the Debug GameObject pass runs at depth 16 or deeper, **Then** actionable outline sub-scopes are exported or a shallower aggregate scope is emitted so trace evidence is not hidden by the profiler depth cap.
7. **Given** the mask path samples the current scene depth, **When** threaded pass inputs are built, **Then** the scene depth view is declared read-only for mask capture and mask/composite texture reads and writes are ordered explicitly.
8. **Given** selected-object outline is enabled for a large selected subtree, **When** the primary threaded path runs, **Then** the only per-selected-mesh work is mask capture; edge detection, soft outline filtering, and final outline output are bounded to the composite pass.
9. **Given** selected mask capture is valid, **When** prepared helper passes are emitted, **Then** they are emitted as an ordered vector of mask and composite pass inputs with matching metadata and consumption semantics.
10. **Given** selected materials use alpha clipping or a selection-specific shader pass, **When** the current Nullus material system cannot expose matching selection-pass metadata, **Then** the renderer reports `UnsupportedMaterialMask` and skips the current outline frame rather than drawing an opaque incorrect mask or re-entering the expensive shell path.
11. **Given** the selected tree contains renderers that are disabled, hidden, outside the current Scene View render eligibility, or not visible to the current camera/layer filter, **When** mask draw items are collected, **Then** those items are filtered out before mask command capture.
12. **Given** visible and occluded selection coverage are both needed, **When** mask capture records draw work, **Then** selected coverage is retained for all valid selections while the visible-depth refinement contribution is limited to tiny selections to bound CPU command preparation.
13. **Given** a selected Scene View object is active while a large asset or prefab instance is created, **When** threaded prepared-frame object-data slots are temporarily back-pressured by resource publication and selected-outline helper capture, **Then** the main deferred Scene View capture must wait briefly or use available slot headroom instead of publishing an empty scene snapshot that can turn the viewport black.

---

### Edge Cases

- The Scene View is resized repeatedly or minimized; resource preparation must still occur when dimensions or required attachments change.
- GBuffer allocation or wrapper validation fails; deferred graph and threaded prepared execution must not submit GBuffer or Lighting passes with incomplete or mismatched attachments.
- The editor runs with threaded rendering disabled or without a render backend; existing fallback behavior must remain unchanged.
- The profiler panel is open while recording; timeline UI overhead may still appear in the trace but must not corrupt exported event durations.
- The optimization is validated on one backend only; conclusions must state the validated backend and must not claim cross-backend proof.
- Selection outline mask resources cannot be allocated; the renderer must report a structured fallback, avoid black or stale output, and only use the legacy shell path for compatibility cases where selection feedback should remain visible.
- Timeline profiler depth limits can hide nested scopes below `Debug GameObject`; validation must either export shallower aggregate scopes or explicitly state that deeper scopes are suppressed.
- More than 255 selected roots may require wrapped or clamped mask group IDs; wrapping must not crash and must preserve a visible outline even if distinct-group edge separation becomes approximate.
- The Scene View keeps the same size but receives a new scene depth texture/view, different sample count, different view descriptor, or different output target identity; mask resources must not reuse stale depth or incompatible intermediates.
- The Scene View uses MSAA; until the mask path has matching MSAA/resolve support, sample counts other than single-sample or output/depth sample-count mismatches must be rejected with an `UnsupportedSampleCount` skip-frame decision before mask intermediates or legacy 1x shell rendering are submitted.
- Creating or resolving a large prefab while selection outline is active can briefly occupy all threaded prepared-frame slots; this must not make `DeferredSceneRenderer` skip the main scene capture or publish `recordedDraws=0` while scene drawables exist.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST preserve visible Scene View rendering while reducing redundant CPU preparation in stable-size frames.
- **FR-002**: The renderer MUST distinguish stable render target dimensions from actual resize or attachment-change conditions.
- **FR-003**: The renderer MUST continue to refresh resources when the Scene View size changes, becomes valid after being zero-sized, or changes required attachments.
- **FR-004**: The optimization MUST be covered by focused automated regression tests where stable entrypoints exist.
- **FR-005**: The performance validation MUST record exact evidence, including baseline trace observations and post-change test or runtime measurements.
- **FR-006**: The timeline trace export MUST avoid completed-event records with invalid or negative durations.
- **FR-007**: The change MUST avoid hand-editing generated output and MUST preserve current editor and game runtime entrypoints.
- **FR-008**: The renderer MUST reject incomplete or mismatched deferred GBuffer resources before building graph or threaded GBuffer/Lighting pass inputs.
- **FR-009**: Debug GameObject instrumentation MUST split the coarse selected-object cost into named child scopes that can be compared in a later exported trace.
- **FR-010**: Shared editor helper shaders MUST NOT be moved to a backend-specific binding contract without compatibility coverage.
- **FR-011**: Selected-object debug rendering MUST avoid recursive debug draw primitive generation when debug draw is disabled or all selected-object debug categories are disabled.
- **FR-012**: Selected-object outline rendering MUST avoid duplicate selected-tree traversal between stencil and outline shell command emission.
- **FR-013**: Threaded selected-object outline capture MUST avoid opening an empty output render pass before command capture.
- **FR-014**: Selected-object debug and outline preparation MUST share current-frame selected-tree mesh/transform data instead of resolving the same subtree separately for bounds and outline.
- **FR-015**: Prepared editor helper pass inputs MUST avoid unnecessary copies of large recorded draw-command vectors on the main-thread build path.
- **FR-016**: Non-threaded selected-object outline rendering MUST avoid opening an output render pass when the prepared outline draw-item list is empty.
- **FR-017**: Selected-object outline rendering MUST avoid invalidating material binding state when the outline color has not changed.
- **FR-018**: Selected-object outline rendering MUST provide a screen-space mask/composite path inspired by Unity 2018.4 Scene View selection outline instead of relying on inflated shell draws as the primary output.
- **FR-019**: The screen-space outline path MUST reuse the existing selected-tree collection as its source of selected mesh, transform, camera-icon, and parent/child classification data.
- **FR-020**: The screen-space outline path MUST allocate and reuse stable-size mask framebuffer resources, refreshing them only when frame dimensions, required attachment formats, resource identities, view descriptors, actual attachment extents, or supported sample counts change.
- **FR-021**: The screen-space outline path MUST declare explicit helper pass inputs, texture accesses, queue dependencies, and depth/stencil usage so threaded execution and backend barriers remain correct.
- **FR-022**: The screen-space outline path MUST preserve visible selected feedback for visible, occluded, parent, and child selections, with a documented large-selection approximation for visible-vs-occluded channel refinement and structured fallback decisions when required resources are unavailable or incompatible.
- **FR-023**: Trace instrumentation MUST remain actionable even when the selected-object pass sits at the timeline profiler depth cap.
- **FR-024**: The primary screen-space outline path MUST NOT emit the legacy inflated-shell output pass when mask resources and helper-pass support are available.
- **FR-025**: The mask pass MUST use the current scene depth as read-only input and MUST NOT attach stale or mismatched depth resources when mask resources are invalid.
- **FR-026**: The screen-space outline path MUST expose a prepared output model that keeps mask capture and composite work ordered explicitly rather than packing all selection feedback into one opaque helper pass.
- **FR-027**: Mask channel layout MUST have one cross-language source of truth shared or mechanically checked between C++ pass assembly and HLSL shader code.
- **FR-028**: Mask capture pipeline states MUST disable depth writes and stencil writes when using the read-only scene depth view.
- **FR-029**: The fused composite shader MUST cache the unique mask-neighborhood samples it needs and MUST NOT read and write the same texture subresource in one pass.
- **FR-030**: The mask path MUST provide deterministic test injection for missing views, allocation failures, and stale depth/resource identity so fallback reasons can be asserted without relying on real GPU allocation failures.
- **FR-031**: Runtime trace comparison MUST record the selected root identity or name, selected outline item count, Scene View size, outline enabled state, threaded rendering mode, backend, and project so performance changes are attributable.
- **FR-032**: Unity `SceneSelectionPass` and alpha-cutout semantics MUST be preserved where the Nullus material/shader system exposes the required metadata; otherwise the fallback decision or validation notes MUST document the unsupported material case.
- **FR-033**: Editor threaded rendering MUST provide bounded prepared-frame publication headroom for Scene View selection/creation bursts so object-data slot reservation failure does not skip the main deferred scene capture when a retired slot becomes available within the configured frame-latency budget.

### Key Entities

- **Scene View Frame**: A single editor frame that includes Scene View update, render preparation, rendering, UI composition, and presentation.
- **Stable Render Target**: A render target whose dimensions and attachment requirements match the previous usable frame.
- **Performance Trace**: Timeline evidence used to compare baseline and post-change frame-time behavior.
- **Selection Outline Mask**: An offscreen texture that stores selected-object visibility, occlusion, and group/classification channels before screen-space outline compositing.
- **Screen-Space Outline Composite**: Full-screen composite pass that turns the selection mask into the visible Scene View outline by applying fused edge detection and soft outline filtering.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On the provided baseline trace, the primary cause of low frame rate is documented with exact scope names and timings before code changes.
- **SC-002**: Focused regression tests fail before the optimization and pass after it, proving stable-size redundant preparation is removed.
- **SC-003**: In a comparable editor run, the stable Scene View preparation cost is reduced or, if runtime capture is unavailable, the automated tests demonstrate the eliminated redundant operation and the limitation is documented.
- **SC-004**: Exported profiling records used for validation contain no negative-duration completed events.
- **SC-005**: The editor remains runnable after the change for the validated backend and no claims are made for unvalidated backends.
- **SC-006**: Automated tests prove incomplete or mismatched deferred GBuffer resources do not produce threaded deferred pass inputs.
- **SC-007**: The next exported trace can distinguish `Debug GameObject` child phases, while any performance conclusion for that hotspot remains pending until runtime trace recapture.
- **SC-008**: Automated tests prove selected-object debug draw gating, empty-pass avoidance, shared selected-tree outline/debug capture, and prepared helper command-copy avoidance are enforced before runtime trace recapture.
- **SC-009**: Automated tests prove the selected-outline path avoids redundant stable-color material updates before runtime trace recapture.
- **SC-010**: The 2026-05-28 10:15:58 trace is documented with exact `Debug GameObject` timing and the profiler depth-cap limitation before the Unity-style outline work begins.
- **SC-011**: Contract tests prove the primary selected-outline path records mask and screen-space passes rather than two final outline mesh draws per selected mesh.
- **SC-012**: A comparable post-change trace is required before claiming the low-FPS selection-outline issue is fixed; the expected target is a material reduction of the `Debug GameObject` parent scope from the latest 111 ms/frame average.
- **SC-013**: Validation evidence includes either an event-level RenderDoc/RHI pass check or a targeted runtime verification that the mask and composite passes execute in order with expected attachments, read-only depth, color write targets, and write-to-read barrier/resource-state transitions.
- **SC-014**: Automated tests prove valid resources choose the screen-space path with no legacy shell pass, while injected invalid resources produce a structured fallback reason and preserve selection feedback.

## Assumptions

- The initial optimization slice targets the CPU main-thread Scene View bottleneck shown in `App/Win64_Debug_Runtime_Shared/trace.json`.
- The first implementation should be narrow and reversible: resource reuse on stable render target size plus trace export validity.
- The follow-up selected-object work adds instrumentation first, then only applies low-risk CPU reductions that are provable from the code path before a new runtime trace is exported.
- The next selected-outline optimization should change the rendering algorithm rather than continue stacking micro-optimizations on the inflated-shell path.
- DX12 is the backend represented by the supplied trace; other backends require separate validation before claiming equivalent improvement.
- RenderDoc is not required for the first CPU-only optimization unless later evidence points to GPU pass correctness or synchronization behavior.
