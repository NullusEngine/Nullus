# Data Model: Asset Management System

## AssetId

- `guid`: Stable 128-bit identifier.
- Valid when GUID is non-empty.
- Used by source assets, imported artifacts, sub-assets, prefab references, object graph `$asset` references, and runtime manifest entries.

## SubAssetId

- `sourceAssetId`: GUID of the owning source asset.
- `subAssetKey`: Stable string key inside a compound source.
- `artifactType`: Mesh, Model, Material, Texture, Skeleton, Skin, AnimationClip, MorphTarget, Prefab, Scene, Shader, Audio, or Other.
- Generated from source-local identifiers when available. Fallback keys must include deterministic disambiguators and emit diagnostics when stability may depend on names.

## AssetMeta

- `guid`: Source asset GUID.
- `importerId`: Importer selection such as `scene-model`, `texture`, `shader`, `material`, `prefab`, `scene`, `audio`, or `unknown`.
- `importerVersion`: Version number that invalidates old artifacts when importer behavior changes.
- `assetType`: High-level classification used by editor and import registry.
- `defaultSubAsset`: Optional sub-asset key selected by default when a compound source is referenced.
- `importSettings`: Importer-specific settings preserved across scans.
- `sourcePath`: Not persisted as authoritative identity; stored only as scan context or diagnostic hint.

## SourceAssetRecord

- `id`: AssetId from meta GUID.
- `absolutePath`: Canonical or normalized filesystem path.
- `relativePath`: Path relative to the scanned root.
- `metaPath`: Sidecar path.
- `importerId`: Selected importer.
- `assetType`: High-level source type.
- `readOnly`: True for engine or package roots.
- `rootKind`: Project, Engine, Package, or ExternalStaging.

## SourceAssetDatabase

- `records`: Source assets indexed by GUID and path.
- `diagnostics`: Duplicate GUIDs, invalid meta files, inaccessible files, missing sidecars, repaired metadata, and rejected path escapes.
- State transitions: empty -> scanned -> refreshed; duplicate or invalid records attach diagnostics but do not silently alias.

## ImportedScene

- `sourceAssetId`: Source model asset.
- `nodes`: Source hierarchy with stable node keys, parent links, names, and local transforms.
- `meshes`: Mesh records with primitives, vertex streams, index data, material slots, bounds, and stable source keys.
- `materials`: Material records with PBR parameters, texture slots, alpha mode, double-sided state, and unsupported channel diagnostics.
- `textures`: Texture records with source image path or embedded payload reference, color space, sampler, and usage.
- `skeletons`: Joint hierarchy and inverse bind data.
- `skins`: Skin-to-mesh bindings and joint remapping.
- `animations`: Clips, channels, samplers, interpolation, target node/property, and timing range.
- `morphTargets`: Target deltas, weights, names, and mesh bindings.
- `prefabBindings`: Mapping from imported nodes to generated prefab GameObjects and component records.
- `diagnostics`: Format conversion warnings and errors.

## Imported Artifact

- `artifactId`: Stable GUID or hash identity for generated runtime data.
- `sourceAssetId`: Source asset that produced it.
- `subAssetId`: Optional compound child identity.
- `artifactType`: Model, Mesh, Material, Texture, Skeleton, Skin, AnimationClip, MorphTarget, Prefab, Scene, Shader, Audio, or Other.
- `loaderId`: Runtime loader to use for the artifact.
- `targetPlatform`: Build/editor target platform.
- `targetHash`: Hash of source contents, import settings, target platform, importer version, dependencies, and postprocessors.
- `artifactPath`: Project cache or build output path.
- `contentHash`: Hash of the written runtime payload.

## ShaderArtifact

- `sourceAssetId`: GUID of the `.hlsl` source asset.
- `sourcePath`: Editor asset path or source path hint used for diagnostics.
- `subAssetKey`: Stable shader sub-asset key, normally `shader:<source-stem>`.
- `targetPlatform`: Editor or build target that selected the compiled variants.
- `stages`: One record per discovered `VSMain`, `PSMain`, or `CSMain` entry point and requested backend target.
- `stage`: Vertex, Pixel, or Compute.
- `shaderTarget`: DXIL, SPIRV, or GLSL.
- `entryPoint`: Entry point compiled for the stage.
- `targetProfile`: Compiler profile used for the stage.
- `status`: NotCompiled, Succeeded, or Failed.
- `cacheKey`: Compiler cache key for source/include/toolchain invalidation.
- `compiledArtifactPath`: Path to the compiled bytecode/cache artifact when compilation succeeded.
- `diagnostics`: Compiler, reflection, or missing-toolchain diagnostics persisted for editor inspection.
- `sourceDependencies`: Source file and include dependencies captured during import.

## ArtifactManifest

- `sourceAssetId`: Source asset that produced the manifest.
- `importerId`: Importer used for the manifest.
- `importerVersion`: Importer version included in version hashing.
- `targetPlatform`: Platform for generated artifacts.
- `primaryArtifact`: Default artifact for direct source asset references.
- `subAssets`: Generated artifact entries keyed by stable sub-asset ID.
- `dependencies`: Dependency records captured during import.
- `diagnostics`: Latest import diagnostics.
- `previousSuccessfulManifest`: Optional pointer retained while a replacement import is pending or failed.

## AssetVersion

- `sourceContentHash`: Hash of the source file or source object graph.
- `metaHash`: Hash of import settings that affect output.
- `dependencyHash`: Hash over dependency records.
- `importerVersion`: Importer implementation version.
- `postprocessorVersion`: Version token for asset postprocessors.
- `targetPlatform`: Platform or editor target.
- `artifactHash`: Hash over committed artifact payloads.

## DependencyRecord

- `owner`: Asset version or artifact that depends on another item.
- `kind`: SourceFileHash, SourceAssetGuid, ImportedArtifact, PathToGuidMapping, BuildTarget, ImporterVersion, PostprocessorVersion, PrefabBase, NestedPrefab, PrefabOverrideTarget, RuntimeComponentCapability, or RawPackageFile.
- `value`: GUID, sub-asset key, path, hash, target name, version token, component capability name, or package entry.

## ImportDiagnostic

- `severity`: Info, Warning, Error.
- `code`: Stable diagnostic code.
- `assetId`: Asset affected when available.
- `subAssetKey`: Sub-asset affected when available.
- `path`: Source, dependency, or artifact path when relevant.
- `message`: User-visible explanation.
- `sticky`: True when the diagnostic should remain visible until a successful reimport clears it.

## AssetDragPayload

- `sourcePanel`: AssetBrowser, Hierarchy, SceneView, Inspector, or Unknown.
- `assets`: Ordered asset references for dragged source assets, artifacts, folders, materials, textures, prefabs, generated model prefabs, scenes, or sub-assets.
- `hierarchyObjects`: Ordered scene object IDs for dragged `GameObject`, component, renderer, material slot, prefab instance, or generated model prefab instance records.
- `operationHint`: Default, Instantiate, AssignMaterial, CreateMaterialAndAssign, SaveAsPrefab, SaveAndConnectPrefab, CreateVariant, UnpackCopy, Move, Copy, or Link.
- `modifiers`: Platform-normalized drag modifier flags such as copy, move, link, alternate action, or append.
- `sourceContext`: Scene ID, asset folder path, prefab stage ID, selection ID, and user-action flag used for diagnostics and undo/redo grouping.

## HierarchyDropTarget

- `sceneId`: Target scene or prefab stage scene.
- `parentObjectId`: Optional parent `GameObject` for instantiate or reparent operations.
- `insertIndex`: Optional sibling insertion index.
- `rendererObjectId`: Optional renderer component or owning `GameObject` for material and texture drops.
- `materialSlot`: Optional material slot index or append policy.
- `targetContext`: Hierarchy, SceneView, Inspector, PrefabStage, AssetBrowserFolder, or Invalid.
- `editable`: False for read-only package roots, generated artifacts, locked prefab stages, or immutable runtime views.

## AssetDragDropCommandResult

- `status`: Accepted, Rejected, NeedsUserChoice, Cancelled, Failed, or Committed.
- `operation`: Resolved operation executed or offered to the user.
- `createdAssets`: GUIDs/sub-assets created by the command.
- `modifiedAssets`: GUIDs/sub-assets modified by the command.
- `modifiedScenes`: Scene or prefab stage IDs marked dirty.
- `selectedObjects`: Scene object IDs or asset IDs selected after command completion.
- `diagnostics`: User-visible rejection, warning, or failure diagnostics.
- `dependencyRefreshRequests`: Assets, scenes, prefabs, materials, and build manifests that must be marked stale or refreshed.

## ImportJob

- `jobId`: Stable editor job identifier for progress, cancellation, and diagnostics.
- `batchId`: Optional batch identifier shared by related import jobs.
- `sourceAssetId`: Source asset GUID being imported.
- `targetPlatform`: Editor or build target for artifact generation.
- `importerId`: Importer selected for the job.
- `settingsHash`: Hash of importer settings used by this job.
- `dependencyCopyPolicy`: CopyIntoProject, ReferenceExternalWithDiagnostic, SkipMissing, or FailOnMissing.
- `phase`: Queued, CopyDependencies, ParseSource, ConvertIntermediate, WriteArtifacts, Postprocess, Commit, Completed, Failed, or Cancelled.
- `cancelRequested`: True after cooperative cancellation is requested.
- `stagingRoot`: Temporary artifact staging path.
- `committedManifest`: Artifact manifest committed by the job when successful.
- `previousSuccessfulManifest`: Manifest retained when replacement import fails or is cancelled.
- `diagnostics`: Import diagnostics accumulated across phases.

## ImportProgressEvent

- `jobId`: Import job this event belongs to.
- `batchId`: Optional batch identifier.
- `sourceAssetId`: Source asset GUID.
- `sourcePath`: Display path used by the editor.
- `phase`: Current import phase.
- `assetProgress`: Normalized 0.0-1.0 progress for the active asset.
- `batchProgress`: Normalized 0.0-1.0 progress for the batch.
- `message`: User-visible status message.
- `diagnostics`: New diagnostics emitted with this event.
- `terminalStatus`: None, Succeeded, Failed, or Cancelled.

## MaterialConversionRecord

- `sourceMaterialKey`: Stable source-local material identifier.
- `sourceFormat`: glTF, GLB, FBX, OBJ, Prefab, or Unknown.
- `targetMaterialId`: Generated material sub-asset ID.
- `shaderModel`: Nullus material/shader model selected for the converted artifact.
- `textureSlots`: Named slots such as baseColor, metallicRoughness, normal, occlusion, emissive, opacity, specular, or custom.
- `factors`: Scalar/vector material factors such as baseColorFactor, metallicFactor, roughnessFactor, emissiveFactor, normalScale, alphaCutoff, shininess, or opacity.
- `samplers`: Per-texture wrap/filter/mipmap state.
- `uvBindings`: UV set, transform, scale, offset, and rotation for each texture slot.
- `colorSpacePolicy`: Per-texture sRGB or linear policy and any importer override source.
- `alphaMode`: Opaque, Mask, Blend, Premultiplied, or DiagnosticOnly.
- `doubleSided`: Whether the source material requests two-sided rendering.
- `fallbacks`: Default values inserted for missing channels.
- `diagnostics`: Unsupported channel, missing texture, ambiguous source model, color-space conflict, or shader capability diagnostics.

## PrefabSource

- `assetId`: GUID of the `.prefab` source asset.
- `graph`: `Nullus.ObjectGraph.Prefab` document.
- `basePrefab`: Optional asset reference for variants.
- `objects`: Object graph records with stable `ObjectId` values.
- `overrides`: Patch operations for variant or instance modifications.
- `assetReferences`: `$asset` references embedded in reflected fields or explicit graph relationships.

## PrefabArtifact

- `assetId`: Source prefab GUID or generated model prefab sub-asset ID.
- `graph`: Validated runtime object graph.
- `baseChain`: Ordered base prefab dependencies for variants.
- `resolvedAssetReferences`: Manifest-resolved asset references required by the graph.
- `sourceToRuntimeObjectMap`: Mapping used to preserve override identity across import and instantiation.
- `diagnostics`: Validation and resolution diagnostics.

## PrefabInstance

- `prefabAsset`: Asset reference to the prefab or prefab variant.
- `instanceRoot`: Scene object ID of the instantiated root.
- `sourceToInstance`: Mapping from prefab source object IDs to scene instance object IDs.
- `overrides`: Local patch operations against prefab source object IDs.
- `unpacked`: True when the instance has been detached from the source prefab and is now ordinary scene-owned objects.

## BuildManifest

- `schemaVersion`: Manifest schema version.
- `targetPlatform`: Build target.
- `roots`: Scene or prefab assets selected for build.
- `entries`: Asset GUID/sub-asset ID to runtime artifact path, type, loader ID, content hash, and dependency list.
- `prefabEntries`: Runtime prefab graph entries and their dependency closure.
- `rawFiles`: Explicit raw packaged files, excluding source-only editor inputs by default.
- `diagnostics`: Build-time errors and warnings.
