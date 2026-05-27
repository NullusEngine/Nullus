# Quickstart: Validation

## Baseline Evidence

The supplied baseline trace is `App/Win64_Debug_Runtime_Shared/trace.json`.

Observed main-thread baseline:

- `CPU Frame`: 40 frames, average 47.535 ms.
- `Panel::Draw:Scene View`: average 34.512 ms.
- `AView::RendererBeginFrame`: average 25.808 ms.
- `DeferredSceneRenderer::EnsureGBufferTargets`: average 6.344 ms.
- `BaseSceneRenderer::CaptureThreadedPreparedDraw`: about 114 calls per frame, average total 11.683 ms.

## Automated Tests

Run focused tests after adding the failing tests:

```powershell
.\App\Win64_Debug_Runtime_Shared\NullusUnitTests.exe --gtest_filter=DeferredSceneRendererMaterialCacheTests.*:ProfilerDestinationTest.*
```

If the executable path differs, build or locate `NullusUnitTests` from the active CMake preset/build output and run the same filters.

## Runtime Trace Check

When practical, run the editor with the same project and backend used for the baseline trace, keep the Scene View size stable, and capture a short timeline trace.

Compare:

- `AView::RendererBeginFrame`
- `DeferredSceneRenderer::EnsureGBufferTargets`
- `UIBridge::RenderDrawData`
- total main-thread `CPU Frame`

Document the backend and exact command used. Do not use one backend as proof for another.
