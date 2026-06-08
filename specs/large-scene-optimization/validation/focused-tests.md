# Focused Large-Scene Unit Tests

Report: Phase 9 focused validation
Date: 2026-06-05
Commit: worktree local changes, not committed
Branch: large-scene-optimization
Machine: local Windows development workstation
Build configuration: Debug
Raw output log: `focused_large_scene_tests.local.log`

## Command

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*
```

## Result

Exit code: 0

```text
Note: Google Test filter = *RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*
[==========] Running 152 tests from 15 test suites.
[==========] 152 tests from 15 test suites ran. (124474 ms total)
[  PASSED  ] 152 tests.
```

## Coverage Notes

- Covers retained `RenderScene`, spatial index, visibility pipeline, LOD/HLOD, occlusion, streaming residency, renderer stats, frame snapshots, and related editor contracts.
- The full exact stdout/stderr stream is preserved in `focused_large_scene_tests.local.log` because the complete output is long and includes per-test RHI diagnostic noise from intentional failure-path tests.
- Observed RHI warnings/errors are from tests that intentionally exercise `NONE` backend, missing device, quarantine, or failure-path handling; gtest reported no failures.

## Closure Re-Run

Date: 2026-06-06
Raw output log: `focused_large_scene_closure_round9.local.log`

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*
```

```text
[==========] 169 tests from 15 test suites ran. (96577 ms total)
[  PASSED  ] 169 tests.
```

Notes:

- Re-run includes the post-review RenderScene identity, spatial-index budget recovery, runtime streaming parse bridge, and auto-visibility test-runtime closure fixes.
- `RenderSceneCacheTests.AutoVisibilitySettingsCanDisableParallelEvaluation` and `RenderSceneCacheTests.AutoVisibilityStaysSerialUntilPersistentJobSystemExists` were narrowed to the intended non-spatial auto-visibility behavior so the focused suite no longer spends 100s+ per test on unrelated spatial-index construction.

## Post-Review Blocker Targeted Re-Run

Date: 2026-06-06
Raw output logs:

- Build: `build_review_blocker_green_attempt2.local.log`
- Tests: `targeted_review_blockers_attempt1.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_review_blocker_green_attempt2.local.log
```

Targeted test command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="DeferredSceneRendererMaterialCacheTests.*HZB*:FrameGraphSceneTargetsTests.*HZB*:ShaderArchitectureAlignmentTests.*HZB*:SceneStreamingResidencyTests.*PrimitiveDependency*:RenderSceneCacheTests.BaseSceneRendererBuildsHZBPacketsFromSparseVisibleHandlesAfterVisibility:RenderSceneCacheTests.BaseSceneRendererParseSceneBuildsHZBOcclusionPacketsFromVisibleOpaquePrimitives:SceneSpatialIndexTests.BudgetRecoveryRebuildsStaticIndexWithoutNewDirtyHandles"
```

Result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
[==========] 31 tests from 6 test suites ran. (673 ms total)
[  PASSED  ] 31 tests.
```

Coverage notes:

- Verifies HZB build dispatch groups are texture-sized while occlusion dispatch groups are primitive-count-sized.
- Verifies current HZB frame-graph access declarations are intentionally mip0-only until the shader grows a real mip-chain build.
- Verifies HLSL/C++ occlusion primitive packet member order and shared layout offsets stay aligned.
- Verifies streaming primitive dependency duplicate registration uses the hash-set path.
- Verifies `BaseSceneRenderer` builds HZB packets from sparse visible handles after visibility instead of taking a hidden full-scene pre-visibility snapshot.
- Verifies spatial-index budget recovery rebuilds a last-good static index without requiring a new dirty static handle.

## Post-Review Focused Re-Run

Date: 2026-06-06
Raw output log: `focused_large_scene_postreview_round10.local.log`

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*"
```

Result:

```text
[==========] 171 tests from 15 test suites ran. (118614 ms total)
[  PASSED  ] 171 tests.
```

Notes:

- This is the freshest focused suite after the HZB packet-source, dispatch-group, mip0 contract, streaming dependency registration, and spatial-index budget recovery fixes.
- The suite still includes expected `NONE` backend and error-path RHI warnings from negative tests; gtest reported no failures.

## Plan-Review P2 Red-Green Closure

Date: 2026-06-06
Raw output logs:

- Red tests: `review_p2_red_tests.local.log`
- Build: `build_review_p2_green_retry.local.log`
- Narrow green tests: `review_p2_green_tests.local.log`
- Targeted green tests: `review_p2_targeted_green.local.log`

Red test command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="ShaderArchitectureAlignmentTests.HZBOcclusionShaderConsumesPrimitiveInputsAndWritesPrimitiveResults:ShaderArchitectureAlignmentTests.HZBOcclusionMetadataMatchesPrimitiveBufferShaderContract:SceneStreamingResidencyTests.TicketCoalescedRequestCountReflectsCurrentPlanNotLifetime"
```

Red result:

```text
[  FAILED  ] SceneStreamingResidencyTests.TicketCoalescedRequestCountReflectsCurrentPlanNotLifetime
[  FAILED  ] ShaderArchitectureAlignmentTests.HZBOcclusionShaderConsumesPrimitiveInputsAndWritesPrimitiveResults
[  FAILED  ] ShaderArchitectureAlignmentTests.HZBOcclusionMetadataMatchesPrimitiveBufferShaderContract
```

Green build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_review_p2_green_retry.local.log
```

Green build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Narrow green result:

```text
[==========] 3 tests from 2 test suites ran. (2 ms total)
[  PASSED  ] 3 tests.
```

Targeted green command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="DeferredSceneRendererMaterialCacheTests.*HZB*:FrameGraphSceneTargetsTests.*HZB*:ShaderArchitectureAlignmentTests.*HZB*:SceneStreamingResidencyTests.*PrimitiveDependency*:SceneStreamingResidencyTests.TicketCoalescingKeepsFirstRequestAndAppliesPriorityAging:SceneStreamingResidencyTests.TicketCoalescedRequestCountReflectsCurrentPlanNotLifetime:SceneOcclusionTests.RhiTextureFormatCapabilitiesGateOcclusionResources"
```

Targeted green result:

```text
[==========] 31 tests from 5 test suites ran. (59 ms total)
[  PASSED  ] 31 tests.
```

Notes:

- Removed the unused intermediate HZB occlusion output texture and `u_OcclusionOutput` shader binding. Current HZB occlusion reads the HZB texture and writes primitive result buffers only.
- Changed streaming residency ticket `coalescedRequestCount` to reflect the current plan's coalesced requests rather than a cumulative lifetime counter.

## Post-P2 Focused Re-Run

Date: 2026-06-06
Raw output log: `focused_large_scene_postreview_round11.local.log`

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*"
```

Result:

```text
[==========] 172 tests from 15 test suites ran. (83645 ms total)
[  PASSED  ] 172 tests.
```

Notes:

- This is the freshest focused suite after closing the plan-review P2 items.
- The run includes the new streaming coalesced-count regression and the updated HZB occlusion resource contract tests.

## Post-P1 Performance Fix Re-Run

Date: 2026-06-07
Raw output logs:

- Build: `build_large_scene_p1fix2_green.local.log`
- Targeted tests: `large_scene_p1fix2_targeted.local.log`
- Focused suite: `focused_large_scene_p1fix.local.log`
- HZB mip0 coverage contract: `hzb_mip0_coverage_targeted_green.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_large_scene_p1fix2_green.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted P1 regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneVisibilityPipelineTests.RepresentationCandidateExpansionUsesPipelineMetadata:SceneVisibilityPipelineTests.RepresentationCandidateExpansionUsesPrimitiveAdjacencyIndexes:SceneStreamingResidencyTests.MemoryBudgetEvictsOffscreenResidentResources:SceneStreamingResidencyTests.FallbackAwareEvictionSkipsPinnedVisibleResources:SceneStreamingResidencyTests.CommitReturnsCurrentAndEvictedTicketsWithoutCopyingHistoricalTable:SceneStreamingResidencyTests.MemoryEvictionSkipsResidentScanWhenWithinBudget"
```

Targeted P1 regression result:

```text
[==========] 6 tests from 2 test suites ran. (3 ms total)
[  PASSED  ] 6 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*"
```

Focused large-scene result:

```text
[==========] 174 tests from 15 test suites ran. (179120 ms total)
[  PASSED  ] 174 tests.
```

HZB mip0 coverage contract result:

```text
[==========] 38 tests from 4 test suites ran. (160 ms total)
[  PASSED  ] 38 tests.
```

Notes:

- LOD/HLOD representation expansion now consumes primitive adjacency maps supplied by `RenderScene`, avoiding a hidden full registry scan when sparse metadata is available.
- Streaming memory eviction now exits before resident dependency scans when the CPU and GPU resident byte totals are already within budget.
- Current HZB occlusion shader coverage is mip0-only and conservative: it exhaustively scans covered mip0 pixels up to `kHZBOcclusionMaxMip0ScanPixels`, and treats larger coverage as visible.

## Final P1-Closure Re-Run

Date: 2026-06-07
Raw output logs:

- Build: `build_large_scene_p1closure_tests3.local.log`
- Targeted P1 closure: `large_scene_p1closure_targeted3.local.log`
- Focused suite: `focused_large_scene_p1closure2.local.log`
- HZB fake-device depth-format fix: `hzb_packet_depth_format_fix.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_large_scene_p1closure_tests3.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted P1 closure command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererCommitsStreamingResidencyWithRetiredFramePins:RenderSceneCacheTests.BaseSceneRendererPublishesStreamingPinsThroughFrameSnapshots:RenderSceneCacheTests.SpatialVisibilityTelemetryDoesNotUnderReportPipelineTouches:ShaderArchitectureAlignmentTests.*HZB*:SceneOcclusionTests.CapabilityRequestUsesRequestedOpaqueDepthFormat:DeferredSceneRendererMaterialCacheTests.HZBCapabilityGateUsesActualDeferredDepthTextureFormat:SceneSpatialIndexTests.PrimitiveClassCountersDoNotScanAllRecordsOnTelemetryRead:SceneSpatialIndexTests.WideQueryVisitsOccupiedBucketsInsteadOfEmptyCellVolume"
```

Targeted P1 closure result:

```text
[==========] 12 tests from 5 test suites ran. (11 ms total)
[  PASSED  ] 12 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*"
```

Focused large-scene result:

```text
[==========] 180 tests from 15 test suites ran. (83200 ms total)
[  PASSED  ] 180 tests.
```

Notes:

- `BaseSceneRenderer` now publishes runtime streaming dependency pins through `FrameSnapshot` and `RenderScenePackage`, and residency commit consumes retired-frame pins from `DriverRendererAccess::CollectStreamingDependencyPins(m_driver)`.
- Large-scene streaming telemetry now merges committed resident CPU/GPU bytes, not only requested bytes.
- Spatial visibility telemetry now reports the max of spatial-candidate touches and downstream visibility-pipeline touches, avoiding under-reporting after LOD/HLOD/occlusion expansion.
- The HZB packet test fake device was aligned with the actual deferred depth SSoT (`Depth24Stencil8`) instead of the stale `Depth32F` assumption.

## Review P1/P2 Closure Re-Run

Date: 2026-06-07
Raw output logs:

- Build: `build_large_scene_review_p1fix5.local.log`
- Targeted review closure: `large_scene_review_p1fix_targeted3.local.log`
- Focused suite: `focused_large_scene_review_p1fix2.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_large_scene_review_p1fix5.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted review-closure command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneSpatialIndexTests.WideQueryUsesDirectCellsAndOnlyFallsBackForHugeVolumes:RenderSceneCacheTests.SpatialVisibilityTelemetryDoesNotUnderReportPipelineTouches:RenderSceneCacheTests.BaseSceneRendererCommitsStreamingResidencyWithRetiredFramePins:RenderSceneCacheTests.BaseSceneRendererPublishesStreamingPinsThroughFrameSnapshots:RenderSceneCacheTests.BaseSceneRendererFeedsModeledStreamingResidencyBackIntoVisibility:RendererStatsTests.RendererStatsAggregatesLargeSceneTelemetryAndResetsPerFrame:RendererStatsTests.RendererStatsUsesLatestLargeSceneResidencyByteGauge:SceneStreamingResidencyTests.DependencyClosureDeduplicatesVisibleAndRepresentationInterest:SceneStreamingResidencyTests.RepresentationResidencySnapshotStateUpdatesAreMutuallyExclusive:ThreadedRenderingLifecycleTests.*StreamingDependencyPins*:ThreadedRenderingLifecycleTests.PreparedBuilderResolveCompletesFallbackWhenMissingPackageBuilderThrows:ThreadedRenderingLifecycleTests.SnapshotHarnessResolveCompletesFallbackWhenHarnessBuilderThrows:PanelWindowHookTests.FrameInfoPanelFormatsLargeSceneTelemetrySnapshot"
```

Targeted review-closure result:

```text
[==========] 13 tests from 6 test suites ran. (19 ms total)
[  PASSED  ] 13 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*"
```

Focused large-scene result:

```text
[==========] 182 tests from 15 test suites ran. (236488 ms total)
[  PASSED  ] 182 tests.
```

Notes:

- Spatial index wide queries now prefer direct query-cell lookup and only fall back to occupied-bucket scans when the query cell volume is larger than occupied bucket count.
- Spatial visibility telemetry now sums spatial-candidate touches and downstream pipeline touches, preventing low reporting when LOD/HLOD/occlusion work expands beyond initial candidates.
- Renderer residency byte counters are treated as modeled global gauges, while primitive/draw/work counters remain additive across render scenes.
- Prepared-builder and snapshot-harness fallback render-scene packages preserve streaming dependency pins through frame retirement.
- Runtime LOD/HLOD readiness now feeds the previous frame's modeled streaming-residency commit back into visibility through `RenderSceneVisibilityOptions::representationResidency`; direct `RenderScene` unit tests without that runtime snapshot still use current resource readiness.

## Final P1 Green Re-Run

Date: 2026-06-07
Raw output logs:

- Build: `build_large_scene_final_p1_green_attempt8.local.log`
- Single contract regression: `large_scene_retired_pins_contract_green2.local.log`
- Focused suite: `focused_large_scene_final_p1_green2.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_large_scene_final_p1_green_attempt8.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Single contract regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererCommitsStreamingResidencyWithRetiredFramePins" *> large_scene_retired_pins_contract_green2.local.log
```

Single contract regression result:

```text
[==========] 1 test from 1 test suite ran. (1 ms total)
[  PASSED  ] 1 test.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_final_p1_green2.local.log
```

Focused large-scene result:

```text
[==========] 183 tests from 15 test suites ran. (85998 ms total)
[  PASSED  ] 183 tests.
```

Notes:

- The retired-frame streaming pin source contract was updated to the current global per-frame residency commit path: `allSceneStreamingInput` is planned once, `allSceneStreamingPlan` is committed once, and `DriverRendererAccess::CollectStreamingDependencyPins(m_driver)` is passed through `StreamingResidencyFramePins`.
- The focused suite is the freshest local large-scene suite after the final P1 fixes for global streaming budget aggregation, post-submit HZB readback, residency snapshot mutual exclusion, and benchmark/doc claim tightening.

## Final Review P2 Red-Green And Focused Re-Run

Date: 2026-06-07
Raw output logs:

- Red build: `build_large_scene_p2_regressions_red.local.log`
- Red tests: `large_scene_p2_regressions_red.local.log`
- Green build: `build_large_scene_p2_regressions_green2.local.log`
- Green tests: `large_scene_p2_regressions_green2.local.log`
- Focused suite: `focused_large_scene_final_p1_green4.local.log`

Red regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererAggregatesLegacyDrawOptimizationStatsAcrossAdditiveScenes:RenderSceneCacheTests.HandleScopedPrimitiveSnapshotPreservesGlobalCommandOffsetRanges" *> large_scene_p2_regressions_red.local.log
```

Red regression result:

```text
[  FAILED  ] RenderSceneCacheTests.BaseSceneRendererAggregatesLegacyDrawOptimizationStatsAcrossAdditiveScenes
[  FAILED  ] RenderSceneCacheTests.HandleScopedPrimitiveSnapshotPreservesGlobalCommandOffsetRanges
```

Green regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererAggregatesLegacyDrawOptimizationStatsAcrossAdditiveScenes:RenderSceneCacheTests.HandleScopedPrimitiveSnapshotPreservesGlobalCommandOffsetRanges:RenderSceneCacheTests.LargeSceneTelemetrySeparatesTombstonedSlotsFromLiveVisibilityWork" *> large_scene_p2_regressions_green2.local.log
```

Green regression result:

```text
[==========] 3 tests from 1 test suite ran. (2419 ms total)
[  PASSED  ] 3 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_final_p1_green4.local.log
```

Focused large-scene result:

```text
[==========] 185 tests from 15 test suites ran. (84492 ms total)
[  PASSED  ] 185 tests.
```

Notes:

- Legacy FrameInfo draw-optimization counters now aggregate dynamic instance group counts, largest group size, and object-data overflow drops across main and additive render scenes.
- Handle-scoped primitive snapshots now preserve global command offset ranges without forcing `RenderScene` command-offset cache rebuild telemetry to be consumed before `GatherVisibleCommands`.
- The focused suite includes the tombstoned-slot telemetry regression to prove sparse HZB/debug snapshots do not hide command-offset rebuild touched counts from the next visibility pass.

## Final Multi-Agent P2 Closure

Date: 2026-06-07
Raw output logs:

- Red build: `build_global_overflow_red.local.log`
- Red test: `global_overflow_red.local.log`
- Green build: `build_global_overflow_green.local.log`
- Green test: `global_overflow_green.local.log`
- Focused suite: `focused_large_scene_final_review5.local.log`
- Build: `build_large_scene_final_review_green2.local.log`

Global overflow regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.SceneRendererRespectsGlobalObjectDataCapacityAcrossAdditiveScenes" *> global_overflow_green.local.log
```

Global overflow regression result:

```text
[==========] 1 test from 1 test suite ran.
[  PASSED  ] 1 test.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_final_review5.local.log
```

Focused large-scene result:

```text
[==========] 185 tests from 15 test suites ran.
[  PASSED  ] 185 tests.
```

Multi-agent architecture re-review result:

```text
P0: none
P1: none
P2: none
```

Notes:

- Additive-scene global object-data capacity overflow now increments `objectDataOverflowDroppedObjectCount` during final merged-queue object-index reassignment when the global range cannot be resolved.
- The regression extends the existing additive capacity test so FrameInfo reports the dropped overflow count instead of silently showing zero.
- The architecture re-review confirmed the prior P2 is resolved and found no new P0/P1/P2 issues.

## R1 Plan-Review Occlusion Fix Closure

Date: 2026-06-07
Raw output logs:

- Build: `build_occlusion_review_p2_shader_green.local.log`
- Targeted shader/occlusion tests: `occlusion_review_p2_shader_green.local.log`
- Focused suite: `focused_large_scene_final_gate_after_review_fix.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_occlusion_review_p2_shader_green.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted shader/occlusion command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="ShaderArchitectureAlignmentTests.HZBShaderTypeLookupReportsMissingRegistryEntries:ShaderArchitectureAlignmentTests.GlobalShaderClassesRegisterLightGridAndDeferredLightingTypes:ShaderArchitectureAlignmentTests.HZBOcclusionShaderConsumesPrimitiveInputsAndWritesPrimitiveResults:SceneOcclusionTests.*" *> occlusion_review_p2_shader_green.local.log
```

Targeted shader/occlusion result:

```text
[==========] 29 tests from 2 test suites ran. (88 ms total)
[  PASSED  ] 29 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_final_gate_after_review_fix.local.log
```

Focused large-scene result:

```text
[==========] 187 tests from 15 test suites ran. (100778 ms total)
[  PASSED  ] 187 tests.
```

Notes:

- Covers the R1 plan-review fixes for conservative HZB primitive packet rejection at near-plane/perspective-risk bounds, bounded per-handle occlusion history keys, and missing HZB shader registry diagnostics.
- The focused suite includes the new occlusion history pruning and near-plane HZB packet regressions alongside the retained large-scene pipeline coverage.

## R2 Plan-Review Occlusion Fix Closure

Date: 2026-06-07
Raw output logs:

- Red build attempt: `build_r2_occlusion_regressions_red.local.log`
- Green build: `build_r2_occlusion_fixes_attempt1.local.log`
- Targeted R2 regression tests: `r2_occlusion_fixes_targeted_attempt1.local.log`
- Occlusion/shader suite: `r2_occlusion_shader_suite_attempt1.local.log`
- Focused suite: `focused_large_scene_r2_occlusion_fixes_full.local.log`

Targeted R2 regression command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneOcclusionTests.HZBPrimitivePacketsProjectPerspectiveBoundsWithoutFullscreenFallback:SceneOcclusionTests.HZBPrimitivePacketsRejectNearPlaneCrossingBoundsConservatively:SceneOcclusionTests.HistoryPrunesRemovedHandlesAndGenerationsToLiveSet:SceneOcclusionTests.HistoryPrunesOldPerHandleKeysAndKeepsLatestKeyClassifiable:ShaderArchitectureAlignmentTests.HZBShaderTypeLookupReportsMissingRegistryEntries:ShaderArchitectureAlignmentTests.GlobalShaderClassesRegisterLightGridAndDeferredLightingTypes" *> r2_occlusion_fixes_targeted_attempt1.local.log
```

Targeted R2 regression result:

```text
[==========] 6 tests from 2 test suites ran. (9 ms total)
[  PASSED  ] 6 tests.
```

Occlusion/shader suite command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneOcclusionTests.*:ShaderArchitectureAlignmentTests.*HZB*" *> r2_occlusion_shader_suite_attempt1.local.log
```

Occlusion/shader suite result:

```text
[==========] 34 tests from 2 test suites ran. (236 ms total)
[  PASSED  ] 34 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_r2_occlusion_fixes_full.local.log
```

Focused large-scene result:

```text
[==========] 189 tests from 15 test suites ran. (91097 ms total)
[  PASSED  ] 189 tests.
```

Notes:

- Perspective HZB primitive packets now project conservative bounds without falling back to fullscreen for ordinary valid perspective bounds.
- Near-plane crossing bounds still reject packet generation conservatively, keeping those primitives visible.
- Occlusion history now has a live-handle lifecycle prune path so tombstoned handles, stale generations, and removed scene handles do not accumulate indefinitely.
- HZB shader registry diagnostics are covered by a behavior test using an empty `ShaderTypeRegistry`, not only by source-text inspection.

## R3 Plan-Review HZB History Prune Closure

Date: 2026-06-07
Raw output logs:

- Build: `build_r3_prune_fix_attempt6.local.log`
- Targeted prune telemetry/integration: `r3_prune_fix_targeted_attempt6.local.log`
- Related HZB/occlusion contracts: `r3_prune_related_attempt6.local.log`
- Focused suite: `focused_large_scene_r3_prune_fix.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_r3_prune_fix_attempt6.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted prune command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererPrunesHZBHistoryOnlyForRemovedPrimitiveHandles:RenderSceneCacheTests.BaseSceneRendererPrunesDroppedAdditiveHZBHistoryWithoutRemovingMainHistory:RendererStatsTests.RendererStatsAggregatesLargeSceneTelemetryAndResetsPerFrame:PanelWindowHookTests.FrameInfoPanelFormatsLargeSceneTelemetrySnapshot" *> r3_prune_fix_targeted_attempt6.local.log
```

Targeted prune result:

```text
[==========] 4 tests from 3 test suites ran. (2043 ms total)
[  PASSED  ] 4 tests.
```

Related contract command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererParseSceneBuildsHZBOcclusionPacketsFromVisibleOpaquePrimitives:RenderSceneCacheTests.BaseSceneRendererBuildsHZBPacketsFromSparseVisibleHandlesAfterVisibility:SceneOcclusionTests.HZBPrimitivePacketsProjectPerspectiveBoundsWithoutFullscreenFallback:ShaderArchitectureAlignmentTests.HZBShaderTypeLookupReportsMissingRegistryEntries" *> r3_prune_related_attempt6.local.log
```

Related contract result:

```text
[==========] 4 tests from 3 test suites ran. (1159 ms total)
[  PASSED  ] 4 tests.
```

Focused large-scene command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_r3_prune_fix.local.log
```

Focused large-scene result:

```text
[==========] 191 tests from 15 test suites ran. (93369 ms total)
[  PASSED  ] 191 tests.
```

Notes:

- HZB history pruning now uses removed primitive handles for ordinary scene lifecycle updates instead of collecting and hashing every live primitive each HZB-enabled frame.
- Stable HZB frames report zero `hzbHistoryPruneTouchedHandleCount`; primitive removal reports touched/removed handle and key counts through `LargeSceneTelemetry`, `RendererStats`, and FrameInfo.
- Dropped additive scenes prune only that scene's handles and preserve main-scene HZB history.

## R4 Plan-Review HZB Prune API Guard

Date: 2026-06-07
Raw output logs:

- Build: `build_r4_p2_api_guard_attempt1.local.log`
- Targeted API guard: `r4_p2_api_guard_targeted_attempt1.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_r4_p2_api_guard_attempt1.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="SceneOcclusionTests.HistoryPrunesRemovedHandlesAndGenerationsToLiveSet:RenderSceneCacheTests.BaseSceneRendererPrunesHZBHistoryOnlyForRemovedPrimitiveHandles:RenderSceneCacheTests.BaseSceneRendererPrunesDroppedAdditiveHZBHistoryWithoutRemovingMainHistory" *> r4_p2_api_guard_targeted_attempt1.local.log
```

Targeted result:

```text
[==========] 3 tests from 2 test suites ran. (1325 ms total)
[  PASSED  ] 3 tests.
```

Notes:

- The previous public `PruneToLiveHandles` symbol was renamed to `PruneByLiveHandleSweepForLifecycleFallback`.
- The API now documents that ordinary frame removal must use `PruneHandles()` to avoid hidden live-scene scans.

## R5 Plan-Review Additive Prune Naming Tidy

Date: 2026-06-07
Raw output logs:

- Build: `build_r5_p2_name_tidy_attempt1.local.log`
- Targeted tests: `r5_p2_name_tidy_targeted_attempt1.local.log`

Build command:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_r5_p2_name_tidy_attempt1.local.log
```

Build result:

```text
NullusUnitTests.vcxproj -> ...\build\windows\bin\Debug\NullusUnitTests.exe
```

Targeted command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="RenderSceneCacheTests.BaseSceneRendererPrunesDroppedAdditiveHZBHistoryWithoutRemovingMainHistory:RenderSceneCacheTests.BaseSceneRendererPrunesHZBHistoryOnlyForRemovedPrimitiveHandles" *> r5_p2_name_tidy_targeted_attempt1.local.log
```

Targeted result:

```text
[==========] 2 tests from 1 test suite ran. (1340 ms total)
[  PASSED  ] 2 tests.
```

Notes:

- The dropped-additive cleanup variable was renamed from `liveHandles` to `droppedSceneHandles` so the rare lifecycle fallback cannot be mistaken for ordinary stable-frame live-scene pruning.

## Final Post-Deeper-Audit Focused Re-Run

Date: 2026-06-07
Raw output log: `focused_large_scene_r5_final.local.log`

Command:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*RenderScene*:*SceneSpatialIndex*:*SceneVisibility*:*SceneLOD*:*SceneHLOD*:*SceneOcclusion*:*SceneStreaming*:*RendererStats*" *> focused_large_scene_r5_final.local.log
```

Result:

```text
[==========] 191 tests from 15 test suites ran. (157358 ms total)
[  PASSED  ] 191 tests.
```

Notes:

- This focused suite was re-run after R5's final additive-prune naming tidy and after the deeper audit reported 0 P0/P1.
- The run preserves the same functional/contract coverage as the R3 prune fix focused suite and closes the deeper-audit evidence freshness note.
