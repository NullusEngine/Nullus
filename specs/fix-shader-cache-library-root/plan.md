# Implementation Plan: Shader Cache Uses Project Library

**Branch**: `fix-shader-cache-library-root` | **Date**: 2026-05-18 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/fix-shader-cache-library-root/spec.md`

## Summary

Shader cache database path selection must prefer the configured project asset root before falling back to source-path inference. Shader compiler DXIL/SPIR-V binary artifact output must also prefer the configured project shader cache database directory, so project cache metadata and compiled binaries live together under `<project>/Library/ShaderCache`.

This mirrors Unity's asset pipeline split: built-in resources can be read from engine/package locations, but imported artifacts and cache metadata are scoped to the current project's `Library`.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus runtime resource managers, shader loader, shader compiler cache database
**Storage**: File-based shader cache database and compiled shader binary artifacts under `Library/ShaderCache`
**Testing**: `NullusUnitTests` with GoogleTest
**Target Platform**: Desktop editor/game runtime
**Project Type**: C++ engine/runtime
**Performance Goals**: O(path-depth) path resolution; no shader compilation hot-path regression beyond existing path normalization
**Constraints**: Preserve direct `ShaderLoader` fallback behavior for non-`App` asset roots and do not edit generated files
**Scale/Scope**: Existing Runtime/Core resource manager, Runtime/Rendering shader loader, and Runtime/Rendering shader compiler cache path selection

## Constitution Check

- Spec scope: Present, because this is a Runtime/Rendering behavior change.
- Generated-file boundaries: No `Runtime/*/Gen/` edits planned.
- Backend/platform validation: Unit-level path resolution is backend-independent; no backend correctness claim will be made.
- Product runtime viability: Editor/Game continue using existing resource manager flow; fallback remains for direct callers.
- Evidence path: Focused `NullusUnitTests` filter for shader cache path resolution, `App/Assets` fallback guard, configured artifact output under project `Library`, and relevant shader compiler cache tests.

## Project Structure

### Documentation

```text
specs/fix-shader-cache-library-root/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Core/ResourceManagement/
├── ShaderManager.h
└── ShaderManager.cpp

Runtime/Rendering/Resources/Loaders/
├── ShaderLoader.h
└── ShaderLoader.cpp

Runtime/Rendering/ShaderCompiler/
├── ShaderCompiler.h
└── ShaderCompiler.cpp

Tests/Unit/
└── ShaderCompilerTests.cpp
```

**Structure Decision**: Keep project cache root discovery in the existing shader resource loading path and let the shader compiler derive binary artifact output from the configured database path. Bare compiler calls without a database path keep the current user-local fallback.

**Unity Alignment**: Treat `App/Assets/Engine` like Unity built-in/prebuilt resources and treat `<project>/Library/ShaderCache` like Unity's project-local imported artifact/cache store.

## Complexity Tracking

No constitution violations.
