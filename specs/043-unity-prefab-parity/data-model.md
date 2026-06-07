# Data Model: Unity Prefab Parity Phase 2

## PrefabSourceIdentity

Represents the stable source identity of a normal prefab or imported/model prefab.

Fields:

- `projectRootId`: normalized project identity.
- `sourceAssetId`: asset guid or equivalent stable id.
- `sourceAssetPath`: normalized project-relative path.
- `prefabSubAssetKey`: selected prefab artifact/sub-asset key.
- `assetType`: normal prefab, imported model prefab, generated model prefab, or legacy prefab.
- `importerId`: importer identity.
- `importerVersion`: importer version stamp.

Validation:

- Must include project identity to prevent cross-project cache reuse.
- Must include sub-asset key when multiple prefab artifacts can come from one source.

## PrefabInstanceRecord

Authoritative scene document representation of a connected prefab instance.

Fields:

- `recordId`: scene document prefab-instance id.
- `source`: `PrefabSourceIdentity`.
- `instanceRootId`: scene object id for the instance root.
- `correspondence`: list of `PrefabObjectCorrespondence`.
- `modifications`: list of `PrefabModification`.
- `readOnlySource`: true for generated/model prefabs that reject apply-to-asset.
- `missingSourceRecovery`: optional source and hierarchy recovery metadata.
- `legacyScenePrefabFallback`: optional legacy metadata consumed only when no record exists.

State transitions:

- `Connected` -> `MissingSource` when source cannot resolve.
- `Connected` -> `Disconnected` only through explicit unpack.
- `MissingSource` -> `Connected` when source is repaired and correspondence can be restored.

## PrefabObjectCorrespondence

Maps source prefab objects to scene instance objects.

Fields:

- `sourceObjectId`: stable source local id or artifact object key.
- `instanceObjectId`: scene object id.
- `objectType`: GameObject, Transform, MeshRenderer, MeshFilter, Material reference, or component.
- `valid`: false when source no longer exists but recovery data is preserved.

Validation:

- Each connected instance object should map to at most one source object.
- Missing source objects must not delete scene objects silently.

## PrefabModification

Scene-local override stored on a prefab instance.

Fields:

- `targetSourceObjectId`: source object id being modified.
- `targetInstanceObjectId`: instance object id when available.
- `propertyPath`: serialized property path or structural operation path.
- `value`: serialized override value or object reference.
- `operation`: set property, add object, remove object, add component, remove component, reorder child, or material override.
- `applyPolicy`: normal apply allowed, revert only, or generated-model source read-only.

Validation:

- Normal prefab apply/revert paths can consume supported modifications.
- Generated/model prefab apply-to-asset must reject while keeping scene-local modifications.

## UnifiedPrefabLoadRequest

Shared loading request for scene load, preview, drop, duplication, and repeated instantiation.

Fields:

- `source`: `PrefabSourceIdentity`.
- `loadMode`: scene restore, drag preview, final drop, duplicate, inspector preview, or prewarm.
- `ownerKind`: scene instance, preview scene, inspector, async job.
- `ownerScopeId`: scene id, preview id, inspector id, or job id.
- `placement`: optional transform for preview/drop.
- `requiredReadiness`: prefab graph only or mesh/material/texture ready together.
- `allowPending`: whether the caller can receive pending instead of committed/visible result.

Validation:

- Load mode participates in cache key when it changes visibility or ownership semantics.
- Final committed visibility requires mesh/material/texture readiness together.

## PrefabRuntimeCacheEntry

Current cached prefab graph and renderer-resource state.

Fields:

- `key`: source identity plus artifact stamps.
- `prefabGraph`: loaded object graph or graph template.
- `rendererDependencies`: mesh/material/texture dependencies.
- `resourceHandles`: optional warm runtime resource handles.
- `byteEstimate`: memory budget accounting.
- `lastUsedTick`: deterministic LRU ordering.
- `generation`: invalidates stale handles and cached pointers.

Validation:

- Missing material/texture or pending import is not cached as a successful ready entry.
- Reimport or artifact stamp change invalidates entry generation.

## PreviewScenePrefabInstance

Temporary preview-only instance used while dragging over Scene View.

Fields:

- `previewSceneId`: private preview scene or render-only scene id.
- `source`: `PrefabSourceIdentity`.
- `loadRequestId`: associated load request.
- `ownerToken`: resource lifetime owner token.
- `placement`: latest Scene View placement.
- `visible`: true only after mesh/material/texture readiness gate passes.

State transitions:

- `PendingLoad` -> `VisiblePreview` when ready.
- `VisiblePreview` -> `Committed` on successful drop handoff.
- `PendingLoad` or `VisiblePreview` -> `Cancelled` on drag cancel/exit/source invalidation.

## ResourceOwnerToken

Explicit owner lease for a resource usage scope.

Fields:

- `ownerId`: unique token id.
- `ownerKind`: scene instance, preview scene, inspector, async job.
- `scopeId`: scene id, preview id, object id, or job id.
- `resourceIds`: acquired normalized resources.

Validation:

- Owner release must release only resources acquired by that owner.
- Shared owners prevent trim/unload.

## ResourceHandle<T>

Move-only typed runtime resource lease.

Fields:

- `resourceId`: normalized resource id.
- `generation`: expected resource generation.
- `ownerToken`: owning lease.
- `cachedPointer`: optional pointer, valid only if generation still matches.

Validation:

- Copy construction is forbidden.
- Move transfers ownership.
- `Get()` returns null after generation mismatch, unload, or reset.

## ScenePrefabStreamingJob

Bounded background/per-frame job for scene-loaded prefab renderer resources.

Fields:

- `jobId`: unique job id.
- `sceneId`: owning scene.
- `instanceRootId`: target prefab root.
- `loadRequest`: unified request.
- `ownerToken`: async job owner token.
- `cancelled`: cancellation flag.
- `progress`: dependency resolution progress.

Validation:

- Scene unload, object deletion, or reimport cancels/detaches job.
- Reveal occurs only after mesh/material/texture readiness together.

## PrefabEditorState

Hierarchy and inspector presentation state.

Fields:

- `connectionState`: connected, missing source, disconnected, unpacked, or pending.
- `overrideSummary`: supported scene-local modifications.
- `applyAvailability`: allowed, read-only rejected, unsupported, or unavailable.
- `resourceState`: ready, pending, failed, or cancelled.
- `diagnostics`: user-facing messages.
