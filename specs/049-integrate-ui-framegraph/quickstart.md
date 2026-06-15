# Quickstart: UI FrameGraph Validation

## Build And Unit Tests

From `D:\VSProject\Nullus`:

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=UiDrawDataSnapshotTests.*:RHIUiOverlayPassTests.*:RHIUiTextureRegistryTests.*:ThreadedRenderingLifecycleTests.*:FrameGraphSceneTargetsTests.*:ProfilerDestinationTest.*:RHIUiOverlaySourceGuardTests.* --gtest_break_on_failure=1
```

Expected coverage:

- UI snapshot survives ImGui frame turnover.
- Overlay pass order is scene passes then `RHIFrameGraph::UIOverlay`.
- UI-only frames use the normal swapchain package path.
- Texture registry retains/retires views safely.
- DX12 migrated path source guards exclude legacy direct-submit calls.

## Source Guard Checks

```powershell
rg -n "WaitForBackbufferReuse|WaitForAllocatorReuse|DX12UIBridge::ExecuteCommandLists|DX12UIBridge::SubmitCommandBuffer|m_uiBridge->RenderDrawData|SubmitUIRendering|PrepareUIRender|BeginStandaloneUiExplicitFrame|PresentStandaloneUiFrame|ExecuteCommandLists\\(|->ExecuteCommandLists|ID3D12CommandQueue::Signal|Signal\\(|m_queue->Signal|m_graphicsQueue->Signal|PresentInternal|->Present\\(|\\.Present\\(|swapchain->Present" Runtime Project Tests
```

Expected result after implementation:

- Legacy symbols may exist in fallback implementation/tests.
- Migrated DX12 Editor/Launcher call path must not invoke them.
- `RHIImGuiOverlayRenderer`, `RHIImGuiFontAtlas`, and migrated UI texture code must not contain native DX12 queue submit/signal/present calls.
- Legitimate hits in normal RHI queue, readback, descriptor initialization, profiler resolve, fallback bridge, or tests require call-path triage; they do not pass or fail the migrated UI path by name alone.
- Non-DX12 backends must report unsupported `UIOverlayFrameGraph` reasons unless separately migrated.
- `RHIFrameGraph::UIOverlay` must appear in render pass/profile naming and profiler classification.

## DX12 Editor TimelineProfiler Evidence

1. Launch Editor with DX12 and TimelineProfiler enabled.
2. Open the Profiler panel and leave dense UI visible for at least 300 frames.
3. Export or inspect `TestProject/Logs/trace.json`.
4. Record in `specs/049-integrate-ui-framegraph/validation/final-diagnostics.md`:
   - build/configuration
   - CPU/GPU, OS, driver version where available
   - backend actually selected
   - `UIOverlayFrameGraph` capability state and runtime-selectable reason
   - reproducible workload (Profiler panel visible, dock/layout, scene, window size)
   - frame count inspected
   - number and percentage of main UI frames containing `DX12UIBridge::WaitForBackbufferReuse`
   - presence of `RHIFrameGraph::UIOverlay`
   - graphics queue submit count per migrated swapchain frame
   - native UI bridge submit/present count per migrated swapchain frame
   - before/after frame-time and main-thread wait deltas from a pre-migration or explicit baseline trace
   - snapshot-copy CPU time, copied vertex/index counts, and overlay dynamic-buffer allocation/reallocation counts
   - any device-lost/quarantine/readback errors

Acceptance target: fewer than 1% of main UI render frames contain `DX12UIBridge::WaitForBackbufferReuse`.
Acceptance target: migrated frames have no second UI-owned queue submit, no standalone UI present, and no private UI upload fence wait.

## DX12 RenderDoc Evidence

Follow `Docs/Rendering/RenderDocDebugging.md`.

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 120 --timeout 180
```

If capture succeeds, analyze the latest `.rdc`:

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py <path-to-capture.rdc> --json-out Build\RenderDocAnalysis\ui-framegraph.json
```

Record:

- capture path
- UI overlay event/pass location relative to scene pass and present
- swapchain transition evidence for scene/editor render target -> `RHIFrameGraph::UIOverlay` render target -> present
- font atlas and registered UI texture shader-read state evidence, including subresource range or view range
- dynamic vertex/index buffer upload-to-read visibility evidence
- whether UI text/icons/texture previews are visible
- any high/medium RenderDoc diagnostics

If RenderDoc capture is unavailable, record the exact command, failure/timeout, scoped sign-off limitation, and equivalent RHI telemetry/test evidence for pass order, resource access records, exported transitions, and submit/present ownership in `validation/final-diagnostics.md`.

## Manual Product Smoke

- Editor DX12: scene view, game view, top bar, docked panels, profiler panel, texture preview.
- Launcher DX12: main window UI remains visible and responsive.
- Game DX12: launches, renders, presents, and exits without device-lost/quarantine logs.
- Resize while UI-only frame activity is present.
- Shutdown after UI texture previews were shown.

Do not claim Vulkan, DX11, OpenGL, or Metal UI overlay support from these DX12 checks.
