# Phase 6 Occlusion CPU Validation

Date: 2026-06-04
Worktree: `.worktrees/large-scene-optimization`

## Scope

This evidence covers the CPU-side conservative occlusion history slice:

- `Runtime/Engine/Rendering/SceneOcclusion.h`
- `Runtime/Engine/Rendering/SceneOcclusion.cpp`
- `Runtime/Engine/Rendering/SceneVisibilityPipeline.h`
- `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- `Tests/Unit/SceneOcclusionTests.cpp`
- `Tests/Unit/SceneVisibilityPipelineTests.cpp`

GPU HZB pass construction, FrameGraph texture hazards, RHI capability gates, DX12 RenderDoc evidence, and backend-specific synchronization remain separate Phase 6 work.

## TDD Evidence

RED:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1

SceneOcclusionTests.cpp(6,10): error C1083:
无法打开包括文件: “Rendering/SceneOcclusion.h”: No such file or directory
```

GREEN:

```text
cmake --build build\windows --target NullusUnitTests --config Debug -- /m:1
Exit code: 0
```

Focused tests:

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneOcclusionTests.*

[==========] Running 7 tests from 1 test suite.
[  PASSED  ] 7 tests.
```

```text
build\windows\bin\Debug\NullusUnitTests.exe --gtest_filter=SceneVisibilityPipelineTests.*

[==========] Running 10 tests from 1 test suite.
[  PASSED  ] 10 tests.
```

## Covered Requirements

- Invalid or missing history keeps primitives conservatively visible.
- Valid recent history can cull an otherwise visible primitive.
- Primitive transform/bounds generation changes invalidate history.
- Representation id changes invalidate history.
- Projection, jitter, depth format, and viewport changes invalidate history.
- Backend unsupported and disabled feature states fall back conservatively.
- Depth-write-ineligible primitives do not use occlusion history.
- Ordinary CPU occlusion evaluation reports no synchronous readback, GPU fence wait, readback-buffer map block, or current-frame readback request.
- Visibility pipeline applies occlusion after visibility/representation selection and removes occluded primitives before command eligibility.
