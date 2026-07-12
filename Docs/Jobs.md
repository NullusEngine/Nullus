# JobSystem

Nullus JobSystem provides a Unity-style native scheduling surface under `Runtime/Base/Jobs`.
It follows Unity 2018 scheduling concepts in a Nullus-native C++ implementation without copying Unity source or claiming managed C# job support.

## Native Scheduling

Use `InitializeJobSystem` once before scheduling work and `ShutdownJobSystem` at owner shutdown. `TryInitializeJobSystem` is the ownership-aware variant: it returns true only when the call created the runtime, and returns false if a JobSystem is already alive or shutting down. Code that uses `TryInitializeJobSystem` must call `ShutdownJobSystem` only when that call returned true. `JobHandle{}` is always complete.

```cpp
NLS::Base::Jobs::JobSystemConfig config;
config.workerCount = 4; // kAutoJobWorkerCount chooses a hardware-based default.
config.backgroundWorkerCount = 1;
NLS::Base::Jobs::InitializeJobSystem(config);
```

Short CPU work uses `ScheduleJob`, typed native `IJob` adapters, `ScheduleDifferentJobsConcurrent`, `ScheduleJobForEach`, typed native `IJobParallelFor` adapters, or `JobBatchDispatcher`.
Call `Complete` to wait and clear the handle. `CompleteNoClear` keeps the caller's copy valid for later queries, and `HasBeenSynced` reports true once that handle reaches a terminal state.
Parallel-for jobs accept a caller-controlled `batchSize` and reject ranges larger than the native range planner can represent.
Foreground jobs may provide a cancel callback for payload cleanup. The callback is invoked at most once for queued or dependency-waiting work that is cancelled by immediate shutdown or by a failed or cancelled dependency. Once a foreground callback starts, its cancel callback is not invoked for either success or failure.
The typed native adapters copy the supplied job object at schedule time and call `Execute()` or `Execute(uint32_t index)` from the underlying scheduler. `IJobParallelFor` creates one copied job object per scheduled range shard, so mutable fields are not shared between shards; shared external objects referenced by the job still need caller-managed synchronization. They are C++ convenience wrappers over the native descriptors, not Unity managed C# producers.
`JobBatchDispatcher` supports explicit per-dispatcher kick, destructor kick, configured auto-kick thresholds, and multi-dependency fan-in handles. Dispatcher instances are caller-owned batching helpers and are not thread-safe; callers that add or kick from multiple producers must serialize access.
`IsCompleted` and `Complete` both make pending batch jobs worker-visible before querying or waiting, analogous to Unity's handle-triggered batching while staying scoped to Nullus dispatcher/handle visibility.
Creating a multi-dependency fan-in through `AddMultiDependency` or `ScheduleMultiDependencyJob` kicks the dispatcher's pending inputs before creating the fence. A fan-in made entirely from repeated copies of one handle normalizes back to that same handle after the kick, so the dependency can still progress without allocating an extra fence. Completing a handle from an un-kicked dispatcher kicks that pending group before waiting. Nullus does not expose Unity's global `ScheduleBatchedJobs`; pending batch visibility is per dispatcher, fan-in creation, or per handle.

`workerCount = kAutoJobWorkerCount` resolves to a safe hardware-based default. Excessive explicit worker counts are clamped to a bounded runtime limit to avoid accidental resource exhaustion. `workerCount = 0` is an explicit synchronous/test mode; work then progresses through `Complete`, `ExecuteOneJobQueueJob`, background dependency helping, or shutdown drain.

Completion may run eligible jobs on the calling thread. To avoid recursive wait hazards, waiting only helps jobs for the waited fence/dependency chain or jobs marked `JobSafetyPolicy::GuaranteedNoSyncWait`. A `GuaranteedNoSyncWait` callback is executed inside a no-sync-wait scope; attempts to call `Complete` from that callback record a violation and return without blocking.
Completed native and binding handles remain queryable through a bounded retired-handle history while the queue is alive. Cross-queue dependency waiters retain terminal status until the waiter retires, so background/foreground fan-in does not depend on an unbounded global completed-handle cache.

## Known Limits

`ScheduleJobForEach` rejects iteration counts or batch sizes that exceed the native `int` range planner limit. Very large workloads should be split by the caller until a wider range planner is introduced.
The current scheduler favors correctness and deterministic cleanup over maximal throughput. Foreground and background queues still use central mutex-protected registries; future work can replace hot queue paths with lock-free or sharded queues after the semantics are stable.

Foreground/background cross-queue dependency promotion uses explicit notifications plus a bounded timed fallback. Correctness does not rely on the fallback, but heavy cross-queue traffic can still pay up to the current 100 ms polling latency before a missed notification is rechecked.

## Memory and Data Safety

Job callbacks receive raw `void* userData`; the caller owns that payload and must keep it alive until the associated handle reaches a terminal state or its cancel/cleanup callback has run. Cancel and terminal cleanup callbacks can run during completion or shutdown, so payload ownership must not assume a particular thread.

Nullus does not provide Unity `NativeArray`, `AtomicSafetyHandle`, or managed container race checks in this native layer. Shared mutable state must be protected by the caller with atomics, locks, immutable snapshots, or dependency ordering. `JobSafetyPolicy` only describes synchronous-wait safety for scheduler helping; it does not prove memory safety or data-race freedom.

Job callbacks should not throw. If a foreground, background, combine, cancel, or cleanup callback throws, the scheduler catches the exception boundary, records `JobViolationKind::CallbackException`, and moves the handle through the documented failure or cancellation path.

## Background Work

Longer editor or IO-heavy work should use `ScheduleBackgroundJob`. Main-thread follow-up work should use `ScheduleMainThreadContinuation` and execute only from `DrainMainThreadContinuations`. Ready continuations are promoted into a dedicated ready queue so each drain consumes ready work instead of repeatedly scanning every blocked continuation. The editor update loop drains a bounded continuation budget once per frame after delayed editor actions. Shutdown does not implicitly execute main-thread continuations; owners should drain required continuations before stopping the JobSystem.
Background jobs may provide a cancel callback so queued or dependency-waiting payloads can be released during immediate shutdown. Background cancel callbacks are invoked at most once for queued or dependency-waiting work cancelled by immediate shutdown, by a failed or cancelled dependency before execution, or by a callback exception after execution starts. They are not invoked for successful callbacks.
Unity 2018's background scheduling lane is used as a semantic reference, not as a source transplant. The local mapping is:

- Unity `Runtime/Jobs/JobBatchDispatcher.h` maps to `Runtime/Base/Jobs/JobBatchDispatcher.*` for batched dispatch and fan-in concepts.
- Unity `Runtime/Jobs/WorkStealingRange.h` and `BlockRangeJob.h` map to `Runtime/Base/Jobs/JobRange.*` for partitioned work.
- Unity managed `JobHandle` producers map to the native opaque-handle contract in `Runtime/Base/Jobs/JobBindings.*`.
- Unity worker/profiler integration maps to `JobQueue.*`, `BackgroundJobQueue.*`, and `JobDiagnostics.*`.

Unity 2018 background jobs do not support sync fences; Nullus intentionally exposes waitable background handles so native engine systems can compose foreground fences, background work, and main-thread continuations through one dependency vocabulary. Nullus keeps the background worker count configurable and treats dependency-aware continuations as a Nullus-native extension. The implementation remains correctness-first and synchronized; it does not claim Unity allocator, managed container safety, or API compatibility.

The editor background task path in `EditorActions::TrackBackgroundTask` now delegates through `EditorBackgroundTaskTracker` to the shared JobSystem background queue instead of owning private worker threads.
`EditorBackgroundTaskTracker` enforces the editor task capacity, rejects new work after stop, tracks submitted handles, and completes those handles during destruction even when the JobSystem is owned by the application.

## Diagnostics

Set `JobSystemConfig::enableDiagnostics` to collect bounded snapshots through `CopyJobDiagnosticSnapshot`. `DisallowJobSyncWaitScope` records structured sync-wait violations when code attempts a blocking `Complete` inside a no-wait scope.

## Binding API

`JobBindings.h` exposes a minimal binding-ready C-compatible subset with `structSize`, `NLS_JOB_BINDING_VERSION`, opaque handle ids, named priority/safety constants, and explicit status codes. Non-default query, complete, and clear calls return `NLS_JOB_STATUS_NOT_INITIALIZED` when the runtime is stopped. Completing a cancelled binding handle returns `NLS_JOB_STATUS_CANCELLED` and clears the handle. Binding ABI v1 maps a completion attempt that cannot reach terminal state to `NLS_JOB_STATUS_FAILED` and leaves the handle set. Binding structs are plain ABI types and do not own STL objects. Native C++ JobSystem headers, including diagnostics/range helpers and the batch dispatcher, are same-toolchain engine SDK APIs rather than a stable cross-compiler ABI surface.

## Validation Scope

Current validation commands in the feature bundle are Windows Debug commands. Linux and macOS remain portability targets through standard C++ primitives, but they are not proven by a Windows-only test run unless those platform commands are run separately.

## Unsupported Unity Areas

This feature does not include Unity managed C# job producers, C# reflection bindings, `NativeArray`/`AtomicSafetyHandle`, JobsDebugger UI, or Unity's exact private/manual `JobFence` APIs. Nullus does provide native C++ `IJob`/`IJobParallelFor` adapters, copyable `JobHandle` fences, and multi-dependency fan-in. Unity-specific scripting, container, and tooling layers require future work above the native scheduler.
