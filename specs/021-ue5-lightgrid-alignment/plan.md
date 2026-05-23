# Implementation Plan: UE5 LightGrid Alignment

**Branch**: `020-lightgrid-performance-toggle` | **Date**: 2026-05-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/021-ue5-lightgrid-alignment/spec.md`

## Summary

Align Nullus LightGrid with the supplied UE 4.27 forward local light grid source by replacing fixed grid dimensions with UE-style pixel-sized XY cells, UE logarithmic Z slicing and culling capacity, cell-owned GPU injection, staged linked-list culling support, and tests that lock the resulting data-flow for forward and deferred scene paths.

## Technical Context

**Language/Version**: C++20 and HLSL
**Primary Dependencies**: Nullus Runtime Rendering, Engine scene renderers, FrameGraph, RHI buffer/binding/pipeline APIs, ShaderManager, GoogleTest
**Storage**: Existing editor settings JSON only for the existing LightGrid enabled toggle; UE-aligned runtime defaults live in renderer settings/code
**Testing**: `NullusUnitTests`, shader compile/build validation, RenderDoc DX12 frame capture for runtime evidence
**Target Platform**: Windows DX12 for first validation; backend-neutral contracts where existing RHI support allows
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: Match UE-style culling work granularity; avoid CPU duplicate preparation already fixed by the previous LightGrid cache work; prevent unbounded buffer writes on dense light scenes
**Constraints**: Do not hand-edit generated files; do not claim cross-backend parity without evidence; use `F:\Epic Games\UE_4.27\Engine` as local reference but keep Nullus code original and adapted to Nullus RHI/material data
**Scale/Scope**: Runtime LightGrid settings/data layout, LightGrid compute shaders, frame-graph integration tests, forward/deferred graphics binding consumers

## Constitution Check

*GATE: Must pass before implementation.*

- Spec-first scope: PASS. This rendering pipeline/shader change is tracked under `specs/021-ue5-lightgrid-alignment/`.
- Validation matches subsystem: PASS. Plan requires targeted unit tests, shader/build validation, and DX12 RenderDoc evidence before visual correctness claims.
- Generated code boundary: PASS. No `Gen/` files will be hand-edited.
- Incremental verified delivery: PASS. Tasks split defaults/grid sizing, buffer contract, linked-list staging, shader consumption, and runtime evidence.
- Product runtime preservation: PASS. Existing LightGrid toggle remains; default behavior stays enabled; disabled mode remains explicit.

## Project Structure

### Documentation

```text
specs/021-ue5-lightgrid-alignment/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
└── tasks.md
```

### Source Code

```text
Runtime/Engine/Rendering/ClusteredShading.h
Runtime/Engine/Rendering/ClusteredShading.cpp
Runtime/Engine/Rendering/LightGridPrepass.h
Runtime/Engine/Rendering/LightGridPrepass.cpp
Runtime/Engine/Rendering/BaseSceneRenderer.cpp
Runtime/Rendering/Settings/DriverSettings.h
Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.*
App/Assets/Engine/Shaders/LightGridCommon.hlsli
App/Assets/Engine/Shaders/LightGridInjection.hlsl
App/Assets/Engine/Shaders/LightGridCompact.hlsl
App/Assets/Engine/Shaders/DeferredLighting.hlsl
App/Assets/Engine/Shaders/Standard.hlsl
App/Assets/Engine/Shaders/StandardPBR.hlsl
App/Assets/Engine/Shaders/Lambert.hlsl
Tests/Unit/LightingDataProviderTests.cpp
Tests/Unit/RenderFrameworkContractTests.cpp
Tests/Unit/FrameGraphSceneTargetsTests.cpp
Tests/Unit/ThreadedRenderingLifecycleTests.cpp
F:/Epic Games/UE_4.27/Engine/Source/Runtime/Renderer/Private/LightGridInjection.cpp (read-only reference)
F:/Epic Games/UE_4.27/Engine/Shaders/Private/LightGridInjection.usf (read-only reference)
F:/Epic Games/UE_4.27/Engine/Shaders/Private/LightGridCommon.ush (read-only reference)
F:/Epic Games/UE_4.27/Engine/Shaders/Private/ForwardLightingCommon.ush (read-only reference)
```

**Structure Decision**: Keep the existing `LightGridPrepass` and frame-graph boundaries. Replace internal settings and buffer contracts incrementally so forward/deferred scene renderers keep using a shared prepared LightGrid context.

## Complexity Tracking

No constitution violations.
