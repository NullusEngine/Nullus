# Quickstart: Nullus Unity-Style Native JobSystem

## Build

Use the normal Nullus Windows test flow:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests -- /m:4
```

Linux/macOS equivalent:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target NullusUnitTests
```

Only claim validation for the platforms whose commands were actually run. The Windows Debug commands below do not prove Linux or macOS behavior unless those platform runs are executed separately.

## Focused Tests

Run the JobSystem tests directly:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystem*"
```

Or through CTest:

```powershell
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
```

## Required Validation Slices

1. **Scheduler MVP**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemSchedulerTests.*"
   ```

   Expected evidence:

   - Single jobs execute exactly once.
   - Dependency chains complete in order.
   - Main-thread completion can execute eligible work.
   - Shutdown drains or rejects work according to mode.

2. **Parallel and Batched Jobs**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemParallelTests.*"
   ```

   Expected evidence:

   - Different jobs under one handle all complete.
   - Parallel-for covers each index exactly once.
   - Combine callbacks run once after iteration callbacks.
   - Work stealing covers uneven workloads.

3. **Background Queue**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemBackgroundTests.*"
   ```

   Expected evidence:

   - Background work uses the background lane.
   - Main-thread continuations run only when drained.
   - Shutdown does not run main-thread continuations on the shutdown caller.
   - Shutdown does not hang.

4. **Diagnostics and Safety**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemDiagnosticsTests.*"
   ```

   Expected evidence:

   - Diagnostic snapshots include lifecycle state.
   - Disallowed wait violations are structured records.
   - Worker names and profiler registration points are visible through test destinations or source contract tests.

5. **Binding-Ready Contract**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemBindingsTests.*"
   ```

   Expected evidence:

   - Opaque handles schedule and complete work.
   - Version and struct size mismatches fail deterministically.
   - Invalid handles never crash.

6. **Consumer Migration**

   ```powershell
   Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystemMigrationTests.*:EditorRenderPathContractTests.*"
   ```

   Expected evidence:

   - At least one existing private background worker consumer delegates to JobSystem.
   - Existing behavior and shutdown semantics are preserved.

## Completion Gate

Before claiming completion or preparing a commit:

```powershell
cmake --build build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter="JobSystem*"
ctest --test-dir build -C Debug --output-on-failure -R NullusUnitTests
```

Run `/plan-review` quality review according to `AGENTS.md` after every completed implementation phase.
