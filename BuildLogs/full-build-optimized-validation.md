# Full Build Optimized Validation

Date: 2026-05-11

## Fresh Optimized Debug Build

Command:

```powershell
cmake -S . -B BuildFullOptimizedVerify -G "Visual Studio 17 2022" -A x64
cmake --build BuildFullOptimizedVerify --config Debug --target Launcher Editor Game NullusUnitTests ReflectionTest -- /m:4
```

Observed result:

- Configure: 12.1 seconds
- Fresh full build: 379.7 seconds / 6.33 minutes
- Exit code: 0

Comparison:

- Original clean Debug baseline: 10:42 / 642 seconds
- Optimized clean Debug build: 6:20 / 379.7 seconds
- Improvement: about 40.9% faster

The optimized build met the full-build success target and produced `Launcher`, `Editor`, `Game`, `NullusUnitTests`, and `ReflectionTest` Debug artifacts.

## Runtime Smoke Verification

Commands:

```powershell
BuildFullOptimizedVerify\bin\Debug\ReflectionTest.exe
BuildFullOptimizedVerify\bin\Debug\NullusUnitTests.exe --gtest_filter=GuidTests.*:GraphicsBackendUtilsTests.*:*ObjectGraph*
```

Observed result:

- `ReflectionTest`: passed, all reflection tests passed.
- Targeted `NullusUnitTests` smoke: 71 tests from 5 suites passed.

The full `Build\bin\Debug\NullusUnitTests.exe` run remains tracked separately in `BuildLogs/incremental-build-validation.md`: latest full-suite run passed 719 tests and failed the previously observed environment-sensitive `PanelWindowHookTests.ProfilerPanelDrawDoesNotAdvanceTimelineFrameInsideActiveScope`, while focused reruns of that test and `PanelWindowHookTests.*` passed.

## Self-Review

Checks:

```powershell
git diff --name-only -- Runtime/*/Gen Project/Editor/Gen
git -C ThirdParty\ImGui status --short
git diff --check
rg -n "NLS_ASSIMP_BUILD_ALL_FORMATS|ASSIMP_BUILD_(FBX|OBJ)_IMPORTER|ASSIMP_NO_EXPORT" ThirdParty\CMakeLists.txt Docs\Testing.md
```

Observed result:

- No generated-file diffs under `Runtime/*/Gen` or `Project/Editor/Gen`.
- No `ThirdParty\ImGui` submodule edits.
- `git diff --check` reported only CRLF conversion warnings and no whitespace errors.
- Assimp defaults keep FBX and OBJ importers enabled, disable exporters, and retain the documented `NLS_ASSIMP_BUILD_ALL_FORMATS=ON` escape hatch for broad format coverage.

Risk notes:

- Platform-specific `/MP` behavior is guarded for MSVC only.
- The Assimp default intentionally narrows importer/exporter coverage for developer builds; compatibility validation can enable `NLS_ASSIMP_BUILD_ALL_FORMATS=ON` without source edits.
- No generated reflection outputs were hand-edited.
