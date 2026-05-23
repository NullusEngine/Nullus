# Implementation Plan: UE4.27 Deferred Lighting Alignment

**Branch**: `025-ue427-deferred-alignment` | **Date**: 2026-05-12 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/025-ue427-deferred-alignment/spec.md`

## Summary

Align Nullus deferred rendering with the first visible stage pattern of UE4.27: keep GBuffer generation, initialize/composite SceneColor through deferred lighting, consume the full scene light list for ambient/directional/point/spot contribution, and keep editor overlay/debug passes after the main scene. This phase fixes the current ambient-floor-only output without implementing UE's full tiled/shadow/reflection/translucency stack.

## Technical Context

**Language/Version**: C++20-style engine code and HLSL shaders
**Primary Dependencies**: Nullus RHI, FrameGraph, Material/ShaderType metadata, LightGridPrepass, RenderDoc tooling
**Storage**: N/A
**Testing**: GoogleTest `NullusUnitTests`, CMake build targets, RenderDoc DX12 capture analysis
**Target Platform**: Windows editor with DX12 validation; other backends are not claimed by this phase
**Project Type**: Desktop game/editor engine runtime
**Performance Goals**: Restore visible lighting with one fullscreen deferred lighting pass; defer tiled optimization for a later phase
**Constraints**: Do not hand-edit generated files; preserve editor/game runtime viability; do not remove existing forward/clustered light-grid support
**Scale/Scope**: One phase-one deferred renderer alignment across shader, light data contract, frame graph labels, and focused tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major changes**: PASS. Rendering pipeline/shader/frame-graph behavior is covered by this spec bundle.
- **Validation matches subsystem**: PASS. Plan includes targeted unit/contract tests, build commands, and RenderDoc DX12 capture evidence.
- **Generated/backend boundaries**: PASS. No files under `Runtime/*/Gen/` will be edited; DX12 validation is not generalized to other backends.
- **Incremental verified delivery**: PASS. Work is split into tests, shader contract, runtime data path, and capture validation.
- **Product runtime preservation**: PASS. Editor remains the validation product; degraded behavior for missing resources remains skip/fallback rather than crash.

## Project Structure

### Documentation (this feature)

```text
specs/025-ue427-deferred-alignment/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
App/Assets/Engine/Shaders/
├── DeferredLighting.hlsl
└── LightGridCommon.hlsli

Runtime/Engine/Rendering/
├── LightGridPrepass.cpp
└── LightGridPrepass.h

Runtime/Rendering/FrameGraph/
├── SceneRenderGraphBuilderDeferred.cpp
└── SceneRenderGraphBuilderDeferred.h

Tests/Unit/
├── LightingDataProviderTests.cpp
├── RenderFrameworkContractTests.cpp
└── EditorRenderPathContractTests.cpp
```

**Structure Decision**: Keep the existing deferred fullscreen lighting pass for phase one, but change its shader contract so it can loop over the packed scene light list directly. Keep light-grid buffers bound for forward compatibility and future tiled optimization.

## Phase 0: Research

See [research.md](research.md).

## Phase 1: Design & Contracts

See [data-model.md](data-model.md) and [quickstart.md](quickstart.md). No external API contracts are created because this is an internal renderer pipeline change.

## Constitution Check Post-Design

- **Spec-first major changes**: PASS. Design artifacts remain inside `specs/025-ue427-deferred-alignment/`.
- **Validation matches subsystem**: PASS. Quickstart contains build/test/RenderDoc commands and capture expectations.
- **Generated/backend boundaries**: PASS. Planned edits stay outside generated folders and state DX12 as the verified backend.
- **Incremental verified delivery**: PASS. Tasks are independently reviewable and start with failing tests/contracts.
- **Product runtime preservation**: PASS. The editor overlay/debug path remains appended after deferred lighting.

## Complexity Tracking

No constitution violations are required.
