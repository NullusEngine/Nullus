# Feature Specification: Optimize Draw-Call Scalability

**Feature Branch**: `038-optimize-draw-calls`  
**Created**: 2026-05-29  
**Status**: Draft  
**Input**: User description: "Investigate how UE 4.27 solves low FPS when a scene has many draw calls, then start an optimization plan for Nullus. Use `F:\Epic Games\UE_4.27\Engine` as reference. Do not switch the main checkout; use a worktree."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Repeated Static Objects Stay Interactive (Priority: P1)

An engine user can place many compatible opaque static objects in a scene without the frame rate collapsing from one draw submission per object.

**Why this priority**: This is the direct draw-call bottleneck reported by the user and is the fastest path to a visible frame-rate improvement.

**Independent Test**: Create a stress scene or unit harness with at least 1,000 visible opaque objects sharing the same mesh, material, and render state. The renderer must submit grouped instance draws while preserving each object's transform and object-indexed shader data.

**Acceptance Scenarios**:

1. **Given** 1,000 compatible visible opaque objects, **When** the scene is rendered, **Then** the renderer reports at least a 90% reduction versus one draw operation per object.
2. **Given** a compatible object group whose transforms change but mesh/material/render state do not, **When** the next frame is prepared, **Then** draw command state is reused and only the visible per-object data changes.
3. **Given** compatible objects mixed with incompatible materials or meshes, **When** visible commands are gathered, **Then** only compatible objects are grouped and incompatible objects remain separately drawable.

---

### User Story 2 - Large Non-Groupable Scenes Avoid Single-Threaded Submission Stalls (Priority: P2)

An engine user can render scenes containing many visible but non-groupable draw commands without losing correctness when the renderer records bounded draw ranges. Attachment-free passes may use ordered work units. Attachment-backed DX12 scene passes use UE-inspired draw-range partitioning with DX12 bundle-backed in-render-pass child recording: the parent command buffer owns pass begin/end, attachment state, and barriers, while worker child command buffers record draw ranges that are executed inside the parent pass in deterministic order. This aligns with UE 4.27's parallel mesh draw command range partitioning goal, while using DX12 bundle semantics rather than UE's exact `FParallelCommandListSet` implementation.

**Why this priority**: Instancing cannot solve all draw-call-heavy scenes; unique meshes/materials still need faster command preparation and submission.

**Independent Test**: Create recorded-rendering tests with at least 2,000 visible non-groupable draw commands. Attachment-free passes must split into multiple ordered work units with no lost draws. Attachment-backed DX12-capable passes must record one parent render pass and multiple child draw-range command buffers with no child BeginRenderPass/EndRenderPass/barriers. Unsupported paths must remain correct and report a serial fallback reason when child recording is unavailable or unsafe.

**Acceptance Scenarios**:

1. **Given** an attachment-free pass with more draw commands than the configured threshold, **When** the package is materialized, **Then** the pass is split into ordered work units that collectively record all expected draws.
2. **Given** a backend or render target path that cannot safely use parallel command recording, **When** the same scene is rendered, **Then** the renderer falls back to the serial path without changing visual output.
3. **Given** an attachment-backed render pass with many draw commands and child command buffer support, **When** the pass is rendered, **Then** only the parent command buffer begins/ends the render pass and worker child command buffers record draw ranges executed inside that pass.
4. **Given** an attachment-backed render pass with many draw commands but no child command buffer support, **When** the pass is rendered, **Then** the renderer falls back to the original unsliced pass input and reports a serial fallback reason.

---

### User Story 3 - Dense Instance Fields Have a Scalable Follow-Up Path (Priority: P3)

An engine user can scale from repeated-object batching to very dense foliage or prop fields without the architecture blocking cluster-level culling and future indirect rendering.

**Why this priority**: UE's long-term answer for extreme repeated geometry includes ISM/HISM-style persistent instance buffers and hierarchy culling; the first Nullus change should not paint that path into a corner.

**Independent Test**: Document and model dense-instance groups so that later cluster culling can be added without replacing the P1/P2 draw command pipeline.

**Acceptance Scenarios**:

1. **Given** a very large repeated-instance group, **When** the design is reviewed, **Then** it identifies where per-instance bounds, visibility ranges, and instance-buffer ownership will live.
2. **Given** the MVP is complete, **When** dense-instance culling is deferred, **Then** the renderer still has measurable batching and parallel-command benefits without requiring the deferred feature.

---

### Edge Cases

- Transparent or order-dependent objects must not be merged in a way that changes back-to-front sorting.
- Materials or shaders that cannot consume indexed per-object data must keep the existing safe draw path.
- Existing material GPU-instancing settings must not be collapsed incorrectly into dynamic object grouping.
- Object-data ranges must not exceed the renderer's maximum object-data count.
- External scene outputs, editor Scene View, swapchain output, and unsupported backend capability paths must explicitly choose a safe serial fallback.
- A material parameter, shader, mesh, render-state, or culling-mode change must invalidate only the affected cached draw state.
- Frustum culling disabled/custom/model/mesh modes must preserve their current behavior.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The renderer MUST expose enough per-frame telemetry to compare raw visible objects before grouping, submitted scene draw counts after grouping, largest instance group, cache rebuilds, parallel command work units, and serial fallback reasons.
- **FR-002**: The renderer MUST reuse cached draw state across stable frames and rebuild it only when mesh, material, shader, render-state, or primitive-mode inputs change.
- **FR-003**: The renderer MUST group compatible opaque static objects into instanced submissions while preserving each object's transform and object data index.
- **FR-004**: The renderer MUST reject grouping for transparent objects, incompatible mesh/material/state combinations, incompatible shader object-data support, and pre-existing material instance counts that would change behavior.
- **FR-005**: The renderer MUST split grouped submissions during global object-index allocation so object-data limits are never exceeded and overflow never silently drops indexed draws.
- **FR-006**: The renderer MUST split large recorded draw sets into multiple ordered command work units only on paths that can preserve pass semantics for sliced work; attachment-backed passes MUST use renderpass-internal child command recording or fall back to unsliced serial recording.
- **FR-007**: The renderer MUST preserve pass clear/load behavior, resource visibility, and draw ordering when command work is split.
- **FR-008**: The renderer MUST provide an explicit serial fallback when parallel command recording or sliced work-unit submission is unavailable or unsafe; that fallback MUST record the original unsliced pass inputs or an explicitly proven serial-safe sliced plan.
- **FR-011**: Child command buffers used for in-render-pass parallel recording MUST NOT call BeginRenderPass, EndRenderPass, or resource barrier APIs; only the parent command buffer may own pass and visibility transitions.
- **FR-009**: The optimization MUST apply to the active forward/deferred scene draw paths without regressing Editor or Game runtime viability.
- **FR-010**: The plan MUST include a RenderDoc-backed validation route for DX12, plus targeted automated tests for grouping, cache invalidation, object-data ranges, and command-work splitting.

### Key Entities

- **Visible Object**: A scene object that passed activity and culling checks and can produce a drawable command.
- **Cached Draw State**: Stable mesh/material/render-state data reused across frames.
- **Instance Group**: A compatible set of opaque visible objects submitted as one or more instanced draws.
- **Command Work Unit**: A bounded subset of recorded commands that can be recorded serially or in parallel while preserving pass semantics.
- **In-Render-Pass Child Command Buffer**: A backend-gated command buffer recorded by a worker thread for draw commands only, then executed by the parent command buffer between BeginRenderPass and EndRenderPass.
- **Optimization Telemetry**: Per-frame counters used to prove draw-call reduction and fallback behavior.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A 1,000-object compatible opaque stress scene reports at least 90% fewer draw submissions than one draw per object.
- **SC-002**: A stable repeated-object scene reports no cached draw-state rebuilds on the second unchanged frame.
- **SC-003**: A 2,000-draw attachment-free recorded pass reports more than one ordered work unit and records all expected draws; a 2,000-draw attachment-backed DX12-capable scene pass reports one parent pass and multiple in-render-pass child draw ranges without duplicate clears.
- **SC-004**: Transparent draw ordering remains back-to-front in the existing sorting tests and is not grouped into instanced opaque-style batches.
- **SC-005**: A DX12 RenderDoc capture, when available, verifies grouped draw calls use instanced draw commands and confirms attachment-backed parallel draw ranges execute inside one parent render pass.
- **SC-006**: All affected unit tests pass, including render scene cache tests, renderer frame object binding tests, threaded rendering lifecycle tests, and DX12 draw-call contract tests.

## Assumptions

- The first implementation targets the active DX12 runtime path. Other backend enum paths remain capability-gated until they have direct evidence.
- The reported bottleneck is CPU draw submission/recording overhead for many visible scene objects, not primarily pixel cost, shader cost, or GPU bandwidth.
- The MVP focuses on opaque static mesh draw scalability. Transparent batching, GPU-driven indirect draws, and full HISM-style cluster culling are follow-up work.
- Existing indexed object-data shader support remains the compatibility boundary for dynamic grouping.
- Existing generated files under `Runtime/*/Gen/` and `Project/*/Gen/` are out of scope and must not be hand-edited.
