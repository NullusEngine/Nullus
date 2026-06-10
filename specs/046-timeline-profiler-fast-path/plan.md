# Implementation Plan: TimelineProfiler GPU Fast Path

**Branch**: `046-timeline-profiler-fast-path` | **Date**: 2026-06-11 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `/specs/046-timeline-profiler-fast-path/spec.md`

## Summary

Reduce TimelineProfiler start-of-frame CPU overhead by skipping GPU timestamp readback submission and fence waits for frames that contain no GPU query ranges. Keep the current conservative behavior for frames with pending command-list query data or open queue events, and preserve GPU event publication when query pairs exist.

The technical approach is to add a small, testable decision layer to the TimelineProfiler GPU frame lifecycle, then apply it inside the DX12 GPU profiler tick path so empty frames are marked complete without submitting no-op resolve command lists.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus runtime profiling facade, TimelineProfiler integration, Direct3D 12 timestamp query support on Windows  
**Storage**: N/A  
**Testing**: GoogleTest via `NullusUnitTests`; targeted tests in `Tests/Unit/TimelineProfilerGpuLifecycleTests.cpp`  
**Target Platform**: Windows DX12 for GPU timeline validation; non-DX12 builds must continue compiling with existing feature guards  
**Project Type**: Desktop editor/game runtime profiling subsystem  
**Performance Goals**: Empty GPU profiler frames should avoid multi-millisecond start-of-frame waits; validated trace target is less than 0.25 ms attributable empty-GPU-profiler overhead  
**Constraints**: Do not hand-edit generated files; do not claim Vulkan/macOS/Linux GPU timeline behavior from Windows DX12 evidence; preserve editor/game runtime viability  
**Scale/Scope**: Narrow TimelineProfiler GPU frame lifecycle optimization; no new GPU profiling UI, no backend expansion

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This Runtime/UI and DX12 profiling behavior change uses `specs/046-timeline-profiler-fast-path/`.
- **Validation matches subsystem**: PASS. Plan uses targeted unit tests for lifecycle decisions and a Windows DX12 profiler trace/manual validation for runtime evidence.
- **Generated code/backend boundaries**: PASS. No files under `Runtime/*/Gen/` are touched. DX12-specific behavior stays inside existing TimelineProfiler/DX12 integration boundaries.
- **Incremental verified delivery**: PASS. Tests are written first for empty/non-empty/mixed GPU frame lifecycle decisions, then implementation follows.
- **Product runtime preservation**: PASS. TimelineProfiler disabled path and normal editor runtime remain unchanged; validation includes recording enabled/disabled smoke evidence where practical.

## Project Structure

### Documentation (this feature)

```text
specs/046-timeline-profiler-fast-path/
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
Runtime/UI/ImGuiExtensions/TimelineProfiler/
├── Profiler.h
└── Profiler.cpp

Runtime/UI/Profiling/
└── TimelineProfilerSink.cpp

Tests/Unit/
└── TimelineProfilerGpuLifecycleTests.cpp
```

**Structure Decision**: Reuse the existing TimelineProfiler implementation files and lifecycle unit test file. Keep the behavior decision in `Profiler.h` where existing constexpr lifecycle helpers already live, and keep DX12 command submission changes inside `Profiler.cpp`.

## Phase 0: Research

See [research.md](./research.md).

## Phase 1: Design

See [data-model.md](./data-model.md) and [quickstart.md](./quickstart.md). No external contracts are required because this is an internal profiler lifecycle change with no public API surface.

## Constitution Check (Post-Design)

- **Spec-first major change**: PASS. Spec, plan, research, data model, quickstart, and tasks will remain in one feature bundle.
- **Validation matches subsystem**: PASS. Unit tests cover lifecycle decisions; runtime validation is explicitly scoped to Windows DX12 trace evidence.
- **Generated code/backend boundaries**: PASS. Planned edits avoid generated files and do not split renderer architecture.
- **Incremental verified delivery**: PASS. Tasks separate test additions, minimal implementation, instrumentation, and validation.
- **Product runtime preservation**: PASS. Disabled recording and no-GPU-scope paths are included in acceptance criteria and validation notes.

## Complexity Tracking

No constitution violations require justification.
