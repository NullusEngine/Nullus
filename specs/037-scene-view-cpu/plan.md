# Implementation Plan: Editor Scene View CPU Frame-Time Optimization

**Branch**: `037-scene-view-cpu` | **Date**: 2026-05-27 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/037-scene-view-cpu/spec.md`

## Summary

The provided trace shows editor main-thread frames averaging about 47.5 ms, with most time under Scene View rendering. The first implementation slice reduces redundant stable-size deferred renderer preparation by reusing already-created GBuffer targets and wrapped texture resources until dimensions or attachments change. A second slice makes trace export ignore incomplete events so future performance evidence is not distorted by invalid durations.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: CMake, GoogleTest, ImGui, Nullus Rendering/RHI runtime  
**Storage**: N/A  
**Testing**: `NullusUnitTests` focused GoogleTest filters, optional editor runtime trace validation  
**Target Platform**: Windows editor runtime first; backend evidence limited to the backend used by each validation run  
**Project Type**: Desktop editor and engine runtime  
**Performance Goals**: Reduce stable-size Scene View CPU preparation cost observed in `AView::RendererBeginFrame`; avoid negative-duration trace export events  
**Constraints**: Preserve Editor and Game runtime viability, do not hand-edit generated output, do not claim unvalidated backend/platform behavior  
**Scale/Scope**: Narrow renderer hot-path optimization plus trace export validity; no frame-graph architecture rewrite

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. Work is under `specs/037-scene-view-cpu/` before code changes.
- **Validation matches subsystem**: PASS. Automated renderer/profiler unit tests are required; editor runtime trace is planned where practical. RenderDoc is deferred because the first slice is CPU preparation, not visual GPU correctness.
- **Generated code/backend boundaries**: PASS. No files under `Runtime/*/Gen/` will be edited. RHI behavior remains behind existing renderer/RHI boundaries.
- **Incremental verified delivery**: PASS. Work is split into stable-size GBuffer reuse and trace export validity, each with focused tests.
- **Product runtime preservation**: PASS. Editor/Game entrypoints remain unchanged; backend claims are limited to validated runs.

## Project Structure

### Documentation (this feature)

```text
specs/037-scene-view-cpu/
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
Runtime/
├── Engine/Rendering/
│   ├── DeferredSceneRenderer.h
│   └── DeferredSceneRenderer.cpp
└── UI/ImGuiExtensions/TimelineProfiler/
    └── ProfilerWindow.cpp

Tests/
└── Unit/
    ├── DeferredSceneRendererMaterialCacheTests.cpp
    └── ProfilerDestinationTests.cpp
```

**Structure Decision**: Keep changes in the existing runtime modules and their established unit-test files. Do not add a new framework, command path, or backend-specific fork.

## Phase 0: Research

See [research.md](research.md).

## Phase 1: Design

See [data-model.md](data-model.md) and [quickstart.md](quickstart.md). No external contracts are introduced.

## Constitution Check (Post-Design)

- **Spec scope**: PASS. Design remains in one focused spec bundle.
- **Generated-file boundary**: PASS. No generated files are in scope.
- **Backend/platform validation**: PASS. Tests are backend-light where possible; runtime evidence will name the validated backend.
- **Runtime viability**: PASS. The optimization preserves current editor/game entrypoints and existing fallback paths.
- **Evidence path**: PASS. Failing tests before implementation, passing targeted tests after implementation, and optional trace comparison are defined.

## Complexity Tracking

No constitution violations require justification.
