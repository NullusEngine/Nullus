# Contract: UI Frame Routing

## Scene + UI Frame

When a scene/editor frame targets the swapchain and a visible UI snapshot exists, the snapshot must be attached to that frame's `RenderScenePackage`. The RHI frame records the overlay before present.

## UI-Only Frame

When no scene package is available but UI needs to update, `DriverUIAccess` must publish a normal swapchain `RenderScenePackage` containing only the UI overlay pass. This frame uses the same frame context acquisition, command buffer reset, resource transitions, queue submit, frame fence, and present logic as scene frames.

## Unsupported Backend

If `RHIDeviceFeature::UIOverlayFrameGraph` is unsupported:

- DX12 migrated path must report a visible unsupported/fallback state and must not silently call the old direct-submit bridge.
- Non-DX12 backends must remain explicitly capability-gated until separately validated.

If the feature enum exists but the backend reports `runtimeSelectable == false`, product code must treat it as unsupported for Editor/Launcher routing. Tests may force internal overlay recording paths, but the product path must not select them until scene+UI frames, UI-only frames, font atlas, texture registry, and legacy direct-submit exclusion are all implemented.

## Retired APIs On Migrated Path

For a backend with `UIOverlayFrameGraph` support, the following old calls must not be part of the main UI frame path:

- `DriverUIAccess::PrepareUIRender()` starting a standalone UI explicit frame
- `UIManager::ResolveUISignalSemaphore()`
- `DriverUIAccess::SetUICompositionSignal()`
- `UIManager::SubmitUIRendering()` submitting a native UI command list
- `RhiThreadCoordinator::PresentStandaloneUiFrame()`

These APIs may remain temporarily for unsupported backends or tests, but DX12 migrated Editor validation must not route through them.

The migrated path must also avoid hidden native DX12 queue ownership through the overlay renderer or resource upload path. A passing validation run must show no additional UI-owned `ExecuteCommandLists`, native `Signal`, or standalone present around the UI overlay work.

## Required Tests

- Scene + UI frame publishes one package with an overlay pass.
- UI-only frame publishes a normal swapchain package with only an overlay pass.
- Pending resize during UI-only frames uses normal frame retirement and does not drain UI bridge submitted work.
- DX12 capability plumbing remains not runtime-selectable until the full migrated path is wired.
- Non-DX12 backends report unsupported reasons and do not silently enter the migrated path.
- DX12 migrated path source guard prevents `SubmitUIRendering()` from calling direct bridge submit.
