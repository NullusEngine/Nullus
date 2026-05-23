# Data Model: Unified Profiler Integration

## Profiling Scope Event

Represents one measured unit of work.

**Fields**:

- `name`: Display label shown in profiler destinations.
- `sourceFunction`: Automatically captured calling function label when no explicit name is provided.
- `category`: Optional grouping label for future filtering.
- `threadId`: Identity of the thread that emitted the event.
- `startTime`: Timestamp captured at scope entry.
- `endTime`: Timestamp captured at scope exit.
- `depth`: Nesting depth on the emitting thread, when available.
- `destinationMask`: Destinations that accepted the event.

**Validation rules**:

- `name` must be non-empty after default-name resolution.
- `endTime` must not precede `startTime`.
- Begin/end must occur on the same logical scope instance.
- Nested events must unwind in LIFO order per thread for destinations that require strict nesting.

## Profiler Session

Represents active profiling state for one process run.

**Fields**:

- `enabled`: Whether the shared profiler facade accepts events.
- `destinations`: Registered destination list.
- `acceptedEventCount`: Diagnostic count of events accepted by at least one destination.
- `droppedEventCount`: Diagnostic count of events ignored because profiling or destinations were unavailable.
- `capabilities`: Runtime availability for CPU scopes, GPU scopes, editor timeline visualization, and external profiling.

**State transitions**:

- `Disabled` -> `Enabled`: Runtime or build configuration allows events to be routed.
- `Enabled` -> `PartiallyAvailable`: At least one destination is enabled and at least one destination is unavailable.
- `Enabled` -> `Unavailable`: No destination can accept events.
- `Enabled` -> `Disabled`: Profiling is turned off or the process shuts down.

## Profiler Destination

Represents one output target for profiling events.

**Fields**:

- `id`: Stable destination name such as `tracy` or `timeline`.
- `enabled`: User/build/runtime state.
- `availability`: `Available`, `Disabled`, `Unavailable`, or `Unsupported`.
- `capabilities`: Supported event types, such as CPU scopes and GPU scopes.
- `lastError`: Human-readable diagnostic message when unavailable.

**Validation rules**:

- Disabled or unavailable destinations must not prevent other destinations from accepting events.
- Destination wrappers must expose availability without requiring UI code to know third-party details.

## Editor Profiler Window

Represents the dockable editor panel that displays TimelineProfiler output.

**Fields**:

- `opened`: Whether the panel is visible.
- `timelineAvailability`: Availability state for the embedded timeline view.
- `displayMode`: `Live`, `Empty`, `Paused`, `Unavailable`, or `Unsupported`.
- `lastStatusMessage`: User-facing status text for non-live states.

**State transitions**:

- `Closed` -> `Open`: User opens the panel from the editor window/menu workflow.
- `Open` -> `Live`: Timeline data is available and events are arriving.
- `Open` -> `Empty`: Timeline is available but no events have been recorded yet.
- `Open` -> `Unavailable`: Timeline destination cannot be initialized or is disabled.
- `Open` -> `Unsupported`: Active backend/platform does not support requested data, especially GPU timeline data.
