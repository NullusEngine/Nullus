# Contract: Large Scene Optimization

This contract describes internal renderer-facing APIs and invariants. It is not an external ABI.

## RenderScene Synchronization Contract

```cpp
RenderSceneSyncStats RenderScene::Synchronize(SceneSystem::Scene& scene, const RenderSceneSyncOptions& options);
```

**Required behavior**:

- Preserve existing draw output when large-scene feature gates are disabled.
- Create or update `ScenePrimitiveRecord` entries with stable handles and generations.
- Store primitive records in slot-map/free-list/tombstone storage; do not use dense vector erase for handle identity.
- Copy `GameObject::GetLayer()` and primitive min/max draw-distance settings into the immutable primitive record.
- Update cached bounds when transform, mesh bounds, custom bounds, or frustum behavior changes.
- Mark draw command slots dirty only when mesh, material, shader, render state, primitive mode, or override-material inputs change.
- Emit sync telemetry for added, reused, removed, dirty-bounds, dirty-command, and not-resident primitives.
- In large-scene mode, report any live `Scene::FastAccessComponents` source sweep explicitly and use dirty handles or sparse handles for downstream spatial-index maintenance, visibility, and queue finalization. Fully O(changed) source synchronization requires dirty lists, source-scene change serials, or equivalent event-driven data and must not be implied while the source sweep fallback remains active.
- Publish `ScenePrimitiveSnapshot` data with command-offset tables, dirty handles, removed handles, and generation-safe dense mappings for visibility and queue finalization.

**Forbidden behavior**:

- Do not synchronously load meshes, materials, textures, HLOD proxies, or scene cells from synchronization.
- Do not expose raw component pointers to asynchronous visibility jobs without immutable snapshots or generation checks.
- Do not silently full-sweep the live scene in large-scene mode; source sweep fallbacks must be reported in telemetry and must not be used as evidence that synchronization is fully O(changed).

## SceneSpatialIndex Contract

```cpp
class SceneSpatialIndex
{
public:
    void Update(const ScenePrimitiveSnapshot& primitives, const SceneSpatialIndexUpdateOptions& options);
    VisibilityCandidateSet Query(const VisibilityFrameInput& input) const;
    VisibilityCandidateSet FullScanForComparison(const VisibilityFrameInput& input) const;
};
```

**Required behavior**:

- Maintain separate static and dynamic update paths.
- Use maintained active, layer, and distance metadata to avoid full primitive scans before candidate evaluation.
- Return candidate handles with valid generations only.
- Support deterministic full-scan comparison in tests.
- Use last-good static index data plus dirty overlays, staged rebuilds, async/double-buffer rebuilds, or explicit rebuild budgets when topology churns.
- Dynamic queries must use a spatial/dense moving-object structure with bounded candidate and touched counts for localized views; a full dynamic list scan is only allowed for debug comparison or explicitly reported fallback.
- Record query, refit, rebuild, dirty-overlay, last-good, dynamic-candidate, dynamic-touched, and fallback counters.

**Fallback behavior**:

- If the index is disabled, empty, invalid, or rebuilding, visibility may use full-scan candidates only as a reported fallback. Rebuild-in-progress frames should prefer last-good index data plus dirty overlays when available.

## Visibility Pipeline Contract

```cpp
VisibilityFrameResult SceneVisibilityPipeline::Evaluate(
    const VisibilityFrameInput& input,
    const ScenePrimitiveSnapshot& primitives,
    const SceneSpatialIndex& spatialIndex,
    const SceneRepresentationState& representation,
    const SceneOcclusionState& occlusion);
```

**Stage order**:

1. Build spatial, active, layer, and distance candidate sets from maintained index metadata.
2. Merge candidates and generation-check handles.
3. Revalidate activity, layer, distance, and editor overrides on candidates.
4. Run frustum tests.
5. Select LOD and HLOD representation.
6. Apply conservative occlusion.
7. Emit command eligibility, selected representation, cull reasons, and telemetry.
8. Let `RenderScene` finalize queues, sort/merge opaque commands, preserve transparent order, assign object indices, and record draw-call stats.

**Required behavior**:

- Build view layer filters from `Camera`/`CameraComponent`, editor overrides, or an explicit all-layers default.
- Large-scene mode must not scan all retained primitives before the spatial/layer/distance candidate query. If an implementation chooses a semantically equivalent order, it must prove bounded `primitiveRecordsTouched` and `visibilityTestedPrimitiveCount` in tests.
- `ScenePrimitiveSnapshot` is the input SSoT for visibility jobs; jobs must not reach back into mutable `RenderScene` storage.
- Visibility results must include sparse visible primitive handles and sparse eligible command ranges for queue finalization.
- Serial and parallel evaluations must produce identical visible bits, selected representations, and cull reasons.
- Missing or invalid optional systems must conservatively preserve visibility.
- View-local state must not mutate global primitive state.
- Queue finalization, opaque sort/merge, dynamic instancing, and object-index assignment remain owned by `RenderScene`.

## RenderScene Queue Finalization Contract

```cpp
RenderSceneQueueStats RenderScene::FinalizeVisibleQueues(
    const ScenePrimitiveSnapshot& snapshot,
    const VisibilityFrameResult& visibility);
```

**Required behavior**:

- Consume sparse visible primitive handles, sparse eligible command ranges, or cached command-offset tables from the snapshot and visibility result.
- Preserve transparent back-to-front ordering and exclude incompatible transparent/order-dependent draws from unsafe merging or HLOD suppression.
- Preserve dynamic instancing and object-index allocation semantics from the existing draw-call optimization path.
- Record finalization touched primitive count, finalization touched command count, command-offset rebuild count, and queue finalization time.

**Forbidden behavior**:

- Do not rescan all registered primitives or all mesh command ranges after visibility produced a bounded candidate set, except in explicitly reported debug/full-comparison mode.

## LOD Contract

```cpp
LODSelectionResult SceneLODSystem::Select(
    const VisibilityFrameInput& input,
    const ScenePrimitiveSnapshot& primitives,
    const LODGroupRecord& group,
    LODSelectionHistory& history);
```

**Required behavior**:

- Use documented screen-relative size, LOD bias, and hysteresis; preserve optional fade metadata for a later rendered cross-fade slice.
- Keep selection per view.
- Allow explicit editor/runtime forced LOD.
- Record selected LOD and reserved fade metadata in `VisibilityFrameResult` without claiming dual-representation rendering.

**Forbidden behavior**:

- Do not rebuild cached draw commands for transform-only LOD evaluation.
- Do not submit multiple LODs in this slice; rendered fade transitions require a future explicit contract update.

## HLOD Contract

```cpp
HLODSelectionResult SceneHLODSystem::SelectClusters(
    const VisibilityFrameInput& input,
    const ScenePrimitiveSnapshot& primitives,
    const HLODClusterSet& clusters,
    const RepresentationResidencySnapshot& residency);
```

**Required behavior**:

- Select proxy or child representation per view.
- Suppress child primitives only through the view's visibility result.
- Fall back to children if proxy resources are missing and children are resident.
- Respect compatibility flags for transparent, order-dependent, animated, skinned, editor-only, and proxy-safe children.
- Preserve selected child inspection in editor views when requested.

**Forbidden behavior**:

- Do not globally disable child primitives when a single view selects a proxy.
- Do not suppress transparent or order-dependent children unless the cluster contract explicitly marks the proxy safe for that view and render queue.
- Do not synchronously load proxy resources from selection.
- Do not issue streaming requests directly from HLOD selection; emit interest for the streaming layer.

## Occlusion Contract

```cpp
SceneOcclusionResult SceneOcclusionSystem::Evaluate(
    const VisibilityFrameInput& input,
    const VisibilityFrameResult& preOcclusion,
    const HZBFrameResources* hzbResources,
    const OcclusionHistory& history);
```

**Required behavior**:

- Invalid history yields visible, not hidden.
- Camera cuts, projection changes, incompatible projection jitter, viewport extent/origin changes, render-scale changes, near/far or depth convention changes, depth-resource identity changes, depth-format/sample-count changes, primitive bounds/transform generation changes, LOD/HLOD representation changes, material depth-write eligibility changes, and backend reset invalidate history.
- HZB depth must be built only from an eligible opaque depth-writing pass. Transparent, overlay, editor gizmo, debug, custom-order, alpha-blended, and non-depth-write primitives are not valid occluders unless a later contract explicitly proves their safety.
- HZB build and occlusion passes declare resource accesses, subresource ranges, exported transitions, and producer-consumer dependency edges through FrameGraph/RHI.
- HZB and occlusion support must be gated through `RHIDevice::GetCapabilities()` / `RHIDeviceFeature` and texture-format capabilities.
- Prepared compute dispatches that touch HZB or occlusion textures must carry texture resource accesses and texture visibility transitions, not only buffer barriers, so dependency graph construction and RHI execution observe the same subresource ranges.
- The current HZB mip0 build path must declare UAV write to SRV read ordering with narrow subresource ranges. Future mip-chain generation must add matching subresource declarations when the shader writes those mips.
- DX12 runtime correctness requires RenderDoc or RHI-event evidence before sign-off.

**Forbidden behavior**:

- Do not wait for GPU readback on the main/editor frame for current-frame culling.
- Do not consume HZB read/write resources without explicit transitions.
- Do not call synchronous `ReadPixelsChecked` fallback, wait for `BeginReadPixels` completion, wait on GPU fences, block on readback-buffer mapping, or perform any equivalent CPU/GPU synchronization from ordinary occlusion history update frames.

## Streaming Residency Contract

```cpp
StreamingResidencyPlan SceneStreamingResidency::Plan(
    const VisibilityFrameInput& input,
    const VisibilityFrameResult& visibility,
    const LargeSceneSettings& settings);

StreamingCommitResult SceneStreamingResidency::Commit(
    const StreamingResidencyPlan& plan,
    const FrameRetirementSnapshot& retirement);
```

**Required behavior**:

- Convert visibility, LOD, HLOD, and editor-interest into resource requests.
- Expand selected representations into `StreamingResourceDependency` closure before budgeting.
- Deduplicate equivalent requests into `ResidencyTicket` records with priority aging, cancellation, coalescing, and pin counts.
- Respect CPU, IO, GPU upload, CPU memory, and GPU memory budgets.
- Pin resources referenced by in-flight frames and prepared render packages.
- Report requests, commits, evictions, budget exhaustion, resident/requested bytes, and fallback resources.

**Forbidden behavior**:

- Do not synchronously load or upload resources from visibility.
- Do not evict resources referenced by non-retired frames.
- Do not treat a streaming cell's flat artifact list as sufficient when LOD/HLOD/material/texture dependencies require a closure.

## Telemetry Contract

```cpp
void RendererStats::RecordLargeSceneTelemetry(const LargeSceneTelemetry& telemetry);
```

**Required behavior**:

- Reset per-frame counters at frame begin.
- Preserve deterministic counts in unit tests.
- Include backend, feature gates, and fallback reasons in runtime evidence logs.
- Expose a snapshot to editor UI and FrameInfo.

**Forbidden behavior**:

- Do not require editor panels to traverse live scene components to show large-scene counters.
- Do not hide fallback paths behind success-looking counters.

## Backend Capability Contract

```cpp
struct LargeSceneBackendCapabilities
{
    bool supportsHZB;
    bool supportsOcclusionCompute;
    bool supportsAsyncReadback;
    bool supportsInRenderPassChildCommandBuffers;
};
```

**Required behavior**:

- This derived view is a convenience snapshot only; feature gates must be derived from the authoritative `RHIDeviceCapabilities` and texture-format capabilities before building GPU passes.
- `supportsAsyncReadback` must map to an authoritative existing or newly added `RHIDeviceFeature` in `Runtime/Rendering/RHI/RHITypes.h`; it must not become an independent capability truth source.
- Unsupported paths must fall back conservatively and record a reason.
- Backend-specific validation evidence must name the backend and build.

**Forbidden behavior**:

- Do not claim Vulkan, Linux, or macOS behavior from Windows DX12 validation.
