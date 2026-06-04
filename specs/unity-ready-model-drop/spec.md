# Feature Specification: Unity-Ready Model Drop

**Feature Branch**: `unity-ready-model-drop`
**Created**: 2026-05-31
**Status**: Draft
**Input**: User description: "调研unity是怎么做的，和unity对齐，为什么unity可以一拖入场景就能立马看到正确的效果；开始对齐unity方案"

## User Scenarios & Testing

### User Story 1 - Already Imported Models Start Preview Quickly (Priority: P1)

An editor user starts dragging an already imported large model into Scene View and gets a preview quickly enough that the interaction feels immediate rather than blocked by avoidable main-thread preparation work.

**Why this priority**: The reported workflow is specifically about already imported large models taking too long before anything useful appears. This is the primary speed expectation for the drag/drop interaction.

**Independent Test**: Drag an already imported large model whose generated prefab and dependency artifacts are current. The preview appears quickly without rebuilding mesh, material, texture, and prefab relationships on the drag path.

**Acceptance Scenarios**:

1. **Given** an already imported generated model prefab with current mesh, material, and texture relationships, **When** the user begins dragging it over Scene View, **Then** the preview appears quickly enough to support interactive placement.
2. **Given** the same already imported large model is dragged repeatedly, **When** the drag path runs, **Then** it reuses stabilized import results instead of reconstructing mesh, material, texture, and prefab relationships during drag.
3. **Given** the asset browser creates a drag payload for a current generated prefab artifact, **When** Scene View receives pre-drop hover updates, **Then** the hover path consumes the payload readiness hint and bridge fast-preview loader instead of refreshing the full asset database.

---

### User Story 2 - Drag Preview Follows The Mouse Before Drop (Priority: P1)

An editor user drags an imported model asset over the Scene View and sees a temporary preview object follow the mouse before the button is released.

**Why this priority**: This is the missing interaction the user explicitly asked to align with Unity. Without preview feedback, large-model drag/drop feels delayed and uncertain even if final placement succeeds.

**Independent Test**: Start dragging an imported model over Scene View. A preview object appears before drop, follows the current placement point, and disappears or converts only when the drag ends.

**Acceptance Scenarios**:

1. **Given** a ready imported model or prefab asset is dragged over Scene View, **When** the cursor moves across valid placement space before mouse release, **Then** a preview-only mesh ghost remains visible and updates to the latest placement point.
2. **Given** a preview object is active during drag, **When** the drag is still in progress, **Then** the preview does not become a committed scene instance and does not appear as a normal Hierarchy item.
3. **Given** a ready imported model has a loadable generated prefab artifact, **When** Scene View creates the drag preview, **Then** the preview uses a private preview scene or equivalent render-only proxy rather than the active scene.

---

### User Story 3 - Drop Commits Only Renderer-Ready Instances (Priority: P1)

An editor user releases the mouse after dragging an imported model, and the first committed scene instance is already ready to render with the intended mesh and material state.

**Why this priority**: The current failure mode is that large models can take a long time to appear because committed instances depend on delayed post-drop resource completion. The Unity-aligned target is that the committed result is either ready immediately or not committed yet.

**Independent Test**: Drop a generated model prefab whose manifest and cached renderer dependencies are current. The preview ends and one committed scene instance appears at the preview location without a delayed hidden-instance phase.

**Acceptance Scenarios**:

1. **Given** a generated model prefab with current prefab, mesh, and material dependencies, **When** the user releases the drag in Scene View or Hierarchy, **Then** the drop commits exactly one visible scene instance at the resolved drop target.
2. **Given** the final preview position is valid, **When** the committed instance is created, **Then** its placement matches the final preview placement rather than jumping to an unrelated fallback location.

---

### User Story 4 - Not-Ready Assets Stay Pending Instead Of Creating Delayed Hidden Instances (Priority: P2)

An editor user drops a model whose imported prefab or renderer dependencies are still stale, missing, or otherwise not ready. The editor reports a pending or not-ready result instead of creating a formal scene object that stays hidden while long asynchronous fix-up work runs.

**Why this priority**: This is the root-cause correction from the investigation. Large-model drag/drop should no longer rely on "commit first, hide root, wait for conservative async completion, then reveal" as the main path.

**Independent Test**: Simulate a generated model prefab with missing or stale required dependencies. The preview can end, but no committed scene object is left behind and the result indicates import or readiness is pending.

**Acceptance Scenarios**:

1. **Given** a generated model prefab whose required committed dependencies are stale or missing, **When** the user releases the drag, **Then** the editor does not create a committed scene instance and returns a pending or not-ready result.
2. **Given** a background import or readiness refresh is triggered for a not-ready asset, **When** that work completes, **Then** a later drag may commit normally through the ready path.

---

### User Story 5 - Legacy Async Resolution Cannot Regress The Main Drag/Drop Experience (Priority: P3)

If an older or non-primary path still instantiates generated model prefabs before all renderer references are fully resolved, it must remain a compatibility fallback only and must not reintroduce the long hidden-root waiting behavior as the normal large-model drop experience.

**Why this priority**: Existing async resource-resolution code still matters for compatibility, but it should no longer define the primary user-visible interaction for large model placement.

**Independent Test**: Exercise legacy generated-model resolution paths and verify they either complete as explicit fallback handling or fail cleanly without becoming the default large-model drag/drop path.

**Acceptance Scenarios**:

1. **Given** the primary drag/drop readiness gate succeeds, **When** the user drops a large model, **Then** the committed result does not depend on a long hidden-root async resolution sequence to become visible.
2. **Given** a fallback async resolution path is still used elsewhere, **When** it cannot resolve required renderer inputs, **Then** it fails or cancels without leaving a permanently hidden live object behind.

---

### User Story 6 - Scene Prefab Instances Save And Reload Like Unity PrefabInstance Records (Priority: P1)

An editor user drops or edits a prefab instance in a scene, saves the scene, closes or reloads it, and finds the instance restored from a Unity-style PrefabInstance record rather than from a plain unpacked object copy.

**Why this priority**: This is a confirmed data-loss bug. A successful drop is not usable if the scene cannot persist and restore the generated model instance. The current failure is not only a UI issue: scene save/load still behaves like ordinary object-graph serialization and does not yet reliably persist generated-model prefab instance identity together with enough reconstructible hierarchy state.

**Independent Test**: Commit a generated model instance into a scene, save the scene through the editor scene-save path, inspect the scene document, and verify it contains a document-level prefab instance record with source prefab identity, modifications, and source/instance correspondence. Reloading through the editor scene-loading path restores the model from that prefab instance relationship and rebuilds prefab-instance tracking.

**Acceptance Scenarios**:

1. **Given** a generated model instance was committed to a scene, **When** the scene is saved through editor scene save, **Then** the scene document contains a Unity-style prefab instance record instead of relying only on a `scenePrefab` property on the root object.
2. **Given** a generated model instance has transform/name/material or hierarchy overrides, **When** the scene is saved, **Then** those differences are persisted as prefab-instance modifications targeting source prefab objects.
3. **Given** a scene containing prefab instance records is reloaded through the editor, **When** the source prefab artifact is available, **Then** the instance is restored as a prefab-backed instance and editor prefab tracking is rebuilt.
4. **Given** the source prefab artifact is missing or corrupt on load, **When** the scene is opened, **Then** the editor preserves the prefab instance recovery record and reports missing prefab state rather than silently unpacking or losing the object.

### Edge Cases

- The cursor is over empty Scene View space and no scene geometry is hit; preview placement still needs a stable fallback plane.
- The asset was already imported successfully, but the drag path still encounters a cold or stale cache path; the drag experience should consume stabilized import results rather than rebuild relationships on the main path.
- The user drags over Scene View, then leaves the view or cancels the drag before release; the preview object must be cleaned up.
- The user drops onto Hierarchy instead of Scene View; no mouse-follow preview is required there, but committed-instance readiness rules still apply.
- The dragged asset handle points to an old asset id after reimport.
- A prefab artifact exists but one required renderer artifact file was deleted from `Library/Artifacts`.
- A legacy fallback path begins resolution work and the scene changes or the target object is destroyed during that work.
- A generated model instance is the only scene object added before save; scene reload must still restore it even if no other prefab instances exist.
- A saved scene references a generated model prefab sub-asset key; reload must resolve the same sub-asset rather than dropping the object when registry state starts empty.
- A generated model instance may carry most of its visible mesh hierarchy under one prefab root; scene persistence must not rely on only a flat root entry if that would drop the reconstructible model subtree.
- A scene created before Unity-style PrefabInstance records existed may still contain legacy `scenePrefab` root metadata; load must preserve compatibility, but new editor saves must prefer document-level prefab instance records.
- A normal prefab instance may be modified in the scene; scene reload must preserve the scene override without applying it back to the prefab asset.
- A generated model prefab instance may be modified in the scene; scene reload must preserve the scene override, while applying those changes back to the generated model asset remains rejected.

### User Story 7 - Unity-Style Artifact-To-Memory Loading Is Fast, Bounded, And Complete (Priority: P1)

An editor user repeatedly drags, previews, drops, or reopens an already imported large model, and the editor moves from committed artifacts to a renderable in-memory model through a measured, bounded, Unity-style pipeline: stable import artifacts, fast prefab graph reuse, cancellable renderer-resource prewarm, atomic mesh/material/texture visibility, low-copy native artifact reads, and model-level runtime cache reuse.

**Why this priority**: The remaining user-visible delay is no longer source import; it is the artifact-to-memory path. Unity-like behavior depends on stable import artifacts plus reclaimable hot caches and prewarmed renderer resources, not on keeping every imported mesh, material, and texture permanently resident or rebuilding every sub-resource synchronously on mouse release.

**Independent Test**: Load the same current generated model prefab through Scene View preview and final drop paths multiple times. The first cold load emits stage-level timing telemetry for prefab graph load, manifest validation, native container read/parse/hash, CPU deserialize, runtime resource creation, and GPU upload. The second and later loads reuse valid hot/cache entries where possible, treat mesh/material/texture as one renderability gate, avoid post-release blocking work, invalidate on artifact changes, and remain within a configurable memory budget.

**Acceptance Scenarios**:

1. **Given** a cold current generated model prefab is requested for preview, **When** the artifact-to-memory path runs, **Then** the editor records stage-level timings and byte counts that identify whether the delay is prefab graph load, manifest validation, native artifact file I/O, container parse/hash, CPU deserialization, runtime resource creation, or GPU upload.
2. **Given** a current generated model prefab was loaded from committed artifacts, **When** the same prefab is requested again during Scene View hover, drop, scene load, or selection preview, **Then** the bridge reuses valid hot prefab/model entries instead of rereading and reparsing unchanged prefab graph artifacts.
3. **Given** the manifest, prefab artifact, any mesh/material/texture artifact, source asset identity, importer version, project root, or load mode changes, **When** the prefab is requested again, **Then** stale cache entries are rejected and rebuilt from current artifacts.
4. **Given** many imported prefabs are previewed or dropped, **When** the hot cache exceeds its configured entry or byte budget, **Then** least-recently-used entries that are not active scene references are evicted instead of growing without bound.
5. **Given** a generated model has mesh, material, and texture dependencies, **When** the preview path considers the model renderable, **Then** mesh, material, and texture readiness are treated as one visibility gate so the user never sees a white model as the primary ready preview state.
6. **Given** the user begins dragging an already imported model, **When** renderer-resource prewarm is still running, **Then** the work is cancellable or resumable and mouse release does not synchronously wait for long cold loads on the UI thread.
7. **Given** mesh, material, and texture artifacts use the native artifact container format, **When** the loader reads them from disk, **Then** the hot path uses low-copy container views or equivalent single-allocation reads while preserving validation and hash semantics.
8. **Given** an imported model contains many submeshes sharing one model manifest, **When** repeated preview/drop paths request those submeshes, **Then** the model-level cache reuses the already opened/parsed model package or submesh alias mapping rather than independently cold-loading every submesh artifact.

---

### User Story 8 - Scene Load Streams Prefab Renderer Resources (Priority: P1)

An editor user opens a scene containing large generated/model prefab instances and can start using the editor as soon as the scene object graph is loaded, while prefab renderer resources continue loading in bounded background/per-frame work instead of blocking startup or scene switch completion.

**Why this priority**: The latest reported workflow is initial level load. Showing a startup/task progress dialog until every prefab's mesh/material/texture resource is restored makes the editor feel frozen and delays scene availability. Unity-like behavior loads scene/prefab identity first, then streams renderer resources and reveals each prefab only when its renderer dependencies are ready together.

**Independent Test**: Open a saved scene containing generated model prefab instances. The scene manager does not reread/reparse the scene document during prefab restore, scene-load renderer-resource resolution does not open a native blocking progress dialog after scene activation, and restored prefab resource jobs advance through the existing delayed/background step system.

**Acceptance Scenarios**:

1. **Given** a scene file has already been read and parsed by `SceneManager::LoadScene`, **When** editor prefab restoration runs, **Then** it reuses the loaded `ObjectGraphDocument` instead of reading and parsing the scene file a second time.
2. **Given** scene load restored generated model prefab instances, **When** their renderer resources are not yet ready, **Then** scene activation and editor interaction are not blocked by a native modal progress dialog for renderer-resource resolution.
3. **Given** a restored prefab has many mesh/material/texture dependencies, **When** resource resolution runs after scene activation, **Then** work is chunked through existing background/per-frame queues and each prefab subtree becomes visible only after mesh/material/texture are ready together.
4. **Given** a scene switch or editor shutdown happens while scene prefab streaming is active, **When** the old scene unloads, **Then** owner-tokened streaming work is cancelled or detached and cannot reveal or keep resources for destroyed scene instances.

## Requirements

### Functional Requirements

- **FR-001**: The import phase MUST generate and stabilize the relationship between mesh, material, texture, and generated model prefab artifacts before Scene View drag/drop consumes that asset.
- **FR-002**: The already-imported large-model drag/drop main path MUST reuse those stabilized import results instead of rebuilding mesh, material, texture, and prefab relationships on the drag path.
- **FR-003**: Dragging an imported model over Scene View MUST create a temporary drag preview object before mouse release.
- **FR-004**: For ready imported model and prefab assets, the drag preview SHOULD render a preview-only mesh ghost based on the generated prefab or prefab artifact.
- **FR-004a**: Scene View hover preview MUST NOT call full asset database refresh as part of the normal ready-preview path; it MUST use a payload-level ready hint and an asset-layer fast preview prefab loader based on current import artifacts.
- **FR-004b**: Scene View SHOULD reuse the existing preview-only mesh ghost for repeated hover updates of the same asset identity instead of re-instantiating the preview scene every frame.
- **FR-005**: The drag preview object MUST NOT be treated as a committed scene instance, MUST NOT appear as a normal Hierarchy item, and MUST NOT become the final persisted result automatically.
- **FR-006**: Ending a Scene View drag MUST destroy or replace the preview object as part of final drop handling.
- **FR-007**: Releasing a ready imported model drop MUST commit exactly one scene instance using the final preview placement or resolved drop target.
- **FR-008**: Generated model prefab drag/drop MUST treat renderer dependency readiness as part of drop-time asset readiness.
- **FR-009**: The primary generated-model drop path MUST NOT commit a formal scene instance that depends on a long hidden-root async resolution phase before first visibility.
- **FR-010**: If required committed renderer dependencies are not ready at drop time, the editor MUST return a pending, importing, or not-ready result instead of committing the scene instance.
- **FR-011**: Fast prefab loading MUST verify current manifest dependencies and required committed renderer artifact files, not only the prefab artifact file.
- **FR-012**: The already-imported large-model drag/drop main path MUST avoid heavy synchronous work whose responsibility belongs to import-time asset stabilization.
- **FR-013**: Scene View preview behavior MAY use controlled visual degradation for partially cached assets, but committed scene instances MUST follow the ready-or-pending rule.
- **FR-014**: Hierarchy and Scene View drops MUST share the same committed-instance readiness gate even if only Scene View provides mouse-follow preview.
- **FR-015**: Existing async renderer resolution for generated models MUST be treated as a compatibility fallback path rather than the primary large-model Scene View drop experience.
- **FR-016**: Existing async renderer resolution MUST still fail or cancel cleanly when required assets cannot be resolved and MUST NOT leave a still-live generated model root permanently hidden.
- **FR-017**: Editor scene save MUST persist prefab instances as document-level Unity-style `PrefabInstance` records containing source prefab identity, scene instance root identity, property/structure modifications, and source-to-instance object correspondence.
- **FR-018**: New editor scene saves MUST NOT rely on a root-object-only `scenePrefab` metadata property as the authoritative prefab instance representation; that property MAY remain readable as a legacy compatibility fallback.
- **FR-019**: Scene reload MUST restore committed prefab instances from their prefab asset id and prefab sub-asset identity, then apply saved prefab-instance modifications instead of treating the result as an untracked plain object copy.
- **FR-020**: Loading a scene that contains prefab instance records MUST rebuild the corresponding prefab-instance tracking state needed by editor prefab presentation and later prefab-aware operations.
- **FR-021**: The generated-model save and reload path MUST preserve normal transform, hierarchy parentage, prefab-sub-asset targeting, and source/instance correspondence for restored instances.
- **FR-022**: Scene persistence for generated model instances MUST NOT depend solely on transient editor-only prefab instance registry state.
- **FR-023**: Scene persistence for generated model instances MUST preserve enough data to reconstruct the full committed model instance, including prefab-backed hierarchy recovery rather than only a flat root scene entry.
- **FR-024**: Normal prefab instances MUST allow scene-local overrides to be saved and reloaded without automatically applying those overrides back to the prefab asset.
- **FR-025**: Generated model prefab instances MUST allow scene-local overrides to be saved and reloaded, but applying overrides back to the generated model prefab asset MUST remain rejected with a model-prefab/read-only diagnostic.
- **FR-026**: Unity-style prefab instance restoration MUST keep legacy `scenePrefab` scenes loadable, but any newly saved scene that has prefab registry state MUST emit the new `PrefabInstance` record path.
- **FR-027**: The artifact-to-memory path MUST emit editor-session telemetry for prefab graph load, manifest validation, dependency scan, native artifact file I/O, native container parse/hash validation, CPU deserialization, runtime resource creation, GPU upload, cache hit/miss, cancellation, and eviction events.
- **FR-028**: Telemetry MUST be available to focused tests and manual editor validation so cold and warm drag/drop behavior can be compared without relying on subjective "feels faster" reports.
- **FR-029**: The already-imported prefab fast-load path MUST use a bounded editor-session hot cache for successful prefab artifact loads so repeated Scene View hover/drop requests do not synchronously reread and reparse the same current prefab artifact.
- **FR-030**: The prefab hot cache key MUST include source asset id, normalized asset path, prefab sub-asset key, asset type, load mode, importer id/version, project root identity, manifest file stamp, and prefab artifact file stamp so reimported, edited, or cross-project artifacts are not reused stale.
- **FR-031**: The prefab hot cache MUST store only successful prefab graph and resolved-asset results; renderer dependency readiness failures, missing artifacts, partial material loads, and pending import states MUST remain uncached or cached only as short-lived negative telemetry that cannot hide a later success.
- **FR-032**: Mesh, material, and texture readiness MUST be evaluated as one atomic preview/drop visibility gate for generated model prefabs; a preview or committed instance MUST NOT show a white model as the normal ready state when material or texture dependencies are still missing.
- **FR-033**: Renderer-resource prewarm MAY run after import completion, asset selection, hover, editor idle, or scene load, but it MUST be cancellable, resumable where practical, and bounded by the same memory and active-reference rules as the hot cache.
- **FR-034**: Mouse release in Scene View MUST NOT start a long synchronous artifact-to-memory load for already imported current assets when the same work could have been scheduled on hover or editor-idle prewarm.
- **FR-035**: Native artifact readers used by mesh, material, texture, and prefab fast paths MUST support a low-copy container view or equivalent single-allocation read path that avoids repeated full-file and payload copies while preserving existing validation, hash, and error-reporting semantics.
- **FR-036**: The loader MUST avoid per-submesh repeated cold reads when a model-level runtime package or cache entry can provide submesh aliases, shared material bindings, and texture dependency references from one current model artifact relationship.
- **FR-037**: Hot caches MUST expose explicit entry-count and byte-budget controls plus deterministic LRU-style eviction; they MUST NOT keep every imported model's mesh/material/texture payload permanently resident just because import completed.
- **FR-038**: Cache invalidation MUST cover manifest, prefab, mesh, material, texture, importer version, source asset identity, project root identity, explicit reimport, artifact deletion, and budget eviction.
- **FR-039**: Active scene instances MUST keep their required runtime resources alive through normal resource ownership, but removal of a prefab instance from the scene MUST release scene-held references so caches may evict the model and CPU/GPU work no longer continues for removed objects.
- **FR-040**: The implementation MUST treat the current runtime mesh/resource-manager artifact cache and equivalent-path aliasing as the SSoT for imported model runtime cache behavior; the historical `.nmodel`/`ModelManager` plan from `specs/026-asset-management-system` is a reference only unless the current branch reintroduces that concrete API through a separate migration.
- **FR-041**: Removing or cancelling a preview/committed prefab instance MUST cancel or detach related in-flight prewarm requests, async renderer-resolution jobs, renderer queues, per-frame update sources, and draw-source registrations associated only with that instance.
- **FR-042**: The default validation budget for an already imported current model MUST require textured preview first visibility within 200 ms on warm cache, Scene View mouse-release UI-thread synchronous work within one frame budget at 60 Hz, and hot-cache fast-load lookup within 10 ms; exceeding any budget MUST fail Phase 7 validation unless the plan records a measured hardware-specific waiver with the responsible stage and follow-up task.
- **FR-043**: Renderer-specific evidence MUST be captured at least once for the ready textured preview/committed model path during implementation validation, using RenderDoc or an equivalent backend-specific inspection, so material/texture binding correctness is not inferred only from unit tests.
- **FR-044**: Runtime resource residency MUST follow a Unity-like owner/reference model: scene instances, preview scenes, editor inspectors, and asynchronous prewarm jobs acquire mesh/material/texture resources under explicit owner tokens, and owner removal releases references without blindly destroying resources that other owners still use.
- **FR-045**: The editor MUST provide an `UnloadUnusedAssets`-style trim path that unloads only resources with zero active owners, using deterministic LRU/budget rules and normalized artifact-path aliases so absolute paths and `Library/...` paths share the same lifetime entry.
- **FR-046**: Preview cancellation, Scene View drag exit, scene object destruction, and scene unload MUST release their owner tokens; the later trim pass MAY evict unreferenced resources, but active scene or inspector references MUST keep shared mesh/material/texture resources alive.
- **FR-047**: Resource managers MUST expose a `ResourceHandle<T>` acquisition path that records a resource id, generation, owner token, and normalized path, while preserving the legacy raw-pointer API during migration.
- **FR-048**: `ResourceHandle<T>` MUST be move-only RAII: constructing or acquiring a handle adds an owner reference, destruction/reset releases that exact resource reference, and `Get()` MUST return null after the registry generation changes.
- **FR-049**: Cross-frame editor/runtime code SHOULD store `ResourceHandle<T>` or `ResourceId` instead of raw `Mesh*`, `Material*`, or `Texture2D*`; long-lived raw pointer storage remains legacy-only until each subsystem is migrated.
- **FR-050**: Scene load MUST separate scene-object activation from generated/model prefab renderer-resource restoration so opening a scene does not synchronously wait for every prefab mesh/material/texture artifact to finish loading.
- **FR-051**: Editor prefab restoration after `SceneManager::LoadScene` MUST reuse the already parsed scene `ObjectGraphDocument` when available and MUST NOT reread/reparse the same scene file as the normal startup or scene-switch path.
- **FR-052**: Scene-load renderer-resource restoration MUST use non-blocking/background progress semantics rather than opening a native blocking task dialog after the scene object graph is active.
- **FR-053**: Scene-load prefab streaming MUST keep the no-white-model invariant: restored generated/model prefab renderers remain suppressed until their mesh/material/texture dependencies are ready together, then are revealed atomically per prefab instance or safe subtree.
- **FR-054**: Scene unload, scene switch, prefab removal, or editor shutdown MUST cancel/detach scene-load streaming work and release owner tokens for the affected scene instances.

### Key Entities

- **Generated Model Prefab**: Imported model prefab artifact produced from an external model source.
- **Drag Preview Object**: Temporary, non-committed Scene View object used only while the user is actively dragging before mouse release.
- **Preview-Only Mesh Ghost**: A ready prefab/model preview rendered from a private preview scene or render-only proxy, separate from the committed scene object graph.
- **Preview-Ready Payload Hint**: A drag payload flag set when the asset browser observes a current prefab artifact in the stabilized import output, allowing Scene View hover to avoid full database refresh and heavy relationship reconstruction.
- **Committed Scene Instance**: The formal scene object created only after a drop succeeds.
- **Renderer Dependency Readiness Gate**: Validation step that decides whether a generated model can be committed immediately or must remain pending.
- **Legacy Async Resolution Path**: Older compatibility flow that resolves deferred renderer references after instantiation.
- **PrefabInstance Record**: The Unity-style persisted scene representation of a committed prefab instance, including source prefab reference, local modifications, and correspondence between prefab source objects and scene instance objects.
- **Prefab Modification**: A scene-local patch such as a replaced property, added/removed owned component or GameObject, or ordering change that is saved on the instance rather than applied to the source prefab.
- **Stripped/Correspondence Record**: The scene-side mapping that identifies which scene object corresponds to which object in the source prefab, analogous to Unity stripped object records.
- **ArtifactLoadTelemetry**: Stage-level timing and byte-count record for moving committed artifacts into preview, scene, runtime, and GPU-ready memory.
- **ImportedPrefabHotCache**: Bounded editor-session cache for successful current prefab graph and resolved-asset loads, keyed by source asset id, normalized asset path, prefab sub-asset key, asset type, load mode, importer id/version, project root identity, manifest stamp, and prefab artifact stamp.
- **RendererResourcePrewarmRequest**: Cancellable request that warms mesh, material, texture, runtime resource, and GPU upload dependencies before the final drop needs them.
- **NativeArtifactContainerView**: Low-copy validated view over a native artifact container payload used by artifact loaders to avoid redundant file and payload copies.
- **ModelRuntimeCacheEntry**: Bounded runtime/cache entry backed by the current mesh/resource-manager artifact cache and equivalent-path aliasing SSoT, representing loaded submesh artifacts, shared material bindings, and texture dependency readiness without duplicating model package ownership.
- **ArtifactMemoryBudget**: Configurable memory budget that controls prefab graph, model package, mesh/material/texture, and prewarm cache residency.
- **ResourceLifetimeRegistry**: Unity-like owner/reference registry that records which scene, preview, inspector, or async job currently owns normalized mesh/material/texture artifact resources, tracks last-use order, and produces trim candidates only after all owners release the resource.
- **ResourceHandle<T>**: Move-only typed resource lease returned by resource managers. It stores the resource id, generation, owner token, and optional cached pointer, automatically releases the owner reference on reset/destruction, and refuses to expose a pointer after reimport/unload generation invalidation.
- **ScenePrefabStreamingQueue**: Scene-load restoration flow that queues generated/model prefab renderer-resource work after scene activation and advances it through existing background/per-frame editor actions without blocking the native startup or task progress UI.

## Success Criteria

### Measurable Outcomes

- **SC-001**: During Scene View drag, a ready imported model shows a preview-only mesh ghost before mouse release and tracks the cursor placement continuously until the drag ends.
- **SC-002**: For already imported large models with current generated prefab relationships, preview appearance is fast enough that drag placement remains interactive instead of feeling blocked by main-thread stabilization work.
- **SC-003**: A ready generated model drop commits exactly one visible scene instance without a user-visible long hidden-instance waiting phase.
- **SC-004**: A not-ready generated model drop commits zero scene instances and returns a pending or not-ready result instead of leaving a delayed hidden formal instance in the scene.
- **SC-005**: Focused tests cover Scene View preview lifecycle, ready commit, not-ready pending behavior, save-and-reload persistence of generated model instances, and legacy async cancellation or failure fallback handling.
- **SC-006**: For the reported already-imported large-model workflow, the drag/drop path no longer requires the user to wait through a long post-drop invisibility period before the first committed object appears.
- **SC-007**: Saving and reloading a scene containing a committed generated model instance restores that instance without manual repair or re-drop.
- **SC-008**: New editor scene saves for prefab instances contain document-level `PrefabInstance` records with source prefab references and modifications, and do not depend on root-only `scenePrefab` metadata as the primary persistence path.
- **SC-009**: Scene-local prefab instance modifications round-trip for normal prefabs and generated model prefabs; generated model prefab asset apply remains rejected.
- **SC-010**: Cold and warm ready generated-model Scene View preview/drop requests produce artifact-to-memory telemetry that identifies the dominant stage and shows cache hit/miss behavior.
- **SC-011**: Repeated ready generated-model Scene View preview/drop requests hit the prefab/model hot caches after the first successful load, while manifest, prefab, mesh, material, or texture artifact edits force a miss and reload.
- **SC-012**: Ready generated-model previews and committed drops render mesh, material, and texture together; the primary ready path does not expose a white model while texture or material dependencies are still pending.
- **SC-013**: Warm repeated drag/drop avoids prefab artifact reread/reparse and avoids repeated per-submesh cold reads when a current model-level runtime cache entry exists.
- **SC-014**: Low-copy native artifact readers preserve existing validation and error diagnostics while reducing redundant full-file or payload copies on the hot path.
- **SC-015**: Hot cache eviction keeps memory growth bounded when many imported prefabs are touched during one editor session, and removing a prefab from the scene releases scene-held references so CPU/GPU work can fall when the object is gone.
- **SC-016**: Warm already-imported large-model drag/drop telemetry reports textured preview first visibility, release-time synchronous work, cache lookup time, and any budget misses with enough stage detail to identify the remaining bottleneck.
- **SC-017**: Removing or cancelling a prefab/preview stops instance-owned prewarm, async resolution, renderer queue, update, and draw registrations; no instance-only CPU work continues after the object is gone.
- **SC-018**: Renderer-specific validation confirms that ready preview and committed draws bind the intended mesh, material, and texture resources together.
- **SC-019**: Initial scene load with saved generated/model prefabs activates the scene without rereading/reparsing the same scene file for prefab restore and without showing a native blocking renderer-resource dialog after scene activation.
- **SC-020**: Large restored generated/model prefabs stream renderer resources after scene activation and reveal only when textured renderer dependencies are ready together; scene switch or removal cancels old streaming work.

## Assumptions

- This phase targets the editor/DX12 workflow reported by the user; Vulkan/macOS/Linux claims require separate validation.
- The current asset pipeline continues to generate and stabilize model-prefab, mesh, material, and texture relationships before scene placement.
- The current known save/reload failure is caused by missing or incomplete persistence of generated-model prefab instance identity and reconstructible hierarchy state; this phase is expected to close that gap rather than work around it manually.
- Scene View preview and committed-instance placement may reuse existing editor picking and spawn-point logic where possible.
- Preview interaction is in scope for Scene View drag; Hierarchy drop continues to focus on committed result behavior rather than mouse-follow preview.
- Full Unity parity for advanced placement behaviors such as surface-normal alignment or complex multi-scene drag semantics is out of scope for this phase.
- Persisting generated model instances should preserve prefab-instance semantics rather than silently converting them into unrelated unpacked scene copies.
