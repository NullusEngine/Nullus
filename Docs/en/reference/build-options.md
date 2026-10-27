# Build Options

This page lists the CMake configuration options and build script arguments. Defaults come from the repository root `CMakeLists.txt`.

## CMake options

### Build output

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_USE_SHARED_LIBRARY` | `ON` | Build the runtime as shared libraries |

### Feature switches

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_ENABLE_CSHARP_SCRIPTING` | `ON` | CoreCLR scripting backend |
| `NLS_ENABLE_LUA_SCRIPTING` | `ON` | Lua scripting backend |
| `NLS_ENABLE_GAME_UI` | `ON` | Retained-mode game UI runtime and input injection |
| `NLS_ENABLE_LUAPANDA_DEBUGGING` | `OFF` | Compile the LuaPanda debugger integration in Debug builds |

### Performance and diagnostics

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_ENABLE_PROFILING` | `ON` | Shared profiling markers |
| `NLS_ENABLE_TRACY` | `OFF` | Tracy destination |
| `NLS_ENABLE_TIMELINE_PROFILER` | `OFF` | Built-in editor Timeline Profiler |
| `NLS_BUILD_TRACY_PROFILER` | Follows `NLS_ENABLE_TRACY` | Build the Tracy GUI tool |

### Tests

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_BUILD_TESTS` | `OFF` | Build `NullusUnitTests` |
| `NLS_BUILD_PERFORMANCE_TESTS` | `OFF` | Build `NullusPerformanceTests` |

### Compilation

| Option | Default | Effect |
| --- | --- | --- |
| `NLS_ENABLE_PCH` | `ON` | Narrow precompiled headers for the main targets |
| `NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION` | `ON` | Enable target-internal MSVC `/MP` |
| `NLS_MSVC_MP_JOBS` | empty | Limit target-internal `/MP` worker count |
| `NLS_ENABLE_CXX_MODULES` | `OFF` | Experimental C++20 modules pilot targets |

The C++ standard is fixed at C++20 with `CMAKE_CXX_STANDARD_REQUIRED` enabled.

## Configuration examples

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DNLS_BUILD_TESTS=ON `
  -DNLS_ENABLE_TRACY=ON `
  -DNLS_MSVC_MP_JOBS=8
```

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Debug -G Ninja \
  -DNLS_BUILD_TESTS=ON
```

## Build scripts

### Windows

```bat
build_windows.bat [Debug|Release] [x64|ARM64]
```

| Behavior | Notes |
| --- | --- |
| Build directory | `build/windows` |
| Parallelism | `NUMBER_OF_PROCESSORS`, overridable with `NLS_BUILD_JOBS` |
| Configuration types | Restricted to the selected configuration via `-DCMAKE_CONFIGURATION_TYPES` |
| Extra arguments | `NLS_MSVC_MP_JOBS` is forwarded to CMake |
| Target selection | `NLS_BUILD_TARGETS` restricts the build |

### Linux

```bash
./build_linux.sh <debug|release> [make|ninja]
```

| Behavior | Notes |
| --- | --- |
| Build directory | `build/linux` |
| Generator | Ninja when available, otherwise Unix Makefiles |
| Compiler | Prefers `gcc` / `g++` when unset |
| Extra output | `CMAKE_EXPORT_COMPILE_COMMANDS=ON` |
| Extra arguments | `NLS_CMAKE_OPTIONS`, `NLS_BUILD_JOBS`, `NLS_BUILD_TARGETS` |

### macOS

```bash
./build_macos.sh <debug|release>
```

### WSL and Vulkan

```bash
./build_wsl_vulkan.sh [--install-deps] [--run-editor]
```

| Argument | Notes |
| --- | --- |
| `--install-deps` | Installs Vulkan, GLFW, and Ninja prerequisites with apt |
| `--run-editor` | Starts a Debug editor smoke run after both configurations build |
| Build directory | `build/wsl-vulkan` |

## Build targets

| Target | Notes |
| --- | --- |
| `Editor` | The editor |
| `Game` | Game runtime |
| `Launcher` | Project launcher |
| `NullusBuild` | Headless packaging CLI and shared build pipeline |
| `NullusUnitTests` | Behavior tests |
| `NullusPerformanceTests` | Performance and stress tests |
| `ReflectionTest` | Reflection smoke tool |

## Output locations

| Path | Content |
| --- | --- |
| `build/`, `build/windows`, `build/linux`, `build/wsl-vulkan` | CMake build trees |
| `build/runtime/<config>` | Direct runtime library output |
| `App/<platform>_<config>_Runtime_<linkage>/` | Deployed executables and dependencies |

Editor builds deploy the Tracy tools to `App/<platform>_<config>_Runtime_*/Tracy/<version>`, so a packaged directory carries a protocol-matched profiler.

## Next steps

- [Environment and Build](../getting-started/build.md) for the complete from-scratch walkthrough.
- [Debugging and Testing](../quality/debugging.md) for running the test targets.
