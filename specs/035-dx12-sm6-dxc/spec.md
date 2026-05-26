# Feature Specification: Prefer Project DXC And Gate DX12 SM6

**Feature Branch**: `033-debugdraw-batching`
**Created**: 2026-05-25
**Status**: Draft
**Input**: User requested that Nullus prefer project-internal `dxc.exe` and check DX12 Shader Model 6 support during initialization so unsupported machines fail clearly instead of showing a black Scene View.

## User Scenarios & Testing

### User Story 1 - Consistent Project Shader Compiler (Priority: P1)

Developers and artists can move a Nullus checkout to another Windows machine and have shader compilation use the compiler bundled with the project before any system-installed SDK compiler.

**Why this priority**: A mismatched system DXC can produce artifacts or reflection behavior that differs from the project toolchain and can surface later as opaque rendering failures.

**Independent Test**: Source-level regression coverage verifies the DXC lookup order prefers project-internal candidates before `DXC_PATH`, Vulkan SDK, or Windows SDK paths.

**Acceptance Scenarios**:

1. **Given** a project checkout with `Tools/DXC/.../dxc.exe`, **When** shader compilation resolves DXC, **Then** the project-internal executable is considered before environment and system SDK locations.
2. **Given** no project-internal DXC is available, **When** shader compilation resolves DXC, **Then** existing environment and SDK fallbacks remain available.
3. **Given** DXC cannot be found anywhere, **When** shader compilation fails, **Then** the diagnostic directs the user to project-internal DXC or an explicit override.

---

### User Story 2 - Clear DX12 Shader Model Gate (Priority: P1)

Users running DX12 on a machine without Shader Model 6 support receive an explicit initialization failure before Scene View rendering begins.

**Why this priority**: Nullus DXIL shaders target SM6-era DXC output; waiting until PSO creation fails makes the editor look like a black rendering bug instead of an unsupported device/toolchain issue.

**Independent Test**: Source-level and device-resource contract tests verify DX12 initialization calls `CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL)` for SM6 before command queues are created or the backend is marked valid.

**Acceptance Scenarios**:

1. **Given** a DX12 device whose highest shader model is below 6.0, **When** DX12 resources are initialized, **Then** initialization returns invalid resources and logs an actionable SM6 diagnostic.
2. **Given** `CheckFeatureSupport` fails, **When** DX12 resources are initialized, **Then** initialization fails early with the HRESULT and requested shader model in the diagnostic.
3. **Given** a DX12 device supporting SM6, **When** DX12 resources are initialized, **Then** resources record the supported shader model and continue normal queue/capability initialization.
4. **Given** a multi-adapter system where an earlier adapter lacks SM6 and a later adapter supports it, **When** DX12 selects hardware, **Then** selection skips the unsupported adapter and continues probing.

### Edge Cases

- Windows machines with no DX12 adapter must keep the existing "failed to find suitable DX12 adapter" path.
- Machines where D3D12 device creation succeeds but `CheckFeatureSupport` fails must not create command queues or mark `BackendReady`.
- Machines where every hardware adapter is rejected for SM6 reasons must include the SM6 rejection in the adapter failure diagnostic.
- The SM6 gate must not claim anything about Vulkan, OpenGL, Linux, or macOS behavior.
- A user-supplied `DXC_PATH` must remain a fallback/override when project-internal DXC is absent.

## Requirements

### Functional Requirements

- **FR-001**: DXC executable discovery MUST prefer project-internal DXC locations under the checkout before environment variables or system SDK paths.
- **FR-002**: DXC executable discovery MUST retain the existing `DXC_PATH`, Vulkan SDK, `VK_SDK_PATH`, and Windows SDK fallbacks after project-internal candidates.
- **FR-003**: DXC-not-found diagnostics MUST mention the preferred project-internal tool location and the explicit override path.
- **FR-004**: DX12 device initialization MUST call shader-model feature support on the created device before creating command queues.
- **FR-005**: DX12 device initialization MUST require at least Shader Model 6.0 for the current DX12 backend.
- **FR-006**: Unsupported or unqueryable SM6 support MUST produce an explicit log message that includes the required shader model and the failing/supported value where available.
- **FR-007**: Valid DX12 resources MUST expose whether SM6 was confirmed and the shader model confirmed by the gate.
- **FR-008**: Adapter selection MUST skip DX12 hardware adapters that can create a D3D12 device but cannot satisfy the SM6 gate, and MUST preserve a clear rejection diagnostic if no adapter qualifies.
- **FR-009**: The change MUST include targeted regression coverage that fails before the implementation and passes afterward where the current worktree permits.
- **FR-010**: The change MUST avoid hand-editing generated files and MUST keep backend/platform claims limited to Windows DX12.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A regression test confirms project-internal DXC lookup appears before `DXC_PATH`, Vulkan SDK, and Windows SDK lookup.
- **SC-002**: A regression test confirms DX12 initialization checks `D3D12_FEATURE_SHADER_MODEL` for `D3D_SHADER_MODEL_6_0` after device creation and before command queue creation.
- **SC-003**: Targeted unit tests build and pass after implementation.
- **SC-004**: On DX12-capable test hardware, successful device-resource creation records SM6 support; on unsupported hardware, initialization fails before rendering begins.

## Assumptions

- Nullus DX12 shader compilation currently requires DXC/SM6-era DXIL because engine HLSL targets SM6 profiles through the DXC backend.
- Early explicit failure is preferable to a silent fallback because no lower shader model path is currently defined for the DX12 backend.
- Unit/source contract tests cover ordering and initialization gates; live rendering validation remains backend- and hardware-specific.
