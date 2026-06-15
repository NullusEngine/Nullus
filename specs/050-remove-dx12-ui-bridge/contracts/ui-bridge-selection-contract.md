# Contract: UI Bridge Selection

## Scope

`Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` is the runtime selection point for UI renderer bridges. For the migrated DX12 path, it must not select the legacy direct-submit DX12 bridge.

## Required Behavior

- When `RHIDeviceFeature::UIOverlayFrameGraph` is supported, `CreateRHIUIBridge(...)` must return a null/unsupported bridge instead of a DX12 legacy bridge.
- When the migrated overlay path is unavailable, the selection logic must still fail closed rather than silently entering the removed DX12 bridge.
- The selection result must not depend on platform input backend setup.

## Required Tests

- Migrated DX12 selection returns null/unsupported bridge.
- Unsupported backend selection does not create a legacy DX12 bridge.
- Platform backend initialization remains independent from bridge selection.
