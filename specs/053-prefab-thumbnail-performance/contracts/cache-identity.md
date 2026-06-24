# Contract: Cache Identity and Invalidation

## Scope

This contract defines the observable identity, invalidation, and capacity behavior expected from caches used by prefab instantiation and thumbnail generation.

## Cache Roles

Each cache role owns a distinct identity and freshness boundary. Cross-role reuse is permitted only through explicit, versioned handoff data.

### PreparedPrefabCache

Purpose: Store successfully prepared prefab data.

Minimum key fields:
- Prefab GUID
- Prefab artifact stamp
- Prefab importer version
- Reflection schema/version
- Serialization format version
- Dependency manifest version

Rules:
- Any key mismatch invalidates the entry.
- Failed or incomplete prepared data must not be served as a cache hit.
- Failed prepared-prefab imports must not be stored in the L1 hot cache or the L2 disk cache.
- JSON debug export may exist, but the runtime fast path must not depend on JSON parsing once a binary artifact is introduced.
- This cache may provide prepared graph/dependency data to preview snapshot building, but it must not own preview textures or runtime scene instances.

### RuntimeResourceCache

Purpose: Store shared mesh, material, texture, shader, and related GPU resources.

Minimum key fields:
- Asset GUID
- SubAssetId
- Artifact stamp
- Resource type

Rules:
- Equal in-flight requests must coalesce.
- Material artifact requests with equivalent resolved paths must share the same pending request.
- Entries expose `Unloaded`, `Loading`, `Ready`, and `Failed`.
- Immutable resources are shared across prefab instantiation and thumbnail preview.
- Failed entries have a finite retry policy.

### PreviewSnapshotCache

Purpose: Store lightweight preview draw items and bounds.

Minimum key fields:
- Prefab/model artifact stamp
- Preview builder version
- Dependency manifest stamp

Rules:
- A preview snapshot must not own runtime scene objects, script instances, or physics objects.
- Bounds and draw items must be invalidated when artifact or dependency freshness changes.
- A preview snapshot may reference prepared prefab and runtime resource identities, but it must not replace either cache's freshness authority.

### ThumbnailTextureCache

Purpose: Store final thumbnail textures in memory for editor display.

Minimum key fields:
- ThumbnailKey

Rules:
- Bounded by max entries or memory.
- Eviction must allow memory usage to fall under budget.
- This cache must not be used as prefab instantiation data.
- This cache owns final display texture lifetime only.

### ThumbnailDiskCache

Purpose: Store final thumbnail images across editor sessions.

Minimum key fields:
- ThumbnailKey

Rules:
- Disk entries are fresh only when all key inputs match.
- Writes must be successful before metadata reports a fresh cache hit.
- Encoding and writes for GPU previews must not block the editor main thread.
- Disk pruning must enforce both max-entry and max-byte budgets and report scanned, removed, and remaining entry/byte counts.
- This cache owns final encoded thumbnail images only.

## Shared Acceptance Rules

- Every cache role exposes hit/miss statistics.
- Every cache role exposes invalidation and cleanup behavior.
- Every cache role has explicit capacity or memory limits.
- Concurrent builds for the same key coalesce where applicable.
- Half-built entries are never served as successful results.
- Stale artifact entries are never served as fresh.
