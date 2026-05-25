# Contract: Diagnostics, Safety, and Profiling

## Scope

Diagnostics provide structured state for tests and tools. Logs may be emitted, but logs are not the primary contract.

## Snapshot API

```cpp
namespace NLS::Base::Jobs
{
enum class JobLifecycleState : uint8_t
{
    Created,
    WaitingForDependencies,
    Queued,
    Running,
    Completed,
    Cancelled,
    Failed,
    Count
};

enum class JobViolationKind : uint8_t
{
    SyncWaitDisallowed,
    InvalidHandle,
    NullCallback,
    ShutdownSchedulingRejected,
    StaleHandle,
    ClearedWithoutSync,
    CallbackException
};

struct JobDiagnosticRecord
{
    uint64_t id = 0;
    uint32_t generation = 0;
    JobLifecycleState state = JobLifecycleState::Created;
    std::string debugName;
    std::string workerName;
    uint64_t dependencyCount = 0;
};

struct JobViolationRecord
{
    JobViolationKind kind = JobViolationKind::InvalidHandle;
    uint64_t jobId = 0;
    std::string message;
    std::string threadName;
};

struct JobDiagnosticSnapshot
{
    bool initialized = false;
    uint32_t workerCount = 0;
    uint64_t queuedJobCount = 0;
    uint64_t runningJobCount = 0;
    uint64_t completedJobCount = 0;
    uint64_t failedJobCount = 0;
    uint64_t droppedHistoryCount = 0;
    std::vector<JobDiagnosticRecord> recentJobs;
    std::vector<JobViolationRecord> recentViolations;
};

JobDiagnosticSnapshot CopyJobDiagnosticSnapshot();
void ResetJobDiagnosticsForTesting();
}
```

## Safety Scopes

```cpp
namespace NLS::Base::Jobs
{
class DisallowJobSyncWaitScope
{
public:
    DisallowJobSyncWaitScope();
    ~DisallowJobSyncWaitScope();
};

bool IsJobSyncWaitDisallowedForCurrentThread();
}
```

Contract rules:

- Nested disallow scopes are supported.
- Destroying scopes restores the previous thread-local state.
- Completing a handle while disallowed records `SyncWaitDisallowed`.
- Job, cancel, cleanup, or continuation callbacks that cross an exception boundary record `CallbackException`.
- Test hooks can assert violations without relying on logging.

## Profiling

Contract rules:

- Worker threads register stable profiler thread names such as `Job Worker 0`.
- Background worker threads register distinct names such as `Background Job Worker 0`.
- Each job callback is wrapped in a named profiling scope when profiling is enabled.
- Profiling must compile away through existing `NLS_PROFILE_*` macros when profiling is disabled.

## History Boundaries

- Diagnostic histories are bounded.
- Evicted records increment `droppedHistoryCount`.
- Snapshot creation copies data and does not expose internal locks, queues, or mutable records.
