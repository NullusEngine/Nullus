# Implementation Plan: Unity-Ready Model Drop

**Branch**: `unity-ready-model-drop` | **Date**: 2026-06-01 | **Spec**: `specs/unity-ready-model-drop/spec.md`
**Input**: Feature specification from `specs/unity-ready-model-drop/spec.md`

## Summary

Align imported-model drag/drop with the approved Unity-like interaction model and align prefab scene persistence with Unity 2018.4 `PrefabInstance` semantics. Import time is responsible for generating and stabilizing mesh, material, texture, and generated-model-prefab relationships. Asset Browser drag payloads expose whether a current prefab artifact is preview-ready, and Scene View hover consumes that hint through an asset-layer fast preview prefab loader instead of refreshing the full asset database. Ready assets create a temporary preview-only mesh ghost that follows the mouse before release, repeated hover updates reuse the same ghost for the same asset identity, final drop commits a formal scene instance only when the generated model prefab is renderer-ready, and not-ready assets stay pending instead of committing a hidden instance that waits through long asynchronous post-drop resolution. Committed prefab instances must persist through editor scene save and reload as document-level Unity-style `PrefabInstance` records containing source prefab identity, modifications, and source/instance correspondence rather than as root-only metadata or plain unpacked object copies.

## Technical Context

**Language/Version**: C++17/C++20 project style already used by Nullus
**Primary Dependencies**: Nullus editor Scene View panel, editor asset drag/drop bridge, prefab workflow, scene object-graph serialization/loading, import progress tracking, native artifact manifest validation
**Storage**: Project `Library/Artifacts/<asset-guid>/manifest.json`, native artifact files, and editor scene documents containing document-level prefab instance records
**Testing**: `NullusUnitTests` with focused gtest filters plus targeted editor interaction contract tests
**Target Platform**: Windows editor, DX12 validation path
**Project Type**: Desktop editor/runtime engine
**Performance Goals**: For already imported large models, Scene View preview must appear immediately enough to support interactive drag placement, ready preview should render the imported mesh/prefab shape, and the committed drop path must avoid heavy synchronous work that belongs to import-time asset stabilization
**Constraints**: No hand edits under `Runtime/*/Gen/`; preserve existing watcher/import progress flow; do not regress prefab or non-generated asset drops; keep Scene View preview non-committed, disposable, and outside the active scene object graph
**Scale/Scope**: Large imported scenes such as Sponza, especially the user workflow of dragging an already imported large model into Scene View, modifying the prefab instance in scene, and persisting the resulting committed instance and overrides in scene files

## Constitution Check

- Spec-first major change: PASS. This bundle records the editor asset lifecycle and Scene View interaction change.
- Validation matches subsystem: PASS. Unit tests and focused editor interaction coverage match the editor/runtime asset workflow; RenderDoc remains secondary runtime evidence.
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
├── EditorAssetDragDropBridge.cpp         # Fast imported prefab loading, preview artifact loading, import-result reuse, and drop-time readiness gate
├── EditorAssetDragDropBridge.h           # Public fast preview prefab artifact API
├── EditorAssetDragPayload.cpp            # Preview-ready payload hint helpers
├── EditorAssetDragPayload.h              # Trivially-copyable drag payload shape
└── PrefabUtilityFacade.cpp               # Unity-style PrefabInstance scene record emission, restoration, and legacy scenePrefab fallback

Runtime/Engine/
├── SceneSystem/SceneManager.cpp          # Scene save/load flow
├── Serialize/ObjectGraphDocument.h       # Document-level PrefabInstance record model
├── Serialize/ObjectGraphReader.h         # PrefabInstance scene record parsing
├── Serialize/ObjectGraphWriter.h         # PrefabInstance scene record emission
└── Serialize/ObjectGraphSerializer.h     # Scene object graph plus prefab-instance-compatible object IDs

Tests/Unit/
├── EditorAssetDragDropTests.cpp          # Ready vs pending generated-model drop behavior
├── SceneObjectGraphSerializationTests.cpp# Scene save/load persistence for generated-model instances
└── EditorRenderPathContractTests.cpp     # Legacy async fallback and hidden-root regression protection
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

### Phase 6 - Validation

- Verify import-result reuse, preview lifecycle, ready commit behavior, pending/not-ready behavior, and save/load persistence with focused tests.
- Preserve the previously added regression coverage around fallback async resolution cleanup.

## Validation

Run:

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorAssetDragDropTests.*GeneratedModel*:EditorAssetDragDropTests.*SceneView*:SceneObjectGraphSerializationTests.*GeneratedModel*:EditorRenderPathContractTests.*GeneratedModel*:EditorRenderPathContractTests.*MaterialArtifact*
git diff --check
```

Manual editor validation:

```text
1. Drag an already imported large model over Scene View and confirm a mesh ghost preview appears quickly enough to support interactive placement.
2. Confirm the preview is not treated as a normal Hierarchy scene object during drag.
3. Release over a ready asset and confirm one committed visible instance appears at the preview placement without a long hidden wait.
4. Release over a not-ready asset and confirm no committed instance is created and the result reports pending or not ready.
5. Save the scene, reload it, and confirm the committed generated model instance is restored instead of disappearing.
```

Optional runtime evidence after unit validation:

```powershell
python tools/renderdoc/rdc_doctor.py App/Win64_Release_Runtime_Shared/Build/RenderDocCaptures/Editor/<new-capture>.rdc
```

Expected runtime outcome: representative large-model drop no longer relies on a long post-drop hidden formal instance before first visibility, ready committed draws still bind intended resources, and committed generated-model instances survive scene round-trip save/load.
