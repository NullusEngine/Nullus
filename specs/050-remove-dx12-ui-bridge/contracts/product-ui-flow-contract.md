# Contract: Product UI Flow

## Scope

Editor and Launcher must continue to render UI through the migrated frame-graph path and process platform input after the legacy DX12 bridge is removed.

## Required Behavior

- `UIManager::Render()` publishes draw data snapshots for migrated overlay rendering and does not depend on `RenderDrawData()` for DX12.
- `Editor::RenderEditorUI()` does not set UI composition signals or submit a standalone UI command buffer for migrated DX12.
- `Launcher::Run()` does not submit a standalone UI command buffer for migrated DX12.
- `ImGui_ImplWin32` and `ImGui_ImplGlfw` platform backend setup, new-frame, and shutdown calls remain intact.

## Required Tests

- Source guard proves product migrated paths do not call standalone direct-submit UI commands.
- Editor and Launcher DX12 smoke validation confirms visible and responsive UI.
- Platform backend awareness tests continue to pass.
