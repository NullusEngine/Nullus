# TaskGraph and JobSystem unification

## Audit before implementation (2026-10-29)

This audit uses the current working tree, including pre-existing changes. In
particular, `TaskGraph::SubmitTracked` is retained. Older architecture notes that
describe TaskGraph as a shared FIFO without stealing are out of date.

| Area | Existing implementation | Decision |
| --- | --- | --- |
| TaskGraph | `Runtime/Base/TaskGraph/TaskGraph.cpp` owns worker threads, per-worker priority deques, an injection queue, and Game/Render/RHI queues. It already implements DAG dependency counters, retained completion tokens, iterative cancellation, and nested worker assistance. | Retain dependencies, tokens and named queues; remove its independent general worker pool. |
| Jobs | `Runtime/Base/Jobs/JobQueue.cpp` owns a separate foreground worker pool and shared ready-group queues, with dependencies, groups, cleanup callbacks, bounded terminal history and restricted cooperative waits. | Preserve public scheduling APIs, script ABI and cleanup semantics; make Jobs the sole compute execution infrastructure. |
| ParallelFor | `JobSystem.h`, `JobRange.cpp` already provide batch/grain controls and work-stealing index ranges. | Reuse; range stealing is distinct from worker queue stealing. |
| Background work | `BackgroundJobQueue.cpp` owns an independently budgeted pool used for blocking I/O and background asset work. | Preserve deliberate I/O isolation. It is not a second general compute pool. |
| Thread budgets | `ExecutionThreadBudget.h` splits CPU capacity between TaskGraph and Jobs; both pools have minimum counts. | Account for one shared compute pool plus background I/O. |
| Named affinity | TaskGraph queues are separate, but public `Pump` and shutdown can execute a named lane without checking registration. Inline execution also bypasses affinity. | Require explicit lane ownership, including the existing logical RHI lane on Jobs workers. |
| Shutdown | Immediate TaskGraph shutdown publishes terminal state for running bodies, and prunes the node map while iterating it. Drain shutdown pumps all named lanes on the caller. | Keep running bodies alive until they return; cancel only unstarted work; never impersonate another named thread. |
| Graph construction | Incremental submission accepts only existing prerequisite handles, so cycles cannot be created through that API. There is no sealed graph execution/completion object. | Keep incremental compatibility and add an explicit, validated, one-shot graph batch. |
| Integration | Rendering and Physics use TaskGraph. ECS and CPU VFX already use Jobs. Asset streaming uses Background Jobs. Editor and Player both initialize Jobs. | Migrate centrally through the existing facades, preserving play-loop phase order and module semantics. |

## Implemented architecture and contracts

```text
Engine systems
  |-- TaskGraph: prerequisites, DAG validation, graph/node completion
  |       |-- Any -> Jobs::Internal::DispatchJob -> JobQueue
  |       `-- Game / Render / logical RHI -> registered named queues
  `-- Jobs: independent jobs, groups, batches, ParallelFor
          `-- one shared foreground pool: local LIFO / stealing FIFO / injection FIFO

Blocking asset I/O -> separately budgeted BackgroundJobQueue
```

TaskGraph's private threads, worker loop, per-worker lanes and injection queues
were removed. No public facade was deleted. `TaskHandle`, `TaskCompletion`,
`Submit`, `SubmitTracked`, cancellation and the named queues are retained.
`JobQueue` remains the sole compute scheduler. Its group/dependency/cleanup
machinery, `JobRange`, `JobBatchDispatcher`, diagnostics, script bindings and
background queue are reused. Queue locks are per lane; group state still uses a
short metadata lock. This is deliberately not a lock-free scheduler.

Ready graph nodes use the internal `DispatchJob` entry point. It retains the
same `JobQueue` active-group registry, priority queues, worker pool, admission
limit and shutdown cleanup. The graph already owns dependency and completion
state, so this entry point rejects JobHandle prerequisites and does not create a
second public JobHandle or terminal-handle history. A single callback occupies
the group's existing callback slot without allocating the batch vectors used by
ordinary Jobs. Rejection leaves payload ownership with the graph; accepted
queued callbacks are cancelled exactly once, and running callbacks finish before
drain or shutdown returns. Normal `ScheduleJob` and batch handles keep their
existing dependency, failure and history behavior.

Completion observers and idle workers use separate notifications. Finishing a
callback wakes completion observers; ordinary group dependency fan-out also
wakes enough workers for all newly runnable groups. Idle-worker notification
selects the most recently parked worker through `WorkerWakeSignal`, using fixed
per-worker waiters and condition-variable sleep. This sleeper selection does not
change task priority or stealing order. Graph
`Finalize` may retain a single ready successor on the current worker only after
an Any task's body and captured-payload destructors have returned. Multiple
successors still wake other workers and can be stolen. Submission from inside a
task body, external submission and named-lane completion do not use this tail
continuation hint: a parent may block after submitting a child. Graph task
profiling remains at `ExecuteTaskBody`; the dispatch bridge does not duplicate
that scope.

The first initializer fixes the process-wide worker configuration. Later graphs
acquire a scheduler lease and report the actual worker count. Explicit Jobs
ownership and graph leases are separate: releasing one graph does not stop
another graph or the application owner. The final lease released inside a worker
uses a temporary, process-owned shutdown/join thread, which never pumps main
continuations. It is not another persistent execution pool. Graph dispatches
retain their implementation and node without a separate dispatch allocation;
even the facade's last reference may be released by its own task capture.

`TaskGraphBatch` is a one-shot construction object. `Add` and `DependsOn` build
the graph; `Execute` first validates all edges with a topological pass and then
publishes every node before releasing roots. Invalid/cyclic graphs accept no
work. An empty graph succeeds. A second Execute or mutation after Execute is
rejected. `GraphCompletion` retains completion tokens for every node, even when
the per-node terminal retention limit is zero. Failed nodes cancel their
unstarted descendants; independent running branches finish before whole-graph
completion. Failure has precedence over cancellation in the aggregate result.
The legacy incremental API still accepts only existing prerequisites, making
cycles impossible through structural mutation. Generations distinguish graphs
as well as restarts, so another graph's handle cannot accidentally bind locally.

Named queues require explicit registration. Compute workers cannot register
Game/Render; RHI Tasks mode may explicitly register its logical RHI lane. Inline
mode preserves this restriction. An unregistered Pump performs no work. Drain
shutdown pumps only lanes owned by the caller; orphaned named tasks are
cancelled, and registered owners must continue pumping until their work ends.
When one thread owns several named lanes, completion waits visit every owned
lane; `PumpCurrentThread` shares its task budget across those lanes. This also
supports combined Game/Render/RHI ownership without creating dedicated threads.
Immediate shutdown cancels unstarted tasks and waits for running bodies and
payload cleanup. Submit is rejected after shutdown until explicit Start.

Workers waiting on graph completion assist the same Jobs pool. Jobs' existing
dependency-aware Complete policy is retained. Explicit self/ancestor dependency
waits fail rather than deadlock, and finite waits check their deadline even while
helping. A callback itself cannot be preempted by that timeout. User mutex cycles
and unpumped named-lane dependencies remain caller errors; a scheduler cannot
make arbitrary blocking code safe. A lifecycle owner must call Shutdown outside
that graph's active callback. Deep dependency failure/cancellation and empty-job
fence propagation are iterative, avoiding continuation recursion.

`JobSystemConfig::maxPendingJobCount` defaults to 65536 accepted callbacks per
domain: foreground, and background plus main continuations each have that bound.
Waiting and running work consume capacity. `TaskGraphConfig::maxPendingTaskCount`
separately bounds graph nodes. Saturation rejects submission without taking
ownership of caller payloads. Terminal histories remain bounded; cancelled named
queue entries are removed even when no owner pumps them. Payload destructors run
outside the graph mutex before completion is published.

Public Jobs additions are `GetCompletionStatus`, `RequestCancel`,
`IsCancellationRequested`, and `DrainAcceptedJobs`. For a queued compute group,
cancel skips bodies and runs existing cleanup. For a started group it records a
cooperative request; bodies determine the eventual result. Background I/O does
not expose per-handle cancellation (`RequestCancel` returns false); it retains
shutdown cancellation. Normal/High enum values remain unchanged and Low is
added. Main continuations now validate their owner thread.

### API examples

Existing independent jobs and ParallelFor need no TaskGraph:

```cpp
namespace Jobs = NLS::Base::Jobs;
struct Update : Jobs::IJobParallelFor
{
    float* values = nullptr;
    void Execute(uint32_t index) { values[index] *= 2.0f; }
};
Update work;
work.values = values.data();
Jobs::JobParallelForScheduleOptions options;
options.batchSize = 64;
auto handle = Jobs::ScheduleParallelFor(work, uint32_t(values.size()), options);
// A nonempty submission returning id == 0 was rejected; caller handles fallback.
Jobs::Complete(handle);
```

A retained TaskGraph can execute a new one-shot batch each frame:

```cpp
namespace TG = NLS::Base::TaskGraph;
TG::TaskGraph graph;
graph.RegisterCurrentThread(TG::NamedThread::Game);
TG::TaskGraphBatch frame;
const auto input = frame.Add({.function = ReadInput,
                             .targetThread = TG::NamedThread::Game});
const auto physics = frame.Add({.function = PreparePhysics});
const auto animation = frame.Add({.function = PrepareAnimation});
const auto render = frame.Add({.function = PrepareRenderData});
frame.DependsOn(physics, input);
frame.DependsOn(animation, input);
frame.DependsOn(render, physics);
frame.DependsOn(render, animation);
auto completion = frame.Execute(graph);
if (completion.IsValid())
    HandleFrameResult(completion.Wait()); // registered Game caller pumps Game
graph.Shutdown();
```

The application initializes Jobs once before its producers and rendering
systems. The example represents dependency ordering; it does not prescribe
parallel physics/animation access to overlapping scene data.

## Validation provenance

The pre-change sources and measurements live in
`tmp/task-unification/validation/baseline`. Focused validation uses the repository's
GoogleTest sources and real Base implementations in an isolated build so unrelated
working-tree changes and running applications are not overwritten.

The final focused scheduler suite passed 236/236, with another 4/4 rendering
lifecycle cases, on Windows Release, MSVC AddressSanitizer, Linux GCC 10 Release,
and the documented GCC 10 ThreadSanitizer configuration. TSan requires a
per-process address-layout setting and a standard-library timed-wait fallback on
this host; see the [toolchain scope](../Tests/TaskSystem/results/2026-10-29-linux-tsan.md).
The [test and benchmark record](../Tests/TaskSystem/results/2026-10-29-validation.md)
contains the final commands, source hashes, raw data and result tables.

The initial unification exposed a regression in the 5,000-edge no-op chain.
Profiling counters isolated repeated cross-worker steals of its single runnable
successor, while source review found duplicated JobHandle lifecycle work and
short-lived graph finalization allocations. The final implementation separates
completion wakeups and uses the owner-tracked dispatch path and the restricted
tail continuation hint above. Successful Any nodes with at most one successor
now use a direct finalizer. Other finalizations use a small private PMR scratch
buffer; large fan-out and cancellation cascades use allocator overflow. Private
per-call storage preserves reentrancy when a captured object's destructor
submits or waits for other work.

Measurements compare three source snapshots: the original separate-pool
baseline, the initial unified implementation and the final optimized scheduler.
Chain steal/local/injection counters are collected outside the timed interval.
Paired runs and host-load controls are retained in the benchmark record rather
than treating a single timing as a universal improvement. The no-op chain tests
scheduling cost; real frame preparation, physics and asset workloads still need
their own profiles.

A [follow-up tiny-task investigation](../Tests/TaskSystem/results/2026-10-29-tiny-task-performance.md)
isolated repeated worker handoff in sequential small tasks. The selected minimal
change retains only LIFO sleeper selection and its test hooks; the other hot-path
experiments were withdrawn, and TaskGraph is restored to the implementation from
before that follow-up. Six added regression cases remain. An independent
LIFO-only comparison supports improved four-worker Graph latency, with host-load
limits that preclude ranking different measurement windows by absolute time.
Fresh validation of the minimal version passed 242 scheduler and four rendering
lifecycle cases in Windows Release, ASan, Linux Release and TSan. In the final
three-round window, four-worker Graph submit/wait improved in every round
(median 29.9711 to 4.7605 ms). Host load varied substantially, and single-worker
and chain costs remain unresolved; the full record retains all measurements and
separates the final version from historical combined candidates.

A [dispatch-record reuse follow-up](../Tests/TaskSystem/results/2026-10-29-dispatch-reuse-performance.md)
then removed repeated allocation of anonymous JobGroups and their map entries.
The shared queue retains at most 64 completed records and reuses only entries
with no remaining execution references, assigning fresh IDs and resetting their
lifecycle fields. Public JobHandle retirement remains separate. A 2,000-node
single-worker chain allocated two records and reused them 1,998 times.
The follow-up passed 248 scheduler and four rendering lifecycle tests in Release,
ASan, Linux Release and TSan. Four-worker chain-release medians decreased from
5.5270 to 3.9827 ms in its own matched window; the record preserves load variation
and all unfavorable samples.

The [single-worker and long-chain follow-up](../Tests/TaskSystem/results/2026-10-30-tiny-sync-performance.md)
adds the direct success finalizer and merges the started submission admission
checks into one graph-lock interval. Lazy startup still unlocks and revalidates
admission. Capture cleanup stays outside the lock; facade self-destruction on
inline, named-pump and cancellation paths is covered by six added regressions.
The selected version passed 254 scheduler and four rendering lifecycle cases in
Windows Release, ASan, Linux Release and TSan. In its final matched window,
single-worker chain release decreased from 5.1205 to 3.4293 ms and four-worker
chain release from 4.2992 to 3.0406 ms. Host controls also changed; single-worker
tiny tasks and chain release still measured 12.6% and 18.7% above the original
separate-pool reference. A direct-ready worker-loop experiment was withdrawn.
All samples, including regressions, remain in the linked report.

Headless scheduler results do not establish Editor/Player launch, GPU submission
or visible Play/Stop behavior. Application compilation and runtime acceptance
must be assessed separately from these tests. The main output tree was shared
with other builds, so this task's isolated validation preserves those processes
and outputs. macOS validation remains outstanding.

## Engine integration and lifecycle

`ExecutionThreadBudget.h` now counts a shared compute pool and an isolated
background I/O pool. `taskGraphWorkerCount` is a compatibility alias of
`jobsForegroundWorkerCount`; it must not be added again to the physical worker
total. The minimum is one compute worker and one background worker. Explicit
background overrides redistribute the same bounded total.

The Editor establishes its Jobs owner in the first RAII member of `Context`,
before Driver can acquire the global TaskGraph. This preserves
`NLS_EDITOR_BACKGROUND_WORKERS`. Player establishes its owner immediately before
Driver construction. Both use a conservative dedicated-RHI reservation at this
stage; the first scheduler owner fixes the worker configuration. Driver reports
the actual shared worker counts instead of a second theoretical split.

Editor shutdown stops `EditorActions` background producers, closes Asset Browser
watchers and async refresh coordinators, and shuts down the thumbnail pipeline
while its resources and render lanes remain alive. It then drains accepted work,
including queued main-thread continuations and their descendants until both
queues are stably idle; its pump only services the calling registered game lane.
Context drains again before stopping rendering, releases
the global TaskGraph after the last rendering system owner has left, and releases
its Jobs owner. Player uses the same drain / render stop / graph release / Jobs
release ordering. The RAII owners also release on partial Context construction.
Suspended rendering systems retain an owner count separate from the active GPU
thread count. Neither Driver suspension nor stopping one of several rendering
system owners shuts down the process-wide graph.

Global graph creation publishes only after initial startup. Existing graph Start
and Shutdown run outside the pointer lock so an accepted worker callback can
still query the graph while shutdown waits for it. After an explicit global shutdown, Get does not
silently restart the scheduler; InitializeGlobalTaskGraph is the explicit restart
operation. Ordinary first access still supports lazy initialization.

Existing workloads retain their domain semantics:

| Workload | Current route / integration decision |
| --- | --- |
| Render preparation | `RenderScene.cpp`, render command pipes and RHI recording/translation use TaskGraph; their Any tasks now use the shared compute workers. Named Render/RHI lanes remain with RenderingThreadSystem. |
| Visibility | `SceneVisibilityPipeline.cpp` already uses Jobs ParallelFor; no new DAG was added. |
| Physics | PhysicsWorld and PhysicsWorld2D already adapt PhysX/Box2D work to TaskGraph, including externally supplied graphs. They inherit shared execution centrally. No physics phase or SDK dispatch contract was changed. |
| ECS | JobBatch and SystemScheduler already use Jobs; component/resource conflict detection remains in ECS. SceneStreaming uses the background I/O queue. |
| CPU particles | CpuEmitter already uses ScheduleParallelFor and Complete. Batch sizing and frame semantics are preserved. |
| Animation | AnimationSystem and Scene::UpdateAnimations currently evaluate serially. Parallel animation was not invented as part of scheduler unification. |
| Asset loading | Mesh/Texture/Material managers and thumbnails already use Background Jobs; blocking loads remain isolated from compute workers. |
| Play lifecycle | RuntimePlaySession retains PreFixedUpdate, FixedUpdate, FixedSimulation, physics, PostPhysics, Update, animation, LateUpdate, Presentation and Cleanup ordering. |

## Remaining workload-specific concurrency

Unifying TaskGraph and Jobs does not remove every OS thread. Dedicated rendering,
file watchers, debug endpoints, profiler output and child-process pipe readers
have service-specific ownership and remain separate.

The audit also found temporary concurrency outside the schedulers:
`ExternalAssetImporter::ParallelForModelImportItems` (up to four workers),
`AssetDatabaseFacade` parallel model preparation (four), startup metadata reads
(up to eight), DistanceFieldBuilder brick work (up to eight), and several
`std::async` operations for material builds, managed compilation and asset loads.
These are not additional long-lived general-purpose worker pools, but they can
still oversubscribe the CPU and are follow-up migration targets.

In particular, ShaderCompiler::CompileBatch and ReflectBatch launch one async
operation per input. The existing test
`CompileBatchRunsIndependentStagesConcurrentlyAndPreservesOrder` explicitly
requires concurrency without an initialized JobSystem. Shader batches can also
be called from background asset jobs; synchronously waiting for children on that
same bounded background pool can deadlock. They are therefore retained until a
bounded batch API can preserve exception/result ordering, standalone operation,
and nested-call behavior. No claim is made that this change bounds those shader
threads.

## External design references

The [Unreal Engine Tasks System documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine)
confirms that UE Tasks and TaskGraph share their scheduler and worker backend.
It recommends dependencies over blocking waits. Its current documentation also
records that UE 5.5 deprecated unrestricted busy waiting: executing unrelated
work while waiting can create same-stack dependency cycles, latency spikes and
stack overflow. Nullus's cooperative wait policy must therefore be assessed
against its own dependency and affinity rules; it is not claimed to reproduce
UE's standby-thread implementation.

[Unity's job system overview](https://docs.unity.com/en-us/engine/6000.0/manual/scripting/programming-distribute-work-threads/job-system/overview)
describes shared native workers and work stealing. Its
[IJobParallelFor reference](https://docs.unity.com/en-us/engine/6000.0/script-reference/unity/jobs/ijobparallelfor)
requires independent iterations and explains the tradeoff between batch size
and stealing cost. Nullus reuses its existing range batching; ECS read/write
safety remains a separate domain responsibility. These references guide the
layering, not a claim of API or performance parity.

## Headless rendering lifecycle validation

The standalone `NullusRenderingThreadLifecycleTests` target compiles the real
rendering thread system, command pipe, fence, logger and shared scheduler without
a graphics device. Four integration scenarios passed in Release and with MSVC
AddressSanitizer, including suspended owners and the background/render/game drain
chain. See the [test scope, results and source hashes](../Tests/TaskSystem/results/2026-10-29-render-lifecycle.md).
This does not replace Editor/Player or GPU acceptance.

## Application acceptance

The final shared scheduler also ran in independently linked Editor and Player
executables on Windows/DX12. Player completed 12 presents and GPU readback;
Editor completed its finite-frame run and three real Play/Pause/Step/Resume/Stop
cycles through its project control endpoint. Each final run exited normally
with code 0. Complete dependency provenance, source hashes, commands and the
camera-only fixture scope are recorded in the
[application lifecycle results](../Tests/TaskSystem/results/2026-10-29-application-lifecycle.md).
The separate [Play lifecycle suite](../Tests/TaskSystem/results/2026-10-29-play-lifecycle.md)
covers the real runtime session and repeated scene recovery.

Mouse interaction remains an explicit gap: the Windows desktop was locked, and
the foreground-window guard sent no mouse input. A rendered window capture and
control-endpoint responses are not reported as a visible mouse acceptance pass.
