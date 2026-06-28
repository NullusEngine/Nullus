# Data Model: Unity-Aligned Asset Reuse

## ModelTextureResolutionSettings

Controls automatic texture resolution for one model source asset.

Fields:
- `settingsVersion`: integer, current `1`. Missing values are treated as version `1` for first-phase compatibility.
- `useExternalTextures`: boolean, default `true`. When false, the importer skips external remap/path/name resolution and uses model-local fallback when payload exists.
- `searchByName`: boolean, default `true`. Enables unique project-wide name matching after source path lookup fails.
- `autoImportMissingTextureFiles`: boolean, default `true`. Allows the importer to import a project texture file found by source path when metadata or artifacts are missing.
- `embeddedTextureMode`: enum, default `ModelSubAsset`. Phase one supports `ModelSubAsset`; future modes may extract embedded content to project files.

Validation:
- Unknown boolean text falls back to the documented default and produces no hard import failure.
- Unknown `embeddedTextureMode` falls back to `ModelSubAsset` and reports a warning in the resolution report.
- Settings and remap payloads use percent encoding for values that can contain `%`, `=`, `#`, `;`, `|`, carriage return, newline, or non-ASCII bytes. Malformed encoded values are ignored with diagnostics rather than partially applied.

## TextureSourceKind

Classifies where a model texture reference came from.

Values:
- `ExternalFile`: URI or file path that resolves outside the model container.
- `EmbeddedData`: data URI or importer-provided embedded texture bytes.
- `BufferView`: glTF/GLB buffer view texture payload.
- `Missing`: material references a texture key but the importer could not provide URI or payload information.

## TextureSourceIdentifier

Stable identifier for one texture reference discovered in a model source.

Fields:
- `sourceKey`: importer-provided texture key used by material channels. This remains the key for the phase-one material resource path map.
- `materialTextureKey`: exact `ImportedScene` texture key consumed by material conversion. For current importers this is normally equal to `sourceKey`; it is stored separately so future importers can preserve material channel identity even when the display/source key changes.
- `displayName`: human-readable source name when available.
- `uri`: original source URI when available.
- `normalizedUri`: URI after slash normalization, dot-segment cleanup, percent-decoding only where it is safe for path comparison, and case folding only on case-insensitive filesystem comparisons.
- `bufferViewKey`: importer-provided container key such as `bufferView/3` when applicable.
- `embeddedIndex`: importer-provided embedded texture ordinal when applicable.
- `stableDiscriminator`: importer-provided or importer-derived stable differentiator such as image index, texture index, material texture channel path, FBX embedded texture ID, OBJ MTL map declaration key, or another source-stable ordinal. It must not be based on display order alone when a format-provided identifier exists.
- `kind`: `TextureSourceKind`.
- `stableKey`: versioned deterministic remap key derived by the algorithm below.
- `stableKeyStatus`: `Stable`, `OrderDerived`, or `Insufficient`. `OrderDerived` means the importer had to use scene-order fallback to distinguish otherwise identical references.

Validation:
- `stableKey` must be deterministic across repeated imports of the same source and must not depend on imported artifact paths.
- The stable key algorithm is:
  1. Start with prefix `mtxsrc:v1:`.
  2. Append `kind=<kind>`.
  3. Append `source=<sourceKey>` when `sourceKey` is non-empty.
  4. Append `uri=<normalizedUri>` for external file/data URI sources when available.
  5. Append `bufferView=<bufferViewKey>` for buffer-view sources when available.
  6. Append `embedded=<embeddedIndex>` for embedded sources when available.
  7. Append `discriminator=<stableDiscriminator>` when present.
  8. Append `name=<displayName>` only when no `sourceKey`, `normalizedUri`, `bufferViewKey`, `embeddedIndex`, or `stableDiscriminator` is available.
  9. Percent-encode every component value before joining with `;`.
- If two discovered texture references in the same model produce the same base `stableKey`, append the most stable available differentiator in this order: format image/texture index, container ordinal, `materialTextureKey`, material channel path, then importer-provided source-stable ordinal. Only when none exists may the implementation append `;dup=<zero-based occurrence>` ordered by first appearance in the imported scene.
- Any key that uses order-only `dup=<n>` must set `stableKeyStatus=OrderDerived`, emit `model-texture-source-key-order-derived`, and never silently reuse an existing explicit remap without showing the order-derived warning in Asset Properties and the import report.
- Empty `sourceKey` entries are allowed in reports and remaps if a URI, buffer view, embedded index, or display name can produce a stable key. They are ignored for material path map generation unless `materialTextureKey` is non-empty.

## ExternalTextureRemap

User-authored binding from a `TextureSourceIdentifier` to a project texture asset.

Fields:
- `sourceStableKey`: stable source identifier key.
- `targetAssetId`: target texture asset GUID.
- `targetSubAssetKey`: target sub-asset key, defaulting to the texture primary artifact when empty.
- `targetEditorPath`: optional user-facing project path cached for display.

Validation:
- Target asset must exist in project roots.
- Target asset type must be Texture.
- Invalid targets produce warnings and do not block fallback resolution.
- Remap lookup uses `sourceStableKey`; applying a remap must not mutate `sourceKey` or `materialTextureKey`.

## ResolvedTextureReference

Authoritative result for one source texture reference during model import.

Fields:
- `source`: `TextureSourceIdentifier`.
- `resolutionKind`: `ExplicitRemap`, `SourcePath`, `NameSearch`, `ModelEmbeddedFallback`, `Missing`, or `Invalid`.
- `materialTextureKey`: exact material conversion lookup key copied from `source.materialTextureKey`.
- `targetAssetId`: valid when resolved to a project texture asset.
- `targetSubAssetKey`: valid when resolved to a project texture artifact.
- `resourcePath`: material-compatible texture artifact path for phase one.
- `modelSubAssetKey`: populated only for model-local fallback.
- `diagnostics`: zero or more warning/error records.

Validation:
- Project texture resolutions must have a valid target asset and non-empty resource path.
- Model-local fallback must have a model texture payload and model sub-asset key.
- Ambiguous name matches must not select a target automatically.
- The material resource path map must be keyed by `materialTextureKey`, not by display name or stable remap key. This preserves glTF base color/normal texture slots, FBX diffuse/opacity slots, and OBJ MTL texture references that share names but have distinct importer texture keys.

## ModelTextureResolutionReport

Persisted report for Asset Properties and debugging.

Fields:
- `reportVersion`: integer, current `1`.
- `modelAssetId`: source model asset GUID.
- `targetPlatform`: import target platform.
- `importerVersion`: model importer version that produced the report.
- `settingsFingerprint`: deterministic fingerprint of model texture resolution settings and remaps used for the import.
- `entries`: ordered list of `ResolvedTextureReference` display records.
- `summary`: counts by resolution kind and warning/error count.

Validation:
- The report reflects the last committed import result, not unsaved UI edits or a failed import attempt.
- Report writes are atomic: write to a temporary file under the pending artifact root, validate the serialized header, then move/replace it only as part of the successful model artifact commit.
- Asset Properties must hide a report when `modelAssetId`, `targetPlatform`, `importerVersion`, `settingsFingerprint`, or `reportVersion` do not match the selected model/current import context.
- Report parse failure should hide the report UI and never block model preview.

## Import Dependency Records

Dependencies that make a model stale when reused textures change.

Records:
- `SourceAssetGuid`: `value=<texture-guid>`, `hashOrVersion=<texture-meta-version-or-content-hash>`. Records that a resolved external texture asset identity and its import settings participate in model freshness.
- `ImportedArtifact`: `value=<texture-guid>#<sub-asset-key>@<targetPlatform>`, `hashOrVersion=<artifact-content-hash-or-manifest-version>`. Records the concrete texture artifact used by material paths.
- `PathToGuidMapping`: `value=<resolution-scope>|<normalized-query>|<match-mode>`, `hashOrVersion=<mapping-fingerprint>`. Records path or name lookup results, including negative and ambiguous name-search results. The `value` fields and canonical fingerprint fields must percent-encode `|`, `;`, `%`, `=`, `#`, carriage return, newline, and non-ASCII bytes before joining.

Validation:
- Dependencies are deduplicated.
- Missing or invalid external targets are reported as diagnostics rather than recorded as successful dependencies.
- A model becomes stale when a target texture file/meta changes, a target texture artifact changes, a source path maps to a different GUID, a previously missing path gains a texture asset, or a unique-name candidate set changes from zero/one/many candidates.
- `PathToGuidMapping.hashOrVersion` is the hash of a canonical candidate list with one row per viable, missing, or rejected candidate:
  `rootIndex|normalizedProjectPath|assetGuid|assetType|importedState|caseFoldPolicy|caseFoldedName|artifactSubAssetKey|artifactHashOrVersion`.
  The list is sorted by root order, normalized path, then GUID. Fingerprints must change when the candidate count stays the same but any GUID, path, asset type, imported/unimported state, case policy, or artifact identity changes.

## Unique Name Search Scope

Defines the deterministic project-wide name match used after source-path lookup fails.

Rules:
- Search only configured project asset roots.
- Candidate assets must have texture `AssetType`; model sub-assets and non-texture assets are excluded.
- Match by filename including extension first. If the source has no extension, match by stem only.
- Case-sensitive filesystems compare case-sensitively; case-insensitive filesystems compare folded names but report case collisions as ambiguous.
- Imported texture assets and unimported texture files are candidates only when `autoImportMissingTextureFiles` is enabled for the latter.
- Candidate ordering is deterministic by root order, then normalized project-relative path, then GUID. Exactly one viable candidate binds; zero candidates falls through; more than one viable candidate reports `model-texture-name-ambiguous` and does not bind.
- Matching never uses content hashes.

## Automatic Texture Import Request

Represents a bounded request to import a project texture file discovered while importing a model.

Fields:
- `textureEditorPath`: project-relative path to the discovered texture file.
- `expectedAssetType`: always Texture.
- `requestedByModelAssetId`: source model asset GUID for diagnostics.

Validation:
- Requests must be limited to files inside configured project asset roots.
- Requests must not import model assets recursively.
- Failed requests produce `model-texture-auto-import-failed` diagnostics and fall back to later resolution steps where possible.
