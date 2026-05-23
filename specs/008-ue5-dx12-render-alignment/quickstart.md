# Quickstart: UE5-Style DX12 Render Alignment

## Intent

This feature bundle is a planning and architecture-alignment package for making Nullus render like a UE5-style DX12 engine mainline:

- one authoritative Game Thread -> Render Thread -> RHI Thread ownership model,
- one authoritative RDG scheduler,
- one authoritative Editor + Game frame pipeline,
- zero runtime fallback or compatibility render paths in the accepted DX12 mainline,
- centralized PSO, descriptor, transient-lifetime, and retirement systems.

## Current Status

Implementation is now landed through Phase 7:

- one authoritative Game Thread -> Render Thread -> RHI Thread lifecycle is in place,
- RDG owns scheduling/import/extract/readback authority,
- Editor and Game consume the same frame pipeline surfaces,
- DX12-only phase-1 startup gating is active,
- centralized descriptor, PSO, transient-lifetime, and retirement diagnostics are wired into `ThreadedFrameTelemetry`, `RendererStats`, and `FrameInfo`.

Phase 8 acceptance is now closed. This bundle is now backed by:

- DX12 Editor runtime validation passes,
- DX12 Game runtime validation passes,
- RenderDoc evidence exists for visible correctness,
- explicit DX12 startup-failure validation is captured,
- and the planned UE source audit is recorded.

Current acceptance progress on 2026-04-23:

- Editor DX12 startup now survives the previously blocking FrameGraph assertion on imported offscreen outputs.
- Editor RenderDoc capture is working again when startup capture is armed on frame `1`.
- Game DX12 threaded RenderDoc capture now contains representative scene draws, and explicit non-DX12 startup rejection has been observed.
- Backend-specific ImGui bridge execution has now been moved out of `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` and into backend-local bridge files.
- Common driver/UI coordination no longer carries an OpenGL-specific texture-handle special case.
- Launcher project creation now exposes only the DX12 phase-1 runtime backend.
- Dead legacy compatibility shim files and dead legacy-output fallback plumbing have been removed from the RDG-driven mainline.
- Scene pass scheduling descriptors are now frame-graph owned.
  - `ScenePassSchemas.h` is reduced to descriptive taxonomy and pass-kind mapping.
  - `SceneRenderGraphBuilder.*` is now the only shared seam that publishes the authoritative forward/deferred scene-pass descriptors used by runtime, editor debug helpers, and contract tests.
- T041 is now closed with a representative threaded Game scene frame.
- T040 is now closed with explicit Editor scene/game/offscreen/picking/selection-overlay evidence.

## Build Commands

```powershell
cmake --build Build\windows --config Debug --target NLS_Engine -- /m:4
cmake --build Build\windows --config Debug --target Editor -- /m:4
cmake --build Build\windows --config Debug --target Game -- /m:4
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:4
```

## Target Automated Validation Surface

The feature is expected to land or extend focused suites in these areas:

- `GraphicsBackendUtilsTests.*`
- `ThreadedRenderingLifecycleTests.*`
- `FrameGraphSceneTargetsTests.*`
- `RendererFrameObjectBindingTests.*`
- `CompositeRendererExplicitDrawOrderTests.*`
- `PanelWindowHookTests.*`
- `UE5RenderArchitectureContractTests.*`
- `EditorRenderPathContractTests.*`

## Focused Test Evidence

Validated on 2026-04-23 with:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="UE5RenderArchitectureContractTests.CentralPipelineStateCreationRemainsOnPipelineCacheMainline:UE5RenderArchitectureContractTests.ReadbackSelectionRemainsOnFrameGraphAndFramebufferMainline:FrameGraphSceneTargetsTests.PreferredReadbackRegistrationDeduplicatesAndPromotesReadbackTexture:FrameGraphSceneTargetsTests.FinalizePreparedForwardScenePackageRegistersExtractionVisibilityAndReadbackSource:EditorRenderPathContractTests.EditorReadbackPrefersGraphExtractionBeforeSwapchainFallback:EditorRenderPathContractTests.EditorReadbackHonorsPreferredReadbackTextureBeforeGenericExtractions:ThreadedRenderingLifecycleTests.ThreadedPreparedFramePublishesCompletedPreferredReadbackTexture:RendererStatsTests.RendererStatsTracksInfrastructureMainlineDiagnostics:ThreadedRenderingLifecycleTests.SubmissionDiagnosticsCaptureDescriptorAndTransientLifetimeStats"
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="UE5RenderArchitectureContractTests.CentralDescriptorBindingCreationRemainsOnDriverMainline:UE5RenderArchitectureContractTests.CentralPipelineStateCreationRemainsOnPipelineCacheMainline:UE5RenderArchitectureContractTests.BindingPipelineAndTransientBypassesRemainRejectedInAcceptedSources:RendererFrameObjectBindingTests.ExplicitBindingSetCreationRequiresCentralDescriptorAllocator:RendererStatsTests.RendererStatsTracksInfrastructureMainlineDiagnostics:ThreadedRenderingLifecycleTests.CreateExplicitBindingSetTracksTransientAndPersistentDescriptorLifetime:ThreadedRenderingLifecycleTests.SubmissionDiagnosticsCaptureDescriptorAndTransientLifetimeStats"
```

Observed result:

- both focused filters passed
- the descriptor mainline now rejects missing-allocator binding creation instead of returning a raw backend binding set
- centralized pipeline/readback/transient diagnostics remained green after the T033/T037 cleanup

Additional focused cleanup validation on 2026-04-23:

```powershell
cmake --build Build\windows --config Debug --target Editor NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="*HarnessPublish*:*DriverHarnessScenePackageResolutionIsRejected:*Dx12PreparedBuilderDrainDoesNotUseHarnessFallbackAttribution:*SynchronousDrainMarksHarnessRejectionAndSubmissionAttribution:*DriverRhiWorkerSkipsRejectedHarnessOffscreenFramesWithoutAcquireOrPresent:*PreparedBuilderResolutionIgnoresHarnessFallbackBuilders:*DeferredThreadedOffscreenPackageCarriesExternalOutputAttachmentViews:*ForwardThreadedOffscreenPackageRegistersExternalOutputExtraction:*ThreadedResizeWaitsForInFlightFrameRetirementBeforeResizingSwapchain"
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="UIAndToolingBackendAwarenessTests.*:*PreparedBuilderResolutionIgnoresHarnessFallbackBuilders:*HarnessPublish*:*DriverHarnessScenePackageResolutionIsRejected:*ThreadedResizeWaitsForInFlightFrameRetirementBeforeResizingSwapchain"
cmake --build Build\windows --config Debug --target Editor Launcher NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="ProjectCreationWizardValidationTests.*:UIAndToolingBackendAwarenessTests.*"
```

Observed result:

- `Editor` and `NullusUnitTests` both rebuilt successfully
- the 13-test focused cleanup filter passed after the T042 internal naming and fallback-material cleanup slice
- the backend-awareness plus harness regression filter also passed after the `RHIUIBridge` native-backend routing cleanup
- `Editor`, `Launcher`, and `NullusUnitTests` rebuilt successfully after the backend-local UI bridge split and launcher backend-picker tightening
- `ProjectCreationWizardValidationTests.*` and `UIAndToolingBackendAwarenessTests.*` both passed after the latest T042 cleanup slice
- `FrameGraphSceneTargetsTests.*` also passed after removing dead legacy-output fallback hooks from the scene graph / renderer seam

Post-T040 verification on 2026-04-23:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests Editor Launcher -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="EditorRenderPathContractTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests`, `Editor`, and `Launcher` rebuilt successfully
- the 16-test verification filter passed
- the direct post-acceptance regression surface stayed green for:
  - editor path contracts
  - shared view/frame-info panel lifecycle
  - RenderDoc/UI backend-awareness seams
  - launcher-side project creation validation

Post-cleanup verification on 2026-04-23 after removing the unused runtime validation capture scheduler:

```powershell
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="EditorRenderPathContractTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- the same 16-test regression surface passed again after the startup-only validation seam cleanup
- no stale references remained to the removed runtime validation scheduler symbols

Post-readiness cleanup verification on 2026-04-23 after removing fallback semantics from runtime startup gating:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="GraphicsBackendUtilsTests.*:EditorRenderPathContractTests.*:DriverNullDeviceReadinessTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully
- the combined readiness/editor/tooling regression surface passed `50 / 50`
- runtime startup gating now reports explicit readiness failure instead of exposing `Fallback` semantics in accepted DX12-path APIs

Post-RHI-seam cleanup verification on 2026-04-23 after removing the extra executor-backend enum hop from `Driver`:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="GraphicsBackendUtilsTests.*:EditorRenderPathContractTests.*:DriverNullDeviceReadinessTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully after the RHI executor factory signature cleanup
- the same combined regression surface passed `50 / 50`
- `Driver` no longer performs native-backend to executor-enum translation before crossing into the RHI factory seam

Post-threaded-path cleanup verification on 2026-04-23 after removing backend-named parallel execution modes from common orchestration:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="GraphicsBackendUtilsTests.*:EditorRenderPathContractTests.*:DriverNullDeviceReadinessTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully after the threaded-path mode cleanup
- the same combined regression surface passed `50 / 50`
- common threaded orchestration no longer carries `DX12OrderedSubmit` / `VulkanOrderedSubmit` mode names

Post-native-backend translation cleanup verification on 2026-04-23 after moving `NativeBackendType` naming helpers into `GraphicsBackendUtils.h`:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="GraphicsBackendUtilsTests.*:EditorRenderPathContractTests.*:DriverNullDeviceReadinessTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully after the native-backend helper cleanup
- the same combined regression surface passed `50 / 50`
- `Driver` no longer keeps local native-backend translation switches for active-backend reporting or RenderDoc backend naming

Post-scene-pass descriptor cleanup verification on 2026-04-24 after moving authoritative scene-pass descriptors out of `ScenePassSchemas.h`:

```powershell
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="FrameGraphSceneTargetsTests.SceneRenderGraphBuilderExposesExpectedForwardAndDeferredOrdering:FrameGraphSceneTargetsTests.PrepareForwardSceneGraphImportsExternalTargetsAndPreservesForwardPassOrder:UE5RenderArchitectureContractTests.RuntimeCompilationRebuildsPreparedPassPlanFromForwardSceneMetadata:GraphicsBackendUtilsTests.*:EditorRenderPathContractTests.*:DriverNullDeviceReadinessTests.*:PanelWindowHookTests.*:UIAndToolingBackendAwarenessTests.*:ProjectCreationWizardValidationTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully after the scene-pass descriptor ownership cleanup
- the combined regression surface passed `53 / 53`
- `ScenePassSchemas.h` no longer owns queue policy, graph pass names, clear/load behavior, or output propagation for forward/deferred scene passes

Post-closure source-purity cleanup verification on 2026-04-27:

```powershell
cmake -S . -B Build\windows
cmake --build Build\windows --config Debug --target NullusUnitTests -- /m:1
Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter="UE5RenderArchitectureContractTests.*:ThreadedRenderingLifecycleTests.*:FrameGraphSceneTargetsTests.*:EditorRenderPathContractTests.*:GraphicsBackendUtilsTests.*:RendererFrameObjectBindingTests.*:RHIPipelineStateUtilsTests.*"
```

Observed result:

- `NullusUnitTests` rebuilt successfully after refreshing the CMake source list for deleted files.
- the focused UE5-alignment regression surface passed `192 / 192`.
- dead recorded-command-list surfaces were removed from `Runtime/Rendering/RHI/Core`.
- the top-level RHI device factory no longer carries Vulkan/Metal inactive-backend creation code and remains DX12-only for phase 1.
- legacy DX11/OpenGL explicit RHI factory files were removed from the accepted source surface.
- accepted DX12/RHI factory sources no longer carry `fallback` terminology.

## Planned Runtime Validation

### Editor DX12

```powershell
$env:NLS_RENDERDOC_ENABLE='1'
$env:NLS_RENDERDOC_CAPTURE='1'
$env:NLS_RENDERDOC_CAPTURE_AFTER_FRAMES='1'
$env:NLS_RENDERDOC_CAPTURE_DIR='D:\VSProject\Nullus\Build\RenderDocCaptures\editor\dx12_validation_scene_selected_startup'
$env:NLS_RENDERDOC_CAPTURE_LABEL='editor_validation_scene_selected_startup'
$env:NLS_RENDERDOC_AUTO_OPEN='0'
App\Win64_Debug_Runtime_Static\Editor.exe --backend dx12 --editor-validation-focus-view scene --editor-validation-select-actor "Validation Cube" D:\VSProject\Nullus\TestProject\RenderValidation.nullus
py -3 Tools/RenderDoc/rdc_analyze.py Build\RenderDocCaptures\editor\dx12_validation_scene_selected_startup\editor_validation_scene_selected_startup_DX12_capture.rdc
```

Validated on 2026-04-23 with these captures and logs:

- Baseline scene-view startup capture remained healthy after the imported-output fix:
  - `Build\RenderDocCaptures\editor\dx12_sceneview_nonthreaded\editor_dx12_sceneview_nonthreaded_DX12_capture.rdc`
  - `D3D12`, `44` events, `15` draws, `3` explicit color passes
- Scene view with selected actor, gizmo, outline, and picking enabled:
  - `Build\RenderDocCaptures\editor\dx12_validation_scene_selected_startup\editor_validation_scene_selected_startup_DX12_capture.rdc`
  - `D3D12`, `67` events, `20` draws, `12` copies
  - main scene pass grew to `11` draws / `5430` triangles
  - startup log recorded:
    - `Editor validation pre-focused Scene View.`
    - `Editor validation pre-selected actor: Validation Cube`
    - model loads for `Sphere`, `Arrow_Translate`, and `Arrow_Picking`
- Scene view with selected actor but picking disabled:
  - `Build\RenderDocCaptures\editor\dx12_validation_scene_selected_no_picking\editor_validation_scene_selected_no_picking_DX12_capture.rdc`
  - `D3D12`, `54` events, `15` draws, `10` copies
  - color+depth pass count dropped from `2` to `1`
  - this capture delta is the acceptance evidence that editor picking remains a graph-visible offscreen path instead of a bypass
- Game view startup path with picking disabled:
  - `Build\RenderDocCaptures\editor\dx12_validation_gameview_no_picking\editor_validation_gameview_no_picking_DX12_capture.rdc`
  - `D3D12`, `38` events, `11` draws, `6` copies
  - `1` color+depth scene pass plus `1` UI color pass
  - startup log recorded `Editor validation pre-focused Game View.`
- Editor threaded runtime smoke after the descriptor-allocator fix stayed alive with no repeating allocator-missing warnings:
  - `TestProject\Logs\2026-04-23_18-33-26.log`

Acceptance interpretation:

- offscreen editor surfaces remain on the unified external-framebuffer path through `AView`
- scene-view selection overlays are visible on the same DX12/RDG mainline as runtime scene rendering
- picking adds a distinct color+depth offscreen pass plus extra copy/readback traffic instead of a side-channel submit path
- no sampled Editor validation log introduced fallback, compatibility present/acquire, or direct-submit wording

### Game DX12

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target game --backend dx12 --capture --capture-after-frames 1 --timeout 90
py -3 Tools/RenderDoc/rdc_analyze.py Build\RenderDocCaptures\game\dx12\game_dx12_DX12_capture.rdc
```

Validation goals:

- visible runtime frame renders correctly
- offscreen rendering remains valid
- no logs indicate fallback, compatibility present/acquire, or direct-submit path usage

Observed evidence so far:

- Fresh threaded validation on 2026-04-23 produced:
  - `Build\RenderDocCaptures\game\dx12_threaded_fixed\game_dx12_threaded_fixed_DX12_capture.rdc`
  - `rdc_analyze.py` reported `D3D12`, `8` events, `2` draws, and `1` explicit color pass
  - sampled draws:
    - `DrawIndexed` with `12` triangles
    - `Draw` with `12` triangles
- Root cause for the previously empty Game captures:
  - threaded prepared-frame material binding creation was asking `DriverRendererAccess` for a descriptor allocator before `explicitFrameActive` became true
  - this dropped material binding sets during Render/Build-stage package construction and silently removed scene draws from the accepted DX12 mainline
  - the accepted path now resolves the owning frame-slot descriptor allocator for threaded prepared-frame binding creation

### DX12 Failure Path

Validation goals:

- an unavailable or explicitly disabled DX12 path stops startup truthfully
- startup does not continue by selecting another backend path

Observed evidence so far:

```powershell
App\Win64_Debug_Runtime_Static\Game.exe --backend vulkan
```

- exit code: `1`
- stderr:

```text
[Game main] Game CLI only supports DX12 during UE5 alignment phase 1. Requested backend: Vulkan.
```

## Planned Manual Validation Checklist

- Verify scene view and game view share the same frame lifecycle telemetry.
- Verify offscreen output uses the same import/extract and retirement path as visible output.
- Verify picking readback retires safely with the owning frame.
- Verify gizmo, grid, outline, and overlays remain graph-visible work.
- Verify resize and shutdown drain in-flight DX12 work without reviving compatibility branches.
- Verify logs and diagnostics show zero accepted fallback-path events.

## Closure Evidence

- Focused unit test results
- DX12 Editor runtime evidence
- DX12 Game runtime evidence
- RenderDoc captures for Editor and Game
- Explicit DX12 startup-failure evidence
- Documented UE source-audit notes showing final parity review
