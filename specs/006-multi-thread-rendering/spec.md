# Feature Specification: Multi-Thread Rendering Framework

**Feature Branch**: `006-multi-thread-rendering`
**Created**: 2026-04-18
**Status**: Draft
**Input**: User description: "检查当前渲染框架，实现多线程渲染，框架。分为逻辑主线程，渲染现场，和RHI线程"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Keep Game Logic Responsive While Rendering Runs Asynchronously (Priority: P1)

As an engine user running the editor or game runtime, I need gameplay and editor logic to stay on the logic main thread while rendering work is handed off to dedicated rendering stages, so that scene updates do not stall on same-thread GPU submission work.

**Why this priority**: This is the primary value of the feature. If logic still blocks on rendering orchestration, the thread split does not solve the intended problem.

**Independent Test**: Run a representative scene with the multithreaded rendering path enabled and confirm that logic frames continue advancing while rendering work from prior frames is still being processed and presented.

**Acceptance Scenarios**:

1. **Given** the engine is running a scene with active simulation, **When** a new frame begins on the logic main thread, **Then** logic state is advanced without directly executing backend submission or present work on that thread.
2. **Given** the renderer is still processing a previously prepared frame, **When** the logic main thread produces the next frame snapshot, **Then** the handoff either succeeds or applies bounded back-pressure without corrupting live scene state.
3. **Given** the rendering system is temporarily slower than logic updates, **When** the handoff queue reaches its allowed limit, **Then** the engine applies a predictable throttle policy instead of deadlocking or dropping ownership of frame data.

---

### User Story 2 - Build Renderable Scene Data On a Dedicated Render Scene Thread (Priority: P1)

As a rendering maintainer, I need scene parsing, visibility collection, render-pass preparation, and draw preparation to run on a dedicated render scene thread from an immutable frame snapshot, so that rendering no longer depends on reading mutable gameplay state directly.

**Why this priority**: The render scene thread is the contract boundary between live engine state and GPU work. If this stage is not isolated, the RHI thread cannot safely run in parallel.

**Independent Test**: Feed the renderer a scene containing opaque, transparent, skybox, offscreen, and editor helper content; verify that the render scene stage can prepare a complete frame package without reading live scene objects after the snapshot is taken.

**Acceptance Scenarios**:

1. **Given** the logic main thread publishes a frame snapshot, **When** the render scene thread begins work, **Then** it uses only snapshot-owned scene, camera, lighting, and render target data for that frame.
2. **Given** a scene contains multiple renderer paths such as forward, deferred, and editor overlays, **When** the render scene thread prepares the frame, **Then** it produces a complete render package for the active path without requiring gameplay-thread callbacks during preparation.
3. **Given** the snapshot contains no visible drawables or no active lights, **When** the render scene thread prepares the frame, **Then** it still produces a valid empty or minimal frame package that the next stage can consume safely.

---

### User Story 3 - Submit GPU Work On a Dedicated RHI Thread (Priority: P2)

As a backend maintainer, I need backend-facing command recording, queue submission, synchronization, and presentation to run on a dedicated RHI thread, so that backend rules stay centralized and independent from scene traversal work.

**Why this priority**: The RHI thread is the boundary that protects backend correctness and makes cross-backend scheduling behavior auditable.

**Independent Test**: Run supported runtime backends through a smoke scene and verify that command recording, queue submission, fence handling, swapchain acquisition, and presentation all complete from the RHI thread.

**Acceptance Scenarios**:

1. **Given** the render scene thread finishes preparing a frame package, **When** the RHI thread receives it, **Then** all backend submission, synchronization, and present steps happen from the RHI thread.
2. **Given** a frame targets an offscreen render target rather than the swapchain, **When** the RHI thread executes that frame, **Then** it completes recording and submission without requiring swapchain presentation.
3. **Given** the engine shuts down or the graphics device must stop accepting work, **When** the RHI thread drains in-flight frames, **Then** all remaining frame ownership is resolved without hanging the process.

---

### User Story 4 - Preserve Runtime and Editor Rendering Behavior During the Transition (Priority: P3)

As an editor or runtime user, I need scene views, game views, debug helpers, and resize behavior to remain correct while the multithreaded framework is introduced, so that the new architecture does not regress daily workflows.

**Why this priority**: A thread model change touches the full render path. Keeping visible behavior stable is required before any performance win matters.

**Independent Test**: Exercise editor scene view, game view, offscreen render targets, and swapchain resize flows with the multithreaded path enabled and confirm the visible output stays correct.

**Acceptance Scenarios**:

1. **Given** editor and runtime rendering both use the shared rendering framework, **When** the multithreaded path is enabled, **Then** both flows continue to produce correct scene imagery.
2. **Given** the window or swapchain size changes while frames are in flight, **When** the resize is applied, **Then** old in-flight work drains safely and subsequent frames render at the new size.
3. **Given** diagnostic passes or editor helper passes are active, **When** a frame is prepared and submitted, **Then** they continue to appear in the correct order with no duplicated or missing presentation.

### Edge Cases

- The logic main thread produces frames faster than the render scene thread or RHI thread can consume them.
- A frame contains only offscreen targets and never presents to the swapchain.
- Swapchain resize, surface loss, or window minimization happens while one or more frames are in flight.
- Shutdown begins while the render scene thread is preparing a frame and the RHI thread is still submitting a previous frame.
- A frame snapshot references resources that are created, updated, or released near the handoff boundary.
- A supported rendering path finishes GPU work later than expected and blocks reuse of per-frame resources.
- Frame diagnostics are requested while work is spread across multiple threads.
- The renderer must recover from an empty scene, a missing camera target, or a no-light scene without stalling any thread.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST introduce a three-stage rendering framework consisting of a logic main thread, a render scene thread, and an RHI thread.
- **FR-002**: The logic main thread MUST remain the owner of gameplay and editor state mutation and MUST hand rendering work to later stages through a frame snapshot rather than through shared mutable scene access.
- **FR-003**: The system MUST define a stable frame handoff contract that identifies when a frame snapshot is complete, when the render scene package is complete, and when the RHI thread has fully retired that frame.
- **FR-004**: The render scene thread MUST prepare culling results, scene draw data, pass ordering, and render-target usage from frame-owned data without requiring live callbacks into gameplay-thread objects during frame preparation.
- **FR-005**: The RHI thread MUST own backend-facing command recording, queue submission, synchronization, present decisions, and retirement of per-frame graphics resources.
- **FR-006**: The system MUST support bounded in-flight work between the three stages and MUST apply a documented back-pressure policy when any downstream stage falls behind.
- **FR-007**: The system MUST preserve current scene-rendering behavior for supported editor and runtime workflows while the multithreaded framework is enabled.
- **FR-008**: The system MUST preserve existing renderer-owned frame data, object data, lighting publication, scene-pass orchestration, and renderer statistics under the new thread model.
- **FR-009**: The system MUST define cross-thread ownership rules for frame snapshots, prepared scene data, transient upload work, render targets, and backend synchronization objects.
- **FR-010**: The system MUST prevent resource destruction, resize, or reuse from occurring before the owning frame has been retired by the RHI thread.
- **FR-011**: The system MUST handle offscreen rendering and swapchain presentation as distinct frame outcomes within the same multithreaded lifecycle.
- **FR-012**: The system MUST drain or cancel in-flight rendering work safely during shutdown, device reset, or renderer reinitialization without deadlock.
- **FR-013**: The system MUST keep editor-specific rendering paths, including scene views and helper passes, compatible with the same multithreaded lifecycle used by runtime rendering.
- **FR-014**: The system MUST expose enough diagnostics to identify frame stage ownership, in-flight depth, and whether a frame was throttled, dropped, cancelled, or presented.
- **FR-015**: The system MUST provide validation evidence for at least one supported runtime backend and one editor workflow showing that the multithreaded path produces correct output and stable frame completion.

### Key Entities *(include if feature involves data)*

- **Frame Snapshot**: An immutable description of a single logic frame's camera state, scene visibility inputs, renderer mode, targets, and submission requests handed from the logic main thread to the render scene thread.
- **Render Scene Package**: The prepared per-frame rendering work produced by the render scene thread, including ordered draw data, pass intentions, and target usage needed by the RHI thread.
- **RHI Submission Frame**: The backend-owned unit of work that the RHI thread records, submits, synchronizes, presents or resolves offscreen, and eventually retires.
- **In-Flight Frame Slot**: The bounded slot that tracks one frame's ownership as it moves through logic, render scene, RHI submission, and retirement.
- **Frame Retirement Signal**: The completion state that allows resources, snapshots, and staging data associated with a frame to be reused or released safely.
- **Thread Ownership Contract**: The rules that define which thread may read, prepare, submit, present, resize, or retire each rendering artifact at each frame stage.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A representative editor scene and a representative runtime scene each complete 300 consecutive frames with the multithreaded path enabled without deadlock, crash, or forced fallback to the old single-threaded behavior.
- **SC-002**: In supported validation scenes, 100% of rendered frames move through the ordered lifecycle of snapshot handoff, scene preparation, RHI submission, and retirement with no ambiguous ownership state.
- **SC-003**: Supported rendering workflows preserve correct visible output across normal scene rendering, offscreen rendering, and window resize verification for at least one editor flow and one runtime flow.
- **SC-004**: When downstream rendering stages fall behind, the engine reports the throttled or delayed frame outcome for 100% of affected frames instead of silently stalling.
- **SC-005**: Frame retirement prevents premature resource reuse in all validation runs, with zero observed cases of in-flight resource destruction, use-after-retire, or resize against still-owned frame data.
- **SC-006**: Stage-level diagnostics make it possible to attribute each completed frame to the logic main thread, render scene thread, and RHI thread during focused validation runs.

## Assumptions

- The requested "渲染现场" stage is interpreted as a dedicated render scene preparation thread between gameplay logic and backend submission.
- The initial scope is the rendering framework and frame lifecycle, not a broader asynchronous asset-streaming or job-system rewrite.
- Existing renderer-owned frame data, object data, lighting publication, and scene-pass orchestration remain foundational and should be adapted rather than replaced wholesale.
- The feature targets the currently supported runtime backends and editor workflows already recognized by the repository; unsupported backends are not part of the initial acceptance scope.
- The migration may be incremental, but each completed slice must leave the engine able to run a scene, retire frames safely, and shut down cleanly.
- Generated files under `Runtime/*/Gen/` remain out of scope for direct manual edits.
