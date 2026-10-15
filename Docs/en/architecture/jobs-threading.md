# Jobs and Threading

Nullus has two parallel scheduling facilities: the **JobSystem** for general CPU work and the **TaskGraph** for rendering and engine phase tasks. The first answers "how do I hand work off", the second answers "how do phases hand off to each other".

## JobSystem

`Runtime/Base/Jobs` provides the native scheduling interface.

### Initialization

```cpp
NLS::Base::Jobs::JobSystemConfig config;
config.workerCount = 4;                // kAutoJobWorkerCount derives a default from the hardware
config.backgroundWorkerCount = 1;
NLS::Base::Jobs::InitializeJobSystem(config);
```

`TryInitializeJobSystem` is the ownership-aware variant: it returns `true` only when that call actually created the runtime. Code using it must call `ShutdownJobSystem` only on the `true` branch.

`JobHandle{}` is always complete.

### Scheduling forms

| Interface | Purpose |
| --- | --- |
| `ScheduleJob` | Short CPU work |
| `ScheduleDifferentJobsConcurrent` | A batch of distinct concurrent jobs |
| `ScheduleJobForEach` | Batched iteration with a caller-controlled `batchSize` |
| Native `IJob` / `IJobParallelFor` adapters | Typed wrappers for C++ users |
| `JobBatchDispatcher` | Batched dispatch and multi-dependency fan-in |

### Completion and cancellation

- `Complete` waits and clears the handle.
- `CompleteNoClear` keeps the caller's copy valid for later queries.
- `HasBeenSynced` reports whether the handle reached a terminal state.
- Foreground jobs may supply a cancel callback to release their payload. It runs at most once for queued or dependency-waiting work cancelled by immediate shutdown or by a failed or cancelled dependency, and never once the job has started executing.

### Batching and fan-in

`JobBatchDispatcher` supports explicit per-dispatcher kicks, destructor kicks, configured auto-kick thresholds, and multi-dependency fan-in handles. A dispatcher is a caller-owned batching helper and is not thread-safe; producers adding to or kicking it from multiple threads must serialize access.

`IsCompleted` and `Complete` both make pending batches worker-visible before querying or waiting. There is no global `ScheduleBatchedJobs`; batch visibility is per dispatcher, per fan-in creation, or per handle.

### Zero workers and helping

- `workerCount = 0` is an explicit synchronous and test mode.
- Completion may execute eligible jobs on the calling thread to advance dependencies.
- Waiting only helps jobs on the waited fence's dependency chain, or jobs marked `GuaranteedNoSyncWait`.
- A `GuaranteedNoSyncWait` callback runs inside a no-sync-wait scope, where calling `Complete` records a violation and returns without blocking.

### Known limits

- `ScheduleJobForEach` rejects iteration counts or batch sizes beyond the native `int` range planner.
- Foreground and background queues still use mutex-protected central registries; throughput work is deferred until the semantics are stable.
- Cross-queue dependency promotion uses explicit notifications plus a bounded timed fallback. Correctness does not depend on the fallback, but heavy cross-queue traffic can still pay the polling latency.

### Memory and data safety

Job callbacks receive raw `void*` payloads whose lifetime the caller owns; the payload must stay alive until the handle reaches a terminal state or its cleanup callback has run. Callbacks can run during completion or shutdown, so they must not assume a particular thread.

Nullus provides no managed-container race checking such as `NativeArray` or safety handles. Shared mutable state must be protected by the caller with atomics, locks, immutable snapshots, or dependency ordering. `JobSafetyPolicy` describes synchronous-wait safety only; it does not prove memory safety.

Callbacks should not throw. If a foreground, background, combine, cancel, or cleanup callback throws, the scheduler catches it at the boundary, records `JobViolationKind::CallbackException`, and advances the handle along the documented failure or cancellation path.

### Background work and main-thread continuations

- Long-running or IO-heavy work uses `ScheduleBackgroundJob`.
- Main-thread follow-up work uses `ScheduleMainThreadContinuation` and only executes from `DrainMainThreadContinuations`.
- Ready continuations are promoted into a dedicated ready queue so each drain consumes ready work instead of scanning every blocked continuation.
- The editor update loop drains a bounded budget once per frame after delayed editor actions.
- Shutdown does not implicitly execute main-thread continuations; owners must drain what they need before stopping the JobSystem.

The editor's background task path goes through the shared background queue instead of private threads, enforces a task capacity limit, rejects submissions after stop, and completes submitted handles in its destructor.

### Diagnostics

`JobSystemConfig::enableDiagnostics` collects bounded snapshots, and `DisallowJobSyncWaitScope` records structured violations when a blocking wait is attempted inside a no-wait scope.

### Binding interface

`JobBindings.h` exposes a minimal C-compatible subset: `structSize`, `NLS_JOB_BINDING_VERSION`, opaque handle IDs, named priority and safety constants, and explicit status codes.

- When the runtime is stopped, non-default query, complete, and clear calls return `NLS_JOB_STATUS_NOT_INITIALIZED`.
- Completing a cancelled binding handle returns `NLS_JOB_STATUS_CANCELLED` and clears the handle.
- A completion attempt that cannot reach a terminal state maps to `NLS_JOB_STATUS_FAILED` and leaves the handle set.

Binding structs are plain ABI types holding no STL objects. The native C++ headers are same-toolchain SDK interfaces, not a stable cross-compiler ABI.

## TaskGraph

The TaskGraph carries render scheduling and engine phase tasks:

- Each worker keeps priority-partitioned local queues.
- External submissions enter a shared injection queue.
- Idle workers steal work.
- `TaskCompletion::Wait()` helps execute runnable tasks on a worker to avoid nested-wait deadlocks.
- `RequestCancel` propagates along the dependency graph and wakes every waiter.

## Execution lanes

Rendering uses three logical lanes:

| Lane | Responsibility | Query |
| --- | --- | --- |
| Game | Produce commands | `IsInGameThread()` |
| Render | Consume commands and build the frame | `IsInRenderingThread()` |
| RHI | Translate and submit native command buffers | `IsInRhiThread()` |

Render and RHI attach to the TaskGraph once started. In `tasks` mode the RHI lane is a TLS execution context rather than a fixed OS thread.

## Hand-off between lanes

```text
Game Thread
  EnqueueRenderCommand (strict FIFO serial queue)
       |  scene proxy add/remove/change, frame render, resize, resource release
       v
Render Thread
  build the frame, generate commands
       |
       v
RHI executor
  Dispatch (serial intake, assigns a global submission sequence)
  Translate (parallel native command buffer generation on workers)
  Submit (ordered queue submission, cross-queue semaphores, readback, Present)
```

The async pipe only carries independent resource preparation and merges it explicitly through dependency events before consumption.

## Frame synchronization depth

| Layer | Purpose |
| --- | --- |
| `FrameResourceRing` | Answers whether the GPU is still using a batch of resources |
| `FrameLatencyController` | Controls CPU frame latency |
| `RenderCommandFence` | Render / Rhi / Swapchain sync depths |

## Thread naming

Thread names are registered at startup and are visible to profilers. Named entries include the Game Thread, `Render Thread`, a dedicated `RHI Thread`, `TaskGraph Worker <n>`, `Background Job Worker <n>`, and the editor's metadata/import, startup cache, Prefab preparation, shader compile and reflect, file watcher, managed script build, debug endpoint, progress dialog, and trace writer workers.

In `tasks` mode, RHI work keeps the physical worker's name instead of claiming a dedicated RHI thread.

## Next steps

- [Rendering Overview](../rendering/overview.md) shows how the lanes build a frame.
- [Profiling and Graphics Debugging](../quality/profiling.md) shows how to observe these threads.
