# Implementation Plan: ImGuizmo Transform Gizmo

**Branch**: `012-imguizmo-transform` | **Date**: 2026-05-04 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/012-imguizmo-transform/spec.md`

## Summary

Replace the editor Scene View's custom model-rendered transform axes with an ImGuizmo-based Dear ImGui viewport gizmo while preserving selected-actor move, rotate, scale, W/E/R mode switching, snapping intent, actor picking, and camera control behavior. The implementation will vendor ImGuizmo beside the existing Dear ImGui dependency, add a focused Scene View gizmo adapter, draw it as an ImGui overlay aligned to the rendered Scene View image, and stop drawing/picking the old custom gizmo by default.

## Technical Context

**Language/Version**: C++20 with CMake 3.18 project configuration  
**Primary Dependencies**: Existing `ImGui` static target under `ThirdParty/ImGui`; new vendored ImGuizmo source under `ThirdParty/ImGuizmo`  
**Storage**: N/A; feature mutates existing in-memory actor transforms and uses existing scene serialization indirectly  
**Testing**: `cmake --build Build --config Debug --target Editor NullusUnitTests -- /m:1`, `ctest --test-dir Build -C Debug --output-on-failure`, plus focused manual Scene View verification  
**Target Platform**: Nullus Editor desktop builds; validate on the active local backend first and document backend/platform limits  
**Project Type**: Desktop editor application inside a multi-backend engine repository  
**Performance Goals**: Gizmo overlay should add no visible frame hitch during normal Scene View interaction and should remain responsive during continuous drag  
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; do not alter game/runtime rendering behavior outside editor viewport overlay; preserve runnable Editor and Game products; avoid claiming untested backend parity  
**Scale/Scope**: Single Scene View transform tool replacement for one selected actor at a time; no runtime gizmos, multi-selection manipulation, pivot mode, or new local/world mode UI in this change

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This is an editor behavior and rendering-overlay change, tracked in `specs/012-imguizmo-transform/`.
- **Validation matches subsystem**: PASS. Plan requires targeted build/tests plus manual Scene View runtime verification; RenderDoc is not mandatory because the new manipulator is an ImGui editor overlay, but backend-specific claims remain limited to validated runs.
- **Generated code/backend boundaries**: PASS. No generated files under `Runtime/*/Gen/` are touched. Backend behavior is coordinated through existing ImGui/UI paths, not renderer-specific forks.
- **Incremental verified delivery**: PASS. The plan separates dependency vendoring, adapter behavior, Scene View integration, old gizmo disablement, tests, and verification.
- **Product runtime preservation**: PASS. Editor remains the target executable; Game/runtime rendering should be unaffected because the integration is scoped to editor Scene View UI.

## Project Structure

### Documentation (this feature)

```text
specs/012-imguizmo-transform/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── scene-view-gizmo.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
ThirdParty/
├── CMakeLists.txt
├── imgui.cmake
└── ImGuizmo/
    ├── ImGuizmo.cpp
    ├── ImGuizmo.h
    ├── LICENSE
    └── README.md

Project/Editor/
├── CMakeLists.txt
├── Panels/
│   ├── AView.cpp
│   ├── AView.h
│   ├── SceneView.cpp
│   └── SceneView.h
└── Core/
    ├── GizmoBehaviour.cpp
    └── GizmoBehaviour.h

Project/Editor/Rendering/
├── DebugSceneRenderer.cpp
├── DebugSceneRenderer.h
├── GizmoRenderer.cpp
├── GizmoRenderer.h
├── PickingRenderPass.cpp
└── PickingRenderPass.h

Tests/Unit/
├── CMakeLists.txt
└── ImGuizmoTransformAdapterTests.cpp
```

**Structure Decision**: Vendor ImGuizmo in `ThirdParty` because Nullus already vendors Dear ImGui and builds it through a local CMake target. Keep new editor logic close to `SceneView`, with a small adapter or helper in `Project/Editor/Core` if matrix conversion/manipulation needs unit coverage. Existing renderer-side gizmo files remain available until tasks decide whether to remove or leave them unused; generated directories remain untouched.

## Complexity Tracking

No constitution violations require justification.

## Phase 0 Research Summary

See [research.md](./research.md). Key decisions:

- Vendor upstream ImGuizmo source beside Dear ImGui instead of introducing package-manager dependency.
- Render ImGuizmo in the Scene View ImGui pass after the render target image is drawn.
- Use ImGuizmo active/hover state to suppress old actor-picking side effects while dragging or hovering the gizmo.
- Update selected actor transforms through the existing transform component so inspector, serialization, and rendering observe the same state.

## Phase 1 Design Summary

See [data-model.md](./data-model.md), [contracts/scene-view-gizmo.md](./contracts/scene-view-gizmo.md), and [quickstart.md](./quickstart.md).

Post-design constitution check:

- **Spec-first major change**: PASS. Plan artifacts remain in the same `012-imguizmo-transform` bundle.
- **Validation matches subsystem**: PASS. Quickstart includes build/test commands plus manual Scene View verification for editor interaction.
- **Generated code/backend boundaries**: PASS. Design uses existing editor UI/render target boundaries and does not touch generated output.
- **Incremental verified delivery**: PASS. Design identifies separable dependency, adapter, integration, cleanup, and verification phases for tasks generation.
- **Product runtime preservation**: PASS. Runtime game rendering is explicitly out of scope and should be checked by building affected products without moving game renderer paths.
