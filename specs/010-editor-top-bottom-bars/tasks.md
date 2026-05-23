# Tasks: Editor Top And Bottom Bars

**Input**: Design documents from `/specs/010-editor-top-bottom-bars/`  
**Prerequisites**: `plan.md` (required), `spec.md` (required for user stories), `research.md`, `data-model.md`, `contracts/`

**Tests**: No new automated tests were explicitly requested in the feature spec. Validation for this feature is driven by Editor build checks plus the manual verification flow in `specs/010-editor-top-bottom-bars/quickstart.md`.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g. `US1`, `US2`, `US3`)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Introduce the reusable UI scaffolding that will host fixed editor-global bars.

- [X] T001 Create `Runtime/UI/Panels/PanelViewportBar.h` declaring a reusable top/bottom viewport-anchored bar base for editor-global UI
- [X] T002 Implement `Runtime/UI/Panels/PanelViewportBar.cpp` with main-viewport anchoring, fixed bar sizing, and non-dockable/non-resizable behavior
- [X] T003 [P] Create `Project/Editor/Panels/EditorTopBar.h` declaring the fixed top bar panel on top of `Runtime/UI/Panels/PanelViewportBar.h`
- [X] T004 [P] Create `Project/Editor/Panels/EditorStatusBar.h` declaring the fixed bottom status bar panel on top of `Runtime/UI/Panels/PanelViewportBar.h`

**Checkpoint**: The repository has concrete scaffolding for fixed viewport bars and editor-specific top/status bar panels.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Make the fixed bars coexist with the Editor runtime and expose the shared state they need.

**⚠️ CRITICAL**: No user story work should begin until this phase is complete.

- [X] T005 Update `Runtime/UI/Modules/Canvas.cpp` so the dockspace root coexists cleanly with `Runtime/UI/Panels/PanelViewportBar.cpp` during normal layout, reset, and resize flows
- [X] T006 [P] Update `Project/Editor/Panels/SceneView.h` and `Project/Editor/Panels/SceneView.cpp` to expose the current gizmo operation and controlled mutation hooks for external top-bar UI
- [X] T007 [P] Update `Project/Editor/Core/Editor.h` and `Project/Editor/Core/Editor.cpp` to cache editor frame timing/FPS data for global status-bar consumption
- [X] T008 Update `Project/Editor/Core/Editor.cpp` to register `Project/Editor/Panels/EditorTopBar.h` and `Project/Editor/Panels/EditorStatusBar.h` in the startup panel set while keeping default layout recovery stable

**Checkpoint**: The Editor can host fixed global bars, and shared scene-tool plus frame-timing state is externally consumable.

---

## Phase 3: User Story 1 - Use A Unified Editor Top Bar (Priority: P1) 🎯 MVP

**Goal**: Replace the fragmented startup experience with one fixed top bar that exposes menus and play controls in a Unity-like location.

**Independent Test**: Launch the Editor with a valid project and confirm the top bar stays fixed at the top edge while exposing main menus and play controls without opening additional windows.

### Implementation for User Story 1

- [X] T009 [P] [US1] Refactor `Project/Editor/Panels/MenuBar.h` and `Project/Editor/Panels/MenuBar.cpp` so the existing menu content can be embedded inside the fixed top bar instead of requiring a standalone `PanelMenuBar`
- [X] T010 [P] [US1] Refactor `Project/Editor/Panels/Toolbar.h` and `Project/Editor/Panels/Toolbar.cpp` so existing play-control wiring and editor-mode button-state logic can be reused by the fixed top bar
- [X] T011 [US1] Implement `Project/Editor/Panels/EditorTopBar.cpp` with a fixed top-bar layout that hosts the embedded menu region and embedded play-control region
- [X] T012 [US1] Update `Project/Editor/Core/Editor.cpp` and `Project/Editor/Panels/MenuBar.cpp` so `Project/Editor/Panels/EditorTopBar.cpp` becomes the default top-level entry point instead of relying on standalone `Menu Bar` and `Toolbar` windows
- [ ] T013 [US1] Validate the unified top bar workflow against `specs/010-editor-top-bottom-bars/quickstart.md` by confirming menu commands and play controls are reachable without extra windows

**Checkpoint**: User Story 1 is complete when the Editor starts with one fixed top bar that provides menu access and play controls in a Unity-like location.

---

## Phase 4: User Story 2 - Adjust Scene Editing Mode From The Top Bar (Priority: P2)

**Goal**: Make the top bar express and control the core scene-editing mode, including honest handling of unsupported Unity-style affordances.

**Independent Test**: With Scene View active, switch transform modes from the top bar and confirm the active state and subsequent scene interaction both reflect the selected mode.

### Implementation for User Story 2

- [X] T014 [US2] Update `Project/Editor/Panels/EditorTopBar.h` and `Project/Editor/Panels/EditorTopBar.cpp` to add scene-tool command groups for translate/rotate/scale with active-state rendering
- [X] T015 [US2] Update `Project/Editor/Panels/EditorTopBar.cpp` and `Project/Editor/Panels/SceneView.cpp` so top-bar tool clicks drive the live Scene View gizmo operation
- [X] T016 [US2] Update `Project/Editor/Panels/EditorTopBar.cpp` to render coordinate/reference-mode affordances as interactive controls when supported and explicit disabled placeholders when unsupported
- [ ] T017 [US2] Validate scene editing mode behavior using `specs/010-editor-top-bottom-bars/quickstart.md` and confirm unsupported Unity-style affordances are not presented as working actions

**Checkpoint**: User Story 2 is complete when the top bar both displays and controls the supported scene-editing mode state without pretending to support missing Unity behaviors.

---

## Phase 5: User Story 3 - Read Frame Rate From A Bottom Status Bar (Priority: P3)

**Goal**: Add a persistent bottom status bar that continuously displays FPS during normal editor use.

**Independent Test**: Launch the Editor, keep any main view visible, and confirm a fixed bottom bar shows FPS from startup through steady rendering and window resize.

### Implementation for User Story 3

- [X] T018 [US3] Implement `Project/Editor/Panels/EditorStatusBar.cpp` with a fixed bottom-bar layout and an FPS indicator bound to the editor frame-timing state from `Project/Editor/Core/Editor.cpp`
- [X] T019 [US3] Update `Project/Editor/Core/Editor.cpp` and `Project/Editor/Panels/EditorStatusBar.cpp` to keep the bottom status bar visible across startup, focus changes, and resize-driven redraw passes
- [ ] T020 [US3] Validate FPS status behavior using `specs/010-editor-top-bottom-bars/quickstart.md` and confirm the indicator initializes cleanly and updates during steady rendering

**Checkpoint**: User Story 3 is complete when the Editor keeps a fixed bottom status bar visible and the FPS indicator remains readable and live-updating.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Remove redundant entry points, harden cramped-layout behavior, and close out validation/documentation.

- [X] T021 [P] Update `Project/Editor/Core/Editor.cpp` and `Project/Editor/Panels/MenuBar.cpp` to remove or hide redundant default standalone `Toolbar` and `FrameInfo` entry points that would duplicate the new global bars
- [X] T022 [P] Tighten narrow-width and resize fallback behavior in `Runtime/UI/Panels/PanelViewportBar.cpp` and `Project/Editor/Panels/EditorTopBar.cpp` so core controls remain identifiable in cramped layouts
- [X] T023 Update `specs/010-editor-top-bottom-bars/quickstart.md` with final validation notes and any DX12-Editor-specific limitations discovered during implementation
- [ ] T024 Validate the integrated feature end-to-end by following `specs/010-editor-top-bottom-bars/quickstart.md` after rebuilding the Editor target

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1: Setup** has no dependencies and can start immediately.
- **Phase 2: Foundational** depends on Phase 1 and blocks all user-story implementation.
- **Phase 3: US1** depends on Phase 2 and is the suggested MVP.
- **Phase 4: US2** depends on US1 because scene-mode controls extend the fixed top bar introduced there.
- **Phase 5: US3** depends on Phase 2 and can proceed after the foundational bar scaffolding and timing state are ready.
- **Phase 6: Polish** depends on all desired user stories being complete.

### User Story Dependencies

- **User Story 1 (P1)**: Starts after Foundational and delivers the first usable fixed global bar.
- **User Story 2 (P2)**: Builds on User Story 1’s fixed top bar and shared Scene View state.
- **User Story 3 (P3)**: Can start after Foundational and is functionally independent from US2, but still benefits from the shared bar infrastructure established for US1.

### Within Each User Story

- Shared state exposure before consuming UI
- Reusable/refactored legacy logic before final bar composition
- Final story validation after implementation tasks

### Parallel Opportunities

- **Setup**: `T003` and `T004` can run in parallel because they create different panel headers.
- **Foundational**: `T006` and `T007` can run in parallel because scene-tool state exposure and frame-timing exposure touch different concerns.
- **US1**: `T009` and `T010` can run in parallel because menu reuse and toolbar reuse target different files.
- **Polish**: `T021` and `T022` can run in parallel because redundant-entry cleanup and cramped-layout behavior touch different finalization scopes.

---

## Parallel Example: Foundational Phase

```text
T006: Update Project/Editor/Panels/SceneView.h and Project/Editor/Panels/SceneView.cpp to expose gizmo-operation state
T007: Update Project/Editor/Core/Editor.h and Project/Editor/Core/Editor.cpp to cache frame-timing/FPS state
```

## Parallel Example: User Story 1

```text
T009: Refactor Project/Editor/Panels/MenuBar.h and Project/Editor/Panels/MenuBar.cpp for embedded menu content reuse
T010: Refactor Project/Editor/Panels/Toolbar.h and Project/Editor/Panels/Toolbar.cpp for embedded play-control reuse
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1
4. Stop and validate the fixed top bar using `specs/010-editor-top-bottom-bars/quickstart.md`

### Incremental Delivery

1. Deliver the fixed top bar shell and shared bar infrastructure first
2. Add scene-editing mode controls as the second increment
3. Add the FPS bottom status bar as the third increment
4. Finish with redundant-entry cleanup and cramped-layout hardening

### Suggested MVP Scope

**User Story 1 only**. It provides the core visible product change by making the Editor start with one fixed, Unity-style top bar that exposes menus and play controls.

---

## Notes

- `[P]` tasks use separate files or non-overlapping concerns and can run in parallel.
- Each user story ends with a validation task tied to `specs/010-editor-top-bottom-bars/quickstart.md`.
- No new automated tests are required by the feature spec, so the primary completion evidence is Editor build health plus focused manual verification.
