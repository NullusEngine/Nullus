# Dynamic stage controls

Copy the whole directory into a project's Assets directory and open
`DynamicSystem.vfx` in **Window > Particle Editor**.

The inherited GPU emitter pins a module and nested iteration function.
Its first Simulation Stage binds **Enabled Binding** to `User.StageEnabled`
and **Num Iterations Binding** to `User.StageIterations`. The System defaults
enable that stage for three iterations. The final offset stage still executes
when the first stage is disabled, so the two contributions can be compared.

In **User Parameters**, toggle `User.StageEnabled` or change the integer control
for `User.StageIterations`. These are preview overrides and do not edit the
asset or compile a new program. **Parameters** edits the saved defaults and
triggers compilation. Set iterations to zero to skip the stage; negative runtime
values also skip it. Each simulation frame freezes its controls, including when
parameters change before GPU submission finishes. Reset keeps runtime overrides.

Choose **None** in either binding picker to restore its local value. Inherited
binding changes are separate source-ID overrides and can be undone or restored
with **Reset Stage to Parent**. The library editor accepts User parameter names
that the consuming System must declare.

The candidate still stores controls in scalar Float parameters. Enabled values
must be 0 or 1; iteration values must be integral. Full Niagara Bool/Int parameter
types remain part of the acceptance matrix. The current runtime supports 64
stages, 128 iterations per stage and 1024 active iterations per substep. Invalid
values or a batch exceeding that budget leave the previous parameters intact.

System sources and compiled programs using dynamic bindings or zero literal
iterations use format 3; existing sources/programs without these features can
retain format 2. Shader layout stays
10. Old runtime readers reject format 3 rather than silently ignoring controls.
The current reader also accepts existing format 1/2 programs.

```text
NullusVfxCompile compile Templates/VFX/Examples/DynamicStages/DynamicSystem.vfx build/DynamicSystem.compiled.json --backend all
```

The cooked program retains control defaults and bindings after graphs, HLSL and
all library sources are removed. Data-interface iteration, execution filters,
particle state selection and custom dispatch layouts remain pending.
