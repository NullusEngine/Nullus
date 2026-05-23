# Contract: Editor Profiler Window

## Window Availability

**Input**: The editor starts with the profiler feature compiled in.

**Required behavior**:

- A dockable `Profiler` window is available through the existing editor window/menu workflow.
- Opening and closing the window does not reset or corrupt the profiler session.
- The window can be reopened during the same editor session.

## Live Timeline Display

**Input**: TimelineProfiler destination is enabled and can accept CPU events.

**Required behavior**:

- The window displays recent timeline events emitted by the shared instrumentation facade.
- New events appear without restarting the editor.
- Default function-name events and explicit named events are distinguishable by their labels.

## Empty, Disabled, And Unsupported States

**Input**: No timeline events have arrived, the destination is disabled/unavailable, or the requested data type is unsupported.

**Required behavior**:

- Empty data displays an explicit empty state.
- Disabled or unavailable destination state displays a concise status message.
- Unsupported GPU/backend/platform state is shown as unsupported rather than stale or blank live data.
- Tracy output may continue while the editor timeline is closed, disabled, or unsupported.
