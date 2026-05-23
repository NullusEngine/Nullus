# Implementation Plan: Scene Camera Focus

**Branch**: `013-scene-camera-focus` | **Date**: 2026-05-04 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/013-scene-camera-focus/spec.md`

## Summary

Introduce an editor-only Scene View camera focus state shared by manual camera controls and ViewGizmo click rotation. The focus state stores a world-space point and camera-to-focus distance. ViewGizmo click rotations orbit this focus instead of a fixed length, middle-mouse panning converts screen-space mouse movement into world movement at the focus plane, and right-mouse/FPS movement updates or moves the focus so subsequent controls remain coherent.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Existing Nullus editor camera, ImGui, ImGuizmo, Runtime Math  
**Storage**: N/A, transient editor state only  
**Testing**: GoogleTest via `NullusUnitTests`; Editor Debug build for integration compile validation  
**Target Platform**: Windows Debug Editor for current validation  
**Project Type**: Desktop editor application  
**Performance Goals**: Camera updates remain per-frame O(1) and suitable for interactive 60 FPS editor navigation  
**Constraints**: Preserve generated-file boundaries; do not hand-edit `Runtime/*/Gen/`; keep Editor runnable; avoid changing runtime game cameras  
**Scale/Scope**: Scene View camera navigation only, integrated with existing ImGuizmo adapter and CameraController

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major changes**: PASS. This behavior spans editor camera control and ViewGizmo integration, so the feature uses `specs/013-scene-camera-focus/`.
- **Validation matches subsystem**: PASS. Unit tests will cover pure camera/focus math and adapter decisions; Editor Debug build will validate integration compile/link. Manual Scene View validation remains recommended for feel.
- **Generated code and backend boundaries**: PASS. No generated files or renderer backend contracts are in scope.
- **Incremental, verified delivery**: PASS. Implementation is split into math helpers, CameraController integration, ViewGizmo integration, and validation.
- **Product runtime preservation**: PASS. Editor must continue building and existing scene rendering paths remain untouched.

## Project Structure

### Documentation (this feature)

```text
specs/013-scene-camera-focus/
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
├── SceneCameraFocus.h        # new pure focus math adapter
├── SceneCameraFocus.cpp      # new pure focus math adapter
├── SceneViewImGuizmo.h
└── SceneViewImGuizmo.cpp

Project/Editor/Panels/
├── SceneView.h
└── SceneView.cpp

Tests/Unit/
├── CMakeLists.txt
├── ImGuizmoTransformAdapterTests.cpp
└── SceneCameraFocusTests.cpp # new unit tests
```

**Structure Decision**: Add a small `SceneCameraFocus` helper under `Project/Editor/Core` to keep focus math testable and avoid coupling it to ImGui input state. Store the active focus state in `SceneView` and pass it into `CameraController` and ViewGizmo integration.

## Phase 0 Research

See [research.md](research.md).

## Phase 1 Design

See [data-model.md](data-model.md) and [quickstart.md](quickstart.md). No external contracts are needed because this is internal editor behavior.

## Constitution Check Post-Design

- **Spec-first scope**: PASS. Plan, data model, quickstart, and tasks are in one feature directory.
- **Generated files**: PASS. Planned files avoid generated directories.
- **Validation**: PASS. Unit and Editor build commands are explicit.
- **Runtime viability**: PASS. Editor build is part of completion validation.

## Complexity Tracking

No constitution violations.
