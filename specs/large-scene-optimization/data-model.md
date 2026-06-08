# Data Model: Large Scene Optimization

## LargeSceneSettings

Configures feature gates and budgets.

**Fields**:

- `enableSpatialIndex`: Enables static/dynamic spatial candidate queries.
- `enableParallelVisibility`: Allows JobSystem-backed visibility ranges.
- `enableLOD`: Enables per-object LOD selection.
- `enableHLOD`: Enables cluster proxy selection and child suppression.
- `enableHZBOcclusion`: Enables HZB/history occlusion when backend support exists.
- `maxVisibilityJobs`: Upper bound for per-view visibility jobs.
- `parallelVisibilityPrimitiveThreshold`: Primitive threshold for auto parallel visibility.
- `parallelVisibilityPrimitivesPerTask`: Primitive batch size for visibility worker tasks.
- `staticRebuildDirtyRatio`: Dirty/topology threshold for static index rebuild.
- `streamingCpuBudgetUs`: Per-frame CPU commit budget.
- `streamingGpuUploadBudgetBytes`: Per-frame GPU upload budget.
- `streamingIoBudgetBytes`: Per-frame IO budget.
- `streamingCpuMemoryBudgetBytes`: Resident CPU-side memory budget.
- `streamingGpuMemoryBudgetBytes`: Resident GPU-side memory budget.
- `maxOcclusionHistoryAge`: Maximum frame age for occlusion history reuse.

**Validation Rules**:

- Disabled feature gates must fall back to the existing retained render-scene behavior.
- Budgets must be named settings and surfaced in telemetry.
- Backend-gated settings must not enable unsupported GPU paths silently.

## ScenePrimitiveHandle

Stable identifier for a retained primitive.

**Fields**:

- `sceneId`: Renderer scene identity when primitives from multiple retained scenes are aggregated for a view.
- `index`: Slot index into renderer-owned primitive storage.
- `generation`: Incremented when a slot is reused.

**Validation Rules**:

- A handle is valid only when both index and generation match a non-tombstoned slot.
- A handle is valid only inside its owning `RenderScene`; aggregate views must carry `{sceneId, handle}` pairs.
- Parallel jobs may store handles and immutable snapshots, not raw component pointers that can be destroyed concurrently.
- Dense iteration order is stored in snapshot arrays and must never be used as handle identity.

## ScenePrimitiveSlot

Slot-map storage entry for retained primitive records.

**Fields**:

- `generation`: Current generation for this slot.
- `record`: Optional primitive record.
- `occupied`: Whether the slot currently owns a live record.
- `tombstoned`: Whether old handles must be rejected.
- `nextFree`: Free-list link when unoccupied.

**Validation Rules**:

- Removing a primitive tombstones the slot and increments or reserves a new generation before reuse.
- Erasing a lower-index primitive must not change the identity of any other live handle.
- Spatial nodes, HLOD clusters, streaming cells, and job snapshots must validate generation before dereferencing.

## ScenePrimitiveRecord

Persistent renderer-side primitive record.

**Fields**:

- `handle`: Stable `ScenePrimitiveHandle`.
- `meshRenderer`: Source component pointer used only during synchronization.
- `owner`: Source game object pointer used only during synchronization.
- `mesh`: Resolved mesh resource pointer.
- `materials`: Resolved material/resource state for draw command slots.
- `localBounds`: Mesh or custom local bounds.
- `worldBounds`: Cached world-space bounds for spatial and visibility tests.
- `frustumBehaviour`: Disabled, model, mesh, or custom culling mode.
- `layerMask`: Render/editor visibility layers.
- `staticness`: Static, dynamic, or editor-forced-dynamic classification.
- `lodGroup`: Optional LOD group handle.
- `hlodCluster`: Optional HLOD cluster handle.
- `streamingCell`: Optional streaming cell handle.
- `commandSlots`: Cached draw command slots.
- `resourceState`: Ready, fallback, missing mesh, invalid material, or not resident.
- `visibilitySettings`: Layer, min draw distance, max draw distance, and distance-culling enablement for the primitive.
- `syncSerial`: Last synchronization serial.
- `syncDirtySerial`: Last source-scene dirty serial consumed by synchronization.
- `boundsSerial`: Last bounds update serial.
- `commandOffsetRange`: Stable cached command-offset range used by queue finalization.

**State Transitions**:

- `Unregistered` -> `Registered`: Component is observed during scene sync.
- `Registered` -> `Ready`: Mesh, material, bounds, and command slots are valid.
- `Ready` -> `DirtyBounds`: Transform or custom bounds changed.
- `Ready` -> `DirtyCommand`: Mesh/material/render-state inputs changed.
- `Ready` -> `NotResident`: Required resource is evicted or pending.
- `Any` -> `Removed`: Component no longer exists or owner is destroyed.

## SceneSpatialIndex

Renderer-owned acceleration structure for primitive bounds.

**Fields**:

- `staticNodes`: Cache-friendly tree, binned BVH, or equivalent static primitive nodes.
- `dynamicCells`: Loose grid, dynamic tree, or equivalent dynamic primitive buckets.
- `lastGoodStaticNodes`: Previous queryable static structure used while a rebuild is in progress.
- `dirtyOverlayHandles`: Dirty static handles queried alongside the last-good index during bounded rebuilds.
- `staticPrimitiveHandles`: Primitive handles owned by static nodes.
- `dynamicPrimitiveHandles`: Primitive handles owned by dynamic buckets.
- `layerBuckets`: Optional per-layer handle sets or bitsets used to avoid full scans for layer filtering.
- `distanceBuckets`: Optional coarse distance-band metadata used to avoid full scans for distance filtering.
- `activeBits`: Maintained active/live primitive bitset.
- `dirtyStaticHandles`: Static handles requiring refit or rebuild.
- `dirtyDynamicHandles`: Dynamic handles requiring reinsert.
- `rebuildSerial`: Serial incremented after topology rebuild.
- `rebuildBudgetUs`: CPU time budget for incremental rebuild work.
- `rebuildInProgress`: Whether an async or staged rebuild is active.
- `lastFallbackReason`: Reason full scan or degraded query was used.

**Validation Rules**:

- Static and dynamic records must not share mutable node storage.
- Queries must return generation-checked handles.
- Localized views in partitioned scenes must not scan every primitive.
- Dynamic-heavy localized views must not scan every dynamic primitive unless debug comparison mode is active.
- Static rebuilds must use last-good data plus dirty overlays, staged rebuilds, or explicit budgeted fallback reporting.
- Query telemetry must report primitive records touched and visibility-tested primitive counts, not only candidate counts.
- Debug full-scan comparison must be available in tests.

## ScenePrimitiveSnapshot

Immutable frame-local view of retained primitive records.

**Fields**:

- `snapshotSerial`: Unique serial for the snapshot.
- `sceneId`: Owning retained scene identity.
- `frameSerial`: Frame that produced the snapshot.
- `primitiveRecords`: Compact immutable records needed by visibility and representation selection.
- `handleToDenseIndex`: Mapping from generation-checked handles to dense snapshot indices.
- `denseIndexToHandle`: Reverse mapping for serial/parallel range iteration.
- `dirtySyncHandles`: Handles changed since the previous sync snapshot.
- `removedHandles`: Tombstoned handles observed this frame.
- `liveHandleBits`: Bitset of live snapshot handles.
- `commandOffsetTable`: Primitive-handle to cached draw-command offset ranges.
- `visiblePrimitiveScratch`: Reusable sparse output storage for candidate and visible handles.
- `memoryArenaSerial`: Arena or pool serial used to prove snapshot memory lifetime.

**Validation Rules**:

- Visibility jobs and queue finalization consume snapshots, not mutable `RenderScene` arrays.
- Stable frames should reuse snapshot memory, report live source-scene sweep fallback cost explicitly, and keep downstream spatial-index maintenance plus queue finalization dirty/sparse.
- Removing a primitive while jobs are in flight is safe because handles are generation-checked and snapshot memory lives until frame retirement.
- Full live-scene sweeps are allowed only for baseline/debug fallback and must be reported in telemetry.

## PrimitiveVisibilitySettings

Per-primitive non-geometric visibility inputs.

**Fields**:

- `layer`: Layer copied from `GameObject::GetLayer()` during synchronization.
- `distanceCullingEnabled`: Whether min/max draw distance is active.
- `minDrawDistance`: Optional minimum camera distance for visibility.
- `maxDrawDistance`: Optional maximum camera distance for visibility.
- `editorForceVisible`: View-debug or selection override hint.

**Validation Rules**:

- Layer must be clamped to the engine's 0-31 layer range.
- Distance culling is disabled unless explicit min/max distances are configured.
- Negative or inverted distance ranges must be rejected or normalized with diagnostics before use.
- Editor force-visible state is view-local and must not mutate runtime culling data.

## VisibilityFrameInput

Immutable per-view visibility input.

**Fields**:

- `viewId`: Stable identifier for Scene View, Game View, shadow view, or test view.
- `frameSerial`: Renderer frame serial.
- `cameraPosition`: View origin.
- `viewProjection`: View-projection matrix.
- `frustum`: Frustum planes.
- `viewportExtent`: Width and height.
- `viewportOrigin`: Viewport origin in render-target coordinates.
- `renderScale`: Render-scale factor applied to the depth and HZB resources.
- `projectionJitter`: Temporal jitter value and compatibility policy for history reuse.
- `layerMask`: View layer filter.
- `lodBias`: Per-view LOD bias.
- `allowHLOD`: Whether this view may suppress children through HLOD.
- `selectionOverride`: Optional selected primitive handles that must remain inspectable.
- `occlusionHistoryKey`: Key for compatible HZB/history reuse.
- `streamingInterestMode`: Runtime, editor, selection, or validation.

**Validation Rules**:

- The input must be immutable while visibility jobs run.
- Camera cut, projection change, incompatible projection jitter, viewport extent/origin change, render-scale change, near/far plane change, reversed-Z/depth convention change, depth-format change, or MSAA sample-count change must produce an occlusion-incompatible key.
- If the camera or editor view does not supply a mask, the input must use an explicit all-layers default rather than an uninitialized mask.

## OcclusionHistoryKey

Compatibility key for conservative HZB/history reuse.

**Fields**:

- `viewId`: View identity for Scene View, Game View, shadow view, or test view.
- `viewProjectionStableHash`: Hash of the projection state after applying the selected jitter compatibility policy.
- `viewportExtent`: Width and height.
- `viewportOrigin`: Origin in render-target coordinates.
- `renderScale`: Render-scale factor.
- `nearPlane` and `farPlane`: Depth range parameters.
- `depthConvention`: Normal-Z or reversed-Z.
- `depthFormat`: Depth resource format.
- `sampleCount`: MSAA sample count.
- `depthResourceSerial`: Backend depth texture identity serial.
- `hzbResourceSerial`: HZB texture identity serial.
- `backendSerial`: Backend/device reset serial.
- `primitiveBoundsGeneration`: Generation of the occludee bounds/transform used by the previous test.
- `representationId`: Selected LOD/HLOD/proxy representation used by the previous test.
- `occludeeEligibilitySerial`: Serial covering material depth-write, transparency, overlay/debug, and editor-gizmo eligibility.

**Validation Rules**:

- Any field mismatch makes history incompatible unless a documented jitter policy explicitly allows reuse.
- Moving an occludee or occluder, changing bounds, switching LOD/HLOD, or changing material depth-write eligibility must force conservative visibility until new history is produced.
- History keys are per-view and must not be shared between Scene View, Game View, shadow views, or validation views unless every compatibility field matches.

## VisibilityCandidateSet

Intermediate output from spatial and coarse filters.

**Fields**:

- `candidatePrimitiveBits`: Candidate primitive bitset.
- `candidatePrimitiveHandles`: Optional dense list for cache-friendly iteration.
- `source`: Full scan, static index, dynamic index, or merged index.
- `candidateCount`: Number of candidates.
- `queryTimeNs`: Query timing.

**Validation Rules**:

- Candidate handles must be generation-checked before use.
- Full-scan source is allowed only when feature flags or test comparison require it.

## VisibilityFrameResult

Per-view visibility output.

**Fields**:

- `primitiveBits`: Registered primitive visibility bitset.
- `meshBits`: Mesh/draw-slot visibility bitset.
- `visiblePrimitiveHandles`: Sparse visible primitive handles in deterministic submission-prep order.
- `eligibleCommandHandles`: Sparse command handles or primitive-command ranges eligible for finalization.
- `selectedLOD`: Per-primitive selected LOD index.
- `activeHLODClusters`: Cluster handles active for the view.
- `suppressedByHLOD`: Child primitive bitset suppressed by active clusters.
- `occludedBits`: Primitive bitset rejected by occlusion.
- `cullReasons`: Per-primitive dominant cull reason for debug output.
- `commandEligibilityBits`: Draw-command slots eligible for `RenderScene` queue finalization.
- `finalizationTouchedPrimitiveCount`: Number of primitive records touched by queue finalization.
- `finalizationTouchedCommandCount`: Number of command records touched by queue finalization.
- `representationInputs`: Selected LOD/HLOD representation decisions consumed by queue finalization.
- `telemetry`: Counts and timing for this result.

**Validation Rules**:

- Serial and parallel paths must produce identical result fields for deterministic test inputs.
- Transparent queue ordering must remain back-to-front after filtering.
- Large-scene queue finalization must consume sparse handle/command lists or command offset tables instead of rescanning all registered primitives.
- Missing debug reason must be treated as a test failure for culled primitives in debug mode.
- `RenderScene`, not the visibility pipeline, owns final queue sorting, dynamic instancing, and object-index assignment.

## CullReason

Dominant reason a primitive is absent from the submitted scene queue.

**Values**:

- `Visible`
- `Inactive`
- `LayerMasked`
- `DistanceCulled`
- `SpatialMiss`
- `FrustumCulled`
- `LODInactive`
- `HLODChildSuppressed`
- `Occluded`
- `NotResident`
- `MissingMesh`
- `InvalidMaterial`
- `BackendUnsupported`

**Validation Rules**:

- `Visible` is used only for submitted or intentionally represented primitives.
- `Occluded` is never used when occlusion history is invalid.
- `NotResident` must emit streaming interest unless the resource is intentionally disabled.

## LODGroupRecord

Per-object LOD selection data.

**Fields**:

- `groupHandle`: Stable LOD group handle.
- `primitiveHandles`: Member primitives.
- `thresholds`: Screen-relative thresholds from highest to lowest detail.
- `fadeDurations`: Reserved optional transition durations; metadata only in this slice.
- `hysteresis`: Threshold stability band.
- `worldReferencePoint`: Bounds center or authored reference.
- `worldSize`: Bounds-derived or authored size.
- `forcedLOD`: Optional editor or runtime override.

**Validation Rules**:

- Selection chooses one active LOD.
- Fade windows are not rendered in this slice and must not double-submit LODs until a later cross-fade contract lands.
- LOD state is view-local.

## HLODClusterRecord

Cluster-level representation data.

**Fields**:

- `clusterHandle`: Stable HLOD cluster handle.
- `childPrimitives`: Member primitive handles.
- `proxyPrimitive`: Optional proxy primitive handle.
- `clusterBounds`: World or cell-local cluster bounds.
- `activationThreshold`: Screen-relative or distance threshold.
- `fadePolicy`: None, dither, or temporal fade.
- `residency`: Proxy resource residency state.
- `source`: Imported hierarchy, authored group, or generated cluster.
- `compatibilityFlags`: Whether children are opaque-only, transparent, order-dependent, skinned, animated, editor-only, or proxy-safe.
- `transparentChildPolicy`: Forbid proxy suppression by default unless the cluster is explicitly order-safe.

**Validation Rules**:

- Proxy activation suppresses children only in the active view result.
- Transparent or order-dependent children are not suppressed by HLOD unless compatibility flags explicitly prove the proxy is safe for that view.
- Selected children can override suppression in editor inspection views.
- Missing proxy residency falls back to children or lower-detail placeholder, never synchronous load.

## RepresentationResidencySnapshot

Read-only resource readiness snapshot used by LOD/HLOD before full streaming is implemented.

**Fields**:

- `readyPrimitiveResources`: Primitive handles whose mesh/material command inputs are render-ready.
- `readyHLODProxyResources`: HLOD proxy handles whose proxy resources are render-ready.
- `fallbackPrimitiveResources`: Primitive handles that can render with placeholders.
- `notResidentResources`: Handles that must request streaming interest.

**Validation Rules**:

- This snapshot is read-only and cannot issue loads or evictions.
- HLOD selection may use it to choose proxy or child fallback.
- The full streaming system replaces the data source later without changing HLOD selection semantics.

## OcclusionHistoryEntry

Conservative occlusion state for a primitive or cluster.

**Fields**:

- `visibilityKey`: View/history compatibility key.
- `primitiveHandle`: Primitive or cluster handle.
- `primitiveGeneration`: Generation of the primitive handle when history was recorded.
- `primitiveBoundsGeneration`: Bounds/transform generation when the history was recorded.
- `representationId`: LOD/HLOD/proxy representation when history was recorded.
- `occludeeEligibilitySerial`: Material/depth-write/transparency/overlay eligibility serial when history was recorded.
- `lastTestFrame`: Last frame tested.
- `lastVisibleFrame`: Last frame known visible.
- `lastOccludedFrame`: Last frame known occluded.
- `screenBounds`: Last projected bounds.
- `confidence`: Conservative confidence score.

**Validation Rules**:

- Invalid key, stale primitive generation, stale bounds generation, stale representation id, stale occludee eligibility, old history, camera cut, or backend reset forces visible.
- Occlusion history is advisory and cannot override explicit editor selection visibility.

## HZBFrameResources

GPU resources for HZB build and optional occlusion tests.

**Fields**:

- `depthSource`: Qualified opaque scene depth texture identity and subresource range.
- `depthSourcePassClass`: Pass class used to build HZB, such as main opaque depth prepass or opaque base pass.
- `occluderEligibilityMask`: Rules used to exclude transparent, overlay, editor gizmo, debug, custom-order, alpha-blended, and non-depth-write primitives.
- `hzbPyramid`: HZB texture and views; the current compute shader populates mip0 only.
- `hzbMipRanges`: Subresource ranges used for UAV writes and SRV reads; current HZB build/occlusion declares mip0-only ranges until a real mip-chain shader lands.
- `occlusionInputBuffer`: Bounds or screen-rect buffer.
- `occlusionOutputBuffer`: GPU-visible result buffer or indirect mask.
- `resourceSerial`: Backend resource identity serial.
- `validForHistoryKey`: View key used for compatibility.

**Validation Rules**:

- HZB writes and reads must be declared through FrameGraph/RHI access.
- HZB depth source must come from eligible opaque depth-writing geometry only.
- Transparent, overlay, editor gizmo, debug, non-depth-write, alpha-blended, and custom-order passes must not contribute occluder depth unless a later contract explicitly proves they are safe.
- Each mip write/read dependency must use the narrow subresource range needed for that mip.
- CPU readback is not required for current-frame visibility.
- Resource identity changes invalidate history.

## StreamingCell

Scene and resource residency unit.

**Fields**:

- `cellHandle`: Stable cell handle.
- `bounds`: Cell world bounds.
- `primitiveHandles`: Primitive handles in the cell.
- `meshArtifacts`: Required mesh or LOD resources.
- `textureArtifacts`: Required texture resources.
- `hlodProxyArtifacts`: Required proxy resources.
- `dependencyClosure`: Resolved dependency edges required by the currently selected representation.
- `activeTickets`: Residency tickets currently requesting or pinning this cell.
- `priority`: Request priority from visibility and editor interest.
- `state`: Residency state.
- `cpuBytes`: CPU-side resident or pending bytes.
- `gpuBytes`: GPU-side resident or pending bytes.
- `lastRequestedFrame`: Last frame interest was generated.
- `lastVisibleFrame`: Last frame any member was visible.

**Validation Rules**:

- A cell request must be expanded into dependency edges before budgeting.
- Duplicate resource dependencies across cells, LODs, and HLOD proxies must coalesce into shared tickets.
- Eviction must respect active tickets, in-flight frames, prepared render packages, and fallback requirements.

## StreamingResourceDependency

Dependency edge from scene interest to a concrete resource.

**Fields**:

- `dependencyId`: Stable dependency identifier.
- `source`: Visibility, LOD, HLOD, editor selection, import, or validation.
- `ownerCell`: Optional streaming cell handle.
- `representationId`: Selected primitive, LOD, HLOD proxy, or placeholder representation.
- `resourceType`: Mesh, material, texture, HLOD proxy, scene cell, or placeholder.
- `artifactId`: Asset/import artifact identifier.
- `cpuBytes`: CPU-side estimated or known byte size.
- `gpuBytes`: GPU-side estimated or known byte size.
- `requiredForVisibleRepresentation`: Whether the resource is required to draw the currently selected representation.
- `fallbackDependency`: Optional lower-detail dependency used when this dependency is not resident.

**Validation Rules**:

- Dependency closure must be deterministic for a visibility result.
- Required visible dependencies cannot be evicted until their frame-retirement pins are released.
- Missing optional dependencies may degrade to fallback dependencies but must not block the render frame.

## ResidencyTicket

Deduplicated streaming request and residency pin.

**Fields**:

- `ticketId`: Stable ticket identifier.
- `dependencyId`: Dependency being requested or pinned.
- `priority`: Priority after distance, visibility, editor interest, and aging are applied.
- `state`: Requested, LoadingCpu, PendingGpuUpload, Resident, VisibleResident, CancelPending, or EvictPending.
- `requestFrame`: Frame that first requested the ticket.
- `lastInterestFrame`: Most recent frame that refreshed interest.
- `pinCount`: Number of in-flight frames, prepared packages, editor selections, or cells holding the resource.
- `cancelReason`: Optional reason when a request is canceled or superseded.
- `coalescedRequestCount`: Number of requests merged into this ticket.

**Validation Rules**:

- Equivalent dependency requests must deduplicate into one active ticket.
- Priority aging must prevent long-lived visible or editor-interest resources from starving.
- Cancellation may stop future work but must not release resources pinned by non-retired frames.

**State Transitions**:

- `Unrequested` -> `Requested`: Visibility or editor interest asks for the cell.
- `Requested` -> `LoadingCpu`: Background IO begins.
- `LoadingCpu` -> `PendingGpuUpload`: CPU data ready.
- `PendingGpuUpload` -> `Resident`: Budgeted GPU upload committed.
- `Resident` -> `VisibleResident`: Cell is visible this frame.
- `Resident` -> `EvictPending`: Eviction selected after safety checks.
- `EvictPending` -> `Evicted`: Resources released after frame retirement.

## LargeSceneTelemetry

Renderer-owned statistics for validation and editor display.

**Fields**:

- `registeredPrimitiveCount`
- `staticPrimitiveCount`
- `dynamicPrimitiveCount`
- `unclassifiedPrimitiveCount`
- `spatialCandidateCount`
- `fullScanCandidateCount`
- `visiblePrimitiveCount`
- `visibleMeshCount`
- `culledByReason[]`
- `lodSelectionCount[]`
- `activeHLODClusterCount`
- `occlusionTestCount`
- `occlusionCulledCount`
- `streamingRequestCount`
- `streamingCommitCount`
- `streamingEvictCount`
- `streamingDependencyCount`
- `residencyTicketCount`
- `residentCpuBytes`
- `residentGpuBytes`
- `requestedCpuBytes`
- `requestedGpuBytes`
- `primitiveRecordsTouched`
- `allocatedPrimitiveSlotCount`
- `tombstonedPrimitiveSlotCount`
- `syncSweepTouchedSlotCount`
- `syncTouchedPrimitiveCount`
- `syncFullSweepCount`
- `boundsDirtyPrimitiveCount`
- `primitiveSlotReuseCount`
- `visibilityTestedPrimitiveCount`
- `finalizationTouchedPrimitiveCount`
- `finalizationTouchedCommandCount`
- `commandOffsetRebuildCount`
- `rawVisibleDrawCount`
- `submittedDrawCount`
- `dynamicInstanceGroupCount`
- `dynamicCandidateCount`
- `dynamicRecordsTouched`
- `staticIndexRefitCount`
- `staticIndexRebuildCount`
- `staticIndexLastGoodQueryCount`
- `staticIndexDirtyOverlayCount`
- `spatialRebuildFallbackCount`
- `dynamicIndexUpdateCount`
- `syncTimeNs`
- `serialVisibilityTimeNs`
- `parallelVisibilityTimeNs`
- `queueFinalizationTimeNs`
- `hzbBuildTimeNs`
- `streamingCommitTimeNs`

**Validation Rules**:

- Counters reset per frame or are explicitly marked cumulative.
- In the US1 baseline phase, `fullScanCandidateCount` records retained full-scan candidates while
  `spatialCandidateCount` remains zero until US2 spatial indexing lands.
- In the US1 baseline phase, primitives that have not been classified by the spatial-index layer
  are counted in `unclassifiedPrimitiveCount` rather than being reported as static or dynamic.
- Timings are optional in deterministic unit tests but required in runtime evidence reports.
- Editor display must read this snapshot, not live scene state.
