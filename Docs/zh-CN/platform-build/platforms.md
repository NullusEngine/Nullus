# 平台支持矩阵

本页说明引擎在各平台上的构建与运行状态。区分三件事：**能构建**、**能运行冒烟**、**有持续验证**。

## 支持矩阵

| 平台 | 构建 | 默认图形后端 | 运行时状态 |
| --- | --- | --- | --- |
| Windows (x64) | 支持，MSVC / VS 2022 | DX12 | 运行时主线，有持续验证 |
| Linux | 支持，GCC / Clang | Vulkan 1.1 | 运行时主线 |
| WSL | 支持，`build_wsl_vulkan.sh` | Vulkan 1.1 | WSLg 基线为 `llvmpipe` |
| macOS | 支持 | Metal | 构建目标与原生实现；Metal 运行时验证属于独立门槛 |

`build_windows.bat` 同时接受 `x64` 与 `ARM64` 平台参数。ARM64 属于构建目标，其运行状态需要单独验证，不能从 x64 结果推断。

## 图形后端

| 后端 | 平台 | 状态 |
| --- | --- | --- |
| DX12 | Windows | 支持 |
| Vulkan 1.1 | Linux / WSL | 支持 |
| Metal | macOS | 原生实现；非 Apple 平台不支持 |
| DX11 | Windows | 显式受限，启动时报告不受支持并正常退出 |
| OpenGL | Windows | 显式受限，启动时报告不受支持并正常退出 |

受限后端不会作为回退路径被启用。完整能力矩阵见[图形后端与大场景特性](../rendering/backends.md)。

## 构建配置

| 配置 | 说明 |
| --- | --- |
| `Debug` | 未优化，带断言与调试信息 |
| `RelWithDebInfo` | 优化并保留调试信息，用于性能测量 |
| `Release` | 优化构建，CI 与发布使用 |

Windows 上 CI 只保留 Release 配置，Debug 覆盖由 Linux 与 macOS 工作流承担。这意味着 Windows 的 Debug 回归需要本地执行，见[调试与测试](../quality/debugging.md)。

## 依赖的版本固定

| 依赖 | 固定方式 |
| --- | --- |
| .NET SDK | `global.json` 固定为 `8.0.408`，`rollForward` 为 `disable` |
| Vulkan SDK | CI 固定安装 `1.4.341` |
| CMake | 工程声明最低 `3.18` |
| C++ 标准 | `CMAKE_CXX_STANDARD 20` |

`.NET SDK` 缺失时 CMake 会下载到仓库内，不修改系统 `PATH`。

## 平台差异

| 主题 | 差异 |
| --- | --- |
| 启动资源配置 | Windows 默认 `pipelined`，其它平台默认 `async` |
| RHI 线程模式 | DX12 / Vulkan 默认 `tasks`；Metal 保持 `dedicated_thread` |
| 路径分隔符 | 资产路径统一使用正斜杠 |
| 环境清理 | `Tools/BuildCleanEnvironment.cmd` 面向 Windows 构建宿主 |

## 交叉编译与容器

仓库提供 WSL 专用的构建脚本，通过 `build/wsl-vulkan` 目录与 Linux 构建隔离。容器或远程环境下的构建沿用同一套 CMake 入口，只是图形后端选择与依赖准备方式不同。

## 尚未支持的平台

移动端、主机与网页目标不在当前范围内。这些目标的引入需要后端抽象、资源管线与构建系统的配套工作，属于[包装与分发规划](runtime.md)之外的长期方向。

## 下一步

- [环境准备与构建](../getting-started/build.md)：具体命令与故障处理。
- [Game 运行时与资产清单](runtime.md)：运行时如何消费构建产物。
