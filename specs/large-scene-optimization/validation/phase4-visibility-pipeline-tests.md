# Phase 4 Visibility Pipeline Focused Test Evidence

```text
Report: US2 visibility pipeline focused regression tests
Date: 2026-06-04
Branch: large-scene-optimization
Machine: local developer workstation
OS: Windows
Backend: None for unit-test renderer setup
Build configuration: Debug
Feature gates: enableSpatialIndex=true for spatial visibility path; serial/parallel/full-scan modes exercised explicitly
Build command: cmake --build build\windows --target NullusUnitTests --config Debug
Result: build succeeded; SceneVisibilityPipeline.cpp and SceneVisibilityPipelineTests.cpp were included by CMake glob pickup
```

```text
Validation command: .\build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.*:SceneSpatialIndexTests.*:RenderSceneCacheTests.SpatialIndexVisibilityMatchesFullScanAndReportsSpatialCandidates:RenderSceneCacheTests.SpatialIndexSyncUsesDirtyHandlesAfterInitialRebuild:RenderSceneCacheTests.SpatialIndexDirtyTransformUpdatePreservesVisibilityEquivalence:RenderSceneCacheTests.SpatialIndexDirtyActiveStateUpdatePreservesVisibilityEquivalence:RenderSceneCacheTests.SpatialIndexUsesConservativeWideFrustumQueryRadius:RenderSceneCacheTests.SpatialIndexPreservesFrustumDisabledPrimitiveVisibility:RenderSceneCacheTests.SpatialVisibilityPipelineKeepsQueueFinalizationOwnedByRenderScene:RenderSceneCacheTests.SpatialVisibilityPipelinePreservesOneThousandCompatibleOpaqueReduction:RenderSceneCacheTests.SerialAndParallelVisibilityProduceEquivalentQueues:RenderSceneCacheTests.VisibleLayerMaskAppliesConsistentlyAcrossVisibilityModes:RendererFrameObjectBindingTests.SpatialVisibilityPipelineDrawsKeepRendererAssignedObjectIndices
Result: 22 tests from 4 test suites ran; 22 passed; 0 failed
```

```text
Whitespace validation command: git diff --check --ignore-submodules
Result: passed; Git emitted LF-to-CRLF conversion warnings only, with no whitespace errors.
```

```text
Generated-file boundary command: git diff -- Runtime\Engine\Gen Project\Editor\Gen Runtime\Rendering\Gen
Result: empty diff; no generated files were hand-edited.
```

## Covered Regression Areas

- `SceneVisibilityPipeline` declarations and implementation compile into `NLS_Engine`.
- Spatial candidate evaluation uses `SceneSpatialIndex` output, revalidates active, layer, distance, mesh, and frustum state, and emits cull reasons without returning to a hidden full-scene visibility scan.
- Serial, parallel, and full-scan comparison modes produce equivalent primitive bits, mesh bits, and sparse visible handle outputs.
- Parallel evaluation preserves sparse slot-map handle identity by setting primitive bits with `ScenePrimitiveHandle::index`, not dense snapshot ranges.
- Auto mode falls back to serial evaluation when the JobSystem is unavailable or scheduling cannot complete.
- `RenderScene::GatherVisibleCommands` routes spatial visibility through the pipeline while retaining ownership of opaque merge, transparent back-to-front ordering, dynamic instancing, object-index assignment, and queue telemetry.
- Sparse visible primitive indices keep finalization touched counts bounded to visible primitives for spatial visibility, preventing queue finalization from hiding another primitive-wide scan.
- Retained command-offset tables are reused on stable frames; `RenderSceneCacheTests.SpatialVisibilityPipelineKeepsQueueFinalizationOwnedByRenderScene` asserts the second spatial gather reports `commandOffsetRebuildCount=0` and does not add an offset-table full primitive scan to `primitiveRecordsTouched`.
- The existing 1,000 compatible opaque object dynamic-instancing reduction remains one submitted draw after spatial visibility pipeline integration.
- Renderer object-index binding still consumes `RenderScene` assigned object indices after pipeline integration.

## Scope Notes

- This evidence signs off the second US2 visibility-pipeline slice: T039-T048.
- `RenderScene::Synchronize` still performs the existing live-scene sweep because the current `SceneSystem::Scene` fast-access API is the available source of truth. That cost remains explicitly reported through `syncFullSweepCount` and `syncSweepTouchedSlotCount`; this slice removes hidden full-scene snapshot and queue-finalization scans from the spatial visibility path, not the source scene sweep itself.
- LOD/HLOD, occlusion, streaming, and editor overlays remain disabled future phases and are not claimed by this evidence.
