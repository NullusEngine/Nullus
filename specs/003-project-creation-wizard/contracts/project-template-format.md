# Contract: Project Template Format

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## Overview

项目模板以文件系统目录结构定义，Launcher 通过扫描模板根目录动态发现和加载模板。

## Directory Layout

```
<template-root>/                    # 由 Launcher 配置决定，默认为 Assets/Templates/
└── <template-id>/                  # 模板唯一标识（目录名即 ID）
    ├── template.json               # 模板元数据（必须）
    ├── preview.png                 # 预览缩略图（可选，支持 png/jpg）
    └── content/                    # 初始内容（可选）
        ├── Assets/                 # 模板预置资源
        ├── ProjectSettings/        # 模板预置设置
        └── ...                     # 其他预置文件/目录
```

## template.json Schema

```json
{
  "name": "string (required)",          // 模板显示名称，如 "Empty Project"
  "description": "string (required)",   // 模板简短描述
  "preview": "string (optional)",       // 预览图文件名，相对于模板目录
  "sortOrder": "int (optional)"         // UI 排序权重，默认 0（越小越靠前）
}
```

### Validation Rules

- `name`: 非空字符串，最大 64 字符
- `description`: 非空字符串，最大 256 字符
- `preview`: 如果指定，文件必须存在于模板目录中
- `sortOrder`: 如果指定，必须 >= 0

## Template Loading Behavior

1. Launcher 启动时扫描 `<template-root>/` 下的所有一级子目录
2. 对每个子目录，尝试读取 `template.json`
3. 如果 `template.json` 不存在或格式无效，跳过该目录（静默忽略，不报错）
4. 按 sortOrder 升序排列模板列表
5. 预览图加载失败时显示默认占位图

## Project Creation from Template

创建项目时：
1. 创建目标目录结构：`<project_path>/`、`Assets/`、`Logs/`、`UserSettings/`、`ProjectSettings/`
2. 如果模板有 `content/` 目录，将其内容递归复制到 `<project_path>/`
3. 创建 `.nullus` 配置文件，填入向导中的用户设置（后端、分辨率等）
4. 模板预置文件与标准目录结构冲突时，模板内容优先

## v1: Empty Project Template

```
Assets/Templates/EmptyProject/
├── template.json
└── (无 content/ 目录)
```

```json
{
  "name": "Empty Project",
  "description": "A blank project with default settings. Start from scratch.",
  "sortOrder": 0
}
```

## Invariants

- 模板 ID（目录名）仅允许 `[a-zA-Z0-9_-]`
- 至少一个有效模板时才允许创建项目（v1 保证有 EmptyProject）
- 模板加载失败不应阻止 Launcher 启动
- 模板内容为只读，Launcher 不会修改模板文件
