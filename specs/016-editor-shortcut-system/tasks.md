# Tasks: Editor Shortcut System

**Input**: Design documents from `/specs/016-editor-shortcut-system/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/shortcut-service.md, quickstart.md
**Tests**: Required by FR-023 and SC-005 for registration, context resolution, conflict detection, persistence, and migrated shortcuts.
**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches different files and has no dependency on another incomplete task.
- **[Story]**: Which user story this task belongs to.
- Every task includes exact file paths.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the editor-only shortcut module and make it visible to the editor and unit test targets.

- [X] T001 Create `Project/Editor/Shortcuts/` directory and empty source/header files listed in `specs/016-editor-shortcut-system/plan.md`
- [X] T002 Add shortcut source files to the Editor target through existing glob behavior and verify no CMake change is needed in `Project/Editor/CMakeLists.txt`
- [X] T003 Add planned shortcut source files to `Tests/Unit/CMakeLists.txt` so `NullusUnitTests` can compile editor shortcut implementation code
- [X] T004 [P] Create placeholder unit test files `Tests/Unit/EditorShortcutBindingTests.cpp`, `Tests/Unit/EditorShortcutConflictTests.cpp`, `Tests/Unit/EditorShortcutPersistenceTests.cpp`, and `Tests/Unit/EditorShortcutResolutionTests.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define shared models and utility behavior required by every user story.

**CRITICAL**: No user story work can begin until this phase is complete.

- [X] T005 Implement shortcut key/modifier data types and display normalization in `Project/Editor/Shortcuts/EditorShortcutBinding.h`
- [X] T006 [P] Implement shortcut command metadata types in `Project/Editor/Shortcuts/EditorShortcutCommand.h`
- [X] T007 [P] Implement shortcut context metadata types and built-in context IDs in `Project/Editor/Shortcuts/EditorShortcutContext.h`
- [X] T008 Implement binding equality, ordering, validation, and modifier-only rejection in `Project/Editor/Shortcuts/EditorShortcutBinding.h`
- [X] T009 Add binding display and validation tests in `Tests/Unit/EditorShortcutBindingTests.cpp`
- [X] T010 Update `Tests/Unit/CMakeLists.txt` if compilation shows additional editor shortcut sources must be explicitly listed

**Checkpoint**: Shortcut entities compile and binding tests can fail/pass independently before command behavior exists.

---

## Phase 3: User Story 1 - Execute Editor Commands Through Central Shortcuts (Priority: P1) MVP

**Goal**: Existing editor shortcuts execute through a central editor-only service instead of panel-local hard-coded checks.

**Independent Test**: Register the initial editor commands, simulate matching key presses and contexts, and verify the same command action runs once.

### Tests for User Story 1

- [X] T011 [P] [US1] Add command registration uniqueness tests in `Tests/Unit/EditorShortcutResolutionTests.cpp`
- [X] T012 [P] [US1] Add context resolution tests for global, Scene View, Hierarchy, and text-input suppression in `Tests/Unit/EditorShortcutResolutionTests.cpp`
- [X] T013 [P] [US1] Add migrated shortcut command mapping tests for new scene, save scene, save as, play, RenderDoc capture/open, and delete selected actor in `Tests/Unit/EditorShortcutResolutionTests.cpp`

### Implementation for User Story 1

- [X] T014 [US1] Implement command registration and query behavior in `Project/Editor/Shortcuts/EditorShortcutRegistry.h` and `Project/Editor/Shortcuts/EditorShortcutRegistry.cpp`
- [X] T015 [US1] Implement active-context resolution and execute-at-most-one behavior in `Project/Editor/Shortcuts/EditorShortcutResolver.h` and `Project/Editor/Shortcuts/EditorShortcutResolver.cpp`
- [X] T016 [US1] Implement the service facade for registering commands, resolving shortcuts, and exposing binding display text in `Project/Editor/Shortcuts/EditorShortcutService.h` and `Project/Editor/Shortcuts/EditorShortcutService.cpp`
- [X] T017 [US1] Register default editor commands against `EditorActions` in `Project/Editor/Core/Editor.cpp` and `Project/Editor/Core/EditorActions.h`
- [X] T018 [US1] Replace hard-coded global shortcut handling in `Project/Editor/Core/Editor.cpp` with `EditorShortcutService` dispatch
- [X] T019 [US1] Replace hard-coded menu shortcut handling in `Project/Editor/Panels/MenuBar.cpp` and `Project/Editor/Panels/EditorTopBar.cpp` with `EditorShortcutService` dispatch/display
- [X] T020 [US1] Add or adjust input state helpers needed by shortcut matching in `Runtime/Platform/Windowing/Inputs/InputManager.h` and `Runtime/Platform/Windowing/Inputs/InputManager.cpp`
- [X] T021 [US1] Verify `Tests/Unit/EditorShortcutResolutionTests.cpp` and `Tests/Unit/EditorShortcutBindingTests.cpp` pass for US1 behavior

**Checkpoint**: MVP complete. Migrated shortcuts work through the central service without persistence or settings UI.

---

## Phase 4: User Story 2 - Avoid Shortcut Conflicts (Priority: P2)

**Goal**: Proposed shortcut bindings are rejected or accepted according to global, same-context, global-context, and mutually exclusive context rules.

**Independent Test**: Assign duplicate bindings across command/context combinations and verify blocking conflicts appear only where required.

### Tests for User Story 2

- [X] T022 [P] [US2] Add duplicate global and same-context conflict tests in `Tests/Unit/EditorShortcutConflictTests.cpp`
- [X] T023 [P] [US2] Add mutually exclusive context reuse tests in `Tests/Unit/EditorShortcutConflictTests.cpp`
- [X] T024 [P] [US2] Add global-context collision and explicit override tests in `Tests/Unit/EditorShortcutConflictTests.cpp`
- [X] T025 [P] [US2] Add invalid binding conflict tests for modifier-only and unsupported bindings in `Tests/Unit/EditorShortcutConflictTests.cpp`

### Implementation for User Story 2

- [X] T026 [US2] Add shortcut conflict data types and messages in `Project/Editor/Shortcuts/EditorShortcutCommand.h`
- [X] T027 [US2] Implement conflict validation in `Project/Editor/Shortcuts/EditorShortcutRegistry.cpp`
- [X] T028 [US2] Expose proposed-binding validation through `Project/Editor/Shortcuts/EditorShortcutService.h` and `Project/Editor/Shortcuts/EditorShortcutService.cpp`
- [X] T029 [US2] Ensure resolver ignores disabled commands and ambiguous conflicts in `Project/Editor/Shortcuts/EditorShortcutResolver.cpp`
- [X] T030 [US2] Verify `Tests/Unit/EditorShortcutConflictTests.cpp` passes for US2 behavior

**Checkpoint**: Shortcut assignments can be validated safely before persistence or UI editing.

---

## Phase 5: User Story 3 - Persist User Shortcut Profiles (Priority: P3)

**Goal**: User binding overrides save to disk, reload on editor startup, and never overwrite built-in defaults.

**Independent Test**: Save overrides and unassigned commands to a temporary profile, reload them, and verify missing, malformed, and stale data fall back safely.

### Tests for User Story 3

- [X] T031 [P] [US3] Add profile save/load override tests in `Tests/Unit/EditorShortcutPersistenceTests.cpp`
- [X] T032 [P] [US3] Add unassigned command persistence tests in `Tests/Unit/EditorShortcutPersistenceTests.cpp`
- [X] T033 [P] [US3] Add missing, malformed, invalid binding, and stale command ID fallback tests in `Tests/Unit/EditorShortcutPersistenceTests.cpp`

### Implementation for User Story 3

- [X] T034 [US3] Implement shortcut profile model and JSON serialization in `Project/Editor/Shortcuts/EditorShortcutProfile.h` and `Project/Editor/Shortcuts/EditorShortcutProfile.cpp`
- [X] T035 [US3] Integrate profile load/save and default fallback into `Project/Editor/Shortcuts/EditorShortcutService.cpp`
- [X] T036 [US3] Wire profile path resolution to project `UserSettings` in `Project/Editor/Core/Context.h` and `Project/Editor/Core/Context.cpp`
- [X] T037 [US3] Ensure binding changes, clears, and resets update the active profile in `Project/Editor/Shortcuts/EditorShortcutService.cpp`
- [X] T038 [US3] Verify `Tests/Unit/EditorShortcutPersistenceTests.cpp` passes for US3 behavior

**Checkpoint**: Customized shortcuts persist across service reload without settings UI.

---

## Phase 6: User Story 4 - Browse and Edit Shortcuts by Category (Priority: P4)

**Goal**: Users can browse, search, edit, clear, and reset editor shortcuts through a shortcut settings panel with conflict feedback.

**Independent Test**: Open the panel, find a command, change a valid binding, block a conflicting binding, and confirm menu/list display updates.

### Tests for User Story 4

- [X] T039 [P] [US4] Add command list filtering and display-model tests in `Tests/Unit/EditorShortcutResolutionTests.cpp`
- [X] T040 [P] [US4] Add assign, clear, and reset workflow tests through the service facade in `Tests/Unit/EditorShortcutPersistenceTests.cpp`

### Implementation for User Story 4

- [X] T041 [US4] Implement shortcut settings panel declaration in `Project/Editor/Panels/ShortcutSettingsPanel.h`
- [X] T042 [US4] Implement category list, command table, search, edit, clear, reset, and conflict display in `Project/Editor/Panels/ShortcutSettingsPanel.cpp`
- [X] T043 [US4] Register the shortcut settings panel and Window menu entry in `Project/Editor/Core/Editor.cpp` and `Project/Editor/Panels/EditorTopBar.cpp`
- [X] T044 [US4] Update menu item shortcut display to query current bindings in `Project/Editor/Panels/MenuBar.cpp`
- [X] T045 [US4] Add optional visual keyboard adapter point without making it a core dependency in `Project/Editor/Panels/ShortcutSettingsPanel.cpp`
- [ ] T046 [US4] Verify settings panel behavior manually using `specs/016-editor-shortcut-system/quickstart.md`

**Checkpoint**: Users can manage editor shortcuts from UI with conflict-safe persistence.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, cleanup, and documentation updates across completed stories.

- [X] T047 [P] Update shortcut system notes in `Docs/AIWorkflow.md` or another appropriate editor workflow document if repository guidance needs a user-facing note
- [X] T048 Review all shortcut source files under `Project/Editor/Shortcuts/` for naming consistency, generated-file boundary compliance, and editor-only includes
- [X] T049 Run focused automated validation with `ctest --test-dir build --output-on-failure -R NullusUnitTests`
- [ ] T050 Run focused manual editor validation from `specs/016-editor-shortcut-system/quickstart.md`
- [X] T051 Confirm Runtime/Game input behavior remains unchanged by reviewing edits in `Runtime/Platform/Windowing/Inputs/InputManager.h` and `Runtime/Platform/Windowing/Inputs/InputManager.cpp`
- [X] T052 Update `specs/016-editor-shortcut-system/quickstart.md` with any corrected local build command discovered during validation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories.
- **US1 MVP (Phase 3)**: Depends on Foundational.
- **US2 Conflicts (Phase 4)**: Depends on US1 registry/service APIs.
- **US3 Persistence (Phase 5)**: Depends on US1 service APIs and US2 validation rules.
- **US4 Settings UI (Phase 6)**: Depends on US1, US2, and US3 so UI edits use the real service path.
- **Polish (Phase 7)**: Depends on all implemented stories.

### User Story Dependencies

- **US1 (P1)**: First independently useful increment; no dependency on other stories after Foundation.
- **US2 (P2)**: Depends on US1 command registry and binding model.
- **US3 (P3)**: Depends on US1 command IDs/defaults and US2 validation before saving.
- **US4 (P4)**: Depends on the service, conflict validation, and persistence.

### Within Each User Story

- Tests should be written before implementation and fail before the implementation task is completed.
- Models and service contracts precede integration into `Editor.cpp`, `MenuBar.cpp`, and panels.
- Each checkpoint should be validated before moving to the next story.

---

## Parallel Opportunities

- T004 can run in parallel with T001-T003 after the file list is known.
- T006 and T007 can run in parallel after T005 defines shared binding vocabulary.
- US1 tests T011-T013 can run in parallel.
- US2 tests T022-T025 can run in parallel.
- US3 tests T031-T033 can run in parallel.
- US4 tests T039-T040 can run in parallel.
- Polish documentation T047 can run in parallel with final code review T048.

## Parallel Example: User Story 2

```text
Task: "T022 [US2] Add duplicate global and same-context conflict tests in Tests/Unit/EditorShortcutConflictTests.cpp"
Task: "T023 [US2] Add mutually exclusive context reuse tests in Tests/Unit/EditorShortcutConflictTests.cpp"
Task: "T024 [US2] Add global-context collision and explicit override tests in Tests/Unit/EditorShortcutConflictTests.cpp"
Task: "T025 [US2] Add invalid binding conflict tests for modifier-only and unsupported bindings in Tests/Unit/EditorShortcutConflictTests.cpp"
```

## Parallel Example: User Story 3

```text
Task: "T031 [US3] Add profile save/load override tests in Tests/Unit/EditorShortcutPersistenceTests.cpp"
Task: "T032 [US3] Add unassigned command persistence tests in Tests/Unit/EditorShortcutPersistenceTests.cpp"
Task: "T033 [US3] Add missing, malformed, invalid binding, and stale command ID fallback tests in Tests/Unit/EditorShortcutPersistenceTests.cpp"
```

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational shortcut entities.
3. Complete Phase 3 US1 central command execution.
4. Stop and validate US1 with focused unit tests and migrated shortcut manual checks.

### Incremental Delivery

1. US1 centralizes current shortcuts without changing user customization.
2. US2 adds conflict safety.
3. US3 adds disk-backed user profile overrides.
4. US4 adds the settings UI and optional visual keyboard adapter point.

### Risk Controls

- Keep all command semantics editor-only.
- Do not hand-edit generated files.
- Avoid making `imgui_keyboard` a required dependency for core shortcut logic.
- Record exact validation evidence before claiming completion.
