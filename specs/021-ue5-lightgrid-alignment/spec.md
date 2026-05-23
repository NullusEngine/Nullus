# Feature Specification: UE5 LightGrid Alignment

**Feature Branch**: `020-lightgrid-performance-toggle`  
**Created**: 2026-05-10  
**Status**: Draft  
**Input**: User description: "继续LightGrid ，要求和UE5实现方式完全对齐"; licensed reference path supplied: `F:\Epic Games\UE_4.27\Engine`

## User Scenarios & Testing

### User Story 1 - Match UE5 Light Grid Shape (Priority: P1)

An engine developer can render forward/deferred scenes using a LightGrid whose cell sizing, Z slice count, and light culling capacity follow UE5's forward local light grid defaults instead of Nullus-specific fixed dimensions.

**Why this priority**: The current Nullus grid is fixed at 16x9x24 with 128 lights per cluster. UE5 exposes a pixel-sized XY grid, 32 Z slices, and a default culling capacity of 32, so matching this contract is the first visible alignment point.

**Independent Test**: Can be tested by building LightGrid frame data for several viewport sizes and verifying that XY dimensions derive from a 64-pixel cell size, Z defaults to 32, and capacity defaults to 32.

**Acceptance Scenarios**:

1. **Given** a 1920x1080 render target, **When** LightGrid frame data is prepared, **Then** grid dimensions are `ceil(width / 64)` by `ceil(height / 64)` rather than fixed 16x9 dimensions.
2. **Given** a scene using default LightGrid settings, **When** LightGrid constants are generated, **Then** the Z slice count is 32 and the max culled lights per cell is 32.
3. **Given** the render size changes, **When** the next frame prepares LightGrid data, **Then** the XY grid size updates consistently from the render size and pixel cell size.

---

### User Story 2 - Match UE5 Culling Data Flow (Priority: P1)

An engine developer can inspect the LightGrid GPU work and see the same UE reference flow: dispatch compute over frustum-space grid cells, build a per-cell view-space AABB, test local lights against the cell, store per-cell light references, and compact the result into a buffer consumed by base/deferred lighting.

**Why this priority**: The user explicitly asked for UE5 implementation alignment. The current two-pass inject/compact flow is close in spirit, but its settings, data layout, and overflow semantics are Nullus-specific.

**Independent Test**: Can be tested by comparing CPU-side frame-data contracts and shader resource layouts against the UE5-aligned contract without requiring a live GPU.

**Acceptance Scenarios**:

1. **Given** a scene with local point and spot lights, **When** LightGrid culling runs, **Then** each compute thread owns a grid cell and assigns only lights intersecting that cell's view-space AABB.
2. **Given** a cell with more lights than the fixed capacity path allows, **When** LightGrid culling runs with linked-list culling disabled, **Then** the cell is clamped to the configured fixed capacity.
3. **Given** linked-list culling is enabled, **When** LightGrid culling runs, **Then** cell storage uses a global capacity model instead of a fixed per-cell limit.

---

### User Story 3 - Preserve Existing Toggle And Debug Workflow (Priority: P2)

An editor user can keep using the Project Settings LightGrid toggle and existing profiling workflow while the underlying LightGrid implementation is aligned to UE5.

**Why this priority**: The previous performance-toggle work must remain useful and must not regress the ability to disable LightGrid when profiling or debugging.

**Independent Test**: Can be tested with existing editor settings and renderer tests plus a LightGrid disabled-path regression.

**Acceptance Scenarios**:

1. **Given** LightGrid is disabled from Project Settings, **When** a scene renders, **Then** no UE5-aligned LightGrid preparation or compute dispatch is performed.
2. **Given** LightGrid is re-enabled, **When** a scene renders, **Then** the UE5-aligned grid is rebuilt on the next applicable frame.
3. **Given** editor helper-only scene frames, **When** the scene contains no scene draw work, **Then** helper draws do not receive LightGrid pass bindings.

### Edge Cases

- Very small render targets must still produce at least one XY grid cell.
- Zero-width or zero-height frame descriptors must not divide by zero.
- Cameras with invalid near/far ranges must fall back to a safe LightGrid range or skip preparation with diagnostics enabled.
- Empty light lists must still provide valid buffers/bindings for shaders that expect the LightGrid pass layout.
- Scenes exceeding fixed per-cell capacity must have explicit clamp or linked-list behavior; silent out-of-bounds writes are not allowed.
- Backend support claims must be limited to validated backends, with DX12 treated as the first verification target.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST derive LightGrid XY dimensions from a configurable pixel cell size, defaulting to the UE5 value of 64 pixels per cell.
- **FR-002**: The system MUST default LightGrid Z slices to the UE5 value of 32.
- **FR-003**: The system MUST default fixed per-cell culled light capacity to the UE5 value of 32.
- **FR-004**: The system MUST expose a linked-list culling mode flag matching UE5's default-enabled model conceptually, while preserving a fixed-capacity fallback.
- **FR-005**: The system MUST use UE's logarithmic Z slice model, where the shader computes `log2(SceneDepth * B + O) * S` from per-frame LightGrid Z parameters.
- **FR-006**: The system MUST compact or otherwise publish per-cell light references into graphics-readable buffers before scene lighting passes consume them.
- **FR-007**: The system MUST preserve the project-level LightGrid enable setting and skip all LightGrid preparation when disabled.
- **FR-008**: The system MUST keep forward and deferred scene render paths consuming the same LightGrid prepared state.
- **FR-009**: The system MUST include automated tests that lock the UE5-aligned defaults, render-size-derived grid dimensions, disabled-path behavior, and overflow behavior.
- **FR-010**: The system MUST dispatch LightGrid injection over grid cells with a 4x4x4 threadgroup shape, matching the local UE reference.
- **FR-011**: The system MUST document any remaining differences from the supplied UE reference source if Nullus lacks the dependent renderer feature, such as reflection captures or shadow-channel packing.

### Key Entities

- **UE5-Aligned LightGrid Settings**: Runtime settings for pixel cell size, Z slice count, fixed per-cell capacity, and linked-list culling mode.
- **LightGrid Frame Constants**: Per-frame shader constants containing camera transforms, render size, grid dimensions, culling capacity, and lighting counts.
- **LightGrid Culling Buffers**: GPU buffers storing packed lights, per-cell counters or heads, scratch/linked-list nodes, compact records, and compact light indices.
- **LightGrid Graphics Binding Set**: Graphics-pass binding set consumed by forward and deferred lighting shaders.

## Success Criteria

### Measurable Outcomes

- **SC-001**: For default settings, automated tests verify LightGrid cell size 64, Z slices 32, and fixed capacity 32.
- **SC-002**: For at least three render sizes, automated tests verify XY grid dimensions are render-size-derived and never fixed to 16x9.
- **SC-003**: With LightGrid disabled, automated tests verify no LightGrid compute source is produced and scene package creation remains valid.
- **SC-004**: Automated tests verify UE-style logarithmic Z parameter calculation against the local UE reference formula.
- **SC-005**: With LightGrid enabled, RenderDoc or equivalent renderer evidence on DX12 shows a LightGrid culling/compaction sequence before scene lighting consumes the grid.
- **SC-006**: Any intentional non-parity with the local UE reference is listed in `research.md` with reason and follow-up path.

## Assumptions

- The supplied UE 4.27 source tree is treated as the licensed local reference for LightGrid internals; UE5 wording from the user is interpreted as aligning with the same Forward LightGrid architecture unless a UE5 source path is supplied later.
- Implementation should copy algorithms and contracts conceptually, but Nullus code and shaders should remain original and adapted to Nullus RHI/material data.
- DX12 is the first validation backend for this phase; other backends require their own evidence before being called aligned.
- Reflection capture culling parity is a follow-up unless Nullus already has reflection capture entities wired into runtime lighting descriptors.
