# Tasks: MetaParser Resource Reflection

**Input**: Design documents from `specs/030-metaparser-resource-reflection/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. This migration follows test-first source-contract coverage plus focused runtime reflection validation.

## Phase 1: Setup

- [x] T001 Confirm the handwritten resource bridge inventory with `rg "StaticMetaTypeName\\(|GetObjectTypeName\\(" Runtime/Rendering/Resources --glob "*.h"`.
- [x] T002 Confirm `Runtime/Rendering/ExternalReflection.h` currently owns manual resource registrations that must be removed.

## Phase 2: Foundational Tests

- [x] T003 Add failing source-contract regression coverage in `Tests/Unit/MetaParserGenerationModuleTests.cpp` for resource headers using `CLASS`/`GENERATED_BODY()` and not handwritten bridges.
- [x] T004 Add failing generated-output coverage in `Tests/Unit/MetaParserGenerationModuleTests.cpp` for generated resource registrations.
- [x] T005 Add or update runtime reflection coverage in `Tests/Unit/ReflectionRuntimeCoreTests.cpp` for migrated resource class and pointer type validity.

## Phase 3: User Story 1 - Generated Resource Type Identity (Priority: P1)

**Goal**: Migrate owned resource class declarations to the MetaParser path.

**Independent Test**: Source-contract test detects no handwritten bridges and generated outputs exist after build.

- [x] T006 [US1] Update `Runtime/Rendering/Resources/Mesh.h` to include reflection macros/generated header and use `CLASS(... Mesh)` plus `GENERATED_BODY()`.
- [x] T007 [US1] Update `Runtime/Rendering/Resources/Material.h` to include reflection macros/generated header and use `CLASS(... Material)` plus `GENERATED_BODY()`.
- [x] T008 [US1] Update `Runtime/Rendering/Resources/Shader.h` to include reflection macros/generated header and use `CLASS(... Shader)` plus `GENERATED_BODY()`.
- [x] T009 [US1] Update `Runtime/Rendering/Resources/Texture.h` to include reflection macros/generated header and use `CLASS(... Texture)` plus `GENERATED_BODY()`.
- [x] T010 [US1] Update `Runtime/Rendering/Resources/Texture2D.h` to include reflection macros/generated header and use `CLASS(... Texture2D)` plus `GENERATED_BODY()`.
- [x] T011 [US1] Update `Runtime/Rendering/Resources/TextureCube.h` to include reflection macros/generated header and use `CLASS(... TextureCube)` plus `GENERATED_BODY()`.

## Phase 4: User Story 2 - Preserve Existing Resource Reflection Behavior (Priority: P1)

**Goal**: Preserve stable runtime resource type behavior after migration.

**Independent Test**: `PPtrTests.*`, runtime reflection tests, and `ReflectionTest` remain green.

- [x] T012 [US2] Build `NullusUnitTests` through the normal MetaParser generation path to regenerate rendering resource outputs.
- [x] T013 [US2] Run focused runtime/PPtr reflection tests and fix only regressions caused by this migration.
- [x] T013a [US2] Register `NLS::NamedObject` as a native reflected base deriving from `NLS::Object` so generated resource bases resolve without diagnostics.
- [x] T013b [US2] Update MetaParser base generation to keep reflected bases and skip non-reflected runtime-only interfaces such as `NLS::Render::Resources::IMesh`.
- [x] T013c [US2] Reuse the reflected type catalog for generated object-bridge inheritance so cross-header object-derived classes still emit `GetObjectTypeName()`.
- [x] T013d [US2] Create generated-header stubs for target headers before parsing clean headers that include their own `.generated.h`.

## Phase 5: User Story 3 - Keep External Reflection For External Types Only (Priority: P2)

**Goal**: Remove duplicate external registrations for owned resource classes while keeping external/value reflection.

**Independent Test**: Source-contract test confirms resource manual registration is gone and runtime tests confirm value/external types remain.

- [x] T014 [US3] Remove resource `NLS_META_EXTERNAL_TYPE_NAME` declarations and `RegisterResourceReferenceType` calls from `Runtime/Rendering/ExternalReflection.h`.
- [x] T015 [US3] Keep `Bounds` external reflection registration intact in `Runtime/Rendering/ExternalReflection.h`.

## Phase 6: Polish & Validation

- [x] T016 Run `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false`.
- [x] T017 Run `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:PPtrTests.*`.
- [x] T018 Run `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` and `.\Build\bin\Debug\ReflectionTest.exe`.
- [x] T019 Run source grep to confirm no production handwritten resource object type bridges remain.
- [x] T020 Run plan-review/self-review gate and record evidence before completion.

## Phase 7: Review Gate Fixes

- [x] T021 [US4] Add runtime material prewarm coverage in `Tests/Unit/GameLaunchArgsTests.cpp`.
- [x] T022 [US4] Implement `PrewarmRuntimeMaterialAssets()` and call it from `Project/Game/Core/Context.cpp`.
- [x] T023 [US5] Change `ArtifactDatabase::UpsertManifest()` to rebuild its index once per upsert.
- [x] T024 [US5] Add dirty artifact database cache flushing in `AssetDatabaseFacade` and flush at `StopAssetEditing()`.
- [x] T025 [US5] Add batch central-index flush coverage in `Tests/Unit/AssetDatabaseFacadeTests.cpp`.
- [x] T026 [US6] Replace hard-coded `publish/Debug/MetaParser.exe` and `cmd /c` fixture execution with a portable helper.
- [x] T027 [US6] Make PPtr target macro parsing fail loudly and accept wrapped macro entries.
- [x] T028 [US6] Update reflection workflow docs to match intentional `vector<T>`/`Array<T>` shorthand normalization.
- [x] T029 [US7] Add FrameGraph uniform-buffer barrier contract coverage.
- [x] T030 [US7] Map FrameGraph uniform-buffer read/write requests to the CPU-visible `GenericRead` state contract.
- [x] T031 Run focused tests for review fixes.
- [x] T032 Run full `NullusUnitTests` and `ReflectionTest`.
- [x] T033 Run mandatory plan-review/multi-agent review gate.

## Validation Evidence

- 2026-05-22: `dotnet publish Build\Tools\MetaParser\generated\src\MetaParser.csproj --configuration Debug --runtime win-x64 --self-contained false --nologo -o Build\Tools\MetaParser\publish\Debug` passed after clearing a stale MetaParser process lock.
- 2026-05-22: `Build\Runtime\Rendering\NLS_Render_run_metaparser_Debug.cmd Build\Runtime\Rendering\NLS_Render.precompile.json` passed and regenerated 9 rendering reflection types.
- 2026-05-22: `cmake --build Build --target NullusUnitTests --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed.
- 2026-05-22: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.RegistersBaseReflectionTypes:ReflectionRuntimeTestFixture.RegistersRenderingResourceTypesThroughGeneratedReflection:ReflectionRuntimeTestFixture.RenderingResourceReflectionDoesNotReportMissingBaseTypes:MetaParserGenerationModuleTests.RenderingResourceReflectionSkipsNonReflectedRuntimeInterfacesAsBases:MetaParserGenerationModuleTests.MetaParserKeepsCrossHeaderReflectedBasesAndSkipsRuntimeInterfaces:MetaParserGenerationModuleTests.MetaParserCreatesTargetGeneratedHeaderStubsBeforeParsingCleanHeaders` passed 6/6.
- 2026-05-22: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=ReflectionRuntimeTestFixture.RegistersEngineReflectionTypes:ReflectionRuntimeTestFixture.RegistersSpecialCasePropertyBindingsWithExpectedTypes` passed 2/2.
- 2026-05-22: `cmake --build Build --target ReflectionTest --config Debug -- /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false` passed.
- 2026-05-22: `.\Build\bin\Debug\ReflectionTest.exe` passed.
- 2026-05-22: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=MetaParserGenerationModuleTests.*:ReflectionRuntimeTestFixture.*:PPtrTests.*` passed 85/85.
- 2026-05-22: Grep confirmed no generated `Mesh` base reference to `IMesh`, no handwritten resource `StaticMetaTypeName`/`GetObjectTypeName`, and no resource manual external registrations.
- 2026-05-23: `cmake --build Build --target NullusUnitTests ReflectionTest --config Debug -- /m:1 /nr:false` passed after review-gate fixes.
- 2026-05-23: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_filter=GameLaunchArgsTests.*:AssetDatabaseFacadeTests.ArtifactDatabaseBatchUpsertsDoNotReloadCentralIndexPerManifest:AssetDatabaseFacadeTests.ArtifactDatabaseBatchUpsertsFlushCentralIndexOnceOnStopAssetEditing:MetaParserGenerationModuleTests.MetaParserKeepsCrossHeaderReflectedBasesAndSkipsRuntimeInterfaces:MetaParserGenerationModuleTests.MetaParserCreatesTargetGeneratedHeaderStubsBeforeParsingCleanHeaders:MetaParserGenerationModuleTests.ExternalReflectionTypeDiscoveryHandlesCommentsStringsAndElifBranches:MetaParserGenerationModuleTests.RejectsPPtrFieldsWhoseTargetsAreNotSupportedUnityObjects:MetaParserGenerationModuleTests.PPtrTargetMacroParsingFailsLoudlyAndAcceptsWrappedEntries:MetaParserGenerationModuleTests.GeneratesStdVectorAndArrayReflectedValueFields:MetaParserGenerationModuleTests.GeneratesRangeMetadataAndDefaultPrivateFieldAccessors:MetaParserGenerationModuleTests.RejectsPrivateAccessorPropertiesBeforeGeneratingInvalidPrivateFieldShim:MetaParserGenerationModuleTests.RejectsMalformedRangeMetadataBeforeGeneratingCpp:MetaParserGenerationModuleTests.RejectsUnknownPropertyMetadataBeforeGeneratingCpp:MetaParserGenerationModuleTests.RejectsInvertedRangeMetadataBeforeGeneratingCpp:RenderFrameworkContractTests.FrameGraphUniformBufferUsesGenericReadStateForExplicitTracking --gtest_color=no` passed 19/19.
- 2026-05-23: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_color=no` passed 1472/1472.
- 2026-05-23: `.\Build\bin\Debug\ReflectionTest.exe` passed.
- 2026-05-23: `cmake -S . -B Build -DNLS_BUILD_TESTS=ON -DNLS_ENABLE_TEST_HOOKS=ON` passed after R2 code-quality fixes.
- 2026-05-23: `cmake -S . -B BuildNoTestsCheck -DNLS_BUILD_TESTS=OFF -DNLS_ENABLE_TEST_HOOKS=OFF` passed; `BuildNoTestsCheck\Runtime\Core\NLS_Core.vcxproj` and `BuildNoTestsCheck\Project\Editor\Editor.vcxproj` contain no `NLS_ENABLE_TEST_HOOKS`.
- 2026-05-23: `cmake --build Build --target NullusUnitTests ReflectionTest --config Debug -- /p:TrackFileAccess=false /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false /p:UseIncrementalLinking=false /v:minimal` built `NullusUnitTests`; the combined command timed out while `ReflectionTest` MSBuild remained alive, then `cmake --build Build --target ReflectionTest --config Debug -- /p:TrackFileAccess=false /p:UseSharedCompilation=false /p:UseMultiToolTask=false /m:1 /nodeReuse:false /p:LinkIncremental=false /p:UseIncrementalLinking=false /v:minimal` passed separately.
- 2026-05-23: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_color=no --gtest_filter="AssetImportPipelineTests.ArtifactDatabaseUpsertsUpdateIndexIncrementally:AssetImportPipelineTests.ArtifactDatabasePersistsCentralIndexBySourceSubAssetAndStatus:EditorAssetDragDropTests.*:EditorAssetDatabaseTests.*:GameObjectAssetImportTests.*:ReflectedPropertyDrawerTests.*"` passed 126/126.
- 2026-05-23: `.\Build\bin\Debug\NullusUnitTests.exe --gtest_color=no` passed 1491/1491 after R2 code-quality fixes.
- 2026-05-23: `.\Build\bin\Debug\ReflectionTest.exe` passed after R2 code-quality fixes.
- 2026-05-23: `git diff --name-only -- 'Runtime/*/Gen/*' 'Project/*/Gen/*'` returned empty; `git diff --check` returned only LF-to-CRLF warnings.
- 2026-05-23: R2 multi-agent plan-review completed: architecture/performance 73/80 PASS with 0 P0/P1, GPU/RHI 75/80 PASS with 0 P0/P1, deeper audit 72/80 PASS with 0 P0/P1, code-quality lane found two P1 blockers. R2 blockers were fixed by moving editor drag payload production callers off `ForTesting`, guarding test-only payload and artifact-database counter APIs behind `NLS_ENABLE_TEST_HOOKS`, adding the `NLS_Core` test hook compile definition, and broadening `.gitignore` to `Project/*/Gen/`.
- 2026-05-23: R3 multi-agent plan-review after R2 blocker fixes passed: code-quality 74/80 PASS with 0 P0/P1 and deeper audit 74/80 PASS with 0 P0/P1. Remaining P2 items are historical test-hook surface cleanup and missing `Docs/REVIEW_PATTERNS.md` process debt.

## Dependencies & Execution Order

- T003-T005 before production migration.
- T006-T011 before generated-output validation.
- T014-T015 can run after T006-T011 to avoid losing the old fallback before generated registration exists.
- T016-T020 complete the validation and review gate.

## Parallel Opportunities

- T006-T011 are independent header edits, but should be applied carefully in one patch to keep include/generation style consistent.
- Runtime tests and source grep can be run independently after the build succeeds.
