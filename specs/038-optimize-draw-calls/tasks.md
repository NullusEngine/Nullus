# Tasks: Optimize Draw-Call Scalability

**Input**: Design documents from `specs/038-optimize-draw-calls/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Required. Rendering behavior, draw grouping, object data, telemetry, and threaded command splitting must be test-first or test-with-change.

**Organization**: Tasks are grouped by user story to enable independent implementation and validation.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish baseline and local development guardrails.

- [x] T001 Record baseline draw-call diagnostics and artifact paths for the current high-draw scene or deterministic stress harness in `specs/038-optimize-draw-calls/quickstart.md`
- [x] T002 [P] Verify `.worktrees/038-optimize-draw-calls` remains the active feature workspace with `git status --short --branch`
- [x] T003 Commit the spec bundle before runtime code changes, then confirm no generated files under `Runtime/*/Gen/` or `Project/*/Gen/` are modified before implementation (historical spec-before-code commit was not available in the handed-off worktree; generated-file hygiene is clean in the final validation pass)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add shared data structures and telemetry required by all user stories.

- [x] T004 [P] Add failing telemetry assertions for raw visible count, submitted/grouped draw count, largest instance group, and cache rebuild count in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T005 [P] Add failing renderer stats assertions for draw-call optimization counters in `Tests/Unit/RendererStatsTests.cpp`
- [x] T006 Define `DrawCallOptimizationStats`, reset semantics, and `GetLastDrawCallOptimizationStatsForTesting()` in `Runtime/Engine/Rendering/RenderScene.h`
- [x] T007 Add `RendererStats::RecordDrawCallOptimizationStats` and reset/apply logic in `Runtime/Rendering/Core/RendererStats.h` and `Runtime/Rendering/Core/RendererStats.cpp`
- [x] T008 Expose separate raw-visible, submitted/grouped draw, draw-call optimization, and threaded work-unit counters in `Runtime/Rendering/Data/FrameInfo.h` without changing the existing submitted pass-count semantics used by frame-package building

**Checkpoint**: Shared counters compile and fail only because implementation has not populated them yet.

---

## Phase 3: User Story 1 - Repeated Static Objects Stay Interactive (Priority: P1) MVP

**Goal**: Compatible opaque objects collapse into bounded instanced draw submissions with stable object-data ranges.

**Independent Test**: `RenderSceneCacheTests` creates at least 1,000 compatible opaque objects and verifies at least 90% draw reduction, stable cache reuse on the second frame, correct object-data ranges, and no transparent grouping.

### Tests for User Story 1

- [x] T009 [P] [US1] Add a 1,000-object compatible opaque grouping test in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T010 [P] [US1] Add a stable-second-frame cache reuse test in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T011 [P] [US1] Add object-data limit, batch-splitting, shader/object-data compatibility rejection, and cumulative `kMaxObjectDataCount + 1` overflow tests that assert non-silent split/fallback behavior in `Tests/Unit/RenderSceneCacheTests.cpp`
- [x] T012 [P] [US1] Add instanced object-data upload and pre-existing material GPU instance regression tests in `Tests/Unit/RendererFrameObjectBindingTests.cpp`

### Implementation for User Story 1

- [x] T013 [US1] Add explicit draw state bucket key helpers in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T014 [US1] Store bucket identity and rebuild diagnostics in `RenderCachedDrawCommand` in `Runtime/Engine/Rendering/RenderScene.h`
- [x] T015 [US1] Refactor opaque queue finalization into bucketed instance grouping in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T016 [US1] Split oversized instance groups while assigning global object-data indices so each emitted draw owns a valid contiguous range and overflow cannot silently skip indexed draws in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T017 [US1] Preserve existing material GPU instance behavior in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T018 [US1] Populate `DrawCallOptimizationStats` during `GatherVisibleCommands`, including raw pre-merge visible object count, `submittedSceneDrawCount`, largest group size, and last sync rebuild count in `Runtime/Engine/Rendering/RenderScene.cpp`
- [x] T019 [US1] Preserve `RenderScene::Synchronize` stats in `Runtime/Engine/Rendering/BaseSceneRenderer.cpp` and feed the last render-scene optimization stats into renderer frame stats after `GatherVisibleCommands` without reusing existing visible pass-count fields for raw counts

**Checkpoint**: User Story 1 is complete when compatible repeated objects reduce draw submissions by at least 90% and all US1 tests pass.

---

## Phase 4: User Story 2 - Large Non-Groupable Scenes Avoid Single-Threaded Submission Stalls (Priority: P2)

**Goal**: Large attachment-free recorded draw arrays are split into ordered command work units while attachment-backed scene passes preserve serial fallback until backend-gated in-render-pass child recording is available.

**Independent Test**: `ThreadedRenderingLifecycleTests` builds a 2,000-draw attachment-free recorded pass and verifies multiple ordered work units with complete draw coverage; it also verifies attachment-backed packages keep original pass inputs for fallback and later execute as child draw ranges when the backend exposes in-render-pass child command buffers.

### Tests for User Story 2

- [x] T020 [P] [US2] Add attachment-free pass-splitting metadata tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [x] T021 [P] [US2] Add regression coverage proving attachment-backed scene passes keep safe unsliced fallback coverage until renderpass-internal child recording is available in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [x] T022 [P] [US2] Add a divergent sliced-vs-unsliced serial fallback test in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` where a package contains sliced `parallelCommandWorkUnits`, ordered submission is unsafe, and RHI submission must ignore slices, record original unsliced `passCommandInputs`, and report the fallback reason
- [x] T023 [P] [US2] Add DX12 draw command count contract coverage in `Tests/Unit/DX12PipelineLayoutUtilsTests.cpp`

### Implementation for User Story 2

- [x] T024 [US2] Add pass-slice metadata fields and a `requiresOrderedSlicedSubmission`/serial-fallback policy to `ParallelCommandWorkUnit` and `ParallelDrawCommandBatchMetadata` in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- [x] T025 [US2] Add deterministic recorded-draw slicing thresholds and helpers in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` and `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- [x] T026 [US2] Generate sliced `ParallelCommandWorkUnit` entries while keeping original unsliced `passCommandInputs` as the authoritative serial fallback source in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h` and `Runtime/Rendering/Context/RenderScenePackageBuilder.cpp`
- [x] T027 [US2] Remap dependency edges so incoming pass dependencies target the first slice, outgoing pass dependencies source from the last slice, and adjacent slices preserve ordering through ordered submission or explicit intra-pass dependency edges in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- [x] T028 [US2] Ensure cleared or attachment-backed passes are not sliced in the MVP, while attachment-free slices carry no clear ownership in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- [x] T029 [US2] Report parallel work-unit count, parallel worker count, and serial fallback reason through `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`, `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`, and `Runtime/Rendering/Data/FrameInfo.h`
- [x] T030 [US2] Change `RhiThreadCoordinator` work-unit selection so external-output, unsupported-backend, or unsafe paths ignore sliced `parallelCommandWorkUnits`, rebuild unsliced serial work from `passCommandInputs`, and record the fallback reason in `Runtime/Rendering/Context/RhiThreadCoordinator.cpp`

**Checkpoint**: User Story 2 is complete when large attachment-free recorded draw sets produce multiple ordered work units, attachment-backed child recording keeps one parent render pass on capable DX12 paths, and serial fallback remains correct.

---

## Phase 5: User Story 3 - Dense Instance Fields Have a Scalable Follow-Up Path (Priority: P3)

**Goal**: Prepare the architecture for future HISM-style cluster culling without implementing GPU-driven rendering in the MVP.

**Independent Test**: Design artifacts and tests identify where per-instance bounds, visibility ranges, and future instance-buffer ownership attach to the existing grouped draw path.

### Tests for User Story 3

- [x] T031 [P] [US3] Add a dense-instance grouping boundary test in `Tests/Unit/RenderSceneCacheTests.cpp`

### Implementation for User Story 3

- [x] T032 [US3] Document future instance-cluster ownership and culling insertion points in `specs/038-optimize-draw-calls/research.md`
- [x] T033 [US3] Keep dense-instance follow-up data fields out of runtime code unless required by US1 or US2 in `Runtime/Engine/Rendering/RenderScene.h`

**Checkpoint**: User Story 3 is complete when the MVP remains small and the follow-up culling path is explicitly documented.

---

## Final Phase: Polish & Cross-Cutting Concerns

**Purpose**: Validation, review, and sign-off evidence.

- [x] T034 Run `.\build_windows.bat Debug x64` from `.worktrees/038-optimize-draw-calls`
- [x] T035 Run `ctest --test-dir build/windows/Tests/Unit -C Debug --output-on-failure -R NullusUnitTests` and direct full `NullusUnitTests.exe`; note the top-level `build/windows` CTest aggregation-path access violation in `validation/final-diagnostics.md`
- [ ] T036 Capture DX12 RenderDoc evidence using `py -3 Tools/RenderDoc/renderdoc_runner.py --target editor --backend dx12 --capture --capture-after-frames 180` (attempted; direct automatic capture launched the DX12 editor but did not produce an `.rdc` before timeout, see `validation/final-diagnostics.md`)
- [x] T037 Run final `/plan-review` quality gate and fix all P0/P1 findings before sign-off (multi-agent review completed; final targeted re-review confirmed 0 P0/P1, with P2 follow-ups documented in `validation/final-diagnostics.md`)
- [x] T038 Update `specs/038-optimize-draw-calls/quickstart.md` with final validation commands and capture path

---

## Phase 6: UE-Inspired DX12 Bundle-Backed In-Render-Pass Parallel Recording

**Goal**: Replace the attachment-backed "do not slice" limitation with backend-gated child command buffers that record draw ranges in parallel and execute inside one parent render pass.

**Independent Test**: `ThreadedRenderingLifecycleTests` builds a 2,000-draw attachment-backed pass, enables DX12-style child command buffer support, and verifies the parent command buffer begins/ends the render pass exactly once while child command buffers record draw ranges without pass begin/end/barriers.

- [x] T039 [P] [US2] Add RHI child command buffer contract tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` proving child buffers cannot own BeginRenderPass/EndRenderPass/barriers for in-pass draw recording.
- [x] T040 [P] [US2] Add source-level DX12 contract tests in `Tests/Unit/DX12PipelineLayoutUtilsTests.cpp` requiring `D3D12_COMMAND_LIST_TYPE_BUNDLE`, `ExecuteBundle`, and no child render pass calls.
- [x] T041 [US2] Add RHI child command buffer creation/execution APIs and an `InRenderPassChildCommandBuffers` capability in `Runtime/Rendering/RHI/Core/RHICommand.h` and `Runtime/Rendering/RHI/RHITypes.h`.
- [x] T042 [US2] Implement DX12 bundle creation and parent `ExecuteChildCommandBuffer` in `Runtime/Rendering/RHI/Backends/DX12/DX12Command.h/.cpp`.
- [x] T043 [US2] Mark large attachment-backed recorded draw passes as in-render-pass child ranges while keeping unsliced `passCommandInputs` as the authoritative fallback in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h/.cpp`.
- [x] T044 [US2] Document the alignment boundary: Nullus uses UE-inspired draw-range partitioning with DX12 bundle-backed child recording, not UE 4.27's exact `FParallelCommandListSet` internals, in `specs/038-optimize-draw-calls/spec.md`.
- [x] T045 [US2] Teach `RhiThreadCoordinator` to record child draw ranges on workers, begin/end the parent render pass once, execute children in source order, and fall back to serial unsliced recording when child support is unavailable.
- [x] T046 [US2] Retain parent/child threaded submit resources and defer frame-scoped descriptor/upload/transient retirement per frame-context slot when a submitted frame fence wait times out, covered by `ThreadedRenderingLifecycleTests.InRenderPassChildCommandRecordingRetainsChildBuffersWhenFenceWaitTimesOut` and `ThreadedRenderingLifecycleTests.InRenderPassChildCommandRecordingKeepsTimedOutSlotResourcesAcrossOtherSlotReuse`.
- [x] T047 [US2] Run targeted red/green validation: `NullusUnitTests.exe --gtest_filter=ThreadedRenderingLifecycleTests.*:DX12PipelineLayoutUtilsTests.* --gtest_color=no`.
- [x] T048 [US2] Use slot-scoped reusable child command pools/buffers and draw-time DX12 descriptor heap/root-table flushing so worker child recording does not share active command pools, avoids per-frame bundle object creation, and reports descriptor heap incompatibility as a visible child execution failure instead of silently dropping draws.
- [x] T049 [US2] Re-run full build/tests, RenderDoc diagnostics where available, `/plan-review`, and mandatory multi-agent review for RHI/GPU sync changes. (build/tests and multi-agent review complete; RenderDoc direct capture attempted but no `.rdc` was produced, see `validation/final-diagnostics.md`)

---

## Dependencies & Execution Order

- **Phase 1**: No dependencies.
- **Phase 2**: Blocks all user stories because stats and shared counters are needed for validation.
- **User Story 1**: Starts after Phase 2 and delivers the MVP draw-call reduction.
- **User Story 2**: Starts after Phase 2 and can proceed in parallel with US1 after shared metadata is stable.
- **User Story 3**: Starts after US1 design is stable; it should not block MVP.
- **Final Phase**: Runs after selected user stories are implemented.

## Parallel Opportunities

- T004 and T005 can be written independently.
- T009 through T012 can be written in parallel before US1 implementation.
- T020 through T023 can be written in parallel before US2 implementation.
- US1 grouping work and US2 command slicing can proceed independently after Phase 2, but both touch telemetry and require integration review.

## Implementation Strategy

1. Complete Phase 1 and Phase 2.
2. Implement US1 first and validate the 1,000-object compatible opaque MVP.
3. Implement US2 and validate 2,000 non-groupable recorded draws on DX12 threaded rendering.
4. Keep US3 as documentation and boundary tests unless the MVP reveals a blocker.
5. Finish with unit tests, RenderDoc evidence, and `/plan-review`.
