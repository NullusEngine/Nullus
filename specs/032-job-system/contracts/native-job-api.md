# Contract: Native Job API

## Scope

This contract defines the C++ API shape for Nullus runtime users. Names may be refined during implementation, but behavior must remain stable.

## Namespace

`NLS::Base::Jobs`

## Lifecycle

```cpp
struct JobSystemConfig
{
    uint32_t workerCount = kAutoJobWorkerCount; // kAutoJobWorkerCount resolves to a safe default.
    uint32_t backgroundWorkerCount = 1;
    bool enableDiagnostics = false;
};

enum class JobSystemShutdownMode
{
    DrainAcceptedWork,
    Immediate
};

bool InitializeJobSystem(const JobSystemConfig& config);
bool TryInitializeJobSystem(const JobSystemConfig& config);
void ShutdownJobSystem(JobSystemShutdownMode mode);
bool IsJobSystemInitialized();
uint32_t GetJobWorkerCount();
bool ExecuteOneJobQueueJob();
```

`TryInitializeJobSystem` returns true only when that call created the runtime and therefore owns the matching shutdown responsibility. It returns false if another owner already initialized the JobSystem or shutdown is in progress. Callers that use `TryInitializeJobSystem` must call `ShutdownJobSystem` only after a true return; callers that just need an initialized runtime should use `InitializeJobSystem`.

`workerCount = 0` is an explicit synchronous/test mode. It creates no short CPU worker threads; callers must progress accepted short work by completion helping, `ExecuteOneJobQueueJob`, background dependency helping, or shutdown drain. Excessive explicit worker counts are clamped to a bounded runtime limit before worker threads are started.

## Handles and Priority

```cpp
struct JobHandle
{
    uint64_t id = 0;
    uint32_t generation = 0;
};

enum class JobPriority : uint8_t
{
    Normal,
    High
};

enum class JobSafetyPolicy : uint8_t
{
    MaySyncWait,
    GuaranteedNoSyncWait
};
```

Contract rules:

- Default `JobHandle{}` is complete.
- Copying a handle allocates no memory.
- Completing the same valid handle more than once is safe.
- Stale handles are detected and never dereference recycled internal groups.

## Scheduling

```cpp
using JobFunction = void(*)(void* userData);
using JobForEachFunction = void(*)(void* userData, uint32_t index);

struct JobScheduleDesc
{
    JobFunction function = nullptr;
    void* userData = nullptr;
    JobFunction cancelFunction = nullptr;
    void* cancelUserData = nullptr;
    JobHandle dependency;
    JobPriority priority = JobPriority::Normal;
    JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
    const char* debugName = nullptr;
};

JobHandle ScheduleJob(const JobScheduleDesc& desc);
JobHandle ScheduleJobDepends(const JobScheduleDesc& desc, JobHandle dependency);
JobHandle ScheduleDifferentJobsConcurrent(
    const JobScheduleDesc* jobs,
    size_t jobCount,
    JobHandle dependency = {});
JobHandle ScheduleMultiDependencyJob(
    JobBatchDispatcher& dispatcher,
    const JobHandle* dependencies,
    size_t dependencyCount);

struct IJob {};

struct JobScheduleOptions
{
    JobHandle dependency;
    JobPriority priority = JobPriority::Normal;
    JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
    const char* debugName = nullptr;
};

template<typename TJob>
JobHandle Schedule(TJob job, const JobScheduleOptions& options = {});
```

Contract rules:

- Null job callbacks are rejected except where a dependency-only group is explicitly documented.
- Mixed grouped scheduling rejects any null callback instead of silently dropping it.
- Scheduled jobs execute at most once.
- Dependency jobs complete before dependent job callbacks run.
- If a callback reports an exception boundary failure, the group completes in failed state and diagnostics record the failure.
- Foreground `cancelFunction`, when provided, is called at most once for queued or dependency-waiting payloads that are cancelled by immediate shutdown or by failed or cancelled dependencies.
- Foreground `cancelFunction` is not called once the foreground callback starts, including success and exception/failure paths; started callbacks own their own cleanup.
- Multi-dependency fan-in returns a fence that completes only after all supplied dependencies complete.
- Native C++ `IJob` adapters copy the supplied job object at schedule time and call `Execute()` through `ScheduleJob`; they are not Unity managed C# producers.

## Completion and Query

```cpp
void Complete(JobHandle& handle);
void CompleteNoClear(JobHandle handle);
void CompleteAll(JobHandle* handles, size_t handleCount);
bool IsCompleted(JobHandle handle);
void ClearWithoutSync(JobHandle& handle);
bool HasBeenSynced(JobHandle handle);
```

Contract rules:

- `Complete(JobHandle&)` clears the handle after terminal completion. If sync wait is disallowed for the current thread and the handle is still pending, completion records a violation and leaves the handle set.
- `CompleteNoClear(JobHandle)` preserves the caller's copy.
- `HasBeenSynced(JobHandle)` returns true for default handles and terminal completed/failed handles, including handles completed through `CompleteNoClear`.
- Completed handles are retained in a bounded retired-handle history for stale-handle detection. Very old completed binding handles may age out and become invalid for binding queries; native unknown handles remain non-blocking.
- `CompleteAll` accepts null or zero-count arrays as no-op.
- `IsCompleted` on a pending batch handle schedules that handle's pending group before returning the completion state, analogous to Unity-style batched job polling while remaining scoped to Nullus dispatcher/handle visibility instead of exposing Unity's global `ScheduleBatchedJobs`.
- `ClearWithoutSync` records an intentional ownership handoff when diagnostics are enabled.
- Completion may execute jobs in the waited fence/dependency chain. Unrelated queued work is eligible during a wait only when marked `JobSafetyPolicy::GuaranteedNoSyncWait`.
- `GuaranteedNoSyncWait` jobs execute inside a no-sync-wait scope; attempts to call `Complete` from such callbacks record `SyncWaitDisallowed` and return without blocking.

## Parallel-For

```cpp
struct JobForEachDesc
{
    JobForEachFunction function = nullptr;
    void* userData = nullptr;
    uint32_t iterationCount = 0;
    uint32_t batchSize = 1;
    JobFunction combineFunction = nullptr;
    JobFunction cancelFunction = nullptr;
    JobHandle dependency;
    JobPriority priority = JobPriority::Normal;
    JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
    const char* debugName = nullptr;
};

JobHandle ScheduleJobForEach(const JobForEachDesc& desc);

struct IJobParallelFor {};

struct JobParallelForScheduleOptions
{
    JobHandle dependency;
    uint32_t batchSize = 1;
    JobPriority priority = JobPriority::Normal;
    JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
    const char* debugName = nullptr;
};

template<typename TJob>
JobHandle ScheduleParallelFor(
    TJob job,
    uint32_t iterationCount,
    const JobParallelForScheduleOptions& options = {});
```

Contract rules:

- Each index in `[0, iterationCount)` is processed once.
- `batchSize` controls the requested inner-loop batch size; zero is treated as one.
- `combineFunction`, when present, runs after all iteration callbacks.
- `cancelFunction`, when present, runs if the parallel-for group is cancelled before successful combine cleanup.
- Zero iterations complete without invoking the iteration callback.
- Iteration counts that exceed the native `int` range are rejected instead of truncating.
- Batch sizes that exceed the native `int` range are rejected instead of truncating.
- `JobRange` block and work-stealing helpers are Unity-inspired Nullus policies, not Unity heuristic-compatible contracts. Nullus bounds range job counts to the available execution lanes and keeps the heuristic replaceable behind these helper APIs.
- Native C++ `IJobParallelFor` adapters copy the supplied job object at schedule time, create one copied job object per scheduled range shard, and call `Execute(uint32_t index)` through the same work-stealing range policy used by `ScheduleJobForEach`.
- Mutable fields inside an `IJobParallelFor` job are shard-local; external objects referenced by the job remain caller-owned shared state and require synchronization or dependency ordering.

## Batch Dispatcher

```cpp
class JobBatchDispatcher
{
public:
    JobBatchDispatcher(int jobsPerBatch = kManualJobKick);
    ~JobBatchDispatcher();
    JobHandle AddJob(const JobScheduleDesc& desc);
    JobHandle AddForEach(const JobForEachDesc& desc);
    JobHandle AddMultiDependency(const JobHandle* dependencies, size_t dependencyCount);
    void Kick();
    bool Empty() const;
};
```

Contract rules:

- Added groups are not worker-visible until dispatcher `Kick`, dispatcher destruction, auto-kick, multi-dependency fan-in creation, per-handle completion, or per-handle completion query makes them visible.
- `JobBatchDispatcher` instances are caller-owned batching helpers and are not thread-safe; callers must serialize access when sharing one dispatcher across producer threads.
- Destroying a dispatcher kicks any remaining pending work.
- Non-manual dispatchers auto-kick when their configured batch threshold is reached.
- Completing a handle from an un-kicked batch kicks the pending group before waiting.
- `AddMultiDependency` and `ScheduleMultiDependencyJob` kick the dispatcher before creating the fan-in fence so the supplied pending inputs can progress.
- A fan-in made entirely from repeated copies of one handle normalizes back to that handle after the kick.
- Shutdown drain kicks accepted but un-kicked batch groups so process exit does not hang on `PendingKick` work.

## Background Queue

```cpp
using MainThreadContinuationFunction = void(*)(void* userData);

struct BackgroundJobDesc
{
    JobFunction function = nullptr;
    void* userData = nullptr;
    JobFunction cancelFunction = nullptr;
    void* cancelUserData = nullptr;
    JobHandle dependency;
    const char* debugName = nullptr;
};

struct MainThreadContinuationDesc
{
    MainThreadContinuationFunction function = nullptr;
    void* userData = nullptr;
    JobHandle dependency;
    const char* debugName = nullptr;
};

JobHandle ScheduleBackgroundJob(const BackgroundJobDesc& desc);
bool ScheduleMainThreadContinuation(const MainThreadContinuationDesc& desc);
uint32_t DrainMainThreadContinuations(uint32_t maxContinuations = 0);
```

Contract rules:

- Background jobs do not run on short CPU job workers.
- Main-thread continuations run only from `DrainMainThreadContinuations`.
- Shutdown never invokes main-thread continuations implicitly; owners that require follow-up work must drain it explicitly before stopping the JobSystem.
- Dependency-aware main-thread continuations are a Nullus extension; Unity 2018 is only the scheduling reference.
- Continuation order is deterministic among ready continuations.
- `DrainMainThreadContinuations` promotes ready continuations once per drain call and then consumes a ready queue up to the requested budget.
- Background `cancelFunction`, when provided, is called at most once for queued or dependency-waiting work cancelled by immediate shutdown, failed or cancelled dependencies before execution, or callback exception/failure after execution starts.
- Background `cancelFunction` is not called after a successful callback.
