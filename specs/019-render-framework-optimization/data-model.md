# Data Model: Render Framework Optimization

## OptimizationTask

- `id`: Stable task id such as `T001`.
- `priority`: P0, P1, or P2.
- `direction`: Correctness, resource/synchronization, RHI architecture, pipeline/material, FrameGraph, performance, tooling/ease-of-use.
- `description`: Concrete user-provided improvement.
- `status`: unchecked, in-progress, checked, or blocked.
- `evidence`: Build/test/runtime evidence when checked.
- `notes`: Blockers, dependencies, or split-task references.

## ValidationEvidence

- `command`: Exact command or runtime workflow used.
- `result`: Pass/fail/skipped/blocked.
- `date`: Verification date.
- `scope`: Targeted test, full unit suite, runtime, RenderDoc, or static check.

## RenderingContract

- `name`: Contract under test.
- `ownerSubsystem`: RHI, FrameGraph, Resources, Context, Pipeline, or Tools.
- `failureMode`: What breaks without the contract.
- `testPath`: Test file or runtime verification location.
