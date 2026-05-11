# Incremental Build Validation

Date: 2026-05-11

## MetaParser Stable Write Verification

Command:

```powershell
& "Build\Runtime\Base\NLS_Base_run_metaparser_Debug.cmd" "Build\Runtime\Base\NLS_Base.precompile.json"
```

Observed generated outputs before and after the repeated invocation:

| File | Before UTC | After UTC | Stable |
| --- | --- | --- | --- |
| `Runtime/Base/Gen/MetaGenerated.cpp` | `2026-05-11T09:53:58.1039229Z` | `2026-05-11T09:53:58.1039229Z` | yes |
| `Runtime/Base/Gen/MetaGenerated.h` | `2026-05-11T09:53:58.1039229Z` | `2026-05-11T09:53:58.1039229Z` | yes |
| `Runtime/Base/Gen/NLS_Base_MetaGenerated.h` | `2026-05-11T09:53:58.1039229Z` | `2026-05-11T09:53:58.1039229Z` | yes |

Implementation note:

- `Tools/MetaParser/src/MetaParserTool.Generation.cs` writes generated text with `WriteAllTextIfChanged`, comparing UTF-8 bytes before writing.

## MetaParser Changed Input Verification

Temporary validation edit:

- added a reflected `FUNCTION()` method to `Runtime/Base/Reflection/MetaParserFieldMethodSample.h`
- invoked `Build\Runtime\Base\NLS_Base_run_metaparser_Debug.cmd`
- observed `Runtime/Base/Gen/Reflection/MetaParserFieldMethodSample.generated.cpp` and `.h` timestamps update
- removed the temporary method
- invoked the same launcher again to restore generated output through the normal generator path

Final diff check for the temporary source and generated outputs was clean.

## Windows MetaParser Launcher

Sample generated launcher:

```text
Build\Runtime\Base\NLS_Base_run_metaparser_Debug.cmd
```

The launcher prepends libclang native directories to `PATH` and checks dependency existence in-place. It no longer copies `libclang.dll` or `libClangSharp.dll` into a shared publish directory per MetaParser invocation, avoiding parallel copy races.

## Windows Build Parallelism

`build_windows.bat` now derives MSBuild parallelism from:

- default: `%NUMBER_OF_PROCESSORS%`, falling back to `4`
- override: `NLS_BUILD_JOBS`
- serialized compatibility mode: `NLS_BUILD_JOBS=1`

It also forwards `NLS_MSVC_MP_JOBS` to CMake so outer MSBuild `/m` and inner MSVC `/MPn` can be tuned independently.

## Targeted Build Verification

First targeted build after current source edits:

```powershell
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
```

Result:

- exit code: `0`
- elapsed: `46.8` seconds
- `Runtime/Base/Gen/MetaGenerated.cpp`, `MetaGenerated.h`, and `NLS_Base_MetaGenerated.h` timestamps remained stable.

Immediate no-change rebuild:

```powershell
cmake --build Build --config Debug --target NullusUnitTests ReflectionTest -- /m:4
```

Result:

- exit code: `0`
- elapsed: `12.3` seconds
- the same `Runtime/Base/Gen` timestamps remained stable.

## Test Executables

```powershell
Build\bin\Debug\ReflectionTest.exe
```

Result: passed, all reflection tests passed.

```powershell
Build\bin\Debug\NullusUnitTests.exe
```

Result: 719 passed, 1 failed:

- `PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope`

Focused reruns:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.*
```

Result: both focused reruns passed. This matches the previously recorded flaky/environment-sensitive behavior in `BuildLogs/header-include-baseline.md`.

## Full-Suite Recheck

Rechecked on 2026-05-12 local time:

```powershell
Build\bin\Debug\NullusUnitTests.exe
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope
```

Result:

- Full suite: 720 tests from 89 suites ran, 719 passed, 1 failed.
- Failed test: `PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope`.
- Focused rerun of the failed test: passed.

T012 remains unchecked until the complete unfiltered suite exits with code 0.

## Full-Suite Recheck After ProfilerPanel Test Fix

Rechecked on 2026-05-12 local time after moving
`NLS::Base::Profiling::Profiler::ResetForTesting()` before `ProfilerPanel`
construction in `Tests/Unit/PanelWindowHookTests.cpp`.

```powershell
cmake --build Build --config Debug --target NullusUnitTests -- /m:4
Build\bin\Debug\NullusUnitTests.exe --gtest_filter=PanelWindowHookTests.*
Build\bin\Debug\NullusUnitTests.exe
```

Result:

- Targeted `NullusUnitTests` build passed.
- Long-prefix `PanelWindowHookTests.*` reproduction passed: `178/178`.
- Full unfiltered unit test suite passed: `720/720`.

Root cause:

- Earlier DX12 tests could leave a Profiler GPU context with native device/queue
  pointers. Constructing `ProfilerPanel` before resetting the Profiler enabled
  the timeline sink and replayed stale GPU context into the panel test.

Fix:

- Reset the Profiler before constructing `ProfilerPanel`, so the panel starts
  from a clean test-local profiling state.
