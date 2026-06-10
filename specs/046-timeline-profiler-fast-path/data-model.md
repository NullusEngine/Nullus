# Data Model: TimelineProfiler GPU Fast Path

## Timeline Capture Frame

Represents one frame in TimelineProfiler history.

**Fields**:

- `frameIndex`: Monotonic profiler frame identifier.
- `cpuScopes`: CPU timeline scopes recorded during the frame.
- `gpuQueryFrame`: Optional GPU query state associated with the frame.

**Validation rules**:

- CPU frame progression is independent from GPU event publication.
- GPU state may be absent or empty without making the CPU frame invalid.

## GPU Query Frame

Represents GPU timeline query state for one capture frame.

**Fields**:

- `recordedQueryCount`: Number of timestamp queries recorded for the frame.
- `recordedEventCount`: Number of GPU event ranges allocated for the frame.
- `hasPendingCommandListQueries`: Whether any tracked command list still owns query records.
- `hasOpenQueueEvents`: Whether any queue track has unmatched begin/end scope state.
- `submittedReadback`: Whether this frame submitted GPU readback/resolve work.
- `completedReadback`: Whether this frame is safe for readback progression.

**State transitions**:

1. `Collecting`: The current frame may receive GPU query records.
2. `EmptyComplete`: The frame has no query ranges and no pending/open GPU scope state, so no readback work is submitted.
3. `Submitted`: The frame has query work and submitted readback/resolve work.
4. `Complete`: Submitted readback work has completed and events can be published.

**Validation rules**:

- A frame can enter `EmptyComplete` only when it has no recorded GPU query work and no pending/open GPU scope state.
- A frame with pending command-list queries or open queue events must not be skipped as empty.
- Shutdown and context replacement only need to wait for frames whose `submittedReadback` is true.

## Queue Track

Represents a GPU queue lane in TimelineProfiler output.

**Fields**:

- `name`: Human-readable queue name.
- `trackIndex`: Timeline track identifier.
- `queryHeap`: Query storage used by that queue class.

**Validation rules**:

- Empty GPU frames must not publish queue events.
- Non-empty GPU frames must continue publishing complete events to the correct queue track.
