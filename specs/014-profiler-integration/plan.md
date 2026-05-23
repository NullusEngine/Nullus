# Implementation Plan: Unified Profiler Integration

**Branch**: `014-profiler-integration` | **Date**: 2026-05-05 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/014-profiler-integration/spec.md`

## Summary

Introduce a low-level Nullus profiler facade that lets engine/editor code mark one scoped event and route it to both Tracy and TimelineProfiler. Tracy remains the external deep profiling destination, while TimelineProfiler is embedded as a dockable editor Profiler window with explicit unavailable/empty states. The first implementation slice focuses on shared CPU scope events, optional destination enablement, automatic function-name labels, and editor integration that preserves existing runtime and test workflows.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Existing Nullus runtime/editor modules, ImGui, Tracy, TimelineProfiler as a UI-owned ImGui extension
**Storage**: N/A for v1; profiling data is transient per process/session
**Testing**: GoogleTest via `NullusUnitTests`; focused Editor Debug build and manual editor profiler verification
**Target Platform**: Windows Debug Editor for initial validation; design must degrade explicitly on unsupported platforms/backends
**Project Type**: Desktop game engine runtime and editor tooling
**Performance Goals**: Disabled profiling markers compile or execute with negligible overhead; enabled CPU scopes avoid avoidable allocations in hot paths and remain suitable for per-frame editor/runtime instrumentation; GPU scopes are limited to command-recording boundaries where timestamp overhead is intentional
**Constraints**: Do not hand-edit `Runtime/*/Gen/`; keep Editor and Game runnable; do not introduce a parallel build/test workflow; profiler destinations must be independently optional; GPU timeline support is reported only for validated DX12 TimelineProfiler capability and degrades explicitly elsewhere
**Scale/Scope**: Runtime-wide scoped CPU instrumentation facade, DX12-capable shared GPU scope surface, render/RHI thread markers, two profiler destinations, one dockable editor panel, unit tests, documentation, and focused manual validation

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Pre-Design Check

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | Major runtime/editor/tooling integration uses `specs/014-profiler-integration/`. |
| II. Validation Matches Subsystem | PASS | Unit tests cover shared instrumentation semantics; Editor build/manual run validates panel integration; backend-specific GPU claims are scoped to explicit evidence. |
| III. Generated Code / Backend Boundaries | PASS | Plan avoids `Runtime/*/Gen/`; GPU/backend support is represented as capability/degraded state instead of backend assumptions. |
| IV. Incremental Delivery | PASS | Work is staged as facade/tests, build toggles, destination adapters, editor panel, docs, and validation. |
| V. Product Runtime Preservation | PASS | Profiling can be disabled; Editor/Game remain runnable without profiler viewer processes. |

## Project Structure

### Documentation (this feature)

```text
specs/014-profiler-integration/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── profiler-instrumentation-contract.md
│   └── editor-profiler-window-contract.md
└── tasks.md                 # Phase 2 output from /speckit.tasks
```

### Source Code (repository root)

```text
Runtime/Base/
├── Profiling/
│   ├── Profiler.h           # new public facade, macros/helpers, destination state
│   ├── Profiler.cpp         # new routing/session implementation
│   ├── ProfilerScope.h      # new RAII scope event type
│   ├── ProfilerScope.cpp
│   ├── TracyProfiler.h      # new Tracy destination wrapper
│   └── TracyProfiler.cpp
└── CMakeLists.txt           # link/include profiler dependencies conditionally

Runtime/UI/
├── ImGuiExtensions/
│   ├── CMakeLists.txt       # central Dear ImGui extension registry
│   ├── cmake/RegisterImGuiExtension.cmake
│   ├── ImGuizmo/            # source-level ImGui extension, built into NLS_UI
│   └── TimelineProfiler/    # source-level ImGui extension, built into NLS_UI
└── Profiling/
    ├── TimelineProfilerSink.h
    └── TimelineProfilerSink.cpp # CPU forwarding plus DX12 TimelineProfiler GPU bridge

Runtime/Rendering/
├── Context/
│   ├── Driver.cpp           # render/RHI worker thread naming and loop scopes
│   ├── RenderThreadCoordinator.cpp # render-thread publication/build scopes
│   └── RhiThreadCoordinator.cpp # standalone/threaded frame, pass recording, submit, present scopes
├── Core/
│   └── ABaseRenderer.cpp    # renderer frame and publish scopes
└── RHI/
    ├── Core/
    │   └── RHICommand.h     # backend-neutral GPU profiler virtual hooks
    └── Backends/DX12/
        ├── DX12Command.*    # TimelineProfiler GPU begin/end bridge for command lists
        ├── DX12Queue.*      # TimelineProfiler ExecuteCommandLists bridge
        └── DX12ExplicitDeviceFactory.cpp # TimelineProfiler GPU initialization from native DX12 handles

Project/Editor/
├── Core/
│   ├── Editor.cpp           # create/register Profiler panel and update profiler session
│   └── PanelsManager.h      # no planned ownership model change; referenced integration point
├── Panels/
│   ├── MenuBar.cpp          # add Profiler window toggle under existing window/debug menu path if needed
│   ├── EditorTopBar.cpp     # existing window registration path may expose the new panel automatically
│   ├── ProfilerPanel.h      # new dockable TimelineProfiler panel
│   └── ProfilerPanel.cpp
└── CMakeLists.txt           # add destination include/link data when needed

ThirdParty/
└── Tracy/                   # vendored or submodule dependency

Tests/Unit/
├── CMakeLists.txt
├── ProfilerScopeTests.cpp   # new shared instrumentation tests
└── ProfilerDestinationTests.cpp # new destination enablement/fallback tests with test doubles

Docs/
└── Profiling.md             # new developer usage and validation guide
```

**Structure Decision**: Place the shared facade in `Runtime/Base/Profiling` so Runtime, Editor, Game, and future render code can use one instrumentation surface without depending on editor UI. Keep Tracy behind a Base destination wrapper because it is an external profiler dependency. Keep TimelineProfiler source inside `Runtime/UI/ImGuiExtensions` and expose its destination wrapper from `Runtime/UI/Profiling`, because it is an in-editor Dear ImGui extension rather than a standalone third-party library. Add the editor visualization only in `Project/Editor/Panels`, using existing panel registration and menu/window toggle conventions. GPU profiling uses backend-neutral hooks on `RHICommandBuffer`; the DX12 backend is the first implementation because TimelineProfiler's GPU timestamp implementation is DX12-specific.

## Phase 0 Research

See [research.md](research.md).

## Phase 1 Design

See [data-model.md](data-model.md), [quickstart.md](quickstart.md), and contracts:

- [profiler-instrumentation-contract.md](contracts/profiler-instrumentation-contract.md)
- [editor-profiler-window-contract.md](contracts/editor-profiler-window-contract.md)

## Constitution Check Post-Design

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | `plan.md`, `research.md`, `data-model.md`, `contracts/`, and `quickstart.md` are kept in one feature bundle. |
| II. Validation Matches Subsystem | PASS | Quickstart defines unit tests, Editor build, and manual editor/Tracy verification; GPU support remains capability-scoped. |
| III. Generated Code / Backend Boundaries | PASS | Planned files avoid generated directories; backend-specific GPU behavior is an availability state, not an assumed contract. |
| IV. Incremental Delivery | PASS | Design supports test-first facade work before third-party/editor integration and allows each destination to land independently. |
| V. Product Runtime Preservation | PASS | Disabled/unavailable destinations are explicit states; normal Editor/Game runs do not require profiler viewers. |

## Complexity Tracking

No constitution violations.
