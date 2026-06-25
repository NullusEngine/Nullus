# Implementation Plan: Deferred Transparent and Decal Passes

**Branch**: `044-deferred-transparent-pass` | **Date**: 2026-06-06 | **Spec**: `specs/044-deferred-transparent-pass/spec.md`
**Input**: Feature specification from `specs/044-deferred-transparent-pass/spec.md`

## Summary

Deferred rendering currently has no explicit distinction between ordinary transparent surfaces and blendable mesh decals. Keep the post-lighting Transparent pass for real transparent materials, add explicit material surface classification, and schedule deferred decals between GBuffer and Lighting so their albedo GBuffer contribution participates in deferred lighting.

## Technical Context

**Language/Version**: C++17
**Primary Dependencies**: Nullus rendering frame graph, threaded rendering lifecycle, RHI abstractions
**Storage**: N/A
**Testing**: GoogleTest unit tests in `Tests/Unit`
**Target Platform**: Editor/runtime rendering, with DX12 evidence from the supplied capture
**Project Type**: Desktop engine/editor
**Performance Goals**: Avoid extra passes when no transparent or decal drawables exist; preserve existing command batching behavior
**Constraints**: Do not hand-edit generated `Runtime/*/Gen/`; do not disturb unrelated prefab/editor changes in the dirty worktree
**Scale/Scope**: Material classification, scene queue routing, deferred scene pass scheduling, and renderer draw capture

## Constitution Check

- Rendering pipeline change: spec bundle created before code changes.
- Validation will include a failing regression test before production edits, then targeted unit tests.
- RenderDoc evidence is used for root-cause analysis; no backend-general claim will be made without validation.
- The implementation stays in existing frame graph and renderer boundaries.

## Project Structure

```text
Runtime/Engine/Rendering/
├── BaseSceneRenderer.cpp
├── BaseSceneRenderer.h
├── DeferredSceneRenderer.cpp
├── DeferredSceneRenderer.h
├── RenderScene.cpp
└── RenderScene.h

Runtime/Rendering/Resources/
├── Material.cpp
├── Material.h
└── Loaders/MaterialLoader.cpp

Runtime/Rendering/Assets/
└── MaterialConversion.cpp

Runtime/Rendering/Context/
├── RenderScenePackageBuilder.cpp
├── ThreadedRenderingLifecycle.cpp
└── ThreadedRenderingLifecycle.h

Runtime/Rendering/FrameGraph/
├── SceneRenderGraphBuilderDeferred.cpp
└── SceneRenderGraphBuilderDeferred.h

Tests/Unit/
├── AssetMaterialConversionTests.cpp
├── FrameGraphSceneTargetsTests.cpp
└── ThreadedRenderingLifecycleTests.cpp
```

**Structure Decision**: Add explicit surface mode to `Material`, route `Decal` drawables through scene queues and frame packages, then update deferred pass compilation in-place with regression coverage in the existing frame graph and threaded lifecycle test suites.

## Validation Evidence

- RenderDoc replay: `py -3 Tools\RenderDoc\rdc_analyze.py d:\VSProject\Nullus\App\Win64_Release_Runtime_Shared\Build\RenderDocCaptures\Editor_frame553.rdc` passed on 2026-06-06. The capture is D3D12 and contains `Nullus/DeferredGBuffer` followed by `Nullus/DeferredLighting`, then editor/grid/picking/UI work, with no scene Transparent or Decal pass in the captured bad frame.
- RenderDoc replay: `py -3 Tools\RenderDoc\rdc_analyze.py d:\VSProject\Nullus\App\Win64_Release_Runtime_Shared\Build\RenderDocCaptures\Editor_frame3600.rdc` passed on 2026-06-06. The capture is D3D12 and contains `Nullus/DeferredGBuffer` followed by `Nullus/DeferredLighting`, then `Nullus/DeferredTransparent`, but no `Nullus/DeferredDecal` pass.
- Material artifact check: `dirt_decal` in `TestProject\Library\Artifacts\907a615f-bfd4-4d8c-b8e9-6dd60ec33ed0\materials\material%3Amaterial%2F21.mat` has `<blendable>true</blendable>` but no `<surfaceMode>`, while the source glTF material has `"alphaMode": "BLEND"` and `"name": "dirt_decal"`.
- `NLS_Render` Debug passed on 2026-06-06 after decal material identity inference (`cmake --build Build/windows --target NLS_Render --config Debug -- /m:1`).
- `NLS_Engine` Debug passed on 2026-06-06 after the same changes (`cmake --build Build/windows --target NLS_Engine --config Debug -- /m:1`).
- Full `NullusUnitTests` Debug with project references remains blocked by unrelated dirty-worktree editor code: `Project\Editor\Core\EditorActions.cpp(1542,64): error C2838: "Instantiable": member declaration uses an illegal qualified name` and follow-on `ObjectRecord` initializer errors (`build_decal_identity_editor_debug.local.log`).
- `NullusUnitTests` Debug rebuilt successfully without rebuilding project references (`MSBuild.exe Build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildProjectReferences=false /m:1`, 0 warnings, 0 errors; `build_decal_identity_unit_norefs_debug.local.log`).
- Focused material identity tests passed on 2026-06-06: `Build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=AssetMaterialConversionTests.GltfBlendMaterialWithDecalNameSerializesAsDecalSurface:AssetMaterialConversionTests.GltfBlendMaterialWithCamelCaseDecalNameSerializesAsDecalSurface:AssetMaterialConversionTests.GltfBlendMaterialWithAcronymDecalNameSerializesAsDecalSurface:AssetMaterialConversionTests.GltfBlendMaterialWithoutDecalTokenStaysTransparent:AssetMaterialConversionTests.MaterialLoaderInfersLegacyDecalSurfaceModeFromSerializedName:AssetMaterialConversionTests.MaterialLoaderKeepsLegacyNonDecalBlendAsTransparent:AssetMaterialConversionTests.MaterialLoaderIgnoresDecalTokenInSourceWhenNameIsNonDecal:AssetMaterialConversionTests.MaterialLoaderUsesSourceSubAssetForLegacyDecalWhenNameMissing:AssetMaterialConversionTests.MaterialLoaderExplicitSurfaceModeOverridesLegacyDecalInference:AssetMaterialConversionTests.MaterialLoaderExplicitOpaqueSurfaceModeOverridesBlendableFlag --gtest_brief=1` ran 10 tests and passed.
- Generated-output guard: `git diff --name-only -- Runtime\Rendering\Gen Runtime\Engine\Gen Runtime\Core\Gen Project\Editor\Gen` produced no paths.
- Whitespace guard: `git diff --check` over the focused rendering/spec files only reported existing LF-to-CRLF conversion warnings.

## Complexity Tracking

No constitution violations expected.
