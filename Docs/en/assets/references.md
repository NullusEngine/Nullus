# References, Artifacts, and Caches

The asset system answers three questions: who references what, which data the runtime reads, and when data can be rebuilt. This page covers all three.

## References

The engine has two reference kinds with the same shape but different meaning:

| Kind | Conditions | Use |
| --- | --- | --- |
| In-scene reference | Non-zero `fileID`, empty `guid`, `type = NonAssetType` | Points at an object in the same document |
| Asset reference | Non-zero `fileID`, valid asset `guid`, asset type | Points at a disk asset and its sub-assets |

`filePath` is only a hint. Resolution is by GUID plus sub-asset key, which makes renaming and moving files safe.

Lifecycle ownership uses a separate `$owned` relationship: a scene owns GameObjects and a GameObject owns components. Do not express ownership with an asset reference.

## Artifact manifest

Every imported source asset produces a manifest:

| Field | Notes |
| --- | --- |
| `sourceAssetId` | Source asset ID |
| `importerId` / `importerVersion` | Importer and version that produced the artifacts |
| `targetPlatform` | Platform the artifacts target |
| `primarySubAssetKey` | Primary artifact key |
| `subAssets` | Type, loader, path, content hash, and display name per artifact |
| `dependencies` | Dependency record list |

Artifact types cover `Model`, `Mesh`, `Material`, `Texture`, `Skeleton`, `Skin`, `AnimationClip`, `MorphTarget`, `Prefab`, `Scene`, `Shader`, `Audio`, `UIDocument`, `UIStyleSheet`, `Font`, and `LocalizationTable`.

## Dependency kinds

Dependency records are expressed as a kind plus a value, covering:

- Source file hash, source asset GUID, and path-to-GUID mapping
- Importer version, postprocessor version, and build target
- Imported artifacts and raw package files
- Prefab base, nested Prefab, Prefab override target, and Prefab validation
- Runtime component capability

The first groups decide when a reimport is required; the last decides whether artifacts are usable on the target platform.

## What triggers a reimport

An artifact becomes stale when:

- The source file content hash changes
- The importer or postprocessor version changes
- Import settings change
- The build target changes
- A referenced asset GUID or Prefab base changes
- Runtime component capability is no longer satisfied

The editor reimports only the affected portion. The dependency chain is resolved through manifests rather than by scanning the whole project.

## Runtime loading

The runtime never parses source files; it loads artifacts. Loading passes an authorization check: only artifact paths registered as runtime-authorized can be read by the game process, and unauthorized paths are rejected and logged.

Loading is instrumented, so type, duration, and hit behavior per load can be collected to find startup bottlenecks.

## Caches

Rebuildable intermediate data lives under the project's `Library/` directory:

| Content | Notes |
| --- | --- |
| Artifacts and indexes | Import results and the asset database |
| Thumbnails | Editor preview cache |
| IDE workspace | Project files generated for debuggers and editors |
| `Library/ScriptApi/ScriptApi.json` | Merged scripting API manifest |

`Library/` can be deleted and regenerated, so it is not committed. Only the project file, `Assets/`, and `ProjectSettings/` need to be preserved long term.

## Next steps

- [Reflection and Serialization](../architecture/reflection-serialization.md) for the full object graph and reference rules.
- [Game Runtime and Asset Manifests](../platform-build/runtime.md) for how the runtime consumes these artifacts.
