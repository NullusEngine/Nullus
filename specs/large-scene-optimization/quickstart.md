# Quickstart: Large Scene Optimization Validation

This design package does not change runtime code. Use this quickstart when implementing or reviewing the future phases.

## Current Design-Package Checks

Run from the worktree root:

```powershell
git status --short --branch
rg --glob '!**/checklists/requirements.md' -n "TO[D]O|TB[D]|\[NEEDS CLARIFICATION\]" specs/large-scene-optimization
git diff --check
```

Expected result:

- The branch is `large-scene-optimization`.
- The placeholder scan has no matches.
- The markdown diff has no whitespace errors.

## Baseline Evidence Before Code Changes

Before changing runtime behavior, capture a baseline report with:

- Build configuration and backend.
- Hardware and OS.
- Scene type: imported model, synthetic grid, repeated props, or tiled world.
- Primitive count, visible count, draw count, and FrameInfo counters.
- RenderScene sync time, visibility time, draw queue time, RHI recording time, and editor presentation time.
- Any known streaming or resource-resolution stalls.

Recommended output path:

```text
specs/large-scene-optimization/validation/baseline-large-scene.md
```

## Focused Automated Tests By Phase

Use the existing `NullusUnitTests` target and add focused tests before behavior changes.

### Phase 0: Telemetry

Recommended test files:

```text
Tests/Unit/RendererStatsTests.cpp
Tests/Unit/RenderSceneCacheTests.cpp
Tests/Unit/EditorRenderPathContractTests.cpp
```

Expected coverage:

- Large-scene counters reset per frame.
- FrameInfo exposes counters without editor scene traversal.
- Existing draw-call optimization counters still work.
- Stable-frame sync touched counts, visibility touched counts, queue-finalization touched counts, and streaming dependency/ticket counters are reported separately.

### Phase 1: Spatial Index

Recommended test files:

```text
Tests/Unit/SceneSpatialIndexTests.cpp
Tests/Unit/RenderSceneCacheTests.cpp
```

Expected coverage:

- Static insert, refit, rebuild threshold, remove.
- Last-good static index, dirty overlay, rebuild budget, and topology-churn fallback reporting.
- Dynamic move, reinsert, remove.
- Dynamic-heavy localized query candidate/touched-count bounds.
- Candidate query versus full-scan equivalence.
- Generation-stale handles rejected.
- Candidate count below 30% and visibility-tested primitive count below 35% for the partitioned 100,000 primitive fixture.

### Phase 2: Visibility Pipeline

Recommended test files:

```text
Tests/Unit/SceneVisibilityPipelineTests.cpp
Tests/Unit/RenderSceneCacheTests.cpp
```

Expected coverage:

- Stage cull reasons.
- Serial and parallel equivalence.
- JobSystem unavailable fallback.
- Multiple views with independent results.
- `RenderScene` remains the owner of queue finalization, opaque merge, dynamic instancing, and object-index assignment.
- Queue finalization consumes sparse visible handles or command-offset ranges and does not rescan all registered primitives.
- Existing 1,000 compatible opaque draw-call reduction and object-index tests still pass after the new visibility pipeline.

### Phase 3: LOD And HLOD

Recommended test files:

```text
Tests/Unit/SceneLODTests.cpp
Tests/Unit/SceneHLODTests.cpp
Tests/Unit/EditorRenderPathContractTests.cpp
```

Expected coverage:

- Screen-relative LOD thresholds.
- Hysteresis and fade windows.
- HLOD proxy child suppression.
- Transparent and order-dependent HLOD children are not suppressed unless proxy compatibility is explicit.
- Editor selected-child inspection override.

### Phase 4: HZB Occlusion

Recommended test files:

```text
Tests/Unit/SceneOcclusionTests.cpp
Tests/Unit/FrameGraphSceneTargetsTests.cpp
Tests/Unit/DX12RenderPassUtilsTests.cpp
```

Expected coverage:

- Invalid history is visible.
- History invalidation triggers for view compatibility, moved primitives, representation changes, depth-write eligibility, jitter/projection/depth-format/sample-count changes, and backend reset.
- HZB resource access declarations.
- HZB depth source uses eligible opaque depth-writing geometry only.
- Backend unsupported fallback reason.
- HZB feature gates derive from `RHIDevice::GetCapabilities()` and texture-format capabilities.
- Prepared compute paths carry texture resource accesses, texture visibility transitions, dependency edges, exported transitions, and subresource ranges for depth SRV and HZB UAV/SRV, plus buffer resource access for HZB primitive result resources.
- HZB transitions use narrow subresource ranges; the current compute shader path declares mip0-only HZB ranges until mip-chain generation lands.
- Ordinary occlusion frames do not call synchronous `ReadPixelsChecked` fallback, wait for `BeginReadPixels` completion, wait on GPU fences, or block on readback-buffer maps.

Runtime evidence required:

- DX12 RenderDoc or RHI-event capture proving depth, HZB, occlusion, and consumption pass ordering.
- No CPU readback wait on ordinary visibility frames.

### Phase 5: Streaming Budgets

Recommended test files:

```text
Tests/Unit/SceneStreamingResidencyTests.cpp
Tests/Unit/AssetImportPipelineTests.cpp
Tests/Unit/AssetImporterFacadeTests.cpp
Tests/Unit/ThreadedRenderingLifecycleTests.cpp
```

Expected coverage:

- Request, loading, pending GPU upload, resident, visible, eviction states.
- Deterministic dependency closure from selected visibility/LOD/HLOD representations.
- Deduplicated residency tickets, priority aging, cancellation, coalescing, and pin counts.
- Per-frame budget limits.
- CPU/GPU memory byte budgets and resident/requested byte telemetry.
- Frame-retirement resource pinning.
- Shared budgets with editor import/drop background work, with `AssetImportPipelineTests.cpp` covering the asset pipeline and `AssetImporterFacadeTests.cpp` covering the facade budget-sharing contract.

## Runtime Stress Scenarios

Create or reuse scenes for:

- `100k-static-grid`: Static primitives across many cells with localized camera paths.
- `10k-dynamic-props`: Moving dynamic primitives to stress dynamic index updates and localized dynamic query bounds.
- `lod-hlod-campus`: Mixed LOD groups and HLOD clusters with editor selection.
- `occluded-corridor`: Large occluders and hidden occludees for HZB validation.
- `streaming-tiles`: Camera crossing cell boundaries with mesh and texture residency changes.
- `1000-compatible-opaque`: Draw-call grouping regression from `specs/038-optimize-draw-calls`.

## Suggested Commands

Use the repository's established build directory and configuration. Example focused commands:

```powershell
cmake --build build --target NullusUnitTests --config Release
ctest --test-dir build -C Release --output-on-failure -R "RenderScene|SceneSpatialIndex|SceneVisibility|SceneLOD|SceneHLOD|SceneOcclusion|SceneStreaming|RendererStats"
```

Phase sign-off should also include the existing draw-call scalability filters:

```powershell
ctest --test-dir build -C Release --output-on-failure -R "RenderSceneCache|RendererFrameObjectBinding|ThreadedRenderingLifecycle|RendererStats"
```

If the local build directory or generator differs, record the exact command used in the validation report.

## Evidence Rules

- Do not claim a phase is complete from source inspection alone.
- Do not claim GPU occlusion correctness without RenderDoc or equivalent RHI-event evidence.
- Do not claim Vulkan, Linux, or macOS correctness from Windows DX12 runs.
- Record fallback reasons when feature gates or backend capabilities disable a path.
- Keep baseline and post-change reports comparable by using the same scene, camera path, backend, build type, and validation hardware.
