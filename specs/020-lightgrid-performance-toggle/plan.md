# Implementation Plan: LightGrid Performance Toggle

**Branch**: `020-lightgrid-performance-toggle` | **Date**: 2026-05-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/020-lightgrid-performance-toggle/spec.md`

## Summary

Add a project-level LightGrid rendering toggle exposed through Editor Project Settings, keep LightGrid enabled by default, and reduce the traced frame-time hotspot by reusing the per-frame LightGrid compile context instead of preparing it repeatedly in the same scene frame.

## Technical Context

**Language/Version**: C++20, CMake/Visual Studio 2022 on Windows for current validation
**Primary Dependencies**: Nullus Runtime Rendering, Engine scene renderers, Editor settings reflection/persistence, GoogleTest
**Storage**: Existing editor settings JSON under `UserSettings/editor-settings.json`
**Testing**: `NullusUnitTests` targeted rendering/settings tests
**Target Platform**: Windows DX12 validation for this change; logic remains backend-neutral where possible
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: Avoid duplicate LightGrid preparation in a single threaded scene frame; allow users to bypass LightGrid prep entirely when disabled
**Constraints**: Preserve existing rendering by default; do not hand-edit generated files under `Gen/`; do not claim cross-backend visual correctness without backend evidence
**Scale/Scope**: Runtime render settings, forward/deferred scene renderers, editor settings UI/persistence, targeted unit tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first scope: PASS. Rendering pipeline/editor settings changes are tracked under `specs/020-lightgrid-performance-toggle/`.
- Validation matches subsystem: PASS. Plan uses targeted rendering/settings tests and notes DX12/runtime visual verification as follow-up evidence.
- Generated code boundary: PASS. Reflection generated files will be produced by the normal MetaParser/build flow, not hand-edited.
- Incremental verified delivery: PASS. Tasks split setting, disabled path, and caching behavior with tests before implementation.
- Product runtime preservation: PASS. LightGrid defaults enabled; disabled mode is explicit degraded behavior.

## Project Structure

### Documentation (this feature)

```text
specs/020-lightgrid-performance-toggle/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/Settings/DriverSettings.h
Runtime/Rendering/Context/Driver.cpp
Runtime/Rendering/Context/DriverInternal.h
Runtime/Rendering/Context/DriverRendererAccess.h
Runtime/Engine/Rendering/BaseSceneRenderer.h
Runtime/Engine/Rendering/BaseSceneRenderer.cpp
Runtime/Engine/Rendering/ForwardSceneRenderer.cpp
Runtime/Engine/Rendering/DeferredSceneRenderer.cpp
Project/Editor/Settings/EditorSettings.h
Project/Editor/Settings/EditorSettings.cpp
Project/Editor/Core/Editor.cpp
Project/Game/Core/Context.cpp
Project/Launcher/Core/ProjectCreationWizard.cpp
Tests/Unit/EditorSettingsPersistenceTests.cpp
Tests/Unit/ThreadedRenderingLifecycleTests.cpp
Tests/Unit/SceneRenderGraphBuilderStructureTests.cpp
```

**Structure Decision**: Use existing settings, driver access, and scene renderer boundaries. The Editor Project Settings UI is reflection-driven, so adding a reflected editor rendering settings object is enough to surface the control.

## Complexity Tracking

No constitution violations.
