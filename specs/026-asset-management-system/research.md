# Research: Asset Management System

## Decision: Use GUID-backed sidecar `.meta` files for source identity

**Rationale**: Nullus already has `.meta` files for texture/model settings and object graph `$asset` references expect stable `AssetId` values. Adding GUID/importer fields preserves existing workflows while removing path fragility.

**Alternatives considered**: Central-only GUID database was rejected because moving projects between machines becomes harder and source control diffs lose visible identity data.

## Decision: Model the system after reference editor's source/imported split

**Rationale**: reference editor separates source asset scanning, importer versions, artifact/target hashes, resolver state, and postprocessors. Nullus needs the same conceptual separation to support glTF/FBX/OBJ conversion, hot reload, and build manifests.

**Alternatives considered**: Extending `AResourceManager` directly was rejected because it is path-keyed and runtime-object-oriented rather than import/artifact-oriented.

## Decision: Use a common ImportedScene representation for glTF, FBX, and OBJ

**Rationale**: glTF, FBX, and OBJ differ in capabilities, but all can be converted through scene, node, mesh, material, texture, skeleton, animation, and morph target records. This avoids format-specific runtime paths.

**Alternatives considered**: A glTF-only importer was rejected after FBX/OBJ became required. A fully generic importer framework first was rejected as too large for the first implementation slice.

## Decision: Treat Prefab as a first-class asset type backed by ObjectGraph

**Rationale**: Nullus already centers scene and prefab persistence on `Runtime/Engine/Serialize`, including `PrefabDocument`, `$owned`, `$ref`, `$asset`, and patch operations. Reusing this path lets imported model prefabs, user-authored prefabs, prefab variants, and scene instances share one validation and runtime loading model.

**Alternatives considered**: A separate prefab JSON format was rejected because it would duplicate object graph validation and asset reference semantics. Treating imported models only as render resources was rejected because it would lose hierarchy, material slots, override behavior, and build dependency closure.

## Decision: Generated model prefabs are read-only artifacts by default

**Rationale**: Imported glTF/FBX/OBJ files can be reimported at any time, so editing their generated hierarchy in place would create unclear ownership. Read-only generated prefabs keep imports reproducible while still allowing editable prefab variants or unpacked scene copies.

**Alternatives considered**: Writing generated prefabs as editable `.prefab` sources was rejected because reimport would overwrite user edits. Fully forbidding edits was rejected because users need to customize imported models; variants and unpacking provide the editable escape hatch.

## Decision: Runtime builds consume manifests and artifacts only

**Rationale**: Build outputs should be reproducible and independent of source paths, editor caches, `.meta` files, and external model files. A runtime manifest can map GUID/sub-asset IDs to packaged artifacts and loader IDs while preserving dependency closure.

**Alternatives considered**: Copying the `Assets/` tree into builds was rejected because it packages unrelated source files and does not solve imported sub-assets or GUID resolution. Loading directly from source model files was rejected because importers are editor/build-time tools, not runtime dependencies.

## Decision: Deliver source identity and scanning first

**Rationale**: Importers, dependency tracking, and runtime manifests all depend on stable source asset indexing. This slice is independently testable and does not disturb existing rendering/runtime paths.

**Alternatives considered**: Starting with glTF conversion was rejected because generated sub-assets would lack stable IDs and database ownership rules.

## Decision: Align reference behavior through compatibility contracts

**Rationale**: The local reference source tree is valuable for API semantics, state models, and editor workflow expectations. Nullus should match the user-visible behavior for asset import and Prefab authoring while keeping implementation, serialization, and runtime artifacts native to Nullus.

**Reference entry points**:

- `Editor/Mono/AssetDatabase/AssetDatabase.cs`
- `Editor/Mono/AssetDatabase/AssetDatabase.bindings.cs`
- `Editor/Mono/AssetDatabase/AssetDatabaseSearching.cs`
- `Editor/Src/AssetPipeline/AssetImporter.h`
- `Editor/Src/AssetPipeline/AssetImporter.cpp`
- `Editor/Mono/AssetPipeline/AssetImporter.bindings.cs`
- `Editor/Src/AssetPipeline/TextureImporting/TextureImporter.h`
- `Editor/Src/AssetPipeline/TextureImporting/TextureImporter.cpp`
- `Editor/Mono/AssetPipeline/TextureImporter.bindings.cs`
- `Editor/Mono/AssetPipeline/TextureImporterEnums.cs`
- `Editor/Mono/AssetPostprocessor.cs`
- `Editor/Mono/Prefabs/PrefabUtility.cs`
- `Editor/Mono/Prefabs/PrefabUtility.bindings.cs`
- `Editor/Src/Prefabs/PrefabUtility.h`
- `Editor/Src/Prefabs/PrefabUtility.cpp`
- `Editor/Src/Prefabs/PrefabCreation.cpp`
- `Editor/Src/Prefabs/PrefabConnection.cpp`
- `Editor/Src/AssetPipeline/PrefabImporter.h`
- `Editor/Src/AssetPipeline/PrefabImporter.cpp`
- `Editor/Mono/Prefabs/PrefabImporter.bindings.cs`
- `Editor/Mono/Prefabs/PrefabOverrides/PrefabOverridesUtility.cs`
- `Editor/Mono/Prefabs/PrefabOverrides/PrefabOverridesWindow.cs`
- `Editor/Mono/ProjectWindow/ProjectWindowUtil.cs`
- `Editor/Mono/GUI/TreeView/AssetOrGameObjectTreeViewDragging.cs`
- `Editor/Mono/GUI/TreeView/GameObjectTreeViewGUI.cs`
- `Editor/Src/BuildPipeline` asset pack build flow
- `Editor/Src/BuildPipeline` asset pack builder flow
- `Editor/Mono/Inspector` asset pack metadata UI flow

**Alternatives considered**: Bit-for-bit reference editor compatibility was rejected because it would require reference private serialization/cache formats and would undermine Nullus-native runtime manifests. A loose "inspired by reference editor" approach was rejected because it would not give implementers a testable definition of complete alignment.

## Decision: Benchmark against broader engine asset pipelines

**Rationale**: reference editor is the primary requested compatibility reference, but industrial asset pipeline quality should also be checked against common engine patterns from Unreal, Godot, and CryEngine-style source-to-product pipelines. The local plan-review benchmark entry `benchmarks/game_asset_pipelines.md` records this comparison and the minimum industrial standards for stable identity, import artifacts, dependency tracking, Prefab/scene composition, non-blocking import, material correctness, and runtime packaging.

**Concrete comparison**:

| Engine | Relevant pattern | Nullus mapping |
|--------|------------------|----------------|
| reference editor | `.meta` GUIDs, AssetDatabase, AssetImporter, Prefab/Variant/Nested Prefab, asset pack metadata | GUID/meta source records, editor-compatible facades, Prefab object graph, asset pack/runtime manifest metadata |
| Unreal Engine | Asset Registry, Factory/Reimport flow, cooked packages, dependency closure | Source asset database, importer registry, artifact manifests, runtime manifests, dependency graph |
| Godot 4 | ResourceUID, `.import` remaps, EditorFileSystem scan, ResourceLoader | GUID/sub-asset IDs, source scan, import metadata, runtime loader IDs |
| CryEngine-style pipelines | Resource compiler and product assets separated from editable sources | Artifact writer, atomic staging/commit, runtime source-file exclusion |

**Alternatives considered**: single-reference benchmarking was rejected because it can miss modern expectations such as explicit runtime cooking boundaries and asynchronous editor responsiveness. Unreal/Godot exact API compatibility was rejected because the user requirement is editor compatibility and Nullus should expose one coherent editor model, not multiple engine APIs.

## Decision: Implement editor drag/drop as command workflows

**Rationale**: Asset Browser and Hierarchy drag/drop affects scenes, prefabs, materials, import state, undo/redo, and dependency refresh. Keeping this logic in panel widgets would duplicate rules and make tests brittle. A command workflow can be tested without a live UI and then bound to editor panels.

**Reference editor entry points**: `ProjectWindowUtil.cs`, `AssetOrGameObjectTreeViewDragging.cs`, `GameObjectTreeViewGUI.cs`, `PrefabUtility.cs`, and Prefab Stage code under `Editor/Mono/SceneManagement/StageManager/PrefabStage`.

**Alternatives considered**: Direct panel-level implementation was rejected because it would make Prefab, material, and import side effects hard to validate. Treating drag/drop as file copy only was rejected because reference-style workflows instantiate prefabs, assign materials, and create prefabs/variants.

## Decision: Make import progress a first-class pipeline output

**Rationale**: Large glTF/GLB/FBX imports may involve dependency copy, source parse, conversion, texture/material processing, artifact writes, postprocessors, and commit. The editor must not appear frozen, and users need progress, diagnostics, and cancellation. Progress must be tied to atomic commit so the UI does not report success before the artifact is usable.

**Alternatives considered**: Blocking import on the UI thread was rejected because large assets would freeze the editor. Fire-and-forget background import was rejected because users need progress, cancellation, and reliable diagnostics.

## Decision: Treat material conversion as part of import correctness

**Rationale**: A model import is not complete if the generated prefab appears with wrong color space, missing normal maps, or incorrect alpha behavior. Material conversion must map source material channels into Nullus artifacts used by preview, viewport, runtime, and builds.

**Alternatives considered**: Deferring material correctness to the renderer was rejected because the importer owns source channel interpretation, texture color-space policy, and material fallback diagnostics. Using source material records directly at runtime was rejected because runtime builds must consume converted artifacts.
