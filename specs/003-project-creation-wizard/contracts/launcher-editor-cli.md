# Contract: Launcher → Editor CLI Interface

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## Overview

Launcher 可执行程序通过命令行参数启动 Editor 可执行程序。此契约定义了 Launcher 传递给 Editor 的参数格式和语义。

## Command Format

```
Editor.exe [options] <project_path>
```

## Parameters

### Positional: `project_path` (Required)

- **Type**: filesystem path (string)
- **Format**: 指向 `.nullus` 文件的绝对路径，或包含 `.nullus` 文件的目录绝对路径
- **Quoting**: 路径含空格时必须用双引号包裹
- **Validation**: Editor 启动时验证路径存在性；无效路径导致错误退出

### Option: `--backend <name>` / `-b <name>` (Optional)

- **Type**: string enum
- **Values**: `dx12`, `vulkan`, `opengl`, `dx11`, `metal`
- **Default**: 使用 .nullus 文件中 `graphics_backend` 键的值；如未设置，使用平台默认
- **Behavior**: 覆盖项目文件中的后端设置

### Option: `--renderdoc` (Optional, flag)

- **Type**: boolean flag
- **Default**: disabled
- **Behavior**: 启用 RenderDoc 帧捕获支持

### Option: `--no-renderdoc` (Optional, flag)

- **Type**: boolean flag
- **Default**: N/A
- **Behavior**: 显式禁用 RenderDoc 支持

### Option: `--capture-after-frames <N>` (Optional)

- **Type**: uint32
- **Default**: 0 (immediate)
- **Behavior**: 在 N 帧后自动触发 RenderDoc 捕获。隐式启用 `--renderdoc`

### Option: `--help` / `-h` (Optional, flag)

- **Type**: boolean flag
- **Behavior**: 显示帮助信息并退出

## Error Behavior

| Condition | Exit Code | Output |
|-----------|-----------|--------|
| 无 project_path 参数 | 1 | `No project specified. Launch Editor through Launcher.exe or provide a project path as argument.` |
| project_path 不存在 | 1 | `Project path does not exist: <path>` |
| 未知的 backend 名称 | 1 | `Unknown graphics backend: <name>. Supported: dx12, vulkan, opengl, dx11, metal` |
| --help | 0 | 帮助文本 |

## Examples

```bash
# Launcher 启动新创建的项目（Vulkan 后端）
Editor.exe --backend vulkan "D:\Projects\MyGame\MyGame.nullus"

# Launcher 启动已有项目（使用项目默认后端）
Editor.exe "D:\Projects\MyGame\MyGame.nullus"

# 使用 RenderDoc 调试
Editor.exe --renderdoc --capture-after-frames 60 "D:\Projects\MyGame\MyGame.nullus"

# 无效：缺少项目路径
Editor.exe --backend dx12
# → Error exit, prompts to use Launcher
```

## Invariants

- Editor 在无 project_path 参数时 MUST NOT 启动 Launcher 界面（Launcher 已是独立进程）
- 所有路径参数 MUST 使用绝对路径
- 后端覆盖优先级：命令行参数 > .nullus 配置 > 平台默认
- 此契约与现有 Game 可执行程序的参数格式保持一致（参见 `Project/Game/LaunchArgs.h`）
