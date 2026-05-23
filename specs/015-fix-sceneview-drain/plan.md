# Implementation Plan: Reduce Scene View Frame Stalls

**Branch**: `015-fix-sceneview-drain` | **Date**: 2026-05-05 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/015-fix-sceneview-drain/spec.md`

## Summary

Scene View currently requests immediate retired-frame readback for picking, which makes `AView` synchronously drain the threaded rendering lifecycle after each Scene View render. The fix is to keep Scene View on the delayed picking readback lifecycle that already exists, so normal rendering no longer forces a post-render drain while actor picking still uses the latest readable frame.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus Editor panels, rendering context, threaded rendering lifecycle, GoogleTest  
**Storage**: N/A  
**Testing**: `NullusUnitTests` via GoogleTest  
**Target Platform**: Windows editor DX12 validation path for this investigation  
**Project Type**: Desktop editor/runtime engine  
**Performance Goals**: Remove the steady-state Scene View main-thread drain associated with immediate picking readback; preserve interactive actor picking  
**Constraints**: Do not hand-edit generated files; do not redesign threaded rendering or RHI submission; retain resize safety for retired-frame resource consumers  
**Scale/Scope**: Narrow Editor panel/rendering behavior change in `Project/Editor/Panels` with targeted unit tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Spec-first scope: Pass. Behavior change is tracked in `specs/015-fix-sceneview-drain/`.
- Validation matches subsystem: Pass. Targeted editor/rendering lifecycle unit tests are required; runtime Tracy/RenderDoc evidence is recommended after code-level validation.
- Generated-file boundaries: Pass. No `Runtime/*/Gen/` files are touched.
- Incremental verified delivery: Pass. The change is a small policy adjustment covered by a failing test first.
- Product runtime preservation: Pass. Editor Scene View remains runnable; picking degrades to delayed readable-frame consumption rather than blocking the frame.

## Project Structure

### Documentation (this feature)

```text
specs/015-fix-sceneview-drain/
├── spec.md
├── plan.md
├── tasks.md
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
Project/Editor/Panels/
├── SceneView.cpp
└── ViewFrameLifecycle.h

Tests/Unit/
└── PanelWindowHookTests.cpp
```

**Structure Decision**: Keep the behavior in the existing panel lifecycle policy header and connect Scene View to that policy. This keeps the fix small, reviewable, and independently testable.

## Research

### Decision: Use delayed picking readback for Scene View by default

**Rationale**: `PickingReadbackLifecycle` already supports pending/readable frames. Threaded picking submits a frame and promotes it later when readback is available. Scene View's immediate-readback flag bypasses that design by forcing synchronous retirement every frame.

**Alternatives considered**:

- Disable picking pass entirely: improves FPS but removes selection behavior.
- Change threaded rendering drain logic globally: broader risk across RHI submission, UI, resize, and shutdown paths.
- Only drain on click: still risks click-time stalls and ignores existing delayed readback support.

## Data Model

- **Scene View immediate readback policy**: Boolean policy that determines whether Scene View requests same-frame readback. Default is false.
- **Retirement-aware resize policy**: Existing behavior that may drain before resizing views that consume retired frame resources. This remains unchanged.
- **Picking readback lifecycle**: Existing pending/readable frame state machine. This remains the source of truth for picking availability.

## Contracts

No external API or file format contracts are introduced.

## Quickstart

1. Build the unit test target:
   `cmake --build Build --target NullusUnitTests --config Debug`
2. Run the targeted policy and picking tests:
   `Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.*:PickingReadbackLifecycleTests.*`
3. Optional runtime validation: run Editor with threaded rendering and inspect Tracy for absence or reduction of steady-state Scene View `DrainPendingRenderFrameBuildsSynchronously` stalls.

## Constitution Check Post-Design

- Spec-first scope: Pass.
- Validation matches subsystem: Pass; exact unit commands are listed and runtime profiler evidence is identified as optional follow-up.
- Generated-file boundaries: Pass.
- Incremental verified delivery: Pass.
- Product runtime preservation: Pass.

## Complexity Tracking

No constitution violations or additional complexity exceptions.
