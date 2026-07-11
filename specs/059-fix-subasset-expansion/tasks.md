# Tasks: Unity-Aligned Sub-Asset Expansion

**Input**: Design documents from `/specs/059-fix-subasset-expansion/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Required. Every behavior-changing implementation task follows a failing focused test task.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches separate files and has no dependency on unfinished work.
- **[Story]**: Maps the task to a user story in `spec.md`.

## Phase 1: Setup and Baseline

**Purpose**: Freeze scope and establish repeatable evidence without modifying existing user work.

- [x] T001 Record the current branch, dirty-worktree baseline, and focused test inventory in `specs/059-fix-subasset-expansion/quickstart.md`
- [x] T002 Run the current focused Asset Browser/facade tests and record pre-change failures or baseline results in `specs/059-fix-subasset-expansion/quickstart.md`
- [x] T003 Verify `NLS_ENABLE_TEST_HOOKS` and the existing `NullusUnitTests`/`Editor` targets are available using `CMakeLists.txt` and `Tests/Unit/CMakeLists.txt`

---

## Phase 2: Foundational Immutable State

**Purpose**: Establish canonical identity and coherent facade publication required by all user stories.

**CRITICAL**: No story implementation begins until this phase is green.

- [ ] T004 Add failing canonical project-path and defaulted `AssetBrowserItem` equality tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [ ] T005 Implement shared canonical project-path normalization in `Project/Editor/Assets/EditorAssetPath.h` and defaulted item equality in `Project/Editor/Assets/AssetBrowserPresentation.h`
- [ ] T006 [P] Add failing valid/error/index identity, two-level state reuse, same-count replacement, and read/write synchronization tests in `Tests/Unit/EditorAssetDatabaseTests.cpp`
- [ ] T007 Add failing off-lock G1/G2/G3 compare-and-publish, lock-scope hook, duplicate/invalid path, and contradictory/unknown status tests in `Tests/Unit/EditorAssetDatabaseTests.cpp`
- [ ] T008 Implement consumer-neutral snapshot/index/diagnostic and immutable `FacadePublishedState` types plus `GetPublishedState()` in `Project/Editor/Assets/AssetDatabaseFacade.h`
- [ ] T009 Implement off-lock candidate build/validation, two-level identity reuse, O(1) locked compare-and-publish, and lock telemetry in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [ ] T010 Migrate `CreateReadOnlySnapshot` and all facade manifest/current/snapshot writers to aggregate state publication in `Project/Editor/Assets/AssetDatabaseFacade.cpp`
- [ ] T011 Migrate direct facade tests and already-compatible consumers to `GetPublishedState()` in `Tests/Unit/EditorAssetDatabaseTests.cpp` while retaining remaining legacy declarations until panel/picker story migration completes
- [ ] T012 Run `EditorAssetDatabaseTests.FacadePublishedState*` and `EditorAssetDatabaseTests.SnapshotIndexError*` from `Build/bin/Debug/NullusUnitTests.exe`

**Checkpoint**: Every facade consumer can retain one coherent current state or an explicit error state.

---

## Phase 3: User Story 1 - Trust Expanded Sub-Assets (Priority: P1) MVP

**Goal**: Exact per-source child membership/count under all supported filters with no speculative children.

**Independent Test**: Interleaved sources and the complete filter matrix produce exact counts, stable order, and no cross-source membership.

### Tests for User Story 1

- [ ] T013 [US1] Add failing exhaustive artifact eligibility, missing snapshot, fallback-name, interleaved-source, and source/child filter-matrix tests with separate expanded emitted-count and collapsed disclosure-membership assertions in `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [ ] T014 [US1] Add failing action-key delimiter, duplicate, same-path changed-`AssetId`, group ID, thumbnail-root, invalid build-key, and deterministic linear operation-count tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`

### Implementation for User Story 1

- [ ] T015 [US1] Define immutable root/expansion/filter views, structured action keys, validated result classes, display group IDs, and presentation bundle types in `Project/Editor/Assets/AssetBrowserPresentation.h`
- [ ] T016 [US1] Implement one exhaustive generated-child projection and group-aware filter/count/emission path in `Project/Editor/Assets/AssetBrowserPresentation.cpp`
- [ ] T017 [US1] Implement pure validated `BuildAssetBrowserPresentationBundle` with action index and thumbnail roots in `Project/Editor/Assets/AssetBrowserPresentation.cpp`
- [ ] T018 [US1] Replace panel count hints, snapshot pointer maps, materialized children, and progressive display prefixes with immutable roots/expansion/filter inputs in `Project/Editor/Panels/AssetBrowser.h`
- [ ] T019 [US1] Build current-folder root snapshots from the exact facade state and remove refresh-time child-count fabrication in `Project/Editor/Panels/AssetBrowser.cpp`
- [ ] T020 [US1] Run `AssetBrowserPresentationTests.ExpansionProjection*` and `AssetBrowserPresentationTests.PresentationBundle*` from `Build/bin/Debug/NullusUnitTests.exe`

**Checkpoint**: Expanded/collapsed sources report exact proven child state independently of async publication.

---

## Phase 4: User Story 2 - Filter and Refresh Without Stale Actions (Priority: P1)

**Goal**: Publish only current complete bundles and current-state picker/action/thumbnail data.

**Independent Test**: G1/G2 update orders, rapid semantic changes, failures, replacement identity, and teardown never expose stale actionable state.

### Tests for User Story 2

- [ ] T021 [US2] Add failing fake-scheduler tests for all presentation coordinator state transitions, single-flight/latest-key, mixed-generation rejection, stale/current result branches, rejection, exception, unchanged-failure suppression, retained retry-identity replacement/non-aliasing, fail-closed epoch exhaustion, thumbnail late completion, public cache-key mismatch, worker-Full `EvaluateAssetThumbnailCache` Fresh/Stale/Missing/Failed branches, post-evaluation state change, zero main-thread Full/hash calls, and bounded burst publication after same-path replacement in `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [ ] T022 [US2] Add failing project-wide picker result/cache state binding, G1/G2 pending window, same-path replacement, Browser-visibility independence, scheduler failure, exception/cancel, retry, main-thread assertion, owner A closed to owner B fresh lifetime, same-facade lifetime non-aliasing, late A completion rejection, and teardown tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`

### Implementation for User Story 2

- [ ] T023 [US2] Implement the explicit `Idle/Loading/Success/Failure/Closed` presentation coordinator, nonthrowing scheduling, retained retry identity, and single-flight/latest-key behavior without `this` capture in `Project/Editor/Panels/AssetBrowser.cpp` and `Project/Editor/Panels/AssetBrowser.h`
- [ ] T024 [US2] Publish only current successful bundles and resolve selection/actions through structured authoritative keys in `Project/Editor/Panels/AssetBrowser.cpp`
- [ ] T025 [US2] Key thumbnail generation/decode/upload by active bundle identity, epoch, authoritative action identity, request size, immutable complete `AssetThumbnailRequest`, and public `BuildAssetThumbnailCacheKey`; perform Full `EvaluateAssetThumbnailCache` on the worker, then revalidate bundle/action/current cache key on the main-thread pump immediately before Fresh-only binding in `Project/Editor/Panels/AssetBrowser.cpp` and `Project/Editor/Panels/AssetBrowser.h`
- [ ] T026 [US2] Implement factory-created picker results, result-bearing provider, retained `PickerCacheLifetime`, lifetime/state-bound main-thread cache, diagnostic getter, and cache mutation helpers in `Project/Editor/Assets/AssetBrowserPresentation.h` and `Project/Editor/Assets/AssetBrowserPresentation.cpp`
- [ ] T027 [US2] Replace picker generation/vector futures with state-identity single-flight scheduling and fail-closed completion in `Project/Editor/Panels/AssetBrowser.h` and `Project/Editor/Panels/AssetBrowser.cpp`
- [ ] T028 [US2] Migrate picker diagnostics and guarded current entries into `Project/Editor/Panels/ReflectedPropertyDrawer.cpp`
- [ ] T029 [US2] Run `AssetBrowserPresentationTests.PresentationCoordinator*` and `AssetBrowserPresentationTests.PickerCache*` from `Build/bin/Debug/NullusUnitTests.exe`

**Checkpoint**: No stale display, selection, picker, action, or thumbnail state survives a semantic input change.

---

## Phase 5: User Story 3 - See Connected Unity-Style Groups (Priority: P2)

**Goal**: Continuous clipped generated-child backgrounds in grid and list modes.

**Independent Test**: Pure geometry and deterministic pixel output cover single/multi-child, wrap, residual width, list slice, and active clipping.

### Tests for User Story 3

- [ ] T030 [US3] Add failing pure grid/list segment geometry, residual-width, wrapped-row true segment outer-corner, actual list group outer-corner, and clipped-list square-continuation tests in `Tests/Unit/AssetBrowserPresentationTests.cpp`
- [ ] T031 [US3] Add failing RAII ImGui context and deterministic software raster tests for offsets, alpha, internal seams, rounded edges, and parent clipping in `Tests/Unit/AssetBrowserPresentationTests.cpp`

### Implementation for User Story 3

- [ ] T032 [US3] Define pure group-segment geometry inputs/results in `Project/Editor/Assets/AssetBrowserPresentation.h`
- [ ] T033 [US3] Implement maximal grid-row and visible-list group segment resolution in `Project/Editor/Assets/AssetBrowserPresentation.cpp`
- [ ] T034 [US3] Replace per-child filmstrip fills with one intersect-clipped fill per segment in `Project/Editor/Panels/AssetBrowser.cpp`
- [ ] T035 [US3] Preserve source exclusion and item-local hover/selection overlay ordering in grid/list drawing paths in `Project/Editor/Panels/AssetBrowser.cpp`
- [ ] T036 [US3] Run `AssetBrowserPresentationTests.GeneratedGroupGeometry*` and `AssetBrowserPresentationTests.GeneratedGroupImGuiRaster*` from `Build/bin/Debug/NullusUnitTests.exe`

**Checkpoint**: Generated groups are visually connected without painting outside the active content clip.

---

## Phase 6: Polish and Cross-Cutting Verification

**Purpose**: Remove obsolete paths and collect complete evidence.

- [ ] T037 Run the deterministic presentation scale/operation-count tests from T014, facade lock-scope tests from T007, and thumbnail main-thread file-read/hash plus burst-publication telemetry tests from T021 to verify documented budgets in `Build/bin/Debug/NullusUnitTests.exe`
- [ ] T038 Remove legacy progressive/count-hint/picker-vector APIs and caches from `Project/Editor/Assets/AssetDatabaseFacade.h`, `Project/Editor/Assets/AssetDatabaseFacade.cpp`, `Project/Editor/Assets/AssetBrowserPresentation.h`, `Project/Editor/Assets/AssetBrowserPresentation.cpp`, `Project/Editor/Panels/AssetBrowser.h`, and `Project/Editor/Panels/AssetBrowser.cpp`, then require zero remaining references using `specs/059-fix-subasset-expansion/quickstart.md`
- [ ] T039 Run `AssetBrowserPresentationTests.*:EditorAssetDatabaseTests.*:AssetDatabaseFacadeTests.*` from `Build/bin/Debug/NullusUnitTests.exe`
- [ ] T040 Run `ctest --test-dir Build -C Debug --output-on-failure`
- [ ] T041 Build the Windows `Editor` target using the command in `specs/059-fix-subasset-expansion/quickstart.md`
- [ ] T042 Perform and record manual narrow/wide grid, list, filter, refresh, replacement-identity, and clipping checks in `specs/059-fix-subasset-expansion/quickstart.md`
- [ ] T043 Reconcile completed tasks and validation evidence across `specs/059-fix-subasset-expansion/spec.md`, `plan.md`, `tasks.md`, and `quickstart.md`
- [ ] T044 Run mandatory multi-round `/plan-review`, fix all P0/P1/P2 findings required by repository policy, and record final score/evidence in `specs/059-fix-subasset-expansion/quickstart.md`

---

## Dependencies and Execution Order

### Phase Dependencies

- **Phase 1**: Starts immediately and records the baseline.
- **Phase 2**: Depends on Phase 1; blocks every user story.
- **User Story 1**: Depends on Phase 2 and provides the MVP exact-membership builder.
- **User Story 2**: Depends on User Story 1 bundle types and completes stale-state safety.
- **User Story 3**: Depends on User Story 1 group IDs; geometry tests can begin once those types exist.
- **Phase 6**: Depends on all selected user stories.

### Within Each Story

- RED may initially be an expected compile failure only when a new API/type does not yet exist; add the minimum declaration-only scaffold immediately, then require the behavioral test to compile and fail before implementing behavior. Existing-API changes must use a compiling assertion failure as RED.
- Pure data/projection logic precedes panel integration.
- Coordinator and cache failures are tested with fakes/hooks before real panel plumbing.
- Focused tests pass before moving to the next story.

### Parallel Opportunities

- T006 can run in parallel with T004/T005 because it is in `EditorAssetDatabaseTests.cpp`.
- After foundational types exist, facade publication tests and presentation projection test design can be prepared independently, but implementation remains ordered.
- T021 and T022 both modify `Tests/Unit/AssetBrowserPresentationTests.cpp` and therefore execute sequentially.
- Manual scenario preparation can proceed while broad automated tests run, but manual results are recorded only against the final build.

## Parallel Example: Foundational Tests

```text
Task A: Add canonical path/equality RED tests in Tests/Unit/AssetBrowserPresentationTests.cpp
Task B: Add facade publication/error RED tests in Tests/Unit/EditorAssetDatabaseTests.cpp
```

## Implementation Strategy

### MVP First

1. Complete immutable facade publication.
2. Complete User Story 1 pure exact-membership bundle.
3. Validate count/membership/filter behavior before panel concurrency migration.

### Incremental Delivery

1. Foundation establishes one current asset state.
2. US1 establishes exact membership and structured identity.
3. US2 makes background publication and consumers fail closed.
4. US3 changes only geometry/drawing after group data is stable.
5. Cleanup removes old paths only after all replacements are green.

## Notes

- Do not hand-edit `Runtime/*/Gen/`.
- Preserve unrelated dirty-worktree changes and inspect overlapping diffs before each edit.
- Use `apply_patch` for manual file changes.
- Do not claim Linux/macOS correctness without corresponding evidence.
