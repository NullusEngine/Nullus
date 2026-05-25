# Tasks: Nullus Unity-Style Native JobSystem

**Input**: Design documents from `D:/VSProject/Nullus/specs/032-job-system/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Required. This feature changes Runtime behavior and follows test-first delivery.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to
- Each task includes concrete repository paths

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the subsystem skeleton and focused test files.

- [X] T001 Create `Runtime/Base/Jobs/JobTypes.h` with public handle, priority, safety, config, and shutdown type declarations.
- [X] T002 Create `Runtime/Base/Jobs/JobSystem.h` and `Runtime/Base/Jobs/JobSystem.cpp` with lifecycle API stubs only.
- [X] T003 [P] Create `Runtime/Base/Jobs/JobQueue.h` and `Runtime/Base/Jobs/JobQueue.cpp` with internal scheduler type placeholders.
- [X] T004 [P] Create `Runtime/Base/Jobs/JobBatchDispatcher.h` and `Runtime/Base/Jobs/JobBatchDispatcher.cpp` with class declarations.
- [X] T005 [P] Create `Runtime/Base/Jobs/JobRange.h` and `Runtime/Base/Jobs/JobRange.cpp` with block range and work-stealing declarations.
- [X] T006 [P] Create `Runtime/Base/Jobs/BackgroundJobQueue.h` and `Runtime/Base/Jobs/BackgroundJobQueue.cpp` with public background API declarations.
- [X] T007 [P] Create `Runtime/Base/Jobs/JobDiagnostics.h` and `Runtime/Base/Jobs/JobDiagnostics.cpp` with diagnostic snapshot declarations.
- [X] T008 [P] Create `Runtime/Base/Jobs/JobSafety.h` and `Runtime/Base/Jobs/JobSafety.cpp` with disallow-sync scope declarations.
- [X] T009 [P] Create `Runtime/Base/Jobs/JobBindings.h` and `Runtime/Base/Jobs/JobBindings.cpp` with C-compatible binding declarations.
- [X] T010 [P] Create empty focused test files `Tests/Unit/JobSystemSchedulerTests.cpp`, `Tests/Unit/JobSystemParallelTests.cpp`, `Tests/Unit/JobSystemBackgroundTests.cpp`, `Tests/Unit/JobSystemDiagnosticsTests.cpp`, `Tests/Unit/JobSystemBindingsTests.cpp`, and `Tests/Unit/JobSystemMigrationTests.cpp`.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish deterministic test harness helpers and safe lifecycle reset behavior.

**CRITICAL**: No user story implementation can begin until this phase is complete.

- [X] T011 Add test fixture helpers for initializing and shutting down JobSystem per test in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T012 Implement minimal `InitializeJobSystem`, `ShutdownJobSystem`, `IsJobSystemInitialized`, and `GetJobWorkerCount` in `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T013 Add reset-for-testing hook declarations guarded by `NLS_ENABLE_TEST_HOOKS` in `Runtime/Base/Jobs/JobSystem.h`.
- [X] T014 Implement reset-for-testing cleanup in `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T015 Verify the foundational lifecycle tests fail before T012 and pass after T014 using `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemSchedulerTests.Lifecycle*"`.

---

## Phase 3: User Story 1 - Schedule Native Jobs With Fences and Dependencies (Priority: P1) MVP

**Goal**: Provide a usable native scheduler with job handles, dependencies, completion, query, and shutdown.

**Independent Test**: Schedule single and dependent jobs, complete fences from the main thread, and verify execution order and completion state.

### Tests for User Story 1

- [X] T016 [P] [US1] Add failing single-job execution and repeated-completion tests in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T017 [P] [US1] Add failing dependency-chain and fan-in completion tests in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T018 [P] [US1] Add failing main-thread-helping completion test in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T019 [P] [US1] Add failing high-priority-before-normal bounded ordering test in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T020 [P] [US1] Add failing shutdown drain and reject tests in `Tests/Unit/JobSystemSchedulerTests.cpp`.

### Implementation for User Story 1

- [X] T021 [US1] Implement generation-checked `JobHandle` registry and internal `JobGroup` state in `Runtime/Base/Jobs/JobQueue.h` and `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T022 [US1] Implement worker startup, wake, join, and queue ownership in `Runtime/Base/Jobs/JobSystem.cpp` and `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T023 [US1] Implement `ScheduleJob`, `ScheduleJobDepends`, `IsCompleted`, `Complete`, `CompleteNoClear`, `CompleteAll`, `ClearWithoutSync`, and `HasBeenSynced` in `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T024 [US1] Implement dependency resolution and dependent wakeups in `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T025 [US1] Implement main-thread helping through `ExecuteOneJobQueueJob` and completion draining in `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T026 [US1] Implement high-priority lane and bounded normal-job fairness in `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T027 [US1] Implement shutdown drain and immediate rejection behavior in `Runtime/Base/Jobs/JobSystem.cpp` and `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T028 [US1] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemSchedulerTests.*"` and fix US1 failures.

**Checkpoint**: User Story 1 is fully functional and testable independently.

---

## Phase 4: User Story 2 - Execute Batched, Concurrent, and Parallel-For Work (Priority: P1)

**Goal**: Add Unity-style batched jobs, different-jobs-concurrent, parallel-for, block range, and work stealing.

**Independent Test**: Large and uneven range workloads cover each index once and run combine callbacks after all worker callbacks.

### Tests for User Story 2

- [X] T029 [P] [US2] Add failing different-jobs-concurrent shared handle tests in `Tests/Unit/JobSystemParallelTests.cpp`.
- [X] T030 [P] [US2] Add failing parallel-for exact-once and zero-iteration tests in `Tests/Unit/JobSystemParallelTests.cpp`.
- [X] T031 [P] [US2] Add failing combine-after-iterations test in `Tests/Unit/JobSystemParallelTests.cpp`.
- [X] T032 [P] [US2] Add failing block range helper boundary tests in `Tests/Unit/JobSystemParallelTests.cpp`.
- [X] T033 [P] [US2] Add failing work-stealing range uneven workload tests in `Tests/Unit/JobSystemParallelTests.cpp`.
- [X] T034 [P] [US2] Add failing batch dispatcher kick-before-visible tests in `Tests/Unit/JobSystemParallelTests.cpp`.

### Implementation for User Story 2

- [X] T035 [US2] Implement `ScheduleDifferentJobsConcurrent` and shared group completion in `Runtime/Base/Jobs/JobSystem.cpp` and `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T036 [US2] Implement `ScheduleJobForEach` with optional combine callback in `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T037 [US2] Implement block range helpers in `Runtime/Base/Jobs/JobRange.cpp`.
- [X] T038 [US2] Implement work-stealing range initialization and claiming in `Runtime/Base/Jobs/JobRange.cpp`.
- [X] T039 [US2] Implement `JobBatchDispatcher` add, kick, and empty behavior in `Runtime/Base/Jobs/JobBatchDispatcher.cpp`.
- [X] T040 [US2] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemParallelTests.*"` and fix US2 failures.

**Checkpoint**: User Stories 1 and 2 both work independently.

---

## Phase 5: User Story 3 - Support Background Jobs and Main-Thread Continuations (Priority: P2)

**Goal**: Add long-running background queue and deterministic main-thread continuation draining.

**Independent Test**: Background jobs run separately from short workers, continuations run only when drained, and shutdown is clean.

### Tests for User Story 3

- [X] T041 [P] [US3] Add failing background job execution and handle completion tests in `Tests/Unit/JobSystemBackgroundTests.cpp`.
- [X] T042 [P] [US3] Add failing continuation-only-runs-on-drain test in `Tests/Unit/JobSystemBackgroundTests.cpp`.
- [X] T043 [P] [US3] Add failing continuation dependency ordering test in `Tests/Unit/JobSystemBackgroundTests.cpp`.
- [X] T044 [P] [US3] Add failing background shutdown drain test in `Tests/Unit/JobSystemBackgroundTests.cpp`.

### Implementation for User Story 3

- [X] T045 [US3] Implement background worker pool and queue ownership in `Runtime/Base/Jobs/BackgroundJobQueue.cpp`.
- [X] T046 [US3] Implement `ScheduleBackgroundJob`, background handle completion, and dependency checks in `Runtime/Base/Jobs/BackgroundJobQueue.cpp`.
- [X] T047 [US3] Implement `ScheduleMainThreadContinuation` and `DrainMainThreadContinuations` in `Runtime/Base/Jobs/BackgroundJobQueue.cpp`.
- [X] T048 [US3] Wire background queue lifecycle into `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T049 [US3] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemBackgroundTests.*"` and fix US3 failures.

**Checkpoint**: Background lane is usable without migrating consumers yet.

---

## Phase 6: User Story 4 - Diagnose Safety, Debugging, and Profiling State (Priority: P2)

**Goal**: Provide structured diagnostics, disallowed sync wait scopes, and profiler integration.

**Independent Test**: Diagnostic snapshots and safety violations are deterministic in tests.

### Tests for User Story 4

- [X] T050 [P] [US4] Add failing lifecycle snapshot tests in `Tests/Unit/JobSystemDiagnosticsTests.cpp`.
- [X] T051 [P] [US4] Add failing disallowed sync wait violation tests in `Tests/Unit/JobSystemDiagnosticsTests.cpp`.
- [X] T052 [P] [US4] Add failing cleared-without-sync diagnostic tests in `Tests/Unit/JobSystemDiagnosticsTests.cpp`.
- [X] T053 [P] [US4] Add failing profiler thread-name source contract or test-destination test in `Tests/Unit/JobSystemDiagnosticsTests.cpp`.

### Implementation for User Story 4

- [X] T054 [US4] Implement bounded diagnostic history and `CopyJobDiagnosticSnapshot` in `Runtime/Base/Jobs/JobDiagnostics.cpp`.
- [X] T055 [US4] Record lifecycle, invalid handle, null callback, stale handle, and shutdown rejection diagnostics from `Runtime/Base/Jobs/JobSystem.cpp` and `Runtime/Base/Jobs/JobQueue.cpp`.
- [X] T056 [US4] Implement `DisallowJobSyncWaitScope` and thread-local sync wait state in `Runtime/Base/Jobs/JobSafety.cpp`.
- [X] T057 [US4] Enforce disallowed sync wait reporting during `Complete` in `Runtime/Base/Jobs/JobSystem.cpp`.
- [X] T058 [US4] Add profiler thread registration and named job scopes in worker execution paths in `Runtime/Base/Jobs/JobQueue.cpp` and `Runtime/Base/Jobs/BackgroundJobQueue.cpp`.
- [X] T059 [US4] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemDiagnosticsTests.*"` and fix US4 failures.

**Checkpoint**: Diagnostics and safety can support consumer migration.

---

## Phase 7: User Story 5 - Provide Binding-Ready Public Contracts (Priority: P3)

**Goal**: Add versioned C-compatible opaque handle APIs for future generated or script-facing integrations.

**Independent Test**: C-compatible API schedules, completes, queries, and rejects invalid handles deterministically.

### Tests for User Story 5

- [X] T060 [P] [US5] Add failing binding schedule and complete tests in `Tests/Unit/JobSystemBindingsTests.cpp`.
- [X] T061 [P] [US5] Add failing binding invalid handle and default handle tests in `Tests/Unit/JobSystemBindingsTests.cpp`.
- [X] T062 [P] [US5] Add failing binding struct size and version mismatch tests in `Tests/Unit/JobSystemBindingsTests.cpp`.
- [X] T063 [P] [US5] Add failing source contract test that binding structs avoid STL-owning ABI in `Tests/Unit/JobSystemBindingsTests.cpp`.

### Implementation for User Story 5

- [X] T064 [US5] Implement binding struct constants, status conversion, and validation helpers in `Runtime/Base/Jobs/JobBindings.cpp`.
- [X] T065 [US5] Implement `NLS_Jobs_GetWorkerCount`, `NLS_Jobs_Schedule`, `NLS_Jobs_ScheduleForEach`, `NLS_Jobs_IsCompleted`, `NLS_Jobs_Complete`, and `NLS_Jobs_ClearWithoutSync` in `Runtime/Base/Jobs/JobBindings.cpp`.
- [X] T066 [US5] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemBindingsTests.*"` and fix US5 failures.

**Checkpoint**: Native binding contracts are stable enough for future script work.

---

## Phase 8: User Story 6 - Migrate Existing Ad Hoc Threaded Workloads Incrementally (Priority: P3)

**Goal**: Migrate one low-risk existing private worker path to the shared JobSystem while preserving product behavior.

**Independent Test**: Existing behavior and shutdown semantics remain intact for the migrated consumer.

### Tests for User Story 6

- [X] T067 [P] [US6] Add failing migration contract test for Editor background task queue behavior in `Tests/Unit/JobSystemMigrationTests.cpp`.
- [X] T068 [P] [US6] Add failing source contract test that `Project/Editor/Core/EditorActions.cpp` no longer starts private background worker threads for tracked background tasks in `Tests/Unit/JobSystemMigrationTests.cpp`.
- [X] T069 [P] [US6] Add failing shutdown behavior test or source contract for Editor background task cleanup in `Tests/Unit/JobSystemMigrationTests.cpp`.

### Implementation for User Story 6

- [X] T070 [US6] Replace `EditorActions::TrackBackgroundTask` private queue scheduling with JobSystem background scheduling in `Project/Editor/Core/EditorActions.cpp`.
- [X] T071 [US6] Remove or narrow private background worker fields from `Project/Editor/Core/EditorActions.h` while preserving delayed actions and listener cleanup.
- [X] T072 [US6] Adapt `EditorActions` destructor cleanup to the shared JobSystem contract in `Project/Editor/Core/EditorActions.cpp`.
- [X] T073 [US6] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemMigrationTests.*:EditorRenderPathContractTests.*"` and fix US6 failures.

**Checkpoint**: One existing consumer uses the shared scheduler without product behavior regressions.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Final documentation, validation, and review gates.

- [X] T074 [P] Document JobSystem usage and migration notes in `Docs/Jobs.md`.
- [X] T075 [P] Add include/path sanity source contract tests for public JobSystem headers in `Tests/Unit/JobSystemSchedulerTests.cpp`.
- [X] T076 Run focused JobSystem validation from `specs/032-job-system/quickstart.md`.
- [X] T077 Run `cmake --build build --config Debug --target NullusUnitTests -- /m:4`.
- [X] T078 Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystem*"`.
- [X] T079 Run `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests`.
- [ ] T080 Run `/plan-review` automatic quality review loop according to `AGENTS.md`.

### Validation Evidence

Windows Debug validation only; Linux and macOS were not run in this pass.

- 2026-05-25: `cmake --build build --config Debug --target NullusUnitTests -- /m:4 /nodeReuse:false` completed successfully after clearing stale MSBuild nodes.
- 2026-05-25: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemBackgroundTests.BackgroundJobFailureKeepsHandlePendingUntilCancelCleanupReturns:JobSystemBackgroundTests.ImmediateShutdownKeepsQueuedBackgroundJobPendingUntilCancelCleanupReturns:JobSystemBackgroundTests.HelpedForegroundCallbackOnBackgroundWorkerRejectsBackgroundWait:JobSystemBackgroundTests.CompletingBackgroundJobDoesNotHelpUnrelatedBackgroundForegroundDependency:JobSystemBackgroundTests.BackgroundWorkerRejectsForegroundWaitWhenTransitiveDependencyContainsPendingBackground:JobSystemSchedulerTests.TryInitializeReportsOwnershipOnlyForNewRuntime" --gtest_break_on_failure` passed: 6/6 P0/P1 regression tests.
- 2026-05-25: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemBackgroundTests.*" --gtest_break_on_failure` passed: 40/40 tests.
- 2026-05-25: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystem*"` passed: 146/146 tests.
- 2026-05-25: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemMigrationTests.*:EditorRenderPathContractTests.*"` passed: 82/82 tests.
- 2026-05-25: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemBackgroundTests.BackgroundWorkerRejectsSynchronousWaitForBackgroundJob" --gtest_repeat=50 --gtest_break_on_failure` passed: 50/50 repeats.
- 2026-05-25: `ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests` passed: 1/1 ctest, 0 failures.
- 2026-05-25: `git diff --check -- Runtime/Base/Jobs Tests/Unit/JobSystemBackgroundTests.cpp Tests/Unit/JobSystemParallelTests.cpp Tests/Unit/JobSystemDiagnosticsTests.cpp Tests/Unit/JobSystemBindingsTests.cpp Tests/Unit/JobSystemSchedulerTests.cpp Tests/Unit/JobSystemMigrationTests.cpp Tests/Unit/JobSystemBindingCAbiCompileTest.c Docs/Jobs.md Docs/REVIEW_PATTERNS.md specs/032-job-system Project/Editor/Core/EditorBackgroundTaskTracker.cpp Project/Editor/Core/EditorBackgroundTaskTracker.h Project/Editor/Core/EditorActions.cpp Project/Editor/Core/EditorActions.h Tests/Unit/CMakeLists.txt` reported no whitespace errors; it only printed LF-to-CRLF warnings for existing Windows-tracked files.
- 2026-05-25: `git diff --name-only -- Runtime/*/Gen/* Project/*/Gen/*` returned empty, confirming no generated-file edits in this validation pass.
- 2026-05-26: `cmake --build build --config Debug --target NullusUnitTests -- /m:1 /nodeReuse:false` completed successfully after P1 review fixes.
- 2026-05-26: Added red/green regressions for typed `IJobParallelFor` shard-local job copies, scoped background completion not running unrelated `GuaranteedNoSyncWait` foreground work, and diagnostics queued-count migration after waiting dependency cancellation.
- 2026-05-26: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystem*"` passed: 157/157 tests.
- 2026-05-26: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="JobSystemMigrationTests.*:EditorRenderPathContractTests.*"` passed: 82/82 tests.
- 2026-05-26: `git diff --check -- Runtime/Base/Jobs Tests/Unit/JobSystemBackgroundTests.cpp Tests/Unit/JobSystemParallelTests.cpp Tests/Unit/JobSystemDiagnosticsTests.cpp Tests/Unit/JobSystemBindingsTests.cpp Tests/Unit/JobSystemSchedulerTests.cpp Tests/Unit/JobSystemMigrationTests.cpp Tests/Unit/JobSystemBindingCAbiCompileTest.c Docs/Jobs.md Docs/REVIEW_PATTERNS.md specs/032-job-system Project/Editor/Core/EditorBackgroundTaskTracker.cpp Project/Editor/Core/EditorBackgroundTaskTracker.h Project/Editor/Core/EditorActions.cpp Project/Editor/Core/EditorActions.h Tests/Unit/CMakeLists.txt` reported no whitespace errors; it only printed LF-to-CRLF warnings for existing Windows-tracked files.
- 2026-05-26: `git diff --name-only -- Runtime/*/Gen/* Project/*/Gen/*` returned empty, confirming no generated-file edits in this validation pass.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on Setup completion and blocks all stories.
- **US1 and US2 (Phases 3-4)**: US2 depends on US1 scheduler fundamentals.
- **US3 (Phase 5)**: Depends on US1 lifecycle/handles.
- **US4 (Phase 6)**: Depends on US1 and US3 execution paths.
- **US5 (Phase 7)**: Depends on US1 and US2 native APIs.
- **US6 (Phase 8)**: Depends on US3 and US4.
- **Polish (Phase 9)**: Depends on desired implementation phases.

### Parallel Opportunities

- Setup placeholder files T003-T010 can be created in parallel.
- Test tasks within each user story can be written in parallel before implementation.
- Diagnostics, binding, and documentation tasks touch separate files after core APIs stabilize.

## Implementation Strategy

### MVP First

1. Complete Setup and Foundational phases.
2. Complete US1 scheduler MVP.
3. Validate `JobSystemSchedulerTests.*`.
4. Stop and review before expanding into parallel/background layers.

### Incremental Delivery

1. Scheduler MVP.
2. Parallel and batched jobs.
3. Background jobs and continuations.
4. Diagnostics and safety.
5. Binding-ready contracts.
6. One consumer migration.

### TDD Rule

Each behavior task must start with the corresponding test task and the failure must be observed before implementation code is written.
