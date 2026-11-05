# Task system validation

This standalone CMake project builds the production Jobs and TaskGraph sources,
their GoogleTests, and a repeatable CPU benchmark. It does not require the Editor,
graphics backends, or generated reflection code. Run from the repository root:

```powershell
cmake -S Tests/TaskSystem -B build/task-system -G "Visual Studio 17 2022" -A x64
./Tools/BuildCleanEnvironment.ps1 cmake --build build/task-system --config Release --target NullusTaskSystemTests NullusRenderingThreadLifecycleTests NullusTaskBenchmark
ctest --test-dir build/task-system -C Release --output-on-failure
./build/task-system/Release/NullusTaskBenchmark.exe > build/task-system/benchmark.csv
```

On Linux or macOS, use the host's CMake generator and omit `--config Release` for
single-configuration generators (configure with `-DCMAKE_BUILD_TYPE=Release`).
The tests use the repository's existing GoogleTest source.

`NullusRenderingThreadLifecycleTests` additionally compiles the real rendering
thread system, command pipe, fence and logger without creating a graphics device.
It checks suspended owner retention, repeated suspend/resume/shutdown, named lane
affinity, and the background-to-render-to-game drain sequence used by application
shutdown. The same test source is included by the full engine test suite.

The benchmark reports five-sample medians for 1 and 4 foreground workers:

- 1,000 individual job and TaskGraph submit/completion pairs.
- 20,000 individually submitted small jobs.
- A serial and parallel 1,048,576-element computation at grains 1, 64, and 1,024.
- Submission plus release and release alone for a gated 5,000-edge dependency chain.
- Chain-local, injection, and stolen execution totals over its five timed samples;
  these counters are read outside the timing windows.
- Worker count, smallest/largest share, and steals for 128 child tasks.
- Process CPU time while the pools are idle for approximately 500 ms.

The calling thread may assist `Jobs::Complete`, so a one-worker parallel run can
use the worker and the caller. The child-distribution probe includes a 100-us
sleep to make stealing observable; it is a correctness/load-distribution probe,
not a simulation of production compute throughput. Idle CPU is process-wide,
with OS timer granularity; a reported zero does not prove zero instructions.
CPU frequency, concurrent processes, and instrumented builds affect results.
Compare runs using the same compiler, configuration, benchmark source, and host.

To benchmark a preserved implementation, copy its `Jobs`, `TaskGraph`, and
`Threading` directories under a snapshot `Base` directory, then configure a
separate build with `-DNLS_TASK_SOURCE_BASE=<snapshot>/Base` and build only
`NullusTaskBenchmark`. Never restore old sources over the working checkout.

For additional instrumentation, configure a separate build with
`-DNLS_TASK_SANITIZE_ADDRESS=ON`. Windows ASan needs the MSVC ASan runtime on PATH.
`-DNLS_TASK_SANITIZE_THREAD=ON` is available on supported Clang/GCC hosts and is
explicitly rejected on Windows. ASan and TSan must use separate build directories.

This suite validates the scheduler and headless rendering thread ownership. It
does not substitute for Editor/Player startup, GPU rendering, Play/Stop
interaction, or platform acceptance.

Recorded validation, raw three-way benchmark samples, and source hashes are in
[the 2026-10-29 results](results/2026-10-29-validation.md). The
[Linux/TSan notes](results/2026-10-29-linux-tsan.md) document the existing GCC 10
runtime's timed-condition-variable limitation and the instrumented-build-only
compatibility setting used for that run.

The [dispatch-record reuse follow-up](results/2026-10-29-dispatch-reuse-performance.md)
records the bounded completed-record cache, six additional regressions and the
matched benchmark against the prior LIFO implementation.

The [single-worker and long-chain follow-up](results/2026-10-30-tiny-sync-performance.md)
records the direct success finalizer, combined submission admission checks,
254-task validation matrix and same-window original/before/final comparison,
including the remaining single-worker regressions.
