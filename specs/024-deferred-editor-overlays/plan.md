# Implementation Plan: Deferred Editor Overlays

**Branch**: `024-deferred-editor-overlays` | **Date**: 2026-05-12 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/024-deferred-editor-overlays/spec.md`

## Summary

Scene View currently renders through `DebugSceneRenderer`, which inherits `ForwardSceneRenderer`; the rest of the game-facing default now uses `DeferredSceneRenderer`. This feature migrates the Scene View debug renderer to a deferred main scene path while preserving editor overlays and picking by adding a deferred package extension flow. Runtime deferred code will expose generic pass-input append/finalization hooks; editor-only grid, helper, outline, debug draw, and picking logic remains in `Project/Editor`.

## Technical Context

**Language/Version**: C++20 via the existing CMake/MSBuild toolchain
**Primary Dependencies**: Existing Nullus Runtime, Engine, Editor, FrameGraph, RHI, GoogleTest
**Storage**: N/A
**Testing**: `NullusUnitTests` with renderer package and editor debug renderer tests
**Target Platform**: Windows/DX12 phase 1 validation; no new backend claims
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: Preserve current deferred renderer pass count for non-editor views; add editor helper passes only when visible/enabled
**Constraints**: Do not edit `Runtime/*/Gen/`; keep runtime free of editor-only type dependencies; preserve Scene View product viability
**Scale/Scope**: One renderer migration spanning `Runtime/Engine/Rendering`, `Runtime/Rendering/FrameGraph`, `Project/Editor/Rendering`, and targeted unit tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This rendering pipeline/editor behavior change has a dedicated bundle under `specs/024-deferred-editor-overlays/`.
- **Validation matches subsystem**: PASS. Automated renderer-package tests will verify pass ordering and picking/readback contracts; final verification will run focused renderer tests and full `NullusUnitTests`. RenderDoc remains the next step for visual GPU validation if automated tests pass but editor visuals remain suspect.
- **Generated code and backend boundaries**: PASS. No hand edits under `Runtime/*/Gen/`; runtime deferred renderer exposes generic extension hooks and does not include editor-only pass classes.
- **Incremental verified delivery**: PASS. Tasks are split into tests, runtime extension hooks, editor renderer migration, and validation.
- **Product runtime preservation**: PASS. Game/GameView default deferred path remains intact; Scene View keeps overlay/picking behavior through explicit debug renderer support.

## Project Structure

### Documentation (this feature)

```text
specs/024-deferred-editor-overlays/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── deferred-editor-overlays.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/
├── Engine/Rendering/
│   ├── DeferredSceneRenderer.h
│   └── DeferredSceneRenderer.cpp
└── Rendering/FrameGraph/
    ├── SceneRenderGraphBuilderDeferred.h
    └── SceneRenderGraphBuilderDeferred.cpp

Project/
└── Editor/Rendering/
    ├── DebugSceneRenderer.h
    └── DebugSceneRenderer.cpp

Tests/
└── Unit/
    ├── RenderFrameworkContractTests.cpp
    ├── RendererFrameObjectBindingTests.cpp
    └── ThreadedRenderingLifecycleTests.cpp
```

**Structure Decision**: Runtime owns deferred scene graph/package primitives. Editor owns all overlay/debug/picking pass classes and composes them through generic deferred extension points.

## Phase 0: Research

### Decision 1: Migrate `DebugSceneRenderer` inheritance to `DeferredSceneRenderer`

**Decision**: Change `DebugSceneRenderer` to derive from `DeferredSceneRenderer` and override the prepared package builder to append editor passes after deferred lighting.

**Rationale**: This makes Scene View share the same main scene path as GameView/runtime while keeping editor helper code localized.

**Alternatives considered**:
- Keep `DebugSceneRenderer` on Forward and duplicate deferred features into Forward: rejected because it preserves the mismatch.
- Put editor passes directly into `DeferredSceneRenderer`: rejected because runtime would depend on editor concerns.

### Decision 2: Add generic deferred pass append helpers

**Decision**: Add a framegraph/package helper that compiles deferred GBuffer/Lighting pass inputs and then appends caller-provided pass metadata/inputs.

**Rationale**: This lets Editor add helpers without Runtime naming or including editor pass classes.

**Alternatives considered**:
- Rebuild all debug pass input generation in Editor by copying deferred internals: rejected due to duplicate pass binding/resource logic.
- Add virtual hooks for every editor pass to `DeferredSceneRenderer`: rejected because it hard-codes editor concepts into Runtime.

### Decision 3: Preserve picking readback registration in Editor

**Decision**: Keep picking pass creation and preferred readback texture registration in `DebugSceneRenderer` after package construction.

**Rationale**: Picking is an editor feature and already owns its readback texture selection logic.

## Phase 1: Design

### Data Model

See [data-model.md](data-model.md).

### Contracts

See [contracts/deferred-editor-overlays.md](contracts/deferred-editor-overlays.md).

### Quickstart

See [quickstart.md](quickstart.md).

### Constitution Check Re-evaluation

- Runtime/editor boundary remains clean: PASS.
- Validation path is explicit and test-first: PASS.
- Product runtime is preserved by keeping explicit forward renderer available and not changing GameView/runtime call sites beyond existing deferred default: PASS.

## Complexity Tracking

No constitution violations require justification.
