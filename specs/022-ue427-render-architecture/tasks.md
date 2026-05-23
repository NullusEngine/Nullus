# Tasks: UE4.27 Render Architecture Alignment

**Input**: Design documents from `/specs/022-ue427-render-architecture/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Tests are required by FR-013 and must be written before implementation tasks for each story.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish traceability and non-parity documentation before code changes.

- [x] T001 Update `specs/022-ue427-render-architecture/research.md` if additional UE4.27 references are discovered during implementation
- [x] T002 [P] Add UE4.27 architecture tracking notes to `specs/022-ue427-render-architecture/quickstart.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add shared validation naming and helper conventions used by all four stories.

- [x] T003 Add `UE427`-prefixed test cases to existing renderer test files without changing production behavior
- [x] T004 Run targeted `NullusUnitTests` filter from `specs/022-ue427-render-architecture/quickstart.md` and confirm new tests fail for missing contracts

**Checkpoint**: Foundation ready - user story implementation can now begin in priority order.

---

## Phase 3: User Story 1 - RHI Command List Submission Contract (Priority: P1) MVP

**Goal**: Expose command-list lifecycle and submission metadata equivalent to UE4.27 command list flow.

**Independent Test**: Contract tests can create empty, graphics, compute, and child-list submissions and inspect metadata without a live backend.

### Tests for User Story 1

- [x] T005 [P] [US1] Add command list lifecycle contract tests in `Tests/Unit/RenderFrameworkContractTests.cpp`
- [x] T006 [P] [US1] Add child command list ordering tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`

### Implementation for User Story 1

- [x] T007 [US1] Add RHI command list lifecycle metadata types in `Runtime/Rendering/RHI/Core/RHICommand.h`
- [x] T008 [US1] Thread command list submission metadata into frame telemetry in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- [x] T009 [US1] Populate command list submission metadata in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- [x] T010 [US1] Run `NullusUnitTests` for `RenderFrameworkContractTests.*UE427*:ThreadedRenderingLifecycleTests.*UE427*`

**Checkpoint**: User Story 1 is independently testable and forms the MVP architecture slice.

---

## Phase 4: User Story 2 - RDG-Style Frame Graph Pass Ownership (Priority: P1)

**Goal**: Validate pass/resource declarations, side-effect retention, and graph-owned transitions before command recording.

**Independent Test**: Synthetic graph passes compile into resource dependencies or diagnostics without backend execution.

### Tests for User Story 2

- [x] T011 [P] [US2] Add pass side-effect retention tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [x] T012 [P] [US2] Add resource access diagnostic tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`

### Implementation for User Story 2

- [x] T013 [US2] Extend pass/resource validation diagnostics in `Runtime/Rendering/FrameGraph/FrameGraphExecutionPlan.h`
- [x] T014 [US2] Ensure prepared compute and scene graph passes publish declared accesses in `Runtime/Rendering/FrameGraph/SceneRenderGraphBuilder*.cpp`
- [x] T015 [US2] Preserve side-effect and extraction transition ownership in `Runtime/Rendering/FrameGraph/FrameGraphExecutionContext.h`
- [x] T016 [US2] Run `NullusUnitTests` for `FrameGraphSceneTargetsTests.*UE427*`

**Checkpoint**: RDG-style pass/resource contract is independently testable.

---

## Phase 5: User Story 3 - Shader Parameter Binding Contract (Priority: P2)

**Goal**: Represent frame, object, material, and pass bindings as structured parameter groups before pipeline layout creation.

**Independent Test**: Reflection-only tests validate group ordering, conflicts, missing resources, and empty descriptor set preservation.

### Tests for User Story 3

- [x] T017 [P] [US3] Add shader parameter group ordering tests in `Tests/Unit/ShaderBindingLayoutUtilsTests.cpp`
- [x] T018 [P] [US3] Add missing or stale pass binding diagnostics tests in `Tests/Unit/ShaderBindingLayoutUtilsTests.cpp`

### Implementation for User Story 3

- [x] T019 [US3] Add shader parameter group contract helpers in `Runtime/Rendering/Resources/ShaderBindingLayoutUtils.h`
- [x] T020 [US3] Implement shader parameter group validation in `Runtime/Rendering/Resources/ShaderBindingLayoutUtils.cpp`
- [x] T021 [US3] Keep RHI binding descriptors compatible with parameter groups in `Runtime/Rendering/RHI/Core/RHIBinding.h`
- [x] T022 [US3] Run `NullusUnitTests` for `ShaderBindingLayoutUtilsTests.*UE427*`

**Checkpoint**: Shader parameter binding contract is independently testable.

---

## Phase 6: User Story 4 - Parallel Draw Command Build And Submit (Priority: P2)

**Goal**: Publish draw command batches separately from immediate submission and preserve threaded dependency/eligibility metadata.

**Independent Test**: Prepared draw commands can be grouped into pass-role batches and applied to a threaded plan with dependency telemetry.

### Tests for User Story 4

- [x] T023 [P] [US4] Add draw command batch grouping tests in `Tests/Unit/ThreadedRenderingLifecycleTests.cpp`
- [x] T024 [P] [US4] Add compute-to-graphics dependency promotion tests in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`

### Implementation for User Story 4

- [x] T025 [US4] Add parallel draw command batch metadata in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.h`
- [x] T026 [US4] Build draw command batch metadata from scene packages in `Runtime/Rendering/Context/ThreadedRenderingLifecycle.cpp`
- [x] T027 [US4] Integrate batch metadata with `Runtime/Engine/Rendering/BaseSceneRenderer.cpp`
- [x] T028 [US4] Run `NullusUnitTests` for `ThreadedRenderingLifecycleTests.*UE427*:FrameGraphSceneTargetsTests.*UE427*`

**Checkpoint**: Parallel draw command batch contract is independently testable.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Validate the combined architecture slice and document remaining parity gaps.

- [x] T029 Run renderer contract test set from `specs/022-ue427-render-architecture/quickstart.md`
- [x] T030 Run `cmake --build .\Build --config Debug --target Editor`
- [x] T031 Attempt DX12 RenderDoc capture following `Docs/Rendering/RenderDocDebugging.md` and record evidence or blocker in `specs/022-ue427-render-architecture/research.md`
- [x] T032 Update `specs/022-ue427-render-architecture/tasks.md` with completion notes and remaining non-parity

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies.
- **Foundational (Phase 2)**: Depends on setup and blocks all user stories.
- **US1 and US2 (P1)**: Start after foundational. US1 should land before deeper submission integration; US2 can proceed independently in frame graph files.
- **US3 and US4 (P2)**: Start after foundational. US3 can proceed independently; US4 should account for US1 telemetry if already present.
- **Polish**: Depends on all implemented stories for this feature slice.

### Parallel Opportunities

- T005 and T006 can be written in parallel.
- T011 and T012 can be written in parallel.
- T017 and T018 can be written in parallel.
- T023 and T024 can be written in parallel.
- US2 and US3 implementation can proceed in parallel if write scopes remain separate.

## Implementation Strategy

### MVP First

1. Complete setup and foundational test naming.
2. Complete US1 RHI command list submission contract.
3. Validate US1 targeted tests before touching broader renderer flow.

### Incremental Delivery

1. Add failing contract tests.
2. Implement only the metadata or validation needed for the current story.
3. Run the story-specific test filter.
4. Rebuild Editor after completing each major phase.
5. Update research notes whenever a UE4.27 behavior is intentionally not matched.

## Notes

- Do not hand-edit generated files.
- Do not directly copy UE source into Nullus.
- Keep LightGrid compile context and Project Settings toggle behavior intact.
- Treat cross-backend parity as unproven until backend-specific evidence exists.

## 2026-05-10 LightGrid Hot Path Follow-up

- [x] Add a LightGrid contract test for stable-frame prepared resource caching in `Tests/Unit/RenderFrameworkContractTests.cpp`.
- [x] Cache `LightGridPrepass` prepared resources for identical frame/camera/light/grid inputs and reuse the graphics binding set without re-running LightGrid compute.
- [x] Use persistent descriptor allocations for cached LightGrid binding sets so cross-frame reuse does not retain transient frame descriptors.
- [x] Add GPU-side clear/reset pass for linked-list scratch/output resources so buffer reuse can also cover camera or light changes without stale links.
- [x] Cache dynamic-frame linked-list/output buffers by byte size and align their C++/shader naming with UE-style `StartOffsetGrid` and `CulledLightLinks`.
- [x] Align the graphics-facing LightGrid contract with UE4.27-style `ForwardLightData`, `ForwardLocalLightBuffer`, `NumCulledLightsGrid`, and `CulledLightDataGrid` names across C++, binding layouts, and HLSL.
- [x] Expose UE4.27 LightGrid stride/group-size constants and carry a lightweight `ForwardLightingResources` descriptor in `LightGridCompileContext`.
- [ ] Follow-up: collect Tracy or RenderDoc runtime evidence on DX12 before claiming the LightGrid hot path is fully UE4.27-parity.

## 2026-05-10 Shader Parameter Struct Follow-up

- [x] Add UE-style ShaderParameterStruct / GlobalShader / ComputeShaderUtils contract tests for pass layout, binding set, and dispatch creation.
- [x] Add lightweight header-only ShaderParameterStruct and ComputeShaderUtils helpers in Runtime/Rendering/Resources.
- [x] Migrate LightGrid reset/injection/compact compute pipeline layouts, binding sets, and recorded dispatch inputs to the new parameter-struct helpers.
- [ ] Follow-up: migrate graphics/material shaders (Standard, Lambert, StandardPBR, DeferredLighting, editor/debug shaders) to the same C++ definition and binding style.


## 2026-05-10 LightGrid Graphics Parameter Struct Follow-up

- [x] Extend LightGrid graphics pass bindings to LightGridGraphicsParameters so graphics LightGrid layout and binding set creation use the same shader-parameter-struct path as reset/injection/compact compute.
- [x] Remove remaining LightGrid graphics-side graphicsSetDesc.entries hand-built binding entries from LightGridPrepass.cpp.
- [x] Move Standard/Lambert/StandardPBR/DeferredLighting frame/material/object/pass binding definitions onto renderer-owned shader parameter structs registered by ShaderLoader.
- [x] Make Material explicit material layout and graphics pipeline layout prefer renderer-owned shader parameter structs, with reflection-only fallback preserved for custom/test shaders.
- [x] Document the complete UE4.27 source-level shader architecture alignment plan in `specs/022-ue427-render-architecture/shader-architecture-alignment-plan.md`.
- [ ] Follow-up: move editor/debug shaders and DeferredGBuffer onto the same renderer-owned shader class path.

## 2026-05-10 Shader Architecture Registry Slice

- [x] Add `ShaderArchitectureAlignmentTests` covering engine shader type lookup by name/source path and custom shader fallback behavior.
- [x] Add lightweight `ShaderPermutationId`, `ShaderPermutationParameters`, `ShaderType`, and `ShaderTypeRegistry` infrastructure.
- [x] Register Standard/Lambert/StandardPBR/DeferredLighting pixel shader types with root parameter structs owned by the registry.
- [x] Make `ShaderLoader` prefer shader-type-owned root parameter structs for registered engine shader paths while preserving the legacy fallback for non-migrated shaders.
- [x] Validate with `NullusUnitTests` filters `ShaderArchitectureAlignmentTests.*` and `ShaderArchitectureAlignmentTests.*:ShaderBindingLayoutUtilsTests.*`.
- [ ] Follow-up: replace the remaining filename fallback with concrete shader classes, shader maps, and typed pass/material binding before claiming UE4.27 shader architecture parity.

## 2026-05-10 Shader Architecture Completion Slice

- [x] Add contract coverage for root parameter metadata, global shader classes, material shader classes, shader map refs, and `ShaderLoader` no-filename-switch ownership.
- [x] Add `ShaderRootParameterMetadata` and make shader types expose metadata as the source of root parameter structs.
- [x] Register material VS/PS shader types for Standard, Lambert, StandardPBR, and DeferredGBuffer.
- [x] Register global shader types for DeferredLightingPS and LightGrid reset/injection/compact compute shaders.
- [x] Add renderer-owned shader class headers for material shaders, DeferredLighting, and LightGrid.
- [x] Add lightweight `ShaderMap` / `ShaderMapRef` and RDG-style parameter allocator / binder scaffolding.
- [x] Remove engine shader filename parameter switches from `ShaderLoader.cpp`; registered engine shaders now resolve parameters through `ShaderTypeRegistry`, and unregistered/custom shader paths continue through reflection fallback.
- [x] Move LightGrid compute parameter definitions to the shader type metadata path used by the new shader classes.
- [ ] Follow-up: collect DX12 runtime evidence and RenderDoc/Tracy capture before claiming full runtime parity.
- [ ] Follow-up: replace remaining raw `Shader*` ownership in `Material` and renderer passes with live shader-map-owned compiled shader instances.

