# Feature Specification: Prefab and Thumbnail Performance

**Feature Branch**: `053-prefab-thumbnail-performance`  
**Created**: 2026-06-18  
**Status**: Draft  
**Input**: User description: "Optimize Nullus Engine current slow prefab instantiation and asset thumbnail generation flows. First identify real bottlenecks with profiling and repeatable benchmarks, then improve prepared prefab reuse, prefab graph instantiation, resource loading, preview thumbnail generation, GPU readback scheduling, cache design, and tests without changing prefab semantics."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Diagnose Real Prefab and Thumbnail Bottlenecks (Priority: P1)

An engine developer or editor maintainer can run repeatable profiling scenarios for prefab instantiation and asset thumbnail generation, then receive a report that ranks the actual time spent in each important stage before any optimization is claimed.

**Why this priority**: Without measured bottlenecks, additional caching or refactoring can hide the real problem and risk regressions.

**Independent Test**: Can be tested by running the profiling benchmarks for small, medium, large, nested, repeated, cold-cache, hot-cache, and thumbnail request scenarios, then verifying that the report includes per-stage timing, counts, cache hit data, synchronization data, and bottleneck rankings.

**Acceptance Scenarios**:

1. **Given** a prefab benchmark suite containing small, 100 object, 1000 object, multi-renderer, nested, repeated, resource cold-cache, and resource hot-cache cases, **When** the benchmark is run, **Then** the report shows total time, main-thread time, background-thread time, object counts, component counts, renderer counts, dependency counts, cache hit rates, synchronous resource loads, GPU upload bytes, and the top bottleneck stages.
2. **Given** a thumbnail benchmark suite containing first-generation, memory-cache hit, disk-cache hit, and rapid Project Browser scrolling cases, **When** the benchmark is run, **Then** the report shows cache lookup cost, preview preparation cost, render submission cost, fence wait cost, readback cost, encode/write cost, duplicate request count, queue depth/backlog, in-flight request count by priority, cancellation latency, coalescing pressure, per-frame thumbnail generation count, and the top bottleneck stages.
3. **Given** no profiling data has been collected for a run, **When** an optimization summary is requested, **Then** the system reports missing evidence instead of asserting that performance improved.

---

### User Story 2 - Keep Prefab Instantiation Fast Without Changing Semantics (Priority: P2)

An editor user repeatedly instantiates prefabs, including nested prefabs with overrides and external resource references, and sees faster hot-cache behavior while object identity, overrides, internal references, external asset handles, renderer registration, physics registration, script registration, and lifecycle behavior remain unchanged.

**Why this priority**: Prefab instantiation is a core editor workflow; performance must improve without breaking authored content.

**Independent Test**: Can be tested by comparing baseline and optimized runs for repeated prefab instantiation, editor restart L2 cache hits, changed artifact stamps, changed dependency stamps, changed reflection schema, deferred activation, nested overrides, and shared external asset handles.

**Acceptance Scenarios**:

1. **Given** the same prefab is instantiated repeatedly in one editor session, **When** the benchmark is run after warm-up, **Then** repeated instantiation avoids unnecessary source parsing, dependency manifest rebuilding, path resolution, and synchronous resource loading while preserving visible behavior.
2. **Given** an editor restart with a valid prepared prefab cache, **When** the same prefab is instantiated, **Then** the prepared data is reused only if all cache identity and freshness inputs match.
3. **Given** a prefab artifact stamp, dependency stamp, reflection schema version, importer version, serialization format version, or dependency manifest version changes, **When** the prefab is instantiated, **Then** stale prepared data is rejected and rebuilt before use.
4. **Given** a nested prefab with overrides and internal/external references, **When** it is instantiated through the optimized path, **Then** overrides, internal reference fixups, shared external asset handles, renderer registration, physics registration, script registration, and lifecycle invocation match the baseline behavior.

---

### User Story 3 - Generate Thumbnails Without Blocking Editor Interaction (Priority: P3)

An editor user scrolls the Project Browser or selects prefab/model assets and receives placeholders or cached thumbnails immediately while new thumbnails are prepared, rendered, read back, encoded, and stored asynchronously within per-frame budgets.

**Why this priority**: Thumbnail generation is user-facing and can make the editor feel frozen if GPU fences, readback, encoding, or disk writes block the main thread.

**Independent Test**: Can be tested by simulating rapid Project Browser scrolling, duplicate thumbnail requests, cache hits, pending GPU work, cancellation, renderer/resource failure, and editor shutdown while validating that the main thread does not wait for GPU thumbnail completion.

**Acceptance Scenarios**:

1. **Given** multiple visible Project Browser items request the same thumbnail key, **When** generation is already queued or running, **Then** the system coalesces the request and performs the work at most once for that key.
2. **Given** a thumbnail has been submitted to the GPU but the fence is not complete, **When** the editor frame continues, **Then** the main thread does not block waiting for the fence and the UI keeps showing a placeholder, previous thumbnail, or asset type icon.
3. **Given** an item scrolls out of the visible area before work starts, **When** cancellation is processed, **Then** queued thumbnail work is cancelled or deprioritized without accumulating unbounded tasks.
4. **Given** a prefab or model thumbnail is generated, **When** the preview pipeline runs, **Then** it does not execute gameplay scripts, physics simulation, audio registration, navigation registration, full scene registration, full post-processing, or lifecycle callbacks that are unnecessary for preview rendering.
5. **Given** thumbnail rendering succeeds, **When** disk caching is needed, **Then** readback, encoding, and cache writes occur asynchronously or within the active thumbnail generation budget and do not synchronously stall editor interaction.

---

### User Story 4 - Share Artifact and Resource Caches Without Mixing Responsibilities (Priority: P4)

An engine maintainer can inspect and tune separate caches for prepared prefab data, runtime resources, preview snapshots, in-memory thumbnail textures, and disk thumbnails, with explicit keys, versions, stamps, capacity limits, invalidation rules, coalescing, failure behavior, and debug statistics.

**Why this priority**: Prefab instantiation and thumbnail generation should share expensive prepared artifacts and resources, but thumbnail caches must not become hidden prefab instantiation caches.

**Independent Test**: Can be tested by exercising cache hit, miss, invalidation, capacity eviction, failed build, concurrent request coalescing, retry limit, and debug statistic scenarios for each cache role.

**Acceptance Scenarios**:

1. **Given** a prefab is used for both instantiation and thumbnail generation, **When** prepared data and dependencies are valid, **Then** both systems reuse the same prepared artifact and resource cache entries while keeping scene instances, script instances, physics objects, and preview texture lifetimes separate.
2. **Given** a resource request for the same mesh, material, texture, or shader is already loading, **When** another system requests it, **Then** the request is coalesced rather than decoding, loading, or uploading the same resource twice.
3. **Given** any cache entry is incomplete, failed, or stale, **When** the cache is queried, **Then** the incomplete result is not served as a success and any retry obeys a finite policy.
4. **Given** cache capacity is exceeded, **When** eviction runs, **Then** memory usage can fall back below the configured budget without invalidating unrelated live editor behavior.

### Edge Cases

- Prefab source exists but artifact payload is missing, malformed, stale, or from an unsupported format version.
- Prepared prefab data is present on disk but one freshness input differs.
- Reflection schema changes after prepared prefab data was generated.
- Dependency manifest contains missing, failed, renamed, or reimported mesh/material/texture/shader assets.
- Nested prefab overrides refer to objects, components, fields, or external assets that were changed or removed.
- Prefab graph has zero renderers, many renderers, deep transform hierarchy, wide transform hierarchy, or invalid transform data.
- Resource loading is cold, hot, already in flight, failed, cancelled, or evicted.
- GPU upload budget is exhausted before all preview resources are ready.
- Thumbnail request key changes because asset stamp, material/shader dependency stamp, preview settings, resolution, color space, or renderer version changes.
- Thumbnail render produces an empty frame, oversized bounds, tiny bounds, invalid bounds, unsupported shader, missing mesh, missing material, or fallback material.
- GPU readback fence is not complete for multiple frames.
- Encoding or disk write fails after render success.
- Project Browser rapidly changes visible set while generation, readback, encoding, or disk writes are in progress.
- Editor shuts down while thumbnail or resource work is queued, running, waiting for GPU, readback, encoding, or writing.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST expose named profiling stages for prefab artifact loading, prepared cache loading, prepared parsing, dependency resolution, object allocation, component deserialization, internal reference fixup, external reference binding, renderer registration, physics registration, script registration, lifecycle invocation, resource waiting, GPU resource upload, and total instantiation.
- **FR-002**: The system MUST expose named profiling stages for thumbnail cache lookup, preview asset preparation, preview instance creation, bounds calculation, preview resource preparation, preview render recording, preview render submission, preview fence waiting, preview readback, preview encoding, preview cache storage, and total thumbnail generation.
- **FR-003**: Profiling statistics MUST include total time, main-thread time, background-thread time, call count, object count, component count, renderer count, dependency count, cache hit/miss counts, synchronous resource load count, GPU upload bytes, fence wait time, duplicate thumbnail request count, queue depth/backlog, in-flight request count by priority, cancellation latency, coalescing pressure, and thumbnails generated per frame where applicable.
- **FR-004**: Performance tests MUST cover small prefabs, 100 object prefabs, 1000 object prefabs, multi-renderer prefabs, nested prefabs, repeated same-prefab instantiation, editor restart prepared-cache hits, resource cold cache, resource hot cache, first thumbnail generation, memory thumbnail cache hits, disk thumbnail cache hits, and rapid Project Browser request cancellation/deduplication.
- **FR-005**: Every optimization report MUST compare against a collected baseline for the same scenario before claiming improvement.
- **FR-005A**: The diagnostic phase MUST be completed as a read-only measurement gate before any behavior-changing optimization is accepted.
- **FR-006**: Prepared prefab cache entries MUST have explicit key fields covering prefab identity, artifact stamp, importer version, reflection schema version, serialization format version, and dependency manifest version.
- **FR-007**: Prepared prefab cache entries MUST be reused only when all key and freshness inputs match.
- **FR-008**: Prepared prefab caches MUST store only successfully built results and MUST reject incomplete, failed, or stale data.
- **FR-009**: The optimized prefab instantiation flow MUST preserve existing prefab reference semantics, nested prefab behavior, overrides, internal references, external resource references, renderer behavior, physics behavior, script behavior, and lifecycle behavior.
- **FR-010**: Prefab graph creation MUST NOT synchronously wait for nonessential mesh, material, texture, shader, or GPU upload readiness by default.
- **FR-010A**: Editor scene restore and committed prefab drops MUST synchronously block until the prefab graph and required visible renderer dependencies are ready; graph-only or partially loaded prefab instances MUST NOT be committed for these user-visible operations.
- **FR-010B**: Drag hover previews, thumbnail previews, and background prewarm MAY keep using graph-only or deferred-resource policies when they do not commit scene objects.
- **FR-010C**: Loading a scene or committing a large prefab drop MUST publish user-visible progress while the editor blocks for synchronous prefab graph and renderer dependency readiness.
- **FR-011**: Repeated instantiation of the same prefab MUST share immutable external mesh, material, texture, and shader resources instead of loading, decoding, or uploading duplicate copies.
- **FR-012**: Thumbnail generation MUST NOT block the editor main thread waiting for GPU preview fences.
- **FR-013**: Thumbnail generation MUST deduplicate concurrent requests for the same thumbnail key.
- **FR-014**: Thumbnail generation MUST prioritize currently visible Project Browser items over prefetch and background items.
- **FR-015**: Thumbnail generation MUST be cancellable or deprioritizable for queued work that is no longer visible.
- **FR-016**: Thumbnail generation MUST use an explicit thumbnail generation budget configuration for CPU preparation, preview render count, GPU upload bytes, readback count, and cache write count, and tests MUST be able to inject deterministic budget values.
- **FR-017**: Prefab and model thumbnail preview MUST avoid executing gameplay scripts, physics registration/simulation, audio registration, navigation registration, unnecessary lifecycle callbacks, and full runtime scene registration unless explicitly required for preview correctness.
- **FR-018**: Preview rendering MUST reuse long-lived preview world, camera, lighting, render target, depth target, descriptor, command, shader, and pipeline resources where compatible.
- **FR-019**: Thumbnail cache keys MUST include asset identity, asset artifact stamp, preview renderer version, preview settings hash, material/shader dependency stamp, thumbnail resolution, and color/HDR mode when relevant.
- **FR-020**: Thumbnail disk cache writes and image encoding MUST not run synchronously on the main thread for GPU preview thumbnails.
- **FR-021**: Runtime resource cache requests MUST be coalesced for identical in-flight asset requests and MUST distinguish unloaded, loading, ready, and failed states.
- **FR-022**: Each cache role MUST expose capacity limits, hit/miss statistics, invalidation behavior, concurrent build coalescing, finite retry policy for failures, cleanup behavior, and debug inspection.
- **FR-022A**: Each cache role MUST own a distinct identity and freshness boundary; cross-role reuse is allowed only through explicit versioned handoff data, never by serving one cache role's entry as another role's authoritative result.
- **FR-023**: Thumbnail caches MUST NOT be used as the authority for prefab instantiation or prepared prefab reuse.
- **FR-024**: Background threads MUST NOT directly create or mutate engine objects that require main-thread access.
- **FR-025**: No generated reflection files under `Runtime/*/Gen/` may be hand-edited as part of this feature.
- **FR-026**: Tests MUST verify cache invalidation for artifact stamp changes, dependency stamp changes, reflection schema changes, failed prepared builds, thumbnail key stamp changes, material/shader changes, capacity limits, cancellation, editor shutdown, and non-blocking GPU work.

### Key Entities *(include if feature involves data)*

- **Prefab Performance Sample**: A measured record for one prefab instantiation scenario. It contains scenario identity, stage timings, thread timing split, object/component/renderer/dependency counts, cache outcomes, synchronous resource loads, GPU upload bytes, and diagnostic flags.
- **Thumbnail Performance Sample**: A measured record for one thumbnail scenario. It contains scenario identity, stage timings, cache outcomes, duplicate request counts, generated-per-frame counts, fence wait time, readback/encode/write activity, and diagnostic flags.
- **Prepared Prefab Entry**: Prepared prefab data that can be reused across instantiation or preview work only when its key, version, stamp, dependency, and schema inputs match.
- **Runtime Resource Entry**: Shared runtime resource data for meshes, materials, textures, shaders, and related GPU resources with explicit state, stamp, ownership, capacity cost, and retry behavior.
- **Preview Snapshot**: Lightweight preview-ready renderable information, including bounds and draw items, derived from prepared asset data without requiring a full runtime scene instance.
- **Thumbnail Task**: A scheduled thumbnail request with key, state, priority, cancellation status, resource readiness, render/readback/encode/write progress, retry count, and result.
- **Thumbnail Cache Entry**: In-memory or disk thumbnail output keyed by preview identity and freshness inputs, with capacity cost, freshness state, and statistics.
- **Diagnostic Report**: A run summary that ranks stage costs and records whether the evidence is a baseline, optimized run, or invalid comparison.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every required prefab and thumbnail stage appears in profiling output for at least one covered benchmark scenario.
- **SC-002**: Each covered benchmark scenario reports total time, thread timing split, call counts, relevant object/resource counts, cache hit data, synchronization data, and diagnostics without relying on source-code string inspection.
- **SC-003**: The diagnostic report ranks the top five bottleneck stages for prefab instantiation and thumbnail generation from measured data.
- **SC-004**: For every performance improvement claim, the report includes a baseline value, optimized value, and percentage change for the same scenario; missing-baseline and mismatched-scenario comparisons are reported as invalid.
- **SC-005**: Hot-cache prefab instantiation avoids prefab source parsing, resource path resolution, dependency manifest rebuilding, and synchronous resource loading in the measured hot-cache scenarios.
- **SC-006**: Repeated prefab instantiation shares external mesh, material, texture, and shader handles in 100% of relevant compatibility tests.
- **SC-007**: Prefab graph creation no longer waits for nonessential GPU upload completion in covered tests.
- **SC-007A**: Scene restore and final prefab drop tests show committed scene objects are created only after required mesh/material/texture artifacts are ready, with no graph-only committed fallback when dependencies are missing.
- **SC-007B**: Scene loading and large prefab drop tests observe progress events during synchronous prefab readiness work.
- **SC-008**: GPU preview thumbnail generation records zero main-thread blocking fence waits in the non-complete fence test.
- **SC-009**: Concurrent thumbnail requests for the same key result in exactly one generating task in the deduplication test.
- **SC-010**: Rapid scrolling of 500 asset requests stays within the injected per-frame thumbnail CPU, render, readback, and write budget values in the benchmark report.
- **SC-011**: Thumbnail generation does not execute script lifecycle, physics, audio, navigation, full runtime scene registration, or full post-processing logic in 100% of preview-mode tests.
- **SC-012**: Preview world and render target reuse are observed across repeated thumbnail generations in the reuse tests.
- **SC-013**: Cache invalidation tests pass for artifact stamp, dependency stamp, reflection schema, material/shader stamp, preview settings, resolution, and color/HDR changes.
- **SC-014**: Cache capacity tests show memory usage returning below the configured limit after eviction.
- **SC-015**: Editor shutdown tests complete without leaving running thumbnail, readback, encode, write, or resource tasks.
- **SC-016**: Phase 1 diagnostic tests can pass without changing prefab or thumbnail behavior beyond emitting measurement data.

## Assumptions

- The first implementation increment is diagnostic: it adds profiling scopes, statistics, and repeatable benchmarks before large performance refactors.
- Optimization work is staged behind measurement gates. P0 starts with the single largest measured bottleneck, then each additional P0 item requires its own baseline and compatibility evidence.
- Existing JSON prepared prefab cache remains compatible while a versioned binary prepared prefab artifact is designed and introduced later.
- The first profiling report may show that some hypothesized bottlenecks are smaller than expected; later optimization tasks will follow the measured ranking.
- Tests use stable automated checks for behavior and counters, not brittle source-code grep assertions.
- Render backend correctness is not inferred across backends unless each backend is explicitly validated.
- Existing user changes in the worktree are preserved and not reverted by this feature.
