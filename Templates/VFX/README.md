# VFX templates

For current authoring, start with [Examples/Starter.vfx](Examples/Starter.vfx) or the
other module-stack examples in [Examples/README.md](Examples/README.md), then open
**Window > Particle Editor**. This is the only particle editor. New assets and
`NullusVfxCompile new` / `compile` use this format.

The graph-format files in this directory are retained as runtime/import
fixtures. The removed graph editor cannot open them; no files are automatically
converted. The descriptions below document runtime capabilities and legacy
fixtures; they are not instructions for opening these files in the current editor.

## Legacy runtime fixtures

`BasicEmitter.vfx` contains a CPU emitter with a position distribution, velocity
integration, color, size, lifetime and looping spawn rate. Import it through the
runtime import path by placing it in the project's Assets directory. Use the
module-stack examples above for current editor authoring.

`BasicGpuEmitter.vfx` uses the same graph with GPU simulation. The Particle Editor
is available in the Window menu. Its inspector exposes the CPU/GPU target, capacity,
seed, lifetime defaults, spawn rate, looping, interpolated spawning and bursts.
Spawn and Update graphs share source diagnostics and undo/redo. Enable **Sort by
View Depth** for transparent particles to draw from far to near. Each view builds
its own GPU index buffer for both CPU and GPU emitters; equal depths use particle
IDs for a repeatable order. Within one emitter the simulation stream is sorted;
across emitters the scene renderer orders the transparent draws by the same view
depth, so systems and emitters that overlap at different depths no longer paint
in declaration order. CPU emitters contribute their real simulation bounds. GPU
emitters copy their bounds back asynchronously: the first snapshot is requested
immediately and later snapshots refresh every four submitted frames, without a
CPU fence wait. A same-component GPU system therefore switches to its real
far-to-near order after the first readback completes. Until that first snapshot
or if the device readback fails, the component origin remains the fallback. It
also applies to the editor preview and survives source saves and cooked packages.

**Frustum Culling** is enabled by default. The renderer calculates dynamic bounds,
preserves Sprite dimensions in world units, and builds separate visible indices
and indirect draw counts for each view. It keeps the simulation alive off screen;
particles with nonfinite positions or dimensions are omitted from drawing and
counted as invalid. Disable the option to inspect the full valid particle stream.
Use **Show Bounds** in Preview to display the projected bounds and alive/visible/
culled/invalid counts. GPU debug values arrive asynchronously and identify their
own preview image; they never gate simulation or drawing.

**Initial Allocation** controls the simulation's starting storage; zero chooses
up to 256 slots. **Capacity** remains the particle limit. CPU storage grows as
particles spawn. GPU storage copies each attribute column and 64-bit IDs into a
larger allocation before dispatching births, keeps old frame resources alive,
and avoids reading GPU counts on the CPU. GPU growth uses a conservative live
count bound, so long-running emitters can eventually allocate their full limit.
Reset retains allocated storage.

The **Simulation Stages** tab edits GPU particle stages: add/remove, rename,
enable, reorder, choose iterations, and edit each stage's graph with undo/redo.
Stages execute in order after Particle Update/Spawn for each simulation substep.
Each iteration sees the previous iteration's writes and receives the substep's
Delta Time; it does not advance Age automatically. **Stage Index** and **Stage
Iteration** nodes are zero-based. Random streams include both indices. Lifetime
compaction and bounds run after the stages. The current stage source is live
particles; Grid/data-interface iteration, filters and dynamic iteration bindings
remain in the implementation plan. CPU emitters with stages report a compile error.

Select the Preview tab to view the selected CPU or GPU emitter through the common
particle renderer. Shader compilation runs asynchronously after edits settle; images are
published after their GPU fence completes. Play/Pause and Restart control the preview.
DX12 and Vulkan simulation/rendering have device tests. Preview shows one selected
emitter. The scene runtime plays every emitter in a system.

`MeshShards.vfx` contains CPU and GPU mesh emitters with animated quaternion
orientation. Copy `Meshes/ParticleShard.gltf` **and its `.meta` file**, import the
mesh first, then import the effect. The sample references mesh subasset
`mesh:mesh/0` by GUID/localId, so moving the source with its meta preserves the
reference. The embedded glTF buffer needs no external binary file.

Choose **Renderer: Mesh** and select an imported mesh in the inspector. **Scale
Attribute** accepts Float or Float3 (default `Particles.Scale`), and **Orientation
Attribute** accepts a Float4 quaternion in xyzw order (default
`Particles.MeshOrientation`). Missing attributes use the renderer defaults; the
**Add Scale / Orientation Attributes** button exposes them to Spawn/Update graphs.
Quaternions are normalized; a zero quaternion uses identity. Scale is in mesh
units and includes the component's world transform. Sprite Size/Rotation,
billboard alignment and the circular mask are ignored for Mesh.

Mesh uses indexed instancing, the selected mesh's UVs, per-view indirect counts,
sorting and conservative bounds that include its local center, scale, quaternion
and world transform. Nonfinite transforms remain in simulation and are counted
as invalid for drawing. The default unlit color, textures, blend/depth rules,
soft distance and `ParticleSprite` fragment material ABI work for either geometry
type. The domain keeps its existing name for asset compatibility. Mesh supports up
to eight slots. **Add Mesh Slot** selects another imported mesh; each slot can
override the emitter material. **Mesh Index Attribute** defaults to
`Particles.MeshIndex`: finite values round to the nearest slot and clamp to the
available range, missing attributes select slot zero, and NaN/Inf are invalid for
drawing. Each slot freezes its own color, textures, samplers and material constants.

**LOD (-1 = Auto)** selects one level per slot and view from its projected mesh
bounds and the component transform. Nonnegative values request an imported LOD,
clamped to the mesh's MinLOD and available levels. Every captured LOD retains strong
vertex/index buffers; changing a non-primary LOD or slot material redraws paused
previews. Selection is shared by all particles using that slot in the view; per
particle LOD, per-section materials, lighting, vertex deformation and motion vectors
remain in the implementation plan. Multiple slot draws also group transparent
particles by mesh, so overlap between different slots does not preserve a single
global transparency order.

`MultiMeshShards.vfx` demonstrates CPU/GPU emitters choosing between two shapes in
Spawn. Copy `Meshes/ParticleShard.gltf`, `Meshes/ParticleChip.gltf` and both `.meta`
files, import both meshes first, then import the effect. Its deterministic random
mesh index is retained by saves and compiled packages.

`MixedFountains.vfx` combines an orange CPU fountain and a blue GPU fountain. Their
velocities diverge so both targets can be inspected in one scene.

`SortedTranslucency.vfx` spawns overlapping, differently colored GPU sprites with
alpha 0.45 and depth sorting enabled. Orbit the scene camera to inspect their
composition, or switch the emitter to CPU simulation to use the same source graph.

`StagedGpuEmitter.vfx` starts with 16 slots, grows with a burst and continuous
births, and runs Lift, Contract and Iteration Size stages. Change `User.Lift` or
reorder/disable stages to inspect their effects. The cooked package contains
precompiled stage kernels and needs no source graphs or HLSL at runtime.

`AnimatedSpriteAtlas.vfx` animates four frames, blends adjacent images, rotates
sprites and aligns them with velocity. Copy it together with `Textures/` into
your project's Assets directory, including `SpriteAtlas.tga.meta`, so its stable
texture reference is preserved. Import the texture first, then the effect. The
same graph also runs with CPU simulation.

The **Sprite Renderer** inspector chooses a material or a 2D texture override,
tint, emissive intensity, camera/velocity alignment, optional circular mask,
SubUV grid and frame interpolation. **Add Rotation / SubImage Attributes** adds
`Particles.Rotation` (radians) and `Particles.SubImageIndex` (fractional frame).
Write them from Spawn/Update or a GPU Simulation Stage. Frames clamp to the first
and last atlas tile. A one-tile texture uses the material's UV tiling and sampler;
atlas tiles clamp at a base-level half texel. Atlases need suitable padding/mips
for minification.

Blend modes are Translucent, Additive, Premultiplied, Masked and Opaque. Masked
and Opaque write scene depth and draw before transparent passes. **Soft Distance**
fades transparent sprites near scene geometry in view-space units when a sampled,
single-sample scene depth target is available. **Depth Plane** in Preview lets you
adjust a test plane to inspect depth fading and clipping.

The default material path captures unlit color, a base texture, UV transform,
emissive intensity and a sampler from imported engine materials. It recognizes
`u_Diffuse`/`u_Albedo`/`u_BaseColor` and `_BaseColor`/`_Color`, corresponding base
texture slots and UV properties. These values and strong RHI handles are frozen
before rendering.

Enable **Material Shader (ParticleSprite)** to execute a material's particle pass.
`ParticleMaterial.shader` demonstrates the domain: declare only `#pragma fragment
ParticleMaterialPS`, include `NullusShaderLibrary/ParticleSprite.hlsl`, and implement
`float4 EvaluateParticleMaterial(ParticleMaterialInput input)`. The engine supplies
the Sprite vertex shader, SubUV sampling and final masking/depth fade/blending.
Inputs include UV, sampled base color, particle color, world position, velocity,
age/lifetime/normalized age, and four dynamic float4 values. Declare custom material
constants at b0-b15, textures at t0-t15 and samplers at s0-s15 in space2. The current
domain supports 2D textures, at most 64 KiB of material constants, and no writable
resources. General surface shaders, lighting, visual material graphs and vertex
deformation remain future work.

**Dynamic Material Parameters** binds four float4 particle attributes. The default
names are `Particles.DynamicMaterialParameter` and the suffixes `1`, `2`, `3`.
When an attribute is missing, its **Fallback Parameter** uses a system parameter,
then its **Default** value. Existing attributes take precedence. The default
material multiplies its color by the first dynamic float4; a particle material
can use all four values for its own expressions.

**Material Parameter Bindings** maps typed system parameters such as `User.Tint`
to reflected material constants such as `_Tint`. Missing system values use the
binding's default. The material asset itself is shared; changes create new
constant snapshots for the next draw. **User Parameter Overrides** in Preview
lets you edit these values, including while paused. Scene components expose
`SetFloatParameter` and `SetVectorParameter` for the same bindings.

Copy `DynamicMaterialSprite.vfx`, `ParticleMaterial.shader`, `ParticleMaterial.mat`,
their `.meta` files and `Textures/` into Assets. Import the texture and shader,
then the material and effect. The native `.mat` source retains its authoritative
ShaderLab reference. Imported material artifacts record Shader/texture identities;
runtime particle loading resolves compiled Shader artifacts through the manifest
and does not require source files, `.meta` files or the editor ArtifactDB. Keep the
effect's material/texture dependencies in the packaged runtime manifest. The
VFX-only command-line JSON does not bundle those external assets.

To play an effect in a scene, copy a template into the project's Assets directory,
let the Asset Browser import it, and add **Effects > Particle System** to a GameObject.
Assign the `.vfx` asset to **System** in the Inspector, then enter Play mode.
**Create > Particle System** in a folder's context menu writes a default editable
system, imports it and opens the Particle Editor; right-click **Edit** or
double-click an existing `.vfx` source asset reopens it there.
**Play On Awake** starts the effect; the component also exposes Activate, Stop,
Restart, SetPaused, GetAge, SetFloatParameter and SetVectorParameter through the reflected script API.
Forward and Deferred use the same textured Sprite path as the editor preview.

Collision comes from scene geometry. Add **Physics > Collider** to the objects that
should block particles (box, sphere or capsule), set **Layers** so they overlap the
emitter's collision layers, then enable collision in the emitter graph. The scene
creates its own PhysX query world on first use, and the particle component binds to
it automatically before the first tick; `SetCollisionWorld` remains available when a
component has to query a different world.

`NullusVfxCompile compile <source.vfx> <compiled.json> --backend all` produces a package
with CPU instructions, DX12 DXIL, Vulkan SPIR-V and Metal input SPIR-V, with source
graphs and HLSL stripped. Select `dx12`, `vulkan` or `metal` for a single backend.
The editor importer places the package inside a native content artifact and tracks
the shader toolchain version. Runtime components resolve that artifact through the
runtime manifest; they do not compile source shaders. The CLI's JSON output is a
compiler inspection artifact; use the asset importer for manifest-backed loading.

Full Niagara modules, material/renderers, production tools, standalone application
packaging, Linux devices and macOS/Metal acceptance remain in the implementation plan.
