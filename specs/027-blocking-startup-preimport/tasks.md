# Tasks: Blocking Startup Preimport

**Input**: Design documents from `specs/027-blocking-startup-preimport/`
**Prerequisites**: `spec.md`, `plan.md`

**Tests**: Required because this changes editor startup behavior.

## Phase 1: Setup

- [x] T001 Create spec bundle in `specs/027-blocking-startup-preimport/`
- [x] T002 Inspect startup order in `Project/Editor/Core/Application.cpp`, `Project/Editor/Core/Context.cpp`, `Project/Editor/Core/Editor.cpp`, and `Project/Editor/Panels/AssetBrowser.cpp`

---

## Phase 2: Failing Tests

- [x] T003 [US1] Add failing startup preimport behavior test in `Tests/Unit/EditorAssetDatabaseTests.cpp`
- [x] T004 [US1] Run `cmake --build Build --target NullusUnitTests --config Debug` and confirm failure because `Assets/EditorStartupAssetPreimport.h` does not exist
- [x] T005 [US1] Add startup ordering and AssetBrowser no-startup-import contract test in `Tests/Unit/EditorRenderPathContractTests.cpp`

---

## Phase 3: User Story 1 - Open Editor With Hot Initial Assets (Priority: P1)

**Goal**: Initial model/prefab assets are imported before the editor UI opens.

**Independent Test**: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorAssetDatabaseTests.BlockingStartupPreimportWarmsColdModelBeforeReturning`

- [x] T006 [US1] Add `Project/Editor/Assets/EditorStartupAssetPreimport.h`
- [x] T007 [US1] Add `Project/Editor/Assets/EditorStartupAssetPreimport.cpp`
- [x] T008 [US1] Wire `Project/Editor/Core/Application.cpp` to call blocking startup preimport before constructing `Editor`, `RunEditorFrame(0.0f)`, and `CompleteStartupProgress()`
- [x] T009 [US1] Add new startup preimport files to `Tests/Unit/CMakeLists.txt`

---

## Phase 4: User Story 2 - Incremental Imports After UI Opens (Priority: P2)

**Goal**: After UI opens, Asset Browser handles only watcher/copy/move changed-path imports.

**Independent Test**: `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorRenderPathContractTests.StartupPreimportRunsBeforeEditorUiAndAssetBrowserDoesNotScheduleStartupImport`

- [x] T010 [US2] Remove `AssetPreimportReason::EditorStartup` scheduling from Asset Browser watcher-ready first-draw path in `Project/Editor/Panels/AssetBrowser.cpp`
- [x] T011 [US2] Keep `FileWatcherChanged` and `AssetCopiedOrMoved` scheduling intact in `Project/Editor/Panels/AssetBrowser.cpp`
- [x] T012 [US2] Keep editor window hidden until startup completion on all platforms in `Project/Editor/Core/Context.cpp`
- [x] T012a [US2] Arm asset file watchers before startup preimport and consume watcher-observed changed paths before `CompleteStartupProgress()`
- [x] T012b [US2] Close startup progress dialog with RAII ownership instead of release/detach
- [x] T012c [US2] Keep generated-model renderer resource resolution out of the native blocking task-progress dialog in `Project/Editor/Core/Context.cpp`
- [x] T012d [US2] Avoid synchronous cold material loads during generated-model renderer resource resolution in `Project/Editor/Core/EditorActions.cpp`
- [x] T012e [US2] Add source-contract coverage for non-blocking renderer resource resolution in `Tests/Unit/EditorRenderPathContractTests.cpp`
- [x] T012f [US2] Add source-contract coverage that Scene View and Hierarchy model/prefab drops use imported `EditorAssetDragPayload` handles instead of legacy file-path payloads
- [x] T012g [US2] Remove legacy file-path model/prefab instantiation targets from `Project/Editor/Panels/SceneView.cpp` and `Project/Editor/Panels/Hierarchy.cpp`
- [x] T012h [US2] Remove legacy file-path model preview synchronous loads from `Project/Editor/Panels/AssetView.cpp`
- [x] T012i [US2] Canonicalize project `Library/...` model package and mesh artifact cache keys so startup prewarm and later absolute drag/runtime requests reuse the same `ModelManager` resources.
- [x] T012j [US2] Require successful main model package prewarm before renderer mesh alias tasks proceed, and skip startup material prewarm unless material, shader, and texture manager services are registered.

---

## Phase 5: Validation

- [x] T013 Run `cmake -S . -B Build`
- [x] T014 Run `cmake --build Build --target NullusUnitTests --config Debug`
- [x] T015 Run `cmake --build Build --target Editor --config Debug`
- [x] T016 Run focused startup/preimport and drag/drop regression tests
- [x] T017 Run plan-review quality gate before final report

## Dependencies

- Phase 2 must precede production code.
- US1 must precede US2 validation because Asset Browser no longer owns startup import.
- Validation follows all code changes.
