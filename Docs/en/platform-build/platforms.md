# Platform Support

This page states the build and run status per platform. Three things stay separate: **can build**, **can run a smoke test**, and **has continuous validation**.

## Support matrix

| Platform | Build | Default backend | Runtime status |
| --- | --- | --- | --- |
| Windows (x64) | Supported, MSVC / VS 2022 | DX12 | Primary runtime line with continuous validation |
| Linux | Supported, GCC / Clang | Vulkan 1.1 | Primary runtime line |
| WSL | Supported via `build_wsl_vulkan.sh` | Vulkan 1.1 | WSLg baseline is `llvmpipe` |
| macOS | Supported | Metal | Build target and native implementation; Metal runtime validation is a separate gate |

`build_windows.bat` accepts both the `x64` and `ARM64` platform arguments. ARM64 is a build target whose runtime status needs separate validation and cannot be inferred from x64 results.

## Graphics backends

| Backend | Platform | Status |
| --- | --- | --- |
| DX12 | Windows | Supported |
| Vulkan 1.1 | Linux / WSL | Supported |
| Metal | macOS | Native implementation; unsupported on non-Apple platforms |
| DX11 | Windows | Explicitly gated; startup reports unsupported and exits cleanly |
| OpenGL | Windows | Explicitly gated; startup reports unsupported and exits cleanly |

Gated backends are never enabled as a fallback path. The full capability matrix is in [Graphics Backends and Large-Scene Features](../rendering/backends.md).

## Build configurations

| Configuration | Notes |
| --- | --- |
| `Debug` | Unoptimized with assertions and debug info |
| `RelWithDebInfo` | Optimized with debug info; used for performance measurements |
| `Release` | Optimized; used by CI and releases |

Windows CI keeps only the Release configuration, so Debug coverage comes from the Linux and macOS workflows. Windows Debug regressions must therefore be run locally; see [Debugging and Testing](../quality/debugging.md).

## Pinned dependency versions

| Dependency | How it is pinned |
| --- | --- |
| .NET SDK | `global.json` pins `8.0.408` with `rollForward` set to `disable` |
| Vulkan SDK | CI installs `1.4.341` |
| CMake | The project declares a 3.18 minimum |
| C++ standard | `CMAKE_CXX_STANDARD 20` |

When the .NET SDK is missing, CMake downloads it into the repository rather than touching the system `PATH`.

## Platform differences

| Topic | Difference |
| --- | --- |
| Startup resource configuration | Windows defaults to `pipelined`, other platforms to `async` |
| RHI thread mode | DX12 and Vulkan default to `tasks`; Metal stays on `dedicated_thread` |
| Path separators | Asset paths always use forward slashes |
| Environment cleanup | `Tools/BuildCleanEnvironment.cmd` targets Windows build hosts |

## Cross-compilation and containers

The repository ships a WSL-specific build script that isolates its work under `build/wsl-vulkan` from the Linux build. Builds in containers or remote environments reuse the same CMake entry points; only backend selection and dependency preparation differ.

## Platforms not yet supported

Mobile, console, and web targets are out of scope today. Adding them requires coordinated work across backend abstraction, the content pipeline, and the build system, which sits beyond the packaging discussion in [Game Runtime and Asset Manifests](runtime.md).

## Next steps

- [Environment and Build](../getting-started/build.md) for concrete commands and troubleshooting.
- [Game Runtime and Asset Manifests](runtime.md) for how the runtime consumes build output.
