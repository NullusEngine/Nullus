# Tasks: Unity-Ready Model Drop

**Input**: `specs/unity-ready-model-drop/spec.md`, `specs/unity-ready-model-drop/plan.md`
**Prerequisites**: Existing generated-model readiness gate work and current Scene View drag/drop entry points

## Phase 1: Import Result Reuse And Speed Guardrails

- [ ] T001 Add focused RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving the already-imported Scene View drag path consumes stabilized generated-model import results rather than rebuilding mesh, material, texture, and prefab relationships during drag.
- [ ] T002 Add targeted instrumentation or contract coverage for already-imported large-model drag latency so preview-path main-thread work is observable during validation.
- [ ] T003 Update the already-imported drag path in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` and related entry points so it reuses stabilized import relationships and avoids heavy synchronous work that belongs to import-time asset preparation.

## Phase 2: Scene View Drag Preview

- [ ] T004 Add focused RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` or the closest existing editor interaction test file proving Scene View imported-model drag creates a temporary preview lifecycle before final drop.
- [ ] T005 Implement a temporary Scene View drag preview path in `Project/Editor/Panels/SceneView.cpp` for imported model and prefab assets.
- [ ] T006 Ensure the preview object is non-committed, is not treated as a normal Hierarchy item, and is cleaned up on drop or cancellation.
- [ ] T007 Preserve stable placement behavior so the preview follows the resolved Scene View drop target instead of jumping between unrelated fallback positions.

## Phase 3: Ready-Or-Pending Commit

- [ ] T008 Add RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a ready generated model drop commits one visible formal instance using the final preview or drop placement.
- [ ] T009 Add RED coverage in `Tests/Unit/EditorAssetDragDropTests.cpp` proving a not-ready generated model drop commits zero formal scene instances and reports pending or not-ready state.
- [ ] T010 Route Scene View final drop handling through the generated-model drop-time readiness gate in `Project/Editor/Core/EditorActions.cpp` and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`.
- [ ] T011 Remove the main-path dependency on "commit formal instance first, then hide root until long async resolution completes" for ready large-model Scene View drops.

## Phase 4: Hierarchy And Legacy Fallback Consistency

- [ ] T012 Ensure Hierarchy imported-model drop continues to use the same committed-instance readiness gate even without Scene View mouse-follow preview.
- [ ] T013 Add or update contract coverage in `Tests/Unit/EditorRenderPathContractTests.cpp` proving legacy async generated-model resolution remains a compatibility fallback and still cleans up hidden-root cancellation or failure correctly.
- [ ] T014 Keep `Project/Editor/Core/EditorActions.cpp` fallback cleanup behavior strong enough that cancelled or failed deferred resolution does not strand live hidden roots.

## Phase 5: Scene Persistence And Reload

- [ ] T015 Add RED save/load coverage in `Tests/Unit/SceneObjectGraphSerializationTests.cpp` proving a committed generated model instance survives scene save and reload.
- [ ] T016 Extend scene serialization/loading so committed generated-model instances round-trip as prefab-backed scene data instead of disappearing.
- [ ] T017 Rebuild prefab-instance tracking state for restored generated-model scene instances during scene load so editor prefab presentation and later prefab-aware operations still work.

## Phase 6: Verification

- [ ] T018 Build `NullusUnitTests`.
- [ ] T019 Run focused generated-model drag/drop, Scene View preview, scene persistence, and fallback cleanup tests.
- [ ] T020 Run `git diff --check`.
- [ ] T021 Perform a manual editor pass covering already-imported large-model preview speed, Scene View preview behavior, ready commit, not-ready pending behavior, and save/reload persistence.
