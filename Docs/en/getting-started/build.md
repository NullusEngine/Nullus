# Environment and Build

This page walks through building Nullus from scratch: dependencies, the three platform entry points, common failures, and where build artifacts land.

## Prerequisites

| Dependency | Requirement | Notes |
| --- | --- | --- |
| Git | Any recent version | Submodules must be fetched |
| CMake | 3.18 or newer | Minimum declared by the project |
| C++ compiler | C++20 capable | MSVC (VS 2022) on Windows, GCC/Clang on Linux, Xcode toolchain on macOS |
| Python | 3.8 or newer | Used by the dependency setup scripts |
| .NET SDK | 8.0.408 | Pinned by `global.json`; when missing, CMake downloads it into the repository instead of touching the system `PATH` |
| Vulkan SDK | 1.4.341 or compatible | Needed only for the Vulkan backend or Linux/WSL validation |

## Getting the source

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

## Preparing dependencies

Windows:

```powershell
.\SetupDependencies.bat
```

Linux / macOS:

```bash
./SetupDependencies.sh
```

The scripts prepare third-party content inside the repository working tree and do not require elevated system permissions. The .NET SDK follows the same repository-local policy.

## Building

### Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target Editor Launcher
```

The repository also ships a wrapper that creates `build/windows`, derives MSBuild parallelism from the CPU count, and restores the MetaParser project when needed:

```powershell
build_windows.bat Debug x64
```

Environment variables: `NLS_BUILD_JOBS` overrides MSBuild parallelism, `NLS_MSVC_MP_JOBS` limits target-internal `/MP` compilers, and `NLS_BUILD_TARGETS` restricts the build to named targets.

### Linux

```bash
./build_linux.sh debug
./build_linux.sh release ninja
```

The script creates `build/linux`, prefers `gcc`/`g++` when `CC`/`CXX` are unset, and exports `compile_commands.json`.

### macOS

```bash
./build_macos.sh debug
```

macOS builds default to the Metal backend. Metal is not supported on non-Apple platforms and cannot be reached through a null-device fallback.

### WSL and Vulkan

```bash
./build_wsl_vulkan.sh --install-deps
./build_wsl_vulkan.sh --run-editor
```

This uses the `build/wsl-vulkan` directory. `--install-deps` installs the Vulkan/GLFW/Ninja prerequisites, and `--run-editor` starts a Debug editor smoke run after both configurations build.

## Build targets

| Target | Purpose |
| --- | --- |
| `Editor` | The editor |
| `Game` | The game runtime |
| `Launcher` | Project launcher |
| `NullusBuild` | Headless packaging CLI and shared build pipeline |
| `NullusUnitTests` | Behavior tests (needs `NLS_BUILD_TESTS=ON`) |
| `NullusPerformanceTests` | Performance and stress tests (needs `NLS_BUILD_PERFORMANCE_TESTS=ON`) |
| `ReflectionTest` | Reflection smoke tool |

Executables are deployed under `App/`, for example `App/Win64_Debug_Runtime_Shared/Editor.exe` and `App/Win64_Release_Runtime_Shared/Editor.exe` on Windows. Debugger launch profiles rely on that layout; see [Script Debugging and Hot Reload](../scripting/debugging.md).

## Packaging a player

Build the headless packaging tool, then run a profile from the project:

```powershell
cmake --build build --config RelWithDebInfo --target Game NullusBuild
build/bin/RelWithDebInfo/NullusBuild.exe build `
  --project TestProject/TestProject.nullus `
  --profile windows-shipping `
  --player App/Win64_RelWithDebInfo_Runtime_Shared `
  --output Dist `
  --validate-launch
```

The pipeline reads the project's `BuildProfiles.json`, bakes target-platform content with `NLS_Cook` (textures, ShaderLab passes, materials, models and UI), stages the player, writes `base.npak`, validates it and runs the packaged self-test. C# projects are compiled from `Assets/**/*.cs` into a project-specific `GameScripts.dll`; the prebuilt template assembly is never reused. `--player` points at a directory containing `Game.exe` and its runtime DLLs (the `Game` target's `App/Win64_<Config>_Runtime_Shared` output). See [Player Packaging and Distribution](../platform-build/packaging.md) for profiles, `maxTextureSize`, package layout and known limitations.

### Linux and macOS packages

Add a profile whose target uses `platform: "linux"`, `architecture: "x64"` and `graphicsBackend: "vulkan"` (or `platform: "macos"`, `architecture: "arm64"`, `graphicsBackend: "metal"`). The player template must contain `Game`, its shared libraries, `Assets/` and optional `Scripts/`/`Runtime/dotnet/` payloads:

```bash
NullusBuild build --project MyGame.nullus --profile linux-shipping \
  --player App/Linux_Release_Shared --output Dist --validate-launch
```

Linux packages can be assembled from Windows; the packaged self-test is skipped on a non-Linux host with an explicit `build-launch-cross-host` warning. Linux ELF `RPATH`/`RUNPATH` is rewritten to `$ORIGIN`, and ZIP entries carry Unix executable modes. macOS packaging requires a macOS host, a `.app` (or raw `Game`) template, and currently reports unsigned output; Developer ID signing and notarization are not implemented yet. The Unix adapter currently emits ZIP or directory output only. See [Player Packaging and Distribution](../platform-build/packaging.md) for the current Linux template limitation and mobile status.

## Build options

Common switches (full list in [Build Options](../reference/build-options.md)):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DNLS_BUILD_TESTS=ON `
  -DNLS_ENABLE_TRACY=ON
```

Enabled by default: `NLS_ENABLE_PROFILING`, `NLS_ENABLE_CSHARP_SCRIPTING`, `NLS_ENABLE_LUA_SCRIPTING`, `NLS_ENABLE_GAME_UI`, `NLS_ENABLE_PCH`, and `NLS_USE_SHARED_LIBRARY`. Disabled by default: `NLS_BUILD_TESTS`, `NLS_BUILD_PERFORMANCE_TESTS`, `NLS_ENABLE_TRACY`, `NLS_ENABLE_TIMELINE_PROFILER`, and `NLS_ENABLE_CXX_MODULES`.

## Troubleshooting

### `MSB6001`: duplicated `Path` / `PATH`

When both `Path` and `PATH` exist in the host environment, MSBuild refuses to start. Use the wrapper that cleans only the build subprocess:

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug --target Editor
```

It does not modify user environment variables.

### Out of memory or PDB contention

Lower the parallelism:

```bat
set NLS_BUILD_JOBS=8
set NLS_MSVC_MP_JOBS=4
build_windows.bat Debug x64
```

### Diagnosing header coupling or macro leakage

Reconfigure with precompiled headers disabled:

```powershell
cmake -S . -B build-no-pch -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_PCH=OFF
```

### Only verifying the reflection pipeline

Build and run the lightweight tool:

```powershell
cmake --build build --config Debug --target ReflectionTest
```

## Next steps

- [Your First Project](first-project.md) uses the editor you just built.
- [Build Options](../reference/build-options.md) lists every CMake switch.
- [Debugging and Testing](../quality/debugging.md) covers behavior and performance test runs.
