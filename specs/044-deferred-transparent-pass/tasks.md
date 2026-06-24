# Tasks: Deferred Transparent and Decal Passes

**Input**: `specs/044-deferred-transparent-pass/spec.md`, `specs/044-deferred-transparent-pass/plan.md`
**Prerequisites**: Root-cause evidence from RenderDoc capture and local code tracing

## Phase 1: Regression Test

- [x] T001 Add a failing unit test proving deferred package compilation preserves transparent draw commands after Lighting.
- [x] T002 Run the focused test and confirm it fails for the expected missing Transparent pass.

## Phase 2: Implementation

- [x] T003 Add deferred Transparent pass metadata and execution kind after Lighting.
- [x] T004 Preserve/record transparent draw commands in `DeferredSceneRenderer` threaded capture.
- [x] T005 Build deferred Transparent pass inputs from the post-lighting command range.
- [x] T006 Attach external scene output and depth resources to the Transparent pass without changing helper pass behavior.

## Phase 3: Validation

- [x] T007 Re-run the focused regression test and confirm it passes.
- [x] T008a Build the relevant broader rendering/engine targets (`NLS_Render`, `NLS_Engine`) after the fix.
- [x] T008b Run the most relevant broader unit test target (`NullusUnitTests`).
- [x] T009 Perform required quality review before reporting completion.

## Phase 4: Deferred Decal Separation

- [x] T010 Add failing tests for explicit decal material classification and deferred pass order (`GBuffer -> Decal -> Lighting -> Transparent`).
- [x] T011 Add explicit material surface mode serialization/loading with legacy blendable compatibility.
- [x] T012 Route decal drawables into a dedicated scene queue and frame package count.
- [x] T013 Add deferred Decal graph/pass metadata, command slicing, and GBuffer depth/resource access.
- [x] T014 Capture and draw decal commands in threaded and non-threaded deferred renderer paths.
- [x] T015 Re-run focused frame graph/threaded rendering tests and relevant rendering/engine builds.
  - `NLS_Render` Debug passed after typed pass-input resolver, schema SSoT wrapper, and validator cleanup (`build_deferred_decal_render_after_review_tidy.local.log`).
  - `NLS_Engine` Debug passed after the same changes (`build_deferred_decal_engine_after_review_tidy.local.log`).
  - `NullusUnitTests` Debug compiled the affected test translation units (`FrameGraphSceneTargetsTests.cpp`, `RenderSceneCacheTests.cpp`) and reached link, but remains blocked by unrelated prefab/editor unresolved symbols: `BuildSceneLoadPrefabResourceResolutionOptions` and `PrefabInstanceRegistry::MarkAssetMissing` (`build_deferred_decal_unit_after_review_tidy.local.log`).
- [x] T015a Add focused material identity regression tests for glTF `dirt_decal` / `DirtDecal` import, legacy `.mat` decal inference, and ordinary blend material transparency preservation (`Tests/Unit/AssetMaterialConversionTests.cpp`).
- [x] T015b Re-run RenderDoc analysis on `Editor_frame3600.rdc` and confirm the new failure shape is missing `DeferredDecal` despite a post-lighting `DeferredTransparent` pass.
- [x] T015c Re-run relevant rendering/engine builds after material identity inference.
  - `NLS_Render` Debug passed (`cmake --build Build/windows --target NLS_Render --config Debug -- /m:1`).
  - `NLS_Engine` Debug passed (`cmake --build Build/windows --target NLS_Engine --config Debug -- /m:1`).
  - Full `NullusUnitTests` Debug with project references remains blocked by unrelated dirty-worktree editor code in `Project\Editor\Core\EditorActions.cpp(1542,64)` with `Instantiable` / `ObjectRecord` initializer errors (`build_decal_identity_editor_debug.local.log`).
  - `NullusUnitTests` Debug rebuilt successfully without rebuilding project references (`MSBuild.exe Build\windows\Tests\Unit\NullusUnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:BuildProjectReferences=false /m:1`, 0 warnings, 0 errors; `build_decal_identity_unit_norefs_debug.local.log`).
  - Focused `AssetMaterialConversionTests` material identity filter ran 10 tests and passed.
- [x] T016 Re-run required quality review after decal separation.
  - R1 found one P1 acronym-token false negative and two P2 gaps; fixed `UIDecal` / explicit `surfaceMode` / API-comment coverage.
  - R2 confirmed P0/P1 clear and found one P2 sourceSubAsset fallback test gap; fixed with `MaterialLoaderUsesSourceSubAssetForLegacyDecalWhenNameMissing`.
  - R3 deeper audit: 0 P0, 0 P1, 70/80, code-level可合; only stale validation docs were noted and updated.
