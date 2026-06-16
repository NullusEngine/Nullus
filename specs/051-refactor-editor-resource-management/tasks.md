# Tasks: Editor Resource Management Refactor

**Input**: Design documents from `/specs/051-refactor-editor-resource-management/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

## Phase 1: Setup (Shared Infrastructure)

- [x] T001 Create `Project/Editor/Core/EditorResourceCatalog.h` and `Project/Editor/Core/EditorResourceCatalog.cpp` skeletons for the unified resource index
- [x] T002 Add `tests/unit/EditorResourceCatalogTests.cpp` for executable-relative lookup and stable ID resolution
- [x] T003 Update `Tests/Unit/CMakeLists.txt` if needed so the new catalog tests compile with `NullusUnitTests`

---

## Phase 2: Foundational (Blocking Prerequisites)

- [x] T004 Implement executable-relative root resolution in `Project/Editor/Core/Context.cpp` and `Project/Game/Core/Context.cpp`
- [x] T005 Implement the shared editor resource catalog backend interface in `Project/Editor/Core/EditorResourceCatalog.h` and `Project/Editor/Core/EditorResourceCatalog.cpp`
- [x] T006 [P] Add catalog resource path coverage tests in `Tests/Unit/EditorResourceCatalogTests.cpp`
- [x] T007 [P] Add catalog failure and backend-mode tests in `Tests/Unit/EditorResourceCatalogTests.cpp`
- [x] T008 Refactor `Project/Editor/Core/EditorResources.h` and `Project/Editor/Core/EditorResources.cpp` to delegate lookups to the new catalog

---

## Phase 3: User Story 1 - Stable Editor Resource Lookup (Priority: P1)

**Goal**: Editor resources resolve correctly regardless of current working directory.

**Independent Test**: Run catalog/path unit tests and confirm startup resource root resolution no longer depends on CWD.

- [x] T009 [US1] Replace `../Assets/...` resource root assumptions in `Project/Editor/Core/Context.cpp`
- [x] T010 [US1] Replace `../Assets/...` launcher resource assumptions in `Project/Launcher/Core/Launcher.cpp`
- [x] T011 [US1] Replace editor font path assumptions in `Project/Editor/Core/Context.cpp`
- [x] T012 [P] [US1] Add unit coverage for launcher/editor resource root resolution in `Tests/Unit/EditorResourceCatalogTests.cpp`

---

## Phase 4: User Story 2 - Unified Catalog Across Development and Release (Priority: P1)

**Goal**: Editor resource IDs resolve through one catalog in both development and packaged modes.

**Independent Test**: Catalog tests resolve the same IDs through loose-file and packaged backend paths.

- [x] T013 [US2] Populate `Project/Editor/Core/EditorResourceCatalog.cpp` with Nullus-style IDs for editor fonts, brand, icons, helper models, and shaders
- [x] T014 [US2] Wire `Project/Editor/Core/EditorResources.cpp` to load from catalog IDs instead of raw file-name strings
- [x] T015 [US2] Update `Project/Editor/Panels/Toolbar.cpp` and `Project/Editor/Panels/AssetBrowser.cpp` to consume catalog-backed editor resources
- [x] T016 [P] [US2] Add packaged-backend contract coverage to `Tests/Unit/EditorResourceCatalogTests.cpp`

---

## Phase 5: User Story 3 - Clean Nullus Icon Set (Priority: P2)

**Goal**: Retained editor icons use Nullus naming, and only used icons remain.

**Independent Test**: Resource catalog and icon-file assertions show no Unity naming and no unused retained icons.

- [x] T017 [US3] Rename fallback icon IDs in `Project/Editor/Assets/AssetBrowserPresentation.cpp` to Nullus-style names
- [x] T018 [US3] Rename thumbnail fallback icon IDs in `Project/Editor/Assets/AssetThumbnailService.cpp` to Nullus-style names
- [x] T019 [US3] Replace Unity-named icon overrides in `Project/Editor/Core/EditorResources.cpp` with Nullus-named icon assets under `App/Assets/Editor`
- [x] T020 [P] [US3] Update `Tests/Unit/AssetBrowserPresentationTests.cpp` to assert the new Nullus icon IDs and no Unity names remain
- [x] T021 [P] [US3] Remove unused Unity-named editor icon files and any now-dead references under `App/Assets/Editor/Icon`

---

## Phase 6: User Story 4 - Asset Previews Remain Separate From Editor Icons (Priority: P3)

**Goal**: Asset previews remain thumbnail-cache driven and distinct from static editor icons.

**Independent Test**: Previewable assets still generate thumbnails while non-previewable assets use the new fallback icon IDs.

- [x] T022 [US4] Keep `Project/Editor/Assets/AssetThumbnailService.cpp` preview generation on ThumbnailCache paths while swapping fallback IDs to the cataloged icon names
- [x] T023 [US4] Update `Project/Editor/Panels/AssetBrowser.cpp` rendering paths so preview textures and catalog icons remain visually distinct
- [x] T024 [P] [US4] Add thumbnail cache regression coverage in `Tests/Unit/AssetThumbnailCacheTests.cpp`
- [x] T025 [P] [US4] Add prefab and material preview coverage in `Tests/Unit/AssetBrowserPresentationTests.cpp`

---

## Phase 7: Polish & Cross-Cutting Concerns

- [x] T026 [P] Add `specs/051-refactor-editor-resource-management/contracts/` notes for the resource catalog backend contract if needed
- [x] T027 Remove obsolete Unity-named icon references from remaining editor tests and call sites
- [x] T028 Verify `App/Assets/Editor` and `App/Assets/Engine/Brand` contain only retained resources used by the catalog
- [x] T029 Run focused unit tests for catalog, asset browser presentation, thumbnail cache, and launcher/editor startup behavior

---

## Dependencies & Execution Order

- Phase 1 creates the new catalog surface and tests.
- Phase 2 blocks all story work by establishing the shared backend and executable-relative roots.
- US1 must land before the remaining UI/resource migration work.
- US2 depends on the catalog and provides the stable ID layer.
- US3 can proceed after US2 because it renames the public icon surface.
- US4 depends on US2 and US3 because preview fallback IDs and icon naming must be settled first.

## Parallel Opportunities

- T006 and T007 can run in parallel.
- T012 can run alongside other US1 work once the catalog API exists.
- T016 can run in parallel with US2 implementation after the catalog backend exists.
- T020 and T021 can run in parallel once the new icon IDs are chosen.
- T024 and T025 can run in parallel after preview fallback IDs are updated.

## Implementation Strategy

### MVP First

1. Complete the catalog skeleton and executable-relative path resolver.
2. Prove the new lookup behavior with tests.
3. Migrate the editor and launcher call sites.
4. Rename and prune the icon set.
5. Keep thumbnail generation separate and verify previews still work.

### Incremental Delivery

1. Make resource lookup stable.
2. Introduce the unified catalog.
3. Clean up icon IDs and files.
4. Preserve preview behavior.
5. Finish with verification and pruning.
