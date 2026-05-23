# Data Model: Project Creation Wizard

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## Entities

### ProjectTemplate

项目模板，定义项目的初始结构和元数据。

| Field | Type | Description | Validation |
|-------|------|-------------|------------|
| name | string | 模板显示名称 | 非空，唯一 |
| description | string | 模板简短描述 | 非空 |
| previewImagePath | string | 预览图相对路径 | 可选，相对于模板目录 |
| templateDirectory | filesystem path | 模板内容目录的绝对路径 | 必须存在 |
| sortOrder | int | 模板在 UI 中的显示顺序 | >= 0 |

**Relationships**: 无直接关系。模板是独立的实体。

**State**: 无状态变化。模板在启动时从文件系统加载，运行时为只读。

**Storage**: 文件系统目录结构：
```
Assets/Templates/<template-id>/
├── template.json
├── preview.png (optional)
└── content/    (optional - v1 空项目模板无此目录)
```

`template.json` 格式：
```json
{
  "name": "Empty Project",
  "description": "A blank project with default settings",
  "preview": "preview.png",
  "sortOrder": 0
}
```

---

### ProjectCreationConfig

用户在创建向导中填写的配置信息，仅在创建过程中使用，创建完毕后转化为 .nullus 文件。

| Field | Type | Description | Default | Validation |
|-------|------|-------------|---------|------------|
| projectName | string | 项目名称 | "" | 非空，合法文件名字符（字母、数字、下划线、连字符），长度 1-128 |
| projectLocation | string | 项目存储的父目录绝对路径 | "" | 非空，目录必须存在且可写 |
| selectedTemplate | ProjectTemplate* | 选中的项目模板 | 第一个模板 | 非空 |
| graphicsBackend | enum EGraphicsBackend | 渲染后端 | 平台默认 | 有效的后端枚举值 |
| windowWidth | int | 窗口宽度 | 1280 | 640-7680 |
| windowHeight | int | 窗口高度 | 720 | 480-4320 |
| vsync | bool | 垂直同步 | true | - |
| multiSampling | bool | 多重采样 | true | - |
| sampleCount | int | 采样数 | 4 | 1, 2, 4, 8 |

**Derived Fields**:
- `fullProjectPath` = `projectLocation / projectName`
- `nullusFilePath` = `fullProjectPath / projectName + ".nullus"`

**Relationships**:
- 引用一个 `ProjectTemplate`（多对一，多个配置可引用同一模板）

**State Transitions**:
```
[空] → [填写中] → [验证通过] → [已创建] → [启动Editor]
                ↓
           [验证失败] → [填写中]（用户修正后重新提交）
```

**Storage**: 运行时内存对象，不持久化。创建结果写入 .nullus INI 文件。

---

### LauncherLaunchParams

Launcher 传递给 Editor 进程的命令行参数。

| Field | Type | CLI Flag | Required | Description |
|-------|------|----------|----------|-------------|
| projectPath | string | 位置参数 | 是 | .nullus 文件或项目目录路径 |
| backend | enum EGraphicsBackend | `--backend <name>` / `-b` | 否 | 渲染后端覆盖 |
| renderDocEnabled | bool | `--renderdoc` / `--no-renderdoc` | 否 | RenderDoc 调试开关 |
| captureAfterFrames | uint32 | `--capture-after-frames <N>` | 否 | 延迟帧捕获数 |

**Serialization**: 命令行字符串，格式：
```
Editor.exe --backend vulkan --renderdoc "D:/Projects/MyGame/MyGame.nullus"
```

**Validation**:
- `projectPath` 必须指向存在的 .nullus 文件或包含 .nullus 文件的目录
- `backend` 如果指定，必须是有效的 `EGraphicsBackend` 枚举值
- 路径含空格时必须用引号包裹

**Relationships**: 由 `ProjectCreationConfig`（新项目创建时）或最近项目列表（打开已有项目时）生成。

---

### ProcessLaunchResult

进程启动结果，用于向用户报告 Editor 启动状态。

| Field | Type | Description |
|-------|------|-------------|
| success | bool | 进程是否成功创建 |
| errorMessage | string | 失败时的错误描述（如 "Editor.exe not found"） |

**State**: 一次性结果对象，无状态转换。

---

## Validation Rules Summary

| Rule | Applies To | Description |
|------|------------|-------------|
| V-001 | ProjectCreationConfig.projectName | 仅允许 `[a-zA-Z0-9_-]`，长度 1-128 |
| V-002 | ProjectCreationConfig.projectLocation | 目录必须存在且可写 |
| V-003 | ProjectCreationConfig.fullProjectPath | 不能已存在同名目录（除非用户确认覆盖） |
| V-004 | ProjectCreationConfig.graphicsBackend | 必须是当前平台支持的后端 |
| V-005 | ProjectCreationConfig.windowWidth/Height | 必须在有效范围内 |
| V-006 | LauncherLaunchParams.projectPath | 必须指向存在的文件或目录 |
