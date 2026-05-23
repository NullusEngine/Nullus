# Quickstart: UE4.27 Deferred Lighting Alignment

## Targeted Test Loop

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\Win64_Debug_Runtime_Shared\Tests\Unit\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*Deferred*:LightingDataProviderTests.*LightGrid*:EditorRenderPathContractTests.*Deferred*
```

Expected result: the target builds and the filtered tests pass.

## Editor Build

```powershell
cmake --build Build --target Editor --config Debug
```

Expected result: the editor target builds for the local Windows DX12 workflow.

## RenderDoc Capture

```powershell
py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --project TestProject\TestProject.nullus --capture --capture-after-frames 60 --timeout 120
```

Then analyze the emitted capture:

```powershell
py -3 Tools\RenderDoc\rdc_analyze.py <capture-path>
```

Expected capture evidence:
- Event tree contains `Nullus/LightGridReset`, `Nullus/LightGridInjection`, `Nullus/LightGridCompact` when light grid is enabled.
- Event tree contains `Nullus/DeferredGBuffer` before `Nullus/DeferredLighting`.
- Editor overlay/debug pass names remain visible after `Nullus/DeferredLighting`.
- GBuffer albedo has non-zero values and final lit SceneColor exceeds ambient-floor-only output.
