# Tasks: Merge Mesh And Material Renderer

**Input**: Design documents from `specs/merge-mesh-material-renderer/`  
**Prerequisites**: plan.md, spec.md

**Tests**: Required. This change affects runtime rendering, reflection, serialization, asset import, and editor workflows.

## Phase 1: Setup

- [X] T001 Inventory production `MaterialRenderer` usage with `rg -n "MaterialRenderer|GetComponent<.*MaterialRenderer|AddComponent<.*MaterialRenderer" Runtime Project -S` and record call-site groups in `specs/merge-mesh-material-renderer/tasks.md`.
- [X] T002 Inventory test/fixture `MaterialRenderer` usage with `rg -n "MaterialRenderer" Tests -S` and group tests by runtime rendering, serialization, asset import, reflection, and editor coverage.
- [X] T003 Confirm build ownership for `Runtime/Engine/Components/MaterialRenderer.*` in `Runtime/Engine/CMakeLists.txt` and generated reflection include ownership in `Runtime/Engine/Gen/MetaGenerated.cpp`.

Inventory notes:

- Production runtime references are concentrated in `Runtime/Engine/Components/MaterialRenderer.*`, `Runtime/Engine/Components/MeshFilter.cpp`, `Runtime/Engine/Rendering/RenderScene.*`, `Runtime/Engine/PrimitiveFactory.cpp`, `Runtime/Engine/Assets/ModelPrefabBuilder.cpp`, `Runtime/Engine/Serialize/ObjectGraphInstantiator.h`, `Runtime/Engine/AssemblyEngine.cpp`, and `Runtime/Engine/Rendering/SceneRendererFactory.cpp`.
- Production editor references are concentrated in `Project/Editor/Assets/AssetDragDropWorkflow.cpp`, `Project/Editor/Core/EditorActions.cpp`, `Project/Editor/Panels/AssetView.*`, `Project/Editor/Panels/ComponentSearchPanel.cpp`, and `Project/Editor/Rendering/PickingRenderPass.cpp`.
- Test references span rendering cache/material cache, object graph and prefab serialization, asset import/prefab pipeline, reflected property drawing, component picker, editor render path contracts, and rendering viewport evidence tests.
- Build/generation ownership currently includes `Runtime/Engine/AssemblyEngine.cpp` and generated `Runtime/Engine/Gen/MetaGenerated.cpp`; generated files must be updated only through the normal MetaParser/build flow.

---

## Phase 2: Foundational Tests

**Purpose**: Create failing or updated coverage before production removal.

- [X] T004 [P] Add/update render-scene tests in `Tests/Unit/RenderSceneCacheTests.cpp` proving `MeshRenderer` material assignment drives draw command material resolution without `MaterialRenderer`.
- [X] T005 [P] Add/update material cache tests in `Tests/Unit/DeferredSceneRendererMaterialCacheTests.cpp` to create `MeshFilter + MeshRenderer` only.
- [X] T006 [P] Add/update reflection expectations in `Tests/Unit/ReflectionRuntimeEngineTests.cpp` so `MeshRenderer` owns `materials` and `userMatrixValues` and `MaterialRenderer` is absent.
- [X] T007 [P] Add/update generated reflection expectations in `Tests/Unit/MetaParserGenerationEngineTests.cpp` for `MeshRenderer` material fields and no `MaterialRenderer.generated.cpp` dependency.
- [X] T008 [P] Add/update source-contract checks in `Tests/Unit/EditorRenderPathContractTests.cpp` proving production editor/resource binding code no longer queries `MaterialRenderer`.

---

## Phase 3: User Story 1 - Single Mesh Rendering Component (Priority: P1) 🎯 MVP

**Goal**: `MeshRenderer` owns material state and runtime rendering consumes it.

**Independent Test**: A scene containing `MeshFilter + MeshRenderer` renders with the assigned material and no sibling `MaterialRenderer`.

- [X] T009 [US1] Move material type aliases, material arrays, path caches, material names, user matrix, and public material/user-matrix declarations from `Runtime/Engine/Components/MaterialRenderer.h` into `Runtime/Engine/Components/MeshRenderer.h`.
- [X] T010 [US1] Move material assignment, path/reference resolution, material slot update, and user matrix method implementations from `Runtime/Engine/Components/MaterialRenderer.cpp` into `Runtime/Engine/Components/MeshRenderer.cpp`.
- [X] T011 [US1] Update `Runtime/Engine/Components/MeshRenderer.cpp` copy constructor and assignment operator to copy material references, material path caches, material name state, user matrix state, frustum behavior, and custom bounding sphere while preserving owner/lifecycle semantics.
- [X] T012 [US1] Update `Runtime/Engine/Components/MeshFilter.cpp` so mesh changes call the owning `MeshRenderer::UpdateMaterialList()`.
- [X] T013 [US1] Update `Runtime/Engine/Rendering/RenderScene.h` to remove `Components::MaterialRenderer*` from `RenderPrimitive`.
- [X] T014 [US1] Update `Runtime/Engine/Rendering/RenderScene.cpp` to resolve materials through `primitive.meshRenderer->ResolveMaterials()` and remove sibling `MaterialRenderer` lookup.
- [X] T015 [US1] Update primitive creation in `Runtime/Engine/PrimitiveFactory.cpp` to create/use `MeshRenderer` for both render activation and material assignment.

---

## Phase 4: User Story 2 - Destructive Removal Of MaterialRenderer (Priority: P1)

**Goal**: Standalone `MaterialRenderer` is gone from production component registration, generation, serialization, and tests.

**Independent Test**: Reflection/component/source-contract tests confirm `MaterialRenderer` is absent and new data lives on `MeshRenderer`.

- [X] T016 [US2] Remove `MaterialRenderer` includes/registration from `Runtime/Engine/AssemblyEngine.cpp` and `Runtime/Engine/CMakeLists.txt`.
- [X] T017 [US2] Remove or orphan `Runtime/Engine/Components/MaterialRenderer.h` and `Runtime/Engine/Components/MaterialRenderer.cpp` from the production build without hand-editing generated files.
- [X] T018 [US2] Update `Runtime/Engine/Serialize/ObjectGraphInstantiator.h` so material reference/path handling applies to `MeshRenderer` records only and no compatibility conversion from `MaterialRenderer` records is added.
- [X] T019 [US2] Update `Runtime/Engine/Assets/ModelPrefabBuilder.cpp` to emit only one `MeshRenderer` component record per mesh node, with `frustumBehaviour` and `materials` properties on that record.
- [X] T020 [US2] Update object graph fixtures in `Tests/Unit/Fixtures/ObjectGraph/` to remove `MaterialRenderer` records and place material data on `MeshRenderer`.
- [X] T021 [US2] Update serialization tests in `Tests/Unit/SceneObjectGraphSerializationTests.cpp` and `Tests/Unit/PrefabObjectGraphSerializationTests.cpp` to use `MeshRenderer` material APIs and new object graph records.
- [X] T022 [US2] Update asset pipeline tests in `Tests/Unit/AssetPrefabPipelineTests.cpp`, `Tests/Unit/GameObjectAssetImportTests.cpp`, and `Tests/Unit/AssetManifestTests.cpp` to expect no `MaterialRenderer` records.

---

## Phase 5: User Story 3 - Editor And Importer Workflows Use MeshRenderer Materials (Priority: P2)

**Goal**: Editor authoring and preview workflows target merged `MeshRenderer` material slots.

**Independent Test**: Editor/action/import tests pass using `MeshRenderer` material APIs.

- [X] T023 [US3] Update material drag/drop logic in `Project/Editor/Assets/AssetDragDropWorkflow.cpp` to require and assign `MeshRenderer` materials.
- [X] T024 [US3] Update prefab/resource binding logic in `Project/Editor/Core/EditorActions.cpp` to bind material references on `MeshRenderer`.
- [X] T025 [US3] Update preview object setup in `Project/Editor/Panels/AssetView.h` and `Project/Editor/Panels/AssetView.cpp` to remove `m_materialRenderer` and assign materials through `m_modelRenderer`.
- [X] T026 [US3] Update component picker/editor search expectations in `Project/Editor/Panels/ComponentSearchPanel.cpp` and `Tests/Unit/EditorComponentPickerTests.cpp` to remove `Rendering/Material Renderer`.
- [X] T027 [US3] Update reflected property drawer tests in `Tests/Unit/ReflectedPropertyDrawerTests.cpp` to use `MeshRenderer*` references/material fields instead of `MaterialRenderer*`.
- [X] T028 [US3] Remove obsolete `MaterialRenderer` includes from editor rendering/debug files in `Project/Editor/Rendering/` and runtime scene renderer files in `Runtime/Engine/Rendering/`.

---

## Phase 6: Generated Reflection And Cleanup

**Purpose**: Regenerate reflection outputs through the normal build and remove stale standalone references.

- [X] T029 Run `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` to regenerate reflection outputs and expose compile failures.
- [X] T030 Update tests that inspect generated files in `Tests/Unit/MetaParserGenerationEngineTests.cpp`, `Tests/Unit/MetaParserGenerationModuleTests.cpp`, and `Tests/Unit/ReflectionRuntimeCoreTests.cpp` to match generated `MeshRenderer` fields and removed `MaterialRenderer` output.
- [X] T031 Run `rg -n "MaterialRenderer|GetComponent<.*MaterialRenderer|AddComponent<.*MaterialRenderer" Runtime Project -S` and remove remaining production references unless they are explicitly documenting removal.
- [X] T032 Run `rg -n "MaterialRenderer" Tests -S` and remove or rewrite remaining test references except tests intentionally asserting absence.
- [X] T033 Update reflection documentation references in `Docs/Reflection/ReflectionWorkflow.en.md` and `Docs/Reflection/ReflectionWorkflow.zh-CN.md` if they list `MaterialRenderer` as an expected reflected component.

---

## Phase 7: Validation & Review

- [X] T034 Run `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=RenderSceneCacheTests.*:DeferredSceneRendererMaterialCacheTests.*:SceneObjectGraphSerializationTests.*:PrefabObjectGraphSerializationTests.*:AssetPrefabPipelineTests.*:GameObjectAssetImportTests.*`.
- [X] T035 Run `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.*:MetaParserGenerationEngineTests.*:MetaParserGenerationModuleTests.GeneratesComponentMenuTypeMetadataBindings:EditorComponentPickerTests.*:ReflectedPropertyDrawerTests.*:EditorRenderPathContractTests.*`.
- [X] T036 Run `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` and `.\Build\bin\Debug\ReflectionTest.exe`.
- [X] T037 Run final source scans for `MaterialRenderer` and document any intentional remaining references in `specs/merge-mesh-material-renderer/tasks.md`.
- [X] T038 Run `/plan-review` quality gate for the completed implementation, fix required findings, and record validation evidence before completion.

Validation notes:

- `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false /p:CL_MPCount=1` passed after refreshing CMake metadata; stale `MaterialRenderer.h` MSB8064 warning is gone.
- `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false /p:CL_MPCount=1` passed.
- `.\Build\bin\Debug\ReflectionTest.exe` passed and explicitly reports `NLS::Engine::Components::MaterialRenderer is absent`.
- Material-slot boundary regression: `ReflectionRuntimeTestFixture.MeshRendererMaterialSlotAccessIgnoresOutOfRangeIndices` first failed with MSVC `array subscript out of range`, then passed after adding `MeshRenderer` slot bounds checks.
- Runtime/import/serialization suite: 104/104 tests passed.
- Reflection/editor contract suite: 159/159 tests passed.
- Final production scan `rg -n "MaterialRenderer|GetComponent<.*MaterialRenderer|AddComponent<.*MaterialRenderer" Runtime Project -S` returned no matches.
- Final test/tool scan leaves only intentional absence assertions in `Tests/Unit/ReflectionRuntimeEngineTests.cpp`, `Tests/Unit/EditorComponentPickerTests.cpp`, `Tests/Unit/MetaParserGenerationEngineTests.cpp`, and `Tools/ReflectionTest/src/main.cpp`.
- Plan-review R1 scored 49/80 and blocked on one P0 (`SetMaterialAtIndex(255, ...)` out-of-bounds) and one P1 (copy/assignment copied raw `Material*` cache); both were fixed and covered by focused regression tests.
- Plan-review R2 scored 73/80 with 0 P0/P1 findings.
- Final deeper audit scored 74/80 with 0 new P0/P1 findings; remaining P2 notes are non-blocking: `uint8_t` material-slot loops are brittle if the 255-slot contract changes, and serialized `materials` arrays can contain more than 255 entries while runtime rendering consumes only the first 255 slots.

## Dependencies & Execution Order

- Phase 2 tests should be adjusted before or alongside the implementation they cover.
- T009-T015 deliver the runtime MVP and must complete before destructive file/build removal.
- T016-T022 remove the old component schema and should follow the runtime MVP.
- T023-T028 can run after `MeshRenderer` material APIs compile.
- T029 must run before generated-output tests are considered authoritative.
- T034-T038 are final validation and review gates.

## Parallel Opportunities

- T004-T008 can be worked in parallel because they touch separate tests.
- T021-T022 can be split by test file after the new `MeshRenderer` API exists.
- T023-T028 can be split across editor asset workflow, editor actions, preview panel, picker tests, and reflected drawer tests.
- T034 and T035 can be run independently after the build succeeds.

## Implementation Strategy

### MVP First

1. Add/adjust render-scene coverage.
2. Move material state/API into `MeshRenderer`.
3. Update `RenderScene`, `MeshFilter`, and primitive creation.
4. Validate that `MeshFilter + MeshRenderer` can render with assigned materials.

### Complete Destructive Merge

1. Remove production `MaterialRenderer` ownership and generated reflection references.
2. Update serialization/import/editor workflows to write and read material data from `MeshRenderer`.
3. Regenerate reflection outputs by building.
4. Run focused test suites and final source scans.

### Review Gate

Run plan-review after implementation and before declaring the task complete. For this scope, if the implementation spans at least five commits or touches GPU sync/RHI behavior, run the repository's multi-agent review gate; otherwise the standard plan-review loop applies.
