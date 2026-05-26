# Implementation Plan: Optional Assimp FBX Import

**Branch**: `036-assimp-fbx-option` | **Date**: 2026-05-25 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/036-assimp-fbx-option/spec.md`

## Summary

Add a controlled Assimp FBX read path without changing existing FBX imports by default. The implementation will add a narrow Assimp FBX build option, persist an FBX reader selection in model import settings, route editor model import and native mesh cache generation through a shared reader-selection helper, and emit explicit diagnostics when Assimp is unavailable or used as a fallback.

## Technical Context

**Language/Version**: C++20, CMake, PowerShell Spec-Kit workflow
**Primary Dependencies**: Autodesk FBX SDK target, Assimp, GoogleTest, existing Nullus asset import and artifact writer systems
**Storage**: Asset `.meta` files, generated artifact manifests under project `Library/Artifacts`
**Testing**: `NullusUnitTests` GoogleTest executable plus source-level CMake contract tests
**Target Platform**: Windows, Linux, macOS editor/runtime builds; live Autodesk SDK coverage depends on bundled SDK availability
**Project Type**: C++ desktop/editor runtime with engine asset pipeline
**Performance Goals**: Preserve current default FBX import timing and avoid extra parser attempts unless fallback is explicitly selected
**Constraints**: Do not hand-edit generated files; do not change default FBX output; do not enable unrelated Assimp importers/exporters through the narrow option
**Scale/Scope**: Model import settings, editor import routing, built-in mesh artifact generation, and ThirdParty Assimp configuration

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec scope**: PASS. This is a behavior change in `Runtime/` and `Project/`, so it uses a dedicated `specs/036-assimp-fbx-option/` bundle.
- **Generated boundaries**: PASS. No files under `Runtime/*/Gen/` or generated editor metadata will be hand-edited.
- **Validation path**: PASS. Use targeted unit/source contract tests for settings, CMake options, import routing, diagnostics, and reader preservation. Live Autodesk success tests remain conditional on `NLS_HAS_AUTODESK_FBX_SDK`.
- **Product runtime viability**: PASS. Existing editor/game FBX defaults stay on Autodesk; Assimp fallback is explicit and diagnostic-driven.
- **Backend/platform claims**: PASS. This change is asset import/build configuration only and will not claim graphics backend correctness.

## Project Structure

### Documentation (this feature)

```text
specs/036-assimp-fbx-option/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
ThirdParty/
└── CMakeLists.txt

Project/Editor/Assets/
├── AssetImporterSettings.h
├── AssetImporterSettings.cpp
├── AssetImporterFacade.h
├── AssetImporterFacade.cpp
├── ExternalAssetImporter.h
└── ExternalAssetImporter.cpp

Runtime/Core/ResourceManagement/
└── MeshManager.cpp

Runtime/Rendering/Resources/Parsers/
├── AssimpParser.h
└── AssimpParser.cpp

Tests/Unit/
├── AssetImporterFacadeTests.cpp
├── AssetImportPipelineTests.cpp
├── FbxSdkIntegrationContractTests.cpp
└── CMakeLists.txt

Docs/
└── Testing.md
```

**Structure Decision**: Reuse the existing editor asset import settings/facade, external model importer, parser classes, and built-in mesh manager paths. Add no new subsystem unless a tiny helper boundary inside existing asset importer code is needed to avoid duplicated reader selection logic.

## Complexity Tracking

No constitution violations require justification.

## Phase 0: Research Summary

See [research.md](research.md). Decisions:

- Add a narrow `NLS_ENABLE_ASSIMP_FBX_IMPORTER` CMake option.
- Persist FBX reader selection as a model importer setting.
- Keep Autodesk as default and gate fallback behind an explicit reader mode.
- Emit diagnostics for unavailable Assimp and fallback events.

## Phase 1: Design Summary

See [data-model.md](data-model.md) and [quickstart.md](quickstart.md). The design keeps reader choice as asset metadata and routes FBX scene import and mesh-cache generation through the same effective reader selection.

## Constitution Check - Post Design

- **Spec scope**: PASS. The plan remains within the feature bundle and existing import/build boundaries.
- **Generated boundaries**: PASS. No generated files are listed for edits.
- **Validation path**: PASS. Unit/source contract tests are identified before implementation.
- **Product runtime viability**: PASS. Default Autodesk behavior remains the MVP acceptance gate.
- **Backend/platform claims**: PASS. Validation is limited to asset import/build behavior.
