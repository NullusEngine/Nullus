# DX12 HZB Occlusion RenderDoc Evidence

Date: 2026-06-05
Backend: DX12 / D3D12
Target: Editor Scene View validation project
Project: `build/RenderDocValidation/LargeSceneHZB/LargeSceneHZB.nullus`
Capture: `build/RenderDocCaptures/editor/dx12/large-scene-hzb-dx12-runtime_DX12_capture.rdc`
Log: `build/RenderDocValidation/LargeSceneHZB/Logs/2026-06-05_16-23-57.log`
Analysis JSON:

- `build/RenderDocValidation/LargeSceneHZB/hzb_capture_summary_after_renderdoc_fix.json`
- `build/RenderDocValidation/LargeSceneHZB/hzb_build_eid36_after_renderdoc_fix.json`
- `build/RenderDocValidation/LargeSceneHZB/hzb_occlusion_eid45_after_renderdoc_fix.json`

## Current Gate Status

DX12 `HierarchicalZBuffer` and `ConservativeOcclusion` are enabled only after the final DX12 A/B evidence recorded
below. The evidence is DX12-specific and does not enable or prove Vulkan, OpenGL, Metal, or DX11 behavior.

## Commands

```powershell
cmake --build build\windows --target Editor --config Debug -- /m:1 /nr:false

py -3 Tools\RenderDoc\renderdoc_runner.py `
  --target editor `
  --backend dx12 `
  --project build\RenderDocValidation\LargeSceneHZB\LargeSceneHZB.nullus `
  --capture `
  --capture-after-frames 240 `
  --capture-label large-scene-hzb-dx12-runtime `
  --terminate-after-capture `
  --timeout 300 `
  --app-arg=--editor-validation-exclusive-view `
  --app-arg=scene `
  --app-arg=--editor-validation-focus-view `
  --app-arg=scene `
  --app-arg=--renderdoc

py -3 Tools\RenderDoc\rdc_analyze.py build\RenderDocCaptures\editor\dx12\large-scene-hzb-dx12-runtime_DX12_capture.rdc
py -3 Tools\RenderDoc\rdc_analyze.py build\RenderDocCaptures\editor\dx12\large-scene-hzb-dx12-runtime_DX12_capture.rdc --focus-eid 36 --json-out build\RenderDocValidation\LargeSceneHZB\hzb_build_eid36_after_renderdoc_fix.json
py -3 Tools\RenderDoc\rdc_analyze.py build\RenderDocCaptures\editor\dx12\large-scene-hzb-dx12-runtime_DX12_capture.rdc --focus-eid 45 --json-out build\RenderDocValidation\LargeSceneHZB\hzb_occlusion_eid45_after_renderdoc_fix.json
```

## RenderDoc Summary

`rdc_analyze.py` reported:

- API: `D3D12`
- Events: `87`
- Draws: `43 (40 indexed, 3 non-indexed, 2 dispatches)`
- Dispatches: `2`
- Pass sequence:
  - `Nullus/DeferredGBuffer` EID `21` -> `45`, dispatches `2`
  - `Nullus/DeferredLighting` EID `52` -> `64`, draws `1`
  - `Nullus/EditorGridPass` EID `84` -> `170`, draws `7`
  - final editor UI colour pass EID `211` -> `325`, draws `35`

Additional RenderDoc replay API inspection reported:

```text
dispatch_count=2
dispatch eid=36 marker=- stack=Nullus/HZBBuild
dispatch eid=45 marker=- stack=Nullus/HZBOcclusion
```

This demonstrates the DX12 editor Scene View capture includes the HZB build and occlusion compute passes before deferred lighting.

## HZB Dispatch Evidence

Focused RenderDoc replay inspection reported:

- EID `36`: `ID3D12GraphicsCommandList::Dispatch`, `ThreadGroupCountX=235`, `ThreadGroupCountY=117`, `ThreadGroupCountZ=1`, root signature `844`, compute pipeline `845`.
- EID `45`: `ID3D12GraphicsCommandList::Dispatch`, `ThreadGroupCountX=235`, `ThreadGroupCountY=117`, `ThreadGroupCountZ=1`, root signature `844`, compute pipeline `847`.
- Resource `845`: `DX12 ComputePSO "HZBBuildPipeline" CS=...\HZBBuild.hlsl:CSMain`.
- Resource `847`: `DX12 ComputePSO "HZBOcclusionPipeline" CS=...\HZBOcclusion.hlsl:CSMain`.

Focused binding inspection reported the compute descriptor slots used by both dispatches:

```text
EID 36 cs ro set=1 slot=0 u_OpaqueDepth
EID 36 cs rw set=1 slot=1 u_HZBOutput
EID 45 cs ro set=1 slot=0 u_OpaqueDepth
EID 45 cs rw set=1 slot=1 u_HZBOutput
```

The EID `45` names above are RenderDoc binding labels for the shared compute layout. At the time of this capture,
`HZBOcclusion.hlsl` still declared an intermediate debug output texture, so the resource usage matrix below is the
authoritative evidence for which concrete textures EID `45` read and wrote in this historical capture:

```text
Resource 954 MultiFramebufferDepthTexture Depth24Stencil8:
  EID 24 Clear, EID 29/31 Barrier, EID 36 CS_Resource, EID 47 Barrier, EID 64 All_Resource
Resource 959 SceneHZB R32F:
  EID 31 Barrier, EID 36 CS_RWResource, EID 37 Barrier, EID 45 CS_Resource
Resource 960 SceneHZBOcclusionOutput R32F:
  EID 40 Barrier, EID 45 CS_RWResource
```

`rdc log --json` returned an empty array for this capture, so RenderDoc replay did not report debug/validation messages while inspecting the captured frame. This historical evidence demonstrates the DX12 runtime capture contained the intended opaque-depth read, HZB write, HZB read, and then-current occlusion-output write flow around the HZB dispatches.

Post-review code cleanup removed the unused intermediate `SceneHZBOcclusionOutput` texture and `u_OcclusionOutput`
binding. The current HZB occlusion contract is narrower: `HZBBuild` writes `SceneHZB` mip0, and `HZBOcclusion` reads
that HZB texture while writing the primitive result buffer used for asynchronous history publication.

## Capture Boundary Fix

Earlier captures were incomplete: `large-scene-hzb-dx12-runtime_DX12_capture.rdc` previously contained `42` events, `35` UI draws, and `0` dispatches. Root cause was RenderDoc queued capture starting only at a presentable frame. Editor Scene View renders offscreen before the final UI present, so the queued capture could miss Scene View rendering and capture only the final ImGui/UI pass.

The capture controller now starts a queued capture once the present-frame countdown has reached `1`, even if the next RHI work is an offscreen frame. RenderDoc native capture hotkeys are disabled so F11 is owned by Nullus, preventing duplicate immediate RenderDoc captures from racing the queued whole-frame capture. Manual F11 capture uses a `2`-present countdown so a shortcut triggered late in the current frame captures the next whole frame instead of the current frame's remaining UI present.

The latest validation log confirms the native key path is disabled:

```text
TryLoadRenderDoc: native capture keys disabled; Nullus queues captures via shortcuts
RenderDoc API connected: 1.7.0, native capture keys disabled
RenderDoc queued StartFrameCapture before presentable frame -> success
RenderDoc queued EndFrameCapture after present -> success, latest="...\large-scene-hzb-dx12-runtime_DX12_capture.rdc"
```

Follow-up capture-boundary hardening after manual F11 testing:

- `QueueCapture()` now rejects re-entry while a capture is queued, active, or waiting for `TriggerCapture()` fallback resolution.
- `OnPostFrame()` can end queued captures on pure offscreen/non-present frames and polls pending `TriggerCapture()` fallback state even when no present occurs.
- `EndStandaloneExplicitFrame()` now calls `OnPostFrame(willPresentSwapchain, outputMayBePresentedLater)` after submit/present handling so standalone non-present frames do not leave queued captures active indefinitely.
- Scene-to-UI handoff frames pass `outputMayBePresentedLater=true`, preserving the later UI present in the same whole-frame capture instead of ending immediately after the offscreen scene pass.

## Scope

This evidence is DX12-only. It validates runtime RenderDoc visibility of `HZBBuild` and `HZBOcclusion` dispatches and their resource contract in an experimental path. It does not claim Vulkan, OpenGL, Metal behavior, production feature-gate enablement, or final pass-order acceptance.

## Primitive Buffer Resource Contract Follow-Up

After the initial RenderDoc capture, the HZB occlusion shader/resource contract was tightened so the runtime pass no longer relies only on texture descriptors:

- `HZBOcclusion.hlsl` declares `StructuredBuffer<OcclusionPrimitiveInput> u_OcclusionPrimitiveInputs` and `RWStructuredBuffer<uint> u_OcclusionPrimitiveResults`.
- `HZBOcclusionCS` metadata now matches the shader-side primitive input stride: `6 * sizeof(float)` bytes.
- `DeferredSceneRenderer` creates renderer-owned sentinel primitive input/result buffers with deterministic zero initial uploads, binds them into the HZB occlusion descriptor set, and carries them in `HZBFrameResourceRequest`.
- `BuildHZBPreparedComputeDispatchSource` declares the primitive input buffer as compute shader-read and the result buffer as compute shader-write/UAV-barriered.
- Prepared threaded compute pass inputs preserve those buffer resource accesses for RHI-thread hazard/dependency handling.

Focused verification:

```powershell
& 'F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe' build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /nr:false /v:minimal *> build_hzb_sentinel_upload_green.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ShaderArchitectureAlignmentTests.HZBOcclusionMetadataMatchesPrimitiveBufferShaderContract:DeferredSceneRendererMaterialCacheTests.HZBOcclusionBindsPrimitiveInputAndResultBuffers:DeferredSceneRendererMaterialCacheTests.ReusesHZBFramePersistentDescriptorsUntilSourceTexturesChange *> hzb_sentinel_upload_green.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DeferredSceneRendererMaterialCacheTests.HZBFrameRequestCarriesRuntimePrimitiveOcclusionBuffers *> hzb_runtime_request_green.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=FrameGraphSceneTargetsTests.*HZB*:FrameGraphSceneTargetsTests.*PreparedCompute*Texture*:FrameGraphSceneTargetsTests.PreparedComputeThreadedPassInputCarriesPrimitiveOcclusionBufferAccesses:ShaderArchitectureAlignmentTests.*HZB*:ShaderBindingLayoutUtilsTests.*RWTexture*:DeferredSceneRendererMaterialCacheTests.*HZB* *> hzb_framegraph_runtime_regression2.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneOcclusionTests.*:SceneVisibilityPipelineTests.*Occlusion*:RenderSceneCacheTests.GatherVisibleCommandsConsumesOcclusionHistoryBeforeSubmission *> occlusion_history_runtime_regression2.local.log
```

Results:

```text
hzb_sentinel_upload_green.local.log: 3 tests passed.
hzb_runtime_request_green.local.log: 1 test passed.
hzb_framegraph_runtime_regression2.local.log: 17 tests passed.
occlusion_history_runtime_regression2.local.log: 22 tests passed.
```

## Renderer History Bridge Follow-Up

The renderer-side HZB bridge now carries primitive packets beyond the frame-graph descriptor contract:

- `BaseSceneRenderer` owns an HZB occlusion history and a pending observation batch across frames.
- `BaseSceneRenderer::ParseScene()` builds the current-frame occlusion frame identity, resolves RHI capability gates, and feeds valid renderer-owned history into `RenderScene::GatherVisibleCommands()`.
- `DeferredSceneRenderer::BeginFrame()` uploads the parsed primitive packets before HZB frame resources are prepared, then creates a pending observation batch from the same frame identity and primitive inputs when the HZB capability gate is valid.
- `DeferredSceneRenderer` exposes a tested completion path that merges ready primitive result flags into renderer-owned history without synchronous readback flags, GPU fence waits, or blocking maps.
- Primitive input/result buffers now resize to the uploaded packet count, refresh when same-count packet contents change, bind descriptor ranges over the full prepared buffers, and clear one result `uint32_t` per packet.

Focused verification:

```powershell
& 'F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe' build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /nr:false /v:minimal *> build_hzb_renderer_history_green4.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DeferredSceneRendererMaterialCacheTests.*HZB*:SceneOcclusionTests.*Observation*:SceneOcclusionTests.*Readback*:RenderSceneCacheTests.*Occlusion*:RenderSceneCacheTests.*HZB*:FrameGraphSceneTargetsTests.*HZB* *> hzb_renderer_history_regression2.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderSceneCacheTests.VisibilitySnapshotCarriesVisiblePrimitiveHandlesForHZBPacketSources:RenderSceneCacheTests.BaseSceneRendererParseSceneBuildsHZBOcclusionPacketsFromVisibleOpaquePrimitives:RenderSceneCacheTests.GatherVisibleCommandsConsumesOcclusionHistoryBeforeSubmission:SceneVisibilityPipelineTests.*Occlusion* *> hzb_visibility_bridge_regression.local.log
```

Results:

```text
build_hzb_renderer_history_green4.local.log: build passed.
hzb_renderer_history_regression2.local.log: 29 tests passed.
hzb_visibility_bridge_regression.local.log: 5 tests passed.
```

## DX12 Buffer Async Readback Follow-Up

The HZB GPU observation loop now has a dedicated buffer readback path instead of attempting to reuse texture `ReadPixels`:

- `RHIDevice::BeginReadBuffer()` was added with `RHIBufferReadbackDesc` for non-blocking buffer readback requests.
- DX12 implements `DX12ReadbackContext::BeginBuffer()` with `CopyBufferRegion()` into a readback heap, fence-backed completion, and `Poll()`-only finalization before mapping the readback heap.
- `NativeDX12ExplicitDevice::BeginReadBuffer()` forwards the public RHI call to the DX12 readback context.
- DX12 now advertises `RHIDeviceFeature::AsyncReadback=true` based on live texture and buffer async readback tests.
- `DeferredSceneRenderer` marks `SceneHZBOcclusionPrimitiveResults` as `Storage | CopySrc`, starts an async readback after the non-threaded HZB framegraph pass executes, polls previous completion at the start of later frames, and merges completed flags into renderer-owned occlusion history without calling `Wait()`.
- Unit coverage verifies pending readback does not alter history, completed readback publishes occlusion history, and ordinary renderer polling does not wait on the completion token.

Focused verification:

```powershell
& 'F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe' build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /nr:false /v:minimal *> build_hzb_buffer_readback_green.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12ReadbackUtilsTests.SourceContainsDedicatedBufferReadbackPath:DeferredSceneRendererMaterialCacheTests.HZBResultReadbackPollPublishesRendererHistoryWithoutWait:DeferredSceneRendererMaterialCacheTests.ReadyHZBOcclusionObservationBatchPublishesRendererHistory:DeferredSceneRendererMaterialCacheTests.*HZB*:SceneOcclusionTests.*Observation*:SceneOcclusionTests.*Readback*:RenderSceneCacheTests.*Occlusion*:RenderSceneCacheTests.*HZB*:FrameGraphSceneTargetsTests.*HZB* *> hzb_buffer_readback_green.local.log

& 'F:\Microsoft Visual Studio\2022\MSBuild\Current\Bin\MSBuild.exe' build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /nr:false /v:minimal *> build_hzb_dx12_buffer_live2.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12ReadbackUtilsTests.PollingCompletedAsyncBufferReadbackCopiesGpuBufferBytes:DX12ReadbackUtilsTests.SourceContainsDedicatedBufferReadbackPath:DeferredSceneRendererMaterialCacheTests.HZBResultReadbackPollPublishesRendererHistoryWithoutWait *> hzb_dx12_buffer_live2.local.log
```

Results:

```text
build_hzb_buffer_readback_green.local.log: build passed.
hzb_buffer_readback_green.local.log: 31 tests passed.
build_hzb_dx12_buffer_live2.local.log: build passed.
hzb_dx12_buffer_live2.local.log: 3 tests passed, including live DX12 GPUOnly buffer -> readback heap copy validation.
```

Post-review hardening on 2026-06-06:

- `RHIBufferReadbackDesc` now carries explicit `sourceState`.
- HZB primitive result readback requests set `sourceState=ShaderWrite` so DX12 readback inserts the explicit `ShaderWrite -> CopySrc -> ShaderWrite` transition instead of inferring state from `source->GetState()`.
- DX12 buffer readback pending-copy and quarantine records retain the source `RHIBuffer` shared pointer until completion/quarantine release, so async GPU copy lifetime does not depend on caller-owned references.

Focused verification:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1 /nodeReuse:false *> build_dx12_readback_contract_green.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12ReadbackUtilsTests.BufferReadbackSourceStateAndLifetimeAreExplicit:DX12ReadbackUtilsTests.PollingCompletedAsyncBufferReadbackCopiesGpuBufferBytes:DeferredSceneRendererMaterialCacheTests.HZBResultReadbackPollPublishesRendererHistoryWithoutWait *> dx12_readback_contract_green.local.log
```

Results:

```text
build_dx12_readback_contract_green.local.log: build passed.
dx12_readback_contract_green.local.log: 3 tests passed, including live DX12 GPUOnly buffer -> readback heap copy validation.
```

## Threaded Prepared Post-Submit Buffer Readback Follow-Up

Threaded/prepared frames now carry post-submit buffer readback requests on the `RenderScenePackage` so HZB result readback starts only after the RHI-thread submit path has accepted the frame:

- `PostSubmitBufferReadbackRequest` stores the `RHIBufferReadbackDesc`, shared result state, and destination keep-alive.
- `DeferredSceneRenderer` attaches the HZB primitive result readback request to prepared render-scene packages.
- `RhiThreadCoordinator::ExecuteThreadedSubmitPlan()` begins queued buffer readbacks after computing `submittedSuccessfully`.
- Failed threaded submits mark the shared readback state as attempted/failed without calling `RHIDevice::BeginReadBuffer()`.
- Renderer polling still consumes the shared completion state later without synchronous waits.

Focused verification:

```powershell
cmake --build build\windows --target NullusUnitTests --config Debug *> build_threaded_postsubmit_readback_behavior2.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.SuccessfulThreadedSubmitBeginsPostSubmitBufferReadback:ThreadedRenderingLifecycleTests.FailedThreadedSubmitMarksPostSubmitBufferReadbackFailedWithoutDeviceCall *> threaded_postsubmit_readback_behavior.local.log

build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=DX12PipelineLayoutUtilsTests.DX12HZBOcclusionCapabilitiesStayEvidenceGated:DX12ReadbackUtilsTests.PollingCompletedAsyncBufferReadbackCopiesGpuBufferBytes:DX12ReadbackUtilsTests.SourceContainsDedicatedBufferReadbackPath:DeferredSceneRendererMaterialCacheTests.HZBResultReadbackPollPublishesRendererHistoryWithoutWait:DeferredSceneRendererMaterialCacheTests.ThreadedPreparedHZBRequestsPostSubmitResultReadback:DeferredSceneRendererMaterialCacheTests.ReadyHZBOcclusionObservationBatchPublishesRendererHistory:DeferredSceneRendererMaterialCacheTests.*HZB*:SceneOcclusionTests.*Observation*:SceneOcclusionTests.*Readback*:SceneOcclusionTests.RhiCapabilitiesGateHZBOcclusionAndAsyncReadback:RenderSceneCacheTests.*Occlusion*:RenderSceneCacheTests.*HZB*:FrameGraphSceneTargetsTests.*HZB*:ThreadedRenderingLifecycleTests.SuccessfulThreadedSubmitBeginsPostSubmitBufferReadback:ThreadedRenderingLifecycleTests.FailedThreadedSubmitMarksPostSubmitBufferReadbackFailedWithoutDeviceCall *> hzb_threaded_readback_regression2.local.log
```

Results:

```text
build_threaded_postsubmit_readback_behavior2.local.log: build passed.
threaded_postsubmit_readback_behavior.local.log: 2 tests passed.
hzb_threaded_readback_regression2.local.log: 36 tests passed.
```

## Final DX12 HZB A/B Gate

Date: 2026-06-06
Backend: DX12 / D3D12
Build log: `build_hzb_ab_editor_final.local.log`

The final A/B pass was rerun after the validation disable path was fixed to skip HZB packet/resource preparation and after
`HZBOcclusion.hlsl` was tightened to use a biased strict depth comparison. RenderDoc replay was available through
`rdc doctor` with RenderDoc 1.43 and replay API support.

### HZB Enabled Capture

Capture:

- `build/RenderDocCaptures/editor/dx12/large-scene-hzb-ab-on-after-fix_DX12_capture.rdc`

Analysis:

- `build/RenderDocValidation/LargeSceneHZB/AB/hzb_ab_on_after_fix_summary.json`
- `t078f_hzb_ab_on_after_fix_capture.local.log`
- `t078f_hzb_ab_on_after_fix_analyze.local.log`
- Runtime log: `build/RenderDocValidation/LargeSceneHZB/Logs/2026-06-06_17-01-34.log`

Replay summary:

```text
API: D3D12
Events: 87
Draws: 43 (40 indexed, 3 non-indexed, 2 dispatches)
Passes:
  Nullus/DeferredGBuffer EID 21 -> 46, dispatches 2
  Nullus/DeferredLighting EID 54 -> 66, dispatches 0
  Nullus/EditorGridPass EID 86 -> 172, dispatches 0
  Colour Pass #4 EID 213 -> 327, dispatches 0
```

This proves the enabled path captured a Scene View frame with the HZB compute work submitted before deferred lighting.

### HZB Disabled Capture

The first disabled capture, `large-scene-hzb-ab-off-after-fix_DX12_capture.rdc`, proved `0 dispatches` but captured only
the final UI colour pass. It is recorded as an invalid A/B comparator because it did not include the Scene View offscreen
rendering work.

The valid disabled comparator is:

- `build/RenderDocCaptures/editor/dx12/large-scene-hzb-ab-off-after-fix-181_DX12_capture.rdc`

Analysis:

- `build/RenderDocValidation/LargeSceneHZB/AB/hzb_ab_off_after_fix_181_summary.json`
- `t078f_hzb_ab_off_after_fix_181_capture.local.log`
- `t078f_hzb_ab_off_after_fix_181_analyze.local.log`
- Runtime log: `build/RenderDocValidation/LargeSceneHZB/Logs/2026-06-06_17-03-53.log`

Replay summary:

```text
API: D3D12
Events: 120
Draws: 51 (45 indexed, 6 non-indexed, 0 dispatches)
Passes:
  Nullus/DeferredLighting EID 35 -> 47, dispatches 0
  Nullus/EditorGridPass EID 67 -> 153, dispatches 0
  Nullus/DeferredLighting EID 210 -> 222, dispatches 0
  Nullus/EditorGridPass EID 242 -> 328, dispatches 0
  Colour Pass #5 EID 342 -> 456, dispatches 0
```

Runtime log evidence for the disabled path repeatedly reports:

```text
[BaseSceneRenderer][HZB] disabled by editor validation override
[DeferredSceneRenderer][HZB] Prepare skipped: disabled by editor validation override
```

### Gate Result

The final DX12 A/B gate is accepted for this implementation phase:

- Enabled path: Scene View frame captured with `Nullus/DeferredGBuffer` containing `2` HZB dispatches before
  `Nullus/DeferredLighting`.
- Disabled path: Scene View comparator captured with deferred lighting and editor grid passes present, and `0`
  compute dispatches.
- Runtime logs confirm the disabled validation flag prevents renderer-side HZB packet/resource preparation.
- The UI-only disabled capture is explicitly excluded from the final comparator set.

This evidence covers DX12 only. It does not claim Vulkan, OpenGL, Metal, or cross-backend equivalence.

## Post-Review DX12 HZB A/B Recheck

Date: 2026-06-06
Backend: DX12 / D3D12

This recheck was run after the post-review fixes for the shared occlusion packet layout, conservative mip0 HZB
coverage contract, dispatch group coverage, sync-setting propagation, HZB telemetry accounting, readback state
tracking, and capability-default hardening. The earlier `--capture-after-frames 180` enabled capture timed out under
Debug DX12 GPU validation after reaching only 37 frames in 300 seconds, so the recheck uses a low-frame whole-frame
capture. Both captures were reported stable by `renderdoc_runner.py`.

### HZB Enabled Recheck Capture

Capture:

- `build/RenderDocCaptures/editor/dx12/large-scene-hzb-ab-on-after-review-fix-f10_DX12_capture.rdc`

Analysis:

- `build/RenderDocValidation/LargeSceneHZB/AB/hzb_ab_on_after_review_fix_f10_summary.json`
- `t078f_hzb_ab_on_after_review_fix_f10_capture.local.log`
- `t078f_hzb_ab_on_after_review_fix_f10_analyze.local.log`
- Runtime log: `build/RenderDocValidation/LargeSceneHZB/Logs/2026-06-06_17-56-10.log`

Replay summary:

```text
API: D3D12
Events: 135
Draws: 51 (45 indexed, 6 non-indexed, 4 dispatches)
Passes:
  Nullus/DeferredGBuffer EID 22 -> 47, dispatches 2
  Nullus/DeferredLighting EID 55 -> 67, dispatches 0
  Nullus/EditorGridPass EID 87 -> 173, dispatches 0
  Nullus/DeferredGBuffer EID 234 -> 259, dispatches 2
  Nullus/DeferredLighting EID 267 -> 279, dispatches 0
  Nullus/EditorGridPass EID 299 -> 385, dispatches 0
  Colour Pass #7 EID 398 -> 512, dispatches 0
```

This shows both captured Scene View instances run the `HZBBuild` and `HZBOcclusion` dispatch pair inside
`Nullus/DeferredGBuffer`, before `Nullus/DeferredLighting`.

### HZB Disabled Recheck Capture

Capture:

- `build/RenderDocCaptures/editor/dx12/large-scene-hzb-ab-off-after-review-fix-f10_DX12_capture.rdc`

Analysis:

- `build/RenderDocValidation/LargeSceneHZB/AB/hzb_ab_off_after_review_fix_f10_summary.json`
- `t078f_hzb_ab_off_after_review_fix_f10_capture.local.log`
- `t078f_hzb_ab_off_after_review_fix_f10_analyze.local.log`

Replay summary:

```text
API: D3D12
Events: 46
Draws: 8 (5 indexed, 3 non-indexed, 0 dispatches)
Passes:
  Nullus/DeferredLighting EID 30 -> 42, dispatches 0
  Nullus/EditorGridPass EID 62 -> 148, dispatches 0
```

The disabled capture still contains the offscreen Scene View deferred lighting and editor grid work, but no compute
dispatches. `rdc_analyze.py` records the disabled `Nullus/DeferredGBuffer` marker events and clears in the event list
rather than as a dispatch-bearing named pass because the HZB compute work is absent.

### Recheck Result

The post-review DX12 recheck preserves the expected A/B behavior:

- Enabled path: D3D12 capture with HZB dispatch pairs in `Nullus/DeferredGBuffer` before deferred lighting.
- Disabled path: D3D12 Scene View comparator with `0` dispatches.
- The low-frame recheck replaces the timed-out 180-frame attempt only for post-review evidence freshness; the previous
  181-frame disabled comparator remains valid historical evidence.

## Current Shader Contract After P1 Audit

Date: 2026-06-07
Raw output log: `hzb_mip0_coverage_targeted_green.local.log`

The current `HZBOcclusion.hlsl` no longer uses sparse corner/grid sampling. It performs bounded exhaustive coverage
over mip0 pixels:

- `IsConservativelyOccludedByHZBMip0Coverage` scans every covered mip0 pixel for small projected bounds.
- `kHZBOcclusionMaxMip0ScanPixels` caps the exhaustive scan at `64` pixels.
- Any empty or larger projected coverage returns visible conservatively.
- The shader writes only `u_OcclusionPrimitiveResults`; the old intermediate `SceneHZBOcclusionOutput` capture above is
  retained strictly as historical RenderDoc evidence.

Targeted contract result:

```text
[==========] 38 tests from 4 test suites ran. (160 ms total)
[  PASSED  ] 38 tests.
```
