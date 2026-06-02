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

## Phase 6: Verification

- [x] T018 Build `NullusUnitTests`.
- [x] T019 Run focused generated-model drag/drop, Scene View preview, scene persistence, Unity-style PrefabInstance, and fallback cleanup tests.
- [x] T020 Run `git diff --check`.
- [ ] T021 Perform a manual editor pass covering already-imported large-model preview speed, Scene View preview behavior, ready commit, not-ready pending behavior, and save/reload persistence.
