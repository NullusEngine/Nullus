# 构建选项

本页列出 CMake 配置项与构建脚本参数。默认值来自仓库根目录的 `CMakeLists.txt`。

## CMake 选项

### 构建产物

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NLS_USE_SHARED_LIBRARY` | `ON` | 运行时以共享库构建 |

### 功能开关

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NLS_ENABLE_CSHARP_SCRIPTING` | `ON` | CoreCLR 脚本后端 |
| `NLS_ENABLE_LUA_SCRIPTING` | `ON` | Lua 脚本后端 |
| `NLS_ENABLE_GAME_UI` | `ON` | 保留模式游戏 UI 运行时与输入注入 |
| `NLS_ENABLE_LUAPANDA_DEBUGGING` | `OFF` | Debug 构建下编译 LuaPanda 调试集成 |

### 性能与诊断

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NLS_ENABLE_PROFILING` | `ON` | 共享性能标记 |
| `NLS_ENABLE_TRACY` | `OFF` | Tracy 目标端 |
| `NLS_ENABLE_TIMELINE_PROFILER` | `OFF` | 编辑器内置 Timeline Profiler |
| `NLS_BUILD_TRACY_PROFILER` | 跟随 `NLS_ENABLE_TRACY` | 构建 Tracy GUI 工具 |

### 测试

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NLS_BUILD_TESTS` | `OFF` | 构建 `NullusUnitTests` |
| `NLS_BUILD_PERFORMANCE_TESTS` | `OFF` | 构建 `NullusPerformanceTests` |

### 编译

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NLS_ENABLE_PCH` | `ON` | 主要的目标使用窄范围预编译头 |
| `NLS_ENABLE_MSVC_MULTIPROCESSOR_COMPILATION` | `ON` | 目标内启用 MSVC `/MP` |
| `NLS_MSVC_MP_JOBS` | 空 | 限制目标内 `/MP` 的 worker 数 |
| `NLS_ENABLE_CXX_MODULES` | `OFF` | 实验性 C++20 modules 试点目标 |

C++ 标准固定为 C++20，且 `CMAKE_CXX_STANDARD_REQUIRED` 已打开。

## 配置示例

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

## 构建脚本

### Windows

```bat
build_windows.bat [Debug|Release] [x64|ARM64]
```

| 行为 | 说明 |
| --- | --- |
| 构建目录 | `build/windows` |
| 并行度 | `NUMBER_OF_PROCESSORS`，可用 `NLS_BUILD_JOBS` 覆盖 |
| 配置类型 | 通过 `-DCMAKE_CONFIGURATION_TYPES` 限定为所选配置 |
| 额外参数 | `NLS_MSVC_MP_JOBS` 透传给 CMake |
| 目标选择 | `NLS_BUILD_TARGETS` 只构建指定目标 |

### Linux

```bash
./build_linux.sh <debug|release> [make|ninja]
```

| 行为 | 说明 |
| --- | --- |
| 构建目录 | `build/linux` |
| 生成器 | 有 Ninja 时默认用 Ninja，否则用 Unix Makefiles |
| 编译器 | 未显式设置时优先 `gcc` / `g++` |
| 附加输出 | `CMAKE_EXPORT_COMPILE_COMMANDS=ON` |
| 额外参数 | `NLS_CMAKE_OPTIONS`、`NLS_BUILD_JOBS`、`NLS_BUILD_TARGETS` |

### macOS

```bash
./build_macos.sh <debug|release>
```

### WSL + Vulkan

```bash
./build_wsl_vulkan.sh [--install-deps] [--run-editor]
```

| 参数 | 说明 |
| --- | --- |
| `--install-deps` | 用 apt 安装 Vulkan、GLFW、Ninja 等构建前置 |
| `--run-editor` | 两种配置构建完成后启动 Debug 编辑器冒烟 |
| 构建目录 | `build/wsl-vulkan` |

## 构建目标

| 目标 | 说明 |
| --- | --- |
| `Editor` | 编辑器 |
| `Game` | 游戏运行时 |
| `Launcher` | 项目启动器 |
| `NullusBuild` | 无界面打包命令行与共享构建流水线 |
| `NullusUnitTests` | 行为测试 |
| `NullusPerformanceTests` | 性能与压力测试 |
| `ReflectionTest` | 反射冒烟工具 |

## 输出位置

| 路径 | 内容 |
| --- | --- |
| `build/`、`build/windows`、`build/linux`、`build/wsl-vulkan` | CMake 构建树 |
| `build/runtime/<配置>` | 运行时库直接输出 |
| `App/<平台>_<配置>_Runtime_<链接方式>/` | 部署的可执行文件与依赖 |

编辑器构建会把 Tracy 工具部署到 `App/<platform>_<config>_Runtime_*/Tracy/<version>`，使打包目录自带协议匹配的分析器。

## 下一步

- [环境准备与构建](../getting-started/build.md)：从零构建的完整步骤。
- [调试与测试](../quality/debugging.md)：测试目标的运行方式。
