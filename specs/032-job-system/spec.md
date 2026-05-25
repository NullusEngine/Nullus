# Feature Specification: Nullus Unity-Style Native JobSystem

**Feature Branch**: `032-job-system`
**Created**: 2026-05-24
**Status**: Draft
**Input**: User description: "Implement JobSystem, reproduce Unity implementation from D:\VSProject\Unity2018.4.0f1. Scope confirmed as full JobSystem." This feature delivers full Nullus-native scheduler coverage of the Unity-style concepts that fit the current C++ runtime layer, without copying Unity source or adding a managed C# container/runtime layer.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Schedule Native Jobs With Fences and Dependencies (Priority: P1)

As a Nullus runtime maintainer, I need a native JobSystem that can schedule single jobs, dependency-ordered jobs, and grouped jobs with Unity-style fences, so that runtime systems can hand CPU work to worker threads and deterministically wait only at explicit sync points.

**Why this priority**: This is the minimum useful JobSystem. Every later feature depends on stable scheduling, dependency completion, worker lifecycle, and deterministic fence semantics.

**Independent Test**: Schedule jobs that mutate test-owned counters and buffers, chain dependencies between them, complete their fences from the main thread, and verify that completion order and data visibility match the dependency graph.

**Acceptance Scenarios**:

1. **Given** the JobSystem is initialized with one or more worker threads, **When** a single job is scheduled and its fence is completed, **Then** the job runs exactly once and the fence is marked complete.
2. **Given** job B depends on job A, **When** both jobs are scheduled before completion is requested, **Then** job A completes before job B can observe or mutate dependent data.
3. **Given** the main thread waits on a fence while eligible work remains queued, **When** completion is requested, **Then** the waiting thread may execute eligible jobs to avoid idle waiting.
4. **Given** the JobSystem is shut down, **When** remaining work is drained or rejected according to the configured shutdown mode, **Then** no worker thread, fence, or queue resource leaks beyond shutdown.

---

### User Story 2 - Execute Batched, Concurrent, and Parallel-For Work (Priority: P1)

As a rendering, asset, or engine subsystem developer, I need Unity-style batched jobs, different-jobs-concurrent scheduling, block ranges, and parallel-for work stealing, so that array and frame workloads can be split into balanced worker tasks without each subsystem inventing its own scheduler.

**Why this priority**: The feature request is specifically for a Unity-style JobSystem, not just a thread pool. Batched and parallel-for work is the core Unity-style value above simple async jobs.

**Independent Test**: Schedule a large range workload with uneven per-index cost, verify all indices are processed once, verify combine jobs run after workers, and verify batch fences complete after all constituent jobs.

**Acceptance Scenarios**:

1. **Given** a set of unrelated job functions share one fence, **When** they are scheduled as different concurrent jobs, **Then** all jobs complete before that fence reports done.
2. **Given** a parallel-for job has a combine function, **When** all iteration jobs finish, **Then** the combine function runs exactly once and only after all iterations complete.
3. **Given** a range workload has more batches than workers, **When** workers finish local ranges at different times, **Then** work stealing redistributes remaining ranges without missing or duplicating iterations.
4. **Given** a workload is smaller than the minimum work per job, **When** block ranges are configured, **Then** the JobSystem produces a bounded, valid range plan instead of spawning excessive empty jobs.

---

### User Story 3 - Support Background Jobs and Main-Thread Continuations (Priority: P2)

As an editor and asset pipeline maintainer, I need a low-priority background job queue for long-running or IO-heavy work plus main-thread continuation draining, so that existing ad hoc editor background workers can migrate to a shared runtime service without blocking gameplay or editor updates.

**Why this priority**: Nullus already has scattered background worker implementations. The native JobSystem needs a safer background lane distinct from short CPU jobs.

**Independent Test**: Schedule background jobs with dependencies, schedule main-thread continuations from completed work, tick continuation draining from a test harness, and verify ordering, cancellation, and shutdown behavior.

**Acceptance Scenarios**:

1. **Given** a long-running background job is scheduled, **When** short normal-priority jobs are also scheduled, **Then** the long-running work does not starve normal JobSystem workers.
2. **Given** a background job completes and queues a main-thread continuation, **When** the main-thread drain entrypoint is called, **Then** the continuation runs on the caller thread in deterministic queue order.
3. **Given** background shutdown begins while work is pending, **When** the shutdown mode drains work, **Then** all accepted work completes or is reported as cancelled without hanging process exit.

---

### User Story 4 - Diagnose Safety, Debugging, and Profiling State (Priority: P2)

As an engine developer debugging parallel runtime code, I need JobSystem diagnostics, safety checks, and profiler integration, so that dependency mistakes, illegal waits, unfinished fences, worker activity, and stuck jobs can be identified before they become intermittent runtime bugs.

**Why this priority**: A native JobSystem without diagnostics is difficult to adopt safely. Unity's implementation includes debugger and safety concepts; Nullus needs native equivalents that fit the existing profiler and test hooks.

**Independent Test**: Enable diagnostics in tests, schedule valid and intentionally invalid dependency patterns, verify that diagnostics report lifecycle state, disallowed sync attempts, unfinished fences, and worker thread names without corrupting normal execution.

**Acceptance Scenarios**:

1. **Given** diagnostics are enabled, **When** jobs are scheduled, started, completed, and waited on, **Then** the JobSystem exposes current and recent job states with job names, fence ids, dependency ids, and thread attribution.
2. **Given** a subsystem enters a scope where synchronous fence waits are disallowed, **When** code attempts to wait on a fence, **Then** the violation is reported through diagnostics and tests can assert the failure path.
3. **Given** profiling is enabled, **When** worker threads start and jobs execute, **Then** threads are registered with the existing Nullus profiler and job execution scopes are visible to profiler destinations.
4. **Given** a fence is cleared without sync for an ownership handoff, **When** diagnostics inspect the fence, **Then** the system distinguishes intentional handoff from forgotten completion.

---

### User Story 5 - Provide Binding-Ready Public Contracts (Priority: P3)

As a future scripting or reflection integration maintainer, I need a stable binding-ready surface for job handles, scheduling parameters, batch queries, and scheduler safety policy metadata, so that managed or script-facing jobs can be layered on top without changing the native scheduler contract.

**Why this priority**: The requested Unity scope includes jobs bindings, but Nullus does not currently have a Unity-compatible managed runtime layer. The correct first step is a stable native contract and C ABI boundary, not a premature scripting implementation.

**Independent Test**: Use only the exported binding-ready functions from a C++ test fixture to create job handles, schedule native callback jobs, complete handles, query worker counts, and validate error results for invalid handles.

**Acceptance Scenarios**:

1. **Given** external code only sees opaque job handles, **When** it schedules and completes callback jobs through the binding-ready API, **Then** it observes the same completion semantics as native C++ users.
2. **Given** external code passes an invalid or expired handle, **When** it queries or completes the handle, **Then** the API reports a deterministic error without dereferencing invalid memory.
3. **Given** future generated bindings need stable names and layouts, **When** contract tests inspect the public headers, **Then** the binding-ready structs remain small, versioned, and free of STL-owning ABI in exported C-compatible surfaces.

---

### User Story 6 - Migrate Existing Ad Hoc Threaded Workloads Incrementally (Priority: P3)

As a Nullus product maintainer, I need existing editor, asset, rendering, and tooling background work to migrate incrementally onto the shared JobSystem, so that the new scheduler becomes the single runtime path without breaking Editor or Game while migration is in progress.

**Why this priority**: Migration is required for long-term value, but it should not block the scheduler MVP or destabilize rendering and editor product paths during the first delivery.

**Independent Test**: Migrate one low-risk existing ad hoc worker path to the JobSystem behind the same user-facing behavior, run its existing unit tests, and verify no change in product runtime behavior.

**Acceptance Scenarios**:

1. **Given** an existing subsystem uses a private background queue, **When** it is migrated to the shared JobSystem, **Then** existing behavior, ordering guarantees, and shutdown behavior remain intact.
2. **Given** the rendering threaded lifecycle already has frame ownership rules, **When** it eventually uses JobSystem workers for parallel work, **Then** existing frame retirement and backend validation rules remain authoritative.
3. **Given** a subsystem cannot safely migrate in the current phase, **When** the JobSystem ships, **Then** the remaining private worker path is documented and does not silently conflict with the shared scheduler.

### Edge Cases

- Worker count is zero, one, greater than hardware concurrency, or configured after initialization.
- Jobs schedule dependent jobs from inside worker execution.
- Main thread waits on a fence whose dependency chain includes queued, running, completed, and invalidated groups.
- Two jobs accidentally try to complete the same group or decrement the same pending counter.
- A job throws, asserts, or reports failure while other jobs in the same group continue.
- A shutdown request races with scheduling, waiting, or background continuation draining.
- Parallel-for iteration count is zero, smaller than worker count, much larger than 16-bit batch windows, or unevenly divisible by batch size.
- Work stealing observes empty local ranges while other workers still own ranges in later phases.
- A high-priority job flood should not permanently starve normal jobs.
- A job marked as safe to execute while waiting actually attempts a synchronous wait.
- Diagnostics are disabled in release-like builds but tests still need deterministic hooks when `NLS_ENABLE_TEST_HOOKS` is enabled.
- Binding-ready APIs receive null callbacks, invalid handles, stale generation ids, or mismatched struct versions.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a runtime-wide native JobSystem service with explicit initialization, shutdown, worker count reporting, and worker thread lifecycle management.
- **FR-002**: The system MUST provide Unity-style job handle or fence semantics that can be copied cheaply and completed or queried repeatedly.
- **FR-003**: The system MUST support scheduling single jobs, dependency-ordered jobs, and grouped jobs that all complete through a fence or handle.
- **FR-004**: The system MUST support completion from the main thread and worker threads without deadlocking when eligible work can be executed by the waiting thread.
- **FR-005**: The system MUST support a high-priority lane for work that should be completed ahead of normal queued work while still avoiding permanent starvation of normal work.
- **FR-006**: The system MUST support a documented safe-wait or no-sync-fence scheduling policy equivalent to Unity's guarantee that certain jobs do not wait recursively.
- **FR-007**: The system MUST support batched scheduling where jobs can be created, accumulated, kicked, and completed later.
- **FR-008**: The system MUST support scheduling multiple different job functions under one shared completion handle.
- **FR-009**: The system MUST support parallel-for scheduling with optional combine work that runs after all iterations.
- **FR-010**: The system MUST provide block range helpers that choose bounded work partitions from array length, minimum indices per job, and available worker count.
- **FR-011**: The system MUST provide work-stealing range helpers for uneven parallel-for workloads.
- **FR-012**: The system MUST provide a background job queue for long-running or IO-heavy work that is isolated from the short CPU job queue.
- **FR-013**: The background queue MUST support main-thread continuations that are explicitly drained by product update loops or tests.
- **FR-014**: The system MUST expose diagnostics for job lifecycle, worker activity, fence state, dependency state, wait state, violations, and shutdown state.
- **FR-015**: The system MUST integrate worker thread naming and job execution scopes with the existing Nullus profiling API.
- **FR-016**: The system MUST provide safety/debug scopes for disallowing synchronous waits in sections that cannot safely block.
- **FR-017**: The system MUST provide deterministic test hooks for reset, synchronous draining, and diagnostic snapshots without requiring generated file edits.
- **FR-018**: The system MUST provide binding-ready public contracts for opaque handles, scheduling parameters, worker count query, completion query, completion wait, and callback scheduling.
- **FR-019**: Binding-ready contracts MUST avoid exposing STL-owning ABI across C-compatible surfaces and MUST version public structs that external callers may pass by value.
- **FR-020**: The system MUST document migration boundaries for existing editor, asset, rendering, and tooling thread paths and migrate at least one low-risk consumer after the core scheduler is validated.
- **FR-021**: The system MUST preserve Editor and Game runtime viability during all staged migration slices.
- **FR-022**: The system MUST not hand-edit files under `Runtime/*/Gen/`.
- **FR-023**: The system MUST include automated tests for scheduler correctness, dependency ordering, fence completion, batched jobs, parallel-for coverage, background continuations, diagnostics, and binding-ready error handling.
- **FR-024**: The system MUST clearly document unsupported Unity-specific areas that are not meaningful in Nullus yet, such as managed C# job producer integration, until the corresponding runtime layer exists.

### Key Entities *(include if feature involves data)*

- **JobSystem**: Runtime service that owns worker pools, queues, scheduling, draining, diagnostics, and shutdown policy.
- **Job Handle/Fence**: Copyable completion token that identifies scheduled work and its generation without owning job payload memory directly.
- **Job Group**: Internal completion unit containing one or more jobs, pending counters, dependency state, priority, and diagnostics metadata.
- **Job Record**: A callable payload plus user data, name, priority, safety policy, and optional iteration index.
- **Job Queue**: Worker-visible structure that orders normal, high-priority, dependency-ready, and waiting-help work.
- **Batch Dispatcher**: Accumulates schedule requests before kicking them into worker-visible queues.
- **Parallel Range Plan**: Block range or work-stealing metadata that maps iteration work to worker jobs.
- **Background Job Queue**: Separate long-running work lane with its own worker policy and continuation queue.
- **Main-Thread Continuation**: A callback scheduled by background work and executed only when the main-thread drain entrypoint is called.
- **Safety Handle/Scope**: Diagnostic state that marks whether synchronous waiting is allowed and reports violations.
- **Job Diagnostic Snapshot**: Immutable copy of current and recent scheduler state for tests, tools, and profiler-facing diagnostics.
- **Binding Contract**: Versioned opaque-handle API surface intended for future script or generated binding layers.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Scheduler unit tests cover single-job schedules, dependency waits, repeated fence queries, stale-handle detection, failure propagation, and worker/thread shutdown with zero missed executions, duplicate executions, or leaked worker threads in the focused test run.
- **SC-002**: Dependency tests demonstrate 100% ordered completion for chains, fan-in groups, and different-jobs-concurrent groups across at least one single-worker configuration and one multi-worker configuration.
- **SC-003**: Parallel-for tests process 100% of requested indices exactly once across zero, small, uneven, and large iteration counts, including work-stealing workloads.
- **SC-004**: Background queue tests demonstrate deterministic main-thread continuation draining, cross-queue dependency helping, failure propagation, cancellation cleanup, and clean shutdown without hangs in the focused test run.
- **SC-005**: Diagnostic tests capture job lifecycle, worker names, disallowed wait violations, and intentionally cleared fences with deterministic assertions.
- **SC-006**: Binding-ready contract tests schedule and complete jobs through opaque handles and report deterministic errors for invalid handles, null callbacks, and version mismatches.
- **SC-007**: At least one existing ad hoc threaded Nullus consumer is migrated to the shared JobSystem while its existing tests continue to pass and its product behavior is unchanged.
- **SC-008**: Relevant validation includes `NullusUnitTests` focused JobSystem tests and documents exact commands or runtime evidence used before any completion claim.

## Assumptions

- "Full Unity-style JobSystem" means covering Unity 2018-style native scheduling concepts in a Nullus-native implementation, not copying Unity source files or preserving Unity's exact private implementation details.
- Nullus does not currently expose a managed C# job runtime; therefore managed bindings are represented by versioned native binding-ready contracts and tests until a script runtime exists.
- Unity-specific managed C# job producers, `NativeArray`/`AtomicSafetyHandle`, JobsDebugger UI, and Unity's exact private/manual `JobFence` APIs are out of scope for this native foundation slice; Nullus native C++ `IJob`/`IJobParallelFor` adapters, copyable `JobHandle` fences, and multi-dependency fan-in are supported.
- The first production placement is `Runtime/Base/Jobs` so Rendering, Engine, Editor, Game, and tooling can all depend on the scheduler without introducing Rendering-owned infrastructure.
- The first scheduler target is portable C++20 using standard threading primitives where possible; OS-specific priority, affinity, and semaphore optimizations can be introduced behind platform abstractions after correctness is established.
- Existing threaded rendering lifecycle ownership remains authoritative; JobSystem integration with rendering must not weaken RenderDoc or backend-specific validation expectations.
- The implementation will be staged, but each stage must leave Editor and Game runnable and must include tests before behavior-changing code.
