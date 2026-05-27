# Implementation Plan: Loaded Primitive Rendering

**Branch**: `[fix-loaded-primitive-rendering]` | **Date**: 2026-05-27 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/fix-loaded-primitive-rendering/spec.md`

## Summary

Saved primitive cubes currently restore with a built-in mesh reference and an empty material list. The render scene must still produce a visible draw by resolving the built-in mesh and applying an existing valid fallback material without breaking explicit material references or deferred retry behavior.

## Technical Context

**Language/Version**: C++20 via existing Nullus CMake/MSVC setup  
**Primary Dependencies**: Existing Nullus engine runtime, scene object graph serialization, resource managers, render scene cache  
**Storage**: Object graph `.scene` files  
**Testing**: `NullusUnitTests` targeted GoogleTest filters  
**Target Platform**: Windows editor/runtime path for this fix; no cross-platform behavior claims beyond CPU-side tests  
**Project Type**: Desktop engine/editor  
**Performance Goals**: No per-frame blocking asset load in the main scene parse path beyond existing accepted fallback behavior  
**Constraints**: Do not hand-edit generated files under `Runtime/*/Gen/`; preserve deferred resource retry behavior  
**Scale/Scope**: One saved primitive cube regression path plus existing render scene fallback contracts

## Constitution Check

- Spec scope: Required because this is runtime/rendering behavior under `Runtime/`; this bundle records the change.
- Generated boundaries: No generated files will be edited.
- Backend/platform validation: Primary evidence is CPU-side draw generation through unit tests. RenderDoc follow-up is only needed if a GPU-side discrepancy remains.
- Product runtime viability: Editor/Game runtime behavior should remain compatible; no backend forks or parallel workflow introduced.
- Evidence path: Targeted `NullusUnitTests` filters for render scene cache and scene object graph serialization.

## Project Structure

### Documentation (this feature)

```text
specs/fix-loaded-primitive-rendering/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Engine/Rendering/
├── BaseSceneRenderer.cpp
└── RenderScene.cpp

Runtime/Engine/Components/
└── MeshFilter.cpp

Tests/Unit/
├── RenderSceneCacheTests.cpp
└── SceneObjectGraphSerializationTests.cpp
```

**Structure Decision**: Keep the change in existing runtime render/resource paths and existing unit test suites.

## Complexity Tracking

No constitution violations expected.
