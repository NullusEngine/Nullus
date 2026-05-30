# Implementation Plan: Scene View Camera State Machine

**Branch**: `040-scene-view-camera-state-machine` | **Date**: 2026-05-30 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `specs/040-scene-view-camera-state-machine/spec.md`

## Summary

Replace Scene View camera control's per-frame cursor mutation model with an explicit interaction state machine. The new design centralizes navigation state transitions, cursor ownership, mouse capture, and reset behavior for neutral, blocked, fly, pan, and orbit interactions. `SceneView` remains responsible for high-level editor blocking conditions, while `CameraController` and a new state-machine helper own navigation state transitions and event-driven cursor changes.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: `Project/Editor/Core/CameraController.*`, `Project/Editor/Panels/SceneView.*`, `Runtime/UI/UIManager.*`, `Runtime/Platform/Windowing/*`, ImGui input state  
**Storage**: N/A, transient editor interaction state only  
**Testing**: GoogleTest via `NullusUnitTests`; focused Editor/Scene View contract tests; Debug build compile validation  
**Target Platform**: Windows Debug Editor for current validation evidence  
**Project Type**: Desktop editor application inside a multi-backend engine  
**Performance Goals**: Keep Scene View camera interaction O(1) per frame and suitable for interactive 60 FPS editor navigation  
**Constraints**: Do not hand-edit generated files; preserve current camera gestures; preserve Editor runtime viability; avoid backend-specific cursor assumptions; keep cursor writes event-driven rather than per-frame churn  
**Scale/Scope**: `SceneView`, `CameraController`, and related editor-side input/cursor ownership only; no gameplay camera changes; no render backend behavior changes beyond existing UI cursor coordination

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major changes**: PASS. This is an editor behavior and architecture change under `Project/`, so the spec bundle under `specs/040-scene-view-camera-state-machine/` is required and present.
- **Validation matches subsystem**: PASS. Targeted `NullusUnitTests` coverage will validate pure state logic and Scene View gating rules; Editor Debug build will validate integration compile and linkage. Manual Scene View verification remains part of completion evidence because cursor feel is runtime-facing editor behavior.
- **Generated code and backend boundaries**: PASS. No `Runtime/*/Gen/` files are in scope. The change coordinates with existing `UIManager` and windowing APIs without altering renderer/backend capability contracts.
- **Incremental, verified delivery**: PASS. Work can be split into state model extraction, `CameraController` adoption, `SceneView` integration, and regression validation.
- **Product runtime preservation**: PASS. `Editor` must remain runnable throughout; no temporary product-breakage is planned or required.

## Project Structure

### Documentation (this feature)

```text
specs/040-scene-view-camera-state-machine/
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
Project/Editor/Core/
├── CameraController.h
├── CameraController.cpp
├── SceneViewCameraInteractionStateMachine.h   # new pure state/transition helper
└── SceneViewCameraInteractionStateMachine.cpp # new pure state/transition helper

Project/Editor/Panels/
├── AViewControllable.cpp
├── SceneView.h
├── SceneView.cpp
└── SceneViewPickingPolicy.h

Runtime/UI/
├── UIManager.h
└── UIManager.cpp

Tests/Unit/
├── CameraControllerInputTests.cpp
├── PanelWindowHookTests.cpp
└── SceneViewCameraInteractionStateMachineTests.cpp # new
```

**Structure Decision**: Add a dedicated `SceneViewCameraInteractionStateMachine` helper under `Project/Editor/Core` so state transitions and cursor/capture side effects are testable without requiring full editor panel runtime. Keep `SceneView` responsible for editor-wide blocking conditions, and keep `CameraController` responsible for camera movement math and applying the currently active interaction behavior.

## Phase 0 Research

See [research.md](./research.md).

## Phase 1 Design

See [data-model.md](./data-model.md) and [quickstart.md](./quickstart.md). No external API contracts are required because this feature changes internal editor interaction behavior only.

## Constitution Check Post-Design

- **Spec-first scope**: PASS. Design artifacts remain within a single feature bundle.
- **Generated files**: PASS. Planned file set stays outside generated directories.
- **Validation path**: PASS. Pure state-machine tests, Scene View policy tests, and Editor Debug build validation are explicit.
- **Runtime viability**: PASS. Integration is staged so existing navigation remains available while cursor ownership moves into explicit state transitions.

## Complexity Tracking

No constitution violations.
