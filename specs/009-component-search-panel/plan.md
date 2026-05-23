# Implementation Plan: Reflection-Driven Component Search Dropdown

**Branch**: `[009-component-search-panel]` | **Date**: 2026-05-02 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/009-component-search-panel/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Replace the Inspector's current hardcoded component dropdown with a Unity-style searchable popup that opens under the `Add Component` button, discovers addable component types through the existing reflection system, groups them by reflection-declared menu metadata, filters out invalid or duplicate options for the selected actor, and adds components through the same runtime `GameObject` rules already used by dynamic component creation.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus Editor panels, Nullus UI widget system and ImGui popup/menu primitives, Nullus reflection runtime (`meta::Type`, `TypeCreator`, `MetaManager`), C# MetaParser generation templates, Engine `GameObject` component APIs  
**Storage**: N/A  
**Testing**: `NullusUnitTests`, targeted editor manual verification  
**Target Platform**: Nullus Editor desktop runtime on Windows first, with implementation kept backend-agnostic and platform-neutral where possible  
**Project Type**: Native desktop editor application  
**Performance Goals**: Component picker opens without perceptible lag in normal editor use and search filtering updates within the same interaction for the current reflected component set  
**Constraints**: Must not hand-edit generated reflection output under `Runtime/*/Gen/`; must preserve editor runtime viability; must keep actual addability driven by engine component rules rather than a second hand-maintained registry  
**Scale/Scope**: Single editor workflow centered on Inspector component addition, with one new picker interaction, a small extension to type-level reflection metadata generation, shared reflection-based discovery, and targeted coverage for eligible engine component types

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first scope**: Pass. This change affects `Project/Editor` behavior and already has a dedicated spec bundle under `specs/009-component-search-panel/`.
- **Validation matched to subsystem**: Pass. Planned evidence is targeted unit coverage where feasible plus exact manual verification in the Editor for the add/search flow.
- **Generated-code boundaries respected**: Pass. No hand edits are planned under `Runtime/*/Gen/`; reflection will be consumed through runtime APIs only.
- **Incremental verified delivery**: Pass. The implementation can be split into discovery/filtering logic, picker UI behavior, and Inspector integration.
- **Product runtime preservation**: Pass. The Editor remains runnable throughout; the feature replaces an Inspector affordance without changing renderer/backend architecture.

## Project Structure

### Documentation (this feature)

```text
specs/009-component-search-panel/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── component-picker-ui-contract.md
└── tasks.md
```

### Source Code (repository root)

```text
Project/
├── Editor/
│   ├── Core/
│   │   ├── Editor.cpp
│   │   └── PanelsManager.h
│   └── Panels/
│       ├── Inspector.cpp
│       ├── Inspector.h
│       ├── ComponentSearchPanel.cpp
│       └── ComponentSearchPanel.h

Runtime/
├── Base/
│   └── Reflection/
│       ├── RuntimeMetaProperties.h
│       ├── Type.h
│       └── Type.cpp
├── Engine/
│   ├── Components/
│   │   ├── CameraComponent.h
│   │   ├── Component.h
│   │   ├── LightComponent.h
│   │   ├── MaterialRenderer.h
│   │   ├── MeshRenderer.h
│   │   └── SkyBoxComponent.h
│   └── GameObject.cpp

Tools/
└── MetaParser/
    └── src/
        ├── MetaParserTool.TextParser.cs
        ├── MetaParserTool.Generation.cs
        ├── Models/ReflectionModels.cs
        ├── Generation/GenerationModels.cs
        └── Templates/HeaderGenerated.cpp.tt

Tests/
└── Unit/
    ├── CMakeLists.txt
    ├── EditorComponentPickerTests.cpp
    ├── MetaParserGenerationModuleTests.cpp
    └── ReflectionRuntimeCoreTests.cpp
```

**Structure Decision**: Keep the behavior centered in `Project/Editor/Panels`, but convert the picker from a standalone editor window into an Inspector-owned popup widget/controller so it can anchor directly under the `Add Component` button. Add a minimal type-level reflection metadata path to the MetaParser and runtime meta-property set so component categories come from reflection declarations instead of hardcoded Inspector logic.

## Phase 0: Research

See [research.md](./research.md) for resolved design decisions covering reflection traversal, addability filtering, and picker UI shape.

## Phase 1: Design & Contracts

- Data model captured in [data-model.md](./data-model.md)
- UI behavior contract captured in [contracts/component-picker-ui-contract.md](./contracts/component-picker-ui-contract.md)
- Verification flow captured in [quickstart.md](./quickstart.md)
- Agent context updated after artifact generation

## Post-Design Constitution Check

- **Spec-first scope**: Still passes. The plan remains inside the same spec bundle.
- **Validation matched to subsystem**: Still passes. Design includes unit tests for deterministic picker logic and manual editor verification for interaction behavior.
- **Generated-code boundaries respected**: Still passes. Design edits the MetaParser inputs/templates and consumes regenerated `Runtime/*/Gen/*` output without hand-editing generated files directly.
- **Incremental verified delivery**: Still passes. Work remains decomposable into small reviewable steps.
- **Product runtime preservation**: Still passes. The Editor stays runnable, and no backend/platform claims extend beyond validated targets.

## Complexity Tracking

No constitution violations require justification for this feature.
