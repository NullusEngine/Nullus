# Editor Scene View Large-Scene Validation

Report: Phase 8 editor debugging remains trustworthy
Date: 2026-06-05
Commit: worktree local changes, not committed
Branch: large-scene-optimization
Machine: local Windows development workstation
OS: Windows
Backend: Unit-test snapshot contracts; no backend-specific GPU claim
Build configuration: Debug
Scene fixture: Synthetic visibility pipeline snapshots and editor source contracts
Camera path: Unit-test forward frustum plus snapshot-only Scene View contracts
Feature gates: Large-scene visibility, cull-reason debug snapshots, FrameInfo counters, optional Scene View culling overlay
Validation command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nr:false
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.*:EditorRenderPathContractTests.*LargeScene*:EditorRenderPathContractTests.*Cull*:EditorRenderPathContractTests.*Culling*:ThreadedRenderingLifecycleTests.PublishesCullReasonDebugSnapshotWithoutSceneDrain:ThreadedRenderingLifecycleTests.*Streaming*:RendererStatsTests.*LargeScene*
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.*:EditorRenderPathContractTests.*LargeScene*:EditorRenderPathContractTests.*Cull*:EditorRenderPathContractTests.*Culling*:DebugSceneLifecycleTests.CullingOverlayItemsAreBuiltFromFrameSnapshotOnlyWhenEnabled:ThreadedRenderingLifecycleTests.PublishesCullReasonDebugSnapshotWithoutSceneDrain:ThreadedRenderingLifecycleTests.*Streaming*:RendererStatsTests.*LargeScene*
```

Result: Build succeeded. 25 tests from 5 test suites passed.

## Evidence Summary

- `SceneVisibilityPipelineTests.CullReasonDebugSnapshotUsesPrimitiveSnapshotAndContainsNoLivePointers` verifies cull-reason debug output is derived from `ScenePrimitiveSnapshot` and `SceneVisibilityPipelineResult`, carrying handles/reasons/command offsets instead of live component pointers.
- `ThreadedRenderingLifecycleTests.PublishesCullReasonDebugSnapshotWithoutSceneDrain` verifies `FrameSnapshot` carries cull-reason debug data through the threaded lifecycle without requiring recorded draw-command drains.
- `EditorRenderPathContractTests.BaseSceneRendererPublishesCullReasonSnapshotAfterVisibility` verifies `BaseSceneRenderer` publishes the cull-reason snapshot only after `RenderScene::GatherVisibleCommands` has evaluated visibility.
- `EditorRenderPathContractTests.FrameInfoShowsCullReasonAndStreamingBudgetCountersFromSnapshot` verifies FrameInfo consumes large-scene cull, streaming, residency, and occlusion counters from `FrameInfo` snapshots only.
- `EditorRenderPathContractTests.DebugSceneCullingOverlayConsumesFrameSnapshotOnly` verifies the optional Scene View culling overlay builds its data from `FrameSnapshot::largeSceneCullReasonSnapshot` and does not traverse live scene state, call synchronization, or drain resource queues.
- `DebugSceneLifecycleTests.CullingOverlayItemsAreBuiltFromFrameSnapshotOnlyWhenEnabled` verifies the production overlay helper filters hidden and visible primitives directly from a `FrameSnapshot` without constructing editor passes or requiring Driver/RHI texture initialization.

## Focused Test Output

```text
Note: Google Test filter = SceneVisibilityPipelineTests.*:EditorRenderPathContractTests.*LargeScene*:EditorRenderPathContractTests.*Cull*:EditorRenderPathContractTests.*Culling*:DebugSceneLifecycleTests.CullingOverlayItemsAreBuiltFromFrameSnapshotOnlyWhenEnabled:ThreadedRenderingLifecycleTests.PublishesCullReasonDebugSnapshotWithoutSceneDrain:ThreadedRenderingLifecycleTests.*Streaming*:RendererStatsTests.*LargeScene*
[==========] Running 25 tests from 5 test suites.
[----------] 1 test from DebugSceneLifecycleTests
[       OK ] DebugSceneLifecycleTests.CullingOverlayItemsAreBuiltFromFrameSnapshotOnlyWhenEnabled
[----------] 6 tests from EditorRenderPathContractTests
[       OK ] EditorRenderPathContractTests.FrameInfoLargeSceneCountersConsumeFrameInfoSnapshotOnly
[       OK ] EditorRenderPathContractTests.FrameInfoShowsCullReasonAndStreamingBudgetCountersFromSnapshot
[       OK ] EditorRenderPathContractTests.DebugSceneCullingOverlayConsumesFrameSnapshotOnly
[       OK ] EditorRenderPathContractTests.BaseSceneRendererPublishesCullReasonSnapshotAfterVisibility
[       OK ] EditorRenderPathContractTests.EditorViewportCamerasEnableGeometryFrustumCullingForLargeImportedModels
[       OK ] EditorRenderPathContractTests.DebugCameraPassKeepsBackfaceCullingForCameraMesh
[----------] 3 tests from RendererStatsTests
[       OK ] RendererStatsTests.RendererStatsAggregatesLargeSceneTelemetryAndResetsPerFrame
[       OK ] RendererStatsTests.RendererStatsSumsLargeScenePrimitiveCountsAcrossRenderScenes
[       OK ] RendererStatsTests.RendererStatsSumsLargeSceneResidencyBytesAcrossSceneTelemetry
[----------] 13 tests from SceneVisibilityPipelineTests
[       OK ] SceneVisibilityPipelineTests.SpatialCandidatesMatchFullScanAndKeepVisibilityTouchedBounded
[       OK ] SceneVisibilityPipelineTests.CullReasonDebugSnapshotUsesPrimitiveSnapshotAndContainsNoLivePointers
[       OK ] SceneVisibilityPipelineTests.AutoFallsBackToSerialWhenJobSystemIsUnavailable
[       OK ] SceneVisibilityPipelineTests.ParallelAndFullScanComparisonPreserveSparseHandleBits
[       OK ] SceneVisibilityPipelineTests.EmptySnapshotReportsZeroPrimitiveCount
[       OK ] SceneVisibilityPipelineTests.HLODVisibilityMembershipRequiresExactHandle
[       OK ] SceneVisibilityPipelineTests.LODSelectionSuppressesInactiveLevelsBeforeCommandEligibility
[       OK ] SceneVisibilityPipelineTests.HLODProxySuppressesChildrenWithoutGlobalPrimitiveMutation
[       OK ] SceneVisibilityPipelineTests.HLODMissingProxyExposesStreamingInterest
[       OK ] SceneVisibilityPipelineTests.StreamingPlanInputUsesFinalVisibilityAndRepresentationInterest
[       OK ] SceneVisibilityPipelineTests.HLODProxyPrimitiveIsInactiveUnlessClusterSelectsIt
[       OK ] SceneVisibilityPipelineTests.ValidOcclusionHistoryRemovesVisiblePrimitiveBeforeCommandEligibility
[       OK ] SceneVisibilityPipelineTests.OcclusionConsumesFinalHLODRepresentationOnly
[----------] 2 tests from ThreadedRenderingLifecycleTests
[       OK ] ThreadedRenderingLifecycleTests.PublishesCullReasonDebugSnapshotWithoutSceneDrain
[       OK ] ThreadedRenderingLifecycleTests.PreparedFrameStreamingDependencyPinsLiveUntilFrameRetirement
[==========] 25 tests from 5 test suites ran.
[  PASSED  ] 25 tests.
```

## Touched-Count And Traversal Notes

The Phase 8 validation is contract-focused rather than a GPU/backend performance capture. It proves the debug overlay does not introduce additional live-scene traversal or resource drains:

| Consumer | Data Source | Live Scene Traversal | Resource Drain |
|----------|-------------|----------------------|----------------|
| FrameInfo large-scene counters | `Render::Data::FrameInfo::largeScene` | No | No |
| Scene View culling overlay | `Render::Context::FrameSnapshot::largeSceneCullReasonSnapshot` | No | No |
| Threaded renderer publication | `FrameSnapshot` copy | No | No |

Fallback reason: none in focused unit contracts.
Backend capability gate: not applicable; Phase 8 does not claim backend-specific GPU occlusion correctness.
RenderDoc/RHI evidence: DX12 HZB/occlusion capture evidence is recorded separately in `hzb-occlusion-dx12.md`.
Manual editor observation: not captured in this unit-test-only pass.
Follow-up task: Phase 9 focused/full validation and `/plan-review` quality gate.
