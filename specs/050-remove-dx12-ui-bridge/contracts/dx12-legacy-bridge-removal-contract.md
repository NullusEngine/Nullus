# Contract: DX12 Legacy Bridge Removal

## Scope

`Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp` currently contains the old native ImGui DX12 renderer, private descriptor heap, UI command list, UI fences, and direct queue submission. This feature removes that path from supported runtime behavior.

## Prohibited Runtime Behavior

The DX12 migrated UI path must not execute or retain a callable bridge path for:

- `DX12UIBridge::RenderDrawData`
- `DX12UIBridge::WaitForBackbufferReuse`
- `DX12UIBridge::WaitForAllocatorReuse`
- `DX12UIBridge::ExecuteCommandLists`
- `DX12UIBridge::SubmitCommandBuffer`
- UI-owned native queue signal/present outside the normal RHI frame path

## Required Result

- The old bridge implementation is deleted or reduced so it cannot be selected for runtime rendering.
- Direct-submit bridge declarations are removed when no remaining caller needs them.
- Source guards cover the old path names so accidental resurrection is caught.

## Required Tests

- Source guard proves no DX12 legacy bridge selection remains.
- Source guard proves migrated overlay renderer/font/texture code does not call legacy bridge direct-submit APIs.
- Focused unit tests still pass after removal.
