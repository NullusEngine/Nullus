# Implementation Plan: Unity-Ready Model Drop

**Branch**: `unity-ready-model-drop` | **Date**: 2026-06-01 | **Spec**: `specs/unity-ready-model-drop/spec.md`
**Input**: Feature specification from `specs/unity-ready-model-drop/spec.md`

## Summary

Align imported-model drag/drop with the approved Unity-like interaction model. Import time is responsible for generating and stabilizing mesh, material, texture, and generated-model-prefab relationships. Scene View drag then consumes those stabilized results: it creates a temporary preview object that follows the mouse before release, commits a formal scene instance only when the generated model prefab is renderer-ready, and keeps not-ready assets pending instead of committing a hidden instance that waits through long asynchronous post-drop resolution. Committed generated-model instances must also persist through scene save and reload as prefab-backed instances rather than disappearing.

## Technical Context

**Language/Version**: C++17/C++20 project style already used by Nullus
**Primary Dependencies**: Nullus editor Scene View panel, editor asset drag/drop bridge, prefab workflow, scene object-graph serialization/loading, import progress tracking, native artifact manifest validation
**Storage**: Project `Library/Artifacts/<asset-guid>/manifest.json` and native artifact files
**Testing**: `NullusUnitTests` with focused gtest filters plus targeted editor interaction contract tests
**Target Platform**: Windows editor, DX12 validation path
**Project Type**: Desktop editor/runtime engine
**Performance Goals**: For already imported large models, Scene View preview must appear immediately enough to support interactive drag placement, and the committed drop path must avoid heavy synchronous work that belongs to import-time asset stabilization
**Constraints**: No hand edits under `Runtime/*/Gen/`; preserve existing watcher/import progress flow; do not regress prefab or non-generated asset drops; keep Scene View preview non-committed and disposable
**Scale/Scope**: Large imported scenes such as Sponza, especially the user workflow of dragging an already imported large model into Scene View and persisting the resulting committed instance in scene files

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
├── SceneView.cpp                         # Scene View drag target, preview lifecycle, final drop commit
└── Hierarchy.cpp                         # Shared committed-drop behavior without Scene View preview

Project/Editor/Core/
├── EditorActions.cpp                     # Formal scene object creation, pending import completion, fallback resource resolution
└── EditorActions.h                       # Preview/commit helper declarations if needed

Project/Editor/Assets/
├── EditorAssetDragDropBridge.cpp         # Fast imported prefab loading, import-result reuse, and drop-time readiness gate
└── PrefabUtilityFacade.cpp               # Prefab-aware reconnect/recovery helpers if reused for persisted instances

Runtime/Engine/
├── SceneSystem/SceneManager.cpp          # Scene save/load flow
└── Serialize/ObjectGraphSerializer.h     # Scene serialization shape for persisted prefab-backed instances

Tests/Unit/
├── EditorAssetDragDropTests.cpp          # Ready vs pending generated-model drop behavior
├── SceneObjectGraphSerializationTests.cpp# Scene save/load persistence for generated-model instances
└── EditorRenderPathContractTests.cpp     # Legacy async fallback and hidden-root regression protection
```

**Structure Decision**: Keep import-time artifact stabilization in the existing asset pipeline, keep Scene View preview orchestration in the panel layer, keep committed-instance creation and delayed completion in `EditorActions`, keep drop-time readiness validation and import-result reuse in the existing drag/drop bridge, and extend scene serialization/loading so generated-model committed instances round-trip as prefab-backed scene data.

## Research

- Unity 2018.4 `SceneView` drag handling first drives per-editor drag callbacks, then calls the editor's native Scene View drag handler on `DragUpdated` and `DragPerform`.
- Unity treats imported 3D models as `ModelPrefab` assets and instantiates them through prefab instantiation rather than rebuilding the asset at drop time.
- Unity also uses a temporary drag object or preview behavior during drag, separate from the final committed placement.
- The Nullus investigation found that current generated-model drop behavior can commit a formal instance and then hide its root while conservative asynchronous renderer-resolution work runs for many frames.
- The user clarified the desired lifecycle boundary: import is responsible for stabilizing mesh/material/texture to generated-model-prefab relationships; drag/drop should consume that stabilized result instead of rebuilding those relationships on the Scene View path.
- The same investigation also found that current scene save/load appears to serialize ordinary object trees without reliably restoring generated-model prefab instance identity on reload, which explains the reported disappearance bug.
- Therefore the Unity-aligned correction is not only "correct final materials" but also a lifecycle split: temporary drag preview during drag, then ready-or-pending commit on release, plus prefab-backed persistence for committed instances.

## Data Model

- `DragPreviewObject`: temporary Scene View-only object lifecycle used while mouse drag is active.
- `CommittedDropResult`: success, pending, or rejected result of final drop handling.
- `RendererDependencyReadiness`: aggregate readiness result used by the primary committed-drop gate.
- `StabilizedImportRelationship`: import-produced mapping between generated model prefab content and its mesh, material, and texture dependencies that the drag path can consume directly.
- `PersistedGeneratedModelInstance`: serialized scene representation that preserves prefab asset identity and prefab sub-asset identity across save/load.
- `LegacyAsyncResolutionFallback`: existing deferred resolution path retained only for compatibility or non-primary cases.

## Phases

### Phase 1 - Import Result Reuse And Speed Guardrails

- Make the already-imported drag path explicitly consume stabilized import relationships instead of rebuilding them on the drag path.
- Add focused instrumentation or contract coverage for the already-imported large-model drag path so preview latency and main-thread work are observable.
- Keep heavy synchronous relationship-building out of the Scene View drag hot path.

### Phase 2 - Scene View Preview Lifecycle

- Add a dedicated Scene View drag preview path for imported model assets.
- Ensure preview objects are temporary, non-Hierarchy, and cleaned up on drop or cancellation.
- Reuse existing placement logic where practical so final preview placement is stable and meaningful.

### Phase 3 - Ready-Or-Pending Commit Path

- Route Scene View final drop through the existing generated-model readiness gate.
- Commit only ready formal instances.
- Keep not-ready assets in pending or importing state without creating a hidden formal instance as the main path.

### Phase 4 - Scene Persistence And Reload

- Extend scene serialization and load-time restoration so committed generated-model instances survive save/reload as prefab-backed instances.
- Rebuild editor prefab-instance tracking state when scenes containing generated-model instances are loaded.
- Keep transform, parentage, and prefab sub-asset identity stable across round-trip save/load.

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
1. Drag an already imported large model over Scene View and confirm preview appears quickly enough to support interactive placement.
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
