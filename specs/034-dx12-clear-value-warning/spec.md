# Feature Specification: Fix DX12 Clear Value Warning

**Feature Branch**: `033-debugdraw-batching`
**Created**: 2026-05-24
**Status**: Implemented
**Input**: User reported repeated DX12 debug-layer warning `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`, first with mismatched clear values and then with `The application did not pass any clear value to resource creation`.

## User Scenarios & Testing

### User Story 1 - DX12 Frames Do Not Spam Clear Mismatch Warnings (Priority: P1)

Developers can run the DX12 editor/render path without repeated debug-layer warnings from `ClearRenderTargetView` on render targets whose resource clear metadata does not match the actual clear operation.

**Why this priority**: The warning floods output and points to an avoidable slower clear path or an unavoidable external-backbuffer clear that should not hide real offscreen clear-value mismatches.

**Independent Test**: Regression tests cover framebuffer, frame graph, and MultiFramebuffer color target descriptors, plus DX12 render-pass clear planning for owned render targets versus DXGI backbuffers.

**Acceptance Scenarios**:

1. **Given** a default-cleared framebuffer color attachment, **When** its DX12 texture descriptor is created, **Then** its optimized color clear value is enabled and matches the default clear `{0,0,0,1}`.
2. **Given** a transient frame graph color attachment, **When** its explicit RHI texture is created, **Then** its default optimized color clear value uses alpha `1.0`, not the storage default alpha `0.0`.
3. **Given** a MultiFramebuffer/GBuffer color attachment, **When** its explicit color textures are created, **Then** their optimized clear values match the default color clear path.
4. **Given** a framebuffer whose clear color is explicitly synchronized to a camera/view clear color, **When** the framebuffer color texture is recreated, **Then** that explicit optimized color clear value is preserved.
5. **Given** a depth attachment with an explicit optimized depth/stencil clear value, **When** the frame graph creates the explicit RHI texture, **Then** the supplied depth/stencil value is not overwritten by helper defaults.
6. **Given** a recorded framebuffer pass that omits an explicit clear color, **When** it clears its owned color attachment, **Then** the default clear color uses opaque black `{0,0,0,1}` to match the optimized clear value.
7. **Given** a DXGI swapchain backbuffer, **When** DX12 clears it through the RHI backbuffer pass or UI bridge, **Then** warning #820 is filtered only around that external-resource clear because the backbuffer was not created through our resource creation API.

### Edge Cases

- Color clears with alpha `1.0` must not be compared against an optimized clear alpha of `0.0`.
- Imported external output textures must keep their existing explicit texture descriptors.
- The fix must not weaken depth clear optimization where the clear value is stable.
- Explicit optimized depth/stencil and framebuffer color clear values must be preserved.
- Scoped DX12 warning filtering must apply only to swapchain/backbuffer `ClearRenderTargetView`; owned offscreen render-target mismatches must remain visible.
- The separate `Live Object ... Refcount: 0` warning should remain independently observable if it persists.

## Requirements

### Functional Requirements

- **FR-001**: RHI default color optimized clear helpers MUST use `{0,0,0,1}`.
- **FR-002**: Frame graph, framebuffer, and GBuffer color attachments that clear to the default color MUST create textures with matching optimized color clear values.
- **FR-003**: Existing framebuffer code that explicitly synchronizes optimized color clear values with the view/camera clear color MUST keep that behavior.
- **FR-004**: Explicit optimized clear values supplied by callers MUST be preserved instead of replaced by helper defaults.
- **FR-005**: DX12 swapchain backbuffers MUST be identifiable through explicit external-resource metadata instead of inferring ownership from `TextureUsageFlags::Present` alone.
- **FR-006**: DX12 MUST suppress warning #820 only around swapchain/backbuffer clears that cannot receive our optimized clear value at resource creation.
- **FR-007**: The change MUST include targeted regression tests that fail before the production fixes and pass after them where the current worktree permits.
- **FR-008**: The change MUST avoid hand-editing generated files and MUST preserve backend/platform claims to the validated scope.
- **FR-009**: Public aggregate initialization of `OptimizedClearValue` with `enabled=true` MUST default color alpha to `1.0` so future color attachments do not recreate the original mismatch.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Targeted descriptor tests fail before the descriptor fix and pass after it.
- **SC-002**: DX12 render-pass clear planning distinguishes owned render-target clears from swapchain/backbuffer clears.
- **SC-003**: `NLS_Render` builds after the DX12 changes.
- **SC-004**: A DX12 editor smoke reaches real rendering and UI bridge submission without logging `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`.

## Assumptions

- The first repeated mismatch warning was caused by owned color attachments using optimized clear alpha `0.0` while render passes cleared with alpha `1.0`.
- The later `did not pass any clear value` warning was caused by clearing DXGI swapchain backbuffers, which are external resources and cannot be created with Nullus-provided `D3D12_CLEAR_VALUE`.
- Unit validation covers descriptor and clear-planning contracts; live DX12 output is backend-specific runtime evidence, not proof for other backends.
