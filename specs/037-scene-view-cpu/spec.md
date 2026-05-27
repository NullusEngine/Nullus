# Feature Specification: Editor Scene View CPU Frame-Time Optimization

**Feature Branch**: `037-scene-view-cpu`  
**Created**: 2026-05-27  
**Status**: Draft  
**Input**: User description: "Optimize editor Scene View CPU frame time by reducing redundant deferred renderer preparation work observed in trace.json, starting with stable-size GBuffer resource reuse and measurable main-thread frame-time validation."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Scene View Rendering Is Smoother (Priority: P1)

An editor user keeps the Scene View open at a stable size while inspecting a scene and expects the editor to remain responsive rather than spending most of each frame repeating unchanged render preparation.

**Why this priority**: The provided trace shows the dominant frame-time cost in the Scene View render path, so reducing redundant work there gives the largest immediate improvement.

**Independent Test**: Run the focused renderer regression tests and a short editor trace with the same stable Scene View size; verify that the Scene View render preparation scopes are reduced without visual or runtime regressions.

**Acceptance Scenarios**:

1. **Given** the Scene View size is unchanged across consecutive frames, **When** the editor renders those frames, **Then** the renderer avoids repeating resource preparation that is only needed after size or attachment changes.
2. **Given** the same scene is rendered before and after the optimization, **When** a frame is captured or traced, **Then** the Scene View remains visible and the relevant main-thread preparation scopes do not regress.

---

### User Story 2 - Frame-Time Evidence Is Trustworthy (Priority: P2)

An engine developer records timeline evidence and expects the exported trace to represent completed events so performance conclusions are not distorted by invalid or partially recorded scopes.

**Why this priority**: The supplied trace contains invalid durations and truncated-looking records outside the main thread, which makes follow-up optimization harder to validate.

**Independent Test**: Export or inspect a timeline recording and confirm exported events exclude incomplete timing records and do not emit negative durations.

**Acceptance Scenarios**:

1. **Given** a profiling event has not completed, **When** trace export runs, **Then** that event is not emitted as a completed duration event.
2. **Given** profiling data is exported, **When** it is loaded into a trace viewer or aggregation script, **Then** durations are non-negative and usable for performance analysis.

---

### Edge Cases

- The Scene View is resized repeatedly or minimized; resource preparation must still occur when dimensions or required attachments change.
- The editor runs with threaded rendering disabled or without a render backend; existing fallback behavior must remain unchanged.
- The profiler panel is open while recording; timeline UI overhead may still appear in the trace but must not corrupt exported event durations.
- The optimization is validated on one backend only; conclusions must state the validated backend and must not claim cross-backend proof.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST preserve visible Scene View rendering while reducing redundant CPU preparation in stable-size frames.
- **FR-002**: The renderer MUST distinguish stable render target dimensions from actual resize or attachment-change conditions.
- **FR-003**: The renderer MUST continue to refresh resources when the Scene View size changes, becomes valid after being zero-sized, or changes required attachments.
- **FR-004**: The optimization MUST be covered by focused automated regression tests where stable entrypoints exist.
- **FR-005**: The performance validation MUST record exact evidence, including baseline trace observations and post-change test or runtime measurements.
- **FR-006**: The timeline trace export MUST avoid completed-event records with invalid or negative durations.
- **FR-007**: The change MUST avoid hand-editing generated output and MUST preserve current editor and game runtime entrypoints.

### Key Entities

- **Scene View Frame**: A single editor frame that includes Scene View update, render preparation, rendering, UI composition, and presentation.
- **Stable Render Target**: A render target whose dimensions and attachment requirements match the previous usable frame.
- **Performance Trace**: Timeline evidence used to compare baseline and post-change frame-time behavior.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On the provided baseline trace, the primary cause of low frame rate is documented with exact scope names and timings before code changes.
- **SC-002**: Focused regression tests fail before the optimization and pass after it, proving stable-size redundant preparation is removed.
- **SC-003**: In a comparable editor run, the stable Scene View preparation cost is reduced or, if runtime capture is unavailable, the automated tests demonstrate the eliminated redundant operation and the limitation is documented.
- **SC-004**: Exported profiling records used for validation contain no negative-duration completed events.
- **SC-005**: The editor remains runnable after the change for the validated backend and no claims are made for unvalidated backends.

## Assumptions

- The initial optimization slice targets the CPU main-thread Scene View bottleneck shown in `App/Win64_Debug_Runtime_Shared/trace.json`.
- The first implementation should be narrow and reversible: resource reuse on stable render target size plus trace export validity.
- DX12 is the backend represented by the supplied trace; other backends require separate validation before claiming equivalent improvement.
- RenderDoc is not required for the first CPU-only optimization unless later evidence points to GPU pass correctness or synchronization behavior.
