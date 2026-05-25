# Research: Nullus Unity-Style Native JobSystem

## Decision: Use Unity 2018 as a semantic reference, not a source transplant

**Rationale**: Unity's `Runtime/Jobs` implementation is deeply tied to Unity allocators, macro gates, profiler infrastructure, managed bindings, and platform thread wrappers. Nullus already has its own runtime module graph, CMake globbing, profiler, and test style. Reproducing the relevant native scheduling semantics lets Nullus keep Unity-style concepts while avoiding brittle source-level dependencies.

**File-level benchmark points**:

| Unity 2018 reference | Nullus counterpart | Difference |
| --- | --- | --- |
| `Runtime/Jobs/JobBatchDispatcher.h` batched dispatch and fan-in concepts | `Runtime/Base/Jobs/JobBatchDispatcher.*` | Nullus keeps batching per dispatcher/per handle and does not expose Unity's global `ScheduleBatchedJobs`. |
| `Runtime/Jobs/WorkStealingRange.h` and `Runtime/Jobs/BlockRangeJob.h` range partitioning concepts | `Runtime/Base/Jobs/JobRange.*` | Nullus uses replaceable native heuristics bounded by execution lanes, not Unity heuristic compatibility. |
| `Runtime/Jobs/ScriptBindings/JobHandle.bindings.cs` and managed `IJob*` producers | `Runtime/Base/Jobs/JobBindings.*` | Nullus provides a C-compatible native ABI surface, not managed C# job producers or `NativeArray` safety. |
| Unity worker/profiler integration under `Runtime/Jobs` | `Runtime/Base/Jobs/JobQueue.*`, `BackgroundJobQueue.*`, `JobDiagnostics.*` | Nullus integrates with existing `NLS_PROFILE_*` scopes and bounded diagnostic snapshots. |

**Alternatives considered**:

- Copy Unity file structure directly: rejected because it imports allocator/thread/macro assumptions that do not exist in Nullus.
- Build a generic thread pool only: rejected because it does not satisfy fences, dependency groups, batched jobs, work stealing, background queue, safety, and bindings requested by the feature.

**Additional industry references**:

- Unreal Engine Tasks System documents prerequisites, nested tasks, waiting, and task events as the closest engine-level DAG comparison. Nullus matches the explicit dependency/fan-in intent but keeps the first API in `Runtime/Base/Jobs` instead of adopting Unreal's task naming or gameplay-thread abstractions.
- oneTBB task scheduler documentation is the closest general C++ scheduler comparison for worker-managed task execution and work stealing. Nullus uses a correctness-first mutex/condition-variable scheduler with replaceable range helpers instead of oneTBB's mature arena and scheduler machinery.
- These references are comparison points only; the feature does not claim Unreal or oneTBB API compatibility.

## Decision: Place the subsystem in `Runtime/Base/Jobs`

**Rationale**: `NLS_Base` is the lowest common runtime dependency and already owns profiling primitives. A scheduler placed there can be used by Core, Rendering, Engine, UI, Project Editor/Game, and tests without circular dependencies.

**Alternatives considered**:

- `Runtime/Core/Jobs`: rejected because Rendering and Engine can use Core, but some Base-level systems and future platform utilities should not depend upward.
- `Runtime/Engine/Jobs`: rejected because editor/tooling/rendering infrastructure should not depend on gameplay engine code.
- `Runtime/Rendering/Jobs`: rejected because JobSystem is a general runtime service.

## Decision: Start with correctness-first synchronized queues

**Rationale**: Unity uses specialized atomic queues and stacks. Nullus needs correct dependency semantics before lock-free optimization. A mutex/condition-variable queue can pass deterministic tests, preserve API contracts, and leave internals replaceable behind `JobQueue`.

**Alternatives considered**:

- Lock-free queue from day one: rejected for first slice because ABA, memory reclamation, and dependency wakeup bugs would obscure API correctness.
- Per-subsystem worker pools: rejected because it keeps current fragmentation.

## Decision: Use generation-checked handles instead of raw group pointers

**Rationale**: Unity's `JobGroupID` combines a group pointer and generation. Nullus should preserve cheap copyable handles but not expose raw internal pointers through public APIs. A compact id plus generation allows stale-handle detection and binding-ready opaque handles.

**Alternatives considered**:

- `std::shared_ptr<JobGroup>` handles: rejected because it changes lifetime semantics, increases atomic refcount traffic, and is unsuitable for C ABI.
- Raw pointer handles: rejected because binding-ready contracts need deterministic stale-handle errors.

## Decision: Main-thread helping is required for fence completion

**Rationale**: Unity's `SyncFence` may execute eligible jobs while waiting. This prevents deadlocks and idle waits when the waiting thread can make progress on the dependency chain. Nullus should implement the same user-visible behavior for short CPU jobs.

**Alternatives considered**:

- Passive condition-variable waits only: rejected because it can deadlock if all workers are blocked behind dependencies that the waiter could execute.
- Always execute any queued job while waiting: rejected because recursive wait safety requires dependency-aware eligibility.

## Decision: Keep background jobs isolated from short CPU jobs

**Rationale**: Unity distinguishes background jobs for slow or IO-heavy work. Nullus currently has editor private background workers; merging those into the short CPU worker pool would risk starving frame-critical jobs. A separate background queue with main-thread continuation draining preserves product behavior.

**Alternatives considered**:

- One worker pool with low priority tags: deferred until priority/affinity support is mature; a separate first implementation is clearer and safer.
- Leave editor workers alone forever: rejected because the JobSystem scope includes migration away from fragmented ad hoc queues.

## Decision: Binding-ready native API, not managed C# jobs in this feature

**Rationale**: Nullus does not currently provide a Unity-compatible managed runtime, C# job producer reflection, or NativeArray safety model. The correct deliverable is a stable native opaque-handle contract that future script or generated bindings can call without changing scheduler internals.

**Alternatives considered**:

- Implement C# bindings now: rejected because no runtime integration layer exists in the current project.
- Skip bindings entirely: rejected because the requested scope explicitly includes Unity-style bindings.

## Decision: Diagnostics are testable runtime data, not just logs

**Rationale**: Job bugs are intermittent. Tests and tools need structured snapshots of job ids, states, dependencies, worker names, violations, and shutdown state. Logs alone are hard to assert and too lossy for safety checks.

**Alternatives considered**:

- Log-only diagnostics: rejected because unit tests need deterministic assertions.
- Always-on heavyweight history: rejected because release-like builds should avoid unnecessary overhead; diagnostics can be gated and bounded.

## Decision: First consumer migration should be low-risk Editor background task tracking

**Rationale**: `Project/Editor/Core/EditorActions` has an isolated background queue with clear schedule, worker, shutdown, and completion counters. It is a suitable migration candidate after core scheduler tests pass. Rendering migration should wait because existing threaded rendering has backend validation obligations.

**Alternatives considered**:

- Migrate threaded rendering first: rejected because rendering needs RenderDoc/backend validation and should not be bundled with scheduler bring-up.
- Migrate asset import internals first: deferred because asset import has broader persistence and progress semantics.
