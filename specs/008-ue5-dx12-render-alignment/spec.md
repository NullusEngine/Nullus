# Feature Specification: UE5-Style DX12 Render Alignment

**Feature Branch**: `008-ue5-dx12-render-alignment`
**Created**: 2026-04-21
**Status**: Draft
**Input**: User description: "详细研究UE5渲染架构后再制定方案，确保底层渲染框架和UE完全对齐"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Establish Single Authoritative Rendering Ownership (Priority: P1)

As a rendering maintainer, I need Nullus to use a UE5-style Game Thread, Render Thread, and RHI Thread ownership model, so that Game and Editor rendering no longer execute through mixed same-thread shortcuts or ambiguous submission paths.

**Why this priority**: This is the contract that everything else depends on. If frame ownership remains mixed, RDG, Editor integration, and DX12 submission cannot become clean or auditable.

**Independent Test**: Run focused Editor and Game validation on DX12 and confirm that mutable engine state stays on the Game Thread, frame build happens on the Render Thread, and backend submission/present/readback happen on the RHI Thread without compatibility bypasses.

**Acceptance Scenarios**:

1. **Given** gameplay or editor state changes on the Game Thread, **When** a new render frame begins, **Then** the Game Thread publishes immutable render input and does not directly record, submit, or present backend work.
2. **Given** a frame is accepted for rendering, **When** rendering preparation begins, **Then** the Render Thread becomes the sole authoritative owner of frame build, view setup, and graph construction.
3. **Given** a render-ready frame is produced, **When** backend execution starts, **Then** the RHI Thread alone performs command translation, queue submission, synchronization, present decisions, and retirement.

---

### User Story 2 - Make RDG The Only Rendering Scheduler (Priority: P1)

As a renderer developer, I need RDG to become the sole truth source for pass ordering, resource lifetime, external resource import/extract, and readback scheduling, so that runtime and editor rendering stop relying on handwritten orchestration outside the graph.

**Why this priority**: UE5 alignment depends on RDG being authoritative. If pass order or resource lifetime can still be decided elsewhere, the architecture stays split and fragile.

**Independent Test**: Execute representative Editor and Game frames on DX12 and confirm that scene color, depth, offscreen targets, picking readback, and editor auxiliary passes all compile and execute through the graph.

**Acceptance Scenarios**:

1. **Given** a frame uses swapchain targets, offscreen targets, and extracted outputs, **When** the frame is compiled, **Then** resource import, pass order, and extraction are described only through RDG.
2. **Given** a frame contains scene passes plus editor-only passes such as gizmo, grid, outline, or picking, **When** the frame is compiled, **Then** those passes appear as graph-visible work rather than side-channel rendering.
3. **Given** a frame requests readback for picking or validation, **When** the graph executes, **Then** readback scheduling and completion are represented as explicit graph-owned or graph-visible work rather than a bypass path.

---

### User Story 3 - Remove Compatibility Paths And Fallbacks From The DX12 Mainline (Priority: P2)

As a backend maintainer, I need the DX12 runtime path to use one authoritative rendering architecture with explicit failure behavior, so that compatibility paths, fallback rendering, and emergency orchestration code no longer remain in the normal execution surface.

**Why this priority**: A clean architecture is impossible while the engine still keeps old execution routes alive in the mainline.

**Independent Test**: Run focused unit and runtime validation proving that DX12 frames no longer use direct-submit shortcuts, driver-built fallback packages, or compatibility acquire/present branches, and that DX12 initialization failures stop explicitly instead of falling back.

**Acceptance Scenarios**:

1. **Given** DX12 is available and the engine is rendering a frame, **When** the frame completes, **Then** no compatibility or fallback render path is used.
2. **Given** DX12 cannot be created or validated at startup, **When** the product launches, **Then** startup fails truthfully instead of falling back to another rendering path.
3. **Given** a workload cannot be parallelized or optimized, **When** it executes, **Then** it may use a serial schedule inside the same authoritative architecture, but it does not switch to a compatibility renderer or fallback submission path.

---

### User Story 4 - Unify Editor And Game Under The Same Render Architecture (Priority: P2)

As an engine user, I need Editor and Game rendering to share the same render architecture, so that scene view, game view, offscreen rendering, picking, gizmo, and overlays behave as first-class uses of the same frame pipeline instead of living on editor-only side paths.

**Why this priority**: UE-style alignment is not complete if the Editor still keeps special rendering bypasses.

**Independent Test**: Validate DX12 Editor and Game flows with scene view, game view, offscreen targets, picking, gizmo, and overlays enabled, and confirm that all of them execute through the same thread ownership and RDG pipeline.

**Acceptance Scenarios**:

1. **Given** the Editor renders scene view and game view in the same session, **When** frames are built and executed, **Then** both views consume the same authoritative render pipeline with different graph inputs rather than different submission paths.
2. **Given** picking, gizmo, grid, outline, or editor overlays are active, **When** a frame is compiled, **Then** they appear as part of the same frame pipeline rather than as an editor-only bypass.
3. **Given** an offscreen render or viewport readback is requested, **When** the frame retires, **Then** the result is produced and retired through the same authoritative frame ownership model as the visible frame.

---

### User Story 5 - Enforce Central Rendering Infrastructure As Mandatory Mainline (Priority: P2)

As a rendering architect, I need PSO lookup, descriptor lifetime, transient resource lifetime, and frame retirement to be mandatory centralized systems, so that the architecture stays pure and no pass or subsystem can secretly reintroduce its own mini-renderer.

**Why this priority**: Even with correct thread ownership and RDG usage, the architecture remains impure if low-level resource systems are optional or bypassable.

**Independent Test**: Run focused DX12 validation and unit tests that prove graphics/compute pipelines, bindings, transient graph resources, and frame retirements all flow through centralized infrastructure with no accepted bypasses.

**Acceptance Scenarios**:

1. **Given** a frame needs graphics or compute pipelines, **When** those pipelines are acquired, **Then** they are resolved through centralized PSO management rather than per-pass ad hoc creation.
2. **Given** a frame allocates transient graph resources or descriptor-backed bindings, **When** the frame executes and retires, **Then** lifetime and reuse are controlled only by centralized systems.
3. **Given** diagnostics are collected during validation, **When** results are reviewed, **Then** they can prove that no accepted frame used bypass resource-management paths.

### Edge Cases

- An Editor frame mixes scene view, game view, offscreen rendering, hit-proxy or picking readback, gizmo, and UI overlay work.
- A resize, shutdown, or device-loss event begins while one or more DX12 frames are still in flight.
- A frame contains no visible scene geometry but still needs editor overlays, readback, or offscreen results.
- A picking request completes after the visible frame has presented and must still obey frame retirement ownership.
- DX12 initialization is unavailable, partially valid, or degraded; the engine must stop explicitly rather than switching to another runtime path.
- A frame must run serially for correctness or capability reasons, but must still remain inside the authoritative RT/RDG/RHI architecture.
- A later multi-backend phase must add another backend without reviving deleted fallback or compatibility paths in renderer, editor, or game code.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST use a UE5-style rendering ownership baseline in which mutable gameplay and editor state remain on the Game Thread, authoritative frame build happens on the Render Thread, and backend execution happens on the RHI Thread.
- **FR-002**: The Game Thread MUST publish immutable render input and MUST NOT directly perform normal DX12 command recording, queue submission, presentation, or readback.
- **FR-003**: The Render Thread MUST become the sole authoritative owner of runtime and editor frame build, including view preparation, pass selection, and RDG construction.
- **FR-004**: The RHI Thread MUST become the sole authoritative owner of DX12 command translation, queue submission, synchronization, present, readback completion, and frame retirement.
- **FR-005**: DX12 MUST be the only active runtime backend in the first implementation phase of this feature.
- **FR-006**: If DX12 cannot be created or validated for this phase, startup MUST fail explicitly and MUST NOT fall back to another rendering path.
- **FR-007**: RDG MUST become the sole truth source for pass ordering, dependency resolution, transient resource lifetime, external resource import/extract, and readback scheduling.
- **FR-008**: Runtime and Editor rendering MUST use the same authoritative frame pipeline, with differences expressed through graph inputs and pass content rather than alternate submission paths.
- **FR-009**: Scene view, game view, offscreen rendering, picking, gizmo, grid, outline, and overlay work MUST be represented as graph-visible frame work.
- **FR-010**: The system MUST remove normal-path main-thread explicit frame recording and submission APIs from runtime and editor rendering entrypoints.
- **FR-011**: The system MUST remove normal-path driver-built compatibility scene-package construction, on-demand present/acquire compatibility behavior, and equivalent fallback orchestration branches.
- **FR-012**: The system MUST allow serial execution only as a scheduling decision inside the same authoritative Render Thread, RDG, and RHI Thread architecture, not as a compatibility or legacy fallback path.
- **FR-013**: The system MUST centralize PSO lookup, descriptor allocation lifetime, transient resource lifetime, and frame retirement as mandatory mainline systems.
- **FR-014**: Backend-specific execution logic for the first phase MUST remain inside DX12 backend implementations or narrow backend-facing interfaces rather than spreading through renderer, editor, or game code.
- **FR-015**: Phase-1 renderer and editor code MUST avoid active backend-specific execution branching except for explicit startup or capability gating needed to select the active phase backend.
- **FR-016**: The architecture MUST preserve a future path to support additional backends without restoring deleted compatibility paths or fallback architecture.
- **FR-017**: The system MUST provide diagnostics that can prove whether any accepted frame used a forbidden compatibility path, fallback path, or resource-management bypass.
- **FR-018**: The system MUST validate Editor and Game DX12 flows with explicit evidence for scene view, game view, offscreen rendering, picking, gizmo, and overlay behavior.
- **FR-019**: The system MUST preserve safe frame retirement for visible frames, offscreen frames, extracted resources, and readback requests during resize, shutdown, and device-loss handling.
- **FR-020**: The system MUST treat UE5.7 public rendering contracts as the baseline for architectural alignment and MUST require a documented source-level audit before claiming closure as fully aligned.

### Key Entities *(include if feature involves data)*

- **Game Frame Input**: The immutable frame-owned description published by the Game Thread for runtime or editor rendering.
- **Render Frame Build**: The authoritative Render Thread product that owns view data, graph construction inputs, and frame execution intent before backend submission.
- **Graph External Resource**: An explicitly imported or extracted resource that crosses the boundary between RDG-owned work and systems outside the graph.
- **Editor Auxiliary Pass Request**: A frame-owned request for editor-specific rendering such as picking, gizmo, grid, outline, or overlay work.
- **DX12 Submission Batch**: The backend-facing execution batch owned by the RHI Thread for command translation, queue submission, synchronization, and retirement.
- **Readback Request**: A frame-owned request whose scheduling, synchronization, completion, and retirement must stay inside the authoritative frame pipeline.
- **Frame Retirement Token**: The state that proves a frame and all of its resources, extracted outputs, and readbacks are safe to reuse or release.
- **Pipeline Cache Entry**: The centralized reusable pipeline identity used to prevent per-pass ad hoc PSO creation.
- **Descriptor Allocation Scope**: The centrally managed lifetime interval for transient or persistent descriptor-backed bindings.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: DX12 Editor and Game each complete 300 consecutive validation frames on the new path without deadlock, crash, or execution through a forbidden compatibility or fallback render path.
- **SC-002**: In focused validation runs, 100% of accepted frames can be attributed unambiguously to Game Thread publication, Render Thread frame build, and RHI Thread execution or retirement.
- **SC-003**: DX12 validation proves that scene view, game view, offscreen rendering, picking, gizmo, and overlays all execute successfully through the same authoritative frame pipeline.
- **SC-004**: In accepted DX12 validation runs, forbidden compatibility-path diagnostics and fallback-path diagnostics remain at zero for all completed frames.
- **SC-005**: A targeted architectural audit of designated renderer, editor, and runtime entrypoints finds zero accepted call sites for removed direct-submit frame APIs, driver-built fallback package APIs, or compatibility acquire/present APIs.
- **SC-006**: DX12 startup failure validation proves that unsupported or unavailable DX12 configurations stop explicitly and never continue by switching to another render path.
- **SC-007**: Focused validation demonstrates that centralized PSO, descriptor, transient-resource, and retirement diagnostics account for 100% of accepted frame execution, with zero accepted bypass events.

## Assumptions

- The first implementation phase targets DX12 only on the validated Windows product path.
- Future multi-backend support remains an architecture goal, but no additional runtime backend is part of this feature's first-phase acceptance.
- Alignment is measured against UE5.7 public rendering contracts first; a documented source-level audit is still required before declaring full closure as "completely aligned".
- `Driver` remains the repository's graphics entry point during this feature, but it is expected to lose mixed orchestration responsibilities as ownership moves to explicit Render Thread and RHI Thread services.
- Generated files under `Runtime/*/Gen/` remain out of scope for manual edits.
