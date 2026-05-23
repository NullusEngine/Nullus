# Implementation Plan: Shader Cache Uses Project Library

**Branch**: `fix-shader-cache-library-root` | **Date**: 2026-05-18 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/fix-shader-cache-library-root/spec.md`

## Summary

Shader cache database path selection must prefer the configured project asset root before falling back to source-path inference. The implementation will expose a narrowly testable cache path resolver in the shader loader, pass `ShaderManager`'s configured project assets path into that resolver, and prevent direct `App/Assets` engine shader fallback from creating `App/Library`.

This mirrors Unity's asset pipeline split: built-in resources can be read from engine/package locations, but imported artifacts and cache metadata are scoped to the current project's `Library`.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus runtime resource managers, shader loader, shader compiler cache database
**Storage**: File-based shader cache database at `Library/ShaderCache/ShaderCache.tsv`
**Testing**: `NullusUnitTests` with GoogleTest
**Target Platform**: Desktop editor/game runtime
**Project Type**: C++ engine/runtime
**Performance Goals**: O(path-depth) path resolution; no shader compilation hot-path regression beyond existing path normalization
**Constraints**: Preserve direct `ShaderLoader` fallback behavior for non-`App` asset roots and do not edit generated files
**Scale/Scope**: One Runtime/Core resource manager and one Runtime/Rendering loader path

## Constitution Check

- Spec scope: Present, because this is a Runtime/Rendering behavior change.
- Generated-file boundaries: No `Runtime/*/Gen/` edits planned.
- Backend/platform validation: Unit-level path resolution is backend-independent; no backend correctness claim will be made.
- Product runtime viability: Editor/Game continue using existing resource manager flow; fallback remains for direct callers.
- Evidence path: Focused `NullusUnitTests` filter for shader cache path resolution, `App/Assets` fallback guard, and relevant shader compiler cache tests.

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

Tests/Unit/
└── ShaderCompilerTests.cpp
```

**Structure Decision**: Keep the fix in the existing shader resource loading path. The shader compiler remains responsible for persistence once the database path is configured.

**Unity Alignment**: Treat `App/Assets/Engine` like Unity built-in/prebuilt resources and treat `<project>/Library/ShaderCache` like Unity's project-local imported artifact/cache store.

## Complexity Tracking

No constitution violations.
