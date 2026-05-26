# Implementation Plan: Fix DX12 Clear Value Warning

**Branch**: `033-debugdraw-batching` | **Date**: 2026-05-24 | **Spec**: [spec.md](spec.md)

## Summary

Stop DX12 clear-value warning spam without hiding real resource mistakes. Owned render targets get optimized clear values that match the clear values Nullus actually uses, including alpha `1.0`. Recorded render-pass defaults now also use opaque black. DXGI swapchain backbuffers expose explicit external-clear metadata and receive a narrowly scoped DX12 info-queue filter only while clearing those backbuffers, because their resources are created by DXGI rather than by Nullus `CreateCommittedResource`.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: Nullus Rendering RHI, FrameGraph, DX12 backend
**Storage**: N/A
**Testing**: GoogleTest via `NullusUnitTests` where the current worktree permits
**Target Platform**: Windows DX12 warning path; no cross-backend correctness claims beyond descriptor-level behavior
**Project Type**: Desktop engine/editor runtime
**Performance Goals**: Remove repeated debug-layer #820 output while preserving clears and keeping owned render-target mismatch warnings visible
**Constraints**: Do not edit `Runtime/*/Gen/`; preserve existing explicit framebuffer clear-value synchronization; keep change narrow
**Scale/Scope**: Rendering bugfix touching RHI texture descriptor helpers, framebuffer/frame graph descriptor creation, DX12 swapchain metadata, DX12 render-pass clear planning, UI bridge clear filtering, and focused tests

## Constitution Check

- Spec scope: Required because this is rendering/RHI behavior under `Runtime/`.
- Generated boundaries: No generated files are edited.
- Backend validation: Unit tests validate descriptor/clear-planning contracts; DX12 smoke validates the warning behavior in a live frame.
- Product runtime: The fix preserves editor/game rendering semantics and existing frame graph ownership.
- Evidence path: Use failing tests first, implement narrow descriptor and DX12 backbuffer fixes, build `NLS_Render`, run targeted tests/smoke, then run required quality review.

## Project Structure

```text
specs/034-dx12-clear-value-warning/
├── spec.md
├── plan.md
└── tasks.md

Runtime/Rendering/RHI/Core/
├── RHICommand.h
├── RHIResource.h
└── RHIResource.cpp

Runtime/Rendering/Core/
├── ABaseRenderer.h
└── RenderClearValues.h

Runtime/Rendering/Context/
└── ThreadedRenderingLifecycle.h

Runtime/Rendering/FrameGraph/
├── FrameGraphExecutionTypes.h
└── FrameGraphTexture.cpp

Runtime/Rendering/Buffers/
├── Framebuffer.h
├── Framebuffer.cpp
└── MultiFramebuffer.cpp

Runtime/Rendering/RHI/Backends/DX12/
├── DX12Command.cpp
├── DX12InfoQueueUtils.h
├── DX12RenderPassUtils.h
├── DX12RenderPassUtils.cpp
├── DX12Swapchain.h
├── DX12Swapchain.cpp
└── DX12UIBridge.cpp

Tests/Unit/
├── FrameGraphSceneTargetsTests.cpp
└── DX12RenderPassUtilsTests.cpp
```

**Structure Decision**: Keep descriptor fixes at texture creation points, centralize default opaque clear color at the renderer level, and keep DX12 warning filtering local to the two backbuffer clear call sites through a shared scoped InfoQueue helper.

## Complexity Tracking

No constitution violations expected. `Docs/REVIEW_PATTERNS.md` was requested by repo guidance, but no such file was found in this worktree.
