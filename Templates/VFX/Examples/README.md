# Particle Editor templates

Use **Window > Particle Editor** and Open to edit `Starter.vfx` or
`GpuStarter.vfx`. They use the ordered module-stack format. This is the only
particle editor: Window, asset creation, right-click Edit, double-click and
component System references use the same window. Legacy graph-format files report a format diagnostic when opened; no fallback
editor or automatic conversion exists.

The candidate workspace includes System Overview, an editable module stack,
dynamic input graphs, curves/gradients, parameters, timeline, diagnostics and
whole-system CPU/GPU preview. Try adding a second emitter, changing Initialize
Particle's color or linking it to a User parameter. Add Gravity Force above
Solve Forces and Velocity to affect particle motion. Disabled modules do not
execute. Input changes participate in undo/redo; invalid scripts retain the
previous successfully compiled preview.

For curve input, choose **Curve / Gradient**, select that module and open the
Curves tab. Linear and cubic interpolation, color keys and explicit arrive/leave
tangents compile to the same CPU/GPU expression program. Dynamic Input opens a
separate expression graph whose Output type matches its parent input.

The candidate now supports standalone versioned script and emitter assets,
function calls, static switches and inherited input/settings overrides. The
`Library/` example demonstrates this workflow. The full module library, complete
inheritance upgrade UI, data interfaces, additional renderers, and production
tools remain incomplete. See the public [Particle Authoring Workflow](../../../Docs/en/audio-vfx/particles.md) for current scope.
No automatic migration is performed, and saving v2 over a legacy file is refused.

Saving a candidate under the current project's Assets directory imports its
compiled runtime program through the normal asset database. The system can then
be referenced by the existing ParticleSystemComponent. An import failure retains
the prior published artifact. Open the source through Particle Editor or the
asset browser; both use the same editor.

Command-line creation and compilation:

```text
NullusVfxCompile new MySystem.vfx "My System"
NullusVfxCompile compile MySystem.vfx MySystem.compiled.json --backend all
```

`--backend all` produces precompiled backend packages; it does not prove Linux
or Metal device execution. The compiled package is consumed by the existing
runtime; editor module stacks and layout are not required for playback.
