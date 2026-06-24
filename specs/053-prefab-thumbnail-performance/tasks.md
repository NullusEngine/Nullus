# Tasks: Prefab and Thumbnail Performance

**Input**: Design documents from `/specs/053-prefab-thumbnail-performance/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. This feature changes performance-critical editor/runtime behavior; write benchmark-style and regression-style tests before or with implementation.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to
- Include exact file paths in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish the shared stage statistics and benchmark scaffolding used by all stories.

- [X] T001 Create `Runtime/Base/Profiling/PerformanceStageStats.h` and `Runtime/Base/Profiling/PerformanceStageStats.cpp` for stage timing, counter aggregation, snapshot, and report data structures
- [X] T002 [P] Add `Tests/Unit/PerformanceStageStatsTests.cpp` covering stage duration aggregation, thread split accounting, counter accumulation, empty snapshot behavior, and top-bottleneck ranking
- [X] T003 [P] Add shared benchmark fixture helpers in `Tests/Unit/AssetPrefabPerformanceTests.cpp` for prefab scenario generation and baseline/comparison snapshots
- [X] T004 [P] Add shared benchmark fixture helpers in `Tests/Unit/AssetThumbnailPerformanceTests.cpp` for thumbnail scenario generation, queue state setup, and baseline/comparison snapshots

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The diagnostics and measurement foundation that MUST be complete before any optimization story begins.

**CRITICAL**: No P0/P1/P2 optimization work should begin until this phase is complete.

- [X] T005 Add failing tests in `Tests/Unit/AssetPrefabPerformanceTests.cpp` for prefab stage emission, call counts, object/component/renderer/dependency counters, hot-cache comparison reporting, and bottleneck ranking
- [X] T006 Add failing tests in `Tests/Unit/AssetThumbnailPerformanceTests.cpp` for thumbnail stage emission, duplicate request counting, per-frame generation counters, encode/write accounting, and bottleneck ranking
- [X] T006C Add failing tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` for GPU preview fence wait accounting at the RHI readback completion boundary
- [X] T006A [P] Add failing tests in `Tests/Unit/AssetPrefabPerformanceTests.cpp` for missing-baseline and mismatched-scenario comparison rejection
- [X] T006B [P] Add failing tests in `Tests/Unit/AssetThumbnailPerformanceTests.cpp` for deterministic `ThumbnailGenerationBudget` injection and queue/backlog/in-flight/cancellation-latency/coalescing-pressure counters
- [X] T007 Add failing tests in `Tests/Unit/AssetThumbnailCacheTests.cpp` for thumbnail key invalidation when asset stamp, settings hash, dependency stamp, or renderer version changes
- [X] T008 Add failing tests in `Tests/Unit/AssetPrefabPipelineTests.cpp` for prepared prefab freshness rejection when artifact stamp, dependency stamp, reflection schema, importer version, or serialization format version changes
- [X] T009 Implement `PerformanceStageStats` snapshot/merge/report logic in `Runtime/Base/Profiling/PerformanceStageStats.cpp` and expose the public API in `Runtime/Base/Profiling/PerformanceStageStats.h`
- [X] T010 Wire prefab and thumbnail diagnostics to collect a shared baseline/comparison report structure in `Project/Editor/Assets/AssetDatabaseFacade.cpp`, `Project/Editor/Assets/AssetThumbnailService.cpp`, and `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`
- [X] T011 Run `ctest --test-dir build --output-on-failure -R "PerformanceStageStatsTests|AssetPrefabPerformanceTests|AssetThumbnailPerformanceTests|AssetPrefabPipelineTests|AssetThumbnailCacheTests"` and verify the new failing tests fail for the expected missing behavior

**Checkpoint**: Diagnostics foundation ready. Real baseline data can now be collected before optimization.

---

## Phase 3: User Story 1 - Diagnose Real Prefab and Thumbnail Bottlenecks (Priority: P1) 🎯 MVP

**Goal**: Produce repeatable prefab and thumbnail performance reports that rank the actual hot stages before optimization claims are made.

**Independent Test**: Running the benchmark suites yields a report with stage timings, counts, and bottleneck ranking for each covered scenario.

### Tests for User Story 1

- [X] T012 [P] [US1] Add prefab benchmark scenarios for small, 100 object, 1000 object, multi-renderer, nested, repeated, cold-cache, hot-cache, and L2 cache-hit cases in `Tests/Unit/AssetPrefabPerformanceTests.cpp`
- [X] T013 [P] [US1] Add thumbnail benchmark scenarios for first generation, memory cache hit, disk cache hit, and rapid scrolling/cancellation/deduplication cases in `Tests/Unit/AssetThumbnailPerformanceTests.cpp` using injected deterministic `ThumbnailGenerationBudget` values
- [X] T014 [US1] Add report assertions for baseline vs optimized comparison output and top-five bottleneck ranking in `Tests/Unit/AssetPrefabPerformanceTests.cpp`
- [X] T015 [US1] Add report assertions for thumbnail per-frame counts, duplicate request counts, queue depth/backlog, in-flight request counts, cancellation latency, coalescing pressure, fence wait time, and top-five bottleneck ranking in `Tests/Unit/AssetThumbnailPerformanceTests.cpp`

### Implementation for User Story 1

- [X] T016 [US1] Instrument prefab artifact loading and prepared prefab cache lookup stages in `Project/Editor/Assets/AssetDatabaseFacade.cpp` and `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`
- [X] T017 [US1] Instrument prefab instantiation stage timings and counters in `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, `Runtime/Engine/Assets/PrefabAsset.cpp`, and `Runtime/Engine/SceneSystem/Scene.cpp`
- [X] T018 [US1] Instrument thumbnail queueing, preview preparation, render submission, fence wait, readback, encode, and write stages in `Project/Editor/Assets/AssetThumbnailService.cpp`, `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, and `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T019 [US1] Add benchmark report assembly and console/file output helpers in `Project/Editor/Assets/AssetThumbnailService.cpp` or a focused diagnostics helper under `Project/Editor/Assets/`
- [X] T020 [US1] Run `ctest --test-dir build --output-on-failure -R "PerformanceStageStatsTests|AssetPrefabPerformanceTests|AssetThumbnailPerformanceTests"` and verify the baseline report is produced for every required scenario and that invalid comparisons are rejected

**Checkpoint**: User Story 1 delivers measurable bottleneck evidence and a baseline report.

---

## Phase 4: User Story 2 - Keep Prefab Instantiation Fast Without Changing Semantics (Priority: P2)

**Goal**: Reduce prefab hot-path cost while preserving prefab identity, nested overrides, external references, and lifecycle behavior.

**Independent Test**: Repeated instantiation and editor-restart cache-hit scenarios improve relative to baseline while semantic tests still pass.

### Tests for User Story 2

- [X] T021 [P] [US2] Add failing tests for repeated instantiation hot-path reuse, deferred activation, and semantic preservation in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [X] T022 [P] [US2] Add failing tests for editor-restart prepared-cache hits and freshness invalidation in `Tests/Unit/AssetPrefabPipelineTests.cpp`

### Implementation for User Story 2

- [X] T023 [US2] Introduce batch allocation and populate phases in `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T024 [US2] Separate internal fixup, external resource binding, and registration phases in `Runtime/Engine/Serialize/ObjectGraphInstantiator.h` and `Runtime/Engine/SceneSystem/Scene.cpp`
- [X] T025 [US2] Add deferred activation support so lifecycle callbacks run only after all prefab data is prepared in `Runtime/Engine/SceneSystem/Scene.cpp` and `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T026 [US2] Reduce synchronous resource loading in prefab instantiation by binding handles and pending states instead of loading directly in `Runtime/Engine/Assets/PrefabAsset.cpp` and resource manager call sites
- [X] T027 [US2] Run prefab semantic and repeated-instantiation tests in `Tests/Unit/AssetPrefabPipelineTests.cpp` and compare optimized timing against the baseline report

**Checkpoint**: User Story 2 improves prefab hot-path timing while keeping semantics intact.

---

## Phase 5: User Story 3 - Generate Thumbnails Without Blocking Editor Interaction (Priority: P3)

**Goal**: Make thumbnail generation asynchronous, deduplicated, cancellable, and preview-reuse friendly without blocking the editor main thread.

**Independent Test**: Rapid browser scrolling and duplicate requests show non-blocking placeholder behavior, deduped tasks, and async readback/encode/write.

### Tests for User Story 3

- [X] T028 [P] [US3] Add failing tests for thumbnail request deduplication, visible-item priority, cancellation, and non-blocking fence behavior in `Tests/Unit/AssetThumbnailCacheTests.cpp`
- [X] T029 [P] [US3] Add failing tests for preview world/render target reuse, preview-mode behavior that excludes scripts, physics, audio, navigation, full runtime scene registration, post-processing, and unnecessary lifecycle callbacks, plus editor-shutdown safety while readback/encode/write/resource work is in flight in `Tests/Unit/AssetThumbnailPerformanceTests.cpp`

### Implementation for User Story 3

- [X] T030 [US3] Add injectable `ThumbnailGenerationBudget` defaults and make `AssetThumbnailService` queue processing budget-aware, cancellation-aware, and backlog-aware in `Project/Editor/Assets/AssetThumbnailService.cpp` and `Project/Editor/Assets/AssetThumbnailService.h`
- [X] T031 [US3] Change thumbnail GPU preview readback path to use `BeginReadPixels` plus non-blocking completion polling while keeping `ReadPixelsChecked` as the explicit blocking API in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp` and `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`
- [X] T032 [US3] Reuse preview scene, camera, render targets, depth targets, descriptor pools, and preview shaders in `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`
- [X] T033 [US3] Move thumbnail encoding and disk writes off the main thread or into bounded background work in `Project/Editor/Assets/AssetThumbnailService.cpp` and `Project/Editor/Assets/AssetThumbnailCache.cpp`
- [X] T034 [US3] Verify the rapid-scroll benchmark shows deduplication, cancellation, and budget adherence in `Tests/Unit/AssetThumbnailPerformanceTests.cpp`

**Checkpoint**: User Story 3 delivers non-blocking thumbnail generation with reuse and budgets.

---

## Phase 6: User Story 4 - Share Artifact and Resource Caches Without Mixing Responsibilities (Priority: P4)

**Goal**: Share prepared artifacts and runtime resources across prefab and thumbnail flows while keeping each cache role bounded, versioned, and separately invalidated.

**Independent Test**: Cache hit, miss, invalidation, eviction, retry, and concurrent request behavior passes for each cache role independently.

### Tests for User Story 4

- [X] T035 [P] [US4] Add failing tests for prepared prefab cache key/version/stamp invalidation, resource request coalescing, and bounded eviction in `Tests/Unit/AssetPrefabPipelineTests.cpp`
- [X] T036 [P] [US4] Add failing tests for thumbnail texture cache and disk cache capacity limits, stale invalidation, and finite retry behavior in `Tests/Unit/AssetThumbnailCacheTests.cpp`

### Implementation for User Story 4

- [X] T037 [US4] Define explicit prepared prefab cache keys, binary artifact versioning hooks, and success-only reuse gates in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp` and `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [X] T038 [US4] Introduce or extend runtime resource cache state/coalescing logic for mesh/material/texture/shader requests in `Runtime/Rendering/ResourceManagement/MeshManager.cpp`, `Runtime/Rendering/ResourceManagement/MaterialManager.cpp`, and related resource managers
- [X] T039 [US4] Add preview snapshot cache and thumbnail texture/disk cache capacity/invalidation/statistics hooks in `Project/Editor/Assets/AssetThumbnailCache.cpp` and `Project/Editor/Assets/AssetThumbnailService.cpp`
- [X] T040 [US4] Add cache debug/clear/report helpers and verify memory recovers after eviction in `Project/Editor/Assets/AssetThumbnailCache.cpp` and `Project/Editor/Assets/AssetThumbnailService.cpp`

**Checkpoint**: All cache roles are explicit, bounded, and independently testable.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, final validation, and regression guardrails across the whole performance feature.

- [X] T041 Update `specs/053-prefab-thumbnail-performance/quickstart.md` with the exact final test and benchmark commands used during implementation
- [X] T042 Update `specs/053-prefab-thumbnail-performance/contracts/performance-diagnostics.md`, `cache-identity.md`, and `thumbnail-scheduler.md` with the final observable behavior and acceptance rules
- [X] T043 Run the full validation suite from `quickstart.md` and confirm the final report includes baseline and optimized comparisons for prefab and thumbnail scenarios
- [X] T044 Run `/plan-review` quality gate for the completed design and implementation before reporting completion
- [X] T045 Add bounded recent-work gating for drag-triggered prefab hot-cache preload requests in `Project/Editor/Core/RecentBackgroundWorkGate.*`, `Project/Editor/Panels/AssetBrowser.cpp`, `Project/Editor/Panels/Hierarchy.cpp`, and `Project/Editor/Panels/SceneView.cpp` so repeated drag hover does not refill the editor background task queue with the same key
- [X] T046 Add Tracy-visible scopes around Asset Browser folder-open hotspots in `Project/Editor/Panels/AssetBrowser.cpp`, `Project/Editor/Assets/AssetBrowserPresentation.cpp`, and `Project/Editor/Assets/AssetThumbnailService.cpp` to split `Panel::Draw.Asset Browser` into folder tree, folder item refresh, sub-asset snapshot/materialization, display item rebuild, and thumbnail request/cache lookup segments
- [X] T047 Add Tracy-visible scopes inside `AssetBrowser::DrawCurrentFolderGrid` and reduce grid draw CPU amplification by using binary-search label ellipsizing and a single columns block for visible grid rows in `Project/Editor/Panels/AssetBrowser.cpp`
- [X] T048 Defer Asset Browser GPU thumbnail pumping after folder/display rebuild, avoid eager preview renderer creation, and keep prefab hot-cache preload tied to drag sources instead of thumbnail completion in `Project/Editor/Panels/AssetBrowser.cpp`
- [X] T049 Disable Asset Browser main-thread GPU preview generation for model/prefab thumbnails after Tracy showed `PumpThumbnailGeneration.TryGpu` blocking folder-open frames without producing preview images; existing disk thumbnails and fallback icons remain available in `Project/Editor/Panels/AssetBrowser.cpp`
- [X] T050 Restore PrefabPreview requests as non-blocking pending thumbnail work while keeping ModelPreview generation out of the Asset Browser main-thread pump, so prefab thumbnails are not permanently filtered while folder-open frames avoid `TryGpu` stalls in `Project/Editor/Panels/AssetBrowser.cpp`
- [X] T051 Add a RenderScene synchronization fast path for unchanged mesh renderer inputs, plus Tracy sub-scopes and regression coverage, after Tracy identified `BaseSceneRenderer::ParseScene::SynchronizeRenderScene` as the remaining large-folder stall in `Runtime/Engine/Rendering/RenderScene.*`, `Runtime/Engine/Components/*`, `Runtime/Engine/GameObject.*`, and `Tests/Unit/RenderSceneCacheTests.cpp`
- [X] T052 Disable Asset Browser draw-path heavy GPU preview start after Tracy showed `AssetBrowser::PumpIdleHeavyGpuThumbnailGeneration` still blocking large-folder frames; keep PrefabPreview queued/pending/fallback until a true async or lightweight preview pipeline can service it in `Project/Editor/Assets/AssetBrowserPresentation.*`, `Project/Editor/Panels/AssetBrowser.*`, and `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [X] T053 Restore visible Prefab thumbnails through the existing CPU structure-thumbnail generator, without requiring a GPU preview renderer or full preview prefab instantiation from Asset Browser, in `Project/Editor/Assets/AssetThumbnailService.cpp` and `Tests/Unit/AssetThumbnailCacheTests.cpp`
- [X] T054 Add `PreviewRenderableSnapshot` for prefab graph renderer dependencies and route Prefab GPU preview through lightweight draw items plus the existing non-blocking readback pipeline instead of full `InstantiatePrefabArtifact` preview instances, in `Project/Editor/Assets/PreviewRenderableSnapshot.*`, `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, and `Tests/Unit/AssetThumbnailPerformanceTests.cpp`
- [X] T055 Reduce scene-open Unity-style prefab restore CPU/memory amplification by letting restored prefab instance metadata share the loaded source prefab graph instead of copying it per instance, while keeping scene-local overrides as patches, in `Project/Editor/Assets/PrefabEditorWorkflow.*`, `Project/Editor/Assets/PrefabUtilityFacade.cpp`, `Project/Editor/Panels/Inspector.cpp`, and `Tests/Unit/PrefabUtilityFacadeTests.cpp`
- [X] T056 Fix Asset Browser thumbnails remaining invisible during interactive folder browsing by allowing bounded interactive CPU thumbnail starts and cached thumbnail texture decode/upload pumping, while keeping heavy GPU preview starts disabled, in `Project/Editor/Assets/AssetBrowserPresentation.*`, `Project/Editor/Panels/AssetBrowser.*`, and `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [X] T057 Prevent prefab instance render sync crashes from invalid declared material texture paths by treating unnormalizable texture resource paths as unresolved instead of throwing from `std::filesystem::path`, in `Runtime/Engine/Rendering/RenderScene.cpp` and `Tests/Unit/RenderSceneCacheTests.cpp`
- [X] T058 Stop caching the debug prefab structure diagram as a Prefab thumbnail by routing `PrefabPreview` through the real preview renderer path, keeping renderer-less requests on the prefab icon/fallback path, and bumping the Prefab thumbnail settings fingerprint to invalidate old structure-diagram cache entries in `Project/Editor/Assets/AssetThumbnailService.cpp` and `Tests/Unit/AssetThumbnailCacheTests.cpp`
- [X] T059 Change committed prefab drops and scene restore back to synchronous full-readiness loading, with tests proving `FinalDrop`/`SceneRestore` reject or block instead of committing graph-only objects when renderer dependencies are missing, in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, `Project/Editor/Assets/PrefabEditorWorkflow.*`, `Project/Editor/Core/EditorActions.cpp`, `Tests/Unit/GameObjectAssetImportTests.cpp`, and `Tests/Unit/EditorAssetDragDropTests.cpp`
- [X] T060 Add user-visible progress coverage for blocking scene load and large prefab drop readiness work, reusing `Context::PresentTaskProgress` / `ImportProgressTracker` rather than adding a parallel progress system, in `Project/Editor/Core/EditorActions.cpp`, `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, and focused unit tests
- [X] T061 Fix Scene View prefab mouse-release commits so releasing before preview streaming finishes falls back to the synchronous final-drop path, destroys the transient preview root, cancels preview resource work before final loading, preserves placement, and shows the existing blocking prefab-drop progress in `Project/Editor/Panels/SceneView.cpp`, `Project/Editor/Core/EditorActions.cpp`, and `Tests/Unit/EditorAssetDragDropTests.cpp`
- [X] T062 Treat drag-triggered prefab hot-cache preloads as opportunistic background work so saturated editor queues quietly defer preview warming while synchronous final-drop loading still blocks and reports progress, in `Project/Editor/Core/EditorBackgroundTaskTracker.*`, `Project/Editor/Core/EditorActions.*`, `Project/Editor/Panels/AssetBrowser.cpp`, `Project/Editor/Panels/Hierarchy.cpp`, `Project/Editor/Panels/SceneView.cpp`, and `Tests/Unit/JobSystemMigrationTests.cpp`
- [X] T063 Fix already-imported prefab drops bypassing blocking progress by removing the no-tracker probe path, make Hierarchy and EditorActions fallbacks use the blocking drop API, and speed final drops by promoting drag-preview prefab graph hot-cache entries after renderer readiness validation in `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, `Project/Editor/Panels/Hierarchy.cpp`, `Project/Editor/Core/EditorActions.cpp`, `Tests/Unit/EditorAssetDragDropTests.cpp`, and `Tests/Unit/GameObjectAssetImportTests.cpp`
- [X] T064 Treat Scene View drag-preview mesh artifact preloads as opportunistic hover work so saturated editor queues silently defer preview-only mesh warming without logging queue-full warnings during prefab drag, in `Project/Editor/Panels/SceneView.cpp` and `Tests/Unit/EditorLaunchArgsTests.cpp`
- [X] T065 Commit Scene View prefab drops by connecting the existing drag-preview root as the final prefab instance, so mouse release does not destroy/reload ready previews and not-yet-ready previews continue their resource handoff to completion in `Project/Editor/Core/EditorActions.cpp`, `Tests/Unit/EditorAssetDragDropTests.cpp`, and `Tests/Unit/EditorLaunchArgsTests.cpp`
- [X] T066 Systematically reduce large prefab instantiation CPU cost by direct-applying serialized math values, batching Transform TRS restore into one local transform update, and skipping runtime graph copies when prepared prefabs have no external dependencies, with performance guards in `Tests/Unit/AssetPrefabPerformanceTests.cpp`, `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, `Runtime/Engine/Assets/PrefabAsset.cpp`, and `Runtime/Engine/Components/TransformComponent.*`
- [X] T067 Reduce large resource-prefab instantiation CPU cost when many renderers share the same mesh/material/texture-backed material dependencies by caching per-instantiation mesh/material path resolution and resource-manager lookup results, with performance guards in `Tests/Unit/AssetPrefabPerformanceTests.cpp` and `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`
- [X] T068 Optimize MaterialLoader and render-scene texture dependency resolution for large material/texture-backed prefabs by reusing equivalent cached texture artifacts, indexing TextureManager artifact paths, and caching declared texture lookups per render-scene sync in `Runtime/Core/ResourceManagement/TextureManager.h`, `Runtime/Rendering/ResourceManagement/TextureManager.cpp`, `Runtime/Rendering/Resources/Loaders/MaterialLoader.cpp`, `Runtime/Engine/Rendering/RenderScene.*`, `Tests/Unit/AssetMaterialConversionTests.cpp`, and `Tests/Unit/RenderSceneCacheTests.cpp`
- [X] T069 Promote Scene View prefab drag-preview pending material/texture loads to scene-owned shared interest on mouse-release commit, so preview cleanup cannot cancel not-yet-ready resources and final renderer resolution continues loading to ready in `Project/Editor/Core/EditorActions.cpp`, `Project/Editor/Core/RendererResourcePrewarmRequest.h`, and `Tests/Unit/EditorAssetDragDropTests.cpp`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - blocks all user stories
- **User Story 1 (P1)**: Depends on Foundational completion
- **User Story 2 (P2)**: Depends on User Story 1 diagnostics and may begin once baseline data exists
- **User Story 3 (P3)**: Depends on User Story 1 diagnostics and can proceed alongside User Story 2 after the baseline is stable
- **User Story 4 (P4)**: Depends on the cache/statistics groundwork from User Story 1 and can be expanded after P0/P1 behavior is measured
- **Polish**: Depends on the selected stories being complete

### Parallel Opportunities

- T002-T004 can run in parallel.
- T012-T015 can run in parallel once the shared stats API exists.
- T021-T022 can run in parallel.
- T028-T029 can run in parallel.
- T035-T036 can run in parallel.

### MVP Scope

MVP is Phase 1 + Phase 2 + Phase 3. It delivers repeatable prefab and thumbnail profiling with ranked bottlenecks and the evidence needed to safely start P0 optimization.

### Implementation Strategy

1. Build the shared stage stats and benchmark fixtures.
2. Add instrumentation and benchmark reports.
3. Validate and compare baseline performance.
4. Remove the largest P0 thumbnail stall first.
5. Batch and defer prefab and thumbnail hot-path work only after the measurements justify it.
6. Introduce bounded cache/refcount/coalescing improvements last, with explicit invalidation and memory caps.
