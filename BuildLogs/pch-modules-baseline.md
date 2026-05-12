# PCH And C++20 Modules Baseline

Date: 2026-05-11

Environment sampled before the first PCH implementation pass:

- Generator: Visual Studio 17 2022, x64
- CMake: 3.25.3
- MSVC: 19.39.33523
- Project C++ standard: C++20
- Root `cmake_minimum_required`: 3.18

Initial implementation notes:

- `NLS_ENABLE_PCH` is intended to be enabled by default for native C++ Runtime, Project, and Tests targets.
- Generated reflection sources should not be forced through PCH.
- `NLS_ENABLE_CXX_MODULES` is an experimental opt-in because practical C++20 Modules support requires a stricter CMake/generator/compiler matrix than the current project minimum.

Verification commands:

- `cmake -S . -B BuildPchCheck -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_PCH=ON`
- `cmake --build BuildPchCheck --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:1`
- `cmake -S . -B BuildNoPchCheck -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_PCH=OFF`
- `cmake --build BuildNoPchCheck --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:1`
- `BuildPchCheck\bin\Debug\NullusUnitTests.exe`
- `BuildPchCheck\bin\Debug\ReflectionTest.exe`

Observed results:

- PCH-enabled configuration completed successfully.
- PCH-enabled targeted build completed successfully after clearing stale MSBuild processes left by an interrupted build.
- PCH-disabled configuration completed successfully.
- PCH-disabled targeted build completed successfully.
- `NullusUnitTests`: 720 tests from 89 suites ran, 717 passed, 3 skipped.
- `ReflectionTest`: all reflection tests passed.
- `cmake -S . -B BuildMpJobsCheck -G "Visual Studio 17 2022" -A x64 -DNLS_MSVC_MP_JOBS=8 -DNLS_ENABLE_PCH=ON` generated Visual Studio projects with `MultiProcessorCompilation=true` and `ProcessorNumber=8` for checked native targets.
- `cmake -S . -B BuildMpJobsInvalid -G "Visual Studio 17 2022" -A x64 -DNLS_MSVC_MP_JOBS=zero` failed during configure with the expected validation error.

Timing caveat:

- The first PCH-enabled build attempt was interrupted and left MSBuild processes locking intermediate object files, so the resumed PCH build is valid as a compile verification but not as a clean timing sample.
- The PCH-disabled fresh build with `/m:1` took 401.2 seconds for `NLS_Render NullusUnitTests ReflectionTest`.

## C++20 Modules Pilot

Implementation:

- Added `Runtime/Math/_DISABLED/ModulesPilot/NullusMathPilot.ixx`, exporting a tiny low-dependency `nullus.math.pilot` module.
- Added `Runtime/Math/_DISABLED/ModulesPilot/NullusMathPilotConsumer.cpp`, importing the module and validating behavior with `static_assert`.
- Added opt-in target `NLS_ModulesPilot` behind `NLS_ENABLE_CXX_MODULES=ON`.
- The pilot is intentionally standalone: it lives under a runtime glob-excluded `_DISABLED` directory, is not linked into Runtime targets, is not installed into public APIs, and is not part of the MetaParser reflected header scan.
- Current pilot support is Windows MSVC with the Visual Studio generator. Other generators/platforms emit a configure warning and continue without creating the pilot target.

Verification:

```powershell
cmake -S . -B BuildModulesPilot -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_CXX_MODULES=ON
cmake --build BuildModulesPilot --config Debug --target NLS_ModulesPilot -- /m:4
```

Result: passed. MSVC built `NullusMathPilot.ixx` into an IFC and compiled the importing consumer.

Fallback/default behavior:

```powershell
cmake -S . -B BuildModulesOffCheck -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_CXX_MODULES=OFF
cmake --build Build --config Debug --target NLS_Render NullusUnitTests ReflectionTest -- /m:4
```

Result: passed. The default build path remains normal headers/PCH and does not require the module pilot.

Timing samples:

| Scenario | Configure | `NLS_ModulesPilot` build |
| --- | ---: | ---: |
| Modules-only pilot (`NLS_ENABLE_CXX_MODULES=ON`, `NLS_ENABLE_PCH=OFF`) | 11.4s | 0.7s |
| PCH-plus-Modules pilot (`NLS_ENABLE_CXX_MODULES=ON`, `NLS_ENABLE_PCH=ON`) | 11.8s | 0.7s |

Decision:

- The pilot proves the MSVC module chain works for a tiny isolated slice.
- The timing is too small and too isolated to justify broader module migration in this change.
- Broader adoption should wait for a separate migration plan that raises/branches the CMake support matrix and avoids reflected/generated code until MetaParser explicitly supports module interface files.

Self-review notes:

- ODR risk is low because the module exports only constexpr free functions in an isolated namespace and is not linked into production targets.
- Macro leakage risk is low because the pilot imports no project PCH and includes no macro-heavy headers.
- Generated-file interaction risk is low because `.ixx`/module files are outside reflected header globs and not parsed by MetaParser.
- Cross-platform fallback is explicit: unsupported generator/platform combinations warn and skip the pilot while `NLS_ENABLE_CXX_MODULES=OFF` remains the default.
