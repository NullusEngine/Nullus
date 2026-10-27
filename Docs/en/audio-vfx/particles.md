# Particle Authoring Workflow

Particle Editor authors systems as ordered module stacks. The runtime supports CPU/GPU emitters and Sprite/Mesh rendering. Editing, compilation, importing, and playback exist; the full module library, production tools, and platform device acceptance remain incomplete.

## Create and preview

1. Copy `Templates/VFX/Examples/Starter.vfx` or `GpuStarter.vfx` into project Assets, open **Window > Particle Editor**, and open that file. Asset Browser double-click, Edit, and component System references use the same editor.
2. Select an emitter in System Overview and edit module-stack inputs. Put **Gravity Force** before **Solve Forces and Velocity** to affect motion. Disabled modules do not execute.
3. Define User parameters and bind module inputs. Curve/gradient inputs are edited in Curves; Dynamic Input opens an expression graph matching its parent input type.
4. Inspect CPU/GPU emitters in the whole-system preview, then play, pause, or restart. Invalid scripts report diagnostics and retain the previous successfully compiled preview.
5. Save under project Assets to import the compiled program. Assign the asset to a Particle System component's System field and enter Play mode.

Input edits participate in Undo/Redo. Working changes, successful preview, and successful import are separate states. Failed importing retains the previous published artifact.

## Assets and command line

Reusable versioned scripts and emitters support function calls, static switches, and inherited input/settings overrides. Modules live under `App/Assets/Engine/VFX/Modules`; examples live under `Templates/VFX/Examples`. Preserve dependency `.meta` files when copying assets.

```text
NullusVfxCompile new MySystem.vfx "My System"
NullusVfxCompile compile MySystem.vfx MySystem.compiled.json --backend all
```

`--backend all` produces compiled backend packages; it does not establish Linux, Vulkan, or Metal device execution. The Player consumes compiled programs without editor stack layouts. Old graph-format samples remain runtime/import fixtures. The current editor reports unsupported formats, does not migrate them automatically, and refuses to overwrite legacy files with v2.

## Scene playback and queries

Particle System components expose activation, stop, restart, pause, and typed parameter controls. Scene playback runs the system's emitters and binds the owning scene's query world for particle collision queries. [Static colliders](../physics/roadmap.md) provide scene geometry.

Sprite and Mesh paths provide visibility, instancing, bounds, and transparent ordering. GPU bounds update through asynchronous readback and use a conservative fallback until the first result. Delayed debug statistics are not the current image's results. Separate Mesh slot draws do not preserve one global order for every transparent particle.

## Acceptance and limits

Check module ordering, User inputs, curves, Undo/Redo, error recovery, multi-emitter preview, save/reopen, scene playback, and a cooked standalone Player separately. Record GPU compilation, focused device tests, Editor interaction, and packaged rendering as distinct results.

Full Niagara parity, complete data interfaces, additional renderers, and asset upgrade tooling remain incomplete. See [Audio and VFX Status and Plans](roadmap.md) for audio, video, and post-processing.
