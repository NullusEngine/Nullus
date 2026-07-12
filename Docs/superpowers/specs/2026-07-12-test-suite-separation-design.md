# Test Suite Separation Design

## Goal

Keep the default Nullus test path focused on deterministic behavior correctness. Move performance, stress, scale, timing, and benchmark scenarios into a separately built test module that is disabled by default.

## Current Problem

`Tests/Unit/CMakeLists.txt` recursively adds every source file under `Tests/Unit` to `NullusUnitTests`. This includes `AssetPrefabPerformanceTests.cpp` and `AssetThumbnailPerformanceTests.cpp`, which mix deterministic behavior checks with large-scale and performance-report scenarios. As a result, the default CTest entry can run sustained multi-core workloads for many minutes.

## Classification Rules

Classify every existing test by intent rather than by its current filename.

A test remains in `NullusUnitTests` when it verifies deterministic functional behavior, including:

- state transitions and returned results;
- cache identity, invalidation, and reuse semantics;
- scheduling, cancellation, retry, and failure behavior;
- diagnostics or telemetry fields that are part of a functional contract;
- bounded-work behavior expressed through deterministic counters or limits rather than wall-clock time.

A test moves to `NullusPerformanceTests` when it depends on or primarily measures:

- elapsed time, throughput, latency, or relative speed;
- repeated workload execution intended to expose performance characteristics;
- large object, component, asset, queue, or scene populations used as a stress workload;
- benchmark baselines, comparisons, reports, or profiling output;
- real project assets or opt-in benchmark environment variables;
- scalability validation whose purpose is performance rather than a functional boundary.

Do not use machine-speed thresholds in `NullusUnitTests`. Small deterministic fixtures remain valid even when they exercise code that was optimized for performance.

## Source Layout

- `Tests/Unit/` contains only default behavior tests.
- `Tests/Performance/` contains performance, stress, scale, and benchmark tests.
- Mixed files are split by test intent. Behavior tests move into functionally named Unit files; performance tests and their performance-only helpers move into Performance files.
- Shared test support is extracted only when both modules genuinely need the same substantial fixture or builder. Small helpers stay local to avoid unnecessary coupling.

The initial classification covers every test in the two mixed performance files and audits the rest of `Tests/Unit` for clear timing, stress, benchmark, or scale scenarios.

## Build And CTest Integration

Add a top-level CMake option:

```cmake
option(NLS_BUILD_PERFORMANCE_TESTS "Build Nullus performance and stress tests" OFF)
```

`NullusUnitTests` remains controlled by the existing `NLS_BUILD_TESTS` workflow and remains the default CTest behavior entrypoint.

When `NLS_BUILD_PERFORMANCE_TESTS=ON`, CMake adds `Tests/Performance`, builds the independent `NullusPerformanceTests` executable, and registers it with CTest using the `performance` label. With the option off, the performance target is neither built nor registered.

Both targets use the existing Nullus test dependencies, runtime DLL copying, editor resource copying, platform filtering, test hooks, and generated metadata dependencies. Common CMake setup may be factored into a narrowly scoped helper if that prevents the two targets from drifting.

## Validation

Validation must demonstrate the boundary as well as test correctness:

1. Configure with performance tests disabled and verify `NullusPerformanceTests` is absent from the build and CTest inventory.
2. Build and run `NullusUnitTests`; it must complete as the default behavior suite without performance or stress scenarios.
3. Configure with `NLS_BUILD_PERFORMANCE_TESTS=ON`, build `NullusPerformanceTests`, and verify CTest lists it with the `performance` label.
4. Run the performance target separately to ensure migrated cases compile and execute through the opt-in path.
5. Run `ReflectionTest` because the broad current branch changes include runtime and generated-registration-sensitive code.

## Non-Goals

- Redesigning the benchmark framework or its report format.
- Introducing wall-clock assertions into behavior tests.
- Changing production behavior solely to make test classification easier.
- Enabling performance tests in the default local or CI test path.
