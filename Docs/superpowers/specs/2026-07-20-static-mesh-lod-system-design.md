# Static Mesh LOD System Design

## Status

Approved for implementation on 2026-07-20.

This specification covers StaticMesh LOD only. SkeletalMesh LOD remains a
separate project because it requires bone influence reduction, morph target
handling, cloth data, and animation-specific validation.

## Problem

Nullus currently has a partial scene-side LOD selector and visibility filter,
but it does not form a complete asset-to-runtime lifecycle. Production mesh
components do not consistently register LOD sets, mesh artifacts have no
authoring/build contract for multiple levels, screen-size selection is not
projection-aware, lifecycle updates can leave stale state, and neither import
nor Cook can deterministically generate reduced meshes.

The result is a feature that can be exercised by synthetic tests but cannot be
relied on from source asset import through editor preview, thumbnail rendering,
Cook, packaged loading, scene registration, and runtime selection.

## Goals

- Provide a complete StaticMesh LOD lifecycle from import through Cook and
  runtime rendering.
- Match UE4.26 StaticMesh behavior where it fits Nullus architecture.
- Keep source/build settings separate from derived runtime render data.
- Generate automatic LODs deterministically with the existing meshoptimizer
  dependency.
- Provide UE4.26-compatible LODGroup preset names and default values.
- Preserve authored LODs and per-level overrides during reimport and preset
  changes.
- Use projection-aware, per-view runtime LOD selection with forced LOD, MinLOD,
  hysteresis, transitions, and residency fallback.
- Build the same derived artifact in editor builds and target-platform Cook.
- Make asset-browser thumbnails select from formal LOD data without generating
  preview-only simplified meshes.
- Add focused unit, integration, compatibility, and end-to-end tests.

## Non-Goals

- SkeletalMesh LOD, bone reduction, morph targets, or cloth.
- HLOD cluster generation.
- Nanite-style virtualized geometry.
- Runtime mesh reduction.
- A new independent per-LOD package streaming system. This project defines the
  residency contract and fallback behavior while the current resource system
  may load all levels from one artifact.

## UE4.26 Reference And Deliberate Difference

The runtime selection model follows UE4.26 behavior represented by
`ComputeBoundsScreenRadiusSquared`, `ComputeStaticMeshLOD`, and
`FStaticMeshSceneProxy::GetLOD`/`GetLODMask`: bounds-sphere projection,
per-view distance factors, forced and minimum LOD constraints, coherent screen
size thresholds, and temporal transition state.

The built-in preset names and defaults come from UE4.26
`Engine/Config/BaseEngine.ini` under `[StaticMeshLODSettings]`.

There is one intentional product difference. UE4.26 asset thumbnails force
LOD0 and disable automatic LOD selection. Nullus asset-browser thumbnails will
instead run the standard screen-size selector against formal built LOD data so
that the rendered thumbnail uses the level appropriate for its view and
viewport. Nullus will not generate a temporary simplified thumbnail mesh.

## Architecture

The system is split into authoring input, deterministic build services, derived
artifacts, and runtime consumption:

```text
Source file / authored LODs
        |
        v
StaticMeshSourceAsset + LODGroup settings
        |
        v
StaticMeshBuilder -> MeshReductionInterface -> meshoptimizer
        |
        v
Multi-LOD MeshArtifact + ArtifactManifest
        |
        v
Mesh resource loading -> PrimitiveLODSet registration
        |
        v
Per-view SceneLODSystem -> visibility -> renderer
```

The editor importer and Cook never implement separate reduction paths. Both
resolve settings and call `StaticMeshBuilder`. Runtime code consumes only the
derived `MeshArtifact` and never links to or invokes mesh reduction.

## Authoring Data Model

`StaticMeshSourceAsset` is the serialized input to mesh building. It contains:

- `lodGroup`: selected preset name, defaulting to `None`.
- `minLOD`: the minimum allowed runtime LOD index.
- `autoComputeLODScreenSize`: whether non-overridden screen sizes are computed
  from bounds and pixel error.
- `sourceModels`: ordered source/build descriptions with LOD0 at index zero.

Each `StaticMeshSourceModel` contains:

- imported source geometry for LOD0, or an authored source/reference for an
  explicitly imported higher LOD;
- build settings shared with the normal mesh build path;
- reduction settings, including base LOD, target triangle percentage, and
  permitted error;
- screen-size value and an explicit-override flag;
- provenance: `Imported`, `Authored`, or `Generated`;
- per-field override information required to preserve user edits when a group
  preset is reapplied.

Generated source models describe how to build a level. They do not serialize a
second authoritative copy of generated vertex/index data into the source
asset.

## Derived Artifact Model

`MeshArtifact` remains the runtime-facing mesh artifact and becomes a versioned
multi-LOD container. It contains an ordered `lodResources` array. Each level
contains:

- vertex and index streams;
- render sections and their canonical material-slot mapping;
- local bounds;
- screen-size threshold;
- build metadata needed for validation and diagnostics.

Artifact-level metadata contains:

- artifact schema version;
- source content fingerprint;
- normalized StaticMesh build-settings fingerprint;
- LODGroup fingerprint;
- reducer identifier and version;
- importer and postprocessor versions;
- target platform identity.

Old single-mesh artifacts are read as a one-element `lodResources` array. A
subsequent editor build or Cook upgrades them to the current schema; no global
one-time migration is required.

## Core Invariants

- LOD0 must exist and pass geometry, index, section, and material validation.
- A mesh with only LOD0 is valid and renders normally.
- Screen-size thresholds are finite, normalized, and strictly descending.
- Authored LODs are never overwritten by automatic reduction.
- LOD0 defines the canonical material-slot contract. Every other LOD section
  must resolve to those slots by stable identity.
- A generated artifact is disposable and reproducible from source data,
  settings, tool versions, and target platform.
- A runtime primitive handle belongs to no more than one `PrimitiveLODSet`.
- Runtime selection never repairs invalid asset data or generates geometry.

## LODGroup Settings

`StaticMeshLODSettingsRegistry` owns engine defaults plus project overrides.
Project overrides are project-scoped settings and participate in build
fingerprints.

The initial built-in presets are:

| Group | NumLODs | Per-level ratio | PixelError | Notes |
| --- | ---: | ---: | ---: | --- |
| None | 1 | N/A | N/A | No automatic LOD generation |
| LevelArchitecture | 4 | 50% | 12 | UE4.26 default |
| SmallProp | 4 | 50% | 10 | UE4.26 default |
| LargeProp | 4 | 50% | 10 | UE4.26 default |
| Deco | 4 | 50% | 10 | UE4.26 default |
| Vista | 1 | N/A | N/A | UE4.26 default |
| Foliage | 1 | N/A | N/A | UE4.26 default |
| HighDetail | 6 | 50% | 6 | UE4.26 default |

The ratio is expanded cumulatively from LOD0. A 50% group therefore targets
approximately 50%, 25%, and 12.5% of the LOD0 triangles for LOD1, LOD2, and
LOD3. Each level is generated directly from LOD0 at its cumulative target so
that chained simplification does not accumulate avoidable error.

Applying a preset:

- adds generated source-model descriptions up to `NumLODs`;
- leaves authored levels unchanged;
- updates non-overridden generated reduction and screen-size settings;
- preserves fields marked as explicit per-level overrides;
- disables surplus generated levels when switching to a smaller group instead
  of deleting authored data.

New imports default to `None` and therefore only LOD0. Extra levels are created
only when the user selects a generating LODGroup, explicitly enables automatic
generation, or imports authored LODs.

## Mesh Reduction

`MeshReductionInterface` isolates the builder from the concrete reduction
library. The initial implementation uses the meshoptimizer dependency already
present in the repository.

Reduction requirements:

- simplify within material-section boundaries;
- preserve every vertex channel supported by the input mesh contract,
  including positions, normals, tangents, UVs, and vertex colors;
- preserve canonical material-slot mapping;
- remove degenerates and unreferenced vertices;
- optimize the final index/vertex order using the existing meshoptimizer
  facilities;
- produce deterministic output for the same normalized input and settings;
- return structured warnings when the requested target cannot be reached while
  retaining valid geometry;
- return an error for invalid LOD0 data, invalid indices, empty required
  geometry, or an unusable reduction result.

Failure to reach the exact requested triangle count is not itself fatal when a
valid best result is available. Corrupt authored higher LODs are fatal and are
not silently replaced with generated data.

## Import And Reimport

StaticMesh import options add `LODGroup`, `ImportMeshLODs`, and automatic
screen-size configuration.

Default import reads only LOD0 and uses `LODGroup=None`. When
`ImportMeshLODs` is enabled, the importer recognizes source-format LOD groups
and conventional `_LOD0`, `_LOD1`, and subsequent naming, then records those
levels as authored source models.

After parsing source data, the importer creates or updates
`StaticMeshSourceAsset`, applies the selected preset, and queues a
`StaticMeshBuilder` build. The importer does not contain a second reduction
implementation.

Reimport:

- preserves LODGroup selection, explicit per-level overrides, and independent
  authored LODs;
- replaces only imported source models that correspond to the reimported
  source;
- invalidates generated derivatives that depend on changed inputs;
- matches material slots by stable name/identity before falling back to order;
- reports an error if the canonical LOD0 material mapping cannot be restored.

## Build Triggers

An editor build is queued after:

- initial import or reimport;
- source-file content changes;
- LODGroup changes;
- reduction, screen-size, or mesh build-setting changes;
- adding, replacing, enabling, disabling, or deleting an authored LOD;
- importer, postprocessor, reducer, or artifact-schema version changes;
- an explicit `Rebuild LODs` command.

Changes may be debounced by the editor asset-build queue, but the completed
artifact is committed atomically. A cancelled or failed build leaves the last
valid artifact available and attaches diagnostics to the pending source state;
it never publishes a partially updated multi-LOD artifact.

## Cook Integration

Cook resolves target-platform settings and computes a build identity from:

- source content hash and source asset identity;
- normalized source-model and build settings;
- LODGroup definition fingerprint;
- importer and postprocessor versions;
- reducer identifier, version, and options;
- artifact schema version;
- target platform and platform overrides.

On a cache hit, Cook reuses the matching artifact. On a miss, it calls the same
`StaticMeshBuilder` used by editor builds, validates the result, writes it
atomically, and records the inputs in `ArtifactManifest` dependencies. Cook
packages only enabled LOD resources and runtime metadata.

Cook does not modify source assets, silently add LOD levels, or invoke any
runtime-only code. LOD0 failures, invalid indices, unrecoverable material-slot
mapping, or builder exceptions fail that asset's Cook with asset, sub-asset,
LOD, and stage diagnostics. A valid reduction that could not reach its exact
target emits a warning and may continue.

## Runtime Screen-Size Selection

`SceneLODSystem` uses a bounds sphere and the actual view projection to compute
screen size. Viewport dimensions, perspective FOV or orthographic projection,
and the view's LOD distance factor affect the result. The existing
`worldSize / distance` approximation is removed.

The ordered selection pipeline is:

1. Validate the registered set and its LOD0 resource.
2. Compute projection-aware bounds screen size for the current view.
3. Apply forced LOD, MinLOD, editor/runtime LOD show flags, quality limits, and
   platform limits.
4. Select against strictly descending screen-size thresholds.
5. Clamp or fall back to an actually resident valid level.
6. Apply per-primitive, per-view hysteresis and temporal transition state.
7. Return the selected LOD mask to visibility submission.

Forced LOD indices are clamped to a valid configured range. MinLOD prevents
selection of a more detailed index than permitted. A missing selected resource
uses the nearest valid resident level, preferring a coarser resident level when
both directions are possible, consistent with streaming pressure. If no
coarser level is resident, the nearest finer resident level is used. LOD0 is
the final fallback whenever it is resident and valid.

The current implementation may load all levels together, so normal assets will
have all enabled levels resident. The residency API still exists so later
streaming work does not have to redefine selection behavior.

## Hysteresis And Transitions

History is keyed by stable primitive identity and stable view identity. The
renderer must not use a constant view key. Crossing a threshold does not switch
until the configured hysteresis band is exceeded, preventing oscillation near
the boundary.

When dithered transitions are supported, selection returns previous LOD,
current LOD, and transition alpha so both levels can be submitted during the
transition interval. Materials or passes that do not support the transition
contract use a stable hard switch. The existing `fadeDurationSeconds` setting
must either drive this state or be removed; it must not remain an unused public
option.

History is invalidated when relevant bounds, thresholds, forced/minimum LOD,
resource identity, or residency changes. Destroyed views and primitives remove
their history entries.

## RenderScene Lifecycle

The scene-side concept currently called an LOD group is renamed
`PrimitiveLODSet` to avoid confusion with asset LODGroup presets.

StaticMeshComponent/MeshRenderer render-state creation registers one
`PrimitiveLODSet` containing the primitive handles for every built LOD. A
single-LOD mesh registers a one-element set and follows the same path.

The production lifecycle must support:

- register on render-state creation;
- atomic update after mesh hot reload, bounds changes, LOD setting changes, or
  residency updates;
- unregister all level handles when a component is disabled, destroyed,
  removed from the scene, or assigned a different mesh;
- reject empty sets, duplicate handles, overlapping membership, invalid LOD0,
  and incoherent thresholds;
- invalidate selection history for any update that changes selection inputs.

Spatial visibility runs before LOD selection. Only candidate sets execute the
selector, after which inactive LOD handles are suppressed. This avoids creating
history or transition work for primitives that were already culled.

## Thumbnail Behavior

Asset-browser StaticMesh thumbnails use formal built `MeshArtifact` LOD data.
The thumbnail view supplies its real viewport and projection to the same
screen-size selector used by scene rendering. With identical asset data,
camera, and thumbnail dimensions, selection must be deterministic.

If an asset has only LOD0, the thumbnail uses LOD0. If the selected level is
not resident, normal runtime fallback rules apply.

The existing thumbnail-only simplification and sampling path is removed,
including `LoadMeshArtifactPreviewSample`,
`SimplifyMeshArtifactForPreview`, their scheduling branches, caches, and
fallback behavior. Asset-panel preview does not generate a temporary mesh, add
a formal LOD, mutate source data, or write a preview proxy to Cook output.

Thumbnail cache identity includes the multi-LOD artifact fingerprint,
thresholds, bounds, materials, render environment, viewport/configuration, and
thumbnail renderer version. LOD or reduction setting changes therefore
invalidate the cached result.

The StaticMesh editor preview may expose `Auto` and explicit LOD inspection
modes. That inspection control does not change the asset-browser thumbnail
selection contract.

## Diagnostics And Telemetry

Build diagnostics include source asset, sub-asset, LOD index, build stage,
severity, and a stable diagnostic code. They distinguish invalid source data,
invalid authored LOD data, reduction limitations, material mapping failures,
and artifact publication failures.

Renderer telemetry records selected primitive counts per LOD, residency
fallback count, transition count, invalid-set rejection count, and stale
history cleanup. A debug visualization can display selected LOD, computed
screen size, active threshold, and the forced/minimum/fallback reason.

Production builds do not emit per-frame LOD logs.

## Test Strategy

Implementation follows test-driven development. Focused tests are added before
each behavior change.

### Mesh Reduction Tests

- target triangle ratios and best-effort warnings;
- section and canonical material-slot preservation;
- vertex-channel preservation;
- deterministic geometry and artifact hashes;
- degenerate geometry, empty sections, invalid indices, and unreachable
  targets.

### LODGroup Tests

- exact UE4.26 built-in default values;
- project override fingerprinting;
- generated source-model expansion;
- explicit per-level override preservation;
- authored LOD preservation across preset changes;
- disabling surplus generated levels without deleting authored data.

### Builder And Serialization Tests

- LOD0-only artifacts;
- mixed authored and generated levels;
- automatic and explicit screen-size values;
- invalid or non-descending thresholds;
- invalid LOD0 and invalid authored higher LOD behavior;
- old single-mesh artifact compatibility and rebuild upgrade.

### Import And Reimport Tests

- default import produces only LOD0;
- source LOD naming/group recognition when enabled;
- reimport preserves group and overrides;
- stable material-slot mapping;
- generated dependent levels are invalidated when their base changes.

### Cook Tests

- cache hit and every required invalidation input;
- target-platform identity separation;
- editor and Cook builders produce equivalent normalized artifacts;
- atomic output and structured failures;
- Cook does not mutate source assets;
- packaged runtime does not link or invoke mesh reduction.

### Runtime Selection Tests

- perspective FOV, orthographic projection, viewport, bounds, and distance
  effects;
- threshold boundary behavior;
- forced LOD, MinLOD, quality/platform limits, and show flags;
- missing-level and residency fallback;
- hysteresis and transition timing;
- independent histories for multiple views;
- update, unregister, view destruction, and duplicate-membership rejection.

### Thumbnail Tests

- thumbnail uses formal LOD resources and standard screen-size selection;
- deterministic selection for fixed thumbnail view inputs;
- LOD changes invalidate cache identity;
- no thumbnail path calls or contains preview mesh simplification;
- LOD0-only assets still render thumbnails normally.

## End-To-End Acceptance

The maintained integration path must demonstrate:

```text
Import LOD0
-> select a generating LODGroup
-> build reduced levels
-> save and reopen
-> register in a scene
-> select levels across camera/view changes
-> verify independent multi-view behavior
-> render an asset-browser thumbnail from formal LOD data
-> Cook for a target platform
-> load the Cooked artifact
-> reproduce the expected runtime selection behavior
```

The implementation is accepted when:

- a valid asset with no extra LOD data renders LOD0 everywhere;
- automatic and authored LODs survive save/reopen and Cook;
- editor build and Cook use one deterministic builder and cache identity;
- runtime selection is projection-aware and respects every configured
  constraint;
- register, update, removal, and history lifecycle leave no stale handles;
- thumbnails select an appropriate formal LOD and never generate a simplified
  preview mesh;
- focused and maintained unit/integration suites pass;
- a packaged runtime validation confirms that no reduction path is required at
  runtime.
