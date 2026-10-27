# Simulation Stage example

Copy this directory into a project's Assets directory. Open `IterativeSystem.vfx`
in **Window > Particle Editor**. Open the emitter and script sources in
**Window > Particle Library Editor**.

The system pins a version of `IterativeEmitter.vfxemitter`. Its first particle
stage repeats an offset module; a disabled stage preserves its place in the
stage sequence; the final stage applies another offset. The system overrides
the first stage's iteration count from two to three without changing its parent.

`StageOffset.vfxscript` reads and writes `Particles.Position`, takes `User.Offset`
as a module input and calls `IterationWeight.vfxscript`. The function returns
`StageIndex + StageIteration + 1`; both indices start at zero. Changing the user
parameter, order or iteration count changes the resulting motion.

Use the stage header context menu to duplicate, move or delete a local stage.
Local stages can also be reordered by dragging their headers. The stage controls
edit its name, enabled state and iteration count. Inherited controls write
separate overrides; **Reset Stage to Parent** removes those overrides. Each edit
uses the document's undo history.

This candidate supports GPU stages that iterate live particles after ordinary
spawn/update. Each stage keeps an independent parameter map; its modules read
earlier module writes, and each iteration reads the prior iteration's result.
Stage execution does not automatically advance particle age. CPU emitters reject
these stages with a compile diagnostic. Dynamic User scalar control bindings are
demonstrated in the neighboring DynamicStages example. Data-interface iteration,
execution filters and custom dispatch layouts remain pending in the UE parity
matrix.

Compile the example for the existing runtime:

```text
NullusVfxCompile compile Templates/VFX/Examples/SimulationStages/IterativeSystem.vfx build/IterativeSystem.compiled.json --backend all
```

The compiled package contains the lowered stages and backend bytecode and can
omit all authoring graphs, HLSL and library source assets. Backend compilation
alone does not confirm Linux or Metal device behavior.
