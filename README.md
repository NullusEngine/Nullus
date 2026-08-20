# Nullus
<p align="center">
    <img src="NullusLogo.png" width="400" alt="Nullus Engine logo">
</p>

![License Source Available](https://img.shields.io/badge/License-Source%20Available-blue.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake >=3.16](https://img.shields.io/badge/CMake-%E2%89%A53.16-064F8C)
![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4)
![Build Windows](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)
![Build Linux](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)
![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)

[中文版](./README.md) | [English](./README.en.md)

Nullus 是一个仍在持续演进中的 C++ 3D 引擎项目，当前聚焦于场景系统、资源系统、编辑器、反射代码生成和现代渲染管线。

## 界面预览

### Launcher

![Nullus Launcher](Docs/Screenshots/launcher.png)

### Editor

![Nullus Editor](Docs/Screenshots/editor.png)

## 项目亮点

- 运行时模块完整拆分为 `Base`、`Core`、`Engine`、`Math`、`Platform`、`Rendering`、`UI`
- 内置编辑器与运行时 `Game` 程序，共享同一套引擎主链路
- 基于 MetaParser 的代码生成式反射系统
- 基于反射的场景与对象序列化链路
- 前向与延迟两套场景渲染路径，并已接入 `FrameGraph`
- 支持 Windows、Linux、macOS 的跨平台构建流程

## 当前包含的核心能力

- 场景系统：`Scene / SceneManager / GameObject / Component`
- 资源系统：`Model / Texture / Shader / Material`
- 反射系统：MetaParser 驱动的代码生成式反射
- 序列化：场景与对象数据的反射式序列化
- 渲染系统：前向渲染、延迟渲染、GBuffer、clustered shading
- 编辑器能力：基础视图、调试绘制、选取、Gizmo、资源预览

## 快速开始

### 环境要求

- CMake 3.16 及以上
- 支持 C++20 的编译器
- .NET SDK 8.0.408（CMake 配置时默认自动安装到仓库内）
- Python 3.8 及以上（用于源码依赖准备脚本）
- Git

### 获取源码

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

### 准备第三方依赖

代码生成、MetaParser 和 C# 游戏脚本统一使用仓库固定的 .NET SDK 8.0.408。CMake 配置时如果本地缺失，会自动下载、校验并安装到 `Tools/Dotnet/<platform>/<arch>`，不会修改系统 PATH。也可以手动执行：

```powershell
.\SetupDependencies.bat --dependency dotnet-sdk
```

```bash
./SetupDependencies.sh --dependency dotnet-sdk
```

源码构建前先运行依赖准备脚本。它会在明确接受 Autodesk FBX SDK EULA 后，从官方地址下载并校验 FBX SDK，然后安装到 `ThirdParty/FBX/sdk/<platform>`：

```bash
./SetupDependencies.sh
```

Windows 也可以使用：

```powershell
.\SetupDependencies.bat
```

CI / headless 环境必须显式传入接受信号：

```bash
NLS_ACCEPT_AUTODESK_FBX_EULA=1 ./SetupDependencies.sh --non-interactive
```

Windows CI 如需固定目标架构，可附加 `--arch x64` 或 `--arch ARM64`。

### Windows 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

如果当前终端由工具宿主启动并报告 `MSB6001`（`Path` 与 `PATH` 重复），使用项目内的环境清理入口构建；它只清理构建子进程，不修改系统 PATH：

```powershell
.\Tools\BuildCleanEnvironment.cmd cmake --build build --config Debug
```

也可以使用仓库脚本：

```powershell
build_windows.bat Debug
build_windows.bat Release
build_windows.bat Debug ARM64
```

### Linux 构建

```bash
./build_linux.sh debug
./build_linux.sh release
```

### macOS 构建

```bash
./build_macos.sh debug
./build_macos.sh release
```

### 运行

构建完成后可以运行：

- `Editor`
- `Game`

### C# 脚本调试（VS Code / Visual Studio）

Editor 会使用仓库内 .NET 8 SDK 自动构建 `Debug` 版 `GameScripts.dll` 和 portable PDB。Editor 启动后会准备一次构建，保存 `Assets/**/*.cs` 后由 Asset Watcher 自动排队增量重建；构建完成后在帧边界热重载。C# 调试器附加到正在运行的 Editor 进程。在 Editor 的 `Debug` 菜单中：

1. 使用 `Generate VS Code Configuration` 或 `Generate Visual Studio Configuration`（仅生成 IDE 配置，不是编译前置条件）。
2. VS Code 打开项目生成的 `Library/IDE/VSCode/Nullus.code-workspace`；Visual Studio 打开 `Library/IDE/VisualStudio/Nullus.Project.sln`，直接按 F5。两者都会复用或启动正确的 Editor，不需要选择进程。
3. 进入 Play，在 `Assets/**/*.cs` 或 `Managed/Nullus.GameScripts/Scripts/**/*.cs` 设置断点；保存 `Assets/**/*.cs` 后等待自动 Debug 构建完成即可命中新程序集。

成功的脚本构建会在帧边界切换 collectible CoreCLR 加载上下文并保留字段；构建失败时旧程序集继续运行。调试配置只写项目内 `Library/IDE`，不会修改系统 `PATH`。

Visual Studio 的原生 Editor 配置直接读取当前方案配置：选择 `Debug|x64` 会启动 `App/Win64_Debug_Runtime_Shared/Editor.exe`，选择 `Release|x64` 会启动 `App/Win64_Release_Runtime_Shared/Editor.exe`。不需要维护单独的 Release 调试 profile；`Nullus: C# Scripts`、`Nullus: C# Attach and Play` 和 `Nullus: Editor + C# Mixed` 都遵循当前的 `Debug|x64` / `Release|x64` 选择。VSIX 位于 `Tools/Debug/VisualStudioExtension/Nullus.ScriptDebugger.vsix`，安装后重启 VS；完整流程见 [脚本调试指南](Docs/Scripting/ScriptDebugging.zh-CN.md)。

## 文档入口

- 脚本 API（中文）：`Docs/Scripting/ScriptingApi.zh-CN.md`
- Scripting API (English): `Docs/Scripting/ScriptingApi.en.md`
- 脚本调试指南（中文）：`Docs/Scripting/ScriptDebugging.zh-CN.md`
- Script debugging guide (English): `Docs/Scripting/ScriptDebugging.en.md`
- 反射工作流（中文）：`Docs/Reflection/ReflectionWorkflow.zh-CN.md`
- Reflection workflow (English): `Docs/Reflection/ReflectionWorkflow.en.md`
- 测试说明：`Docs/Testing.md`
- AI 工作流与仓库规范：`Docs/AIWorkflow.md`

## 反射与生成说明

Nullus 当前统一使用 MetaParser 生成反射代码：

1. 构建主工程时会先构建 `Tools/MetaParser`
2. `Runtime` 模块会在编译前运行 MetaParser
3. 生成对应的 `*.generated.h` / `*.generated.cpp`
4. 运行时通过生成代码完成类型声明、定义和注册

请不要手动修改 `Runtime/*/Gen/` 下的生成文件。

## 平台与 CI

GitHub Actions 当前覆盖：

- Windows
- Linux
- macOS

CI 会正常构建工程，并继续执行反射与单元测试相关目标。

## 常见问题

### `Failed to Init GLFW`

通常是图形环境没有准备好。请优先检查：

- 本机是否有可用桌面环境
- Linux / WSL 下 `DISPLAY` 是否正确

### MetaParser / libclang 相关崩溃

请优先检查：

- 是否已通过 CMake 或 `SetupDependencies --dependency dotnet-sdk` 安装 .NET 8.0.408 SDK
- 是否正确执行了 `dotnet restore`
- 是否误用了系统里的旧版 `libclang`

## 贡献

提交 Issue、建议或 Pull Request 前，请先阅读 [CONTRIBUTING.md](./CONTRIBUTING.md)
和 [CLA.md](./CLA.md)。向 Nullus Engine 提交贡献即表示你接受其中的贡献授权条款。

## License

Nullus Engine 使用自定义 Source-Available License。允许查看和修改源码、商业使用以及发布使用 Nullus Engine 制作的游戏；禁止分发引擎源码或使用本项目制作竞争引擎。详情见 [LICENSE](./LICENSE)。
