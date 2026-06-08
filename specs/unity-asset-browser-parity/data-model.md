# Data Model: Unity Asset Browser Parity

## Project Folder Selection

- **Purpose**: Represents the selected project folder that drives the tree highlight, breadcrumb, and right grid.
- **Fields**:
  - `projectRelativePath`: normalized path rooted at `Assets/`.
  - `absolutePath`: resolved path under the active project root.
  - `exists`: whether the folder still exists after refresh.
- **Validation**:
  - Must stay under the active project root.
  - Must resolve to `Assets/` or a descendant folder.
  - Must not point into `Library/` or Engine asset roots.

## Asset Browser Item

- **Purpose**: A user-facing grid entry.
- **Fields**:
  - `displayName`
  - `sourceAssetPath`
  - `absolutePath` for source-backed files/folders
  - `itemKind`: folder, source asset, generated sub-asset
  - `assetType`: folder, model, prefab, mesh, material, texture, shader, scene, script, generic file
  - `assetId`
  - `subAssetKey`
  - `dragPayload`
  - `thumbnailRequest`
  - `readOnlyGenerated`
- **Relationships**:
  - Generated sub-assets belong to a source asset.
  - Selection may target Asset Properties, Asset View, Scene View, or Prefab Stage depending on type.
- **Validation**:
  - Meta files and Library artifacts are excluded.
  - Generated sub-assets require database records and a valid source association.

## Asset Type Filter

- **Purpose**: Narrows visible current-folder items.
- **Values**:
  - All
  - Folder
  - Model
  - Prefab
  - Mesh
  - Material
  - Texture
  - Shader
  - Scene
  - Script
  - Other supported file
- **Validation**:
  - Filtering applies only to the current folder result set in v1.

## Thumbnail Request

- **Purpose**: Describes the desired preview image for one grid item.
- **Fields**:
  - `assetId`
  - `sourceAssetPath`
  - `subAssetKey`
  - `thumbnailKind`: icon, texture, material sphere, model preview, prefab preview
  - `requestedSize`
  - `freshnessInputs`
- **Validation**:
  - Requests must not write outside `Library/AssetThumbnails/`.
  - Requests for unsupported assets resolve to icon fallback.

## Thumbnail Cache Entry

- **Purpose**: A persisted generated thumbnail and freshness metadata.
- **Fields**:
  - `cacheKey`
  - `imagePath`
  - `metadataPath`
  - `generatedAt`
  - `freshnessInputs`
  - `status`: fresh, stale, missing, failed
- **Validation**:
  - Cache key includes asset identity, sub-asset key, source/meta/artifact freshness, thumbnail kind, and settings.
  - Stale entries are not presented as fresh after source or artifact changes.

## Generated Sub-Asset Entry

- **Purpose**: A grid item backed by an imported artifact record.
- **Fields**:
  - `displayName`
  - `sourceAssetPath`
  - `subAssetKey`
  - `artifactType`
  - `assetId`
  - `dragResourcePath`
  - `generatedReadOnly`
- **Validation**:
  - Must be discoverable from the project asset database.
  - Must retain drag payload compatibility with object references and scene drops.
