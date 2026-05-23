# Feature Specification: Large Model Scene Performance

**Feature Branch**: `028-large-model-scene-performance`
**Created**: 2026-05-18
**Status**: Draft
**Input**: User report: "场景加载了模型后会变得非常卡顿，掉帧严重，基本上用不了"

## User Scenarios & Testing

### User Story 1 - Imported Models Stay Interactive In Scene (Priority: P1)

An editor user can drag or load a large imported model into a scene and continue navigating, selecting, and inspecting the scene without the editor becoming unusably slow.

**Why this priority**: This is the blocking workflow for imported asset use. If a loaded model destroys editor frame rate, the asset pipeline is not usable even when import succeeds.

**Independent Test**: Load an imported model prefab into the scene and verify the editor render path uses normal geometry culling and stable mesh draw resources rather than forcing all model meshes through the frame every time.

**Acceptance Scenarios**:

1. **Given** an imported model prefab, **When** it is generated or resolved into scene objects, **Then** mesh renderers use model-bound culling by default.
2. **Given** an editor viewport or default scene camera, **When** it renders a scene containing imported mesh nodes, **Then** geometry frustum culling is enabled by default.
3. **Given** a mesh visible across repeated frames, **When** the renderer asks for its RHI mesh, **Then** the same RHI mesh adapter is reused instead of allocating a new wrapper per draw.

### User Story 2 - Existing Cached Imported Models Refresh Safely (Priority: P2)

An editor user with old imported model artifacts gets refreshed generated prefabs after this fix so stale cached assets do not continue disabling culling.

**Why this priority**: Existing Library artifacts can preserve the previous slow behavior. A correct code path is not enough if cached prefabs keep the old setting.

**Independent Test**: Verify the model-scene importer version changes so existing manifests become invalid and regenerate with the new renderer defaults.

**Acceptance Scenarios**:

1. **Given** an existing model-scene cache produced by the previous importer version, **When** the editor scans assets after this change, **Then** the model-scene importer version mismatch causes a reimport.

### User Story 3 - Scene Draw Collection Scales With Many Meshes (Priority: P2)

An editor user can load a model with many mesh nodes without the renderer spending avoidable CPU time on per-draw tree-node allocation while collecting opaque, transparent, and skybox drawables.

**Why this priority**: The trace evidence showed editor UI as the current largest cost, but renderer scene parsing still runs every visible scene frame. Large imported scenes multiply this path by mesh count, so draw collection must avoid avoidable hot-path allocations before larger render-scene caching work begins.

**Independent Test**: Static render-path contract tests verify `BaseSceneRenderer` no longer uses node-allocated `std::multimap` for drawable collection and instead sorts contiguous drawable arrays after collection.

**Acceptance Scenarios**:

1. **Given** many visible opaque meshes, **When** `ParseScene()` collects drawables, **Then** opaque drawables are stored in contiguous storage and sorted after collection instead of inserted into a node-based sorted container per draw.
2. **Given** transparent meshes, **When** `ParseScene()` completes, **Then** transparent drawables preserve back-to-front distance ordering.
3. **Given** existing forward and deferred render paths, **When** they consume collected drawables, **Then** they iterate the sorted collection without changing draw submission semantics.
4. **Given** missing or unresolved material bindings, **When** `ParseScene()` applies the default material fallback, **Then** it only uses already resident material resources and never synchronously loads assets from the render preparation path.

### User Story 4 - Editor UI Draw Scales With Large Hierarchies (Priority: P2)

An editor user can keep the Hierarchy and other panels open while inspecting a large imported scene without the UI layer spending avoidable per-frame CPU time on generic panel scopes, TreeNode label string rebuilds, or child garbage scans when no widget was destroyed.

**Why this priority**: The latest trace showed editor UI and canvas drawing as the current dominant frame cost after the renderer hot-path changes. Large imported scenes create many Hierarchy tree nodes, so the UI draw path must both expose actionable per-panel profiling names and avoid obvious per-node/per-container hot-path work.

**Independent Test**: Editor render-path and panel hook tests verify panels emit cached per-panel profiler scope names, TreeNode ImGui labels are cached until their visible label or stable widget ID changes, and widget containers skip garbage scans unless a child is destroyed while preserving widget ownership across parent changes.

**Acceptance Scenarios**:

1. **Given** the editor draws multiple panels, **When** profiling is enabled, **Then** each panel draw scope includes the panel name or panel ID so the next trace can attribute UI cost to specific panels.
2. **Given** a Hierarchy tree node is drawn repeatedly without a name or widget-ID change, **When** `TreeNode::_Draw_Impl()` submits the ImGui label, **Then** it reuses the cached label string instead of rebuilding `name + "###" + id` every draw.
3. **Given** a widget container has no destroyed children, **When** the container draws or a nested layout asks to collect garbage, **Then** collection returns without scanning the child vector.
4. **Given** a widget is destroyed, **When** the parent container next collects garbage, **Then** the dirty flag causes the destroyed widget to be removed and internally managed memory to be released.
5. **Given** an already destroyed external widget is moved to another container before collection, **When** that new container collects garbage, **Then** it treats the widget as dirty and removes it.
6. **Given** an externally managed widget leaves a container through explicit removal, full clear, or garbage collection, **When** it is later destroyed or moved, **Then** it no longer references the old parent container.
7. **Given** a widget is created already destroyed or is moved directly between containers, **When** garbage collection runs, **Then** only the owning container retains it and destroyed widgets are collected.
8. **Given** an internal or external widget is automatically moved from one container to another, **When** the new container removes all widgets, **Then** the widget keeps its previous memory ownership mode so internal widgets are deleted and external widgets are not.
9. **Given** a hierarchy node has prefab presentation coloring, **When** the node is drawn each frame, **Then** it reads cached color state instead of invoking a per-node dynamic provider from the draw path.

### User Story 5 - Updated Trace Identifies And Avoids Resource Resolution Stalls (Priority: P2)

An editor user can capture a timeline trace for a large imported scene and get actionable, non-duplicated editor-panel scopes while renderer resource resolution does not spend hundreds of frames walking missing material slots one at a time.

**Why this priority**: The updated trace still showed 63-97 ms CPU frames dominated by `RenderEditorUI` / `UIManager::DrawCanvas`, but the trace lacked panel-level depth and contained repeated/truncated events. The companion log also showed the Sponza renderer resource queue taking from 20:59:21 to 21:00:40, with hundreds of material cache misses, indicating a separate resource-resolution stall that kept the scene in a degraded state for many frames.

**Independent Test**: Profiler destination tests verify the timeline sink keeps deep editor panel scopes, trace recording keeps the profiler timeline visible, and the trace exporter advances by frame instead of rewriting previous frames. Editor render-path contract tests verify deferred material binding scans cache-only material slots in bounded batches without breaking after the first unresolved or invalid cached material.

**Acceptance Scenarios**:

1. **Given** nested editor UI scopes under `Canvas::Draw`, **When** the timeline profiler records a frame, **Then** panel-level scopes survive the depth limit so traces can attribute the UI cost to specific panels, including a dedicated profiler timeline draw scope.
2. **Given** trace export is enabled across multiple UI draws, **When** the profiler window updates the trace file, **Then** each completed frame is exported once instead of being appended again on every update.
3. **Given** a generated model material renderer has many material path slots that are not yet cached or are cached without a valid shader, **When** deferred material binding runs, **Then** it scans a bounded batch of material slots in the current task pass, yields when the frame budget expires, keeps visible fallback material in unresolved slots, and does not synchronously load cold materials on the editor frame.
4. **Given** a material artifact is prewarmed only to populate cache metadata, **When** the material references a shader that is not resident, **Then** prewarm does not synchronously load or compile that shader dependency and does not register the invalid material.
5. **Given** a large generated prefab has many mesh and material asset references, **When** renderer resource-resolution tasks are collected and processed, **Then** resolved asset lookups and live source-object lookups use bounded indexes instead of repeated linear scans.
6. **Given** an async drag/drop import throws on the background worker, **When** the worker exits, **Then** the original completion callback is invoked with a rejected non-pending result and an actionable diagnostic instead of leaving the editor in a pending-import state.

### User Story 6 - Scene View Avoids Per-Frame Thread Drain Stalls (Priority: P1)

An editor user can navigate or inspect a large imported scene in Scene View without the main thread draining threaded rendering every frame merely to keep the viewport image perfectly synchronized.

**Why this priority**: The latest trace shows the dominant recurring main-thread stall under `Panel::Draw:Scene View` is `AView::DrainThreadedRendering`, with many frames blocking 18-20 ms. Removing the default drain is the largest measured interactive-frame win after the previous resource-resolution work.

**Independent Test**: Panel lifecycle contract tests prove Scene View defaults to asynchronous retired-frame presentation, camera or gizmo activity no longer requests a synchronized drain by itself, and explicit resize/readback policy still drains when required.

**Acceptance Scenarios**:

1. **Given** Scene View renders with threaded rendering enabled, **When** no immediate readback or resize is required, **Then** Scene View does not request synchronized retired-frame presentation by default.
2. **Given** the Scene View camera moves or a gizmo is active, **When** the view renders normally, **Then** those presentation hints do not force `AView::DrainThreadedRendering` on the main thread.
3. **Given** Scene View requires an immediate readback, a resize-safe retired frame, or validation readback, **When** the view render lifecycle reaches the drain policy, **Then** the existing explicit drain paths remain enabled.
4. **Given** a trace is captured after this change, **When** ordinary Scene View frames are inspected, **Then** `AView::DrainThreadedRendering` should be absent from the steady-state Scene View hot path except for explicit synchronization cases.

### User Story 7 - Retained Render Prep Hotspots Are Observable And Cached (Priority: P1)

An engine developer can inspect a large-model trace and FrameInfo panel to see how much work remains in scene parsing, drawable collection, GBuffer material synchronization, binding-set creation, snapshot-buffer creation, hierarchy scale, and panel drawing, while repeated visible frames avoid re-synchronizing deferred GBuffer materials unless the source material actually changed.

**Why this priority**: The latest 2026-05-19 trace shows the previous `AView::DrainThreadedRendering` stall is gone, but `DeferredSceneRenderer::GetOrCreateGBufferMaterial`, threaded prepared draw capture, Scene View panel drawing, and renderer begin-frame work remain hot. This is the measured bridge toward UE-style retained render-thread state without attempting the full persistent scene rewrite in one risky step.

**Independent Test**: Renderer stats, deferred renderer material cache, FrameInfo/panel tests, and editor render-path contract tests verify the counters, panel/hierarchy observability, material revision stamps, and revision-safe material editing APIs.

**Acceptance Scenarios**:

1. **Given** a frame parses the scene, **When** `ParseScene()` completes, **Then** renderer-owned frame stats record the parse count and opaque/transparent/skybox drawable counts.
2. **Given** the same deferred source material is rendered repeatedly without parameter or render-state changes, **When** `GetOrCreateGBufferMaterial()` reuses the cached GBuffer material, **Then** it skips `SyncGBufferMaterial()` and records no GBuffer material sync for that frame.
3. **Given** the source material identity, parameter revision, or render-state revision changes, **When** the deferred renderer asks for the GBuffer material again, **Then** only that cached entry is synchronized and the sync counter increments.
4. **Given** material uniforms are edited through the material editor or texture deletion propagation, **When** values change, **Then** updates go through `Material::Set<T>()` so material parameter revisions and explicit binding caches are invalidated.
5. **Given** FrameInfo is open for a target view, **When** renderer stats are refreshed, **Then** it displays ParseScene calls, drawable counts, GBuffer syncs, binding-set creations, snapshot-buffer creations, and target-panel draw time.
6. **Given** the Hierarchy panel is part of a large imported scene, **When** performance attribution is needed, **Then** the hierarchy node count is exposed without forcing a full traversal from FrameInfo.

### User Story 8 - Renderer Uses Persistent Render Scene State (Priority: P1)

An engine developer can load a large model scene and have the renderer keep persistent render-thread-style primitive state and cached draw commands, so visible frames only refresh visibility/object data and submit visible command queues instead of rebuilding mesh/material draw inputs from components every frame.

**Why this priority**: This is the formal UE4.27-aligned architecture step after the low-risk hot-spot bridge. The latest trace still shows render preparation and threaded prepared draw capture costs after material sync caching, so Nullus needs retained scene state before larger visibility, object-buffer, sort/merge, and instancing work can be correct and measurable.

**Independent Test**: Render-scene unit tests verify that primitives and cached draw commands persist across identical frames, rebuild only when mesh/material command inputs change, update visible object descriptors every frame for transform/user-matrix changes, and expose visible opaque/transparent queues equivalent to the existing renderer path.

**Acceptance Scenarios**:

1. **Given** a scene with stable mesh renderer components, **When** consecutive frames synchronize render scene state, **Then** `RenderPrimitive` entries are reused and cached draw commands are not rebuilt.
2. **Given** a mesh, material, material state, or renderer override material changes, **When** the render scene synchronizes, **Then** only affected primitive cached draw commands are invalidated and rebuilt.
3. **Given** a transform or material-renderer user matrix changes, **When** visibility is gathered, **Then** cached draw commands are reused while the visible drawable object descriptor carries the latest matrices.
4. **Given** a camera frustum is active, **When** visibility is gathered, **Then** visible primitive/mesh membership is written into a bitset-backed visibility result before building visible command queues.
5. **Given** a scene has enough primitives to benefit from parallel work, **When** visibility is gathered, **Then** the visibility pass can partition primitive ranges and evaluate them in parallel without changing visible results.
6. **Given** visible draws are submitted, **When** per-object constants are required, **Then** per-frame object data is written into a ring/array buffer and draw commands carry an object index rather than forcing one uniform snapshot and binding set per draw.
7. **Given** visible cached commands share compatible state, **When** the command queue is finalized, **Then** opaque commands are sorted for state locality and front-to-back behavior while transparent commands keep back-to-front ordering.
8. **Given** compatible visible commands share mesh/material/pipeline state, **When** dynamic instancing is enabled, **Then** the queue can merge them into instanced command ranges with per-instance object indices.

### Edge Cases

- Generated model prefabs with no material bindings still need visible fallback material behavior without disabling mesh culling.
- Editor-created deferred mesh bindings must use the same culling policy as generated imported prefabs.
- Non-imported custom render helpers that intentionally use custom or disabled culling are out of scope unless they participate in imported model scene instantiation.
- Transparent draw order must remain distance-descending after replacing node-based collection with contiguous arrays.
- Opaque draw order may continue to be distance-ascending for compatibility until a future state-bucket/material sort is specified.
- Default material fallback must not turn a missing material slot into first-frame file IO during `ParseScene()`.
- Panel profiler labels must be cached without assuming all panel subclasses have a user-visible title.
- Destroying a widget without a parent container must remain safe.
- Removing or unconsidering one widget must not accidentally clear a pending dirty state for another destroyed widget in the same container.
- Moving an already destroyed widget into a new container must not strand that widget in the clean path.
- Removing an externally managed widget must clear its parent pointer so later destruction cannot dirty an unrelated old container.
- Public widget-list access must stay read-only so child ownership and dirty-state invariants cannot be bypassed by external vector mutation.
- Considering a widget already owned by another container must detach it from the previous parent first, and considering it twice in the same container must not duplicate ownership.
- Automatic cross-container widget movement must preserve the previous `EMemoryMode`; a move must not silently turn an external widget into an internally deleted widget or leak a previously internal widget.
- TreeNode label caching must preserve existing text-color styling hooks when present without dynamic provider callbacks in `_Draw_Impl()`; visual styling is not expanded in this performance pass.
- Timeline profiler CPU scope depth must stay high enough for current editor nesting without making unmatched suppressed scopes corrupt later scope balancing.
- Trace export must not replay frames that are still in the profiler history ring after a previous export call.
- Missing generated-model material slots must not force full texture loads from the resolution path, must yield when the per-frame resolution budget is exhausted, and must not prevent fallback material visibility.
- Material artifact prewarm that is used as a cache probe must not force cold shader or texture dependencies to load; normal material loading may still opt into dependency loading.
- Cache-only material misses are allowed to remain on the fallback material for this phase; async material/texture streaming and later replacement with true materials is a follow-up requirement, not a completed guarantee.
- Resolved prefab asset references may contain many sub-assets; renderer task collection must not linearly rescan the resolved-asset table for every mesh or material slot.
- Live generated-prefab source-object mappings may change while delayed resource-resolution tasks are queued; per-step lookup acceleration must be rebuilt from the currently live instance rather than stored as long-lived object pointers.
- Deferred editor-created mesh artifact loading may read `.nmesh` CPU data and compute mesh bounds off the UI frame, but RHI buffer/model construction must remain on the editor/RHI-owned frame step until a dedicated async GPU upload queue exists.
- Repeated generated-model mesh references to the same `.nmesh` must share one pending CPU artifact load instead of scheduling duplicate background reads.
- `.nmesh` v2 header and bounding-sphere fields must be encoded field-by-field in little-endian order. Vertex and index payloads remain a native editor cache layout in this phase and are not claimed as a cross-ABI interchange format.
- Background editor work must be globally bounded instead of creating one operating-system thread per scheduled task.
- DX12 upload-heap buffers must remain in their fixed `GenericRead` physical state, expose CPU-visible effective memory usage, and must not be transitioned by copy/barrier paths.
- Scene View may present the latest retired frame while a newer frame is still in flight; this is an accepted interactivity trade-off for ordinary navigation and inspection.
- Camera motion and gizmo activity should keep publishing new frames but must not independently force the main thread to wait for the just-submitted frame to retire.
- Explicit readback and resize safety paths remain allowed to drain because they require a known retired texture/resource lifetime.
- Renderer preparation counters must be renderer-owned frame stats so FrameInfo can report them without scraping profiler trace text.
- GBuffer material sync caching must key invalidation on stable source material identity plus explicit material revisions: identity prevents two runtime material instances with the same pass variant from sharing stale parameters, while revisions catch in-place edits.
- Public material uniform access must not expose a mutable map that bypasses revision tracking; mutation paths must go through `Material::Set<T>()` or an equivalent dirtying API.
- This phase does not introduce UE-style persistent `RenderScene` / cached draw-command invalidation yet; it only adds the instrumentation and local caches needed to make that larger rewrite measurable.
- Persistent render scene synchronization may still scan `Scene::FastAccessComponents` while the first retained-scene phase lands; the required behavior is that `RenderPrimitive` and `CachedDrawCommand` state persists and invalidates explicitly, not that scene-component discovery is already event-only.
- Transform/user-matrix changes must update per-frame object data or visible drawable descriptors without rebuilding cached mesh/material commands.
- Override materials are renderer-owned command inputs and must participate in cached command invalidation without mutating the underlying scene primitive state.
- Bitset visibility, parallel range partitioning, per-frame object buffers, sort/merge, and dynamic instancing are staged after the persistent primitive/command foundation so each step has an independent correctness gate.

## Requirements

### Functional Requirements

- **FR-001**: Imported model prefab generation MUST default mesh renderer frustum behavior to model-bound culling.
- **FR-002**: Editor deferred resource binding for imported meshes MUST preserve model-bound culling.
- **FR-003**: Editor viewport cameras and newly created default scene cameras MUST enable geometry frustum culling by default.
- **FR-004**: Mesh render resources used by per-frame draw preparation MUST be stable across repeated calls for the same mesh.
- **FR-005**: The model-scene importer version MUST change when generated prefab culling defaults change, so stale cached artifacts are regenerated.
- **FR-006**: Automated tests MUST cover imported prefab culling, editor viewport culling defaults, stable RHI mesh reuse, and importer version selection.
- **FR-007**: Scene drawable collection MUST avoid node-allocated sorted containers in the per-frame `ParseScene()` hot path.
- **FR-008**: Scene drawable collection MUST preserve existing opaque, transparent, and skybox draw iteration order semantics after switching to contiguous storage.
- **FR-009**: Scene render preparation MUST NOT synchronously load renderer assets, including default fallback materials, inside `ParseScene()`.
- **FR-010**: Panel drawing MUST expose cached per-panel profiler scope names that include the user-visible window name when available.
- **FR-011**: Hierarchy TreeNode drawing MUST avoid rebuilding the ImGui label string every draw when the node name and widget ID are unchanged.
- **FR-012**: WidgetContainer garbage collection MUST be dirty-gated so clean containers skip child-vector scans during normal draw.
- **FR-013**: Destroying a widget MUST mark its current parent container dirty when one exists and remain safe when the widget has no parent.
- **FR-014**: Cross-container widget reparenting MUST preserve the widget's prior memory management mode unless a future explicit ownership-transfer API is introduced.
- **FR-015**: TreeNode draw MUST NOT invoke dynamic color provider callbacks; hierarchy presentation color MUST be cached by the owning panel before draw.
- **FR-016**: Timeline profiler CPU scope capture MUST preserve at least the editor UI nesting needed to expose `Canvas` and panel draw scopes.
- **FR-017**: Timeline trace export MUST advance from the last exported frame and MUST NOT duplicate previously exported profiler frames.
- **FR-018**: Deferred generated-model material binding MUST NOT stop processing the current material task after the first missing cached material slot, MUST only probe cached material resources without synchronous material or texture loading, and MUST still respect the per-frame resource-resolution budget.
- **FR-019**: Editor generated-model fallback material creation MUST use already loaded editor startup shaders and MUST NOT call default material manager loading from the drag/drop resource-resolution path.
- **FR-020**: Editor deferred generated-mesh binding SHOULD avoid synchronous `.nmesh` file IO, synchronous `ModelManager` loading, and mesh bounds scans on the UI frame by loading CPU mesh artifact data, computing/storing artifact bounds, sharing duplicate pending artifact loads in background work, probing only already cached `ModelManager` resources, and sharing a transient CPU-to-GPU fallback model for cold paths, then constructing at most a bounded number of mesh/model resources per editor frame on the editor/RHI-owned path until the renderer has an async GPU upload queue.
- **FR-021**: Editor background task execution MUST use a bounded worker queue instead of creating one OS thread per task, MUST keep completed worker handles reusable, MUST stop launching queued work during editor shutdown, and MUST report scheduling rejection to async drag/drop completion paths instead of leaving pending work unresolved.
- **FR-022**: DX12 upload-heap buffers MUST report a fixed `GenericRead` state and CPU-to-GPU effective memory usage, MUST skip only legal fixed-state CPU-visible transitions, MUST reject illegal CPU-visible copy/barrier requests and CPU-to-GPU storage/UAV buffers, and DX12 textures MUST reject CPU-visible memory usage because upload/readback heaps do not support texture resources.
- **FR-023**: Material artifact prewarm MUST support cache-only shader and texture dependency resolution so cache probes do not synchronously load cold shader or texture resources.
- **FR-024**: Generated-prefab renderer resource resolution MUST index resolved asset metadata during task collection and MUST use a per-step live source-object index during delayed binding rather than repeated O(n) scans or long-lived cached object pointers.
- **FR-025**: Async drag/drop background import failures, including exceptions thrown before import completion, MUST always invoke the completion callback with a rejected status and diagnostic code.
- **FR-026**: Scene View MUST default to asynchronous retired-frame presentation and MUST NOT request synchronized retired-frame presentation solely because the camera moved, camera control is active, a transform gizmo is active, or a click-picking request is pending.
- **FR-027**: The generic view render drain policy MUST continue to drain when immediate readback or resize-safe retired-frame consumption explicitly requires it.
- **FR-028**: Renderer frame stats MUST record `ParseScene()` call count and parsed opaque, transparent, and skybox drawable counts for the current frame.
- **FR-029**: Renderer frame stats MUST record deferred GBuffer material synchronization count, render binding-set creation count, and render snapshot-buffer creation count for the current frame.
- **FR-030**: Deferred GBuffer material cache entries MUST skip source-to-GBuffer material synchronization until the stable source material identity, parameter revision, or render-state revision changes.
- **FR-031**: Material parameter mutations used by editor UI and texture deletion propagation MUST go through revision-aware material setters, and public material uniform/block access MUST NOT expose mutable map/block writes that bypass revision tracking.
- **FR-032**: FrameInfo MUST display ParseScene counts, drawable counts, GBuffer sync count, binding/snapshot creation counts, and target panel draw duration.
- **FR-033**: UI panels MUST expose the last measured draw duration, and Hierarchy MUST expose the current hierarchy node count for actionable panel-scale performance attribution.
- **FR-034**: The scene renderer MUST own persistent `RenderScene` state containing `RenderPrimitive` entries keyed by live mesh-renderer components.
- **FR-035**: Each render primitive MUST cache mesh/material draw command inputs and rebuild them only when model, mesh, material identity, material parameter revision, material render-state revision, primitive mode, or renderer override material inputs change.
- **FR-036**: Per-frame visibility gathering MUST reuse cached draw commands while updating object descriptors from current transform and user-matrix state.
- **FR-037**: Visibility results MUST have a bitset-backed representation suitable for range partitioning before visible command queues are built.
- **FR-038**: Large-scene visibility MUST support parallel primitive range evaluation without changing serial visible queue results.
- **FR-039**: Per-object constants MUST move toward a per-frame ring/array/structured buffer with object indices so visible draws do not create one uniform snapshot and binding set per object.
- **FR-040**: Visible cached command queues MUST support state-aware sorting/merge without breaking transparent back-to-front ordering.
- **FR-041**: Dynamic instancing MUST merge compatible visible cached commands into instance ranges using per-instance object indices and must remain disabled for incompatible materials, meshes, or pipeline state.

### Key Entities

- **Imported Model Prefab**: Generated scene hierarchy for a source model, including mesh renderer component settings and asset references.
- **Editor View Camera**: Camera used by SceneView and AssetView to render editor content.
- **Default Scene Camera**: Camera created with a new scene for GameView and runtime-preview rendering.
- **Mesh RHI Adapter**: Rendering-side representation returned for a mesh during draw preparation.
- **Model Scene Importer Version**: Cache invalidation marker used by ArtifactDB manifests.
- **Scene Drawable Collection**: Per-frame list of renderable mesh/material pairs used by forward and deferred scene renderers.
- **Panel Profiler Scope**: Cached profiling label for a UI panel draw call, used to identify expensive editor panels in timeline traces.
- **Hierarchy TreeNode Label**: ImGui display/ID label for a tree node, composed from the visible name and stable widget ID.
- **Widget Garbage Dirty Flag**: Container-local flag indicating whether a child has been destroyed and the child vector needs cleanup.
- **Timeline Trace Export Cursor**: Profiler-window state tracking the last exported CPU frame so history frames are not appended repeatedly.
- **Deferred Material Slot Cursor**: Per-task material slot index used by renderer resource resolution while binding cached generated-model materials.
- **Material Revision Stamp**: Stable source material instance identity plus parameter and render-state revisions used to decide whether a cached GBuffer material needs re-synchronization.
- **Render Preparation Counters**: Renderer-owned per-frame counters for scene parsing, drawable counts, GBuffer material syncs, binding-set creation, and snapshot-buffer creation.
- **Panel Draw Duration**: Last measured UI panel draw time in microseconds, used by FrameInfo to correlate editor-panel cost with renderer stats.
- **RenderScene**: Renderer-owned persistent scene cache synchronized from the engine scene and used to gather visibility and visible command queues.
- **RenderPrimitive**: Persistent render-side representation of a mesh renderer, its owner, model, material renderer, bounds inputs, and cached draw commands.
- **CachedDrawCommand**: Reusable mesh/material/state command input built when primitive render inputs change and referenced by visible-frame command queues.
- **Visibility Bitset**: Per-frame bit array marking visible primitives or commands before draw queue construction.
- **Object Data Index**: Per-frame index into object constant data used by cached draw commands after the object-buffer phase.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Imported model prefab tests prove generated mesh renderers use culling instead of disabling it.
- **SC-002**: Editor render path contract tests prove editor viewport and default scene cameras enable geometry culling.
- **SC-003**: Mesh resource tests prove repeated RHI mesh requests for one mesh return the same adapter.
- **SC-004**: Asset import tests prove the current model-scene importer version is updated for cache refresh.
- **SC-005**: Relevant unit-test targets pass after the implementation.
- **SC-006**: Editor render path contract tests prove drawable collection no longer uses `std::multimap` and sorts contiguous arrays after collection.
- **SC-007**: Editor render path contract tests prove default fallback material lookup in `ParseScene()` uses non-loading cached resource access.
- **SC-008**: Editor render path contract tests prove UI panel scopes, TreeNode label caching, and dirty-gated widget garbage collection are present.
- **SC-009**: Panel/window behavior tests prove dirty-gated garbage collection removes destroyed children only after `Destroy()` marks the parent container dirty.
- **SC-010**: Panel/window behavior tests prove automatic reparenting preserves internal and external widget ownership modes.
- **SC-011**: Editor render path contract tests prove TreeNode draw no longer contains a dynamic text-color provider callback.
- **SC-012**: Profiler destination tests prove timeline CPU scope depth keeps nested editor panel scopes and trace export uses a frame cursor.
- **SC-013**: Editor render path contract tests prove missing deferred material slots no longer delay generated-model resource resolution one frame per slot and remain cache-only fallback slots instead of synchronous material loads.
- **SC-014**: Editor render path and mesh resource tests prove generated-model fallback material uses loaded editor shader state, cold deferred mesh artifact loading stays CPU-only in background work, duplicate `.nmesh` references share a pending load and fallback transient model, completed binds do not call synchronous `ModelManager::operator[]`, CPU-to-GPU fallback mesh construction happens only during the bounded editor/RHI-owned bind step, precomputed artifact bounds are reused at bind time, and default mesh construction remains GPU-only.
- **SC-015**: Editor render path, DX12, and upload-context tests prove background work is queued on bounded reusable workers, drag/drop scheduling rejection completes with diagnostics, async drop completion revalidates parent objects from the live scene instead of dereferencing stale raw pointers, upload-heap buffers report `GenericRead` plus CPU-to-GPU effective memory usage, CPU-to-GPU storage buffers and CPU-visible textures are rejected, illegal CPU-visible buffer states are not silently swallowed, and CPU-visible upload destinations are not fed through copy/barrier upload paths.
- **SC-016**: Material conversion and editor render-path contract tests prove material artifact prewarm uses cache-only shader and texture dependency resolution, generated-prefab task collection uses resolved-asset indexes, live object lookup indexes are rebuilt once per resolution step from the current prefab instance, and async background import exceptions complete with `dragdrop-background-import-failed`.
- **SC-017**: Panel lifecycle tests prove Scene View no longer synchronizes retired-frame presentation by default, ordinary camera/gizmo/picking hints do not request a drain, and immediate readback/resize drain cases remain covered.
- **SC-018**: Updated trace evidence shows ordinary Scene View frames no longer spend their steady-state hot path in `AView::DrainThreadedRendering`.
- **SC-019**: Renderer stats tests prove render-preparation counters reset and accumulate correctly per frame.
- **SC-020**: Deferred renderer material-cache tests prove GBuffer material sync is skipped on unchanged source material identity/revisions and resumes after source material identity or revision changes.
- **SC-021**: Panel/window and editor render-path contract tests prove FrameInfo displays renderer-owned preparation counters, panels expose draw duration, Hierarchy exposes node count, and material uniform editing/deletion paths remain revision-aware.
- **SC-022**: Updated trace analysis identifies `DeferredSceneRenderer::GetOrCreateGBufferMaterial`, threaded prepared draw capture, Scene View panel draw, and renderer begin-frame work as the remaining hotspots after `AView::DrainThreadedRendering` disappears.
- **SC-023**: Render-scene tests prove stable scenes reuse persistent primitives and cached draw commands across frames.
- **SC-024**: Render-scene tests prove material/model command input changes invalidate cached draw commands while transform/user-matrix changes only update visible object data.
- **SC-025**: Visibility tests prove bitset-backed serial and parallel visibility produce identical visible opaque/transparent command queues.
- **SC-026**: Object-buffer tests prove repeated visible draws write object constants into per-frame indexed storage without creating per-draw object binding snapshots.
- **SC-027**: Command queue tests prove state-aware opaque sort/merge and dynamic instancing preserve draw eligibility and transparent ordering.

## Assumptions

- The current performance failure is dominated by editor render preparation and visibility work after the model is loaded, not by FBX source parsing.
- Large imported scenes contain many mesh nodes, making disabled culling and per-draw allocation visible as severe editor frame-rate loss.
- Focused automated tests are acceptable validation for this CPU/editor render-path fix; RenderDoc capture is reserved for GPU correctness or backend-specific rendering bugs.
- The current trace attribution is sufficient to prioritize the editor UI draw path, but per-panel scopes are needed before selecting a larger UI virtualization or caching pass.
- The updated trace and log point to CPU/editor instrumentation and resource-resolution behavior, not a GPU backend synchronization defect; RenderDoc evidence remains a follow-up only if later traces implicate render passes or backend state.
- UE4.27 comparison is used as architectural direction: persistent render-thread scene state, cached mesh draw commands, bitset/parallel visibility, sort/merge, dynamic instancing, and uniform-buffer content updates are long-term targets. This phase deliberately implements only low-risk observability and cache invalidation steps compatible with the current renderer.
