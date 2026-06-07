# Implementation Plan: glTF Texture Orientation

**Branch**: `045-gltf-texture-orientation` | **Date**: 2026-06-07 | **Spec**: `specs/045-gltf-texture-orientation/spec.md`  
**Input**: Feature specification from `specs/045-gltf-texture-orientation/spec.md`

## Summary

Fix external glTF/GLB model texture import so embedded or referenced images are decoded without a vertical flip. Keep legacy non-glTF behavior scoped to the current importer path, and bump the external texture postprocessor version so old flipped artifacts rebuild.

## Technical Context

**Language/Version**: C++17 in the existing Nullus build  
**Primary Dependencies**: Existing external model importer, stb image decode path, Nullus texture artifact serialization  
**Storage**: Native texture artifacts under `Library/Artifacts`  
**Testing**: `NullusUnitTests` with focused `AssetImportPipelineTests` filters  
**Target Platform**: Windows editor/runtime import pipeline; fix is data-level and backend-neutral  
**Project Type**: Desktop engine/editor  
**Performance Goals**: No additional per-pixel processing beyond removing an unnecessary flip for glTF/GLB textures  
**Constraints**: Do not alter shader UV sampling, mesh UV import, generated files, or global standalone texture behavior  
**Scale/Scope**: One importer helper, one postprocessor version, one focused unit regression plus nearby focused tests

## Constitution Check

- Spec scope: Required because the change affects `Project/` asset import behavior and rendering-visible artifacts. This bundle is scoped only to glTF/GLB external model texture orientation.
- Generated files: No files under `Runtime/*/Gen/` or generated output will be hand-edited.
- Validation: Use TDD with a focused texture artifact orientation test. Runtime RenderDoc validation is desirable after reimport but not required to prove the importer row-order bug.
- Product runtime viability: No renderer, frame graph, or shader pipeline changes are planned, so Editor/Game runtime viability should be preserved.
- Evidence path: RED focused test, production fix, GREEN focused test group, then code self-review/plan-review.

## Project Structure

### Documentation

```text
specs/045-gltf-texture-orientation/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Assets/ExternalAssetImporter.h
Project/Editor/Assets/ExternalAssetImporter.cpp
Project/Editor/Assets/AssetDatabaseFacade.cpp
Project/Editor/Assets/EditorAssetDragDropBridge.cpp
Runtime/Rendering/Assets/TextureArtifact.cpp
Tests/Unit/AssetImportPipelineTests.cpp
```

**Structure Decision**: Keep glTF orientation selection in the editor asset import pipeline where external model texture payloads are decoded. Keep STB flip state local to the decoding thread. Expose the external texture postprocessor version from the importer header so writer and current-manifest consumers share one version key. Keep verification in existing asset import/facade/drag-drop unit tests.

## Complexity Tracking

No constitution violations are expected.
