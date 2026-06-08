# Phase 4 Spatial Index Focused Test Evidence

```text
Report: US2 spatial index focused regression tests
Date: 2026-06-04
Branch: large-scene-optimization
Machine: local developer workstation
OS: Windows
Backend: None for unit-test renderer setup
Build configuration: Debug
Feature gates: enableSpatialIndex=true for RenderScene spatial candidate path
Build command: cmake --build build\windows --target NullusUnitTests --config Debug
Result: build succeeded; SceneSpatialIndex.cpp and SceneSpatialIndexTests.cpp were included by CMake glob pickup
```

```text
Validation command: .\build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneSpatialIndexTests.*:RenderSceneCacheTests.SpatialIndexVisibilityMatchesFullScanAndReportsSpatialCandidates:RenderSceneCacheTests.SpatialIndexSyncUsesDirtyHandlesAfterInitialRebuild:RenderSceneCacheTests.SpatialIndexDirtyTransformUpdatePreservesVisibilityEquivalence:RenderSceneCacheTests.SpatialIndexDirtyActiveStateUpdatePreservesVisibilityEquivalence:RenderSceneCacheTests.SpatialIndexUsesConservativeWideFrustumQueryRadius:RenderSceneCacheTests.SpatialIndexPreservesFrustumDisabledPrimitiveVisibility:RenderSceneCacheTests.LargeSceneTelemetryReportsVisibilityAndQueueFinalizationTouchedCounts:RenderSceneCacheTests.LargeSceneTelemetrySeparatesTombstonedSlotsFromLiveVisibilityWork:RenderSceneCacheTests.SerialAndParallelVisibilityProduceEquivalentQueues:RenderSceneCacheTests.VisibleLayerMaskAppliesConsistentlyAcrossVisibilityModes
Result: 18 tests from 2 test suites ran; 18 passed; 0 failed
```

## Covered Regression Areas

- `SceneSpatialIndex` declarations and implementation compile into `NLS_Engine`.
- Static localized query on a 100,000+ primitive partitioned fixture matches full-scan comparison while staying under candidate and touched-count budgets.
- Dynamic localized query reports `dynamicCandidateCount` and `dynamicRecordsTouched` without scanning all dynamic records.
- High-dynamic-count partitioned scenes still use localized dynamic buckets and stay under touched-count budgets.
- Large-radius primitives are inserted into all overlapped cells and remain queryable from distant overlapped buckets.
- Active, layer, and distance metadata filters are applied before returning candidate handles.
- Transformed bounds use the full world matrix for local bounds center and conservative max-axis world scale for radius, preventing scaled or offset local bounds from being spatially missed.
- Static rebuild budget uses last-good static buckets plus dirty overlay and reports last-good, dirty-overlay, and rebuild-fallback counters.
- Topology churn removes stale handles and rebuilds to a candidate set matching full-scan comparison.
- `RenderScene` owns and updates a spatial index after synchronization.
- `RenderScene` uses dirty and removed primitive handles for spatial-index updates after the initial rebuild; stable frames report zero spatial rebuild/refit/update work while still reporting the existing sync full-sweep fallback counters.
- Transform-only primitive movement dirties retained world bounds for spatial-index refit without rebuilding cached draw commands, and spatial visibility remains equivalent to full-scan visibility.
- Owner active/alive state changes dirty retained spatial metadata, preventing inactive-to-active primitives from being missed by spatial candidate filtering.
- Spatial queries use a conservative frustum query radius derived from far-plane corners instead of `farDistance * 2`, preventing wide-FOV edge primitives from being missed.
- `FrustumBehaviour::DISABLED` primitives stay conservatively eligible for spatial candidates, preserving full-scan visibility semantics.
- With `enableSpatialIndex=true`, `RenderScene` visibility agrees with full-scan serial visibility, reports `spatialCandidateCount`, keeps `fullScanCandidateCount=0`, and uses sparse visible primitive indices for bounded queue finalization.

## Scope Notes

- This evidence signs off the first US2 spatial-index and dirty-update slice: T031-T038, scoped to spatial-index maintenance and fallback reporting.
- `RenderScene::Synchronize` still performs the existing live-scene sweep because the current `SceneSystem::Scene` fast-access API is the available source of truth. That cost is explicitly reported through `syncFullSweepCount` and `syncSweepTouchedSlotCount`; spatial-index maintenance after the initial build now uses dirty/removed handles instead of a hidden full snapshot/rebuild. A future scene-dirty source is still required to remove the live-scene sweep itself.
- `SceneVisibilityPipeline` and serial/parallel/full-scan pipeline comparison modes are covered by the follow-up evidence in `phase4-visibility-pipeline-tests.md`; LOD/HLOD, occlusion, and streaming remain future phases.
