# Phase 6 Occlusion Capability Gate Validation

Date: 2026-06-04
Worktree: `.worktrees/large-scene-optimization`

## Scope

This evidence covers the RHI capability and no-stall occlusion slice:

- `Runtime/Rendering/RHI/RHITypes.h`
- `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`
- `Runtime/Engine/Rendering/SceneOcclusion.h`
- `Runtime/Engine/Rendering/SceneOcclusion.cpp`
- `Tests/Unit/SceneOcclusionTests.cpp`
- `Tests/Unit/RHITypesTests.cpp`
- `Tests/Unit/DX12PipelineLayoutUtilsTests.cpp`
- `Tests/Unit/FrameGraphSceneTargetsTests.cpp`

DX12 HZB execution remains evidence-gated: `HierarchicalZBuffer` and `ConservativeOcclusion` are explicitly advertised as disabled until DX12 resource-state and RenderDoc/RHI-event evidence lands. `AsyncReadback` is now advertised as supported after live DX12 texture and GPU buffer async readback tests passed; see `hzb-occlusion-dx12.md`.

## TDD Evidence

RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

SceneOcclusionTests.cpp: error C2039:
"SceneOcclusionCapabilityRequest": is not a member of "NLS::Engine::Rendering"

SceneOcclusionTests.cpp: error C2039:
"ResolveCapabilities": is not a member of "NLS::Engine::Rendering::SceneOcclusionSystem"

SceneOcclusionTests.cpp: error C2065:
"HierarchicalZBuffer", "ConservativeOcclusion", and "AsyncReadback" are undeclared RHIDeviceFeature values
```

DX12 evidence-gate RED:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated

[  FAILED  ] DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated
Expected: (hzb) != (std::string::npos), actual: npos
```

GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0
```

Focused tests:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.HZBPreparedComputeDeclaresOpaqueDepthReadAndMip0WriteUntilMipChainLands:FrameGraphSceneTargetsTests.HZBPreparedComputeRequiresEligibleOpaqueDepthAndCompleteTextures:FrameGraphSceneTargetsTests.HZBOcclusionConsumesBuildExportedHZBStateWithoutDuplicatePreTransition:FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesTextureAccessesAndTransitions:FrameGraphSceneTargetsTests.HZBPreparedComputeEmitsMip0BuildToOcclusionDependencyEdge:FrameGraphSceneTargetsTests.ThreadedResourceDependencySlicesPartialTextureWritersForFullRangeRead:FrameGraphSceneTargetsTests.ThreadedResourceDependencyNormalizesDefaultFullRangeBeforeSlicing:SceneOcclusionTests.*:DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated

[==========] Running 19 tests from 3 test suites.
[  PASSED  ] 19 tests.
```

Post-review focused tests after fixing the history-miss scan and adding P2 coverage:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.HZBPreparedComputeDeclaresOpaqueDepthReadAndMip0WriteUntilMipChainLands:FrameGraphSceneTargetsTests.HZBPreparedComputeRequiresEligibleOpaqueDepthAndCompleteTextures:FrameGraphSceneTargetsTests.HZBOcclusionConsumesBuildExportedHZBStateWithoutDuplicatePreTransition:FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesTextureAccessesAndTransitions:FrameGraphSceneTargetsTests.PreparedComputeDirectRecordingOrdersTextureBarriersAroundDispatch:FrameGraphSceneTargetsTests.HZBPreparedComputeEmitsMip0BuildToOcclusionDependencyEdge:FrameGraphSceneTargetsTests.ThreadedResourceDependencySlicesPartialTextureWritersForFullRangeRead:FrameGraphSceneTargetsTests.ThreadedResourceDependencyNormalizesDefaultFullRangeBeforeSlicing:SceneOcclusionTests.*:RHITypesTests.OcclusionFeatureFlagsRoundTripThroughLegacyFields:DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated

[==========] Running 26 tests from 4 test suites.
[  PASSED  ] 26 tests.
```

## Covered Requirements

- `SceneOcclusionSystem::ResolveCapabilities(RHIDevice&, ...)` consumes `device.GetCapabilities()` rather than issuing RHI work.
- `RHIDeviceFeature` and `RHIDeviceCapabilities` include explicit HZB, conservative occlusion, and async-readback feature gates.
- Opaque depth, HZB, and occlusion output texture formats are validated through `TextureFormatCapability`.
- Disabled HZB, disabled conservative occlusion, disabled async readback, unsupported opaque depth sampling, unsupported HZB storage/sampling, and unsupported occlusion output storage each produce conservative fallback reasons.
- `requireAsyncReadback=false`, invalid history texture, expired history, and capability diagnostic propagation are directly covered.
- Occlusion history miss classification uses a handle-indexed bucket instead of scanning the entire history map per primitive.
- Ordinary `SceneOcclusionSystem::Evaluate` has source and result regressions guarding against synchronous `ReadPixelsChecked`, `BeginReadPixels`, GPU fence waits, and blocking readback-buffer maps.
- Direct prepared-compute recording is covered for pre texture barriers -> dispatch -> exported texture barriers, including the null-pipeline no-barrier path.
- New occlusion-related RHI feature flags round-trip through legacy bool fields and `RHIDeviceFeature` states.
- DX12 keeps HZB/occlusion disabled by default with explicit RenderDoc/RHI-event/resource-state diagnostics until end-to-end runtime evidence lands, while async readback is enabled based on live DX12 readback coverage.

## Visibility Pipeline Integration Evidence

T075 RED:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.OcclusionConsumesFinalHLODRepresentationOnly

[ RUN      ] SceneVisibilityPipelineTests.OcclusionConsumesFinalHLODRepresentationOnly
Tests\Unit\SceneVisibilityPipelineTests.cpp(854): error: Value of: result.occludedPrimitiveHandles.empty()
  Actual: false
Expected: true
[  FAILED  ] SceneVisibilityPipelineTests.OcclusionConsumesFinalHLODRepresentationOnly
```

The failure proved `SceneVisibilityPipeline` still allowed stale sparse handles removed by HLOD to feed conservative occlusion. That could report a suppressed child as occluded after the final view representation had already switched to an HLOD proxy.

T075 GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.OcclusionConsumesFinalHLODRepresentationOnly:SceneVisibilityPipelineTests.ValidOcclusionHistoryRemovesVisiblePrimitiveBeforeCommandEligibility

[==========] Running 2 tests from 1 test suite.
[  PASSED  ] 2 tests.
```

Focused T075/T063-T076 regression:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.*:SceneOcclusionTests.*:RHITypesTests.OcclusionFeatureFlagsRoundTripThroughLegacyFields:DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated

[==========] Running 29 tests from 4 test suites.
[  PASSED  ] 29 tests.
```

Covered integration guarantees:

- Conservative occlusion is applied by `SceneVisibilityPipeline` after LOD/HLOD representation selection.
- Occlusion input collection now checks the authoritative visibility bitset before evaluating a handle, so stale sparse entries removed by HLOD/LOD cannot be reported as occluded.
- Valid occlusion history still removes an actually visible primitive before command eligibility.
- HLOD-selected proxies remain visible when only a suppressed child's old occlusion history exists.
- This was a CPU-side visibility pipeline integration slice only. Prepared-compute execution, DX12 resource-state validation, and RenderDoc evidence are covered by later validation entries, with runtime DX12 evidence recorded in `hzb-occlusion-dx12.md`.

## Prepared Compute Execution Evidence

T071 RED:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch

[ RUN      ] ThreadedRenderingLifecycleTests.RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch
Tests\Unit\ThreadedRenderingLifecycleTests.cpp(9966): error: Expected equality of these values:
  computeSubmittedCommandBuffer->events
    Which is: { "Begin", "Dispatch", "End" }
  std::vector<std::string>({ "Begin", "Barrier", "Dispatch", "Barrier", "End" })
    Which is: { "Begin", "Barrier", "Dispatch", "Barrier", "End" }
Tests\Unit\ThreadedRenderingLifecycleTests.cpp(9967): error: Expected equality of these values:
  computeSubmittedCommandBuffer->barrierHistory.size()
    Which is: 0
  2u
[  FAILED  ] ThreadedRenderingLifecycleTests.RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch
```

The failure proved the threaded RHI worker consumed prepared compute dispatches but did not execute the dispatch-owned texture transitions that FrameGraph already declared.

T071 GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch

[==========] Running 1 test from 1 test suite.
[  PASSED  ] 1 test.
```

Focused execution regression:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.SerialCommandPathConsumesPreparedParallelCommandWorkUnits:ThreadedRenderingLifecycleTests.TranslationMergeInsertsBarrierBatchForComputeVisibilityRequests:ThreadedRenderingLifecycleTests.AsyncComputeSchedulingBuildsGraphicsComputeGraphicsSubmissionChain:ThreadedRenderingLifecycleTests.AsyncComputeSchedulingAllowsNonAdjacentGraphicsConsumerToWaitOnLastComputeBatch:ThreadedRenderingLifecycleTests.TranslationVisibilityBatchFailureMarksSubmissionAsFailedRetirement:ThreadedRenderingLifecycleTests.TranslationVisibilityBarrierFailureMarksSubmissionAsFailedRetirement

[==========] Running 6 tests from 1 test suite.
[  PASSED  ] 6 tests.

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesTextureAccessesAndTransitions:FrameGraphSceneTargetsTests.PreparedComputeDirectRecordingOrdersTextureBarriersAroundDispatch:FrameGraphSceneTargetsTests.HZBPreparedComputeEmitsMip0BuildToOcclusionDependencyEdge:FrameGraphSceneTargetsTests.HZBPreparedComputeDeclaresOpaqueDepthReadAndMip0WriteUntilMipChainLands:FrameGraphSceneTargetsTests.HZBOcclusionConsumesBuildExportedHZBStateWithoutDuplicatePreTransition

[==========] Running 5 tests from 1 test suite.
[  PASSED  ] 5 tests.
```

Covered execution guarantees:

- Threaded RHI worker execution records prepared compute texture transitions before dispatch and exported texture transitions after dispatch.
- The recorded command order is `Barrier -> Dispatch -> Barrier` inside the submitted compute command buffer.
- Texture transition barriers preserve the declared texture, subresource range, before/after states, source access, and destination access.
- Existing async compute submission, translation-merge dependency visibility, and FrameGraph prepared compute declaration tests remain green.
- The implementation lives in the shared compute dispatch recorder consumed by `RhiThreadCoordinator` execution paths, so serial fallback, threaded worker, and prepared compute pass execution share the same texture-transition behavior.
- This still did not claim DX12-specific render-pass/resource-state correctness or RenderDoc evidence at the time. Later DX12 resource-state evidence is covered by T077, and runtime RenderDoc evidence is recorded in `hzb-occlusion-dx12.md`.

## DX12 Resource-State Contract Evidence

T077 RED:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12RenderPassUtilsTests.HZBOcclusionResourceStateContractsStayExplicitAndSubresourceAware:DX12RenderPassUtilsTests.PartialUnknownTextureBarrierFilteringKeepsHZBStorageWrites

[ RUN      ] DX12RenderPassUtilsTests.PartialUnknownTextureBarrierFilteringKeepsHZBStorageWrites
Tests\Unit\DX12RenderPassUtilsTests.cpp(256): error: Expected: (filterBody.find("IsD3D12CommonPromotionAllowedForTexture(barrier.after)")) != (std::string::npos), actual: npos vs npos
[  FAILED  ] DX12RenderPassUtilsTests.PartialUnknownTextureBarrierFilteringKeepsHZBStorageWrites
```

The failure proved the DX12 partial-texture filter would discard partial `Unknown` barriers, including HZB mip0 `Unknown -> ShaderWrite` UAV writes. That is not safe for occlusion/HZB because storage writes must remain explicit rather than relying on D3D12 common promotion.

T077 GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12RenderPassUtilsTests.HZBOcclusionResourceStateContractsStayExplicitAndSubresourceAware:DX12RenderPassUtilsTests.PartialUnknownTextureBarrierFilteringKeepsHZBStorageWrites:DX12PipelineLayoutUtilsTests.DX12CommandAllowsCommonPromotableUnresolvedPartialTextureBarriers:DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated

[==========] Running 4 tests from 2 test suites.
[  PASSED  ] 4 tests.
```

Focused resource-state regression:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.HZBPreparedComputeDeclaresOpaqueDepthReadAndMip0WriteUntilMipChainLands:FrameGraphSceneTargetsTests.HZBPreparedComputeRequiresEligibleOpaqueDepthAndCompleteTextures:FrameGraphSceneTargetsTests.HZBOcclusionConsumesBuildExportedHZBStateWithoutDuplicatePreTransition:FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesTextureAccessesAndTransitions:FrameGraphSceneTargetsTests.PreparedComputeDirectRecordingOrdersTextureBarriersAroundDispatch:FrameGraphSceneTargetsTests.HZBPreparedComputeEmitsMip0BuildToOcclusionDependencyEdge:FrameGraphSceneTargetsTests.ThreadedResourceDependencySlicesPartialTextureWritersForFullRangeRead:FrameGraphSceneTargetsTests.ThreadedResourceDependencyNormalizesDefaultFullRangeBeforeSlicing

[==========] Running 8 tests from 1 test suite.
[  PASSED  ] 8 tests.

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.RhiWorkerRecordsPreparedComputeTextureBarriersAroundDispatch:ThreadedRenderingLifecycleTests.TranslationMergeInsertsBarrierBatchForComputeVisibilityRequests

[==========] Running 2 tests from 1 test suite.
[  PASSED  ] 2 tests.
```

Covered DX12 contracts:

- `ResourceState::ShaderWrite` maps to DX12 unordered access for HZB/UAV writes.
- `ResourceState::ShaderRead` maps to both pixel and non-pixel shader resource states for sampled HZB consumption.
- `DepthRead` and `DepthWrite` map to the expected DX12 depth states, and read-only depth render-pass handling requests `ReadOnlyDepthStencil` DSV access.
- Texture barriers split partial subresource ranges through `BuildDX12BarrierSubresourceIndices`, preserving current mip0 HZB barriers. Future mip-chain barriers require shader support that actually writes those mips.
- Same-state unordered-access texture barriers remain UAV barriers.
- Partial `Unknown` barriers are filtered only when `barrier.after` is D3D12 common-promotion safe; partial `Unknown -> ShaderWrite` storage writes are retained.
- These are DX12 source/contract tests plus RHI-unit evidence. Runtime RenderDoc evidence is recorded separately in `hzb-occlusion-dx12.md`.
