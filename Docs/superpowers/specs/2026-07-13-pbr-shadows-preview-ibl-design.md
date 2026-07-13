# PBR Shadows And Material Preview IBL Design

## Status

The interactive design choices have been approved. This written specification
must be reviewed before an implementation plan is created. No production code
is authorized by this document alone.

## Problem

Nullus currently evaluates every visible direct light without shadow
visibility. `LightGridCommon.hlsli` decodes a fixed 16-word light record and
both Forward and Deferred PBR call the same direct-light evaluator, but
`NullusShaderLibrary/Shadows.hlsl` returns constant visibility. This produces
three related failures:

- light inside a building illuminates exterior surfaces through walls;
- a normal-mapped back face can become bright where its shading normal crosses
  the geometric light horizon;
- material-sphere thumbnails use two discrete directional samples and a weak
  ambient term, which creates a visible light/dark terminator.

The solution is one coordinated change: real cached shadows for direct lights,
geometric-normal correctness around the direct-light horizon, and precomputed
studio IBL for material thumbnails.

## Goals

- Support shadowed directional, point, and spot lights in Forward and Deferred.
- Keep the existing 16-word packed light record binary-compatible.
- Prevent unshadowed direct light while a requested shadow is unavailable.
- Bound CPU and GPU shadow work so increasing scene complexity does not stall
  the editor.
- Preserve two-sided PBR behavior and make `Cull Off` geometry cast two-sided
  shadows.
- Remove normal-map light leaks without discarding normal-map detail.
- Replace the thumbnail's hard directional terminator with deterministic,
  neutral studio IBL.
- Preserve the GPU-only prefab preview path, complete-model framing, async
  readback, and source ShaderLab keywords.

## Non-Goals

- Ray-traced shadows, virtual shadow maps, contact shadows, or screen-space
  shadow reconstruction.
- Shadows from ordinary transparent surfaces. Opaque and AlphaTest surfaces
  are in scope; transparent and decal shadow casting are not.
- Area-light integration in the scene renderer.
- Runtime convolution of arbitrary HDR environments for thumbnails.
- Changing the existing light intensity units or tone mapper.
- Runtime validation of Vulkan, Linux, or macOS from DX12 evidence.
- RenderDoc capture for this change, as explicitly requested by the user.

## Design Principles

1. Missing shadow data fails closed. A light that asks to cast shadows produces
   no direct contribution until it has a complete valid map.
2. Last-good data remains usable. Once a light has a complete map, an invalidated
   map may remain active while its replacement is delayed by the frame budget.
3. One light index is the join key. Packed light data, shadow metadata, matrices,
   cache state, telemetry, and shader evaluation use the same scene-light index.
4. One shader implementation serves Forward and Deferred. Backend or renderer
   paths may bind resources differently, but they do not reimplement visibility.
5. All expensive work is budgeted, cached, asynchronous, and observable.

## Architecture

The scene snapshot gains stable shadow inputs but remains immutable after
capture. The renderer derives a `ShadowFramePlan` before graph compilation:

```text
Scene snapshot
  -> shadow candidate collection and stable priority
  -> cache lookup and invalidation
  -> ShadowFramePlan within view, draw, and triangle budgets
  -> FrameGraph shadow depth passes
  -> shared shadow metadata/matrix/map bindings
  -> Forward opaque/transparent or Deferred GBuffer/lighting
```

`ShadowManager` owns persistent shadow resources, cache records, slot
allocation, last-good state, and telemetry. `ShadowFramePlanner` is a pure,
unit-testable policy layer that converts immutable scene/view inputs and cache
state into updates and suppression decisions. `ShadowRenderer` records the
selected depth-only views through the existing scene snapshots and prepared
draw-command caches. The existing FrameGraph remains the scheduling authority.

No new render thread, asset scheduler, or parallel build workflow is added.

## Component And Snapshot Contract

`LightComponent` exposes reflected `CastShadows` accessors and a serialized
field whose default is `true`. The render-side `Light` and captured light
snapshot carry:

- stable light identity;
- light type and existing photometric data;
- `castShadows`;
- transform and shadow-relevant revision.

`MeshRenderer` exposes reflected `CastShadows` and `ReceiveShadows` accessors.
Both serialized fields default to `true`, participate in copy/assignment and
render revision changes, and are captured into drawable snapshots. Reflection
changes go through the normal MetaParser build; generated files under
`Runtime/*/Gen/` are never hand-edited.

Dynamic instancing may combine drawables only when their shadow-casting and
shadow-receiving flags match. A receiver bit is included in prepared draw
identity and push constants, so shared materials do not need to be cloned.
The expanded object draw constants are 16 bytes and contain object index,
cast/receive flag bits, and two explicit padding words. They are visible to
vertex and fragment stages and propagate intact through immediate, prepared,
and threaded recorded-draw paths. Forward shaders consume the receiver bit
directly. Deferred GBuffer writes the bit to the material attachment alpha
channel; decal pipelines must preserve that channel by disabling writes to
non-albedo attachments. Tests protect this MRT write-mask contract.

## Shadow Quality Profile

The default balanced profile is a named renderer setting, not scattered magic
numbers:

| Setting | Default |
|---|---:|
| Directional cascades | 4 |
| Directional map resolution | 2048 x 2048 per cascade |
| Spot map resolution | 1024 x 1024 |
| Point map resolution | 512 x 512 per cube face |
| Shadow views updated per frame | 12 |
| Shadow caster draws submitted per frame | 2048 |
| Shadow caster triangles submitted per frame | 2,000,000 |
| Directional shadow slots | 1 light / 4 array layers |
| Spot shadow slots | 8 lights / 8 array layers |
| Point shadow slots | 4 lights / 24 array layers |
| Depth format | `Depth32F` |
| Directional/spot PCF | 3 x 3 comparison samples |
| Point PCF | 9 angular comparison samples |
| CSM split lambda | 0.70 |
| CSM transition width | 10% of each split interval |
| Maximum directional shadow distance | 200 world units |
| Geometric horizon fade width | 0.10 in cosine space |

The profile lives in the existing renderer/driver settings ownership boundary
as `ShadowSettings`; it is captured into immutable frame inputs and has no
separate editor-thread control path.

The default active depth allocation is 120 MiB: 64 MiB directional, 32 MiB
spot, and 24 MiB point. A reusable staging group adds at most 74 MiB: four
directional layers, one spot layer, and six point layers. Peak default shadow
depth storage is therefore 194 MiB. Pool creation is prewarmed in three bounded
steps (directional, spot, point/staging) through the existing renderer resource
preparation path, at most one step per frame. Until a pool is ready, affected
shadow-requesting lights are suppressed. Ordinary thumbnail and scene frames
never synchronously create an entire pool on first use. Resources are retained
across frames and released on device loss or renderer shutdown. Settings may
lower capacities or resolutions, but a zero-capacity pool is an explicit
unsupported condition and therefore suppresses its shadow-requesting lights.

Only the highest-priority directional light owns the default CSM slot. Extra
shadow-requesting directional lights are capacity-suppressed. This limitation
is explicit and observable rather than silently rendering them unshadowed.

## Shadow Resource And Metadata Layout

The existing `NLS_LIGHT_WORD_STRIDE == 16` layout is unchanged. Pass space 1
adds resources after the current light-grid bindings:

| Binding | Resource |
|---|---|
| `t3, space1` | `StructuredBuffer<ShadowLightMetadata>` |
| `t4, space1` | `StructuredBuffer<float4x4>` shadow matrices |
| `t5, space1` | directional `Texture2DArray<float>` |
| `t6, space1` | spot `Texture2DArray<float>` |
| `t7, space1` | point-face `Texture2DArray<float>` |
| `s0, space1` | comparison sampler |
| `b1, space1` | `ShadowFrameData` |

`ShadowLightMetadata[lightIndex]` contains flags, shadow type, ready state,
pool slot, first matrix index, view count, near/far values, and four cascade
split depths. Non-shadowing lights have a disabled record. A record is ready
only when every view required by that light is valid: four for directional,
one for spot, and six for point.

The matrix buffer uses the same view ordering as the FrameGraph passes. Point
shadows render six 90-degree faces into a 2D array. Sampling reprojects each
angular PCF direction to a face and UV, including offsets that cross cube-face
boundaries; this avoids seams without requiring a separate cube-array RHI path.

Updates render into the reusable staging group for that light type. After all
views in the selected update group complete, FrameGraph copies the staged depth
subresources into the light's active layers and publishes matching metadata.
Spot groups are serialized through their single staging layer. This staging
contract prevents a partial CSM or point update from overwriting last-good
data, without doubling every capacity slot.

All depth projections and compare functions use the active RHI clip-space and
depth convention. Shader tests cover the DXIL and SPIR-V forms; backend helper
functions, not call sites, own any projection or Y-axis differences.

## Cascaded Directional Shadows

Four split distances are computed from the camera near plane to
`min(cameraFar, 200)` using practical split blending with lambda 0.70. Split
depths are stored in positive view-space distance. Each orthographic cascade:

- encloses its camera-frustum interval plus the 10% blend overlap;
- derives stable XY extents from the camera-frustum interval and extends only
  light-space Z for eligible caster bounds intersecting that interval;
- snaps its light-space XY center to one shadow texel;
- retains stable square extents until the quantized extent changes;
- uses the same snapped matrix for rendering and sampling.

Within an overlap, visibility from adjacent cascades is linearly blended. The
nearer cascade wins outside the overlap. A missing farther cascade does not
invalidate already complete last-good CSM data; a new directional light remains
suppressed until all four cascades are ready.

The update scheduler prioritizes cascade 0, then 1, 2, and 3. Camera movement
invalidates only cascades whose snapped projection or caster set changes.

## Spot And Point Shadows

Spot shadow projection uses the light outer cone, a finite near plane derived
from map texel size and light range, and the light effect range as far plane.
Point shadows use six canonical 90-degree views and the same near/far policy.
Invalid or non-positive ranges produce no shadow plan and suppress direct light
for that shadow-requesting light.

Directional and spot filters use nine comparison samples over a 3 x 3 texel
kernel. Point filtering constructs a stable tangent basis around the
light-to-fragment direction and takes nine angular samples whose radius equals
one face texel at the receiver depth. Every sample is finite-checked and
clamped to a valid array layer.

Bias is expressed in shadow texels so it scales with resolution. The balanced
profile uses a 1.25-texel constant receiver bias, a 1.75 slope multiplier, and
a 0.50-texel world-normal offset. Bias uses the oriented geometric normal, not
the normal-map result. These values are named settings and are exercised by
acne and detached-shadow fixtures.

## Caster Eligibility And Pass Selection

A drawable enters a shadow view only when all of the following are true:

- its renderer is active and `CastShadows` is true;
- its bounds intersect that shadow view;
- its material surface mode is Opaque, or the material has `_ALPHATEST_ON`;
- required mesh and material resources are ready.

Ordinary Transparent and Decal surfaces do not cast in this iteration. Opaque
materials use a depth-only shadow caster path. AlphaTest materials sample their
base/opacity texture and apply the same cutoff as the visible pass. Built-in
Standard PBR and ShaderLab Standard PBR provide explicit `ShadowCaster` passes.
Custom ShaderLab shaders may provide a `LightMode = ShadowCaster` pass. If an
opaque custom shader has no such pass, the conservative built-in depth-only
fallback casts its silhouette. If an AlphaTest custom shader lacks a compatible
shadow pass/property contract, it also casts the conservative opaque silhouette
and emits a diagnostic; it never becomes an unshadowed light leak.

Shadow caster collection uses active scene primitives intersecting each shadow
frustum, not the main camera's visible-draw list; an off-screen wall can still
shadow an on-screen receiver. Candidate generation uses the retained
`RenderScene` spatial index and revisioned draw-command cache. It does not scan
or rehash every scene primitive for every light each frame.

Shadow pass raster state inherits the visible material's culling semantics.
In particular, `Cull Off` becomes no culling and both faces cast. Front-only or
back-only settings remain consistent with the source material. Shadow variants
inherit source ShaderLab keywords needed by AlphaTest and property binding.

## Cache, Invalidation, And Budgeting

Each cache record is keyed by stable light identity plus pool generation. It
stores slot ownership, last-good matrices, view validity, caster fingerprint,
light fingerprint, last-used frame, and pending dirty views.

Local-light shadow maps are reused when all relevant inputs are unchanged.
They invalidate on:

- light transform, type, range, cone, or shadow-setting changes;
- caster transform, mesh, LOD/HLOD representation, active/eligibility state,
  cast flag, or bounds changes inside the light volume;
- material surface mode, cull state, AlphaTest keyword/cutoff/texture, shader
  generation, or resource generation changes;
- pool/device generation changes.

Static local lights with static casters therefore become cache hits. Dynamic
casters dirty only affected local lights. Directional cascades additionally
track camera projection, snapped cascade transform, and per-cascade caster
fingerprints.

Before scheduling, cached draw commands provide conservative draw and triangle
costs for every candidate view. The planner assigns at most 12 shadow views,
2048 caster draws, and 2,000,000 caster triangles per frame. A complete update
group must fit all three remaining budgets; otherwise it stays delayed on
last-good data, or its new light stays suppressed. Cost estimation counts
instanced triangle submissions per instance and all cascade/point-face
duplication. A pass may not silently truncate its caster list, because that
would publish a leaking map. Stable priority is:

1. incomplete new lights that already own a slot, ordered by light importance;
2. directional cascades from near to far;
3. invalidated visible local lights by projected influence, intensity, camera
   distance, and stable light identity;
4. maintenance refreshes.

A point light is an atomic six-view update group. A new directional light is an
atomic four-view update group; after first publication, individual dirty CSM
cascades may update independently, with near cascades ordered first. The
planner schedules a group only when the remaining frame budget can contain the
whole group. A spot light needs one view. New lights emit no direct light until
their complete initial group is committed. Once last-good data exists, delayed
replacements keep sampling that data and its matching matrices until the staged
replacement group commits. Partial maps are never published.

When pool capacity is exhausted, the planner keeps the highest-priority lights
and marks lower-priority shadow-requesting lights suppressed. Suppressed lights
contribute neither diffuse nor specular direct light. Non-shadow-casting lights
continue to render normally. Slot eviction is LRU within lower priority and
cannot evict a resource referenced by an in-flight frame.

### Complexity Bounds

Let `L` be shadow-requesting lights, `K` the spatial candidates returned for
their affected volumes, `D` the cached draw commands touched, and `V` the
scheduled views. Retained spatial queries are `O(log N + K)` for static scene
partitions plus the existing bounded dirty overlay. Candidate ranking is
`O(L log L)`, fingerprint/update planning is `O(K + D)`, and FrameGraph plan
emission is `O(V)`. CPU scratch storage is `O(L + K + D)` and is reused between
frames. No stage performs `O(L * N)` full-scene scanning, per-caster heap
allocation in the planning loop, or same-frame GPU readback.

GPU submission is bounded by the three profile limits. Cached static frames
submit zero shadow draws. The implementation must publish observed estimator
error (`estimated` versus `submitted` draws/triangles) so undercounting cannot
silently invalidate the budget.

## FrameGraph Ordering

Shadow resource imports and selected shadow-view passes are added before the
first scene pass that consumes direct lighting:

- Forward: shadow passes, light-grid compute, opaque, decals/sky, transparent.
- Deferred: shadow passes, light-grid compute, GBuffer, decals, deferred
  lighting, transparent.

The exact placement may allow independent light-grid compute and shadow depth
recording to overlap, but FrameGraph dependencies guarantee all published
shadow writes complete before a sampling pass. Shadow depth subresource ranges
name the exact array layer. Transitions never cover unrelated cached layers.

Prepared/threaded rendering captures immutable shadow inputs with the same
frame snapshot as visible draws. No shadow path dereferences mutable components
on the render thread. Existing draw-command caches are reused with a distinct
shadow-pass pipeline key and receiver/caster flags included in invalidation.

## Shared PBR And Shading-Normal Correctness

PBR paths keep two normals:

- `geometryNormalWS`: the transformed mesh normal, oriented by
  `SV_IsFrontFace` for two-sided materials;
- `shadingNormalWS`: the normal-map result from the oriented tangent frame.

Deferred preserves both normals without adding another GBuffer attachment. The
oriented `geometryNormalWS` is octahedrally encoded into two scalar channels:
oct X is stored in `GBuffer Albedo.a`, and oct Y is stored in
`GBuffer Normal.a`. `GBuffer Normal.rgb` continues to store the encoded
`shadingNormalWS`; `GBuffer Material.rgb` continues to store metallic,
roughness, and AO. `GBuffer Material.a` stores `ReceiveShadows` as an exact
zero-or-one receiver bit. Opaque and AlphaTest visibility has already been
resolved before these writes, so deferred lighting does not require surface
alpha afterward. Deferred decal passes write only `GBuffer Albedo.rgb`, disable
writes to the Normal and Material attachments, and preserve all three alpha
control channels; a decal may not replace the receiver bit or either
geometric-normal component.

The shading normal is normalized, finite-checked, and constrained to the
geometry-normal hemisphere. Direct-light evaluation computes geometric
`N_g dot L` and `N_g dot V` first. A non-positive value rejects that direct
sample. Positive values below 0.10 receive a `smoothstep(0, 0.10, value)` fade,
which removes an abrupt terminator while preventing a tangent-space normal from
lighting through the geometric surface.

GGX distribution, Smith masking, Fresnel, diffuse energy conservation, and the
existing normal-variance roughness filter use `shadingNormalWS`. The final
direct contribution is multiplied by both geometric fades and shadow
visibility. Ambient and IBL are evaluated separately and use AO; they are not
multiplied by direct-light shadow visibility.

The shared API accepts both normals, light index, receiver flag, and the current
light data. Forward clustered lighting and Deferred scene lighting call the
same `NLSEvaluateShadowVisibility` and PBR direct evaluator. Phong remains
compatible but also consumes real shadow visibility when used with a receiving
renderer.

If `ReceiveShadows` is false, visibility is exactly 1.0 for that drawable. If a
light does not request shadows, visibility is also 1.0. If a light requests
shadows but metadata is not ready or capacity-suppressed, the direct-light loop
skips the light entirely rather than interpreting missing metadata as visible.

`MAIN_LIGHT_SHADOWS` becomes a renderer-controlled pass keyword for the shared
shadow-capable path; it is not taken from an individual source material as an
opt-in. When the shadow-capable variant cannot be created, native light packing
removes every `CastShadows` light from direct-light evaluation while preserving
non-shadow-casting lights. A missing shader variant therefore cannot turn a
shadow-requesting light into an unshadowed one.

## Material Preview Studio IBL

The existing two-sample key-light rig is replaced as the primary material
illumination. The normal asset-build pipeline deterministically generates a
neutral analytic studio environment and precomputes shared engine artifacts:

| Artifact | Format | Size |
|---|---|---:|
| diffuse irradiance cubemap | `RGBA16F` | 32 x 32 x 6 |
| GGX prefiltered cubemap | `RGBA16F`, full mip chain | 128 x 128 x 6 |
| split-sum BRDF LUT | `RG16F` | 256 x 256 |

The analytic source contains neutral walls and broad white softboxes, with no
colored background or external licensed HDR image. Build-time convolution uses
1024 deterministic Hammersley samples per irradiance texel, per prefilter texel
at each mip, and per BRDF LUT texel. Outputs carry an asset-build recipe version
so a recipe or algorithm change invalidates them through the existing artifact
database.

The runtime thumbnail renderer loads these resources once, prewarms their
pipelines, and shares them across all material requests. It uses split-sum IBL:
Lambert irradiance for diffuse, roughness-selected prefiltered radiance for
specular, and the BRDF LUT for the integrated Fresnel/visibility term. AO
attenuates ambient diffuse and specular occlusion according to the existing PBR
material inputs. A fixed environment rotation, camera, exposure, sphere mesh,
and background make thumbnails comparable.

One weak directional key at intensity 0.15 remains for readable normal detail;
IBL is the dominant source. If any IBL artifact is missing, invalid, or not yet
ready, the request uses a deterministic 16-sample softbox fallback whose total
intensity is 0.85. The fallback does not block the UI and is cached like the
normal preview. If both GPU paths fail, the service returns a diagnostic and
the existing placeholder instead of leaving the thumbnail pending forever.

Stable preview materials continue to copy the source material's ShaderLab
keyword set, textures, sampler overrides, culling state, and relevant PBR
parameters. Prefab/model previews remain GPU-only and retain their resource
pump budgets, complete-snapshot requirement, complete-model framing, and async
readback behavior.

All GPU PBR preview requests move from
`asset-browser-thumbnail-renderer:v9` to `v10`. The legacy v8 software/icon
paths are unchanged.

## Failure Handling

- Device loss invalidates all shadow pool generations and IBL GPU handles.
  Recovery lazily recreates resources; shadow-requesting lights remain
  suppressed until valid maps return.
- Allocation or pipeline creation failure disables the affected pool, records
  a diagnostic once per generation, and suppresses affected lights.
- A failed shadow view never replaces last-good data.
- Invalid matrices, ranges, array indices, NaN depths, and incomplete metadata
  fail closed and increment diagnostics.
- Missing AlphaTest shadow resources cast a conservative silhouette.
- IBL load failure takes the 16-sample fallback without synchronous IO or GPU
  readback on the editor frame.
- Shadow planning and cache updates are CPU-only over immutable snapshots; GPU
  completion is observed through existing completion tokens, never a blocking
  fence wait.

## Telemetry

Renderer stats publish per-frame and rolling values for:

- shadow planning CPU time and GPU shadow-pass time;
- requested, scheduled, completed, and delayed shadow views;
- estimated and submitted caster draws/triangles versus their frame budgets;
- cache hits, invalidations, evictions, and last-good swaps;
- directional/spot/point slot use and capacity-suppressed lights;
- not-ready suppressed lights and stale last-good frame counts;
- caster candidates, accepted caster draws, and reused draw commands;
- IBL ready/fallback/failed thumbnail counts and preview GPU/readback timing.

Telemetry collection cannot add a same-frame readback or global device wait.
FrameInfo exposes aggregate values; detailed diagnostics remain behind existing
render diagnostic settings.

## Tests And Validation

Behavior changes follow TDD: each contract test is added and observed failing
before its production implementation.

### Deterministic Unit And Contract Tests

- `LightComponent.CastShadows` and `MeshRenderer.CastShadows/ReceiveShadows`
  defaults, serialization, copy, reflection registration, and render revisions.
- Exact preservation of the 16-word light record and alignment of shadow
  metadata with scene light indices.
- Stable priority, view/draw/triangle budgets, atomic point/CSM publication,
  last-good swaps, LRU eviction, and capacity suppression.
- Cache invalidation for light, caster, material, AlphaTest, LOD/HLOD, shader,
  resource, camera, and device revisions.
- FrameGraph pass order, exact layer dependencies, and no unrelated cached
  layer transitions.
- Directional CSM split blending, texel snapping, and small camera-motion
  stability.
- Directional, spot, and point occluder visibility fixtures.
- `Cull Off` two-sided shadow casting and AlphaTest holes.
- Geometric-normal horizon regression with a strongly tilted tangent normal.
- Forward/Deferred use of identical metadata and visibility helpers.
- Receiver push-constant layout, instancing split, GBuffer material-alpha
  encoding, and decal preservation.
- IBL artifact descriptors, deterministic recipe version, shared runtime load,
  v10 cache key, source ShaderLab keywords, and 16-sample fallback.

### Shader Validation

- Compile affected HLSL and ShaderLab variants to DXIL and SPIR-V.
- Run `spirv-val` on every emitted SPIR-V module.
- Cover `_NORMALMAP`, `_ALPHATEST_ON`, `MAIN_LIGHT_SHADOWS`, `Cull Off`, shadow
  receive on/off, and fallback IBL combinations.
- Assert all shadow buffer/texture registers match native binding layouts.

### GPU Proof Fixtures

DX12 GPU tests render deterministic PNGs for directional, point, spot,
two-sided wall, AlphaTest fence, closed building, and material-sphere fixtures.
Measurements are performed in linear color before PNG encoding:

- an occluded exterior patch is at most ambient plus 0.02 luminance;
- a back-face brick-gap fixture has no pixel above ambient plus 0.05 when its
  geometric normal faces away from the light;
- Forward/Deferred fixture RMSE is at most 0.02 and maximum interior-pixel
  difference is at most 0.08;
- outside a two-pixel silhouette/AlphaTest mask, no isolated pixel may exceed
  0.98 luminance while its eight-neighbor median is below 0.75;
- on the neutral rough dielectric sphere, the 99th-percentile adjacent
  luminance delta outside the silhouette is at most 0.12;
- red, green, blue, metallic, and roughness thumbnail fixtures retain channel
  ordering and monotonic roughness response.

### Performance Validation

On the recorded DX12 adapter, run repeatable camera paths for a static cached
scene, a moving local light, moving casters, and an intentionally over-budget
scene. Record OS/build, CPU/GPU hardware, driver, fixture parameters, the
deterministic camera path, and comparable before/after per-stage baselines.
Record p50/p95/p99 for shadow planning CPU time, shadow GPU time, total editor
frame time, views, draws, triangles, cache hits, and suppressed lights.
After warmup, the balanced-profile sign-off thresholds are:

- shadow planning CPU p95 at most 0.50 ms;
- shadow GPU p95 at most 2.50 ms for workloads admitted by the default budgets;
- cached static shadow GPU p95 at most 0.10 ms with zero shadow draws;
- over-budget p99 editor-frame increase at most 4.0 ms, with excess groups
  delayed/suppressed and no incomplete map publication;
- no synchronous fence wait, readback, or whole-pool allocation in the measured
  frame path.

These numbers are reference-adapter evidence, not universal hardware claims.
If the target is missed, the implementation is not complete: optimize retained
candidate/draw reuse or revise the named quality profile and re-run review; do
not hide the miss by dropping unshadowed casters.

Run the DX12 Debug Layer and build the Debug Editor. Record the actual backend
and adapter used. SPIR-V compilation is portability evidence only; it is not a
claim of Vulkan runtime correctness. RenderDoc is intentionally omitted.

### Baseline At Specification Time

On Windows/MSVC with SDK 10.0.22621.0, configuration and a Debug build of
`NullusUnitTests` succeeded from the isolated branch. The build warned that the
Autodesk FBX SDK was not prepared, so Assimp FBX is the active importer in this
worktree. `ctest -L behavior` did not complete within a 10-minute observation
window and was terminated; it is not counted as passing baseline evidence.
Implementation validation therefore starts with focused named tests before a
longer full behavior-suite run.

## Acceptance Criteria

- Exterior geometry is not directly illuminated by shadow-casting lights
  enclosed inside opaque geometry.
- Back faces and brick gaps do not receive direct light from the wrong
  geometric hemisphere, including with strong normal maps.
- Directional, point, and spot shadows work in both Forward and Deferred.
- `Cull Off` casts from both faces; AlphaTest holes remain holes in shadows.
- New or capacity-suppressed shadow-requesting lights never fall back to
  unshadowed direct light.
- Shadow work never exceeds 12 scheduled views in one frame under the default
  profile, 2048 caster draws, or 2,000,000 submitted caster triangles; static
  local-light cache hits schedule zero views.
- Increasing polygon count cannot exceed the shadow submission budgets or
  introduce a game-thread wait, same-frame readback, first-use whole-pool
  allocation, or unbounded per-frame map updates.
- Material spheres use shared studio IBL, show no obvious hard terminator or
  isolated bright pixels, and remain comparable across materials.
- Prefab/model previews remain GPU-only, show the complete ready model, inherit
  source ShaderLab keywords, and do not block the editor UI.
- DXIL/SPIR-V shader validation, focused behavior tests, DX12 GPU proof tests,
  DX12 Debug Layer, and Debug Editor build pass before completion is claimed.

## Industry References And Deliberate Differences

The plan-review registry has no dedicated shadow-map or PBR-IBL entry. The
closest registered item is `benchmarks/rendering_layout.md` section
"Rendering Large-Scene Visibility And Residency", whose relevant requirements
are immutable snapshots, explicit FrameGraph dependencies, cached draw
commands, bounded work, telemetry, and no ordinary CPU/GPU synchronization.
This design applies those requirements to shadow visibility.

Concrete rendering references are:

- Unreal Engine 4.27 `Renderer/Private/ShadowSetup.cpp` and
  `Renderer/Private/ShadowRendering.cpp`: per-light shadow setup, cascaded
  directional views, cached shadow work, and explicit shadow rendering phases.
- Google Filament `filament/src/ShadowMap.cpp` and
  `filament/src/ShadowMapManager.cpp`: stable shadow-map transforms, map
  management, and visibility integration.
- Google Filament `libs/ibl/src/CubemapIBL.cpp`: offline cubemap irradiance and
  GGX prefilter generation used by split-sum image-based lighting.

Nullus deliberately uses fixed array pools instead of virtual shadow maps,
because the requested balanced profile and current RHI do not justify virtual
page management. It uses a deterministic analytic studio source instead of a
licensed HDR image, and it preserves the current packed light ABI through a
parallel metadata buffer. A future plan-review benchmark update should add a
dedicated shadow/IBL entry using the references above.
