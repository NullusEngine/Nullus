# Quickstart: Project Creation Wizard

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## Prerequisites

- Nullus 项目已克隆，依赖已安装
- CMake 3.18+ 已安装
- 平台编译工具链就绪（Windows: VS 2022, Linux/macOS: Ninja + GCC/Clang）

## Build

### Windows
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### Linux/macOS
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

构建产物在 `Build/bin/Debug/` 下：
- `Launcher.exe` - 项目启动器（新）
- `Editor.exe` - 编辑器（精简版，不再含 Launcher）
- `Game.exe` - 游戏运行时（不变）

## Run

### 通过 Launcher 启动（正常工作流）
```bash
# 直接运行 Launcher，进入项目选择界面
./Launcher.exe

# 在 Launcher 中：
# 1. 点击 "New Project" → 打开创建向导
# 2. 选择模板、填写名称和路径、选择后端
# 3. 点击 "Create" → 自动启动 Editor 打开新项目
```

### 直接启动 Editor（高级/调试用）
```bash
# 必须传入项目路径
./Editor.exe --backend vulkan "path/to/MyProject.nullus"

# 不传项目路径会报错退出
./Editor.exe
# → Error: No project specified. Launch Editor through Launcher.exe or provide a project path.
```

## Verify

### 验证 Launcher/Editor 分离
1. 启动 `Launcher.exe` → 应显示项目选择界面
2. 在 Launcher 中选择或创建项目 → 应自动启动 `Editor.exe` 并传入正确参数
3. 直接运行 `Editor.exe`（无参数）→ 应显示错误信息并退出

### 验证项目创建向导
1. 启动 `Launcher.exe`
2. 点击 "New Project" → 应显示创建向导（而非系统文件夹对话框）
3. 确认向导包含：模板选择卡片、项目名称输入、路径选择、后端下拉、分辨率设置
4. 填写信息并创建 → 项目目录和 `.nullus` 文件应正确生成
5. Editor 自动启动并加载新项目

### 验证项目配置正确性
1. 通过向导创建项目，选择 Vulkan 后端、1920x1080 分辨率
2. 打开生成的 `.nullus` 文件，确认：
   - `graphics_backend=vulkan`
   - `x_resolution=1920`
   - `y_resolution=1080`

### 验证模板系统
1. 确认 `Assets/Templates/EmptyProject/` 目录存在且包含 `template.json`
2. 在向导中确认显示 "Empty Project" 模板卡片

### 运行测试
```powershell
ctest --test-dir build -C Debug --output-on-failure
```
