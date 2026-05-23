# Contract: Editor Asset Import And Prefab Compatibility

## Alignment Definition

Nullus must align with the reference editor at the user-visible workflow and editor API semantics level for asset import and Prefab workflows. Nullus remains a native implementation: reference source code, private binary cache layouts, reference YAML scene/prefab serialization, and external editor runtime libraries are reference material, not compatibility targets.

Alignment status values:

- `Supported`: Nullus must provide equivalent behavior and test coverage.
- `Supported with Nullus-native naming`: Nullus behavior must match the reference workflow, but the public type names, serialized documents, or runtime artifact names may be Nullus-native.
- `Diagnostic-only`: Nullus must preserve enough data and emit clear diagnostics, but runtime playback or editing is blocked until the dependent subsystem exists.
- `Deferred`: Valid reference behavior, planned after this feature because the matching Nullus subsystem is not part of the asset import or Prefab slice.
- `Out of scope`: reference behavior that must not be claimed as supported by this feature.

## AssetDatabase Alignment

| Reference behavior | Reference entry points | Required Nullus behavior | Alignment status | Evidence |
|-----------------------|------------------------|--------------------------|------------------|----------|
| Path to GUID and GUID to path lookup | `AssetDatabase.AssetPathToGUID`, `AssetDatabase.GUIDToAssetPath` | Resolve by `.meta` GUID and normalized asset path across project, engine, and package roots | Supported | FR-001, FR-003, FR-041, T098-T099 |
| Main asset loading | `LoadMainAssetAtPath`, `GetMainAssetInstanceID` | Return the primary imported artifact or source-backed editor proxy for a path | Supported with Nullus-native naming | FR-045, T098-T099 |
| Sub-asset loading | `LoadAllAssetsAtPath`, `LoadAllAssetRepresentationsAtPath` | Return deterministic sub-assets for model, prefab, texture, material, skeleton, animation, and morph artifacts | Supported with Nullus-native naming | FR-012, FR-045, T098-T099 |
| Asset creation | `CreateAsset`, `AddObjectToAsset` | Create source assets and attach sub-assets through Nullus artifact/source records with stable identity | Supported with Nullus-native naming | FR-041, T147-T148 |
| Asset extraction | `ExtractAsset` | Extract importer-generated materials or sub-assets into editable source assets and remap the importer | Supported with Nullus-native naming | FR-048, T113-T114, T147-T148 |
| Project Browser to Hierarchy drag/drop | Project Window and Scene/Hierarchy drag handlers | Instantiate prefabs/model prefabs, assign materials, create material from texture, or diagnose invalid drops | Supported with Nullus-native naming | FR-071-FR-074, T156-T163 |
| Hierarchy to Project Browser drag/drop | Project Window drag handlers, prefab save APIs | Create prefab sources, save-and-connect, variants, or unpacked copies from Hierarchy objects | Supported with Nullus-native naming | FR-075, T164-T169 |
| Path mutation | `MoveAsset`, `CopyAsset`, `RenameAsset`, `DeleteAsset` | Preserve `.meta` GUID on moves/renames, create new GUID on copies, and preserve recoverable missing references on deletes | Supported | FR-042, T100-T101 |
| Folder operations | `CreateFolder`, `IsValidFolder`, `GenerateUniqueAssetPath` | Create project folders with metadata and generate deterministic conflict-free paths | Supported | FR-041, T100-T101, T147-T148 |
| Refresh and import | `Refresh`, `ImportAsset` | Rescan sources, select importers, queue imports, and atomically commit artifacts | Supported | FR-015, FR-043, T102-T103 |
| Import batching | `StartAssetEditing`, `StopAssetEditing` | Delay import execution until the edit batch closes, then process queued changes once | Supported | FR-043, T102-T103 |
| Dirty import settings | `WriteImportSettingsIfDirty` | Persist importer settings only when changed and schedule reimport when the asset version key changes | Supported with Nullus-native naming | FR-047, FR-049, T149-T150 |
| Search filters | `FindAssets`, `SearchFilter` | Support name, type, label, folder, package, and pack-style filters with deterministic ordering | Supported | FR-044, T106-T107 |
| Labels | `GetLabels`, `SetLabels`, `GetAllLabels` | Store labels in asset metadata for search and editor display | Supported | FR-046, T106-T107 |
| Dependency queries | `GetDependencies` | Return direct or recursive source, artifact, sub-asset, prefab, and pack dependencies | Supported with Nullus-native naming | FR-006, FR-017, T104-T105 |
| Asset pack metadata | `packName`, `packVariant`, pack search filters | Map pack-style name/variant metadata to Nullus asset packs and runtime manifests | Supported with Nullus-native naming | FR-046, FR-067, T108-T109, T142-T144 |
| Asset containment/type queries | `Contains`, `IsMainAsset`, `IsSubAsset`, `IsForeignAsset`, `IsNativeAsset` | Report source asset, generated artifact, sub-asset, editor-only, and runtime-only ownership | Supported with Nullus-native naming | FR-045, T147-T148 |
| Package import callbacks | `ImportPackage` events | Not part of asset import/Prefab alignment unless Nullus adds a package import UI | Deferred | T192 |
| Version control checkout prompts | `IsOpenForEdit`, checkout integrations | Asset mutability must be represented; provider-specific checkout UI is outside this slice | Deferred | T192 |

## Importer Alignment

| Reference behavior | Reference entry points | Required Nullus behavior | Alignment status | Evidence |
|-----------------------|------------------------|--------------------------|------------------|----------|
| Importer lookup | `AssetImporter.GetAtPath` | Return an editor importer facade for the selected source path | Supported | FR-047, T110-T112 |
| Importer identity and version | Native importer registration, importer version hashes | Include importer ID/version in asset version keys and stale checks | Supported with Nullus-native naming | FR-005, FR-049, T110-T112 |
| Importer settings mutation | serialized importer properties | Persist settings in `.meta` and mark the importer dirty until saved or reimported | Supported with Nullus-native naming | FR-047, T111-T112 |
| Save and reimport | `SaveAndReimport` | Persist settings and force import through the selected importer | Supported | FR-047, T111-T112 |
| Non-blocking import progress | Reference import progress/editor progress behavior | Run imports as cancellable jobs and publish per-phase progress without blocking editor interaction | Supported with Nullus-native naming | FR-076-FR-079, T170-T177 |
| User data | `AssetImporter.userData` | Preserve arbitrary editor user data in metadata without changing runtime artifacts unless configured | Supported | FR-049, T110-T112 |
| Asset pack fields | `packName`, `packVariant` | Store pack-style metadata and feed Nullus asset pack generation | Supported with Nullus-native naming | FR-046, FR-067, T108-T109 |
| External object remaps | `AddRemap`, `RemoveRemap`, `GetExternalObjectMap` | Allow generated materials, textures, avatars, or other importer outputs to be replaced by external asset references | Supported | FR-048, T113-T114 |
| Import diagnostics | `AssetImporterLog`, console warnings/errors | Persist diagnostics by source asset, sub-asset, dependency, importer setting, and platform | Supported with Nullus-native naming | FR-013, FR-050, T110-T112 |
| Postprocessor ordering | `AssetPostprocessor.GetPostprocessOrder` | Run pre/post import hooks in deterministic order | Supported with Nullus-native naming | FR-053, T119-T120 |
| Postprocessor versioning | `AssetPostprocessor.GetVersion` | Add postprocessor versions to asset version keys | Supported with Nullus-native naming | FR-049, FR-053, T119-T120 |
| Pre/post callbacks | `OnPreprocessAsset`, `OnPreprocessTexture`, `OnPostprocessTexture`, `OnPreprocessModel`, `OnPostprocessModel`, `OnPostprocessAllAssets` | Provide equivalent C++/editor hook points for asset, texture, model, and batch import workflows | Supported with Nullus-native naming | FR-053, T119-T120 |
| Dependency declarations | `AssetImportContext` dependency APIs | Allow importers/postprocessors to declare extra source, GUID, path, platform, and artifact dependencies | Supported with Nullus-native naming | FR-006, FR-053, T119-T120 |
| Scripted importers | `scripted importer extension points` | Register custom source extensions that emit artifacts, dependencies, sub-assets, and diagnostics | Supported with Nullus-native naming | FR-054, T121-T122 |

## Model And Texture Import Alignment

| Reference behavior | Reference entry points | Required Nullus behavior | Alignment status | Evidence |
|-----------------------|------------------------|--------------------------|------------------|----------|
| Model scale and unit policy | `ModelImporter.globalScale`, axis/unit conversion settings | Apply scale, axis, and unit conversion before generating `ImportedScene` and model prefab data | Supported with Nullus-native naming | FR-051, T115-T116 |
| Model hierarchy policy | `ModelImporter` hierarchy settings | Preserve or flatten imported nodes according to settings while keeping stable object IDs | Supported with Nullus-native naming | FR-051, T115-T116 |
| Mesh import settings | normals, tangents, UV, optimization settings | Import vertex streams and record unsupported mesh channels as diagnostics | Supported with Nullus-native naming | FR-009, FR-010, FR-051, T115-T116 |
| Material import and search | embedded/external material settings, material remaps, extraction | Generate materials, search/remap external materials, extract generated materials, and record remaps | Supported with Nullus-native naming | FR-048, FR-051, T113-T116 |
| Correct imported material preview | model/material importer outputs and Scene View rendering | Convert glTF/FBX/OBJ materials into Nullus material artifacts used consistently by preview, scene viewport, runtime, and builds; renderer-binding automation and deterministic screenshot evidence are recorded | Supported with Nullus-native naming | FR-080-FR-082, T178-T189 |
| Skeleton, skin, and rig settings | model rig/avatar settings | Preserve skeleton/skin artifacts; emit diagnostics for reference humanoid/avatar features until Nullus animation components exist | Diagnostic-only for humanoid avatar playback | FR-010, FR-024, T065-T067, T115-T116 |
| Animation clips | clip import settings | Preserve animation clip artifacts and map playable clips when runtime animation support exists | Diagnostic-only for playback gaps | FR-009, FR-010, FR-024, T065-T067, T115-T116 |
| Blend shapes | blend shape import settings | Preserve morph target artifacts and map playback when runtime morph support exists | Diagnostic-only for playback gaps | FR-009, FR-010, FR-024, T065-T067 |
| Cameras and lights in models | `ModelImporter.importCameras`, `importLights` | Import or diagnose camera/light nodes according to available Nullus runtime components | Diagnostic-only until components exist | FR-051, T115-T116 |
| glTF/GLB import | Nullus extension beyond reference editor built-in import | Fully import glTF 2.0/GLB through the common scene representation | Supported with Nullus-native naming | FR-008, FR-009, T054-T055 |
| FBX import | reference model import workflow | Convert parser-exposed FBX data through `ImportedScene` and diagnose unsupported channels | Supported with Nullus-native naming | FR-008, FR-010, T059 |
| OBJ import | reference model import workflow | Convert OBJ static mesh, MTL, materials, and textures; diagnose missing animation/skeleton support | Supported with Nullus-native naming | FR-008, FR-011, T058 |
| Texture type | `TextureImporter.textureType` | Persist texture usage intent and feed artifact generation | Supported with Nullus-native naming | FR-052, T117-T118 |
| sRGB and linear import | `TextureImporter.sRGBTexture` | Preserve color-space intent for runtime texture artifacts | Supported | FR-052, T117-T118 |
| Alpha handling | `alphaSource`, `alphaIsTransparency` | Preserve alpha policy and emit diagnostics for unsupported conversions | Supported with Nullus-native naming | FR-052, T117-T118 |
| Mipmaps | `mipmapEnabled`, `mipmapFilter`, fade settings | Generate or skip mipmaps per settings and store runtime sampler metadata | Supported with Nullus-native naming | FR-052, T117-T118 |
| Wrap/filter/sampler | `wrapModeU/V/W`, `filterMode` | Store sampler settings in imported texture artifacts and materials | Supported | FR-052, T117-T118 |
| Compression and max size | `maxTextureSize`, compression quality, platform settings | Store platform override intent and select the best supported Nullus runtime format | Supported with Nullus-native naming | FR-052, T117-T118 |
| Sprite-specific import | sprite mode, slicing, borders, physics shape | Preserve diagnostics until the Nullus 2D sprite pipeline owns these assets | Deferred | T192 |

## Prefab Alignment

| Reference behavior | Reference entry points | Required Nullus behavior | Alignment status | Evidence |
|-----------------------|------------------------|--------------------------|------------------|----------|
| Prefab asset type query | `PrefabUtility.GetPrefabAssetType` | Report not prefab, regular prefab, model prefab, variant, missing asset, and corrupt asset states | Supported | FR-055, T123-T125 |
| Prefab instance status query | `PrefabUtility.GetPrefabInstanceStatus` | Report not prefab, connected, disconnected/unpacked, missing asset, and invalid/corrupt states | Supported | FR-056, T123-T125 |
| Corresponding source lookup | `GetCorrespondingObjectFromSource`, original source variants | Resolve source object, original source object, nearest instance root, and outermost instance root by stable object IDs | Supported with Nullus-native naming | PF-012, PF-013, T154-T155 |
| Save prefab asset | `SaveAsPrefabAsset`, `SavePrefabAsset` | Serialize one root `GameObject` hierarchy into `Nullus.ObjectGraph.Prefab` and import it atomically | Supported with Nullus-native naming | FR-057, PF-011, T126-T127 |
| Save and connect | `SaveAsPrefabAssetAndConnect` | Save source prefab and replace the scene hierarchy with a connected instance in one transaction | Supported with Nullus-native naming | PF-027, T126-T127 |
| Prefab contents editing | `LoadPrefabContents`, `SaveAsPrefabAsset`, `UnloadPrefabContents` | Open isolated Prefab Stage contents, track dirty state, save/discard, and refresh dependents after commit | Supported with Nullus-native naming | FR-064, PF-028, T126-T127 |
| Prefab Stage | `PrefabStage`, `PrefabStageUtility` | Provide isolated editing context for regular prefabs and variants; reject direct generated model prefab edits | Supported with Nullus-native naming | FR-034, FR-064, T079-T080 |
| Property modifications | `PropertyModification`, object override APIs | Record source object ID, instance object ID, property path, base value, local value, and owning prefab layer | Supported with Nullus-native naming | FR-058, FR-065, PF-029, T128-T129 |
| Default overrides | reference root transform/name/layer-style default override handling | Classify default overrides separately so apply/revert UI and tests do not mix them with user changes | Supported with Nullus-native naming | T152-T153 |
| Added/removed components | `ApplyAddedComponent`, `RevertAddedComponent`, removed component APIs | Preserve component IDs/order and support apply/revert | Supported with Nullus-native naming | PF-030, T130-T131 |
| Added/removed child objects | `ApplyAddedGameObject`, `RevertAddedGameObject` | Preserve parent identity, sibling order, and stable object IDs when applying or reverting | Supported with Nullus-native naming | PF-031, T132-T133 |
| Apply/revert levels | single override, object/component group, whole instance | Apply/revert selected override, selected group, or whole instance transactionally | Supported with Nullus-native naming | FR-059, T134-T135 |
| Nested prefabs | nested prefab instances and override chains | Preserve nested asset references, reject cycles, and keep nested override chains independent | Supported | FR-063, T136-T137 |
| Prefab variants | variant base chain and override deltas | Store only override deltas against nearest base; support generated model prefab variants | Supported | FR-062, PF-021, PF-022, T081-T082 |
| Model prefabs | model prefab read-only behavior | Keep generated model prefabs read-only; allow scene overrides, editable variants, and unpacking | Supported | FR-061, PF-034, T140-T141 |
| Missing prefab recovery | missing asset instance status | Preserve scene instance data and override records so references recover when the asset returns | Supported | FR-038, PF-035, T136-T137 |
| Unpack | `PrefabUnpackMode.OutermostRoot` | Detach the outer instance while preserving nested prefab links | Supported | FR-060, PF-032, T138-T139 |
| Unpack completely | `PrefabUnpackMode.Completely` | Detach the instance and all nested prefab links into ordinary scene-owned objects | Supported | FR-060, PF-032, T138-T139 |
| Undo/redo participation | `InteractionMode.UserAction`, editor undo stack | Route editor-invoked operations through Nullus command history | Supported with Nullus-native naming | FR-066, T134-T135, T160, T168-T169 |
| Immutable/prefab-applicability checks | model prefab, immutable prefab, invalid component checks | Reject direct writes to generated or invalid prefab assets and return actionable diagnostics | Supported with Nullus-native naming | FR-037, FR-061, T140-T141 |

## Build Packaging And Runtime Alignment

| Reference behavior | Reference entry points | Required Nullus behavior | Alignment status | Evidence |
|-----------------------|------------------------|--------------------------|------------------|----------|
| Asset pack name/variant grouping | Asset pack build metadata | Build Nullus asset packs from pack-style metadata | Supported with Nullus-native naming | FR-067, T142-T144 |
| Dependency closure | Asset pack dependency build data | Include reachable artifacts and dependencies exactly once | Supported with Nullus-native naming | FR-017, FR-067, T142-T144 |
| Content hashes | Asset pack content hash workflows | Store artifact and pack hashes in runtime manifests | Supported with Nullus-native naming | FR-027, FR-067, T142-T144 |
| Runtime loading | asset pack runtime APIs | Resolve packaged Nullus assets by manifest entry, GUID, sub-asset ID, and loader ID | Supported with Nullus-native naming | FR-016, FR-068, T145-T146 |
| Editor/runtime API separation | editor-only `AssetDatabase` and importer APIs | Reject editor-only asset database/importer calls in runtime builds | Supported | FR-068, T145-T146 |

## Explicit Non-Targets

| Reference behavior | Status | Nullus decision |
|----------------|--------|-----------------|
| Reference source code reuse | Out of scope | Reference source is reference material only. Nullus implementation must be original. |
| Reference editor binary artifact cache compatibility | Out of scope | Nullus writes its own artifact manifests and runtime artifacts. |
| Reference YAML scene/prefab file compatibility | Out of scope | Nullus prefab sources use `Nullus.ObjectGraph.Prefab`. |
| Reference `fileID` bit-exact generation | Out of scope | Nullus uses stable object IDs and sub-asset IDs with equivalent source-to-instance semantics. |
| Reference C# `MonoScript` compilation pipeline | Deferred | This belongs to a scripting-system feature, not this asset import/Prefab slice. |
| External package manager and asset store package import UI | Deferred | Package asset roots are supported; external package distribution UX is separate. |
| Provider-specific version-control checkout UX | Deferred | Nullus tracks read-only/editable roots and may add provider checkout in an editor integration feature. |

## Final Acceptance Status

- Supported: GUID/path lookup, main/sub-asset queries, source asset creation/mutation, refresh batching, search/labels, dependency queries, pack metadata, importer lookup/settings/remaps/postprocessors/scripted importers, glTF/GLB/FBX/OBJ scene conversion, texture import settings, prefab type/status, save/connect, Prefab Stage, variants, nested prefabs, apply/revert, unpack modes, drag/drop, async import progress, runtime manifests, and editor/runtime API separation.
- Supported with Nullus-native naming: public API and serialized file names remain Nullus-native, while workflow semantics are covered by `AssetDatabaseFacadeTests`, `AssetImporterFacadeTests`, `PrefabUtilityFacadeTests`, `EditorAssetDragDropTests`, and the asset pipeline regressions.
- Diagnostic-only: humanoid/avatar playback, animation clip playback gaps, blend shape playback gaps, and model camera/light conversion remain diagnostic-only until the matching runtime components exist.
- Deferred: package import callback UI, provider-specific version control checkout prompts, sprite import/slicing, external package distribution UX, and script compilation.
- Out of scope: reference source reuse, reference binary cache compatibility, reference YAML compatibility, and bit-exact reference `fileID` generation.
- Visual evidence: T189 deterministic DX12 screenshot validation for representative glTF, FBX, and OBJ imported material fixtures is recorded in `quickstart.md`; binding-level automation is covered by `AssetMaterialViewportTests`.
