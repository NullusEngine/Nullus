# Tasks: Editor Settings Trim

**Input**: Design documents from `/specs/024-editor-settings-trim/`

## Phase 1: Tests

- [x] T001 [P] Add reflected settings test proving persistent runtime/debug-draw settings exclude diagnostics, automation, and no-op visualizer fields in `Tests/Unit/ProjectSettingsPanelTests.cpp`
- [x] T002 [P] Keep editor CLI diagnostics tests covering validation/readback/logging arguments in `Tests/Unit/EditorLaunchArgsTests.cpp`

## Phase 2: Implementation

- [x] T003 Remove persistent diagnostic, automation, and no-op visualizer fields from `Project/Editor/Settings/EditorSettings.h`
- [x] T004 Simplify `EditorSettings::BuildDiagnosticsSettings()` so persistent settings no longer seed diagnostics in `Project/Editor/Settings/EditorSettings.cpp`
- [x] T005 Remove obsolete debug-draw/frustum menu writes for deleted fields in `Project/Editor/Panels/MenuBar.cpp`
- [x] T006 Regenerate Editor reflection outputs through the CMake/MetaParser build flow for `Project/Editor/Gen`

## Phase 3: Validation

- [x] T007 Run targeted `NullusUnitTests` filters for Project Settings and Editor launch args
- [x] T008 Run the most relevant build target available without clobbering unrelated local work
- [x] T009 Run required plan-review quality gate before reporting completion
