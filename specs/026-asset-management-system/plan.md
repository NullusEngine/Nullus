# Implementation Plan: Asset Management System

**Branch**: `026-asset-management-system` | **Date**: 2026-05-13 | **Spec**: `specs/026-asset-management-system/spec.md`  
**Input**: Feature specification from `specs/026-asset-management-system/spec.md`

## Summary

Build a reference-informed but Nullus-native asset management system that starts with stable source asset identity and grows into imported artifact caching, dependency-based reimport, runtime resolution, build manifests, Prefab authoring/instancing, and scene-model import for glTF/GLB/FBX/OBJ.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Nullus runtime modules, nlohmann JSON, existing `Guid`, existing `.meta`/`IniFile`, existing `Runtime/Engine/Serialize` object graph and `PrefabDocument`; Assimp remains available for model parsing; glTF may use a dedicated parser in a later slice  
**Storage**: Filesystem sidecar `.meta` files, `.prefab` object graph source files, JSON/text asset database files in project cache, per-import artifact manifests, runtime artifact files and build manifests  
**Testing**: `NullusUnitTests`, targeted CTest runs, fixture-based import tests  
**Target Platform**: Desktop editor and game runtime; first verified platform Windows, path logic kept portable  
**Project Type**: Native engine/editor/runtime feature across `Runtime/Core`, `Runtime/Engine`, `Runtime/Rendering`, `Project/Editor`, `Project/Game`, and `Tests`  
**Performance Goals**: 1,000 source assets can be scanned without duplicate GUID aliases; incremental refresh avoids full-project reimport when only one dependency changes  
**Constraints**: Preserve `Runtime/*/Gen/`; keep Editor/Game runnable during staged delivery; keep existing `AResourceManager` path loading as a migration bridge  
**Scale/Scope**: Full asset identity, import, artifact, dependency, prefab, runtime, build, and model-scene conversion architecture, delivered in independent slices

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major change**: Pass. This bundle contains `spec.md`, `plan.md`, and `tasks.md`.
- **Validation matches subsystem**: Pass. Core asset database work uses unit tests; later rendering/model import work requires fixture import tests and runtime/editor checks.
- **Generated code boundaries**: Pass. No files under `Runtime/*/Gen/` will be edited.
- **Incremental verified delivery**: Pass. The first implementation slice is source identity and scanning only.
- **Product runtime preservation**: Pass. Existing ResourceManager and path-based loading remain available during migration.

## Project Structure

### Documentation (this feature)

```text
specs/026-asset-management-system/
├── spec.md
├── plan.md
├── tasks.md
├── research.md
├── data-model.md
├── quickstart.md
└── contracts/
```

### Source Code (repository root)

```text
Runtime/Core/Assets/
├── AssetDiagnostics.h
├── AssetId.h
├── AssetMeta.h
├── AssetMeta.cpp
├── AssetPath.h
├── AssetPath.cpp
├── SourceAssetDatabase.h
├── SourceAssetDatabase.cpp
├── AssetVersion.h
├── ArtifactManifest.h
├── ArtifactWriter.h
├── ArtifactWriter.cpp
└── AssetResolver.h

Runtime/Engine/Assets/
├── RuntimeAssetDatabase.h
├── RuntimeAssetDatabase.cpp
├── PrefabAsset.h
├── PrefabAsset.cpp
├── ModelPrefabBuilder.h
└── ModelPrefabBuilder.cpp

Runtime/Rendering/Assets/
├── ImportedScene.h
├── MaterialConversion.h
├── MaterialConversion.cpp
├── SceneImportPipeline.h
└── SceneImportPipeline.cpp

Project/Editor/Assets/
├── EditorAssetDatabase.h
├── EditorAssetDatabase.cpp
├── AssetDragDropWorkflow.h
├── AssetDragDropWorkflow.cpp
├── ImportProgressTracker.h
├── ImportProgressTracker.cpp
├── PrefabEditorWorkflow.h
├── PrefabEditorWorkflow.cpp
├── AssetDatabaseFacade.h
├── AssetDatabaseFacade.cpp
├── AssetImporterFacade.h
├── AssetImporterFacade.cpp
├── PrefabUtilityFacade.h
├── PrefabUtilityFacade.cpp
├── ExternalAssetImporter.h
└── ExternalAssetImporter.cpp

Tests/Unit/
├── AssetFoundationTests.cpp
├── AssetImportPipelineTests.cpp
├── AssetImportProgressTests.cpp
├── AssetMaterialConversionTests.cpp
├── AssetManifestTests.cpp
├── AssetPrefabPipelineTests.cpp
└── EditorAssetDragDropTests.cpp

Tests/Rendering/
└── AssetMaterialViewportTests.cpp
```

**Structure Decision**: Core identity, metadata, scan, version, artifact, and dependency primitives live in `Runtime/Core/Assets` so Editor, Engine, Game, and Rendering can share them without circular dependencies. Rendering-specific model conversion stays under `Runtime/Rendering/Assets`; generated model prefab construction stays under `Runtime/Engine/Assets` because it creates Engine object graph prefab artifacts; runtime prefab artifacts and manifest resolution also stay under `Runtime/Engine/Assets`; editor orchestration and external copy/import UI stay under `Project/Editor/Assets`; viewport material validation lives under `Tests/Rendering` because imported material correctness must be proven through renderer-facing bindings, not only artifact records.

## Asset Pipeline Architecture

1. **Source database**: Scans project, engine, and package roots; writes `.meta` for editable assets; indexes GUID/path/importer state; distinguishes read-only roots. Source GUIDs, not file basenames, are the identity boundary so same-stem files in different formats import independently.
2. **Importer registry**: Chooses an importer by importer ID, extension, version, and target platform; routes `.gltf`, `.glb`, `.fbx`, and `.obj` into `ImportedScene`.
3. **Intermediate conversion**: Converts model formats into format-neutral nodes, meshes, materials, textures, skeletons, skins, animations, morph targets, and prefab bindings.
4. **Artifact writer**: Writes runtime-ready assets under GUID-keyed artifact roots and a per-import artifact manifest with stable sub-asset IDs, hashes, loader IDs, dependencies, and diagnostics.
5. **Shader artifact pipeline**: Imports `.hlsl` source assets through the same `AssetDatabaseFacade` path as model and prefab sources, writes `.nshader` payloads under GUID-keyed `Library/Artifacts`, records stage/target compile metadata in `ArtifactDB`, and lets runtime shader loading consume committed artifacts without source-path compilation on hot paths.
6. **Prefab pipeline**: Imports `.prefab` source files through `Runtime/Engine/Serialize`; builds generated model prefabs from `ImportedScene`; validates base prefab chains and override patches.
7. **Dependency graph**: Tracks source file hashes, source GUIDs, sub-assets, imported artifacts, target platform, importer/postprocessor versions, path-to-GUID mapping, prefab bases, and override targets.
8. **Resolver and hot reload**: Reports stale/missing/importing/failed/up-to-date state; schedules async import; commits artifacts atomically; refreshes loaded resources or marks them for explicit reload.
9. **Build manifest**: Traverses selected scenes or prefabs, follows asset and prefab references, writes a runtime manifest and only packages reachable artifacts.

## Editor Drag/Drop Workflow Architecture

Asset and hierarchy drag/drop is implemented as a command-service layer so editor panels can stay thin:

1. `Project/Editor/Assets/AssetDragDropWorkflow` owns drag payload validation, action resolution, command execution, undo/redo descriptors, diagnostics, and dirty-state propagation.
2. Asset Browser to Hierarchy drops support prefab instantiation, model source import-and-instantiate via its internal generated prefab, material slot assignment, texture-to-material creation/assignment, and invalid-target diagnostics.
3. Hierarchy to Asset Browser drops support save-as-prefab, save-and-connect, create editable variant from connected prefab/model prefab instances, and deterministic destination conflict handling.
4. Drag/drop commands use `EditorAssetDatabase`, `PrefabEditorWorkflow`, and `PrefabUtilityFacade` rather than directly editing asset metadata or prefab graphs.
5. Model source drops create connected generated-prefab instances. The asset browser does not display a separate virtual `.prefab` child for imported models; dragging generated model prefab instances back to assets creates variants or unpacked editable copies, and the generated artifact is never overwritten.
6. All command results include changed assets/scenes, created GUIDs, diagnostics, and dependency-refresh requests.
7. Project Browser startup, file-watcher changes, and copy/move/import into project `Assets` schedule background preimport for scene-droppable source assets (`ModelScene` and `Prefab`). Startup fills missing artifacts only; watcher/copy requests force reimport only for the changed paths reported by `AssetFileWatcher` or the copied destination.
8. Drag/drop remains a hot-handle operation: cold or missing imported artifacts return pending/diagnostic results on drop, while the preimport scheduler warms model/prefab artifacts before the user normally reaches the drop callback.

## Async Import Progress Architecture

Import work is modeled as cancellable jobs with editor-visible progress:

1. `Project/Editor/Assets/ImportProgressTracker` tracks batch state, per-asset phase, percent complete, diagnostics, cancellation, and terminal result.
2. Import jobs publish phases for dependency copy, source parse, intermediate conversion, artifact write, postprocess, and atomic commit.
3. The editor UI consumes progress events from the tracker and must remain responsive while jobs run.
4. Cancellation is cooperative. A cancelled job removes staged artifacts and keeps the previous committed artifact manifest active.
5. Completion is reported only after artifact commit succeeds and dependent assets are marked stale or refreshed.

## Material Conversion Architecture

Imported material correctness is handled before generated prefabs are considered visually complete:

1. `Runtime/Rendering/Assets/MaterialConversion` maps glTF PBR, OBJ MTL, and parser-exposed FBX material channels into Nullus material artifact records.
2. Color textures default to sRGB; data textures such as normal, metallic, roughness, occlusion, and masks default to linear unless importer settings override them.
3. The converter preserves texture slot names, UV set and transform, sampler wrap/filter state, alpha mode/cutoff, normal scale, emissive factor, metallic/roughness values, and fallback values.
4. Unsupported material channels are diagnostics, not silent data loss.
5. Generated model prefabs reference material artifacts through GUID/sub-asset IDs, so Asset Browser preview, scene viewport, runtime manifest, and build output all use the same converted material data.
6. Converted material payloads prefer shader artifact handles for the selected built-in material shader when a committed shader artifact is available; legacy shader paths remain readable for existing assets and engine fallback materials.
7. Final material acceptance requires renderer-facing evidence: `AssetMaterialViewportTests` plus either RenderDoc capture or deterministic screenshot comparison for representative glTF, FBX, and OBJ fixtures.

## Prefab Editor Workflow Architecture

Prefab editor workflow is an editor-facing orchestration layer over the runtime object graph and prefab artifacts:

1. `Runtime/Engine/Assets/PrefabAsset` owns runtime-safe prefab artifact validation, base-chain validation, deterministic override patch normalization, nested prefab dependency extraction, and cycle diagnostics.
2. `Project/Editor/Assets/PrefabEditorWorkflow` owns editor commands: create prefab from selection, instantiate prefab, open prefab stage, save/discard, discover overrides, apply/revert overrides, create editable variant, and unpack instance.
3. `Project/Editor/Assets/EditorAssetDatabase` exposes command descriptors, generated prefab read-only state, and refresh scheduling so panels can present workflow actions without owning prefab rules.
4. Scene instances store prefab asset references plus source-to-instance maps and local patch operations. They do not inline-copy source prefab graphs unless explicitly unpacked.
5. Prefab Stage edits a prefab source or variant in isolation. Scene instances refresh after a saved prefab import commits; discard leaves scene instances untouched.
6. Generated model prefabs remain read-only. The editor routes customization to editable variants or unpacked scene objects.
7. Apply/revert operations are transactional and dependency-aware: successful applies mark dependent variants, scenes, and build manifests stale; failed writes or imports keep the previous prefab artifact active.

## Editor Compatibility Architecture

Editor compatibility is delivered as a compatibility-semantics layer, not a source-code port:

1. `Project/Editor/Assets/AssetDatabaseFacade` maps editor-compatible `AssetDatabase` operations onto Nullus source database, artifact manifests, dependency graph, and runtime manifest records.
2. `Project/Editor/Assets/AssetImporterFacade` maps editor-compatible `AssetImporter`, `ModelImporter`, `TextureImporter`, postprocessor, and scripted importer concepts onto Nullus importer descriptors, serialized settings, dependency declarations, remaps, diagnostics, and `SaveAndReimport`.
3. `Project/Editor/Assets/PrefabUtilityFacade` maps editor-compatible `PrefabUtility` queries and operations onto `PrefabAsset`, `PrefabEditorWorkflow`, object graph patches, variants, nested prefabs, model prefabs, and Prefab Stage.
4. Asset pack names and variants map to Nullus asset pack/runtime manifest metadata. Runtime code consumes manifest entries and never calls editor-only AssetDatabase/importer APIs.
5. Compatibility tests use representative reference workflow fixtures and expected state transitions. They must not depend on external editor binaries or private implementation internals.
6. Any reference behavior that does not map cleanly to Nullus must be documented in a compatibility matrix with one of: supported, supported with Nullus-native naming, diagnostic-only, deferred, or intentionally out of scope.

## Prefab Design

Nullus Prefab support is built on the existing `Runtime/Engine/Serialize` object graph path:

- Editable prefab sources are `.prefab` files with `Nullus.ObjectGraph.Prefab` documents.
- Prefab records use reflected object fields plus explicit `$owned`, `$ref`, `$asset`, and patch operations.
- Prefab variants reference a base prefab by `AssetId` and store override patches.
- Scene prefab instances reference prefab assets and store local overrides instead of copying full prefab contents inline.
- Imported model prefabs are generated artifacts and are read-only by default. Users can create editable variants or unpack them into scene objects.
- Static imported renderables map to existing `TransformComponent`, `MeshFilter`, and `MeshRenderer` records. Skeletal animation and morph playback require explicit runtime component support before those playback paths are considered complete.
- Runtime prefab artifacts are validated object graphs with manifest-resolved asset references.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Multi-module implementation | Asset identity must be shared by editor, runtime, rendering import, and build logic | Keeping it in Editor would prevent runtime manifest resolution |
| New artifact and dependency concepts | Imported assets need stable, incremental, build-target-aware outputs | Path-based `AResourceManager` cannot safely handle rename, sub-assets, or build manifests |
| Prefab pipeline in runtime assets | Scenes and builds need reusable object graphs and generated model prefabs | Treating imported models as raw render resources would lose hierarchy, overrides, and dependency closure |
