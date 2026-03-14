# Nullus Testing Guide

This document summarizes the current local and CI testing entry points for Nullus.

## Test modules

- `Tests/Unit/NullusUnitTests` is the formal unit test executable registered with `CTest`.
- `Tools/ReflectionTest` remains available as a smaller reflection smoke test.

`NullusUnitTests` currently covers:

- runtime reflection registration for Base, Core, and Engine types
- generated binding validation for `Runtime/*/Gen/MetaGenerated.h/.cpp`
- MetaParser integration regressions that would otherwise only show up at build time

## Local build and test

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target NullusUnitTests -- /m:1
ctest --test-dir build -C Debug --output-on-failure
```

Linux / macOS:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

To run the unit test executable directly:

- Windows: `Build/bin/Debug/NullusUnitTests.exe`
- Linux / macOS: `Build/bin/NullusUnitTests`

## CI behavior

GitHub Actions now runs `ctest` after the normal build step on:

- Windows
- Linux
- macOS

This means a pull request must now pass both:

- project compilation
- runtime reflection and MetaParser verification through `NullusUnitTests`

## Notes

- The test setup is designed to stay project-side and avoid patching third-party source where possible.
- `ThirdParty/json11` must point at a fetchable submodule commit, otherwise clean CI checkouts will fail before tests run.
