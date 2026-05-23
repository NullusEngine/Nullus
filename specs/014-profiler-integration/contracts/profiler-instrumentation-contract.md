# Contract: Profiler Instrumentation

## Scope Marker Contract

Developers have one public instrumentation surface for scoped events.

### Default Function-Name Scope

**Input**: A scope marker is placed inside a function without an explicit display name.

**Required behavior**:

- The emitted event name is derived from the calling function.
- The event begins when the marker object/scope is entered.
- The event ends when the marker object/scope exits, including early returns.
- Every enabled destination receives matching begin/end boundaries for the same event.
- If profiling is disabled, the call site remains valid and does not require profiler tools.

### Explicit Named Scope

**Input**: A scope marker is placed with an explicit display name.

**Required behavior**:

- The explicit display name is used instead of the function-derived name.
- The same explicit name is sent to every enabled destination.
- Empty explicit names are rejected by testable assertion, fallback, or compile-time prevention chosen during implementation.

### Destination Routing

**Input**: One or more destinations are enabled, disabled, unavailable, or unsupported.

**Required behavior**:

- Enabled and available destinations receive events.
- Disabled, unavailable, or unsupported destinations do not crash and do not block other destinations.
- Diagnostics expose which destinations accepted or dropped events.

## Test Expectations

- A test destination can observe begin/end calls without Tracy or TimelineProfiler linked.
- Tests cover default-name labels, explicit labels, disabled profiling, destination independence, nesting, and multi-thread safety at the facade level.
