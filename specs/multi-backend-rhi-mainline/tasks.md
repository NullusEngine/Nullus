# Tasks: Multi-Backend RHI Mainline

**Input**: Design documents from `/specs/multi-backend-rhi-mainline/`
**Prerequisites**: `plan.md`, `spec.md`

**Tests**: Use focused build and runtime validation. Add or update targeted `NullusUnitTests` coverage where stable entrypoints exist, and capture backend-specific runtime evidence for `Editor`, `Game`, and smoke demos.

**Organization**: Tasks are grouped by user story and foundational setup so each slice can be reviewed with explicit backend and product validation outcomes.

## Phase 1: Setup

- [ ] T001 Create and maintain the committed spec bundle in `specs/multi-backend-rhi-mainline/`
- [ ] T002 Record backend tiers, validation policy, and delivery expectations in `specs/multi-backend-rhi-mainline/spec.md`, `specs/multi-backend-rhi-mainline/plan.md`, and `specs/multi-backend-rhi-mainline/tasks.md`
- [ ] T003 Inventory the current formal-RHI-versus-legacy touchpoints in `Runtime/Rendering/Context/Driver.cpp`, `Runtime/Rendering/Core/ABaseRenderer.cpp`, `Runtime/Rendering/Resources/Material.cpp`, and `Runtime/Rendering/RHI/ExplicitRHICompat.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Expand backend configuration and formal RHI entry surfaces before migrating user stories

- [ ] T004 Add `DX11` to backend selection enums and string conversion in `Runtime/Rendering/Settings/EGraphicsBackend.h` and `Runtime/Rendering/Settings/GraphicsBackendUtils.h`
- [ ] T005 Update backend defaults, backend parsing, and product/runtime selection flow in `Runtime/Rendering/Settings/DriverSettings.h`, `Project/Editor/Core/*`, and `Project/Game/Core/*`
- [ ] T006 Extend native backend reporting and factory dispatch in `Runtime/Rendering/RHI/RHITypes.h`, `Runtime/Rendering/RHI/Backends/RenderDeviceFactory.h`, `Runtime/Rendering/RHI/Backends/RenderDeviceFactory.cpp`, `Runtime/Rendering/RHI/Backends/ExplicitDeviceFactory.h`, and `Runtime/Rendering/RHI/Backends/ExplicitDeviceFactory.cpp`
- [ ] T007 Create the `Runtime/Rendering/RHI/Backends/DX11/` backend slice with initial device/factory scaffolding that can participate in the formal RHI entry flow
- [ ] T008 Update UI and tooling backend awareness in `Runtime/UI/UIManager.cpp`, `Runtime/UI/UIManager.h`, `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`, and `Runtime/Rendering/Tooling/RenderDocCaptureController.cpp`

**Checkpoint**: All intended backends can be selected through one product-facing backend matrix, and the repository has a concrete place for Tier B `DX11` work.

---

## Phase 3: User Story 1 - Run The Main Rendering Path Only Through Formal RHI (Priority: P1) 🎯 MVP

**Goal**: Make renderer, frame graph, material, and wrapper mainline code consume formal RHI contracts rather than legacy immediate rendering APIs.

**Independent Test**: Run a Tier A backend and confirm the renderer frame lifecycle, pipeline creation, binding, and resource usage remain on formal RHI objects without renderer-mainline calls back into legacy draw-state APIs.

### Tests And Validation For User Story 1

- [ ] T009 [US1] Add or update focused unit coverage for formal pipeline, binding, or resource translation behavior in `Tests/Unit/`
- [ ] T010 [US1] Capture a codepath audit note in `specs/multi-backend-rhi-mainline/` showing which renderer and material call sites still depend on `GraphicsPipelineDesc`, `BindingSetInstance`, or immediate `IRenderDevice` APIs

### Implementation For User Story 1

- [ ] T011 [US1] Move `Runtime/Rendering/Resources/Material.h` and `Runtime/Rendering/Resources/Material.cpp` to formal-RHI-first pipeline and binding production for renderer mainline consumption
- [ ] T012 [US1] Remove renderer-mainline dependence on legacy pipeline and binding descriptors in `Runtime/Rendering/Core/ABaseRenderer.h` and `Runtime/Rendering/Core/ABaseRenderer.cpp`
- [x] T013 [US1] Finish formal resource and barrier usage in `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`, `Runtime/Rendering/FrameGraph/FrameGraphBuffer.*`, `Runtime/Rendering/FrameGraph/FrameGraphTexture.*`, and related frame graph support files
- [x] T014 [US1] Align wrapper resources and compatibility handoff points in `Runtime/Rendering/Resources/Texture.*`, `Runtime/Rendering/Buffers/*`, and `Runtime/Rendering/Resources/BindingSet.*` so formal RHI objects are the preferred surface
- [ ] T015 [US1] Reduce `Driver` exposure of renderer-mainline legacy operations in `Runtime/Rendering/Context/Driver.h` and `Runtime/Rendering/Context/Driver.cpp`
- [ ] T016 [US1] Update engine and editor rendering integration points in `Runtime/Engine/Rendering/*` and `Project/Editor/Rendering/*` so they consume the formal RHI mainline
- [ ] T017 [US1] Confine `IRenderDevice` usage to compatibility and backend-internal infrastructure in `Runtime/Rendering/RHI/IRenderDevice.h`, `Runtime/Rendering/RHI/ExplicitRHICompat.cpp`, and remaining callers

**Checkpoint**: Tier A renderer mainline no longer requires legacy renderer-side pipeline, binding, or immediate draw-state abstractions.

---

## Phase 4: User Story 2 - Support Multiple Backends Under One RHI Contract (Priority: P2)

**Goal**: Support `DX12`, `Vulkan`, `DX11`, and `OpenGL` through one formal RHI entry, with Tier A backends owning explicit execution and Tier B backends entering through the same formal surface.

**Independent Test**: Create each backend through the same driver path and verify backend-specific capability differences are exposed through the formal device and documentation rather than through renderer architecture forks.

### Tests And Validation For User Story 2

- [ ] T018 [US2] Add backend creation and capability smoke coverage in `Tests/Unit/` for backend parsing, selection, and capability reporting
- [ ] T019 [US2] Record backend-tier acceptance criteria and capability gaps in `specs/multi-backend-rhi-mainline/` before implementation claims are closed

### Implementation For User Story 2

- [ ] T020 [US2] Replace compatibility-backed `DX12` explicit device wrappers in `Runtime/Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.*` with backend-owned formal `RHIDevice` implementations
- [ ] T021 [US2] Replace compatibility-backed `Vulkan` explicit device wrappers in `Runtime/Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.*` with backend-owned formal `RHIDevice` implementations
- [ ] T022 [US2] Implement native queue, swapchain, fence, semaphore, command pool, and command buffer ownership for `DX12` in `Runtime/Rendering/RHI/Backends/DX12/*`
- [ ] T023 [US2] Implement native queue, swapchain, fence, semaphore, command pool, and command buffer ownership for `Vulkan` in `Runtime/Rendering/RHI/Backends/Vulkan/*`
- [ ] T024 [US2] Implement formal pipeline, binding, and resource creation for `DX12` in `Runtime/Rendering/RHI/Backends/DX12/*`
- [ ] T025 [US2] Implement formal pipeline, binding, and resource creation for `Vulkan` in `Runtime/Rendering/RHI/Backends/Vulkan/*`
- [ ] T026 [US2] Integrate `DescriptorAllocator`, `PipelineCache`, `ResourceStateTracker`, and `UploadContext` into `DX12` backend execution through `Runtime/Rendering/RHI/Utils/*` and `Runtime/Rendering/RHI/Backends/DX12/*`
- [ ] T027 [US2] Integrate `DescriptorAllocator`, `PipelineCache`, `ResourceStateTracker`, and `UploadContext` into `Vulkan` backend execution through `Runtime/Rendering/RHI/Utils/*` and `Runtime/Rendering/RHI/Backends/Vulkan/*`
- [ ] T028 [US2] Implement the new Tier B `DX11` backend entry and formal device adaptation in `Runtime/Rendering/RHI/Backends/DX11/*`
- [ ] T029 [US2] Align `OpenGL` formal RHI entry behavior and Tier B capability reporting in `Runtime/Rendering/RHI/Backends/OpenGL/*` and `Runtime/Rendering/RHI/Backends/OpenGL/Compat/*`
- [ ] T030 [US2] Update capability reporting, backend descriptions, and any backend-specific startup assumptions in `Runtime/Rendering/Settings/GraphicsBackendUtils.h`, `Runtime/Rendering/Context/Driver.cpp`, and related backend metadata code

**Checkpoint**: All four backends enter through the formal RHI API surface, with `DX12` and `Vulkan` no longer depending on `CreateCompatibilityExplicitDevice` for their primary path.

---

## Phase 5: User Story 3 - Keep Editor And Game Runnable With Correct Rendering (Priority: P3)

**Goal**: Preserve product usability and rendering correctness for `Editor` and `Game` while the backend architecture changes underneath.

**Independent Test**: Launch `Editor` and `Game` on each supported backend, confirm startup and present, and gather evidence that representative rendering output is correct.

### Tests And Validation For User Story 3

- [ ] T031 [US3] Define backend-by-backend product smoke criteria for `Editor` and `Game` in `specs/multi-backend-rhi-mainline/`
- [ ] T032 [US3] Capture Tier A RenderDoc evidence for `Editor` or `Game` using the workflow in `Docs/Rendering/RenderDocDebugging.md` and `Tools/RenderDoc/*`
- [x] T033 [US3] Record Tier B focused runtime verification notes for `DX11` and `OpenGL` in `specs/multi-backend-rhi-mainline/`

### Implementation For User Story 3

- [x] T034 [US3] Update product startup and driver wiring in `Project/Editor/Core/*`, `Project/Game/Core/*`, and `Runtime/Rendering/Context/Driver.*` for the expanded backend matrix
- [ ] T035 [US3] Ensure editor rendering features remain functional on the formal RHI mainline in `Project/Editor/Rendering/*` and `Project/Editor/Panels/*`
- [ ] T036 [US3] Ensure game runtime rendering remains functional on the formal RHI mainline in `Runtime/Engine/Rendering/*`, `Runtime/Engine/Components/*`, and `Project/Game/*`
- [ ] T037 [US3] Fix backend-specific scene renderer, offscreen target, readback, or present issues discovered during smoke validation in `Runtime/Engine/Rendering/*`, `Runtime/Rendering/FrameGraph/*`, and affected backend files
- [ ] T038 [US3] Update backend-specific UI bridge and presentation integration where product rendering requires it in `Runtime/UI/*` and `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp`

**Checkpoint**: `Editor` and `Game` run on the supported backends, and rendering correctness evidence exists for representative product paths.

---

## Phase 6: User Story 4 - Ship A Minimum Delivery Surface For Ongoing Backend Work (Priority: P4)

**Goal**: Leave behind a maintainable delivery surface with demos, correctness checks, docs, and capability reporting.

**Independent Test**: Run the minimum demo and smoke matrix, inspect the docs, and confirm support claims match recorded evidence.

### Tests And Validation For User Story 4

- [ ] T039 [US4] Add targeted correctness tests for upload, sync, barrier, acquire-present, readback, and pipeline binding in `Tests/Unit/`
- [ ] T040 [US4] Validate the minimum demo set on the supported backend matrix and record outcomes in `specs/multi-backend-rhi-mainline/`

### Implementation For User Story 4

- [ ] T041 [US4] Create the minimum demo surface under `Project/RenderingDemos/`, including build wiring in `Project/RenderingDemos/CMakeLists.txt` and demo slices for `Triangle`, `TexturedQuad`, `OffscreenRender`, and `ComputeClear`
- [ ] T042 [US4] Write backend capability and smoke matrix documentation in `Docs/Rendering/` and `specs/multi-backend-rhi-mainline/`
- [ ] T043 [US4] Document backend tier expectations, product validation evidence, and known gaps in `Docs/Rendering/` and `README.md` if repository-level guidance needs updating
- [ ] T044 [US4] Reconcile RenderDoc tooling and backend-specific validation scripts in `Tools/RenderDoc/*` with the final backend matrix

**Checkpoint**: The repository has a minimum delivery and validation surface for the formal multi-backend RHI.

---

## Final Verification

- [ ] T045 Build the affected targets, including runtime modules, `Editor`, `Game`, `Project/RenderingDemos/*`, `NullusUnitTests`, and `ReflectionTest`
- [x] T046 Run targeted automated checks and record exact commands and results in `specs/multi-backend-rhi-mainline/`
- [ ] T047 Run backend-specific smoke validation for `Editor`, `Game`, and the minimum demo set, recording which backend, what evidence, and what remains unverified
- [ ] T048 Re-read `AGENTS.md`, `Docs/AIWorkflow.md`, and `Docs/Rendering/RenderDocDebugging.md` to confirm the completed change still follows repository workflow and validation rules
