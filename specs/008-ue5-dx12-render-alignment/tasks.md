# Tasks: UE5-Style DX12 Render Alignment

**Input**: Design documents from `/specs/008-ue5-dx12-render-alignment/`  
**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/render-architecture-contract.md`

**Tests**: This feature requires targeted unit tests, DX12 runtime smoke, explicit startup-failure validation, and RenderDoc evidence.

## Format: `[ID] [P?] [Story] Description`

- `[P]`: Can run in parallel when write scopes do not overlap
- `[Story]`: User story mapping (`US1`, `US2`, `US3`, `US4`, `US5`)

## Phase 1: Setup And UE Baseline

**Purpose**: Establish the new spec bundle, encode the UE-alignment baseline, and gate phase 1 to DX12-only execution.

- [X] T001 Maintain the `specs/008-ue5-dx12-render-alignment/` bundle as the single source of truth for this feature.
- [X] T002 [P] Record the UE 5.7 contract baseline and closure-audit expectations in `specs/008-ue5-dx12-render-alignment/research.md`, `specs/008-ue5-dx12-render-alignment/contracts/render-architecture-contract.md`, and `Docs/Rendering/RHIMultiBackendArchitecture.md`.
- [X] T003 [P] Add new contract-test scaffolding in `Tests/Unit/CMakeLists.txt`, `Tests/Unit/UE5RenderArchitectureContractTests.cpp`, and `Tests/Unit/EditorRenderPathContractTests.cpp`.
- [X] T004 Gate the new architecture to DX12-only in `Runtime/Rendering/Settings/GraphicsBackendUtils.h`, `Runtime/Rendering/Context/Driver.cpp`, `Project/Editor/Core/Context.cpp`, `Project/Game/Core/Context.cpp`, `Project/Game/LaunchArgs.*`, and `Project/Launcher/Core/Launcher.cpp`.

**Checkpoint**: The repository has a new DX12-only alignment bundle and a test surface for forbidden paths.

---

## Phase 2: Foundational Ownership And Artifact Convergence

**Purpose**: Introduce the frame-owned artifacts and coordinators that later phases depend on.

- [X] T005 [US1] Add ownership-boundary tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` and `Tests/Unit/UE5RenderArchitectureContractTests.cpp` for Game Thread publication, Render Thread build, and RHI Thread execution.
- [X] T006 [US1] Introduce authoritative frame-owned artifacts in `Runtime/Rendering/Context/RenderFrameInput.h`, `Runtime/Rendering/Context/RenderFrameBuild.h`, and `Runtime/Rendering/Context/ThreadedRenderingLifecycle.*`.
- [X] T007 [US1] Split mixed orchestration out of `Runtime/Rendering/Context/Driver.*` into `Runtime/Rendering/Context/RenderThreadCoordinator.*` and `Runtime/Rendering/Context/RhiThreadCoordinator.*`.
- [X] T008 [US1] Remove normal-path main-thread explicit frame entrypoints from `Runtime/Rendering/Context/DriverAccess.h`, `Runtime/Rendering/Core/ABaseRenderer.*`, and `Runtime/Rendering/Core/CompositeRenderer.*`.

**Checkpoint**: Frame ownership has explicit artifacts and the main thread no longer owns normal-path recording APIs.

---

## Phase 3: User Story 1 - Establish Single Authoritative Rendering Ownership (Priority: P1) 🎯 MVP

**Goal**: Move Nullus to one authoritative Game Thread -> Render Thread -> RHI Thread frame lifecycle.

**Independent Test**: Run focused lifecycle and attribution tests plus DX12 Editor/Game smoke proving the intended ownership stages with no ambiguous same-thread render execution.

### Tests for User Story 1

- [X] T009 [P] [US1] Extend lifecycle attribution coverage in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp` and `Tests/Unit/RendererStatsTests.cpp`.
- [X] T010 [P] [US1] Add DX12 startup-failure and forbidden-direct-submit regression coverage in `Tests/Unit/GraphicsBackendUtilsTests.cpp` and `Tests/Unit/DriverNullDeviceFallbackTests.cpp`.

### Implementation for User Story 1

- [X] T011 [US1] Refactor `Runtime/Rendering/Context/Driver.cpp` so the Game Thread only publishes immutable frame input for runtime and editor rendering.
- [X] T012 [US1] Refactor `Runtime/Engine/Rendering/BaseSceneRenderer.*`, `Runtime/Rendering/Core/CompositeRenderer.*`, and `Runtime/Rendering/Core/ABaseRenderer.*` so the Render Thread becomes the authoritative owner of frame build.
- [X] T013 [US1] Refactor `Runtime/Rendering/Context/RhiThreadCoordinator.*`, `Runtime/Rendering/Context/Driver.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/*` so the RHI Thread alone owns submission, present, readback completion, and retirement.

**Checkpoint**: Ownership rules are explicit, testable, and aligned to the intended UE-style lifecycle.

---

## Phase 4: User Story 2 - Make RDG The Only Rendering Scheduler (Priority: P1)

**Goal**: Make RDG the sole truth source for scheduling, import/extract, lifetime, and graph-visible readback.

**Independent Test**: Execute representative Editor and Game frame construction through the graph and verify that pass order, external resource boundaries, and readback scheduling are graph-owned.

### Tests for User Story 2

- [X] T014 [P] [US2] Add graph-authority coverage in `Tests/Unit/FrameGraphSceneTargetsTests.cpp` and `Tests/Unit/EditorRenderPathContractTests.cpp` for import, extraction, pass order, and readback scheduling.
- [X] T015 [P] [US2] Add forbidden renderer-local scheduling tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` and `Tests/Unit/CompositeRendererExplicitDrawOrderTests.cpp`.

### Implementation for User Story 2

- [X] T016 [US2] Refactor `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`, `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`, and `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder.*` so compiled graph results become the only scheduling truth.
- [X] T017 [US2] Reduce `Runtime/Engine/Rendering/ScenePassSchemas.h` to descriptive pass taxonomy and move authoritative ordering policy into `Runtime/Rendering/FrameGraph/*`.
- [X] T018 [US2] Refactor `Runtime/Engine/Rendering/ForwardSceneRenderer.*`, `Runtime/Engine/Rendering/DeferredSceneRenderer.*`, and `Runtime/Engine/Rendering/FrameGraphSceneTargets.h` so runtime scene passes emerge from RDG build rather than renderer-local orchestration.
- [X] T019 [US2] Introduce explicit external resource and readback bridging in `Runtime/Rendering/FrameGraph/ExternalResourceBridge.*` and integrate it in `Runtime/Rendering/Context/Driver.cpp`.

**Checkpoint**: RDG is the sole authoritative scheduler for runtime and editor-visible frame work.

---

## Phase 5: User Story 3 - Remove Compatibility Paths And Fallbacks From The DX12 Mainline (Priority: P2)

**Goal**: Delete compatibility execution and fallback behavior from the accepted DX12 rendering mainline.

**Independent Test**: Run focused unit and DX12 runtime validation proving that no accepted frame uses direct-submit fallback, driver-built fallback packages, compatibility acquire/present, or alternate runtime backend selection.

### Tests for User Story 3

- [X] T020 [P] [US3] Add forbidden-compatibility regression tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` and `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`.
- [X] T021 [P] [US3] Add DX12-only routing and no-fallback startup tests in `Tests/Unit/GraphicsBackendUtilsTests.cpp` and `Tests/Unit/DriverNullDeviceFallbackTests.cpp`.

### Implementation for User Story 3

- [X] T022 [US3] Remove driver-built fallback scene-package logic from `Runtime/Rendering/Context/Driver.cpp` and `Runtime/Rendering/Context/ThreadedRenderingLifecycle.*`.
- [X] T023 [US3] Remove compatibility acquire/present and on-demand present behavior from `Runtime/Rendering/Context/Driver.cpp` and `Runtime/Rendering/Context/DriverAccess.h`.
- [X] T024 [US3] Remove remaining direct-submit compatibility hooks from `Runtime/Rendering/Core/ABaseRenderer.*`, `Runtime/Rendering/Core/CompositeRenderer.*`, and `Project/Launcher/Core/Launcher.cpp`.
- [X] T025 [US3] Make unavailable DX12 configurations stop explicitly in `Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp`, `Project/Editor/Core/Context.cpp`, `Project/Game/Core/Context.cpp`, `Project/Game/LaunchArgs.*`, and `Project/Launcher/Core/Launcher.cpp`.

**Checkpoint**: The accepted DX12 mainline has no compatibility or fallback rendering path.

---

## Phase 6: User Story 4 - Unify Editor And Game Under The Same Render Architecture (Priority: P2)

**Goal**: Make Editor and Game consumers of the same authoritative render pipeline.

**Independent Test**: Validate scene view, game view, offscreen rendering, picking, gizmo, grid, outline, and overlays through the same DX12 frame pipeline with no editor-only bypasses.

### Tests for User Story 4

- [X] T026 [P] [US4] Add editor-path unification tests in `Tests/Unit/EditorRenderPathContractTests.cpp` and `Tests/Unit/PanelWindowHookTests.cpp`.
- [X] T027 [P] [US4] Add backend-branch and editor-bypass audit tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` and `Tests/Unit/GraphicsBackendUtilsTests.cpp`.

### Implementation for User Story 4

- [X] T028 [US4] Refactor `Project/Editor/Rendering/PickingRenderPass.*`, `Project/Editor/Rendering/GizmoRenderer.*`, `Project/Editor/Rendering/GridRenderPass.*`, and `Project/Editor/Rendering/OutlineRenderer.*` so they execute only as graph-visible editor work or graph-visible readback requests.
- [X] T029 [US4] Refactor `Project/Editor/Panels/SceneView.*`, `Project/Editor/Panels/GameView.*`, and `Project/Editor/Core/Application.*` so viewports and offscreen surfaces consume unified frame outputs rather than special submission paths.
- [X] T030 [US4] Move editor/game external resource handoff into `Runtime/Rendering/FrameGraph/ExternalResourceBridge.*`, `Runtime/Rendering/Context/RenderFrameInput.h`, and `Runtime/Rendering/Context/RhiThreadCoordinator.*`.
- [X] T031 [US4] Narrow renderer and editor code to backend-neutral interfaces in `Runtime/Rendering/RHI/Core/*` and keep DX12-specific execution only under `Runtime/Rendering/RHI/Backends/DX12/*`.

**Checkpoint**: Editor and Game now behave as different consumers of one authoritative render architecture.

---

## Phase 7: User Story 5 - Enforce Central Rendering Infrastructure As Mandatory Mainline (Priority: P2)

**Goal**: Make PSO, descriptor, transient-lifetime, and retirement systems mandatory and prove zero accepted bypasses.

**Independent Test**: Run focused unit and runtime validation proving that accepted frames use only centralized PSO, descriptor, transient-resource, and retirement systems.

### Tests for User Story 5

- [X] T032 [P] [US5] Add centralized-infrastructure tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`, `Tests/Unit/FrameGraphSceneTargetsTests.cpp`, and `Tests/Unit/RHIPipelineStateUtilsTests.cpp`.
- [X] T033 [P] [US5] Add binding, pipeline, and transient-bypass prohibition tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` and `Tests/Unit/RendererFrameObjectBindingTests.cpp`.

### Implementation for User Story 5

- [X] T034 [US5] Make `Runtime/Rendering/RHI/Utils/DescriptorAllocator/*` the only binding allocation path in `Runtime/Rendering/Resources/Material.cpp`, `Runtime/Engine/Rendering/LightGridPrepass.*`, and `Runtime/Rendering/Core/FrameObjectBindingProvider.*`.
- [X] T035 [US5] Make `Runtime/Rendering/RHI/Utils/PipelineCache/*` the only graphics and compute PSO acquisition path in `Runtime/Rendering/Resources/Material.cpp`, `Runtime/Engine/Rendering/LightGridPrepass.*`, and `Runtime/Engine/Rendering/*SceneRenderer.*`.
- [X] T036 [US5] Make `Runtime/Rendering/RHI/Utils/ResourceStateTracker/*` and `Runtime/Rendering/FrameGraph/*` the only transient-lifetime path for graph-created, extracted, and readback-related resources.
- [X] T037 [US5] Extend `Runtime/Rendering/Context/Driver.cpp` and `Runtime/Rendering/Core/RendererStats.*` with diagnostics that prove zero accepted bypasses of descriptor, PSO, transient-lifetime, and retirement systems.

**Checkpoint**: Low-level rendering infrastructure is centralized, mandatory, and auditable.

---

## Phase 8: Polish And Acceptance

**Purpose**: Validate the architecture with product evidence and keep documentation truthful.

- [X] T038 [P] Update `specs/008-ue5-dx12-render-alignment/quickstart.md`, `specs/008-ue5-dx12-render-alignment/research.md`, and `Docs/Rendering/RHIMultiBackendArchitecture.md` with landed behavior and evidence.
- [X] T039 Run targeted unit suites for this feature and summarize the exact passing filters in `specs/008-ue5-dx12-render-alignment/quickstart.md`.
- [X] T040 Run DX12 Editor runtime validation plus RenderDoc evidence for scene view, game view, offscreen rendering, picking, gizmo, grid, outline, and overlays.
- [X] T041 Run DX12 Game runtime validation plus RenderDoc evidence and explicit startup-failure validation when DX12 is unavailable.
- [X] T042 Perform a final UE source-audit pass and self-review diffs for remaining compatibility code, fallback code, dual scheduling truth, editor-only bypasses, or backend-name execution branches.

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 starts immediately and defines the baseline plus DX12-only gate.
- Phase 2 blocks all later work because the frame-owned artifacts and coordinators are foundational.
- User Story 1 depends on Phase 2.
- User Story 2 depends on User Story 1 because graph authority requires explicit thread ownership first.
- User Story 3 depends on User Story 1 and User Story 2 because forbidden-path removal is only safe once the authoritative path exists.
- User Story 4 depends on User Story 2 and User Story 3 because editor unification requires both graph authority and fallback removal.
- User Story 5 depends on User Story 2 and User Story 3 because central infrastructure must be enforced on the final accepted path.
- Phase 8 depends on all accepted story work.

### Parallel Opportunities

- T002 and T003 can run in parallel because documentation and test scaffolding are disjoint.
- Within each user story, tests marked `[P]` can run in parallel before implementation.
- DX12 gate cleanup and editor-path cleanup can proceed in parallel once the foundational ownership types are in place.
- Central PSO, descriptor, and transient-lifetime enforcement can be split across different files once graph authority and forbidden-path removal are complete.

### Implementation Strategy

- First deliver User Story 1 plus the minimal Phase 2 work needed to prove one authoritative ownership model.
- Then deliver User Story 2 so RDG becomes the only scheduler before deleting remaining compatibility code.
- Delete compatibility and fallback behavior in User Story 3 only after the authoritative DX12 path is complete enough to carry both Editor and Game.
- Unify Editor in User Story 4 before declaring architectural purity.
- Close with User Story 5 so low-level infrastructure becomes mandatory rather than advisory.

---

## Phase 9: DX12 Backend Module Split And UE-Style RHI Backend Hygiene

**Purpose**: Split the accepted DX12 backend into single-purpose implementation units while preserving the existing RHI contract and DX12-only runtime path.

- [X] T043 Add DX12 backend split contract tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` proving synchronization objects live outside `DX12ExplicitDeviceFactory.cpp`.
- [X] T044 Move DX12 fence and semaphore implementations into `Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.h` and `Runtime/Rendering/RHI/Backends/DX12/DX12Synchronization.cpp`.
- [X] T045 Rebuild `NullusUnitTests` and run focused DX12 synchronization split tests plus architecture regression.
- [X] T046 Move queue submission and present logic into dedicated DX12 queue source files without changing `RHIQueue` behavior.
- [X] T047 Move command pool and command buffer implementation into dedicated DX12 command source files.
- [X] T048 Move swapchain implementation into dedicated DX12 swapchain source files.
- [X] T049 Move binding set and descriptor writing implementation into dedicated DX12 descriptor source files.
- [X] T050 Move device construction/capability assembly into a small DX12 device module and leave `DX12ExplicitDeviceFactory.cpp` as a thin factory entrypoint.
- [X] T051 Run final build, architecture regression, and source audit proving `DX12ExplicitDeviceFactory.cpp` no longer owns unrelated backend responsibilities.

---

## Phase 10: Resource Wrapper Fail-Fast Hygiene

**Purpose**: Remove silent driver/device acquisition fallbacks from backend-neutral resource wrappers so missing render infrastructure fails explicitly instead of producing half-initialized legacy-compatible objects.

- [X] T052 Add resource-wrapper contract tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp` proving core buffer and texture wrappers do not swallow Driver/RHI acquisition failures.
- [X] T053 Remove silent `catch (...)` device acquisition paths from `Runtime/Rendering/Buffers/*` and `Runtime/Rendering/Resources/Texture.cpp` while preserving formal RHI handles.
- [X] T054 Rebuild `NullusUnitTests` and run focused wrapper contracts plus architecture regression.

---

## Phase 11: Shader Storage Buffer RHI Lifetime Cleanup

**Purpose**: Make shader-storage buffer resources follow explicit RHI lifetime rules: no constructor-time zero-byte placeholder allocations, no dead legacy access mapping, and no missed first upload when the buffer has not been created yet.

- [X] T055 Add shader-storage buffer lifecycle contract tests in `Tests/Unit/UE5RenderArchitectureContractTests.cpp`.
- [X] T056 Refactor `Runtime/Rendering/Buffers/ShaderStorageBuffer.*` so `SendBlocks` creates storage buffers only for non-empty payloads and removes dead legacy access mapping.
- [X] T057 Rebuild `NullusUnitTests` and run focused SSBO contracts plus architecture regression.
