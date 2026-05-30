# Research: Scene View Camera State Machine

## Decision 1: Use an explicit editor-side interaction state machine instead of per-frame cursor branches

**Decision**: Model Scene View camera interaction as a finite set of named states with explicit transitions: neutral, blocked, fly, pan, and orbit.

**Rationale**:

- The current bug class comes from state being implicit and scattered across booleans such as `m_middleMousePressed`, `m_rightMousePressed`, `m_inputActive`, `m_inputBlocked`, and scene-view-side blocking conditions.
- Cursor ownership, infinite wrap, mouse delta suppression, and input cleanup are side effects that should happen on state transitions, not every frame.
- A named-state model makes it possible to test state entry, state exit, and forced resets directly without requiring full panel runtime.

**Alternatives considered**:

- Keep the existing boolean model and only memoize the last cursor: rejected because it reduces cursor churn but leaves hidden coupling and reset rules scattered.
- Move all Scene View/editor interaction into a larger panel-level orchestrator: rejected for this iteration because it expands scope beyond the reported regression and slows delivery.

## Decision 2: Keep Scene View as the source of high-level block conditions

**Decision**: `SceneView` continues to compute editor-wide block conditions such as shortcut modal state, text-entry blocking, and whether pointer context should enable camera interaction, then feeds those conditions into the state machine.

**Rationale**:

- `SceneView` already owns panel focus, hover bounds, gizmo interplay, and editor-only block rules.
- Camera interaction should not need to rediscover panel/UI conditions from lower layers.
- This preserves a clean boundary: `SceneView` decides whether camera input is allowed, and the state machine decides how camera interaction behaves when allowed.

**Alternatives considered**:

- Move all blocking rules into `CameraController`: rejected because it would make panel/UI coordination harder to reason about and harder to reuse in tests.
- Query ImGui/UI global state directly from each interaction state: rejected because it spreads dependency on global UI state across multiple paths again.

## Decision 3: Separate transition logic from camera movement math

**Decision**: Introduce a dedicated helper that computes state transitions, cursor ownership changes, and capture/reset actions, while `CameraController` keeps camera motion math such as fly movement, pan delta, orbit delta, zoom, and focus updates.

**Rationale**:

- The existing `CameraController` already contains valuable camera math that should not be rewritten during the cursor/state refactor.
- The risky part is not the math; it is lifecycle control around input, capture, and cursor ownership.
- Keeping the motion math intact reduces regression risk and lets tests focus on the new control-flow model independently.

**Alternatives considered**:

- Rewrite `CameraController` wholesale into a state-pattern hierarchy: rejected for the first pass because it combines architecture cleanup with a larger behavior migration.
- Leave transition logic inside `CameraController.cpp` as a large switch: acceptable but less testable and likely to regrow coupling; the helper provides a firmer seam.

## Decision 4: Treat text input as a hard blocking state, not a soft preference

**Decision**: Active text entry transitions the interaction model into a blocked state that forces camera navigation cleanup and releases camera-owned cursor state immediately.

**Rationale**:

- The reported issue happens specifically because text-entry state was not treated as a definitive camera-blocking condition.
- A hard blocked state gives deterministic behavior when text input starts during or near an existing camera interaction.
- The same rule also covers future block cases such as modal shortcut settings, panel-wide UI capture, or additional editor overlays.

**Alternatives considered**:

- Only suppress cursor changes while leaving existing camera mode active: rejected because hidden active camera state can still mutate movement and reclaim control unexpectedly.
- Only block text input outside Scene View bounds: rejected because the reproduced bug occurs when text entry is within Scene View interaction territory.

## Decision 5: Add pure transition tests plus panel integration tests

**Decision**: Validate the feature with a new pure state-machine unit test file and existing panel-level policy tests, plus Debug build integration compilation.

**Rationale**:

- Pure tests can cover state entry/exit, cursor ownership rules, and reset semantics without requiring full editor startup.
- Existing Scene View policy tests already cover helper-style interaction gating and are a good home for block-condition regressions.
- Build validation ensures the new helper integrates cleanly with current editor/runtime targets.

**Alternatives considered**:

- Manual-only validation: rejected because this regression class is easy to reintroduce.
- Full end-to-end UI automation: rejected for this iteration because there is no existing stable harness for fine-grained cursor-state automation in the editor.
