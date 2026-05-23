# Contract: Deferred Editor Overlays

## Runtime Deferred Extension Contract

Runtime framegraph helpers MUST allow callers to build a deferred prepared scene package and append generic pass inputs after deferred lighting.

Required behavior:
- Preserve deferred GBuffer as the first scene pass.
- Preserve deferred Lighting as the second scene pass and the external output attachment target.
- Append extra pass inputs in caller-provided order.
- Do not reference editor-only classes in Runtime headers or source.

## Editor Debug Renderer Contract

`DebugSceneRenderer` MUST:
- Derive from `DeferredSceneRenderer`.
- Register existing Scene View editor passes.
- Count visible helper passes in the frame snapshot.
- Build threaded prepared packages that include deferred passes plus editor helper/picking passes.
- Register picking preferred readback texture only when picking pass input exists.

## Compatibility Contract

`ForwardSceneRenderer` MUST remain directly constructible and its existing tests MUST continue to pass.
