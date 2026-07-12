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

Keep the no-scene fast path for frames without deferred scene execution. Before compiling its
metadata, materialize the generic aggregate helper pass input from the snapshot-owned recorded draw
commands and append it beside any explicitly prepared helper inputs. Reuse the deferred frame-graph
helper slicing logic as the single source of truth, but do not force helper-only frames through the
GBuffer-dependent prepared deferred path.

The fix stays within the existing prepared-builder boundary. It does not change slot ownership,
worker scheduling, render-thread synchronization, or backend-specific RHI behavior.

## Error Handling

Builder invocation failures and missing builders are separate conditions. A thrown
`std::exception` should retain its diagnostic instead of being described as a missing builder.
The fallback empty package remains available so the render worker can retire the frame safely.
Unknown exceptions retain a distinct generic diagnostic.

## Tests

Add a focused regression test that builds the aggregate input for a helper-only package and asserts
that its helper pass and recorded draw command are preserved. Keep a routing contract test proving
the no-scene builder calls the shared helper-input constructor. Run the tests against the current
implementation first and confirm they fail because that constructor is not exposed or called.

Keep the existing empty-scene coverage to prove the fast path remains valid. Run the relevant
threaded rendering and editor debug-renderer tests after the fix.

## Acceptance

- Prefab drag proxy frames no longer resolve to `PreparedBuilderMissing`.
- Helper-only recorded draws remain present in the resolved render-scene package.
- Truly empty frames keep using the lightweight no-scene path.
- Builder exceptions are not mislabeled as missing builders.
- Focused unit tests pass without repeated warnings.
