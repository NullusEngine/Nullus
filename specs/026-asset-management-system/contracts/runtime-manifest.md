# Contract: Runtime Manifest

## Manifest Inputs

- Selected root scenes or prefabs.
- Current source asset database.
- Committed artifact manifests for the target platform.
- Dependency graph for source assets, imported artifacts, prefabs, and sub-assets.

## Manifest Outputs

- Runtime manifest with schema version and target platform.
- Entries mapping asset GUID/sub-asset ID to artifact path, artifact type, loader ID, content hash, and dependency list.
- Prefab graph entries for runtime prefab instantiation.
- Build diagnostics for missing, stale, failed, or platform-incompatible artifacts.

## Required Behaviors

- Dependency closure starts from selected scenes or prefabs and includes every reachable artifact exactly once.
- The build must fail when a required source asset has no successful committed artifact for the target platform.
- Source-only files such as `.gltf`, `.glb`, `.fbx`, `.obj`, `.meta`, and editor cache databases must be excluded unless explicitly marked as raw packaged files.
- Runtime resolution must use GUID/sub-asset ID and manifest entries, not absolute project paths.
- Manifest entries must include enough loader information for runtime systems to choose the correct asset loader without editor importer code.
- Prefab entries must include graph artifact path, base prefab dependencies, nested prefab dependencies, and referenced asset artifacts.
- Content hashes must allow runtime validation of missing or mismatched artifact files.
- Existing path-based resource manager fallback may remain during migration but must not be the authoritative runtime build path for new asset references.
