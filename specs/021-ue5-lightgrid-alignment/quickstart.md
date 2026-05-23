# Quickstart: UE5 LightGrid Alignment

## Automated Validation

```powershell
cmake --build .\Build --config Debug --target NullusUnitTests
.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=LightingDataProviderTests.*:RenderFrameworkContractTests.*LightGrid*:FrameGraphSceneTargetsTests.*LightGrid*:ThreadedRenderingLifecycleTests.*LightGrid*
```

Expected evidence:

- Default LightGrid settings match UE defaults: 64 pixel cells, 32 Z slices, 32 fixed capacity, linked-list mode enabled.
- XY grid dimensions derive from render size.
- Disabled LightGrid still produces a valid render package with no compute dispatch source.
- Forward and deferred scene packages resolve LightGrid pass bindings only for scene draw commands.

## Shader/Build Validation

```powershell
cmake --build .\Build --config Debug --target Editor
```

Expected evidence:

- `LightGridInjection.hlsl`, `LightGridCompact.hlsl`, and graphics shaders compile through the normal shader pipeline.
- No generated files under `Gen/` are hand-edited.

## Runtime Evidence

```powershell
rdc doctor
py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 180
```

Expected evidence:

- DX12 capture contains LightGrid culling/compaction compute work before scene lighting consumes the graphics binding set.
- If capture automation fails, record the blocker and do not claim visual/runtime parity.
