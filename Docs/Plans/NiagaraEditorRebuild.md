# Niagara editor rebuild

Reference: installed UE 5.7.4 CL 51494982. Coordinates and world units remain
native to Nullus. NiagaraFluids, NiagaraNanite and NiagaraMRQ are separate future
extensions. The module-stack editor is the single default by explicit user request (2026-10-18); the legacy editor implementation has been removed. Niagara parity remains unaccepted.

Window, new-asset creation, Edit/double-click and the component System reference
open action all use **Particle Editor**. CLI creation/compilation uses `new` and
`compile`; the transitional command aliases were removed. Legacy source files are
preserved and rejected for editing with a diagnostic. Existing runtime package
and import compatibility is independent from the removed editor implementation.
The cutover passed 117 relevant tests (54 runtime, 30 behavior, 16 editor/device,
17 import/scene), with zero failures or skips. The normal Debug executable and
runtime DLLs were deployed and checked. Full desktop clicking and Niagara
acceptance remain outstanding.

The property panels now share the scene Inspector's actual property drawers,
SerializedObject/SerializedProperty staging and native reflected TypeTrees.
Editor-only VFX details views and enums are registered in ReflectionDatabase;
field labels and presentation metadata come from reflected fields and the
Inspector metadata registry. Dynamic module inputs use their declared native
types. VFX document transactions retain their independent Undo histories.
The refactor passed 148 tests: 66 VFX authoring, 66 existing Inspector/stream
contract tests and 16 editor/device tests. Installed UE UI parity remains pending.

## Candidate implementation

Open **Window > Particle Editor**. This is an independent candidate with
version-2/3/4/5 `.vfx` documents. Start with `Templates/VFX/Examples/Starter.vfx`,
`GpuStarter.vfx`, `DynamicStages/DynamicSystem.vfx` or `TypedStages/TypedSystem.vfx`.
Legacy `.vfx` documents are rejected by the candidate with a
version diagnostic and are never rewritten. Saving over a legacy file is refused.

The candidate implements:

- A docked Preview / Parameters / User Parameters / Local Modules / System
  Overview / Details / Timeline / Curves / Niagara Log workspace, with optional
  dynamic input script, generated code and instruction-count views.
- Multiple system documents, shared selection within each document, independent
  `EditorUndoService` histories, continuous input edits, save/discard handling,
  cached node-editor view state and per-document dock IDs. The normal editor
  shortcut dispatcher yields while this workspace owns focus.
- Ordered module stacks with enable/disable, reorder, duplicate, delete,
  stage-compatibility diagnostics and initialization dependency repair. Twelve
  candidate modules cover emitter state, rate/burst spawning, initialization,
  box sampling, velocity, gravity/drag/solver, color and size.
- Literal, typed User parameter, dynamic expression and curve inputs. The stack
  compiler carries attribute values from earlier modules in SSA form and emits
  final writes into the existing CPU/GPU graph compiler. Linear/cubic curves
  and color gradients lower to ordinary CPU/GPU expressions.
- Semantic revision detection separate from overview/graph positions. Failed
  compilation retains the last successful program. Parameter defaults are
  distinct from temporary preview overrides.
- Whole-system CPU/GPU preview using `SystemInstance`, `SystemRenderProxy` and
  the common particle renderer. Every published GPU frame is consumed, including
  seeks; simulation waits for preview capacity rather than dropping frames.
  Isolation, pan/zoom, CPU bounds, paused redraw and immutable output images
  are supported. UI seeks replay in bounded batches.
- The `new` / `compile` CLI commands use the same module-stack format. Compiled backend packages
  use the existing runtime and can omit graph/HLSL sources.
- Saving a v2 System inside the project Assets directory publishes it through
  the normal native importer. The existing ParticleSystemComponent consumes its
  precompiled runtime artifact, including source-free loading and failed-import
  rollback.

The candidate module formulas have behavior tests but are not claimed to match
every corresponding UE script version. The viewport currently uses an
orthographic camera and a grid overlay. Complete UE preview camera, lighting,
floor, gizmos, font/icon metrics and screenshot parity remain pending.

## Reference and acceptance

`NiagaraParity.json` freezes installed content/native declarations, active editor
commands, panel registrations, default/scalability layouts, widget color tokens
and acceptance gates. `Export-NiagaraParity.ps1` preserves existing evidence and
refuses changed source hashes instead of silently replacing the baseline.

`CaptureNiagaraMetadata.py` has been run in the installed Unreal Python commandlet.
It captures registry metadata for 917 assets. `Merge-NiagaraMetadata.ps1` merges
category, library visibility, stage mask and deprecation/suggestion tags without
marking functionality verified. Protected Python properties are recorded as
unavailable; property defaults and data-interface signatures still require a
more complete reference exporter.

Matrix rows remain pending or in progress until UE comparison evidence exists. Passing Nullus tests or finding a
reference file is not UE parity evidence. `Test-NiagaraParity.ps1` returns a
nonzero status while any applicable row lacks implementation, example and
passing test/reference-case report. The recorded user-directed cutover changes routing without marking parity accepted.

## Remaining work before replacement

| Area | Required work |
| --- | --- |
| Asset/IR model | Collection/definition/effect/channel/cache assets; richer types, full Parameter Map graph nodes, Custom HLSL and complete inheritance/version upgrade workflows. Independent versioned Module/Function/DynamicInput/Emitter sources, pinned references, function expansion, static switches and emitter input/settings overrides now execute in the candidate. |
| Execution | System/Emitter stages, event handlers, data-interface iteration and full stage dispatch/binding controls, warmup/fixed-step/world scheduling and complete scripting API. Authored GPU stages now iterate live particles through the existing runtime. Other nonempty unimplemented contexts report compile errors. |
| Module library | Port and compare the complete frozen library, inputs/defaults, visibility predicates and dependencies. The twelve candidate modules are an initial subset. |
| Providers/renderers | Real data-interface providers, Ribbon/Light/Decal/Component/Volume, complete Sprite/Mesh properties and scene integration. |
| Production tools | Stateless, budgets/pooling, SimCache, Baker, sequence integration, debugger/outliner and GPU timing. No placeholder panels count as implementations. |
| Interaction/visual | Complete menus/commands, clipboard/group/comment workflows, curve interaction, rich viewport/gizmos, full DPI metrics, fonts/icons and <=1 DIP geometry comparison. |
| Publishing/platforms | Complete asset-browser routing for v2, independent-player packaging, full C#/Lua scenarios; Linux/Vulkan and macOS/Metal device runs. The candidate v2 System importer now feeds the existing versioned native runtime artifact pipeline, including dependency validation and failure rollback. |
| Final acceptance | Same-machine UE comparisons, full creative workflows, 30-minute mixed stress, CPU/GPU performance and complete matrix evidence. |

Implementation and test reports are recorded in `VfxImplementationStatus.md`.
The original acceptance-first cutover constraint was superseded by the user on
2026-10-18: make the new editor default, remove the old implementation and keep
one editing path. The requirements above remain outstanding acceptance work.

## Versioned library continuation (2026-10-18)

**Window > Particle Library Editor** creates and edits `.vfxscript` Module,
Function and DynamicInput assets, and `.vfxemitter` assets. Each source has a
stable identity, exposed version and independent version GUIDs. Versions retain
input IDs and module IDs; instances pin a version, so changing the exposed
version does not silently upgrade an existing system.

Script graphs lower named `Module.*` inputs, nested function calls, static
switches and particle attribute reads/writes into the common CPU/GPU graph.
Inactive static branches are eliminated before validation of executable
expressions. Function and inheritance cycles, missing pinned versions,
incompatible stages and removed override inputs report diagnostics without
replacing the prior compiled program.

Emitter sources may inherit other emitter sources. Instance settings overrides
are explicit; module/input overrides address stable source module IDs. Each
instance receives deterministic scoped graph/module IDs. The candidate Details
panel exposes source versions, input overrides and Reset to Parent. The module
picker includes library modules; input bindings may use a versioned Function or
DynamicInput source. Source opening routes to the library editor.

Library refresh runs asynchronously. System compile identity includes library
semantics, so editing a dependency recompiles affected previews without editing
the System document. The importer captures transitive source GUID/hash
dependencies. Source-free compiled System playback continues through the normal
runtime asset. Library source artifacts use dedicated native types and preserve
the last published artifact on failure.

The reusable example is `Templates/VFX/Examples/Library/ReferencedSystem.vfx`, with
two emitter instances, a referenced module and a nested function. This is an
implemented candidate workflow, not full Niagara asset/graph parity. Complete
upgrade/conversion scripts, distribution inputs, inherited renderer editing,
all data types and all usage contexts remain pending.

## Simulation Stage continuation (2026-10-18)

The candidate lowers authored Simulation Stage stacks into the existing GPU
runtime. Stage order, disabled entries, iteration counts and stable IDs survive
compilation, cooking and source stripping. Stages run after ordinary particle
spawn/update in each simulation substep, without automatically advancing age.
Each iteration reads the previous iteration's writes. Within an iteration,
module ordering and versioned function/dynamic inputs use the same SSA map as
ordinary particle stacks. User parameter defaults and runtime overrides bind
inside stages, including nested function calls.

Both candidate editors provide stage creation, naming, enable/disable,
duplication, deletion, reorder and iteration edits through document undo. The
System editor also supports stage-header dragging and shows stage code/stats.
Stage/iteration index nodes are available in compatible script contexts.
Inherited stage state, name and iteration overrides address stable source IDs;
Reset Stage to Parent restores defaults. Local stages append to inherited stages.

CPU targets receive a stage-specific diagnostic. Limits remain 64 stages, 128
iterations per stage and 1024 total iterations. Invalid references, usage or
parameters preserve the previous program and imported artifact. The preview
validates and reuses an available backend package, so a cooked program does not
need HLSL to display.

`Templates/VFX/Examples/SimulationStages/IterativeSystem.vfx` is a runnable example
with a versioned emitter, module, nested iteration function, parameter binding
and inherited iteration override. Data-interface iteration,
execution filters, state selection and custom thread
layouts remain pending. These candidate features are not UE behavior or visual
parity acceptance.

## Dynamic stage controls (2026-10-18)

Enabled Binding and Num Iterations Binding select scalar User parameters in the
System editor. Library sources declare the names; consuming Systems validate
their existence, scalar type and default value. Inherited binding overrides can
replace or clear the parent's binding and support document undo.

Control values are validated and frozen before frame recording. Zero and
negative runtime iteration counts skip execution; static disabled stages remain
disabled. Invalid values and active iteration budgets exceeding 1024 reject the
entire parameter update. Each stage retains the candidate limit of 128
iterations. SystemInstance, GPU publication and component hot reload apply whole
parameter batches atomically. Failed replacements keep the prior program and
instance. User Parameters changes affect runtime snapshots without recompiling.

Dynamic controls and zero literal iterations use System source format 3 and
compiled program format 3. Existing v2 sources/programs continue to load, and
old readers reject the new version. The native importer is version 15; shader
layout remains 10. `Templates/VFX/Examples/DynamicStages/DynamicSystem.vfx` demonstrates
the complete source-to-cooked workflow, including a versioned inherited emitter.

The current control storage uses Float scalars constrained to boolean/integer
values. Full Niagara Bool/Int types, script-computed controls, wider reference
iteration bounds and data-interface execution remain pending. UE source was
consulted for behavior; installed-editor visual/behavior parity is unverified.

## Bool and Int32 continuation (2026-10-18)

Bool and scalar Int32 are supported through source serialization, versioned
scripts, module/function inputs, ordered SSA, runtime instructions, CPU data,
GPU simulation and parameter publication. Int32 keeps all 32 bits; Bool uses
Niagara's -1/0 mask. JSON writes native integers/booleans. GPU simulation uses
uint storage and uint4 registers for these values, with explicit bit conversion
at float boundaries, so integer minimum is not lost as floating-point -0.

Integer arithmetic, scalar comparisons, Bool logic, Select and Convert have
shared compiler/interpreter/decoder type checks. Cross-type arithmetic and
assignment require Convert. Integer addition/subtraction/multiplication wrap;
division by zero gives zero, and minimum/-1 gives minimum. Static switches accept
Bool, and stage context indices can use Int32. Curves remain floating point.

The candidate editors expose typed inputs and nodes. Stage Enabled and Iterations
can bind Bool and Int32, while legacy scalar controls remain readable. Component
SetIntParameter and SetBoolParameter have generated C#/Lua declarations and reject
incompatible declared types. Runtime defaults and overrides retain type metadata.

Typed sources and cooked programs use format 4, runtime settings version 2 and
Shader layout 11. Existing compatible Float sources/layout-10 packages still
load; new typed data cannot masquerade as an old format. Importer version is 16.
The `TypedStages/TypedSystem.vfx` example uses shared CPU/GPU integer state,
Bool selection, a static switch and typed stage controls. Full Niagara types,
integer vectors, complete conversion semantics and installed-editor parity
remain pending.

Local validation passed 116 tests with no failures or skips: 56 runtime, 28
candidate behavior, 13 candidate editor/device and 19 legacy editor/scene tests.
Windows DX12/Vulkan typed attributes were compared bit-for-bit with the CPU
reference; floating-point tolerance was 1e-5. Editor built and passed --help;
the temporary PDB output override was removed. Reports and remaining acceptance
gaps are recorded in VfxImplementationStatus.md. These local reports do not mark UE matrix rows verified; the later user-directed
cutover is recorded separately.

## System timing continuation (2026-10-18)

System Details now exposes Warmup Time, Warmup Tick Count, Warmup Tick Delta
and Fixed Tick Delta Time. The defaults and edit conditions follow the installed
reference declarations. Warmup time rounds down to whole ticks; count/delta
fields appear with active warmup, and fixed delta is disabled until enabled.
The delta can also be restored after clearing it to zero.

SystemInstance builds one immutable list of exact simulation ticks for both
CPU and GPU. Fixed ticks accumulate fractional time, execute at most the
reference default of 100 ticks per host tick, and drop excess whole ticks.
Warmup passes through the same fixed accumulator, retains particle/emitter
history and then starts visible System age at zero. Pending warmup executes on
the first active Tick, including Tick(0), after parameter/provider binding.
Reset clears the accumulator and repeats warmup. Pause freezes it.

GPU recording retains all substeps as one transaction, commits only after
submission and uses simulation time including warmup to detect skipped
publications. The candidate preview uses the same path. Seeks replay elapsed
input time in bounded batches and finish between fixed ticks, displaying the
last completed simulation tick. This avoids seeking forever when the visible
simulation age cannot reach an arbitrary fractional target.

Timing sources/programs use format 5 and runtime settings version 3. Older
float and typed formats remain readable; disguising timing in older versions
is rejected. Shader layout stays 11 and the VFX importer becomes 17.
`Templates/VFX/Examples/SystemTiming/WarmupFixedTick.vfx` demonstrates mixed CPU/GPU
warmup and fixed ticks, including source-free backend packages.

The candidate validates fixed delta 0.0001-1 s, warmup delta 0-1 s, at most 1024
warmup ticks and 1024 resolved warmup simulation ticks. Full world batching,
provider/dependency ordering, component warmup overrides, device scalability,
installed-editor visual/behavior comparisons and other platform acceptance
remain pending. Reference source inspection is not an installed-editor parity
result. The new editor is now the single default by user request.

Local validation passed 124 tests without failures or skips: 61 runtime,
29 candidate behavior, 15 candidate editor/device/import and 19 legacy
editor/scene tests. DX12/Vulkan use a 129-particle comparison including exact
Int32 tick counters; Float tolerance is 1e-5. The mixed preview reads back CPU
and GPU pixels after warmup/seek, and native import preserves timing without
sources. Editor built and passed --help. Three backend packages were generated;
this does not establish Linux/Vulkan or macOS/Metal device acceptance.
