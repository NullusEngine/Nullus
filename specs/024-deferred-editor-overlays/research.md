# Research: Deferred Editor Overlays

## Current State

- `DeferredSceneRenderer` builds deferred GBuffer and Lighting passes through `SceneRenderGraphBuilderDeferred`.
- `DebugSceneRenderer` currently inherits `ForwardSceneRenderer` and builds forward pass metadata plus editor helper/picking passes.
- Scene View constructs `DebugSceneRenderer` directly, so it remains forward even after the default scene renderer changed to deferred.
- Existing editor helper passes already expose prepared pass inputs for threaded rendering.

## Decisions

### Use Deferred Inheritance For Scene View Debug Renderer

`DebugSceneRenderer` will derive from `DeferredSceneRenderer`.

This keeps Scene View's main scene path aligned with GameView/runtime and lets editor overlay logic remain in the editor module.

### Add Runtime-Level Deferred Extension Function

Runtime framegraph code will provide a helper that:

1. Compiles and applies the deferred light-grid scene execution.
2. Builds normal deferred pass inputs.
3. Appends caller-provided pass inputs after deferred lighting.

The helper accepts generic metadata and pass input collections, not editor-specific types.

### Keep Picking Readback Editor-Owned

`DebugSceneRenderer` will keep the picking pass and readback registration logic because those concepts are editor-only.

## Risks

- Deferred lighting pass must remain the output pass that receives external output attachments.
- Helper passes must not accidentally replace deferred GBuffer recorded draw commands.
- Picking pass ordering must remain after visual scene output.

## Validation Strategy

- Add contract tests for default/debug renderer inheritance behavior.
- Add package tests verifying pass order: GBuffer, Lighting, editor overlay/debug, optional picking.
- Run focused renderer tests and full `NullusUnitTests`.
- Use RenderDoc later if visual editor output still differs despite correct pass packages.
