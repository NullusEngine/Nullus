# Implementation Plan: Unity-Ready Model Drop

**Branch**: `unity-ready-model-drop` | **Date**: 2026-05-31 | **Spec**: `specs/unity-ready-model-drop/spec.md`
**Input**: Feature specification from `specs/unity-ready-model-drop/spec.md`

## Summary

Align generated model drag/drop with Unity's asset lifecycle: the Project asset must be renderer-ready before it becomes a committed scene instance. The implementation adds a renderer dependency readiness gate to fast prefab loading and hardens the legacy async resolution path so it cannot silently show fallback white textures.

## Technical Context

**Language/Version**: C++17/C++20 project style already used by Nullus
**Primary Dependencies**: Nullus editor asset pipeline, prefab workflow, native artifact manifest, texture artifact reader
**Storage**: Project `Library/Artifacts/<asset-guid>/manifest.json` and native artifact files
**Testing**: `NullusUnitTests` with focused gtest filters
**Target Platform**: Windows editor, DX12 validation path
**Project Type**: Desktop editor/runtime engine
**Performance Goals**: Drag/drop must use manifest/header validation only; no full synchronous large GPU texture upload in the gate
**Constraints**: No hand edits under `Runtime/*/Gen/`; preserve existing watcher/import progress flow; do not regress non-generated prefab drops
**Scale/Scope**: Large imported scenes such as Sponza with hundreds of meshes/materials and dozens of textures

## Constitution Check

- Spec-first major change: PASS. This bundle records the rendering/editor asset lifecycle change.
- Validation matches subsystem: PASS. Unit tests cover asset lifecycle contracts; final runtime evidence should include RenderDoc for the reported model.
- Generated code/backend boundaries: PASS. No generated files are edited; DX12 evidence does not claim other backends.
- Incremental verified delivery: PASS. Work is split into readiness gate, async hardening, and validation.
- Product runtime preservation: PASS. Existing imported prefab flow remains; stale generated models become pending/rejected instead of white visible objects.

## Project Structure

### Documentation

```text
specs/unity-ready-model-drop/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Assets/
├── EditorAssetDragDropBridge.cpp     # Fast imported prefab loading and renderer readiness gate
├── EditorAssetDragDropBridge.h       # Private helper declarations if needed
└── AssetDragDropWorkflow.cpp         # Transactional generated prefab drop checks if needed

Project/Editor/Core/
└── EditorActions.cpp                 # Legacy renderer resource resolution hardening

Tests/Unit/
├── EditorAssetDragDropTests.cpp      # Behavior tests for generated model readiness gate
└── EditorRenderPathContractTests.cpp # Source/contract tests for async hardening
```

**Structure Decision**: Keep changes inside existing editor asset and editor action boundaries. Do not introduce a parallel import system.

## Research

- Unity 2018.4 `SceneView` drag calls editor drag handlers on `DragUpdated`/`DragPerform`, and prefab dragging instantiates a persistent prefab asset preview before final placement.
- Unity `ModelImporter` creates or resolves materials during import, registers internal materials as prefab sub-assets, and tracks external material remaps.
- Unity `TextureImporter` defaults normal/default texture types to mipmap-enabled imported Texture2D data and selects platform compression automatically.
- Therefore Unity's "instant correct result" comes from pre-imported AssetDatabase artifacts, not from scene-drop-time texture repair.

## Data Model

- `RendererDependencyReadiness`: aggregate validation result containing ready flag and diagnostic codes.
- `RequiredArtifact`: manifest sub-asset entry plus resolved file path and type.
- `GeneratedModelDrop`: existing drag/drop request whose payload kind is `GeneratedModelPrefabAsset`.

## Validation

Run:

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=EditorAssetDragDropTests.*GeneratedModel*:EditorAssetDragDropTests.*RendererReady*:EditorRenderPathContractTests.*GeneratedModel*:EditorRenderPathContractTests.*MaterialArtifact*
git diff --check
```

Runtime evidence after unit validation:

```powershell
python tools/renderdoc/rdc_doctor.py App/Win64_Release_Runtime_Shared/Build/RenderDocCaptures/Editor/<new-capture>.rdc
```

Expected RenderDoc outcome: representative Sponza GBuffer draws bind real model texture resources, not only the 1x1 fallback texture.
