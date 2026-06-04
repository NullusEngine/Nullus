# Implementation Plan: Unity-Ready Model Drop

**Branch**: `unity-ready-model-drop` | **Date**: 2026-06-01 | **Spec**: `specs/unity-ready-model-drop/spec.md`
**Input**: Feature specification from `specs/unity-ready-model-drop/spec.md`

## Summary

Align imported-model drag/drop with the approved Unity-like interaction model and align prefab scene persistence with Unity 2018.4 `PrefabInstance` semantics. Import time is responsible for generating and stabilizing mesh, material, texture, and generated-model-prefab relationships. Asset Browser drag payloads expose whether a current prefab artifact is preview-ready, and Scene View hover consumes that hint through an asset-layer fast preview prefab loader instead of refreshing the full asset database. Ready assets create a temporary preview-only mesh ghost that follows the mouse before release, repeated hover updates reuse the same ghost for the same asset identity, final drop commits a formal scene instance only when the generated model prefab is renderer-ready, and not-ready assets stay pending instead of committing a hidden instance that waits through long asynchronous post-drop resolution. Committed prefab instances must persist through editor scene save and reload as document-level Unity-style `PrefabInstance` records containing source prefab identity, modifications, and source/instance correspondence rather than as root-only metadata or plain unpacked object copies. The remaining artifact-to-memory path is handled as a full Unity-style loading pipeline: measure cold/warm stage costs first, add bounded prefab/model hot caches, prewarm mesh/material/texture together, gate preview visibility atomically to prevent white models, reduce native artifact read copies, and release cache-held resources when no scene object needs them. Scene opening follows the same split lifecycle: activate the scene object graph first, restore PrefabInstance identity from the already parsed scene document, and stream generated/model prefab renderer resources through bounded background/per-frame jobs instead of blocking startup or scene switch completion.

## Technical Context

**Language/Version**: C++17/C++20 project style already used by Nullus
**Primary Dependencies**: Nullus editor Scene View panel, editor asset drag/drop bridge, prefab workflow, scene object-graph serialization/loading, import progress tracking, native artifact manifest validation
**Storage**: Project `Library/Artifacts/<asset-guid>/manifest.json`, native artifact files, and editor scene documents containing document-level prefab instance records
**Testing**: `NullusUnitTests` with focused gtest filters plus targeted editor interaction contract tests
**Target Platform**: Windows editor, DX12 validation path
**Project Type**: Desktop editor/runtime engine
**Performance Goals**: For already imported large models, Scene View preview must appear immediately enough to support interactive drag placement, ready preview should render the imported mesh/prefab shape with material and textures together, repeated preview/drop requests should hit bounded hot caches after the first artifact-to-memory load, mouse release should avoid long synchronous cold loads, removed prefabs should stop consuming scene/runtime work, and the committed drop path must avoid heavy synchronous work that belongs to import-time asset stabilization or hover/idle prewarm. Initial implementation budgets target textured warm preview first visibility within 200 ms, Scene View mouse-release UI-thread synchronous work within one 60 Hz frame budget, and hot-cache fast-load lookup within 10 ms; telemetry must report budget misses by stage rather than hiding them behind a pass/fail label.
**Constraints**: No hand edits under `Runtime/*/Gen/`; preserve existing watcher/import progress flow; do not regress prefab or non-generated asset drops; keep Scene View preview non-committed, disposable, and outside the active scene object graph
**Scale/Scope**: Large imported scenes such as Sponza, especially the user workflow of dragging an already imported large model into Scene View, modifying the prefab instance in scene, and persisting the resulting committed instance and overrides in scene files

## Constitution Check

- Spec-first major change: PASS. This bundle records the editor asset lifecycle and Scene View interaction change.
- Validation matches subsystem: PASS. Unit tests and focused editor interaction coverage match the editor/runtime asset workflow; renderer-specific evidence is required for material/texture binding validation, while DX12/RenderDoc evidence is not generalized to other backends.
- Generated code/backend boundaries: PASS. No generated files are edited; DX12 evidence does not claim other backends.
- Incremental verified delivery: PASS. Work is split into import-result reuse, preview lifecycle, committed readiness gate behavior, scene persistence, and fallback hardening.
- Product runtime preservation: PASS. Existing imported prefab flow remains, while generated-model Scene View drop semantics and persistence become more explicit and Unity-aligned.

## Project Structure

### Documentation

```text
specs/unity-ready-model-drop/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Panels/
├── AssetBrowser.cpp                      # Drag payload preview-ready hint from stabilized import artifacts
├── SceneView.cpp                         # Scene View drag target, preview-only mesh ghost lifecycle, final drop commit
└── Hierarchy.cpp                         # Shared committed-drop behavior without Scene View preview

Project/Editor/Rendering/
└── DebugSceneRenderer.cpp                # Editor renderer descriptor awareness for preview scene rendering

Runtime/Engine/Rendering/
├── BaseSceneRenderer.cpp                 # Additive preview scene parsing for render-only ghost drawables
└── BaseSceneRenderer.h                   # SceneDescriptor additive scene input

Project/Editor/Core/
├── EditorActions.cpp                     # Formal scene object creation, pending import completion, fallback resource resolution
└── EditorActions.h                       # Preview/commit helper declarations if needed

Project/Editor/Assets/
├── EditorAssetDragDropBridge.cpp         # Fast imported prefab loading, preview artifact loading, import-result reuse, artifact telemetry, hot cache, and drop-time readiness gate
├── EditorAssetDragDropBridge.h           # Public fast preview prefab artifact API
├── EditorAssetDragPayload.cpp            # Preview-ready payload hint helpers
├── EditorAssetDragPayload.h              # Trivially-copyable drag payload shape
└── PrefabUtilityFacade.cpp               # Unity-style PrefabInstance scene record emission, restoration, and legacy scenePrefab fallback

Runtime/Engine/
├── SceneSystem/SceneManager.cpp          # Scene save/load flow and cached parsed scene document for editor restore
├── SceneSystem/SceneManager.h            # Accessor for the last loaded scene ObjectGraphDocument
├── Serialize/ObjectGraphDocument.h       # Document-level PrefabInstance record model
├── Serialize/ObjectGraphReader.h         # PrefabInstance scene record parsing
├── Serialize/ObjectGraphWriter.h         # PrefabInstance scene record emission
└── Serialize/ObjectGraphSerializer.h     # Scene object graph plus prefab-instance-compatible object IDs

Tests/Unit/
├── EditorAssetDragDropTests.cpp          # Ready vs pending generated-model drop behavior
├── SceneObjectGraphSerializationTests.cpp# Scene save/load persistence for generated-model instances
├── GameObjectAssetImportTests.cpp        # Import artifact cache invalidation and repeated preview behavior
└── EditorRenderPathContractTests.cpp     # Legacy async fallback, hidden-root regression, artifact load telemetry, and cache contracts
```

**Structure Decision**: Keep import-time artifact stabilization in the existing asset pipeline, keep Scene View preview orchestration in the panel layer, render ready previews from a private preview-only scene appended through the renderer scene descriptor, keep committed-instance creation and delayed completion in `EditorActions`, keep drop-time readiness validation and import-result reuse in the existing drag/drop bridge, and extend scene serialization/loading so generated-model committed instances round-trip as prefab-backed scene data.

## Research

- Unity 2018.4 `SceneView` drag handling first drives per-editor drag callbacks, then calls the editor's native Scene View drag handler on `DragUpdated` and `DragPerform`.
- Unity treats imported 3D models as `ModelPrefab` assets and instantiates them through prefab instantiation rather than rebuilding the asset at drop time.
- Unity also uses a temporary drag object or preview behavior during drag, separate from the final committed placement.
- The Nullus investigation found that current generated-model drop behavior can commit a formal instance and then hide its root while conservative asynchronous renderer-resolution work runs for many frames.
- The user clarified the desired lifecycle boundary: import is responsible for stabilizing mesh/material/texture to generated-model-prefab relationships; drag/drop should consume that stabilized result instead of rebuilding those relationships on the Scene View path.
- Unity 2018.4 scene/prefab YAML persists scene prefab instances as `PrefabInstance` objects with `m_SourcePrefab`, `m_Modification.m_Modifications`, removed components, and stripped object correspondence records such as `m_CorrespondingSourceObject` and `m_PrefabInstance`.
- Unity model prefabs are read-only source assets: scene instances may carry overrides, but applying those overrides back to the model prefab asset is rejected.
- The same investigation also found that current scene save/load serialized ordinary object trees plus a root `scenePrefab` property, which restores some registry state but is not fully Unity-aligned because it lacks an authoritative prefab instance record and source/instance correspondence.
- Therefore the Unity-aligned correction is not only "correct final materials" but also a lifecycle split: temporary drag preview during drag, ready-or-pending commit on release, and document-level `PrefabInstance` persistence for committed instances.
- Unity-like loading does not mean keeping all imported data resident forever. The editor should keep stable artifact relationships on disk, maintain bounded hot caches for recently used prefab graphs and runtime resources, and evict cold entries by budget while active scene references remain strong.
- Unity 2018.4 reference anchors used for this design: `Editor/Src/AssetPipeline/AssetImporter.cpp` stabilizes imported sub-assets and local file IDs during import; `Editor/Src/DragAndDropForwarding.cpp` registers Scene/Hierarchy prefab drag paths and instantiates prefabs on perform; `Editor/Mono/Inspector/PreviewRenderUtility.cs` uses a `PreviewScene` and `PrefabUtility.InstantiatePrefab` for isolated preview objects; `Editor/Src/Prefabs/Prefab.cpp` records prefab instance correspondence and property modifications. Nullus should mirror these responsibilities, not copy implementation details blindly.

## Data Model

- `DragPreviewObject`: temporary Scene View-only object lifecycle used while mouse drag is active.
- `PreviewOnlyMeshGhost`: ready asset preview instantiated into a private preview scene and rendered as additive drawables without entering the active scene, Hierarchy, prefab registry, or scene persistence.
- `PreviewReadyPayloadHint`: trivially-copyable drag payload flag set by the asset browser when current prefab artifacts are visible in stabilized import output.
- `CommittedDropResult`: success, pending, or rejected result of final drop handling.
- `RendererDependencyReadiness`: aggregate readiness result used by the primary committed-drop gate.
- `StabilizedImportRelationship`: import-produced mapping between generated model prefab content and its mesh, material, and texture dependencies that the drag path can consume directly.
- `PersistedPrefabInstance`: document-level scene record that stores a source prefab reference, scene instance root object id, saved modifications, source/instance object correspondence, generated/model read-only flag, and optional recovery data.
- `PrefabInstanceModification`: scene-local patch operation equivalent to Unity `PropertyModification` and structural override records.
- `PrefabInstanceCorrespondence`: mapping from source prefab object ids to scene object ids, analogous to Unity stripped object correspondence.
- `LegacyAsyncResolutionFallback`: existing deferred resolution path retained only for compatibility or non-primary cases.
- `ImportedPrefabHotCache`: editor-session cache for successful fast prefab artifact loads, keyed by source asset id, normalized path, prefab sub-asset key, asset type, load mode, importer id/version, project root identity, manifest stamp, and prefab artifact stamp, with LRU-style eviction.
- `ArtifactMemoryBudget`: configurable limit used by preview/drop hot caches so repeated use becomes fast without turning import completion into unbounded memory residency.
- `ArtifactLoadTelemetry`: lightweight editor-session timing and byte accounting for prefab graph load, manifest validation, native artifact I/O, parse/hash validation, CPU deserialize, runtime resource creation, GPU upload, cache hit/miss, prewarm cancellation, and eviction.
- `RendererResourcePrewarmRequest`: cancellable prewarm request for mesh/material/texture runtime resources scheduled by import completion, selection, hover, editor idle, scene load, or explicit preview.
- `NativeArtifactContainerView`: validated low-copy view over native artifact container payloads, used to reduce full-file and payload copies while preserving hash/error semantics.
- `ModelRuntimeCacheEntry`: bounded cache entry backed by the current mesh/resource-manager artifact cache and equivalent-path aliasing SSoT, so repeated submesh requests reuse loaded mesh/material/texture artifacts instead of each cold-reading independent files or recreating duplicate resource keys.

## Phases

### Phase 1 - Import Result Reuse And Speed Guardrails

- Make the already-imported drag path explicitly consume stabilized import relationships instead of rebuilding them on the drag path.
- Mark current prefab artifacts as preview-ready in drag payloads and require Scene View hover to use the asset-layer fast preview loader instead of `AssetDatabaseFacade::Refresh()`.
- Add focused instrumentation or contract coverage for the already-imported large-model drag path so preview latency and main-thread work are observable.
- Keep heavy synchronous relationship-building out of the Scene View drag hot path.

### Phase 2 - Scene View Preview Lifecycle

- Add a dedicated Scene View drag preview path for imported model assets.
- Render ready imported prefab/model assets as a preview-only mesh ghost from a private preview scene; keep the existing debug marker and label as fallback when the prefab artifact cannot be preview-instantiated.
- Reuse the existing preview-only mesh ghost on repeated hover updates for the same asset identity.
- Ensure preview objects are temporary, non-Hierarchy, and cleaned up on drop or cancellation.
- Reuse existing placement logic where practical so final preview placement is stable and meaningful.

### Phase 3 - Ready-Or-Pending Commit Path

- Route Scene View final drop through the existing generated-model readiness gate.
- Commit only ready formal instances.
- Keep not-ready assets in pending or importing state without creating a hidden formal instance as the main path.

### Phase 4 - Unity-Style PrefabInstance Persistence And Reload

- Extend scene document data model, writer, and reader with document-level `prefabInstances` records.
- Emit one prefab instance record per connected prefab instance during editor scene save, including source prefab reference, instance root object id, source/instance correspondence, and discovered local modifications.
- Prefer the new prefab instance records as the authoritative load path and keep root `scenePrefab` metadata readable only as legacy compatibility fallback.
- Reconstruct scene instances from source prefab artifacts plus saved modifications during editor load when artifacts are available; preserve missing-prefab recovery records when they are not.
- Rebuild editor prefab-instance tracking state when scenes containing prefab instance records are loaded.
- Keep transform, parentage, prefab sub-asset identity, scene-local modifications, and source/instance correspondence stable across round-trip save/load.
- Preserve Unity model-prefab semantics: generated model prefab instances can save scene-local overrides but cannot apply changes back to the generated model prefab asset.

### Phase 5 - Legacy Fallback Guardrails

- Preserve explicit fallback handling for older deferred-resolution paths.
- Prevent fallback behavior from becoming the primary large-model Scene View drag experience.
- Keep failure and cancellation cleanup strong so live hidden roots are not stranded.

### Phase 6a - Artifact Load Profiling

- Add telemetry first, before optimizing, so the next changes target measured cold/warm costs instead of guessed bottlenecks.
- Record timings and byte counts for prefab graph load, manifest validation, dependency scan, native file read, container parse/hash, CPU deserialize, runtime resource creation, GPU upload, cache hit/miss, prewarm cancellation, and eviction.
- Make telemetry test-visible and manually exportable from the editor so the user-reported "release stalls" can be correlated with specific stages.
- Before adding new model-level cache ownership, audit the historical `.nmodel`/`ModelManager` plan from `specs/026-asset-management-system` against the current branch. The current branch's concrete SSoT is `MeshManager::PrewarmArtifact`, `MeshManager::ResolveArtifactResourcePath`, material/texture manager equivalent-path cache probing, and existing source contracts that reject direct `model.nmodel` loads; route later prewarm/cache work through those live APIs unless a separate migration reintroduces a concrete model package manager.

### Phase 6b - Prefab Artifact Hot Cache

- Add focused contract and behavior coverage proving repeated current prefab fast loads use a bounded hot cache.
- Cache successful `LoadImportedPrefabFast` results after manifest/dependency/readiness validation and prefab artifact import.
- Include source asset id, normalized asset path, prefab sub-asset key, asset type, load mode, importer id/version, project root identity, manifest stamp, and prefab artifact stamp in the cache key so reimport, path normalization, sub-asset, type, or artifact edits invalidate stale entries.
- Keep the cache bounded by entry count and estimated prefab graph/resolved-asset memory rather than preloading every imported model.
- Preserve the existing ready-or-pending gate: cached prefab graphs still require renderer artifact readiness for preview/drop paths.

### Phase 6c - Mesh/Material/Texture Prewarm And Visibility Gate

- Schedule cancellable renderer-resource prewarm from import completion, selection, hover, editor idle, scene load, or explicit preview.
- Treat mesh/material/texture readiness as one atomic visibility gate for generated model previews and ready committed drops.
- Do not show the ready preview as a white model; if material or texture dependencies are not ready, keep the model pending/not-ready or use an explicit non-ready fallback that cannot be confused with a correct preview.
- Ensure mouse release never starts a long synchronous cold load that should already be running or resumable from hover/idle prewarm.
- Tie each preview/commit prewarm and async renderer-resolution request to an owner token so cancellation, drag exit, scene object destruction, or scene switch detaches renderer queues, per-frame update sources, and draw registrations that belong only to the removed instance.

### Phase 6d - Low-Copy Native Artifact Reader

- Add a validated `NativeArtifactContainerView` or equivalent single-allocation read path for native artifact containers.
- Convert hot prefab/mesh/material/texture readers to avoid redundant full-file and payload copies while keeping existing hash validation and diagnostics.
- Keep the legacy owning container path available where callers need long-lived payload ownership or where platform-specific memory mapping is not suitable.

### Phase 6e - Model-Level Runtime Cache And Resource Release

- Reuse or complete the current mesh/resource-manager artifact cache path so generated model preview/drop reuses loaded mesh artifacts, material bindings, texture dependencies, and equivalent-path aliases without introducing a parallel `model.nmodel` owner.
- Avoid repeated per-submesh cold artifact reads on repeated preview/drop/scene-load paths.
- Bind cache entries to active scene/runtime resource ownership and owner cancellation tokens so removing a prefab releases scene-held references, cancels instance-owned work, detaches renderer registrations, and lets caches evict CPU/GPU resources.

### Phase 6f - Unity-Like Resource Lifetime Registry

- Mirror Unity's practical lifetime model: resource objects remain cached while reachable through scene, preview, inspector, or async-job owners; removal releases owner references, and a later `UnloadUnusedAssets`-style trim evicts only zero-owner resources.
- Introduce a `ResourceLifetimeRegistry` in `Runtime/Core/ResourceManagement` that normalizes artifact path aliases, records owner-token acquisitions for mesh/material/texture resources, tracks last-use order, and reports deterministic trim candidates by resource type/path.
- Extend resource managers with safe trim helpers that call existing `UnloadResource(path)` only after the registry proves the normalized resource has no active owners.
- Connect Scene View drag preview, committed prefab instances, scene unload, and preview handoff cancellation to owner acquire/release so cancelling a preview cannot unload resources still used by saved scene prefab instances.
- Emit lightweight lifetime telemetry for acquire, release, trim skip, and eviction so manual editor validation can distinguish "owner released" from "resource actually unloaded".

### Phase 6g - ResourceHandle Migration Layer

- Add a move-only `ResourceHandle<T>` compatibility layer on top of the lifetime registry before changing long-lived engine/editor call sites.
- Handles store typed `ResourceId`, generation, normalized path, and owner token. Destruction/reset releases the owner reference; `Get()` validates generation and returns null for stale handles after reimport, trim, or reload.
- Keep existing `GetResource`/raw pointer APIs during migration, but require new generated-model preview/drop code to acquire handles or owner tokens rather than silently caching raw pointers across frames.
- Use generation increments on unload/reload/reimport to prevent stale handles from exposing a newly reloaded resource at the same path.
- Migrate Scene View preview and generated-model committed-instance paths first; broader `MeshFilter`/`MeshRenderer` migration remains a follow-up once the compatibility handle proves stable.

### Phase 6h - Scene Prefab Streaming Load

- Split initial scene availability from generated/model prefab renderer-resource readiness. `SceneManager::LoadScene` reads, parses, instantiates, and activates the scene object graph; editor prefab restore consumes that already parsed document instead of rereading/reparsing the scene file.
- Queue restored generated/model prefab renderer-resource work after scene activation through the existing delayed/background `QueuePrefabInstanceAssetResolution` path so the scene and editor UI stay usable while large prefab resources stream in bounded chunks.
- Keep scene-load resource progress diagnostic-only/non-blocking: renderer-resource jobs tagged as scene-load work must not open the native blocking task progress dialog after the scene object graph is active.
- Preserve strict visibility: scene-load generated/model prefab renderers remain transiently suppressed until mesh/material/texture are bound together, then reveal atomically. Failures keep recovery metadata and diagnostics instead of showing a white model or deleting the instance.
- Cancel/detach scene-load streaming work on scene switch, scene unload, prefab removal, or editor shutdown through existing owner-token, destroyed-listener, and resource lifetime cleanup.

### Phase 7 - Performance And Runtime Verification

- Verify import-result reuse, preview lifecycle, ready commit behavior, pending/not-ready behavior, and save/load persistence with focused tests.
- Preserve the previously added regression coverage around fallback async resolution cleanup.
- Verify telemetry, hot-cache hit/miss/invalidation, prewarm cancellation, atomic mesh/material/texture visibility, low-copy reader validation, model-level cache reuse through the current mesh/material/texture resource managers, resource release after scene removal, renderer-specific material/texture evidence, and cache budget enforcement with focused tests and manual editor evidence.

## Validation

Run:

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorAssetDragDropTests.*GeneratedModel*:EditorAssetDragDropTests.*SceneView*:SceneObjectGraphSerializationTests.*GeneratedModel*:EditorRenderPathContractTests.*GeneratedModel*:EditorRenderPathContractTests.*MaterialArtifact*:EditorRenderPathContractTests.*Artifact*:GameObjectAssetImportTests.*HotCache*:GameObjectAssetImportTests.*Prewarm*
git diff --check
```

Manual editor validation:

```text
1. Drag an already imported large model over Scene View and confirm a mesh/material/texture preview appears quickly enough to support interactive placement.
2. Confirm the preview follows the cursor and is not treated as a normal Hierarchy scene object during drag.
3. Release over a ready asset and confirm one committed visible textured instance appears at the preview placement without a long hidden wait.
4. Release over a not-ready asset and confirm no committed instance is created and the result reports pending or not ready.
5. Repeat the drag/drop and compare telemetry for cold vs warm paths, including prefab/model cache hits and avoided per-submesh cold reads.
6. Remove the prefab from the scene and confirm scene/runtime resource references are released, owner-tokened prewarm or async jobs are cancelled/detached, renderer queue and draw registrations are removed, and CPU/GPU work drops rather than continuing for the removed object.
7. Save the scene, reload it, and confirm the committed generated model instance is restored instead of disappearing.
8. Run the unused-resource trim path after preview cancel, scene object removal, and scene unload; confirm only zero-owner mesh/material/texture resources are unloaded and shared resources stay alive.
9. Open a scene that already contains a large generated/model prefab and confirm the scene activates without waiting for every prefab renderer resource to finish; prefabs appear as their mesh/material/texture dependencies become ready together, with no white-model intermediate state and no native blocking renderer-resource dialog after activation.
```

Required renderer evidence after unit validation:

```powershell
python tools/renderdoc/rdc_doctor.py App/Win64_Release_Runtime_Shared/Build/RenderDocCaptures/Editor/<new-capture>.rdc
```

Expected runtime outcome: representative large-model drop no longer relies on a long post-drop hidden formal instance before first visibility, ready preview/committed draws bind intended mesh/material/texture resources together, warm repeated drops avoid cold artifact-to-memory work, removed prefabs stop driving scene CPU/GPU work, and committed generated-model instances survive scene round-trip save/load.
