# Phase 1 Diagnostic Report: Prefab and Thumbnail Performance

Date: 2026-06-18

This report records the first measured diagnostic baseline. It is intentionally limited to the currently instrumented unit scenarios and must not be treated as proof that any optimization has improved runtime/editor performance yet.

## Current Real Calling Chain

### Prefab Artifact And Instantiation

1. Editor and asset-browser callers request prefab data through `AssetDatabaseFacade::LoadPrefabArtifactAtPath` or `AssetDatabaseFacade::LoadPrefabArtifactByAssetId`.
2. `AssetDatabaseFacade` reads the native prefab/model artifact container and emits `LoadPrefabArtifact`.
3. `ImportPrefabArtifact` in `Runtime/Engine/Assets/PrefabAsset.cpp` reads the serialized prefab graph and emits `ParsePreparedPrefab`.
4. `RefreshPrefabResolvedAssetsFromReferences` emits `ResolveDependencies` while rebuilding resolved external asset data from serialized references.
5. `InstantiatePrefabArtifact` validates the artifact, builds a runtime-resolved document, emits `ResolveExternalReferences`, and only emits `WaitForResources` / `UploadGpuResources` when a caller explicitly opts into synchronous asset-reference prewarm.
6. `ObjectGraphInstantiator::InstantiatePrefab` performs the object-graph work:
   - `ResolveDependencies`
   - `AllocateInstanceObjects`
   - `RestoreGameObjectState`
   - `CreateComponents`
   - `DeserializeComponents`
   - `BindExternalAssetReferences`
   - `FixupInternalReferences`
   - `RegisterRenderers`
   - `RegisterPhysics`
   - `RegisterScripts`
   - `InvokeLifecycle`

### Prepared Prefab Cache

1. Drag/drop and imported-model prefab paths enter `EditorAssetDragDropBridge`.
2. Prepared cache lookup under `Library/PreparedPrefabCache/*.json` is instrumented as `LoadPreparedPrefabCache`.
3. Successful prepared-cache writes are instrumented as `StorePreparedPrefabCache`.
4. Current finding: this prepared-cache path is still scoped to the drag/drop/imported prefab bridge. The general `AssetDatabaseFacade` prefab artifact load path and thumbnail preview path still need stronger evidence before the cache is generalized into a shared read-through prepared-data service.

### Thumbnail Generation

1. `AssetThumbnailService::GetThumbnail` evaluates the cache and queues missing work. It emits `ThumbnailCacheLookup`.
2. `AssetThumbnailService::GenerateNextThumbnail` emits `TotalThumbnail`, pops one queued request, re-evaluates freshness, and either runs a CPU thumbnail path or asks `EditorThumbnailPreviewRenderer` for GPU previews.
3. CPU texture thumbnails run through `TryGenerateThumbnailForRequest`, `WriteRgbaThumbnailResult`, `EncodeThumbnailPng`, cache metadata/image writes, `EncodePreview`, and `StorePreviewCache`.
4. GPU preview thumbnails run through `EditorThumbnailPreviewRenderer::Render`:
   - `PreparePreviewAsset`
   - `CreatePreviewInstance`
   - `CalculateBounds`
   - `PreparePreviewResources`
   - `RecordPreviewRender`
   - `SubmitPreviewRender`
   - `ReadbackPreview`
5. `RhiThreadCoordinator::ReadPixelsChecked` emits `WaitPreviewFence` around synchronous readback completion waits.

## Stage To Source Mapping

| Stage | Source file/function |
| --- | --- |
| `LoadPrefabArtifact` | `Project/Editor/Assets/AssetDatabaseFacade.cpp`, `AssetDatabaseFacade::LoadPrefabArtifactAtPath`, `AssetDatabaseFacade::LoadPrefabArtifactByAssetId` |
| `LoadPreparedPrefabCache` | `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, prepared prefab cache lookup |
| `StorePreparedPrefabCache` | `Project/Editor/Assets/EditorAssetDragDropBridge.cpp`, prepared prefab cache publish |
| `ParsePreparedPrefab` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `ImportPrefabArtifact` |
| `ResolveDependencies` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `RefreshPrefabResolvedAssetsFromReferences`; `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, `ObjectGraphInstantiator::InstantiatePrefab` |
| `TotalInstantiate` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `InstantiatePrefabArtifact` |
| `WaitForResources` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `InstantiatePrefabArtifact` |
| `UploadGpuResources` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `PrewarmPrefabMeshArtifacts`, `PrewarmPrefabMaterialArtifacts` |
| `ResolveExternalReferences` | `Runtime/Engine/Assets/PrefabAsset.cpp`, `BuildRuntimeResolvedGraph` call site in `InstantiatePrefabArtifact` |
| `AllocateInstanceObjects` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, object allocation phase |
| `RestoreGameObjectState` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, GameObject state restore before component creation |
| `CreateComponents` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, component creation/order phase |
| `DeserializeComponents` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, GameObject/component property restore phase |
| `BindExternalAssetReferences` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, cached mesh/material binding and deferred external asset reference hints |
| `FixupInternalReferences` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, internal parent/reference fixup phase |
| `RegisterRenderers` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, renderer count and scene registration envelope |
| `RegisterPhysics` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, physics registration phase |
| `RegisterScripts` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, script registration phase |
| `InvokeLifecycle` | `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, lifecycle activation phase |
| `ThumbnailCacheLookup` | `Project/Editor/Assets/AssetThumbnailService.cpp`, `AssetThumbnailService::GetThumbnail` |
| `TotalThumbnail` | `Project/Editor/Assets/AssetThumbnailService.cpp`, `AssetThumbnailService::GenerateNextThumbnail` |
| `PreparePreviewAsset` | `Project/Editor/Assets/AssetThumbnailService.cpp`, GPU preview render call site |
| `EncodePreview` | `Project/Editor/Assets/AssetThumbnailService.cpp`, `WriteThumbnailPngResult` / `EncodeThumbnailPng` call site |
| `StorePreviewCache` | `Project/Editor/Assets/AssetThumbnailService.cpp`, thumbnail cache write call site |
| `CreatePreviewInstance` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, prefab preview loading/instantiation |
| `CalculateBounds` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, prefab/mesh preview bounds calculation |
| `PreparePreviewResources` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, mesh/material prewarm and preview object setup |
| `RecordPreviewRender` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, preview render recording |
| `SubmitPreviewRender` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, preview render submission |
| `ReadbackPreview` | `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`, preview pixel readback |
| `WaitPreviewFence` | `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`, `RhiThreadCoordinator::ReadPixelsChecked` |

## Profiling Data

Report output was generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\performance-reports\053-phase1-broad'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetThumbnailPerformanceTests.*:PerformanceStageStatsTests.*:AssetThumbnailCacheTests.*:GameObjectAssetImportTests.UnifiedPrefabSharedHotCacheReusesPrefabGraphWithoutCopyingOnWarmPreview:GameObjectAssetImportTests.UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps
```

### Prefab: `Prefab_ImportAndInstantiate` Pre-T026 Baseline

This sample was captured before the T026 resource-prewarm decoupling change, so it still contains the old default `WaitForResources` / `UploadGpuResources` stages. Current default instantiation behavior is documented in the synchronous resource prewarm decoupling section below.

- Scenario total wall time: 27,301 us
- `TotalInstantiate`: 1,698 us, 1 call, main thread
- `ParsePreparedPrefab`: 1,242 us, 1 call, main thread
- `DeserializeComponents`: 648 us, 1 call, main thread, `componentCount=2`
- `ResolveDependencies`: 92 us, 2 calls, main thread
- `AllocateInstanceObjects`: 85 us, 1 call, main thread
- `RegisterRenderers`: 78 us, 1 call, main thread
- `ResolveExternalReferences`: 46 us, 1 call, main thread
- `WaitForResources`: 16 us, 1 call, main thread
- `UploadGpuResources`: 8 us, 1 call, main thread

Counters captured in this minimal scenario:

- `objectCount=3` at `TotalInstantiate`
- `componentCount=2` at `DeserializeComponents`
- `dependencyCount=0` at dependency/resource stages

### Prefab Matrix: Small, 100 Objects, 1000 Objects, Multi Renderer

Additional prefab reports were generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\performance-reports\053-phase1-prefab-matrix'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*
```

Key results:

| Scenario | Wall time | Top concrete stage | Stage time | Counters |
| --- | ---: | --- | ---: | --- |
| `Prefab_100Objects` | 110,516 us | `RegisterRenderers` | 72,477 us | `objectCount=100`, `componentCount=101`, `rendererCount=0`, `SceneRebuildFastAccess.rebuildCount=102` |
| `Prefab_1000Objects` | 6,292,612 us | `RegisterRenderers` | 5,855,499 us | `objectCount=1000`, `componentCount=1001`, `rendererCount=0`, `SceneRebuildFastAccess.rebuildCount=1002` |
| `Prefab_MultiRenderer` | 107,412 us | `RegisterRenderers` | 71,458 us | `objectCount=64`, `componentCount=127`, `rendererCount=31`, `SceneRebuildFastAccess.rebuildCount=128` |

Important interpretation: the current `RegisterRenderers` stage wraps `Scene::AddGameObject(result.root)`, so it measures full recursive scene insertion/component registration work, not only mesh renderer registration. The 1000-object scenario reports `rendererCount=0` but still spends 5.86 s in this stage. `SceneRebuildFastAccess` accounts for 5.81 s across 1002 rebuilds and a cumulative `trackedObjectCount=501501`, which points to repeated fast-access cache rebuild as the concrete O(N^2)-shaped bottleneck to address before changing JSON or resource caches.

### Prefab: Batch Phase Separation Follow-Up

`ObjectGraphInstantiator::InstantiatePrefab` now exposes separate batch stages for object allocation, component creation, property population, external cached resource binding, internal parent fixup, and registration. The change intentionally preserves the existing reflection and resource-binding semantics while making the hot path more measurable:

- `AllocateInstanceObjects` records `reservedObjectCount`.
- `RestoreGameObjectState` records `restoredGameObjectCount` before component creation so component `OnCreate` observes restored owner state.
- `CreateComponents` records `createdComponentCount`, `componentRecordCount`, and stage-local indexed/linear lookup counts.
- `DeserializeComponents` records `restoredGameObjectCount`, `restoredComponentCount`, and stage-local indexed/linear lookup counts.
- `BindExternalAssetReferences` records `componentCount`, property-level `assetReferenceBindingCount`, and element-level `assetReferenceElementBindingCount`.
- `FixupInternalReferences` records `parentFixupCount`.
- `RegisterRenderers` records `objectCount`, `componentCount`, and `rendererCount`.

Fresh validation was generated with:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetPrefabPipelineTests.*:PrefabObjectGraphSerializationTests.*:SceneObjectGraphSerializationTests.*:DebugSceneLifecycleTests.*:PrefabEditorWorkflowTests.*
```

Result: build succeeded and 144 prefab/scene/lifecycle tests passed. The follow-up run is compatibility evidence for the phase split; it is not yet a before/after performance improvement claim.

Follow-up review tightened the diagnostics and semantics coverage:

- `CreateComponents` and `DeserializeComponents` now report lookup-count deltas for their own phase instead of cumulative context totals.
- `RegisterComponentMappings` reuses `context.components` instead of re-reading component object records, avoiding duplicate indexed lookups after the populate pass.
- `PrefabObjectGraphSerializationTests.BatchExternalAssetBindingPreservesImmediateAndDeferredResourceReferences` directly verifies immediate and deferred external mesh/material binding preserve PPtr identity, path hints, and cached runtime resource resolution after the batched binding split.
- `PrefabObjectGraphSerializationTests.ComponentOnCreateObservesRestoredGameObjectStateDuringPrefabInstantiation` verifies component creation/lifecycle ordering still lets `OnCreate` observe serialized GameObject state such as active and source-object identity.

Fresh validation after the follow-up:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetPrefabPipelineTests.*:PrefabObjectGraphSerializationTests.*:SceneObjectGraphSerializationTests.*:DebugSceneLifecycleTests.*:PrefabEditorWorkflowTests.*
```

Result: build succeeded and 146 prefab/scene/lifecycle tests passed. The test process still prints the existing `Shaders/Skybox.hlsl` read warning during one no-RHI scene test, but exits successfully.

### Prefab: Synchronous Resource Prewarm Decoupling

`LoadPolicy` now keeps prefab graph creation decoupled from nonessential mesh/material readiness by default. `InstantiatePrefabArtifact` no longer enters the synchronous mesh/material prewarm branch unless `LoadPolicy::synchronousAssetReferencePrewarm` is explicitly enabled. The object graph still restores serialized PPtr identity and the batched external-reference phase still binds already cached mesh/material resources when they are available; it does not force a load or GPU upload on the default instantiation path.

Fresh regression coverage:

- `AssetPrefabPerformanceTests.PrefabInstantiationDoesNotSynchronouslyPrewarmResourcesByDefault` verifies resolved mesh/material dependencies do not cause `WaitForResources` or `UploadGpuResources` stages on the default path.
- `AssetPrefabPerformanceTests.ExplicitSynchronousResourcePrewarmRetainsDiagnosticStages` verifies explicit opt-in retains the old diagnostic stages, `dependencyCount` accounting, and `synchronousResourceLoadCount` when a mesh manager service is present.
- `AssetPrefabPerformanceTests.DeferredAssetResolutionSuppressesSynchronousPrewarmOptIn` verifies `deferAssetReferenceResolution` takes precedence over synchronous prewarm opt-in, protecting thumbnail/preview policies from accidental blocking.
- Existing prefab object-graph tests continue to verify immediate and deferred cached resource binding semantics.

Fresh validation was generated with:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.PrefabImportAndInstantiateEmitDiagnosticStages:AssetPrefabPerformanceTests.PrefabInstantiationDoesNotSynchronouslyPrewarmResourcesByDefault:AssetPrefabPerformanceTests.ExplicitSynchronousResourcePrewarmRetainsDiagnosticStages:AssetPrefabPerformanceTests.DeferredAssetResolutionSuppressesSynchronousPrewarmOptIn
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetPrefabPipelineTests.*:PrefabObjectGraphSerializationTests.*:SceneObjectGraphSerializationTests.*:DebugSceneLifecycleTests.*:PrefabEditorWorkflowTests.*
```

Result: build succeeded; the focused resource-prewarm tests passed; the broader prefab/scene/lifecycle regression group passed 148 tests. The test process still prints the existing `Shaders/Skybox.hlsl` read warning during one no-RHI scene test, but exits successfully.

### Prefab: US2 Optimized Validation (T027)

The US2 prefab validation pass was rerun after the batch instantiation, deferred activation, cached external-resource binding, direct simple-property apply, and default resource-prewarm decoupling changes. Reports were written under `Build\perf-053-us2-t027`.

Fresh commands:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
$env:NLS_PERFORMANCE_REPORT_DIR='Build\perf-053-us2-t027'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.*:PrefabObjectGraphSerializationTests.*:SceneObjectGraphSerializationTests.*:DebugSceneLifecycleTests.*:PrefabEditorWorkflowTests.*
```

Results:

- Build succeeded.
- `AssetPrefabPerformanceTests.*`: 14 tests passed and emitted prefab reports.
- Prefab semantic/editor/scene regression group: 135 tests passed. The existing no-RHI `Shaders/Skybox.hlsl` warning was printed by one scene test, but the process exited successfully.

Measured same-scenario comparison against earlier baseline samples:

| Scenario | Earlier baseline | Current T027 run | Evidence |
| --- | ---: | ---: | --- |
| `Prefab_100Objects` | 110,516 us wall time; `RegisterRenderers` 72,477 us; `SceneRebuildFastAccess.rebuildCount=102` | 37,425 us wall time; `TotalInstantiate=37,358 us`; `SceneRebuildFastAccess.rebuildCount=2`; `linearRecordLookupCount=0` | Recursive scene registration rebuilds are no longer the dominant O(N^2)-shaped cost. |
| `Prefab_1000Objects` | 6,292,612 us wall time; `RegisterRenderers` 5,855,499 us; `SceneRebuildFastAccess.rebuildCount=1002` | 362,298 us wall time; `TotalInstantiate=361,916 us`; `SceneRebuildFastAccess.rebuildCount=2`; `linearRecordLookupCount=0` | Large-prefab hot path now spends most time in component/property restoration instead of repeated scene-cache rebuilds. |
| `Prefab_ImportAndInstantiate` | Pre-T026 sample contained default `WaitForResources=16 us` and `UploadGpuResources=8 us` | Current default report contains no `WaitForResources` or `UploadGpuResources`; explicit prewarm remains opt-in via test coverage | Default prefab graph creation no longer reports synchronous resource prewarm stages. |

Current `Prefab_1000Objects` top concrete stages:

1. `DeserializeComponents`: 204,126 us
2. `DeserializePropertyValue`: 134,544 us
3. `RestoreGameObjectState`: 32,871 us
4. `ResolveExternalReferences`: 24,478 us
5. `ApplyDirectPropertyValue`: 14,822 us

Interpretation: US2 has measurable relative improvement for the repeated large-prefab bottleneck while preserving prefab semantics. Remaining prefab cost is now dominated by reflected component/property restoration; further optimization should not return to broad caching without a new focused measurement.

### Thumbnail: `Thumbnail_TextureFirstGeneration`

- Scenario total wall time: 38,674 us
- `TotalThumbnail`: 29,809 us, 1 call, main thread, `thumbnailsGeneratedThisFrame=1`
- `StorePreviewCache`: 10,387 us, 1 call, main thread, `cacheWriteCount=1`, `storedByteCount=70`
- `ThumbnailCacheLookup`: 8,759 us, 2 calls, main thread, `duplicateThumbnailRequestCount=1`, `queueDepth` reports the maximum observed queued-key depth instead of summing samples
- `EncodePreview`: 60 us, 1 call, main thread, `encodedByteCount=70`

This scenario exercises the CPU texture thumbnail path. It does not yet exercise the GPU preview path, so `CreatePreviewInstance`, `RecordPreviewRender`, `SubmitPreviewRender`, `WaitPreviewFence`, and `ReadbackPreview` are instrumented but not represented in the reported bottleneck ranking above.

### Thumbnail: `Thumbnail_DiskCacheHit`

Additional thumbnail cache reports were generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\performance-reports\053-phase1-thumbnail-cache'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*
```

Disk-cache hit result:

- Scenario total wall time: 2,181 us
- `ThumbnailCacheLookup`: 2,162 us, 1 call, main thread, `cacheHitCount=1`
- No `TotalThumbnail`, `EncodePreview`, or `StorePreviewCache` stage is emitted on the disk-hit path.

### Thumbnail: Memory Cache Hit And Rapid Scroll Deduplication

Additional thumbnail benchmark coverage now exercises:

- `Thumbnail_MemoryCacheHit`: same-service lookup after first generation returns `Fresh` from cache lookup without emitting `TotalThumbnail`, `EncodePreview`, or `StorePreviewCache`.
- `Thumbnail_RapidScrollDeduplication`: 500 simulated Project Browser requests over 32 unique texture keys with `cacheWriteCountBudget=0` report duplicate/coalesced work while leaving queued work pending. This covers deduplication/backlog, not true cancellation.
- `Thumbnail_ReportCounters`: report text includes the deterministic `TopBottlenecks` section plus per-frame count, duplicate request, queue depth/backlog, in-flight count, cancellation latency, coalescing pressure, and fence-wait counters.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.RapidScrollDuplicateRequestsReportBacklogAndCoalescingPressure:AssetThumbnailPerformanceTests.ThumbnailReportIncludesTopFiveAndSchedulerCounters:AssetThumbnailPerformanceTests.MemoryCacheHitReportsLookupHitWithoutRegeneration
```

Result: 3 tests passed.

Rapid-scroll counters in the deterministic fixture include:

- `duplicateThumbnailRequestCount=468`
- `coalescingPressure=468`
- `queueDepth=32` as the peak unique queued-key depth
- `queueBacklog=32`
- `inFlightRequestCount=0`
- `cacheWriteBudgetRemaining=0`

Report-contract counters in the deterministic fixture include:

- `thumbnailsGeneratedThisFrame=1`
- `duplicateThumbnailRequestCount=5`
- `queueDepth=8`
- `queueBacklog=12`
- `inFlightRequestCount=3`
- `cancellationLatency=25`
- `coalescingPressure=4`
- `fenceWaitTime=500`

### Thumbnail: Queued Cancellation Diagnostics

`AssetThumbnailService::SupersedeQueuedRequestsForGeneration` now emits a real cancellation diagnostic sample when it clears queued thumbnail work for a newer generation. The deterministic fixture queues 5 unique texture thumbnails, supersedes the visible-range generation before work starts, and verifies:

- queued work is cleared without generating thumbnails
- `queueBacklog=5`
- `cancelledThumbnailRequestCount=5`
- `cancellationLatency` is present and records the measured queued-clear duration in microseconds

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.SupersededQueuedRequestsReportCancellationDiagnostics
```

Result: 1 test passed.

### Thumbnail: Budget Injection And Deferred Cache Writes

`AssetThumbnailService` now exposes a small `ThumbnailGenerationBudget` configuration object for deterministic tests. The first enforced budget is `cacheWriteCountBudget`: when it is zero, `GenerateNextThumbnail()` records backlog/budget counters and leaves queued work pending instead of encoding or writing a thumbnail on that frame. The asynchronous `StartNextThumbnailGeneration()` path now uses the same zero-write-budget gate before taking a request out of the queue or starting a worker.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.ZeroCacheWriteBudgetDoesNotStartAsyncTextureThumbnail:AssetThumbnailPerformanceTests.ZeroCacheWriteBudgetDefersQueuedTextureThumbnail
```

Result: 2 tests passed.

Fresh follow-up validation after adding RHI fence-wait coverage and the completed thumbnail report-contract assertions:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
```

Result: exit code 0.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*:PerformanceStageStatsTests.*
```

Result: 17 tests passed.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.DriverReadPixelsCheckedReportsWaitPreviewFenceStage:ThreadedRenderingLifecycleTests.DriverReadPixelsCheckedPropagatesExplicitReadbackFailure
```

Result: 2 tests passed.

The tests verify:

- `queueBacklog=1`
- `cacheWriteBudgetRemaining=0`
- queued request remains queued
- no async in-flight thumbnail request is started
- no `EncodePreview` stage is emitted
- no `StorePreviewCache` stage is emitted

This does not complete the full thumbnail scheduler contract yet. CPU prepare time, preview render count, GPU upload bytes, readback count, and full priority/backlog enforcement still need broader tests and implementation before claiming non-blocking thumbnail generation is solved.

Current task status note: the rapid-scroll fixture validates deduplication/backlog counters and the supersede fixture validates queued cancellation latency, so `T013` is covered for CPU scheduler scenarios. `T015` is covered at the report-contract level by deterministic thumbnail report data that includes per-frame counts, duplicate requests, queue depth/backlog, in-flight count, cancellation latency, coalescing pressure, fence wait time, and top-five ranking. GPU-preview rapid-scroll benchmark coverage remains a later scenario gap rather than a report-format gap.

### Thumbnail: RHI Fence Wait Accounting

`RhiThreadCoordinator::ReadPixelsChecked` is now covered by a focused RHI readback regression test. The test drives the real `DriverRendererAccess::ReadPixelsChecked` path with the existing fake explicit device and a completion token, captures `PerformanceStageStats`, and asserts that `WaitPreviewFence` is emitted in the `Thumbnail` domain with one main-thread call.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.DriverReadPixelsCheckedReportsWaitPreviewFenceStage:ThreadedRenderingLifecycleTests.DriverReadPixelsCheckedPropagatesExplicitReadbackFailure
```

Result: 2 tests passed.

This proves fence-wait accounting at the readback completion boundary. It does not claim the thumbnail GPU preview pipeline is non-blocking; `T031` remains the later behavior-changing task for asynchronous fence polling.

### Thumbnail: ThumbnailKey Cache Identity

`AssetThumbnailRequest` now carries `previewRendererVersion`, `dependencyStamp`, `colorSpaceMode`, and `hdrMode` separately from `settingsFingerprint`, and `BuildAssetThumbnailCacheKey` / `BuildAssetThumbnailStablePathKey` include them. This keeps renderer implementation, dependency freshness, color-space, HDR, settings, size, asset identity, artifact path, and existing freshness changes from reusing stale thumbnails through an ambiguous key. Manifest-resolved artifact paths now also contribute `artifact-file` freshness, so source model/prefab thumbnails discovered through `manifest.json` do not miss the actual prepared artifact file stamp.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.CacheKeyInvalidatesWhenDependencyColorSpaceOrHdrModeChanges:AssetThumbnailCacheTests.CacheKeyInvalidatesWhenPreviewRendererVersionChanges:AssetThumbnailCacheTests.CacheKeyIncludesAssetIdentitySubAssetFreshnessAndSettings:AssetThumbnailCacheTests.ServiceBuildsRequestsFromSourceAndGeneratedItems:AssetThumbnailCacheTests.ServiceBuildsSourceModelPrefabPreviewRequestFromManifest
```

Result: 5 tests passed.

### PreparedPrefab: Explicit Freshness Identity

`UnifiedPrefabLoadKey` now carries an explicit prepared-prefab freshness record in addition to the existing runtime cache identity. Persistent prepared prefab JSON entries are accepted only when all of these fields match the current key:

- cache schema version
- runtime cache identity
- manifest stamp
- dependency stamp
- prefab artifact stamp
- renderer artifact stamp
- prefab importer version
- reflection schema version
- serialization format version
- dependency manifest version

Model texture mapping dependencies now use a bounded stamped fingerprint cache before prepared-prefab freshness decisions. When `Library/ArtifactDB/index.tsv` contains matching up-to-date texture artifact records, the fingerprint is built from that index without recursively scanning `Assets`. If the ArtifactDB is present but does not yet include the external texture source needed by a `source-path` dependency, the bridge loads only the exact source `.meta` and texture manifest instead of treating the missing index row as an empty candidate set. The stamped cache key includes the exact source file, `.meta`, ArtifactDB index, and fallback texture manifest stamps, so same-session freshness checks do not reuse a stale texture fingerprint after the texture artifact manifest changes. This preserves L2 cache correctness for imported model prefabs while avoiding the previous full-project recursion on prepared-cache hit paths.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.PreparedPrefabFreshnessRejectsStaleKeyVersionsAndStamps:GameObjectAssetImportTests.GeneratedModelPrefabLoadPersistsPreparedGraphCacheAcrossEditorSession:GameObjectAssetImportTests.PreparedGraphCachePersistsRendererDependencyTemplatesAcrossEditorSession:GameObjectAssetImportTests.UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps
```

Result: 4 tests passed.

A broader prefab pipeline/cache regression pass was also generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.*:GameObjectAssetImportTests.GeneratedModelPrefabLoadPersistsPreparedGraphCacheAcrossEditorSession:GameObjectAssetImportTests.PreparedGraphCachePersistsRendererDependencyTemplatesAcrossEditorSession:GameObjectAssetImportTests.UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps:GameObjectAssetImportTests.GraphOnlySceneRestoreKeySkipsRendererArtifactFileStamps
```

Result: 29 tests passed.

Fresh follow-up validation after the ArtifactDB-backed fingerprint cache fix:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.PreparedPrefabFreshnessRejectsStaleKeyVersionsAndStamps:AssetPrefabPipelineTests.PreparedPrefabMappingDependencyFingerprintReusesStampedCache
```

Result: 2 tests passed.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.PreparedPrefabSourcePathMappingFallbackTracksTextureManifestStamp
```

Result: 1 test passed after first observing the expected red failure when the fallback texture manifest stamp was absent from the cache key.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.*:AssetDatabaseFacadeTests.ArtifactManifestCurrentRejectsStaleModelTextureNameSearchCandidateSet:AssetDatabaseFacadeTests.ArtifactManifestCurrentTracksCaseInsensitiveModelTextureNameSearchCandidates:AssetDatabaseFacadeTests.ArtifactManifestCurrentRejectsMissingModelTextureSourcePathArtifact:GameObjectAssetImportTests.GeneratedModelPrefabLoadPersistsPreparedGraphCacheAcrossEditorSession:GameObjectAssetImportTests.PreparedGraphCachePersistsRendererDependencyTemplatesAcrossEditorSession:GameObjectAssetImportTests.UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps:GameObjectAssetImportTests.GraphOnlySceneRestoreKeySkipsRendererArtifactFileStamps
```

Result: 31 tests passed.

## Bottleneck Ranking

### Current Measured Prefab Ranking

1. `TotalInstantiate` - wrapper total for the measured instantiate call.
2. `RegisterRenderers` - in large scenarios this is the largest concrete stage; it currently represents full `Scene::AddGameObject` recursive insertion and component registration, not just renderer registration.
3. `SceneRebuildFastAccess` - measured separately inside scene registration; 1000 objects trigger 1002 rebuilds and 5.81 s of cumulative rebuild time.
4. `DeserializeComponents` - component creation/property restore is the next-largest large-prefab stage.
5. `ResolveExternalReferences` / `AllocateInstanceObjects` / `ResolveDependencies` - measurable at 1000 objects but far below repeated fast-access rebuild in the current baseline.

### Incremental Optimization: Scene Fast-Access Rebuild Deferral

After the first bottleneck ranking identified repeated `Scene::RebuildFastAccessComponents()` as the largest concrete large-prefab stall, `Scene::AddGameObject` now defers fast-access rebuild requests while recursively registering a GameObject subtree and flushes the pending rebuild once at the end. The behavior-facing compatibility check verifies that deferred prefab scene registration still populates `Scene::GetFastAccessComponents().modelRenderers`.

Fresh report output was generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\perf-053-current'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters
```

Measured comparison against the original Phase 1 prefab matrix:

| Scenario | Original wall time | Current wall time | Original `SceneRebuildFastAccess` | Current `SceneRebuildFastAccess` | New top concrete stage |
| --- | ---: | ---: | ---: | ---: | --- |
| `Prefab_100Objects` | 110,516 us | 38,894 us | 102 calls | 2 calls, 1,259 us | `DeserializeComponents` |
| `Prefab_1000Objects` | 6,292,612 us | 444,902 us | 1002 calls, 5,810,000+ us | 2 calls, 11,745 us | `DeserializeComponents` |

The optimization does not claim to solve the remaining prefab cost. The current 1000-object ranking is now:

1. `DeserializeComponents`: 315,862 us
2. `ResolveExternalReferences`: 24,266 us
3. `ResolveDependencies`: 14,809 us
4. `RegisterRenderers`: 14,044 us
5. `AllocateInstanceObjects`: 13,796 us

This shifts the next evidence-backed prefab work away from scene fast-access rebuilds and toward component/property restoration and reference-resolution overhead.

### Incremental Optimization: Object Record Lookup Index

The next measured hotspot was `DeserializeComponents`. Inspection showed that component restoration and prefab source-to-instance component mapping repeatedly resolved component object ids through linear `FindRecord(document, id)` scans. `ObjectGraphInstantiator::InstanceContext` now builds one `ObjectId -> ObjectRecord*` index per instantiation and the component restore/mapping paths use that index. The diagnostic counters distinguish indexed lookups from linear fallbacks.

Fresh report output was generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\perf-053-current2'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters:AssetPrefabPerformanceTests.LargePrefabComponentRestoreUsesIndexedRecordLookup
```

Measured comparison against the prior fast-access-deferral run:

| Scenario | Prior wall time | Current wall time | Prior `DeserializeComponents` | Current `DeserializeComponents` | Lookup counters |
| --- | ---: | ---: | ---: | ---: | --- |
| `Prefab_100Objects` | 38,894 us | 38,476 us | 25,097 us | 22,985 us | `indexedRecordLookupCount=202`, `linearRecordLookupCount=0` |
| `Prefab_1000Objects` | 444,902 us | 357,859 us | 315,862 us | 228,689 us | `indexedRecordLookupCount=2002`, `linearRecordLookupCount=0` |

The current 1000-object ranking is now:

1. `DeserializeComponents`: 228,689 us
2. `ResolveExternalReferences`: 24,872 us
3. `RegisterRenderers`: 15,449 us
4. `ResolveDependencies`: 13,966 us
5. `AllocateInstanceObjects`: 13,942 us

`DeserializeComponents` remains the largest concrete stage, but the eliminated linear record scan moved approximately 87 ms out of the 1000-object fixture. The next evidence-backed work should inspect reflection field lookup/deserialization inside `ApplyReflectedFields` before adding broader prepared-binary artifacts.

### Field Lookup Cache Attempt And Rejection

An instance-scoped reflected-field cache was prototyped and measured. The counters showed high hit rates, but the end-to-end timings regressed:

- `Prefab_1000Objects` rose from roughly `357.9 ms` to roughly `383.5 ms`
- `DeserializeComponents` rose from roughly `228.7 ms` to roughly `248.3 ms`

That cache was removed after measurement. The useful outcome is negative evidence: caching `Type.GetField(property.name)` in this shape does not help this workload on the current code path, so the next iteration should focus on reducing reflection/property work itself rather than layering another lookup table on top.

### Incremental Optimization: Direct Simple Property Apply

Reflection substage diagnostics showed that primitive/string property restoration spent most of its measured time in the JSON conversion and `Type::DeserializeJson` path. `ObjectGraphInstantiator::ApplyReflectedFields` now attempts a narrow fast path for exact simple values after object-reference handling and before JSON conversion:

- `bool`
- `int`
- `float`
- `double`
- `std::string`

The fast path preserves the existing reflection setter path and leaves arrays, objects, enums, unsigned integers, Unity-style object references, deferred asset references, and shape-mismatched references on the existing deserialize path.

Fresh report output was generated with:

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\perf-053-direct'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters:AssetPrefabPerformanceTests.LargePrefabComponentRestoreAppliesSimplePropertiesDirectly
```

Measured comparison against the pre-direct reflection-substage run:

| Scenario | Prior `DeserializePropertyValue` | Current `DeserializePropertyValue` | Current `ApplyDirectPropertyValue` | Property-path counters |
| --- | ---: | ---: | ---: | --- |
| `Prefab_1000Objects` | ~165,777 us, `propertyCount=7010` | 158,188 us, `propertyCount=3003` | 19,394 us, `propertyCount=4007` | 4007 simple properties bypassed JSON deserialize; 3003 properties remain on the generic path |

The current `Prefab_1000Objects` run reported:

1. `DeserializeComponents`: 296,346 us
2. `DeserializePropertyValue`: 158,188 us
3. `RegisterRenderers`: 29,093 us
4. `ResolveExternalReferences`: 25,184 us
5. `ApplyDirectPropertyValue`: 19,394 us

Interpretation: this is useful because it removes 4007 primitive/string properties from the JSON deserialize path without adding a cache or changing cache invalidation. The end-to-end total remains noisy across runs, so the next prefab optimization should continue to be driven by stage-level counters and repeated same-scenario comparisons rather than a single absolute wall-clock value.

### Prefab: Report Comparison And Top-Five Assertions

The prefab performance tests now assert that formatted benchmark reports include deterministic `TopBottlenecks` output and that baseline/optimized comparison reports expose the scenario, percent change, baseline total, and optimized total fields. This keeps later optimization claims tied to report data rather than source-text inspection or ad hoc console output.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.PrefabReportIncludesTopFiveAndComparisonOutput:AssetPrefabPerformanceTests.MissingBaselineAndMismatchedScenarioComparisonsAreInvalid
```

Result: 2 tests passed.

### Prefab: Repeated Instantiation Hot-Path Guard

`AssetPrefabPipelineTests.RepeatedPrefabInstantiationKeepsHotPathIndexedAndInstancesIndependent` now covers the first US2 repeated-instantiation regression guard. It instantiates the same prefab artifact twice with `LoadPolicy::suppressGameObjectCreatedEvents`, verifies the two runtime graphs are distinct while preserving names/hierarchy/source mappings, and asserts `DeserializeComponents` uses indexed object-record lookups with zero linear lookup fallbacks.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.RepeatedPrefabInstantiationKeepsHotPathIndexedAndInstancesIndependent:AssetPrefabPipelineTests.InstantiatesPrefabArtifactWithStableSourceToInstanceMap
```

Result: 2 tests passed.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.PreparedPrefabFreshnessRejectsStaleKeyVersionsAndStamps:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationMapsDeepImportedHierarchy:AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters
```

Result: 3 tests passed.

### Prefab: Deferred Activation Guard

`Scene::AddGameObject` now supports an explicit deferred-activation mode while preserving immediate activation as the default behavior. `LoadPolicy::deferActivation` uses that mode during prefab registration and then activates the prefab root inside the measured `InvokeLifecycle` stage after allocation, component restoration, internal fixup, and scene registration have completed.

The focused regression tests verify:

- a playing scene can register a GameObject subtree without immediately running `Awake`/`Start`
- explicit activation runs lifecycle once and repeated activation calls do not duplicate `Awake`, `Enable`, or `Start`
- prefab instantiation with `LoadPolicy::deferActivation=true` emits `InvokeLifecycle` with the activated subtree object count
- prefab identity/source mapping and repeated-instantiation independence remain intact

Fresh validation was generated with:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
```

Result: exit code 0.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.SceneDeferredActivationDelaysLifecycleUntilExplicitActivation:AssetPrefabPipelineTests.DeferredPrefabActivationRunsLifecycleInInvokeLifecyclePhase:AssetPrefabPipelineTests.RepeatedPrefabInstantiationKeepsHotPathIndexedAndInstancesIndependent:AssetPrefabPipelineTests.InstantiatesPrefabArtifactWithStableSourceToInstanceMap:AssetPrefabPipelineTests.GeneratedModelPrefabInstantiationMapsDeepImportedHierarchy:AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters
```

Result: 6 tests passed.

This completes the deferred-activation behavior guard for `T021`/`T025`. It does not complete `T027`: full before/after profiling comparison for all US2 prefab scenarios is still pending.

### Prefab: Editor-Restart Prepared Cache Guard

`AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheHitSkipsPrefabGraphLoad` now covers the `T022` editor-restart cache-hit path from the prefab pipeline test suite. The test imports a generated model prefab, performs a cold scene-restore load to create `Library/PreparedPrefabCache/*.json`, clears the imported prefab L1 hot cache to simulate a new editor session, asserts the L1 entry count is zero, and verifies the second load reports a `CacheHit` whose telemetry path is the prepared-cache JSON file with zero `PrefabGraphLoad` telemetry records.

`AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheRejectsStaleFreshnessRecord` mutates the prepared-cache JSON freshness stamp, clears L1 again, asserts the L1 entry count is zero, and verifies the stale entry is rejected: no `CacheHit` is recorded for that prepared-cache JSON file and the reload reparses the prefab graph. `AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheRejectsMalformedFreshnessTypes` also corrupts freshness field types and verifies the cache reader records a `LoadPreparedPrefabCache.cacheMisses` counter instead of throwing or accepting the malformed entry. This keeps the tests tied to structured JSON fields and artifact telemetry rather than source-code string inspection.

Fresh validation was generated with:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheHitSkipsPrefabGraphLoad:AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheRejectsStaleFreshnessRecord:AssetPrefabPipelineTests.EditorRestartPreparedPrefabCacheRejectsMalformedFreshnessTypes:AssetPrefabPipelineTests.PreparedPrefabFreshnessRejectsStaleKeyVersionsAndStamps:AssetPrefabPipelineTests.PreparedPrefabMappingDependencyFingerprintReusesStampedCache:AssetPrefabPipelineTests.PreparedPrefabSourcePathMappingFallbackTracksTextureManifestStamp
```

Result: 6 tests passed.

### Current Measured Thumbnail Ranking

1. `TotalThumbnail` - wrapper total for the measured thumbnail generation call.
2. `StorePreviewCache` - synchronous cache write path is the largest concrete measured stage.
3. `ThumbnailCacheLookup` - two cache lookups plus duplicate request accounting are visible in first generation; disk-cache hit lookup is measured at 2.16 ms in the deterministic texture fixture.
4. `EncodePreview` - small for a 1x1 PNG fixture.

## Cache And Synchronization Findings

- Prepared prefab cache exists and is instrumented in `EditorAssetDragDropBridge`, but it is not yet proven to serve all prefab artifact load and thumbnail preview entry points.
- Thumbnail cache duplicate request counting is active for identical cache keys and the minimal test confirms repeated requests coalesce into pending work instead of immediately generating twice.
- Thumbnail cache hit/miss counters are active. The disk-hit path returns without regenerating, encoding, or storing a thumbnail.
- Thumbnail disk cache identity now has dedicated preview renderer version, dependency stamp, color space, and HDR inputs in addition to settings, size, asset identity, artifact path, and freshness inputs. Manifest-resolved artifact files are included in dependency freshness.
- Prepared-prefab freshness no longer relies on recursive `Assets` scans for exact model texture `source-path` mapping checks when the ArtifactDB index lacks the texture row; it falls back to bounded exact-file metadata and manifest reads, keeping cache invalidation correct without turning thumbnail or prepared-cache lookup into a hidden project scan.
- The CPU thumbnail path still writes cache output synchronously on the main measured path in the minimal scenario.
- GPU preview readback/fence stages are now instrumented, and `WaitPreviewFence` is covered at the RHI readback completion boundary. A full GPU-preview thumbnail scenario is still required before making a measured claim about editor main-thread stalls or non-blocking behavior.
- Prefab instantiation no longer records `WaitForResources` and `UploadGpuResources` on the default path. Those stages are now explicit synchronous-prewarm diagnostics, so future reports should treat their presence as caller opt-in rather than baseline prefab graph creation cost.
- Large prefab data now shows a concrete non-resource bottleneck: `Scene::AddGameObject`-covered registration dominates 100/1000 object cases even with `rendererCount=0`. The specific hot path is repeated `RebuildFastAccessComponents()` calls from `Scene::OnComponentAdded` during recursive registration.
- Scene-open prefab restore has a separate CPU/memory amplification path: `EditorActions::LoadSceneFromDisk` restores Unity-style stripped prefab instances after `SceneManager::LoadScene`, and `RestorePrefabInstancesFromSceneDocument` previously copied the full source prefab graph into every restored `PrefabInstanceRecord`. Repeated large-model prefab instances now keep a shared source prefab artifact for metadata/override queries, while scene-local changes remain stored as patches.
- The current prepared-prefab cache uses JSON and remains a compatibility/debug-friendly representation. A binary artifact should wait until large-scenario measurements show JSON parse/DOM/string lookup cost is a top concrete bottleneck.

## Minimum Viable Optimization Plan

Do not start broad restructuring yet. The next implementation should add the missing benchmark matrix and then attack the largest measured main-thread stall.

P0 candidates, gated by broader data:

1. If GPU preview scenarios show `WaitPreviewFence` or `ReadbackPreview` at the top, replace synchronous thumbnail readback with pollable multi-frame readback.
2. If duplicate Project Browser work dominates rapid-scroll scenarios, strengthen thumbnail request deduplication, cancellation, and visible-priority budgeting.
3. If preview setup dominates, reuse the preview scene/render targets and avoid recreating preview world state per thumbnail.
4. If prefab preview creation dominates, introduce `PreviewRenderableSnapshot` or preview-only instantiation that excludes scripts, physics, audio, navigation, lifecycle callbacks, and full scene registration.
5. If prefab resource stages dominate, decouple prefab graph creation from mesh/material/texture readiness and bind shared asset handles instead of synchronously loading resources.

## Recommended Files To Modify Next

- `Tests/Unit/AssetPrefabPerformanceTests.cpp`: add nested, repeated, cold/hot resource, and L2 prepared-cache scenarios.
- `Tests/Unit/AssetThumbnailPerformanceTests.cpp`: add first GPU prefab/model preview, memory cache hit, disk cache hit, and rapid-scroll cancellation/deduplication scenarios.
- `Tests/Unit/AssetThumbnailPerformanceTests.cpp`: add first GPU prefab/model preview, memory cache hit, and rapid-scroll cancellation/deduplication scenarios.
- `Tests/Unit/AssetThumbnailCacheTests.cpp`: continue expanding thumbnail key invalidation assertions for dependency stamp and color/HDR fields. Renderer version, preview settings, size, asset identity, and freshness inputs are now covered.
- `Tests/Unit/AssetPrefabPipelineTests.cpp`: add prepared-prefab freshness rejection tests for artifact stamp, dependency stamp, reflection schema, importer version, serialization format version, and failed builds.
- `Project/Editor/Assets/AssetThumbnailService.cpp`: broaden budget counters beyond cache-write budget once CPU prepare, preview render, GPU upload, and readback budgets are enforced.
- `Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp`: add GPU-preview benchmark coverage and later preview reuse/lightweight-preview changes.
- `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`: after GPU-preview data, add non-blocking readback/fence polling if `WaitPreviewFence` is proven hot.
- `Runtime/Engine/Serialize/ObjectGraphInstantiator.h` and `Runtime/Engine/SceneSystem/Scene.cpp`: split `RegisterRenderers` into scene insertion, component event registration, renderer registration, and cache rebuild sub-stages before optimizing the large-prefab bottleneck.
- `Runtime/Engine/Assets/PrefabAsset.cpp`: after resource-heavy data, defer nonessential resource waits and avoid repeated resource load/prewarm.

## Recommended Tests To Add Or Extend

- `AssetPrefabPerformanceTests.NestedPrefabBaseline`
- `AssetPrefabPerformanceTests.RepeatedInstantiateHotCacheBaseline`
- `AssetPrefabPerformanceTests.PreparedPrefabDiskCacheHitBaseline`
- `AssetPrefabPerformanceTests.ResourceColdAndHotCacheBaselines`
- `AssetPrefabPerformanceTests.SceneRegistrationSubstageCounters`
- `AssetThumbnailPerformanceTests.FirstGpuPrefabThumbnailBaseline`
- `AssetThumbnailPerformanceTests.ThumbnailMemoryCacheHitBaseline`
- `AssetThumbnailPerformanceTests.RapidProjectBrowserScrollDeduplicatesAndCancels`
- `AssetThumbnailPerformanceTests.GpuPreviewFenceWaitIsReportedWithoutBlockingAssertions`
- `AssetThumbnailCacheTests.ThumbnailKeyInvalidatesOnRendererSettingsDependencyAndColorChanges`
- `AssetPrefabPipelineTests.PreparedPrefabFreshnessRejectsStaleKeys`

## Validation Evidence

Fresh commands run on 2026-06-18:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m
```

Result: exit code 0.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetThumbnailPerformanceTests.*:PerformanceStageStatsTests.*:AssetThumbnailCacheTests.*:GameObjectAssetImportTests.UnifiedPrefabSharedHotCacheReusesPrefabGraphWithoutCopyingOnWarmPreview:GameObjectAssetImportTests.UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps
```

Result: 69 tests passed.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPipelineTests.*
```

Result: 24 tests passed.

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\performance-reports\053-phase1-prefab-matrix'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*
```

Result: 4 tests passed, and prefab matrix reports were written for import/instantiate, small, 100-object, 1000-object, and multi-renderer scenarios.

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='d:\VSProject\Nullus\Build\performance-reports\053-phase1-thumbnail-cache'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailPerformanceTests.*
```

Result: 2 tests passed, and thumbnail reports were written for first texture generation and disk-cache hit.

Fresh follow-up commands run on 2026-06-18 after the direct simple property apply path:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:4 /nr:false
```

Result: exit code 0.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.*:AssetThumbnailPerformanceTests.*:PerformanceStageStatsTests.*
```

Result: 21 tests passed after adding `AssetThumbnailPerformanceTests.ZeroCacheWriteBudgetDoesNotStartAsyncTextureThumbnail`.

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.*:AssetThumbnailPerformanceTests.*:AssetPrefabPerformanceTests.*:PerformanceStageStatsTests.*
```

Result: 79 tests passed after completing the T007 thumbnail key invalidation coverage.

```powershell
$env:NLS_PERFORMANCE_REPORT_DIR='Build\perf-053-direct'
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetPrefabPerformanceTests.LargePrefabScenariosEmitObjectAndComponentScaleCounters:AssetPrefabPerformanceTests.LargePrefabComponentRestoreAppliesSimplePropertiesDirectly
```

Result: 2 tests passed, and direct-property prefab reports were written under `Build\perf-053-direct`.

## Remaining Phase 1 Gaps

- Full benchmark matrix is partially implemented for prefab object-count and multi-renderer baselines plus thumbnail first-generation, disk-cache-hit, memory-cache-hit, rapid-scroll deduplication/backlog, and queued-cancellation baselines; nested, repeated, L2 prepared-cache, resource cold/hot, thumbnail GPU, and GPU-preview rapid-scroll scenarios remain.
- GPU-preview thumbnail reports are not generated yet.
- Budget injection plus queue/backlog/in-flight/coalescing-pressure and queued cancellation-latency counters are covered for CPU scheduler fixtures. RHI fence-wait accounting is covered; full non-blocking GPU-preview scheduler behavior remains open for US3.
- Prepared-prefab freshness tests now cover the required artifact, dependency, reflection schema, importer, serialization format, and dependency-manifest fields. Failed-build cache admission still needs dedicated coverage.
- L2 prepared-cache hit measurement after simulated editor restart is covered for the `EditorAssetDragDropBridge` scene-restore path, including proof that the L1 hot cache is empty and the telemetry hit comes from `Library/PreparedPrefabCache/*.json`. Generalizing this evidence to thumbnail preview and non-bridge prefab artifact load entry points remains open.
- No before/after optimization comparison should be reported until matching optimized runs exist for the same scenarios.
