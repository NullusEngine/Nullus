# Tasks: Large Scene Optimization

**Input**: Design documents from `specs/large-scene-optimization/`
**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/large-scene-optimization-contract.md](contracts/large-scene-optimization-contract.md)

## Phase 1: Setup And Baseline

- [x] T001 Create large-scene validation directory and baseline report template with per-phase sync/candidate/visibility/finalization/streaming timing and touched-count table schema in `specs/large-scene-optimization/validation/README.md`
- [x] T002 [P] Add large-scene test helper declarations in `Tests/Unit/LargeSceneOptimizationTestHelpers.h`
- [x] T003 [P] Add large-scene test helper implementation in `Tests/Unit/LargeSceneOptimizationTestHelpers.cpp`
- [x] T004 [P] Verify `Tests/Unit/CMakeLists.txt` glob pickup for new unit test helpers and record the check in `specs/large-scene-optimization/validation/README.md`
- [x] T005 Capture current no-code baseline notes for RenderScene, draw-call, and Scene View paths in `specs/large-scene-optimization/validation/baseline-large-scene.md`

## Phase 2: Foundational Settings, Telemetry, And Handles

- [x] T006 Add `LargeSceneSettings` declarations and named defaults in `Runtime/Engine/Rendering/LargeSceneSettings.h`
- [x] T007 Implement `LargeSceneSettings` default resolution and debug labels in `Runtime/Engine/Rendering/LargeSceneSettings.cpp`
- [x] T008 Wire `LargeSceneSettings` through RenderScene sync and visibility options in `Runtime/Engine/Rendering/RenderScene.h`
- [x] T009 Migrate existing hard-coded visibility thresholds from `Runtime/Engine/Rendering/RenderScene.cpp` into `LargeSceneSettings`
- [x] T010 [P] Add settings SSoT tests for defaults and threshold usage in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T011 Add `LargeSceneTelemetry` fields to `Runtime/Rendering/Data/FrameInfo.h`
- [x] T012 Add renderer stats recording APIs for large-scene telemetry in `Runtime/Rendering/Core/RendererStats.h`
- [x] T013 Implement renderer stats recording and reset behavior in `Runtime/Rendering/Core/RendererStats.cpp`
- [x] T014 [P] Add telemetry reset and aggregation tests in `Tests/Unit/RendererStatsTests.cpp`
- [x] T015 Add primitive slot-map handle, scene-scoped aggregate handle, generation, tombstone, layer, distance-visibility setting, and `ScenePrimitiveSnapshot` declarations in `Runtime/Engine/Rendering/RenderScene.h`
- [x] T016 Implement primitive slot allocation, free-list reuse, tombstoning, stale-handle rejection, reusable immutable snapshots, dirty-handle lists, and command-offset tables in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T017 [P] Add primitive handle lifecycle, scene-scoped handle, lower-index removal alias prevention, and snapshot lifetime tests in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T018 Add camera/view layer-mask accessors or explicit all-layers fallback in `Runtime/Rendering/Entities/Camera.h`
- [x] T019 Implement camera/view layer-mask accessors or explicit all-layers fallback in `Runtime/Rendering/Entities/Camera.cpp`
- [x] T020 Add `CameraComponent` layer-mask forwarding in `Runtime/Engine/Components/CameraComponent.h`
- [x] T021 Implement `CameraComponent` layer-mask forwarding in `Runtime/Engine/Components/CameraComponent.cpp`
- [x] T022 Update FrameInfo panel presentation for large-scene telemetry in `Project/Editor/Panels/FrameInfoRendererStats.cpp`
- [x] T023 [P] Add editor contract checks for snapshot-based FrameInfo consumption in `Tests/Unit/EditorRenderPathContractTests.cpp`

## Phase 3: User Story 1 - Large Scene Bottlenecks Are Measurable

**Goal**: Make large-scene frame costs observable before changing culling behavior.

**Independent Test**: Renderer stats and a synthetic large-scene helper expose deterministic counts for primitive, sync touched, candidate, visibility touched, finalization touched, visible, command, draw, streaming dependency/ticket, and memory counters.

- [x] T024 [P] [US1] Add synthetic primitive scene builder in `Tests/Unit/LargeSceneOptimizationTestHelpers.cpp`
- [x] T025 [P] [US1] Add expected telemetry assertion helpers in `Tests/Unit/LargeSceneOptimizationTestHelpers.h`
- [x] T026 [US1] Record primitive sync counts, sync touched counts, full-sweep fallback counts, bounds dirty counts, and primitive slot reuse counts in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T027 [US1] Record candidate count, primitive records touched, visibility-tested primitive count, finalization touched primitive/command counts, command-offset rebuild counts, and draw queue counts in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T028 [US1] Propagate large-scene telemetry into `BaseSceneRenderer::ParseScene` and frame snapshots in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- [x] T029 [P] [US1] Add large-scene telemetry tests for stable-frame sync touched counts, reported full-sweep fallback slot cost, finalization touched counts, and command-offset rebuild counts in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T030 [P] [US1] Add FrameInfo formatting tests for new counters in `Tests/Unit/RendererStatsTests.cpp`

## Phase 4: User Story 2 - Visibility Scales Without Full Per-Frame Primitive Scans

**Goal**: Add candidate generation through static/dynamic spatial indexing and route visibility through one pipeline without hiding O(N) scans in sync, dynamic queries, rebuild fallback, visibility, or queue finalization.

**Independent Test**: A partitioned 100,000 primitive fixture returns localized candidates below 30% of registered primitives, visibility-tested primitive counts below 35%, bounded sync/finalization touched counts, bounded dynamic candidate/touched counts, and serial/parallel/full-scan comparison modes agree on visibility.

- [x] T031 [P] [US2] Add `SceneSpatialIndex` declarations in `Runtime/Engine/Rendering/SceneSpatialIndex.h`
- [x] T032 [US2] Implement static primitive insertion, refit, rebuild threshold, last-good index reuse, dirty overlay, rebuild budget, and removal in `Runtime/Engine/Rendering/SceneSpatialIndex.cpp`
- [x] T033 [US2] Implement dynamic primitive update and localized dynamic query path with dynamic candidate/touched telemetry in `Runtime/Engine/Rendering/SceneSpatialIndex.cpp`
- [x] T034 [US2] Add maintained active, layer, distance candidate metadata, rebuild fallback reasons, and last-good/dirty-overlay counters in `Runtime/Engine/Rendering/SceneSpatialIndex.cpp`
- [x] T035 [P] [US2] Add `SceneSpatialIndexTests.cpp` for static, dynamic, last-good rebuild, dirty overlay, and fallback behavior in `Tests/Unit/SceneSpatialIndexTests.cpp`
- [x] T036 [P] [US2] Add 100,000 primitive candidate ratio, high-dynamic-count, topology-churn, rebuild-budget, and touched-count assertion tests in `Tests/Unit/SceneSpatialIndexTests.cpp`
- [x] T037 [US2] Add spatial-index ownership and update calls to `Runtime/Engine/Rendering/RenderScene.h`
- [x] T038 [US2] Integrate spatial-index maintenance with O(changed) dirty/removed handle updates from `RenderScene::Synchronize` and keep existing full-sweep sync fallback explicitly reported in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T039 [P] [US2] Add `SceneVisibilityPipeline` declarations in `Runtime/Engine/Rendering/SceneVisibilityPipeline.h`
- [x] T040 [US2] Implement spatial, active, layer, and distance candidate merge before frustum evaluation in `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T041 [US2] Implement candidate revalidation, frustum culling, and cull-reason assignment in `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T042 [US2] Route `RenderScene::GatherVisibleCommands` through `SceneVisibilityPipeline` sparse visible handles and cached command-offset tables while keeping queue finalization in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T043 [US2] Implement serial, parallel, and full-scan comparison modes in `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T044 [P] [US2] Add serial/parallel/full-scan equivalence tests in `Tests/Unit/SceneVisibilityPipelineTests.cpp`
- [x] T045 [P] [US2] Add JobSystem unavailable fallback tests in `Tests/Unit/SceneVisibilityPipelineTests.cpp`
- [x] T046 [P] [US2] Add queue-finalization ownership and bounded-touched tests proving `RenderScene` still owns opaque merge, transparent order, dynamic instancing, object-index assignment, and does not rescan all primitives in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T047 [P] [US2] Add draw-call scalability regression tests preserving the 1,000 compatible opaque reduction expectation in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T048 [P] [US2] Add object-index allocation regression tests after visibility pipeline integration in `Tests/Unit/RendererFrameObjectBindingTests.cpp`

## Phase 5: User Story 3 - LOD And HLOD Reduce Work Before Draw Submission

**Goal**: Add per-view representation selection before command eligibility while keeping proxy readiness read-only until full streaming lands.

**Independent Test**: LOD thresholds, HLOD proxy selection, child suppression, fade/hysteresis, proxy readiness fallback, and editor selection override are deterministic.

- [x] T049 [P] [US3] Add read-only `RepresentationResidencySnapshot` declarations in `Runtime/Engine/Rendering/SceneStreamingResidency.h`
- [x] T050 [US3] Populate minimal representation residency snapshot from current ready/fallback primitive resources in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T051 [P] [US3] Add LOD data declarations in `Runtime/Engine/Rendering/SceneLOD.h`
- [x] T052 [US3] Implement screen-relative LOD selection with bias and hysteresis in `Runtime/Engine/Rendering/SceneLOD.cpp`
- [x] T053 [P] [US3] Add LOD selection tests in `Tests/Unit/SceneLODTests.cpp`
- [x] T054 [US3] Attach LOD group references to primitive records in `Runtime/Engine/Rendering/RenderScene.h`
- [x] T055 [US3] Integrate LOD selection into `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T056 [P] [US3] Add HLOD data declarations with proxy compatibility flags for transparent/order-dependent/skinned/animated/editor-only children in `Runtime/Engine/Rendering/SceneHLOD.h`
- [x] T057 [US3] Implement HLOD cluster selection and child suppression using `RepresentationResidencySnapshot` and compatibility flags in `Runtime/Engine/Rendering/SceneHLOD.cpp`
- [x] T058 [US3] Integrate HLOD decisions into `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T059 [P] [US3] Add HLOD cluster, proxy-readiness fallback, transparent/order-dependent compatibility, and unsafe child non-suppression tests in `Tests/Unit/SceneHLODTests.cpp`
- [x] T060 [P] [US3] Add editor selected-child override contract tests in `Tests/Unit/EditorRenderPathContractTests.cpp`
- [x] T061 [US3] Add imported hierarchy HLOD metadata extraction hook in `Runtime/Engine/Assets/ModelPrefabBuilder.cpp`
- [x] T062 [P] [US3] Add imported HLOD metadata tests in `Tests/Unit/AssetPrefabPipelineTests.cpp`

## Phase 6: User Story 4 - Occlusion Culling Removes Hidden Work Without GPU Stalls

**Goal**: Add conservative HZB/history occlusion with authoritative RHI capability gates and explicit texture hazards.

**Independent Test**: Invalid history is visible, valid history can cull, moved primitives and changed representations become conservatively visible, unsupported capabilities fallback conservatively, HZB uses only eligible opaque depth, compute texture barriers and dependency edges cover HZB/depth/occlusion resources, current mip0 HZB hazards are explicit, future mip-chain hazards are not claimed until shader support lands, and ordinary occlusion frames do not wait for synchronous readback, fences, or blocking maps.

- [x] T063 [P] [US4] Add occlusion data declarations in `Runtime/Engine/Rendering/SceneOcclusion.h`
- [x] T064 [US4] Implement occlusion history key and invalidation rules for view compatibility, primitive bounds/transform generation, representation id, and depth-write eligibility in `Runtime/Engine/Rendering/SceneOcclusion.cpp`
- [x] T065 [P] [US4] Add occlusion fallback, moved-primitive, representation-change, jitter/projection/depth-format, and invalidation tests in `Tests/Unit/SceneOcclusionTests.cpp`
- [x] T066 [US4] Add HZB frame resource declarations with qualified opaque depth source, occluder eligibility, and narrow subresource ranges in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder.h`
- [x] T067 [US4] Build HZB pass descriptors and mip0 resource access declarations for eligible opaque depth only in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.cpp`
- [x] T068 [US4] Add HZB, occlusion, and async-readback feature checks through `RHIDevice::GetCapabilities()`, `RHIDeviceFeature` / `RHIDeviceCapabilities` in `Runtime/Rendering/RHI/RHITypes.h`, and texture-format capabilities consumed from `Runtime/Rendering/RHI/Core/RHIDevice.h`
- [x] T069 [US4] Extend prepared compute dispatch inputs with texture resource accesses, texture visibility transitions, exported transitions, and per-subresource ranges in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- [x] T070 [US4] Record prepared compute texture resource accesses, transitions, and dependency edges in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- [x] T071 [US4] Execute prepared compute texture transitions and dependency-derived synchronization in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`
- [x] T072 [US4] Add current mip0 HZB UAV-to-SRV ordering, dependency-edge, and opaque-depth eligibility tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [x] T073 [US4] Add capability-off fallback reason tests for unsupported backends and disabled features in `Tests/Unit/SceneOcclusionTests.cpp`
- [x] T074 [US4] Add no-synchronous-readback regression tests guarding ordinary occlusion frames from `ReadPixelsChecked`, `BeginReadPixels` completion waits, GPU fence waits, and blocking readback-buffer maps in `Tests/Unit/SceneOcclusionTests.cpp`
- [x] T075 [US4] Integrate conservative occlusion stage into `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T076 [P] [US4] Add frame-graph texture access and prepared-compute dependency graph tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [x] T077 [P] [US4] Add DX12 render-pass/resource-state contract tests in `Tests/Unit/DX12RenderPassUtilsTests.cpp`
- [x] T078 [US4] Record initial DX12 RenderDoc/RHI-event HZB dispatch, resource-contract, and capture-boundary evidence in `specs/large-scene-optimization/validation/hzb-occlusion-dx12.md`
- [x] T078F [US4] Record final DX12 HZB/ConservativeOcclusion feature-gate enablement evidence after pass order is corrected to build HZB after the depth-producing GBuffer pass and before history consumption

## Phase 7: User Story 5 - Scene And Asset Residency Follow A Frame Budget

**Goal**: Make visibility-generated resource interest budgeted and non-blocking under CPU, IO, GPU upload, CPU memory, and GPU memory limits.

**Independent Test**: Scene cells and asset resources move through dependency closure, deduplicated request tickets, load, upload, resident, visible, cancellation, and eviction states under budgets with resident/requested byte telemetry.

- [x] T079 [P] [US5] Add full streaming residency, `StreamingResourceDependency`, and `ResidencyTicket` declarations in `Runtime/Engine/Rendering/SceneStreamingResidency.h`
- [x] T080 [US5] Implement residency state machine with request deduplication, ticket coalescing, cancellation, priority aging, and pin counts in `Runtime/Engine/Rendering/SceneStreamingResidency.cpp`
- [x] T081 [US5] Add dependency-closure CPU and GPU byte-size accounting to streaming cells and resources in `Runtime/Engine/Rendering/SceneStreamingResidency.cpp`
- [x] T082 [US5] Generate deterministic streaming dependency closure from visibility, LOD, and HLOD results in `Runtime/Engine/Rendering/SceneVisibilityPipeline.cpp`
- [x] T083 [US5] Integrate budgeted CPU commit, IO, GPU upload, CPU memory, GPU memory, dependency count, and residency ticket count accounting in `Runtime/Engine/Rendering/SceneStreamingResidency.cpp`
- [x] T084 [US5] Add frame-retirement resource pinning checks in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- [x] T085 [P] [US5] Add streaming residency state, dependency closure, request dedupe, cancellation, and priority aging tests in `Tests/Unit/SceneStreamingResidencyTests.cpp`
- [x] T086 [P] [US5] Add CPU/GPU memory budget exhaustion, ticket pinning, dependency-aware eviction, and fallback dependency tests in `Tests/Unit/SceneStreamingResidencyTests.cpp`
- [x] T087 [P] [US5] Add frame-retirement pinning tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [x] T088 [US5] Share large-scene streaming budget with editor import background work in `Project/Editor/Assets/AssetImporterFacade.cpp`
- [x] T089 [P] [US5] Add editor import budget contract tests in `Tests/Unit/AssetImporterFacadeTests.cpp`

## Phase 8: User Story 6 - Editor Debugging Remains Trustworthy

**Goal**: Surface culling and residency decisions without adding traversal, sync, or load work.

**Independent Test**: Editor panels consume renderer snapshots and disabled overlays are cost-free.

- [x] T090 [P] [US6] Add debug cull-reason snapshot structures in `Runtime/Engine/Rendering/SceneVisibilityPipeline.h`
- [x] T091 [US6] Add cull-reason snapshot publication from `BaseSceneRenderer` in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- [x] T092 [US6] Display large-scene counters in FrameInfo in `Project/Editor/Panels/FrameInfoRendererStats.cpp`
- [x] T093 [P] [US6] Add optional Scene View culling overlay declarations in `Project/Editor/Rendering/DebugSceneRenderer.h`
- [x] T094 [US6] Implement snapshot-based Scene View culling overlay in `Project/Editor/Rendering/DebugSceneRenderer.cpp`
- [x] T095 [P] [US6] Add editor debug snapshot tests in `Tests/Unit/EditorRenderPathContractTests.cpp`
- [x] T096 [P] [US6] Add Scene View no-drain contract coverage in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [x] T097 [US6] Record comparable Scene View validation evidence in `specs/large-scene-optimization/validation/editor-scene-view-large-scene.md`

## Phase 9: Polish And Cross-Cutting Validation

- [x] T098 Run focused unit tests with filter `RenderScene|SceneSpatialIndex|SceneVisibility|SceneLOD|SceneHLOD|SceneOcclusion|SceneStreaming|RendererStats` and record exact output in `specs/large-scene-optimization/validation/focused-tests.md`
- [x] T099 Run draw-call regression tests with filter `RenderSceneCache|RendererFrameObjectBinding|ThreadedRenderingLifecycle|RendererStats` and record exact output in `specs/large-scene-optimization/validation/draw-call-regression.md`
- [x] T100 Run full `NullusUnitTests` where practical and record exact command output in `specs/large-scene-optimization/validation/final-tests.md`
- [x] T101 Run `git diff --check` and placeholder scan for the implementation branch
- [x] T102 [P] Update `Docs/Rendering/RenderDocDebugging.md` with large-scene HZB capture notes if the HZB path lands
- [x] T103 [P] Update `Docs/Rendering/RHIMultiBackendArchitecture.md` with backend capability notes if new RHI gates land
- [x] T104 Promote `specs/large-scene-optimization/benchmarks/rendering-large-scene-benchmark.md` into the plan-review benchmark registry and update `Docs/REVIEW_PATTERNS.md` with new large-scene review patterns after implementation evidence exists
- [x] T105 Run `/plan-review` quality gate for the completed implementation phase
- [x] T106 If the phase touches GPU sync, RHI abstraction, or cross-language SSoT, run the required multi-agent review gate per repository policy
- [x] T107 Commit only after validation evidence and review gates are complete

## Dependencies

- Setup and foundational settings/telemetry/handles tasks T001-T023 block all user stories.
- US1 tasks T024-T030 should complete before behavior-changing visibility work.
- US2 tasks T031-T048 block LOD, HLOD, occlusion, streaming, editor overlays, and draw-call regression sign-off.
- US3 tasks T049-T062 block HLOD-driven streaming and editor representation debug.
- US4 tasks T063-T078 plus final gate task T078F require the visibility pipeline and must complete before GPU occlusion is enabled by default.
- US5 tasks T079-T089 require visibility and representation interest data.
- US6 tasks T090-T097 require telemetry and visibility result snapshots.
- Polish tasks T098-T107 run after each implementation phase and at final sign-off.

## Parallel Execution Examples

- T002, T003, and T004 can proceed with T005 because they touch test helpers and validation docs.
- T010, T014, T017, and T023 can be implemented after the corresponding settings, telemetry, handle, and editor interfaces are declared.
- T031 and T039 can be declared in parallel before integration tasks T038 and T042.
- T051 and T056 can be declared in parallel because LOD and HLOD data models are separate.
- T065, T072, T073, and T074 can be authored in parallel once the occlusion contracts are declared.
- T085, T086, T087, and T089 can be authored in parallel after the streaming state machine interface is declared.

## Implementation Strategy

Implement the feature as complete, independently validated phases. Do not enable later feature gates by default until their tests and runtime evidence pass. Keep the existing retained render-scene path as a fallback throughout the work so each phase can be measured against baseline behavior. Treat `LargeSceneSettings`, primitive handles, visibility results, HZB capabilities, and streaming budgets as SSoT surfaces; do not create parallel settings or hidden hot-path thresholds.
