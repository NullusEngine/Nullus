# Large Scene Optimization Validation

This directory records comparable evidence for the `large-scene-optimization`
worktree. Keep reports tied to a concrete backend, build type, scene fixture,
camera path, and command output so performance claims do not drift into vibes.

Current implementation evidence is sufficient for functional, contract,
RenderDoc/RHI, telemetry-surface, and unit-test sign-off only. Do not claim
performance or industry benchmark sign-off until a populated baseline and
post-change report records fixed hardware, scene fixture, camera path, backend,
build configuration, feature gates, fallback reasons, and before/after
per-stage deltas.

## Required Report Header

Use this header for every phase report:

```text
Report:
Date:
Commit:
Branch:
Machine:
OS:
Backend:
Build configuration:
Scene fixture:
Camera path:
Feature gates:
Validation command:
Result:
```

## Per-Phase Timing Schema

Record timings in nanoseconds unless the source tool only reports another unit.
Use `n/a` when a phase is not implemented or intentionally disabled.

| Frame | Sync | Candidate Query | Visibility | Queue Finalization | Streaming Commit | RHI/FrameGraph | Total |
|-------|------|-----------------|------------|--------------------|------------------|----------------|-------|
|       |      |                 |            |                    |                  |                |       |

## Touched-Count Schema

The core large-scene rule is that later stages must not hide an O(total scene)
scan. Record total counts and touched counts separately.

| Frame | Registered | Allocated Slots | Tombstoned Slots | Sync Touched | Full-Sweep Fallbacks | Sweep Slots Touched | Spatial Candidates | Full-Scan Candidates | Primitive Records Touched | Visibility Tested | Finalization Primitives | Finalization Commands |
|-------|------------|-----------------|------------------|--------------|----------------------|---------------------|--------------------|----------------------|---------------------------|-------------------|-------------------------|-----------------------|
|       |            |                 |                  |              |                      |                     |                    |                      |                           |                   |                         |                       |

## Draw And Residency Schema

| Frame | Visible Primitives | Visible Meshes | Raw Visible Draws | Submitted Draws | Dynamic Instance Groups | Streaming Dependencies | Residency Tickets | Resident CPU Bytes | Resident GPU Bytes |
|-------|--------------------|----------------|-------------------|-----------------|-------------------------|------------------------|-------------------|--------------------|--------------------|
|       |                    |                |                   |                 |                         |                        |                   |                    |                    |

## Fallback And Evidence Notes

Record any fallback reason exactly as emitted by telemetry or logs.

```text
Fallback reason:
Backend capability gate:
Known unsupported path:
RenderDoc/RHI evidence:
Manual editor observation:
Follow-up task:
```

## CMake Glob Pickup Check

`Tests/Unit/CMakeLists.txt` uses `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` for
`*.cpp`, `*.c`, `*.h`, `*.hpp`, and `*.inl` under `Tests/Unit`. The Phase 1
helper files `Tests/Unit/LargeSceneOptimizationTestHelpers.h` and
`Tests/Unit/LargeSceneOptimizationTestHelpers.cpp` are therefore picked up by
the existing `NullusUnitTests` target without adding a parallel source list.
