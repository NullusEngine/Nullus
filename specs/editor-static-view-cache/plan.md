# Implementation Plan: Editor Static View Cache

**Branch**: `editor-static-view-cache` | **Date**: 2026-06-09 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/editor-static-view-cache/spec.md`

## Summary

Add a conservative static-frame cache at the editor view layer. `AView` owns the reusable cache gate and cache key plumbing, while `SceneView` opts in and contributes editor-specific dirty inputs. Cache hits return before renderer frame setup so unchanged static frames skip FrameGraph/RHI submission work.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus editor panels, engine scene system, renderer stats, GoogleTest
**Storage**: N/A
**Testing**: `NullusUnitTests`
**Target Platform**: Windows editor runtime, backend-aware explicit RHI paths
**Project Type**: Desktop editor / rendering engine
**Performance Goals**: Reduce idle/static Scene View CPU and RHI submission cost by skipping full renderer frames when content is unchanged
**Constraints**: Preserve picking/readback/validation behavior; do not enable caching for Game View or Asset View in this slice
**Scale/Scope**: Scene View large-scene editor idle/static frames

## Constitution Check

- Spec-first: satisfied by this focused spec bundle for the editor static view cache slice.
- Rendering validation: unit tests cover policy; runtime trace/RenderDoc evidence remains the follow-up validation for real DX12 performance.
- Generated code: no generated files edited.
- RHI safety: no global RHI thread changes; optimization is contained above renderer submission.

## Project Structure

### Documentation

```text
specs/editor-static-view-cache/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Project/Editor/Panels/
├── AView.h
├── AView.cpp
├── SceneView.h
└── SceneView.cpp

Runtime/Engine/SceneSystem/
├── Scene.h
├── Scene.cpp
└── SceneManager.cpp

Tests/Unit/
├── PanelWindowHookTests.cpp
└── SceneObjectGraphSerializationTests.cpp
```

**Structure Decision**: The cache lives in `AView` because all editor render panels share the renderer submission lifecycle. `SceneView` is the only opt-in user in this slice because its dirty inputs are well understood and it is the measured large-scene bottleneck. Scene render-content revision lives in `Scene` and is advanced by `SceneManager` dirty marks because the renderer needs a stable scene-level visual invalidation source.

## Complexity Tracking

No constitution violations.
