# Implementation Plan: Nullus Unity-Style Native JobSystem

**Branch**: `032-job-system` | **Date**: 2026-05-24 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `D:/VSProject/Nullus/specs/032-job-system/spec.md`

## Summary

Introduce a Nullus-native, Unity 2018-style JobSystem as a runtime foundation under `Runtime/Base/Jobs`. The system provides deterministic job handles/fences, dependency-ordered scheduling, typed native `IJob`/`IJobParallelFor` adapters, batched and parallel-for work, work-stealing range helpers, background jobs with main-thread continuations, diagnostics/safety/profiling hooks, binding-ready native contracts, and staged migration for existing ad hoc threaded consumers. Unity's source tree is used as a semantic reference; Nullus keeps its own module layout, tests, profiler integration, and C++20 implementation, and does not claim managed C# `IJob` or `NativeArray` support in this slice.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus `NLS_Base`, existing `Profiling/Profiler`, GoogleTest/CTest, standard C++ threading primitives
**Storage**: Runtime-only transient scheduler state; no persistent storage
**Testing**: `NullusUnitTests` focused GoogleTest filters plus `ctest --test-dir build -C Debug --output-on-failure`; recorded validation is platform-specific to the commands actually run
**Target Platform**: Desktop Nullus runtime on Windows first, with portable Linux/macOS C++ thread support planned through standard primitives and requiring separate validation
**Project Type**: Multi-module desktop engine/runtime/editor
**Performance Goals**: Low-overhead worker dispatch for short CPU jobs; bounded queueing; main-thread helping during fence completion; no starvation of normal work under bounded high-priority usage; background work isolated from short CPU jobs
**Constraints**: Keep generated files under `Runtime/*/Gen/` untouched; preserve Editor/Game runtime viability; avoid exposing STL-owning ABI in binding-ready contracts; do not weaken existing rendering thread ownership or backend validation rules
**Scale/Scope**: New `Runtime/Base/Jobs` subsystem, JobSystem unit tests, one low-risk consumer migration, and documentation/contracts in `specs/032-job-system`

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: PASS. This Runtime foundation feature has a committed spec bundle at `D:/VSProject/Nullus/specs/032-job-system/`.
- **Validation matches subsystem**: PASS. Runtime infrastructure will be validated primarily through focused `NullusUnitTests`, stress-style deterministic unit tests, and consumer migration tests; rendering claims remain out of scope unless later rendering integration is separately validated.
- **Generated code and backend boundaries**: PASS. The implementation stays outside `Runtime/*/Gen/` and keeps rendering backend concerns behind existing rendering/RHI boundaries.
- **Incremental, verified delivery**: PASS. Work is split into scheduler MVP, batched/parallel work, background queue, diagnostics/safety, binding-ready contracts, and consumer migration.
- **Product runtime preservation**: PASS. Editor/Game must remain runnable; existing ad hoc workers are migrated only after the shared scheduler is tested.

## Project Structure

### Documentation (this feature)

```text
D:/VSProject/Nullus/specs/032-job-system/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── native-job-api.md
│   ├── binding-ready-api.md
│   └── diagnostics-contract.md
└── tasks.md
```

### Source Code (repository root)

```text
D:/VSProject/Nullus/Runtime/Base/Jobs/
├── JobSystem.h
├── JobSystem.cpp
├── JobTypes.h
├── JobQueue.h
├── JobQueue.cpp
├── JobBatchDispatcher.h
├── JobBatchDispatcher.cpp
├── JobRange.h
├── JobRange.cpp
├── BackgroundJobQueue.h
├── BackgroundJobQueue.cpp
├── JobDiagnostics.h
├── JobDiagnostics.cpp
├── JobSafety.h
├── JobSafety.cpp
├── JobBindings.h
└── JobBindings.cpp

D:/VSProject/Nullus/Tests/Unit/
├── JobSystemSchedulerTests.cpp
├── JobSystemParallelTests.cpp
├── JobSystemBackgroundTests.cpp
├── JobSystemDiagnosticsTests.cpp
├── JobSystemBindingsTests.cpp
└── JobSystemMigrationTests.cpp

D:/VSProject/Nullus/Project/Editor/Core/
├── EditorActions.h
├── EditorActions.cpp
├── EditorBackgroundTaskTracker.h
└── EditorBackgroundTaskTracker.cpp
```

**Structure Decision**: Place the scheduler in `Runtime/Base/Jobs` because `NLS_Base` is already the lowest runtime layer used by Core, Rendering, Engine, UI, Editor, Game, and tests. This keeps the JobSystem usable by all consumers without introducing Rendering-owned or Editor-owned infrastructure. Existing consumer migration happens after the core subsystem is tested.

## Complexity Tracking

No constitution violations require justification.

## Phase 0: Research Output

Research decisions are recorded in [research.md](./research.md). The key decisions are:

- Implement Unity-style semantics, not Unity source copying.
- Start with a correctness-first mutex/condition-variable scheduler and keep queue internals replaceable.
- Represent fences as generation-checked opaque handles rather than raw group pointers.
- Keep background work on a distinct queue.
- Provide binding-ready C-compatible contracts without claiming managed C# job support.

## Phase 1: Design Output

Design artifacts are recorded in:

- [data-model.md](./data-model.md)
- [contracts/native-job-api.md](./contracts/native-job-api.md)
- [contracts/binding-ready-api.md](./contracts/binding-ready-api.md)
- [contracts/diagnostics-contract.md](./contracts/diagnostics-contract.md)
- [quickstart.md](./quickstart.md)

## Phase 2: Implementation Strategy

1. Establish the `Runtime/Base/Jobs` structure and public headers with tests that fail before implementation.
2. Implement core scheduler, fences, dependencies, completion, and shutdown until `JobSystemSchedulerTests` pass.
3. Add batched, concurrent, parallel-for, block range, and work-stealing helpers until `JobSystemParallelTests` pass.
4. Add background queue and continuation draining until `JobSystemBackgroundTests` pass.
5. Add diagnostics, disallowed-sync safety scopes, profiler thread registration, and test hooks until diagnostics tests pass.
6. Add binding-ready C-compatible API and contract tests.
7. Migrate one low-risk existing ad hoc consumer, starting with Editor background task tracking if its behavior can be preserved without touching product semantics.
8. Run focused tests, full `NullusUnitTests` where practical, and `ctest`, then perform `/plan-review` quality gates before completion or commit.

## Post-Design Constitution Check

- **Spec-first major change**: PASS. The same `032-job-system` bundle now contains spec, plan, research, data model, contracts, and quickstart.
- **Validation matches subsystem**: PASS. The validation path is unit-test driven with stress-style deterministic coverage for scheduler behavior and a focused consumer migration test.
- **Generated code and backend boundaries**: PASS. No generated files or rendering backend files are required for the core scheduler.
- **Incremental, verified delivery**: PASS. The implementation sequence provides usable, independently testable increments.
- **Product runtime preservation**: PASS. Product paths are preserved by keeping migration staged and consumer-specific.
