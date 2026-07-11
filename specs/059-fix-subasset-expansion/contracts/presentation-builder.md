# Contract: Asset Browser Presentation Builder

## Inputs

One immutable key containing root, facade, expansion, and filter views. Every pointer, nested index, status, filter value, and root/facade identity is validated before reading items.

## Output

Factory-created success or failure result. Success owns exactly one complete bundle; failure owns no actionable bundle and carries a stable diagnostic.

## Projection

- Supported children: Prefab, Mesh, Material, Texture, Shader.
- Unsupported and sentinel artifact values emit nothing and affect no count.
- Query and type predicates combine with logical AND per candidate.
- Source match without a matching child emits the source with count zero.
- Child match without source match emits the source as context, sets count to matching eligible children, and emits those children only when expanded.
- Source and child match emits the source, matching-child count, and matching children only when expanded.
- Neither match emits no source and no children.
- Collapsed groups scan children for exact context/count but emit no child rows.
- Missing valid snapshot means zero proven children.

## Coordinator

- One active build and one latest queued key.
- Nonthrowing submission returns either one tracking future or none.
- Stale success/failure/exception never publishes.
- Any semantic key change synchronously clears the old actionable bundle and enters loading for the latest key.
- Current failure clears bundle/actions/thumbnails, stores its diagnostic and failed key, and does not retry that unchanged key while the retained retry identity is unchanged.
- Explicit retry creates a new retained retry identity object; semantic key change creates a new loading state. Retry identity is never numeric and cannot wrap or alias an older retained attempt.
- Current success moves one bundle and assigns one nonzero epoch.
- If the current epoch is `uint64_t::max()`, publication fails closed with `PresentationEpochExhausted`, clears actionable state, and never wraps; recreating the panel establishes a new lifetime with no retained old bundle identity.
- Shutdown cannot publish after panel teardown.

Coordinator states are `Idle`, `Loading`, `Success`, `Failure`, and `Closed`, carrying `{key, activeBundle, diagnostic, retryIdentity}` as applicable. `Closed` is terminal.

## Consumer Safety

Actions resolve only through structured keys into the active bundle. Thumbnails use active bundle roots/visible items with matching epoch. Hidden or stale items are ineligible.

Every generated thumbnail/decode/upload request carries `ThumbnailWorkKey {bundleIdentity, epoch, AssetId, optionalSubAssetKey, requestSize, immutableAssetThumbnailRequest, cacheKeyAtSchedule, normalizedImagePath}`. `cacheKeyAtSchedule` comes from public `BuildAssetThumbnailCacheKey(request)`. The worker performs public `EvaluateAssetThumbnailCache(scheduledRequest, AssetThumbnailCacheIntegrityMode::Full)` and returns its evaluation. The main-thread pump performs no Full evaluation/file hash; it rebuilds the current authoritative request, requires the public cache key plus bundle/action identity to match, requires worker status `Fresh`, and repeats those identity checks immediately before binding. `Stale`, `Missing`, and `Failed` reject the completion. Freshness-input ordering/comparison remains inside the existing cache module; no private helper or second algorithm is exposed. A path-only decode cache may retain reusable bytes, but it cannot publish/bind them to a replacement asset unless the full work key still matches.
