# Implementation Plan: Fix Multi-Primitive Materials

**Branch**: `[039-fix-multiprimitive-materials]` | **Date**: 2026-05-29 | **Spec**: `spec.md`
**Input**: Feature specification from `specs/039-fix-multiprimitive-materials/spec.md`

## Summary

Fix generated model material mismatches by preserving source mesh primitive material assignments through mesh artifact generation and prefab creation. The runtime `Mesh` still selects one material by `materialIndex`, so imported multi-primitive meshes must become separately renderable primitive meshes rather than one merged mesh that keeps only the first material.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Existing Nullus asset import, prefab serialization, mesh/material managers
**Storage**: Native `.nmesh`, `.nmat`, `.nprefab` artifacts
**Testing**: `NullusUnitTests` with focused asset import, prefab, renderer contract tests
**Target Platform**: Editor generated model workflow
**Project Type**: Desktop engine/editor
**Performance Goals**: Preserve deferred scene insertion behavior and avoid synchronous texture loads
**Constraints**: Do not hand-edit generated files; keep product runtime viable; validate rendering-related behavior with focused tests and RenderDoc/manual evidence where practical
**Scale/Scope**: Generated model import and prefab instantiation for multi-primitive meshes

## Constitution Check

- Spec scope: Required because the change spans `Runtime/` asset/prefab behavior and `Project/` editor import/runtime resource resolution.
- Generated files: No files under `Runtime/*/Gen/` will be hand-edited.
- Backend/platform validation: Automated tests validate asset/prefab contracts; visual renderer verification remains limited to the local editor backend unless a capture is taken.
- Product runtime: Existing single-primitive generated model prefab behavior must remain compatible; editor drag-drop must remain asynchronous.
- Evidence path: Red/green unit tests plus focused Debug build/test commands.

## Project Structure

```text
Runtime/
├── Engine/Assets/ModelPrefabBuilder.cpp
├── Rendering/Assets/SceneImportPipeline.cpp
└── Rendering/Resources/Parsers/

Project/
└── Editor/Assets/ExternalAssetImporter.cpp

Tests/
└── Unit/
```

**Structure Decision**: Keep the fix inside existing import and prefab generation paths; do not introduce a new runtime submesh rendering API.

## Complexity Tracking

No constitution violations are planned.
