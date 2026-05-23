# Research: Unified Profiler Integration

## Decision: Use a Nullus profiler facade as the only instrumentation surface

**Rationale**: The spec requires one marker to feed Tracy and TimelineProfiler. A small facade in `Runtime/Base/Profiling` gives all engine layers a stable API while keeping third-party headers and destination-specific behavior out of most call sites. It also allows disabled profiling builds to remain cheap and lets tests use destination doubles.

**Alternatives considered**:

- Directly place Tracy and TimelineProfiler macros at every call site: rejected because it duplicates markers and makes destinations drift.
- Make TimelineProfiler the primary profiler and export to Tracy later: rejected because Tracy is a distinct external live profiling workflow and should receive events from the start.
- Put the facade in `Project/Editor`: rejected because runtime/game/render code also needs shared instrumentation.

## Decision: Support CPU scoped events first; make GPU timeline support capability-driven

**Rationale**: Shared CPU scopes satisfy the common-marker requirement and can be validated across ordinary unit/editor runs. TimelineProfiler advertises CPU/GPU profiling but its GPU path is D3D12-oriented, while Nullus has multiple graphics backends. GPU support should appear only when the active backend and destination support it, with the editor panel showing an unsupported state otherwise.

**Alternatives considered**:

- Require GPU event support for all backends in v1: rejected because it would force unvalidated backend assumptions.
- Skip GPU entirely forever: rejected because the profiler design should leave a clean path for backend-specific GPU scopes.

## Decision: Use automatic function-name labels by default with explicit label override

**Rationale**: Developers should be able to add a default scope without typing the current function name. The facade should capture the compiler-provided function spelling at the call site for default labels, and it should still accept a literal or stable string label when the function name is too noisy or a broader category is clearer.

**Alternatives considered**:

- Require all labels to be manually typed: rejected because it violates the spec and increases maintenance during renames.
- Always use only function names: rejected because some scopes need domain labels such as "Render Scene View" or "Asset Import".

## Decision: Keep destination enablement independent at build and runtime

**Rationale**: Tracy may be useful in standalone/editor runs even when the TimelineProfiler panel is not present, and the editor panel should still be usable when a Tracy viewer is not running. Independent toggles also preserve CI, release, and contributor environments where third-party tools may be absent.

**Alternatives considered**:

- One global "profiling on/off" flag only: rejected because one unavailable destination would disable the other.
- Always compile/link both destinations: rejected because it makes normal builds fragile and increases platform friction.

## Decision: Embed TimelineProfiler in a dockable `ProfilerPanel`

**Rationale**: The editor already uses `PanelWindow`, `PanelsManager`, `EditorTopBar`, and menu registration for dockable tools. A `ProfilerPanel` can follow the same lifecycle, expose clear empty/unavailable states, and avoid inventing a new UI system.

**Alternatives considered**:

- Make the profiler a fixed viewport bar: rejected because timeline inspection needs resizable space and belongs with tool panels.
- Launch TimelineProfiler as a separate external window: rejected because the requested experience is built into the editor.

## Decision: Validate with test doubles plus focused real-tool manual checks

**Rationale**: Unit tests can prove begin/end pairing, label selection, nesting, disabled behavior, and destination independence without requiring Tracy or an editor UI. Real Tracy/TimelineProfiler visibility still needs focused manual verification because these are visual/tool integrations.

**Alternatives considered**:

- Only manual testing: rejected because shared instrumentation semantics are easy to regress.
- Fully automate visual profiler viewer checks in v1: rejected because the setup cost is high and brittle compared with the current project test surface.
