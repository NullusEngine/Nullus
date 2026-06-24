# Quickstart: Prefab and Thumbnail Performance

1. Configure a clean build of Nullus and make sure `NullusUnitTests` is available.
2. Build the focused test binary:

```powershell
cmake --build Build --target NullusUnitTests --config Debug -- /m:1 /nr:false
```

3. Run the prefab and thumbnail diagnostics suites:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PerformanceStageStatsTests.*:AssetPrefabPerformanceTests.*:AssetThumbnailPerformanceTests.*
```

4. Run the cache, prepared-prefab, scheduler, and GPU readback regression subset used by this implementation:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetThumbnailCacheTests.*:AssetPrefabPipelineTests.*:AssetMaterialConversionTests.MaterialManagerCoalescesDuplicateAsyncArtifactRequests:FrameGraphSceneTargetsTests.FramebufferSameSizeResizeReusesExplicitResources:ThreadedRenderingLifecycleTests.*
```

5. Collect the diagnostic report output for:
   - small prefab
   - 100 object prefab
   - 1000 object prefab
   - multi-renderer prefab
   - nested prefab
   - repeated instantiation
   - editor restart prepared-cache hit
   - resource cold cache
   - resource hot cache
   - first thumbnail generation
   - thumbnail memory cache hit
   - thumbnail disk cache hit
   - rapid Project Browser cancellation/deduplication

6. Verify the report includes:
   - total time
   - main-thread time
   - background-thread time
   - stage counts
   - object/component/renderer/dependency counts
   - cache hit rate
   - synchronous resource load count
   - GPU upload bytes
   - fence wait time
   - queue depth/backlog
   - in-flight request counts by priority
   - cancellation latency
   - coalescing pressure
   - duplicate thumbnail request count
   - per-frame thumbnail count

7. Verify cache-specific acceptance:
   - prepared prefab L2 hits skip prefab graph load after L1 is cleared
   - stale prepared prefab freshness records are rejected
   - failed prepared prefab imports do not enter L1 or L2 cache
   - thumbnail disk cache pruning enforces entry and byte budgets
   - duplicate material async artifact requests coalesce into one pending request

8. After P0 work lands, repeat the same benchmark set and compare the new report against the baseline.

9. For editor verification, confirm:
   - prefab semantics remain intact
   - thumbnail placeholders appear immediately while work is pending
   - the editor does not wait synchronously on GPU thumbnail completion
   - canceling a visible thumbnail request does not leave stale work running
   - editor shutdown can safely cancel or finish pending thumbnail/readback/encode/write/resource work

10. Verify that invalid comparisons are rejected:
   - no baseline
   - mismatched scenario
   - stale data from a changed stage or cache identity
