# Baseline Large Scene Report

This is the no-code baseline report for the large-scene optimization branch.
Fill concrete numbers before enabling behavior-changing feature gates so later
phase reports can compare against the same scene and camera path.

Current sign-off scope: this branch currently has unit/contract/runtime-readback
evidence only. The empty baseline tables below are not performance acceptance
evidence and must not be used to claim large-scene frame-time or memory-budget
deltas until a concrete hardware, scene fixture, camera path, backend, build
configuration, and feature-gate set are recorded.

```text
Report: No-code retained RenderScene baseline
Date: 2026-06-04
Commit:
Branch: large-scene-optimization
Machine:
OS:
Backend:
Build configuration:
Scene fixture:
Camera path:
Feature gates: existing retained RenderScene path only
Validation command:
Result:
```

## Current Path Notes

- `RenderScene` already retains primitive records, cached draw commands,
  serial/parallel bitset visibility, opaque sorting, object-index assignment,
  and dynamic instancing.
- The current synchronization input is still the live scene/component path, so
  stable-frame touched-count telemetry is required before claiming O(changed)
  sync behavior.
- Visibility currently has serial/parallel evaluation modes, but the large-scene
  design still needs a maintained spatial/layer/distance candidate stage before
  visibility-tested counts can be bounded in partitioned scenes.
- Scene View and FrameInfo currently expose draw and threaded-rendering stats;
  large-scene counters must be snapshot-based so editor debug UI does not become
  another scene traversal.

## Baseline Timing

| Frame | Sync | Candidate Query | Visibility | Queue Finalization | Streaming Commit | RHI/FrameGraph | Total |
|-------|------|-----------------|------------|--------------------|------------------|----------------|-------|
| n/a   | n/a  | n/a             | n/a        | n/a                | n/a              | n/a            | n/a   |

## Baseline Touched Counts

| Frame | Registered | Sync Touched | Full-Sweep Fallbacks | Spatial Candidates | Full-Scan Candidates | Primitive Records Touched | Visibility Tested | Finalization Primitives | Finalization Commands |
|-------|------------|--------------|----------------------|--------------------|----------------------|---------------------------|-------------------|-------------------------|-----------------------|
| n/a   | n/a        | n/a          | n/a                  | n/a                | n/a                  | n/a                       | n/a               | n/a                     | n/a                   |

## Baseline Draw And Residency Counts

| Frame | Visible Primitives | Visible Meshes | Raw Visible Draws | Submitted Draws | Dynamic Instance Groups | Streaming Dependencies | Residency Tickets | Resident CPU Bytes | Resident GPU Bytes |
|-------|--------------------|----------------|-------------------|-----------------|-------------------------|------------------------|-------------------|--------------------|--------------------|
| n/a   | n/a                | n/a            | n/a               | n/a             | n/a                     | n/a                    | n/a               | n/a                | n/a                |

## Baseline Follow-Up

- Populate this report with exact output from a local benchmark run that records
  hardware, OS, backend, build configuration, scene fixture, camera path, feature
  gates, fallback reasons, and before/after deltas.
- Keep the same fixture and camera path when comparing Phase 2 settings,
  telemetry, and primitive handle work.
