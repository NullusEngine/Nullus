# Research: Editor HitProxy Picking

## Decision: Follow UE's split between hit-proxy picking and selection outline

**Decision**: Nullus should model the editor path after UE4.27's separation: hit-proxy data is for picking, while selection outline/highlight is a separate rendering path.

**Rationale**: UE keeps `FViewport::GetHitProxy()` and hit-proxy map invalidation in the viewport/editor picking path, while selection visuals are controlled through renderer flags and custom depth/stencil style paths such as `SelectionOutline` and `bRenderCustomDepth`. This split prevents visual selection changes from implying a picking-buffer rebuild.

**Alternatives considered**:

- Use one picking buffer for both selection and highlight. Rejected because it couples a CPU readback path to visual state and increases rebuild pressure.
- Keep current ad hoc hover budget only. Rejected because it reduces some cost but does not make click/hover/readback state explicit or reliably cacheable.

## Decision: Rebuild picking only on explicit invalidation

**Decision**: Introduce a picking cache signature built from render extent, camera state, pickable draw-source identity, and scene/pickable revision.

**Rationale**: UE viewports invalidate hit proxies through dedicated invalidation paths rather than treating every frame as needing a fresh hit-proxy map. Nullus traces show repeated capture of pickable model sources can dominate frame time; cache compatibility is the missing control point.

**Alternatives considered**:

- Time-based throttle only. Rejected because it can still select from stale data after camera movement.
- Always render picking on click only. Rejected because hover highlight would become unresponsive in normal small scenes.

## Decision: Preserve async readback lifecycle, but make request freshness explicit

**Decision**: Click requests record a minimum acceptable picking frame serial and resolve only against a readable frame satisfying that serial and signature.

**Rationale**: Nullus already has `PickingReadbackLifecycle` and previous fixes around async readback. The unstable behavior appears less about having async readback and more about unclear request freshness and frame validity.

**Alternatives considered**:

- Force synchronous readback on click. Rejected because it risks GPU stalls and repeats the earlier performance problem in another form.
- Decode from any readable frame. Rejected because it can select stale geometry after camera movement or resize.

## Decision: Add diagnostics before deeper renderer rewrites

**Decision**: Add scoped profiler states and FrameInfo-friendly counters for rebuild, reuse, hover skip, pending readback, and click resolution.

**Rationale**: The performance issue has moved across traces as different mitigations landed. Clear diagnostics are required to avoid guessing whether picking, streaming, occlusion, or idle policy is currently responsible.

**Alternatives considered**:

- Only rely on RenderDoc. Rejected because RenderDoc captures are useful for rendering correctness but too slow for every editor interaction regression.
