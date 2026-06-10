# Tasks: Unity Prefab Parity Phase 2

**Input**: Design documents from `specs/043-unity-prefab-parity/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `quickstart.md`

**Tests**: Test tasks are included because this change affects persistence, editor workflow, rendering/resource lifecycle, and previously regressed user-visible behavior.

**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently.

## Phase 1: Setup And Baseline

**Purpose**: Freeze the current failure cases and validation gates before changing implementation.

- [x] T001 Record current manual baseline for drag preview, release stall, white-model state, scene load divergence, delete CPU/GPU release, and save/reload behavior in `specs/043-unity-prefab-parity/plan.md`
- [x] T002 [P] Add baseline telemetry export hooks for prefab load path comparisons in `Runtime/Core/Assets/ArtifactLoadTelemetry.h`
- [x] T003 [P] Add baseline resource lifetime diagnostics for owner acquire/release/trim in `Runtime/Core/ResourceManagement/ResourceLifetimeRegistry.h`
- [x] T004 Update `specs/043-unity-prefab-parity/quickstart.md` with the exact editor project, scene, and imported prefab asset used for manual validation

---

## Phase 2: Foundational Shared Prefab Lifecycle

**Purpose**: Shared model and contracts that must exist before individual user stories can converge.

- [x] T005 Add `UnifiedPrefabLoadRequest` and shared cache-key data structures in `Project/Editor/Assets/EditorAssetDragDropBridge.h`
- [x] T006 Add shared prefab source identity normalization helpers in `Project/Editor/Assets/PrefabUtilityFacade.h`
- [x] T007 Add artifact stamp collection for prefab, manifest, mesh, material, and texture dependencies in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T008 Add focused RED coverage proving scene load and drag/drop produce the same normalized prefab load key in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T009 Add focused RED coverage proving stale manifest/prefab/mesh/material/texture stamps invalidate all prefab load modes consistently in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T010 Run `git diff --check`

**Checkpoint**: Unified identity and invalidation contract is test-defined.

---

## Phase 3: User Story 1 - Prefab Instances Round-Trip Like Unity (Priority: P1) MVP

**Goal**: Scene persistence and reload use authoritative Unity-style prefab-instance records with modifications and correspondence.

**Independent Test**: Save/reload a scene with normal and imported/model prefab instances, then verify document records, registry state, overrides, and missing-source behavior.

### Tests for User Story 1

- [x] T011 [P] [US1] Add RED coverage for authoritative document-level prefab-instance save records in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T012 [P] [US1] Add RED coverage for source/instance correspondence round trip in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [x] T013 [P] [US1] Add RED coverage for normal prefab scene-local override save/reload/apply/revert in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T014 [P] [US1] Add RED coverage for generated/model prefab scene-local override reload plus read-only apply rejection in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T015 [P] [US1] Add RED coverage for missing/corrupt source prefab recovery metadata preservation in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`

### Implementation for User Story 1

- [x] T016 [US1] Extend `Runtime/Engine/Serialize/ObjectGraphDocument.h` with stronger prefab-instance correspondence and recovery fields
- [x] T017 [US1] Update prefab-instance read/write support in `Runtime/Engine/Serialize/ObjectGraphReader.h` and `Runtime/Engine/Serialize/ObjectGraphWriter.h`
- [x] T018 [US1] Update `Project/Editor/Assets/PrefabUtilityFacade.cpp` to emit document-level prefab-instance records as authoritative new-save data
- [x] T019 [US1] Update `Project/Editor/Assets/PrefabUtilityFacade.cpp` to restore prefab instances from source prefab plus modifications before legacy fallback
- [x] T020 [US1] Implement normal prefab apply/revert behavior for supported scene-local modifications in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T021 [US1] Implement generated/model prefab read-only apply rejection while preserving scene-local override save/reload in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T022 [US1] Preserve missing-source recovery objects and diagnostics during scene load in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T023 [US1] Run focused prefab save/reload tests from `Tests/Unit/PrefabUtilityFacadeTests.cpp` and `Tests/Unit/SceneObjectGraphSerializationTests.cpp`

**Checkpoint**: PrefabInstance persistence is independently functional.

---

## Phase 4: User Story 2 - One Prefab Loading Path For Drag, Drop, And Scene Load (Priority: P1)

**Goal**: Scene-loaded, dragged, duplicated, and repeated prefab instances share the same load request, cache, readiness, and ownership behavior.

**Independent Test**: Load a scene with a large imported/model prefab, drag the same prefab again, and verify both paths use the same warm load/resource path.

### Tests for User Story 2

- [x] T024 [P] [US2] Add RED coverage proving scene prefab restore and Scene View drag call the shared `UnifiedPrefabLoadRequest` path in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T025 [P] [US2] Add RED coverage proving repeated drag of a scene-loaded prefab fast-binds already cached runtime mesh/material/texture resources in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T026 [P] [US2] Add RED coverage proving no-white-model readiness is shared by scene load, preview, and final drop in `Tests/Unit/EditorAssetDragDropTests.cpp`

### Implementation for User Story 2

- [x] T027 [US2] Route scene-load prefab restoration through `UnifiedPrefabLoadRequest` in `Project/Editor/Core/EditorActions.cpp`
- [x] T028 [US2] Route Scene View preview and drop through `UnifiedPrefabLoadRequest` in `Project/Editor/Panels/SceneView.cpp`
- [x] T029 [US2] Route hierarchy prefab drop through the shared readiness gate in `Project/Editor/Panels/Hierarchy.cpp`
- [x] T030 [US2] Consolidate successful prefab graph/runtime resource cache entries in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T031 [US2] Ensure cache misses and pending states preserve the shared no-white-model rule in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T032 [US2] Run focused unified-load tests from `Tests/Unit/GameObjectAssetImportTests.cpp` and `Tests/Unit/EditorAssetDragDropTests.cpp`

**Checkpoint**: Scene load and drag/drop no longer diverge for prefab loading.

---

## Phase 5: User Story 3 - Drag Preview Is A Textured Mouse-Follow Prefab Preview (Priority: P1)

**Goal**: Scene View drag creates a private preview-scene prefab object that follows the mouse and renders textured content before release.

**Independent Test**: Drag a ready imported/model prefab over Scene View, observe textured preview while holding the mouse, cancel and drop paths, and verify no committed object appears before drop.

### Tests for User Story 3

- [x] T033 [P] [US3] Add RED coverage for private preview-scene lifecycle and non-committed hierarchy behavior in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T034 [P] [US3] Add RED coverage for preview following transient Scene View hover updates before drop in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T035 [P] [US3] Add RED coverage for textured preview readiness and no crosshair/label-only ready fallback in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T036 [P] [US3] Add RED coverage for preview cancel cleanup not affecting existing scene prefab resources in `Tests/Unit/ResourceLifetimeRegistryTests.cpp`

### Implementation for User Story 3

- [x] T037 [US3] Implement or harden private preview-scene prefab instance ownership in `Project/Editor/Panels/SceneView.cpp`
- [x] T038 [US3] Ensure hover prewarm starts before mouse release in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T039 [US3] Render preview-scene mesh/material/texture drawables through `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [x] T040 [US3] Remove any ready-state crosshair or label-only fallback from imported/model prefab preview in `Project/Editor/Panels/SceneView.cpp`
- [x] T041 [US3] Commit exactly one connected instance from final preview placement in `Project/Editor/Core/EditorActions.cpp`
- [x] T042 [US3] Run focused Scene View preview tests from `Tests/Unit/EditorAssetDragDropTests.cpp` and `Tests/Unit/RenderSceneCacheTests.cpp`

**Checkpoint**: Drag preview is visible, textured, follows mouse, and remains non-committed until release.

---

## Phase 6: User Story 4 - Resource Lifetime Matches Unity-Like Ownership (Priority: P1)

**Goal**: Resource ownership, deletion, cancellation, scene switch, and unused-resource trim behave like Unity-style reachable resource caching.

**Independent Test**: Share one imported model resource set across scene instances, preview, inspector, async jobs, deletion, scene switch, and trim.

### Tests for User Story 4

- [x] T043 [P] [US4] Add RED coverage proving scene, preview, inspector, and async job owners prevent premature trim in `Tests/Unit/ResourceLifetimeRegistryTests.cpp`
- [x] T044 [P] [US4] Add RED coverage proving stale `ResourceHandle<T>` returns null after generation invalidation in `Tests/Unit/ResourceLifetimeRegistryTests.cpp`
- [x] T045 [P] [US4] Add RED coverage proving prefab deletion releases instance-owned jobs, draw registrations, and resource owners in `Tests/Unit/EditorAssetDragDropTests.cpp`
- [x] T046 [P] [US4] Add RED coverage proving trim unloads only zero-owner resources with deterministic LRU order in `Tests/Unit/ResourceLifetimeRegistryTests.cpp`

### Implementation for User Story 4

- [x] T047 [US4] Guard generated/model prefab scene paths with `ResourceHandle<T>`/`ResourceLifetimeRegistry` ownership, eviction cancellation, and trim-safe scene owners while tracking full raw-pointer replacement as follow-up work.
- [x] T048 [US4] Ensure scene instance creation acquires resource owners in `Project/Editor/Core/EditorActions.cpp`
- [x] T049 [US4] Ensure scene unload and object deletion release resource owners and cancel jobs in `Project/Editor/Core/EditorActions.cpp`
- [x] T050 [US4] Ensure preview cancel and preview handoff release or transfer owners in `Project/Editor/Panels/SceneView.cpp`
- [x] T051 [US4] Ensure resource manager trim calls only unload registry-approved zero-owner paths in `Runtime/Rendering/Resources/MeshManager.cpp`, `Runtime/Rendering/Resources/MaterialManager.cpp`, and `Runtime/Rendering/Resources/TextureManager.cpp`
- [x] T052 [US4] Run focused lifetime and trim tests from `Tests/Unit/ResourceLifetimeRegistryTests.cpp`

**Checkpoint**: Removing/cancelling prefabs stops instance-owned work without breaking shared resource users.

---

## Phase 7: User Story 5 - Editor Prefab UX Surfaces Unity-Like State (Priority: P2)

**Goal**: Hierarchy and inspector expose connected, overridden, read-only, missing, pending, and unpacked prefab state.

**Independent Test**: Select each prefab state and verify hierarchy/inspector presentation matches the underlying prefab-instance and resource state.

### Tests for User Story 5

- [x] T053 [P] [US5] Add RED coverage for prefab editor state derivation in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T054 [P] [US5] Add RED coverage for model-prefab read-only apply diagnostic in `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [x] T055 [P] [US5] Add RED coverage for missing-source and pending-resource presentation state in `Tests/Unit/PrefabUtilityFacadeTests.cpp`

### Implementation for User Story 5

- [x] T056 [US5] Add hierarchy connected/missing/pending prefab state presentation in `Project/Editor/Panels/Hierarchy.cpp`
- [x] T057 [US5] Add inspector override/read-only/missing/pending state presentation in `Project/Editor/Panels/InspectorPanel.cpp`
- [x] T058 [US5] Wire apply/revert commands to prefab utility state in `Project/Editor/Assets/PrefabUtilityFacade.cpp`
- [x] T059 [US5] Run focused prefab UX state tests from `Tests/Unit/PrefabUtilityFacadeTests.cpp`

**Checkpoint**: Users can see and act on Unity-like prefab state.

---

## Phase 8: Scene Load Streaming Parity

**Purpose**: Ensure scene-open behavior matches the unified prefab loading and no-white-model rules.

- [x] T060 Add RED coverage proving scene object graph activation does not wait for all generated/model prefab renderer resources in `Tests/Unit/SceneObjectGraphSerializationTests.cpp`
- [x] T061 Add RED coverage proving scene-load prefab streaming uses the shared load request and owner tokens in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T062 Update scene-load restoration queue to reveal prefabs only after mesh/material/texture readiness together in `Project/Editor/Core/EditorActions.cpp`
- [x] T063 Ensure scene switch and object deletion cancel scene-load streaming jobs in `Project/Editor/Core/EditorActions.cpp`
- [x] T064 Run focused scene-load streaming tests from `Tests/Unit/SceneObjectGraphSerializationTests.cpp` and `Tests/Unit/GameObjectAssetImportTests.cpp`

---

## Phase 9: Prepared Prefab Cache Parity

**Purpose**: Keep scene restore and repeated prefab instantiation on the same Unity-like prepared-artifact path instead of reparsing prefab graphs or rescanning renderer dependencies every editor session.

- [x] T065 Add RED coverage proving generated/model prefab scene restore reuses a persistent prepared graph cache after editor-session hot-cache eviction in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T066 Add RED coverage proving persisted renderer dependency templates are consumed from disk, not rebuilt from the prefab graph, in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [x] T067 Persist prepared prefab graph, resolved assets, base chain, and renderer dependency templates under `Library/PreparedPrefabCache/` in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [x] T068 Key prepared cache entries by unified prefab identity plus manifest content, prefab artifact content, and renderer dependency stamps so source/import edits invalidate scene restore and drag/drop consistently
- [x] T069 Add bounded entry/byte budgets and throttled eviction pruning so persistent cache population cannot stall every cold prefab load or retain every imported model indefinitely
- [x] T070 Reuse prepared renderer dependency templates from the L1 hot cache in `Project/Editor/Core/EditorActions.cpp` before falling back to graph scanning
- [x] T071 Ensure missing mesh artifact retries have a finite budget and eventually settle into the existing failure/placeholder path instead of leaving prefab resolution pending forever

---

## Phase 10: Verification, Evidence, And Review

**Purpose**: Prove behavior and prepare for implementation sign-off.

- [ ] T072 Build full `NullusUnitTests` after unrelated compile blockers are cleared
- [x] T073 Run focused unified-load and prepared-cache tests in `Tests/Unit/GameObjectAssetImportTests.cpp`
- [ ] T074 Run remaining focused prefab instance, drag preview, resource lifetime, trim, editor UX, and scene streaming tests
- [x] T075 Run `git diff --check`
- [ ] T076 Perform manual editor validation from `specs/043-unity-prefab-parity/quickstart.md`
- [ ] T077 Capture RenderDoc or equivalent backend evidence proving preview and committed paths bind intended mesh/material/texture resources
- [ ] T078 Update `specs/043-unity-prefab-parity/plan.md` with measured cold/warm timing, cache hit/miss, release stall, deletion release, and scene-load streaming evidence
- [ ] T079 Run `/plan-review` and resolve all P0/P1 findings before merge or commit

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1**: No dependencies.
- **Phase 2**: Depends on Phase 1 and blocks all user story implementation.
- **US1**: Depends on Phase 2 and is the MVP because persistence is the highest-risk data-loss behavior.
- **US2**: Depends on Phase 2 and can proceed after shared identity contracts exist.
- **US3**: Depends on Phase 2 and should integrate with US2 shared load requests.
- **US4**: Depends on Phase 2 and can proceed in parallel with US2/US3 after owner contracts are defined.
- **US5**: Depends on US1 and US4 state being available.
- **Phase 8**: Depends on US2 and US4.
- **Phase 9**: Depends on US2 and Phase 8 because prepared cache keys and scene streaming must share the same unified prefab request.
- **Phase 10**: Depends on selected implementation phases.

### Parallel Opportunities

- RED tests marked `[P]` can be written in parallel because they target different files or independent story slices.
- US1 serialization tests and US4 lifetime tests can proceed in parallel after Phase 2.
- US2 load unification and US3 preview work should coordinate on `EditorAssetDragDropBridge.cpp` and `SceneView.cpp` to avoid file conflicts.
- US5 should wait until prefab state and resource state are stable enough to present.

## Implementation Strategy

### MVP First

1. Complete Phase 1 and Phase 2.
2. Complete US1 so prefab data no longer disappears or silently unpacks after save/reload.
3. Validate US1 independently before changing preview or resource lifetime behavior.

### Next Critical Slice

1. Complete US2 and US4 together because unified load and resource ownership must agree.
2. Complete US3 on top of the shared load/lifetime path.
3. Validate drag preview, drop, and deletion with manual editor evidence.

### Final UX And Streaming

1. Complete US5 after core state is reliable.
2. Complete Phase 8 scene streaming parity.
3. Run Phase 9 evidence and `/plan-review`.
