# Implementation Plan: Reflected Editor Settings Panel

**Branch**: `017-reflected-settings-panel` | **Date**: 2026-05-05 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/017-reflected-settings-panel/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Build an editor-only Unity-style Settings modal opened from `Edit > Settings`, with top search, left category navigation, and right reflected details. Introduce a `SettingObject` registration/persistence layer, extract Inspector's existing reflected field drawing into a shared property drawer, and use that shared drawer from both Inspector and Settings so new settings pages are added by registering reflected objects rather than hand-writing panels.

## Technical Context

<!--
  ACTION REQUIRED: Replace the content in this section with the technical details
  for the project. The structure here is presented in advisory capacity to guide
  the iteration process.
-->

**Language/Version**: C++20 in existing CMake project  
**Primary Dependencies**: Existing Nullus editor UI widgets, ImGui integration, Nullus reflection runtime, json11 for lightweight persistence  
**Storage**: File-based editor settings JSON under the project/user settings area selected by existing editor path conventions  
**Testing**: `NullusUnitTests` plus `Editor` target build  
**Target Platform**: Editor desktop app, validated first on Windows Debug build
**Project Type**: C++ desktop editor application  
**Performance Goals**: Search and category selection complete within the current frame for the initial editor settings set; no per-frame disk IO while the modal is open  
**Constraints**: Editor-only scope, modal rather than dockable, Scene view mouse input blocked while open, no hand-edits under `Runtime/*/Gen/`, preserve existing Inspector behavior  
**Scale/Scope**: Initial implementation supports a small set of editor/project settings objects and the reflected field types currently used by Inspector workflows

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. Spec bundle exists under `specs/017-reflected-settings-panel/`.
- **Validation matches subsystem**: PASS. Plan includes unit tests for registry/search/persistence/shared drawer behavior and `Editor` build verification.
- **Generated code and backend boundaries**: PASS. No generated files under `Runtime/*/Gen/` will be hand-edited; changes are editor UI/reflection consumers only.
- **Incremental, verified delivery**: PASS. Tasks will split shared drawer, settings model/persistence, UI integration, and tests.
- **Product runtime preservation**: PASS. `Editor` remains runnable; runtime `Game` behavior is out of scope and unaffected.

## Project Structure

### Documentation (this feature)

```text
specs/017-reflected-settings-panel/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)
<!--
  ACTION REQUIRED: Replace the placeholder tree below with the concrete layout
  for this feature. Delete unused options and expand the chosen structure with
  real paths (e.g., apps/admin, packages/something). The delivered plan must
  not include Option labels.
-->

```text
Project/Editor/
├── Panels/
│   ├── Inspector.*
│   ├── ReflectedPropertyDrawer.*
│   └── ProjectSettings.*
├── Settings/
│   ├── EditorSettingObject.*
│   ├── EditorSettings.*
│   ├── EditorSettingsRegistry.*
│   └── EditorSettingsPersistence.*
└── Core/
    └── Editor.* / menu integration

Tests/Unit/
├── ReflectedPropertyDrawerTests.cpp
├── EditorSettingsRegistryTests.cpp
├── EditorSettingsPersistenceTests.cpp
└── ProjectSettingsPanelTests.cpp
```

**Structure Decision**: Extend existing `Project/Editor` panel/settings modules. Keep reflected drawing in `Panels` because it is shared by Inspector and Settings UI, and keep registration/persistence in `Settings` because it owns editor SettingObject lifecycle.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | N/A | N/A |

## Post-Design Constitution Check

- **Spec-first major change**: PASS. The plan, research, data model, contracts, quickstart, and tasks stay in one spec bundle.
- **Validation matches subsystem**: PASS. Validation uses unit tests for editor logic and a real `Editor` build.
- **Generated code and backend boundaries**: PASS. No generated files or rendering backend contracts are modified.
- **Incremental, verified delivery**: PASS. Tasks are ordered from shared drawer extraction to settings UI/persistence.
- **Product runtime preservation**: PASS. Editor target remains the product validation surface for this change.
