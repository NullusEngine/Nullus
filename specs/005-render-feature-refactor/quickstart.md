# Quickstart: Render Feature Refactor

## Purpose

Validate the render feature ownership refactor while preserving Editor/Game runtime behavior. The final state removes `ARenderFeature` and the `CompositeRenderer` feature registry.

## Prerequisites

- Repository root: `D:/VSProject/Nullus`
- Existing build directory configured as `build`
- A usable test project for Editor/Game smoke checks
- Supported backend focus on current validated runtime matrix

## Build And Test Commands

### 1. Rebuild focused targets

```powershell
cmake --build build --config Debug --target NLS_Render NLS_Engine NullusUnitTests -- /m:1
```

### 2. Run unit coverage

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

### 3. Run focused rendering tests during migration slices

```powershell
Build/bin/Debug/NullusUnitTests.exe --gtest_filter=CompositeRendererExplicitDrawOrderTests.*:RendererFrameObjectBindingTests.*:DebugDrawTypesTests.*:DebugDrawPassTests.*:LightingDataProviderTests.*:RendererStatsTests.*:ScenePipelineStatePresetsTests.*:PanelWindowHookTests.*
```

Run the source-level removal check:

```powershell
rg -n "ARenderFeature|AddFeature<|GetFeature<|HasFeature<|RemoveFeature<|m_features|DrawHookPolicy" Runtime Project Tests -g"*.h" -g"*.cpp"
```

Expected: no matches, exit code `1`.

## Manual Validation Checklist

### Core Scene Rendering

1. Launch `Editor.exe` with a valid project on a supported backend.
2. Open a scene containing regular mesh objects.
3. Confirm camera motion, object transforms, skybox, and ordinary scene rendering still behave correctly.

### Debug Draw

1. Open an editor scene that renders grid/debug helpers.
2. Confirm grid and debug primitives still appear.
3. Select a camera and confirm its frustum helper appears through the debug draw path.
4. Select point, spot, directional, ambient box, and ambient sphere lights where available; confirm their range/direction/volume helpers appear through the debug draw path.
5. Select a mesh object with bounds visualization enabled and confirm its helper uses the same debug draw visibility controls.
6. Toggle the global debug draw setting and per-category settings; confirm filtering works consistently for grid, camera, lighting, and bounds helpers.
7. Confirm one-frame, timed, and persistent debug primitives still respect lifetime behavior.

### Lighting

1. Open a scene with at least one supported light.
2. Confirm forward rendering still shows expected lit output.
3. If deferred rendering is available in the validation scene, confirm deferred lighting still receives scene lighting data.
4. Repeat with an unlit or no-light scene and confirm rendering still behaves safely.

### Renderer Statistics

1. Render a populated scene and record non-zero batch/instance metrics from the existing consumer path.
2. Render an empty or near-empty scene and confirm zero or reduced metrics.

## RenderDoc Guidance

Use RenderDoc only when a migration slice claims visual correctness for supported rendering behavior and screenshots/logs are not sufficient.

Reference:

- `D:/VSProject/Nullus/Docs/Rendering/RenderDocDebugging.md`

## Slice Completion Criteria

A migration slice is ready for the next slice only when:

- unit tests pass
- relevant focused tests pass
- Editor/Game remain runnable for the affected path
- manual validation covers any moved ownership boundary
- no runtime, project, or test C++ code reintroduces `ARenderFeature` or `CompositeRenderer` feature registry APIs
