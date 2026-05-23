# Feature Specification: Reduce Scene View Frame Stalls

**Feature Branch**: `015-fix-sceneview-drain`
**Created**: 2026-05-05
**Status**: Draft
**Input**: User description: "Editor frame rate is around 20 FPS; investigate and begin fixing the low frame rate caused by Scene View synchronous threaded-rendering drains."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Smooth Scene View Interaction (Priority: P1)

An editor user can keep the Scene View open with threaded rendering enabled and navigate the scene without the main thread synchronously completing every in-flight render frame for picking readback.

**Why this priority**: The observed low frame rate is tied to main-thread `DrainPendingRenderFrameBuildsSynchronously` work during normal Scene View updates.

**Independent Test**: Can be tested by opening the editor with Scene View visible and confirming that normal rendering does not require an immediate post-render threaded lifecycle drain solely for picking/readback readiness.

**Acceptance Scenarios**:

1. **Given** threaded rendering is enabled and Scene View is visible, **When** Scene View renders a frame without an active picking click, **Then** the view does not request immediate retired-frame readback for that frame.
2. **Given** a pending or readable picking frame already exists, **When** the user clicks in Scene View, **Then** picking uses the latest readable frame rather than forcing the just-submitted frame to retire synchronously.

---

### User Story 2 - Preserve Actor Picking (Priority: P2)

An editor user can still click actors in the Scene View after the synchronization reduction.

**Why this priority**: Removing the stall must not make the editor lose a core selection workflow.

**Independent Test**: Can be tested by selecting an actor in Scene View after at least one rendered picking frame is available.

**Acceptance Scenarios**:

1. **Given** the Scene View has produced a readable picking frame, **When** the user clicks on an actor, **Then** the actor can be selected using the readable frame data.
2. **Given** no readable picking frame is available yet, **When** the user clicks in Scene View, **Then** the editor avoids a synchronous frame drain and leaves selection unchanged.

---

### User Story 3 - Keep Resize Safety (Priority: P3)

An editor user can resize docked view panels without consuming invalid retired-frame resources.

**Why this priority**: Some view panels defer resize while in-flight frames exist; this safety behavior should stay intact while removing the per-frame Scene View readback drain.

**Independent Test**: Can be tested by resizing Scene View, Game View, and Asset View while threaded rendering has in-flight work.

**Acceptance Scenarios**:

1. **Given** a view requires retired-frame consumption and has in-flight frames, **When** its content region size changes, **Then** resize is deferred or synchronized using the existing retirement-aware resize behavior.
2. **Given** a view renders texture-only output, **When** it finishes rendering, **Then** it does not force a drain unless an immediate readback consumer truly requires it.

### Edge Cases

- First frames after editor startup may not have a readable picking frame yet; selection must remain stable instead of forcing synchronous completion.
- Picking disabled by diagnostics must continue to skip picking work without triggering immediate readback drains.
- Scene View resize while a frame is in flight must not expose stale or invalid framebuffer resources.
- Existing non-SceneView consumers that explicitly require immediate readback should retain their synchronous behavior.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Scene View MUST avoid marking every rendered frame as requiring immediate retired-frame readback for picking.
- **FR-002**: Scene View actor picking MUST consume the latest readable picking frame when available.
- **FR-003**: If no readable picking frame is available, Scene View actor picking MUST avoid forcing synchronous retirement of the just-submitted frame.
- **FR-004**: Retirement-aware resize behavior MUST remain active for views that require retired-frame resource safety.
- **FR-005**: Diagnostic options that disable picking MUST continue to prevent picking pass use and readback-dependent selection.
- **FR-006**: Existing explicit immediate-readback consumers outside this Scene View picking path MUST keep their current ability to request synchronous retirement.

### Key Entities

- **Scene View**: The editor viewport that renders editor overlays, picking data, and interactive scene manipulation controls.
- **Picking Frame**: A submitted/readable frame record used to map Scene View coordinates to scene actors.
- **Threaded Rendering Lifecycle**: The in-flight rendering pipeline whose frames can be published, prepared, submitted, and retired.
- **Immediate Readback Requirement**: A per-view flag that requests post-render synchronous retirement so readback data is available immediately.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Normal Scene View rendering no longer triggers the post-render immediate readback drain path when no consumer requires same-frame readback.
- **SC-002**: Actor picking remains functional once a readable picking frame has been produced.
- **SC-003**: The known main-thread stall signature, `DrainPendingRenderFrameBuildsSynchronously` under Scene View rendering, is reduced or absent during steady-state Scene View rendering without active immediate readback.
- **SC-004**: Relevant unit tests for view frame lifecycle and picking/readback behavior pass.

## Assumptions

- A one-frame delayed picking result is acceptable for interactive editor use and preferable to a steady frame-rate collapse.
- The current `PickingReadbackLifecycle` pending/readable model is intended to support delayed readback consumption.
- This change is scoped to the Scene View picking/readback stall and does not attempt to redesign threaded rendering, RHI submission, UI swapchain presentation, or profiling infrastructure.
- Runtime validation can be supplemented with Tracy or RenderDoc evidence after the code-level behavior is covered by tests.
