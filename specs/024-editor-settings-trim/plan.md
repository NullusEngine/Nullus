# Implementation Plan: Editor Settings Trim

**Branch**: `024-editor-settings-trim` | **Date**: 2026-05-21 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/024-editor-settings-trim/spec.md`

## Summary

Trim reflected editor settings to only persistent user preferences, while preserving one-shot diagnostics through `EngineDiagnosticsSettings` and CLI overrides. Remove no-op debug draw/frustum settings and persistent diagnostic/automation fields from `EditorSettings`, update menus and tests, and regenerate Editor reflection outputs through MetaParser.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus reflection/MetaParser, ImGui editor UI, GoogleTest  
**Storage**: Project `UserSettings/editor-settings.json` via reflected settings persistence  
**Testing**: `NullusUnitTests`, targeted editor settings and launch args tests  
**Target Platform**: Windows editor path, cross-platform source compatibility  
**Project Type**: Desktop editor/runtime  
**Performance Goals**: No runtime hot-path cost; settings enumeration remains reflection-driven  
**Constraints**: Do not hand-edit `Project/Editor/Gen`; old settings files must load without failing  
**Scale/Scope**: `Project/Editor/Settings`, `Project/Editor/Panels/MenuBar`, editor launch diagnostics tests, generated Editor reflection

## Constitution Check

- Spec-first scope: PASS. This changes reflected editor settings and generated registration behavior, so a spec bundle is present.
- Generated code boundary: PASS if `Project/Editor/Gen` is updated only by the MetaParser build flow.
- Test-with-change: PASS with reflected settings and CLI parsing tests.
- Validation evidence: Requires targeted `NullusUnitTests` filters and build/MetaParser evidence.

## Project Structure

### Documentation

```text
specs/024-editor-settings-trim/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Settings/
├── EditorSettings.h
└── EditorSettings.cpp

Project/Editor/Panels/
└── MenuBar.cpp

Project/Editor/Gen/
└── Project/Editor/Settings/EditorSettings.generated.*

Tests/Unit/
├── ProjectSettingsPanelTests.cpp
└── EditorLaunchArgsTests.cpp
```

**Structure Decision**: Keep settings ownership in existing `EditorSettings` reflected objects. Keep one-shot diagnostic transport in existing `EngineDiagnosticsSettings` and CLI parse flow.

## Complexity Tracking

No constitution violations.
