# Tasks: Unity Asset Browser Parity

**Input**: Design documents from `specs/unity-asset-browser-parity/`
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `quickstart.md`
**Tests**: Required for deterministic behavior changes and cache logic. Renderer-backed thumbnail visuals require separate focused runtime/manual verification before being claimed.

## Phase 1: Setup

- [X] T001 Confirm the isolated worktree branch and document that `.specify` templates were read from the main local checkout because `.specify/` is gitignored.
- [X] T002 Inspect existing `Project/Editor/Panels/AssetBrowser.*`, `Project/Editor/Assets/AssetBrowserPresentation.*`, and related drag/drop tests for reusable workflows.
- [X] T003 [P] Identify existing UI widget/ImGui capabilities needed for split panes, search box, type filter, grid items, and thumbnail-size slider.

## Phase 2: Foundational Presentation And Cache Contracts

- [X] T004 [P] Add failing presentation tests for project-only folder tree and current-folder direct content in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [X] T005 [P] Add failing presentation tests for breadcrumb construction and folder selection fallback in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [X] T006 [P] Add failing thumbnail cache key/path/freshness tests in `Tests/Unit/AssetThumbnailCacheTests.cpp`.
- [X] T007 Implement project-only folder/content presentation types in `Project/Editor/Assets/AssetBrowserPresentation.h`.
- [X] T008 Implement project-only folder/content presentation logic in `Project/Editor/Assets/AssetBrowserPresentation.cpp`.
- [X] T009 Implement thumbnail cache request/key/freshness primitives in `Project/Editor/Assets/AssetThumbnailCache.h` and `Project/Editor/Assets/AssetThumbnailCache.cpp`.
- [X] T010 Confirm the existing `GLOB_RECURSE CONFIGURE_DEPENDS` CMake setup picks up new editor source files and unit test files.

## Phase 3: User Story 1 - Navigate Project Assets Like Unity (Priority: P1)

**Goal**: Replace the old one-tree browser with a project-only split tree/grid layout.

**Independent Test**: Select folders under `Assets/` and verify the right grid and breadcrumb reflect only current-folder project content.

- [X] T011 [US1] Refactor `Project/Editor/Panels/AssetBrowser.h` state around selected folder, search/filter state, thumbnail size, and presentation item lists.
- [X] T012 [US1] Replace the old engine/project tree fill path in `Project/Editor/Panels/AssetBrowser.cpp` with left project-folder navigation and right current-folder content rendering.
- [X] T013 [US1] Add breadcrumb rendering and selected-folder fallback handling in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T014 [US1] Preserve manual refresh/import toolbar actions in the new layout in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T015 [US1] Run focused presentation tests for project-only navigation.

## Phase 4: User Story 2 - Inspect Imported Assets In A Thumbnail Grid (Priority: P1)

**Goal**: Show source assets and generated imported sub-assets as independent grid items.

**Independent Test**: Imported model folders show source file plus generated prefab, mesh, material, and texture items with correct selection/drag payload metadata.

- [X] T016 [P] [US2] Add failing generated sub-asset expansion tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [X] T017 [P] [US2] Add failing drag payload preservation tests for generated grid items in existing asset drag/drop or presentation tests.
- [X] T018 [US2] Extend `Project/Editor/Assets/AssetBrowserPresentation.cpp` to merge source files with generated sub-assets from `AssetDatabaseFacade`.
- [X] T019 [US2] Render grid cards for source assets and generated sub-assets in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T020 [US2] Preserve selection, Asset Properties targeting, Asset View preview, prefab-stage open, scene load, and drag/drop behavior from grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T021 [US2] Run focused generated sub-asset and drag/drop regression tests. Verified with `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal`, `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (138/138 passed), `git diff --check`, and required `/plan-review` R2/R3/deeper-audit/R4 with final R4 score 71/80 and 0 P0/P1.

## Phase 5: User Story 3 - See Real Persistent Thumbnails (Priority: P1)

**Goal**: Generate and reuse persistent thumbnails for textures, materials, models, and prefabs with icon fallback.

**Independent Test**: Visible assets request thumbnails, cache fresh entries under `Library/AssetThumbnails/`, reuse unchanged entries after restart, and rebuild stale entries after source/artifact changes.

- [X] T022 [P] [US3] Add failing thumbnail request/status tests in `Tests/Unit/AssetThumbnailCacheTests.cpp`.
- [X] T023 [US3] Add `Project/Editor/Assets/AssetThumbnailService.h` and `Project/Editor/Assets/AssetThumbnailService.cpp` for thumbnail lookup, fallback, and generation scheduling.
- [X] T024 [US3] Implement texture thumbnail loading/reuse through the thumbnail service.
- [X] T025 [US3] Implement deterministic material thumbnail generation with stable fallback diagnostics.
- [X] T026 [US3] Implement deterministic model/prefab thumbnail generation with stable fallback diagnostics.
- [X] T027 [US3] Persist thumbnail image/metadata outputs under `Library/AssetThumbnails/` without writing under `Assets/`.
- [X] T028 [US3] Integrate thumbnail service status into grid card rendering in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T029 [US3] Run thumbnail cache/service automated tests. Verified initially with `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal`, `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetThumbnailCacheTests.ServiceAdoptsMatchingInFlightRequestAfterGenerationChange"` (1/1 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetThumbnailCacheTests.*"` (17/17 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (151/151 passed), and `git diff --check` (exit 0; LF/CRLF warnings only). Final cache validation after review fixes: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal` (exit 0), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetThumbnailCacheTests.*"` (20/20 passed), and broad focused regression (163/163 passed). Texture thumbnails now persist downsampled PNGs and metadata under `Library/AssetThumbnails/`; source/meta and generated artifact-manifest stamps participate in cache freshness; source textures with only `.meta` IDs can request thumbnails; corrupt cached PNGs with fresh metadata are rejected as stale; async decode now uses thread-local STB flip state. Later final-review fixes added pre-decode source PNG/BMP/TGA/JPEG size budgets, pre-deserialize mesh/texture artifact header budgets, material/prefab preview payload budgets, async exception-to-failed-result handling, publish-before-write freshness revalidation, obsolete in-flight generation nonblocking, and per-cache-key unique-temp cache writes. Material/model/prefab thumbnails are deterministic generated previews or stable fallbacks; focused editor/runtime visual verification and renderer-backed preview parity remain unclaimed.

## Phase 6: User Story 4 - Search And Filter Current Folder (Priority: P2)

**Goal**: Filter right-grid items by current-folder text search and type filter.

**Independent Test**: Mixed folders filter deterministically by query and type without changing selected folder.

- [X] T030 [P] [US4] Add failing search/type filter tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [X] T031 [US4] Implement current-folder search/type filtering in `Project/Editor/Assets/AssetBrowserPresentation.cpp`.
- [X] T032 [US4] Add search input and type filter control to `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T033 [US4] Run focused search/type filtering tests. Verified initially with `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal`, `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.FiltersCurrentFolderItemsByCaseInsensitiveSearchQuery:AssetBrowserPresentationTests.FiltersCurrentFolderItemsByTypeAndSearchQuery"` (2/2 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.*"` (15/15 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (153/153 passed), and `git diff --check` (exit 0; LF/CRLF warnings only). US4 review found stale thumbnail scope and filter-label issues; fixed by keying thumbnail generation on visible item scope, requerying dirty same-scope refreshes, sharing rounded request-size lookup, and moving type labels to `AssetBrowserItemTypeDisplayLabel()`. Post-fix validation: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal` (exit 0), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.ThumbnailGenerationScopeDecisionRequeriesDirtySameScope:AssetBrowserPresentationTests.ItemTypeDisplayLabelsCoverAllFilterOptions"` (2/2 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (156/156 passed), and `git diff --check` (exit 0; LF/CRLF warnings only). US4 plan-review R1 scored 66/80 with one P1 and one P2; R2 scored 72/80 with 0 P0/P1 and two P2 test-hardening notes, which were fixed. Deeper audit then found P1 cache-path race and P1 enum exhaustiveness drift; fixed by deriving thumbnail image/metadata filenames from full cache keys and centralizing filter options behind `AssetBrowserItemTypeFilterOptions()` with count-checked label coverage. Final post-P1-fix validation: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal` (exit 0), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetThumbnailCacheTests.CachePathsChangeWhenFreshnessChanges:AssetBrowserPresentationTests.ItemTypeDisplayLabelsCoverAllFilterOptions:AssetBrowserPresentationTests.ThumbnailGenerationScopeDecisionRequeriesDirtySameScope"` (3/3 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetThumbnailCacheTests.*"` (18/18 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.*"` (18/18 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (157/157 passed), `git diff --check` (exit 0; LF/CRLF warnings only), and `git diff --name-only -- 'Runtime/*/Gen/*' 'Project/*/Gen/*'` (empty).

## Phase 7: User Story 5 - Preserve Existing Asset Workflows (Priority: P1)

**Goal**: Keep existing create/import/reimport/rename/delete/duplicate/preview/drag/drop workflows available in the new panel.

**Independent Test**: Existing asset operations can be exercised from the new tree/grid and produce equivalent project/editor state.

- [X] T034 [US5] Port folder context menu actions to the left tree and folder grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T035 [US5] Port file/source asset context menu actions to grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T036 [US5] Preserve hierarchy-object drop-to-folder prefab creation in the new folder targets in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T037 [US5] Preserve watcher/preimport refresh scheduling after create/move/import/reimport operations in `Project/Editor/Panels/AssetBrowser.cpp`.
- [X] T038 [US5] Run focused existing asset drag/drop, importer, and panel workflow tests. Implemented native ImGui context menus for Unity-style folder tree/grid cards and source asset cards, guarded generated sub-assets from destructive file operations, restored material edit/asset preview/resource actions through legacy editor workflows, connected folder drop targets for folder/file/editor-asset/hierarchy-object payloads, and scheduled `AssetCopiedOrMoved` preimport plus presentation refresh after workflow mutations. This preserves the current Nullus direct-file workflow surface; a Unity-grade command transaction/undo-redo asset operation layer is not claimed in this feature. Later review fixes restored generated sub-asset Asset Properties targeting, marked object-reference picker sub-asset payloads as generated/read-only for physical moves, and hardened physical file drops so non-engine file moves must resolve inside the active project `Assets/` root. Verified with `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal` (exit 0), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetBrowserPresentationTests.WorkflowCapabilitiesExposePhysicalFolderAndSourceAssetActionsOnly:EditorAssetDragDropTests.DropsHierarchyObjectIntoAssetBrowserFolderCreatesPrefabSourceAsset"` (2/2 passed), `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*"` (158/158 passed), final broad focused regression (163/163 passed), `git diff --check` (exit 0; LF/CRLF warnings only), and `git diff --name-only -- 'Runtime/*/Gen/*' 'Project/*/Gen/*'` (empty).

## Phase 8: Validation And Review

- [X] T039 Run focused `NullusUnitTests.exe` filters for AssetBrowserPresentation, AssetThumbnailCache, EditorAssetDragDrop, AssetImportPipeline, and impacted panel tests. Earlier evidence: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /v:minimal` (exit 0) and `build\bin\Debug\NullusUnitTests.exe --gtest_filter="PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*:GameObjectAssetImportTests.EditorAssetDragPayload*:ReflectedPropertyDrawerTests.UnityObjectReference*"` (163/163 passed). Final-review fixes then added extension SSoT coverage, drag-source I/O deferral, strict native material/prefab preview reads, sync/async thumbnail exception envelopes, generated-sub-asset Properties path normalization, nonblocking retired project-database refresh futures, the async thumbnail worker recursion fix, service-level generated artifact symlink rejection, generated material/prefab artifact invalid-path source-fallback suppression, shared editor artifact physical-file containment helper, and DX12 current-frame transient texture retirement discard handling. Latest validation after P0/P1/P2 review fixes: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` (exit 0), `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /nodeReuse:false /v:minimal` (exit 0), targeted texture/material/prefab artifact symlink and normal material/model/prefab generation filter (4/4 passed), current-frame retirement tracker/null bridge filter (3/3 passed), `UIAndToolingBackendAwarenessTests.*` (13/13 passed), `AssetThumbnailCacheTests.*` (50/50 passed), and broad focused regression `PathParserTests.*:AssetBrowserPresentationTests.*:AssetThumbnailCacheTests.*:EditorAssetDragDropTests.*:AssetImportPipelineTests.*:AssetDatabaseFacadeTests.AssetBrowser*:AssetDatabaseFacadeTests.LoadsPersistedPrefabArtifactByAssetIdWhenSourcePathIndexIsMissing:AssetDatabaseFacadeTests.RejectsPersistedPrefabArtifactOutsidePhysicalArtifactRoot:AssetDatabaseFacadeTests.RejectsPersistedPrefabArtifactSymlinkInsidePhysicalArtifactRoot:GameObjectAssetImportTests.EditorAssetDragPayload*:ReflectedPropertyDrawerTests.UnityObjectReference*:AssetFoundationTests.*:UIAndToolingBackendAwarenessTests.*` (251/251 passed). `cmake --build build --config Debug --target Editor -- /m:1 /nr:false /nodeReuse:false /v:minimal` produced `App\Win64_Debug_Runtime_Shared\Editor.exe`; `git diff --check` exited 0 with LF/CRLF warnings only, generated-file diff was empty, and `git ls-files --others --exclude-standard` was empty. Post-cleanup validation after the final naming/status edits: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nr:false /nodeReuse:false /v:minimal` (exit 0), `AssetThumbnailCacheTests.*` (50/50 passed), `UIAndToolingBackendAwarenessTests.*` (13/13 passed), broad focused regression (251/251 passed), `cmake --build build --config Debug --target Editor -- /m:1 /nr:false /nodeReuse:false /v:minimal` (exit 0), `git diff --check` (exit 0; LF/CRLF warnings only), generated-file diff empty, and untracked-file check empty.
- [ ] T040 Perform quickstart manual editor verification from `specs/unity-asset-browser-parity/quickstart.md`. Not executed in this environment: `Editor` target builds successfully to `App\Win64_Debug_Runtime_Shared\Editor.exe`, but this worktree has no `.nullus` project and no GUI interaction evidence was captured.
- [X] T041 Self-review touched files for generated-file boundaries, workflow regressions, cache path safety, stale thumbnail races, thumbnail UI texture retirement, and cross-backend claims. Checked source texture `.meta` fallback IDs, corrupt PNG stale detection, fast-vs-full cache integrity semantics, generated sub-asset Properties routing, generated payload physical-move guards, current-folder refresh planning, source/artifact/material/prefab thumbnail budget guards, cached thumbnail decode size caps, exception-safe sync/async generation, stale persisted manifest rejection before known-current publication, shared artifact-root symlink rejection in both database and thumbnail service paths, generated material/prefab artifact invalid-path source-fallback suppression, object-reference picker snapshot locking/caching, obsolete thumbnail future scheduling without cancelling adoptable in-flight work, retired project-database futures for root changes, same-root in-flight refresh preservation, Asset Properties generated-sub-asset lookup without foreground `Refresh()`, nonblocking DX12 thumbnail view retirement including current-frame discard vs submit ownership, internal-only RHI retirement tracker exposure, and material/model/prefab thumbnail claims. No cross-backend runtime/rendering claim is made; renderer-backed thumbnails for material/model/prefab and Unity-grade asset-operation transaction/undo-redo parity remain out of scope until dedicated evidence/work exists.
- [X] T042 Run required `/plan-review` gate with at least two complete review rounds and one deeper audit; fix P0/P1/P2 findings per repository policy. Final gate evidence: R2 passed at 71/80 with 0 P0/P1 after fixing earlier findings; deeper audit passed at 67/80 with 0 P0/P1; final multi-agent review passed with 0 P0/P1 across GPU/RHI correctness, architecture/performance, code quality/SSoT, and industry/docs audits. Accepted P2 trade-offs: symlink tests may skip on OSes without symlink privileges, no real DX12 device-level thumbnail bridge test exists yet, path validation remains local editor trust-model check-then-open hardening rather than hostile-filesystem open-by-handle hardening, `AssetBrowser` class splitting is deferred, directory child probing can still do main-thread filesystem IO in very large projects, and renderer-backed Unity `AssetPreview` parity plus Unity-grade asset-operation undo/redo remain explicitly out of scope.
- [X] T043 Update `specs/unity-asset-browser-parity/tasks.md` with final completed task status and validation evidence. Updated with latest build/test/static-check evidence after review-fix implementation, final review-gate evidence, and post-cleanup validation evidence; T040 remains unchecked because no GUI quickstart evidence was captured in this environment.

## Dependencies

- T004-T010 block user story implementation.
- US1 layout tasks T011-T015 should complete before US2 grid sub-asset rendering.
- US2 grid item semantics should complete before US3 thumbnail integration.
- US4 filtering depends on the presentation item model from US1/US2.
- US5 workflow preservation can proceed after grid/tree targets exist from US1/US2.
- Final validation depends on all selected user stories and review fixes.

## Parallel Opportunities

- T004, T005, and T006 can be created in parallel.
- T016 and T017 can be created in parallel after presentation contracts exist.
- T022 can proceed in parallel with late US2 UI work once thumbnail cache primitives exist.
- US4 tests can be written while US3 thumbnail generation is being implemented.

## Implementation Strategy

1. Establish deterministic presentation and cache contracts first.
2. Deliver the split project-only browser layout as MVP.
3. Add generated sub-asset grid visibility and preserve drag/drop.
4. Add persistent thumbnail behavior with stable generated previews/fallbacks.
5. Add search/type filter controls.
6. Reconcile all old context menu and watcher workflows.
7. Validate, review, and only then report completion.
