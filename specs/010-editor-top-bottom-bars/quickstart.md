# Quickstart: Editor Top And Bottom Bars

**Feature**: `010-editor-top-bottom-bars`  
**Date**: 2026-05-03

## Prerequisites

- Nullus 仓库已拉取完成
- CMake 与平台工具链可用
- 一个可正常启动的 `.nullus` 项目可用于 Editor 验证

## Build

### Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target Editor -- /m:4
```

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target Editor
```

如需先跑基础回归：

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

## Run

### 通过 Launcher 启动 Editor

```powershell
Build\bin\Debug\Launcher.exe
```

在 Launcher 中选择一个现有项目并启动 Editor。

### 直接启动 Editor

```powershell
Build\bin\Debug\Editor.exe "D:\Path\To\Project\MyProject.nullus"
```

如果需要更聚焦地验证主编辑视图，可使用现有诊断参数：

```powershell
Build\bin\Debug\Editor.exe --editor-validation-focus-view scene "D:\Path\To\Project\MyProject.nullus"
```

## Verify

### Verify Top Bar

1. 启动 Editor 后，确认窗口顶部始终存在一条固定顶栏。
2. 确认无需打开独立 `Toolbar` 窗口，也能从顶栏访问文件/窗口/资源/帮助等主菜单命令。
3. 确认顶栏中存在播放控制入口，且按钮可用态会随着编辑器模式变化。
4. 确认顶栏中能识别当前场景编辑核心模式，至少包含已有的变换模式能力。
5. 当主窗口宽度明显收窄时，确认场景工具区会退化为紧凑按钮或下拉框，但仍能识别并切换 `Move` / `Rotate` / `Scale`。

### Verify Bottom Status Bar

1. 确认窗口底部始终存在一条固定脚栏。
2. 确认脚栏中持续显示 FPS。
3. 保持 Editor 运行数秒，确认 FPS 会更新，而不是停留在初始化占位值。

### Verify Resize And Layout Stability

1. 缩小与放大 Editor 主窗口，确认顶栏和脚栏仍然贴附上下边缘。
2. 在默认布局和重置布局后，确认顶栏/脚栏仍存在且不遮挡主要内容区。
3. 切换 Scene View、Game View 和常见停靠窗口焦点，确认脚栏 FPS 仍持续可见。

### Verify Scope Boundaries

1. 对尚未实现的 Unity 风格入口，确认界面没有把它们表现为“点击即可工作”的可用功能。
2. 确认原有核心工作流没有丢失：主菜单、播放控制、场景基本编辑模式仍然可达。
3. 如需查看详细帧统计，确认 `Frame Info` 仍可通过 `Window` 菜单手动打开，但默认启动时不再与脚栏 FPS 重复展示。

## Implementation Notes

- 当前已完成的自动化验证为 `Editor` 目标的 Debug 构建通过。
- 当前仓库未包含可直接启动验证的 `.nullus` 示例项目，因此仍需使用本地实际项目完成一次手动 UI 验收。
- 本次实现未发现 DX12 专属栏位布局分支；顶栏和脚栏布局逻辑保持在通用 ImGui/UI 层。
