# Quickstart: Deferred Editor Overlays

## Automated Validation

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*DefaultSceneRenderer*:ThreadedRenderingLifecycleTests.*SceneRenderer*:RendererFrameObjectBindingTests.*Deferred*:RendererFrameObjectBindingTests.*Forward*
Build\bin\Debug\NullusUnitTests.exe
```

## Manual Editor Check

1. Launch the editor with the current DX12 backend.
2. Open Scene View and Game View on the same scene.
3. Verify Scene View shows deferred lighting output consistent with Game View.
4. Verify grid, light icons/helpers, camera helpers, selected actor outline, debug draw, and picking still work.

## RenderDoc Follow-up

If visual behavior is suspicious after automated tests pass, capture Scene View and verify pass order:

1. Deferred GBuffer
2. Deferred Lighting
3. Editor grid/helper/outline/debug draw
4. Optional picking pass
