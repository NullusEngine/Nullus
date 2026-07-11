# Research: Unity-Aligned Sub-Asset Expansion

## Decision 1: Match Composite-Asset Semantics, Not Unity Internals

**Decision**: Treat each source as a presentation group whose children come only from its current validated snapshot. Keep `AssetId + subAssetKey` authoritative; canonical project paths group UI items.

**Rationale**: Unity 2018.4 `AssetDatabase`/`AssetImporter`, Unreal Asset Registry, and Godot ResourceUID/import remaps all separate stable identity from source location. Count, membership, actions, and thumbnails must describe the same current composite asset.

**Alternatives considered**: Path-authoritative identity fails rename/replacement cases. `hasGeneratedSubAssets` is capability, not existence evidence. Displaying every artifact type creates unsupported actions. Copying Unity internals would conflict with Nullus manifests and runtime identity.

## Decision 2: Publish One Immutable Facade State

**Decision**: Publish immutable manifest view, known-current paths, and indexed asset/sub-asset snapshots through one `FacadePublishedState`. Expose only `GetPublishedState()` to migrated consumers.

**Rationale**: One retained pointer is a coherent read transaction and generation identity. It prevents G1/G2 mixtures and lets malformed data publish explicit `Error` with diagnostic instead of preserving stale valid entries.

**Alternatives considered**: Independent locked getters can observe different generations. Mutable shared containers require longer reader locks. Counters/hashes add restart, wrap, ABA, or collision concerns. Last-good fallback leaves obsolete identities actionable.

## Decision 3: Build Off-Lock and Compare-and-Publish

**Decision**: Candidate work and exact semantic comparison run outside `m_manifestMutex` against a retained base state. The lock covers only base-pointer equality and pointer commit. A mismatch copies the latest pointer, unlocks, and coalesces one rebuild.

**Rationale**: Existing facade refresh already separates expensive freshness work from publication. Adding base-identity validation closes its lost-update window while keeping lock cost `O(1)`.

**Alternatives considered**: Locking the full rebuild blocks readers. Unconditional off-lock publication loses concurrent updates. Three independent atomic pointers lose aggregate transactionality. Content hashes add maintenance/collision state without replacing exact comparison.

## Decision 4: Use Pure Single-Flight Builders

**Decision**: Build presentation and picker results from immutable inputs on background workers. Each coordinator has one active job and one replaceable latest key/state. Completion revalidates all input identities.

**Rationale**: The presentation build is `O(r + s + n + B)` and can allocate off the UI thread. Single-flight bounds memory/scheduler pressure; one-shot bundle publication keeps display, actions, and thumbnails coherent.

**Alternatives considered**: UI-thread rebuilds can stall large projects. One job per event is unbounded. Progressive child prefixes publish inconsistent count/action/thumbnail state. Cooperative cancellation is deferred because it adds shared state and shutdown ordering while latest-key coalescing already bounds work.

## Decision 5: Make Failure and Picker State Explicit

**Decision**: Use factory-created success/failure results with stable diagnostics. Bind picker cache to `{lifetime, sourceState, status, entries, diagnostic}` and invalidate it synchronously on facade changes. Each provider/AssetBrowser owner creates a fresh retained lifetime; closed owners reject late completion even if a replacement owner observes the same facade state. Scheduling is nonthrowing and retries only on explicit request or semantic change.

**Rationale**: Empty success cannot distinguish no matches from validation or scheduler failure. State binding closes the G2-published/G1-cache window seen by `ReflectedPropertyDrawer`.

**Alternatives considered**: Empty vectors erase diagnostics. Exceptions crossing UI/provider APIs make cancellation inconsistent. Retaining entries during refresh permits obsolete selection. Per-frame retry creates allocation/exception loops under persistent rejection.

## Decision 6: Publish Structured Actions in One Bundle

**Decision**: Publish display items, `{subjectKind, canonicalSourcePath, AssetId, optionalSubAssetKey}` action index, thumbnail roots, and epoch together. Thumbnail work additionally retains bundle/action identity, immutable `AssetThumbnailRequest`, and its public `BuildAssetThumbnailCacheKey`; the worker performs public Full `EvaluateAssetThumbnailCache`, while the main-thread pump only revalidates current bundle/action/cache-key identity and accepts worker status `Fresh`.

**Rationale**: Structured keys cannot be confused by delimiters in valid paths/keys, and `AssetId` prevents same-path replacement rebinding. Bundle/asset/cache-key identity plus existing cache evaluation rejects stale drawn or completed thumbnail work without duplicating the cache module's freshness algorithm.

**Alternatives considered**: Delimiter strings are ambiguous. Separate publication permits mixed state. Path-only selection is not authoritative.

## Decision 7: Draw Segment-Level Backgrounds

**Decision**: Resolve maximal same-group segments per grid row or visible list slice and draw one intersect-clipped fill per segment.

**Rationale**: UnityCsReference's `Editor/Mono/ProjectBrowser/ProjectBrowser.cs` delegates list/grid content to `ObjectListArea`, implemented in `Editor/Mono/ObjectListArea.cs`; these are the concrete Unity Project-window layout/draw surfaces being behaviorally matched. Nullus does not copy their IMGUI internals, but preserves the observed Project-window convention that related sub-assets read as one source-owned visual group in list and grid layouts. Per-child rounded rectangles cannot cover internal layout gaps and create independent antialias edges. Segment fills remain linear and preserve item-local hover/selection overlays.

**Alternatives considered**: Overlapping per-item fills mask rather than remove seams. A general ImGui layout framework is unnecessary scope.

## Decision 8: Reuse Existing Test and Job Infrastructure

**Decision**: Extend `NullusUnitTests`, use `NLS_ENABLE_TEST_HOOKS`, fake scheduler injection, RAII ImGui contexts, and the existing `Editor` build target.

**Rationale**: Existing unit tests link `NLS_UI` and `EditorProject`; no new executable is needed. Deterministic hooks and fake scheduling prove races and failures without timing dependence.

**Alternatives considered**: Real global JobSystem ordering tests are nondeterministic. Screenshot-only geometry tests cannot prove clipping or internal pixel continuity. New test binaries duplicate current responsibilities.
