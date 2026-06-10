# Research: TimelineProfiler GPU Fast Path

## Decision: Skip Resolve Work For GPU Frames With No Query Ranges

**Rationale**: The provided trace contains no exported Direct Queue or Compute Queue events, yet every CPU frame has an unlabelled start gap before `Application::TickFrame`. The TimelineProfiler GPU tick currently advances every frame by resolving query heaps and resetting frame resources even when no GPU query ranges were recorded. Empty query frames do not need GPU-side resolve work, so they can be marked complete in profiler bookkeeping.

**Alternatives considered**:

- Add only CPU instrumentation around the existing work. This would explain the blank region but would not remove the avoidable wait.
- Increase GPU profiler frame latency. This may reduce stalls but increases memory and delays GPU event publication while still doing no-op resolve work.
- Disable GPU profiler initialization until the first GPU scope. This is larger and risks losing early scopes unless command-list lifecycle replay is redesigned.

## Decision: Preserve Conservative Advancement For Pending Or Open GPU Work

**Rationale**: The existing lifecycle intentionally avoids advancing when command lists still hold query data or queue event stacks are open. The optimization should only target frames that are genuinely empty, not frames with incomplete or unsubmitted GPU scope data.

**Alternatives considered**:

- Treat all missing published events as empty. This could hide mismatched begin/end scope pairs and regress GPU trace correctness.
- Flush incomplete work at frame boundaries. That would change profiler semantics and is out of scope.

## Decision: Add Testable Lifecycle Helpers Before DX12 API Changes

**Rationale**: Direct3D objects are expensive to instantiate in unit tests, and the existing test file already validates `TimelineProfilerDetail` helper decisions. Adding small constexpr helpers makes RED/GREEN testing possible before touching production command submission code.

**Alternatives considered**:

- Test only by running a live editor trace. This would validate the symptom but would not provide regression coverage.
- Build a mock D3D12 queue/fence stack. This would be brittle and disproportionately large for the targeted lifecycle decision.

## Decision: Keep Runtime Validation Scoped To Windows DX12

**Rationale**: TimelineProfiler GPU scopes currently route through DX12 timestamp query support. The evidence for the issue comes from a Windows DX12 trace. Other platforms should continue compiling and running through existing guards, but this change does not establish new GPU timeline behavior for them.

**Alternatives considered**:

- Claim all backends are optimized. This would violate Nullus validation rules because no non-DX12 evidence exists.
- Delay the fix until every backend has equivalent timeline support. That would leave a validated DX12 profiler overhead issue unfixed.
