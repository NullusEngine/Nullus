# Helper-Only Prepared Frame Fix

## Problem

When a prefab drag is waiting for its real preview root, `DebugSceneRenderer` emits a lightweight
helper-only drag proxy. The prepared frame contains editor helper metadata and recorded helper draw
commands, but no deferred scene geometry.

The current no-scene fast path compiles the appended metadata and prepared pass inputs directly.
It does not build the generic helper pass input needed for snapshot-owned helper draw commands.
Frame-graph validation therefore throws while resolving the prepared builder. The lifecycle catches
that exception and invokes the same fallback used for a genuinely missing builder, producing an
empty package and logging the misleading warning once per frame.

## Design

Keep the no-scene fast path only for frames that have no deferred scene execution and no helper or
appended render work. A frame with helper draw commands or appended pass metadata/inputs must use
the prepared deferred compilation path, because that path constructs and validates the complete
pass-input set, including the generic helper pass.

The fix stays within the existing prepared-builder boundary. It does not change slot ownership,
worker scheduling, render-thread synchronization, or backend-specific RHI behavior.

## Error Handling

Builder invocation failures and missing builders are separate conditions. A thrown
`std::exception` should retain its diagnostic instead of being described as a missing builder.
The fallback empty package remains available so the render worker can retire the frame safely.
Unknown exceptions retain a distinct generic diagnostic.

## Tests

Add a focused regression test that builds a threaded prepared frame containing helper-only work and
asserts that builder resolution succeeds with the helper pass and draw command preserved. Run the
test against the current implementation first and confirm it fails because frame-graph compilation
rejects the incomplete pass inputs.

Keep the existing empty-scene coverage to prove the fast path remains valid. Run the relevant
threaded rendering and editor debug-renderer tests after the fix.

## Acceptance

- Prefab drag proxy frames no longer resolve to `PreparedBuilderMissing`.
- Helper-only recorded draws remain present in the resolved render-scene package.
- Truly empty frames keep using the lightweight no-scene path.
- Builder exceptions are not mislabeled as missing builders.
- Focused unit tests pass without repeated warnings.
