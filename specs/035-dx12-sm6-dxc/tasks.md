# Tasks: Prefer Project DXC And Gate DX12 SM6

**Input**: Design documents from `specs/035-dx12-sm6-dxc/`
**Prerequisites**: `plan.md`, `spec.md`

## Phase 1: Setup

**Purpose**: Confirm current behavior and create focused regression coverage.

- [X] T001 Inspect DXC lookup order in `Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp`
- [X] T002 Inspect DX12 device creation and capability flow in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.*`
- [X] T003 [P] [US1] Add failing DXC lookup-order regression coverage in `Tests/Unit/ShaderCompilerTests.cpp`
- [X] T004 [P] [US2] Add failing DX12 SM6 initialization-order regression coverage in `Tests/Unit/EditorLaunchArgsTests.cpp`

---

## Phase 2: User Story 1 - Consistent Project Shader Compiler (Priority: P1)

**Goal**: Project-internal DXC candidates are considered before any environment or SDK candidates.

**Independent Test**: Run the new `ShaderCompilerTests` lookup-order regression.

- [X] T005 [US1] Move bundled/project DXC discovery before `DXC_PATH`, Vulkan SDK, and Windows SDK fallback checks in `Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp`
- [X] T006 [US1] Update DXC-not-found diagnostics in `Runtime/Rendering/ShaderCompiler/ShaderCompiler.cpp`

---

## Phase 3: User Story 2 - Clear DX12 Shader Model Gate (Priority: P1)

**Goal**: DX12 resources become valid only after SM6 support is queried and confirmed.

**Independent Test**: Run the new DX12 initialization-order regression and, where hardware allows, existing DX12 device-resource tests.

- [X] T007 [US2] Add SM6 support fields to `DX12DeviceResources` in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.h`
- [X] T008 [US2] Add a DX12 shader-model support query in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`
- [X] T009 [US2] Gate `CreateDX12DeviceResources` on SM6 before command queue creation in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`
- [X] T010 [US2] Update valid-resource checks or tests so only SM6-confirmed DX12 resources are considered ready
- [X] T010a [US2] Require SM6 during DX12 hardware adapter selection and preserve rejection diagnostics in `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`

---

## Phase 4: Polish & Review

**Purpose**: Validate and review the completed change.

- [X] T011 Run targeted unit tests for DXC lookup and DX12 SM6 contract coverage
- [X] T012 Run the narrowest available build/test command if test binary relinking is required
- [X] T013 Update this task list with validation evidence
- [X] T014 Run required plan-review quality gate
- [X] T015 Summarize validation evidence and remaining backend/hardware caveats

Validation notes:

- Red tests failed before implementation: 3/3 failures for DXC lookup order, DX12 SM6 feature query ordering, and missing SM6 resource fields.
- A second red test caught that DX12 adapter selection did not require SM6 before accepting the first hardware adapter; this was fixed by reusing the SM6 query during adapter qualification.
- `cmake --build Build --target NullusUnitTests --config Debug` completed successfully after implementation.
- Targeted DXC/DX12 tests passed: `ShaderCompilerTests.*Dxc*`, `ShaderCompilerTests.*DXC*`, and `EditorLaunchArgsTests.*Dx12*` passed 10/10.
- DXC-focused shader compiler tests passed 3/3, including the DXC structured-buffer reflection regression.
- Full `EditorLaunchArgsTests.*` passed 13/13 on this machine; live DX12 resource creation reported Shader Model 6.0.
- Full `ShaderCompilerTests.*` passed 38/38 after the DXC lookup change and adjacent artifact/cache changes already present in this worktree.
- Project-internal DXC exists at `Tools/DXC/1.9.2602/bin/x64/dxc.exe` in this worktree.
- Plan-review R1 found two P1-equivalent risks: adapter selection initially did not require SM6, and SM6 rejection diagnostics could be hidden behind a generic adapter failure. Both were fixed.
- Plan-review deeper audit R2 found 0 P0/P1. Remaining P2: tests are source-contract heavy, and custom relocated release-package layouts may need an additional runtime install-root DXC probe.

## Dependencies & Execution Order

- T001-T002 precede test design.
- T003-T004 must fail before implementation work is considered complete.
- T005-T010 implement the two user stories.
- T011-T015 depend on implementation completion.

## Implementation Strategy

Use two narrow TDD slices: first prove DXC discovery order prefers the checkout, then prove DX12 initialization has an explicit SM6 gate before queue creation and backend readiness.
