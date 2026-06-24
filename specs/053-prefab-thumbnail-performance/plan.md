# Implementation Plan: Prefab and Thumbnail Performance

**Branch**: `053-prefab-thumbnail-performance` | **Date**: 2026-06-18 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/053-prefab-thumbnail-performance/spec.md`

## Summary

Nullus prefab instantiation and asset thumbnail generation need measured, repeatable performance evidence before optimization. The first increment adds shared stage profiling, statistics aggregation, benchmark scenarios, and a diagnostic report across the real prefab and thumbnail hot paths. Later increments use those measurements to remove main-thread thumbnail GPU fence waits, avoid full runtime prefab instantiation for previews, decouple prefab graph creation from synchronous resource loading, batch prefab instantiation stages, and introduce stricter prepared/cache/resource reuse.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus editor asset pipeline, runtime object graph serialization, scene system, resource managers, RHI readback/completion primitives, existing profiler, GoogleTest  
**Storage**: `Library/Artifacts/`, `Library/PreparedPrefabCache/`, thumbnail cache files/metadata, optional diagnostic report files under test or editor output directories  
**Testing**: `NullusUnitTests` focused GoogleTest cases plus benchmark-style unit/integration tests with deterministic fixture generation  
**Target Platform**: Nullus editor on Windows first; path/cache key logic must remain portable for existing cross-platform helpers  
**Project Type**: Desktop editor and runtime engine asset/scene pipeline  
**Performance Goals**: Establish measured baseline first; optimize relative to baseline for hot prefab instantiation, first thumbnail generation, cache hit paths, rapid browser scrolling, GPU fence wait time, synchronous resource load count, and memory recovery after eviction  
**Constraints**: Do not hand-edit generated files under `Runtime/*/Gen/`; do not add unbounded or uninvalidatable global caches; do not block the editor main thread for thumbnail GPU fences; do not create or mutate main-thread-only engine objects on background threads; preserve prefab references, nested prefab overrides, resources, and lifecycle behavior  
**Scale/Scope**: Prefab source/artifact load through runtime instantiation, prepared prefab cache, runtime resource loading/coalescing, thumbnail request queueing, preview rendering, readback/encoding/cache storage, tests, and diagnostics

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This change has a dedicated `specs/053-prefab-thumbnail-performance/` bundle before implementation.
- **Validation matches subsystem**: PASS. First validation is unit/benchmark instrumentation evidence; rendering-specific P0 changes require RHI/readback tests and focused runtime verification before claims.
- **Generated code and backend boundaries**: PASS. Generated `Runtime/*/Gen/` files are out of scope, and RHI behavior will be changed through existing coordinator/driver contracts rather than backend forks.
- **Incremental, verified delivery**: PASS. The plan explicitly gates optimization behind profiling data and separates P0, P1, and P2 changes.
- **Product runtime preservation**: PASS. Editor and Game must remain runnable; preview fallback/placeholder behavior is preserved while async generation catches up.

## Current Real Calling Chain

### Prefab

```text
Asset Browser / Drag Drop / Editor action
  -> AssetDatabaseFacade::LoadPrefabArtifactAtPath
  -> AssetDatabaseFacade::LoadPrefabArtifactByAssetId
  -> ReadNativeArtifactContainer
  -> ImportPrefabArtifact
  -> ObjectGraphReader::Read
  -> RefreshPrefabResolvedAssetsFromReferences
  -> PrefabArtifact::Validate
  -> InstantiatePrefabArtifact
  -> ObjectGraphInstantiator::InstantiatePrefab
  -> AnalyzeObjectTypes / AnalyzeReflectedObjectReferenceShapes / AnalyzeAssetReferences
  -> CreateGameObject
  -> ApplyGameObjectState
  -> InstantiateComponents
  -> RegisterComponentMappings
  -> ResolveParent
  -> Scene::AddGameObject
  -> Scene::RebuildRuntimeCachesAfterLoad
```

### Prepared Prefab Cache

```text
EditorAssetDragDropBridge prepared-prefab path
  -> PreparedPrefabCache lookup under Library/PreparedPrefabCache/*.json
  -> prepared graph/dependency manifest load
  -> drag/drop hot path instantiation
```

This is currently not the general prefab artifact load path. The first diagnostic increment must measure whether these concrete entry points hit or bypass it:

- `AssetDatabaseFacade::LoadPrefabArtifactAtPath`
- `AssetDatabaseFacade::LoadPrefabArtifactByAssetId`
- `EditorAssetDragDropBridge::InstantiateImportedAsset`
- `EditorThumbnailPreviewRenderer::RenderPrefabPreview`
- `NLS::Engine::Assets::InstantiatePrefabArtifact`

Prepared-prefab reuse is required to become a shared prepared-data service only after Phase 1 proves the bypass cost. The rollout path is: measure all entry points, add a read-through prepared prefab loader behind `AssetDatabaseFacade`/preview code, keep the drag/drop JSON path compatible, then later introduce the versioned binary artifact.

### Thumbnail

```text
AssetBrowser::UpdateThumbnailGenerationScope
  -> BuildAssetThumbnailRequestForItem
  -> AssetThumbnailService::GetThumbnail
  -> EvaluateAssetThumbnailCache
  -> queue by cache key / adopt in-flight request
  -> AssetThumbnailService::GenerateNextThumbnail(EditorThumbnailPreviewRenderer&)
  -> EditorThumbnailPreviewRenderer::Render
  -> EditorThumbnailPreviewRenderer::RenderPrefabPreview (for prefab previews)
  -> AssetDatabaseFacade::LoadPrefabArtifactByAssetId
  -> InstantiatePrefabArtifact with preview LoadPolicy
  -> ApplyPrefabPreviewResolvedMaterials
  -> CollectRenderableBounds / ConfigurePrefabCamera
  -> RenderCurrentPreviewScene
  -> DriverRendererAccess::ReadPixelsChecked
  -> RhiThreadCoordinator::ReadPixelsChecked
  -> completion->Wait()
  -> AssetThumbnailService::WriteRgbaThumbnailResult
  -> WriteThumbnailPngResult
  -> EncodeThumbnailPng
  -> WriteAssetThumbnailCacheFile / WriteAssetThumbnailCacheMetadata
```

## Diagnosed Bottleneck Hypotheses To Validate

1. Thumbnail GPU readback fence wait in `RhiThreadCoordinator::ReadPixelsChecked`.
2. Thumbnail prefab path instantiates a full prefab graph before preview rendering.
3. Thumbnail PNG encode and disk cache writes are synchronous after GPU preview rendering.
4. Prefab hot path repeats object graph parsing, validation, dependency/asset reference scans, path resolution, and reflection/property lookup.
5. Scene registration and transform hierarchy work may amplify costs for large prefabs.
6. Resource loading is partially coalesced for materials but not uniformly for mesh/material/texture/shader resource requests.
7. Prepared prefab cache currently appears scoped to drag/drop instead of the general prefab load and thumbnail paths.

These are hypotheses until the Phase 1 diagnostic report ranks measured costs.

## Project Structure

### Documentation (this feature)

```text
specs/053-prefab-thumbnail-performance/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── performance-diagnostics.md
│   ├── cache-identity.md
│   └── thumbnail-scheduler.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Base/Profiling/
├── Profiler.h/.cpp                         # Existing profiler dispatch; keep compatible
├── PerformanceStageStats.h/.cpp            # New shared stage timing/stat aggregation
└── ProfilerScope.h/.cpp                    # Existing RAII scope; may be reused by stage stats

Runtime/Engine/Assets/
├── PrefabAsset.cpp/.h                      # Prefab artifact parsing, resolved asset refresh, instantiate wrapper instrumentation

Runtime/Engine/Serialize/
├── ObjectGraphInstantiator.h               # Prefab allocation/populate/fixup/register/lifecycle instrumentation and later batching

Runtime/Engine/SceneSystem/
├── Scene.cpp/.h                            # Scene add/rebuild registration measurements and later batch registration hooks

Runtime/Rendering/Context/
├── RhiThreadCoordinator.cpp/.h             # Readback wait measurements and later non-blocking thumbnail readback path
└── Driver.cpp/.h                           # Existing renderer access forwarding

Runtime/Rendering/ResourceManagement/
├── MeshManager.cpp/.h                      # Resource load/prewarm measurements and later request coalescing
├── MaterialManager.cpp/.h                  # Existing async/coalesced material behavior measurements
└── TextureManager/Shader managers          # Add instrumentation if present and in the measured path

Project/Editor/Assets/
├── AssetDatabaseFacade.cpp/.h              # Prefab artifact load and dependency manifest measurements
├── EditorAssetDragDropBridge.cpp/.h        # Prepared prefab cache diagnostics and hit/miss tracking
├── AssetThumbnailService.cpp/.h            # Thumbnail request/cache/scheduler/encode/write measurements and later async work
├── EditorThumbnailPreviewRenderer.cpp/.h   # Preview preparation/render/readback measurements and later snapshot/lightweight preview
└── AssetThumbnailCache.cpp/.h              # Disk/memory cache lookup/store measurements

Project/Editor/Panels/
├── AssetBrowser.cpp/.h                     # Project Browser request budget, visible priority, duplicate/cancel measurements
└── AssetBrowserPresentation.*              # Existing request-selection helpers if needed for tests

Tests/Unit/
├── ProfilerScopeTests.cpp                  # Existing profiler tests
├── PerformanceStageStatsTests.cpp          # New stats aggregation tests
├── AssetPrefabPerformanceTests.cpp         # New prefab benchmark/stat tests
├── AssetThumbnailPerformanceTests.cpp      # New thumbnail benchmark/stat tests
├── AssetPrefabPipelineTests.cpp            # Existing prefab compatibility tests extended only when needed
└── AssetThumbnailCacheTests.cpp            # Existing thumbnail cache/scheduler tests extended only when needed
```

**Structure Decision**: Add a small shared `PerformanceStageStats` utility under `Runtime/Base/Profiling` instead of embedding ad hoc timers in prefab or thumbnail code. Prefab and thumbnail systems emit domain-specific stages into the shared collector, while reports and tests consume snapshots without depending on brittle source text. Later optimization files remain close to the existing hot paths to minimize broad refactors before measurements justify them.

## Phase 0: Research Output

See [research.md](research.md).

## Phase 1: Design Output

See [data-model.md](data-model.md), [quickstart.md](quickstart.md), and contracts under [contracts/](contracts/).

## Measurement Gate And P0/P1/P2 Implementation Order

### Hard Diagnostic Gate

No behavior-changing optimization may be accepted until these are complete:

1. Required prefab and thumbnail stages are emitted.
2. Required counters include queue/backlog, in-flight priority counts, cancellation latency, and coalescing pressure.
3. A baseline report exists for all MVP scenarios.
4. Missing-baseline and mismatched-scenario comparison tests reject improvement claims.
5. The diagnostic-only tests pass without changing prefab or thumbnail behavior beyond measurement output.

Each later optimization must name the measured bottleneck it targets and must compare against the baseline for the same scenario.

### P0

P0 is not a bundle of simultaneous changes. It is an ordered measurement-driven queue:

1. Target the single largest measured editor-main-thread stall first.
2. If GPU fence wait is the largest measured stall, introduce a non-blocking preview readback path.
3. If duplicate thumbnail work is the largest measured scheduler problem, strengthen request deduplication, cancellation, and visible-item prioritization.
4. If preview setup/recreate cost is the largest measured thumbnail cost, reuse preview scene/render targets where compatible.
5. If full prefab preview instantiation is the largest measured preview cost, introduce lightweight preview renderable data.
6. If prefab/resource waiting is the largest measured prefab cost, decouple prefab graph creation from nonessential resource readiness.

P0 thumbnail work uses the active `ThumbnailGenerationBudget` as the configuration authority. Tests inject deterministic values for CPU prepare time, render count, GPU upload bytes, readback count, and cache write count; editor defaults are introduced only through that configuration object.

### P1

1. Batch prefab allocation, component creation, property restore, internal fixup, external handle binding, system registration, and deferred activation.
2. Build `PreviewRenderableSnapshot` or equivalent lightweight preview draw items.
3. Move thumbnail readback, encode, and disk writes to asynchronous or budgeted execution.
4. Coalesce mesh/material/texture/shader resource requests uniformly and track resource states.

### P2

1. Introduce a versioned binary prepared prefab artifact while retaining optional JSON debug export.
2. Optimize nested prefab resolved templates or compact override patch handling.
3. Complete cache capacity/eviction policies across prepared prefab, runtime resource, preview snapshot, thumbnail texture, and thumbnail disk caches.

## Cache Ownership Boundaries

- Prepared prefab cache owns resolved prefab graph data, dependency manifests, and prepared prefab freshness.
- Runtime resource cache owns mesh/material/texture/shader resource states and immutable runtime resource sharing.
- Preview snapshot cache owns bounds and draw-item summaries derived from prepared data for preview only.
- Thumbnail texture cache owns final in-memory thumbnail display textures.
- Thumbnail disk cache owns final cross-session thumbnail image files and metadata.

Cross-role reuse is only allowed through explicit versioned handoff:

- Thumbnail preview may read prepared prefab data but may not store or serve runtime prefab scene instances.
- Preview snapshot may reference runtime resource handles but may not become the source of truth for resource freshness.
- Thumbnail caches may never satisfy prefab instantiation requests.
- Prepared prefab cache may not serve final thumbnail images or preview texture lifetimes.

## Complexity Tracking

No constitution violations are required. This feature is broad, but the plan keeps implementation staged and requires diagnostic evidence before each optimization claim.

## Constitution Check (Post-Design)

- **Spec-first major change**: PASS. All design artifacts live under `specs/053-prefab-thumbnail-performance/`.
- **Validation matches subsystem**: PASS. Diagnostics and unit/benchmark tests are the first evidence; GPU wait removal requires focused RHI/readback validation.
- **Generated code and backend boundaries**: PASS. Generated files remain untouched; backend work remains behind existing RHI abstractions.
- **Incremental, verified delivery**: PASS. Tasks will deliver an independently testable Phase 1 diagnostics increment before P0 optimization work.
- **Product runtime preservation**: PASS. Async thumbnail behavior must preserve immediate placeholder/cached UI behavior and safe shutdown.
