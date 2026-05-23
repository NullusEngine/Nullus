# Implementation Plan: Editor Shortcut System

**Branch**: `016-editor-shortcut-system` | **Date**: 2026-05-05 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/016-editor-shortcut-system/spec.md`

## Summary

Implement an Editor-only shortcut system inspired by Unity Shortcuts Manager. The system centralizes editor command registration, active-context resolution, conflict detection, display text, and user profile persistence, then migrates the existing hard-coded editor shortcuts onto that service. Runtime/Game input remains out of scope. The optional `imgui_keyboard` integration is planned only as a shortcut settings UI helper after the core service is verified.

## Technical Context

**Language/Version**: C++20, matching existing `NullusUnitTests` and editor build settings  
**Primary Dependencies**: Existing Nullus Editor, Windowing `InputManager`, ImGui UI layer, `nlohmann::json` already available under `ThirdParty/Json`  
**Storage**: Project user settings file under `UserSettings`, using JSON for shortcut profile overrides  
**Testing**: GoogleTest via `NullusUnitTests`; focused manual editor verification for UI and runtime command behavior  
**Target Platform**: Nullus Editor desktop targets, with Windows as the initial validation target and no unverified cross-platform claims  
**Project Type**: Desktop editor application subsystem  
**Performance Goals**: Shortcut resolution must stay frame-cheap for the expected editor command set; lookup should be deterministic and bounded by registered commands  
**Constraints**: Do not edit generated files under `Runtime/*/Gen/`; do not change Runtime/Game input semantics; preserve Editor and Game product runnability  
**Scale/Scope**: Initial command set migrates current editor shortcuts and supports future editor panels/tools through registration APIs

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first scope**: PASS. This is an editor behavior/workflow change and has a dedicated spec bundle under `specs/016-editor-shortcut-system/`.
- **Validation matches subsystem**: PASS. Plan uses targeted unit tests for registration, conflicts, context resolution, and persistence, plus focused editor manual checks for shortcut execution and UI behavior.
- **Generated code boundaries**: PASS. No generated files under `Runtime/*/Gen/` are part of the planned edits.
- **Incremental delivery**: PASS. Work is split into core model, persistence, resolution, migration, and UI integration.
- **Product runtime preservation**: PASS. Runtime/Game input is explicitly out of scope, and Editor/Game runnability must be preserved.

## Project Structure

### Documentation (this feature)

```text
specs/016-editor-shortcut-system/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── shortcut-service.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/Editor/
├── Core/
│   ├── Editor.cpp
│   ├── Editor.h
│   ├── EditorActions.cpp
│   └── EditorActions.h
├── Shortcuts/
│   ├── EditorShortcutBinding.h
│   ├── EditorShortcutCommand.h
│   ├── EditorShortcutContext.h
│   ├── EditorShortcutProfile.cpp
│   ├── EditorShortcutProfile.h
│   ├── EditorShortcutRegistry.cpp
│   ├── EditorShortcutRegistry.h
│   ├── EditorShortcutResolver.cpp
│   ├── EditorShortcutResolver.h
│   ├── EditorShortcutService.cpp
│   └── EditorShortcutService.h
└── Panels/
    ├── EditorTopBar.cpp
    ├── EditorTopBar.h
    ├── MenuBar.cpp
    ├── MenuBar.h
    ├── ShortcutSettingsPanel.cpp
    └── ShortcutSettingsPanel.h

Runtime/Platform/Windowing/Inputs/
├── InputManager.cpp
└── InputManager.h

Tests/Unit/
├── EditorShortcutBindingTests.cpp
├── EditorShortcutConflictTests.cpp
├── EditorShortcutPersistenceTests.cpp
└── EditorShortcutResolutionTests.cpp
```

**Structure Decision**: Keep the shortcut system under `Project/Editor/Shortcuts` because it is an editor-only command layer. Use existing `Runtime/Platform/Windowing/Inputs` only for key state consumption or small query helpers if needed. Add tests under `Tests/Unit` and include new editor shortcut sources in the test target as required.

## Phase 0: Research

Research output is captured in [research.md](research.md). All technical unknowns are resolved:

- Unity-style command/context/profile model selected.
- Editor-only scope selected.
- JSON user profile persistence selected.
- Context conflict model selected.
- `imgui_keyboard` treated as optional UI helper only.

## Phase 1: Design & Contracts

Design outputs:

- [data-model.md](data-model.md)
- [contracts/shortcut-service.md](contracts/shortcut-service.md)
- [quickstart.md](quickstart.md)

## Post-Design Constitution Check

- **Spec-first scope**: PASS. Generated artifacts stay in the same spec bundle.
- **Validation matches subsystem**: PASS. Unit and manual validation paths are defined before implementation.
- **Generated code boundaries**: PASS. Planned source layout excludes generated output.
- **Incremental delivery**: PASS. Contracts and data model support task slicing by core, persistence, migration, and UI.
- **Product runtime preservation**: PASS. Plan uses editor-only integration points and preserves Runtime/Game input ownership.

## Complexity Tracking

No constitution violations require justification.
