# Data Model: Prefab and Thumbnail Performance

## Entities

### PerformanceStageDefinition

Represents one measured stage within the prefab or thumbnail pipeline.

Fields:
- `name`: stable stage name such as `LoadPrefabArtifact` or `WaitPreviewFence`
- `domain`: `Prefab` or `Thumbnail`
- `threadCategory`: `MainThread`, `BackgroundThread`, or `Mixed`
- `isGpuRelated`: whether the stage involves GPU submission, fence, or readback
- `counterNames`: list of extra counters captured for the stage

Relationships:
- Referenced by performance samples and diagnostic reports.

Validation rules:
- Stage names must be stable across runs.
- Every required stage in the spec must map to a definition.

### PerformanceStageSample

Represents one measured execution of a stage within a benchmark run.

Fields:
- `stageName`
- `callCount`
- `totalDuration`
- `mainThreadDuration`
- `backgroundThreadDuration`
- `extraCounters`

Relationships:
- Belongs to exactly one performance benchmark run.

Validation rules:
- Duration values must be non-negative.
- Call count must be at least 1 when a sample exists.

### PrefabBenchmarkScenario

Represents a repeatable prefab measurement scenario.

Fields:
- `scenarioName`
- `prefabSizeClass`
- `isNested`
- `isRepeatedInstantiation`
- `resourceCacheState`
- `expectedCachePath`

Relationships:
- Produces one or more performance sample sets.

Validation rules:
- Scenario names must uniquely identify the fixture and cache state.

### ThumbnailBenchmarkScenario

Represents a repeatable thumbnail measurement scenario.

Fields:
- `scenarioName`
- `thumbnailKeyIdentity`
- `viewportVisibilityState`
- `requestBurstProfile`
- `cacheState`
- `gpuFenceState`

Relationships:
- Produces one or more thumbnail performance sample sets.

Validation rules:
- Scenarios must cover first-run, in-memory hit, disk hit, and rapid-scroll cases.

### BenchmarkRun

Represents one collected baseline or optimized run.

Fields:
- `runType`: `Baseline`, `Optimized`, or `Comparison`
- `scenarioName`
- `totalTime`
- `mainThreadTime`
- `backgroundThreadTime`
- `stageSamples`
- `summaryCounters`
- `bottleneckRanking`
- `notes`

Relationships:
- Contains multiple stage samples and summary counters.

Validation rules:
- Comparison runs must reference a baseline and an optimized data set from the same scenario class.

### PrefabCacheKey

Represents the identity and freshness inputs for prepared prefab reuse.

Fields:
- `prefabGuid`
- `artifactStamp`
- `importerVersion`
- `reflectionSchemaVersion`
- `serializationFormatVersion`
- `dependencyManifestVersion`

Relationships:
- Keys prepared prefab entries.

Validation rules:
- Any key field change invalidates reuse.

### RuntimeResourceKey

Represents identity for shared runtime resource reuse.

Fields:
- `assetGuid`
- `subAssetId`
- `artifactStamp`
- `resourceType`

Relationships:
- Keys mesh/material/texture/shader cache entries.

Validation rules:
- Requests with different artifact freshness must not alias.

### ThumbnailKey

Represents the identity for a thumbnail generation request or cached thumbnail result.

Fields:
- `assetGuid`
- `artifactStamp`
- `previewRendererVersion`
- `previewSettingsHash`
- `dependencyStamp`
- `resolution`
- `colorSpaceMode`
- `hdrMode`

Relationships:
- Keys queued thumbnail tasks, in-memory thumbnail entries, and disk cache entries.

Validation rules:
- Any key field change invalidates the cached thumbnail.

### ThumbnailTask

Represents one thumbnail generation unit of work.

Fields:
- `key`
- `state`
- `priority`
- `duplicateRequestCount`
- `cancelled`
- `resourceWaitState`
- `renderState`
- `readbackState`
- `encodeState`
- `writeState`

Relationships:
- Linked to a thumbnail key and zero or more queued requests.

Validation rules:
- Duplicate requests must map to the same active task when keys match.
- A task may move from queued to preparing to rendering to readback to ready/failed/cancelled.

### CacheEntry

Represents one item in a bounded cache.

Fields:
- `key`
- `version`
- `stamp`
- `sizeCost`
- `state`
- `hitCount`
- `lastAccessTime`
- `retryCount`

Relationships:
- Used by prepared prefab, runtime resource, preview snapshot, thumbnail texture, and thumbnail disk caches.

Validation rules:
- Only successful entries may transition into a reusable ready state.
- Failed entries must obey a finite retry policy.

### DiagnosticReport

Represents the output summary of a benchmark run.

Fields:
- `scenarioName`
- `baselineSummary`
- `optimizedSummary`
- `topBottlenecks`
- `cacheStatistics`
- `warnings`

Relationships:
- Summarizes one or more benchmark runs.

Validation rules:
- A report must clearly identify whether it is baseline, optimized, or comparison data.

## State Transitions

### ThumbnailTaskState

- `Missing -> Queued`
- `Queued -> Preparing`
- `Preparing -> WaitingForResources`
- `WaitingForResources -> Rendering`
- `Rendering -> WaitingForGpu`
- `WaitingForGpu -> Readback`
- `Readback -> Ready`
- `Any active state -> Failed`
- `Any active state -> Cancelled`

### CacheEntryState

- `Missing -> Building`
- `Building -> Ready`
- `Building -> Failed`
- `Ready -> Evicted`
- `Failed -> Retrying`
- `Retrying -> Building`

## Relationships

- `BenchmarkRun` aggregates `PerformanceStageSample` entries.
- `PerformanceStageSample` references one `PerformanceStageDefinition`.
- `PrefabBenchmarkScenario` and `ThumbnailBenchmarkScenario` both generate `BenchmarkRun` data.
- `ThumbnailTask` is keyed by `ThumbnailKey`.
- `CacheEntry` is keyed by either `PrefabCacheKey`, `RuntimeResourceKey`, `ThumbnailKey`, or preview snapshot identity.

## Notes

- The model intentionally keeps diagnostics and cache identity separate from runtime scene instance identity.
- Preview snapshots are shared data, not runtime instances.
