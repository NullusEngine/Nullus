# Nullus
<p align="center">
    <img src="NullusLogo.png" width="400" alt="Nullus Engine logo">
</p>

![License MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
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
- .NET SDK 8.0 及以上
- Git

### 获取源码

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

### Windows 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
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

## 文档入口

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

- 是否安装了 .NET 8 SDK
- 是否正确执行了 `dotnet restore`
- 是否误用了系统里的旧版 `libclang`

## License

本仓库使用 MIT License。详情见 [LICENSE](./LICENSE)。
