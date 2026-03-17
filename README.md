# Nullus
<p align="center">
    <img src="NullusLogo.png" width="400" alt="Nullus Engine logo">
</p>

Nullus 3D游戏引擎

***

## Continuous build status

|    Build Type     |                            Status                            |
| :---------------: | :----------------------------------------------------------: |
| **Build Windows** | [![Build MacOS](https://github.com/NullusEngine/Nullus/workflows/Build%20Windows/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_windows.yml) |
|  **Build Linux**  | [![Build MacOS](https://github.com/NullusEngine/Nullus/workflows/Build%20Linux/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_linux.yml) |
|  **Build MacOS**  | [![Build macOS](https://github.com/NullusEngine/Nullus/workflows/Build%20MacOS/badge.svg)](https://github.com/NullusEngine/Nullus/actions/workflows/build_macos.yml) |

## Getting Started

### 基础要求

- CMake >= 3.16
- C++20 编译器
- .NET SDK 8.0+（MetaParser 为 C# 工具，构建时会自动编译并执行）
- Git（建议先执行子模块初始化）

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

### MetaParser / 反射代码生成说明

Nullus 现在包含一个基于 **CppAst.NET** 的跨平台 MetaParser：

- MetaParser 作为 **C# target** 集成在 CMake 工程中
- 构建主工程时会先构建 MetaParser
- Runtime 各模块会在编译前自动执行 MetaParser，生成 `Runtime/*/Gen/MetaGenerated.h/.cpp`
- 生成参数通过 `precompile.json` 传递，包含头文件、宏定义、include 路径、编译目标等信息

### 重要的跨平台约束

MetaParser 依赖 **ClangSharp / libclang 的 NuGet runtime**，不要强行把它绑定到系统自带的旧版 `libclang`。

正确做法：
- 安装 .NET 8 SDK
- 让 `dotnet restore/publish` 自动拉取匹配版本的 NuGet runtime
- 由 CMake 在不同平台上自动注入对应 native runtime 路径（版本号集中在 `Runtime/CMakeLists.txt` 中统一维护）:
  - Windows → `PATH`
  - macOS → `DYLD_LIBRARY_PATH`
  - Linux → `LD_LIBRARY_PATH`

如果手动覆盖到系统里的其它 `libclang`（尤其老版本），可能会导致 MetaParser 在解析 annotate / AST 时崩溃。

当前 Linux 路径下，项目仍保留了一个已验证可用的 `g++-9` 兼容 fallback，用于与 vendored CppAst/ClangSharp 运行时保持稳定配合；后续如果要进一步泛化到更多 Linux 发行版/工具链，建议在保持 CI 通过的前提下再逐步替换成更通用的编译器/系统头发现策略。

### 平台编译与运行

#### Windows（VS2022）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

或直接使用：

```powershell
build_windows.bat Debug
build_windows.bat Release
# 指定架构（例如 Windows on ARM）
build_windows.bat Debug ARM64
```

构建后运行 `Editor` 或 `Game` 目标。

#### Linux

依赖（Ubuntu）：

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  libwayland-dev wayland-protocols \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxfixes-dev libxkbcommon-dev \
  libgl1-mesa-dev libglu1-mesa-dev libvulkan-dev
```

编译：

```bash
./build_linux.sh debug
# 或
./build_linux.sh release
```

说明：
- `build_linux.sh` 默认只会在未设置 `CC/CXX` 时回退到 `gcc/g++`
- 如果你希望使用 `clang/clang++` 或自定义 toolchain，可以在调用前自行设置环境变量

等价命令：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

运行：

```bash
cd App/Linux_Debug_Static
./Editor
# 或
./Game
```

#### WSL（推荐说明）

有图形桌面（WSLg / X Server）：

```bash
cd App/Linux_Debug_Static
export DISPLAY=:0
./Editor
# 或
./Game
```

无图形桌面（后台验证）：

```bash
sudo apt install -y xvfb
cd App/Linux_Debug_Static
xvfb-run -a ./Editor
# 或
xvfb-run -a ./Game
```

#### macOS

```bash
./build_macos.sh debug
# 或
./build_macos.sh release
```

等价命令：

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Debug
```

构建后运行 `Editor` 或 `Game`。

### CI 说明

GitHub Actions 已按平台配置：

- Linux / macOS / Windows 都会先安装 .NET 8 SDK
- MetaParser 所需的 ClangSharp/libclang 运行时由 NuGet restore 自动获取
- CI 不再依赖系统级旧版 `libclang` 来驱动 MetaParser

### 已实现功能点（当前仓库可确认）

- Game / Editor 双目标工程
- Scene / SceneManager 场景系统
- GameObject + Component 架构（Transform/Camera/Light/Skybox 等）
- 资源系统（Model / Texture / Shader / Material）
- 基础渲染流程（场景绘制与帧循环）
- Assimp 模型导入
- JSON 序列化/反序列化框架（Serializer + Handler）
- Windows / Linux / macOS 跨平台构建链路
- 基于 CppAst 的 MetaParser 反射代码生成链路

### 常见问题

- `Failed to Init GLFW`：通常是图形环境未就绪（检查 `DISPLAY`，WSL 可先 `export DISPLAY=:0`）。
- 资源加载失败（shader/skybox）：优先检查路径分隔符，跨平台统一使用 `/`。
- MetaParser / libclang 相关崩溃：优先确认是否误绑到了系统自带的老版本 `libclang`，不要覆盖 CMake 注入的 NuGet runtime 路径。

## TODO

- 序列化/反序列化
- Application初始化流程
- Input系统
- Module
- Level
- Editor
- Imgui

## Recent Rendering Updates

- Integrated a FrameGraph-based render scheduling layer under `ThirdParty/FrameGraph` and `Runtime/Rendering/FrameGraph`.
- Added a deferred renderer for runtime and editor game view with GBuffer generation, fullscreen deferred lighting, skybox composition, and a forward transparent pass on top.
- Added clustered shading support. Deferred lighting now consumes cluster light lists directly instead of falling back to a full-scene light sweep.
- Extended the current OpenGL RHI bridge with multi-attachment framebuffer support, depth blitting, and wrapped external textures for deferred resources.
- Cleaned internal shader/material binding for deferred lighting and skybox rendering so normal startup no longer emits noisy OpenGL uniform warnings.
