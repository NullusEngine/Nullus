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
- Git（建议先执行子模块初始化）

```bash
git clone https://github.com/NullusEngine/Nullus.git
cd Nullus
git submodule update --init --recursive
```

### 平台编译与运行

#### Windows（VS2022）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug -j
```

构建后运行 `Editor` 或 `Game` 目标。

#### Linux

依赖（Ubuntu）：

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  libwayland-dev wayland-protocols \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxfixes-dev libxkbcommon-dev \
  libgl1-mesa-dev libglu1-mesa-dev
```

编译：

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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
```

构建后运行 `Editor` 或 `Game`。

### 已实现功能点（当前仓库可确认）

- Game / Editor 双目标工程
- Scene / SceneManager 场景系统
- GameObject + Component 架构（Transform/Camera/Light/Skybox 等）
- 资源系统（Model / Texture / Shader / Material）
- 基础渲染流程（场景绘制与帧循环）
- Assimp 模型导入
- JSON 序列化/反序列化框架（Serializer + Handler）
- Windows / Linux / macOS 跨平台构建链路

### 常见问题

- `Failed to Init GLFW`：通常是图形环境未就绪（检查 `DISPLAY`，WSL 可先 `export DISPLAY=:0`）。
- 资源加载失败（shader/skybox）：优先检查路径分隔符，跨平台统一使用 `/`。

## TODO

- 序列化/反序列化
- Application初始化流程
- Input系统
- Module
- Level
- Editor
- Imgui
