# Implementation Plan: Fix DX12 MultiFramebuffer Barriers

**Branch**: `031-fix-dx12-multiframebuffer-barriers` | **Date**: 2026-05-23 | **Spec**: [spec.md](spec.md)

## Summary

Fix a DX12 device-removal bug caused by stale texture transition barriers in the threaded deferred path. The narrow approach is to keep the resource state tracker as the source of truth during post-pass visibility recording and suppress redundant non-UAV transitions that are already at the requested destination state.

## Technical Context

**Language/Version**: C++20 project code  
**Primary Dependencies**: Nullus Rendering RHI, FrameGraph, threaded rendering lifecycle  
**Storage**: N/A  
**Testing**: GoogleTest unit tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`  
**Target Platform**: Windows DX12 path, with no regression to other RHI backends  
**Project Type**: Desktop engine/editor runtime  
**Performance Goals**: No additional per-frame heavy synchronization; barrier filtering remains linear in requested barrier count  
**Constraints**: Do not hand-edit generated files; do not assume Vulkan/OpenGL correctness from DX12-only evidence; preserve product runtime viability  
**Scale/Scope**: One rendering bugfix touching threaded RHI barrier planning and focused tests

## Constitution Check

- Spec scope: Required because this is a rendering/RHI backend behavior fix under `Runtime/`.
- Generated boundaries: No files under `Runtime/*/Gen/` will be edited.
- Backend validation: Unit tests validate RHI planning; live DX12/RenderDoc evidence is preferred when available but not assumed for other backends.
- Product runtime: Fix must preserve editor/game threaded rendering paths and existing fallback behavior.
- Evidence path: Add failing GoogleTest regression, implement minimal fix, run targeted tests, then run required quality review.

## Project Structure

```text
specs/031-fix-dx12-multiframebuffer-barriers/
├── spec.md
├── plan.md
└── tasks.md

Runtime/Rendering/
├── Context/RhiThreadCoordinator.cpp
└── FrameGraph/FrameGraphExecutionContext.h

Tests/Unit/
└── ThreadedRenderingLifecycleTests.cpp
```

**Structure Decision**: Keep the fix in existing RHI/threaded rendering modules and add one focused regression test in the existing lifecycle test suite.

## Complexity Tracking

No constitution violations expected.
