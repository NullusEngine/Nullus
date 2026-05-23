# Tasks: Reflected Editor Settings Panel

**Input**: Design documents from `/specs/017-reflected-settings-panel/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/
**Tests**: Unit tests are included because this feature changes editor behavior, reflection drawing reuse, registration, search, and persistence.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare source/test files and build wiring for the reflected Settings system.

- [x] T001 Add editor settings source/header entries for new files in `Project/Editor/CMakeLists.txt`
- [x] T002 Add unit test source coverage entries if needed in `Tests/Unit/CMakeLists.txt`
- [x] T003 [P] Create shared reflected drawer interface stubs in `Project/Editor/Panels/ReflectedPropertyDrawer.h` and `Project/Editor/Panels/ReflectedPropertyDrawer.cpp`
- [x] T004 [P] Create settings model/registry/persistence stubs in `Project/Editor/Settings/EditorSettingObject.h`, `Project/Editor/Settings/EditorSettingsRegistry.h`, `Project/Editor/Settings/EditorSettingsRegistry.cpp`, `Project/Editor/Settings/EditorSettingsPersistence.h`, and `Project/Editor/Settings/EditorSettingsPersistence.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish the shared reflected field path and SettingObject infrastructure used by all stories.

- [x] T005 [P] Add failing registry unit tests for unique registration, duplicate rejection, ordering, and lookup in `Tests/Unit/EditorSettingsRegistryTests.cpp`
- [x] T006 [P] Add failing reflected drawer unit tests for label formatting, supported type classification, and unsupported fallback decisions in `Tests/Unit/ReflectedPropertyDrawerTests.cpp`
- [x] T007 Implement `EditorSettingObject` descriptors and `EditorSettingsRegistry` behavior in `Project/Editor/Settings/EditorSettingObject.h`, `Project/Editor/Settings/EditorSettingsRegistry.h`, and `Project/Editor/Settings/EditorSettingsRegistry.cpp`
- [x] T008 Move Inspector reflection helpers into shared drawer internals in `Project/Editor/Panels/ReflectedPropertyDrawer.cpp`
- [x] T009 Update `Project/Editor/Panels/Inspector.cpp` to call `ReflectedPropertyDrawer` for component fields while preserving existing Inspector layout and behavior
- [x] T010 Run registry/drawer focused unit tests from `Build/bin/Debug/NullusUnitTests.exe`

---

## Phase 3: User Story 1 - Edit Reflected Editor Settings (Priority: P1)

**Goal**: User opens a Unity-style Settings modal from Edit menu, selects a settings page, edits reflected properties, and Scene view mouse input is blocked while the modal is open.

**Independent Test**: Open Settings from Edit menu, edit a reflected field on a registered editor settings page, close/reopen, and verify modal layout and Scene input blocking behavior.

### Tests for User Story 1

- [x] T011 [P] [US1] Add Settings panel modal/open/selection behavior tests in `Tests/Unit/ProjectSettingsPanelTests.cpp`
- [x] T012 [P] [US1] Add editor interaction blocking test coverage for Settings modal in `Tests/Unit/ProjectSettingsPanelTests.cpp`

### Implementation for User Story 1

- [x] T013 [US1] Replace hand-written `ProjectSettings` content with Unity-style modal state, search bar area, left navigation, and right detail container in `Project/Editor/Panels/ProjectSettings.h` and `Project/Editor/Panels/ProjectSettings.cpp`
- [x] T014 [US1] Register an initial reflected editor settings object backed by existing editor settings values in `Project/Editor/Settings/EditorSettings.h` and `Project/Editor/Settings/EditorSettings.cpp`
- [x] T015 [US1] Wire `Edit > Settings` menu command to open the modal Settings window in `Project/Editor/Panels/MenuBar.cpp` and `Project/Editor/Core/Editor.cpp`
- [x] T016 [US1] Block Scene view mouse interaction while Settings modal is open using existing interaction blocker flow in `Project/Editor/Core/Editor.h`, `Project/Editor/Core/Editor.cpp`, and `Project/Editor/Panels/SceneView.cpp`
- [x] T017 [US1] Render selected SettingObject reflected properties through `ReflectedPropertyDrawer` in `Project/Editor/Panels/ProjectSettings.cpp`
- [x] T018 [US1] Run `ProjectSettingsPanelTests` and related editor unit tests from `Build/bin/Debug/NullusUnitTests.exe`

---

## Phase 4: User Story 2 - Share Property Drawing With Inspector (Priority: P2)

**Goal**: Inspector and Settings use the same reflected-property drawing behavior for supported field types.

**Independent Test**: Change shared drawer behavior and verify both Inspector reflected component rendering and Settings reflected object rendering use it.

### Tests for User Story 2

- [x] T019 [P] [US2] Extend reflected drawer tests for bool, int, float, string, vector, quaternion, enum, and unsupported type classification in `Tests/Unit/ReflectedPropertyDrawerTests.cpp`
- [x] T020 [P] [US2] Add regression coverage that Inspector reflection utilities use the shared label/enum formatting behavior in `Tests/Unit/ReflectedPropertyDrawerTests.cpp`

### Implementation for User Story 2

- [x] T021 [US2] Expose shared drawer options and field result/status APIs in `Project/Editor/Panels/ReflectedPropertyDrawer.h`
- [x] T022 [US2] Complete shared drawer support for existing Inspector field types in `Project/Editor/Panels/ReflectedPropertyDrawer.cpp`
- [x] T023 [US2] Remove duplicated reflected field drawing code from `Project/Editor/Panels/Inspector.cpp`
- [x] T024 [US2] Run reflected drawer and Inspector-related unit tests from `Build/bin/Debug/NullusUnitTests.exe`

---

## Phase 5: User Story 3 - Discover Settings Quickly (Priority: P3)

**Goal**: User can search categories, setting names, and reflected property labels with partial case-insensitive text.

**Independent Test**: Register multiple settings entries, search by category/name/property, and verify the Settings window keeps a valid selection and shows empty results safely.

### Tests for User Story 3

- [x] T025 [P] [US3] Add registry search/filter tests for category, display name, property label, no-match, and selection fallback in `Tests/Unit/EditorSettingsRegistryTests.cpp`
- [x] T026 [P] [US3] Add panel search state tests in `Tests/Unit/ProjectSettingsPanelTests.cpp`

### Implementation for User Story 3

- [x] T027 [US3] Implement normalized search matching over registered entries and reflected field labels in `Project/Editor/Settings/EditorSettingsRegistry.cpp`
- [x] T028 [US3] Integrate search filtering, empty state, and selection preservation in `Project/Editor/Panels/ProjectSettings.cpp`
- [x] T029 [US3] Run settings search tests from `Build/bin/Debug/NullusUnitTests.exe`

---

## Phase 6: User Story 4 - Extend Settings Without Rewriting the Window (Priority: P4)

**Goal**: A developer registers a new SettingObject and gets category placement, reflected drawing, duplicate detection, and persistence automatically.

**Independent Test**: Add a test SettingObject, save supported values, reload into a fresh object, and verify duplicate ids are rejected.

### Tests for User Story 4

- [x] T030 [P] [US4] Add persistence tests for missing file defaults, round-trip supported values, unknown fields, invalid values, and enum values in `Tests/Unit/EditorSettingsPersistenceTests.cpp`
- [x] T031 [P] [US4] Add extensibility registration tests for a second SettingObject category in `Tests/Unit/EditorSettingsRegistryTests.cpp`

### Implementation for User Story 4

- [x] T032 [US4] Implement JSON persistence load/save for supported reflected field types in `Project/Editor/Settings/EditorSettingsPersistence.cpp`
- [x] T033 [US4] Connect Settings panel dirty tracking and save-on-close/apply behavior in `Project/Editor/Panels/ProjectSettings.cpp`
- [x] T034 [US4] Add at least one additional real or representative SettingObject registration to validate category extension in `Project/Editor/Settings/EditorSettings.cpp`
- [x] T035 [US4] Run persistence and registry tests from `Build/bin/Debug/NullusUnitTests.exe`

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final verification, cleanup, and documentation sync.

- [x] T036 [P] Update `specs/017-reflected-settings-panel/quickstart.md` if actual commands or manual verification steps changed
- [x] T037 Review UI styling in `Project/Editor/Panels/ProjectSettings.cpp` for Unity alignment and remove obsolete hand-written Settings rows
- [x] T038 Run `cmake --build Build --target NullusUnitTests Editor --config Debug`
- [x] T039 Run `Build/bin/Debug/NullusUnitTests.exe`
- [x] T040 Check `git status --short` and self-review diffs for generated-file edits, unrelated churn, and task/spec consistency

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories.
- **US1 (Phase 3)**: Depends on Foundational and delivers the MVP modal settings workflow.
- **US2 (Phase 4)**: Depends on Foundational and can proceed after shared drawer extraction; final Inspector cleanup should happen before polish.
- **US3 (Phase 5)**: Depends on registry and panel state from US1.
- **US4 (Phase 6)**: Depends on registry and reflected drawer from Foundational plus panel dirty tracking from US1.
- **Polish (Phase 7)**: Depends on selected user stories.

### Parallel Opportunities

- T003 and T004 can be created independently.
- T005 and T006 can be written independently.
- US1 tests T011 and T012 can be written in parallel.
- US2 tests T019 and T020 can be written in parallel.
- US3 tests T025 and T026 can be written in parallel.
- US4 tests T030 and T031 can be written in parallel.

## Implementation Strategy

### MVP First

1. Complete Phase 1 and Phase 2.
2. Complete US1 so Settings opens, edits a reflected page, and blocks Scene input.
3. Validate with focused unit tests before expanding search and persistence.

### Incremental Delivery

1. Extract shared reflected drawer without changing Inspector behavior.
2. Add Settings registry and modal UI with one real page.
3. Add search.
4. Add persistence and extension tests.
5. Build `Editor` and run `NullusUnitTests`.
