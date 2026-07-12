# HZB Occlusion Validation

This document is the maintained evidence record and sign-off checklist for large-scene HZB occlusion. Evidence is backend-specific: a DX12 capture does not prove Vulkan, OpenGL, Metal, or DX11 behavior.

## Current Gate

DX12 `HierarchicalZBuffer` and `ConservativeOcclusion` are enabled only when the runtime capability checks pass. Unsupported capability combinations remain conservative: primitives stay visible and telemetry records the fallback reason.

The accepted DX12 A/B evidence from 2026-06-06 showed:

- Enabled: a D3D12 Scene View capture with `HZBBuild` and `HZBOcclusion` dispatches inside `Nullus/DeferredGBuffer` before `Nullus/DeferredLighting`.
- Disabled: a D3D12 Scene View comparator containing deferred lighting and editor grid work with zero compute dispatches.
- Runtime logs confirmed that the validation disable flag skipped renderer-side HZB packet and resource preparation.

This historical gate remains DX12-only. New changes to HZB shaders, bindings, resource transitions, dispatch dimensions, readback, or pass ordering require fresh evidence.

## Capture

Use one validation project for both sides of the A/B run. In that project's `UserSettings/editor-settings.json`, set `objects["editor.large-scene"].fields.enableHZBOcclusion` to `true` before launching either capture. Keep the same project file, scene, camera, build, occlusion stack size, and capture frame for both runs.

Build the editor:

```powershell
cmake --build Build --target Editor --config Debug -- /m:1 /nr:false
```

Run the enabled capture:

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py `
  --target editor `
  --backend dx12 `
  --project path\to\HZBValidation.nullus `
  --capture `
  --capture-after-frames 10 `
  --capture-label large-scene-hzb-dx12-ab `
  --terminate-after-capture `
  --timeout 300 `
  --app-arg=--editor-validation-exclusive-view `
  --app-arg=scene `
  --app-arg=--editor-validation-focus-view `
  --app-arg=scene `
  --app-arg=--editor-validation-occlusion-stack `
  --app-arg=6 `
  --app-arg=--renderdoc
```

Archive the enabled `.rdc` under an `enabled` name before starting the comparator. Then run the disabled capture with the same arguments and only the HZB disable override added:

```powershell
py -3 Tools/RenderDoc/renderdoc_runner.py `
  --target editor `
  --backend dx12 `
  --project path\to\HZBValidation.nullus `
  --capture `
  --capture-after-frames 10 `
  --capture-label large-scene-hzb-dx12-ab `
  --terminate-after-capture `
  --timeout 300 `
  --app-arg=--editor-validation-exclusive-view `
  --app-arg=scene `
  --app-arg=--editor-validation-focus-view `
  --app-arg=scene `
  --app-arg=--editor-validation-occlusion-stack `
  --app-arg=6 `
  --app-arg=--renderdoc `
  --app-arg=--editor-validation-disable-hzb-occlusion
```

Do not change the project setting to produce the disabled comparator; the command-line disable override is the sole runtime behavior difference between the two runs.

Analyze the resulting capture and keep the JSON beside the validation output:

```powershell
py -3 Tools/RenderDoc/rdc_analyze.py path\to\capture.rdc `
  --json-out Build/RenderDocAnalysis/large-scene-hzb-dx12.json
```

If the Debug validation workload cannot reach a high frame count within the timeout, use a low-frame whole-frame capture. Do not accept a UI-only capture as the disabled comparator.

## Required Evidence

Record the following for both enabled and disabled captures:

- Date, commit, platform, requested backend, and actual backend.
- Capture path, runtime log path, analysis JSON path, and exact launch command.
- Scene View passes proving that offscreen scene rendering is present, not only the final UI pass.
- Dispatch event IDs, pipeline names, thread-group dimensions, and pass order.
- Bound HZB depth/input/output resources and their relevant transitions or access states.
- Replay validation messages and runtime capability/fallback telemetry.

The enabled capture must show:

- Qualified opaque depth-producing geometry feeding HZB.
- `HZBBuild` writing the HZB resource.
- `HZBOcclusion` reading HZB and writing primitive results.
- HZB compute work occurring before deferred lighting consumes the frame.

The disabled comparator must show the same scene rendering path with zero HZB dispatches. A capture containing only ImGui or the final color pass is invalid.

## Runtime Contract

The current implementation requires:

- HZB capability selection from `RHIDevice::GetCapabilities()` and texture-format support, never backend-name guesses.
- Explicit texture and buffer accesses, visibility transitions, dependency edges, and subresource ranges in prepared compute passes.
- Asynchronous primitive-result buffer readback with no synchronous wait in the renderer polling path.
- Source resource lifetime retained until readback completion or quarantine release.
- Conservative visibility when history is missing, stale, unsupported, or incomplete.

Focused behavior verification should include the HZB cases in `DeferredSceneRendererMaterialCacheTests`, `FrameGraphSceneTargetsTests`, `SceneOcclusionTests`, `RenderSceneCacheTests`, and `ThreadedRenderingLifecycleTests`. Run the maintained default workflow in `Docs/Testing.md` in addition to capture-specific checks.

## Evidence Record Template

Append a dated section for each accepted gate:

```text
Date:
Commit:
Platform:
Backend requested / actual:
Enabled capture / analysis / log:
Disabled capture / analysis / log:
Enabled pass and dispatch summary:
Disabled comparator summary:
Resource and transition findings:
Replay validation messages:
Result and remaining backend gaps:
```
