# 环境准备与构建

本页给出从零构建 Nullus 的完整步骤，包括依赖准备、三平台构建入口、常见失败处理，以及构建产物的位置。

## 前置条件

| 依赖 | 要求 | 说明 |
| --- | --- | --- |
| Git | 任意近期版本 | 需要拉取子模块 |
| CMake | 3.18 及以上 | 工程声明的最低版本 |
| C++ 编译器 | 支持 C++20 | Windows 用 MSVC（VS 2022），Linux 用 GCC/Clang，macOS 用 Xcode 工具链 |
| Python | 3.8 及以上 | 供 `SetupDependencies` 脚本使用 |
| .NET SDK | 8.0.408 | 由 `global.json` 固定；缺失时 CMake 会下载到仓库内，不修改系统 `PATH` |
| Vulkan SDK | 1.4.341 或兼容版本 | 仅在构建 Vulkan 后端或跑 Linux/WSL 验证时需要 |

## 获取代码

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

## 准备依赖

Windows：

```powershell
.\SetupDependencies.bat
```

Linux / macOS：

```bash
./SetupDependencies.sh
```

依赖脚本会把第三方内容准备到仓库内的工作目录，不需要提升系统权限。`.NET SDK` 采用同样的仓库内策略。

## 构建

### Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target Editor Launcher
```

仓库同时提供封装脚本，它会创建 `build/windows` 目录、按 CPU 数决定 MSBuild 并行度，并在需要时恢复 MetaParser 项目：

```powershell
build_windows.bat Debug x64
```

可用环境变量：`NLS_BUILD_JOBS` 覆盖 MSBuild 并行度，`NLS_MSVC_MP_JOBS` 限制目标内 `/MP` 编译进程数，`NLS_BUILD_TARGETS` 只构建指定目标。

### Linux

```bash
./build_linux.sh debug
./build_linux.sh release ninja
```

脚本会创建 `build/linux`，在未显式设置 `CC`/`CXX` 时优先使用 `gcc`/`g++`，并导出 `compile_commands.json`。

### macOS

```bash
./build_macos.sh debug
```

macOS 构建默认选择 Metal 后端。Metal 在非 Apple 平台上不受支持，不能通过空设备回退路径启用。

### WSL + Vulkan

```bash
./build_wsl_vulkan.sh --install-deps
./build_wsl_vulkan.sh --run-editor
```

脚本使用 `build/wsl-vulkan` 目录，`--install-deps` 安装 Vulkan/GLFW/Ninja 依赖，`--run-editor` 在两种配置构建完成后启动 Debug 编辑器冒烟。

## 构建目标

| 目标 | 作用 |
| --- | --- |
| `Editor` | 编辑器 |
| `Game` | 游戏运行时 |
| `Launcher` | 项目启动器 |
| `NullusUnitTests` | 行为测试（需要 `NLS_BUILD_TESTS=ON`） |
| `NullusPerformanceTests` | 性能与压力测试（需要 `NLS_BUILD_PERFORMANCE_TESTS=ON`） |
| `ReflectionTest` | 反射冒烟工具 |

构建产物按配置部署到 `App/` 下，例如 Windows 的 `App/Win64_Debug_Runtime_Shared/Editor.exe`、`App/Win64_Release_Runtime_Shared/Editor.exe`。选择调试器启动配置时要以这个目录结构为准，见[脚本调试与热重载](../scripting/debugging.md)。

## 构建选项

常用开关（完整清单见[构建选项](../reference/build-options.md)）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DNLS_BUILD_TESTS=ON `
  -DNLS_ENABLE_TRACY=ON
```

默认开启的选项包括 `NLS_ENABLE_PROFILING`、`NLS_ENABLE_CSHARP_SCRIPTING`、`NLS_ENABLE_LUA_SCRIPTING`、`NLS_ENABLE_GAME_UI`、`NLS_ENABLE_PCH` 与 `NLS_USE_SHARED_LIBRARY`。`NLS_BUILD_TESTS`、`NLS_BUILD_PERFORMANCE_TESTS`、`NLS_ENABLE_TRACY`、`NLS_ENABLE_TIMELINE_PROFILER`、`NLS_ENABLE_CXX_MODULES` 默认关闭。

## 常见问题

### `MSB6001`：`Path` / `PATH` 重复

宿主环境里 `Path` 与 `PATH` 同时存在时，MSBuild 会拒绝启动。使用只影响构建子进程的清理入口：

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug --target Editor
```

该脚本不修改用户环境变量。

### 内存不足或 PDB 竞争

降低并行度：

```bat
set NLS_BUILD_JOBS=8
set NLS_MSVC_MP_JOBS=4
build_windows.bat Debug x64
```

### 需要排查头文件耦合或宏泄漏

关闭预编译头重新配置：

```powershell
cmake -S . -B build-no-pch -G "Visual Studio 17 2022" -A x64 -DNLS_ENABLE_PCH=OFF
```

### 只想验证反射链路

构建并运行轻量工具：

```powershell
cmake --build build --config Debug --target ReflectionTest
```

## 下一步

- [第一个项目](first-project.md)：用构建好的编辑器创建工程。
- [构建选项](../reference/build-options.md)：查看全部 CMake 开关。
- [调试与测试](../quality/debugging.md)：运行行为测试与性能测试。
