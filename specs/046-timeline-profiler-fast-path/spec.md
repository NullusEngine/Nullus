# Feature Specification: TimelineProfiler GPU Fast Path

**Feature Branch**: `046-timeline-profiler-fast-path`  
**Created**: 2026-06-11  
**Status**: Draft  
**Input**: User description: "Optimize TimelineProfiler GPU frame tick so empty GPU query frames do not submit or wait on no-op DX12 timestamp resolve work; preserve existing GPU scope reporting when GPU queries exist."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Empty GPU Frames Stay Lightweight (Priority: P1)

An editor user recording the TimelineProfiler wants frames with no GPU timeline scopes to avoid visible CPU start-of-frame gaps, so profiling the editor does not add misleading idle-looking CPU time.

**Why this priority**: The provided trace shows every CPU frame has an unlabelled start-of-frame blank region, even though no GPU queue events are exported. Removing the avoidable profiling overhead is the primary value of this change.

**Independent Test**: Run the timeline profiler on a frame with no GPU query ranges and verify that the profiler advances without scheduling or waiting for empty GPU readback work.

**Acceptance Scenarios**:

1. **Given** TimelineProfiler recording is enabled and a frame contains no GPU timeline scopes, **When** the profiler begins the next frame, **Then** it skips empty GPU readback work and keeps the CPU frame start gap near profiler bookkeeping cost.
2. **Given** a trace is exported after recording frames with no GPU timeline scopes, **When** the trace is inspected, **Then** the main-thread CPU frame no longer contains a multi-millisecond unlabelled start gap caused by empty GPU profiler work.

---

### User Story 2 - GPU Timeline Data Remains Correct (Priority: P2)

An engineer capturing GPU timeline scopes wants existing GPU events to continue appearing with valid timing, queue ownership, and frame association after the fast path is added.

**Why this priority**: The optimization must not silently disable useful GPU profiling data when GPU scopes are present.

**Independent Test**: Record a frame that contains a paired GPU scope and verify the GPU event is still published after the expected readback latency.

**Acceptance Scenarios**:

1. **Given** TimelineProfiler recording is enabled and a frame records at least one complete GPU scope, **When** the profiler advances through readback latency, **Then** the GPU event appears on the correct queue track.
2. **Given** a mix of empty GPU frames and frames with GPU scopes, **When** the profiler advances across them, **Then** empty frames do not block later non-empty GPU events from being published.

---

### User Story 3 - Profiling Overhead Is Visible When Needed (Priority: P3)

An engineer investigating profiler cost wants enough trace visibility to distinguish TimelineProfiler bookkeeping from editor or renderer work.

**Why this priority**: The original blank region was hard to identify. Minimal profiling visibility makes future regressions easier to diagnose.

**Independent Test**: Capture or inspect a profiler frame and confirm TimelineProfiler frame maintenance is identifiable without creating excessive nested noise.

**Acceptance Scenarios**:

1. **Given** TimelineProfiler recording is enabled, **When** a CPU trace is inspected, **Then** frame-boundary profiler maintenance is identifiable as profiler work rather than anonymous application work.
2. **Given** TimelineProfiler recording is disabled, **When** the editor runs normally, **Then** no new timeline overhead is introduced.

### Edge Cases

- Empty frames can occur before any GPU scopes are ever recorded, between frames that contain GPU scopes, or after GPU scope recording stops.
- Queues may be registered even when no scopes are submitted during the current frame.
- Incomplete GPU scope pairs must continue to prevent unsafe frame advancement until they are completed or cleared by existing lifecycle rules.
- Shutdown and context replacement must still drain any previously submitted non-empty GPU readback work before releasing resources.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: TimelineProfiler MUST skip GPU readback submission and fence waiting for frames that contain no GPU timeline query ranges.
- **FR-002**: TimelineProfiler MUST continue to publish valid GPU timeline events for frames that contain complete GPU scope pairs.
- **FR-003**: Empty GPU frames MUST be treated as complete for readback progression so later frames are not blocked by skipped work.
- **FR-004**: Frames with pending command-list query data or open GPU queue events MUST retain the existing conservative advancement behavior.
- **FR-005**: Shutdown and GPU context replacement MUST continue to wait only for readback work that was actually submitted.
- **FR-006**: CPU profiling output SHOULD identify TimelineProfiler frame maintenance so future captures can distinguish profiler overhead from editor update/render work.
- **FR-007**: The optimization MUST preserve editor and game runtime behavior when TimelineProfiler recording is closed or disabled.

### Key Entities

- **Timeline Capture Frame**: A profiler frame that may contain CPU scopes, GPU scopes, or both.
- **GPU Query Frame**: The GPU timeline data associated with one capture frame, including whether any readback work was submitted.
- **Queue Track**: A timeline lane that receives GPU events for a registered graphics or compute queue.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a trace with no GPU queue events, the main-thread CPU frame start gap attributable to empty GPU profiler work is reduced from the observed multi-millisecond range to under 0.25 ms on the validated DX12 Windows capture.
- **SC-002**: GPU timeline scopes recorded in a non-empty frame still appear in exported traces after the normal readback latency.
- **SC-003**: Automated tests cover empty GPU frame advancement, non-empty GPU frame publication, and mixed empty/non-empty frame ordering.
- **SC-004**: Product runtime validation confirms the editor remains runnable with TimelineProfiler recording enabled and disabled on the validated backend.

## Assumptions

- The observed blank region comes from TimelineProfiler GPU frame maintenance that runs after the CPU frame event begins and before application frame scopes open.
- The immediate validation target is Windows DX12 because the provided trace and current GPU timeline integration use DX12 timestamp queries.
- Other backends must not regress, but this change does not claim new GPU timeline support for them.
- The Profiler panel is the user-facing control for TimelineProfiler recording state.
