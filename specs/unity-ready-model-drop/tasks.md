# Tasks: Unity-Ready Model Drop

**Input**: `specs/unity-ready-model-drop/spec.md`, `specs/unity-ready-model-drop/plan.md`
**Prerequisites**: Existing generated-model readiness gate work and current Scene View drag/drop entry points

## Phase 1: Import Result Reuse And Speed Guardrails

- [x] T001 Add focused RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving the already-imported Scene View drag path consumes stabilized generated-model import results rather than rebuilding mesh, material, texture, and prefab relationships during drag.
- [x] T002 Add targeted instrumentation or contract coverage for already-imported large-model drag latency so preview-path main-thread work is observable during validation.
- [x] T003 Update the already-imported drag path in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` and related entry points so it reuses stabilized import relationships and avoids heavy synchronous work that belongs to import-time asset preparation.
- [x] T003a Add RED coverage proving warm imported asset handles can expose a preview prefab artifact through the asset-layer fast path without Scene View invoking full asset database refresh.
- [x] T003b Add a preview-ready drag payload hint and route Scene View mesh ghost creation through `EditorAssetDragDropBridge::TryLoadPreviewPrefabArtifact`, reusing the existing ghost for repeated hover updates of the same asset.

## Phase 2: Scene View Drag Preview

- [x] T004 Add focused RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` or the closest existing editor interaction test file proving Scene View imported-model drag creates a temporary preview lifecycle before final drop.
- [x] T005 Implement a temporary Scene View drag preview path in `Project/Editor/Panels/SceneView.cpp` for imported model and prefab assets.
- [x] T006 Ensure the preview object is non-committed, is not treated as a normal Hierarchy item, and is cleaned up on drop or cancellation.
- [x] T007 Preserve stable placement behavior so the preview follows the resolved Scene View drop target instead of jumping between unrelated fallback positions.
- [x] T007a Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving ready Scene View preview uses a preview-only mesh ghost scene instead of only a debug box marker.
- [x] T007b Implement preview-only prefab/model mesh ghost lifecycle in `Project/Editor/Panels/SceneView.cpp` without committing objects to the active scene, Hierarchy, prefab registry, or scene persistence.
- [x] T007c Extend `Project/Editor/Rendering/DebugSceneRenderer.cpp` so Scene View rendering can include the preview-only scene while preserving the current fallback marker for not-ready preview assets.

## Phase 3: Ready-Or-Pending Commit

- [x] T008 Add RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a ready generated model drop commits one visible formal instance using the final preview or drop placement.
- [x] T009 Add RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a not-ready generated model drop commits zero formal scene instances and reports pending or not-ready state.
- [x] T010 Route Scene View final drop handling through the generated-model drop-time readiness gate in `Project/Editor/Core/EditorActions.cpp` and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [x] T011 Remove the main-path dependency on "commit formal instance first, then hide root until long async resolution completes" for ready large-model Scene View drops.

## Phase 4: Hierarchy And Legacy Fallback Consistency

- [x] T012 Ensure Hierarchy imported-model drop continues to use the same committed-instance readiness gate even without Scene View mouse-follow preview.
- [x] T013 Add or update contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving legacy async generated-model resolution remains a compatibility fallback and still cleans up hidden-root cancellation or failure correctly.
- [x] T014 Keep `Project/Editor/Core/EditorActions.cpp` fallback cleanup behavior strong enough that cancelled or failed deferred resolution does not strand live hidden roots.

## Phase 5: Legacy Scene Persistence And Reload

- [x] T015 Add RED save/load coverage in `Tests/Unit/SceneObjectGraphSerializationTests.cpp` proving a committed generated model instance survives scene save and reload.
- [x] T016 Extend scene serialization/loading so committed generated-model instances round-trip as prefab-backed scene data instead of disappearing.
- [x] T017 Rebuild prefab-instance tracking state for restored generated-model scene instances during scene load so editor prefab presentation and later prefab-aware operations still work.

## Phase 5a: Unity-Style PrefabInstance Parity

- [x] T017a Add RED coverage in `Tests/Unit/PrefabUtilityFacadeTests.cpp` proving editor scene save emits document-level Unity-style `prefabInstances` records with `sourcePrefab`, `instanceRoot`, `modifications`, and source/instance correspondence instead of relying only on root `scenePrefab` metadata.
- [x] T017b Extend `Runtime/Engine/Serialize/ObjectGraphDocument.h`, `ObjectGraphWriter.h`, and `ObjectGraphReader.h` with a first-class `PrefabInstance` scene record model.
- [x] T017c Update `Project/Editor/Assets/PrefabUtilityFacade.cpp` so `AnnotateSceneDocumentWithPrefabInstances` writes authoritative `prefabInstances` records from `PrefabInstanceRegistry`, including discovered overrides.
- [x] T017d Add RED load coverage proving `RestorePrefabInstancesFromSceneDocument` prefers `prefabInstances` records, reconstructs prefab registry state, and keeps legacy `scenePrefab` root metadata only as fallback.
- [x] T017e Update editor scene load restoration to instantiate from source prefab artifacts plus saved modifications and source/instance correspondence when `prefabInstances` are available.
- [x] T017f Add RED coverage proving a normal prefab scene-local property override saves and reloads without applying back to the prefab asset.
- [x] T017g Add RED coverage proving a generated/model prefab scene-local override saves and reloads, while apply-to-asset remains rejected as read-only model-prefab behavior.
- [x] T017h Preserve missing/corrupt source prefab recovery for `prefabInstances` records without silently unpacking or dropping the scene object.

## Phase 6a: Artifact Load Profiling

- [x] T018 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving imported prefab preview/drop emits `ArtifactLoadTelemetry` for prefab graph load, manifest validation, native artifact I/O, container parse/hash, CPU deserialize, runtime resource creation, GPU upload, cache hit/miss, cancellation, and eviction stages.
- [x] T019 Add RED budget coverage proving warm textured preview first visibility, Scene View mouse-release UI-thread synchronous work, and hot-cache fast-load lookup report explicit pass/miss telemetry against the configured 200 ms, one-frame-at-60-Hz, and 10 ms targets.
- [x] T020 Audit the existing `.nmodel` package and `ModelManager` cache path from `specs/026-asset-management-system/tasks.md` and current runtime managers, then document the SSoT decision in `specs/unity-ready-model-drop/plan.md` before implementing new cache ownership.
- [x] T021 Implement lightweight artifact-to-memory telemetry plumbing in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, `Runtime/Core/Assets/NativeArtifactContainer.cpp`, `Runtime/Rendering/Assets/MeshArtifact.cpp`, `Runtime/Rendering/Assets/MaterialArtifact.cpp`, and texture artifact loading entry points.
- [x] T022 Expose cold/warm telemetry snapshots and budget misses to focused tests and editor diagnostics without forcing release builds to pay heavy logging overhead.

## Phase 6b: Prefab Artifact Hot Cache

- [x] T023 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving imported prefab fast loads use an `ImportedPrefabHotCache` keyed by source asset id, normalized asset path, prefab sub-asset key, asset type, load mode, importer id/version, project root, manifest stamp, and prefab artifact stamp.
- [x] T024 Add RED behavior coverage in `Tests/Unit/GameObjectAssetImportTests.cpp` proving repeated preview loads return through the hot cache while manifest, prefab artifact, normalized path, sub-asset key, asset type, project root, or importer edits invalidate the cached result.
- [x] T025 Implement a bounded editor-session imported prefab artifact hot cache in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [x] T026 Ensure the hot cache stores only successful prefab graph/resolved-asset results and preserves renderer dependency readiness failures, missing material/texture artifacts, and pending import states as uncached pending results.
- [x] T027 Add entry-count, byte-budget, and deterministic LRU eviction guardrails so the cache cannot retain every imported model indefinitely.

## Phase 6c: Mesh/Material/Texture Prewarm And Visibility Gate

- [x] T028 Add RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving ready Scene View preview/drop requires mesh, material, and texture readiness together and never treats a white material-only model as the ready result.
- [x] T029 Add RED coverage in `Tests/Unit/GameObjectAssetImportTests.cpp` proving renderer-resource prewarm can be scheduled from hover or editor idle, cancelled when drag/selection changes, and resumed without blocking mouse release.
- [x] T030 Add RED coverage proving drag cancellation, scene switch, or prefab removal cancels/detaches owner-tokened prewarm, async renderer-resolution jobs, renderer queues, per-frame update sources, and draw-source registrations associated only with that instance.
- [x] T031 Implement cancellable `RendererResourcePrewarmRequest` orchestration across `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, `Project/Editor/Panels/SceneView.cpp`, and the existing mesh/material/texture manager prewarm entry points.
- [x] T032 Update Scene View preview and final drop readiness in `Project/Editor/Panels/SceneView.cpp` and `Project/Editor/Core/EditorActions.cpp` so mesh/material/texture are committed atomically; non-ready assets stay pending or use an explicit not-ready fallback instead of a misleading white model.
- [x] T033 Add resource-release coverage proving removing a prefab instance from the scene releases scene-held mesh/material/texture references and cancels instance-owned in-flight work enough for cache eviction and reduced per-frame work.

## Phase 6d: Low-Copy Native Artifact Reader

- [x] T034 Add RED coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` or `Tests/Unit/AssetImportPipelineTests.cpp` proving native artifact low-copy reads preserve hash validation, corrupt-container diagnostics, and missing-payload errors.
- [x] T035 Introduce `NativeArtifactContainerView` or an equivalent validated single-allocation read API in `Runtime/Core/Assets/NativeArtifactContainer.h` and `Runtime/Core/Assets/NativeArtifactContainer.cpp`.
- [x] T036 Convert hot prefab artifact loading in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` or its artifact reader helper to use the low-copy container path while preserving the existing owning-container fallback.
- [x] T037 Convert hot mesh artifact loading in `Runtime/Rendering/Assets/MeshArtifact.cpp` to use the low-copy container path while preserving the existing owning-container fallback.
- [x] T038 Convert hot material and texture artifact loading paths to use the low-copy container path where their payload lifetime does not require an extra owning copy.
- [x] T039 Add telemetry counters proving redundant full-file or payload copies are reduced on the hot artifact-to-memory path.

## Phase 6e: Model-Level Runtime Cache And Resource Release

- [x] T040 Add RED coverage proving repeated preview/drop of a generated model with many submeshes reuses the current mesh/material/texture resource-manager cache entries or equivalent `ModelRuntimeCacheEntry` instead of cold-loading each submesh artifact independently.
- [x] T041 Implement or complete model-level runtime cache reuse through the current mesh/resource-manager artifact-cache SSoT for generated model prefab preview/drop in the appropriate runtime asset manager and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [x] T042 Ensure model cache invalidation covers manifest, prefab, mesh, material, texture, importer version, explicit reimport, artifact deletion, normalized project-relative path changes, and budget eviction.
- [x] T043 Ensure active scene instances keep required runtime resources alive while removed prefab instances release scene references, cancel owner-tokened work, and detach renderer registrations so the model cache can evict CPU/GPU resources.

## Phase 7: Performance And Runtime Verification

- [x] T044 Build `NullusUnitTests`.
- [x] T045 Run focused generated-model drag/drop, Scene View preview, scene persistence, Unity-style PrefabInstance, telemetry, hot-cache, prewarm, low-copy reader, model-cache, resource-release, owner-cancellation, and fallback cleanup tests.
- [x] T046 Run `git diff --check`.
- [ ] T047 Capture cold and warm telemetry for an already-imported large model drag/drop, verify the 200 ms warm preview, one-frame release work, and 10 ms cache lookup budgets as Phase 7 gates, and record dominant remaining artifact-to-memory stages in `specs/unity-ready-model-drop/plan.md`.
- [ ] T048 Perform a manual editor pass covering textured mouse-follow preview, ready commit, not-ready pending behavior, no white-model ready state, repeated warm drag speed, scene save/reload persistence, scene removal CPU/GPU release, owner-token cancellation, and cache budget behavior.
- [ ] T049 Capture RenderDoc or equivalent renderer evidence for the ready preview and committed model path proving mesh/material/texture bindings are correct.

## Phase 6f: Unity-Like Resource Lifetime Registry

- [x] T050 Add RED coverage in `Tests/Unit/ResourceLifetimeRegistryTests.cpp` proving resource owners acquire/release normalized mesh/material/texture artifact references and shared owners prevent premature unload.
- [x] T051 Add RED coverage proving `UnloadUnusedAssets`-style trim returns only zero-owner LRU candidates and treats absolute artifact paths and `Library/...` aliases as the same resource.
- [x] T052 Implement `Runtime/Core/ResourceManagement/ResourceLifetimeRegistry.h/.cpp` with owner-token acquisition, owner release, normalized path aliasing, last-use tracking, budget-aware trim candidate collection, and focused test hooks.
- [x] T053 Add lightweight lifetime telemetry stages for acquire, release, trim skip, and eviction without adding release-build hot-path overhead.
- [x] T054 Extend `MeshManager`, `MaterialManager`, and `TextureManager` with safe trim helpers that unload only registry-approved zero-owner resource paths.
- [x] T055 Connect Scene View drag preview creation/cancellation and preview handoff adoption to lifetime owner acquire/release.
- [x] T056 Connect committed prefab instance creation, `EditorActions::DestroyGameObject`, and scene unload to lifetime owner acquire/release so active scene instances keep shared resources alive.
- [x] T057 Add regression coverage proving cancelling a preview that shares resources with an existing scene prefab does not unload the scene prefab's mesh/material/texture.
- [x] T058 Add regression coverage proving removing the last scene prefab owner and running trim unloads unreferenced runtime resources and emits eviction telemetry.
- [x] T059 Build `NullusUnitTests`, run focused lifetime/resource-release tests, and run `git diff --check`.

## Phase 6g: ResourceHandle Migration Layer

- [x] T060 Add RED coverage in `Tests/Unit/ResourceLifetimeRegistryTests.cpp` proving `ResourceHandle<T>` is move-only RAII and releases its owner reference on reset/destruction.
- [x] T061 Add RED coverage proving resource generations invalidate stale handles after unload/reload/reimport while a new handle for the same normalized path can resolve the new generation.
- [x] T062 Implement `ResourceId`, `ResourceOwnerToken`, and `ResourceHandle<T>` in `Runtime/Core/ResourceManagement` on top of `ResourceLifetimeRegistry` without removing legacy raw-pointer APIs.
- [x] T063 Add manager-level `AcquireResourceHandle` and safe `TrimUnusedResources` compatibility APIs for mesh/material/texture managers.
- [x] T064 Migrate Scene View imported-model preview to hold owner tokens or resource handles instead of relying on long-lived raw pointers for preview-only resources.
- [x] T065 Migrate generated-model committed-instance creation/destruction to acquire scene owner references and release them on `EditorActions::DestroyGameObject` and scene unload.
- [x] T066 Add contract coverage proving long-lived generated-model preview/drop code paths call the handle/owner API and do not directly call unsafe `UnloadResource(path)`.
- [x] T067 Run focused ResourceHandle, lifetime registry, preview cancellation, scene removal, and generated-model drag/drop tests plus `git diff --check`.

## Phase 6h: Scene Prefab Streaming Load

- [x] T068 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving `SceneManager::LoadScene` keeps the parsed `ObjectGraphDocument` available for editor prefab restore and `EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk` consumes it instead of rereading/reparsing the scene file.
- [x] T069 Add RED contract coverage proving scene-load renderer-resource resolution uses `kSceneLoadRendererResourceResolutionTargetPlatform` for diagnostics but is filtered out of native blocking task progress.
- [x] T070 Implement cached parsed-scene-document access in `Runtime/Engine/SceneSystem/SceneManager.h/.cpp` and clear it on empty scene, scene unload, failed load, and scene source changes.
- [x] T071 Update `Project/Editor/Core/EditorActions.cpp` startup/manual scene prefab restore to use the cached scene document when present, falling back to disk only for legacy or externally restored scenes.
- [x] T072 Ensure restored generated/model prefab renderer resources are queued after scene activation through existing delayed/background resolution, each renderer reveals as soon as its mesh/material/texture bindings are ready together, and scene unload or prefab removal cancels/detaches remaining work.
- [x] T073 Build `NullusUnitTests` and run focused scene-load streaming, PrefabInstance restore, renderer-resource resolution, lifetime owner, and scene-switch cancellation tests.
- [x] T074 Run `git diff --check`.
- [ ] T075 Perform manual editor validation opening a saved large generated/model prefab scene: scene becomes interactive before all prefab renderer resources complete, no native blocking renderer-resource dialog appears after scene activation, no white model appears, and prefabs reveal as resources stream in.

## Phase 6i: Large Prefab Lifetime Hot-Path Optimization

- [x] T076 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving `ResourceLifetimeRegistry` uses indexed resource and owner lookup for large imported prefab loads.
- [x] T077 Replace `ResourceLifetimeRegistry` resource and owner linear scans with hash-indexed lookup while preserving normalized path aliasing, owner lease counting, generations, eviction reservations, and deterministic LRU trim output.
- [x] T078 Remove the registry dependency on recursive locking by making `ReleaseOwnersByKind` share a no-lock release helper under one registry lock.
- [x] T079 Build `NullusUnitTests`, run focused lifetime/resource-release/scene-restore tests, and run `git diff --check`.

## Phase 6j: Cached Runtime Prefab Fast Bind

- [x] T080 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving repeated generated/model prefab drag/drop fast-binds when mesh/material/texture runtime resources are already cached.
- [x] T081 Implement an all-or-nothing cached runtime bind path in `Project/Editor/Core/EditorActions.cpp` before `RendererResourceResolutionState`, import progress jobs, or delayed renderer-resource queues are created.
- [x] T082 Preserve no-white-model behavior by falling back to the existing deferred renderer-resource resolution whenever any mesh, material, or texture dependency is missing or invalid.
- [x] T083 Build `NullusUnitTests`, run focused cached-runtime prefab fast-bind/resource-resolution/lifetime tests, and run `git diff --check`.

## Phase 6k: Scene View Drag Preview Continuity

- [x] T084 Add RED contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving an active Scene View editor-asset drag payload keeps the mesh/material/texture preview alive even when the ImGui viewport drop target transiently misses before delivery.
- [x] T085 Update `Project/Editor/Panels/SceneView.cpp` so `HandleViewportAssetDragDrop` peeks the active editor-asset drag payload and continues updating the preview while the mouse remains over Scene View instead of clearing it.
- [x] T086 Build `NullusUnitTests`, run focused Scene View drag preview lifecycle tests, and run `git diff --check`.

## Phase 6l: Manifest Prefab Key Drag Preview Fix

- [x] T087 Add RED contract coverage proving imported model Scene View preview/drop preserves the manifest-selected prefab sub-asset key instead of replacing it with a guessed `prefab:<file>` key.
- [x] T088 Update `Project/Editor/Panels/AssetBrowser.cpp` so whole-model drag payloads prefer the current manifest's usable prefab artifact sub-asset key.
- [x] T089 Update `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` so generated-model preview, direct drop, and cached-preview commit derive the default prefab key only when the payload key is empty.
- [x] T090 Build `NullusUnitTests`, run focused generated-model Scene View preview/drop tests, imported prefab hot-cache tests, and run `git diff --check`.
