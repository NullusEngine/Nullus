# Research: Editor Resource Management Refactor

## Decision 1: Use one logical resource catalog with interchangeable backends

- **Decision**: Introduce a single editor resource catalog that resolves stable IDs to resources through a backend interface.
- **Rationale**: The editor needs the same IDs in development and release, while the backing storage changes from loose files to packaged resources.
- **Alternatives considered**:
  - Direct filesystem access from callers: rejected because it hard-codes `App/Assets` assumptions into every consumer.
  - Separate development and release APIs: rejected because it duplicates resource identity and increases migration cost.

## Decision 2: Resolve resource roots from the executable location

- **Decision**: Derive editor/runtime asset roots from the executable installation area, not the current working directory.
- **Rationale**: The editor is started from different launchers and IDEs, so CWD-based resolution is fragile.
- **Alternatives considered**:
  - Keep `../Assets` relative paths: rejected because they depend on launch context.
  - Use process CWD with fallback: rejected because it still fails in packaged and embedded launch scenarios.

## Decision 3: Keep thumbnails outside the static icon catalog

- **Decision**: Treat previewable asset thumbnails as separate cacheable textures, while static editor icons stay in the catalog.
- **Rationale**: Thumbnails have invalidation and generation behavior that differs from fixed UI assets.
- **Alternatives considered**:
  - Store previews in the same icon tree: rejected because previews are generated artifacts, not fixed resources.
  - Rebuild previews every time: rejected because it increases startup and browsing cost.

## Decision 4: Retain only the editor icons that current UI actually uses

- **Decision**: Prune unused legacy icon files and keep a curated Nullus-named set.
- **Rationale**: The current icon directory is too large and carries Unity-branded names that do not belong in the long-term resource model.
- **Alternatives considered**:
  - Keep the whole imported Unity set for safety: rejected because it undermines the cleanup goal and makes ownership unclear.
  - Rename files in place without pruning: rejected because the tree would still be bloated and confusing.

