# Implementation Plan: DX12 UI Present Wait Reduction

**Branch**: `047-dx12-ui-present-wait` | **Date**: 2026-06-12 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `/specs/047-dx12-ui-present-wait/spec.md`

## Summary

Reduce main-thread UI render stalls by decoupling DX12 UI command allocator reuse from swapchain backbuffer reuse. The bridge will keep a small allocator pool with per-slot fence tracking, select a completed allocator for each frame, and only wait when all allocator slots are still in flight. Backbuffer resource state transitions and scene wait semaphore ordering remain on the graphics queue.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: DX12 RHI backend, ImGui DX12 backend, Nullus threaded rendering driver, GoogleTest  
**Testing**: `NullusUnitTests`, especially `DX12UIFrameFenceTrackerTests`, UI bridge trace/source tests, and threaded rendering lifecycle subsets  
**Target Platform**: Windows DX12  
**Performance Goals**: Avoid per-frame `DX12UIBridge::WaitForBackbufferReuse` stalls when allocator slack is available  
**Constraints**: Do not hand-edit generated files; preserve device-lost quarantine and shutdown drain behavior; no claims for Vulkan/Linux/macOS  
**Scale/Scope**: Narrow DX12 UI bridge synchronization optimization; no UI rendering feature changes

## Constitution Check

- **Spec-first major change**: PASS. This Runtime/RHI DX12 synchronization change uses `specs/047-dx12-ui-present-wait/`.
- **Validation matches subsystem**: PASS. Plan uses unit tests for allocator wait policy plus DX12-targeted build/runtime evidence.
- **Generated code/backend boundaries**: PASS. Planned edits avoid generated files and stay in DX12 UI bridge/support tests.
- **Incremental verified delivery**: PASS. RED tests precede implementation.
- **Cross-platform assumptions**: PASS. Claims are explicitly scoped to Windows DX12.

## Project Structure

```text
specs/047-dx12-ui-present-wait/
├── spec.md
├── plan.md
├── quickstart.md
└── tasks.md

Runtime/Rendering/RHI/Backends/DX12/
├── DX12UIBridge.cpp
└── DX12UIFrameFenceTracker.h

Tests/Unit/
├── DX12UIFrameFenceTrackerTests.cpp
└── ProfilerDestinationTests.cpp
```

## Design

1. Extend the existing DX12 UI frame tracking helper so it can model a pool of allocator slots with submitted fence values.
2. Add tests that prove:
   - a completed allocator can be selected even when the current backbuffer's previous fence is incomplete;
   - an exhausted allocator pool reports the specific fence value to wait for;
   - resetting swapchain/UI frame state clears allocator tracking.
3. Update `DX12UIBridge` to create multiple command allocators independent of backbuffer count and select one per UI frame.
4. Replace `DX12UIBridge::WaitForBackbufferReuse` with a specific allocator reuse wait scope used only when the allocator pool is exhausted.
5. Keep scene semaphore waits, command list execution, frame fence recording, texture handle retention, and UI composition fence signalling unchanged.

## Complexity Tracking

GPU synchronization correctness is the main risk. The implementation must be reviewed as a DX12 queue/allocator lifetime change, not as a UI-only refactor.
