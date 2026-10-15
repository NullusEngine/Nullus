# Debugging and Testing

This page covers the engine-side debugging entry points and the test system: how to run tests, where they belong, what CI covers, and which Windows regressions must be run locally.

## Test targets

| Target | CTest label | Purpose |
| --- | --- | --- |
| `NullusUnitTests` | `behavior` | Deterministic behavior and integration regressions |
| `NullusPerformanceTests` | `performance` | Performance and stress tests, built on demand |
| `ReflectionTest` | none | Lightweight reflection pipeline smoke tool |

`NullusPerformanceTests` is neither built nor registered with CTest by default. Repeatable, deterministic regressions belong in the unit tests; performance measurements, stress scenarios, and real-asset validation belong in the performance tests, so the default behavior suite never depends on machine state.

## Unit test coverage

- Asset import, database, artifacts, Prefabs, thumbnails, and resource lifetime
- Editor workflows, settings, shortcuts, input, panels, and tooling behavior
- Rendering, RHI, FrameGraph, shaders and materials, scene and visibility pipelines
- JobSystem, profiling markers, serialization, and other deterministic behavior across `Base`, `Core`, and `Engine`

Reflection and MetaParser are a specific focus, including runtime registration verification for key types, generated content verification, and integration regressions that only surface at build time.

## Running behavior tests

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_TESTS=ON
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
ctest --test-dir Build -C Debug -L behavior --output-on-failure
```

`ReflectionTest` is built by that command but is a separate tool and is not part of the `behavior` label.

## Running performance tests

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 -DNLS_BUILD_PERFORMANCE_TESTS=ON
cmake --build Build --config RelWithDebInfo --target NullusPerformanceTests
ctest --test-dir Build -C RelWithDebInfo -L performance --output-on-failure
```

Performance conclusions must come from binaries rebuilt in the target configuration, and should report repeated measurements rather than a single run.

## Responsibility boundaries

| Target | Best fit |
| --- | --- |
| `NullusUnitTests` | Machine-independent behavior assertions, subsystem contracts, fine-grained regressions |
| `NullusPerformanceTests` | Performance measurements, stress scenarios, real asset load |
| `ReflectionTest` | Pattern-level sentinel checks and the cheapest possible registration path check |

Do not let `ReflectionTest` grow a coverage list as long as the unit tests; the two will keep duplicating each other.

## Build parallelism and diagnostics

| Parameter | Effect |
| --- | --- |
| `NLS_BUILD_JOBS` | Overrides outer MSBuild parallelism |
| `NLS_MSVC_MP_JOBS` | Limits target-internal MSVC `/MP` processes |
| `NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION=OFF` | Disables target-internal parallel compilation |
| `NLS_ENABLE_PCH=OFF` | Disables precompiled headers to diagnose include order and macro leakage |

Precompiled headers should only carry stable base headers. Putting Assimp, ImGui, Windows, DX12, or a JSON library into the PCH hides public header coupling.

## CI coverage

| Workflow | Coverage |
| --- | --- |
| Build Windows | Release configuration build |
| Build Linux | Debug and Release |
| Build MacOS | Debug and Release |
| Metal Threading Validation | Metal thread ownership validation |
| Documentation | Strict handbook checks, synchronization tool tests, and Wiki export; documentation changes on `main` publish the Wiki after validation |

Windows CI runs Release only, so Windows Debug regressions are a local responsibility. Cross-platform conclusions must come from an actual run on each platform, never inferred from Windows results.

Documentation also supports manual dispatch. Disable the `publish` input to run validation only; enable it to synchronize the Wiki from `Docs/`. If synchronization fails, inspect the Git diagnostics in the Publish wiki job.

## Debugging rendering problems

The first tool for rendering issues is a RenderDoc capture, not a desktop screenshot. See [Profiling and Graphics Debugging](profiling.md).

## Debugging script problems

Breakpoints, process attach, and hot reload troubleshooting are in [Script Debugging and Hot Reload](../scripting/debugging.md).

## Crashes and stacks

`Tools/StackDump` provides a crash stack dump tool, which must be built first:

```powershell
Tools/StackDump/StackDump.exe
```

In debug builds an assertion failure breaks directly with a call stack, which is far more useful than analyzing logs afterwards.

## Documentation acceptance scope

On 2026-10-14, `NullusUnitTests`, `Editor`, and `Launcher` were rebuilt successfully from the current source on Windows. The 10 C# code blocks in the handbook compiled into separate language validation assemblies with zero errors and zero warnings.

Of 496 selected tests covering Prefabs, UI, the script runtime, editor scripting, Launcher, object graphs, and asset databases, 490 passed and 6 were skipped for environment or explicit feature switches. The 13 shader-related cases passed after setting the repository's DXC path. These tests exercise native/managed contracts; they do not replace the complete desktop interaction workflow.

The current Debug Editor completed OBJ import, generated resources and an instance, activated a C# assembly, and produced a DX12 frame readback in an isolated project. Separate 30-frame and readback runs both exited naturally with code 0. The first run combined both validation modes and waited during shutdown; thread stacks were saved. Passing the separate runs does not close that combined-path issue.

On 2026-10-15, Documentation validation and automatic synchronization on `main` passed. The publishing job used `GITHUB_TOKEN` to update two Wiki pages, and a subsequent manual run created no additional commit. The full desktop exercise and shutdown on the combined path require separate acceptance. Commands, log locations, skip reasons, and Wiki synchronization results are recorded in the repository's `Tools/Docs/ACCEPTANCE.md`. This is evidence for this run, not a support claim for other platforms.

## Further reading

- [Profiling and Graphics Debugging](profiling.md) covers CPU and GPU time plus capture analysis.
- [Build Options](../reference/build-options.md) lists every switch.
