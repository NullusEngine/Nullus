# Tasks: UE5 LightGrid Alignment

**Input**: Design documents from `/specs/021-ue5-lightgrid-alignment/`
**Prerequisites**: spec.md, plan.md, research.md, data-model.md, quickstart.md

**Tests**: Required by spec and TDD workflow.

## Phase 1: Setup

**Purpose**: Lock current implementation boundaries before code changes.

- [x] T001 Inspect current LightGrid settings, constants, buffers, shader bindings, and frame-graph integration in `Runtime/Engine/Rendering/LightGridPrepass.*`, `Runtime/Engine/Rendering/ClusteredShading.*`, `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.*`, and `App/Assets/Engine/Shaders/LightGrid*.hlsl*`
- [x] T002 Record UE public parity anchors from local source path `F:\Epic Games\UE_4.27\Engine` in `specs/021-ue5-lightgrid-alignment/research.md`

---

## Phase 2: Foundational Tests

**Purpose**: Add failing tests for UE-aligned defaults and grid sizing before production changes.

- [x] T003 [P] Add tests for default pixel size 64, Z slices 32, max culled lights 32, and linked-list flag default true in `Tests/Unit/LightingDataProviderTests.cpp`. Evidence: `LightingDataProviderTests.LightGridDefaultsMatchUESourceReference` passed on 2026-05-10.
- [x] T004 [P] Add tests proving LightGrid XY dimensions derive from render size and pixel size in `Tests/Unit/LightingDataProviderTests.cpp`. Evidence: `LightingDataProviderTests.LightGridDimensionsDeriveFromRenderSizeAndPixelSize` passed on 2026-05-10.
- [x] T005 [P] Add tests for fixed-capacity overflow clamp behavior in `Tests/Unit/LightingDataProviderTests.cpp`. Evidence: `LightingDataProviderTests.LightGridCpuBuildUsesDerivedGridDimensionsForCapacityClamp` passed on 2026-05-10; shader contract `RenderFrameworkContractTests.LightGridLinkedListPathCapsGlobalLinksLikeUESource` verifies UE-style linked-list global link capacity guard.
- [x] T006 [P] Add tests preserving disabled LightGrid no-dispatch behavior in `Tests/Unit/RenderFrameworkContractTests.cpp`. Evidence: `RenderFrameworkContractTests.DisabledLightGridContextReturnsBeforePreparingDispatches` passed on 2026-05-10.
- [x] T024 [P] Add tests for UE logarithmic Z params and 4x4x4 grid dispatch shape in `Tests/Unit/LightingDataProviderTests.cpp`. Evidence: `LightGridZParamsMatchUESourceFormula` and `LightGridDispatchShapeMatchesUESourceReference` passed on 2026-05-10.

---

## Phase 3: User Story 1 - Match UE5 Light Grid Shape (Priority: P1)

**Goal**: Nullus LightGrid uses UE-style pixel-sized XY cells and default Z/capacity settings.

**Independent Test**: Default/grid sizing tests pass without requiring a live GPU.

- [x] T007 [US1] Replace fixed `gridSizeX` and `gridSizeY` defaults with `cellPixelSize`-derived dimensions in `Runtime/Engine/Rendering/ClusteredShading.h`
- [x] T008 [US1] Add UE-aligned default settings and validation helpers in `Runtime/Engine/Rendering/ClusteredShading.cpp`
- [x] T009 [US1] Update `LightGridPrepass::BuildFrameData` constants and buffer sizing to use render-size-derived grid dimensions in `Runtime/Engine/Rendering/LightGridPrepass.cpp`
- [x] T010 [US1] Update `LightGridCommon.hlsli` helpers to use UE-style tile pixel sizing, `ClipToView`, and logarithmic `LightGridZParams` in `App/Assets/Engine/Shaders/LightGridCommon.hlsli`

---

## Phase 4: User Story 2 - Match UE5 Culling Data Flow (Priority: P1)

**Goal**: LightGrid culls lights into a frustum-space grid and publishes compact graphics-readable cell records.

**Independent Test**: CPU contract tests and shader/build validation pass.

- [x] T011 [US2] Rewrite injection shader to dispatch over grid cells, compute each cell view-space AABB, loop local lights, and clamp explicitly at 32 by default in `App/Assets/Engine/Shaders/LightGridInjection.hlsl`
- [x] T012 [US2] Update compaction buffer sizing and record generation for UE-aligned capacity in `Runtime/Engine/Rendering/LightGridPrepass.cpp` and `App/Assets/Engine/Shaders/LightGridCompact.hlsl`
- [x] T013 [US2] Add linked-list culling setting and buffer contract scaffolding in `Runtime/Engine/Rendering/LightGridPrepass.h`
- [x] T014 [US2] Implement linked-list culling compute path or explicitly route fixed-capacity fallback when linked-list mode is unsupported in `Runtime/Engine/Rendering/LightGridPrepass.cpp` and `App/Assets/Engine/Shaders/LightGridInjection.hlsl`
- [x] T015 [US2] Keep forward/deferred graphics binding layout compatible with updated cell records in `Runtime/Engine/Rendering/LightGridPrepass.cpp`

---

## Phase 5: User Story 3 - Preserve Toggle And Debug Workflow (Priority: P2)

**Goal**: Existing LightGrid enable/disable and editor helper behavior survive UE5 alignment.

**Independent Test**: Existing LightGrid toggle/cache/debug helper tests pass.

- [x] T016 [US3] Verify disabled LightGrid returns an empty compile context before UE-aligned frame data allocation in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`. Evidence: `RenderFrameworkContractTests.DisabledLightGridContextReturnsBeforePreparingDispatches` passed on 2026-05-10.
- [x] T017 [US3] Preserve editor helper pass binding resolution behavior in `Project/Editor/Rendering/DebugSceneRenderer.cpp`. Evidence: `DebugSceneLifecycleTests.EditorHelperDrawsDoNotReceiveLightGridPassBindings` passed on 2026-05-10.
- [x] T018 [US3] Update settings/search tests only if new user-facing LightGrid settings are exposed in `Tests/Unit/EditorSettingsRegistryTests.cpp`. Evidence: no new UI-facing LightGrid settings were added in this alignment pass; existing `EditorSettingsRegistryTests.*LightGrid*` remains covered by the targeted LightGrid test run.

---

## Phase 6: Validation & Evidence

**Purpose**: Prove the implementation and document remaining parity gaps.

- [x] T019 Run targeted unit tests from `specs/021-ue5-lightgrid-alignment/quickstart.md`. Evidence: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=LightingDataProviderTests.*LightGrid*:RenderFrameworkContractTests.*LightGrid*:DebugSceneLifecycleTests.EditorHelperDrawsDoNotReceiveLightGridPassBindings:EditorSettingsRegistryTests.*LightGrid*` passed 16/16 on 2026-05-10.
- [x] T020 Build `Editor` Debug to validate shader compilation and MetaParser flow. Evidence: `cmake --build .\Build --config Debug --target Editor` passed on 2026-05-10 after the `ClipToView` constant/layout update.
- [x] T021 Capture or attempt DX12 RenderDoc frame using `Tools/RenderDoc/renderdoc_runner.py` and record result in this task file. Evidence: `rdc doctor` passed on 2026-05-10, but `py -3 Tools\RenderDoc\renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 180` launched `Editor.exe` and timed out after 180 seconds with `capture not found before timeout`; no runtime visual parity claim is made from this attempt.
- [x] T022 Self-review diffs for generated-file edits, backend overclaims, and UE parity gaps. Evidence: `git status --short | rg "(^.. Runtime/.*/Gen/|^.. Project/.*/Gen/|\\Gen\\|/Gen/)"` found no generated-file modifications; `git diff --check` reported only line-ending warnings; remaining parity gaps are UE reflection capture culling/shadow-channel packing and unverified DX12 runtime capture.
- [x] T023 Update `specs/021-ue5-lightgrid-alignment/tasks.md` completion evidence

## Dependencies & Execution Order

- Phase 1 before all other phases.
- Phase 2 tests before production changes.
- US1 is required before US2 because buffer sizing depends on the grid shape.
- US3 can be verified after US1/US2 changes but must not be deferred past final validation.
- Runtime evidence follows automated validation.

## Parallel Opportunities

- T003, T004, T005, and T006 touch test expectations and can be drafted independently.
- T010 shader helper changes and T009 CPU constant generation can be reviewed together but edited sequentially to keep CPU/GPU contracts matched.
- T016 and T017 are preservation checks and can be reviewed independently after the main LightGrid data contract is green.

## Implementation Strategy

1. Red: add tests for UE defaults, render-size-derived grid dimensions, overflow, and disabled path.
2. Green: change settings and frame constants to UE-aligned defaults.
3. Green: update shader helpers and buffer sizing.
4. Green: stage linked-list behavior or explicit fallback without silent mismatch.
5. Verify: run targeted tests, build shaders, and gather DX12 RenderDoc evidence.
