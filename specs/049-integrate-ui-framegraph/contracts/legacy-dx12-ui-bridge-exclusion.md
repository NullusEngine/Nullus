# Contract: Legacy DX12 UI Bridge Exclusion

## Scope

`Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp` currently owns a native ImGui DX12 renderer, per-backbuffer UI command allocator reuse, UI fences, UI descriptor heaps, and direct queue submission. The migrated DX12 Editor path must not use this direct renderer.

## Prohibited On Migrated DX12 Path

The following operations must not occur from the main Editor UI render path once `RHIDeviceFeature::UIOverlayFrameGraph` is enabled:

- `DX12UIBridge::RenderDrawData`
- `DX12UIBridge::WaitForBackbufferReuse`
- `DX12UIBridge::WaitForAllocatorReuse`
- `DX12UIBridge::ExecuteCommandLists`
- `DX12UIBridge::SubmitCommandBuffer`
- `DX12UIBridge::PresentStandaloneUiFrame` ownership through `RhiThreadCoordinator`
- UI-overlay-owned `ID3D12CommandQueue::ExecuteCommandLists`
- UI-overlay-owned `ID3D12CommandQueue::Signal`
- UI-overlay-owned private upload fence wait
- Native swapchain present outside the normal RHI queue present path

## Allowed Temporary Roles

The old bridge may remain compiled for non-migrated fallback or removal staging, but it must be capability-gated and must not be selected for DX12 Editor validation.

## Required Source Guards

Source-level regression tests must verify:

- `UIManager::Render()` no longer calls `m_uiBridge->RenderDrawData` for the migrated path.
- `Editor::RenderEditorUI()` and `Launcher` no longer call `SubmitUIRendering()` for the migrated DX12 path.
- `RhiThreadCoordinator::PrepareUIRender()` cannot begin standalone UI explicit frames when `UIOverlayFrameGraph` is supported.
- `RHIImGuiOverlayRenderer`, `RHIImGuiFontAtlas`, and migrated UI texture binding code do not call native DX12 queue submit/signal/present APIs.
- TimelineProfiler UI-frame classification includes `RHIFrameGraph::UIOverlay` and no longer depends on `DX12UIBridge::RenderDrawData`.

## Required Runtime Evidence

The DX12 validation trace must show fewer than 1% of main UI render frames containing `DX12UIBridge::WaitForBackbufferReuse` over at least 300 frames with the Profiler panel open. The same run must record exactly one RHI graphics queue submit/present owner for each migrated swapchain frame and no UI-owned second submit/present event.
