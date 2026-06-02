# Implementation Plan: Prepared Draw Cache Optimization

**Branch**: `[042-prepared-draw-cache-optimization]` | **Date**: 2026-05-31 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/042-prepared-draw-cache-optimization/spec.md`

## Summary

Optimize high draw-count deferred scenes by reducing repeated CPU render-preparation work before RHI command recording. The implemented slice adds frame-local GBuffer material resolve caching, persistent prepared recorded draw static-base caching, shader-support query caching, telemetry counters, trace-name ownership fixes, editor UI trace filtering, trace-scale dynamic instancing coverage, targeted DX12 bundle correctness guards for the existing in-render-pass child recording path, and build ownership cleanup needed to keep rendering resource-manager implementations in the rendering module.

This is aligned with the UE direction of separating reusable draw preparation state from per-draw/per-object data, but it is not a claim that full UE `FMeshDrawCommand` persistence for complete recorded command buffers is complete in this slice.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus Runtime/Engine renderer, Rendering RHI abstractions, GoogleTest unit tests  
**Storage**: In-memory frame-local renderer/provider caches plus persistent renderer static-base cache  
**Testing**: `NullusUnitTests` focused on deferred material cache, renderer object binding, stats, and editor FrameInfo formatting tests  
**Target Platform**: Windows DX12 runtime path, backend-agnostic preparation logic  
**Performance Goals**: Reduce CPU time spent in repeated deferred material resolve, static pipeline/material/mesh preparation, and shader reflection scans during high draw-count frames  
**Constraints**: Preserve existing persistent cache correctness, keep object binding capture per draw, do not change generated files  
**Scale/Scope**: Scenes with hundreds to thousands of visible opaque draws, plus CMake target ownership for the touched rendering/project/test modules

## Constitution Check

- Spec-first workflow is used because this is a rendering behavior change under `Runtime/`.
- Tests cover cache hit, cache invalidation, per-object isolation, and telemetry.
- Validation includes focused unit tests and the `NullusUnitTests` build target.
- No generated files under `Runtime/*/Gen/` are hand-edited.

## Project Structure

### Documentation

```text
specs/042-prepared-draw-cache-optimization/
├── spec.md
├── plan.md
└── tasks.md
```

### Source Code

```text
Runtime/Engine/Rendering/
├── DeferredSceneRenderer.h
├── DeferredSceneRenderer.cpp
├── EngineFrameObjectBindingProvider.h
└── EngineFrameObjectBindingProvider.cpp

Runtime/Rendering/Core/
├── ABaseRenderer.h
├── ABaseRenderer.cpp
├── RendererStats.h
└── RendererStats.cpp

Runtime/Rendering/Data/
└── FrameInfo.h

Runtime/Rendering/RHI/Backends/DX12/
├── DX12Command.h
└── DX12Command.cpp

Runtime/Rendering/Context/
└── RhiThreadCoordinator.cpp

Runtime/Rendering/Resources/
├── Material.h
└── Material.cpp

Runtime/Rendering/ResourceManagement/
├── MaterialManager.cpp
├── MeshManager.cpp
├── ShaderManager.cpp
└── TextureManager.cpp

Project/Editor/Panels/
├── FrameInfo.h
└── FrameInfoRendererStats.cpp

Project/
├── CMakeLists.txt
├── Editor/CMakeLists.txt
├── Game/CMakeLists.txt
└── Launcher/CMakeLists.txt

Tests/Unit/
├── DeferredSceneRendererMaterialCacheTests.cpp
├── RendererFrameObjectBindingTests.cpp
├── RendererStatsTests.cpp
└── PanelWindowHookTests.cpp
```

**Structure Decision**: Keep GBuffer resolve caching inside `DeferredSceneRenderer`, generic prepared static-base caching inside `ABaseRenderer`, shader-support query caching inside `EngineFrameObjectBindingProvider`, and telemetry in existing renderer stats/FrameInfo surfaces. Move rendering-dependent resource-manager implementations under `Runtime/Rendering/ResourceManagement` while preserving the existing `Core::ResourceManagement` API/namespace and headers.

## Design

### Frame-Local GBuffer Resolve Cache

`DeferredSceneRenderer` stores a per-frame map from source material instance id to a resolved GBuffer material plus `GBufferMaterialSyncStamp`. The stamp includes material parameter/render-state revisions and shader instance id/generation, so shader reloads or material edits miss safely. `BeginFrame()` clears the cache before scene parsing/capture.

### Persistent Prepared Recorded Draw Static-Base Cache

`ABaseRenderer` stores a persistent cache of reusable static prepared draw state:

- graphics pipeline
- material binding set
- active pass binding set
- RHI mesh
- fallback material GPU instance count

The cache intentionally excludes per-draw object binding sets and per-draw submit fields. `PopulatePreparedRecordedDrawFromStaticBase()` combines cached static state with the current command buffer, object binding capture output, instance count, vertex start, and vertex count.

The key includes material identity/revisions, binding revision, shader instance id/generation, mesh instance id/content revision, pass binding set, primitive mode, depth compare, pipeline state, and pipeline overrides. Stable objects can therefore reuse prepared static base state across frames. When a dirty material/mesh/shader/binding revision produces a new key for the same stable resource tuple, stale entries for that tuple are evicted before inserting the refreshed base.

### Shader Support Query Cache

`EngineFrameObjectBindingProvider` caches `ShaderSupportsIndexedObjectData()` results per frame by shader instance id and generation. The cache is cleared in `OnBeginFrame()` and misses when a shader generation changes.

### Telemetry

`RendererStats` and `FrameInfo` carry hit/miss counters for:

- GBuffer material resolve cache
- prepared recorded draw static-base cache

The editor FrameInfo panel formats the counters so runtime traces and UI snapshots can distinguish cache behavior from RHI submission behavior.

### Trace Reliability And Editor UI Isolation

Timeline `ProfilerEvent` now owns an inline event-name buffer instead of keeping pointers into recycled allocator pages. CPU/GPU/present events set names through `SetName()`, and the profiler window/export path reads through `GetName()`.

Trace export keeps the current full-trace behavior by default and adds an `Export Editor UI` toggle. Disabling it filters editor UI, profiler, ImGui, canvas, and UI bridge events while preserving scene draw preparation scopes such as `CaptureThreadedPreparedDraw`.

### Dynamic Instancing Coverage

`RenderScene` already merges compatible opaque static objects into dynamic instance groups. This slice adds regression coverage at the observed trace scale: 259 compatible objects must collapse to one submitted draw when object-data limits permit.

### Existing DX12 Child Recording Guards

The existing DX12 bundle-backed child command path is kept in place. This slice tightens correctness around that path by invalidating parent descriptor/root-table cache state after `ExecuteBundle()` and treating descriptor heap instability inside a child bundle range as an unsafe child-recording failure. When that instability is detected, the coordinator falls back to serial full-pass recording before the parent render pass opens so frame output is preserved without reusing unsafe child command state.

### Build Ownership Cleanup

Project executables are split into `EditorProject`, `GameProject`, and `LauncherProject` static libraries plus thin entry-point executables so tests can link project code without hardcoded cross-directory source inventories. Rendering-owned resource-manager implementations live under `Runtime/Rendering/ResourceManagement` and are picked up by the rendering module source discovery instead of being injected manually from `Runtime/Rendering/CMakeLists.txt`.

## Validation

- Focused deferred material cache tests.
- Focused renderer object binding/prepared draw cache tests.
- Focused renderer stats and FrameInfo formatting tests.
- Focused DX12 child bundle/descriptor-state contract tests.
- `cmake --build Build/windows --config Release --target NullusUnitTests -- /m:1`.
- Fresh high draw-count runtime trace remains required before claiming measured FPS or trace closure.

## Follow-Up Work

- Capture a fresh post-change trace and compare against `App/Win64_Release_Runtime_Shared/trace.json`.
- Consider persistent draw-command style caching closer to UE `FMeshDrawCommand` if scene/material stability data justifies it.
- Continue RHI-side analysis separately if traces show CPU time moved from preparation into command recording/submission.
