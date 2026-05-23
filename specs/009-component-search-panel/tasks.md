# Tasks: Reflection-Driven Component Search Dropdown

**Input**: Design documents from `/specs/009-component-search-panel/`
**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [data-model.md](./data-model.md), [contracts/component-picker-ui-contract.md](./contracts/component-picker-ui-contract.md), [quickstart.md](./quickstart.md)

**Tests**: Include targeted `NullusUnitTests` coverage for deterministic picker logic, MetaParser/type-metadata coverage, plus manual editor verification from `quickstart.md`.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g. `US1`, `US2`, `US3`)
- Include exact file paths in descriptions

## Path Conventions

- Editor implementation: `Project/Editor/Panels/`
- Engine/runtime integration: `Runtime/Engine/`, `Runtime/Base/Reflection/`
- Unit tests: `Tests/Unit/`

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the new editor picker files and shared test entrypoint that the feature will build on.

- [X] T001 Create the searchable component picker implementation skeleton in `Project/Editor/Panels/ComponentSearchPanel.h` and `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T002 Create the component picker unit test file skeleton in `Tests/Unit/EditorComponentPickerTests.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish shared discovery, naming, actor-context, metadata, and picker lifecycle plumbing before story-specific behavior.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 Implement shared component entry, metadata-derived menu-path, category tree, and selected-actor session state in `Project/Editor/Panels/ComponentSearchPanel.h` and `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T004 Implement reflection-driven component discovery, display-name normalization, metadata lookup, and deterministic sorting in `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T005 Implement a shared addability evaluation path that filters invalid base types, transform, and actor-incompatible component types in `Project/Editor/Panels/ComponentSearchPanel.cpp` and, if needed, `Runtime/Engine/GameObject.h` and `Runtime/Engine/GameObject.cpp`
- [X] T006 Add type-level reflection metadata support for `ComponentMenu(...)` in `Tools/MetaParser/src/` and `Runtime/Base/Reflection/RuntimeMetaProperties.h`
- [X] T007 Refactor `Project/Editor/Panels/Inspector.h` and `Project/Editor/Panels/Inspector.cpp` to own/open the new picker popup instead of using the old combo-box state or a standalone editor window

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Add Any Eligible Component From One Panel (Priority: P1) 🎯 MVP

**Goal**: Replace the hardcoded Inspector component dropdown with a reflection-driven add-component popup that can add eligible components and refresh the Inspector immediately.

**Independent Test**: Select an actor, open `Add Component`, confirm reflected component entries appear under metadata-driven categories, choose a component that was not previously hardcoded, and verify it is added and shown in the Inspector immediately.

### Tests for User Story 1

- [X] T008 [P] [US1] Add unit coverage for reflected component discovery, metadata menu-path resolution, and entry ordering in `Tests/Unit/EditorComponentPickerTests.cpp`
- [X] T009 [P] [US1] Add unit coverage for MetaParser-generated `ComponentMenu` metadata bindings in `Tests/Unit/MetaParserGenerationModuleTests.cpp` and `Tests/Unit/ReflectionRuntimeCoreTests.cpp`

### Implementation for User Story 1

- [X] T010 [US1] Implement the popup open/close flow, metadata category tree construction, and initial browse mode in `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T011 [US1] Implement component selection confirmation through the existing dynamic add path in `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T012 [US1] Update Inspector integration so successful picker adds refresh the Inspector and return to normal editing flow in `Project/Editor/Panels/Inspector.cpp`

**Checkpoint**: User Story 1 should now provide a fully working reflection-driven add-component picker for eligible components.

---

## Phase 4: User Story 2 - Find Components Quickly Through Search (Priority: P2)

**Goal**: Add fast in-popup search so users can narrow the reflection-driven component list by name.

**Independent Test**: Open the picker, type partial component names, verify the UI switches to flat case-insensitive search results, clear the query to restore metadata-driven category browsing, and verify a clear no-results state for unmatched input.

### Tests for User Story 2

- [X] T013 [P] [US2] Add unit coverage for case-insensitive search matching and query normalization in `Tests/Unit/EditorComponentPickerTests.cpp`
- [X] T014 [P] [US2] Add unit coverage for empty-query category mode, flat search mode, and no-results behavior in `Tests/Unit/EditorComponentPickerTests.cpp`

### Implementation for User Story 2

- [X] T015 [US2] Implement search query state, normalization, and filtered entry/view-model generation in `Project/Editor/Panels/ComponentSearchPanel.h` and `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T016 [US2] Implement the popup search input, metadata category browsing, and flat result rendering in `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T017 [US2] Implement an explicit empty-state row/message for unmatched searches in `Project/Editor/Panels/ComponentSearchPanel.cpp`

**Checkpoint**: User Stories 1 and 2 should now work independently, with search narrowing the reflection-driven picker list in one interaction.

---

## Phase 5: User Story 3 - Prevent Invalid Add Actions (Priority: P3)

**Goal**: Keep the picker trustworthy by excluding or safely blocking components that cannot be added in the current actor context.

**Independent Test**: Open the picker on actors that already contain singleton-style components, confirm duplicate or foundational types are unavailable, and verify invalid actor context cannot apply a component.

### Tests for User Story 3

- [X] T018 [P] [US3] Add unit coverage for duplicate exclusion and singleton-style component blocking in `Tests/Unit/EditorComponentPickerTests.cpp`
- [X] T019 [P] [US3] Add unit coverage for abstract/foundation type exclusion and invalid-actor failure handling in `Tests/Unit/EditorComponentPickerTests.cpp`

### Implementation for User Story 3

- [X] T020 [US3] Finalize addability filtering so invalid, foundational, and already-satisfied component types are excluded or disabled consistently in `Project/Editor/Panels/ComponentSearchPanel.cpp`
- [X] T021 [US3] Implement graceful failure handling for stale or invalid actor context during add confirmation in `Project/Editor/Panels/ComponentSearchPanel.cpp` and `Project/Editor/Panels/Inspector.cpp`
- [X] T022 [US3] Ensure picker state refreshes correctly when the selected actor or attached component set changes in `Project/Editor/Panels/Inspector.cpp` and `Project/Editor/Panels/ComponentSearchPanel.cpp`

**Checkpoint**: All user stories should now be independently functional and the picker should avoid invalid add actions.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Finish validation and clean up integration details that affect the whole feature.

- [X] T023 [P] Run focused unit validation for the picker in `Tests/Unit/EditorComponentPickerTests.cpp`
- [X] T024 Run full `NullusUnitTests` validation for the feature's impacted editor/runtime logic from `Tests/Unit/`
- [ ] T025 Run the manual editor verification flow from `specs/009-component-search-panel/quickstart.md` and record the exact evidence in the final implementation summary

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - blocks all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational completion
- **User Story 2 (Phase 4)**: Depends on User Story 1's picker UI shell being available
- **User Story 3 (Phase 5)**: Depends on Foundational completion and benefits from US1 picker integration being complete
- **Polish (Phase 6)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Starts first after foundation and defines the MVP add-component flow
- **User Story 2 (P2)**: Builds on the picker introduced by US1 but remains independently testable once search is added
- **User Story 3 (P3)**: Tightens filtering and failure behavior on top of the picker foundation and remains independently testable through actor-state scenarios

### Within Each User Story

- Unit tests should be written before or alongside the implementation they verify
- Shared picker state and discovery logic must exist before UI rendering tasks
- Add execution must be in place before Inspector refresh verification
- Search state must exist before search rendering and empty-state tasks
- Invalid add handling must be in place before actor-context refresh tasks are considered complete

### Parallel Opportunities

- `T001` and `T002` can run in parallel
- `T008` and `T009` can run in parallel
- `T013` and `T014` can run in parallel
- `T018` and `T019` can run in parallel
- `T023` can run in parallel with final manual verification prep before `T025`

---

## Parallel Example: User Story 1

```bash
# Launch the US1 unit coverage tasks together:
Task: "Add unit coverage for reflected component discovery and entry ordering in Tests/Unit/EditorComponentPickerTests.cpp"
Task: "Add unit coverage for dynamic add execution against reflected component types in Tests/Unit/EditorComponentPickerTests.cpp"
```

## Parallel Example: User Story 2

```bash
# Launch the US2 search coverage tasks together:
Task: "Add unit coverage for case-insensitive search matching and query normalization in Tests/Unit/EditorComponentPickerTests.cpp"
Task: "Add unit coverage for empty-query reset and no-results behavior in Tests/Unit/EditorComponentPickerTests.cpp"
```

## Parallel Example: User Story 3

```bash
# Launch the US3 guard-rail coverage tasks together:
Task: "Add unit coverage for duplicate exclusion and singleton-style component blocking in Tests/Unit/EditorComponentPickerTests.cpp"
Task: "Add unit coverage for abstract/foundation type exclusion and invalid-actor failure handling in Tests/Unit/EditorComponentPickerTests.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Run the US1 unit coverage and manual add-component flow
5. Demo the reflection-driven picker before moving to search and addability polish

### Incremental Delivery

1. Finish Setup + Foundational to establish the picker architecture
2. Deliver User Story 1 as the MVP reflection-driven add flow
3. Layer in User Story 2 search behavior
4. Layer in User Story 3 trust/safety constraints
5. Finish with full validation and manual evidence capture

### Suggested MVP Scope

- **MVP**: Phase 1 + Phase 2 + Phase 3 (User Story 1 only)
- This delivers the highest-value outcome: the Inspector no longer depends on a hardcoded component list.

---

## Notes

- [P] tasks target separate concerns and can be parallelized safely
- User story labels map each task back to the feature spec for traceability
- No generated files under `Runtime/*/Gen/` are part of this task list
- Editor and unit-test source files are already globbed by the existing CMake configuration, so no explicit build-file update task is required unless implementation reveals a missing dependency
