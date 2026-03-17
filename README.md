# Nullus
<p align="center">
    <img src="NullusLogo.png" width="400" alt="Nullus Engine logo">
</p>

## 界面预览

### Launcher

![Nullus Launcher](Docs/Screenshots/launcher.png)

### Editor

![Nullus Editor](Docs/Screenshots/editor.png)

Nullus 是一个仍在持续演进中的 C++ 3D 引擎项目，当前仓库同时包含运行时、编辑器、资源系统、反射代码生成工具和跨平台构建链路。

## 构建状态

| 平台 | 状态 |
| :-- | :-- |
| Windows | [![Build Windows](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_windows.yml) |
| Linux | [![Build Linux](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_linux.yml) |
| macOS | [![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_macos.yml) |

## 项目概览

当前仓库主要包含这些部分：

- `Runtime/`：运行时模块，包括 `Base`、`Core`、`Engine`、`Math`、`Platform`、`Rendering`、`UI`
- `Project/Editor`：编辑器程序
- `Project/Game`：运行时游戏程序
- `Tools/MetaParser`：基于 CppAst 的反射代码生成工具
- `ThirdParty/`：第三方依赖，包括 Assimp、FrameGraph、ImGui、json11 等

已经接入并可在仓库中确认的核心能力包括：

- 场景系统：`Scene / SceneManager / GameObject / Component`
- 资源系统：`Model / Texture / Shader / Material`
- 反射系统：基于 MetaParser 的代码生成式反射
- 序列化：基于新反射系统的场景与对象数据序列化链路
- 渲染系统：前向与延迟两套场景渲染路径
- 编辑器：基础视图、调试绘制、选取、Gizmo、资源预览
- 跨平台构建：Windows / Linux / macOS

## 最近更新

这段时间仓库里比较重要的变更主要有：

- 反射系统完成了从旧兼容层到新生成式系统的迁移
- 数学库和普通结构体支持通过类外声明接入反射
- 场景加载、编辑器启动与基础渲染链路已重新打通
- 接入 `FrameGraph`，主场景渲染开始采用 RDG 调度
- 增加延迟渲染路径，并引入 clustered shading
- 前向渲染路径也已迁到 RDG
- `SceneRenderer` 已拆分为共享基类和两条具体渲染路径

## 渲染架构

当前渲染侧的结构是：

- `BaseSceneRenderer`
  负责共享的场景解析、drawable 收集、公共描述符和帧前准备
- `ForwardSceneRenderer`
  负责前向主场景渲染，主流程通过 RDG 构建
- `DeferredSceneRenderer`
  负责延迟主场景渲染，包含 GBuffer、光照、天空盒和透明物体阶段

这三者的关系是：

- 前向和延迟共用一套场景解析与 graph resource 基础设施
- 主场景渲染由 RDG 驱动
- 旧的注册式 pass 系统没有被简单删除，而是保留为扩展层
- 编辑器里的 grid、picking、debug overlay 这类扩展阶段，仍可以在主 graph 执行后继续复用

另外两类基础抽象的职责如下：

- `ARenderFeature`
  用于提供可复用的渲染能力，例如引擎缓冲、灯光数据、调试图元、描边、Gizmo
- `ARenderPass`
  现在主要用于主场景之外的扩展阶段，而不是承载整帧主渲染流程

当前渲染系统已经具备这些能力：

- RDG 驱动的前向渲染路径
- RDG 驱动的延迟渲染路径
- GBuffer
- clustered shading
- 天空盒合成
- 透明物体前向补绘
- OpenGL RHI 扩展：多附件 framebuffer、depth blit、外部纹理包装

## 反射与 MetaParser

Nullus 当前使用基于 CppAst.NET 的 MetaParser 生成反射代码。

大致流程如下：

1. 构建主工程时会先构建 `Tools/MetaParser`
2. 各 Runtime 模块会在编译前运行 MetaParser
3. 生成对应的 `*.generated.h` / `*.generated.cpp`
4. 运行时通过生成代码完成类型声明、定义和注册

目前这套反射系统的特点是：

- 支持 `CLASS()` / `STRUCT()` / `GENERATED_BODY()`
- 支持每个头文件对应一组生成文件
- 支持按类生成静态注册器
- 支持两阶段注册，避免继承链顺序问题
- 支持类外反射声明，适合数学类型和轻量结构体

### 使用约束

MetaParser 依赖 ClangSharp 和 libclang 的 NuGet runtime，请不要手动把它绑定到系统里其它旧版 `libclang`。

建议做法：

- 安装 .NET 8 SDK
- 让 `dotnet restore` 自动恢复 MetaParser 依赖
- 使用项目自带的 CMake / 构建脚本，不要额外覆盖 MetaParser 运行时路径

如果强行改用系统环境里的旧 `libclang`，很容易在解析 AST 或 annotate 信息时崩溃。

## 快速开始

### 环境要求

- CMake 3.16 及以上
- 支持 C++20 的编译器
- .NET SDK 8.0 及以上
- Git

获取源码：

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

## 构建与运行

### Windows

推荐环境：Visual Studio 2022

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

也可以直接使用脚本：

```powershell
build_windows.bat Debug
build_windows.bat Release
build_windows.bat Debug ARM64
```

构建完成后可以运行：

- `Editor`
- `Game`

### Linux

Ubuntu 常用依赖：

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  libwayland-dev wayland-protocols \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxfixes-dev libxkbcommon-dev \
  libgl1-mesa-dev libglu1-mesa-dev libvulkan-dev
```

构建：

```bash
./build_linux.sh debug
./build_linux.sh release
```

等价命令：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

运行：

```bash
cd App/Linux_Debug_Static
./Editor
./Game
```

### macOS

```bash
./build_macos.sh debug
./build_macos.sh release
```

等价命令：

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Debug
```

### WSL

如果使用 WSL 并带有图形环境：

```bash
cd App/Linux_Debug_Static
export DISPLAY=:0
./Editor
./Game
```

如果只是后台验证：

```bash
sudo apt install -y xvfb
cd App/Linux_Debug_Static
xvfb-run -a ./Editor
xvfb-run -a ./Game
```

## CI 说明

GitHub Actions 当前覆盖 Windows、Linux 和 macOS。

CI 的构建特点：

- 会先安装 .NET 8 SDK
- MetaParser 依赖通过 NuGet 自动恢复
- 不再依赖系统级旧版 `libclang`
- 渲染、反射、测试目标会按平台脚本执行

## 常见问题

### `Failed to Init GLFW`

通常是图形环境没有准备好。

可以优先检查：

- 本机是否有可用桌面环境
- Linux / WSL 下 `DISPLAY` 是否正确

### Shader、天空盒或资源加载失败

优先检查：

- 资源路径是否存在
- 路径分隔符是否统一使用 `/`
- 项目启动时指定的场景和资源目录是否正确

### MetaParser / libclang 相关崩溃

优先检查：

- 是否安装了 .NET 8 SDK
- 是否正确执行过 `dotnet restore`
- 是否误用了系统里的旧版 `libclang`

## 当前状态与后续方向

目前 Nullus 已经不是一个只剩基础壳子的仓库，主链路已经可以覆盖：

- 反射生成
- 场景加载
- 编辑器启动
- 前向渲染
- 延迟渲染
- clustered shading

接下来更值得继续推进的方向包括：

- 渲染侧进一步收敛 RHI 抽象
- 更多 editor 工具和资产工作流
- 更完整的自动化测试
- 序列化格式与资源格式文档化

## License

如果后续仓库单独补充许可证文件，请以仓库中的正式 License 文件为准。
