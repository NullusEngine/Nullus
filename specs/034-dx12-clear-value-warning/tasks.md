# Tasks: Fix DX12 Clear Value Warning

**Input**: Design documents from `specs/034-dx12-clear-value-warning/`
**Prerequisites**: `plan.md`, `spec.md`

## Phase 1: Setup

**Purpose**: Confirm warning root cause and establish regression coverage.

- [X] T001 Inspect DX12 clear and resource creation flow in `Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp`, `Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [X] T002 Inspect frame graph, framebuffer, GBuffer, and swapchain optimized-clear descriptors in `Runtime/Rendering/FrameGraph/FrameGraphTexture.cpp`, `Runtime/Rendering/Buffers/Framebuffer.*`, `Runtime/Rendering/Buffers/MultiFramebuffer.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.cpp`
- [X] T002a Confirm `docs/REVIEW_PATTERNS.md` is absent in this worktree

---

## Phase 2: User Story 1 - DX12 Frames Do Not Spam Clear Mismatch Warnings (Priority: P1)

**Goal**: Match optimized clear values for owned render targets and suppress #820 only for DXGI backbuffer clears that cannot carry a Nullus resource creation clear value.

**Independent Test**: Run targeted descriptor tests in `FrameGraphSceneTargetsTests` and clear-planning tests in `DX12RenderPassUtilsTests`, plus DX12 editor smoke log verification.

### Tests for User Story 1

- [X] T003 [US1] Add failing framebuffer default-color optimized-clear regression coverage in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T004 [US1] Add failing frame graph color optimized-clear regression coverage in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T005 [US1] Add failing MultiFramebuffer/GBuffer optimized-clear regression coverage in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T006 [US1] Add explicit depth optimized-clear preservation regression coverage in `Tests/Unit/FrameGraphSceneTargetsTests.cpp`
- [X] T007 [US1] Add DX12 render-pass clear-planning coverage for owned render targets versus presentable backbuffers in `Tests/Unit/DX12RenderPassUtilsTests.cpp`

### Implementation for User Story 1

- [X] T008 [US1] Add `RHITextureDesc::OptimizedClearValue::Color()` and `DepthStencil()` helpers in `Runtime/Rendering/RHI/Core/RHIResource.h`
- [X] T009 [US1] Use matching default color/depth optimized clear values in `Runtime/Rendering/Buffers/Framebuffer.*`, `Runtime/Rendering/Buffers/MultiFramebuffer.cpp`, and `Runtime/Rendering/FrameGraph/FrameGraphTexture.cpp`
- [X] T010 [US1] Preserve explicit framebuffer camera/view clear-value synchronization in `Runtime/Rendering/Buffers/Framebuffer.cpp`
- [X] T011 [US1] Mark DX12 backbuffer textures with `TextureUsageFlags::Present` in `Runtime/Rendering/RHI/Backends/DX12/DX12Swapchain.cpp`
- [X] T012 [US1] Extend `DX12RenderPassClearPlan` to flag backbuffer clear requests in `Runtime/Rendering/RHI/Backends/DX12/DX12RenderPassUtils.*`
- [X] T013 [US1] Add scoped #820 filtering around RHI backbuffer clears in `Runtime/Rendering/RHI/Backends/DX12/DX12Command.cpp`
- [X] T014 [US1] Add scoped #820 filtering around UI bridge swapchain clears in `Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp`
- [X] T014a [US1] Use explicit `RHITexture::RequiresExternalClearValueMessageFilter()` metadata so owned `Present` textures do not suppress mismatch warnings
- [X] T014b [US1] Centralize default opaque color clear values and cover recorded framebuffer default clear alpha
- [X] T014c [US1] Extract the DX12 InfoQueue scoped filter helper and replace parallel clear-plan vectors with a single request struct

---

## Phase 3: Polish & Review

**Purpose**: Validate and review the completed fix.

- [X] T015 Build `NLS_Render`
- [X] T016 Verify targeted descriptor and DX12 clear-plan tests pass in a freshly linked `NullusUnitTests.exe`
- [X] T017 Rebuild `NullusUnitTests`
- [X] T018 Run DX12 editor smoke and grep stdout/stderr for #820 and related warning strings
- [X] T019 Run required plan-review quality gate
- [X] T020 Summarize validation evidence and remaining caveats

Validation notes:

- `NLS_Render.vcxproj` rebuilt successfully after the DX12 changes and produced `App\Win64_Debug_Runtime_Shared\NLS_Renderd.dll`.
- `NullusUnitTests.vcxproj` rebuilt successfully after adding `RHIResource.cpp`.
- Targeted tests passed in the freshly linked unit-test executable: all `DX12RenderPassUtilsTests.*` plus framebuffer/frame graph/MultiFramebuffer optimized-clear and recorded-default-clear regressions (10/10).
- A 45-second DX12 Editor smoke reached real render frames (`frameId=1668`) and deferred renderer submission; grep found zero `D3D12 WARNING`, `CLEARRENDERTARGETVIEW`, `MISMATCHINGCLEARVALUE`, `ClearRenderTargetView`, or `Live Object` matches in stdout/stderr.
- Review evidence included four independent agents plus a final deeper audit; all reported P1 issues were fixed before sign-off.

---

## Dependencies & Execution Order

- T001-T002 precede test design.
- T003-T007 must fail before their production fixes are considered complete.
- T008-T014 implement the narrow descriptor and DX12 backbuffer fixes.
- T015-T020 depend on T008-T014.

## Implementation Strategy

Use narrow TDD slices: first prove owned color render targets need optimized clear values matching default alpha `1.0`, then separately prove DX12 can identify swapchain backbuffer clear requests and filter only that unavoidable external-resource warning.
