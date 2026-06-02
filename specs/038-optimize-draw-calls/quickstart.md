# Quickstart: Optimize Draw-Call Scalability

## Build and Unit Test

```powershell
.\build_windows.bat Debug x64
ctest --test-dir build/windows/Tests/Unit -C Debug --output-on-failure -R NullusUnitTests
build\windows\bin\Debug\NullusUnitTests.exe
```

The top-level `ctest --test-dir build/windows -C Debug --output-on-failure -R NullusUnitTests`
path is useful as a monitor, but `validation/final-diagnostics.md` records an intermittent
Windows access violation on that aggregation path during this feature's final pass. Use the
`build/windows/Tests/Unit` CTest directory plus direct `NullusUnitTests.exe` run as the
reproducible sign-off commands unless that residual is resolved.

Targeted suites while iterating:

```powershell
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderSceneCacheTests.*DynamicInstancing*:RenderSceneCacheTests.DenseCompatibleInstancesStayBoundedBySubmittedDrawLimit:RendererFrameObjectBindingTests.*Instanced*:RendererFrameObjectBindingTests.EngineProviderRejectsInvalidObjectIndexAfterObjectDataSlotIsFull:ThreadedRenderingLifecycleTests.*Parallel*:ThreadedRenderingLifecycleTests.*Sliced*:ThreadedRenderingLifecycleTests.RecordedDrawCommandSlicingRejectsStencilClearPassesUntilStencilLoadIsExplicit:FrameGraphSceneTargetsTests.ApplyThreadedExecutionPlanSlicesLargeRecordedPassAndRemapsDependencies:RendererStatsTests.*:DX12PipelineLayoutUtilsTests.DX12DrawCommandsForwardInstanceCountsToNativeInstancedCalls
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*InRenderPassChildCommandRecording*:ThreadedRenderingLifecycleTests.SubmitFailureAfterQueuedGpuWorkRetainsResourcesByFrameContextSlot:ThreadedRenderingLifecycleTests.DeviceLostAfterQueuedGpuWorkDoesNotWaitOnUnsignalableFrameFence:DX12PipelineLayoutUtilsTests.*ChildCommand*:DX12PipelineLayoutUtilsTests.DX12BundleRecordingBindsPipelineInsideEachChildRange:DX12PipelineLayoutUtilsTests.DX12QueueRejectsUnclosedCommandBufferSubmission:DX12PipelineLayoutUtilsTests.DX12QueueClassifiesPostExecuteSignalFailureByDeviceStatus
```

## Stress Validation

Record baseline and final evidence under `specs/038-optimize-draw-calls/validation/`:

- `baseline-diagnostics.md`: current scene/harness draw counts before implementation.
- `final-diagnostics.md`: final automated-test output, telemetry numbers, and RenderDoc capture path.

Baseline harness evidence for this worktree is the pre-implementation deterministic stress target described by `RenderSceneCacheTests.DynamicInstancingReducesOneThousandCompatibleOpaqueObjectsToOneSubmittedDraw`: before dynamic grouping, 1,000 compatible opaque mesh renderers submit 1,000 scene draws; the accepted result submits 1 draw on a stable frame with zero cached-command rebuilds. The non-groupable pass baseline is split by safety class: `ThreadedRenderingLifecycleTests.AttachmentFreeRecordedDrawPassSlicesIntoOrderedSerialWorkUnits` proves attachment-free 2,000-draw ranges can be partitioned without lost draws, while `ThreadedRenderingLifecycleTests.InRenderPassChildCommandRecordingUsesOneParentPassAndChildDrawRanges` verifies attachment-backed DX12-capable passes execute child draw ranges inside one parent render pass.

1. Create or load a scene, or use the deterministic stress tests added by this feature, with at least 1,000 visible opaque objects sharing one mesh, one material, and compatible render state.
2. Enable renderer draw-path diagnostics.
3. Capture per-frame telemetry for raw visible object count, submitted grouped draw count, largest instance group, cached command rebuild count, object-data overflow dropped count, command work unit count, and serial fallback reason.
4. Confirm the second stable frame does not rebuild cached draw state.
5. Repeat with 2,000 non-groupable recorded draws and confirm attachment-free passes report multiple ordered work units, while attachment-backed DX12-capable passes report one parent pass plus child draw ranges. Unsupported paths must report serial fallback and record the original unsliced pass input. Confirm repeated same-slot frames reuse child command pools/buffers after fence confirmation instead of creating new child pools each frame.

## RenderDoc Evidence

Follow `Docs/Rendering/RenderDocDebugging.md`.

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 180
```

Inspect the capture for:

- Instanced draw calls for compatible repeated opaque objects.
- No duplicate render-pass clears; attachment-backed DX12-capable child ranges should execute inside one parent render pass.
- Expected draw count in split non-groupable scenes.
- No missing resource transitions or invalid attachment use.

## Expected Evidence Before Sign-Off

- Unit tests pass for render scene caching/grouping, object data upload, renderer stats, and threaded command work units.
- DX12 RenderDoc capture path is recorded with the capture filename when capture succeeds; otherwise the failed capture command, timeout/result, and scoped sign-off limitation are recorded in `validation/final-diagnostics.md`.
- Final report distinguishes validated DX12 behavior from unvalidated future backend behavior.

If RenderDoc is unavailable on the current machine, record that explicitly in `validation/final-diagnostics.md` and keep the sign-off scoped to unit/build evidence plus DX12 contract tests.
