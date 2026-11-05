# Input Core Release performance and allocation checks

This target measures production Input Core sources without opening an editor,
creating physical devices, loading scripts or building the renderer. It uses the
repository's Guid/vector implementations and bundled GoogleTest; no dependencies
are downloaded. It can also be built from the main project when
`NLS_BUILD_PERFORMANCE_TESTS=ON`.

From the repository root on Windows:

```powershell
Tools/BuildCleanEnvironment.cmd cmake -S Tests/InputPerformance -B build/input-release-performance -G "Visual Studio 17 2022" -A x64
Tools/BuildCleanEnvironment.cmd cmake --build build/input-release-performance --config Release --target NullusInputBenchmark NullusInputReleaseTests -- /m:1 /v:minimal
ctest --test-dir build/input-release-performance -C Release --output-on-failure
& build/input-release-performance/Release/NullusInputBenchmark.exe --output build/input-release-results.json
```

Other platforms can use their native CMake generator and
`-DCMAKE_BUILD_TYPE=Release`. Those platforms have not been validated by the
Windows run. `NullusInputReleaseTests` is the existing device/action/serialization
test suite compiled with the same core sources, rather than rewritten tests.

## Measurement contract

- Default: 256 warmup operations, 64 samples of 32 operations, then a separate
  allocation pass of 2,048 operations. `--samples`, `--batch` and `--warmup`
  adjust these counts. P50/P95/P99 describe the **batch average per operation**,
  not the latency of individual frames. Each result describes its operation;
  some contain multiple Update calls or users.
- Timing uses `steady_clock` without the allocation scope enabled. Sample
  storage, JSON output and console reporting occur outside measurement.
  Input itself uses an injected monotonic clock advancing at 120 Hz, independently
  of the measurement clock. Fixed-step workloads deliberately vary consumption.
  `empty_harness` provides a loop/checksum baseline. Absolute timing is not a CI
  gate; record CPU, compiler flags, power/affinity settings and background load
  when comparing results.
- All production Input Core translation units are statically linked into the
  benchmark. Replacing `operator new` only in a test executable would miss
  allocations inside the normal input DLL. Ordinary, array, aligned, nothrow
  and sized-delete paths are handled; a startup calibration verifies direct,
  aligned and STL container allocations and frees.
- The allocation probe covers **C++ allocations on the evaluating thread**,
  including Guid/vector support code. It does not measure C `malloc`, OS heaps,
  other threads, platform backends or external DLLs, nor does it prove absence of
  leaks. Input Core currently makes no direct C allocator calls.
- After warmup, ordinary input scenarios must allocate zero times. Cold
  definition compilation and binding overrides are separate, explicitly
  permitted structural allocations. `--allow-allocations` records a failing
  allocation gate for before/after investigation without failing the process;
  behavioral errors still fail. CI does not use that option.
- Zero allocation applies to the tested warmed capacity and control population.
  A new device/control, a larger event burst, accumulating fixed events without
  consuming a tick, large callback captures, registration, reload or arbitrary
  recursively queued configuration work can require additional storage. Buffers
  are allowed to grow without discarding input; they are not capped to pass a
  benchmark. The normal device DLL and whole Editor must be profiled separately.

## Workloads

The 21 results include an empty harness, 18 ordinary input workloads and two
structural workloads:

- Idle and held input with 128 actions; four same-frame short taps; 128 actions
  and subscribers driven by one physical control.
- Never/OnActuated/OnStarted/OnPerformed context consumption, exact Primary
  modifier chords, overlapping binding slots and partial releases.
- Four users sharing definitions/devices but retaining independent state;
  mouse Delta and gamepad Rate with zero/one/three fixed ticks. Event totals
  verify zero lost/duplicate fixed consumption.
- Hold/Tap/Repeat with no new events; callback routing mutations; domain changes;
  context cancellation and RequireRelease recovery.
- Cached handle, stable ID and initialization-time name lookup (1,024 queries
  per operation). Cache handles in a high-frequency consumer, and refresh them
  after definition generation changes.
- Cold compilation of 128 actions and per-user binding override application.

Callbacks, snapshot state, same-frame event counts and fixed event totals are
checked during the workloads. Existing behavioral tests additionally cover
replay, reload, unsubscribing, subscriber-vector growth and exceptions/nested
changes at update boundaries.
