# Contract: UE5-Style Render Foundation

## Scope

This contract defines the boundary for the first-phase UE5-style render foundation in Nullus.

## Capability Contract

- `RHIDeviceCapabilities` is the only runtime-facing truth source for backend foundation readiness.
- Capability flags represent engine-wired support, not theoretical backend API support.
- DX12 and Vulkan are the only Tier A backends for this feature bundle.
- Legacy backends may continue to run legacy paths, but they must not report UE5-style foundation readiness unless they satisfy the same capability contract.

## Rollout Contract

- No feature slice may enable async compute or parallel recording based only on `supportsCompute`.
- RDG, Render Thread ownership, and RHI Thread ownership work must consume capability helpers instead of duplicating ad hoc backend checks.
- Descriptor and PSO reuse policy must remain centralized and diagnosable as later slices land.

## Validation Contract

- Capability model changes require targeted unit tests.
- Execution-model changes require targeted unit tests plus runtime evidence.
- Final completion claims for this feature require DX12 and Vulkan validation, not one backend standing in for the other.
