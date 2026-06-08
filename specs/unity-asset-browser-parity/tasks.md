# Tasks: Unity Asset Browser Parity

**Input**: Design documents from `specs/unity-asset-browser-parity/`  
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `quickstart.md`  
**Tests**: Required for deterministic behavior changes and cache logic. Rendered thumbnail visuals also require focused runtime/manual verification.

## Phase 1: Setup

- [ ] T001 Confirm the isolated worktree branch and document that `.specify` templates were read from the main local checkout because `.specify/` is gitignored.
- [ ] T002 Inspect existing `Project/Editor/Panels/AssetBrowser.*`, `Project/Editor/Assets/AssetBrowserPresentation.*`, and related drag/drop tests for reusable workflows.
- [ ] T003 [P] Identify existing UI widget/ImGui capabilities needed for split panes, search box, type filter, grid items, and thumbnail-size slider.

## Phase 2: Foundational Presentation And Cache Contracts

- [ ] T004 [P] Add failing presentation tests for project-only folder tree and current-folder direct content in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [ ] T005 [P] Add failing presentation tests for breadcrumb construction and folder selection fallback in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [ ] T006 [P] Add failing thumbnail cache key/path/freshness tests in `Tests/Unit/AssetThumbnailCacheTests.cpp`.
- [ ] T007 Implement project-only folder/content presentation types in `Project/Editor/Assets/AssetBrowserPresentation.h`.
- [ ] T008 Implement project-only folder/content presentation logic in `Project/Editor/Assets/AssetBrowserPresentation.cpp`.
- [ ] T009 Implement thumbnail cache request/key/freshness primitives in `Project/Editor/Assets/AssetThumbnailCache.h` and `Project/Editor/Assets/AssetThumbnailCache.cpp`.
- [ ] T010 Wire new source files into `Project/Editor/CMakeLists.txt` and `Tests/Unit/CMakeLists.txt`.

## Phase 3: User Story 1 - Navigate Project Assets Like Unity (Priority: P1)

**Goal**: Replace the old one-tree browser with a project-only split tree/grid layout.

**Independent Test**: Select folders under `Assets/` and verify the right grid and breadcrumb reflect only current-folder project content.

- [ ] T011 [US1] Refactor `Project/Editor/Panels/AssetBrowser.h` state around selected folder, search/filter state, thumbnail size, and presentation item lists.
- [ ] T012 [US1] Replace the old engine/project tree fill path in `Project/Editor/Panels/AssetBrowser.cpp` with left project-folder navigation and right current-folder content rendering.
- [ ] T013 [US1] Add breadcrumb rendering and selected-folder fallback handling in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T014 [US1] Preserve manual refresh/import toolbar actions in the new layout in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T015 [US1] Run focused presentation tests for project-only navigation.

## Phase 4: User Story 2 - Inspect Imported Assets In A Thumbnail Grid (Priority: P1)

**Goal**: Show source assets and generated imported sub-assets as independent grid items.

**Independent Test**: Imported model folders show source file plus generated prefab, mesh, material, and texture items with correct selection/drag payload metadata.

- [ ] T016 [P] [US2] Add failing generated sub-asset expansion tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [ ] T017 [P] [US2] Add failing drag payload preservation tests for generated grid items in existing asset drag/drop or presentation tests.
- [ ] T018 [US2] Extend `Project/Editor/Assets/AssetBrowserPresentation.cpp` to merge source files with generated sub-assets from `AssetDatabaseFacade`.
- [ ] T019 [US2] Render grid cards for source assets and generated sub-assets in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T020 [US2] Preserve selection, Asset Properties targeting, Asset View preview, prefab-stage open, scene load, and drag/drop behavior from grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T021 [US2] Run focused generated sub-asset and drag/drop regression tests.

## Phase 5: User Story 3 - See Real Persistent Thumbnails (Priority: P1)

**Goal**: Generate and reuse real thumbnails for textures, materials, models, and prefabs with icon fallback.

**Independent Test**: Visible assets request thumbnails, cache fresh entries under `Library/AssetThumbnails/`, reuse unchanged entries after restart, and rebuild stale entries after source/artifact changes.

- [ ] T022 [P] [US3] Add failing thumbnail request/status tests in `Tests/Unit/AssetThumbnailCacheTests.cpp`.
- [ ] T023 [US3] Add `Project/Editor/Assets/AssetThumbnailService.h` and `Project/Editor/Assets/AssetThumbnailService.cpp` for thumbnail lookup, fallback, and generation scheduling.
- [ ] T024 [US3] Implement texture thumbnail loading/reuse through the thumbnail service.
- [ ] T025 [US3] Implement material sphere thumbnail generation or generation hook with stable fallback diagnostics.
- [ ] T026 [US3] Implement model/prefab thumbnail generation or generation hook with stable fallback diagnostics.
- [ ] T027 [US3] Persist thumbnail image/metadata outputs under `Library/AssetThumbnails/` without writing under `Assets/`.
- [ ] T028 [US3] Integrate thumbnail service status into grid card rendering in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T029 [US3] Run thumbnail cache tests and focused editor/runtime thumbnail verification.

## Phase 6: User Story 4 - Search And Filter Current Folder (Priority: P2)

**Goal**: Filter right-grid items by current-folder text search and type filter.

**Independent Test**: Mixed folders filter deterministically by query and type without changing selected folder.

- [ ] T030 [P] [US4] Add failing search/type filter tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`.
- [ ] T031 [US4] Implement current-folder search/type filtering in `Project/Editor/Assets/AssetBrowserPresentation.cpp`.
- [ ] T032 [US4] Add search input and type filter control to `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T033 [US4] Run focused search/type filtering tests.

## Phase 7: User Story 5 - Preserve Existing Asset Workflows (Priority: P1)

**Goal**: Keep existing create/import/reimport/rename/delete/duplicate/preview/drag/drop workflows available in the new panel.

**Independent Test**: Existing asset operations can be exercised from the new tree/grid and produce equivalent project/editor state.

- [ ] T034 [US5] Port folder context menu actions to the left tree and folder grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T035 [US5] Port file/source asset context menu actions to grid cards in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T036 [US5] Preserve hierarchy-object drop-to-folder prefab creation in the new folder targets in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T037 [US5] Preserve watcher/preimport refresh scheduling after create/move/import/reimport operations in `Project/Editor/Panels/AssetBrowser.cpp`.
- [ ] T038 [US5] Run focused existing asset drag/drop, importer, and panel workflow tests.

## Phase 8: Validation And Review

- [ ] T039 Run focused `NullusUnitTests.exe` filters for AssetBrowserPresentation, AssetThumbnailCache, EditorAssetDragDrop, AssetImportPipeline, and impacted panel tests.
- [ ] T040 Perform quickstart manual editor verification from `specs/unity-asset-browser-parity/quickstart.md`.
- [ ] T041 Self-review touched files for generated-file boundaries, workflow regressions, cache path safety, stale thumbnail races, and cross-backend claims.
- [ ] T042 Run required `/plan-review` gate with at least two complete review rounds and one deeper audit; fix P0/P1/P2 findings per repository policy.
- [ ] T043 Update `specs/unity-asset-browser-parity/tasks.md` with completed task status and validation evidence.

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
4. Add persistent high-fidelity thumbnail behavior with fallback.
5. Add search/type filter controls.
6. Reconcile all old context menu and watcher workflows.
7. Validate, review, and only then report completion.
