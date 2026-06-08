# Phase 6 HZB FrameGraph Validation

Date: 2026-06-04
Worktree: `.worktrees/large-scene-optimization`

## Scope

This evidence covers the HZB prepared-compute resource contract slice:

- `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h`
- `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`
- `Tests/Unit/FrameGraphSceneTargetsTests.cpp`

RHI capability gates, DX12 backend-specific render-pass/resource-state evidence, and RenderDoc/RHI-event validation remain separate Phase 6 work.

## TDD Evidence

RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

FrameGraphSceneTargetsTests.cpp(3186,30): error C2039:
"HZBFrameResourceRequest": is not a member of "NLS::Render::FrameGraph"

FrameGraphSceneTargetsTests.cpp(3238,19): error C2039:
"textureResourceAccesses": is not a member of "NLS::Render::Context::RecordedComputeDispatchInput"
```

GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0
```

Focused tests:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.HZBPreparedComputeDeclaresOpaqueDepthReadAndMip0WriteUntilMipChainLands:FrameGraphSceneTargetsTests.HZBPreparedComputeRequiresEligibleOpaqueDepthAndCompleteTextures:FrameGraphSceneTargetsTests.HZBOcclusionConsumesBuildExportedHZBStateWithoutDuplicatePreTransition:FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesTextureAccessesAndTransitions:FrameGraphSceneTargetsTests.HZBPreparedComputeEmitsMip0BuildToOcclusionDependencyEdge:FrameGraphSceneTargetsTests.ThreadedResourceDependencySlicesPartialTextureWritersForFullRangeRead:FrameGraphSceneTargetsTests.ThreadedResourceDependencyNormalizesDefaultFullRangeBeforeSlicing

[==========] Running 7 tests from 1 test suite.
[  PASSED  ] 7 tests.
```

## Covered Requirements

- HZB prepared compute is only emitted when the opaque depth source is eligible and all required textures exist.
- HZB build declares qualified opaque depth as a shader-read texture access.
- HZB build declares the currently written HZB mip0 as an explicit shader-write texture access. Additional mip-chain accesses are intentionally not declared until a shader path writes them.
- Prepared compute dispatch inputs carry texture resource accesses, pre-dispatch texture visibility transitions, and exported texture transitions.
- Threaded FrameGraph planning emits the mip0 HZB build-to-occlusion UAV-to-SRV dependency edge for the current shader path.
- HZB occlusion consumes the HZB build exported shader-read state instead of declaring a duplicate `ShaderWrite -> ShaderRead` pre-dispatch transition.
- Existing partial/full texture dependency slicing behavior remains covered after the prepared-compute texture path change.
