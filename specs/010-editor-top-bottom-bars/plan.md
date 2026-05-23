# Implementation Plan: Editor Top And Bottom Bars

**Branch**: `010-editor-top-bottom-bars` | **Date**: 2026-05-03 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/010-editor-top-bottom-bars/spec.md`

## Summary

为 Nullus Editor 增加一套固定在主窗口上下边缘的编辑器栏位体系：顶部提供 Unity 风格的统一菜单与主工具区，底部提供持续可见的状态脚栏，并在首版中只展示 FPS。实现上优先复用现有 `MenuBar`、`Toolbar`、`SceneView` 编辑模式和时钟/帧时间信息，但需要补一层可复用的“视口锚定栏位”UI 承载能力，以避免继续把顶栏/脚栏当作普通可停靠窗口处理。

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: NLS_Engine, ImGui, GLFW  
**Storage**: 文件系统（现有 ImGui layout ini；本特性本身不新增持久化数据）  
**Testing**: CTest + NullusUnitTests（回归保障），手动验证（Editor UI 行为）  
**Target Platform**: Windows DX12 Editor 主路径；设计上不引入后端专有 UI 分叉  
**Project Type**: Desktop application（游戏引擎编辑器）  
**Performance Goals**: 顶栏和脚栏加入后，Editor 常规交互保持实时响应；FPS 状态读数持续更新且不阻塞主编辑循环  
**Constraints**: 不修改 `Runtime/*/Gen/`；不把 Unity 生态入口错误伪装成可用功能；Editor 与 Game 必须继续可运行；优先复用现有 ImGui/UI 框架而不是引入新 UI 系统；窗口 resize 后栏位必须稳定贴边且不遮挡主内容  
**Scale/Scope**: 1 个 Editor 产品路径，涉及 `Runtime/UI` 与 `Project/Editor` 两层；首版范围限定为固定顶栏 + FPS 脚栏 + 与现有编辑模式/播放模式对接

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Pre-Design Check

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | spec bundle 已创建于 `specs/010-editor-top-bottom-bars/` |
| II. Validation Matches Subsystem | PASS | 变更属于 `Project/Editor` UI 行为，计划采用 Editor 构建 + 精确手动验证；不对未验证后端做正确性声明 |
| III. Generated Code / Backend Boundaries | PASS | 设计不触碰 `Runtime/*/Gen/`，不新增渲染后端分叉，只在现有 UI/Editor 层扩展栏位承载与交互 |
| IV. Incremental Delivery | PASS | 计划按“UI 承载层 → 顶栏整合 → 脚栏/FPS → 清理与验证”分阶段推进 |
| V. Product Runtime Preservation | PASS | Editor 在整个迁移过程中保持可运行；Game 不被纳入行为变更范围 |

### Post-Design Check

| Gate | Status | Evidence |
|------|--------|----------|
| I. Spec-First | PASS | `plan.md`、`research.md`、`data-model.md`、`contracts/`、`quickstart.md` 已补齐 |
| II. Validation Matches Subsystem | PASS | `quickstart.md` 定义了 Editor 定向构建与栏位行为手动验证步骤 |
| III. Generated Code / Backend Boundaries | PASS | 设计限于 `Runtime/UI` 与 `Project/Editor`，没有要求修改生成代码或后端特化 UI 路径 |
| IV. Incremental Delivery | PASS | 数据模型、UI 契约和实施阶段均支持逐步替换现有 `MenuBar`/`Toolbar`/`FrameInfo` |
| V. Product Runtime Preservation | PASS | 设计明确保留既有编辑命令与播放模式语义，只迁移入口呈现方式 |

## Project Structure

### Documentation (this feature)

```text
specs/010-editor-top-bottom-bars/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output - design decisions
├── data-model.md        # Phase 1 output - UI/domain entities
├── quickstart.md        # Phase 1 output - build and verify guide
├── contracts/           # Phase 1 output - UI behavior contracts
│   └── editor-bars-ui-contract.md
└── tasks.md             # Phase 2 output (/speckit.tasks)
```

### Source Code (repository root)

```text
Runtime/UI/
├── Modules/
│   └── Canvas.cpp                   # 视口 dockspace 与全局栏位共存规则可能需要调整
├── Panels/
│   ├── PanelMenuBar.*               # 现有主菜单栏基座
│   ├── PanelWindow.*                # 现有窗口面板基座
│   └── PanelViewportBar.*           # 新增：固定锚定主视口上下边缘的栏位抽象
└── Widgets/
    ├── Buttons/
    ├── Selection/
    └── Texts/

Project/Editor/
├── Core/
│   ├── Editor.cpp                   # 面板注册、更新、焦点和栏位布局接入点
│   ├── Editor.h
│   ├── EditorActions.cpp            # 顶栏命令入口和可复用状态查询
│   └── EditorActions.h
├── Panels/
│   ├── MenuBar.*                    # 现有菜单内容，可迁移到新顶栏组合区
│   ├── Toolbar.*                    # 现有播放工具栏，可改为顶栏主工具区
│   ├── FrameInfo.*                  # 现有统计窗口，FPS 状态来源的参考/复用对象
│   ├── SceneView.*                  # 现有 W/E/R 模式状态来源
│   ├── EditorTopBar.*               # 新增：组合式顶栏
│   └── EditorStatusBar.*            # 新增：底部 FPS 脚栏
└── Resources/                       # 现有图标资源，如播放按钮等继续复用
```

**Structure Decision**: 在 `Runtime/UI/Panels/` 新增一个可复用的“主视口锚定栏位”抽象，作为 Editor 顶栏和脚栏的统一承载基座；在 `Project/Editor/Panels/` 新增 `EditorTopBar` 与 `EditorStatusBar` 两个面板，分别整合现有 `MenuBar`、`Toolbar` 与 FPS 状态显示。现有 `SceneView` 和 `EditorActions` 继续作为状态与命令来源，而不是重复发明新的编辑状态系统。

## Phase 0 Research

见 [research.md](research.md)。本阶段已解决：

1. 顶栏/脚栏应使用固定视口锚定承载，而不是普通可停靠窗口。
2. FPS 应来自现有主循环/时钟帧时间，而不是复用 `FrameInfo` 整窗。
3. Unity 风格顶栏首版应映射 Nullus 已有能力，并对未实现能力明确禁用或不展示。

## Phase 1 Design Artifacts

- [data-model.md](data-model.md): 顶栏区域、命令组、场景工具状态、脚栏 FPS 状态等实体
- [contracts/editor-bars-ui-contract.md](contracts/editor-bars-ui-contract.md): 顶栏/脚栏的布局、状态、交互和降级契约
- [quickstart.md](quickstart.md): Editor 构建与手动验证路径

## Implementation Phases

### Phase A: Add A Reusable Viewport Bar Foundation

1. 在 `Runtime/UI/Panels/` 引入固定锚定主视口边缘的新栏位抽象，支持顶部/底部、固定高度、全宽铺展和无停靠行为。
2. 确认该抽象与现有 `Canvas`/dockspace 共存，不会被普通窗口布局覆盖，也不会在 resize 时漂移。
3. 保持该抽象通用，避免把 Editor 专有逻辑下沉到 Runtime/UI 基座。
4. 验证：空栏位可在 Editor 中稳定贴附主视口上下边缘。

### Phase B: Consolidate Menu And Toolbar Into A Unity-Style Top Bar

1. 新增 `EditorTopBar` 组合面板，承载主菜单区、播放控制区和核心编辑模式区。
2. 迁移或复用 `MenuBar` 的既有菜单内容，保证命令语义不变。
3. 迁移或复用 `Toolbar` 的播放控制按钮，使其不再依赖独立可停靠窗口才能访问。
4. 为顶栏补上场景编辑核心模式状态展示，最小闭环包括现有变换模式；支持能力范围内再接入坐标/参考模式。
5. 对当前未具备行为支撑的 Unity 风格入口，采用明确禁用态或不展示，避免伪可用交互。
6. 验证：常见文件/窗口/资源命令和播放控制均可从顶栏直达。

### Phase C: Add Bottom Status Bar With FPS Indicator

1. 新增 `EditorStatusBar` 固定脚栏。
2. 引入 FPS 读数来源，优先使用现有主循环/时钟帧时间形成稳定展示值。
3. 处理启动早期、暂停态、resize 跟帧等情况下的初始值与更新节奏，保证脚栏不显示空白或明显错误值。
4. 验证：FPS 在编辑过程中持续可见、随帧更新，并在窗口尺寸变化后仍保持布局稳定。

### Phase D: Remove Redundant Entry Points And Stabilize Layout

1. 评估现有独立 `Toolbar` 与 `FrameInfo` 在新栏位体系下的角色，避免功能重复和入口分裂。
2. 若旧窗口仍保留，明确它们与新顶栏/脚栏的职责边界；若移除默认入口，确保不丢失现有能力。
3. 对窄窗口、默认布局恢复、首帧聚焦、dockspace 重置等情形做布局稳定性修正。
4. 验证：默认布局与重置布局下都能看到正确的顶栏/脚栏，且主内容区不被覆盖。

## Complexity Tracking

无宪法违规需要记录。
