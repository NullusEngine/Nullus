# Data Model: Unity-Aligned Sub-Asset Expansion

## Editor Asset Snapshot

Represents one source asset and its generated children.

- Canonical source path
- Authoritative `AssetId`
- Ordered sub-asset snapshots

Validation: canonical path must be a valid `Assets` descendant; source keys are unique; child identity is `AssetId + subAssetKey`.

## Editor Asset Snapshot Index

Immutable indexed snapshot view.

- Status: `Valid` or `Error`
- Stable diagnostic
- Ordered source snapshots
- Canonical-source-path to snapshot index map

Invariants:

- `Valid` has a clear diagnostic and internally consistent vector/map content.
- `Error` has a nonempty diagnostic and empty payload/index.
- Unknown status values are invalid.
- Index content exactly references the ordered payload.

## Facade Published State

One coherent facade publication.

- Immutable artifact manifest view
- Known-current canonical paths
- Immutable snapshot index

Identity rules:

- Snapshot-index pointer is reused only when status, diagnostic, ordered payload, and index content are equal.
- Full state pointer is reused only when manifests, known-current paths, and snapshot index are all semantically unchanged.
- A replacement facade always has a distinct state identity.

State transition:

```text
base state -> build/validate candidate off-lock -> compare base under lock
  equal    -> publish candidate
  changed  -> discard candidate -> unlock -> coalesce rebuild from latest state
```

## Asset Browser Root Snapshot

Immutable ordered current-folder roots plus the exact facade state used to derive them.

Validation: nonnull state; every `AssetBrowserItem` field participates in semantic equality; roots and current facade state must match before scheduling.

## Expansion and Filter Snapshots

- Expansion: exact set of canonical expanded source keys.
- Filter: normalized query and valid item-type filter.

Identity changes only on semantic content changes. Invalid enum values fail the build key.

## Presentation Build Key

Tuple of root, facade, expansion, and filter shared views.

Invariants:

- All views and the nested snapshot index are nonnull.
- Root state identity equals key facade state identity.
- Snapshot status is valid.
- Filter enum is within range.

## Presentation Bundle

One atomic actionable presentation.

- Monotonic nonzero epoch
- Ordered display items with exact child counts and group IDs
- Structured action-key to display-index map
- Thumbnail root indices

Invariants: group IDs are nonzero only for children; action keys are unique; indices are in range; thumbnail roots are bundle-owned; no partial prefix is publishable. Epoch zero is invalid; `uint64_t::max()` is a fail-closed exhaustion boundary and never wraps.

## Action Key

Structured UI/action identity.

- Subject kind: folder, source, or generated child
- Canonical source path
- `AssetId` for asset-backed subjects
- Optional sub-asset key with engagement included in equality

An old key never rebinds to a new `AssetId` at the same path.

## Presentation Coordinator State

Main-thread state for asynchronous bundle publication.

- Current semantic key
- Status: `Idle`, `Loading`, `Success`, `Failure`, or `Closed`
- Active immutable bundle, if successful
- Stable diagnostic, if failed
- Retained retry identity object
- One active future and at most one latest queued key

Transitions:

```text
semantic input change -> Loading(new key, no active bundle)
current success       -> Success(key, bundle, clear diagnostic)
current failure       -> Failure(key, no bundle, diagnostic, retained retry identity)
stale completion      -> no publication; launch only revalidated latest key
explicit retry        -> Loading(same key, newly allocated retry identity)
unchanged failed key  -> no resubmission
epoch exhaustion      -> Failure(key, no bundle, PresentationEpochExhausted)
teardown              -> Closed (terminal)
```

Retry identity uses retained object identity, not a numeric counter, so it cannot wrap or alias an older attempt while stale work retains the old object. Actions and thumbnails are disabled in every state except `Success`.

## Thumbnail Work Key

Identity retained by thumbnail generation, decode, and upload work.

- Active bundle identity and epoch
- Authoritative `AssetId`
- Optional sub-asset key
- Requested size
- Immutable complete `AssetThumbnailRequest` view retained at scheduling
- `BuildAssetThumbnailCacheKey(request)` captured at scheduling
- Normalized image path

The worker performs public `EvaluateAssetThumbnailCache(scheduledRequest, AssetThumbnailCacheIntegrityMode::Full)` and returns that evaluation with its result. The main-thread pump never performs Full evaluation or image hashing. It rebuilds the current authoritative request for the active item, requires its public `BuildAssetThumbnailCacheKey` output to equal the scheduled key, requires the worker evaluation to be `Fresh`, and rechecks bundle/action/cache-key identity immediately before binding. `Stale`, `Missing`, or `Failed` rejects the completion. Ordering/comparison of freshness inputs remains internal to existing cache APIs; no private helper or parallel freshness algorithm is exposed. A path-only byte/texture cache is not actionable identity.

## Picker Cache

Main-thread-only cache bound to a facade state.

- Retained `PickerCacheLifetime` identity owned by one provider/AssetBrowser lifetime
- Source facade state
- Status: `Loading`, `Success`, `Failure`, or `Closed`
- Entries
- Diagnostic

Transitions:

```text
facade change -> loading(current state, empty entries)
snapshot Error -> failure(current state, empty entries, diagnostic)
current success -> success(current state, entries)
current failure -> failure(current state, empty entries, diagnostic)
stale completion -> no transition
owner A teardown -> Closed(lifetime A, empty non-actionable cache)
owner B provider install -> new lifetime B in Loading, never reopen lifetime A
```

`Closed` is terminal for one cache instance/lifetime; all entry/diagnostic mutation attempts for that lifetime are ignored and entry getters return empty. Installing a new provider constructs a fresh cache instance with a new retained lifetime identity. Worker/result publication requires both cache lifetime and source facade state to match, so owner A completion cannot affect owner B even when both observed the same facade state.

## Group Segment

Pure draw geometry for one maximal contiguous child run.

- Group ID
- First/last visible item indices
- Minimum/maximum bounds
- True segment-start/end outer-edge flags
- Corner flags

Grid segments cannot cross row boundaries; each row boundary creates a true segment outer edge and therefore a rounded grid edge even when the same group continues on the next row. List segments represent only the visible group intersection; viewport clipping does not create a true group outer edge, so clipped continuation edges remain square. All drawing intersects the active content clip.
