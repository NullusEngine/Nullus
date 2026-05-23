# Quickstart: UE5-Style RHI/RDG Threading Foundation

## Current Status

The feature is no longer at the initial capability-only slice.

The currently delivered layer includes:

- truthful DX12/Vulkan capability reporting for the UE-style rendering foundation,
- explicit threaded ownership boundaries and lifecycle attribution,
- graph-backed execution-plan helpers used by forward and deferred renderers,
- ordered multi-command-buffer submission plumbing,
- hooks and diagnostics for async compute, transient lifetime, PSO cache, and descriptor allocator,
- shared GPU `LightGrid` compute prepass (`Injection` + `Compact`) for forward and deferred paths.

This is a foundation layer, not the final UE-aligned closure layer.

The largest remaining gaps are:

- Render Thread and RDG are not yet the sole authoritative frame-build owners,
- DX12/Vulkan runtime acceptance evidence and RenderDoc capture evidence are still incomplete,
- descriptor and transient lifetime systems are only partially adopted and still need broader runtime/test closure,
- PSO caching is now centralized and prewarm-capable, but not yet a durable backend cache/precache system.

## Build

```powershell
cmake --build Build\windows --config Debug --target NLS_Engine -- /m:4
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:4
cmake --build Build\windows --config Debug --target Editor -- /m:4
```

## Run Current Validation

### Focused Threaded-RDG Validation

```powershell
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanPrependsAdditionalOrderedWorkUnits:FrameGraphSceneTargetsTests.BuildThreadedExecutionPlanEmitsResourceAccessDependencyEdges:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanMapsMultipleResourceDependencyEdgesToTargetWorkUnit:ThreadedRenderingLifecycleTests.AsyncComputeSchedulingBuildsGraphicsComputeGraphicsSubmissionChain:ThreadedRenderingLifecycleTests.AsyncComputeSchedulingAllowsNonAdjacentGraphicsConsumerToWaitOnLastComputeBatch:ThreadedRenderingLifecycleTests.TranslationMergeInsertsBarrierBatchForDeferredWorkUnits:ThreadedRenderingLifecycleTests.TranslationMergeInsertsBarrierBatchForComputeVisibilityRequests"
```

Expected result:

- `7 / 7` tests pass

### Full Feature Suites

```powershell
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="FrameGraphSceneTargetsTests.*:ThreadedRenderingLifecycleTests.*"
```

Expected result:

- `92 / 92` tests pass

### Capability Gating Suites

```powershell
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="GraphicsBackendUtilsTests.*" --gtest_brief=1
```

## Evidence Achieved So Far

- `NullusUnitTests` rebuild succeeds with `cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:4`.
- The focused edge-driven barrier / queue-wait regression filter passes `7 / 7`.
- `FrameGraphSceneTargetsTests.*` plus `ThreadedRenderingLifecycleTests.*` pass `92 / 92`.
- `GraphicsBackendUtilsTests.*` remains the focused capability-gating suite for backend truth checks.
- DX12 reports Tier A foundation readiness without overclaiming async compute, transient lifetime, or parallel recording support.
- Vulkan reports Tier A foundation readiness only when the selected graphics queue family also exposes compute support; otherwise it stays truthfully below the foundation bar.
- Legacy backends are not accidentally elevated to Tier A foundation support.
- Compute-only shader assets no longer emit false `VS/PS` compile-error noise during test runs.

## Remaining Acceptance Evidence

Later slices in this bundle will extend validation with:

- targeted DX12 editor/game runtime validation
- DX12 RenderDoc captures
- Vulkan runtime validation
- Vulkan capture-based evidence where the platform path is stable
- final UE-alignment closure validation for:
  - authoritative Render Thread / RDG ownership
  - true parallel recording / translation
  - truthful async compute scheduling
  - descriptor / transient / PSO main-path adoption
