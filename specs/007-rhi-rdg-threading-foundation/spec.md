# Feature Specification: UE5-Style RHI/RDG Threading Foundation

**Feature Branch**: `007-rhi-rdg-threading-foundation`
**Created**: 2026-04-19
**Status**: Draft
**Input**: User description: "按 DX12 + Vulkan 作为第一阶段主目标，对标 UE5 的 Render Thread / RHI Thread / RDG / 并行命令录制 / 资源生命周期 / async compute / PSO 与 descriptor 管理，并直接开始落实"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Establish UE5-Style Thread Ownership Boundaries (Priority: P1)

As a rendering maintainer, I need the engine to use clear Game Thread, Render Thread, and RHI Thread ownership boundaries for DX12 and Vulkan, so that scene building, command translation, submission, and presentation are no longer mixed in ad hoc code paths.

**Why this priority**: Without stable thread ownership boundaries, every later goal in this feature becomes fragile. This is the foundation that prevents the renderer from regressing back into mixed ownership and direct-submit shortcuts.

**Independent Test**: Run the editor and game on DX12 and Vulkan with the new path enabled and confirm that frame build, translation, queue submission, and present are attributed to the intended stage with no main-thread runtime direct-submit fallback.

**Acceptance Scenarios**:

1. **Given** a runtime frame begins on the main thread, **When** the frame is prepared for rendering, **Then** scene construction work is handed to the Render Thread rather than directly recording backend work on the main thread.
2. **Given** a render-ready frame is produced, **When** backend submission begins, **Then** command translation, queue submit, synchronization, and present decisions are performed on the RHI Thread.
3. **Given** a frame is still in flight, **When** another stage attempts to reuse frame-owned resources or frame contexts, **Then** the engine blocks or delays reuse according to documented ownership rules instead of allowing overlap.

---

### User Story 2 - Make RDG the Authoritative Rendering Scheduler (Priority: P1)

As a renderer developer, I need the render graph to become the authoritative owner of pass dependencies, transient resources, external resource import/export, and barrier planning, so that frame execution is data-driven rather than hard-coded pass orchestration.

**Why this priority**: UE5-style multithreaded rendering depends on the render graph being the truth source for execution order and resource lifetime. If RDG remains optional or secondary, parallel recording and async compute cannot scale cleanly.

**Independent Test**: Execute representative forward and deferred frames through the render graph and confirm that imported targets, transient targets, pass dependencies, and extracted outputs are resolved entirely through the graph.

**Acceptance Scenarios**:

1. **Given** a frame uses swapchain targets, offscreen targets, and intermediate scene textures, **When** the frame is compiled, **Then** resource reads, writes, and extraction are described through the render graph rather than by handwritten per-pass sequencing.
2. **Given** a pass consumes transient resources created earlier in the same frame, **When** the graph executes, **Then** allocation, state transition, and release are driven by graph lifetime rather than by manual caller ownership.
3. **Given** a frame needs to expose one or more outputs outside the graph, **When** execution completes, **Then** those outputs are exported through explicit extraction rules rather than by keeping graph-internal resources alive implicitly.

---

### User Story 3 - Support Parallel Command Recording and Translation (Priority: P2)

As a backend maintainer, I need the renderer to support parallel command recording and translation for DX12 and Vulkan, so that independent render work can be prepared in parallel before final submission on the RHI Thread.

**Why this priority**: This is the first major scalability win after the ownership model is correct. It aligns Nullus with UE5's parallel command preparation model and prevents the render path from remaining effectively single-threaded.

**Independent Test**: Record a frame with multiple independent graphics passes and verify that pass recording work can execute in parallel, then merge into an ordered submission sequence on the RHI Thread for both DX12 and Vulkan.

**Acceptance Scenarios**:

1. **Given** a frame contains multiple independent graphics workloads, **When** the Render Thread prepares backend work, **Then** those workloads may be recorded in parallel without violating submission order or resource hazards.
2. **Given** parallel command recording is enabled, **When** the frame is submitted, **Then** the RHI Thread merges and submits those command lists using explicit backend ordering and synchronization.
3. **Given** a pass cannot participate in parallel recording because of dependencies or backend limits, **When** the frame is compiled, **Then** the engine falls back to a documented serial path without corrupting frame results.

---

### User Story 4 - Add Explicit Resource Lifetime, Async Compute, and PSO/Descriptor Management (Priority: P2)

As a rendering architect, I need transient resource lifetime, async compute scheduling, and PSO/descriptor management to be explicit, centralized, and backend-aware for DX12 and Vulkan, so that the engine can scale beyond a basic graphics-only command path.

**Why this priority**: This is the difference between "threaded rendering exists" and "UE5-style rendering infrastructure exists". Resource lifetime, async compute, and pipeline/binding management are the systems that make the threading model sustainable under real workloads.

**Independent Test**: Run representative frames that use transient render targets, graphics-to-compute dependencies, and repeated pipeline/binding reuse, and confirm correct execution plus observable cache/allocation behavior on DX12 and Vulkan.

**Acceptance Scenarios**:

1. **Given** a frame creates temporary render targets and buffers, **When** the graph executes and the frame retires, **Then** those resources are allocated, transitioned, and released through explicit per-frame lifetime rules.
2. **Given** a frame contains a compute workload that can overlap with graphics work, **When** async compute is enabled and backend capabilities allow it, **Then** the workload is scheduled on an explicit compute path with correct cross-queue synchronization.
3. **Given** repeated frames use the same or compatible pipelines and descriptor layouts, **When** work is submitted across frames, **Then** PSO creation and descriptor allocation reuse are observable and do not devolve into per-draw rebuild behavior.

### Edge Cases

- A frame mixes swapchain output, offscreen output, and extracted intermediate resources.
- A render graph pass needs to read a resource that was previously written by graphics and then consumed by compute in the same frame.
- A transient resource must remain alive across queue boundaries until a fence or external access point is reached.
- DX12 and Vulkan expose different queue or descriptor constraints for the same frame topology.
- The render graph compiles a frame where some passes are parallelizable and others must remain serial.
- The engine must drain or cancel in-flight graphics and compute work during resize, shutdown, or device recreation.
- A pipeline or descriptor cache miss occurs in a hot path and must not cause ownership violations or per-draw rebuild loops.
- Legacy DX11/OpenGL/Metal code remains in the tree while DX12/Vulkan become the only Tier A path for this feature.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST define a DX12 and Vulkan first-stage rendering architecture with explicit Game Thread, Render Thread, and RHI Thread ownership boundaries.
- **FR-002**: The Render Thread MUST become the authoritative stage for render graph construction, scene render packet construction, and pass scheduling decisions.
- **FR-003**: The RHI Thread MUST become the authoritative stage for backend command translation, queue submission, synchronization, and presentation for the new path.
- **FR-004**: The system MUST remove runtime direct-submit fallback as the normal execution path for DX12 and Vulkan in the new architecture.
- **FR-005**: The render graph MUST become the authoritative owner of pass dependencies, transient resource lifetime, and imported or extracted external resources.
- **FR-006**: The system MUST support explicit import and extraction of graph-external resources so that swapchain, editor views, offscreen targets, and inter-system handoff remain compatible with the graph.
- **FR-007**: The system MUST support parallel command recording or translation for independent render work on DX12 and Vulkan where backend constraints allow it.
- **FR-008**: The system MUST provide a correct serial fallback for workloads that cannot be parallelized without changing visible results.
- **FR-009**: The system MUST define explicit lifetime rules for transient textures, transient buffers, frame contexts, descriptor allocations, and queue synchronization artifacts.
- **FR-010**: The system MUST prevent transient resource reuse or destruction before the owning frame and relevant queue work are fully retired.
- **FR-011**: The system MUST support explicit async compute scheduling and cross-queue synchronization for workloads that are marked eligible and supported by backend capabilities.
- **FR-012**: The system MUST expose backend capability reporting that distinguishes graphics-only support from compute queue, copy queue, async compute, and explicit descriptor or binding model support.
- **FR-013**: The system MUST centralize PSO lookup, creation, reuse, and cache diagnostics for DX12 and Vulkan.
- **FR-014**: The system MUST centralize descriptor allocation lifetime, reuse policy, and diagnostics for DX12 and Vulkan.
- **FR-015**: The system MUST preserve editor and game product paths during migration, including resize, shutdown, and offscreen rendering, while keeping DX12 and Vulkan as the only Tier A targets for this feature.
- **FR-016**: The system MUST degrade unsupported backends truthfully by gating or fallback-to-legacy isolation rather than by pretending the new architecture is fully supported on them.
- **FR-017**: The system MUST provide diagnostics for thread ownership, graph compilation, parallel command recording, transient lifetime, PSO cache activity, and descriptor allocation behavior.
- **FR-018**: The system MUST provide validation evidence on both DX12 and Vulkan for at least one game runtime flow and one editor flow on the new path.

### Key Entities *(include if feature involves data)*

- **Render Thread Frame Build**: The per-frame work unit that owns scene-derived render preparation and render graph construction before backend translation begins.
- **RHI Submission Batch**: The per-frame or per-queue backend submission unit produced from render-graph-compiled work and executed by the RHI Thread.
- **Render Graph Pass**: A pass definition that declares resource reads, writes, queue intent, and execution dependencies.
- **Transient Resource Lifetime**: The tracked lifetime interval that determines when a temporary buffer or texture may be allocated, transitioned, reused, extracted, or released.
- **Parallel Recording Work Unit**: An independent portion of render work that may be recorded or translated in parallel before ordered submission.
- **Async Compute Work Unit**: A graph-declared compute workload that may run on a compute queue when capabilities and dependencies allow it.
- **PSO Cache Entry**: A reusable compiled pipeline object identified by a stable pipeline key and backend-specific realization.
- **Descriptor Allocation Lifetime**: The ownership interval for transient and persistent descriptor allocations across a frame or longer-lived resource use.
- **External Resource Access Point**: An explicit import or extraction boundary between render-graph-owned resources and systems outside the graph.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: DX12 and Vulkan each complete 300 consecutive editor frames and 300 consecutive game frames on the new path without deadlock, crash, or forced runtime fallback to the old direct-submit path.
- **SC-002**: In focused validation runs, 100% of tested frames on the new path can be attributed to the intended Game Thread, Render Thread, and RHI Thread lifecycle with no ambiguous ownership state.
- **SC-003**: Representative forward and deferred frames execute through the render graph with explicit import or extraction for all tested external targets, with zero observed implicit lifetime leaks in validation runs.
- **SC-004**: A representative frame containing at least two independent graphics workloads demonstrates successful parallel command recording or translation on both DX12 and Vulkan, or produces an explicit diagnostic explaining why serial fallback was used.
- **SC-005**: A representative frame containing at least one eligible compute workload demonstrates correct async compute scheduling and synchronization on supported backends, or emits a truthful capability-based disable reason.
- **SC-006**: PSO and descriptor diagnostics show stable reuse behavior across repeated validation frames, with no per-draw recreation pattern observed in the focused profiling runs chosen for acceptance.
- **SC-007**: Resize, shutdown, and frame retirement validation produce zero observed cases of transient resource reuse before retirement or queue completion in the targeted acceptance runs.

## Assumptions

- The first implementation phase intentionally targets DX12 and Vulkan as Tier A paths and does not require feature parity on DX11, OpenGL, or Metal.
- Existing `006-multi-thread-rendering` work is treated as a precursor and integration baseline rather than as the final architecture for this feature.
- Editor and game product paths must remain runnable during migration, even if some subsystems temporarily route through staged compatibility layers.
- Nullus will continue using `Driver` as the top-level graphics entry point during this phase, but internal ownership beneath `Driver` may be substantially reorganized.
- The current frame graph utilities and threaded rendering skeleton are not assumed to already satisfy UE5-style architecture requirements; they may be replaced, narrowed, or absorbed into new abstractions.
- Generated files under `Runtime/*/Gen/` remain out of scope for manual edits in this feature.
