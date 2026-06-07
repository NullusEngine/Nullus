# Implementation Plan: Fix FBX Assimp Unit Scale

**Branch**: `fix-fbx-assimp-unit-scale` | **Date**: 2026-06-07 | **Spec**: `specs/fix-fbx-assimp-unit-scale/spec.md`  
**Input**: Feature specification from `specs/fix-fbx-assimp-unit-scale/spec.md`

## Summary

Apply FBX global unit scaling in the external model import path so FBX mesh payloads use the same meter-scale convention as glTF while authored node scale remains intact. Normalize only Assimp's synthetic FBX root unit wrapper when it appears in detailed scene data.

## Technical Context

**Language/Version**: C++17 in the existing Nullus build  
**Primary Dependencies**: External model importer, Assimp parser path, Nullus imported scene data, prefab artifact serialization  
**Storage**: Generated model mesh and prefab artifacts under `Library/Artifacts`  
**Testing**: `NullusUnitTests` with focused `AssetImportPipelineTests` filters  
**Target Platform**: Windows editor/runtime asset import pipeline; data-level fix is renderer-backend neutral  
**Project Type**: Desktop engine/editor  
**Performance Goals**: No measurable import-time overhead beyond existing Assimp/FbxSdk global-scale postprocessing  
**Constraints**: Do not edit generated files, do not alter mesh serialization format, do not strip authored FBX child scale  
**Scale/Scope**: One importer implementation file, one parser implementation file, one focused regression assertion, one spec bundle

## Constitution Check

- Spec scope: Required because this changes Runtime asset import behavior under the rendering resource parser.
- Generated files: No files under `Runtime/*/Gen/` will be edited.
- Validation: Use the existing failing focused import pipeline unit test, then run nearby Assimp/FBX import tests.
- Product runtime viability: No renderer or product lifecycle code changes are planned.
- Evidence path: RED focused test already observed, production fix, GREEN focused tests, then required plan-review.

## Project Structure

### Documentation

```text
specs/fix-fbx-assimp-unit-scale/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Rendering/Resources/Parsers/AssimpParser.cpp
Project/Editor/Assets/ExternalAssetImporter.cpp
Tests/Unit/AssetImportPipelineTests.cpp
```

**Structure Decision**: Keep FBX unit-scale flag selection in `ExternalAssetImporter.cpp`, where parser flags are chosen per source format. Keep Assimp synthetic-root normalization in `AssimpParser.cpp`, where Assimp nodes are converted into Nullus imported scene records. Keep verification in the existing asset import pipeline unit test file.

## Complexity Tracking

No constitution violations are expected.
