# Research: Render Feature Refactor

## Decision 1: Use staged migration instead of deleting `ARenderFeature` first

**Decision**: Migrate `ARenderFeature` responsibilities in slices and keep a temporary compatibility layer until each responsibility has a replacement owner.

**Rationale**:

- `EngineBufferRenderFeature` currently supplies frame/object data needed by normal scene draws.
- `DebugShapeRenderFeature` currently combines debug draw submission and rendering behavior.
- `FrameInfoRenderFeature` and `LightingRenderFeature` still provide live runtime data during frames.
- Editor renderer code (`DebugSceneRenderer`, `GridRenderPass`, `OutlineRenderFeature`, `GizmoRenderFeature`) still looks up features directly, so removing the base abstraction first would break editor runtime preservation.

**Alternatives considered**:

- **Delete `ARenderFeature` immediately**: Rejected because it would break frame/object binding, debug draw, and editor feature lookups in one step.
- **Leave `ARenderFeature` as-is and only document new rules**: Rejected because it does not remove the hidden draw-time ownership problem.

## Decision 2: Move frame/object binding into renderer-owned draw preparation

**Decision**: Renderer-owned state becomes the authoritative owner of frame-level and object-level binding data. Draw preparation binds core frame/object resources before material resources.

**Rationale**:

- Core scene rendering must not depend on optional extensions.
- The current `EngineBufferRenderFeature` uses draw hooks to update and bind mandatory frame/object data.
- Renderer-owned binding aligns with the existing `CompositeRenderer` / `ABaseRenderer` draw orchestration and keeps required resources available regardless of optional extension registration.

**Alternatives considered**:

- **Keep frame/object binding in a required feature that is always registered**: Rejected because “required optional feature” preserves the architectural ambiguity.
- **Push frame/object data into materials**: Rejected because these are renderer-owned update-frequency layers, not material-owned data.

## Decision 3: Split debug draw into submission ownership and render-stage ownership

**Decision**: Preserve the debug draw queue and submission API, but migrate rendering into an explicit debug drawing stage or pass instead of a broad feature hook.

**Rationale**:

- Existing debug draw tests and editor workflows depend on queue semantics such as category visibility, frame limits, lifetime rules, and transient shape submission.
- Explicit ownership makes debug drawing easier to schedule, validate, and reason about than hidden draw-time hooks.
- This preserves current `DebugShapeRenderFeature` behavior while enabling a later split into queue/system plus render-stage responsibilities.

**Alternatives considered**:

- **Keep debug draw permanently inside `DebugShapeRenderFeature`**: Rejected because it keeps rendering ownership coupled to the feature hook model.
- **Rewrite debug draw from scratch in one pass**: Rejected because current semantics and in-flight debug primitive work should be preserved.

## Decision 4: Treat lighting as renderer/scene data, not a draw hook

**Decision**: Lighting information becomes scene or renderer data consumed by forward and deferred passes, while any compatibility feature remains transitional only.

**Rationale**:

- `LightingRenderFeature` already acts more like a provider than a rendering step.
- Forward and deferred rendering should share the same lighting source.
- Renderer/scene-owned lighting data is consistent with the spec requirement that passes not depend on hidden feature hook side effects.

**Alternatives considered**:

- **Keep lighting in `LightingRenderFeature` indefinitely**: Rejected because it hides lighting ownership behind lifecycle hooks.
- **Move lighting directly into every pass independently**: Rejected because it duplicates scene collection logic and risks divergence.

## Decision 5: Move frame statistics into renderer-owned diagnostics

**Decision**: Frame statistics become renderer-owned diagnostics updated during draw submission rather than through `FrameInfoRenderFeature` event hooks.

**Rationale**:

- Statistics should represent renderer behavior even if optional feature registration changes.
- Current `FrameInfoRenderFeature` depends on event listeners and feature registration.
- Renderer-owned stats align with `CompositeRenderer::DrawEntity()` and prepared draw submission flow.

**Alternatives considered**:

- **Leave stats in `FrameInfoRenderFeature`**: Rejected because statistics remain optional-feature dependent.
- **Drop stats from the refactor scope**: Rejected because the spec explicitly preserves stats availability.

## Decision 6: Keep editor utility features in compatibility scope for this refactor

**Decision**: Editor-side utility features such as `DebugModelRenderFeature`, `GizmoRenderFeature`, and `OutlineRenderFeature` remain compatibility consumers in this feature unless a slice directly requires changing them.

**Rationale**:

- They currently function more like editor drawing utilities than core renderer ownership providers.
- The highest-risk migration targets are frame/object binding, debug draw ownership, lighting data ownership, and stats ownership.
- Preserving runtime/editor operability requires limiting the blast radius of the first architectural pass.

**Alternatives considered**:

- **Refactor all editor features in the same feature**: Rejected because it expands scope well beyond the core ownership change.
- **Ignore editor consumers entirely**: Rejected because editor runtime preservation is a constitutional gate.

## Decision 7: Validation combines unit coverage with product smoke checks

**Decision**: Validation for this refactor uses targeted `NullusUnitTests`, editor/game smoke runs, and RenderDoc only when visual correctness claims are made for supported backends.

**Rationale**:

- The constitution requires subsystem-appropriate evidence.
- Much of the migration is ownership and orchestration behavior that is best caught by focused unit tests.
- Product runtime preservation still requires editor/game smoke validation after each slice.

**Alternatives considered**:

- **Unit tests only**: Rejected because renderer ownership changes can still break product startup or frame execution.
- **Manual smoke only**: Rejected because ownership regressions need targeted, repeatable checks.
