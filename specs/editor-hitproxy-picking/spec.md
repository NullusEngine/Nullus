# Feature Specification: Editor HitProxy Picking

**Feature Branch**: `editor-hitproxy-picking`  
**Created**: 2026-06-10  
**Status**: Draft  
**Input**: User approved continuing the UE-style redesign after asking "UE是怎么做的" while investigating camera-move frame drops and picking/selection instability.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Cached Picking (Priority: P1)

As an editor user navigating a large scene, I want hover and click selection to use a cached picking result that is rebuilt only when the view or pickable scene state actually changes, so camera movement and idle hover do not repeatedly rebuild every pickable draw command.

**Why this priority**: Trace evidence shows `PickingRenderPass::CapturePickableModelSources` can spike badly and dominate camera movement frames. Without fixing this, other large-scene optimizations are masked by editor picking overhead.

**Independent Test**: Can be tested by moving the Scene View camera over a large imported prefab scene and confirming picking frame rebuild count stays bounded while click selection still resolves the correct object.

**Acceptance Scenarios**:

1. **Given** the mouse is hovering over the Scene View and neither the camera nor pickable scene state changed, **When** the editor renders the next frame, **Then** the picking pass reuses the latest compatible hit-proxy frame instead of recapturing all pickable draw commands.
2. **Given** the user clicks inside the Scene View after the camera moved, **When** a fresh hit-proxy frame becomes readable, **Then** the click resolves against that fresh frame and does not select from stale camera data.
3. **Given** the user is actively controlling the camera, **When** hover picking would exceed the configured large-scene budget, **Then** hover picking is skipped without blocking click picking.

---

### User Story 2 - Selection Highlight Is Separate From Picking (Priority: P1)

As an editor user selecting objects, I want selected-object outline/highlight to update independently from the picking buffer, so selection visuals do not force a heavy picking rebuild and picking changes do not regress highlight stability.

**Why this priority**: UE keeps hit-proxy selection and selection outline as separate renderer paths. Nullus already has a selection outline mask renderer; this feature must preserve that separation as an explicit contract.

**Independent Test**: Can be tested by changing selection without moving the camera and confirming the selection outline updates while picking cache invalidation is not forced unless the pickable scene itself changed.

**Acceptance Scenarios**:

1. **Given** a GameObject is selected from the hierarchy, **When** the Scene View redraws, **Then** the selection outline updates through the selection outline path without requesting a new picking frame solely because selection changed.
2. **Given** the picking buffer is unavailable or still pending, **When** a selection already exists, **Then** selection outline rendering remains driven by current selection data and does not disappear because picking readback is delayed.

---

### User Story 3 - Readable Diagnostics For Picking Cost (Priority: P2)

As a developer profiling editor performance, I want FrameInfo and profiler scopes to show whether picking used cache, rebuilt, skipped hover budget, or waited on readback, so I can explain frame drops without digging through RenderDoc every time.

**Why this priority**: The current symptoms are hard to reason about because "picking did work" and "picking used a stale/pending frame" are not visible enough in traces or FrameInfo.

**Independent Test**: Can be tested with trace captures and the existing FrameInfo panel by verifying each picking state appears with clear counters and without obsolete or ambiguous values.

**Acceptance Scenarios**:

1. **Given** picking reused an existing hit-proxy frame, **When** the profiler trace is opened, **Then** the trace contains a clear reuse scope or counter and no expensive capture scope for that frame.
2. **Given** hover picking is skipped due to large-scene budget, **When** FrameInfo is visible, **Then** it reports hover picking skipped separately from click picking failures.
3. **Given** click picking waits for an async readback frame, **When** diagnostics are viewed, **Then** the pending/readable frame serials make the wait state understandable without slash-separated ambiguous values.

### Edge Cases

- The mouse leaves the Scene View while a click-pick readback is pending; the pending click must be cancelled or resolved only if the original view/frame contract is still valid.
- The Scene View render size changes; cached hit-proxy textures and readable metadata must be invalidated because coordinates no longer match.
- The scene graph changes, an object is destroyed, or a prefab stream changes residency; cached hit-proxy registries must not return dangling GameObject pointers.
- Multiple rendered views are visible; picking diagnostics must identify or aggregate visible view state without relying on focus.
- GPU readback is unsupported or delayed; click picking must degrade gracefully without logging noisy repeated warnings.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST maintain a hit-proxy-style picking frame cache keyed by render size, camera/view state, pickable scene revision, and pickable draw-source identity.
- **FR-002**: Hover picking MUST reuse a compatible readable picking frame and MUST NOT rebuild the picking pass every frame while the view and pickable scene are unchanged.
- **FR-003**: Click picking MUST be able to force or wait for a fresh picking frame when the cached frame is older than the click request.
- **FR-004**: Hover picking MAY be skipped when visible pickable draw count exceeds the large-scene hover budget, but click picking MUST remain eligible even when hover picking is skipped.
- **FR-005**: Selection outline/highlight MUST remain driven by selection state and the selection outline renderer, not by picking readback state.
- **FR-006**: Selection state changes MUST NOT invalidate the picking cache unless they also change pickable geometry, transforms, visibility, or render size.
- **FR-007**: Cached pick registries MUST validate object liveness or scene revision before returning a GameObject from a readable frame.
- **FR-008**: The system MUST expose diagnostics for picking rebuild, reuse, skipped-hover-budget, pending-readback, readable serial, and click-resolution states.
- **FR-009**: Diagnostics displayed in FrameInfo MUST use table-like, clearly separated fields and MUST avoid ambiguous slash-separated multi-values.
- **FR-010**: Existing gizmo suppression, text-input blocking, resizing checks, and async readback lifecycle rules MUST be preserved.

### Key Entities

- **HitProxy Picking Frame**: A rendered picking target plus metadata needed to decode object IDs for a specific Scene View render size, camera state, and pickable scene revision.
- **Picking Cache Signature**: The stable identity used to decide whether a readable picking frame can be reused.
- **Pick Registry Entry**: The mapping between a color-encoded picking ID and a live GameObject identity.
- **Picking Request**: A hover or click request that records input position, minimum acceptable frame serial, and whether stale data is allowed.
- **Selection Visual State**: The selected-object data consumed by the outline/highlight renderer, independent from picking readback.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In a large imported prefab scene with the camera stationary and mouse stationary over the Scene View, hover picking rebuilds at most once per compatible cache interval and does not rebuild every rendered frame.
- **SC-002**: During continuous camera movement, hover picking contributes no more than a small bounded number of heavy capture frames per second when the visible pickable draw count exceeds the configured hover budget.
- **SC-003**: Click selection still resolves the expected object after camera movement once a fresh readable frame is available.
- **SC-004**: Selection outline remains visible and stable for selected objects even when picking readback is pending or hover picking is skipped.
- **SC-005**: Trace and FrameInfo evidence can distinguish cache reuse, rebuild, hover skip, and pending readback without requiring source-code inspection.

## Assumptions

- The first implementation targets the existing DX12 editor path because current evidence and RenderDoc captures are DX12-focused.
- Existing selection outline mask rendering remains the selection/highlight path; this feature should not replace it with picking-buffer based highlighting.
- The large-scene hover budget remains configurable in code initially; adding a user-facing setting can be a follow-up if needed.
- Object identity validation can initially use existing scene/object lifetime signals and scene mutation revisions rather than requiring a new global ECS identity system.
