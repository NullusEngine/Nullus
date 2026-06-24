# Contract: Performance Diagnostics

## Scope

This contract defines the observable diagnostic output for prefab instantiation and thumbnail generation benchmark runs.

## Required Report Sections

- Run identity: scenario name, run type, timestamp, and build/configuration label when available.
- Prefab stage table: all required prefab stages with total time, main-thread time, background-thread time, call count, and relevant counters.
- Thumbnail stage table: all required thumbnail stages with total time, main-thread time, background-thread time, call count, and relevant counters.
- Summary counters: object count, component count, renderer count, dependency count, cache hits/misses, synchronous resource loads, GPU upload bytes, fence wait time, duplicate thumbnail request count, and thumbnails generated per frame.
- Scheduler counters: queue depth/backlog, in-flight request count by priority, cancellation latency, and coalescing pressure.
- Cache counters: prepared-cache hit/miss counts, thumbnail disk prune scanned/removed/remaining counts when pruning runs, and resource request coalescing pressure where exposed.
- Bottleneck ranking: at least top five stages by total time for each domain with ties kept deterministic by stage name.
- Comparison section: baseline value, optimized value, and percentage change when an optimized run is compared to a baseline.
- Warnings: missing stages, missing baseline, stale comparison data, cancelled scenarios, failed resources, or incomplete runs.

## Required Prefab Stages

- LoadPrefabArtifact
- LoadPreparedPrefabCache
- ParsePreparedPrefab
- ResolveDependencies
- AllocateInstanceObjects
- DeserializeComponents
- FixupInternalReferences
- ResolveExternalReferences
- RegisterRenderers
- RegisterPhysics
- RegisterScripts
- InvokeLifecycle
- WaitForResources
- UploadGpuResources
- TotalInstantiate

## Required Thumbnail Stages

- ThumbnailCacheLookup
- PreparePreviewAsset
- CreatePreviewInstance
- CalculateBounds
- PreparePreviewResources
- RecordPreviewRender
- SubmitPreviewRender
- WaitPreviewFence
- ReadbackPreview
- EncodePreview
- StorePreviewCache
- TotalThumbnail

## Acceptance Rules

- A report must state whether it is a baseline, optimized run, or comparison.
- A comparison report is invalid unless baseline and optimized runs have the same scenario identity.
- A comparison report is invalid when baseline data is missing.
- Missing required stages must be reported as warnings, not silently omitted.
- A performance improvement claim is invalid without baseline and optimized values for the same scenario.
- Stage ordering in tables and rankings must be deterministic.
