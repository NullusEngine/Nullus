# Tasks: UE4.27 Deferred Lighting Alignment

## Phase 1: Setup

- [x] T001 Confirm current working branch is `025-ue427-deferred-alignment` and record dirty files with `git status --short --branch`.
- [x] T002 [P] Inspect UE4.27 reference symbols `GatherLightsAndComputeLightGrid`, `RenderLights`, `RenderLight`, and `RenderTiledDeferredLighting` under `F:\Epic Games\UE_4.27\Engine\Source\Runtime\Renderer\Private`.

## Phase 2: Foundational Tests

- [x] T003 [P] Add a failing shader contract test in `Tests/Unit/RenderFrameworkContractTests.cpp` requiring `DeferredLighting.hlsl` to use a full scene light-list accumulation function, not `NLSAccumulateClusteredLightingPBR`.
- [x] T004 [P] Add a failing shader/common contract test in `Tests/Unit/RenderFrameworkContractTests.cpp` requiring Ambient Sphere to be treated as a global deferred light contributor.
- [x] T005 Run `cmake --build Build --target NullusUnitTests --config Debug` and the focused test filter to verify the new tests fail for the current clustered-only deferred path.

## Phase 3: User Story 1 - Deferred Scene Is Lit By Scene Lights (P1)

- [x] T006 [US1] Add light-list accumulation helpers to `App/Assets/Engine/Shaders/LightGridCommon.hlsli` for deferred PBR-style ambient, directional, point, and spot contribution across `NLSGetSceneLightCount()`.
- [x] T007 [US1] Change `App/Assets/Engine/Shaders/DeferredLighting.hlsl` to call the full scene light-list accumulation helper and preserve sky fallback.
- [x] T008 [US1] Update `Runtime/Engine/Rendering/LightGridPrepass.cpp` or related contracts only if the packed light list binding size/count needs adjustment for deferred consumption.
- [x] T009 [US1] Run the focused shader/light contract tests and confirm they pass.

## Phase 4: User Story 2 - RenderDoc Shows UE-Like Deferred Stages (P2)

- [x] T010 [P] Review `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp` and existing debug-name tests to ensure main stage names remain stable.
- [x] T011 [US2] Add or adjust tests in `Tests/Unit/EditorRenderPathContractTests.cpp` if any new SceneColor initialization label or overlay ordering contract is introduced.
- [x] T012 [US2] Run focused editor render path and DX12 debug-name tests.

## Phase 5: User Story 3 - Scope Remains Phase-One Compatible (P3)

- [x] T013 [P] Update tests or comments to state shadows, SSAO, reflections, translucency, and full tiled deferred are intentionally out of scope for this phase.
- [x] T014 [US3] Verify no generated files under `Runtime/*/Gen/` were edited.

## Phase 6: Validation & Review

- [x] T015 Run `cmake --build Build --target Editor --config Debug`.
- [x] T016 Run `cmake --build Build --target NullusUnitTests --config Debug`.
- [x] T017 Run focused tests: `Build\Win64_Debug_Runtime_Shared\Tests\Unit\NullusUnitTests.exe --gtest_filter=RenderFrameworkContractTests.*Deferred*:LightingDataProviderTests.*LightGrid*:EditorRenderPathContractTests.*Deferred*:DX12DebugNameUtilsTests.*`.
- [x] T018 Capture a DX12 editor frame with `py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --project TestProject\TestProject.nullus --capture --capture-after-frames 60 --timeout 120`.
- [x] T019 Analyze the capture with `py -3 Tools\RenderDoc\rdc_analyze.py <capture-path>` and record whether DeferredLighting output exceeds ambient-floor-only evidence.
- [x] T020 Self-review `git diff -- App/Assets/Engine/Shaders Runtime/Engine/Rendering Runtime/Rendering/FrameGraph Tests/Unit specs/025-ue427-deferred-alignment`.
- [x] T021 Preserve the actual queued GBuffer draw count from `DeferredSceneRenderer::BeginFrame` so threaded deferred pass splitting does not infer zero scene draws from editor helper-only recorded command totals.
- [x] T022 Rebuild `NullusUnitTests`, run the deferred/editor render-path focused tests, and capture a fresh DX12 Editor frame confirming `Nullus/DeferredGBuffer`, `Nullus/DeferredLighting`, and `Nullus/EditorGridPass` appear in RenderDoc.
- [x] T023 Make deferred lighting refresh material texture bindings through cache-invalidating `Material::Set` calls so DX12 does not sample stale GBuffer textures while editor overlays use current-frame matrices.
- [x] T024 Capture DX12 Scene View A/B camera frames and confirm the lit cube and selected outline move together under different camera transforms.
- [x] T025 Investigate `trace.json` performance evidence and identify the current low-FPS hotspot as main-thread `UIManager::DrawCanvas`/`Canvas::Draw` time rather than deferred GPU pass cost.
- [x] T026 Restore the Scene View retired-frame presentation policy so idle Scene View rendering no longer forces a synchronized threaded-rendering drain every frame.
- [x] T027 Run focused lifecycle and picking tests covering the Scene View drain policy after the performance fix.

## Dependencies

- T003-T005 must precede shader/runtime implementation.
- T006-T009 complete the MVP for US1.
- T010-T012 depend on stable deferred pass naming from the existing frame graph.
- T015-T020 run after code changes.

## MVP Scope

Complete T003-T009 first. That restores visible deferred lighting independent of clustered per-pixel membership.
