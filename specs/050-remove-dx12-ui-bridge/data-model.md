# Data Model: Remove DX12 Legacy UI Bridge

## Entities

### UI Bridge Selection

- **Purpose**: Describes which UI rendering path is active at runtime.
- **Key Fields**:
  - `backend`: active render backend
  - `overlaySupported`: whether the migrated overlay path is available
  - `bridgeState`: active, null, or unsupported
- **Rules**:
  - DX12 migrated runs must resolve to the frame-graph overlay path.
  - The legacy DX12 bridge must not become the active state when overlay support exists.

### Backend Capability State

- **Purpose**: Captures whether the runtime supports the migrated UI overlay path.
- **Key Fields**:
  - `backendType`
  - `uiOverlayFrameGraphSupported`
  - `reason`
- **Rules**:
  - Unsupported states must be explicit.
  - Capability state is used for selection and diagnostics, not as a silent fallback trigger.

### Product UI Flow

- **Purpose**: Represents the editor and launcher render loops that consume UI rendering services.
- **Key Fields**:
  - `canvasRendered`
  - `platformBackendInitialized`
  - `uiFrameSubmitted`
- **Rules**:
  - Platform backend initialization remains independent of renderer bridge removal.
  - UI frame submission should not depend on the removed direct-submit bridge.
