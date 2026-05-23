# Feature Specification: Asset Management System

**Feature Branch**: `026-asset-management-system`
**Created**: 2026-05-13
**Status**: Draft
**Input**: User description: "Design and implement a complete engine-level asset management system for Nullus, using the provided local reference source tree for workflow semantics. Support external asset import, stable GUID-backed references, source scanning, meta files, imported artifacts, dependencies, runtime resolution, build manifests, hot reload, conversion of glTF/GLB/FBX/OBJ into Nullus engine assets, and a complete Prefab implementation."

## Pipeline Scope

The asset pipeline covers the full path from user-visible source files to runtime-loadable artifacts:

1. Mount editable project asset roots and read-only engine/package asset roots.
2. Scan source files, create or preserve sidecar `.meta` identity, and index by GUID and normalized path.
3. Select importers from file type, `.meta` overrides, and importer registry capabilities.
4. Copy external files into project assets by default, including related dependencies such as MTL files, external glTF buffers, and external images.
5. Convert source files into typed intermediate records, especially `ImportedScene` for glTF/GLB/FBX/OBJ.
6. Generate deterministic Nullus engine assets: textures, materials, meshes, models, skeletons, animation clips, morph targets, prefabs, scenes, shaders, and audio artifacts.
7. Persist imported artifact records, dependency records, diagnostics, and asset version hashes in the project cache.
8. Detect stale assets from source content, import settings, dependency, importer, target platform, or postprocessor changes.
9. Commit successful imports atomically so failed imports keep the previous successful artifact resolvable.
10. Build runtime manifests from selected scenes or prefabs and include only reachable converted artifacts.
11. Resolve assets at runtime by GUID/sub-asset ID and manifest records, not by editor source files or absolute project paths.
12. Hot reload loaded editor/runtime resources when artifacts change or mark them for explicit reload when replacement is unsafe.
13. Support bidirectional editor drag/drop between the Asset Browser and Hierarchy tree for instantiation, material assignment, prefab creation, and variant creation.
14. Run imports asynchronously so the editor remains responsive, shows per-asset and batch progress, supports cancellation, and commits completed artifacts atomically.
15. Convert imported materials into Nullus runtime material artifacts that render with correct PBR/legacy visual intent in editor previews and scene viewports.

## Editor Compatibility Scope

Nullus asset import and Prefab workflows MUST align with the provided reference editor behavior at the workflow/API-semantics level while remaining a native Nullus implementation. The reference source tree is material for concepts, state models, and editor workflows; Nullus must not copy implementation code.

Compatibility covers these reference editor concepts:

- Asset database facade: path/GUID lookup, asset creation/deletion/move/copy/rename, folder operations, import/refresh batching, dependency queries, labels, asset pack names/variants, main asset and sub-asset loading, and deterministic search/filter behavior.
- Asset importer facade: importer lookup by path, serialized import settings, importer versioning, `SaveAndReimport` behavior, external object remaps, user data, asset pack settings, and import diagnostics.
- `ModelImporter`: model scale, hierarchy import, mesh/normals/tangents/UV/material extraction, animation clips, rig/skeleton/avatar policy where supported by Nullus, material search/extraction policy, and unsupported-feature diagnostics.
- `TextureImporter`: texture type, sRGB/linear policy, alpha policy, mipmaps, compression intent, wrap/filter/sampler settings, and platform override records.
- Asset postprocessor and scripted importer equivalents: pre/post import hooks, dependency declarations, importer version invalidation, ordered callback execution, and diagnostics.
- Prefab utility and prefab importer semantics: regular prefab, model prefab, prefab variant, nested prefab, missing prefab, Prefab Stage, granular overrides, apply/revert operations, unpacking, and instance status/type queries.
- Build packaging: asset pack concepts are mapped to Nullus runtime manifests and artifact packs, including dependency closure, pack names/variants, content hashes, and source-file exclusion.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Asset Identity (Priority: P1)

An editor user can add, move, rename, or rescan files under project assets without breaking scene, prefab, or material references, because every source asset receives a stable GUID-backed identity.

**Why this priority**: Stable identity is the foundation for import, hot reload, build manifests, prefab instances, and object graph `$asset` references.

**Independent Test**: Create assets with and without `.meta` files, scan the asset root, verify GUID creation, GUID reuse after rescan, and lookup by GUID/path.

**Acceptance Scenarios**:

1. **Given** a source asset with no `.meta`, **When** the asset root is scanned, **Then** a sidecar `.meta` is created with a valid GUID and importer metadata.
2. **Given** a source asset with an existing valid `.meta`, **When** the asset root is scanned again, **Then** the same GUID remains associated with the asset.
3. **Given** a source asset is renamed with its `.meta`, **When** the asset root is scanned, **Then** references by GUID can resolve the new path.

---

### User Story 2 - External Model Import (Priority: P2)

An editor user can import `.gltf`, `.glb`, `.fbx`, and `.obj` files from outside the project and have them converted into engine assets rather than loaded directly from source files.

**Why this priority**: External asset import is the primary user-facing value of the new system.

**Independent Test**: Import representative glTF/GLB/FBX/OBJ fixtures and verify generated model, mesh, material, texture, prefab, skeleton, animation, and morph target artifacts where supported by the source format.

**Acceptance Scenarios**:

1. **Given** a glTF 2.0 asset with meshes, PBR materials, textures, skeleton, animations, and morph targets, **When** it is imported, **Then** Nullus assets are generated for each supported source object with stable sub-asset IDs.
2. **Given** an OBJ plus MTL and textures, **When** it is imported, **Then** static mesh, material, texture, model, and generated prefab assets are created and source dependencies are recorded.
3. **Given** an FBX model with animation data, **When** it is imported, **Then** available scene, mesh, material, texture, skeleton, skin, animation, morph, model, and prefab data are converted through the common scene import representation.

---

### User Story 3 - Prefab Authoring And Instancing (Priority: P2)

An editor user can create a prefab from a GameObject hierarchy, instantiate it into scenes, override instance properties, and apply or revert changes without losing stable asset references.

**Why this priority**: Imported models need generated prefabs, and user-authored prefabs are the natural unit for scenes, builds, and reusable content.

**Independent Test**: Serialize a prefab with nested GameObjects, mesh/material asset references, a base prefab reference, and overrides; reload it; instantiate it into a scene; verify source-to-instance mapping and override behavior.

**Acceptance Scenarios**:

1. **Given** a GameObject hierarchy with reflected components and `$asset` references, **When** the user saves it as a prefab, **Then** a `.prefab` source asset is written using `Nullus.ObjectGraph.Prefab`, receives a `.meta` GUID, and imports into a runtime prefab artifact.
2. **Given** a model import produces a scene hierarchy, **When** import succeeds, **Then** the importer generates a read-only model prefab sub-asset that references generated mesh, material, texture, skeleton, animation, and morph artifacts by stable asset IDs.
3. **Given** a scene contains a prefab instance, **When** the base prefab changes and the scene is refreshed, **Then** unchanged instance data follows the new base while local override patches remain attached to stable object IDs.
4. **Given** a prefab variant references a base prefab, **When** the variant is imported or instantiated, **Then** base prefab cycles are rejected and overrides are validated before runtime use.
5. **Given** a prefab instance has local property, component, or child hierarchy changes, **When** the editor shows overrides, **Then** each override can be applied to the source prefab or reverted from the instance without changing unrelated overrides.
6. **Given** a user opens a prefab for editing, **When** the prefab is edited in isolation and saved or discarded, **Then** scene instances remain reference-based and refresh only after a saved prefab import commits.
7. **Given** a generated model prefab is selected, **When** the user attempts to customize it, **Then** the editor offers create-variant and unpack actions instead of editing the generated artifact in place.

---

### User Story 4 - Incremental Reimport And Hot Reload (Priority: P3)

An editor user can modify source files, import settings, prefab overrides, or dependent textures and see affected assets reimported without restarting the editor.

**Why this priority**: Editing assets is iterative; the system must avoid full-project rebuilds for small changes.

**Independent Test**: Modify a dependent texture, prefab base, or model setting, run refresh, and verify only affected assets become stale and reload.

**Acceptance Scenarios**:

1. **Given** a model depends on external images, **When** an image changes, **Then** the dependent model import is marked stale and scheduled for reimport.
2. **Given** a prefab depends on mesh/material assets, **When** one dependency changes, **Then** only the prefab and assets that depend on that dependency are marked stale.
3. **Given** a source asset is reimported while loaded in the editor, **When** the new artifact commits successfully, **Then** loaded runtime resources are refreshed or marked for explicit reload.

---

### User Story 5 - Runtime Resolution And Builds (Priority: P4)

A game build can load only converted runtime assets through a manifest, without depending on editor source files or absolute project paths.

**Why this priority**: Build reproducibility and runtime startup require a clear separation between source assets and runtime artifacts.

**Independent Test**: Build a manifest from a start scene, verify dependency closure, then resolve assets and prefab instances by GUID from the runtime artifact store.

**Acceptance Scenarios**:

1. **Given** a start scene with asset and prefab references, **When** a build manifest is generated, **Then** all reachable artifacts and dependencies are included exactly once.
2. **Given** the game runs from a build output directory, **When** it resolves an asset reference, **Then** it loads from runtime artifacts and not project source files.
3. **Given** a runtime scene instantiates a prefab, **When** the prefab artifact is resolved, **Then** its object graph and referenced runtime artifacts are loaded through manifest entries.

---

### User Story 6 - Editor Drag/Drop, Import Feedback, And Material Preview (Priority: P2)

An editor user can drag assets into the Hierarchy to instantiate or assign them, drag Hierarchy objects back into the Asset Browser to create prefabs or variants, monitor long imports without the editor freezing, and see imported materials render correctly after import.

**Why this priority**: The pipeline must feel usable in the editor, not just pass offline import tests. Large models and texture sets are common, and users need visible progress plus trustworthy visual results.

**Independent Test**: Simulate drag/drop commands and long-running imports, verify scene/prefab changes, progress events, cancellation behavior, material artifact data, and viewport/render evidence without relying on a blocking UI thread.

**Acceptance Scenarios**:

1. **Given** a prefab or generated model prefab in the Asset Browser, **When** the user drags it into the Hierarchy, **Then** the editor creates a connected prefab instance at the chosen hierarchy target and preserves asset references and source-to-instance mappings.
2. **Given** a material asset in the Asset Browser and a Hierarchy object with a renderer, **When** the user drops the material onto that object, **Then** the renderer material slot is assigned through a GUID-backed asset reference and the scene becomes dirty.
3. **Given** a texture asset in the Asset Browser and a Hierarchy object with a renderer, **When** the user drops the texture onto that object, **Then** the editor offers or performs a deterministic create-material-and-assign action according to editor policy.
4. **Given** a Hierarchy `GameObject`, prefab instance, or model prefab instance, **When** the user drags it into an Asset Browser folder, **Then** the editor creates a new prefab source, prefab variant, or unpacked prefab copy according to the source object type and user command.
5. **Given** a large glTF/GLB/FBX import is running, **When** the importer parses files, writes artifacts, and commits results, **Then** the editor remains responsive and the user sees per-phase progress, current asset name, diagnostics, and cancellation state.
6. **Given** an import is cancelled or fails, **When** the import job ends, **Then** partial artifacts are discarded, the previous successful artifact remains active, and diagnostics explain whether cancellation or failure occurred.
7. **Given** a glTF PBR, FBX material, or OBJ MTL fixture imports successfully, **When** the generated prefab is shown in the viewport, **Then** base color, metallic/roughness or specular fallback, normal map, emissive map, alpha mode, UV transform, sampler, and color-space behavior match the supported Nullus material model or emit explicit diagnostics.

### Edge Cases

- Duplicate GUIDs in two `.meta` files are reported, and editable duplicate assets receive repaired GUIDs without silently aliasing two files.
- Missing source dependencies are reported as import diagnostics and retained in the asset database for user inspection.
- Importers must reject source paths that escape the project or engine asset roots after normalization.
- External import must handle filename conflicts by preserving the user's chosen destination and creating deterministic conflict suffixes or diagnostics.
- An interrupted import must not leave the database pointing at partial artifacts.
- Engine and package assets are treated as read-only unless explicitly copied into project assets.
- Imported sub-assets must keep stable IDs when source object order changes but names or source-local identifiers remain stable.
- Duplicate source-local names must be disambiguated deterministically and reported when the fallback could make IDs less stable.
- Unsupported glTF extensions, FBX data channels, OBJ features, or material models must produce diagnostics instead of silently dropping content.
- Prefab cycles, missing base prefabs, dangling object references, duplicate object IDs, invalid override targets, and orphaned owned objects must be rejected or preserved with editor diagnostics according to load policy.
- Runtime builds must fail if a required artifact has no successful import for the selected target platform.
- Drag/drop must reject invalid source/target combinations with diagnostics rather than silently mutating scenes or assets.
- Dragging generated model prefab content back to assets must create an editable variant or unpacked copy, never overwrite the generated import artifact.
- Progress UI must not report completed imports until artifact commit succeeds.
- Cancelling an import while a dependency is being copied or an artifact is being staged must leave source metadata and committed artifacts consistent.
- Imported material conversion must avoid color-space double conversion and must report unsupported shader/material channels.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST assign every source asset a stable GUID stored in a sidecar `.meta` file.
- **FR-002**: The system MUST preserve existing supported `.meta` settings while adding new asset identity and importer fields.
- **FR-003**: The system MUST scan project, engine, and package asset roots, skip `.meta` files as source assets, and index assets by GUID and normalized path.
- **FR-004**: The system MUST distinguish editable project assets from read-only engine and package assets.
- **FR-005**: The system MUST store importer ID, importer version, import settings hash, target platform, dependency hash, and artifact hash information for each imported asset.
- **FR-006**: The system MUST track dependencies on source files, source asset GUIDs, imported assets, build target, importer version, postprocessor version, path-to-GUID mappings, prefab base assets, and prefab override targets.
- **FR-007**: The system MUST provide a resolver that reports asset state as up-to-date, missing, importing, failed, or needing import.
- **FR-008**: The system MUST convert glTF/GLB/FBX/OBJ source files through a common imported-scene representation before generating Nullus runtime assets.
- **FR-009**: The glTF importer MUST support glTF 2.0 static meshes, vertex attributes, node hierarchy, transforms, PBR metallic-roughness materials, external and embedded buffers/images, skeletons, skins, animation clips, and morph targets.
- **FR-010**: The FBX importer MUST support all source data exposed by the selected parser and report diagnostics for unsupported material, animation, skeleton, skin, or morph data.
- **FR-011**: The OBJ importer MUST support static mesh, MTL material, and texture conversion while clearly reporting that OBJ has no native skeleton, skin, animation, or morph support.
- **FR-012**: The system MUST generate stable sub-asset IDs for model-derived mesh, material, texture, skeleton, skin, animation clip, morph target, model, and prefab assets.
- **FR-013**: The system MUST persist import diagnostics so the editor can display the latest import status after restart.
- **FR-014**: The editor MUST support external import by copying selected files and their related dependencies into project assets by default.
- **FR-015**: The system MUST support asynchronous import requests with progress, cancellation, and atomic commit of successful artifacts.
- **FR-016**: The runtime MUST resolve assets by GUID/artifact manifest and avoid requiring editor-only source files.
- **FR-017**: The build pipeline MUST generate a manifest containing reachable runtime artifacts and dependency closure from selected scenes or prefabs.
- **FR-018**: Existing path-based `AResourceManager` consumers MUST continue to work during migration, while new serialization prefers GUID-backed asset references.
- **FR-019**: The system MUST classify `.prefab` files as Prefab source assets and import them through the maintained object graph serialization path.
- **FR-020**: Prefab source assets MUST use `Runtime/Engine/Serialize` object graph documents with `Nullus.ObjectGraph.Prefab` format, reflected object fields, `$owned`, `$ref`, `$asset`, and patch operations.
- **FR-021**: Prefab instances MUST preserve source-to-instance object ID mappings so overrides remain stable across base prefab reimports.
- **FR-022**: Prefab variants MUST reference their base prefab by `AssetId` and MUST validate base chains, cycles, missing bases, and override targets before runtime use.
- **FR-023**: Model imports MUST generate an internal prefab artifact/sub-asset that recreates the imported scene hierarchy using Nullus GameObject/component records and asset references to generated artifacts.
- **FR-024**: Imported model prefabs MUST use existing `TransformComponent`, `MeshFilter`, and `MeshRenderer` records for static renderables and MUST define required runtime component gaps for skinned animation and morph playback before those features are marked complete.
- **FR-025**: The artifact cache MUST retain the latest successful artifact version until a replacement import commits successfully.
- **FR-026**: The artifact writer MUST emit a machine-readable artifact manifest for each import result listing primary asset, sub-assets, dependencies, diagnostics, content hashes, and runtime artifact paths.
- **FR-027**: The runtime manifest MUST map asset GUIDs and sub-asset IDs to artifact files, artifact type, loader ID, content hash, dependency list, and schema version.
- **FR-028**: Asset references in scenes, prefabs, materials, and generated artifacts MUST store GUID/sub-asset IDs plus optional type and path hints for editor recovery.
- **FR-029**: The editor asset browser MUST surface source asset state, import state, diagnostics, and reimport actions without showing generated model prefabs as separate virtual `.prefab` files; the model source file is the user-visible drag/drop entry for instantiating the generated prefab.
- **FR-030**: Build output MUST exclude source-only files unless explicitly marked for raw packaging.
- **FR-031**: The editor MUST support creating a prefab source asset from a selected `GameObject` hierarchy, including all owned children, components, reflected fields, and asset references.
- **FR-032**: The editor MUST support instantiating prefab assets into scenes as prefab instances that store an asset reference plus local override patches, not a detached full copy.
- **FR-033**: The editor MUST present prefab instance overrides as a deterministic list of property, component, and owned-object changes that can be applied or reverted independently.
- **FR-034**: The editor MUST support a prefab editing stage where a prefab can be opened, edited, saved, or discarded without mutating scene instances until the prefab asset is saved and reimported.
- **FR-035**: The editor MUST support nested prefab references while rejecting nested cycles and preserving nested instance override chains.
- **FR-036**: The editor MUST support prefab variants that reference a base prefab asset and store only override patches relative to that base.
- **FR-037**: Generated model prefabs MUST be read-only in the editor; customization MUST happen through editable variants or unpacked scene objects.
- **FR-038**: The editor MUST preserve recoverable missing prefab bases, missing nested prefab references, and unresolved asset references with diagnostics, while runtime loading MUST fail for invalid required data.
- **FR-039**: Applying prefab changes MUST mark dependent variants, scenes, build manifests, and loaded instances stale through the dependency graph.
- **FR-040**: Unpacking a prefab instance MUST detach it from the source prefab while preserving the current resolved object hierarchy and asset references.
- **FR-041**: The editor MUST expose an `AssetDatabaseFacade` API for GUID/path lookup, asset creation, deletion, copy, move, rename, folder creation, refresh, import, dependency queries, labels, and main/sub-asset lookup.
- **FR-042**: `AssetDatabaseFacade` path operations MUST update or preserve `.meta` GUID identity consistently across asset moves, copies, and renames.
- **FR-043**: `AssetDatabaseFacade` refresh MUST support batch editing semantics through `StartAssetEditing`/`StopAssetEditing`, delaying imports until the batch closes.
- **FR-044**: `AssetDatabase` search MUST support type, label, name, and folder filters with deterministic results.
- **FR-045**: Assets MUST have a main object and zero or more sub-assets, and loading APIs MUST distinguish main asset, all assets, and representation/sub-assets.
- **FR-045A**: Source assets with the same basename but different formats, such as `Sponza.gltf` and `Sponza.fbx`, MUST coexist as separate assets with distinct `.meta` GUIDs, distinct GUID-keyed artifact roots, and no stem-based artifact overwrite risk. Sub-asset keys may share names across those sources only because runtime/editor resolution always includes the source asset GUID.
- **FR-046**: The system MUST support asset labels and bundle name/variant metadata for editor search and build packaging.
- **FR-047**: Importers MUST expose importer settings lookup by path, serialized settings mutation, dirty state, and `SaveAndReimport`.
- **FR-048**: Importers MUST support external object remap records so materials, textures, avatars, or other external references can override importer-generated defaults.
- **FR-049**: Importers MUST support user data, importer version, target platform settings, and dependency declarations as part of the asset version key.
- **FR-050**: Import diagnostics MUST preserve enough context to display import warnings/errors for source asset, sub-asset, dependency, importer setting, and target platform failures.
- **FR-051**: Model import MUST expose model-specific settings for scale, axis/unit conversion, hierarchy policy, mesh optimization, normals/tangents, UVs, materials, rig/skeleton, animation clips, cameras/lights policy, and unsupported capability diagnostics.
- **FR-052**: Texture import MUST expose texture-specific settings for texture type, sRGB, alpha, mipmaps, compression intent, sampler/wrap/filter, max size, and platform overrides.
- **FR-053**: Import postprocessors MUST run in deterministic order around import operations and MUST be able to declare additional dependencies or diagnostics.
- **FR-054**: Scripted/custom importer registration MUST allow new source extensions to produce artifacts, dependencies, diagnostics, and sub-assets without changing core asset database code.
- **FR-055**: Prefab APIs MUST expose prefab asset type queries: not prefab, regular prefab, model prefab, prefab variant, missing asset, and generated read-only model prefab.
- **FR-056**: Prefab APIs MUST expose prefab instance status queries: not prefab, connected, disconnected/unpacked, missing asset, and invalid/corrupt.
- **FR-057**: Prefab APIs MUST support save operations: save as prefab asset, save and connect scene object to prefab, load prefab contents for isolated editing, save prefab contents, and unload prefab contents.
- **FR-058**: Prefab APIs MUST support granular override operations for property overrides, added/removed components, added/removed child objects, object removal, and nested prefab overrides.
- **FR-059**: Prefab APIs MUST support apply/revert at three levels: single override, selected object/component override group, and whole prefab instance.
- **FR-060**: Prefab APIs MUST support two unpack modes: preserving nested prefab links or completely unpacking nested prefab links.
- **FR-061**: Model prefabs MUST be read-only generated assets; customizations MUST be stored in variants, scene overrides, or unpacked objects.
- **FR-062**: Prefab variants MUST preserve base prefab links and store only override deltas relative to the nearest base layer, including generated model prefab bases.
- **FR-063**: Nested prefabs MUST preserve independent prefab asset references and override chains, with cycle detection and missing-asset diagnostics.
- **FR-064**: Prefab Stage MUST support isolated editing, dirty state, save/discard, nested context, generated-read-only rejection, and dependency refresh after commit.
- **FR-065**: Prefab modifications MUST be queryable as deterministic records covering property modifications, object overrides, added components, removed components, and added child objects.
- **FR-066**: Asset import and prefab operations MUST participate in undo/redo or an equivalent editor command history when invoked through editor UI.
- **FR-067**: Build packaging MUST map asset pack name/variant metadata to Nullus artifact packs/runtime manifests, preserving dependency closure and content hashes.
- **FR-068**: Runtime builds MUST support loading packaged assets by manifest entry and MUST reject editor-only source/import APIs.
- **FR-069**: Editor compatibility tests MUST use representative fixture assets and expected state transitions instead of relying on reference implementation internals.
- **FR-070**: Any reference behavior intentionally not implemented MUST be listed in a compatibility matrix with a Nullus alternative, diagnostic, or explicit out-of-scope decision.
- **FR-071**: The editor MUST support dragging prefab, generated model prefab, model, material, texture, scene, and folder assets from the Asset Browser onto valid Hierarchy targets with deterministic action resolution.
- **FR-072**: Dropping prefab or generated model prefab assets onto the Hierarchy MUST instantiate connected prefab instances and preserve source-to-instance object ID mappings.
- **FR-073**: Dropping material assets onto Hierarchy renderer targets MUST assign material slots through GUID-backed asset references and participate in undo/redo or equivalent editor command history.
- **FR-074**: Dropping texture assets onto Hierarchy renderer targets MUST create or update material assets according to a deterministic editor policy and report diagnostics when no valid assignment exists.
- **FR-075**: Dragging Hierarchy objects into the Asset Browser MUST support creating prefab sources, creating prefab variants from connected prefab/model prefab instances, and deterministic destination conflict handling.
- **FR-076**: Import jobs MUST run without blocking the editor UI thread and MUST expose progress phases for dependency copy, source parse, conversion, artifact write, postprocess, and commit.
- **FR-077**: Import progress MUST expose batch progress, per-asset progress, current phase, diagnostics, cancellation state, and final success/failure state to editor panels.
- **FR-078**: Import cancellation MUST be cooperative and MUST discard staged artifacts while preserving the latest successful committed artifact.
- **FR-079**: The editor MUST surface import progress and diagnostics in the Asset Browser or equivalent status surface while allowing the user to keep navigating scenes and assets.
- **FR-080**: Imported material conversion MUST map glTF PBR metallic-roughness, OBJ MTL, and parser-exposed FBX material channels to Nullus material artifacts with deterministic texture slots and fallback values.
- **FR-081**: Imported texture/material conversion MUST preserve sRGB/linear intent, normal map interpretation, alpha mode, emissive contribution, UV transform, wrap/filter/sampler state, and unsupported-channel diagnostics.
- **FR-082**: Generated model prefabs MUST reference converted material artifacts so editor preview, scene viewport, runtime manifest, and build output use the same material data.
- **FR-083**: Asset Browser to Scene View/Hierarchy model and prefab drags MUST publish imported asset handles containing GUID, source path, prefab sub-asset key, artifact type, and import readiness; Scene View and Hierarchy drops MUST prefer those handles over raw file paths.
- **FR-084**: Dropping a model or prefab asset handle MUST NOT synchronously import source files, parse source model formats, refresh the whole asset database, or decode mesh/material payloads on the editor UI thread. Missing, stale, or importing artifacts MUST return a pending/diagnostic state and let the asynchronous import/cache pipeline complete the asset.
- **FR-085**: A warm generated model prefab drop MUST instantiate from already committed imported artifacts and defer renderer resource realization through the bounded editor resource queue; it MUST NOT schedule one blocking source parse or one synchronous mesh decode per sub-asset during the drop callback.
- **FR-086**: The editor MUST schedule background preimport for model-scene and prefab source assets during Project Browser startup, project asset file changes, and copy/move/import into project `Assets`, so drag/drop normally receives warm imported handles before the user drops.
- **FR-087**: File-watcher-driven preimport MUST use changed asset paths when available and MUST avoid force-reimporting every model/prefab asset for unrelated changes.
- **FR-088**: Prefab source imports MUST persist their artifact manifest and runtime prefab payload under the project `Library` cache so a new asset database refresh can still classify the prefab as warm/imported.
- **FR-089**: `.hlsl` shader source assets MUST import through `AssetDatabaseFacade` into GUID-keyed `Library/Artifacts` shader artifacts and `Library/ArtifactDB/index.tsv`, with source/meta/importer/build-target dependencies recorded like model and prefab imports.
- **FR-090**: Shader import artifacts MUST persist stage, target platform, entry point, profile, cache key, compiled bytecode artifact path, reflection diagnostic status, and compiler diagnostics so editor startup can inspect shader readiness without invoking runtime source compilation.
- **FR-091**: Editor startup, file-watcher changes, and copy/move/import into project `Assets` MUST preimport shader source assets along with model and prefab assets; unchanged shader artifacts MUST be skipped by manifest freshness checks.
- **FR-092**: Materials SHOULD reference shader assets through GUID/sub-asset artifact handles where available, while legacy path-based `:Shaders/*.hlsl` references remain a compatibility fallback during migration.
- **FR-093**: `ShaderLoader` MUST be able to load committed shader artifact payloads directly and only fall back to source compilation when given a source `.hlsl` path or when an artifact explicitly lacks a usable compiled variant.

### Import Capability Matrix

| Source Format | Required Import Results | Required Diagnostics |
|---------------|-------------------------|----------------------|
| `.gltf` / `.glb` | Scene hierarchy, transforms, static meshes, vertex attributes, PBR materials, texture references, embedded/external buffers and images, skeletons, skins, animations, morph targets, generated prefab | Unsupported extensions, invalid accessors, missing buffers/images, unsupported texture encodings, invalid skins, invalid animation targets, duplicate unstable names |
| `.fbx` | Parser-exposed hierarchy, meshes, materials, textures, skeletons, skins, animations, morph targets, generated prefab | Any parser data not mapped to Nullus, unsupported material channels, unsupported animation curves, missing texture files |
| `.obj` | Static meshes, groups/material slots, MTL materials, textures, generated prefab | Missing MTL/textures, unsupported free-form surfaces, no skeleton/skin/animation/morph support |
| `.prefab` | Object graph validation, base prefab dependency, override patch validation, runtime prefab artifact | Missing base prefab, cycles, invalid override target, duplicate object ID, orphaned object, unresolved asset reference |

### Material Conversion Requirements

- glTF materials MUST map base color, metallic factor, roughness factor, base color texture, metallic-roughness texture, normal texture, occlusion texture, emissive texture, emissive factor, alpha mode, alpha cutoff, double-sided flag, UV set, and sampler state into Nullus material records where supported.
- FBX materials MUST map parser-exposed diffuse/base color, specular/glossiness or roughness, normal/bump, emissive, opacity, texture paths, and UV channels into Nullus material records or diagnostics.
- OBJ MTL materials MUST map diffuse color/texture, specular color, shininess, opacity/dissolve, normal/bump map, and illumination hints into Nullus material records or diagnostics.
- Texture color-space policy MUST be deterministic: color maps use sRGB by default, data maps such as normal/metallic/roughness/occlusion use linear by default, and importer settings can override the policy.
- Generated material artifacts MUST be the source of truth for editor preview, scene viewport, runtime manifest, and build output.

### Runtime Playback Component Requirements

- Static imported renderables may instantiate with the existing `TransformComponent`, `MeshFilter`, and `MeshRenderer` records.
- Skinned mesh playback is not complete until the runtime provides an instantiable reflected component that can bind a mesh artifact, a skin artifact, skeleton/joint matrices, inverse bind poses, and per-frame bone palette updates to the renderer.
- Animation playback is not complete until the runtime provides an instantiable reflected component or animation graph record that can reference animation clip artifacts, evaluate channels over time, target skeleton joints or object transforms by stable IDs, and expose deterministic play/stop/seek state for serialization.
- Morph target playback is not complete until the runtime provides an instantiable reflected component that can reference morph target artifacts, preserve named weights, combine multiple active targets, and pass deformed vertex data or morph weights to the renderer.
- Generated model prefabs MUST continue to emit explicit capability diagnostics for skins, skeletons, animations, or morph targets until all required runtime components above exist and are registered in reflection.
- When the playback components exist, generated model prefabs MUST replace the capability diagnostics with component records that reference the generated skeleton, skin, animation clip, and morph target artifacts by GUID/sub-asset ID.
- Runtime manifests MUST include skeleton, skin, animation clip, and morph target artifacts whenever generated prefab playback components reference them.

### Prefab Implementation Requirements

- **PF-001**: A Prefab is an asset-backed object graph whose root is a `GameObject` and whose owned hierarchy is represented by `ObjectRecord` entries.
- **PF-002**: A saved prefab source file MUST contain enough reflected component state to instantiate without editor-only source files.
- **PF-003**: Prefab references inside scenes MUST be stored as asset references, not copied inline, unless the user explicitly unpacks the instance.
- **PF-004**: Instance overrides MUST be stored as patch operations against stable source object IDs and property paths.
- **PF-005**: Applying an override to a prefab MUST update the prefab asset and schedule dependent prefab variants, scenes, and builds for refresh.
- **PF-006**: Reverting an override MUST remove the instance patch and reveal the current base prefab value.
- **PF-007**: Nested prefabs MUST be represented through asset references and override chains with cycle detection.
- **PF-008**: Imported model prefabs are generated artifacts by default and MUST NOT be edited in place; users may create editable variants or unpacked copies.
- **PF-009**: Prefab import MUST preserve unknown editor-safe records and unresolved asset references for round-trip editing, while runtime loading MUST fail on invalid required data.
- **PF-010**: Prefab build inclusion MUST include the prefab graph, all referenced assets, nested base prefabs, and override dependencies.
- **PF-011**: Creating a prefab from a scene selection MUST choose one root `GameObject`, serialize all owned children and components, and reject selections with multiple unrelated roots unless the editor creates an explicit wrapper root.
- **PF-012**: Prefab object IDs MUST be stable across save/load cycles and MUST remain the target identity for overrides, apply/revert operations, and source-to-instance mappings.
- **PF-013**: Prefab instances inside scenes MUST serialize the prefab asset reference, the instantiated root object ID, source-to-instance mapping, and local override patches.
- **PF-014**: Override patches MUST cover reflected property replacement, component add/remove/reorder, child GameObject add/remove/reorder, nested prefab instance changes, and object removal.
- **PF-015**: The editor override view MUST distinguish inherited base values, local overrides, missing targets, and unapplied source changes.
- **PF-016**: Applying a selected override MUST update the nearest editable prefab source or variant layer that owns the target object, then schedule reimport for dependent assets.
- **PF-017**: Reverting a selected override MUST remove only the local patch for that property or owned object and reveal the current base value.
- **PF-018**: Applying all overrides MUST be transactional: either all selected patches commit to the prefab source and import succeeds, or the previous prefab artifact remains active.
- **PF-019**: Prefab Stage editing MUST track dirty state, support save/discard, and show whether the edited asset is a source prefab, variant, nested prefab, or generated model prefab variant.
- **PF-020**: Nested prefab instances MUST remain asset references in the object graph; unpacking is the only operation that turns them into ordinary owned objects.
- **PF-021**: Variant creation from a source prefab or generated model prefab MUST create a new editable `.prefab` asset whose `basePrefab` points to the selected source asset or generated prefab sub-asset.
- **PF-022**: Generated model prefab variants MUST keep their base generated prefab reference so model reimport can update unchanged hierarchy and asset references.
- **PF-023**: Unpacking a generated model prefab instance MUST copy the resolved current hierarchy into the scene and sever the base prefab dependency.
- **PF-024**: Deleting or moving a prefab source without its `.meta` MUST surface missing-base or missing-prefab diagnostics without silently rebinding instances to another asset.
- **PF-025**: Prefab editor operations MUST preserve unknown editor-safe records for round-trip editing and report diagnostics for records that cannot be instantiated at runtime.
- **PF-026**: Prefab type and instance status queries MUST be available for every selected object, source prefab, generated model prefab, nested prefab, variant, and missing reference.
- **PF-027**: Save-and-connect MUST write a prefab asset, preserve source object IDs, and replace the scene object with a connected prefab instance in one transaction.
- **PF-028**: Loading prefab contents for isolated editing MUST create an editable stage copy whose save operation writes the source prefab and whose unload/discard operation leaves the source prefab unchanged.
- **PF-029**: Property modifications MUST preserve property path, source object ID, instance object ID, previous base value, local value, and owning prefab layer.
- **PF-030**: Added and removed component overrides MUST preserve component order and stable component object IDs when applied or reverted.
- **PF-031**: Added and removed child GameObject overrides MUST preserve sibling order and parent identity when applied or reverted.
- **PF-032**: Unpack must support two modes: preserving nested prefab links and completely unpacking all nested prefab links.
- **PF-033**: Prefab variant apply operations MUST target the nearest editable variant layer by default and MUST require an explicit command to push changes farther up the base chain.
- **PF-034**: Model prefab instances MUST allow scene overrides and variant creation while rejecting direct writes to the generated model prefab artifact.
- **PF-035**: Missing prefab assets MUST preserve scene instance data and override records for recovery when the missing asset returns.
- **PF-036**: Prefab override lists MUST be stable across editor sessions so UI selection, undo/redo, and diagnostics do not reorder unrelated overrides.

### Key Entities

- **Source Asset**: A user-visible file under a mounted asset root, paired with a `.meta` file and stable GUID.
- **Asset Meta**: Sidecar metadata containing GUID, importer selection, importer version, import settings, compatibility data, and optional default sub-asset selection.
- **Imported Artifact**: Runtime-ready output generated from source assets and import settings.
- **Artifact Manifest**: Per-import manifest describing generated artifacts, sub-assets, dependencies, diagnostics, hashes, and loader IDs.
- **Asset Version**: Hashable identity of source contents, importer version, target platform, dependencies, and postprocessors.
- **Sub-Asset**: Stable child asset generated from a compound source asset, such as a mesh or animation clip inside a model file.
- **Prefab Source**: Editable `.prefab` file containing a `Nullus.ObjectGraph.Prefab` document.
- **Prefab Artifact**: Runtime-ready prefab object graph with resolved asset references and validated override/base dependencies.
- **Prefab Instance**: Scene object that references a prefab asset and stores local override patches.
- **Prefab Variant**: Prefab asset that references a base prefab and stores override patches.
- **Imported Scene**: Format-neutral intermediate representation for model import, including nodes, meshes, materials, textures, skeletons, skins, animations, morph targets, and prefab bindings.
- **Dependency Record**: Relationship from one asset version to source files, GUIDs, imported assets, path mappings, target platform state, prefab bases, or override targets.
- **Import Diagnostic**: Persisted warning or error from scan, import, conversion, prefab validation, build, or resolution.
- **Build Manifest**: Runtime document that maps asset GUIDs and sub-assets to packaged artifacts.
- **AssetDatabase Facade**: Editor-facing API layer that exposes asset lookup, mutation, import, refresh, search, labels, and dependency queries over Nullus asset records.
- **Importer Settings**: Serialized per-importer settings, target platform overrides, remaps, user data, and version tokens that affect artifact generation.
- **Asset Postprocessor**: Ordered import hook that can observe or modify import results, add dependencies, and emit diagnostics.
- **Prefab Stage**: Isolated editor context for editing prefab contents before committing changes back to the source asset.
- **Prefab Override Record**: Deterministic editor-visible record describing a property, component, child object, nested prefab, or removal delta against a prefab base.
- **Asset Pack**: Build-time grouping based on pack name/variant metadata, resolved by Nullus runtime manifests.
- **Asset Drag Payload**: Editor command payload describing dragged assets or Hierarchy objects, source context, intended operation, and optional destination folder or hierarchy target.
- **Hierarchy Drop Target**: Editor target record for a scene, parent object, renderer component, material slot, or insertion index that can accept an asset drag operation.
- **Import Job**: Asynchronous unit of import work with source asset, target platform, dependency list, phase progress, cancellation token, diagnostics, and staged artifact paths.
- **Import Progress Event**: Editor-visible update containing batch count, current asset, phase, percentage, diagnostics, and terminal status.
- **Material Conversion Record**: Deterministic mapping from source material channels and textures to Nullus material artifact fields, fallback values, and diagnostics.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Scanning 1,000 mixed project assets creates or updates identity metadata without duplicate GUID aliases.
- **SC-002**: Renaming an asset together with its `.meta` preserves GUID lookup and does not require rewriting scene or prefab references.
- **SC-003**: Importing representative glTF/GLB/FBX/OBJ fixtures generates deterministic sub-asset IDs across two imports with unchanged source data.
- **SC-004**: Importing a representative model scene generates a prefab artifact whose hierarchy, static mesh renderers, material slots, and asset references match the imported scene.
- **SC-005**: Saving, reloading, instantiating, applying, and reverting a prefab override preserves stable object IDs and produces no dangling references.
- **SC-006**: Modifying a texture dependency marks only dependent imported assets stale, rather than forcing all model assets to reimport.
- **SC-007**: Modifying a base prefab marks dependent variants, scenes, and build manifests stale without invalidating unrelated assets.
- **SC-008**: A runtime build manifest for a scene includes all reachable asset and prefab artifacts and excludes unrelated project source files.
- **SC-009**: Import failures leave the previous successful artifact resolvable and persist user-visible diagnostics.
- **SC-010**: Runtime asset resolution for a packaged build succeeds without reading `.gltf`, `.glb`, `.fbx`, `.obj`, `.meta`, or editor cache database files.
- **SC-011**: Editor compatibility fixture tests cover AssetDatabase facade lookup/mutation/import/search/dependency operations and pass without requiring external editor runtime libraries.
- **SC-012**: Editor compatibility fixture tests cover importer setting changes, `SaveAndReimport`, external object remaps, postprocessor dependencies, and target platform invalidation.
- **SC-013**: Editor compatibility fixture tests cover regular prefab, model prefab, variant, nested prefab, missing prefab, Prefab Stage, granular apply/revert, and unpack modes.
- **SC-014**: Generated model prefabs behave as read-only model prefab assets while variants and scene overrides remain editable and survive model reimport.
- **SC-015**: Runtime artifact packs built from bundle-style metadata include all reachable dependencies exactly once and reject editor-only source APIs at runtime.
- **SC-016**: Asset Browser to Hierarchy drag/drop tests create connected prefab instances, assign materials, and reject invalid drops without corrupting scenes.
- **SC-017**: Hierarchy to Asset Browser drag/drop tests create prefab sources or variants with stable GUIDs, stable object IDs, and deterministic conflict handling.
- **SC-018**: A simulated large import emits progress updates for every required phase, supports cancellation, and leaves the editor command loop responsive.
- **SC-019**: Imported glTF, FBX, and OBJ material fixtures produce Nullus material artifacts whose preview data matches expected channel mappings and color-space policy.
- **SC-020**: Imported glTF, FBX, and OBJ material fixtures render in editor preview or scene viewport validation with expected supported material intent, with RenderDoc capture or deterministic renderer screenshot evidence recorded for visual regressions.
- **SC-021**: A cold model asset handle dropped into Scene View/Hierarchy returns pending without creating scene objects, writing artifacts, or invoking a synchronous import path.
- **SC-022**: A warm imported large model asset handle dropped into Scene View/Hierarchy creates the connected generated prefab instance without reading source `.gltf`, `.glb`, `.fbx`, or `.obj` files and without synchronously decoding mesh payloads on the drop callback.

## Assumptions

- The first implementation slice delivers source asset identity and scanning before model conversion.
- The system references editor asset database concepts such as source/imported databases, importer versions, postprocessors, and asset version hashes, but implementation remains native to Nullus.
- Existing path-based resource managers remain available as migration bridges.
- Importer support is delivered incrementally, with glTF treated as the reference path and FBX/OBJ routed through the common scene import representation.
- Existing `Runtime/Engine/Serialize` object graph and `PrefabDocument` are the maintained prefab serialization foundation.
- Full skeletal animation and morph playback may require new runtime components; until those exist, imports must preserve artifacts and emit explicit capability diagnostics rather than pretending playback is complete.
- Editor drag/drop behavior is implemented as command-layer services first; panels can bind to those services without owning asset or prefab rules.
- Material correctness is not considered complete with artifact/channel checks alone. The final acceptance requires converted material artifacts to drive editor preview or scene viewport bindings and to include RenderDoc capture or deterministic renderer screenshot evidence for glTF, FBX, and OBJ fixtures.
- Windows/DX12 is the primary validation target for early runtime checks, with cross-platform file path behavior covered by unit tests where practical.
