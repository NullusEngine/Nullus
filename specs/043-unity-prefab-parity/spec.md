# Feature Specification: Unity Prefab Parity Phase 2

**Feature Branch**: `043-unity-prefab-parity`  
**Created**: 2026-06-04  
**Status**: Draft  
**Input**: User description: "Align Nullus prefab behavior with Unity 2018.4: complete PrefabInstance semantics, unify drag/drop and scene-load prefab loading, provide real PreviewScene-style mouse-follow preview, close ResourceHandle/ResourceLifetime ownership gaps, and make save, load, modify, delete, trim, and missing-prefab behavior Unity-like."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Prefab Instances Round-Trip Like Unity (Priority: P1)

An editor user places a normal prefab or imported model prefab into a scene, edits its instance data, saves the scene, closes or switches scenes, and later reloads the scene with the same prefab connection, scene-local overrides, source-to-instance correspondence, and missing-prefab recovery state.

**Why this priority**: The most damaging failure mode is data loss or silently unpacked prefab instances. Unity's core behavior is that scenes persist a `PrefabInstance` relationship plus modifications instead of relying on a copied hierarchy that can drift from the source prefab.

**Independent Test**: Create a scene with one normal prefab instance and one imported model prefab instance, edit transform/name/material and hierarchy structure where supported, save, reload, and inspect the document and runtime registry.

**Acceptance Scenarios**:

1. **Given** a connected prefab instance with scene-local transform and material overrides, **When** the scene is saved, **Then** the scene document contains an authoritative prefab-instance record with source prefab identity, instance root identity, modifications, and source/instance correspondence.
2. **Given** a saved prefab instance scene, **When** the scene is reloaded, **Then** the instance is reconstructed from the source prefab plus modifications and remains connected to the prefab registry.
3. **Given** an imported/model prefab instance is modified in the scene, **When** the user saves and reloads, **Then** scene-local overrides survive while apply-to-model-prefab remains rejected with a read-only model prefab diagnostic.
4. **Given** the source prefab artifact is missing or corrupt, **When** the scene is loaded, **Then** the recovery record remains visible and the editor reports missing prefab state instead of silently deleting or unpacking the instance.

---

### User Story 2 - One Prefab Loading Path For Drag, Drop, And Scene Load (Priority: P1)

An editor user observes the same prefab readiness, cache, material/texture binding, and first-visible behavior whether a prefab is opened from an existing scene, dragged from the asset browser, duplicated, or repeatedly instantiated.

**Why this priority**: The reported behavior shows scene-load prefabs and directly dragged prefabs still diverge. Unity-like behavior depends on one asset identity and runtime resource path rather than separate "scene restore" and "drag/drop" implementations.

**Independent Test**: Load a scene containing an imported model prefab, then drag the same prefab again. Both paths must use the same prefab load request model, cache key, renderer-resource readiness gate, ResourceHandle ownership, and no-white-model visibility rule.

**Acceptance Scenarios**:

1. **Given** a scene already contains a loaded generated/model prefab, **When** the user drags the same prefab into Scene View, **Then** the second instance fast-binds from the same current cached runtime resources without a new cold synchronous load on mouse release.
2. **Given** a prefab is restored during scene load, **When** its mesh/material/texture resources are not ready together, **Then** the prefab remains non-renderable or explicitly pending until all renderer dependencies can bind atomically.
3. **Given** manifest, prefab, mesh, material, texture, importer version, project root, or artifact stamps change, **When** any path requests the prefab again, **Then** stale cache entries are rejected consistently for scene load, preview, and drop.

---

### User Story 3 - Drag Preview Is A Textured Mouse-Follow Prefab Preview (Priority: P1)

An editor user drags a prefab or imported model over Scene View and immediately sees the actual prefab shape, with mesh, material, and textures together, following the mouse before release. The preview is not a label, crosshair, white model, or committed scene object.

**Why this priority**: This is the user's primary interaction expectation and the most visible gap versus Unity. The editor should begin work on hover and show a temporary preview object before the final drop.

**Independent Test**: Drag an already imported large model prefab across Scene View without releasing the mouse. The preview appears while dragging, tracks placement updates, renders textured materials, and disappears on cancel or converts to exactly one committed instance on drop.

**Acceptance Scenarios**:

1. **Given** a ready prefab drag payload enters Scene View, **When** hover updates are received, **Then** the editor creates or reuses a private preview-scene instance that follows the resolved mouse placement.
2. **Given** material or texture dependencies are missing, **When** the preview path evaluates readiness, **Then** it does not present a white model as the ready state.
3. **Given** the user releases the mouse, **When** the asset is ready, **Then** the final committed prefab instance appears at the last preview placement without long UI-thread blocking.
4. **Given** the user cancels the drag or leaves Scene View, **When** preview ownership ends, **Then** preview scene objects, prewarm jobs, owner tokens, and draw registrations are cleaned up without touching existing scene instances.

---

### User Story 4 - Resource Lifetime Matches Unity-Like Ownership (Priority: P1)

An editor user deletes a prefab, switches scenes, cancels a drag, or runs an unused-resource trim and sees resources remain alive only while active owners need them. Removed objects stop consuming CPU/GPU work, but shared resources are not prematurely destroyed while another scene, preview, or inspector owner still uses them.

**Why this priority**: Previous fixes introduced resource lifetime hazards: removed prefabs kept consuming work, preview cancellation could affect existing scene prefabs, and cached resources could either live forever or disappear too early.

**Independent Test**: Load two instances sharing one imported model resource set, remove one, cancel a preview using the same resources, trim unused assets, then remove the final owner and trim again.

**Acceptance Scenarios**:

1. **Given** two prefab instances share mesh/material/texture resources, **When** one instance is removed, **Then** only that instance's owner tokens and jobs are released while the remaining instance stays visible.
2. **Given** no scene, preview, inspector, or async job owns a resource, **When** the trim path runs, **Then** the resource becomes eligible for deterministic LRU/budget eviction.
3. **Given** a stale `ResourceHandle` remains after unload or reimport, **When** code attempts to resolve it, **Then** it returns null rather than exposing a pointer to destroyed or newly reloaded memory.

---

### User Story 5 - Editor Prefab UX Surfaces Unity-Like State (Priority: P2)

An editor user can understand and control prefab state through hierarchy and inspector presentation: connected prefab, model-prefab read-only source, overrides, missing source, apply/revert availability, and pending renderer-resource loading state.

**Why this priority**: The runtime behavior must be correct first, but Unity parity also includes clear editor feedback so users can tell whether an object is connected, overridden, missing, pending, or unpacked.

**Independent Test**: Select normal prefab, imported model prefab, missing-source prefab, and pending-resource prefab instances and verify hierarchy badges plus inspector state match the underlying prefab-instance and resource-readiness state.

**Acceptance Scenarios**:

1. **Given** a connected prefab instance is selected, **When** the inspector opens, **Then** it lists scene-local overrides and offers valid apply/revert actions.
2. **Given** an imported/model prefab instance is selected, **When** the user attempts apply-to-asset, **Then** the editor rejects the apply operation with a model-prefab read-only diagnostic while still allowing scene-local revert.
3. **Given** a missing prefab source is loaded from scene recovery data, **When** the object appears in hierarchy/inspector, **Then** it is marked missing and preserves recovery metadata for future repair.

### Edge Cases

- Drag payload has a valid source asset id but an empty or stale prefab sub-asset key.
- Scene file contains legacy root-only `scenePrefab` metadata but no document-level prefab-instance record.
- Source prefab reimports while a preview, scene load, or async prewarm job is in flight.
- Two projects contain equivalent relative asset paths but different project roots and artifact identities.
- Prefab has nested children whose local ids changed after reimport.
- User deletes a prefab instance while scene-load streaming is still binding renderer resources.
- User cancels drag after preview resources were acquired but before the preview scene is visible.
- A material references a texture artifact that exists on disk but fails validation.
- Trim runs while an inspector preview and a scene instance share the same texture.
- Scene load completes object graph activation while renderer-resource streaming is still pending.
- A saved override targets a source object that no longer exists in the current prefab.
- Hierarchy drop path has no mouse-follow preview but must still use the same readiness and lifetime rules.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Scene save MUST persist connected prefab instances as first-class document-level prefab-instance records with source prefab identity, instance root identity, modifications, and source/instance correspondence.
- **FR-002**: Scene load MUST restore prefab instances from source prefab assets plus saved modifications before treating any legacy root-only prefab metadata as a fallback.
- **FR-003**: Prefab correspondence MUST use stable source object identifiers and scene instance identifiers so property and structural overrides can target the intended object after reload.
- **FR-004**: Normal prefab instances MUST allow scene-local overrides to save, reload, apply, and revert according to Unity-like semantics.
- **FR-005**: Imported/model prefab instances MUST allow scene-local overrides to save and reload, but applying changes back to the generated/model prefab asset MUST be rejected as read-only.
- **FR-006**: Missing or corrupt source prefab load MUST preserve recovery metadata and expose missing-prefab state instead of deleting, silently unpacking, or fabricating a disconnected object.
- **FR-007**: Scene-load, Scene View preview, final drop, duplication, and repeated instantiation MUST all route through one prefab load request model and one renderer-resource readiness gate.
- **FR-008**: The unified prefab load key MUST include project identity, source asset id/path, prefab sub-asset key, importer id/version, manifest stamp, prefab artifact stamp, and mesh/material/texture artifact stamps.
- **FR-009**: Cache invalidation MUST behave identically for scene load, drag preview, drop, and duplicate paths.
- **FR-010**: Scene View drag preview MUST instantiate or render a private preview-scene prefab object before mouse release for ready prefab/model assets.
- **FR-011**: Preview scene objects MUST NOT enter the active scene hierarchy, prefab registry, scene persistence, selection set as committed objects, or normal scene undo history.
- **FR-012**: Preview placement MUST follow the resolved Scene View mouse placement and final drop MUST use the last preview placement when valid.
- **FR-013**: Mesh, material, and texture readiness MUST be treated atomically for preview and committed visibility; the normal ready path MUST NOT show a white model while material or texture data is pending.
- **FR-014**: Mouse release MUST NOT start a long cold synchronous prefab artifact-to-memory load when hover, selection, scene load, or editor-idle prewarm could have scheduled the work earlier.
- **FR-015**: Preview cancel, scene switch, scene unload, object deletion, and editor shutdown MUST release their owner tokens and cancel or detach related async jobs, renderer queues, update registrations, and draw-source registrations.
- **FR-016**: Active scene instances, preview scenes, inspectors, and async jobs MUST acquire explicit resource owners for mesh/material/texture runtime resources.
- **FR-017**: `ResourceHandle<T>` MUST be move-only RAII and MUST release exactly its acquired owner reference on reset or destruction.
- **FR-018**: Resource generation changes after reimport, reload, unload, or trim MUST invalidate stale handles and prevent stale raw pointers from becoming authoritative.
- **FR-019**: The unused-resource trim path MUST unload only zero-owner resources, using deterministic LRU/budget behavior and normalized artifact path aliasing.
- **FR-020**: Removing the last scene instance of a prefab MUST make its resources trim-eligible and stop instance-owned CPU/GPU work after related queues observe cancellation.
- **FR-021**: Scene-load prefab restoration MUST activate the scene object graph before large generated/model prefab renderer-resource streaming completes, while still preventing white-model intermediate visibility.
- **FR-022**: Scene-load and drag/drop telemetry MUST report cold/warm timing stages, cache hits/misses, resource owner acquire/release, cancellation, trim, and first textured visibility.
- **FR-023**: Editor hierarchy and inspector MUST expose connected, overridden, read-only model-prefab, missing-prefab, and pending-resource states after the core runtime behavior is stable.
- **FR-024**: The implementation MUST preserve existing legacy prefab scenes and root `scenePrefab` scenes as readable compatibility input, but new saves MUST prefer document-level prefab-instance records.
- **FR-025**: Validation MUST include automated tests plus manual editor evidence for textured mouse-follow preview, save/reload, override round trip, deletion release, scene switch cancellation, no-white-model state, and scene-load streaming.
- **FR-026**: Rendering correctness for at least one validated backend MUST be proven with RenderDoc or equivalent renderer evidence showing mesh/material/texture bindings for preview and committed paths.

### Key Entities

- **PrefabSourceIdentity**: Stable identity for a prefab asset or imported model prefab source, including project root, source asset id/path, importer version, and prefab sub-asset key.
- **PrefabInstanceRecord**: Scene document record that links a source prefab to an instance root and contains modifications, correspondence, recovery metadata, and read-only model-prefab flags.
- **PrefabObjectCorrespondence**: Mapping from source object identifiers to scene instance identifiers, equivalent in purpose to Unity stripped object correspondence.
- **PrefabModification**: Scene-local property or structural override stored on the instance rather than applied to the source asset.
- **UnifiedPrefabLoadRequest**: Shared request model used by scene load, preview, final drop, duplicate, and repeated instantiation.
- **PrefabRuntimeCacheEntry**: Current cached prefab graph and renderer-resource state keyed by source identity and artifact stamps.
- **PreviewScenePrefabInstance**: Temporary preview-only object graph rendered in Scene View while dragging.
- **ResourceOwnerToken**: Owner lease for scene instance, preview scene, inspector, or async job resource usage.
- **ResourceHandle<T>**: Move-only typed resource lease that validates generation and releases owner references.
- **ScenePrefabStreamingJob**: Bounded background/per-frame job that restores renderer resources for scene-loaded prefab instances after scene object graph activation.
- **PrefabEditorState**: Hierarchy/inspector presentation state for connected, overridden, missing, pending, read-only, and unpacked prefab instances.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A saved scene containing normal and imported/model prefab instances reloads with connected prefab registry state and all supported scene-local overrides intact.
- **SC-002**: Newly saved scenes contain document-level prefab-instance records and no longer depend on root-only `scenePrefab` metadata as the authoritative representation.
- **SC-003**: Dragging a ready already-imported model prefab over Scene View shows a textured mouse-follow preview before release, with no crosshair-only, label-only, or white-model ready state.
- **SC-004**: Releasing a ready drag commits exactly one connected prefab instance at the final preview placement without a long UI-thread stall.
- **SC-005**: Opening a scene and directly dragging the same prefab use the same load request, cache key, resource readiness gate, owner acquisition, and no-white-model visibility rule.
- **SC-006**: Repeated drag/drop of a prefab already present in the scene reports warm cache/resource fast-bind behavior and avoids cold artifact-to-memory work on mouse release.
- **SC-007**: Removing a prefab instance releases instance-owned resources, cancels or detaches instance-owned work, and allows CPU/GPU cost to fall when no other owner remains.
- **SC-008**: Cancelling a preview that shares resources with an existing scene prefab does not unload or hide the existing scene prefab.
- **SC-009**: `UnloadUnusedAssets`-style trim unloads only zero-owner resources and never unloads resources still owned by scene, preview, inspector, or async jobs.
- **SC-010**: Missing-source prefab scenes preserve recovery metadata and present missing prefab state instead of losing the object.
- **SC-011**: Scene-load prefab streaming keeps the editor interactive after object graph activation and reveals each prefab only when mesh/material/texture dependencies are ready together.
- **SC-012**: Focused tests plus manual editor and RenderDoc/equivalent evidence cover preview, commit, save/reload, override, resource lifetime, trim, and scene-load streaming behavior.

## Assumptions

- Unity parity means behavior-level alignment with Unity 2018.4 workflows and persistence semantics, not a source-level clone of Unity internals.
- The existing `specs/unity-ready-model-drop` work remains the baseline and this feature is the next phase that closes remaining semantic and lifecycle gaps.
- The first implementation target is Windows editor with the currently validated renderer backend; cross-backend claims require separate evidence.
- Legacy scenes must remain loadable, but new editor saves may migrate them to the new authoritative prefab-instance records.
- Full nested prefab and prefab variant UX may be staged, but the data model must not block those features later.
