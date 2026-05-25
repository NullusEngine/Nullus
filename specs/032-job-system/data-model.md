# Data Model: Nullus Unity-Style Native JobSystem

## JobSystem

**Purpose**: Owns scheduler lifetime, short-job workers, background workers, queues, diagnostics, test hooks, and shutdown behavior.

**Key Fields**:

- `initialized`: whether scheduling is currently accepted.
- `workerCount`: number of short CPU workers.
- `backgroundWorkerCount`: number of background workers.
- `shutdownMode`: immediate, drain accepted work, or reject new work.
- `diagnosticsEnabled`: whether structured job snapshots are recorded.

**Relationships**:

- Owns one short `JobQueue`.
- Owns one `BackgroundJobQueue`.
- Owns a handle registry for `JobGroup` generation validation.
- Publishes `JobDiagnosticSnapshot` copies.

## JobHandle

**Purpose**: Copyable completion token visible to callers and binding-ready APIs.

**Key Fields**:

- `id`: opaque scheduler-assigned group id.
- `generation`: validates that the handle still refers to the intended group.

**Validation Rules**:

- A default handle is already complete.
- A stale handle must report invalid or completed according to the public API contract, never dereference freed state.
- Completing an already-complete handle must be a safe no-op.

## JobGroup

**Purpose**: Internal completion unit for one or more jobs sharing one handle.

**Key Fields**:

- `id` and `generation`
- `pendingJobCount`
- `pendingDependencyCount`
- `priority`
- `state`: created, queued, running, completed, cancelled, failed
- `dependencies`: handles this group must observe before jobs are runnable
- `dependents`: groups to wake when this group completes
- `jobs`: job records owned by the group
- `completeJob`: optional combine callback

**State Transitions**:

1. Created
2. WaitingForDependencies or Ready
3. Queued
4. Running
5. Completed, Cancelled, or Failed
6. Recycled only after stale handles cannot be confused with new generation

## JobRecord

**Purpose**: A callable unit of work.

**Key Fields**:

- `function`: callback entrypoint
- `userData`: caller-owned payload pointer or wrapper
- `debugName`
- `forEachIndex`: optional iteration index
- `safetyPolicy`: whether synchronous waits are allowed
- `state`: queued, running, completed, failed

**Validation Rules**:

- Null callbacks are rejected for public scheduling APIs unless the specific API explicitly accepts a dependency-only group.
- A job must run at most once.
- Exceptions are caught at scheduler boundaries and surfaced through diagnostics.

## JobBatchDispatcher

**Purpose**: Accumulates groups before making them worker-visible.

**Key Fields**:

- `pendingGroups`
- `kickedCount`
- `lastKickFrameOrSequence`

**Validation Rules**:

- A batched group is not executable until kicked.
- Completing a handle may kick or drain required work according to the native API contract.

## ParallelRangePlan

**Purpose**: Describes bounded work partitioning for parallel-for jobs.

**Key Fields**:

- `iterationCount`
- `batchSize`
- `minimumIterationsPerJob`
- `workerCountUsed`
- `ranges`: start and count pairs

**Validation Rules**:

- Ranges must cover each requested iteration exactly once.
- Empty iteration counts produce an already-complete handle or zero executable jobs.
- Range count must be bounded by workload and configured worker count.

## WorkStealingRange

**Purpose**: Runtime metadata for uneven parallel-for workloads.

**Key Fields**:

- `batchSize`
- `jobCount`
- `totalIterationCount`
- `phaseCount`
- `batchesPerPhase`
- per-job atomic start/end state
- per-job phase state

**Validation Rules**:

- Each successful range claim returns a non-overlapping half-open interval.
- Stealing must not skip later phases.
- All iteration indices in `[0, totalIterationCount)` are claimable exactly once.

## BackgroundJobQueue

**Purpose**: Separate lane for low-priority long-running work and IO-heavy tasks.

**Key Fields**:

- `workerCount`
- `pendingJobs`
- `runningJobCount`
- `completedJobCount`
- `continuationQueue`
- `shutdownRequested`

**Relationships**:

- Can schedule `MainThreadContinuation` records.
- May expose handles compatible with JobSystem completion queries.

## MainThreadContinuation

**Purpose**: Callback that executes only when an owner explicitly drains the continuation queue.

**Key Fields**:

- `function`
- `userData`
- `debugName`
- `dependency`
- `state`

**Validation Rules**:

- Continuations do not run on background worker threads.
- Continuations with unfinished dependencies remain queued.
- Drain order is deterministic among ready continuations.

## JobSafetyScope

**Purpose**: Tracks whether the current thread may synchronously wait on job handles.

**Key Fields**:

- `syncWaitDisallowedDepth`
- `threadName`
- `activeJobHandle`

**Validation Rules**:

- Entering nested disallow scopes increments depth.
- Leaving scopes decrements depth without underflow.
- Completion attempts while depth is non-zero produce a structured violation.

## JobDiagnosticSnapshot

**Purpose**: Immutable structured copy of scheduler state for tests, tools, and profiling-oriented panels.

**Key Fields**:

- runtime initialized state and worker count
- queued/running/completed counts
- recent job records
- recent violations
- dropped history count

**Validation Rules**:

- Snapshot creation must not expose mutable scheduler internals.
- Bounded histories must report dropped history counts when entries are evicted.

## BindingJobHandle

**Purpose**: C-compatible opaque handle for generated or script-facing integrations.

**Key Fields**:

- `structSize`
- `version`
- `id`
- `generation`

**Validation Rules**:

- Version mismatch returns a deterministic error.
- Struct size mismatch returns a deterministic error.
- The contract must not require callers to own STL types.
