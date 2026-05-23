# Tasks: Unified Profiler Integration

**Input**: Design documents from `specs/014-profiler-integration/`
**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md)

**Tests**: Included because the feature spec and plan require `NullusUnitTests` coverage for shared instrumentation, destination independence, and disabled behavior.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel after its phase prerequisites because it touches different files or only depends on completed shared infrastructure.
- **[Story]**: User story label from [spec.md](spec.md).
- Every task includes exact repository paths.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare build switches, third-party locations, and empty source/test files so the profiler can be implemented incrementally.

- [X] T001 Add `NLS_ENABLE_PROFILING`, `NLS_ENABLE_TRACY`, and `NLS_ENABLE_TIMELINE_PROFILER` CMake options with default-off safe behavior in `CMakeLists.txt`
- [X] T002 Add conditional placeholder target wiring for `ThirdParty/Tracy` and `ThirdParty/TimelineProfiler` in `ThirdParty/CMakeLists.txt`
- [X] T003 Create profiler source directory and empty facade files in `Runtime/Base/Profiling/Profiler.h`, `Runtime/Base/Profiling/Profiler.cpp`, `Runtime/Base/Profiling/ProfilerScope.h`, and `Runtime/Base/Profiling/ProfilerScope.cpp`
- [X] T004 [P] Create empty destination wrapper files in `Runtime/Base/Profiling/TracyProfiler.h`, `Runtime/Base/Profiling/TracyProfiler.cpp`, `Runtime/Base/Profiling/TimelineProfilerSink.h`, and `Runtime/Base/Profiling/TimelineProfilerSink.cpp`
- [X] T005 Add the new `Runtime/Base/Profiling` files to the existing `NLS_Base` build through `Runtime/Base/CMakeLists.txt`
- [X] T006 [P] Create empty unit test files `Tests/Unit/ProfilerScopeTests.cpp` and `Tests/Unit/ProfilerDestinationTests.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish the shared profiler vocabulary and test seams that every user story depends on.

**Critical**: No user story work should begin until this phase is complete.

- [X] T007 Define `NLS::Base::Profiling::ProfilerDestinationId`, `ProfilerAvailability`, `ProfilerCapabilityFlags`, `ProfilerDestinationState`, and `ProfilerSessionStats` in `Runtime/Base/Profiling/Profiler.h`
- [X] T008 Define the `NLS::Base::Profiling::IProfilerDestination` testable interface with `BeginScope`, `EndScope`, and `GetState` in `Runtime/Base/Profiling/Profiler.h`
- [X] T009 Implement destination registration, unregister/reset, enable/disable state, and diagnostics counters in `Runtime/Base/Profiling/Profiler.cpp`
- [X] T010 Implement `NLS::Base::Profiling::ProfilerScope` RAII begin/end behavior in `Runtime/Base/Profiling/ProfilerScope.h` and `Runtime/Base/Profiling/ProfilerScope.cpp`
- [X] T011 Define default function-name and explicit-name instrumentation macros or inline helpers in `Runtime/Base/Profiling/Profiler.h`
- [X] T012 [P] Add `RecordingProfilerDestination` test double helpers inside `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T013 [P] Add `RecordingProfilerDestination` test double helpers inside `Tests/Unit/ProfilerScopeTests.cpp`
- [X] T014 Run `cmake --build Build --config Debug --target NullusUnitTests -- /m:1` and record any build failures before user-story implementation continues

**Checkpoint**: Shared profiler facade compiles and can be tested without Tracy or TimelineProfiler.

---

## Phase 3: User Story 1 - Add One Instrumentation Point For Both Profilers (Priority: P1) MVP

**Goal**: A developer marks one scope and both enabled destinations receive matching scoped events with default function-name labels or explicit override labels.

**Independent Test**: Add test destinations, emit one default-named scope and one explicit-named scope, and verify both destinations observe matching begin/end events.

### Tests for User Story 1

- [X] T015 [P] [US1] Add a failing default function-name scope test in `Tests/Unit/ProfilerScopeTests.cpp`
- [X] T016 [P] [US1] Add a failing explicit named scope test in `Tests/Unit/ProfilerScopeTests.cpp`
- [X] T017 [P] [US1] Add a failing multi-destination routing test in `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T018 [P] [US1] Add a failing nested scope ordering test in `Tests/Unit/ProfilerScopeTests.cpp`

### Implementation for User Story 1

- [X] T019 [US1] Implement default function-name resolution for the public scope helper in `Runtime/Base/Profiling/Profiler.h`
- [X] T020 [US1] Implement explicit display-name override handling in `Runtime/Base/Profiling/ProfilerScope.cpp`
- [X] T021 [US1] Implement matching begin/end dispatch to every available destination in `Runtime/Base/Profiling/Profiler.cpp`
- [X] T022 [US1] Implement per-thread nesting depth tracking in `Runtime/Base/Profiling/Profiler.cpp`
- [X] T023 [US1] Add a representative shared marker to editor update or UI rendering code in `Project/Editor/Core/Editor.cpp`
- [X] T024 [US1] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=ProfilerScopeTests.*:ProfilerDestinationTests.*` and fix failures in `Runtime/Base/Profiling/*` or `Tests/Unit/Profiler*.cpp`

**Checkpoint**: User Story 1 is independently functional and testable as the MVP.

---

## Phase 4: User Story 2 - Inspect Recent Profiling Data Inside The Editor (Priority: P2)

**Goal**: The editor exposes a dockable Profiler window that renders TimelineProfiler data or a clear unavailable/empty state.

**Independent Test**: Build and run the editor, open the Profiler window from the existing window/menu workflow, exercise a profiled path, and verify live or explicit unavailable/empty display.

### Tests for User Story 2

- [X] T025 [P] [US2] Add a failing Profiler panel availability/status unit test in `Tests/Unit/PanelWindowHookTests.cpp`
- [X] T026 [P] [US2] Add a failing Timeline destination state formatting test in `Tests/Unit/ProfilerDestinationTests.cpp`

### Implementation for User Story 2

- [X] T027 [US2] Implement `TimelineProfilerSink` availability, CPU scope forwarding, and unsupported GPU capability reporting in `Runtime/Base/Profiling/TimelineProfilerSink.h` and `Runtime/Base/Profiling/TimelineProfilerSink.cpp`
- [X] T028 [US2] Create `NLS::Editor::Panels::ProfilerPanel` in `Project/Editor/Panels/ProfilerPanel.h`
- [X] T029 [US2] Implement Profiler panel drawing, live/empty/disabled/unavailable/unsupported status messages, and TimelineProfiler draw call integration in `Project/Editor/Panels/ProfilerPanel.cpp`
- [X] T030 [US2] Register the Profiler panel with `PanelsManager` and the existing top bar window list in `Project/Editor/Core/Editor.cpp`
- [X] T031 [US2] Add `ProfilerPanel` sources to the editor build through `Project/Editor/CMakeLists.txt`
- [X] T032 [US2] Run `cmake --build Build --config Debug --target Editor -- /m:1` and fix compile/link issues in `Project/Editor/Panels/ProfilerPanel.*` or profiler CMake wiring
- [ ] T033 [US2] Manually open the editor Profiler window and record the live/empty/unavailable result in `specs/014-profiler-integration/quickstart.md`

**Checkpoint**: User Story 2 can be demonstrated independently in the editor.

---

## Phase 5: User Story 3 - Keep Profiling Optional And Low Intrusion (Priority: P3)

**Goal**: Profiling markers remain safe when profiling is disabled or when either destination is unavailable.

**Independent Test**: Build and run with profiling disabled, Tracy-only, TimelineProfiler-only, and both-enabled configurations; verify instrumented code paths still build/run and available destinations receive events.

### Tests for User Story 3

- [X] T034 [P] [US3] Add a failing disabled-profiling no-destination test in `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T035 [P] [US3] Add a failing unavailable-destination-does-not-block-available-destination test in `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T036 [P] [US3] Add a failing multi-threaded scope emission test in `Tests/Unit/ProfilerScopeTests.cpp`

### Implementation for User Story 3

- [X] T037 [US3] Implement disabled-profiling fast path and safe no-destination behavior in `Runtime/Base/Profiling/Profiler.h` and `Runtime/Base/Profiling/Profiler.cpp`
- [X] T038 [US3] Implement unavailable/disabled destination filtering and dropped-event diagnostics in `Runtime/Base/Profiling/Profiler.cpp`
- [X] T039 [US3] Implement thread-safe destination routing and per-thread scope state in `Runtime/Base/Profiling/Profiler.cpp`
- [X] T040 [US3] Implement `TracyProfiler` availability and CPU scope forwarding behind `NLS_ENABLE_TRACY` in `Runtime/Base/Profiling/TracyProfiler.h` and `Runtime/Base/Profiling/TracyProfiler.cpp`
- [X] T041 [US3] Confirm destination-specific compile definitions and include paths are isolated in `Runtime/Base/CMakeLists.txt` and `Project/Editor/CMakeLists.txt`
- [X] T042 [US3] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=ProfilerScopeTests.*:ProfilerDestinationTests.*` for enabled and disabled profiler configurations

**Checkpoint**: All user stories are independently functional and normal builds do not require profiler tools.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, broad validation, and cleanup across all stories.

- [X] T043 [P] Document default function-name scope usage, explicit names, destination toggles, and editor panel workflow in `Docs/Profiling.md`
- [X] T044 [P] Add short profiler troubleshooting notes for unavailable Tracy, unavailable TimelineProfiler, and unsupported GPU data in `Docs/Profiling.md`
- [X] T045 [P] Add final validation notes for unit tests, Editor build, manual editor panel check, and Tracy check to `specs/014-profiler-integration/quickstart.md`
- [X] T046 Review all changed files to confirm no manual edits were made under `Runtime/*/Gen/`
- [X] T047 Run `ctest --test-dir Build -C Debug --output-on-failure` and fix profiler-related failures
- [X] T048 Run `cmake --build Build --config Debug --target Editor -- /m:1` and fix profiler-related build or link failures
- [X] T049 Move ImGuizmo and TimelineProfiler source integration into `Runtime/UI/ImGuiExtensions/` instead of standalone `ThirdParty` CMake targets
- [X] T050 Add `Runtime/UI/ImGuiExtensions/cmake/RegisterImGuiExtension.cmake` and document the reusable extension onboarding flow in `Runtime/UI/ImGuiExtensions/README.md`
- [X] T051 Move `TimelineProfilerSink` from `Runtime/Base/Profiling` to `Runtime/UI/Profiling` so Base does not depend on UI-owned ImGui extension sources

---

## Phase 7: Rendering, RHI Thread, And GPU Profiling Expansion

**Purpose**: Extend the shared profiler from editor-facing CPU samples into representative render/RHI CPU scopes, stable render/RHI worker thread labels, and DX12 TimelineProfiler GPU scopes.

### Tests for Rendering/GPU Expansion

- [X] T052 [P] Add profiler thread naming and GPU-scope destination routing tests in `Tests/Unit/ProfilerDestinationTests.cpp`
- [X] T053 [P] Add disabled/unavailable GPU destination safety tests in `Tests/Unit/ProfilerScopeTests.cpp`

### Implementation for Rendering/GPU Expansion

- [X] T054 Add `RegisterThread`, `ProfilerGpuScopeEvent`, GPU destination hooks, GPU routing, and `NLS_GPU_PROFILE_*` macros in `Runtime/Base/Profiling/Profiler.h`, `Runtime/Base/Profiling/Profiler.cpp`, `Runtime/Base/Profiling/ProfilerScope.h`, and `Runtime/Base/Profiling/ProfilerScope.cpp`
- [X] T055 Extend `TracyProfiler` and `TimelineProfilerSink` state reporting so GPU capability is destination-specific and only TimelineProfiler reports GPU scopes after DX12 GPU initialization
- [X] T056 Add backend-neutral GPU profiler hooks to `Runtime/Rendering/RHI/Core/RHICommand.h`
- [X] T057 Bridge DX12 command-list begin/end GPU events and queue submit command-list handoff in `Runtime/Rendering/RHI/Backends/DX12/DX12Command.*`, `Runtime/Rendering/RHI/Backends/DX12/DX12Queue.*`, and `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.cpp`
- [X] T058 Add CPU scopes and stable thread registration to `Runtime/Rendering/Context/Driver.cpp`, `Runtime/Rendering/Context/RenderThreadCoordinator.cpp`, `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`, and `Runtime/Rendering/Core/ABaseRenderer.cpp`
- [X] T059 Add representative GPU scopes around render pass and threaded pass command recording in `Runtime/Rendering/Core/ABaseRenderer.cpp` and `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [X] T060 Update `Docs/Profiling.md` and `specs/014-profiler-integration/quickstart.md` with render/RHI/GPU profiler usage and validation notes
- [X] T061 Run focused profiler tests, `ctest --test-dir Build -C Debug --output-on-failure`, and `cmake --build Build --config Debug --target Editor -- /m:1`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 Setup**: No dependencies.
- **Phase 2 Foundational**: Depends on Phase 1 and blocks all user stories.
- **Phase 3 US1**: Depends on Phase 2. This is the MVP and should be completed first.
- **Phase 4 US2**: Depends on Phase 2 and benefits from US1 markers for live data, but can be developed with synthetic timeline events if needed.
- **Phase 5 US3**: Depends on Phase 2 and should be validated after US1 destination routing exists.
- **Phase 6 Polish**: Depends on desired user stories being complete.
- **Phase 7 Rendering/GPU Expansion**: Depends on the shared facade, TimelineProfiler UI extension ownership, and editor panel integration from Phases 1-6.

### User Story Dependencies

- **US1 (P1)**: No dependency on other stories after Phase 2.
- **US2 (P2)**: Uses the shared facade from Phase 2 and ideally US1 representative markers for manual data.
- **US3 (P3)**: Uses the shared facade from Phase 2 and hardens routing from US1.

### Parallel Opportunities

- T004 and T006 can run after T001-T003 because they create independent files.
- T012 and T013 can run in parallel after T008.
- T015-T018 can run in parallel because they add separate test cases.
- T025 and T026 can run in parallel because they touch different test concerns.
- T034-T036 can run in parallel because they add independent failing tests.
- T043-T045 can run in parallel after implementation behavior is known.

---

## Parallel Example: User Story 1

```text
Task: "T015 [US1] Add a failing default function-name scope test in Tests/Unit/ProfilerScopeTests.cpp"
Task: "T016 [US1] Add a failing explicit named scope test in Tests/Unit/ProfilerScopeTests.cpp"
Task: "T017 [US1] Add a failing multi-destination routing test in Tests/Unit/ProfilerDestinationTests.cpp"
Task: "T018 [US1] Add a failing nested scope ordering test in Tests/Unit/ProfilerScopeTests.cpp"
```

## Parallel Example: User Story 2

```text
Task: "T025 [US2] Add a failing Profiler panel availability/status unit test in Tests/Unit/PanelWindowHookTests.cpp"
Task: "T026 [US2] Add a failing Timeline destination state formatting test in Tests/Unit/ProfilerDestinationTests.cpp"
```

## Parallel Example: User Story 3

```text
Task: "T034 [US3] Add a failing disabled-profiling no-destination test in Tests/Unit/ProfilerDestinationTests.cpp"
Task: "T035 [US3] Add a failing unavailable-destination-does-not-block-available-destination test in Tests/Unit/ProfilerDestinationTests.cpp"
Task: "T036 [US3] Add a failing multi-threaded scope emission test in Tests/Unit/ProfilerScopeTests.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational profiler facade.
3. Complete Phase 3 US1 tests and implementation.
4. Stop and validate with `Build/bin/Debug/NullusUnitTests.exe --gtest_filter=ProfilerScopeTests.*:ProfilerDestinationTests.*`.

### Incremental Delivery

1. Deliver US1 shared instrumentation so call sites have one profiler marker.
2. Deliver US2 editor panel so TimelineProfiler data can be inspected inside the editor.
3. Deliver US3 optional/disabled/unavailable behavior to harden normal development and CI workflows.
4. Finish with documentation and full quickstart validation.

### Team Parallel Strategy

After Phase 2, one worker can finish US1 routing, another can build the editor panel shell with synthetic TimelineProfiler state, and a third can add disabled/unavailable destination tests. Merge order should still prefer US1 before US2/US3 final integration because real timeline display and destination fallback depend on shared routing behavior.
