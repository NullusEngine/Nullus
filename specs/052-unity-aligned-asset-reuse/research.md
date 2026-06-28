# Research: Unity-Aligned Asset Reuse

## Decision: Reuse by stable asset identity, not by content hash

**Rationale**: Unity 2018.4 uses `.meta` GUIDs and local file IDs as the durable identity model. Model importer remaps bind source identifiers to external objects, while imported material references resolve through those external assets. This matches Nullus' existing `AssetMeta.id`, `subAssetKey`, and artifact manifest model.

This is also the project quality benchmark target in `C:/Users/Chenyang/.codex/skills/plan-review/benchmarks/game_asset_pipelines.md`: industrial asset pipelines keep stable GUID/sub-asset identity distinct from source paths and generated artifacts, and they record importer settings, dependencies, target platform, and artifact identity for incremental refresh.

**Alternatives considered**:
- Global content-addressed dedup: rejected for phase one because identical texture bytes can have different import settings, color space, compression target, or user intent.
- Per-import exact payload aliasing: deferred because it reduces disk writes but does not solve cross-model identity and can blur source ownership.

## Decision: Use versioned stable texture source keys for remaps

**Rationale**: Unity external object remaps survive reimport because they are keyed by importer-discovered source identifiers, not by generated artifact paths. Nullus needs the same property for model texture references, including glTF data URIs, GLB buffer views, FBX embedded textures, OBJ MTL paths, and importers that provide empty or unstable display names. A versioned key (`mtxsrc:v1`) gives us room to evolve the algorithm while keeping old remaps interpretable.

**Chosen algorithm**: Derive the base key from `TextureSourceKind`, importer `sourceKey`, normalized URI, buffer view key, embedded ordinal, and a stable discriminator such as format image/texture index or material channel path, then percent-encode components. `displayName` is used only when none of those identity fields exist. If two references collide within the same model, prefer a format-stable differentiator; order-derived `dup=<n>` is a last resort and is reported as `OrderDerived` so old remaps are not silently trusted.

**Alternatives considered**:
- Use only importer `sourceKey`: rejected because some importers can expose empty keys or keys that do not distinguish embedded/container sources enough for user remaps.
- Use only URI/name: rejected because embedded data, buffer views, duplicate material slots, and renamed display names would collide or drift.
- Use content hash: rejected because remap identity should describe source reference intent, not texture bytes.

## Decision: Texture resolution priority is explicit remap, source path, unique name, model-local fallback

**Rationale**: This mirrors Unity's user-authorable external object map first, then automatic search heuristics, while preserving deterministic behavior when candidates are ambiguous. Source path resolution should use existing editor asset root helpers so project-relative and absolute paths remain portable.

Unique-name search is deliberately narrow: it searches texture assets inside configured project roots, matches filename including extension first or stem only when the source lacks an extension, orders candidates deterministically by root/path/GUID, and binds only when exactly one viable texture candidate exists. Unimported files participate only when automatic texture import is enabled. Matching never uses content hash.

**Alternatives considered**:
- Name search before path: rejected because path carries stronger source intent.
- Auto-pick first name match: rejected because multiple project textures with the same filename are common and silent selection would be hard to debug.

## Decision: Store model texture remaps in model `.meta` settings

**Rationale**: `AssetMeta.settings` already persists importer settings and is loaded by the import facade before reimport. Storing remaps there aligns with Unity's `.meta` importer remap behavior without adding another sidecar format.

**Alternatives considered**:
- Store remaps only in generated manifests: rejected because generated artifacts should not be the authoring source for user choices.
- Store remaps in a new project database: rejected because the remap belongs to the model importer and should travel with the model source.

## Decision: Keep material artifact texture uniforms path-compatible in phase one

**Rationale**: Existing material conversion and loading use resolved texture resource paths. Rewriting material payloads to carry strong `AssetId + subAssetKey` references would touch material loaders and runtime resource resolution beyond the duplicate model texture problem. The manifest and remap settings still preserve identity and dependency information.

The path-compatible map must be keyed by the exact `ImportedScene` texture key used by material channels (`materialTextureKey`), while remaps are keyed by `sourceStableKey`. Keeping these two keys separate prevents glTF base color/normal, FBX diffuse/opacity, and OBJ MTL references from accidentally using display names or stable remap keys where the material converter expects source texture keys.

**Alternatives considered**:
- Strong asset reference uniforms immediately: deferred because it is valuable but larger than this phase and would require a material/resource contract migration.

## Decision: Persist a texture resolution report for editor visibility

**Rationale**: Asset Properties needs to show the last imported resolution result without reparsing all model source data on every UI draw. A small report artifact or metadata entry can record discovered source references, resolution kind, target identity, target resource path, and diagnostics.

The report is editor-only and lives beside the committed model artifacts. It is atomically replaced only after a successful import commit and includes `modelAssetId`, `targetPlatform`, importer version, settings/remap fingerprint, and report version so Asset Properties can hide stale or malformed data instead of showing a misleading result.

**Alternatives considered**:
- Recompute resolution live in Asset Properties: rejected because it duplicates importer logic and could show results that do not match the last committed import.
- Show only manifest sub-assets: insufficient because externally reused textures disappear from model-owned sub-assets and still need to be explainable.
- Store report as a runtime manifest sub-asset: rejected because runtime loading does not need editor diagnostics and stale UI reports should not affect packaged asset contracts.

## Decision: Record dependency freshness for texture identity, artifact, and lookup mappings

**Rationale**: Source-path and name-search reuse are only safe if model imports become stale when their target texture asset, target texture artifact, or path/name candidate set changes. Nullus already has `AssetDependencyRecord` with `SourceAssetGuid`, `ImportedArtifact`, and `PathToGuidMapping`; this feature should populate those records with explicit value/hash schemas rather than inventing a new dependency type.

**Chosen schema**:
- `SourceAssetGuid`: `value=<texture-guid>`, `hashOrVersion=<texture-meta-version-or-content-hash>`.
- `ImportedArtifact`: `value=<texture-guid>#<sub-asset-key>@<targetPlatform>`, `hashOrVersion=<artifact-content-hash-or-manifest-version>`.
- `PathToGuidMapping`: `value=<resolution-scope>|<normalized-query>|<match-mode>`, `hashOrVersion=<mapping-fingerprint>`, including zero/one/many candidate sets. The fingerprint is a hash of canonical candidate rows containing root index, normalized path, GUID, asset type, imported/unimported state, case-fold policy, folded name, artifact sub-asset key, and artifact hash/version.

**Alternatives considered**:
- Only depend on resolved texture artifacts: rejected because a path could later point to a different texture GUID or an ambiguous name set could become unique without changing the old artifact.
- Only depend on source file paths: rejected because texture import settings/artifact changes would not re-stale the model material reference.

## Decision: Add new focused resolver/report files instead of expanding importer and UI panels

**Rationale**: `ExternalAssetImporter.cpp` and `AssetProperties.cpp` are already large orchestration files. Keeping texture matching and report serialization in focused units makes tests easier and limits future churn.

**Alternatives considered**:
- Implement all matching in `ExternalAssetImporter.cpp`: rejected for maintainability.
- Implement UI-only matching: rejected because import output must be authoritative and testable headlessly.

## Decision: Validation centers on asset pipeline unit tests

**Rationale**: Existing `AssetImportPipelineTests.cpp` already covers external model import, material texture uniforms, model texture artifacts, manifest behavior, path resolution, and diagnostics. This feature is an editor asset pipeline change, not a renderer backend change.

**Alternatives considered**:
- RenderDoc validation: not required because no graphics backend or frame rendering behavior is changed.
- Manual-only validation: rejected because the resolver and importer behavior must be regression-tested.

## Decision: Automatic missing texture import is texture-only and non-fatal

**Rationale**: A model import may discover a project texture file before that texture has a current artifact. The importer may request a texture import for that file, but it must not recursively import arbitrary asset types or turn a texture import failure into a whole-model failure when model-local fallback or a missing-texture diagnostic is sufficient.

**Alternatives considered**:
- Recursively import any referenced asset type: rejected because this feature only resolves model texture references and should not expand import graph behavior.
- Fail the whole model import when automatic texture import fails: rejected because Unity-style model import should preserve diagnostics and fallback paths when possible.
