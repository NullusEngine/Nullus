# Data Model: Editor Resource Management Refactor

## Entities

### EditorResourceRecord

- **Represents**: One logical editor resource entry.
- **Key attributes**:
  - `id`: stable Nullus-style resource ID
  - `category`: brand, font, icon, helper model, shader, preview, or catalog metadata
  - `scope`: editor-only, runtime shared, project/user, or generated
  - `developmentPath`: loose-file path used in development mode
  - `packagedPath`: packaged resource key/path used in release mode
  - `fallbackBehavior`: what happens if the resource is unavailable

### EditorResourceCatalog

- **Represents**: The indexed collection of editor resource records.
- **Relationships**:
  - Contains many `EditorResourceRecord` entries.
  - Serves many editor call sites through lookup by ID.
  - Uses one backend provider at a time.

### EditorResourceBackend

- **Represents**: The active lookup source.
- **Types**:
  - Loose-file backend for development
  - Packaged-resource backend for release

### ThumbnailCacheEntry

- **Represents**: One cached generated preview texture.
- **Key attributes**:
  - `assetId`
  - `thumbnailPath`
  - `sourceSignature`
  - `lastGeneratedTime`

## Rules

- Resource IDs must be stable and not derived from file names.
- Catalog records must not use Unity-branded names in retained entries.
- Thumbnail cache entries must not be treated as static editor icons.
- Runtime/editor asset boundaries must remain explicit in catalog metadata.

