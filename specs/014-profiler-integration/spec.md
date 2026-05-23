# Feature Specification: Unified Profiler Integration

**Feature Branch**: `014-profiler-integration`  
**Created**: 2026-05-05  
**Status**: Draft  
**Input**: User description: "设计一个Profiler系统同时接入tracy 和https://github.com/simco50/TimelineProfiler，TimelineProfiler作为一个docker窗口内置在引擎编辑器中。两套方案共用一个埋点，支持自动传入调用函数名"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Add One Instrumentation Point For Both Profilers (Priority: P1)

As an engine developer, I want to mark a scope once in engine or editor code and have that same scope appear in both the external profiling workflow and the in-editor timeline view, so profiling coverage stays consistent without duplicate instrumentation work.

**Why this priority**: Shared instrumentation is the core value of the feature. Without it, the two profiler integrations drift and create maintenance overhead.

**Independent Test**: Add a profiling marker around a known update or render-adjacent scope, run the engine with profiling enabled, and verify that one source marker produces visible events in both profiler destinations.

**Acceptance Scenarios**:

1. **Given** profiling is enabled and both profiler destinations are available, **When** a developer enters a marked scope, **Then** the scope is recorded once by the shared instrumentation point and appears in both destinations with matching timing boundaries.
2. **Given** a developer uses the default profiling marker inside a function, **When** the event is recorded, **Then** the event name defaults to the calling function name without the developer typing that name manually.
3. **Given** a developer provides an explicit event name, **When** the event is recorded, **Then** both destinations show the explicit name instead of the automatically derived function name.

---

### User Story 2 - Inspect Recent Profiling Data Inside The Editor (Priority: P2)

As an engine or tools developer using the editor, I want a dockable profiler panel that shows recent TimelineProfiler data inside the editor layout, so I can inspect hot paths without leaving the editor.

**Why this priority**: The editor-integrated timeline makes performance investigation faster during normal editor work, especially for frame-by-frame UI, scene, and rendering analysis.

**Independent Test**: Open the editor, enable the profiler panel from the editor window/menu workflow, exercise a known profiled path, and verify the panel updates with current frame timing data.

**Acceptance Scenarios**:

1. **Given** the editor is running with profiling enabled, **When** the user opens the Profiler window, **Then** a dockable editor panel appears and renders TimelineProfiler data in the editor.
2. **Given** the Profiler window is open, **When** new profiled scopes are recorded, **Then** the panel updates without requiring an editor restart.
3. **Given** the Profiler window is closed or hidden, **When** profiling remains enabled, **Then** external profiler output can continue and the editor remains stable.

---

### User Story 3 - Keep Profiling Optional And Low Intrusion (Priority: P3)

As a developer building or running Nullus in configurations where profiling is not needed or an integration is unavailable, I want profiling markers to remain safe and low overhead, so normal development, testing, and release workflows are not disrupted.

**Why this priority**: Profiling code will touch broad runtime and editor paths. It must not make normal builds fragile or force every environment to run every profiler.

**Independent Test**: Build and run with profiling disabled, with only the external profiler destination enabled, and with only the editor timeline destination enabled; verify the same instrumented code paths remain functional in each mode.

**Acceptance Scenarios**:

1. **Given** profiling is disabled, **When** code containing profiling markers executes, **Then** the markers do not require profiler services to be running and do not alter user-visible behavior.
2. **Given** one profiler destination is unavailable, **When** profiling markers execute, **Then** the available destination continues receiving events and the unavailable destination fails gracefully.
3. **Given** profiling is enabled in a platform or graphics-backend context with limited timeline support, **When** the user opens the editor profiler panel, **Then** the UI clearly indicates unavailable data instead of crashing or presenting stale data as live.

### Edge Cases

- Profiling markers may be hit from multiple engine, editor, render, and worker threads at the same time; recorded data must remain coherent and must not introduce races.
- Render-thread and RHI-thread work must appear on recognizable tracks when threaded rendering is enabled, and must still be safe when those workers are not running.
- Recursive or deeply nested scopes must preserve parent-child timing relationships where the destination supports nesting.
- Event names derived from functions may include compiler-specific spelling; the displayed name must be stable enough for developers to recognize the originating function.
- A profiling destination may be compiled out, disabled at runtime, disconnected, or unsupported on the current platform/backend.
- The editor panel may be opened after profiling has already started; it should show available recent data or a clear empty state.
- GPU-oriented timeline data may be unavailable on non-supported graphics backends; CPU profiling must remain usable and unsupported GPU paths must not present stale or synthetic GPU timing as real data.
- Profiling should not allocate unexpectedly in extremely hot scopes when profiling is disabled.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a single developer-facing instrumentation point for scoped timing events that can feed both the external profiler destination and the editor timeline destination.
- **FR-002**: The system MUST support default event naming from the calling function name when the developer does not provide an explicit name.
- **FR-003**: The system MUST allow developers to override the automatic function-derived name with an explicit display name for scopes that need clearer labels.
- **FR-004**: The system MUST preserve matching begin/end timing boundaries for the same event across all enabled profiler destinations.
- **FR-005**: The system MUST support nested scope events and concurrent events from multiple threads without corrupting profiling output.
- **FR-006**: The system MUST make each profiler destination independently enableable so that one unavailable destination does not prevent the other from operating.
- **FR-007**: The system MUST allow profiling to be disabled such that existing instrumented code paths continue to build and run without requiring profiler tools to be present at runtime.
- **FR-008**: The editor MUST provide a dockable Profiler window that displays TimelineProfiler data as part of the normal editor window/menu workflow.
- **FR-009**: The editor Profiler window MUST communicate clearly when timeline data is unavailable, unsupported, paused, or empty.
- **FR-010**: The profiler system MUST expose enough state for tests or diagnostics to verify which destinations are enabled and whether profiling events are being accepted.
- **FR-011**: The profiler integration MUST document developer usage patterns for default function-name scopes, explicit named scopes, destination selection, and editor panel usage.
- **FR-012**: The system MUST keep generated-code directories out of manual profiler edits; any reflection or generated registration effects must use the normal generation workflow.
- **FR-013**: The profiling feature MUST remain compatible with existing runtime, editor, rendering, and unit-test workflows without introducing a parallel build or test process.
- **FR-014**: The system MUST provide a shared GPU profiling scope API that can be placed around RHI command recording without exposing TimelineProfiler headers to ordinary engine call sites.
- **FR-015**: The TimelineProfiler destination MUST report GPU scope capability only when its DX12 GPU profiler path is initialized against a supported native device and command queues.
- **FR-016**: The render frame, render-thread worker, RHI-thread worker, threaded RHI submission, command recording, queue submit, and present paths MUST include representative CPU profiling scopes.
- **FR-017**: The render thread and RHI thread MUST register stable profiler thread names when the threaded rendering workers run.

### Key Entities *(include if feature involves data)*

- **Profiling Scope Event**: A timed unit of work with a display name, start time, end time, thread identity, nesting context, and optional category.
- **GPU Profiling Scope Event**: A command-buffer-associated timed GPU range with a display name and source function that is recorded only by destinations with GPU scope capability.
- **Profiler Destination**: A profiling output target that receives scope events and publishes them to a tool or viewer. Destinations can be enabled, disabled, unavailable, or unsupported.
- **Profiler Session**: The active runtime state for collecting, routing, and presenting profiling events during one engine/editor run.
- **Editor Profiler Window**: A dockable editor panel that visualizes recent timeline data and communicates availability state.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A developer can add a new profiled scope for both profiler destinations by changing one call site and without typing the current function name.
- **SC-002**: In a profiling-enabled editor run, a known profiled CPU scope appears in both destinations with the same recognizable event name within one frame of being executed.
- **SC-003**: With profiling disabled, instrumented code paths run through existing tests with no profiler tool process or editor profiler window required.
- **SC-004**: The editor Profiler window can be opened, closed, and reopened during a session without crashing and displays either live timeline data or an explicit unavailable/empty state every time.
- **SC-005**: If one profiler destination is disabled or unavailable, at least one automated or focused manual verification demonstrates that the other enabled destination still receives profiling events.
- **SC-006**: Developer documentation enables a new contributor to add a default function-name scope and an explicit named scope in under 5 minutes.
- **SC-007**: In a DX12 profiling-enabled editor run, representative GPU pass scopes are routed to TimelineProfiler GPU tracks, while non-DX12 or uninitialized GPU paths clearly report unsupported GPU scope capability.
- **SC-008**: In threaded rendering mode, render-thread and RHI-thread CPU work appears under recognizable thread labels in available profiler destinations.

## Assumptions

- "docker窗口" is interpreted as a dockable editor window/panel integrated into the existing editor UI workflow.
- TimelineProfiler is intended as the in-editor timeline visualization destination, while Tracy is intended as the external, deeper profiling workflow.
- The first version focuses on CPU scoped events shared by both destinations; GPU timeline data is supported where the selected backend and destination can provide it, but lack of GPU data must not block CPU profiling.
- Destination-specific viewer capabilities may differ; the shared instrumentation contract is responsible for consistent event naming and boundaries, not identical UI presentation.
- Profiling availability will be controlled by existing project build/runtime configuration patterns rather than a new independent workflow.
- Existing editor panel/menu conventions and third-party dependency conventions should be reused during planning and implementation.
