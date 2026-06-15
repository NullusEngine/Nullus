# Research: Remove DX12 Legacy UI Bridge

## Decision 1: Remove the DX12 bridge from runtime selection rather than keep a hidden fallback

- **Decision**: `Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp` will stop selecting `CreateDX12RHIUIBridge` for migrated DX12 runs, and the DX12 legacy bridge will no longer be a runtime option on the supported frame-graph path.
- **Rationale**: The migrated UI overlay path already owns DX12 UI rendering. Keeping the old bridge as a fallback creates duplicate ownership, extra waits, and the risk of resurrecting the path accidentally.
- **Alternatives considered**: Keep the bridge behind a disabled selector; keep a compatibility stub that logs and returns null; leave the old DX12 path in place but make it unreachable from the new UI overlay flow.

## Decision 2: Preserve platform/input backends untouched

- **Decision**: `ImGui_ImplWin32` and `ImGui_ImplGlfw` setup/teardown stays in `Runtime/UI/UIManager.cpp`.
- **Rationale**: The bridge removal concerns the renderer submission path, not platform event processing or window integration.
- **Alternatives considered**: Move platform backend ownership into the RHI bridge; delete all backend-specific UI code. Both would widen scope and risk input regressions.

## Decision 3: Fail closed when a supported migrated UI path is unavailable

- **Decision**: `CreateRHIUIBridge` will return the null bridge for unsupported configurations instead of silently falling back to legacy DX12 submission.
- **Rationale**: The feature goal is to remove the old path, not to preserve it as an emergency escape hatch.
- **Alternatives considered**: Keep legacy DX12 bridge creation for unsupported overlay cases; add a product-specific fallback flag. Both would preserve legacy behavior and weaken the cleanup.

## Decision 4: Keep product rendering code simple and explicit

- **Decision**: `UIManager`, `Editor`, and `Launcher` keep their existing frame flow, but their legacy bridge-dependent calls are removed or reduced to no-op behavior on the migrated path.
- **Rationale**: This keeps the cleanup localized and makes it obvious which branch owns rendering.
- **Alternatives considered**: Rewrite product frame loops around a new abstraction. That would be disproportionate to the removal scope.
